/*
 * Copyright (c) 2000-2020 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */
/*
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988 Carnegie Mellon University
 * All Rights Reserved.
 *
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */

/*
 * Hardware trap/fault handler.
 */

#include <mach_kdp.h>
#include <mach_ldebug.h>

#include <types.h>
#include <i386/eflags.h>
#include <i386/trap.h>
#include <i386/pmap.h>
#include <i386/fpu.h>
#include <i386/panic_notify.h>
#include <i386/lapic.h>

#include <mach/exception.h>
#include <mach/kern_return.h>
#include <mach/vm_param.h>
#include <mach/i386/thread_status.h>

#include <vm/vm_kern.h>
#include <vm/vm_fault.h>

#include <kern/kern_types.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/sched.h>
#include <kern/sched_prim.h>
#include <kern/exception.h>
#include <kern/spl.h>
#include <kern/misc_protos.h>
#include <kern/debug.h>
#if CONFIG_TELEMETRY
#include <kern/telemetry.h>
#endif
#include <sys/kdebug.h>
#include <kperf/kperf.h>
#include <prng/random.h>
#include <prng/entropy.h>

#include <string.h>

#include <i386/postcode.h>
#include <i386/mp_desc.h>
#include <i386/proc_reg.h>
#include <i386/machine_routines.h>
#if CONFIG_MCA
#include <i386/machine_check.h>
#endif
#include <mach/i386/syscall_sw.h>

#include <libkern/OSDebug.h>
#include <i386/cpu_threads.h>
#include <machine/pal_routines.h>

extern void throttle_lowpri_io(int);
extern void kprint_state(x86_saved_state64_t *saved_state);
#if DEVELOPMENT || DEBUG
int insnstream_force_cacheline_mismatch = 0;
extern int panic_on_cacheline_mismatch;
extern char panic_on_trap_procname[];
extern uint32_t panic_on_trap_mask;
#endif

extern int insn_copyin_count;

/*
 * Forward declarations
 */
static void panic_trap(x86_saved_state64_t *saved_state, uint32_t pl, kern_return_t fault_result) __dead2;
static void set_recovery_ip(x86_saved_state64_t *saved_state, vm_offset_t ip);
#if DEVELOPMENT || DEBUG
static __attribute__((noinline)) void copy_instruction_stream(thread_t thread, uint64_t rip, int trap_code, bool inspect_cacheline);
#else
static __attribute__((noinline)) void copy_instruction_stream(thread_t thread, uint64_t rip, int trap_code);
#endif

#if CONFIG_DTRACE
/* See <rdar://problem/4613924> */
perfCallback tempDTraceTrapHook = NULL; /* Pointer to DTrace fbt trap hook routine */

extern boolean_t dtrace_tally_fault(user_addr_t);
extern boolean_t dtrace_handle_trap(int, x86_saved_state_t *);
#endif

#ifdef MACH_BSD
extern char *   proc_name_address(void *p);
#endif /* MACH_BSD */

extern boolean_t pmap_smep_enabled;
extern boolean_t pmap_smap_enabled;

__attribute__((noreturn))
void
thread_syscall_return(
	kern_return_t ret)
{
	thread_t        thr_act = current_thread();
	boolean_t       is_mach;
	int             code;

	pal_register_cache_state(thr_act, DIRTY);

	if (thread_is_64bit_addr(thr_act)) {
		x86_saved_state64_t     *regs;

		regs = USER_REGS64(thr_act);

		code = (int) (regs->rax & SYSCALL_NUMBER_MASK);
		is_mach = (regs->rax & SYSCALL_CLASS_MASK)
		    == (SYSCALL_CLASS_MACH << SYSCALL_CLASS_SHIFT);
		if (kdebug_enable && is_mach) {
			/* Mach trap */
			KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
			    MACHDBG_CODE(DBG_MACH_EXCP_SC, code) | DBG_FUNC_END,
			    ret, 0, 0, 0, 0);
		}
		regs->rax = ret;
#if DEBUG
		if (is_mach) {
			DEBUG_KPRINT_SYSCALL_MACH(
				"thread_syscall_return: 64-bit mach ret=%u\n",
				ret);
		} else {
			DEBUG_KPRINT_SYSCALL_UNIX(
				"thread_syscall_return: 64-bit unix ret=%u\n",
				ret);
		}
#endif
	} else {
		x86_saved_state32_t     *regs;

		regs = USER_REGS32(thr_act);

		code = ((int) regs->eax);
		is_mach = (code < 0);
		if (kdebug_enable && is_mach) {
			/* Mach trap */
			KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
			    MACHDBG_CODE(DBG_MACH_EXCP_SC, -code) | DBG_FUNC_END,
			    ret, 0, 0, 0, 0);
		}
		regs->eax = ret;
#if DEBUG
		if (is_mach) {
			DEBUG_KPRINT_SYSCALL_MACH(
				"thread_syscall_return: 32-bit mach ret=%u\n",
				ret);
		} else {
			DEBUG_KPRINT_SYSCALL_UNIX(
				"thread_syscall_return: 32-bit unix ret=%u\n",
				ret);
		}
#endif
	}

#if DEBUG || DEVELOPMENT
	kern_allocation_name_t
	prior __assert_only = thread_get_kernel_state(thr_act)->allocation_name;
	assertf(prior == NULL, "thread_set_allocation_name(\"%s\") not cleared", kern_allocation_get_name(prior));
#endif /* DEBUG || DEVELOPMENT */

	throttle_lowpri_io(1);

	thread_exception_return();
	/*NOTREACHED*/
}

/*
 * Fault recovery in copyin/copyout routines.
 */
struct recovery {
	uintptr_t       fault_addr;
	uintptr_t       recover_addr;
};

extern struct recovery  recover_table[];
extern struct recovery  recover_table_end[];

const char *    trap_type[] = {TRAP_NAMES};
unsigned        TRAP_TYPES = sizeof(trap_type) / sizeof(trap_type[0]);

extern void     PE_incoming_interrupt(int interrupt);

