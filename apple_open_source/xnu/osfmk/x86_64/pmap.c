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
 *	File:	pmap.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *	(These guys wrote the Vax version)
 *
 *	Physical Map management code for Intel i386, i486, and i860.
 *
 *	Manages physical address maps.
 *
 *	In addition to hardware address maps, this
 *	module is called upon to provide software-use-only
 *	maps which may or may not be stored in the same
 *	form as hardware maps.  These pseudo-maps are
 *	used to store intermediate results from copy
 *	operations to and from address spaces.
 *
 *	Since the information managed by this module is
 *	also stored by the logical address mapping module,
 *	this module may throw away valid virtual-to-physical
 *	mappings at almost any time.  However, invalidations
 *	of virtual-to-physical mappings must be done as
 *	requested.
 *
 *	In order to cope with hardware architectures which
 *	make virtual-to-physical map invalidates expensive,
 *	this module may delay invalidate or reduced protection
 *	operations until such time as they are actually
 *	necessary.  This module is given full information as
 *	to which processors are currently using which maps,
 *	and to when physical maps must be made correct.
 */

#include <string.h>
#include <mach_ldebug.h>

#include <libkern/OSAtomic.h>

#include <mach/machine/vm_types.h>

#include <mach/boolean.h>
#include <kern/thread.h>
#include <kern/zalloc.h>
#include <kern/queue.h>
#include <kern/ledger.h>
#include <kern/mach_param.h>

#include <kern/spl.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <mach/machine/vm_param.h>
#include <machine/thread.h>

#include <kern/misc_protos.h>                   /* prototyping */
#include <i386/misc_protos.h>
#include <i386/i386_lowmem.h>
#include <x86_64/lowglobals.h>

#include <i386/cpuid.h>
#include <i386/cpu_data.h>
#include <i386/cpu_number.h>
#include <i386/machine_cpu.h>
#include <i386/seg.h>
#include <i386/serial_io.h>
#include <i386/cpu_capabilities.h>
#include <i386/machine_routines.h>
#include <i386/proc_reg.h>
#include <i386/tsc.h>
#include <i386/pmap_internal.h>
#include <i386/pmap_pcid.h>
#if CONFIG_VMX
#include <i386/vmx/vmx_cpu.h>
#endif

#include <vm/vm_protos.h>
#include <san/kasan.h>

#include <i386/mp.h>
#include <i386/mp_desc.h>
#include <libkern/kernel_mach_header.h>

#include <pexpert/i386/efi.h>
#include <libkern/section_keywords.h>
#if MACH_ASSERT
int pmap_stats_assert = 1;
#endif /* MACH_ASSERT */

#ifdef IWANTTODEBUG
#undef  DEBUG
#define DEBUG 1
#define POSTCODE_DELAY 1
#include <i386/postcode.h>
#endif /* IWANTTODEBUG */

#ifdef  PMAP_DEBUG
#define DBG(x...)       kprintf("DBG: " x)
#else
#define DBG(x...)
#endif
/* Compile time assert to ensure adjacency/alignment of per-CPU data fields used
 * in the trampolines for kernel/user boundary TLB coherency.
 */
char pmap_cpu_data_assert[(((offsetof(cpu_data_t, cpu_tlb_invalid) - offsetof(cpu_data_t, cpu_active_cr3)) == 8) && (offsetof(cpu_data_t, cpu_active_cr3) % 64 == 0)) ? 1 : -1];
boolean_t pmap_trace = FALSE;

boolean_t       no_shared_cr3 = DEBUG;          /* TRUE for DEBUG by default */

#if DEVELOPMENT || DEBUG
int nx_enabled = 1;                     /* enable no-execute protection -- set during boot */
#else
const int nx_enabled = 1;
#endif

#if DEBUG || DEVELOPMENT
int allow_data_exec  = VM_ABI_32;       /* 32-bit apps may execute data by default, 64-bit apps may not */
int allow_stack_exec = 0;               /* No apps may execute from the stack by default */
#else /* DEBUG || DEVELOPMENT */
const int allow_data_exec  = VM_ABI_32; /* 32-bit apps may execute data by default, 64-bit apps may not */
const int allow_stack_exec = 0;         /* No apps may execute from the stack by default */
#endif /* DEBUG || DEVELOPMENT */

uint64_t max_preemption_latency_tsc = 0;

pv_hashed_entry_t     *pv_hash_table;  /* hash lists */

uint32_t npvhashmask = 0, npvhashbuckets = 0;

pv_hashed_entry_t       pv_hashed_free_list = PV_HASHED_ENTRY_NULL;
pv_hashed_entry_t       pv_hashed_kern_free_list = PV_HASHED_ENTRY_NULL;
SIMPLE_LOCK_DECLARE(pv_hashed_free_list_lock, 0);
SIMPLE_LOCK_DECLARE(pv_hashed_kern_free_list_lock, 0);
SIMPLE_LOCK_DECLARE(pv_hash_table_lock, 0);
SIMPLE_LOCK_DECLARE(phys_backup_lock, 0);

SECURITY_READ_ONLY_LATE(zone_t) pv_hashed_list_zone;    /* zone of pv_hashed_entry structures */

/*
 *	First and last physical addresses that we maintain any information
 *	for.  Initialized to zero so that pmap operations done before
 *	pmap_init won't touch any non-existent structures.
 */
boolean_t       pmap_initialized = FALSE;/* Has pmap_init completed? */

static struct vm_object kptobj_object_store VM_PAGE_PACKED_ALIGNED;
static struct vm_object kpml4obj_object_store VM_PAGE_PACKED_ALIGNED;
static struct vm_object kpdptobj_object_store VM_PAGE_PACKED_ALIGNED;

/*
 *	Array of physical page attribites for managed pages.
 *	One byte per physical page.
 */
char            *pmap_phys_attributes;
ppnum_t         last_managed_page = 0;

unsigned pmap_memory_region_count;
unsigned pmap_memory_region_current;

pmap_memory_region_t pmap_memory_regions[PMAP_MEMORY_REGIONS_SIZE];

/*
 *	Other useful macros.
 */
#define current_pmap()          (vm_map_pmap(current_thread()->map))

struct pmap     kernel_pmap_store;
SECURITY_READ_ONLY_LATE(pmap_t)          kernel_pmap = NULL;
SECURITY_READ_ONLY_LATE(zone_t)          pmap_zone; /* zone of pmap structures */
SECURITY_READ_ONLY_LATE(zone_t)          pmap_anchor_zone;
SECURITY_READ_ONLY_LATE(zone_t)          pmap_uanchor_zone;
int             pmap_debug = 0;         /* flag for debugging prints */

unsigned int    inuse_ptepages_count = 0;
long long       alloc_ptepages_count __attribute__((aligned(8))) = 0; /* aligned for atomic access */
unsigned int    bootstrap_wired_pages = 0;

extern  long    NMIPI_acks;

SECURITY_READ_ONLY_LATE(boolean_t)       kernel_text_ps_4K = TRUE;

extern char     end;

static int      nkpt;

#if DEVELOPMENT || DEBUG
SECURITY_READ_ONLY_LATE(boolean_t)       pmap_disable_kheap_nx = FALSE;
SECURITY_READ_ONLY_LATE(boolean_t)       pmap_disable_kstack_nx = FALSE;
SECURITY_READ_ONLY_LATE(boolean_t)       wpkernel = TRUE;
#else
const boolean_t wpkernel = TRUE;
#endif

extern long __stack_chk_guard[];

static uint64_t pmap_eptp_flags = 0;
boolean_t pmap_ept_support_ad = FALSE;

static void process_pmap_updates(pmap_t, bool, addr64_t, addr64_t);
/*
 *	Map memory at initialization.  The physical addresses being
 *	mapped are not managed and are never unmapped.
 *
 *	For now, VM is already on, we only need to map the
 *	specified memory.
 */
vm_offset_t
pmap_map(
	vm_offset_t     virt,
	vm_map_offset_t start_addr,
	vm_map_offset_t end_addr,
	vm_prot_t       prot,
	unsigned int    flags)
{
	kern_return_t   kr;
	int             ps;

	ps = PAGE_SIZE;
	while (start_addr < end_addr) {
		kr = pmap_enter(kernel_pmap, (vm_map_offset_t)virt,
		    (ppnum_t) i386_btop(start_addr), prot, VM_PROT_NONE, flags, TRUE);

		if (kr != KERN_SUCCESS) {
			panic("%s: failed pmap_enter, "
			    "virt=%p, start_addr=%p, end_addr=%p, prot=%#x, flags=%#x",
			    __FUNCTION__,
			    (void *)virt, (void *)start_addr, (void *)end_addr, prot, flags);
		}

		virt += ps;
		start_addr += ps;
	}
	return virt;
}

extern  char                    *first_avail;
extern  vm_offset_t             virtual_avail, virtual_end;
extern  pmap_paddr_t            avail_start, avail_end;
extern  vm_offset_t             sHIB;
extern  vm_offset_t             eHIB;
extern  vm_offset_t             stext;
extern  vm_offset_t             etext;
extern  vm_offset_t             sdata, edata;
extern  vm_offset_t             sconst, econst;

extern void                     *KPTphys;

boolean_t pmap_smep_enabled = FALSE;
boolean_t pmap_smap_enabled = FALSE;

void
pmap_cpu_init(void)
{
	cpu_data_t      *cdp = current_cpu_datap();

	set_cr4(get_cr4() | CR4_PGE);

	/*
	 * Initialize the per-cpu, TLB-related fields.
	 */
	cdp->cpu_kernel_cr3 = kernel_pmap->pm_cr3;
	cpu_shadowp(cdp->cpu_number)->cpu_kernel_cr3 = cdp->cpu_kernel_cr3;
	cdp->cpu_active_cr3 = kernel_pmap->pm_cr3;
	cdp->cpu_tlb_invalid = 0;
	cdp->cpu_task_map = TASK_MAP_64BIT;

	pmap_pcid_configure();
	if (cpuid_leaf7_features() & CPUID_LEAF7_FEATURE_SMEP) {
		pmap_smep_enabled = TRUE;
#if     DEVELOPMENT || DEBUG
		boolean_t nsmep;
		if (PE_parse_boot_argn("-pmap_smep_disable", &nsmep, sizeof(nsmep))) {
			pmap_smep_enabled = FALSE;
		}
#endif
		if (pmap_smep_enabled) {
			set_cr4(get_cr4() | CR4_SMEP);
		}
	}
	if (cpuid_leaf7_features() & CPUID_LEAF7_FEATURE_SMAP) {
		pmap_smap_enabled = TRUE;
#if DEVELOPMENT || DEBUG
		boolean_t nsmap;
		if (PE_parse_boot_argn("-pmap_smap_disable", &nsmap, sizeof(nsmap))) {
			pmap_smap_enabled = FALSE;
		}
#endif
		if (pmap_smap_enabled) {
			set_cr4(get_cr4() | CR4_SMAP);
		}
	}

#if !MONOTONIC
	if (cdp->cpu_fixed_pmcs_enabled) {
		boolean_t enable = TRUE;
		cpu_pmc_control(&enable);
	}
#endif /* !MONOTONIC */
}

static uint32_t
pmap_scale_shift(void)
{
	uint32_t scale = 0;

	if (sane_size <= 8 * GB) {
		scale = (uint32_t)(sane_size / (2 * GB));
	} else if (sane_size <= 32 * GB) {
		scale = 4 + (uint32_t)((sane_size - (8 * GB)) / (4 * GB));
	} else {
		scale = 10 + (uint32_t)MIN(4, ((sane_size - (32 * GB)) / (8 * GB)));
	}
	return scale;
}

LCK_GRP_DECLARE(pmap_lck_grp, "pmap");
LCK_ATTR_DECLARE(pmap_lck_rw_attr, 0, LCK_ATTR_DEBUG);

/*
 *	Bootstrap the system enough to run with virtual memory.
 *	Map the kernel's code and data, and allocate the system page table.
 *	Called with mapping OFF.  Page_size must already be set.
 */

