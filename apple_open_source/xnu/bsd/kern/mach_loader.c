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
 *	Copyright (C) 1988, 1989,  NeXT, Inc.
 *
 *	File:	kern/mach_loader.c
 *	Author:	Avadis Tevanian, Jr.
 *
 *	Mach object file loader (kernel version, for now).
 *
 * 21-Jul-88  Avadis Tevanian, Jr. (avie) at NeXT
 *	Started.
 */

#include <sys/param.h>
#include <sys/vnode_internal.h>
#include <sys/uio.h>
#include <sys/namei.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/malloc.h>
#include <sys/mount_internal.h>
#include <sys/fcntl.h>
#include <sys/file_internal.h>
#include <sys/ubc_internal.h>
#include <sys/imgact.h>
#include <sys/codesign.h>
#include <sys/proc_uuid_policy.h>
#include <sys/reason.h>
#include <sys/kdebug.h>
#include <sys/spawn_internal.h>

#include <mach/mach_types.h>
#include <mach/vm_map.h>        /* vm_allocate() */
#include <mach/mach_vm.h>       /* mach_vm_allocate() */
#include <mach/vm_statistics.h>
#include <mach/task.h>
#include <mach/thread_act.h>

#include <machine/vmparam.h>
#include <machine/exec.h>
#include <machine/pal_routines.h>

#include <kern/ast.h>
#include <kern/kern_types.h>
#include <kern/cpu_number.h>
#include <kern/mach_loader.h>
#include <kern/mach_fat.h>
#include <kern/kalloc.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/page_decrypt.h>

#include <mach-o/fat.h>
#include <mach-o/loader.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_pager.h>
#include <vm/vnode_pager.h>
#include <vm/vm_protos.h>
#include <vm/vm_shared_region.h>
#include <IOKit/IOReturn.h>     /* for kIOReturnNotPrivileged */
#include <IOKit/IOBSD.h>        /* for IOVnodeHasEntitlement */

#include <os/overflow.h>

/*
 * XXX vm/pmap.h should not treat these prototypes as MACH_KERNEL_PRIVATE
 * when KERNEL is defined.
 */
extern pmap_t   pmap_create_options(ledger_t ledger, vm_map_size_t size,
    unsigned int flags);
#if __has_feature(ptrauth_calls) && XNU_TARGET_OS_OSX
extern void pmap_disable_user_jop(pmap_t pmap);
#endif /* __has_feature(ptrauth_calls) && XNU_TARGET_OS_OSX */

/* XXX should have prototypes in a shared header file */
extern int      get_map_nentries(vm_map_t);

extern kern_return_t    memory_object_signed(memory_object_control_t control,
    boolean_t is_signed);

/* An empty load_result_t */
static const load_result_t load_result_null = {
	.mach_header = MACH_VM_MIN_ADDRESS,
	.entry_point = MACH_VM_MIN_ADDRESS,
	.user_stack = MACH_VM_MIN_ADDRESS,
	.user_stack_size = 0,
	.user_stack_alloc = MACH_VM_MIN_ADDRESS,
	.user_stack_alloc_size = 0,
	.all_image_info_addr = MACH_VM_MIN_ADDRESS,
	.all_image_info_size = 0,
	.thread_count = 0,
	.unixproc = 0,
	.dynlinker = 0,
	.needs_dynlinker = 0,
	.validentry = 0,
	.using_lcmain = 0,
	.is_64bit_addr = 0,
	.is_64bit_data = 0,
	.custom_stack = 0,
	.csflags = 0,
	.has_pagezero = 0,
	.uuid = { 0 },
	.min_vm_addr = MACH_VM_MAX_ADDRESS,
	.max_vm_addr = MACH_VM_MIN_ADDRESS,
	.cs_end_offset = 0,
	.threadstate = NULL,
	.threadstate_sz = 0,
	.is_cambria = 0,
	.dynlinker_mach_header = MACH_VM_MIN_ADDRESS,
	.dynlinker_fd = -1,
};

/*
 * Prototypes of static functions.
 */
static load_return_t
parse_machfile(
	struct vnode            *vp,
	vm_map_t                map,
	thread_t                thread,
	struct mach_header      *header,
	off_t                   file_offset,
	off_t                   macho_size,
	int                     depth,
	int64_t                 slide,
	int64_t                 dyld_slide,
	load_result_t           *result,
	load_result_t           *binresult,
	struct image_params     *imgp
	);

static load_return_t
load_segment(
	struct load_command             *lcp,
	uint32_t                        filetype,
	void                            *control,
	off_t                           pager_offset,
	off_t                           macho_size,
	struct vnode                    *vp,
	vm_map_t                        map,
	int64_t                         slide,
	load_result_t                   *result,
	struct image_params             *imgp
	);

static load_return_t
load_uuid(
	struct uuid_command             *uulp,
	char                            *command_end,
	load_result_t                   *result
	);

static load_return_t
load_version(
	struct version_min_command     *vmc,
	boolean_t               *found_version_cmd,
	int                     ip_flags,
	load_result_t           *result
	);

static load_return_t
load_code_signature(
	struct linkedit_data_command    *lcp,
	struct vnode                    *vp,
	off_t                           macho_offset,
	off_t                           macho_size,
	cpu_type_t                      cputype,
	cpu_subtype_t                   cpusubtype,
	load_result_t                   *result,
	struct image_params             *imgp);

#if CONFIG_CODE_DECRYPTION
static load_return_t
set_code_unprotect(
	struct encryption_info_command  *lcp,
	caddr_t                         addr,
	vm_map_t                        map,
	int64_t                         slide,
	struct vnode                    *vp,
	off_t                           macho_offset,
	cpu_type_t                      cputype,
	cpu_subtype_t                   cpusubtype);
#endif

static
load_return_t
load_main(
	struct entry_point_command      *epc,
	thread_t                thread,
	int64_t                         slide,
	load_result_t           *result
	);

static
load_return_t
setup_driver_main(
	thread_t                thread,
	int64_t                         slide,
	load_result_t           *result
	);

static load_return_t
load_unixthread(
	struct thread_command   *tcp,
	thread_t                        thread,
	int64_t                         slide,
	boolean_t                       is_x86_64_compat_binary,
	load_result_t                   *result
	);

static load_return_t
load_threadstate(
	thread_t                thread,
	uint32_t        *ts,
	uint32_t        total_size,
	load_result_t *
	);

static load_return_t
load_threadstack(
	thread_t                thread,
	uint32_t                *ts,
	uint32_t                total_size,
	mach_vm_offset_t        *user_stack,
	int                     *customstack,
	boolean_t               is_x86_64_compat_binary,
	load_result_t           *result
	);

static load_return_t
load_threadentry(
	thread_t                thread,
	uint32_t        *ts,
	uint32_t        total_size,
	mach_vm_offset_t        *entry_point
	);

static load_return_t
load_dylinker(
	struct dylinker_command *lcp,
	integer_t               archbits,
	vm_map_t                map,
	thread_t                thread,
	int                     depth,
	int64_t                 slide,
	load_result_t           *result,
	struct image_params     *imgp
	);


#if __x86_64__
extern int bootarg_no32exec;
static boolean_t
check_if_simulator_binary(
	struct image_params     *imgp,
	off_t                   file_offset,
	off_t                   macho_size);
#endif

struct macho_data;

static load_return_t
get_macho_vnode(
	const char                      *path,
	integer_t               archbits,
	struct mach_header      *mach_header,
	off_t                   *file_offset,
	off_t                   *macho_size,
	struct macho_data       *macho_data,
	struct vnode            **vpp,
	struct image_params     *imgp
	);

static inline void
widen_segment_command(const struct segment_command *scp32,
    struct segment_command_64 *scp)
{
	scp->cmd = scp32->cmd;
	scp->cmdsize = scp32->cmdsize;
	bcopy(scp32->segname, scp->segname, sizeof(scp->segname));
	scp->vmaddr = scp32->vmaddr;
	scp->vmsize = scp32->vmsize;
	scp->fileoff = scp32->fileoff;
	scp->filesize = scp32->filesize;
	scp->maxprot = scp32->maxprot;
	scp->initprot = scp32->initprot;
	scp->nsects = scp32->nsects;
	scp->flags = scp32->flags;
}

static void
note_all_image_info_section(const struct segment_command_64 *scp,
    boolean_t is64, size_t section_size, const void *sections,
    int64_t slide, load_result_t *result)
{
	const union {
		struct section s32;
		struct section_64 s64;
	} *sectionp;
	unsigned int i;


	if (strncmp(scp->segname, "__DATA_DIRTY", sizeof(scp->segname)) != 0 &&
	    strncmp(scp->segname, "__DATA", sizeof(scp->segname)) != 0) {
		return;
	}
	for (i = 0; i < scp->nsects; ++i) {
		sectionp = (const void *)
		    ((const char *)sections + section_size * i);
		if (0 == strncmp(sectionp->s64.sectname, "__all_image_info",
		    sizeof(sectionp->s64.sectname))) {
			result->all_image_info_addr =
			    is64 ? sectionp->s64.addr : sectionp->s32.addr;
			result->all_image_info_addr += slide;
			result->all_image_info_size =
			    is64 ? sectionp->s64.size : sectionp->s32.size;
			return;
		}
	}
}

#if __arm64__
/*
 * Allow bypassing some security rules (hard pagezero, no write+execute)
 * in exchange for better binary compatibility for legacy apps built
 * before 16KB-alignment was enforced.
 */
const int fourk_binary_compatibility_unsafe = TRUE;
const int fourk_binary_compatibility_allow_wx = FALSE;
#endif /* __arm64__ */

#if __has_feature(ptrauth_calls) && XNU_TARGET_OS_OSX
/**
 * Determines whether this is an arm64e process which may host in-process
 * plugins.
 */
static inline bool
arm64e_plugin_host(struct image_params *imgp, load_result_t *result)
{
	if (imgp->ip_flags & IMGPF_NOJOP) {
		return false;
	}

	if (!result->platform_binary) {
		return false;
	}

	struct cs_blob *csblob = csvnode_get_blob(imgp->ip_vp, imgp->ip_arch_offset);
	const char *identity = csblob_get_identity(csblob);
	if (!identity) {
		return false;
	}

	/* Check if override host plugin entitlement is present and posix spawn attribute to disable A keys is passed */
	if (IOVnodeHasEntitlement(imgp->ip_vp, (int64_t)imgp->ip_arch_offset, OVERRIDE_PLUGIN_HOST_ENTITLEMENT)) {
		return imgp->ip_flags & IMGPF_PLUGIN_HOST_DISABLE_A_KEYS;
	}

	/* Disabling library validation is a good signal that this process plans to host plugins */
	const char *const disable_lv_entitlements[] = {
		"com.apple.security.cs.disable-library-validation",
		"com.apple.private.cs.automator-plugins",
		CLEAR_LV_ENTITLEMENT,
	};
	for (size_t i = 0; i < ARRAY_COUNT(disable_lv_entitlements); i++) {
		if (IOVnodeHasEntitlement(imgp->ip_vp, (int64_t)imgp->ip_arch_offset, disable_lv_entitlements[i])) {
			return true;
		}
	}

	/* From /System/Library/Security/HardeningExceptions.plist */
	const char *const hardening_exceptions[] = {
		"com.apple.perl5", /* Scripting engines may load third party code and jit*/
		"com.apple.perl", /* Scripting engines may load third party code and jit*/
		"org.python.python", /* Scripting engines may load third party code and jit*/
		"com.apple.expect", /* Scripting engines may load third party code and jit*/
		"com.tcltk.wish", /* Scripting engines may load third party code and jit*/
		"com.tcltk.tclsh", /* Scripting engines may load third party code and jit*/
		"com.apple.ruby", /* Scripting engines may load third party code and jit*/
		"com.apple.bash", /* Required for the 'enable' command */
		"com.apple.zsh", /* Required for the 'zmodload' command */
		"com.apple.ksh", /* Required for 'builtin' command */
	};
	for (size_t i = 0; i < ARRAY_COUNT(hardening_exceptions); i++) {
		if (strncmp(hardening_exceptions[i], identity, strlen(hardening_exceptions[i])) == 0) {
			return true;
		}
	}

	return false;
}
#endif /* __has_feature(ptrauth_calls) && XNU_TARGET_OS_OSX */