#if defined(__x86_64__) && DEBUG
void
kprint_state(x86_saved_state64_t        *saved_state)
{
	kprintf("current_cpu_datap() 0x%lx\n", (uintptr_t)current_cpu_datap());
	kprintf("Current GS base MSR 0x%llx\n", rdmsr64(MSR_IA32_GS_BASE));
	kprintf("Kernel  GS base MSR 0x%llx\n", rdmsr64(MSR_IA32_KERNEL_GS_BASE));
	kprintf("state at 0x%lx:\n", (uintptr_t) saved_state);

	kprintf("      rdi    0x%llx\n", saved_state->rdi);
	kprintf("      rsi    0x%llx\n", saved_state->rsi);
	kprintf("      rdx    0x%llx\n", saved_state->rdx);
	kprintf("      r10    0x%llx\n", saved_state->r10);
	kprintf("      r8     0x%llx\n", saved_state->r8);
	kprintf("      r9     0x%llx\n", saved_state->r9);

	kprintf("      cr2    0x%llx\n", saved_state->cr2);
	kprintf("real  cr2    0x%lx\n", get_cr2());
	kprintf("      r15    0x%llx\n", saved_state->r15);
	kprintf("      r14    0x%llx\n", saved_state->r14);
	kprintf("      r13    0x%llx\n", saved_state->r13);
	kprintf("      r12    0x%llx\n", saved_state->r12);
	kprintf("      r11    0x%llx\n", saved_state->r11);
	kprintf("      rbp    0x%llx\n", saved_state->rbp);
	kprintf("      rbx    0x%llx\n", saved_state->rbx);
	kprintf("      rcx    0x%llx\n", saved_state->rcx);
	kprintf("      rax    0x%llx\n", saved_state->rax);

	kprintf("      gs     0x%x\n", saved_state->gs);
	kprintf("      fs     0x%x\n", saved_state->fs);

	kprintf("  isf.trapno 0x%x\n", saved_state->isf.trapno);
	kprintf("  isf._pad   0x%x\n", saved_state->isf._pad);
	kprintf("  isf.trapfn 0x%llx\n", saved_state->isf.trapfn);
	kprintf("  isf.err    0x%llx\n", saved_state->isf.err);
	kprintf("  isf.rip    0x%llx\n", saved_state->isf.rip);
	kprintf("  isf.cs     0x%llx\n", saved_state->isf.cs);
	kprintf("  isf.rflags 0x%llx\n", saved_state->isf.rflags);
	kprintf("  isf.rsp    0x%llx\n", saved_state->isf.rsp);
	kprintf("  isf.ss     0x%llx\n", saved_state->isf.ss);
}
#endif


/*
 * Non-zero indicates latency assert is enabled and capped at valued
 * absolute time units.
 */

uint64_t interrupt_latency_cap = 0;
boolean_t ilat_assert = FALSE;

void
interrupt_latency_tracker_setup(void)
{
	uint32_t ilat_cap_us;
	if (PE_parse_boot_argn("interrupt_latency_cap_us", &ilat_cap_us, sizeof(ilat_cap_us))) {
		interrupt_latency_cap = ilat_cap_us * NSEC_PER_USEC;
		nanoseconds_to_absolutetime(interrupt_latency_cap, &interrupt_latency_cap);
	} else {
		interrupt_latency_cap = LockTimeOut;
	}
	PE_parse_boot_argn("-interrupt_latency_assert_enable", &ilat_assert, sizeof(ilat_assert));
}

void
interrupt_reset_latency_stats(void)
{
	uint32_t i;
	for (i = 0; i < real_ncpus; i++) {
		cpu_data_ptr[i]->cpu_max_observed_int_latency =
		    cpu_data_ptr[i]->cpu_max_observed_int_latency_vector = 0;
	}
}

void
interrupt_populate_latency_stats(char *buf, unsigned bufsize)
{
	uint32_t i, tcpu = ~0;
	uint64_t cur_max = 0;

	for (i = 0; i < real_ncpus; i++) {
		if (cur_max < cpu_data_ptr[i]->cpu_max_observed_int_latency) {
			cur_max = cpu_data_ptr[i]->cpu_max_observed_int_latency;
			tcpu = i;
		}
	}

	if (tcpu < real_ncpus) {
		snprintf(buf, bufsize, "0x%x 0x%x 0x%llx", tcpu, cpu_data_ptr[tcpu]->cpu_max_observed_int_latency_vector, cpu_data_ptr[tcpu]->cpu_max_observed_int_latency);
	}
}

uint32_t interrupt_timer_coalescing_enabled = 1;
uint64_t interrupt_coalesced_timers;

/*
 * Handle interrupts:
 *  - local APIC interrupts (IPIs, timers, etc) are handled by the kernel,
 *  - device interrupts go to the platform expert.
 */
void
interrupt(x86_saved_state_t *state)
{
	uint64_t        rip;
	uint64_t        rsp;
	int             interrupt_num;
	boolean_t       user_mode = FALSE;
	int             ipl;
	int             cnum = cpu_number();
	cpu_data_t      *cdp = cpu_data_ptr[cnum];
	int             itype = DBG_INTR_TYPE_UNKNOWN;
	int             handled;

	x86_saved_state64_t     *state64 = saved_state64(state);
	rip = state64->isf.rip;
	rsp = state64->isf.rsp;
	interrupt_num = state64->isf.trapno;
	if (state64->isf.cs & 0x03) {
		user_mode = TRUE;
	}

#if DEVELOPMENT || DEBUG
	uint64_t frameptr = is_saved_state64(state) ? state64->rbp : saved_state32(state)->ebp;
	uint32_t traptrace_index = traptrace_start(interrupt_num, rip, mach_absolute_time(), frameptr);
#endif

	if (cpu_data_ptr[cnum]->lcpu.package->num_idle == topoParms.nLThreadsPerPackage) {
		cpu_data_ptr[cnum]->cpu_hwIntpexits[interrupt_num]++;
	}

	if (interrupt_num == (LAPIC_DEFAULT_INTERRUPT_BASE + LAPIC_INTERPROCESSOR_INTERRUPT)) {
		itype = DBG_INTR_TYPE_IPI;
	} else if (interrupt_num == (LAPIC_DEFAULT_INTERRUPT_BASE + LAPIC_TIMER_INTERRUPT)) {
		itype = DBG_INTR_TYPE_TIMER;
	} else {
		itype = DBG_INTR_TYPE_OTHER;
	}

	KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
	    MACHDBG_CODE(DBG_MACH_EXCP_INTR, 0) | DBG_FUNC_START,
	    interrupt_num,
	    (user_mode ? rip : VM_KERNEL_UNSLIDE(rip)),
	    user_mode, itype, 0);

	SCHED_STATS_INC(interrupt_count);

#if CONFIG_TELEMETRY
	if (telemetry_needs_record) {
		telemetry_mark_curthread(user_mode, FALSE);
	}