void
pmap_bootstrap(
	__unused vm_offset_t    load_start,
	__unused boolean_t      IA32e)
{
	assert(IA32e);

	vm_last_addr = VM_MAX_KERNEL_ADDRESS;   /* Set the highest address
	                                         * known to VM */
	/*
	 *	The kernel's pmap is statically allocated so we don't
	 *	have to use pmap_create, which is unlikely to work
	 *	correctly at this part of the boot sequence.
	 */

	kernel_pmap = &kernel_pmap_store;
	os_ref_init(&kernel_pmap->ref_count, NULL);
#if DEVELOPMENT || DEBUG
	kernel_pmap->nx_enabled = TRUE;
#endif
	kernel_pmap->pm_task_map = TASK_MAP_64BIT;
	kernel_pmap->pm_obj = (vm_object_t) NULL;
	kernel_pmap->pm_pml4 = IdlePML4;
	kernel_pmap->pm_upml4 = IdlePML4;
	kernel_pmap->pm_cr3 = (uintptr_t)ID_MAP_VTOP(IdlePML4);
	kernel_pmap->pm_ucr3 = (uintptr_t)ID_MAP_VTOP(IdlePML4);
	kernel_pmap->pm_eptp = 0;

	pmap_pcid_initialize_kernel(kernel_pmap);

	current_cpu_datap()->cpu_kernel_cr3 = cpu_shadowp(cpu_number())->cpu_kernel_cr3 = (addr64_t) kernel_pmap->pm_cr3;

	nkpt = NKPT;
	OSAddAtomic(NKPT, &inuse_ptepages_count);
	OSAddAtomic64(NKPT, &alloc_ptepages_count);
	bootstrap_wired_pages = NKPT;

	virtual_avail = (vm_offset_t)(VM_MIN_KERNEL_ADDRESS) + (vm_offset_t)first_avail;
	virtual_end = (vm_offset_t)(VM_MAX_KERNEL_ADDRESS);

	if (!PE_parse_boot_argn("npvhash", &npvhashmask, sizeof(npvhashmask))) {
		npvhashmask = ((NPVHASHBUCKETS) << pmap_scale_shift()) - 1;
	}

	npvhashbuckets = npvhashmask + 1;

	if (0 != ((npvhashbuckets) & npvhashmask)) {
		panic("invalid hash %d, must be ((2^N)-1), "
		    "using default %d\n", npvhashmask, NPVHASHMASK);
	}

	lck_rw_init(&kernel_pmap->pmap_rwl, &pmap_lck_grp, &pmap_lck_rw_attr);
	kernel_pmap->pmap_rwl.lck_rw_can_sleep = FALSE;

	pmap_cpu_init();

	if (pmap_pcid_ncpus) {
		printf("PMAP: PCID enabled\n");
	}

	if (pmap_smep_enabled) {
		printf("PMAP: Supervisor Mode Execute Protection enabled\n");
	}
	if (pmap_smap_enabled) {
		printf("PMAP: Supervisor Mode Access Protection enabled\n");
	}

#if     DEBUG
	printf("Stack canary: 0x%lx\n", __stack_chk_guard[0]);
	printf("early_random(): 0x%qx\n", early_random());
#endif
#if     DEVELOPMENT || DEBUG
	boolean_t ptmp;
	/* Check if the user has requested disabling stack or heap no-execute
	 * enforcement. These are "const" variables; that qualifier is cast away
	 * when altering them. The TEXT/DATA const sections are marked
	 * write protected later in the kernel startup sequence, so altering
	 * them is possible at this point, in pmap_bootstrap().
	 */
	if (PE_parse_boot_argn("-pmap_disable_kheap_nx", &ptmp, sizeof(ptmp))) {
		boolean_t *pdknxp = (boolean_t *) &pmap_disable_kheap_nx;
		*pdknxp = TRUE;
	}

	if (PE_parse_boot_argn("-pmap_disable_kstack_nx", &ptmp, sizeof(ptmp))) {
		boolean_t *pdknhp = (boolean_t *) &pmap_disable_kstack_nx;
		*pdknhp = TRUE;
	}
#endif /* DEVELOPMENT || DEBUG */

	boot_args *args = (boot_args *)PE_state.bootArgs;
	if (args->efiMode == kBootArgsEfiMode32) {
		printf("EFI32: kernel virtual space limited to 4GB\n");
		virtual_end = VM_MAX_KERNEL_ADDRESS_EFI32;
	}
	kprintf("Kernel virtual space from 0x%lx to 0x%lx.\n",
	    (long)KERNEL_BASE, (long)virtual_end);
	kprintf("Available physical space from 0x%llx to 0x%llx\n",
	    avail_start, avail_end);

	/*
	 * The -no_shared_cr3 boot-arg is a debugging feature (set by default
	 * in the DEBUG kernel) to force the kernel to switch to its own map
	 * (and cr3) when control is in kernelspace. The kernel's map does not
	 * include (i.e. share) userspace so wild references will cause
	 * a panic. Only copyin and copyout are exempt from this.
	 */
	(void) PE_parse_boot_argn("-no_shared_cr3",
	    &no_shared_cr3, sizeof(no_shared_cr3));
	if (no_shared_cr3) {
		kprintf("Kernel not sharing user map\n");
	}

#ifdef  PMAP_TRACES
	if (PE_parse_boot_argn("-pmap_trace", &pmap_trace, sizeof(pmap_trace))) {
		kprintf("Kernel traces for pmap operations enabled\n");
	}
#endif  /* PMAP_TRACES */

#if MACH_ASSERT
	PE_parse_boot_argn("pmap_asserts", &pmap_asserts_enabled, sizeof(pmap_asserts_enabled));
	PE_parse_boot_argn("pmap_stats_assert",
	    &pmap_stats_assert,
	    sizeof(pmap_stats_assert));
#endif /* MACH_ASSERT */
}

void
pmap_virtual_space(
	vm_offset_t *startp,
	vm_offset_t *endp)
{
	*startp = virtual_avail;
	*endp = virtual_end;
}




#if HIBERNATION

#include <IOKit/IOHibernatePrivate.h>
#include <machine/pal_hibernate.h>

int32_t         pmap_npages;
int32_t         pmap_teardown_last_valid_compact_indx = -1;

void    pmap_pack_index(uint32_t);
int32_t pmap_unpack_index(pv_rooted_entry_t);

int32_t
pmap_unpack_index(pv_rooted_entry_t pv_h)
{
	int32_t indx = 0;

	indx = (int32_t)(*((uint64_t *)(&pv_h->qlink.next)) >> 48);
	indx = indx << 16;
	indx |= (int32_t)(*((uint64_t *)(&pv_h->qlink.prev)) >> 48);

	*((uint64_t *)(&pv_h->qlink.next)) |= ((uint64_t)0xffff << 48);
	*((uint64_t *)(&pv_h->qlink.prev)) |= ((uint64_t)0xffff << 48);

	return indx;
}


void
pmap_pack_index(uint32_t indx)
{
	pv_rooted_entry_t       pv_h;

	pv_h = &pv_head_table[indx];

	*((uint64_t *)(&pv_h->qlink.next)) &= ~((uint64_t)0xffff << 48);
	*((uint64_t *)(&pv_h->qlink.prev)) &= ~((uint64_t)0xffff << 48);

	*((uint64_t *)(&pv_h->qlink.next)) |= ((uint64_t)(indx >> 16)) << 48;
	*((uint64_t *)(&pv_h->qlink.prev)) |= ((uint64_t)(indx & 0xffff)) << 48;
}


void
pal_hib_teardown_pmap_structs(addr64_t *unneeded_start, addr64_t *unneeded_end)
{
	int32_t         i;
	int32_t         compact_target_indx;

	compact_target_indx = 0;

	for (i = 0; i < pmap_npages; i++) {
		if (pv_head_table[i].pmap == PMAP_NULL) {
			if (pv_head_table[compact_target_indx].pmap != PMAP_NULL) {
				compact_target_indx = i;
			}
		} else {
			pmap_pack_index((uint32_t)i);

			if (pv_head_table[compact_target_indx].pmap == PMAP_NULL) {
				/*
				 * we've got a hole to fill, so
				 * move this pv_rooted_entry_t to it's new home
				 */
				pv_head_table[compact_target_indx] = pv_head_table[i];
				pv_head_table[i].pmap = PMAP_NULL;

				pmap_teardown_last_valid_compact_indx = compact_target_indx;
				compact_target_indx++;
			} else {
				pmap_teardown_last_valid_compact_indx = i;
			}
		}
	}
	*unneeded_start = (addr64_t)&pv_head_table[pmap_teardown_last_valid_compact_indx + 1];
	*unneeded_end = (addr64_t)&pv_head_table[pmap_npages - 1];

	HIBLOG("pal_hib_teardown_pmap_structs done: last_valid_compact_indx %d\n", pmap_teardown_last_valid_compact_indx);
}


void
pal_hib_rebuild_pmap_structs(void)
{
	int32_t                 cindx, eindx, rindx = 0;
	pv_rooted_entry_t       pv_h;

	eindx = (int32_t)pmap_npages;

	for (cindx = pmap_teardown_last_valid_compact_indx; cindx >= 0; cindx--) {
		pv_h = &pv_head_table[cindx];

		rindx = pmap_unpack_index(pv_h);
		assert(rindx < pmap_npages);

		if (rindx != cindx) {
			/*
			 * this pv_rooted_entry_t was moved by pal_hib_teardown_pmap_structs,
			 * so move it back to its real location
			 */
			pv_head_table[rindx] = pv_head_table[cindx];
		}
		if (rindx + 1 != eindx) {
			/*
			 * the 'hole' between this vm_rooted_entry_t and the previous
			 * vm_rooted_entry_t we moved needs to be initialized as
			 * a range of zero'd vm_rooted_entry_t's
			 */
			bzero((char *)&pv_head_table[rindx + 1], (eindx - rindx - 1) * sizeof(struct pv_rooted_entry));
		}
		eindx = rindx;
	}
	if (rindx) {
		bzero((char *)&pv_head_table[0], rindx * sizeof(struct pv_rooted_entry));
	}

	HIBLOG("pal_hib_rebuild_pmap_structs done: last_valid_compact_indx %d\n", pmap_teardown_last_valid_compact_indx);
}

#endif

/*
 * Create pv entries for kernel pages mapped by early startup code.
 * These have to exist so we can ml_static_mfree() them later.
 */
static void
pmap_pv_fixup(vm_offset_t start_va, vm_offset_t end_va)
{
	ppnum_t           ppn;
	pv_rooted_entry_t pv_h;
	uint32_t          pgsz;

	start_va = round_page(start_va);
	end_va = trunc_page(end_va);
	while (start_va < end_va) {
		pgsz = PAGE_SIZE;
		ppn = pmap_find_phys(kernel_pmap, start_va);
		if (ppn != 0 && IS_MANAGED_PAGE(ppn)) {
			pv_h = pai_to_pvh(ppn);
			assert(pv_h->qlink.next == 0);           /* shouldn't be init'd yet */
			assert(pv_h->pmap == 0);
			pv_h->va_and_flags = start_va;
			pv_h->pmap = kernel_pmap;
			queue_init(&pv_h->qlink);
			if (pmap_query_pagesize(kernel_pmap, start_va) == I386_LPGBYTES) {
				pgsz = I386_LPGBYTES;
			}
		}
		start_va += pgsz;
	}
}

/*
 *	Initialize the pmap module.
 *	Called by vm_init, to initialize any structures that the pmap
 *	system needs to map virtual memory.
 */
void
pmap_init(void)
{
	long                    npages;
	vm_offset_t             addr;
	vm_size_t               s, vsize;
	vm_map_offset_t         vaddr;
	ppnum_t ppn;


	kernel_pmap->pm_obj_pml4 = &kpml4obj_object_store;
	_vm_object_allocate((vm_object_size_t)NPML4PGS * PAGE_SIZE, &kpml4obj_object_store);

	kernel_pmap->pm_obj_pdpt = &kpdptobj_object_store;
	_vm_object_allocate((vm_object_size_t)NPDPTPGS * PAGE_SIZE, &kpdptobj_object_store);

	kernel_pmap->pm_obj = &kptobj_object_store;
	_vm_object_allocate((vm_object_size_t)NPDEPGS * PAGE_SIZE, &kptobj_object_store);

	/*
	 *	Allocate memory for the pv_head_table and its lock bits,
	 *	the modify bit array, and the pte_page table.
	 */

	/*
	 * zero bias all these arrays now instead of off avail_start
	 * so we cover all memory
	 */

	npages = i386_btop(avail_end);
#if HIBERNATION
	pmap_npages = (uint32_t)npages;
#endif
	s = (vm_size_t) (sizeof(struct pv_rooted_entry) * npages
	    + (sizeof(struct pv_hashed_entry_t *) * (npvhashbuckets))
	    + pv_lock_table_size(npages)
	    + pv_hash_lock_table_size((npvhashbuckets))
	    + npages);
	s = round_page(s);
	if (kernel_memory_allocate(kernel_map, &addr, s, 0,
	    KMA_KOBJECT | KMA_PERMANENT, VM_KERN_MEMORY_PMAP)
	    != KERN_SUCCESS) {
		panic("pmap_init");
	}

	memset((char *)addr, 0, s);

	vaddr = addr;
	vsize = s;

#if PV_DEBUG
	if (0 == npvhashmask) {
		panic("npvhashmask not initialized");
	}
#endif

	/*
	 *	Allocate the structures first to preserve word-alignment.
	 */
	pv_head_table = (pv_rooted_entry_t) addr;
	addr = (vm_offset_t) (pv_head_table + npages);

	pv_hash_table = (pv_hashed_entry_t *)addr;
	addr = (vm_offset_t) (pv_hash_table + (npvhashbuckets));

	pv_lock_table = (char *) addr;
	addr = (vm_offset_t) (pv_lock_table + pv_lock_table_size(npages));

	pv_hash_lock_table = (char *) addr;
	addr = (vm_offset_t) (pv_hash_lock_table + pv_hash_lock_table_size((npvhashbuckets)));

	pmap_phys_attributes = (char *) addr;

	ppnum_t  last_pn = i386_btop(avail_end);
	unsigned int i;
	pmap_memory_region_t *pmptr = pmap_memory_regions;
	for (i = 0; i < pmap_memory_region_count; i++, pmptr++) {
		if (pmptr->type != kEfiConventionalMemory) {
			continue;
		}
		ppnum_t pn;
		for (pn = pmptr->base; pn <= pmptr->end; pn++) {
			if (pn < last_pn) {
				pmap_phys_attributes[pn] |= PHYS_MANAGED;

				if (pn > last_managed_page) {
					last_managed_page = pn;
				}

				if ((pmap_high_used_bottom <= pn && pn <= pmap_high_used_top) ||
				    (pmap_middle_used_bottom <= pn && pn <= pmap_middle_used_top)) {
					pmap_phys_attributes[pn] |= PHYS_NOENCRYPT;
				}
			}
		}
	}
	while (vsize) {
		ppn = pmap_find_phys(kernel_pmap, vaddr);

		pmap_phys_attributes[ppn] |= PHYS_NOENCRYPT;

		vaddr += PAGE_SIZE;
		vsize -= PAGE_SIZE;
	}
	/*
	 *	Create the zone of physical maps,
	 *	and of the physical-to-virtual entries.
	 */
	pmap_zone = zone_create_ext("pmap", sizeof(struct pmap),
	    ZC_NOENCRYPT | ZC_ZFREE_CLEARMEM, ZONE_ID_PMAP, NULL);

	/* The anchor is required to be page aligned. Zone debugging adds
	 * padding which may violate that requirement. Tell the zone
	 * subsystem that alignment is required.
	 */
	pmap_anchor_zone = zone_create("pagetable anchors", PAGE_SIZE,
	    ZC_NOENCRYPT | ZC_ALIGNMENT_REQUIRED);

/* TODO: possible general optimisation...pre-allocate via zones commonly created
 * level3/2 pagetables
 */
	/* The anchor is required to be page aligned. Zone debugging adds
	 * padding which may violate that requirement. Tell the zone
	 * subsystem that alignment is required.
	 */
	pmap_uanchor_zone = zone_create("pagetable user anchors", PAGE_SIZE,
	    ZC_NOENCRYPT | ZC_ALIGNMENT_REQUIRED);

	pv_hashed_list_zone = zone_create("pv_list", sizeof(struct pv_hashed_entry),
	    ZC_NOENCRYPT | ZC_ALIGNMENT_REQUIRED);

	/*
	 * Create pv entries for kernel pages that might get pmap_remove()ed.
	 *
	 * - very low pages that were identity mapped.
	 * - vm_pages[] entries that might be unused and reclaimed.
	 */
	assert((uintptr_t)VM_MIN_KERNEL_ADDRESS + avail_start <= (uintptr_t)vm_page_array_beginning_addr);
	pmap_pv_fixup((uintptr_t)VM_MIN_KERNEL_ADDRESS, (uintptr_t)VM_MIN_KERNEL_ADDRESS + avail_start);
	pmap_pv_fixup((uintptr_t)vm_page_array_beginning_addr, (uintptr_t)vm_page_array_ending_addr);

	pmap_initialized = TRUE;

	max_preemption_latency_tsc = tmrCvt((uint64_t)MAX_PREEMPTION_LATENCY_NS, tscFCvtn2t);

	/*
	 * Ensure the kernel's PML4 entry exists for the basement
	 * before this is shared with any user.
	 */
	pmap_expand_pml4(kernel_pmap, KERNEL_BASEMENT, PMAP_EXPAND_OPTIONS_NONE);

#if CONFIG_VMX
	pmap_ept_support_ad = vmx_hv_support() && (VMX_CAP(MSR_IA32_VMX_EPT_VPID_CAP, MSR_IA32_VMX_EPT_VPID_CAP_AD_SHIFT, 1) ? TRUE : FALSE);
	pmap_eptp_flags = HV_VMX_EPTP_MEMORY_TYPE_WB | HV_VMX_EPTP_WALK_LENGTH(4) | (pmap_ept_support_ad ? HV_VMX_EPTP_ENABLE_AD_FLAGS : 0);
#endif /* CONFIG_VMX */
}