load_return_t
load_machfile(
	struct image_params     *imgp,
	struct mach_header      *header,
	thread_t                thread,
	vm_map_t                *mapp,
	load_result_t           *result
	)
{
	struct vnode            *vp = imgp->ip_vp;
	off_t                   file_offset = imgp->ip_arch_offset;
	off_t                   macho_size = imgp->ip_arch_size;
	off_t                   total_size = 0;
	off_t                   file_size = imgp->ip_vattr->va_data_size;
	pmap_t                  pmap = 0;       /* protected by create_map */
	vm_map_t                map;
	load_result_t           myresult;
	load_return_t           lret;
	boolean_t enforce_hard_pagezero = TRUE;
	int in_exec = (imgp->ip_flags & IMGPF_EXEC);
	task_t task = current_task();
	int64_t                 aslr_page_offset = 0;
	int64_t                 dyld_aslr_page_offset = 0;
	int64_t                 aslr_section_size = 0;
	int64_t                 aslr_section_offset = 0;
	kern_return_t           kret;
	unsigned int            pmap_flags = 0;

	if (os_add_overflow(file_offset, macho_size, &total_size) ||
	    total_size > file_size) {
		return LOAD_BADMACHO;
	}

	result->is_64bit_addr = ((imgp->ip_flags & IMGPF_IS_64BIT_ADDR) == IMGPF_IS_64BIT_ADDR);
	result->is_64bit_data = ((imgp->ip_flags & IMGPF_IS_64BIT_DATA) == IMGPF_IS_64BIT_DATA);
#if defined(HAS_APPLE_PAC)
	pmap_flags |= (imgp->ip_flags & IMGPF_NOJOP) ? PMAP_CREATE_DISABLE_JOP : 0;
#endif /* defined(HAS_APPLE_PAC) */
	pmap_flags |= result->is_64bit_addr ? PMAP_CREATE_64BIT : 0;

	task_t ledger_task;
	if (imgp->ip_new_thread) {
		ledger_task = get_threadtask(imgp->ip_new_thread);
	} else {
		ledger_task = task;
	}

#if XNU_TARGET_OS_OSX && _POSIX_SPAWN_FORCE_4K_PAGES && PMAP_CREATE_FORCE_4K_PAGES
	if (imgp->ip_px_sa != NULL) {
		struct _posix_spawnattr* psa = (struct _posix_spawnattr *) imgp->ip_px_sa;
		if (psa->psa_flags & _POSIX_SPAWN_FORCE_4K_PAGES) {
			pmap_flags |= PMAP_CREATE_FORCE_4K_PAGES;
		}
	}
#endif /* XNU_TARGET_OS_OSX && _POSIX_SPAWN_FORCE_4K_PAGES && PMAP_CREATE_FORCE_4K_PAGE */

	pmap = pmap_create_options(get_task_ledger(ledger_task),
	    (vm_map_size_t) 0,
	    pmap_flags);
	if (pmap == NULL) {
		return LOAD_RESOURCE;
	}
	map = vm_map_create(pmap,
	    0,
	    vm_compute_max_offset(result->is_64bit_addr),
	    TRUE);

#if defined(__arm64__)
	if (result->is_64bit_addr) {
		/* enforce 16KB alignment of VM map entries */
		vm_map_set_page_shift(map, SIXTEENK_PAGE_SHIFT);
	} else {
		vm_map_set_page_shift(map, page_shift_user32);
	}
#elif (__ARM_ARCH_7K__ >= 2) && defined(PLATFORM_WatchOS)
	/* enforce 16KB alignment for watch targets with new ABI */
	vm_map_set_page_shift(map, SIXTEENK_PAGE_SHIFT);
#endif /* __arm64__ */

#if PMAP_CREATE_FORCE_4K_PAGES
	if (pmap_flags & PMAP_CREATE_FORCE_4K_PAGES) {
		DEBUG4K_LIFE("***** launching '%s' as 4k *****\n", vp->v_name);
		vm_map_set_page_shift(map, FOURK_PAGE_SHIFT);
	}
#endif /* PMAP_CREATE_FORCE_4K_PAGES */

#ifndef CONFIG_ENFORCE_SIGNED_CODE
	/* This turns off faulting for executable pages, which allows
	 * to circumvent Code Signing Enforcement. The per process
	 * flag (CS_ENFORCEMENT) is not set yet, but we can use the
	 * global flag.
	 */
	if (!cs_process_global_enforcement() && (header->flags & MH_ALLOW_STACK_EXECUTION)) {
		vm_map_disable_NX(map);
		// TODO: Message Trace or log that this is happening
	}
#endif

	/* Forcibly disallow execution from data pages on even if the arch
	 * normally permits it. */
	if ((header->flags & MH_NO_HEAP_EXECUTION) && !(imgp->ip_flags & IMGPF_ALLOW_DATA_EXEC)) {
		vm_map_disallow_data_exec(map);
	}

	/*
	 * Compute a random offset for ASLR, and an independent random offset for dyld.
	 */
	if (!(imgp->ip_flags & IMGPF_DISABLE_ASLR)) {
		vm_map_get_max_aslr_slide_section(map, &aslr_section_offset, &aslr_section_size);
		aslr_section_offset = (random() % aslr_section_offset) * aslr_section_size;

		aslr_page_offset = random();
		aslr_page_offset %= vm_map_get_max_aslr_slide_pages(map);
		aslr_page_offset <<= vm_map_page_shift(map);

		dyld_aslr_page_offset = random();
		dyld_aslr_page_offset %= vm_map_get_max_loader_aslr_slide_pages(map);
		dyld_aslr_page_offset <<= vm_map_page_shift(map);

		aslr_page_offset += aslr_section_offset;
	}
	if (vm_map_page_shift(map) < (int)PAGE_SHIFT) {
		DEBUG4K_LOAD("slide=0x%llx dyld_slide=0x%llx\n", aslr_page_offset, dyld_aslr_page_offset);
	}

	if (!result) {
		result = &myresult;
	}

	*result = load_result_null;

	/*
	 * re-set the bitness on the load result since we cleared the load result above.
	 */
	result->is_64bit_addr = ((imgp->ip_flags & IMGPF_IS_64BIT_ADDR) == IMGPF_IS_64BIT_ADDR);
	result->is_64bit_data = ((imgp->ip_flags & IMGPF_IS_64BIT_DATA) == IMGPF_IS_64BIT_DATA);

	lret = parse_machfile(vp, map, thread, header, file_offset, macho_size,
	    0, aslr_page_offset, dyld_aslr_page_offset, result,
	    NULL, imgp);

	if (lret != LOAD_SUCCESS) {
		vm_map_deallocate(map); /* will lose pmap reference too */
		return lret;
	}

#if __x86_64__
	/*
	 * On x86, for compatibility, don't enforce the hard page-zero restriction for 32-bit binaries.
	 */
	if (!result->is_64bit_addr) {
		enforce_hard_pagezero = FALSE;
	}

	/*
	 * For processes with IMGPF_HIGH_BITS_ASLR, add a few random high bits
	 * to the start address for "anywhere" memory allocations.
	 */
#define VM_MAP_HIGH_START_BITS_COUNT 8
#define VM_MAP_HIGH_START_BITS_SHIFT 27
	if (result->is_64bit_addr &&
	    (imgp->ip_flags & IMGPF_HIGH_BITS_ASLR)) {
		int random_bits;
		vm_map_offset_t high_start;

		random_bits = random();
		random_bits &= (1 << VM_MAP_HIGH_START_BITS_COUNT) - 1;
		high_start = (((vm_map_offset_t)random_bits)
		        << VM_MAP_HIGH_START_BITS_SHIFT);
		vm_map_set_high_start(map, high_start);
	}
#endif /* __x86_64__ */

	/*
	 * Check to see if the page zero is enforced by the map->min_offset.
	 */
	if (enforce_hard_pagezero &&
	    (vm_map_has_hard_pagezero(map, 0x1000) == FALSE)) {
#if __arm64__
		if (
			!result->is_64bit_addr && /* not 64-bit address space */
			!(header->flags & MH_PIE) &&      /* not PIE */
			(vm_map_page_shift(map) != FOURK_PAGE_SHIFT ||
			PAGE_SHIFT != FOURK_PAGE_SHIFT) && /* page size != 4KB */
			result->has_pagezero && /* has a "soft" page zero */
			fourk_binary_compatibility_unsafe) {
			/*
			 * For backwards compatibility of "4K" apps on
			 * a 16K system, do not enforce a hard page zero...
			 */
		} else
#endif /* __arm64__ */
		{
			vm_map_deallocate(map); /* will lose pmap reference too */
			return LOAD_BADMACHO;
		}
	}

#if __arm64__
	if (enforce_hard_pagezero && result->is_64bit_addr && (header->cputype == CPU_TYPE_ARM64)) {
		/* 64 bit ARM binary must have "hard page zero" of 4GB to cover the lower 32 bit address space */
		if (vm_map_has_hard_pagezero(map, 0x100000000) == FALSE) {
			vm_map_deallocate(map); /* will lose pmap reference too */
			return LOAD_BADMACHO;
		}
	}
#endif

	vm_commit_pagezero_status(map);

	/*
	 * If this is an exec, then we are going to destroy the old
	 * task, and it's correct to halt it; if it's spawn, the
	 * task is not yet running, and it makes no sense.
	 */
	if (in_exec) {
		proc_t p = vfs_context_proc(imgp->ip_vfs_context);
		/*
		 * Mark the task as halting and start the other
		 * threads towards terminating themselves.  Then
		 * make sure any threads waiting for a process
		 * transition get informed that we are committed to
		 * this transition, and then finally complete the
		 * task halting (wait for threads and then cleanup
		 * task resources).
		 *
		 * NOTE: task_start_halt() makes sure that no new
		 * threads are created in the task during the transition.
		 * We need to mark the workqueue as exiting before we
		 * wait for threads to terminate (at the end of which
		 * we no longer have a prohibition on thread creation).
		 *
		 * Finally, clean up any lingering workqueue data structures
		 * that may have been left behind by the workqueue threads
		 * as they exited (and then clean up the work queue itself).
		 */
		kret = task_start_halt(task);
		if (kret != KERN_SUCCESS) {
			vm_map_deallocate(map); /* will lose pmap reference too */
			return LOAD_FAILURE;
		}
		proc_transcommit(p, 0);
		workq_mark_exiting(p);
		task_complete_halt(task);
		workq_exit(p);

		/*
		 * Roll up accounting info to new task. The roll up is done after
		 * task_complete_halt to make sure the thread accounting info is
		 * rolled up to current_task.
		 */
		task_rollup_accounting_info(get_threadtask(thread), task);
	}
	*mapp = map;

#if __has_feature(ptrauth_calls) && defined(XNU_TARGET_OS_OSX)
	/*
	 * arm64e plugin hosts currently run with JOP keys disabled, since they
	 * may need to run arm64 plugins.
	 */
	if (arm64e_plugin_host(imgp, result)) {
		imgp->ip_flags |= IMGPF_NOJOP;
		pmap_disable_user_jop(pmap);
	}
#endif /* __has_feature(ptrauth_calls) && defined(XNU_TARGET_OS_OSX) */


#ifdef CONFIG_32BIT_TELEMETRY
	if (!result->is_64bit_data) {
		/*
		 * This may not need to be an AST; we merely need to ensure that
		 * we gather telemetry at the point where all of the information
		 * that we want has been added to the process.
		 */
		task_set_32bit_log_flag(get_threadtask(thread));
		act_set_astbsd(thread);
	}
#endif /* CONFIG_32BIT_TELEMETRY */

	return LOAD_SUCCESS;
}

int macho_printf = 0;
#define MACHO_PRINTF(args)                              \
	do {                                            \
	        if (macho_printf) {                     \
	                printf args;                    \
	        }                                       \
	} while (0)


static boolean_t
pie_required(
	cpu_type_t exectype,
	cpu_subtype_t execsubtype)
{
	switch (exectype) {
	case CPU_TYPE_X86_64:
		return FALSE;
	case CPU_TYPE_ARM64:
		return TRUE;
	case CPU_TYPE_ARM:
		switch (execsubtype) {
		case CPU_SUBTYPE_ARM_V7K:
			return TRUE;
		}
		break;
	}
	return FALSE;
}

/*
 * The file size of a mach-o file is limited to 32 bits; this is because
 * this is the limit on the kalloc() of enough bytes for a mach_header and
 * the contents of its sizeofcmds, which is currently constrained to 32
 * bits in the file format itself.  We read into the kernel buffer the
 * commands section, and then parse it in order to parse the mach-o file
 * format load_command segment(s).  We are only interested in a subset of
 * the total set of possible commands. If "map"==VM_MAP_NULL or
 * "thread"==THREAD_NULL, do not make permament VM modifications,
 * just preflight the parse.
 */