#endif

	ipl = get_preemption_level();

	/*
	 * Handle local APIC interrupts
	 * else call platform expert for devices.
	 */
	handled = lapic_interrupt(interrupt_num, state);

	if (!handled) {
		if (interrupt_num == (LAPIC_DEFAULT_INTERRUPT_BASE + LAPIC_CMCI_INTERRUPT)) {
			/*
			 * CMCI can be signalled on any logical processor, and the kexts
			 * that implement handling CMCI use IOKit to register handlers for
			 * the CMCI vector, so if we see a CMCI, do not encode a CPU
			 * number in bits 8:31 (since the vector is the same regardless of
			 * the handling CPU).
			 */
			PE_incoming_interrupt(interrupt_num);
		} else if (cnum <= lapic_max_interrupt_cpunum) {
			PE_incoming_interrupt((cnum << 8) | interrupt_num);
		}
	}

	if (__improbable(get_preemption_level() != ipl)) {
		panic("Preemption level altered by interrupt vector 0x%x: initial 0x%x, final: 0x%x\n", interrupt_num, ipl, get_preemption_level());
	}


	if (__improbable(cdp->cpu_nested_istack)) {
		cdp->cpu_nested_istack_events++;
	} else {
		uint64_t ctime = mach_absolute_time();
		uint64_t int_latency = ctime - cdp->cpu_int_event_time;
		uint64_t esdeadline, ehdeadline;
		/* Attempt to process deferred timers in the context of
		 * this interrupt, unless interrupt time has already exceeded
		 * TCOAL_ILAT_THRESHOLD.
		 */
#define TCOAL_ILAT_THRESHOLD (30000ULL)

		if ((int_latency < TCOAL_ILAT_THRESHOLD) &&
		    interrupt_timer_coalescing_enabled) {
			esdeadline = cdp->rtclock_timer.queue.earliest_soft_deadline;
			ehdeadline = cdp->rtclock_timer.deadline;
			if ((ctime >= esdeadline) && (ctime < ehdeadline)) {
				interrupt_coalesced_timers++;
				TCOAL_DEBUG(0x88880000 | DBG_FUNC_START, ctime, esdeadline, ehdeadline, interrupt_coalesced_timers, 0);
				rtclock_intr(state);
				TCOAL_DEBUG(0x88880000 | DBG_FUNC_END, ctime, esdeadline, interrupt_coalesced_timers, 0, 0);
			} else {
				TCOAL_DEBUG(0x77770000, ctime, cdp->rtclock_timer.queue.earliest_soft_deadline, cdp->rtclock_timer.deadline, interrupt_coalesced_timers, 0);
			}
		}

		if (__improbable(ilat_assert && (int_latency > interrupt_latency_cap) && !machine_timeout_suspended())) {
			panic("Interrupt vector 0x%x exceeded interrupt latency threshold, 0x%llx absolute time delta, prior signals: 0x%x, current signals: 0x%x", interrupt_num, int_latency, cdp->cpu_prior_signals, cdp->cpu_signals);
		}

		if (__improbable(int_latency > cdp->cpu_max_observed_int_latency)) {
			cdp->cpu_max_observed_int_latency = int_latency;
			cdp->cpu_max_observed_int_latency_vector = interrupt_num;
		}
	}

	/*
	 * Having serviced the interrupt first, look at the interrupted stack depth.
	 */
	if (!user_mode) {
		uint64_t depth = cdp->cpu_kernel_stack
		    + sizeof(struct thread_kernel_state)
		    + sizeof(struct i386_exception_link *)
		    - rsp;
		if (__improbable(depth > kernel_stack_depth_max)) {
			kernel_stack_depth_max = (vm_offset_t)depth;
			KERNEL_DEBUG_CONSTANT(
				MACHDBG_CODE(DBG_MACH_SCHED, MACH_STACK_DEPTH),
				(long) depth, (long) VM_KERNEL_UNSLIDE(rip), 0, 0, 0);
		}
	}

	if (cnum == master_cpu) {
		entropy_collect();
	}

#if KPERF
	kperf_interrupt();
#endif /* KPERF */

	KDBG_RELEASE(MACHDBG_CODE(DBG_MACH_EXCP_INTR, 0) | DBG_FUNC_END,
	    interrupt_num);

	assert(ml_get_interrupts_enabled() == FALSE);

#if DEVELOPMENT || DEBUG
	if (traptrace_index != TRAPTRACE_INVALID_INDEX) {
		traptrace_end(traptrace_index, mach_absolute_time());
	}
#endif
}

static inline void
reset_dr7(void)
{
	long dr7 = 0x400; /* magic dr7 reset value; 32 bit on i386, 64 bit on x86_64 */
	__asm__ volatile ("mov %0,%%dr7" : : "r" (dr7));
}
#if MACH_KDP
unsigned kdp_has_active_watchpoints = 0;
#define NO_WATCHPOINTS (!kdp_has_active_watchpoints)
#else
#define NO_WATCHPOINTS 1
#endif
/*
 * Trap from kernel mode.  Only page-fault errors are recoverable,
 * and then only in special circumstances.  All other errors are
 * fatal.  Return value indicates if trap was handled.
 */