void
pmap_mark_range(pmap_t npmap, uint64_t sv, uint64_t nxrosz, boolean_t NX, boolean_t ro)
{
	uint64_t ev = sv + nxrosz, cv = sv;
	pd_entry_t *pdep;
	pt_entry_t *ptep = NULL;

	/* XXX what if nxrosz is 0?  we end up marking the page whose address is passed in via sv -- is that kosher? */
	assert(!is_ept_pmap(npmap));

	assert(((sv & 0xFFFULL) | (nxrosz & 0xFFFULL)) == 0);

	for (pdep = pmap_pde(npmap, cv); pdep != NULL && (cv < ev);) {
		uint64_t pdev = (cv & ~((uint64_t)PDEMASK));

		if (*pdep & INTEL_PTE_PS) {
#ifdef REMAP_DEBUG
			if ((NX ^ !!(*pdep & INTEL_PTE_NX)) || (ro ^ !!!(*pdep & INTEL_PTE_WRITE))) {
				kprintf("WARNING: Remapping PDE for %p from %s%s%s to %s%s%s\n", (void *)cv,
				    (*pdep & INTEL_PTE_VALID) ? "R" : "",
				    (*pdep & INTEL_PTE_WRITE) ? "W" : "",
				    (*pdep & INTEL_PTE_NX) ? "" : "X",
				    "R",
				    ro ? "" : "W",
				    NX ? "" : "X");
			}
#endif

			if (NX) {
				*pdep |= INTEL_PTE_NX;
			} else {
				*pdep &= ~INTEL_PTE_NX;
			}
			if (ro) {
				*pdep &= ~INTEL_PTE_WRITE;
			} else {
				*pdep |= INTEL_PTE_WRITE;
			}
			cv += NBPD;
			cv &= ~((uint64_t) PDEMASK);
			pdep = pmap_pde(npmap, cv);
			continue;
		}

		for (ptep = pmap_pte(npmap, cv); ptep != NULL && (cv < (pdev + NBPD)) && (cv < ev);) {
#ifdef REMAP_DEBUG
			if ((NX ^ !!(*ptep & INTEL_PTE_NX)) || (ro ^ !!!(*ptep & INTEL_PTE_WRITE))) {
				kprintf("WARNING: Remapping PTE for %p from %s%s%s to %s%s%s\n", (void *)cv,
				    (*ptep & INTEL_PTE_VALID) ? "R" : "",
				    (*ptep & INTEL_PTE_WRITE) ? "W" : "",
				    (*ptep & INTEL_PTE_NX) ? "" : "X",
				    "R",
				    ro ? "" : "W",
				    NX ? "" : "X");
			}
#endif
			if (NX) {
				*ptep |= INTEL_PTE_NX;
			} else {
				*ptep &= ~INTEL_PTE_NX;
			}
			if (ro) {
				*ptep &= ~INTEL_PTE_WRITE;
			} else {
				*ptep |= INTEL_PTE_WRITE;
			}
			cv += NBPT;
			ptep = pmap_pte(npmap, cv);
		}
	}
	DPRINTF("%s(0x%llx, 0x%llx, %u, %u): 0x%llx, 0x%llx\n", __FUNCTION__, sv, nxrosz, NX, ro, cv, ptep ? *ptep: 0);
}

/*
 * Reclaim memory for early boot 4K page tables that were converted to large page mappings.
 * We know this memory is part of the KPTphys[] array that was allocated in Idle_PTs_init(),
 * so we can free it using its address in that array.
 */
static void
pmap_free_early_PT(ppnum_t ppn, uint32_t cnt)
{
	ppnum_t KPTphys_ppn;
	vm_offset_t offset;

	KPTphys_ppn = pmap_find_phys(kernel_pmap, (uintptr_t)KPTphys);
	assert(ppn >= KPTphys_ppn);
	assert(ppn + cnt <= KPTphys_ppn + NKPT);
	offset = (ppn - KPTphys_ppn) << PAGE_SHIFT;
	ml_static_mfree((uintptr_t)KPTphys + offset, PAGE_SIZE * cnt);
}

/*
 * Called once VM is fully initialized so that we can release unused
 * sections of low memory to the general pool.
 * Also complete the set-up of identity-mapped sections of the kernel:
 *  1) write-protect kernel text
 *  2) map kernel text using large pages if possible
 *  3) read and write-protect page zero (for K32)
 *  4) map the global page at the appropriate virtual address.
 *
 * Use of large pages
 * ------------------
 * To effectively map and write-protect all kernel text pages, the text
 * must be 2M-aligned at the base, and the data section above must also be
 * 2M-aligned. That is, there's padding below and above. This is achieved
 * through linker directives. Large pages are used only if this alignment
 * exists (and not overriden by the -kernel_text_page_4K boot-arg). The
 * memory layout is:
 *
 *                       :                :
 *                       |     __DATA     |
 *               sdata:  ==================  2Meg
 *                       |                |
 *                       |  zero-padding  |
 *                       |                |
 *               etext:  ------------------
 *                       |                |
 *                       :                :
 *                       |                |
 *                       |     __TEXT     |
 *                       |                |
 *                       :                :
 *                       |                |
 *               stext:  ==================  2Meg
 *                       |                |
 *                       |  zero-padding  |
 *                       |                |
 *               eHIB:   ------------------
 *                       |     __HIB      |
 *                       :                :
 *
 * Prior to changing the mapping from 4K to 2M, the zero-padding pages
 * [eHIB,stext] and [etext,sdata] are ml_static_mfree()'d. Then all the
 * 4K pages covering [stext,etext] are coalesced as 2M large pages.
 * The now unused level-1 PTE pages are also freed.
 */
extern ppnum_t  vm_kernel_base_page;
static uint32_t dataptes = 0;

void
pmap_lowmem_finalize(void)
{
	spl_t           spl;
	int             i;

	/*
	 * Update wired memory statistics for early boot pages
	 */
	PMAP_ZINFO_PALLOC(kernel_pmap, bootstrap_wired_pages * PAGE_SIZE);

	/*
	 * Free pages in pmap regions below the base:
	 * rdar://6332712
	 *	We can't free all the pages to VM that EFI reports available.
	 *	Pages in the range 0xc0000-0xff000 aren't safe over sleep/wake.
	 *	There's also a size miscalculation here: pend is one page less
	 *	than it should be but this is not fixed to be backwards
	 *	compatible.
	 * This is important for KASLR because up to 256*2MB = 512MB of space
	 * needs has to be released to VM.
	 */
	for (i = 0;
	    pmap_memory_regions[i].end < vm_kernel_base_page;
	    i++) {
		vm_offset_t     pbase = i386_ptob(pmap_memory_regions[i].base);
		vm_offset_t     pend  = i386_ptob(pmap_memory_regions[i].end + 1);

		DBG("pmap region %d [%p..[%p\n",
		    i, (void *) pbase, (void *) pend);

		if (pmap_memory_regions[i].attribute & EFI_MEMORY_KERN_RESERVED) {
			continue;
		}
		/*
		 * rdar://6332712
		 * Adjust limits not to free pages in range 0xc0000-0xff000.
		 */
		if (pbase >= 0xc0000 && pend <= 0x100000) {
			continue;
		}
		if (pbase < 0xc0000 && pend > 0x100000) {
			/* page range entirely within region, free lower part */
			DBG("- ml_static_mfree(%p,%p)\n",
			    (void *) ml_static_ptovirt(pbase),
			    (void *) (0xc0000 - pbase));
			ml_static_mfree(ml_static_ptovirt(pbase), 0xc0000 - pbase);
			pbase = 0x100000;
		}
		if (pbase < 0xc0000) {
			pend = MIN(pend, 0xc0000);
		}
		if (pend > 0x100000) {
			pbase = MAX(pbase, 0x100000);
		}
		DBG("- ml_static_mfree(%p,%p)\n",
		    (void *) ml_static_ptovirt(pbase),
		    (void *) (pend - pbase));
		ml_static_mfree(ml_static_ptovirt(pbase), pend - pbase);
	}

	/* A final pass to get rid of all initial identity mappings to
	 * low pages.
	 */
	DPRINTF("%s: Removing mappings from 0->0x%lx\n", __FUNCTION__, vm_kernel_base);

	/*
	 * Remove all mappings past the boot-cpu descriptor aliases and low globals.
	 * Non-boot-cpu GDT aliases will be remapped later as needed.
	 */
	pmap_remove(kernel_pmap, LOWGLOBAL_ALIAS + PAGE_SIZE, vm_kernel_base);

	/*
	 * Release any memory for early boot 4K page table pages that got replaced
	 * with large page mappings for vm_pages[]. We know this memory is part of
	 * the KPTphys[] array that was allocated in Idle_PTs_init(), so we can free
	 * it using that address.
	 */
	pmap_free_early_PT(released_PT_ppn, released_PT_cnt);

	/*
	 * If text and data are both 2MB-aligned,
	 * we can map text with large-pages,
	 * unless the -kernel_text_ps_4K boot-arg overrides.
	 */
	if ((stext & I386_LPGMASK) == 0 && (sdata & I386_LPGMASK) == 0) {
		kprintf("Kernel text is 2MB aligned");
		kernel_text_ps_4K = FALSE;
		if (PE_parse_boot_argn("-kernel_text_ps_4K",
		    &kernel_text_ps_4K,
		    sizeof(kernel_text_ps_4K))) {
			kprintf(" but will be mapped with 4K pages\n");
		} else {
			kprintf(" and will be mapped with 2M pages\n");
		}
	}
#if     DEVELOPMENT || DEBUG
	(void) PE_parse_boot_argn("wpkernel", &wpkernel, sizeof(wpkernel));
#endif
	if (wpkernel) {
		kprintf("Kernel text %p-%p to be write-protected\n",
		    (void *) stext, (void *) etext);
	}

	spl = splhigh();

	/*
	 * Scan over text if mappings are to be changed:
	 * - Remap kernel text readonly unless the "wpkernel" boot-arg is 0
	 * - Change to large-pages if possible and not overriden.
	 */
	if (kernel_text_ps_4K && wpkernel) {
		vm_offset_t     myva;
		for (myva = stext; myva < etext; myva += PAGE_SIZE) {
			pt_entry_t     *ptep;

			ptep = pmap_pte(kernel_pmap, (vm_map_offset_t)myva);
			if (ptep) {
				pmap_store_pte(ptep, *ptep & ~INTEL_PTE_WRITE);
			}
		}
	}

	if (!kernel_text_ps_4K) {
		vm_offset_t     myva;

		/*
		 * Release zero-filled page padding used for 2M-alignment.
		 */
		DBG("ml_static_mfree(%p,%p) for padding below text\n",
		    (void *) eHIB, (void *) (stext - eHIB));
		ml_static_mfree(eHIB, stext - eHIB);
		DBG("ml_static_mfree(%p,%p) for padding above text\n",
		    (void *) etext, (void *) (sdata - etext));
		ml_static_mfree(etext, sdata - etext);

		/*
		 * Coalesce text pages into large pages.
		 */
		for (myva = stext; myva < sdata; myva += I386_LPGBYTES) {
			pt_entry_t      *ptep;
			vm_offset_t     pte_phys;
			pt_entry_t      *pdep;
			pt_entry_t      pde;
			ppnum_t         KPT_ppn;

			pdep = pmap_pde(kernel_pmap, (vm_map_offset_t)myva);
			KPT_ppn = (ppnum_t)((*pdep & PG_FRAME) >> PAGE_SHIFT);
			ptep = pmap_pte(kernel_pmap, (vm_map_offset_t)myva);
			DBG("myva: %p pdep: %p ptep: %p\n",
			    (void *) myva, (void *) pdep, (void *) ptep);
			if ((*ptep & INTEL_PTE_VALID) == 0) {
				continue;
			}
			pte_phys = (vm_offset_t)(*ptep & PG_FRAME);
			pde = *pdep & PTMASK;   /* page attributes from pde */
			pde |= INTEL_PTE_PS;    /* make it a 2M entry */
			pde |= pte_phys;        /* take page frame from pte */

			if (wpkernel) {
				pde &= ~INTEL_PTE_WRITE;
			}
			DBG("pmap_store_pte(%p,0x%llx)\n",
			    (void *)pdep, pde);
			pmap_store_pte(pdep, pde);

			/*
			 * Free the now-unused level-1 pte.
			 */
			pmap_free_early_PT(KPT_ppn, 1);
		}

		/* Change variable read by sysctl machdep.pmap */
		pmap_kernel_text_ps = I386_LPGBYTES;
	}

	vm_offset_t dva;

	for (dva = sdata; dva < edata; dva += I386_PGBYTES) {
		assert(((sdata | edata) & PAGE_MASK) == 0);
		pt_entry_t dpte, *dptep = pmap_pte(kernel_pmap, dva);

		dpte = *dptep;
		assert((dpte & INTEL_PTE_VALID));
		dpte |= INTEL_PTE_NX;
		pmap_store_pte(dptep, dpte);
		dataptes++;
	}
	assert(dataptes > 0);

	kernel_segment_command_t * seg;
	kernel_section_t         * sec;
	kc_format_t kc_format;

	PE_get_primary_kc_format(&kc_format);

	for (seg = firstseg(); seg != NULL; seg = nextsegfromheader(&_mh_execute_header, seg)) {
		if (!strcmp(seg->segname, "__TEXT") ||
		    !strcmp(seg->segname, "__DATA")) {
			continue;
		}

		/* XXX: FIXME_IN_dyld: This is a workaround (see below) */
		if (kc_format != KCFormatFileset) {
			//XXX
			if (!strcmp(seg->segname, "__KLD")) {
				continue;
			}
		}

		if (!strcmp(seg->segname, "__HIB")) {
			for (sec = firstsect(seg); sec != NULL; sec = nextsect(seg, sec)) {
				if (sec->addr & PAGE_MASK) {
					panic("__HIB segment's sections misaligned");
				}
				if (!strcmp(sec->sectname, "__text")) {
					pmap_mark_range(kernel_pmap, sec->addr, round_page(sec->size), FALSE, TRUE);
				} else {
					pmap_mark_range(kernel_pmap, sec->addr, round_page(sec->size), TRUE, FALSE);
				}
			}
		} else {
			if (kc_format == KCFormatFileset) {
#if 0
				/*
				 * This block of code is commented out because it may or may not have induced an earlier panic
				 * in ledger init.
				 */


				boolean_t NXbit = !(seg->initprot & VM_PROT_EXECUTE),
				    robit = (seg->initprot & (VM_PROT_READ | VM_PROT_WRITE)) == VM_PROT_READ;

				/*
				 * XXX: FIXME_IN_dyld: This is a workaround for primary KC containing incorrect inaccurate
				 * initprot for segments containing code.
				 */
				if (!strcmp(seg->segname, "__KLD") || !strcmp(seg->segname, "__VECTORS")) {
					NXbit = FALSE;
					robit = FALSE;
				}

				pmap_mark_range(kernel_pmap, seg->vmaddr & ~(uint64_t)PAGE_MASK,
				    round_page_64(seg->vmsize), NXbit, robit);
#endif

				/*
				 * XXX: We are marking *every* segment with rwx permissions as a workaround
				 * XXX: until the primary KC's kernel segments are page-aligned.
				 */
				kprintf("Marking (%p, %p) as rwx\n", (void *)(seg->vmaddr & ~(uint64_t)PAGE_MASK),
				    (void *)((seg->vmaddr & ~(uint64_t)PAGE_MASK) + round_page_64(seg->vmsize)));
				pmap_mark_range(kernel_pmap, seg->vmaddr & ~(uint64_t)PAGE_MASK,
				    round_page_64(seg->vmsize), FALSE, FALSE);
			} else {
				pmap_mark_range(kernel_pmap, seg->vmaddr, round_page_64(seg->vmsize), TRUE, FALSE);
			}
		}
	}

	/*
	 * If we're debugging, map the low global vector page at the fixed
	 * virtual address.  Otherwise, remove the mapping for this.
	 */
	if (debug_boot_arg) {
		pt_entry_t *pte = NULL;
		if (0 == (pte = pmap_pte(kernel_pmap, LOWGLOBAL_ALIAS))) {
			panic("lowmem pte");
		}
		/* make sure it is defined on page boundary */
		assert(0 == ((vm_offset_t) &lowGlo & PAGE_MASK));
		pmap_store_pte(pte, kvtophys((vm_offset_t)&lowGlo)
		    | INTEL_PTE_REF
		    | INTEL_PTE_MOD
		    | INTEL_PTE_WIRED
		    | INTEL_PTE_VALID
		    | INTEL_PTE_WRITE
		    | INTEL_PTE_NX);
	} else {
		pmap_remove(kernel_pmap,
		    LOWGLOBAL_ALIAS, LOWGLOBAL_ALIAS + PAGE_SIZE);
	}
	pmap_tlbi_range(0, ~0ULL, true, 0);
	splx(spl);
}