static
load_return_t
parse_machfile(
	struct vnode            *vp,
	vm_map_t                map,
	thread_t                thread,
	struct mach_header      *header,
	off_t                   file_offset,
	off_t                   macho_size,
	int                     depth,
	int64_t                 aslr_offset,
	int64_t                 dyld_aslr_offset,
	load_result_t           *result,
	load_result_t           *binresult,
	struct image_params     *imgp
	)
{
	uint32_t                ncmds;
	struct load_command     *lcp;
	struct dylinker_command *dlp = 0;
	void *                  control;
	load_return_t           ret = LOAD_SUCCESS;
	void *                  addr;
	vm_size_t               alloc_size, cmds_size;
	size_t                  offset;
	size_t                  oldoffset;      /* for overflow check */
	int                     pass;
	proc_t                  p = vfs_context_proc(imgp->ip_vfs_context);
	int                     error;
	int                     resid = 0;
	int                     spawn = (imgp->ip_flags & IMGPF_SPAWN);
	int                     vfexec = (imgp->ip_flags & IMGPF_VFORK_EXEC);
	size_t                  mach_header_sz = sizeof(struct mach_header);
	boolean_t               abi64;
	boolean_t               got_code_signatures = FALSE;
	boolean_t               found_header_segment = FALSE;
	boolean_t               found_xhdr = FALSE;
	boolean_t               found_version_cmd = FALSE;
	int64_t                 slide = 0;
	boolean_t               dyld_no_load_addr = FALSE;
	boolean_t               is_dyld = FALSE;
	vm_map_offset_t         effective_page_mask = PAGE_MASK;
#if __arm64__
	uint64_t                pagezero_end = 0;
	uint64_t                executable_end = 0;
	uint64_t                writable_start = 0;
	vm_map_size_t           effective_page_size;

	effective_page_mask = vm_map_page_mask(map);
	effective_page_size = vm_map_page_size(map);
#endif /* __arm64__ */

	if (header->magic == MH_MAGIC_64 ||
	    header->magic == MH_CIGAM_64) {
		mach_header_sz = sizeof(struct mach_header_64);
	}

	/*
	 *	Break infinite recursion
	 */
	if (depth > 2) {
		return LOAD_FAILURE;
	}

	depth++;

	/*
	 *	Check to see if right machine type.
	 */
	if (((cpu_type_t)(header->cputype & ~CPU_ARCH_MASK) != (cpu_type() & ~CPU_ARCH_MASK))
	    ) {
		return LOAD_BADARCH;
	}

	if (!grade_binary(header->cputype,
	    header->cpusubtype & ~CPU_SUBTYPE_MASK,
	    header->cpusubtype & CPU_SUBTYPE_MASK, TRUE)) {
		return LOAD_BADARCH;
	}

	abi64 = ((header->cputype & CPU_ARCH_ABI64) == CPU_ARCH_ABI64);

	switch (header->filetype) {
	case MH_EXECUTE:
		if (depth != 1 && depth != 3) {
			return LOAD_FAILURE;
		}
		if (header->flags & MH_DYLDLINK) {
			/* Check properties of dynamic executables */
			if (!(header->flags & MH_PIE) && pie_required(header->cputype, header->cpusubtype & ~CPU_SUBTYPE_MASK)) {
				return LOAD_FAILURE;
			}
			result->needs_dynlinker = TRUE;
		} else if (header->cputype == CPU_TYPE_X86_64) {
			/* x86_64 static binaries allowed */
		} else {
			/* Check properties of static executables (disallowed except for development) */
#if !(DEVELOPMENT || DEBUG)
			return LOAD_FAILURE;
#endif
		}
		break;
	case MH_DYLINKER:
		if (depth != 2) {
			return LOAD_FAILURE;
		}
		is_dyld = TRUE;
		break;

	default:
		return LOAD_FAILURE;
	}

	/*
	 *	For PIE and dyld, slide everything by the ASLR offset.
	 */
	if ((header->flags & MH_PIE) || is_dyld) {
		slide = aslr_offset;
	}

	/*
	 *	Get the pager for the file.
	 */
	control = ubc_getobject(vp, UBC_FLAGS_NONE);

	/* ensure header + sizeofcmds falls within the file */
	if (os_add_overflow(mach_header_sz, header->sizeofcmds, &cmds_size) ||
	    (off_t)cmds_size > macho_size ||
	    round_page_overflow(cmds_size, &alloc_size) ||
	    alloc_size > INT_MAX) {
		return LOAD_BADMACHO;
	}

	/*
	 * Map the load commands into kernel memory.
	 */
	addr = kalloc(alloc_size);
	if (addr == NULL) {
		return LOAD_NOSPACE;
	}

	error = vn_rdwr(UIO_READ, vp, addr, (int)alloc_size, file_offset,
	    UIO_SYSSPACE, 0, vfs_context_ucred(imgp->ip_vfs_context), &resid, p);
	if (error) {
		kfree(addr, alloc_size);
		return LOAD_IOERROR;
	}

	if (resid) {
		{
			/* We must be able to read in as much as the mach_header indicated */
			kfree(addr, alloc_size);
			return LOAD_BADMACHO;
		}
	}

	/*
	 *  Scan through the commands, processing each one as necessary.
	 *  We parse in three passes through the headers:
	 *  0: determine if TEXT and DATA boundary can be page-aligned, load platform version
	 *  1: thread state, uuid, code signature
	 *  2: segments
	 *  3: dyld, encryption, check entry point
	 */

	boolean_t slide_realign = FALSE;
#if __arm64__
	if (!abi64) {
		slide_realign = TRUE;
	}
#endif

	for (pass = 0; pass <= 3; pass++) {
		if (pass == 1) {
#if __arm64__
			boolean_t       is_pie;
			int64_t         adjust;

			is_pie = ((header->flags & MH_PIE) != 0);
			if (pagezero_end != 0 &&
			    pagezero_end < effective_page_size) {
				/* need at least 1 page for PAGEZERO */
				adjust = effective_page_size;
				MACHO_PRINTF(("pagezero boundary at "
				    "0x%llx; adjust slide from "
				    "0x%llx to 0x%llx%s\n",
				    (uint64_t) pagezero_end,
				    slide,
				    slide + adjust,
				    (is_pie
				    ? ""
				    : " BUT NO PIE ****** :-(")));
				if (is_pie) {
					slide += adjust;
					pagezero_end += adjust;
					executable_end += adjust;
					writable_start += adjust;
				}
			}
			if (pagezero_end != 0) {
				result->has_pagezero = TRUE;
			}
			if (executable_end == writable_start &&
			    (executable_end & effective_page_mask) != 0 &&
			    (executable_end & FOURK_PAGE_MASK) == 0) {
				/*
				 * The TEXT/DATA boundary is 4K-aligned but
				 * not page-aligned.  Adjust the slide to make
				 * it page-aligned and avoid having a page
				 * with both write and execute permissions.
				 */
				adjust =
				    (effective_page_size -
				    (executable_end & effective_page_mask));
				MACHO_PRINTF(("page-unaligned X-W boundary at "
				    "0x%llx; adjust slide from "
				    "0x%llx to 0x%llx%s\n",
				    (uint64_t) executable_end,
				    slide,
				    slide + adjust,
				    (is_pie
				    ? ""
				    : " BUT NO PIE ****** :-(")));
				if (is_pie) {
					slide += adjust;
				}
			}
#endif /* __arm64__ */

			if (dyld_no_load_addr && binresult) {
				/*
				 * The dyld Mach-O does not specify a load address. Try to locate
				 * it right after the main binary. If binresult == NULL, load
				 * directly to the given slide.
				 */
				mach_vm_address_t max_vm_addr = binresult->max_vm_addr;
				slide = vm_map_round_page(slide + max_vm_addr, effective_page_mask);
			}
		}

		/*
		 * Check that the entry point is contained in an executable segment
		 */
		if ((pass == 3) && (thread != THREAD_NULL)) {
			if (depth == 1 && imgp && (imgp->ip_flags & IMGPF_DRIVER)) {
				/* Driver binaries must have driverkit platform */
				if (result->ip_platform == PLATFORM_DRIVERKIT) {
					/* Driver binaries have no entry point */
					ret = setup_driver_main(thread, slide, result);
				} else {
					ret = LOAD_FAILURE;
				}
			} else if (!result->using_lcmain && result->validentry == 0) {
				ret = LOAD_FAILURE;
			}
			if (ret != KERN_SUCCESS) {
				thread_state_initialize(thread);
				break;
			}
		}

		/*
		 * Check that some segment maps the start of the mach-o file, which is
		 * needed by the dynamic loader to read the mach headers, etc.
		 */
		if ((pass == 3) && (found_header_segment == FALSE)) {
			ret = LOAD_BADMACHO;
			break;
		}

		/*
		 * Loop through each of the load_commands indicated by the
		 * Mach-O header; if an absurd value is provided, we just
		 * run off the end of the reserved section by incrementing
		 * the offset too far, so we are implicitly fail-safe.
		 */
		offset = mach_header_sz;
		ncmds = header->ncmds;

		while (ncmds--) {
			/* ensure enough space for a minimal load command */
			if (offset + sizeof(struct load_command) > cmds_size) {
				ret = LOAD_BADMACHO;
				break;
			}

			/*
			 *	Get a pointer to the command.
			 */
			lcp = (struct load_command *)(addr + offset);
			oldoffset = offset;

			/*
			 * Perform prevalidation of the struct load_command
			 * before we attempt to use its contents.  Invalid
			 * values are ones which result in an overflow, or
			 * which can not possibly be valid commands, or which
			 * straddle or exist past the reserved section at the
			 * start of the image.
			 */
			if (os_add_overflow(offset, lcp->cmdsize, &offset) ||
			    lcp->cmdsize < sizeof(struct load_command) ||
			    offset > cmds_size) {
				ret = LOAD_BADMACHO;
				break;
			}

			/*
			 * Act on struct load_command's for which kernel
			 * intervention is required.
			 * Note that each load command implementation is expected to validate
			 * that lcp->cmdsize is large enough to fit its specific struct type
			 * before dereferencing fields not covered by struct load_command.
			 */
			switch (lcp->cmd) {
			case LC_SEGMENT: {
				struct segment_command *scp = (struct segment_command *) lcp;
				if (scp->cmdsize < sizeof(*scp)) {
					ret = LOAD_BADMACHO;
					break;
				}
				if (pass == 0) {
					if (is_dyld && scp->vmaddr == 0 && scp->fileoff == 0) {
						dyld_no_load_addr = TRUE;
						if (!slide_realign) {
							/* got what we need, bail early on pass 0 */
							continue;
						}
					}

#if __arm64__
					assert(!abi64);

					if (scp->initprot == 0 && scp->maxprot == 0 && scp->vmaddr == 0) {
						/* PAGEZERO */
						if (os_add3_overflow(scp->vmaddr, scp->vmsize, slide, &pagezero_end) || pagezero_end > UINT32_MAX) {
							ret = LOAD_BADMACHO;
							break;
						}
					}
					if (scp->initprot & VM_PROT_EXECUTE) {
						/* TEXT */
						if (os_add3_overflow(scp->vmaddr, scp->vmsize, slide, &executable_end) || executable_end > UINT32_MAX) {
							ret = LOAD_BADMACHO;
							break;
						}
					}
					if (scp->initprot & VM_PROT_WRITE) {
						/* DATA */
						if (os_add_overflow(scp->vmaddr, slide, &writable_start) || writable_start > UINT32_MAX) {
							ret = LOAD_BADMACHO;
							break;
						}
					}
#endif /* __arm64__ */
					break;
				}

				if (pass == 1 && !strncmp(scp->segname, "__XHDR", sizeof(scp->segname))) {
					found_xhdr = TRUE;
				}

				if (pass != 2) {
					break;
				}

				if (abi64) {
					/*
					 * Having an LC_SEGMENT command for the
					 * wrong ABI is invalid <rdar://problem/11021230>
					 */
					ret = LOAD_BADMACHO;
					break;
				}

				ret = load_segment(lcp,
				    header->filetype,
				    control,
				    file_offset,
				    macho_size,
				    vp,
				    map,
				    slide,
				    result,
				    imgp);
				if (ret == LOAD_SUCCESS && scp->fileoff == 0 && scp->filesize > 0) {
					/* Enforce a single segment mapping offset zero, with R+X
					 * protection. */
					if (found_header_segment ||
					    ((scp->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE))) {
						ret = LOAD_BADMACHO;
						break;
					}
					found_header_segment = TRUE;
				}

				break;
			}
			case LC_SEGMENT_64: {
				struct segment_command_64 *scp64 = (struct segment_command_64 *) lcp;
				if (scp64->cmdsize < sizeof(*scp64)) {
					ret = LOAD_BADMACHO;
					break;
				}
				if (pass == 0) {
					if (is_dyld && scp64->vmaddr == 0 && scp64->fileoff == 0) {
						dyld_no_load_addr = TRUE;
					}
					/* got what we need, bail early on pass 0 */
					continue;
				}

				if (pass == 1 && !strncmp(scp64->segname, "__XHDR", sizeof(scp64->segname))) {
					found_xhdr = TRUE;
				}

				if (pass != 2) {
					break;
				}

				if (!abi64) {
					/*
					 * Having an LC_SEGMENT_64 command for the
					 * wrong ABI is invalid <rdar://problem/11021230>
					 */
					ret = LOAD_BADMACHO;
					break;
				}

				ret = load_segment(lcp,
				    header->filetype,
				    control,
				    file_offset,
				    macho_size,
				    vp,
				    map,
				    slide,
				    result,
				    imgp);

				if (ret == LOAD_SUCCESS && scp64->fileoff == 0 && scp64->filesize > 0) {
					/* Enforce a single segment mapping offset zero, with R+X
					 * protection. */
					if (found_header_segment ||
					    ((scp64->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE))) {
						ret = LOAD_BADMACHO;
						break;
					}
					found_header_segment = TRUE;
				}

				break;
			}
			case LC_UNIXTHREAD: {
				boolean_t is_x86_64_compat_binary = FALSE;
				if (pass != 1) {
					break;
				}
				ret = load_unixthread(
					(struct thread_command *) lcp,
					thread,
					slide,
					is_x86_64_compat_binary,
					result);
				break;
			}
			case LC_MAIN:
				if (pass != 1) {
					break;
				}
				if (depth != 1) {
					break;
				}
				ret = load_main(
					(struct entry_point_command *) lcp,
					thread,
					slide,
					result);
				break;
			case LC_LOAD_DYLINKER:
				if (pass != 3) {
					break;
				}
				if ((depth == 1) && (dlp == 0)) {
					dlp = (struct dylinker_command *)lcp;
				} else {
					ret = LOAD_FAILURE;
				}
				break;
			case LC_UUID:
				if (pass == 1 && depth == 1) {
					ret = load_uuid((struct uuid_command *) lcp,
					    (char *)addr + cmds_size,
					    result);
				}
				break;
			case LC_CODE_SIGNATURE:
				/* CODE SIGNING */
				if (pass != 1) {
					break;
				}

				/* pager -> uip ->
				 *  load signatures & store in uip
				 *  set VM object "signed_pages"
				 */
				ret = load_code_signature(
					(struct linkedit_data_command *) lcp,
					vp,
					file_offset,
					macho_size,
					header->cputype,
					header->cpusubtype,
					result,
					imgp);
				if (ret != LOAD_SUCCESS) {
					printf("proc %d: load code signature error %d "
					    "for file \"%s\"\n",
					    p->p_pid, ret, vp->v_name);
					/*
					 * Allow injections to be ignored on devices w/o enforcement enabled
					 */
					if (!cs_process_global_enforcement()) {
						ret = LOAD_SUCCESS; /* ignore error */
					}
				} else {
					got_code_signatures = TRUE;
				}

				if (got_code_signatures) {
					unsigned tainted = CS_VALIDATE_TAINTED;
					boolean_t valid = FALSE;
					vm_size_t off = 0;


					if (cs_debug > 10) {
						printf("validating initial pages of %s\n", vp->v_name);
					}

					while (off < alloc_size && ret == LOAD_SUCCESS) {
						tainted = CS_VALIDATE_TAINTED;

						valid = cs_validate_range(vp,
						    NULL,
						    file_offset + off,
						    addr + off,
						    MIN(PAGE_SIZE, cmds_size),
						    &tainted);
						if (!valid || (tainted & CS_VALIDATE_TAINTED)) {
							if (cs_debug) {
								printf("CODE SIGNING: %s[%d]: invalid initial page at offset %lld validated:%d tainted:%d csflags:0x%x\n",
								    vp->v_name, p->p_pid, (long long)(file_offset + off), valid, tainted, result->csflags);
							}
							if (cs_process_global_enforcement() ||
							    (result->csflags & (CS_HARD | CS_KILL | CS_ENFORCEMENT))) {
								ret = LOAD_FAILURE;
							}
							result->csflags &= ~CS_VALID;
						}
						off += PAGE_SIZE;
					}
				}

				break;
#if CONFIG_CODE_DECRYPTION
			case LC_ENCRYPTION_INFO:
			case LC_ENCRYPTION_INFO_64:
				if (pass != 3) {
					break;
				}
				ret = set_code_unprotect(
					(struct encryption_info_command *) lcp,
					addr, map, slide, vp, file_offset,
					header->cputype, header->cpusubtype);
				if (ret != LOAD_SUCCESS) {
					os_reason_t load_failure_reason = OS_REASON_NULL;
					printf("proc %d: set_code_unprotect() error %d "
					    "for file \"%s\"\n",
					    p->p_pid, ret, vp->v_name);
					/*
					 * Don't let the app run if it's
					 * encrypted but we failed to set up the
					 * decrypter. If the keys are missing it will
					 * return LOAD_DECRYPTFAIL.
					 */
					if (ret == LOAD_DECRYPTFAIL) {
						/* failed to load due to missing FP keys */
						proc_lock(p);
						p->p_lflag |= P_LTERM_DECRYPTFAIL;
						proc_unlock(p);

						KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_PROC, BSD_PROC_EXITREASON_CREATE) | DBG_FUNC_NONE,
						    p->p_pid, OS_REASON_EXEC, EXEC_EXIT_REASON_FAIRPLAY_DECRYPT, 0, 0);
						load_failure_reason = os_reason_create(OS_REASON_EXEC, EXEC_EXIT_REASON_FAIRPLAY_DECRYPT);
					} else {
						KERNEL_DEBUG_CONSTANT(BSDDBG_CODE(DBG_BSD_PROC, BSD_PROC_EXITREASON_CREATE) | DBG_FUNC_NONE,
						    p->p_pid, OS_REASON_EXEC, EXEC_EXIT_REASON_DECRYPT, 0, 0);
						load_failure_reason = os_reason_create(OS_REASON_EXEC, EXEC_EXIT_REASON_DECRYPT);
					}

					/*
					 * Don't signal the process if it was forked and in a partially constructed
					 * state as part of a spawn -- it will just be torn down when the exec fails.
					 */
					if (!spawn) {
						assert(load_failure_reason != OS_REASON_NULL);
						if (vfexec) {
							psignal_vfork_with_reason(p, get_threadtask(imgp->ip_new_thread), imgp->ip_new_thread, SIGKILL, load_failure_reason);
							load_failure_reason = OS_REASON_NULL;
						} else {
							psignal_with_reason(p, SIGKILL, load_failure_reason);
							load_failure_reason = OS_REASON_NULL;
						}
					} else {
						os_reason_free(load_failure_reason);
						load_failure_reason = OS_REASON_NULL;
					}
				}
				break;
#endif
			case LC_VERSION_MIN_IPHONEOS:
			case LC_VERSION_MIN_MACOSX:
			case LC_VERSION_MIN_WATCHOS:
			case LC_VERSION_MIN_TVOS: {
				struct version_min_command *vmc;

				if (depth != 1 || pass != 0) {
					break;
				}
				vmc = (struct version_min_command *) lcp;
				ret = load_version(vmc, &found_version_cmd, imgp->ip_flags, result);
#if XNU_TARGET_OS_OSX
				if (ret == LOAD_SUCCESS) {
					if (result->ip_platform == PLATFORM_IOS) {
						vm_map_mark_alien(map);
					} else {
						assert(!vm_map_is_alien(map));
					}
				}
#endif /* XNU_TARGET_OS_OSX */
				break;
			}
			case LC_BUILD_VERSION: {
				if (depth != 1 || pass != 0) {
					break;
				}
				struct build_version_command* bvc = (struct build_version_command*)lcp;
				if (bvc->cmdsize < sizeof(*bvc)) {
					ret = LOAD_BADMACHO;
					break;
				}
				if (found_version_cmd == TRUE) {
					ret = LOAD_BADMACHO;
					break;
				}
				result->ip_platform = bvc->platform;
				result->lr_sdk = bvc->sdk;
				result->lr_min_sdk = bvc->minos;
				found_version_cmd = TRUE;
#if XNU_TARGET_OS_OSX
				if (result->ip_platform == PLATFORM_IOS) {
					vm_map_mark_alien(map);
				} else {
					assert(!vm_map_is_alien(map));
				}
#endif /* XNU_TARGET_OS_OSX */
				break;
			}
			default:
				/* Other commands are ignored by the kernel */
				ret = LOAD_SUCCESS;
				break;
			}
			if (ret != LOAD_SUCCESS) {
				break;
			}
		}
		if (ret != LOAD_SUCCESS) {
			break;
		}
	}

	if (ret == LOAD_SUCCESS) {
		if (!got_code_signatures && cs_process_global_enforcement()) {
			ret = LOAD_FAILURE;
		}

		/* Make sure if we need dyld, we got it */
		if (result->needs_dynlinker && !dlp) {
			ret = LOAD_FAILURE;
		}

		if ((ret == LOAD_SUCCESS) && (dlp != 0)) {
			/*
			 * load the dylinker, and slide it by the independent DYLD ASLR
			 * offset regardless of the PIE-ness of the main binary.
			 */
			ret = load_dylinker(dlp, header->cputype, map, thread, depth,
			    dyld_aslr_offset, result, imgp);
		}


		if ((ret == LOAD_SUCCESS) && (depth == 1)) {
			if (result->thread_count == 0) {
				ret = LOAD_FAILURE;
			}
#if CONFIG_ENFORCE_SIGNED_CODE
			if (result->needs_dynlinker && !(result->csflags & CS_DYLD_PLATFORM)) {
				ret = LOAD_FAILURE;
			}
#endif
		}
	}

	if (ret == LOAD_BADMACHO && found_xhdr) {
		ret = LOAD_BADMACHO_UPX;
	}

	kfree(addr, alloc_size);

	return ret;
}