void
kernel_trap(
	x86_saved_state_t       *state,
	uintptr_t *lo_spp)
{
	x86_saved_state64_t     *saved_state;
	int                     code;
	user_addr_t             vaddr;
	int                     type;
	vm_map_t                map = 0;        /* protected by T_PAGE_FAULT */
	kern_return_t           result = KERN_FAILURE;
	kern_return_t           fault_result = KERN_SUCCESS;
	thread_t                thread;
	boolean_t               intr;
	vm_prot_t               prot;
	struct recovery         *rp;
	vm_offset_t             kern_ip;
	int                     is_user;
	int                     trap_pl = get_preemption_level();

	thread = current_thread();

	if (__improbable(is_saved_state32(state))) {
		panic("kernel_trap(%p) with 32-bit state", state);
	}
	saved_state = saved_state64(state);

	/* Record cpu where state was captured */
	saved_state->isf.cpu = cpu_number();

	vaddr = (user_addr_t)saved_state->cr2;
	type  = saved_state->isf.trapno;
	code  = (int)(saved_state->isf.err & 0xffff);
	intr  = (saved_state->isf.rflags & EFL_IF) != 0;        /* state of ints at trap */
	kern_ip = (vm_offset_t)saved_state->isf.rip;

	is_user = (vaddr < VM_MAX_USER_PAGE_ADDRESS);

#if DEVELOPMENT || DEBUG
	uint32_t traptrace_index = traptrace_start(type, kern_ip, mach_absolute_time(), saved_state->rbp);
#endif

#if CONFIG_DTRACE
	/*
	 * Is there a DTrace hook?
	 */
	if (__improbable(tempDTraceTrapHook != NULL)) {
		if (tempDTraceTrapHook(type, state, lo_spp, 0) == KERN_SUCCESS) {
			/*
			 * If it succeeds, we are done...
			 */
			goto common_return;
		}
	}

	/* Handle traps originated from probe context. */
	if (thread != THREAD_NULL && thread->t_dtrace_inprobe) {
		if (dtrace_handle_trap(type, state)) {
			goto common_return;
		}
	}

#endif /* CONFIG_DTRACE */

	/*
	 * we come here with interrupts off as we don't want to recurse
	 * on preemption below.  but we do want to re-enable interrupts
	 * as soon we possibly can to hold latency down
	 */
	if (__improbable(T_PREEMPT == type)) {
		ast_taken_kernel();

		KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
		    (MACHDBG_CODE(DBG_MACH_EXCP_KTRAP_x86, type)) | DBG_FUNC_NONE,
		    0, 0, 0, VM_KERNEL_UNSLIDE(kern_ip), 0);

		goto common_return;
	}

	user_addr_t     kd_vaddr = is_user ? vaddr : VM_KERNEL_UNSLIDE(vaddr);
	KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
	    (MACHDBG_CODE(DBG_MACH_EXCP_KTRAP_x86, type)) | DBG_FUNC_NONE,
	    (unsigned)(kd_vaddr >> 32), (unsigned)kd_vaddr, is_user,
	    VM_KERNEL_UNSLIDE(kern_ip), 0);


	if (T_PAGE_FAULT == type) {
		/*
		 * assume we're faulting in the kernel map
		 */
		map = kernel_map;

		if (__probable((thread != THREAD_NULL) && (thread->map != kernel_map) &&
		    (vaddr < VM_MAX_USER_PAGE_ADDRESS))) {
			/* fault occurred in userspace */
			map = thread->map;

			/* Intercept a potential Supervisor Mode Execute
			 * Protection fault. These criteria identify
			 * both NX faults and SMEP faults, but both
			 * are fatal. We avoid checking PTEs (racy).
			 * (The VM could just redrive a SMEP fault, hence
			 * the intercept).
			 */
			if (__improbable((code == (T_PF_PROT | T_PF_EXECUTE)) &&
			    (pmap_smep_enabled) && (saved_state->isf.rip == vaddr))) {
				goto debugger_entry;
			}

			/*
			 * Additionally check for SMAP faults...
			 * which are characterized by page-present and
			 * the AC bit unset (i.e. not from copyin/out path).
			 */
			if (__improbable(code & T_PF_PROT &&
			    pmap_smap_enabled &&
			    (saved_state->isf.rflags & EFL_AC) == 0)) {
				goto debugger_entry;
			}

			/*
			 * If we're not sharing cr3 with the user
			 * and we faulted in copyio,
			 * then switch cr3 here and dismiss the fault.
			 */
			if (no_shared_cr3 &&
			    (thread->machine.specFlags & CopyIOActive) &&
			    map->pmap->pm_cr3 != get_cr3_base()) {
				pmap_assert(current_cpu_datap()->cpu_pmap_pcid_enabled == FALSE);
				set_cr3_raw(map->pmap->pm_cr3);
				return;
			}
			if (__improbable(vaddr < PAGE_SIZE) &&
			    ((thread->machine.specFlags & CopyIOActive) == 0)) {
				goto debugger_entry;
			}
		}
	}

	(void) ml_set_interrupts_enabled(intr);

	switch (type) {
	case T_NO_FPU:
		fpnoextflt();
		goto common_return;

	case T_FPU_FAULT:
		fpextovrflt();
		goto common_return;

	case T_FLOATING_POINT_ERROR:
		fpexterrflt();
		goto common_return;

	case T_SSE_FLOAT_ERROR:
		fpSSEexterrflt();
		goto common_return;

	case T_INVALID_OPCODE:
		fpUDflt(kern_ip);
		goto debugger_entry;

	case T_DEBUG:
		if ((saved_state->isf.rflags & EFL_TF) == 0 && NO_WATCHPOINTS) {
			/* We've somehow encountered a debug
			 * register match that does not belong
			 * to the kernel debugger.
			 * This isn't supposed to happen.
			 */
			reset_dr7();
			goto common_return;
		}
		goto debugger_entry;
	case T_INT3:
		goto debugger_entry;
	case T_PAGE_FAULT:

#if CONFIG_DTRACE
		if (thread != THREAD_NULL && thread->t_dtrace_inprobe) { /* Executing under dtrace_probe? */
			if (dtrace_tally_fault(vaddr)) { /* Should a fault under dtrace be ignored? */
				/*
				 * DTrace has "anticipated" the possibility of this fault, and has
				 * established the suitable recovery state. Drop down now into the
				 * recovery handling code in "case T_GENERAL_PROTECTION:".
				 */
				goto FALL_THROUGH;
			}
		}
#endif /* CONFIG_DTRACE */

		prot = VM_PROT_READ;

		if (code & T_PF_WRITE) {
			prot |= VM_PROT_WRITE;
		}
		if (code & T_PF_EXECUTE) {
			prot |= VM_PROT_EXECUTE;
		}

		fault_result = result = vm_fault(map,
		    vaddr,
		    prot,
		    FALSE, VM_KERN_MEMORY_NONE,
		    THREAD_UNINT, NULL, 0);

		if (result == KERN_SUCCESS) {
			goto common_return;
		}
		/*
		 * fall through
		 */
#if CONFIG_DTRACE
FALL_THROUGH:
#endif /* CONFIG_DTRACE */

	case T_GENERAL_PROTECTION:
		/*
		 * If there is a failure recovery address
		 * for this fault, go there.
		 */
		for (rp = recover_table; rp < recover_table_end; rp++) {
			if (kern_ip == rp->fault_addr) {
				set_recovery_ip(saved_state, rp->recover_addr);
				goto common_return;
			}
		}

		/*
		 * Check thread recovery address also.
		 */
		if (thread != THREAD_NULL && thread->recover) {
			set_recovery_ip(saved_state, thread->recover);
			thread->recover = 0;
			goto common_return;
		}
		/*
		 * Unanticipated page-fault errors in kernel
		 * should not happen.
		 *
		 * fall through...
		 */
		OS_FALLTHROUGH;
	default:
		/*
		 * Exception 15 is reserved but some chips may generate it
		 * spuriously. Seen at startup on AMD Athlon-64.
		 */
		if (type == 15) {
			kprintf("kernel_trap() ignoring spurious trap 15\n");
			goto common_return;
		}
debugger_entry:
		/* Ensure that the i386_kernel_state at the base of the
		 * current thread's stack (if any) is synchronized with the
		 * context at the moment of the trap, to facilitate
		 * access through the debugger.
		 */
		sync_iss_to_iks(state);
#if  MACH_KDP
		if (kdp_i386_trap(type, saved_state, result, (vm_offset_t)vaddr)) {
			goto common_return;
		}
#endif
	}
	pal_cli();
	panic_trap(saved_state, trap_pl, fault_result);
	/*
	 * NO RETURN
	 */

common_return:
#if DEVELOPMENT || DEBUG
	if (traptrace_index != TRAPTRACE_INVALID_INDEX) {
		traptrace_end(traptrace_index, mach_absolute_time());
	}
#endif
	return;
}

static void
set_recovery_ip(x86_saved_state64_t  *saved_state, vm_offset_t ip)
{
	saved_state->isf.rip = ip;
}

static void
panic_trap(x86_saved_state64_t *regs, uint32_t pl, kern_return_t fault_result)
{
	const char      *trapname = "Unknown";
	pal_cr_t        cr0, cr2, cr3, cr4;
	boolean_t       potential_smep_fault = FALSE, potential_kernel_NX_fault = FALSE;
	boolean_t       potential_smap_fault = FALSE;

	pal_get_control_registers( &cr0, &cr2, &cr3, &cr4 );
	assert(ml_get_interrupts_enabled() == FALSE);
	current_cpu_datap()->cpu_fatal_trap_state = regs;
	/*
	 * Issue an I/O port read if one has been requested - this is an
	 * event logic analyzers can use as a trigger point.
	 */
	panic_notify();

	kprintf("CPU %d panic trap number 0x%x, rip 0x%016llx\n",
	    cpu_number(), regs->isf.trapno, regs->isf.rip);
	kprintf("cr0 0x%016llx cr2 0x%016llx cr3 0x%016llx cr4 0x%016llx\n",
	    cr0, cr2, cr3, cr4);

	if (regs->isf.trapno < TRAP_TYPES) {
		trapname = trap_type[regs->isf.trapno];
	}

	if ((regs->isf.trapno == T_PAGE_FAULT) && (regs->isf.err == (T_PF_PROT | T_PF_EXECUTE)) && (regs->isf.rip == regs->cr2)) {
		if (pmap_smep_enabled && (regs->isf.rip < VM_MAX_USER_PAGE_ADDRESS)) {
			potential_smep_fault = TRUE;
		} else if (regs->isf.rip >= VM_MIN_KERNEL_AND_KEXT_ADDRESS) {
			potential_kernel_NX_fault = TRUE;
		}
	} else if (pmap_smap_enabled &&
	    regs->isf.trapno == T_PAGE_FAULT &&
	    regs->isf.err & T_PF_PROT &&
	    regs->cr2 < VM_MAX_USER_PAGE_ADDRESS &&
	    regs->isf.rip >= VM_MIN_KERNEL_AND_KEXT_ADDRESS) {
		potential_smap_fault = TRUE;
	}

#undef panic
	panic("Kernel trap at 0x%016llx, type %d=%s, registers:\n"
	    "CR0: 0x%016llx, CR2: 0x%016llx, CR3: 0x%016llx, CR4: 0x%016llx\n"
	    "RAX: 0x%016llx, RBX: 0x%016llx, RCX: 0x%016llx, RDX: 0x%016llx\n"
	    "RSP: 0x%016llx, RBP: 0x%016llx, RSI: 0x%016llx, RDI: 0x%016llx\n"
	    "R8:  0x%016llx, R9:  0x%016llx, R10: 0x%016llx, R11: 0x%016llx\n"
	    "R12: 0x%016llx, R13: 0x%016llx, R14: 0x%016llx, R15: 0x%016llx\n"
	    "RFL: 0x%016llx, RIP: 0x%016llx, CS:  0x%016llx, SS:  0x%016llx\n"
	    "Fault CR2: 0x%016llx, Error code: 0x%016llx, Fault CPU: 0x%x%s%s%s%s, PL: %d, VF: %d\n",
	    regs->isf.rip, regs->isf.trapno, trapname,
	    cr0, cr2, cr3, cr4,
	    regs->rax, regs->rbx, regs->rcx, regs->rdx,
	    regs->isf.rsp, regs->rbp, regs->rsi, regs->rdi,
	    regs->r8, regs->r9, regs->r10, regs->r11,
	    regs->r12, regs->r13, regs->r14, regs->r15,
	    regs->isf.rflags, regs->isf.rip, regs->isf.cs & 0xFFFF,
	    regs->isf.ss & 0xFFFF, regs->cr2, regs->isf.err, regs->isf.cpu,
	    virtualized ? " VMM" : "",
	    potential_kernel_NX_fault ? " Kernel NX fault" : "",
	    potential_smep_fault ? " SMEP/User NX fault" : "",
	    potential_smap_fault ? " SMAP fault" : "",
	    pl,
	    fault_result);
}