/*
 *	Mark the const data segment as read-only, non-executable.
 */
void
x86_64_protect_data_const()
{
	boolean_t doconstro = TRUE;
#if DEVELOPMENT || DEBUG
	(void) PE_parse_boot_argn("dataconstro", &doconstro, sizeof(doconstro));
#endif
	if (doconstro) {
		if (sconst & PAGE_MASK) {
			panic("CONST segment misaligned 0x%lx 0x%lx\n",
			    sconst, econst);
		}
		kprintf("Marking const DATA read-only\n");
		pmap_protect(kernel_pmap, sconst, econst, VM_PROT_READ);
	}
}
/*
 * this function is only used for debugging fron the vm layer
 */
boolean_t
pmap_verify_free(
	ppnum_t pn)
{
	pv_rooted_entry_t       pv_h;
	int             pai;
	boolean_t       result;

	assert(pn != vm_page_fictitious_addr);

	if (!pmap_initialized) {
		return TRUE;
	}

	if (pn == vm_page_guard_addr) {
		return TRUE;
	}

	pai = ppn_to_pai(pn);
	if (!IS_MANAGED_PAGE(pai)) {
		return FALSE;
	}
	pv_h = pai_to_pvh(pn);
	result = (pv_h->pmap == PMAP_NULL);
	return result;
}

#if MACH_ASSERT
void
pmap_assert_free(ppnum_t pn)
{
	int pai;
	pv_rooted_entry_t pv_h = NULL;
	pmap_t pmap = NULL;
	vm_offset_t va = 0;
	static char buffer[32];
	static char *pr_name = "not managed pn";
	uint_t attr;
	pt_entry_t *ptep;
	pt_entry_t pte = -1ull;

	if (pmap_verify_free(pn)) {
		return;
	}

	if (pn > last_managed_page) {
		attr = 0xff;
		goto done;
	}

	pai = ppn_to_pai(pn);
	attr = pmap_phys_attributes[pai];
	pv_h = pai_to_pvh(pai);
	va = pv_h->va_and_flags;
	pmap = pv_h->pmap;
	if (pmap == kernel_pmap) {
		pr_name = "kernel";
	} else if (pmap == NULL) {
		pr_name = "pmap NULL";
	} else if (pmap->pmap_procname[0] != 0) {
		pr_name = &pmap->pmap_procname[0];
	} else {
		snprintf(buffer, sizeof(buffer), "pmap %p", pv_h->pmap);
		pr_name = buffer;
	}

	if (pmap != NULL) {
		ptep = pmap_pte(pmap, va);
		if (ptep != NULL) {
			pte = (uintptr_t)*ptep;
		}
	}

done:
	panic("page not FREE page: 0x%lx attr: 0x%x %s va: 0x%lx PTE: 0x%llx",
	    (ulong_t)pn, attr, pr_name, va, pte);
}
#endif /* MACH_ASSERT */

boolean_t
pmap_is_empty(
	pmap_t          pmap,
	vm_map_offset_t va_start,
	vm_map_offset_t va_end)
{
	vm_map_offset_t offset;
	ppnum_t         phys_page;

	if (pmap == PMAP_NULL) {
		return TRUE;
	}

	/*
	 * Check the resident page count
	 * - if it's zero, the pmap is completely empty.
	 * This short-circuit test prevents a virtual address scan which is
	 * painfully slow for 64-bit spaces.
	 * This assumes the count is correct
	 * .. the debug kernel ought to be checking perhaps by page table walk.
	 */
	if (pmap->stats.resident_count == 0) {
		return TRUE;
	}

	for (offset = va_start;
	    offset < va_end;
	    offset += PAGE_SIZE_64) {
		phys_page = pmap_find_phys(pmap, offset);
		if (phys_page) {
			kprintf("pmap_is_empty(%p,0x%llx,0x%llx): "
			    "page %d at 0x%llx\n",
			    pmap, va_start, va_end, phys_page, offset);
			return FALSE;
		}
	}

	return TRUE;
}

void
hv_ept_pmap_create(void **ept_pmap, void **eptp)
{
	pmap_t p;

	if ((ept_pmap == NULL) || (eptp == NULL)) {
		return;
	}

	p = pmap_create_options(get_task_ledger(current_task()), 0, (PMAP_CREATE_64BIT | PMAP_CREATE_EPT));
	if (p == PMAP_NULL) {
		*ept_pmap = NULL;
		*eptp = NULL;
		return;
	}

	assert(is_ept_pmap(p));

	*ept_pmap = (void*)p;
	*eptp = (void*)(p->pm_eptp);
	return;
}

/*
 * pmap_create() is used by some special, legacy 3rd party kexts.
 * In our kernel code, always use pmap_create_options().
 */
extern pmap_t pmap_create(ledger_t ledger, vm_map_size_t sz, boolean_t is_64bit);

__attribute__((used))
pmap_t
pmap_create(
	ledger_t      ledger,
	vm_map_size_t sz,
	boolean_t     is_64bit)
{
	return pmap_create_options(ledger, sz, is_64bit ? PMAP_CREATE_64BIT : 0);
}

/*
 *	Create and return a physical map.
 *
 *	If the size specified for the map
 *	is zero, the map is an actual physical
 *	map, and may be referenced by the
 *	hardware.
 *
 *	If the size specified is non-zero,
 *	the map will be used in software only, and
 *	is bounded by that size.
 */

pmap_t
pmap_create_options(
	ledger_t        ledger,
	vm_map_size_t   sz,
	unsigned int    flags)
{
	pmap_t          p;
	vm_size_t       size;
	pml4_entry_t    *pml4;
	pml4_entry_t    *kpml4;
	int             i;

	PMAP_TRACE(PMAP_CODE(PMAP__CREATE) | DBG_FUNC_START, sz, flags);

	size = (vm_size_t) sz;

	/*
	 *	A software use-only map doesn't even need a map.
	 */

	if (size != 0) {
		return PMAP_NULL;
	}

	/*
	 *	Return error when unrecognized flags are passed.
	 */
	if (__improbable((flags & ~(PMAP_CREATE_KNOWN_FLAGS)) != 0)) {
		return PMAP_NULL;
	}

	p = (pmap_t) zalloc(pmap_zone);
	if (PMAP_NULL == p) {
		panic("pmap_create zalloc");
	}

	/* Zero all fields */
	bzero(p, sizeof(*p));

	lck_rw_init(&p->pmap_rwl, &pmap_lck_grp, &pmap_lck_rw_attr);
	p->pmap_rwl.lck_rw_can_sleep = FALSE;

	bzero(&p->stats, sizeof(p->stats));
	os_ref_init(&p->ref_count, NULL);
#if DEVELOPMENT || DEBUG
	p->nx_enabled = 1;
#endif
	p->pm_shared = FALSE;
	ledger_reference(ledger);
	p->ledger = ledger;

	p->pm_task_map = ((flags & PMAP_CREATE_64BIT) ? TASK_MAP_64BIT : TASK_MAP_32BIT);

	p->pagezero_accessible = FALSE;
	p->pm_vm_map_cs_enforced = FALSE;

	if (pmap_pcid_ncpus) {
		pmap_pcid_initialize(p);
	}

	p->pm_pml4 = zalloc(pmap_anchor_zone);
	p->pm_upml4 = zalloc(pmap_uanchor_zone); //cleanup for EPT

	pmap_assert((((uintptr_t)p->pm_pml4) & PAGE_MASK) == 0);
	pmap_assert((((uintptr_t)p->pm_upml4) & PAGE_MASK) == 0);

	memset((char *)p->pm_pml4, 0, PAGE_SIZE);
	memset((char *)p->pm_upml4, 0, PAGE_SIZE);

	if (flags & PMAP_CREATE_EPT) {
		p->pm_eptp = (pmap_paddr_t)kvtophys((vm_offset_t)p->pm_pml4) | pmap_eptp_flags;
		p->pm_cr3 = 0;
	} else {
		p->pm_eptp = 0;
		p->pm_cr3 = (pmap_paddr_t)kvtophys((vm_offset_t)p->pm_pml4);
		p->pm_ucr3 = (pmap_paddr_t)kvtophys((vm_offset_t)p->pm_upml4);
	}

	/* allocate the vm_objs to hold the pdpt, pde and pte pages */

	p->pm_obj_pml4 = vm_object_allocate((vm_object_size_t)(NPML4PGS) *PAGE_SIZE);
	if (NULL == p->pm_obj_pml4) {
		panic("pmap_create pdpt obj");
	}

	p->pm_obj_pdpt = vm_object_allocate((vm_object_size_t)(NPDPTPGS) *PAGE_SIZE);
	if (NULL == p->pm_obj_pdpt) {
		panic("pmap_create pdpt obj");
	}

	p->pm_obj = vm_object_allocate((vm_object_size_t)(NPDEPGS) *PAGE_SIZE);
	if (NULL == p->pm_obj) {
		panic("pmap_create pte obj");
	}

	if (!(flags & PMAP_CREATE_EPT)) {
		/* All host pmaps share the kernel's pml4 */
		pml4 = pmap64_pml4(p, 0ULL);
		kpml4 = kernel_pmap->pm_pml4;
		for (i = KERNEL_PML4_INDEX; i < (KERNEL_PML4_INDEX + KERNEL_PML4_COUNT); i++) {
			pml4[i] = kpml4[i];
		}
		pml4[KERNEL_KEXTS_INDEX]   = kpml4[KERNEL_KEXTS_INDEX];
		for (i = KERNEL_PHYSMAP_PML4_INDEX; i < (KERNEL_PHYSMAP_PML4_INDEX + KERNEL_PHYSMAP_PML4_COUNT); i++) {
			pml4[i] = kpml4[i];
		}
		pml4[KERNEL_DBLMAP_PML4_INDEX] = kpml4[KERNEL_DBLMAP_PML4_INDEX];
#if KASAN
		for (i = KERNEL_KASAN_PML4_FIRST; i <= KERNEL_KASAN_PML4_LAST; i++) {
			pml4[i] = kpml4[i];
		}
#endif
		pml4_entry_t    *pml4u = pmap64_user_pml4(p, 0ULL);
		pml4u[KERNEL_DBLMAP_PML4_INDEX] = kpml4[KERNEL_DBLMAP_PML4_INDEX];
	}

#if MACH_ASSERT
	p->pmap_stats_assert = TRUE;
	p->pmap_pid = 0;
	strlcpy(p->pmap_procname, "<nil>", sizeof(p->pmap_procname));
#endif /* MACH_ASSERT */

	PMAP_TRACE(PMAP_CODE(PMAP__CREATE) | DBG_FUNC_END,
	    VM_KERNEL_ADDRHIDE(p));

	return p;
}

/*
 * We maintain stats and ledgers so that a task's physical footprint is:
 * phys_footprint = ((internal - alternate_accounting)
 *                   + (internal_compressed - alternate_accounting_compressed)
 *                   + iokit_mapped
 *                   + purgeable_nonvolatile
 *                   + purgeable_nonvolatile_compressed
 *                   + page_table)
 * where "alternate_accounting" includes "iokit" and "purgeable" memory.
 */

#if MACH_ASSERT
static void pmap_check_ledgers(pmap_t pmap);
#else /* MACH_ASSERT */
static inline void
pmap_check_ledgers(__unused pmap_t pmap)
{
}
#endif /* MACH_ASSERT */

/*
 *	Retire the given physical map from service.
 *	Should only be called if the map contains
 *	no valid mappings.
 */
extern int vm_wired_objects_page_count;