load_return_t
validate_potential_simulator_binary(
	cpu_type_t               exectype __unused,
	struct image_params      *imgp __unused,
	off_t                    file_offset __unused,
	off_t                    macho_size __unused)
{
#if __x86_64__
	/* Allow 32 bit exec only for simulator binaries */
	if (bootarg_no32exec && imgp != NULL && exectype == CPU_TYPE_X86) {
		if (imgp->ip_simulator_binary == IMGPF_SB_DEFAULT) {
			boolean_t simulator_binary = check_if_simulator_binary(imgp, file_offset, macho_size);
			imgp->ip_simulator_binary = simulator_binary ? IMGPF_SB_TRUE : IMGPF_SB_FALSE;
		}

		if (imgp->ip_simulator_binary != IMGPF_SB_TRUE) {
			return LOAD_BADARCH;
		}
	}
#endif
	return LOAD_SUCCESS;
}

#if __x86_64__
static boolean_t
check_if_simulator_binary(
	struct image_params     *imgp,
	off_t                   file_offset,
	off_t                   macho_size)
{
	struct mach_header      *header;
	char                    *ip_vdata = NULL;
	kauth_cred_t            cred = NULL;
	uint32_t                ncmds;
	struct load_command     *lcp;
	boolean_t               simulator_binary = FALSE;
	void *                  addr = NULL;
	vm_size_t               alloc_size, cmds_size;
	size_t                  offset;
	proc_t                  p = current_proc();             /* XXXX */
	int                     error;
	int                     resid = 0;
	size_t                  mach_header_sz = sizeof(struct mach_header);


	cred =  kauth_cred_proc_ref(p);

	/* Allocate page to copyin mach header */
	ip_vdata = kalloc(PAGE_SIZE);
	bzero(ip_vdata, PAGE_SIZE);
	if (ip_vdata == NULL) {
		goto bad;
	}

	/* Read the Mach-O header */
	error = vn_rdwr(UIO_READ, imgp->ip_vp, ip_vdata,
	    PAGE_SIZE, file_offset,
	    UIO_SYSSPACE, (IO_UNIT | IO_NODELOCKED),
	    cred, &resid, p);
	if (error) {
		goto bad;
	}

	header = (struct mach_header *)ip_vdata;

	if (header->magic == MH_MAGIC_64 ||
	    header->magic == MH_CIGAM_64) {
		mach_header_sz = sizeof(struct mach_header_64);
	}

	/* ensure header + sizeofcmds falls within the file */
	if (os_add_overflow(mach_header_sz, header->sizeofcmds, &cmds_size) ||
	    (off_t)cmds_size > macho_size ||
	    round_page_overflow(cmds_size, &alloc_size) ||
	    alloc_size > INT_MAX) {
		goto bad;
	}

	/*
	 * Map the load commands into kernel memory.
	 */
	addr = kalloc(alloc_size);
	if (addr == NULL) {
		goto bad;
	}

	error = vn_rdwr(UIO_READ, imgp->ip_vp, addr, (int)alloc_size, file_offset,
	    UIO_SYSSPACE, IO_NODELOCKED, cred, &resid, p);
	if (error) {
		goto bad;
	}

	if (resid) {
		/* We must be able to read in as much as the mach_header indicated */
		goto bad;
	}

	/*
	 * Loop through each of the load_commands indicated by the
	 * Mach-O header; if an absurd value is provided, we just
	 * run off the end of the reserved section by incrementing
	 * the offset too far, so we are implicitly fail-safe.
	 */
	offset = mach_header_sz;
	ncmds = header->ncmds;

	while (ncmds--) {
		/* ensure enough space for a minimal load command */
		if (offset + sizeof(struct load_command) > cmds_size) {
			break;
		}

		/*
		 *	Get a pointer to the command.
		 */
		lcp = (struct load_command *)(addr + offset);

		/*
		 * Perform prevalidation of the struct load_command
		 * before we attempt to use its contents.  Invalid
		 * values are ones which result in an overflow, or
		 * which can not possibly be valid commands, or which
		 * straddle or exist past the reserved section at the
		 * start of the image.
		 */
		if (os_add_overflow(offset, lcp->cmdsize, &offset) ||
		    lcp->cmdsize < sizeof(struct load_command) ||
		    offset > cmds_size) {
			break;
		}

		/* Check if its a simulator binary. */
		switch (lcp->cmd) {
		case LC_VERSION_MIN_WATCHOS:
			simulator_binary = TRUE;
			break;

		case LC_BUILD_VERSION: {
			struct build_version_command *bvc;

			bvc = (struct build_version_command *) lcp;
			if (bvc->cmdsize < sizeof(*bvc)) {
				/* unsafe to use this command struct if cmdsize
				* validated above is too small for it to fit */
				break;
			}
			if (bvc->platform == PLATFORM_IOSSIMULATOR ||
			    bvc->platform == PLATFORM_WATCHOSSIMULATOR) {
				simulator_binary = TRUE;
			}

			break;
		}

		case LC_VERSION_MIN_IPHONEOS: {
			simulator_binary = TRUE;
			break;
		}

		default:
			/* ignore other load commands */
			break;
		}

		if (simulator_binary == TRUE) {
			break;
		}
	}

bad:
	if (ip_vdata) {
		kfree(ip_vdata, PAGE_SIZE);
	}

	if (cred) {
		kauth_cred_unref(&cred);
	}

	if (addr) {
		kfree(addr, alloc_size);
	}

	return simulator_binary;
}
#endif /* __x86_64__ */

#if CONFIG_CODE_DECRYPTION

#define APPLE_UNPROTECTED_HEADER_SIZE   (3 * 4096)

static load_return_t
unprotect_dsmos_segment(
	uint64_t        file_off,
	uint64_t        file_size,
	struct vnode    *vp,
	off_t           macho_offset,
	vm_map_t        map,
	vm_map_offset_t map_addr,
	vm_map_size_t   map_size)
{
	kern_return_t   kr;
	uint64_t        slice_off;

	/*
	 * The first APPLE_UNPROTECTED_HEADER_SIZE bytes (from offset 0 of
	 * this part of a Universal binary) are not protected...
	 * The rest needs to be "transformed".
	 */
	slice_off = file_off - macho_offset;
	if (slice_off <= APPLE_UNPROTECTED_HEADER_SIZE &&
	    slice_off + file_size <= APPLE_UNPROTECTED_HEADER_SIZE) {
		/* it's all unprotected, nothing to do... */
		kr = KERN_SUCCESS;
	} else {
		if (slice_off <= APPLE_UNPROTECTED_HEADER_SIZE) {
			/*
			 * We start mapping in the unprotected area.
			 * Skip the unprotected part...
			 */
			uint64_t delta_file;
			vm_map_offset_t delta_map;

			delta_file = (uint64_t)APPLE_UNPROTECTED_HEADER_SIZE;
			delta_file -= slice_off;
			if (os_convert_overflow(delta_file, &delta_map)) {
				return LOAD_BADMACHO;
			}
			if (os_add_overflow(map_addr, delta_map, &map_addr)) {
				return LOAD_BADMACHO;
			}
			if (os_sub_overflow(map_size, delta_map, &map_size)) {
				return LOAD_BADMACHO;
			}
		}
		/* ... transform the rest of the mapping. */
		struct pager_crypt_info crypt_info;
		crypt_info.page_decrypt = dsmos_page_transform;
		crypt_info.crypt_ops = NULL;
		crypt_info.crypt_end = NULL;
#pragma unused(vp, macho_offset)
		crypt_info.crypt_ops = (void *)0x2e69cf40;
		vm_map_offset_t crypto_backing_offset;
		crypto_backing_offset = -1; /* i.e. use map entry's offset */
#if VM_MAP_DEBUG_APPLE_PROTECT
		if (vm_map_debug_apple_protect) {
			struct proc *p;
			p = current_proc();
			printf("APPLE_PROTECT: %d[%s] map %p "
			    "[0x%llx:0x%llx] %s(%s)\n",
			    p->p_pid, p->p_comm, map,
			    (uint64_t) map_addr,
			    (uint64_t) (map_addr + map_size),
			    __FUNCTION__, vp->v_name);
		}
#endif /* VM_MAP_DEBUG_APPLE_PROTECT */

		/* The DSMOS pager can only be used by apple signed code */
		struct cs_blob * blob = csvnode_get_blob(vp, file_off);
		if (blob == NULL || !blob->csb_platform_binary || blob->csb_platform_path) {
			return LOAD_FAILURE;
		}

		kr = vm_map_apple_protected(map,
		    map_addr,
		    map_addr + map_size,
		    crypto_backing_offset,
		    &crypt_info,
		    CRYPTID_APP_ENCRYPTION);
	}

	if (kr != KERN_SUCCESS) {
		return LOAD_FAILURE;
	}
	return LOAD_SUCCESS;
}
#else   /* CONFIG_CODE_DECRYPTION */
static load_return_t
unprotect_dsmos_segment(
	__unused        uint64_t        file_off,
	__unused        uint64_t        file_size,
	__unused        struct vnode    *vp,
	__unused        off_t           macho_offset,
	__unused        vm_map_t        map,
	__unused        vm_map_offset_t map_addr,
	__unused        vm_map_size_t   map_size)
{
	return LOAD_SUCCESS;
}
#endif  /* CONFIG_CODE_DECRYPTION */


/*
 * map_segment:
 *	Maps a Mach-O segment, taking care of mis-alignment (wrt the system
 *	page size) issues.
 *
 *	The mapping might result in 1, 2 or 3 map entries:
 *      1. for the first page, which could be overlap with the previous
 *         mapping,
 *      2. for the center (if applicable),
 *      3. for the last page, which could overlap with the next mapping.
 *
 *	For each of those map entries, we might have to interpose a
 *	"fourk_pager" to deal with mis-alignment wrt the system page size,
 *	either in the mapping address and/or size or the file offset and/or
 *	size.
 *	The "fourk_pager" itself would be mapped with proper alignment
 *	wrt the system page size and would then be populated with the
 *	information about the intended mapping, with a "4KB" granularity.
 */
