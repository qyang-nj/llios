/*
 * Copyright (c) 2007-2020 Apple Inc. All rights reserved.
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

#include <debug.h>

#include <types.h>

#include <mach/mach_types.h>
#include <mach/thread_status.h>
#include <mach/vm_types.h>

#include <kern/kern_types.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <kern/mach_param.h>
#include <kern/spl.h>
#include <kern/machine.h>
#include <kern/kpc.h>

#if MONOTONIC
#include <kern/monotonic.h>
#endif /* MONOTONIC */

#include <machine/atomic.h>
#include <arm64/proc_reg.h>
#include <arm64/machine_machdep.h>
#include <arm/cpu_data_internal.h>
#include <arm/machdep_call.h>
#include <arm/misc_protos.h>
#include <arm/cpuid.h>

#include <vm/vm_map.h>
#include <vm/vm_protos.h>

#include <sys/kdebug.h>


extern int debug_task;
extern bool need_wa_rdar_55577508;

/* zone for debug_state area */
ZONE_DECLARE(ads_zone, "arm debug state", sizeof(arm_debug_state_t), ZC_NONE);
ZONE_DECLARE(user_ss_zone, "user save state", sizeof(arm_context_t), ZC_NONE);

/*
 * Routine: consider_machine_collect
 *
 */
void
consider_machine_collect(void)
{
	pmap_gc();
}

/*
 * Routine: consider_machine_adjust
 *
 */
void
consider_machine_adjust(void)
{
}




static inline void
machine_thread_switch_cpu_data(thread_t old, thread_t new)
{
	/*
	 * We build with -fno-strict-aliasing, so the load through temporaries
	 * is required so that this generates a single load / store pair.
	 */
	cpu_data_t *datap = old->machine.CpuDatap;
	vm_offset_t base  = old->machine.pcpu_data_base;

	/* TODO: Should this be ordered? */

	old->machine.CpuDatap = NULL;
	old->machine.pcpu_data_base = 0;

	new->machine.CpuDatap = datap;
	new->machine.pcpu_data_base = base;
}

/**
 * routine: machine_switch_pmap_and_extended_context
 *
 * Helper function used by machine_switch_context and machine_stack_handoff to switch the
 * extended context and switch the pmap if necessary.
 *
 */

static inline void
machine_switch_pmap_and_extended_context(thread_t old, thread_t new)
{
	pmap_t new_pmap;



	new_pmap = new->map->pmap;
	if (old->map->pmap != new_pmap) {
		pmap_switch(new_pmap);
	} else {
		/*
		 * If the thread is preempted while performing cache or TLB maintenance,
		 * it may be migrated to a different CPU between the completion of the relevant
		 * maintenance instruction and the synchronizing DSB.   ARM requires that the
		 * synchronizing DSB must be issued *on the PE that issued the maintenance instruction*
		 * in order to guarantee completion of the instruction and visibility of its effects.
		 * Issue DSB here to enforce that guarantee.  We only do this for the case in which
		 * the pmap isn't changing, as we expect pmap_switch() to issue DSB when it updates
		 * TTBR0.  Note also that cache maintenance may be performed in userspace, so we
		 * cannot further limit this operation e.g. by setting a per-thread flag to indicate
		 * a pending kernel TLB or cache maintenance instruction.
		 */
		__builtin_arm_dsb(DSB_ISH);
	}


	machine_thread_switch_cpu_data(old, new);
}

/*
 * Routine: machine_switch_context
 *
 */
thread_t
machine_switch_context(thread_t old,
    thread_continue_t continuation,
    thread_t new)
{
	thread_t retval;

#if __ARM_PAN_AVAILABLE__
	if (__improbable(__builtin_arm_rsr("pan") == 0)) {
		panic("context switch with PAN disabled");
	}
#endif

#define machine_switch_context_kprintf(x...) \
	/* kprintf("machine_switch_context: " x) */

	if (old == new) {
		panic("machine_switch_context");
	}

	kpc_off_cpu(old);

	machine_switch_pmap_and_extended_context(old, new);

	machine_switch_context_kprintf("old= %x contination = %x new = %x\n", old, continuation, new);

	retval = Switch_context(old, continuation, new);
	assert(retval != NULL);

	return retval;
}

boolean_t
machine_thread_on_core(thread_t thread)
{
	return thread->machine.CpuDatap != NULL;
}


/*
 * Routine: machine_thread_create
 *
 */