void
pmap_destroy(pmap_t     p)
{
	os_ref_count_t c;

	if (p == PMAP_NULL) {
		return;
	}

	PMAP_TRACE(PMAP_CODE(PMAP__DESTROY) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDe(p));

	PMAP_LOCK_EXCLUSIVE(p);

	c = os_ref_release_locked(&p->ref_count);

	pmap_assert((current_thread() && (current_thread()->map)) ? (current_thread()->map->pmap != p) : TRUE);

	if (c == 0) {
		/*
		 * If some cpu is not using the physical pmap pointer that it
		 * is supposed to be (see set_dirbase), we might be using the
		 * pmap that is being destroyed! Make sure we are
		 * physically on the right pmap:
		 */
		PMAP_UPDATE_TLBS(p, 0x0ULL, 0xFFFFFFFFFFFFF000ULL);
		if (pmap_pcid_ncpus) {
			pmap_destroy_pcid_sync(p);
		}
	}

	PMAP_UNLOCK_EXCLUSIVE(p);

	if (c != 0) {
		PMAP_TRACE(PMAP_CODE(PMAP__DESTROY) | DBG_FUNC_END);
		pmap_assert(p == kernel_pmap);
		return; /* still in use */
	}

	/*
	 *	Free the memory maps, then the
	 *	pmap structure.
	 */
	int inuse_ptepages = 0;

	zfree(pmap_anchor_zone, p->pm_pml4);
	zfree(pmap_uanchor_zone, p->pm_upml4);

	inuse_ptepages += p->pm_obj_pml4->resident_page_count;
	vm_object_deallocate(p->pm_obj_pml4);

	inuse_ptepages += p->pm_obj_pdpt->resident_page_count;
	vm_object_deallocate(p->pm_obj_pdpt);

	inuse_ptepages += p->pm_obj->resident_page_count;
	vm_object_deallocate(p->pm_obj);

	OSAddAtomic(-inuse_ptepages, &inuse_ptepages_count);
	PMAP_ZINFO_PFREE(p, inuse_ptepages * PAGE_SIZE);

	pmap_check_ledgers(p);
	ledger_dereference(p->ledger);
	lck_rw_destroy(&p->pmap_rwl, &pmap_lck_grp);
	zfree(pmap_zone, p);

	PMAP_TRACE(PMAP_CODE(PMAP__DESTROY) | DBG_FUNC_END);
}

/*
 *	Add a reference to the specified pmap.
 */

void
pmap_reference(pmap_t   p)
{
	if (p != PMAP_NULL) {
		PMAP_LOCK_EXCLUSIVE(p);
		os_ref_retain_locked(&p->ref_count);
		PMAP_UNLOCK_EXCLUSIVE(p);;
	}
}

/*
 *	Remove phys addr if mapped in specified map
 *
 */
void
pmap_remove_some_phys(
	__unused pmap_t         map,
	__unused ppnum_t         pn)
{
/* Implement to support working set code */
}


void
pmap_protect(
	pmap_t          map,
	vm_map_offset_t sva,
	vm_map_offset_t eva,
	vm_prot_t       prot)
{
	pmap_protect_options(map, sva, eva, prot, 0, NULL);
}


/*
 *	Set the physical protection on the
 *	specified range of this map as requested.
 *
 * VERY IMPORTANT: Will *NOT* increase permissions.
 *	pmap_protect_options() should protect the range against any access types
 *      that are not in "prot" but it should never grant extra access.
 *	For example, if "prot" is READ|EXECUTE, that means "remove write
 *      access" but it does *not* mean "add read and execute" access.
 *	VM relies on getting soft-faults to enforce extra checks (code
 *	signing, for example), for example.
 *	New access permissions are granted via pmap_enter() only.
 */
void
pmap_protect_options(
	pmap_t          map,
	vm_map_offset_t sva,
	vm_map_offset_t eva,
	vm_prot_t       prot,
	unsigned int    options,
	void            *arg)
{
	pt_entry_t      *pde;
	pt_entry_t      *spte, *epte;
	vm_map_offset_t lva;
	vm_map_offset_t orig_sva;
	boolean_t       set_NX;
	int             num_found = 0;
	boolean_t       is_ept;

	pmap_intr_assert();

	if (map == PMAP_NULL) {
		return;
	}

	if (prot == VM_PROT_NONE) {
		pmap_remove_options(map, sva, eva, options);
		return;
	}

	PMAP_TRACE(PMAP_CODE(PMAP__PROTECT) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDE(map), VM_KERNEL_ADDRHIDE(sva),
	    VM_KERNEL_ADDRHIDE(eva));

	if (prot & VM_PROT_EXECUTE) {
		set_NX = FALSE;
	} else {
		set_NX = TRUE;
	}

#if DEVELOPMENT || DEBUG
	if (__improbable(set_NX && (!nx_enabled || !map->nx_enabled))) {
		set_NX = FALSE;
	}
#endif
	is_ept = is_ept_pmap(map);

	PMAP_LOCK_EXCLUSIVE(map);

	orig_sva = sva;
	while (sva < eva) {
		lva = (sva + PDE_MAPPED_SIZE) & ~(PDE_MAPPED_SIZE - 1);
		if (lva > eva) {
			lva = eva;
		}
		pde = pmap_pde(map, sva);
		if (pde && (*pde & PTE_VALID_MASK(is_ept))) {
			if (*pde & PTE_PS) {
				/* superpage */
				spte = pde;
				epte = spte + 1; /* excluded */
			} else {
				spte = pmap_pte(map, (sva & ~(PDE_MAPPED_SIZE - 1)));
				spte = &spte[ptenum(sva)];
				epte = &spte[intel_btop(lva - sva)];
			}

			for (; spte < epte; spte++) {
				if (!(*spte & PTE_VALID_MASK(is_ept))) {
					continue;
				}

				if (is_ept) {
					if (!(prot & VM_PROT_READ)) {
						pmap_update_pte(spte, PTE_READ(is_ept), 0);
					}
				}
				if (!(prot & VM_PROT_WRITE)) {
					pmap_update_pte(spte, PTE_WRITE(is_ept), 0);
				}
#if DEVELOPMENT || DEBUG
				else if ((options & PMAP_OPTIONS_PROTECT_IMMEDIATE) &&
				    map == kernel_pmap) {
					pmap_update_pte(spte, 0, PTE_WRITE(is_ept));
				}
#endif /* DEVELOPMENT || DEBUG */

				if (set_NX) {
					if (!is_ept) {
						pmap_update_pte(spte, 0, INTEL_PTE_NX);
					} else {
						pmap_update_pte(spte, INTEL_EPT_EX, 0);
					}
				}
				num_found++;
			}
		}
		sva = lva;
	}
	if (num_found) {
		if (options & PMAP_OPTIONS_NOFLUSH) {
			PMAP_UPDATE_TLBS_DELAYED(map, orig_sva, eva, (pmap_flush_context *)arg);
		} else {
			PMAP_UPDATE_TLBS(map, orig_sva, eva);
		}
	}

	PMAP_UNLOCK_EXCLUSIVE(map);

	PMAP_TRACE(PMAP_CODE(PMAP__PROTECT) | DBG_FUNC_END);
}

/* Map a (possibly) autogenned block */
kern_return_t
pmap_map_block_addr(
	pmap_t          pmap,
	addr64_t        va,
	pmap_paddr_t    pa,
	uint32_t        size,
	vm_prot_t       prot,
	int             attr,
	unsigned int    flags)
{
	return pmap_map_block(pmap, va, intel_btop(pa), size, prot, attr, flags);
}

kern_return_t
pmap_map_block(
	pmap_t          pmap,
	addr64_t        va,
	ppnum_t         pa,
	uint32_t        size,
	vm_prot_t       prot,
	int             attr,
	__unused unsigned int   flags)
{
	kern_return_t   kr;
	addr64_t        original_va = va;
	uint32_t        page;
	int             cur_page_size;

	if (attr & VM_MEM_SUPERPAGE) {
		cur_page_size =  SUPERPAGE_SIZE;
	} else {
		cur_page_size =  PAGE_SIZE;
	}

	for (page = 0; page < size; page += cur_page_size / PAGE_SIZE) {
		kr = pmap_enter(pmap, va, pa, prot, VM_PROT_NONE, attr, TRUE);

		if (kr != KERN_SUCCESS) {
			/*
			 * This will panic for now, as it is unclear that
			 * removing the mappings is correct.
			 */
			panic("%s: failed pmap_enter, "
			    "pmap=%p, va=%#llx, pa=%u, size=%u, prot=%#x, flags=%#x",
			    __FUNCTION__,
			    pmap, va, pa, size, prot, flags);

			pmap_remove(pmap, original_va, va - original_va);
			return kr;
		}

		va += cur_page_size;
		pa += cur_page_size / PAGE_SIZE;
	}

	return KERN_SUCCESS;
}

kern_return_t
pmap_expand_pml4(
	pmap_t          map,
	vm_map_offset_t vaddr,
	unsigned int options)
{
	vm_page_t       m;
	pmap_paddr_t    pa;
	uint64_t        i;
	ppnum_t         pn;
	pml4_entry_t    *pml4p;
	boolean_t       is_ept = is_ept_pmap(map);

	DBG("pmap_expand_pml4(%p,%p)\n", map, (void *)vaddr);

	/* With the exception of the kext "basement", the kernel's level 4
	 * pagetables must not be dynamically expanded.
	 */
	assert(map != kernel_pmap || (vaddr == KERNEL_BASEMENT));
	/*
	 *	Allocate a VM page for the pml4 page
	 */
	while ((m = vm_page_grab()) == VM_PAGE_NULL) {
		if (options & PMAP_EXPAND_OPTIONS_NOWAIT) {
			return KERN_RESOURCE_SHORTAGE;
		}
		VM_PAGE_WAIT();
	}
	/*
	 *	put the page into the pmap's obj list so it
	 *	can be found later.
	 */
	pn = VM_PAGE_GET_PHYS_PAGE(m);
	pa = i386_ptob(pn);
	i = pml4idx(map, vaddr);

	/*
	 *	Zero the page.
	 */
	pmap_zero_page(pn);

	vm_page_lockspin_queues();
	vm_page_wire(m, VM_KERN_MEMORY_PTE, TRUE);
	vm_page_unlock_queues();

	OSAddAtomic(1, &inuse_ptepages_count);
	OSAddAtomic64(1, &alloc_ptepages_count);
	PMAP_ZINFO_PALLOC(map, PAGE_SIZE);

	/* Take the oject lock (mutex) before the PMAP_LOCK (spinlock) */
	vm_object_lock(map->pm_obj_pml4);

	PMAP_LOCK_EXCLUSIVE(map);
	/*
	 *	See if someone else expanded us first
	 */
	if (pmap64_pdpt(map, vaddr) != PDPT_ENTRY_NULL) {
		PMAP_UNLOCK_EXCLUSIVE(map);
		vm_object_unlock(map->pm_obj_pml4);

		VM_PAGE_FREE(m);

		OSAddAtomic(-1, &inuse_ptepages_count);
		PMAP_ZINFO_PFREE(map, PAGE_SIZE);
		return KERN_SUCCESS;
	}

#if 0 /* DEBUG */
	if (0 != vm_page_lookup(map->pm_obj_pml4, (vm_object_offset_t)i * PAGE_SIZE)) {
		panic("pmap_expand_pml4: obj not empty, pmap %p pm_obj %p vaddr 0x%llx i 0x%llx\n",
		    map, map->pm_obj_pml4, vaddr, i);
	}
#endif
	vm_page_insert_wired(m, map->pm_obj_pml4, (vm_object_offset_t)i * PAGE_SIZE, VM_KERN_MEMORY_PTE);
	vm_object_unlock(map->pm_obj_pml4);

	/*
	 *	Set the page directory entry for this page table.
	 */
	pml4p = pmap64_pml4(map, vaddr); /* refetch under lock */

	pmap_store_pte(pml4p, pa_to_pte(pa)
	    | PTE_READ(is_ept)
	    | (is_ept ? INTEL_EPT_EX : INTEL_PTE_USER)
	    | PTE_WRITE(is_ept));
	pml4_entry_t    *upml4p;

	upml4p = pmap64_user_pml4(map, vaddr);
	pmap_store_pte(upml4p, pa_to_pte(pa)
	    | PTE_READ(is_ept)
	    | (is_ept ? INTEL_EPT_EX : INTEL_PTE_USER)
	    | PTE_WRITE(is_ept));

	PMAP_UNLOCK_EXCLUSIVE(map);

	return KERN_SUCCESS;
}

kern_return_t
pmap_expand_pdpt(pmap_t map, vm_map_offset_t vaddr, unsigned int options)
{
	vm_page_t       m;
	pmap_paddr_t    pa;
	uint64_t        i;
	ppnum_t         pn;
	pdpt_entry_t    *pdptp;
	boolean_t       is_ept = is_ept_pmap(map);

	DBG("pmap_expand_pdpt(%p,%p)\n", map, (void *)vaddr);

	while ((pdptp = pmap64_pdpt(map, vaddr)) == PDPT_ENTRY_NULL) {
		kern_return_t pep4kr = pmap_expand_pml4(map, vaddr, options);
		if (pep4kr != KERN_SUCCESS) {
			return pep4kr;
		}
	}

	/*
	 *	Allocate a VM page for the pdpt page
	 */
	while ((m = vm_page_grab()) == VM_PAGE_NULL) {
		if (options & PMAP_EXPAND_OPTIONS_NOWAIT) {
			return KERN_RESOURCE_SHORTAGE;
		}
		VM_PAGE_WAIT();
	}

	/*
	 *	put the page into the pmap's obj list so it
	 *	can be found later.
	 */
	pn = VM_PAGE_GET_PHYS_PAGE(m);
	pa = i386_ptob(pn);
	i = pdptidx(map, vaddr);

	/*
	 *	Zero the page.
	 */
	pmap_zero_page(pn);

	vm_page_lockspin_queues();
	vm_page_wire(m, VM_KERN_MEMORY_PTE, TRUE);
	vm_page_unlock_queues();

	OSAddAtomic(1, &inuse_ptepages_count);
	OSAddAtomic64(1, &alloc_ptepages_count);
	PMAP_ZINFO_PALLOC(map, PAGE_SIZE);

	/* Take the oject lock (mutex) before the PMAP_LOCK (spinlock) */
	vm_object_lock(map->pm_obj_pdpt);

	PMAP_LOCK_EXCLUSIVE(map);
	/*
	 *	See if someone else expanded us first
	 */
	if (pmap_pde(map, vaddr) != PD_ENTRY_NULL) {
		PMAP_UNLOCK_EXCLUSIVE(map);
		vm_object_unlock(map->pm_obj_pdpt);

		VM_PAGE_FREE(m);

		OSAddAtomic(-1, &inuse_ptepages_count);
		PMAP_ZINFO_PFREE(map, PAGE_SIZE);
		return KERN_SUCCESS;
	}

#if 0 /* DEBUG */
	if (0 != vm_page_lookup(map->pm_obj_pdpt, (vm_object_offset_t)i * PAGE_SIZE)) {
		panic("pmap_expand_pdpt: obj not empty, pmap %p pm_obj %p vaddr 0x%llx i 0x%llx\n",
		    map, map->pm_obj_pdpt, vaddr, i);
	}
#endif
	vm_page_insert_wired(m, map->pm_obj_pdpt, (vm_object_offset_t)i * PAGE_SIZE, VM_KERN_MEMORY_PTE);
	vm_object_unlock(map->pm_obj_pdpt);

	/*
	 *	Set the page directory entry for this page table.
	 */
	pdptp = pmap64_pdpt(map, vaddr); /* refetch under lock */

	pmap_store_pte(pdptp, pa_to_pte(pa)
	    | PTE_READ(is_ept)
	    | (is_ept ? INTEL_EPT_EX : INTEL_PTE_USER)
	    | PTE_WRITE(is_ept));

	PMAP_UNLOCK_EXCLUSIVE(map);

	return KERN_SUCCESS;
}