static kern_return_t
map_segment(
	vm_map_t                map,
	vm_map_offset_t         vm_start,
	vm_map_offset_t         vm_end,
	memory_object_control_t control,
	vm_map_offset_t         file_start,
	vm_map_offset_t         file_end,
	vm_prot_t               initprot,
	vm_prot_t               maxprot,
	load_result_t           *result)
{
	vm_map_offset_t cur_offset, cur_start, cur_end;
	kern_return_t   ret;
	vm_map_offset_t effective_page_mask;
	vm_map_kernel_flags_t vmk_flags, cur_vmk_flags;

	if (vm_end < vm_start ||
	    file_end < file_start) {
		return LOAD_BADMACHO;
	}
	if (vm_end == vm_start ||
	    file_end == file_start) {
		/* nothing to map... */
		return LOAD_SUCCESS;
	}

	effective_page_mask = vm_map_page_mask(map);

	vmk_flags = VM_MAP_KERNEL_FLAGS_NONE;
	if (vm_map_page_aligned(vm_start, effective_page_mask) &&
	    vm_map_page_aligned(vm_end, effective_page_mask) &&
	    vm_map_page_aligned(file_start, effective_page_mask) &&
	    vm_map_page_aligned(file_end, effective_page_mask)) {
		/* all page-aligned and map-aligned: proceed */
	} else {
#if __arm64__
		/* use an intermediate "4K" pager */
		vmk_flags.vmkf_fourk = TRUE;
#else /* __arm64__ */
		panic("map_segment: unexpected mis-alignment "
		    "vm[0x%llx:0x%llx] file[0x%llx:0x%llx]\n",
		    (uint64_t) vm_start,
		    (uint64_t) vm_end,
		    (uint64_t) file_start,
		    (uint64_t) file_end);
#endif /* __arm64__ */
	}

	cur_offset = 0;
	cur_start = vm_start;
	cur_end = vm_start;
#if __arm64__
	if (!vm_map_page_aligned(vm_start, effective_page_mask)) {
		/* one 4K pager for the 1st page */
		cur_end = vm_map_round_page(cur_start, effective_page_mask);
		if (cur_end > vm_end) {
			cur_end = vm_start + (file_end - file_start);
		}
		if (control != MEMORY_OBJECT_CONTROL_NULL) {
			/* no copy-on-read for mapped binaries */
			vmk_flags.vmkf_no_copy_on_read = 1;
			ret = vm_map_enter_mem_object_control(
				map,
				&cur_start,
				cur_end - cur_start,
				(mach_vm_offset_t)0,
				VM_FLAGS_FIXED,
				vmk_flags,
				VM_KERN_MEMORY_NONE,
				control,
				file_start + cur_offset,
				TRUE, /* copy */
				initprot, maxprot,
				VM_INHERIT_DEFAULT);
		} else {
			ret = vm_map_enter_mem_object(
				map,
				&cur_start,
				cur_end - cur_start,
				(mach_vm_offset_t)0,
				VM_FLAGS_FIXED,
				vmk_flags,
				VM_KERN_MEMORY_NONE,
				IPC_PORT_NULL,
				0, /* offset */
				TRUE, /* copy */
				initprot, maxprot,
				VM_INHERIT_DEFAULT);
		}
		if (ret != KERN_SUCCESS) {
			return LOAD_NOSPACE;
		}
		cur_offset += cur_end - cur_start;
	}
#endif /* __arm64__ */
	if (cur_end >= vm_start + (file_end - file_start)) {
		/* all mapped: done */
		goto done;
	}
	if (vm_map_round_page(cur_end, effective_page_mask) >=
	    vm_map_trunc_page(vm_start + (file_end - file_start),
	    effective_page_mask)) {
		/* no middle */
	} else {
		cur_start = cur_end;
		if ((vm_start & effective_page_mask) !=
		    (file_start & effective_page_mask)) {
			/* one 4K pager for the middle */
			cur_vmk_flags = vmk_flags;
		} else {
			/* regular mapping for the middle */
			cur_vmk_flags = VM_MAP_KERNEL_FLAGS_NONE;
		}

#if !defined(XNU_TARGET_OS_OSX)
		(void) result;
#else /* !defined(XNU_TARGET_OS_OSX) */
		/*
		 * This process doesn't have its new csflags (from
		 * the image being loaded) yet, so tell VM to override the
		 * current process's CS_ENFORCEMENT for this mapping.
		 */
		if (result->csflags & CS_ENFORCEMENT) {
			cur_vmk_flags.vmkf_cs_enforcement = TRUE;
		} else {
			cur_vmk_flags.vmkf_cs_enforcement = FALSE;
		}
		cur_vmk_flags.vmkf_cs_enforcement_override = TRUE;
#endif /* !defined(XNU_TARGET_OS_OSX) */

		if (result->is_cambria && (initprot & VM_PROT_EXECUTE) == VM_PROT_EXECUTE) {
			cur_vmk_flags.vmkf_translated_allow_execute = TRUE;
		}

		cur_end = vm_map_trunc_page(vm_start + (file_end -
		    file_start),
		    effective_page_mask);
		if (control != MEMORY_OBJECT_CONTROL_NULL) {
			/* no copy-on-read for mapped binaries */
			cur_vmk_flags.vmkf_no_copy_on_read = 1;
			ret = vm_map_enter_mem_object_control(
				map,
				&cur_start,
				cur_end - cur_start,
				(mach_vm_offset_t)0,
				VM_FLAGS_FIXED,
				cur_vmk_flags,
				VM_KERN_MEMORY_NONE,
				control,
				file_start + cur_offset,
				TRUE, /* copy */
				initprot, maxprot,
				VM_INHERIT_DEFAULT);
		} else {
			ret = vm_map_enter_mem_object(
				map,
				&cur_start,
				cur_end - cur_start,
				(mach_vm_offset_t)0,
				VM_FLAGS_FIXED,
				cur_vmk_flags,
				VM_KERN_MEMORY_NONE,
				IPC_PORT_NULL,
				0, /* offset */
				TRUE, /* copy */
				initprot, maxprot,
				VM_INHERIT_DEFAULT);
		}
		if (ret != KERN_SUCCESS) {
			return LOAD_NOSPACE;
		}
		cur_offset += cur_end - cur_start;
	}
	if (cur_end >= vm_start + (file_end - file_start)) {
		/* all mapped: done */
		goto done;
	}
	cur_start = cur_end;
#if __arm64__
	if (!vm_map_page_aligned(vm_start + (file_end - file_start),
	    effective_page_mask)) {
		/* one 4K pager for the last page */
		cur_end = vm_start + (file_end - file_start);
		if (control != MEMORY_OBJECT_CONTROL_NULL) {
			/* no copy-on-read for mapped binaries */
			vmk_flags.vmkf_no_copy_on_read = 1;
			ret = vm_map_enter_mem_object_control(
				map,
				&cur_start,
				cur_end - cur_start,
				(mach_vm_offset_t)0,
				VM_FLAGS_FIXED,
				vmk_flags,
				VM_KERN_MEMORY_NONE,
				control,
				file_start + cur_offset,
				TRUE, /* copy */
				initprot, maxprot,
				VM_INHERIT_DEFAULT);
		} else {
			ret = vm_map_enter_mem_object(
				map,
				&cur_start,
				cur_end - cur_start,
				(mach_vm_offset_t)0,
				VM_FLAGS_FIXED,
				vmk_flags,
				VM_KERN_MEMORY_NONE,
				IPC_PORT_NULL,
				0, /* offset */
				TRUE, /* copy */
				initprot, maxprot,
				VM_INHERIT_DEFAULT);
		}
		if (ret != KERN_SUCCESS) {
			return LOAD_NOSPACE;
		}
		cur_offset += cur_end - cur_start;
	}
#endif /* __arm64__ */
done:
	assert(cur_end >= vm_start + (file_end - file_start));
	return LOAD_SUCCESS;
}

static
load_return_t
load_segment(
	struct load_command     *lcp,
	uint32_t                filetype,
	void *                  control,
	off_t                   pager_offset,
	off_t                   macho_size,
	struct vnode            *vp,
	vm_map_t                map,
	int64_t                 slide,
	load_result_t           *result,
	struct image_params     *imgp)
{
	struct segment_command_64 segment_command, *scp;
	kern_return_t           ret;
	vm_map_size_t           delta_size;
	vm_prot_t               initprot;
	vm_prot_t               maxprot;
	size_t                  segment_command_size, total_section_size,
	    single_section_size;
	uint64_t                file_offset, file_size;
	vm_map_offset_t         vm_offset;
	size_t                  vm_size;
	vm_map_offset_t         vm_start, vm_end, vm_end_aligned;
	vm_map_offset_t         file_start, file_end;
	kern_return_t           kr;
	boolean_t               verbose;
	vm_map_size_t           effective_page_size;
	vm_map_offset_t         effective_page_mask;
#if __arm64__
	vm_map_kernel_flags_t   vmk_flags;
	boolean_t               fourk_align;
#endif /* __arm64__ */

	(void)imgp;

	effective_page_size = vm_map_page_size(map);
	effective_page_mask = vm_map_page_mask(map);

	verbose = FALSE;
	if (LC_SEGMENT_64 == lcp->cmd) {
		segment_command_size = sizeof(struct segment_command_64);
		single_section_size  = sizeof(struct section_64);
#if __arm64__
		/* 64-bit binary: should already be 16K-aligned */
		fourk_align = FALSE;

		if (vm_map_page_shift(map) == FOURK_PAGE_SHIFT &&
		    PAGE_SHIFT != FOURK_PAGE_SHIFT) {
			fourk_align = TRUE;
			verbose = TRUE;
		}
#endif /* __arm64__ */
	} else {
		segment_command_size = sizeof(struct segment_command);
		single_section_size  = sizeof(struct section);
#if __arm64__
		/* 32-bit binary: might need 4K-alignment */
		if (effective_page_size != FOURK_PAGE_SIZE) {
			/* not using 4K page size: need fourk_pager */
			fourk_align = TRUE;
			verbose = TRUE;
		} else {
			/* using 4K page size: no need for re-alignment */
			fourk_align = FALSE;
		}
#endif /* __arm64__ */
	}
	if (lcp->cmdsize < segment_command_size) {
		DEBUG4K_ERROR("LOAD_BADMACHO cmdsize %d < %zu\n", lcp->cmdsize, segment_command_size);
		return LOAD_BADMACHO;
	}
	total_section_size = lcp->cmdsize - segment_command_size;

	if (LC_SEGMENT_64 == lcp->cmd) {
		scp = (struct segment_command_64 *)lcp;
	} else {
		scp = &segment_command;
		widen_segment_command((struct segment_command *)lcp, scp);
	}

	if (verbose) {
		MACHO_PRINTF(("+++ load_segment %s "
		    "vm[0x%llx:0x%llx] file[0x%llx:0x%llx] "
		    "prot %d/%d flags 0x%x\n",
		    scp->segname,
		    (uint64_t)(slide + scp->vmaddr),
		    (uint64_t)(slide + scp->vmaddr + scp->vmsize),
		    pager_offset + scp->fileoff,
		    pager_offset + scp->fileoff + scp->filesize,
		    scp->initprot,
		    scp->maxprot,
		    scp->flags));
	}

	/*
	 * Make sure what we get from the file is really ours (as specified
	 * by macho_size).
	 */
	if (scp->fileoff + scp->filesize < scp->fileoff ||
	    scp->fileoff + scp->filesize > (uint64_t)macho_size) {
		DEBUG4K_ERROR("LOAD_BADMACHO fileoff 0x%llx filesize 0x%llx macho_size 0x%llx\n", scp->fileoff, scp->filesize, (uint64_t)macho_size);
		return LOAD_BADMACHO;
	}
	/*
	 * Ensure that the number of sections specified would fit
	 * within the load command size.
	 */
	if (total_section_size / single_section_size < scp->nsects) {
		DEBUG4K_ERROR("LOAD_BADMACHO 0x%zx 0x%zx %d\n", total_section_size, single_section_size, scp->nsects);
		return LOAD_BADMACHO;
	}
	/*
	 * Make sure the segment is page-aligned in the file.
	 */
	if (os_add_overflow(pager_offset, scp->fileoff, &file_offset)) {
		DEBUG4K_ERROR("LOAD_BADMACHO file_offset: 0x%llx + 0x%llx\n", pager_offset, scp->fileoff);
		return LOAD_BADMACHO;
	}
	file_size = scp->filesize;
#if __arm64__
	if (fourk_align) {
		if ((file_offset & FOURK_PAGE_MASK) != 0) {
			/*
			 * we can't mmap() it if it's not at least 4KB-aligned
			 * in the file
			 */
			DEBUG4K_ERROR("LOAD_BADMACHO file_offset 0x%llx\n", file_offset);
			return LOAD_BADMACHO;
		}
	} else
#endif /* __arm64__ */
	if ((file_offset & PAGE_MASK_64) != 0 ||
	    /* we can't mmap() it if it's not page-aligned in the file */
	    (file_offset & vm_map_page_mask(map)) != 0) {
		/*
		 * The 1st test would have failed if the system's page size
		 * was what this process believe is the page size, so let's
		 * fail here too for the sake of consistency.
		 */
		DEBUG4K_ERROR("LOAD_BADMACHO file_offset 0x%llx\n", file_offset);
		return LOAD_BADMACHO;
	}

	/*
	 * If we have a code signature attached for this slice
	 * require that the segments are within the signed part
	 * of the file.
	 */
	if (result->cs_end_offset &&
	    result->cs_end_offset < (off_t)scp->fileoff &&
	    result->cs_end_offset - scp->fileoff < scp->filesize) {
		if (cs_debug) {
			printf("section outside code signature\n");
		}
		DEBUG4K_ERROR("LOAD_BADMACHO end_offset 0x%llx fileoff 0x%llx filesize 0x%llx\n", result->cs_end_offset, scp->fileoff, scp->filesize);
		return LOAD_BADMACHO;
	}

	if (os_add_overflow(scp->vmaddr, slide, &vm_offset)) {
		if (cs_debug) {
			printf("vmaddr too large\n");
		}
		DEBUG4K_ERROR("LOAD_BADMACHO vmaddr 0x%llx slide 0x%llx vm_offset 0x%llx\n", scp->vmaddr, slide, (uint64_t)vm_offset);
		return LOAD_BADMACHO;
	}

	if (scp->vmsize > SIZE_MAX) {
		DEBUG4K_ERROR("LOAD_BADMACHO vmsize 0x%llx\n", scp->vmsize);
		return LOAD_BADMACHO;
	}

	vm_size = (size_t)scp->vmsize;