#if CONFIG_DTRACE
extern kern_return_t dtrace_user_probe(x86_saved_state_t *);
#endif

#if DEBUG
uint32_t fsigs[2];
uint32_t fsigns, fsigcs;
#endif

/*
 *	Trap from user mode.
 */
void
user_trap(
	x86_saved_state_t *saved_state)
{
	int                     exc;
	int                     err;
	mach_exception_code_t   code;
	mach_exception_subcode_t subcode;
	int                     type;
	user_addr_t             vaddr;
	vm_prot_t               prot;
	thread_t                thread = current_thread();
	kern_return_t           kret;
	user_addr_t             rip;
	unsigned long           dr6 = 0; /* 32 bit for i386, 64 bit for x86_64 */
	int                     current_cpu = cpu_number();
#if DEVELOPMENT || DEBUG
	bool                    inspect_cacheline = false;
	uint32_t                traptrace_index;
#endif
	assert((is_saved_state32(saved_state) && !thread_is_64bit_addr(thread)) ||
	    (is_saved_state64(saved_state) && thread_is_64bit_addr(thread)));

	if (is_saved_state64(saved_state)) {
		x86_saved_state64_t     *regs;

		regs = saved_state64(saved_state);

		/* Record cpu where state was captured */
		regs->isf.cpu = current_cpu;

		type = regs->isf.trapno;
		err  = (int)regs->isf.err & 0xffff;
		vaddr = (user_addr_t)regs->cr2;
		rip   = (user_addr_t)regs->isf.rip;
#if DEVELOPMENT || DEBUG
		traptrace_index = traptrace_start(type, rip, mach_absolute_time(), regs->rbp);
#endif
	} else {
		x86_saved_state32_t     *regs;

		regs = saved_state32(saved_state);

		/* Record cpu where state was captured */
		regs->cpu = current_cpu;

		type  = regs->trapno;
		err   = regs->err & 0xffff;
		vaddr = (user_addr_t)regs->cr2;
		rip   = (user_addr_t)regs->eip;
#if DEVELOPMENT || DEBUG
		traptrace_index = traptrace_start(type, rip, mach_absolute_time(), regs->ebp);
#endif
	}

#if DEVELOPMENT || DEBUG
	/*
	 * Copy the cacheline of code into the thread's instruction stream save area
	 * before enabling interrupts (the assumption is that we have not otherwise faulted or
	 * trapped since the original cache line stores).  If the saved code is not valid,
	 * we'll catch it below when we process the copyin() for unhandled faults.
	 */
	if (thread->machine.insn_copy_optout == false &&
	    (type == T_PAGE_FAULT || type == T_INVALID_OPCODE || type == T_GENERAL_PROTECTION)) {
#define CACHELINE_SIZE 64
		THREAD_TO_PCB(thread)->insn_cacheline[CACHELINE_SIZE] = (uint8_t)(rip & (CACHELINE_SIZE - 1));
		bcopy(&cpu_shadowp(current_cpu)->cpu_rtimes[0],
		    &THREAD_TO_PCB(thread)->insn_cacheline[0],
		    sizeof(THREAD_TO_PCB(thread)->insn_cacheline) - 1);
		inspect_cacheline = true;
	}
#endif

	if (type == T_DEBUG) {
		if (thread->machine.ids) {
			unsigned long clear = 0;
			/* Stash and clear this processor's DR6 value, in the event
			 * this was a debug register match
			 */
			__asm__ volatile ("mov %%db6, %0" : "=r" (dr6));
			__asm__ volatile ("mov %0, %%db6" : : "r" (clear));
		}
		/* [Re]Enable LBRs *BEFORE* enabling interrupts to ensure we hit the right CPU */
		i386_lbr_enable();
	}

	pal_sti();

	KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE,
	    (MACHDBG_CODE(DBG_MACH_EXCP_UTRAP_x86, type)) | DBG_FUNC_NONE,
	    (unsigned)(vaddr >> 32), (unsigned)vaddr,
	    (unsigned)(rip >> 32), (unsigned)rip, 0);

	code = 0;
	subcode = 0;
	exc = 0;

#if CONFIG_DTRACE
	/*
	 * DTrace does not consume all user traps, only INT_3's for now.
	 * Avoid needlessly calling tempDTraceTrapHook here, and let the
	 * INT_3 case handle them.
	 */