kern_return_t
machine_thread_create(thread_t thread,
    task_t task)
{
	arm_context_t *thread_user_ss = NULL;
	kern_return_t result = KERN_SUCCESS;

#define machine_thread_create_kprintf(x...) \
	/* kprintf("machine_thread_create: " x) */

	machine_thread_create_kprintf("thread = %x\n", thread);

	if (current_thread() != thread) {
		thread->machine.CpuDatap = (cpu_data_t *)0;
		// setting this offset will cause trying to use it to panic
		thread->machine.pcpu_data_base = (vm_offset_t)VM_MIN_KERNEL_ADDRESS;
	}
	thread->machine.preemption_count = 0;
	thread->machine.cthread_self = 0;
	thread->machine.kpcb = NULL;
	thread->machine.exception_trace_code = 0;
#if defined(HAS_APPLE_PAC)
	thread->machine.rop_pid = task->rop_pid;
	thread->machine.jop_pid = task->jop_pid;
	thread->machine.disable_user_jop = task->disable_user_jop;
#endif



	if (task != kernel_task) {
		/* If this isn't a kernel thread, we'll have userspace state. */
		thread->machine.contextData = (arm_context_t *)zalloc(user_ss_zone);

		if (!thread->machine.contextData) {
			result = KERN_FAILURE;
			goto done;
		}

		thread->machine.upcb = &thread->machine.contextData->ss;
		thread->machine.uNeon = &thread->machine.contextData->ns;

		if (task_has_64Bit_data(task)) {
			thread->machine.upcb->ash.flavor = ARM_SAVED_STATE64;
			thread->machine.upcb->ash.count = ARM_SAVED_STATE64_COUNT;
			thread->machine.uNeon->nsh.flavor = ARM_NEON_SAVED_STATE64;
			thread->machine.uNeon->nsh.count = ARM_NEON_SAVED_STATE64_COUNT;
		} else {
			thread->machine.upcb->ash.flavor = ARM_SAVED_STATE32;
			thread->machine.upcb->ash.count = ARM_SAVED_STATE32_COUNT;
			thread->machine.uNeon->nsh.flavor = ARM_NEON_SAVED_STATE32;
			thread->machine.uNeon->nsh.count = ARM_NEON_SAVED_STATE32_COUNT;
		}
	} else {
		thread->machine.upcb = NULL;
		thread->machine.uNeon = NULL;
		thread->machine.contextData = NULL;
	}



	bzero(&thread->machine.perfctrl_state, sizeof(thread->machine.perfctrl_state));
	result = machine_thread_state_initialize(thread);

done:
	if (result != KERN_SUCCESS) {
		thread_user_ss = thread->machine.contextData;

		if (thread_user_ss) {
			thread->machine.upcb = NULL;
			thread->machine.uNeon = NULL;
			thread->machine.contextData = NULL;
			zfree(user_ss_zone, thread_user_ss);
		}
	}

	return result;
}

/*
 * Routine: machine_thread_destroy
 *
 */
void
machine_thread_destroy(thread_t thread)
{
	arm_context_t *thread_user_ss;

	if (thread->machine.contextData) {
		/* Disassociate the user save state from the thread before we free it. */
		thread_user_ss = thread->machine.contextData;
		thread->machine.upcb = NULL;
		thread->machine.uNeon = NULL;
		thread->machine.contextData = NULL;


		zfree(user_ss_zone, thread_user_ss);
	}

	if (thread->machine.DebugData != NULL) {
		if (thread->machine.DebugData == getCpuDatap()->cpu_user_debug) {
			arm_debug_set(NULL);
		}

		zfree(ads_zone, thread->machine.DebugData);
	}
}


/*
 * Routine: machine_thread_init
 *
 */
void
machine_thread_init(void)
{
}

/*
 * Routine:	machine_thread_template_init
 *
 */
void
machine_thread_template_init(thread_t __unused thr_template)
{
	/* Nothing to do on this platform. */
}

/*
 * Routine: get_useraddr
 *
 */
user_addr_t
get_useraddr()
{
	return get_saved_state_pc(current_thread()->machine.upcb);
}

/*
 * Routine: machine_stack_detach
 *
 */
vm_offset_t
machine_stack_detach(thread_t thread)
{
	vm_offset_t stack;

	KERNEL_DEBUG(MACHDBG_CODE(DBG_MACH_SCHED, MACH_STACK_DETACH),
	    (uintptr_t)thread_tid(thread), thread->priority, thread->sched_pri, 0, 0);

	stack = thread->kernel_stack;
	thread->kernel_stack = 0;
	thread->machine.kstackptr = 0;

	return stack;
}


/*
 * Routine: machine_stack_attach
 *
 */