	if (vm_size == 0) {
		return LOAD_SUCCESS;
	}
	if (scp->vmaddr == 0 &&
	    file_size == 0 &&
	    vm_size != 0 &&
	    (scp->initprot & VM_PROT_ALL) == VM_PROT_NONE &&
	    (scp->maxprot & VM_PROT_ALL) == VM_PROT_NONE) {
		if (map == VM_MAP_NULL) {
			return LOAD_SUCCESS;
		}

		/*
		 * For PIE, extend page zero rather than moving it.  Extending
		 * page zero keeps early allocations from falling predictably
		 * between the end of page zero and the beginning of the first
		 * slid segment.
		 */
		/*
		 * This is a "page zero" segment:  it starts at address 0,
		 * is not mapped from the binary file and is not accessible.
		 * User-space should never be able to access that memory, so
		 * make it completely off limits by raising the VM map's
		 * minimum offset.
		 */
		vm_end = (vm_map_offset_t)(vm_offset + vm_size);
		if (vm_end < vm_offset) {
			DEBUG4K_ERROR("LOAD_BADMACHO vm_end 0x%llx vm_offset 0x%llx vm_size 0x%llx\n", (uint64_t)vm_end, (uint64_t)vm_offset, (uint64_t)vm_size);
			return LOAD_BADMACHO;
		}

		if (verbose) {
			MACHO_PRINTF(("++++++ load_segment: "
			    "page_zero up to 0x%llx\n",
			    (uint64_t) vm_end));
		}
#if __arm64__
		if (fourk_align) {
			/* raise min_offset as much as page-alignment allows */
			vm_end_aligned = vm_map_trunc_page(vm_end,
			    effective_page_mask);
		} else
#endif /* __arm64__ */
		{
			vm_end = vm_map_round_page(vm_end,
			    PAGE_MASK_64);
			vm_end_aligned = vm_end;
		}
		ret = vm_map_raise_min_offset(map,
		    vm_end_aligned);
#if __arm64__
		if (ret == 0 &&
		    vm_end > vm_end_aligned) {
			/* use fourk_pager to map the rest of pagezero */
			assert(fourk_align);
			vmk_flags = VM_MAP_KERNEL_FLAGS_NONE;
			vmk_flags.vmkf_fourk = TRUE;
			ret = vm_map_enter_mem_object(
				map,
				&vm_end_aligned,
				vm_end - vm_end_aligned,
				(mach_vm_offset_t) 0,   /* mask */
				VM_FLAGS_FIXED,
				vmk_flags,
				VM_KERN_MEMORY_NONE,
				IPC_PORT_NULL,
				0,
				FALSE,  /* copy */
				(scp->initprot & VM_PROT_ALL),
				(scp->maxprot & VM_PROT_ALL),
				VM_INHERIT_DEFAULT);
		}
#endif /* __arm64__ */

		if (ret != KERN_SUCCESS) {
			DEBUG4K_ERROR("LOAD_FAILURE ret 0x%x\n", ret);
			return LOAD_FAILURE;
		}
		return LOAD_SUCCESS;
	} else {
#if !defined(XNU_TARGET_OS_OSX)
		/* not PAGEZERO: should not be mapped at address 0 */
		if (filetype != MH_DYLINKER && scp->vmaddr == 0) {
			DEBUG4K_ERROR("LOAD_BADMACHO filetype %d vmaddr 0x%llx\n", filetype, scp->vmaddr);
			return LOAD_BADMACHO;
		}
#endif /* !defined(XNU_TARGET_OS_OSX) */
	}

#if __arm64__
	if (fourk_align) {
		/* 4K-align */
		file_start = vm_map_trunc_page(file_offset,
		    FOURK_PAGE_MASK);
		file_end = vm_map_round_page(file_offset + file_size,
		    FOURK_PAGE_MASK);
		vm_start = vm_map_trunc_page(vm_offset,
		    FOURK_PAGE_MASK);
		vm_end = vm_map_round_page(vm_offset + vm_size,
		    FOURK_PAGE_MASK);

		if (file_offset - file_start > FOURK_PAGE_MASK ||
		    file_end - file_offset - file_size > FOURK_PAGE_MASK) {
			DEBUG4K_ERROR("LOAD_BADMACHO file_start / file_size wrap "
			    "[0x%llx:0x%llx] -> [0x%llx:0x%llx]\n",
			    file_offset,
			    file_offset + file_size,
			    (uint64_t) file_start,
			    (uint64_t) file_end);
			return LOAD_BADMACHO;
		}

		if (!strncmp(scp->segname, "__LINKEDIT", 11) &&
		    page_aligned(file_start) &&
		    vm_map_page_aligned(file_start, vm_map_page_mask(map)) &&
		    page_aligned(vm_start) &&
		    vm_map_page_aligned(vm_start, vm_map_page_mask(map))) {
			/* XXX last segment: ignore mis-aligned tail */
			file_end = vm_map_round_page(file_end,
			    effective_page_mask);
			vm_end = vm_map_round_page(vm_end,
			    effective_page_mask);
		}
	} else
#endif /* __arm64__ */
	{
		file_start = vm_map_trunc_page(file_offset,
		    effective_page_mask);
		file_end = vm_map_round_page(file_offset + file_size,
		    effective_page_mask);
		vm_start = vm_map_trunc_page(vm_offset,
		    effective_page_mask);
		vm_end = vm_map_round_page(vm_offset + vm_size,
		    effective_page_mask);

		if (file_offset - file_start > effective_page_mask ||
		    file_end - file_offset - file_size > effective_page_mask) {
			DEBUG4K_ERROR("LOAD_BADMACHO file_start / file_size wrap "
			    "[0x%llx:0x%llx] -> [0x%llx:0x%llx]\n",
			    file_offset,
			    file_offset + file_size,
			    (uint64_t) file_start,
			    (uint64_t) file_end);
			return LOAD_BADMACHO;
		}
	}

	if (vm_start < result->min_vm_addr) {
		result->min_vm_addr = vm_start;
	}
	if (vm_end > result->max_vm_addr) {
		result->max_vm_addr = vm_end;
	}

	if (map == VM_MAP_NULL) {
		return LOAD_SUCCESS;
	}

	if (vm_size > 0) {
		initprot = (scp->initprot) & VM_PROT_ALL;
		maxprot = (scp->maxprot) & VM_PROT_ALL;
		/*
		 *	Map a copy of the file into the address space.
		 */
		if (verbose) {
			MACHO_PRINTF(("++++++ load_segment: "
			    "mapping at vm [0x%llx:0x%llx] of "
			    "file [0x%llx:0x%llx]\n",
			    (uint64_t) vm_start,
			    (uint64_t) vm_end,
			    (uint64_t) file_start,
			    (uint64_t) file_end));
		}
		ret = map_segment(map,
		    vm_start,
		    vm_end,
		    control,
		    file_start,
		    file_end,
		    initprot,
		    maxprot,
		    result);
		if (ret) {
			DEBUG4K_ERROR("LOAD_NOSPACE start 0x%llx end 0x%llx ret 0x%x\n", (uint64_t)vm_start, (uint64_t)vm_end, ret);
			return LOAD_NOSPACE;
		}

#if FIXME
		/*
		 *	If the file didn't end on a page boundary,
		 *	we need to zero the leftover.
		 */
		delta_size = map_size - scp->filesize;
		if (delta_size > 0) {
			mach_vm_offset_t        tmp;

			ret = mach_vm_allocate_kernel(kernel_map, &tmp, delta_size, VM_FLAGS_ANYWHERE, VM_KERN_MEMORY_BSD);
			if (ret != KERN_SUCCESS) {
				DEBUG4K_ERROR("LOAD_RESOURCE delta_size 0x%llx ret 0x%x\n", delta_size, ret);
				return LOAD_RESOURCE;
			}

			if (copyout(tmp, map_addr + scp->filesize,
			    delta_size)) {
				(void) mach_vm_deallocate(
					kernel_map, tmp, delta_size);
				DEBUG4K_ERROR("LOAD_FAILURE copyout 0x%llx 0x%llx\n", map_addr + scp->filesize, delta_size);
				return LOAD_FAILURE;
			}

			(void) mach_vm_deallocate(kernel_map, tmp, delta_size);
		}
#endif /* FIXME */
	}

	/*
	 *	If the virtual size of the segment is greater
	 *	than the size from the file, we need to allocate
	 *	zero fill memory for the rest.
	 */
	if ((vm_end - vm_start) > (file_end - file_start)) {
		delta_size = (vm_end - vm_start) - (file_end - file_start);
	} else {
		delta_size = 0;
	}
	if (delta_size > 0) {
		vm_map_offset_t tmp_start;
		vm_map_offset_t tmp_end;

		if (os_add_overflow(vm_start, file_end - file_start, &tmp_start)) {
			DEBUG4K_ERROR("LOAD_NOSPACE tmp_start: 0x%llx + 0x%llx\n", (uint64_t)vm_start, (uint64_t)(file_end - file_start));
			return LOAD_NOSPACE;
		}

		if (os_add_overflow(tmp_start, delta_size, &tmp_end)) {
			DEBUG4K_ERROR("LOAD_NOSPACE tmp_end: 0x%llx + 0x%llx\n", (uint64_t)tmp_start, (uint64_t)delta_size);
			return LOAD_NOSPACE;
		}

		if (verbose) {
			MACHO_PRINTF(("++++++ load_segment: "
			    "delta mapping vm [0x%llx:0x%llx]\n",
			    (uint64_t) tmp_start,
			    (uint64_t) tmp_end));
		}
		kr = map_segment(map,
		    tmp_start,
		    tmp_end,
		    MEMORY_OBJECT_CONTROL_NULL,
		    0,
		    delta_size,
		    scp->initprot,
		    scp->maxprot,
		    result);
		if (kr != KERN_SUCCESS) {
			DEBUG4K_ERROR("LOAD_NOSPACE 0x%llx 0x%llx kr 0x%x\n", (unsigned long long)tmp_start, (uint64_t)delta_size, kr);
			return LOAD_NOSPACE;
		}
	}

	if ((scp->fileoff == 0) && (scp->filesize != 0)) {
		result->mach_header = vm_offset;
	}

	if (scp->flags & SG_PROTECTED_VERSION_1) {
		ret = unprotect_dsmos_segment(file_start,
		    file_end - file_start,
		    vp,
		    pager_offset,
		    map,
		    vm_start,
		    vm_end - vm_start);
		if (ret != LOAD_SUCCESS) {
			DEBUG4K_ERROR("unprotect 0x%llx 0x%llx ret %d \n", (uint64_t)vm_start, (uint64_t)vm_end, ret);
			return ret;
		}
	} else {
		ret = LOAD_SUCCESS;
	}

	if (LOAD_SUCCESS == ret &&
	    filetype == MH_DYLINKER &&
	    result->all_image_info_addr == MACH_VM_MIN_ADDRESS) {
		note_all_image_info_section(scp,
		    LC_SEGMENT_64 == lcp->cmd,
		    single_section_size,
		    ((const char *)lcp +
		    segment_command_size),
		    slide,
		    result);
	}

	if (result->entry_point != MACH_VM_MIN_ADDRESS) {
		if ((result->entry_point >= vm_offset) && (result->entry_point < (vm_offset + vm_size))) {
			if ((scp->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) == (VM_PROT_READ | VM_PROT_EXECUTE)) {
				result->validentry = 1;
			} else {
				/* right range but wrong protections, unset if previously validated */
				result->validentry = 0;
			}
		}
	}

	if (ret != LOAD_SUCCESS && verbose) {
		DEBUG4K_ERROR("ret %d\n", ret);
	}
	return ret;
}

static
load_return_t
load_uuid(
	struct uuid_command     *uulp,
	char                    *command_end,
	load_result_t           *result
	)
{
	/*
	 * We need to check the following for this command:
	 * - The command size should be atleast the size of struct uuid_command
	 * - The UUID part of the command should be completely within the mach-o header
	 */

	if ((uulp->cmdsize < sizeof(struct uuid_command)) ||
	    (((char *)uulp + sizeof(struct uuid_command)) > command_end)) {
		return LOAD_BADMACHO;
	}

	memcpy(&result->uuid[0], &uulp->uuid[0], sizeof(result->uuid));
	return LOAD_SUCCESS;
}

static
load_return_t
load_version(
	struct version_min_command     *vmc,
	boolean_t               *found_version_cmd,
	int                     ip_flags __unused,
	load_result_t           *result
	)
{
	uint32_t platform = 0;
	uint32_t sdk;
	uint32_t min_sdk;

	if (vmc->cmdsize < sizeof(*vmc)) {
		return LOAD_BADMACHO;
	}
	if (*found_version_cmd == TRUE) {
		return LOAD_BADMACHO;
	}
	*found_version_cmd = TRUE;
	sdk = vmc->sdk;
	min_sdk = vmc->version;
	switch (vmc->cmd) {
	case LC_VERSION_MIN_MACOSX:
		platform = PLATFORM_MACOS;
		break;
#if __x86_64__ /* __x86_64__ */
	case LC_VERSION_MIN_IPHONEOS:
		platform = PLATFORM_IOSSIMULATOR;
		break;
	case LC_VERSION_MIN_WATCHOS:
		platform = PLATFORM_WATCHOSSIMULATOR;
		break;
	case LC_VERSION_MIN_TVOS:
		platform = PLATFORM_TVOSSIMULATOR;
		break;
#else
	case LC_VERSION_MIN_IPHONEOS: {
#if __arm64__
		extern int legacy_footprint_entitlement_mode;
		if (vmc->sdk < (12 << 16)) {
			/* app built with a pre-iOS12 SDK: apply legacy footprint mitigation */
			result->legacy_footprint = TRUE;
		}
#endif /* __arm64__ */
		platform = PLATFORM_IOS;
		break;
	}
	case LC_VERSION_MIN_WATCHOS:
		platform = PLATFORM_WATCHOS;
		break;
	case LC_VERSION_MIN_TVOS:
		platform = PLATFORM_TVOS;
		break;
#endif /* __x86_64__ */
	/* All LC_VERSION_MIN_* load commands are legacy and we will not be adding any more */
	default:
		sdk = (uint32_t)-1;
		min_sdk = (uint32_t)-1;
		__builtin_unreachable();
	}
	result->ip_platform = platform;
	result->lr_min_sdk = min_sdk;
	result->lr_sdk = sdk;
	return LOAD_SUCCESS;
}