/*
 *	Routine:	pmap_expand
 *
 *	Expands a pmap to be able to map the specified virtual address.
 *
 *	Allocates new virtual memory for the P0 or P1 portion of the
 *	pmap, then re-maps the physical pages that were in the old
 *	pmap to be in the new pmap.
 *
 *	Must be called with the pmap system and the pmap unlocked,
 *	since these must be unlocked to use vm_allocate or vm_deallocate.
 *	Thus it must be called in a loop that checks whether the map
 *	has been expanded enough.
 *	(We won't loop forever, since page tables aren't shrunk.)
 */
kern_return_t
pmap_expand(
	pmap_t          map,
	vm_map_offset_t vaddr,
	unsigned int options)
{
	pt_entry_t              *pdp;
	vm_page_t               m;
	pmap_paddr_t            pa;
	uint64_t                i;
	ppnum_t                 pn;
	boolean_t               is_ept = is_ept_pmap(map);


	/*
	 * For the kernel, the virtual address must be in or above the basement
	 * which is for kexts and is in the 512GB immediately below the kernel..
	 * XXX - should use VM_MIN_KERNEL_AND_KEXT_ADDRESS not KERNEL_BASEMENT
	 */
	if (__improbable(map == kernel_pmap &&
	    !(vaddr >= KERNEL_BASEMENT && vaddr <= VM_MAX_KERNEL_ADDRESS))) {
		if ((options & PMAP_EXPAND_OPTIONS_ALIASMAP) == 0) {
			panic("pmap_expand: bad vaddr 0x%llx for kernel pmap", vaddr);
		}
	}

	while ((pdp = pmap_pde(map, vaddr)) == PD_ENTRY_NULL) {
		assert((options & PMAP_EXPAND_OPTIONS_ALIASMAP) == 0);
		kern_return_t pepkr = pmap_expand_pdpt(map, vaddr, options);
		if (pepkr != KERN_SUCCESS) {
			return pepkr;
		}
	}

	/*
	 *	Allocate a VM page for the pde entries.
	 */
	while ((m = vm_page_grab()) == VM_PAGE_NULL) {
		if (options & PMAP_EXPAND_OPTIONS_NOWAIT) {
			return KERN_RESOURCE_SHORTAGE;
		}
		VM_PAGE_WAIT();
	}

	/*
	 *	put the page into the pmap's obj list so it
	 *	can be found later.
	 */
	pn = VM_PAGE_GET_PHYS_PAGE(m);
	pa = i386_ptob(pn);
	i = pdeidx(map, vaddr);

	/*
	 *	Zero the page.
	 */
	pmap_zero_page(pn);

	vm_page_lockspin_queues();
	vm_page_wire(m, VM_KERN_MEMORY_PTE, TRUE);
	vm_page_unlock_queues();

	OSAddAtomic(1, &inuse_ptepages_count);
	OSAddAtomic64(1, &alloc_ptepages_count);
	PMAP_ZINFO_PALLOC(map, PAGE_SIZE);

	/* Take the oject lock (mutex) before the PMAP_LOCK (spinlock) */
	vm_object_lock(map->pm_obj);

	PMAP_LOCK_EXCLUSIVE(map);

	/*
	 *	See if someone else expanded us first
	 */
	if (pmap_pte(map, vaddr) != PT_ENTRY_NULL) {
		PMAP_UNLOCK_EXCLUSIVE(map);
		vm_object_unlock(map->pm_obj);

		VM_PAGE_FREE(m);

		OSAddAtomic(-1, &inuse_ptepages_count); //todo replace all with inlines
		PMAP_ZINFO_PFREE(map, PAGE_SIZE);
		return KERN_SUCCESS;
	}

#if 0 /* DEBUG */
	if (0 != vm_page_lookup(map->pm_obj, (vm_object_offset_t)i * PAGE_SIZE)) {
		panic("pmap_expand: obj not empty, pmap 0x%x pm_obj 0x%x vaddr 0x%llx i 0x%llx\n",
		    map, map->pm_obj, vaddr, i);
	}
#endif
	vm_page_insert_wired(m, map->pm_obj, (vm_object_offset_t)i * PAGE_SIZE, VM_KERN_MEMORY_PTE);
	vm_object_unlock(map->pm_obj);

	/*
	 *	Set the page directory entry for this page table.
	 */
	pdp = pmap_pde(map, vaddr);
	pmap_store_pte(pdp, pa_to_pte(pa)
	    | PTE_READ(is_ept)
	    | (is_ept ? INTEL_EPT_EX : INTEL_PTE_USER)
	    | PTE_WRITE(is_ept));

	PMAP_UNLOCK_EXCLUSIVE(map);

	return KERN_SUCCESS;
}
/*
 * Query a pmap to see what size a given virtual address is mapped with.
 * If the vaddr is not mapped, returns 0.
 */
vm_size_t
pmap_query_pagesize(
	pmap_t          pmap,
	vm_map_offset_t vaddr)
{
	pd_entry_t      *pdep;
	vm_size_t       size = 0;

	assert(!is_ept_pmap(pmap));
	PMAP_LOCK_EXCLUSIVE(pmap);

	pdep = pmap_pde(pmap, vaddr);
	if (pdep != PD_ENTRY_NULL) {
		if (*pdep & INTEL_PTE_PS) {
			size = I386_LPGBYTES;
		} else if (pmap_pte(pmap, vaddr) != PT_ENTRY_NULL) {
			size = I386_PGBYTES;
		}
	}

	PMAP_UNLOCK_EXCLUSIVE(pmap);

	return size;
}

/*
 * Ensure the page table hierarchy is filled in down to
 * the large page level. Additionally returns FAILURE if
 * a lower page table already exists.
 */
static kern_return_t
pmap_pre_expand_large_internal(
	pmap_t          pmap,
	vm_map_offset_t vaddr)
{
	ppnum_t         pn;
	pt_entry_t      *pte;
	boolean_t       is_ept = is_ept_pmap(pmap);
	kern_return_t   kr = KERN_SUCCESS;

	if (pmap64_pdpt(pmap, vaddr) == PDPT_ENTRY_NULL) {
		if (!pmap_next_page_hi(&pn, FALSE)) {
			panic("pmap_pre_expand_large no PDPT");
		}

		pmap_zero_page(pn);

		pte = pmap64_pml4(pmap, vaddr);

		pmap_store_pte(pte, pa_to_pte(i386_ptob(pn)) |
		    PTE_READ(is_ept) |
		    (is_ept ? INTEL_EPT_EX : INTEL_PTE_USER) |
		    PTE_WRITE(is_ept));

		pte = pmap64_user_pml4(pmap, vaddr);

		pmap_store_pte(pte, pa_to_pte(i386_ptob(pn)) |
		    PTE_READ(is_ept) |
		    (is_ept ? INTEL_EPT_EX : INTEL_PTE_USER) |
		    PTE_WRITE(is_ept));
	}

	if (pmap_pde(pmap, vaddr) == PD_ENTRY_NULL) {
		if (!pmap_next_page_hi(&pn, FALSE)) {
			panic("pmap_pre_expand_large no PDE");
		}

		pmap_zero_page(pn);

		pte = pmap64_pdpt(pmap, vaddr);

		pmap_store_pte(pte, pa_to_pte(i386_ptob(pn)) |
		    PTE_READ(is_ept) |
		    (is_ept ? INTEL_EPT_EX : INTEL_PTE_USER) |
		    PTE_WRITE(is_ept));
	} else if (pmap_pte(pmap, vaddr) != PT_ENTRY_NULL) {
		kr = KERN_FAILURE;
	}

	return kr;
}

/*
 * Wrapper that locks the pmap.
 */
kern_return_t
pmap_pre_expand_large(
	pmap_t          pmap,
	vm_map_offset_t vaddr)
{
	kern_return_t   kr;

	PMAP_LOCK_EXCLUSIVE(pmap);
	kr = pmap_pre_expand_large_internal(pmap, vaddr);
	PMAP_UNLOCK_EXCLUSIVE(pmap);
	return kr;
}

/*
 * On large memory machines, pmap_steal_memory() will allocate past
 * the 1GB of pre-allocated/mapped virtual kernel area. This function
 * expands kernel the page tables to cover a given vaddr. It uses pages
 * from the same pool that pmap_steal_memory() uses, since vm_page_grab()
 * isn't available yet.
 */
void
pmap_pre_expand(
	pmap_t          pmap,
	vm_map_offset_t vaddr)
{
	ppnum_t         pn;
	pt_entry_t      *pte;
	boolean_t       is_ept = is_ept_pmap(pmap);

	/*
	 * This returns failure if a 4K page table already exists.
	 * Othewise it fills in the page table hierarchy down
	 * to that level.
	 */
	PMAP_LOCK_EXCLUSIVE(pmap);
	if (pmap_pre_expand_large_internal(pmap, vaddr) == KERN_FAILURE) {
		PMAP_UNLOCK_EXCLUSIVE(pmap);
		return;
	}

	/* Add the lowest table */
	if (!pmap_next_page_hi(&pn, FALSE)) {
		panic("pmap_pre_expand");
	}

	pmap_zero_page(pn);

	pte = pmap_pde(pmap, vaddr);

	pmap_store_pte(pte, pa_to_pte(i386_ptob(pn)) |
	    PTE_READ(is_ept) |
	    (is_ept ? INTEL_EPT_EX : INTEL_PTE_USER) |
	    PTE_WRITE(is_ept));
	PMAP_UNLOCK_EXCLUSIVE(pmap);
}

/*
 * pmap_sync_page_data_phys(ppnum_t pa)
 *
 * Invalidates all of the instruction cache on a physical page and
 * pushes any dirty data from the data cache for the same physical page
 * Not required in i386.
 */
void
pmap_sync_page_data_phys(__unused ppnum_t pa)
{
	return;
}

/*
 * pmap_sync_page_attributes_phys(ppnum_t pa)
 *
 * Write back and invalidate all cachelines on a physical page.
 */
void
pmap_sync_page_attributes_phys(ppnum_t pa)
{
	cache_flush_page_phys(pa);
}

void
pmap_copy_page(ppnum_t src, ppnum_t dst)
{
	bcopy_phys((addr64_t)i386_ptob(src),
	    (addr64_t)i386_ptob(dst),
	    PAGE_SIZE);
}


/*
 *	Routine:	pmap_pageable
 *	Function:
 *		Make the specified pages (by pmap, offset)
 *		pageable (or not) as requested.
 *
 *		A page which is not pageable may not take
 *		a fault; therefore, its page table entry
 *		must remain valid for the duration.
 *
 *		This routine is merely advisory; pmap_enter
 *		will specify that these pages are to be wired
 *		down (or not) as appropriate.
 */
void
pmap_pageable(
	__unused pmap_t                 pmap,
	__unused vm_map_offset_t        start_addr,
	__unused vm_map_offset_t        end_addr,
	__unused boolean_t              pageable)
{
#ifdef  lint
	pmap++; start_addr++; end_addr++; pageable++;
#endif  /* lint */
}

void
invalidate_icache(__unused vm_offset_t  addr,
    __unused unsigned     cnt,
    __unused int          phys)
{
	return;
}

void
flush_dcache(__unused vm_offset_t       addr,
    __unused unsigned          count,
    __unused int               phys)
{
	return;
}

#if CONFIG_DTRACE
/*
 * Constrain DTrace copyin/copyout actions
 */
extern kern_return_t dtrace_copyio_preflight(addr64_t);
extern kern_return_t dtrace_copyio_postflight(addr64_t);

kern_return_t
dtrace_copyio_preflight(__unused addr64_t va)
{
	thread_t thread = current_thread();
	uint64_t ccr3;
	if (current_map() == kernel_map) {
		return KERN_FAILURE;
	} else if (((ccr3 = get_cr3_base()) != thread->map->pmap->pm_cr3) && (no_shared_cr3 == FALSE)) {
		return KERN_FAILURE;
	} else if (no_shared_cr3 && (ccr3 != kernel_pmap->pm_cr3)) {
		return KERN_FAILURE;
	} else {
		return KERN_SUCCESS;
	}
}

kern_return_t
dtrace_copyio_postflight(__unused addr64_t va)
{
	return KERN_SUCCESS;
}
#endif /* CONFIG_DTRACE */

#include <mach_vm_debug.h>
#if     MACH_VM_DEBUG
#include <vm/vm_debug.h>

int
pmap_list_resident_pages(
	__unused pmap_t         pmap,
	__unused vm_offset_t    *listp,
	__unused int            space)
{
	return 0;
}
#endif  /* MACH_VM_DEBUG */


#if CONFIG_COREDUMP
/* temporary workaround */
boolean_t
coredumpok(__unused vm_map_t map, __unused mach_vm_offset_t va)
{
#if 0
	pt_entry_t     *ptep;

	ptep = pmap_pte(map->pmap, va);
	if (0 == ptep) {
		return FALSE;
	}
	return (*ptep & (INTEL_PTE_NCACHE | INTEL_PTE_WIRED)) != (INTEL_PTE_NCACHE | INTEL_PTE_WIRED);
#else
	return TRUE;
#endif
}
#endif

boolean_t
phys_page_exists(ppnum_t pn)
{
	assert(pn != vm_page_fictitious_addr);

	if (!pmap_initialized) {
		return TRUE;
	}

	if (pn == vm_page_guard_addr) {
		return FALSE;
	}

	if (!IS_MANAGED_PAGE(ppn_to_pai(pn))) {
		return FALSE;
	}

	return TRUE;
}



void
pmap_switch(pmap_t tpmap)
{
	PMAP_TRACE_CONSTANT(PMAP_CODE(PMAP__SWITCH) | DBG_FUNC_START, VM_KERNEL_ADDRHIDE(tpmap));
	assert(ml_get_interrupts_enabled() == FALSE);
	set_dirbase(tpmap, current_thread(), cpu_number());
	PMAP_TRACE_CONSTANT(PMAP_CODE(PMAP__SWITCH) | DBG_FUNC_END);
}

void
pmap_require(pmap_t pmap)
{
	if (pmap != kernel_pmap) {
		zone_id_require(ZONE_ID_PMAP, sizeof(struct pmap), pmap);
	}
}