void
machine_stack_attach(thread_t thread,
    vm_offset_t stack)
{
	struct arm_kernel_context *context;
	struct arm_kernel_saved_state *savestate;
	struct arm_kernel_neon_saved_state *neon_savestate;
	uint32_t current_el;

#define machine_stack_attach_kprintf(x...) \
	/* kprintf("machine_stack_attach: " x) */

	KERNEL_DEBUG(MACHDBG_CODE(DBG_MACH_SCHED, MACH_STACK_ATTACH),
	    (uintptr_t)thread_tid(thread), thread->priority, thread->sched_pri, 0, 0);

	thread->kernel_stack = stack;
	thread->machine.kstackptr = stack + kernel_stack_size - sizeof(struct thread_kernel_state);
	thread_initialize_kernel_state(thread);

	machine_stack_attach_kprintf("kstackptr: %lx\n", (vm_address_t)thread->machine.kstackptr);

	current_el = (uint32_t) __builtin_arm_rsr64("CurrentEL");
	context = &((thread_kernel_state_t) thread->machine.kstackptr)->machine;
	savestate = &context->ss;
	savestate->fp = 0;
	savestate->sp = thread->machine.kstackptr;

	/*
	 * The PC and CPSR of the kernel stack saved state are never used by context switch
	 * code, and should never be used on exception return either. We're going to poison
	 * these values to ensure they never get copied to the exception frame and used to
	 * hijack control flow or privilege level on exception return.
	 */

	const uint32_t default_cpsr = PSR64_KERNEL_POISON;
#if defined(HAS_APPLE_PAC)
	/* Sign the initial kernel stack saved state */
	boolean_t intr = ml_set_interrupts_enabled(FALSE);
	asm volatile (
                "mov	x0, %[ss]"                              "\n"

                "mov	x1, xzr"                                "\n"
                "str	x1, [x0, %[SS64_PC]]"                   "\n"

                "mov	x2, %[default_cpsr_lo]"                 "\n"
                "movk	x2, %[default_cpsr_hi], lsl #16"        "\n"
                "str	w2, [x0, %[SS64_CPSR]]"                 "\n"

                "adrp	x3, _thread_continue@page"              "\n"
                "add	x3, x3, _thread_continue@pageoff"       "\n"
                "str	x3, [x0, %[SS64_LR]]"                   "\n"

                "mov	x4, xzr"                                "\n"
                "mov	x5, xzr"                                "\n"
                "stp	x4, x5, [x0, %[SS64_X16]]"              "\n"

                "mov	x6, lr"                                 "\n"
                "bl	_ml_sign_kernel_thread_state"                   "\n"
                "mov	lr, x6"                                 "\n"
                :
                : [ss]                  "r"(&context->ss),
                  [default_cpsr_lo]     "M"(default_cpsr & 0xFFFF),
                  [default_cpsr_hi]     "M"(default_cpsr >> 16),
                  [SS64_X16]            "i"(offsetof(struct arm_kernel_saved_state, x[0])),
                  [SS64_PC]             "i"(offsetof(struct arm_kernel_saved_state, pc)),
                  [SS64_CPSR]           "i"(offsetof(struct arm_kernel_saved_state, cpsr)),
                  [SS64_LR]             "i"(offsetof(struct arm_kernel_saved_state, lr))
                : "x0", "x1", "x2", "x3", "x4", "x5", "x6"
        );
	ml_set_interrupts_enabled(intr);
#else
	savestate->lr = (uintptr_t)thread_continue;
	savestate->cpsr = default_cpsr;
	savestate->pc = 0;
#endif /* defined(HAS_APPLE_PAC) */
	neon_savestate = &context->ns;
	neon_savestate->fpcr = FPCR_DEFAULT;
	machine_stack_attach_kprintf("thread = %p pc = %llx, sp = %llx\n", thread, savestate->lr, savestate->sp);
}


/*
 * Routine: machine_stack_handoff
 *
 */
void
machine_stack_handoff(thread_t old,
    thread_t new)
{
	vm_offset_t  stack;

#if __ARM_PAN_AVAILABLE__
	if (__improbable(__builtin_arm_rsr("pan") == 0)) {
		panic("stack handoff with PAN disabled");
	}
#endif

	kpc_off_cpu(old);

	stack = machine_stack_detach(old);
	new->kernel_stack = stack;
	new->machine.kstackptr = stack + kernel_stack_size - sizeof(struct thread_kernel_state);
	if (stack == old->reserved_stack) {
		assert(new->reserved_stack);
		old->reserved_stack = new->reserved_stack;
		new->reserved_stack = stack;
	}

	machine_switch_pmap_and_extended_context(old, new);

	machine_set_current_thread(new);
	thread_initialize_kernel_state(new);
}


/*
 * Routine: call_continuation
 *
 */
void
call_continuation(thread_continue_t continuation,
    void *parameter,
    wait_result_t wresult,
    boolean_t enable_interrupts)
{
#define call_continuation_kprintf(x...) \
	/* kprintf("call_continuation_kprintf:" x) */

	call_continuation_kprintf("thread = %p continuation = %p, stack = %p\n", current_thread(), continuation, current_thread()->machine.kstackptr);
	Call_continuation(continuation, parameter, wresult, enable_interrupts);
}

#define SET_DBGBCRn(n, value, accum) \
	__asm__ volatile( \
	        "msr DBGBCR" #n "_EL1, %[val]\n" \
	        "orr %[result], %[result], %[val]\n" \
	        : [result] "+r"(accum) : [val] "r"((value)))