static
load_return_t
load_main(
	struct entry_point_command      *epc,
	thread_t                thread,
	int64_t                         slide,
	load_result_t           *result
	)
{
	mach_vm_offset_t addr;
	kern_return_t   ret;

	if (epc->cmdsize < sizeof(*epc)) {
		return LOAD_BADMACHO;
	}
	if (result->thread_count != 0) {
		return LOAD_FAILURE;
	}

	if (thread == THREAD_NULL) {
		return LOAD_SUCCESS;
	}

	/*
	 * LC_MAIN specifies stack size but not location.
	 * Add guard page to allocation size (MAXSSIZ includes guard page).
	 */
	if (epc->stacksize) {
		if (os_add_overflow(epc->stacksize, 4 * PAGE_SIZE, &result->user_stack_size)) {
			/*
			 * We are going to immediately throw away this result, but we want
			 * to make sure we aren't loading a dangerously close to
			 * overflowing value, since this will have a guard page added to it
			 * and be rounded to page boundaries
			 */
			return LOAD_BADMACHO;
		}
		result->user_stack_size = epc->stacksize;
		if (os_add_overflow(epc->stacksize, PAGE_SIZE, &result->user_stack_alloc_size)) {
			return LOAD_BADMACHO;
		}
		result->custom_stack = TRUE;
	} else {
		result->user_stack_alloc_size = MAXSSIZ;
	}

	/* use default location for stack */
	ret = thread_userstackdefault(&addr, result->is_64bit_addr);
	if (ret != KERN_SUCCESS) {
		return LOAD_FAILURE;
	}

	/* The stack slides down from the default location */
	result->user_stack = (user_addr_t)mach_vm_trunc_page((user_addr_t)addr - slide);

	if (result->using_lcmain || result->entry_point != MACH_VM_MIN_ADDRESS) {
		/* Already processed LC_MAIN or LC_UNIXTHREAD */
		return LOAD_FAILURE;
	}

	/* kernel does *not* use entryoff from LC_MAIN.	 Dyld uses it. */
	result->needs_dynlinker = TRUE;
	result->using_lcmain = TRUE;

	ret = thread_state_initialize( thread );
	if (ret != KERN_SUCCESS) {
		return LOAD_FAILURE;
	}

	result->unixproc = TRUE;
	result->thread_count++;

	return LOAD_SUCCESS;
}

static
load_return_t
setup_driver_main(
	thread_t                thread,
	int64_t                         slide,
	load_result_t           *result
	)
{
	mach_vm_offset_t addr;
	kern_return_t   ret;

	/* Driver binaries have no LC_MAIN, use defaults */

	if (thread == THREAD_NULL) {
		return LOAD_SUCCESS;
	}

	result->user_stack_alloc_size = MAXSSIZ;

	/* use default location for stack */
	ret = thread_userstackdefault(&addr, result->is_64bit_addr);
	if (ret != KERN_SUCCESS) {
		return LOAD_FAILURE;
	}

	/* The stack slides down from the default location */
	result->user_stack = (user_addr_t)addr;
	result->user_stack -= slide;

	if (result->using_lcmain || result->entry_point != MACH_VM_MIN_ADDRESS) {
		/* Already processed LC_MAIN or LC_UNIXTHREAD */
		return LOAD_FAILURE;
	}

	result->needs_dynlinker = TRUE;

	ret = thread_state_initialize( thread );
	if (ret != KERN_SUCCESS) {
		return LOAD_FAILURE;
	}

	result->unixproc = TRUE;
	result->thread_count++;

	return LOAD_SUCCESS;
}

static
load_return_t
load_unixthread(
	struct thread_command   *tcp,
	thread_t                thread,
	int64_t                         slide,
	boolean_t               is_x86_64_compat_binary,
	load_result_t           *result
	)
{
	load_return_t   ret;
	int customstack = 0;
	mach_vm_offset_t addr;
	if (tcp->cmdsize < sizeof(*tcp)) {
		return LOAD_BADMACHO;
	}
	if (result->thread_count != 0) {
		return LOAD_FAILURE;
	}

	if (thread == THREAD_NULL) {
		return LOAD_SUCCESS;
	}

	ret = load_threadstack(thread,
	    (uint32_t *)(((vm_offset_t)tcp) +
	    sizeof(struct thread_command)),
	    tcp->cmdsize - sizeof(struct thread_command),
	    &addr, &customstack, is_x86_64_compat_binary, result);
	if (ret != LOAD_SUCCESS) {
		return ret;
	}

	/* LC_UNIXTHREAD optionally specifies stack size and location */

	if (customstack) {
		result->custom_stack = TRUE;
	} else {
		result->user_stack_alloc_size = MAXSSIZ;
	}

	/* The stack slides down from the default location */
	result->user_stack = (user_addr_t)mach_vm_trunc_page((user_addr_t)addr - slide);

	{
		ret = load_threadentry(thread,
		    (uint32_t *)(((vm_offset_t)tcp) +
		    sizeof(struct thread_command)),
		    tcp->cmdsize - sizeof(struct thread_command),
		    &addr);
		if (ret != LOAD_SUCCESS) {
			return ret;
		}

		if (result->using_lcmain || result->entry_point != MACH_VM_MIN_ADDRESS) {
			/* Already processed LC_MAIN or LC_UNIXTHREAD */
			return LOAD_FAILURE;
		}

		result->entry_point = (user_addr_t)addr;
		result->entry_point += slide;

		ret = load_threadstate(thread,
		    (uint32_t *)(((vm_offset_t)tcp) + sizeof(struct thread_command)),
		    tcp->cmdsize - sizeof(struct thread_command),
		    result);
		if (ret != LOAD_SUCCESS) {
			return ret;
		}
	}

	result->unixproc = TRUE;
	result->thread_count++;

	return LOAD_SUCCESS;
}

static
load_return_t
load_threadstate(
	thread_t        thread,
	uint32_t        *ts,
	uint32_t        total_size,
	load_result_t   *result
	)
{
	uint32_t        size;
	int             flavor;
	uint32_t        thread_size;
	uint32_t        *local_ts = NULL;
	uint32_t        local_ts_size = 0;
	int             ret;

	(void)thread;

	if (total_size > 0) {
		local_ts_size = total_size;
		local_ts = kalloc(local_ts_size);
		if (local_ts == NULL) {
			return LOAD_FAILURE;
		}
		memcpy(local_ts, ts, local_ts_size);
		ts = local_ts;
	}

	/*
	 * Validate the new thread state; iterate through the state flavors in
	 * the Mach-O file.
	 * XXX: we should validate the machine state here, to avoid failing at
	 * activation time where we can't bail out cleanly.
	 */
	while (total_size > 0) {
		if (total_size < 2 * sizeof(uint32_t)) {
			return LOAD_BADMACHO;
		}

		flavor = *ts++;
		size = *ts++;

		if (os_add_and_mul_overflow(size, 2, sizeof(uint32_t), &thread_size) ||
		    os_sub_overflow(total_size, thread_size, &total_size)) {
			ret = LOAD_BADMACHO;
			goto bad;
		}

		ts += size;     /* ts is a (uint32_t *) */
	}

	result->threadstate = local_ts;
	result->threadstate_sz = local_ts_size;
	return LOAD_SUCCESS;

bad:
	if (local_ts) {
		kfree(local_ts, local_ts_size);
	}
	return ret;
}


static
load_return_t
load_threadstack(
	thread_t                thread,
	uint32_t                *ts,
	uint32_t                total_size,
	mach_vm_offset_t        *user_stack,
	int                     *customstack,
	__unused boolean_t      is_x86_64_compat_binary,
	load_result_t           *result
	)
{
	kern_return_t   ret;
	uint32_t        size;
	int             flavor;
	uint32_t        stack_size;

	if (total_size == 0) {
		return LOAD_BADMACHO;
	}

	while (total_size > 0) {
		if (total_size < 2 * sizeof(uint32_t)) {
			return LOAD_BADMACHO;
		}

		flavor = *ts++;
		size = *ts++;
		if (UINT32_MAX - 2 < size ||
		    UINT32_MAX / sizeof(uint32_t) < size + 2) {
			return LOAD_BADMACHO;
		}
		stack_size = (size + 2) * sizeof(uint32_t);
		if (stack_size > total_size) {
			return LOAD_BADMACHO;
		}
		total_size -= stack_size;

		/*
		 * Third argument is a kernel space pointer; it gets cast
		 * to the appropriate type in thread_userstack() based on
		 * the value of flavor.
		 */
		{
			ret = thread_userstack(thread, flavor, (thread_state_t)ts, size, user_stack, customstack, result->is_64bit_data);
			if (ret != KERN_SUCCESS) {
				return LOAD_FAILURE;
			}
		}

		ts += size;     /* ts is a (uint32_t *) */
	}
	return LOAD_SUCCESS;
}

static
load_return_t
load_threadentry(
	thread_t        thread,
	uint32_t        *ts,
	uint32_t        total_size,
	mach_vm_offset_t        *entry_point
	)
{
	kern_return_t   ret;
	uint32_t        size;
	int             flavor;
	uint32_t        entry_size;

	/*
	 *	Set the thread state.
	 */
	*entry_point = MACH_VM_MIN_ADDRESS;
	while (total_size > 0) {
		if (total_size < 2 * sizeof(uint32_t)) {
			return LOAD_BADMACHO;
		}

		flavor = *ts++;
		size = *ts++;
		if (UINT32_MAX - 2 < size ||
		    UINT32_MAX / sizeof(uint32_t) < size + 2) {
			return LOAD_BADMACHO;
		}
		entry_size = (size + 2) * sizeof(uint32_t);
		if (entry_size > total_size) {
			return LOAD_BADMACHO;
		}
		total_size -= entry_size;
		/*
		 * Third argument is a kernel space pointer; it gets cast
		 * to the appropriate type in thread_entrypoint() based on
		 * the value of flavor.
		 */
		ret = thread_entrypoint(thread, flavor, (thread_state_t)ts, size, entry_point);
		if (ret != KERN_SUCCESS) {
			return LOAD_FAILURE;
		}
		ts += size;     /* ts is a (uint32_t *) */
	}
	return LOAD_SUCCESS;
}

struct macho_data {
	struct nameidata        __nid;
	union macho_vnode_header {
		struct mach_header      mach_header;
		struct fat_header       fat_header;
		char    __pad[512];
	} __header;
};

#define DEFAULT_DYLD_PATH "/usr/lib/dyld"

#if (DEVELOPMENT || DEBUG)
extern char dyld_alt_path[];
extern int use_alt_dyld;
#endif

static load_return_t
load_dylinker(
	struct dylinker_command *lcp,
	cpu_type_t              cputype,
	vm_map_t                map,
	thread_t        thread,
	int                     depth,
	int64_t                 slide,
	load_result_t           *result,
	struct image_params     *imgp
	)
{
	const char              *name;
	struct vnode            *vp = NULLVP;   /* set by get_macho_vnode() */
	struct mach_header      *header;
	off_t                   file_offset = 0; /* set by get_macho_vnode() */
	off_t                   macho_size = 0; /* set by get_macho_vnode() */
	load_result_t           *myresult;
	kern_return_t           ret;
	struct macho_data       *macho_data;
	struct {
		struct mach_header      __header;
		load_result_t           __myresult;
		struct macho_data       __macho_data;
	} *dyld_data;

	if (lcp->cmdsize < sizeof(*lcp) || lcp->name.offset >= lcp->cmdsize) {
		return LOAD_BADMACHO;
	}

	name = (const char *)lcp + lcp->name.offset;

	/* Check for a proper null terminated string. */
	size_t maxsz = lcp->cmdsize - lcp->name.offset;
	size_t namelen = strnlen(name, maxsz);
	if (namelen >= maxsz) {
		return LOAD_BADMACHO;
	}

#if (DEVELOPMENT || DEBUG)

	/*
	 * rdar://23680808
	 * If an alternate dyld has been specified via boot args, check
	 * to see if PROC_UUID_ALT_DYLD_POLICY has been set on this
	 * executable and redirect the kernel to load that linker.
	 */

	if (use_alt_dyld) {
		int policy_error;
		uint32_t policy_flags = 0;
		int32_t policy_gencount = 0;

		policy_error = proc_uuid_policy_lookup(result->uuid, &policy_flags, &policy_gencount);
		if (policy_error == 0) {
			if (policy_flags & PROC_UUID_ALT_DYLD_POLICY) {
				name = dyld_alt_path;
			}
		}
	}
#endif

#if !(DEVELOPMENT || DEBUG)
	if (0 != strcmp(name, DEFAULT_DYLD_PATH)) {
		return LOAD_BADMACHO;
	}
#endif

	/* Allocate wad-of-data from heap to reduce excessively deep stacks */

	dyld_data = kheap_alloc(KHEAP_TEMP, sizeof(*dyld_data), Z_WAITOK);
	header = &dyld_data->__header;
	myresult = &dyld_data->__myresult;
	macho_data = &dyld_data->__macho_data;

	{
		cputype = (cputype & CPU_ARCH_MASK) | (cpu_type() & ~CPU_ARCH_MASK);
	}

	ret = get_macho_vnode(name, cputype, header,
	    &file_offset, &macho_size, macho_data, &vp, imgp);
	if (ret) {
		goto novp_out;
	}

	*myresult = load_result_null;
	myresult->is_64bit_addr = result->is_64bit_addr;
	myresult->is_64bit_data = result->is_64bit_data;

	ret = parse_machfile(vp, map, thread, header, file_offset,
	    macho_size, depth, slide, 0, myresult, result, imgp);

	if (ret == LOAD_SUCCESS) {
		if (result->threadstate) {
			/* don't use the app's threadstate if we have a dyld */
			kfree(result->threadstate, result->threadstate_sz);
		}
		result->threadstate = myresult->threadstate;
		result->threadstate_sz = myresult->threadstate_sz;

		result->dynlinker = TRUE;
		result->entry_point = myresult->entry_point;
		result->validentry = myresult->validentry;
		result->all_image_info_addr = myresult->all_image_info_addr;
		result->all_image_info_size = myresult->all_image_info_size;
		if (myresult->platform_binary) {
			result->csflags |= CS_DYLD_PLATFORM;
		}

	}

	struct vnode_attr *va;
	va = kheap_alloc(KHEAP_TEMP, sizeof(*va), Z_WAITOK | Z_ZERO);
	VATTR_INIT(va);
	VATTR_WANTED(va, va_fsid64);
	VATTR_WANTED(va, va_fsid);
	VATTR_WANTED(va, va_fileid);
	int error = vnode_getattr(vp, va, imgp->ip_vfs_context);
	if (error == 0) {
		imgp->ip_dyld_fsid = vnode_get_va_fsid(va);
		imgp->ip_dyld_fsobjid = va->va_fileid;
	}

	vnode_put(vp);
	kheap_free(KHEAP_TEMP, va, sizeof(*va));
novp_out:
	kheap_free(KHEAP_TEMP, dyld_data, sizeof(*dyld_data));
	return ret;
}


static load_return_t
load_code_signature(
	struct linkedit_data_command    *lcp,
	struct vnode                    *vp,
	off_t                           macho_offset,
	off_t                           macho_size,
	cpu_type_t                      cputype,
	cpu_subtype_t                   cpusubtype,
	load_result_t                   *result,
	struct image_params             *imgp)
{
	int             ret;
	kern_return_t   kr;
	vm_offset_t     addr;
	int             resid;
	struct cs_blob  *blob;
	int             error;
	vm_size_t       blob_size;
	uint32_t        sum;
	boolean_t               anyCPU;