#endif

	DEBUG_KPRINT_SYSCALL_MASK(1,
	    "user_trap: type=0x%x(%s) err=0x%x cr2=%p rip=%p\n",
	    type, trap_type[type], err, (void *)(long) vaddr, (void *)(long) rip);

	switch (type) {
	case T_DIVIDE_ERROR:
		exc = EXC_ARITHMETIC;
		code = EXC_I386_DIV;
		break;

	case T_DEBUG:
	{
		pcb_t   pcb;
		/*
		 * Update the PCB with this processor's DR6 value
		 * in the event this was a debug register match.
		 */
		pcb = THREAD_TO_PCB(thread);
		if (pcb->ids) {
			/*
			 * We can get and set the status register
			 * in 32-bit mode even on a 64-bit thread
			 * because the high order bits are not
			 * used on x86_64
			 */
			if (thread_is_64bit_addr(thread)) {
				x86_debug_state64_t *ids = pcb->ids;
				ids->dr6 = dr6;
			} else {         /* 32 bit thread */
				x86_debug_state32_t *ids = pcb->ids;
				ids->dr6 = (uint32_t) dr6;
			}
		}
		exc = EXC_BREAKPOINT;
		code = EXC_I386_SGL;
		break;
	}
	case T_INT3:
#if CONFIG_DTRACE
		if (dtrace_user_probe(saved_state) == KERN_SUCCESS) {
			return; /* If it succeeds, we are done... */
		}
#endif
		exc = EXC_BREAKPOINT;
		code = EXC_I386_BPT;
		break;

	case T_OVERFLOW:
		exc = EXC_ARITHMETIC;
		code = EXC_I386_INTO;
		break;

	case T_OUT_OF_BOUNDS:
		exc = EXC_SOFTWARE;
		code = EXC_I386_BOUND;
		break;

	case T_INVALID_OPCODE:
		if (fpUDflt(rip) == 1) {
			exc = EXC_BAD_INSTRUCTION;
			code = EXC_I386_INVOP;
		}
		break;

	case T_NO_FPU:
		fpnoextflt();
		break;

	case T_FPU_FAULT:
		fpextovrflt();
		/*
		 * Raise exception.
		 */
		exc = EXC_BAD_ACCESS;
		code = VM_PROT_READ | VM_PROT_EXECUTE;
		subcode = 0;
		break;

	case T_INVALID_TSS:     /* invalid TSS == iret with NT flag set */
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_INVTSSFLT;
		subcode = err;
		break;

	case T_SEGMENT_NOT_PRESENT:
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_SEGNPFLT;
		subcode = err;
		break;

	case T_STACK_FAULT:
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_STKFLT;
		subcode = err;
		break;

	case T_GENERAL_PROTECTION:
		/*
		 * There's a wide range of circumstances which generate this
		 * class of exception. From user-space, many involve bad
		 * addresses (such as a non-canonical 64-bit address).
		 * So we map this to EXC_BAD_ACCESS (and thereby SIGSEGV).
		 * The trouble is cr2 doesn't contain the faulting address;
		 * we'd need to decode the faulting instruction to really
		 * determine this. We'll leave that to debuggers.
		 * However, attempted execution of privileged instructions
		 * (e.g. cli) also generate GP faults and so we map these to
		 * to EXC_BAD_ACCESS (and thence SIGSEGV) also - rather than
		 * EXC_BAD_INSTRUCTION which is more accurate. We just can't
		 * win!
		 */
		exc = EXC_BAD_ACCESS;
		code = EXC_I386_GPFLT;
		subcode = err;
		break;

	case T_PAGE_FAULT:
	{
		prot = VM_PROT_READ;

		if (err & T_PF_WRITE) {
			prot |= VM_PROT_WRITE;
		}
		if (__improbable(err & T_PF_EXECUTE)) {
			prot |= VM_PROT_EXECUTE;
		}
#if DEVELOPMENT || DEBUG
		uint32_t fsig = 0;
		fsig = thread_fpsimd_hash(thread);
#if DEBUG
		fsigs[0] = fsig;
#endif
#endif
		kret = vm_fault(thread->map,
		    vaddr,
		    prot, FALSE, VM_KERN_MEMORY_NONE,
		    THREAD_ABORTSAFE, NULL, 0);
#if DEVELOPMENT || DEBUG
		if (fsig) {
			uint32_t fsig2 = thread_fpsimd_hash(thread);
#if DEBUG
			fsigcs++;
			fsigs[1] = fsig2;
#endif
			if (fsig != fsig2) {
				panic("FP/SIMD state hash mismatch across fault thread: %p 0x%x->0x%x", thread, fsig, fsig2);
			}
		} else {
#if DEBUG
			fsigns++;
#endif
		}
#endif
		if (__probable((kret == KERN_SUCCESS) || (kret == KERN_ABORTED))) {
			break;
		} else if (__improbable(kret == KERN_FAILURE)) {
			/*
			 * For a user trap, vm_fault() should never return KERN_FAILURE.
			 * If it does, we're leaking preemption disables somewhere in the kernel.
			 */
			panic("vm_fault() KERN_FAILURE from user fault on thread %p", thread);
		}

		/* PAL debug hook (empty on x86) */
		pal_dbg_page_fault(thread, vaddr, kret);
		exc = EXC_BAD_ACCESS;
		code = kret;
		subcode = vaddr;
	}
	break;

	case T_SSE_FLOAT_ERROR:
		fpSSEexterrflt();
		exc = EXC_ARITHMETIC;
		code = EXC_I386_SSEEXTERR;
		subcode = ((struct x86_fx_thread_state *)thread->machine.ifps)->fx_MXCSR;
		break;


	case T_FLOATING_POINT_ERROR:
		fpexterrflt();
		exc = EXC_ARITHMETIC;
		code = EXC_I386_EXTERR;
		subcode = ((struct x86_fx_thread_state *)thread->machine.ifps)->fx_status;
		break;

	case T_DTRACE_RET:
#if CONFIG_DTRACE
		if (dtrace_user_probe(saved_state) == KERN_SUCCESS) {
			return; /* If it succeeds, we are done... */
		}
#endif
		/*
		 * If we get an INT 0x7f when we do not expect to,
		 * treat it as an illegal instruction
		 */
		exc = EXC_BAD_INSTRUCTION;
		code = EXC_I386_INVOP;
		break;

	default:
		panic("Unexpected user trap, type %d", type);
	}

	if (exc != 0) {
		uint16_t cs;
		boolean_t intrs;

		if (is_saved_state64(saved_state)) {
			cs = saved_state64(saved_state)->isf.cs;
		} else {
			cs = saved_state32(saved_state)->cs;
		}

		if (last_branch_support_enabled) {
			intrs = ml_set_interrupts_enabled(FALSE);
			/*
			 * This is a bit racy (it's possible for this thread to migrate to another CPU, then
			 * migrate back, but that seems rather rare in practice), but good enough to ensure
			 * the LBRs are saved before proceeding with exception/signal dispatch.
			 */
			if (current_cpu == cpu_number()) {
				i386_lbr_synch(thread);
			}
			ml_set_interrupts_enabled(intrs);
		}

		/*
		 * Do not try to copyin from the instruction stream if the page fault was due
		 * to an access to rip and was unhandled.
		 * Do not deal with cases when %cs != USER[64]_CS
		 * And of course there's no need to copy the instruction stream if the boot-arg
		 * was set to 0.
		 */
		if (thread->machine.insn_copy_optout == false && insn_copyin_count > 0 &&
		    (cs == USER64_CS || cs == USER_CS) && (type != T_PAGE_FAULT || vaddr != rip)) {
#if DEVELOPMENT || DEBUG
			copy_instruction_stream(thread, rip, type, inspect_cacheline);
#else
			copy_instruction_stream(thread, rip, type);
#endif
		}

#if DEVELOPMENT || DEBUG
		if (traptrace_index != TRAPTRACE_INVALID_INDEX) {
			traptrace_end(traptrace_index, mach_absolute_time());
		}
#endif
		/*
		 * Note: Codepaths that directly return from user_trap() have pending
		 * ASTs processed in locore
		 */
		i386_exception(exc, code, subcode);
		/* NOTREACHED */
	} else {
#if DEVELOPMENT || DEBUG
		if (traptrace_index != TRAPTRACE_INVALID_INDEX) {
			traptrace_end(traptrace_index, mach_absolute_time());
		}
#endif
	}
}