/*
 * disable no-execute capability on
 * the specified pmap
 */
void
pmap_disable_NX(__unused pmap_t pmap)
{
#if DEVELOPMENT || DEBUG
	pmap->nx_enabled = 0;
#endif
}

void
pmap_flush_context_init(pmap_flush_context *pfc)
{
	pfc->pfc_cpus = 0;
	pfc->pfc_invalid_global = 0;
}

static bool
pmap_tlbi_response(uint32_t lcpu, uint32_t rcpu, bool ngflush)
{
	bool responded = false;
	bool gflushed = (cpu_datap(rcpu)->cpu_tlb_invalid_global_count !=
	    cpu_datap(lcpu)->cpu_tlb_gen_counts_global[rcpu]);

	if (ngflush) {
		if (gflushed) {
			responded = true;
		}
	} else {
		if (gflushed) {
			responded = true;
		} else {
			bool lflushed = (cpu_datap(rcpu)->cpu_tlb_invalid_local_count !=
			    cpu_datap(lcpu)->cpu_tlb_gen_counts_local[rcpu]);
			if (lflushed) {
				responded = true;
			}
		}
	}

	if (responded == false) {
		if ((cpu_datap(rcpu)->cpu_tlb_invalid == 0) ||
		    !CPU_CR3_IS_ACTIVE(rcpu) ||
		    !cpu_is_running(rcpu)) {
			responded = true;
		}
	}
	return responded;
}

extern uint64_t TLBTimeOut;
void
pmap_flush(
	pmap_flush_context *pfc)
{
	unsigned int    my_cpu;
	unsigned int    cpu;
	cpumask_t       cpu_bit;
	cpumask_t       cpus_to_respond = 0;
	cpumask_t       cpus_to_signal = 0;
	cpumask_t       cpus_signaled = 0;
	boolean_t       flush_self = FALSE;
	uint64_t        deadline;
	bool            need_global_flush = false;

	mp_disable_preemption();

	my_cpu = cpu_number();
	cpus_to_signal = pfc->pfc_cpus;

	PMAP_TRACE_CONSTANT(PMAP_CODE(PMAP__FLUSH_DELAYED_TLBS) | DBG_FUNC_START,
	    NULL, cpus_to_signal);

	for (cpu = 0, cpu_bit = 1; cpu < real_ncpus && cpus_to_signal; cpu++, cpu_bit <<= 1) {
		if (cpus_to_signal & cpu_bit) {
			cpus_to_signal &= ~cpu_bit;

			if (!cpu_is_running(cpu)) {
				continue;
			}

			if (pfc->pfc_invalid_global & cpu_bit) {
				cpu_datap(cpu)->cpu_tlb_invalid_global = 1;
				need_global_flush = true;
			} else {
				cpu_datap(cpu)->cpu_tlb_invalid_local = 1;
			}
			cpu_datap(my_cpu)->cpu_tlb_gen_counts_global[cpu] = cpu_datap(cpu)->cpu_tlb_invalid_global_count;
			cpu_datap(my_cpu)->cpu_tlb_gen_counts_local[cpu] = cpu_datap(cpu)->cpu_tlb_invalid_local_count;
			mfence();

			if (cpu == my_cpu) {
				flush_self = TRUE;
				continue;
			}
			if (CPU_CR3_IS_ACTIVE(cpu)) {
				cpus_to_respond |= cpu_bit;
				i386_signal_cpu(cpu, MP_TLB_FLUSH, ASYNC);
			}
		}
	}
	cpus_signaled = cpus_to_respond;

	/*
	 * Flush local tlb if required.
	 * Do this now to overlap with other processors responding.
	 */
	if (flush_self) {
		process_pmap_updates(NULL, (pfc->pfc_invalid_global != 0), 0ULL, ~0ULL);
	}

	if (cpus_to_respond) {
		deadline = mach_absolute_time() +
		    (TLBTimeOut ? TLBTimeOut : LockTimeOut);
		boolean_t is_timeout_traced = FALSE;

		/*
		 * Wait for those other cpus to acknowledge
		 */
		while (cpus_to_respond != 0) {
			long orig_acks = 0;

			for (cpu = 0, cpu_bit = 1; cpu < real_ncpus; cpu++, cpu_bit <<= 1) {
				bool responded = false;
				if ((cpus_to_respond & cpu_bit) != 0) {
					responded = pmap_tlbi_response(my_cpu, cpu, need_global_flush);
					if (responded) {
						cpus_to_respond &= ~cpu_bit;
					}
					cpu_pause();
				}

				if (cpus_to_respond == 0) {
					break;
				}
			}
			if (cpus_to_respond && (mach_absolute_time() > deadline)) {
				if (machine_timeout_suspended()) {
					continue;
				}
				if (TLBTimeOut == 0) {
					if (is_timeout_traced) {
						continue;
					}

					PMAP_TRACE_CONSTANT(PMAP_CODE(PMAP__FLUSH_TLBS_TO),
					    NULL, cpus_to_signal, cpus_to_respond);

					is_timeout_traced = TRUE;
					continue;
				}
				orig_acks = NMIPI_acks;
				NMIPI_panic(cpus_to_respond, TLB_FLUSH_TIMEOUT);
				panic("Uninterruptible processor(s): CPU bitmap: 0x%llx, NMIPI acks: 0x%lx, now: 0x%lx, deadline: %llu",
				    cpus_to_respond, orig_acks, NMIPI_acks, deadline);
			}
		}
	}

	PMAP_TRACE_CONSTANT(PMAP_CODE(PMAP__FLUSH_DELAYED_TLBS) | DBG_FUNC_END,
	    NULL, cpus_signaled, flush_self);

	mp_enable_preemption();
}


static void
invept(void *eptp)
{
	struct {
		uint64_t eptp;
		uint64_t reserved;
	} __attribute__((aligned(16), packed)) invept_descriptor = {(uint64_t)eptp, 0};

	__asm__ volatile ("invept (%%rax), %%rcx"
                 : : "c" (PMAP_INVEPT_SINGLE_CONTEXT), "a" (&invept_descriptor)
                 : "cc", "memory");
}

/*
 * Called with pmap locked, we:
 *  - scan through per-cpu data to see which other cpus need to flush
 *  - send an IPI to each non-idle cpu to be flushed
 *  - wait for all to signal back that they are inactive or we see that
 *    they are at a safe point (idle).
 *  - flush the local tlb if active for this pmap
 *  - return ... the caller will unlock the pmap
 */

void
pmap_flush_tlbs(pmap_t  pmap, vm_map_offset_t startv, vm_map_offset_t endv, int options, pmap_flush_context *pfc)
{
	unsigned int    cpu;
	cpumask_t       cpu_bit;
	cpumask_t       cpus_to_signal = 0;
	unsigned int    my_cpu = cpu_number();
	pmap_paddr_t    pmap_cr3 = pmap->pm_cr3;
	boolean_t       flush_self = FALSE;
	uint64_t        deadline;
	boolean_t       pmap_is_shared = (pmap->pm_shared || (pmap == kernel_pmap));
	bool            need_global_flush = false;
	uint32_t        event_code;
	vm_map_offset_t event_startv, event_endv;
	boolean_t       is_ept = is_ept_pmap(pmap);

	assert((processor_avail_count < 2) ||
	    (ml_get_interrupts_enabled() && get_preemption_level() != 0));

	assert((endv - startv) >= PAGE_SIZE);
	assert(((endv | startv) & PAGE_MASK) == 0);

	if (__improbable(kdebug_enable)) {
		if (pmap == kernel_pmap) {
			event_code = PMAP_CODE(PMAP__FLUSH_KERN_TLBS);
			event_startv = VM_KERNEL_UNSLIDE_OR_PERM(startv);
			event_endv = VM_KERNEL_UNSLIDE_OR_PERM(endv);
		} else if (__improbable(is_ept)) {
			event_code = PMAP_CODE(PMAP__FLUSH_EPT);
			event_startv = startv;
			event_endv = endv;
		} else {
			event_code = PMAP_CODE(PMAP__FLUSH_TLBS);
			event_startv = startv;
			event_endv = endv;
		}
	}

	PMAP_TRACE_CONSTANT(event_code | DBG_FUNC_START,
	    VM_KERNEL_UNSLIDE_OR_PERM(pmap), options,
	    event_startv, event_endv);

	if (__improbable(is_ept)) {
		mp_cpus_call(CPUMASK_ALL, ASYNC, invept, (void*)pmap->pm_eptp);
		goto out;
	}

	/*
	 * Scan other cpus for matching active or task CR3.
	 * For idle cpus (with no active map) we mark them invalid but
	 * don't signal -- they'll check as they go busy.
	 */
	if (pmap_pcid_ncpus) {
		if (pmap_is_shared) {
			need_global_flush = true;
		}
		pmap_pcid_invalidate_all_cpus(pmap);
		mfence();
	}

	for (cpu = 0, cpu_bit = 1; cpu < real_ncpus; cpu++, cpu_bit <<= 1) {
		if (!cpu_is_running(cpu)) {
			continue;
		}
		uint64_t        cpu_active_cr3 = CPU_GET_ACTIVE_CR3(cpu);
		uint64_t        cpu_task_cr3 = CPU_GET_TASK_CR3(cpu);

		if ((pmap_cr3 == cpu_task_cr3) ||
		    (pmap_cr3 == cpu_active_cr3) ||
		    (pmap_is_shared)) {
			if (options & PMAP_DELAY_TLB_FLUSH) {
				if (need_global_flush == true) {
					pfc->pfc_invalid_global |= cpu_bit;
				}
				pfc->pfc_cpus |= cpu_bit;

				continue;
			}
			if (need_global_flush == true) {
				cpu_datap(my_cpu)->cpu_tlb_gen_counts_global[cpu] = cpu_datap(cpu)->cpu_tlb_invalid_global_count;
				cpu_datap(cpu)->cpu_tlb_invalid_global = 1;
			} else {
				cpu_datap(my_cpu)->cpu_tlb_gen_counts_local[cpu] = cpu_datap(cpu)->cpu_tlb_invalid_local_count;
				cpu_datap(cpu)->cpu_tlb_invalid_local = 1;
			}

			if (cpu == my_cpu) {
				flush_self = TRUE;
				continue;
			}

			mfence();

			/*
			 * We don't need to signal processors which will flush
			 * lazily at the idle state or kernel boundary.
			 * For example, if we're invalidating the kernel pmap,
			 * processors currently in userspace don't need to flush
			 * their TLBs until the next time they enter the kernel.
			 * Alterations to the address space of a task active
			 * on a remote processor result in a signal, to
			 * account for copy operations. (There may be room
			 * for optimization in such cases).
			 * The order of the loads below with respect
			 * to the store to the "cpu_tlb_invalid" field above
			 * is important--hence the barrier.
			 */
			if (CPU_CR3_IS_ACTIVE(cpu) &&
			    (pmap_cr3 == CPU_GET_ACTIVE_CR3(cpu) ||
			    pmap->pm_shared ||
			    (pmap_cr3 == CPU_GET_TASK_CR3(cpu)))) {
				cpus_to_signal |= cpu_bit;
				i386_signal_cpu(cpu, MP_TLB_FLUSH, ASYNC);
			}
		}
	}

	if ((options & PMAP_DELAY_TLB_FLUSH)) {
		goto out;
	}

	/*
	 * Flush local tlb if required.
	 * Do this now to overlap with other processors responding.
	 */
	if (flush_self) {
		process_pmap_updates(pmap, pmap_is_shared, startv, endv);
	}

	if (cpus_to_signal) {
		cpumask_t       cpus_to_respond = cpus_to_signal;

		deadline = mach_absolute_time() +
		    (TLBTimeOut ? TLBTimeOut : LockTimeOut);
		boolean_t is_timeout_traced = FALSE;

		/*
		 * Wait for those other cpus to acknowledge
		 */
		while (cpus_to_respond != 0) {
			long orig_acks = 0;

			for (cpu = 0, cpu_bit = 1; cpu < real_ncpus; cpu++, cpu_bit <<= 1) {
				bool responded = false;
				if ((cpus_to_respond & cpu_bit) != 0) {
					responded = pmap_tlbi_response(my_cpu, cpu, need_global_flush);
					if (responded) {
						cpus_to_respond &= ~cpu_bit;
					}
					cpu_pause();
				}
				if (cpus_to_respond == 0) {
					break;
				}
			}
			if (cpus_to_respond && (mach_absolute_time() > deadline)) {
				if (machine_timeout_suspended()) {
					continue;
				}
				if (TLBTimeOut == 0) {
					/* cut tracepoint but don't panic */
					if (is_timeout_traced) {
						continue;
					}

					PMAP_TRACE_CONSTANT(PMAP_CODE(PMAP__FLUSH_TLBS_TO),
					    VM_KERNEL_UNSLIDE_OR_PERM(pmap),
					    cpus_to_signal,
					    cpus_to_respond);

					is_timeout_traced = TRUE;
					continue;
				}
				orig_acks = NMIPI_acks;
				uint64_t tstamp1 = mach_absolute_time();
				NMIPI_panic(cpus_to_respond, TLB_FLUSH_TIMEOUT);
				uint64_t tstamp2 = mach_absolute_time();
				panic("IPI timeout, unresponsive CPU bitmap: 0x%llx, NMIPI acks: 0x%lx, now: 0x%lx, deadline: %llu, pre-NMIPI time: 0x%llx, current: 0x%llx, global: %d",
				    cpus_to_respond, orig_acks, NMIPI_acks, deadline, tstamp1, tstamp2, need_global_flush);
			}
		}
	}

	if (__improbable((pmap == kernel_pmap) && (flush_self != TRUE))) {
		panic("pmap_flush_tlbs: pmap == kernel_pmap && flush_self != TRUE; kernel CR3: 0x%llX, pmap_cr3: 0x%llx, CPU active CR3: 0x%llX, CPU Task Map: %d", kernel_pmap->pm_cr3, pmap_cr3, current_cpu_datap()->cpu_active_cr3, current_cpu_datap()->cpu_task_map);
	}

out:
	PMAP_TRACE_CONSTANT(event_code | DBG_FUNC_END,
	    VM_KERNEL_UNSLIDE_OR_PERM(pmap), cpus_to_signal,
	    event_startv, event_endv);
}