	addr = 0;
	blob = NULL;

	cpusubtype &= ~CPU_SUBTYPE_MASK;

	if (lcp->cmdsize != sizeof(struct linkedit_data_command)) {
		ret = LOAD_BADMACHO;
		goto out;
	}

	sum = 0;
	if (os_add_overflow(lcp->dataoff, lcp->datasize, &sum) || sum > macho_size) {
		ret = LOAD_BADMACHO;
		goto out;
	}

	blob = ubc_cs_blob_get(vp, cputype, cpusubtype, macho_offset);

	if (blob != NULL) {
		/* we already have a blob for this vnode and cpu(sub)type */
		anyCPU = blob->csb_cpu_type == -1;
		if ((blob->csb_cpu_type != cputype &&
		    blob->csb_cpu_subtype != cpusubtype && !anyCPU) ||
		    blob->csb_base_offset != macho_offset) {
			/* the blob has changed for this vnode: fail ! */
			ret = LOAD_BADMACHO;
			goto out;
		}

		/* It matches the blob we want here, let's verify the version */
		if (!anyCPU && ubc_cs_generation_check(vp) == 0) {
			/* No need to revalidate, we're good! */
			ret = LOAD_SUCCESS;
			goto out;
		}

		/* That blob may be stale, let's revalidate. */
		error = ubc_cs_blob_revalidate(vp, blob, imgp, 0, result->ip_platform);
		if (error == 0) {
			/* Revalidation succeeded, we're good! */
			/* If we were revaliding a CS blob with any CPU arch we adjust it */
			if (anyCPU) {
				vnode_lock_spin(vp);
				blob->csb_cpu_type = cputype;
				blob->csb_cpu_subtype = cpusubtype;
				vnode_unlock(vp);
			}
			ret = LOAD_SUCCESS;
			goto out;
		}

		if (error != EAGAIN) {
			printf("load_code_signature: revalidation failed: %d\n", error);
			ret = LOAD_FAILURE;
			goto out;
		}

		assert(error == EAGAIN);

		/*
		 * Revalidation was not possible for this blob. We just continue as if there was no blob,
		 * rereading the signature, and ubc_cs_blob_add will do the right thing.
		 */
		blob = NULL;
	}

	blob_size = lcp->datasize;
	kr = ubc_cs_blob_allocate(&addr, &blob_size);
	if (kr != KERN_SUCCESS) {
		ret = LOAD_NOSPACE;
		goto out;
	}

	resid = 0;
	error = vn_rdwr(UIO_READ,
	    vp,
	    (caddr_t) addr,
	    lcp->datasize,
	    macho_offset + lcp->dataoff,
	    UIO_SYSSPACE,
	    0,
	    kauth_cred_get(),
	    &resid,
	    current_proc());
	if (error || resid != 0) {
		ret = LOAD_IOERROR;
		goto out;
	}

	if (ubc_cs_blob_add(vp,
	    result->ip_platform,
	    cputype,
	    cpusubtype,
	    macho_offset,
	    &addr,
	    lcp->datasize,
	    imgp,
	    0,
	    &blob)) {
		if (addr) {
			ubc_cs_blob_deallocate(addr, blob_size);
			addr = 0;
		}
		ret = LOAD_FAILURE;
		goto out;
	} else {
		/* ubc_cs_blob_add() has consumed "addr" */
		addr = 0;
	}

#if CHECK_CS_VALIDATION_BITMAP
	ubc_cs_validation_bitmap_allocate( vp );
#endif

	ret = LOAD_SUCCESS;
out:
	if (ret == LOAD_SUCCESS) {
		if (blob == NULL) {
			panic("success, but no blob!");
		}

		result->csflags |= blob->csb_flags;
		result->platform_binary = blob->csb_platform_binary;
		result->cs_end_offset = blob->csb_end_offset;
	}
	if (addr != 0) {
		ubc_cs_blob_deallocate(addr, blob_size);
		addr = 0;
	}

	return ret;
}


#if CONFIG_CODE_DECRYPTION

static load_return_t
set_code_unprotect(
	struct encryption_info_command *eip,
	caddr_t addr,
	vm_map_t map,
	int64_t slide,
	struct vnode *vp,
	off_t macho_offset,
	cpu_type_t cputype,
	cpu_subtype_t cpusubtype)
{
	int error, len;
	pager_crypt_info_t crypt_info;
	const char * cryptname = 0;
	char *vpath;

	size_t offset;
	struct segment_command_64 *seg64;
	struct segment_command *seg32;
	vm_map_offset_t map_offset, map_size;
	vm_object_offset_t crypto_backing_offset;
	kern_return_t kr;

	if (eip->cmdsize < sizeof(*eip)) {
		return LOAD_BADMACHO;
	}

	switch (eip->cryptid) {
	case 0:
		/* not encrypted, just an empty load command */
		return LOAD_SUCCESS;
	case 1:
		cryptname = "com.apple.unfree";
		break;
	case 0x10:
		/* some random cryptid that you could manually put into
		 * your binary if you want NULL */
		cryptname = "com.apple.null";
		break;
	default:
		return LOAD_BADMACHO;
	}

	if (map == VM_MAP_NULL) {
		return LOAD_SUCCESS;
	}
	if (NULL == text_crypter_create) {
		return LOAD_FAILURE;
	}

	vpath = zalloc(ZV_NAMEI);

	len = MAXPATHLEN;
	error = vn_getpath(vp, vpath, &len);
	if (error) {
		zfree(ZV_NAMEI, vpath);
		return LOAD_FAILURE;
	}

	/* set up decrypter first */
	crypt_file_data_t crypt_data = {
		.filename = vpath,
		.cputype = cputype,
		.cpusubtype = cpusubtype
	};
	kr = text_crypter_create(&crypt_info, cryptname, (void*)&crypt_data);
#if VM_MAP_DEBUG_APPLE_PROTECT
	if (vm_map_debug_apple_protect) {
		struct proc *p;
		p  = current_proc();
		printf("APPLE_PROTECT: %d[%s] map %p %s(%s) -> 0x%x\n",
		    p->p_pid, p->p_comm, map, __FUNCTION__, vpath, kr);
	}
#endif /* VM_MAP_DEBUG_APPLE_PROTECT */
	zfree(ZV_NAMEI, vpath);

	if (kr) {
		printf("set_code_unprotect: unable to create decrypter %s, kr=%d\n",
		    cryptname, kr);
		if (kr == kIOReturnNotPrivileged) {
			/* text encryption returned decryption failure */
			return LOAD_DECRYPTFAIL;
		} else {
			return LOAD_RESOURCE;
		}
	}

	/* this is terrible, but we have to rescan the load commands to find the
	 * virtual address of this encrypted stuff. This code is gonna look like
	 * the dyld source one day... */
	struct mach_header *header = (struct mach_header *)addr;
	size_t mach_header_sz = sizeof(struct mach_header);
	if (header->magic == MH_MAGIC_64 ||
	    header->magic == MH_CIGAM_64) {
		mach_header_sz = sizeof(struct mach_header_64);
	}
	offset = mach_header_sz;
	uint32_t ncmds = header->ncmds;
	while (ncmds--) {
		/*
		 *	Get a pointer to the command.
		 */
		struct load_command *lcp = (struct load_command *)(addr + offset);
		offset += lcp->cmdsize;

		switch (lcp->cmd) {
		case LC_SEGMENT_64:
			seg64 = (struct segment_command_64 *)lcp;
			if ((seg64->fileoff <= eip->cryptoff) &&
			    (seg64->fileoff + seg64->filesize >=
			    eip->cryptoff + eip->cryptsize)) {
				map_offset = (vm_map_offset_t)(seg64->vmaddr + eip->cryptoff - seg64->fileoff + slide);
				map_size = eip->cryptsize;
				crypto_backing_offset = macho_offset + eip->cryptoff;
				goto remap_now;
			}
			break;
		case LC_SEGMENT:
			seg32 = (struct segment_command *)lcp;
			if ((seg32->fileoff <= eip->cryptoff) &&
			    (seg32->fileoff + seg32->filesize >=
			    eip->cryptoff + eip->cryptsize)) {
				map_offset = (vm_map_offset_t)(seg32->vmaddr + eip->cryptoff - seg32->fileoff + slide);
				map_size = eip->cryptsize;
				crypto_backing_offset = macho_offset + eip->cryptoff;
				goto remap_now;
			}
			break;
		}
	}

	/* if we get here, did not find anything */
	return LOAD_BADMACHO;

remap_now:
	/* now remap using the decrypter */
	MACHO_PRINTF(("+++ set_code_unprotect: vm[0x%llx:0x%llx]\n",
	    (uint64_t) map_offset,
	    (uint64_t) (map_offset + map_size)));
	kr = vm_map_apple_protected(map,
	    map_offset,
	    map_offset + map_size,
	    crypto_backing_offset,
	    &crypt_info,
	    CRYPTID_APP_ENCRYPTION);
	if (kr) {
		printf("set_code_unprotect(): mapping failed with %x\n", kr);
		return LOAD_PROTECT;
	}

	return LOAD_SUCCESS;
}

#endif

/*
 * This routine exists to support the load_dylinker().
 *
 * This routine has its own, separate, understanding of the FAT file format,
 * which is terrifically unfortunate.
 */
static
load_return_t
get_macho_vnode(
	const char              *path,
	cpu_type_t              cputype,
	struct mach_header      *mach_header,
	off_t                   *file_offset,
	off_t                   *macho_size,
	struct macho_data       *data,
	struct vnode            **vpp,
	struct image_params     *imgp
	)
{
	struct vnode            *vp;
	vfs_context_t           ctx = vfs_context_current();
	proc_t                  p = vfs_context_proc(ctx);
	kauth_cred_t            kerncred;
	struct nameidata        *ndp = &data->__nid;
	boolean_t               is_fat;
	struct fat_arch         fat_arch;
	int                     error;
	int resid;
	union macho_vnode_header *header = &data->__header;
	off_t fsize = (off_t)0;

	/*
	 * Capture the kernel credential for use in the actual read of the
	 * file, since the user doing the execution may have execute rights
	 * but not read rights, but to exec something, we have to either map
	 * or read it into the new process address space, which requires
	 * read rights.  This is to deal with lack of common credential
	 * serialization code which would treat NOCRED as "serialize 'root'".
	 */
	kerncred = vfs_context_ucred(vfs_context_kernel());

	/* init the namei data to point the file user's program name */
	NDINIT(ndp, LOOKUP, OP_OPEN, FOLLOW | LOCKLEAF, UIO_SYSSPACE, CAST_USER_ADDR_T(path), ctx);

	if ((error = namei(ndp)) != 0) {
		if (error == ENOENT) {
			error = LOAD_ENOENT;
		} else {
			error = LOAD_FAILURE;
		}
		return error;
	}
	nameidone(ndp);
	vp = ndp->ni_vp;

	/* check for regular file */
	if (vp->v_type != VREG) {
		error = LOAD_PROTECT;
		goto bad1;
	}

	/* get size */
	if ((error = vnode_size(vp, &fsize, ctx)) != 0) {
		error = LOAD_FAILURE;
		goto bad1;
	}

	/* Check mount point */
	if (vp->v_mount->mnt_flag & MNT_NOEXEC) {
		error = LOAD_PROTECT;
		goto bad1;
	}

	/* check access */
	if ((error = vnode_authorize(vp, NULL, KAUTH_VNODE_EXECUTE | KAUTH_VNODE_READ_DATA, ctx)) != 0) {
		error = LOAD_PROTECT;
		goto bad1;
	}

	/* try to open it */
	if ((error = VNOP_OPEN(vp, FREAD, ctx)) != 0) {
		error = LOAD_PROTECT;
		goto bad1;
	}

	if ((error = vn_rdwr(UIO_READ, vp, (caddr_t)header, sizeof(*header), 0,
	    UIO_SYSSPACE, IO_NODELOCKED, kerncred, &resid, p)) != 0) {
		error = LOAD_IOERROR;
		goto bad2;
	}

	if (resid) {
		error = LOAD_BADMACHO;
		goto bad2;
	}

	if (header->mach_header.magic == MH_MAGIC ||
	    header->mach_header.magic == MH_MAGIC_64) {
		is_fat = FALSE;
	} else if (OSSwapBigToHostInt32(header->fat_header.magic) == FAT_MAGIC) {
		is_fat = TRUE;
	} else {
		error = LOAD_BADMACHO;
		goto bad2;
	}

	if (is_fat) {
		error = fatfile_validate_fatarches((vm_offset_t)(&header->fat_header),
		    sizeof(*header));
		if (error != LOAD_SUCCESS) {
			goto bad2;
		}

		/* Look up our architecture in the fat file. */
		error = fatfile_getbestarch_for_cputype(cputype, CPU_SUBTYPE_ANY,
		    (vm_offset_t)(&header->fat_header), sizeof(*header), imgp, &fat_arch);
		if (error != LOAD_SUCCESS) {
			goto bad2;
		}

		/* Read the Mach-O header out of it */
		error = vn_rdwr(UIO_READ, vp, (caddr_t)&header->mach_header,
		    sizeof(header->mach_header), fat_arch.offset,
		    UIO_SYSSPACE, IO_NODELOCKED, kerncred, &resid, p);
		if (error) {
			error = LOAD_IOERROR;
			goto bad2;
		}

		if (resid) {
			error = LOAD_BADMACHO;
			goto bad2;
		}

		/* Is this really a Mach-O? */
		if (header->mach_header.magic != MH_MAGIC &&
		    header->mach_header.magic != MH_MAGIC_64) {
			error = LOAD_BADMACHO;
			goto bad2;
		}

		*file_offset = fat_arch.offset;
		*macho_size = fat_arch.size;
	} else {
		/*
		 * Force get_macho_vnode() to fail if the architecture bits
		 * do not match the expected architecture bits.  This in
		 * turn causes load_dylinker() to fail for the same reason,
		 * so it ensures the dynamic linker and the binary are in
		 * lock-step.  This is potentially bad, if we ever add to
		 * the CPU_ARCH_* bits any bits that are desirable but not
		 * required, since the dynamic linker might work, but we will
		 * refuse to load it because of this check.
		 */
		if ((cpu_type_t)header->mach_header.cputype != cputype) {
			error = LOAD_BADARCH;
			goto bad2;
		}

		*file_offset = 0;
		*macho_size = fsize;
	}

	*mach_header = header->mach_header;
	*vpp = vp;

	ubc_setsize(vp, fsize);
	return error;

bad2:
	(void) VNOP_CLOSE(vp, FREAD, ctx);
bad1:
	vnode_put(vp);
	return error;
}