/*
 * Copyin up to x86_INSTRUCTION_STATE_MAX_INSN_BYTES bytes from the page that includes `rip`,
 * ensuring that we stay on the same page, clipping the start or end, as needed.
 * Add the clipped amount back at the start or end, depending on where it fits.
 * Consult the variable populated by the boot-arg `insn_capcnt'
 */
static __attribute__((noinline)) void
copy_instruction_stream(thread_t thread, uint64_t rip, int __unused trap_code
#if DEVELOPMENT || DEBUG
    , bool inspect_cacheline
#endif
    )
{
#if x86_INSTRUCTION_STATE_MAX_INSN_BYTES > 4096
#error x86_INSTRUCTION_STATE_MAX_INSN_BYTES cannot exceed a page in size.
#endif
	pcb_t pcb = THREAD_TO_PCB(thread);
	vm_map_offset_t pagemask = ~vm_map_page_mask(current_map());
	vm_map_offset_t rip_page = rip & pagemask;
	vm_map_offset_t start_addr;
	vm_map_offset_t insn_offset;
	vm_map_offset_t end_addr = rip + (insn_copyin_count / 2);
	void *stack_buffer;
	int copyin_err = 0;
#if defined(MACH_BSD) && (DEVELOPMENT || DEBUG)
	void *procname;
#endif

#if DEVELOPMENT || DEBUG
	assert(insn_copyin_count <= x86_INSTRUCTION_STATE_MAX_INSN_BYTES);
#else
	if (insn_copyin_count > x86_INSTRUCTION_STATE_MAX_INSN_BYTES ||
	    insn_copyin_count < 64 /* CACHELINE_SIZE */) {
		return;
	}
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Walloca"
	stack_buffer = __builtin_alloca(insn_copyin_count);
#pragma clang diagnostic pop

	if (rip >= (insn_copyin_count / 2)) {
		start_addr = rip - (insn_copyin_count / 2);
	} else {
		start_addr = 0;
	}

	if (start_addr < rip_page) {
		insn_offset = (insn_copyin_count / 2) - (rip_page - start_addr);
		end_addr += (rip_page - start_addr);
		start_addr = rip_page;
	} else if (end_addr >= (rip_page + (~pagemask + 1))) {
		start_addr -= (end_addr - (rip_page + (~pagemask + 1))); /* Adjust start address backward */
		/* Adjust instruction offset due to start address change */
		insn_offset = (insn_copyin_count / 2) + (end_addr - (rip_page + (~pagemask + 1)));
		end_addr = rip_page + (~pagemask + 1);  /* clip to the start of the next page (non-inclusive */
	} else {
		insn_offset = insn_copyin_count / 2;
	}

	disable_preemption();   /* Prevent copyin from faulting in the instruction stream */
	if (
#if DEVELOPMENT || DEBUG
		(insnstream_force_cacheline_mismatch < 2) &&
#endif
		((end_addr > start_addr) && (copyin_err = copyin(start_addr, stack_buffer, end_addr - start_addr)) == 0)) {
		enable_preemption();

		if (pcb->insn_state == 0) {
			pcb->insn_state = kalloc(sizeof(x86_instruction_state_t));
		}

		if (pcb->insn_state != 0) {
			bcopy(stack_buffer, pcb->insn_state->insn_bytes, end_addr - start_addr);
			bzero(&pcb->insn_state->insn_bytes[end_addr - start_addr],
			    insn_copyin_count - (end_addr - start_addr));

			pcb->insn_state->insn_stream_valid_bytes = (int)(end_addr - start_addr);
			pcb->insn_state->insn_offset = (int)insn_offset;

#if DEVELOPMENT || DEBUG
			/* Now try to validate the cacheline we read at early-fault time matches the code
			 * copied in. Before we do that, we have to make sure the buffer contains a valid
			 * cacheline by looking for the 2 sentinel values written in the event the cacheline
			 * could not be copied.
			 */
#define CACHELINE_DATA_NOT_PRESENT 0xdeadc0debeefcafeULL
#define CACHELINE_MASK (CACHELINE_SIZE - 1)

			if (inspect_cacheline &&
			    (*(uint64_t *)(uintptr_t)&pcb->insn_cacheline[0] != CACHELINE_DATA_NOT_PRESENT &&
			    *(uint64_t *)(uintptr_t)&pcb->insn_cacheline[8] != CACHELINE_DATA_NOT_PRESENT)) {
				/*
				 * The position of the cacheline in the instruction buffer is at offset
				 * insn_offset - (rip & CACHELINE_MASK)
				 */
				if (__improbable((rip & CACHELINE_MASK) > insn_offset)) {
					printf("thread %p code cacheline @ %p clipped wrt copied-in code (offset %d)\n",
					    thread, (void *)(rip & ~CACHELINE_MASK), (int)(rip & CACHELINE_MASK));
				} else if (bcmp(&pcb->insn_state->insn_bytes[insn_offset - (rip & CACHELINE_MASK)],
				    &pcb->insn_cacheline[0], CACHELINE_SIZE) != 0
				    || insnstream_force_cacheline_mismatch
				    ) {
#if x86_INSTRUCTION_STATE_CACHELINE_SIZE != CACHELINE_SIZE
#error cacheline size mismatch
#endif
					bcopy(&pcb->insn_cacheline[0], &pcb->insn_state->insn_cacheline[0],
					    x86_INSTRUCTION_STATE_CACHELINE_SIZE);
					/* Mark the instruction stream as being out-of-synch */
					pcb->insn_state->out_of_synch = 1;

					printf("thread %p code cacheline @ %p mismatches with copied-in code [trap 0x%x]\n",
					    thread, (void *)(rip & ~CACHELINE_MASK), trap_code);
					for (int i = 0; i < 8; i++) {
						printf("\t[%d] cl=0x%08llx vs. ci=0x%08llx\n", i, *(uint64_t *)(uintptr_t)&pcb->insn_cacheline[i * 8],
						    *(uint64_t *)(uintptr_t)&pcb->insn_state->insn_bytes[(i * 8) + insn_offset - (rip & CACHELINE_MASK)]);
					}
					if (panic_on_cacheline_mismatch) {
						panic("Cacheline mismatch while processing unhandled exception.");
					}
				} else {
					pcb->insn_state->out_of_synch = 0;
				}
			} else if (inspect_cacheline) {
				printf("thread %p could not capture code cacheline at fault IP %p [offset %d]\n",
				    (void *)thread, (void *)rip, (int)(insn_offset - (rip & CACHELINE_MASK)));
				pcb->insn_state->out_of_synch = 0;
			}
#else
			pcb->insn_state->out_of_synch = 0;
#endif /* DEVELOPMENT || DEBUG */

#if defined(MACH_BSD) && (DEVELOPMENT || DEBUG)
			if (panic_on_trap_procname[0] != 0) {
				char procnamebuf[65] = {0};

				if (thread->task->bsd_info != NULL) {
					procname = proc_name_address(thread->task->bsd_info);
					strlcpy(procnamebuf, procname, sizeof(procnamebuf));

					if (strcasecmp(panic_on_trap_procname, procnamebuf) == 0 &&
					    ((1U << trap_code) & panic_on_trap_mask) != 0) {
						panic("Panic requested on trap type 0x%x for process `%s'", trap_code,
						    panic_on_trap_procname);
						/*NORETURN*/
					}
				}
			}
#endif /* MACH_BSD && (DEVELOPMENT || DEBUG) */
		}
	} else {
		enable_preemption();

		pcb->insn_state_copyin_failure_errorcode = copyin_err;
#if DEVELOPMENT || DEBUG
		if (inspect_cacheline && pcb->insn_state == 0) {
			pcb->insn_state = kalloc(sizeof(x86_instruction_state_t));
		}
		if (pcb->insn_state != 0) {
			pcb->insn_state->insn_stream_valid_bytes = 0;
			pcb->insn_state->insn_offset = 0;

			if (inspect_cacheline &&
			    (*(uint64_t *)(uintptr_t)&pcb->insn_cacheline[0] != CACHELINE_DATA_NOT_PRESENT &&
			    *(uint64_t *)(uintptr_t)&pcb->insn_cacheline[8] != CACHELINE_DATA_NOT_PRESENT)) {
				/*
				 * We can still copy the cacheline into the instruction state structure
				 * if it contains valid data
				 */
				pcb->insn_state->out_of_synch = 1;
				bcopy(&pcb->insn_cacheline[0], &pcb->insn_state->insn_cacheline[0],
				    x86_INSTRUCTION_STATE_CACHELINE_SIZE);
			}
		}
#endif /* DEVELOPMENT || DEBUG */
	}
}