static void
process_pmap_updates(pmap_t p, bool pshared, addr64_t istart, addr64_t iend)
{
	int ccpu = cpu_number();
	bool gtlbf = false;

	pmap_assert(ml_get_interrupts_enabled() == 0 ||
	    get_preemption_level() != 0);

	if (cpu_datap(ccpu)->cpu_tlb_invalid_global) {
		cpu_datap(ccpu)->cpu_tlb_invalid_global_count++;
		cpu_datap(ccpu)->cpu_tlb_invalid = 0;
		gtlbf = true;
	} else {
		cpu_datap(ccpu)->cpu_tlb_invalid_local_count++;
		cpu_datap(ccpu)->cpu_tlb_invalid_local = 0;
	}

	if (pmap_pcid_ncpus) {
		if (p) {
			/* TODO global generation count to
			 * avoid potentially redundant
			 * csw invalidations post-global invalidation
			 */
			pmap_pcid_validate_cpu(p, ccpu);
			pmap_tlbi_range(istart, iend, (pshared || gtlbf), p->pmap_pcid_cpus[ccpu]);
		} else {
			pmap_pcid_validate_current();
			pmap_tlbi_range(istart, iend, true, 0);
		}
	} else {
		pmap_tlbi_range(0, ~0ULL, true, 0);
	}
}

void
pmap_update_interrupt(void)
{
	PMAP_TRACE(PMAP_CODE(PMAP__UPDATE_INTERRUPT) | DBG_FUNC_START);

	if (current_cpu_datap()->cpu_tlb_invalid) {
		process_pmap_updates(NULL, true, 0ULL, ~0ULL);
	}

	PMAP_TRACE(PMAP_CODE(PMAP__UPDATE_INTERRUPT) | DBG_FUNC_END);
}

#include <mach/mach_vm.h>       /* mach_vm_region_recurse() */
/* Scan kernel pmap for W+X PTEs, scan kernel VM map for W+X map entries
 * and identify ranges with mismatched VM permissions and PTE permissions
 */
kern_return_t
pmap_permissions_verify(pmap_t ipmap, vm_map_t ivmmap, vm_offset_t sv, vm_offset_t ev)
{
	vm_offset_t cv = sv;
	kern_return_t rv = KERN_SUCCESS;
	uint64_t skip4 = 0, skip2 = 0;

	assert(!is_ept_pmap(ipmap));

	sv &= ~PAGE_MASK_64;
	ev &= ~PAGE_MASK_64;
	while (cv < ev) {
		if (__improbable((cv > 0x00007FFFFFFFFFFFULL) &&
		    (cv < 0xFFFF800000000000ULL))) {
			cv = 0xFFFF800000000000ULL;
		}
		/* Potential inconsistencies from not holding pmap lock
		 * but harmless for the moment.
		 */
		if (((cv & PML4MASK) == 0) && (pmap64_pml4(ipmap, cv) == 0)) {
			if ((cv + NBPML4) > cv) {
				cv += NBPML4;
			} else {
				break;
			}
			skip4++;
			continue;
		}
		if (((cv & PDMASK) == 0) && (pmap_pde(ipmap, cv) == 0)) {
			if ((cv + NBPD) > cv) {
				cv += NBPD;
			} else {
				break;
			}
			skip2++;
			continue;
		}

		pt_entry_t *ptep = pmap_pte(ipmap, cv);
		if (ptep && (*ptep & INTEL_PTE_VALID)) {
			if (*ptep & INTEL_PTE_WRITE) {
				if (!(*ptep & INTEL_PTE_NX)) {
					kprintf("W+X PTE at 0x%lx, P4: 0x%llx, P3: 0x%llx, P2: 0x%llx, PT: 0x%llx, VP: %u\n", cv, *pmap64_pml4(ipmap, cv), *pmap64_pdpt(ipmap, cv), *pmap_pde(ipmap, cv), *ptep, pmap_valid_page((ppnum_t)(i386_btop(pte_to_pa(*ptep)))));
					rv = KERN_FAILURE;
				}
			}
		}
		cv += PAGE_SIZE;
	}
	kprintf("Completed pmap scan\n");
	cv = sv;

	struct vm_region_submap_info_64 vbr;
	mach_msg_type_number_t vbrcount = 0;
	mach_vm_size_t  vmsize;
	vm_prot_t       prot;
	uint32_t nesting_depth = 0;
	kern_return_t kret;

	while (cv < ev) {
		for (;;) {
			vbrcount = VM_REGION_SUBMAP_INFO_COUNT_64;
			if ((kret = mach_vm_region_recurse(ivmmap,
			    (mach_vm_address_t *) &cv, &vmsize, &nesting_depth,
			    (vm_region_recurse_info_t)&vbr,
			    &vbrcount)) != KERN_SUCCESS) {
				break;
			}

			if (vbr.is_submap) {
				nesting_depth++;
				continue;
			} else {
				break;
			}
		}

		if (kret != KERN_SUCCESS) {
			break;
		}

		prot = vbr.protection;

		if ((prot & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == (VM_PROT_WRITE | VM_PROT_EXECUTE)) {
			kprintf("W+X map entry at address 0x%lx\n", cv);
			rv = KERN_FAILURE;
		}

		if (prot) {
			vm_offset_t pcv;
			for (pcv = cv; pcv < cv + vmsize; pcv += PAGE_SIZE) {
				pt_entry_t *ptep = pmap_pte(ipmap, pcv);
				vm_prot_t tprot;

				if ((ptep == NULL) || !(*ptep & INTEL_PTE_VALID)) {
					continue;
				}
				tprot = VM_PROT_READ;
				if (*ptep & INTEL_PTE_WRITE) {
					tprot |= VM_PROT_WRITE;
				}
				if ((*ptep & INTEL_PTE_NX) == 0) {
					tprot |= VM_PROT_EXECUTE;
				}
				if (tprot != prot) {
					kprintf("PTE/map entry permissions mismatch at address 0x%lx, pte: 0x%llx, protection: 0x%x\n", pcv, *ptep, prot);
					rv = KERN_FAILURE;
				}
			}
		}
		cv += vmsize;
	}
	return rv;
}

#if MACH_ASSERT
extern int pmap_ledgers_panic;
extern int pmap_ledgers_panic_leeway;

static void
pmap_check_ledgers(
	pmap_t pmap)
{
	int     pid;
	char    *procname;

	if (pmap->pmap_pid == 0) {
		/*
		 * This pmap was not or is no longer fully associated
		 * with a task (e.g. the old pmap after a fork()/exec() or
		 * spawn()).  Its "ledger" still points at a task that is
		 * now using a different (and active) address space, so
		 * we can't check that all the pmap ledgers are balanced here.
		 *
		 * If the "pid" is set, that means that we went through
		 * pmap_set_process() in task_terminate_internal(), so
		 * this task's ledger should not have been re-used and
		 * all the pmap ledgers should be back to 0.
		 */
		return;
	}

	pid = pmap->pmap_pid;
	procname = pmap->pmap_procname;

	vm_map_pmap_check_ledgers(pmap, pmap->ledger, pid, procname);

	if (pmap->stats.resident_count != 0 ||
#if 35156815
	    /*
	     * "wired_count" is unfortunately a bit inaccurate, so let's
	     * tolerate some slight deviation to limit the amount of
	     * somewhat-spurious assertion failures.
	     */
	    pmap->stats.wired_count > 10 ||
#else /* 35156815 */
	    pmap->stats.wired_count != 0 ||
#endif /* 35156815 */
	    pmap->stats.device != 0 ||
	    pmap->stats.internal != 0 ||
	    pmap->stats.external != 0 ||
	    pmap->stats.reusable != 0 ||
	    pmap->stats.compressed != 0) {
		if (pmap_stats_assert &&
		    pmap->pmap_stats_assert) {
			panic("pmap_destroy(%p) %d[%s] imbalanced stats: resident=%d wired=%d device=%d internal=%d external=%d reusable=%d compressed=%lld",
			    pmap, pid, procname,
			    pmap->stats.resident_count,
			    pmap->stats.wired_count,
			    pmap->stats.device,
			    pmap->stats.internal,
			    pmap->stats.external,
			    pmap->stats.reusable,
			    pmap->stats.compressed);
		} else {
			printf("pmap_destroy(%p) %d[%s] imbalanced stats: resident=%d wired=%d device=%d internal=%d external=%d reusable=%d compressed=%lld",
			    pmap, pid, procname,
			    pmap->stats.resident_count,
			    pmap->stats.wired_count,
			    pmap->stats.device,
			    pmap->stats.internal,
			    pmap->stats.external,
			    pmap->stats.reusable,
			    pmap->stats.compressed);
		}
	}
}

void
pmap_set_process(
	pmap_t pmap,
	int pid,
	char *procname)
{
	if (pmap == NULL) {
		return;
	}

	pmap->pmap_pid = pid;
	strlcpy(pmap->pmap_procname, procname, sizeof(pmap->pmap_procname));
	if (pmap_ledgers_panic_leeway) {
		/*
		 * XXX FBDP
		 * Some processes somehow trigger some issues that make
		 * the pmap stats and ledgers go off track, causing
		 * some assertion failures and ledger panics.
		 * Turn off the sanity checks if we allow some ledger leeway
		 * because of that.  We'll still do a final check in
		 * pmap_check_ledgers() for discrepancies larger than the
		 * allowed leeway after the address space has been fully
		 * cleaned up.
		 */
		pmap->pmap_stats_assert = FALSE;
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.phys_footprint);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.internal);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.internal_compressed);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.iokit_mapped);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.alternate_accounting);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.alternate_accounting_compressed);
	}
}
#endif /* MACH_ASSERT */


#if DEVELOPMENT || DEBUG
int pmap_pagezero_mitigation = 1;
#endif

void
pmap_advise_pagezero_range(pmap_t lpmap, uint64_t low_bound)
{
#if DEVELOPMENT || DEBUG
	if (pmap_pagezero_mitigation == 0) {
		lpmap->pagezero_accessible = FALSE;
		return;
	}
#endif
	lpmap->pagezero_accessible = ((pmap_smap_enabled == FALSE) && (low_bound < 0x1000));
	if (lpmap == current_pmap()) {
		mp_disable_preemption();
		current_cpu_datap()->cpu_pagezero_mapped = lpmap->pagezero_accessible;
		mp_enable_preemption();
	}
}

uintptr_t
pmap_verify_noncacheable(uintptr_t vaddr)
{
	pt_entry_t *ptep = NULL;
	ptep = pmap_pte(kernel_pmap, vaddr);
	if (ptep == NULL) {
		panic("pmap_verify_noncacheable: no translation for 0x%lx", vaddr);
	}
	/* Non-cacheable OK */
	if (*ptep & (INTEL_PTE_NCACHE)) {
		return pte_to_pa(*ptep) | (vaddr & INTEL_OFFMASK);
	}
	/* Write-combined OK */
	if (*ptep & (INTEL_PTE_PAT)) {
		return pte_to_pa(*ptep) | (vaddr & INTEL_OFFMASK);
	}
	panic("pmap_verify_noncacheable: IO read from a cacheable address? address: 0x%lx, PTE: %p, *PTE: 0x%llx", vaddr, ptep, *ptep);
	/*NOTREACHED*/
	return 0;
}

void
trust_cache_init(void)
{
	// Unsupported on this architecture.
}

kern_return_t
pmap_load_legacy_trust_cache(struct pmap_legacy_trust_cache __unused *trust_cache,
    const vm_size_t __unused trust_cache_len)
{
	// Unsupported on this architecture.
	return KERN_NOT_SUPPORTED;
}

pmap_tc_ret_t
pmap_load_image4_trust_cache(struct pmap_image4_trust_cache __unused *trust_cache,
    const vm_size_t __unused trust_cache_len,
    uint8_t const * __unused img4_manifest,
    const vm_size_t __unused img4_manifest_buffer_len,
    const vm_size_t __unused img4_manifest_actual_len,
    bool __unused dry_run)
{
	// Unsupported on this architecture.
	return PMAP_TC_UNKNOWN_FORMAT;
}


bool
pmap_is_trust_cache_loaded(const uuid_t __unused uuid)
{
	// Unsupported on this architecture.
	return false;
}

bool
pmap_lookup_in_loaded_trust_caches(const uint8_t __unused cdhash[20])
{
	// Unsupported on this architecture.
	return false;
}

uint32_t
pmap_lookup_in_static_trust_cache(const uint8_t __unused cdhash[20])
{
	// Unsupported on this architecture.
	return false;
}

SIMPLE_LOCK_DECLARE(pmap_compilation_service_cdhash_lock, 0);
uint8_t pmap_compilation_service_cdhash[CS_CDHASH_LEN] = { 0 };

void
pmap_set_compilation_service_cdhash(const uint8_t cdhash[CS_CDHASH_LEN])
{
	simple_lock(&pmap_compilation_service_cdhash_lock, LCK_GRP_NULL);
	memcpy(pmap_compilation_service_cdhash, cdhash, CS_CDHASH_LEN);
	simple_unlock(&pmap_compilation_service_cdhash_lock);

#if DEVELOPMENT || DEBUG
	printf("Added Compilation Service CDHash through the PMAP: 0x%02X 0x%02X 0x%02X 0x%02X\n", cdhash[0], cdhash[1], cdhash[2], cdhash[4]);
#endif
}

bool
pmap_match_compilation_service_cdhash(const uint8_t cdhash[CS_CDHASH_LEN])
{
	bool match = false;

	simple_lock(&pmap_compilation_service_cdhash_lock, LCK_GRP_NULL);
	if (bcmp(pmap_compilation_service_cdhash, cdhash, CS_CDHASH_LEN) == 0) {
		match = true;
	}
	simple_unlock(&pmap_compilation_service_cdhash_lock);

#if DEVELOPMENT || DEBUG
	if (match) {
		printf("Matched Compilation Service CDHash through the PMAP\n");
	}
#endif

	return match;
}

bool
pmap_in_ppl(void)
{
	// Nonexistent on this architecture.
	return false;
}

void
pmap_lockdown_image4_slab(__unused vm_offset_t slab, __unused vm_size_t slab_len, __unused uint64_t flags)
{
	// Unsupported on this architecture.
}

kern_return_t
pmap_cs_allow_invalid(__unused pmap_t pmap)
{
	// Unsupported on this architecture.
	return KERN_SUCCESS;
}

void *
pmap_claim_reserved_ppl_page(void)
{
	// Unsupported on this architecture.
	return NULL;
}

void
pmap_free_reserved_ppl_page(void __unused *kva)
{
	// Unsupported on this architecture.
}

#if DEVELOPMENT || DEBUG
/*
 * Used for unit testing recovery from text corruptions.
 */
kern_return_t
pmap_test_text_corruption(pmap_paddr_t pa)
{
	int pai;
	uint8_t *va;

	pai = ppn_to_pai(atop(pa));
	if (!IS_MANAGED_PAGE(pai)) {
		return KERN_FAILURE;
	}

	va = (uint8_t *)PHYSMAP_PTOV(pa);
	va[0] = 0x0f; /* opcode for UD2 */
	va[1] = 0x0b;

	return KERN_SUCCESS;
}
#endif /* DEVELOPMENT || DEBUG */