#define SET_DBGBVRn(n, value) \
	__asm__ volatile("msr DBGBVR" #n "_EL1, %0" : : "r"(value))

#define SET_DBGWCRn(n, value, accum) \
	__asm__ volatile( \
	        "msr DBGWCR" #n "_EL1, %[val]\n" \
	        "orr %[result], %[result], %[val]\n" \
	        : [result] "+r"(accum) : [val] "r"((value)))

#define SET_DBGWVRn(n, value) \
	__asm__ volatile("msr DBGWVR" #n "_EL1, %0" : : "r"(value))

void
arm_debug_set32(arm_debug_state_t *debug_state)
{
	struct cpu_data *  cpu_data_ptr;
	arm_debug_info_t * debug_info    = arm_debug_info();
	boolean_t          intr;
	arm_debug_state_t  off_state;
	uint64_t           all_ctrls = 0;

	intr = ml_set_interrupts_enabled(FALSE);
	cpu_data_ptr = getCpuDatap();

	// Set current user debug
	cpu_data_ptr->cpu_user_debug = debug_state;

	if (NULL == debug_state) {
		bzero(&off_state, sizeof(off_state));
		debug_state = &off_state;
	}

	switch (debug_info->num_breakpoint_pairs) {
	case 16:
		SET_DBGBVRn(15, (uint64_t)debug_state->uds.ds32.bvr[15]);
		SET_DBGBCRn(15, (uint64_t)debug_state->uds.ds32.bcr[15], all_ctrls);
		OS_FALLTHROUGH;
	case 15:
		SET_DBGBVRn(14, (uint64_t)debug_state->uds.ds32.bvr[14]);
		SET_DBGBCRn(14, (uint64_t)debug_state->uds.ds32.bcr[14], all_ctrls);
		OS_FALLTHROUGH;
	case 14:
		SET_DBGBVRn(13, (uint64_t)debug_state->uds.ds32.bvr[13]);
		SET_DBGBCRn(13, (uint64_t)debug_state->uds.ds32.bcr[13], all_ctrls);
		OS_FALLTHROUGH;
	case 13:
		SET_DBGBVRn(12, (uint64_t)debug_state->uds.ds32.bvr[12]);
		SET_DBGBCRn(12, (uint64_t)debug_state->uds.ds32.bcr[12], all_ctrls);
		OS_FALLTHROUGH;
	case 12:
		SET_DBGBVRn(11, (uint64_t)debug_state->uds.ds32.bvr[11]);
		SET_DBGBCRn(11, (uint64_t)debug_state->uds.ds32.bcr[11], all_ctrls);
		OS_FALLTHROUGH;
	case 11:
		SET_DBGBVRn(10, (uint64_t)debug_state->uds.ds32.bvr[10]);
		SET_DBGBCRn(10, (uint64_t)debug_state->uds.ds32.bcr[10], all_ctrls);
		OS_FALLTHROUGH;
	case 10:
		SET_DBGBVRn(9, (uint64_t)debug_state->uds.ds32.bvr[9]);
		SET_DBGBCRn(9, (uint64_t)debug_state->uds.ds32.bcr[9], all_ctrls);
		OS_FALLTHROUGH;
	case 9:
		SET_DBGBVRn(8, (uint64_t)debug_state->uds.ds32.bvr[8]);
		SET_DBGBCRn(8, (uint64_t)debug_state->uds.ds32.bcr[8], all_ctrls);
		OS_FALLTHROUGH;
	case 8:
		SET_DBGBVRn(7, (uint64_t)debug_state->uds.ds32.bvr[7]);
		SET_DBGBCRn(7, (uint64_t)debug_state->uds.ds32.bcr[7], all_ctrls);
		OS_FALLTHROUGH;
	case 7:
		SET_DBGBVRn(6, (uint64_t)debug_state->uds.ds32.bvr[6]);
		SET_DBGBCRn(6, (uint64_t)debug_state->uds.ds32.bcr[6], all_ctrls);
		OS_FALLTHROUGH;
	case 6:
		SET_DBGBVRn(5, (uint64_t)debug_state->uds.ds32.bvr[5]);
		SET_DBGBCRn(5, (uint64_t)debug_state->uds.ds32.bcr[5], all_ctrls);
		OS_FALLTHROUGH;
	case 5:
		SET_DBGBVRn(4, (uint64_t)debug_state->uds.ds32.bvr[4]);
		SET_DBGBCRn(4, (uint64_t)debug_state->uds.ds32.bcr[4], all_ctrls);
		OS_FALLTHROUGH;
	case 4:
		SET_DBGBVRn(3, (uint64_t)debug_state->uds.ds32.bvr[3]);
		SET_DBGBCRn(3, (uint64_t)debug_state->uds.ds32.bcr[3], all_ctrls);
		OS_FALLTHROUGH;
	case 3:
		SET_DBGBVRn(2, (uint64_t)debug_state->uds.ds32.bvr[2]);
		SET_DBGBCRn(2, (uint64_t)debug_state->uds.ds32.bcr[2], all_ctrls);
		OS_FALLTHROUGH;
	case 2:
		SET_DBGBVRn(1, (uint64_t)debug_state->uds.ds32.bvr[1]);
		SET_DBGBCRn(1, (uint64_t)debug_state->uds.ds32.bcr[1], all_ctrls);
		OS_FALLTHROUGH;
	case 1:
		SET_DBGBVRn(0, (uint64_t)debug_state->uds.ds32.bvr[0]);
		SET_DBGBCRn(0, (uint64_t)debug_state->uds.ds32.bcr[0], all_ctrls);
		OS_FALLTHROUGH;
	default:
		break;
	}

	switch (debug_info->num_watchpoint_pairs) {
	case 16:
		SET_DBGWVRn(15, (uint64_t)debug_state->uds.ds32.wvr[15]);
		SET_DBGWCRn(15, (uint64_t)debug_state->uds.ds32.wcr[15], all_ctrls);
		OS_FALLTHROUGH;
	case 15:
		SET_DBGWVRn(14, (uint64_t)debug_state->uds.ds32.wvr[14]);
		SET_DBGWCRn(14, (uint64_t)debug_state->uds.ds32.wcr[14], all_ctrls);
		OS_FALLTHROUGH;
	case 14:
		SET_DBGWVRn(13, (uint64_t)debug_state->uds.ds32.wvr[13]);
		SET_DBGWCRn(13, (uint64_t)debug_state->uds.ds32.wcr[13], all_ctrls);
		OS_FALLTHROUGH;
	case 13:
		SET_DBGWVRn(12, (uint64_t)debug_state->uds.ds32.wvr[12]);
		SET_DBGWCRn(12, (uint64_t)debug_state->uds.ds32.wcr[12], all_ctrls);
		OS_FALLTHROUGH;
	case 12:
		SET_DBGWVRn(11, (uint64_t)debug_state->uds.ds32.wvr[11]);
		SET_DBGWCRn(11, (uint64_t)debug_state->uds.ds32.wcr[11], all_ctrls);
		OS_FALLTHROUGH;
	case 11:
		SET_DBGWVRn(10, (uint64_t)debug_state->uds.ds32.wvr[10]);
		SET_DBGWCRn(10, (uint64_t)debug_state->uds.ds32.wcr[10], all_ctrls);
		OS_FALLTHROUGH;
	case 10:
		SET_DBGWVRn(9, (uint64_t)debug_state->uds.ds32.wvr[9]);
		SET_DBGWCRn(9, (uint64_t)debug_state->uds.ds32.wcr[9], all_ctrls);
		OS_FALLTHROUGH;
	case 9:
		SET_DBGWVRn(8, (uint64_t)debug_state->uds.ds32.wvr[8]);
		SET_DBGWCRn(8, (uint64_t)debug_state->uds.ds32.wcr[8], all_ctrls);
		OS_FALLTHROUGH;
	case 8:
		SET_DBGWVRn(7, (uint64_t)debug_state->uds.ds32.wvr[7]);
		SET_DBGWCRn(7, (uint64_t)debug_state->uds.ds32.wcr[7], all_ctrls);
		OS_FALLTHROUGH;
	case 7:
		SET_DBGWVRn(6, (uint64_t)debug_state->uds.ds32.wvr[6]);
		SET_DBGWCRn(6, (uint64_t)debug_state->uds.ds32.wcr[6], all_ctrls);
		OS_FALLTHROUGH;
	case 6:
		SET_DBGWVRn(5, (uint64_t)debug_state->uds.ds32.wvr[5]);
		SET_DBGWCRn(5, (uint64_t)debug_state->uds.ds32.wcr[5], all_ctrls);
		OS_FALLTHROUGH;
	case 5:
		SET_DBGWVRn(4, (uint64_t)debug_state->uds.ds32.wvr[4]);
		SET_DBGWCRn(4, (uint64_t)debug_state->uds.ds32.wcr[4], all_ctrls);
		OS_FALLTHROUGH;
	case 4:
		SET_DBGWVRn(3, (uint64_t)debug_state->uds.ds32.wvr[3]);
		SET_DBGWCRn(3, (uint64_t)debug_state->uds.ds32.wcr[3], all_ctrls);
		OS_FALLTHROUGH;
	case 3:
		SET_DBGWVRn(2, (uint64_t)debug_state->uds.ds32.wvr[2]);
		SET_DBGWCRn(2, (uint64_t)debug_state->uds.ds32.wcr[2], all_ctrls);
		OS_FALLTHROUGH;
	case 2:
		SET_DBGWVRn(1, (uint64_t)debug_state->uds.ds32.wvr[1]);
		SET_DBGWCRn(1, (uint64_t)debug_state->uds.ds32.wcr[1], all_ctrls);
		OS_FALLTHROUGH;
	case 1:
		SET_DBGWVRn(0, (uint64_t)debug_state->uds.ds32.wvr[0]);
		SET_DBGWCRn(0, (uint64_t)debug_state->uds.ds32.wcr[0], all_ctrls);
		OS_FALLTHROUGH;
	default:
		break;
	}

#if defined(CONFIG_KERNEL_INTEGRITY)
	if ((all_ctrls & (ARM_DBG_CR_MODE_CONTROL_PRIVILEGED | ARM_DBG_CR_HIGHER_MODE_ENABLE)) != 0) {
		panic("sorry, self-hosted debug is not supported: 0x%llx", all_ctrls);
	}
#endif

	/*
	 * Breakpoint/Watchpoint Enable
	 */
	if (all_ctrls != 0) {
		update_mdscr(0, 0x8000); // MDSCR_EL1[MDE]
	} else {
		update_mdscr(0x8000, 0);
	}

	/*
	 * Software debug single step enable
	 */
	if (debug_state->uds.ds32.mdscr_el1 & 0x1) {
		update_mdscr(0x8000, 1); // ~MDE | SS : no brk/watch while single stepping (which we've set)

		mask_saved_state_cpsr(current_thread()->machine.upcb, PSR64_SS, 0);
	} else {
		update_mdscr(0x1, 0);

#if SINGLE_STEP_RETIRE_ERRATA
		// Workaround for radar 20619637
		__builtin_arm_isb(ISB_SY);
#endif
	}

	(void) ml_set_interrupts_enabled(intr);
}

void
arm_debug_set64(arm_debug_state_t *debug_state)
{
	struct cpu_data *  cpu_data_ptr;
	arm_debug_info_t * debug_info    = arm_debug_info();
	boolean_t          intr;
	arm_debug_state_t  off_state;
	uint64_t           all_ctrls = 0;

	intr = ml_set_interrupts_enabled(FALSE);
	cpu_data_ptr = getCpuDatap();

	// Set current user debug
	cpu_data_ptr->cpu_user_debug = debug_state;

	if (NULL == debug_state) {
		bzero(&off_state, sizeof(off_state));
		debug_state = &off_state;
	}

	switch (debug_info->num_breakpoint_pairs) {
	case 16:
		SET_DBGBVRn(15, debug_state->uds.ds64.bvr[15]);
		SET_DBGBCRn(15, (uint64_t)debug_state->uds.ds64.bcr[15], all_ctrls);
		OS_FALLTHROUGH;
	case 15:
		SET_DBGBVRn(14, debug_state->uds.ds64.bvr[14]);
		SET_DBGBCRn(14, (uint64_t)debug_state->uds.ds64.bcr[14], all_ctrls);
		OS_FALLTHROUGH;
	case 14:
		SET_DBGBVRn(13, debug_state->uds.ds64.bvr[13]);
		SET_DBGBCRn(13, (uint64_t)debug_state->uds.ds64.bcr[13], all_ctrls);
		OS_FALLTHROUGH;
	case 13:
		SET_DBGBVRn(12, debug_state->uds.ds64.bvr[12]);
		SET_DBGBCRn(12, (uint64_t)debug_state->uds.ds64.bcr[12], all_ctrls);
		OS_FALLTHROUGH;
	case 12:
		SET_DBGBVRn(11, debug_state->uds.ds64.bvr[11]);
		SET_DBGBCRn(11, (uint64_t)debug_state->uds.ds64.bcr[11], all_ctrls);
		OS_FALLTHROUGH;
	case 11:
		SET_DBGBVRn(10, debug_state->uds.ds64.bvr[10]);
		SET_DBGBCRn(10, (uint64_t)debug_state->uds.ds64.bcr[10], all_ctrls);
		OS_FALLTHROUGH;
	case 10:
		SET_DBGBVRn(9, debug_state->uds.ds64.bvr[9]);
		SET_DBGBCRn(9, (uint64_t)debug_state->uds.ds64.bcr[9], all_ctrls);
		OS_FALLTHROUGH;
	case 9:
		SET_DBGBVRn(8, debug_state->uds.ds64.bvr[8]);
		SET_DBGBCRn(8, (uint64_t)debug_state->uds.ds64.bcr[8], all_ctrls);
		OS_FALLTHROUGH;
	case 8:
		SET_DBGBVRn(7, debug_state->uds.ds64.bvr[7]);
		SET_DBGBCRn(7, (uint64_t)debug_state->uds.ds64.bcr[7], all_ctrls);
		OS_FALLTHROUGH;
	case 7:
		SET_DBGBVRn(6, debug_state->uds.ds64.bvr[6]);
		SET_DBGBCRn(6, (uint64_t)debug_state->uds.ds64.bcr[6], all_ctrls);
		OS_FALLTHROUGH;
	case 6:
		SET_DBGBVRn(5, debug_state->uds.ds64.bvr[5]);
		SET_DBGBCRn(5, (uint64_t)debug_state->uds.ds64.bcr[5], all_ctrls);
		OS_FALLTHROUGH;
	case 5:
		SET_DBGBVRn(4, debug_state->uds.ds64.bvr[4]);
		SET_DBGBCRn(4, (uint64_t)debug_state->uds.ds64.bcr[4], all_ctrls);
		OS_FALLTHROUGH;
	case 4:
		SET_DBGBVRn(3, debug_state->uds.ds64.bvr[3]);
		SET_DBGBCRn(3, (uint64_t)debug_state->uds.ds64.bcr[3], all_ctrls);
		OS_FALLTHROUGH;
	case 3:
		SET_DBGBVRn(2, debug_state->uds.ds64.bvr[2]);
		SET_DBGBCRn(2, (uint64_t)debug_state->uds.ds64.bcr[2], all_ctrls);
		OS_FALLTHROUGH;
	case 2:
		SET_DBGBVRn(1, debug_state->uds.ds64.bvr[1]);
		SET_DBGBCRn(1, (uint64_t)debug_state->uds.ds64.bcr[1], all_ctrls);
		OS_FALLTHROUGH;
	case 1:
		SET_DBGBVRn(0, debug_state->uds.ds64.bvr[0]);
		SET_DBGBCRn(0, (uint64_t)debug_state->uds.ds64.bcr[0], all_ctrls);
		OS_FALLTHROUGH;
	default:
		break;
	}

	switch (debug_info->num_watchpoint_pairs) {
	case 16:
		SET_DBGWVRn(15, debug_state->uds.ds64.wvr[15]);
		SET_DBGWCRn(15, (uint64_t)debug_state->uds.ds64.wcr[15], all_ctrls);
		OS_FALLTHROUGH;
	case 15:
		SET_DBGWVRn(14, debug_state->uds.ds64.wvr[14]);
		SET_DBGWCRn(14, (uint64_t)debug_state->uds.ds64.wcr[14], all_ctrls);
		OS_FALLTHROUGH;
	case 14:
		SET_DBGWVRn(13, debug_state->uds.ds64.wvr[13]);
		SET_DBGWCRn(13, (uint64_t)debug_state->uds.ds64.wcr[13], all_ctrls);
		OS_FALLTHROUGH;
	case 13:
		SET_DBGWVRn(12, debug_state->uds.ds64.wvr[12]);
		SET_DBGWCRn(12, (uint64_t)debug_state->uds.ds64.wcr[12], all_ctrls);
		OS_FALLTHROUGH;
	case 12:
		SET_DBGWVRn(11, debug_state->uds.ds64.wvr[11]);
		SET_DBGWCRn(11, (uint64_t)debug_state->uds.ds64.wcr[11], all_ctrls);
		OS_FALLTHROUGH;
	case 11:
		SET_DBGWVRn(10, debug_state->uds.ds64.wvr[10]);
		SET_DBGWCRn(10, (uint64_t)debug_state->uds.ds64.wcr[10], all_ctrls);
		OS_FALLTHROUGH;
	case 10:
		SET_DBGWVRn(9, debug_state->uds.ds64.wvr[9]);
		SET_DBGWCRn(9, (uint64_t)debug_state->uds.ds64.wcr[9], all_ctrls);
		OS_FALLTHROUGH;
	case 9:
		SET_DBGWVRn(8, debug_state->uds.ds64.wvr[8]);
		SET_DBGWCRn(8, (uint64_t)debug_state->uds.ds64.wcr[8], all_ctrls);
		OS_FALLTHROUGH;
	case 8:
		SET_DBGWVRn(7, debug_state->uds.ds64.wvr[7]);
		SET_DBGWCRn(7, (uint64_t)debug_state->uds.ds64.wcr[7], all_ctrls);
		OS_FALLTHROUGH;
	case 7:
		SET_DBGWVRn(6, debug_state->uds.ds64.wvr[6]);
		SET_DBGWCRn(6, (uint64_t)debug_state->uds.ds64.wcr[6], all_ctrls);
		OS_FALLTHROUGH;
	case 6:
		SET_DBGWVRn(5, debug_state->uds.ds64.wvr[5]);
		SET_DBGWCRn(5, (uint64_t)debug_state->uds.ds64.wcr[5], all_ctrls);
		OS_FALLTHROUGH;
	case 5:
		SET_DBGWVRn(4, debug_state->uds.ds64.wvr[4]);
		SET_DBGWCRn(4, (uint64_t)debug_state->uds.ds64.wcr[4], all_ctrls);
		OS_FALLTHROUGH;
	case 4:
		SET_DBGWVRn(3, debug_state->uds.ds64.wvr[3]);
		SET_DBGWCRn(3, (uint64_t)debug_state->uds.ds64.wcr[3], all_ctrls);
		OS_FALLTHROUGH;
	case 3:
		SET_DBGWVRn(2, debug_state->uds.ds64.wvr[2]);
		SET_DBGWCRn(2, (uint64_t)debug_state->uds.ds64.wcr[2], all_ctrls);
		OS_FALLTHROUGH;
	case 2:
		SET_DBGWVRn(1, debug_state->uds.ds64.wvr[1]);
		SET_DBGWCRn(1, (uint64_t)debug_state->uds.ds64.wcr[1], all_ctrls);
		OS_FALLTHROUGH;
	case 1:
		SET_DBGWVRn(0, debug_state->uds.ds64.wvr[0]);
		SET_DBGWCRn(0, (uint64_t)debug_state->uds.ds64.wcr[0], all_ctrls);
		OS_FALLTHROUGH;
	default:
		break;
	}

#if defined(CONFIG_KERNEL_INTEGRITY)
	if ((all_ctrls & (ARM_DBG_CR_MODE_CONTROL_PRIVILEGED | ARM_DBG_CR_HIGHER_MODE_ENABLE)) != 0) {
		panic("sorry, self-hosted debug is not supported: 0x%llx", all_ctrls);
	}
#endif

	/*
	 * Breakpoint/Watchpoint Enable
	 */
	if (all_ctrls != 0) {
		update_mdscr(0, 0x8000); // MDSCR_EL1[MDE]
	} else {
		update_mdscr(0x8000, 0);
	}

	/*
	 * Software debug single step enable
	 */
	if (debug_state->uds.ds64.mdscr_el1 & 0x1) {
		update_mdscr(0x8000, 1); // ~MDE | SS : no brk/watch while single stepping (which we've set)

		mask_saved_state_cpsr(current_thread()->machine.upcb, PSR64_SS, 0);
	} else {
		update_mdscr(0x1, 0);

#if SINGLE_STEP_RETIRE_ERRATA
		// Workaround for radar 20619637
		__builtin_arm_isb(ISB_SY);
#endif
	}

	(void) ml_set_interrupts_enabled(intr);
}

void
arm_debug_set(arm_debug_state_t *debug_state)
{
	if (debug_state) {
		switch (debug_state->dsh.flavor) {
		case ARM_DEBUG_STATE32:
			arm_debug_set32(debug_state);
			break;
		case ARM_DEBUG_STATE64:
			arm_debug_set64(debug_state);
			break;
		default:
			panic("arm_debug_set");
			break;
		}
	} else {
		if (thread_is_64bit_data(current_thread())) {
			arm_debug_set64(debug_state);
		} else {
			arm_debug_set32(debug_state);
		}
	}
}

#define VM_MAX_ADDRESS32          ((vm_address_t) 0x80000000)
boolean_t
debug_legacy_state_is_valid(arm_legacy_debug_state_t *debug_state)
{
	arm_debug_info_t *debug_info = arm_debug_info();
	uint32_t i;
	for (i = 0; i < debug_info->num_breakpoint_pairs; i++) {
		if (0 != debug_state->bcr[i] && VM_MAX_ADDRESS32 <= debug_state->bvr[i]) {
			return FALSE;
		}
	}

	for (i = 0; i < debug_info->num_watchpoint_pairs; i++) {
		if (0 != debug_state->wcr[i] && VM_MAX_ADDRESS32 <= debug_state->wvr[i]) {
			return FALSE;
		}
	}
	return TRUE;
}

boolean_t
debug_state_is_valid32(arm_debug_state32_t *debug_state)
{
	arm_debug_info_t *debug_info = arm_debug_info();
	uint32_t i;
	for (i = 0; i < debug_info->num_breakpoint_pairs; i++) {
		if (0 != debug_state->bcr[i] && VM_MAX_ADDRESS32 <= debug_state->bvr[i]) {
			return FALSE;
		}
	}

	for (i = 0; i < debug_info->num_watchpoint_pairs; i++) {
		if (0 != debug_state->wcr[i] && VM_MAX_ADDRESS32 <= debug_state->wvr[i]) {
			return FALSE;
		}
	}
	return TRUE;
}

boolean_t
debug_state_is_valid64(arm_debug_state64_t *debug_state)
{
	arm_debug_info_t *debug_info = arm_debug_info();
	uint32_t i;
	for (i = 0; i < debug_info->num_breakpoint_pairs; i++) {
		if (0 != debug_state->bcr[i] && MACH_VM_MAX_ADDRESS <= debug_state->bvr[i]) {
			return FALSE;
		}
	}

	for (i = 0; i < debug_info->num_watchpoint_pairs; i++) {
		if (0 != debug_state->wcr[i] && MACH_VM_MAX_ADDRESS <= debug_state->wvr[i]) {
			return FALSE;
		}
	}
	return TRUE;
}

/*
 * Duplicate one arm_debug_state_t to another.  "all" parameter
 * is ignored in the case of ARM -- Is this the right assumption?
 */
void
copy_legacy_debug_state(arm_legacy_debug_state_t * src,
    arm_legacy_debug_state_t * target,
    __unused boolean_t         all)
{
	bcopy(src, target, sizeof(arm_legacy_debug_state_t));
}

void
copy_debug_state32(arm_debug_state32_t * src,
    arm_debug_state32_t * target,
    __unused boolean_t    all)
{
	bcopy(src, target, sizeof(arm_debug_state32_t));
}

void
copy_debug_state64(arm_debug_state64_t * src,
    arm_debug_state64_t * target,
    __unused boolean_t    all)
{
	bcopy(src, target, sizeof(arm_debug_state64_t));
}

kern_return_t
machine_thread_set_tsd_base(thread_t         thread,
    mach_vm_offset_t tsd_base)
{
	if (thread->task == kernel_task) {
		return KERN_INVALID_ARGUMENT;
	}

	if (tsd_base & MACHDEP_CPUNUM_MASK) {
		return KERN_INVALID_ARGUMENT;
	}

	if (thread_is_64bit_addr(thread)) {
		if (tsd_base > vm_map_max(thread->map)) {
			tsd_base = 0ULL;
		}
	} else {
		if (tsd_base > UINT32_MAX) {
			tsd_base = 0ULL;
		}
	}

	thread->machine.cthread_self = tsd_base;

	/* For current thread, make the TSD base active immediately */
	if (thread == current_thread()) {
		uint64_t cpunum, tpidrro_el0;

		mp_disable_preemption();
		tpidrro_el0 = get_tpidrro();
		cpunum = tpidrro_el0 & (MACHDEP_CPUNUM_MASK);
		set_tpidrro(tsd_base | cpunum);
		mp_enable_preemption();
	}

	return KERN_SUCCESS;
}

void
machine_tecs(__unused thread_t thr)
{
}

int
machine_csv(__unused cpuvn_e cve)
{
	return 0;
}

#if __ARM_ARCH_8_5__
void
arm_context_switch_requires_sync()
{
	current_cpu_datap()->sync_on_cswitch = 1;
}
#endif

#if __has_feature(ptrauth_calls)
boolean_t
arm_user_jop_disabled(void)
{
	return FALSE;
}
#endif /* __has_feature(ptrauth_calls) */