/*
 * Handle exceptions for i386.
 *
 * If we are an AT bus machine, we must turn off the AST for a
 * delayed floating-point exception.
 *
 * If we are providing floating-point emulation, we may have
 * to retrieve the real register values from the floating point
 * emulator.
 */
void
i386_exception(
	int     exc,
	mach_exception_code_t code,
	mach_exception_subcode_t subcode)
{
	mach_exception_data_type_t   codes[EXCEPTION_CODE_MAX];

	DEBUG_KPRINT_SYSCALL_MACH("i386_exception: exc=%d code=0x%llx subcode=0x%llx\n",
	    exc, code, subcode);
	codes[0] = code;                /* new exception interface */
	codes[1] = subcode;
	exception_triage(exc, codes, 2);
	/*NOTREACHED*/
}


/* Synchronize a thread's x86_kernel_state (if any) with the given
 * x86_saved_state_t obtained from the trap/IPI handler; called in
 * kernel_trap() prior to entering the debugger, and when receiving
 * an "MP_KDP" IPI. Called with null saved_state if an incoming IPI
 * was detected from the kernel while spinning with interrupts masked.
 */

void
sync_iss_to_iks(x86_saved_state_t *saved_state)
{
	struct x86_kernel_state *iks = NULL;
	vm_offset_t kstack;
	boolean_t record_active_regs = FALSE;

	/* The PAL may have a special way to sync registers */
	if (saved_state && saved_state->flavor == THREAD_STATE_NONE) {
		pal_get_kern_regs( saved_state );
	}

	if (current_thread() != NULL &&
	    (kstack = current_thread()->kernel_stack) != 0) {
		x86_saved_state64_t     *regs = saved_state64(saved_state);

		iks = STACK_IKS(kstack);

		/* Did we take the trap/interrupt in kernel mode? */
		if (saved_state == NULL || /* NULL => polling in kernel */
		    regs == USER_REGS64(current_thread())) {
			record_active_regs = TRUE;
		} else {
			iks->k_rbx = regs->rbx;
			iks->k_rsp = regs->isf.rsp;
			iks->k_rbp = regs->rbp;
			iks->k_r12 = regs->r12;
			iks->k_r13 = regs->r13;
			iks->k_r14 = regs->r14;
			iks->k_r15 = regs->r15;
			iks->k_rip = regs->isf.rip;
		}
	}

	if (record_active_regs == TRUE) {
		/* Show the trap handler path */
		__asm__ volatile ("movq %%rbx, %0" : "=m" (iks->k_rbx));
		__asm__ volatile ("movq %%rsp, %0" : "=m" (iks->k_rsp));
		__asm__ volatile ("movq %%rbp, %0" : "=m" (iks->k_rbp));
		__asm__ volatile ("movq %%r12, %0" : "=m" (iks->k_r12));
		__asm__ volatile ("movq %%r13, %0" : "=m" (iks->k_r13));
		__asm__ volatile ("movq %%r14, %0" : "=m" (iks->k_r14));
		__asm__ volatile ("movq %%r15, %0" : "=m" (iks->k_r15));
		/* "Current" instruction pointer */
		__asm__ volatile ("leaq 1f(%%rip), %%rax; mov %%rax, %0\n1:"
                                  : "=m" (iks->k_rip)
                                  :
                                  : "rax");
	}
}

/*
 * This is used by the NMI interrupt handler (from mp.c) to
 * uncondtionally sync the trap handler context to the IKS
 * irrespective of whether the NMI was fielded in kernel
 * or user space.
 */
void
sync_iss_to_iks_unconditionally(__unused x86_saved_state_t *saved_state)
{
	struct x86_kernel_state *iks;
	vm_offset_t kstack;

	if ((kstack = current_thread()->kernel_stack) != 0) {
		iks = STACK_IKS(kstack);
		/* Display the trap handler path */
		__asm__ volatile ("movq %%rbx, %0" : "=m" (iks->k_rbx));
		__asm__ volatile ("movq %%rsp, %0" : "=m" (iks->k_rsp));
		__asm__ volatile ("movq %%rbp, %0" : "=m" (iks->k_rbp));
		__asm__ volatile ("movq %%r12, %0" : "=m" (iks->k_r12));
		__asm__ volatile ("movq %%r13, %0" : "=m" (iks->k_r13));
		__asm__ volatile ("movq %%r14, %0" : "=m" (iks->k_r14));
		__asm__ volatile ("movq %%r15, %0" : "=m" (iks->k_r15));
		/* "Current" instruction pointer */
		__asm__ volatile ("leaq 1f(%%rip), %%rax; mov %%rax, %0\n1:" : "=m" (iks->k_rip)::"rax");
	}
}

#if DEBUG
#define TERI 1
#endif

#if TERI
extern void     thread_exception_return_internal(void) __dead2;

void
thread_exception_return(void)
{
	thread_t thread = current_thread();
	ml_set_interrupts_enabled(FALSE);
	if (thread_is_64bit_addr(thread) != task_has_64Bit_addr(thread->task)) {
		panic("Task/thread bitness mismatch %p %p, task: %d, thread: %d", thread, thread->task, thread_is_64bit_addr(thread), task_has_64Bit_addr(thread->task));
	}

	if (thread_is_64bit_addr(thread)) {
		if ((gdt_desc_p(USER64_CS)->access & ACC_PL_U) == 0) {
			panic("64-GDT mismatch %p, descriptor: %p", thread, gdt_desc_p(USER64_CS));
		}
	} else {
		if ((gdt_desc_p(USER_CS)->access & ACC_PL_U) == 0) {
			panic("32-GDT mismatch %p, descriptor: %p", thread, gdt_desc_p(USER_CS));
		}
	}
	assert(get_preemption_level() == 0);
	thread_exception_return_internal();
}
#endif
