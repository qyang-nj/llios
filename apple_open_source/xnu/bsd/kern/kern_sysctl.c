/*
 * Copyright (c) 2000-2019 Apple Inc. All rights reserved.
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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*-
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Mike Karels at Berkeley Software Design, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_sysctl.c	8.4 (Berkeley) 4/14/94
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

/*
 * DEPRECATED sysctl system call code
 *
 * Everything in this file is deprecated. Sysctls should be handled
 * by the code in kern_newsysctl.c.
 * The remaining "case" sections are supposed to be converted into
 * SYSCTL_*-style definitions, and as soon as all of them are gone,
 * this source file is supposed to die.
 *
 * DO NOT ADD ANY MORE "case" SECTIONS TO THIS FILE, instead define
 * your sysctl with SYSCTL_INT, SYSCTL_PROC etc. in your source file.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/file_internal.h>
#include <sys/vnode_internal.h>
#include <sys/unistd.h>
#include <sys/buf.h>
#include <sys/ioctl.h>
#include <sys/namei.h>
#include <sys/tty.h>
#include <sys/disklabel.h>
#include <sys/vm.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/aio_kern.h>
#include <sys/reboot.h>
#include <sys/memory_maintenance.h>
#include <sys/priv.h>
#include <stdatomic.h>
#include <uuid/uuid.h>

#include <security/audit/audit.h>
#include <kern/kalloc.h>

#include <machine/smp.h>
#include <machine/atomic.h>
#include <machine/config.h>
#include <mach/machine.h>
#include <mach/mach_host.h>
#include <mach/mach_types.h>
#include <mach/processor_info.h>
#include <mach/vm_param.h>
#include <kern/debug.h>
#include <kern/mach_param.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_group.h>
#include <kern/processor.h>
#include <kern/cpu_number.h>
#include <kern/cpu_quiesce.h>
#include <kern/sched_prim.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <mach/host_info.h>

#include <sys/mount_internal.h>
#include <sys/kdebug.h>
#include <sys/kern_sysctl.h>

#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>

#include <machine/machine_routines.h>
#include <machine/exec.h>

#include <nfs/nfs_conf.h>

#include <vm/vm_protos.h>
#include <vm/vm_pageout.h>
#include <vm/vm_compressor_algorithms.h>
#include <sys/imgsrc.h>
#include <kern/timer_call.h>
#include <sys/codesign.h>
#include <IOKit/IOBSD.h>
#if CONFIG_CSR
#include <sys/csr.h>
#endif

#if defined(__i386__) || defined(__x86_64__)
#include <i386/cpuid.h>
#endif

#if CONFIG_FREEZE
#include <sys/kern_memorystatus.h>
#endif

#if KPERF
#include <kperf/kperf.h>
#endif

#if HYPERVISOR
#include <kern/hv_support.h>
#endif

/*
 * deliberately setting max requests to really high number
 * so that runaway settings do not cause MALLOC overflows
 */
#define AIO_MAX_REQUESTS (128 * CONFIG_AIO_MAX)

extern int aio_max_requests;
extern int aio_max_requests_per_process;
extern int aio_worker_threads;
extern int lowpri_IO_window_msecs;
extern int lowpri_IO_delay_msecs;
#if DEVELOPMENT || DEBUG
extern int nx_enabled;
#endif
extern int speculative_reads_disabled;
extern unsigned int speculative_prefetch_max;
extern unsigned int speculative_prefetch_max_iosize;
extern unsigned int preheat_max_bytes;
extern unsigned int preheat_min_bytes;
extern long numvnodes;
extern long freevnodes;
extern long num_recycledvnodes;

extern uuid_string_t bootsessionuuid_string;

extern unsigned int vm_max_delayed_work_limit;
extern unsigned int vm_max_batch;

extern unsigned int vm_page_free_min;
extern unsigned int vm_page_free_target;
extern unsigned int vm_page_free_reserved;

#if (DEVELOPMENT || DEBUG)
extern uint32_t vm_page_creation_throttled_hard;
extern uint32_t vm_page_creation_throttled_soft;
#endif /* DEVELOPMENT || DEBUG */

#if CONFIG_LOCKERBOOT
extern const char kernel_protoboot_mount[];
#endif

/*
 * Conditionally allow dtrace to see these functions for debugging purposes.
 */
#ifdef STATIC
#undef STATIC
#endif
#if 0
#define STATIC
#else
#define STATIC static
#endif

extern boolean_t    mach_timer_coalescing_enabled;

extern uint64_t timer_deadline_tracking_bin_1, timer_deadline_tracking_bin_2;

STATIC void
fill_user32_eproc(proc_t, struct user32_eproc *__restrict);
STATIC void
fill_user32_externproc(proc_t, struct user32_extern_proc *__restrict);
STATIC void
fill_user64_eproc(proc_t, struct user64_eproc *__restrict);
STATIC void
fill_user64_proc(proc_t, struct user64_kinfo_proc *__restrict);
STATIC void
fill_user64_externproc(proc_t, struct user64_extern_proc *__restrict);
STATIC void
fill_user32_proc(proc_t, struct user32_kinfo_proc *__restrict);

extern int
kdbg_control(int *name, u_int namelen, user_addr_t where, size_t * sizep);
#if CONFIG_NFS_CLIENT
extern int
netboot_root(void);
#endif
int
sysctl_procargs(int *name, u_int namelen, user_addr_t where,
    size_t *sizep, proc_t cur_proc);
STATIC int
sysctl_procargsx(int *name, u_int namelen, user_addr_t where, size_t *sizep,
    proc_t cur_proc, int argc_yes);
int
sysctl_struct(user_addr_t oldp, size_t *oldlenp, user_addr_t newp,
    size_t newlen, void *sp, int len);

STATIC int sysdoproc_filt_KERN_PROC_PID(proc_t p, void * arg);
STATIC int sysdoproc_filt_KERN_PROC_PGRP(proc_t p, void * arg);
STATIC int sysdoproc_filt_KERN_PROC_TTY(proc_t p, void * arg);
STATIC int  sysdoproc_filt_KERN_PROC_UID(proc_t p, void * arg);
STATIC int  sysdoproc_filt_KERN_PROC_RUID(proc_t p, void * arg);
int sysdoproc_callback(proc_t p, void *arg);

#if CONFIG_THREAD_GROUPS && (DEVELOPMENT || DEBUG)
STATIC int sysctl_get_thread_group_id SYSCTL_HANDLER_ARGS;
#endif

/* forward declarations for non-static STATIC */
STATIC void fill_loadavg64(struct loadavg *la, struct user64_loadavg *la64);
STATIC void fill_loadavg32(struct loadavg *la, struct user32_loadavg *la32);
STATIC int sysctl_handle_kern_threadname(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_sched_stats(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_sched_stats_enable(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_kdebug_ops SYSCTL_HANDLER_ARGS;
#if COUNT_SYSCALLS
STATIC int sysctl_docountsyscalls SYSCTL_HANDLER_ARGS;
#endif  /* COUNT_SYSCALLS */
#if defined(XNU_TARGET_OS_OSX)
STATIC int sysctl_doprocargs SYSCTL_HANDLER_ARGS;
#endif  /* defined(XNU_TARGET_OS_OSX) */
STATIC int sysctl_doprocargs2 SYSCTL_HANDLER_ARGS;
STATIC int sysctl_prochandle SYSCTL_HANDLER_ARGS;
STATIC int sysctl_aiomax(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_aioprocmax(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_aiothreads(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_maxproc(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_osversion(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_sysctl_bootargs(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_maxvnodes(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_securelvl(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_domainname(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_hostname(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_procname(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_boottime(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_bootuuid(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_symfile(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#if CONFIG_NFS_CLIENT
STATIC int sysctl_netboot(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#endif
#ifdef CONFIG_IMGSRC_ACCESS
STATIC int sysctl_imgsrcdev(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#endif
STATIC int sysctl_usrstack(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_usrstack64(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#if CONFIG_COREDUMP
STATIC int sysctl_coredump(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_suid_coredump(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#endif
STATIC int sysctl_delayterm(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_rage_vnode(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_kern_check_openevt(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#if DEVELOPMENT || DEBUG
STATIC int sysctl_nx(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#endif
STATIC int sysctl_loadavg(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_vm_toggle_address_reuse(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_swapusage(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int fetch_process_cputype( proc_t cur_proc, int *name, u_int namelen, cpu_type_t *cputype);
STATIC int sysctl_sysctl_native(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_sysctl_cputype(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_safeboot(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_singleuser(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_minimalboot(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_slide(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);

#ifdef CONFIG_XNUPOST
#include <tests/xnupost.h>

STATIC int sysctl_debug_test_oslog_ctl(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_debug_test_stackshot_mutex_owner(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
STATIC int sysctl_debug_test_stackshot_rwlck_owner(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req);
#endif

extern void IORegistrySetOSBuildVersion(char * build_version);

STATIC void
fill_loadavg64(struct loadavg *la, struct user64_loadavg *la64)
{
	la64->ldavg[0]  = la->ldavg[0];
	la64->ldavg[1]  = la->ldavg[1];
	la64->ldavg[2]  = la->ldavg[2];
	la64->fscale    = (user64_long_t)la->fscale;
}

STATIC void
fill_loadavg32(struct loadavg *la, struct user32_loadavg *la32)
{
	la32->ldavg[0]  = la->ldavg[0];
	la32->ldavg[1]  = la->ldavg[1];
	la32->ldavg[2]  = la->ldavg[2];
	la32->fscale    = (user32_long_t)la->fscale;
}

#if CONFIG_COREDUMP
/*
 * Attributes stored in the kernel.
 */
extern char corefilename[MAXPATHLEN + 1];
extern int do_coredump;
extern int sugid_coredump;
#endif

#if COUNT_SYSCALLS
extern int do_count_syscalls;
#endif

#ifdef INSECURE
int securelevel = -1;
#else
int securelevel;
#endif

STATIC int
sysctl_handle_kern_threadname(  __unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int error;
	struct uthread *ut = get_bsdthread_info(current_thread());
	user_addr_t oldp = 0, newp = 0;
	size_t *oldlenp = NULL;
	size_t newlen = 0;

	oldp = req->oldptr;
	oldlenp = &(req->oldlen);
	newp = req->newptr;
	newlen = req->newlen;

	/* We want the current length, and maybe the string itself */
	if (oldlenp) {
		/* if we have no thread name yet tell'em we want MAXTHREADNAMESIZE - 1 */
		size_t currlen = MAXTHREADNAMESIZE - 1;

		if (ut->pth_name) {
			/* use length of current thread name */
			currlen = strlen(ut->pth_name);
		}
		if (oldp) {
			if (*oldlenp < currlen) {
				return ENOMEM;
			}
			/* NOTE - we do not copy the NULL terminator */
			if (ut->pth_name) {
				error = copyout(ut->pth_name, oldp, currlen);
				if (error) {
					return error;
				}
			}
		}
		/* return length of thread name minus NULL terminator (just like strlen)  */
		req->oldidx = currlen;
	}

	/* We want to set the name to something */
	if (newp) {
		if (newlen > (MAXTHREADNAMESIZE - 1)) {
			return ENAMETOOLONG;
		}
		if (!ut->pth_name) {
			char *tmp_pth_name = (char *)kalloc(MAXTHREADNAMESIZE);
			if (!tmp_pth_name) {
				return ENOMEM;
			}
			bzero(tmp_pth_name, MAXTHREADNAMESIZE);
			if (!OSCompareAndSwapPtr(NULL, tmp_pth_name, &ut->pth_name)) {
				kfree(tmp_pth_name, MAXTHREADNAMESIZE);
				return EBUSY;
			}
		} else {
			kernel_debug_string_simple(TRACE_STRING_THREADNAME_PREV, ut->pth_name);
			bzero(ut->pth_name, MAXTHREADNAMESIZE);
		}
		error = copyin(newp, ut->pth_name, newlen);
		if (error) {
			return error;
		}

		kernel_debug_string_simple(TRACE_STRING_THREADNAME, ut->pth_name);
	}

	return 0;
}

SYSCTL_PROC(_kern, KERN_THREADNAME, threadname, CTLFLAG_ANYBODY | CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_handle_kern_threadname, "A", "");

#define BSD_HOST 1
STATIC int
sysctl_sched_stats(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	host_basic_info_data_t hinfo;
	kern_return_t kret;
	uint32_t size;
	uint32_t buf_size = 0;
	int changed;
	mach_msg_type_number_t count = HOST_BASIC_INFO_COUNT;
	struct _processor_statistics_np *buf;
	int error;

	kret = host_info((host_t)BSD_HOST, HOST_BASIC_INFO, (host_info_t)&hinfo, &count);
	if (kret != KERN_SUCCESS) {
		return EINVAL;
	}

	size = sizeof(struct _processor_statistics_np) * (hinfo.logical_cpu_max + 2); /* One for RT Queue, One for Fair Share Queue */

	if (req->oldlen < size) {
		return EINVAL;
	}

	buf_size = size;
	buf = kheap_alloc(KHEAP_TEMP, buf_size, Z_ZERO | Z_WAITOK);

	kret = get_sched_statistics(buf, &size);
	if (kret != KERN_SUCCESS) {
		error = EINVAL;
		goto out;
	}

	error = sysctl_io_opaque(req, buf, size, &changed);
	if (error) {
		goto out;
	}

	if (changed) {
		panic("Sched info changed?!");
	}
out:
	kheap_free(KHEAP_TEMP, buf, buf_size);
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, sched_stats, CTLFLAG_LOCKED, 0, 0, sysctl_sched_stats, "-", "");

STATIC int
sysctl_sched_stats_enable(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, __unused struct sysctl_req *req)
{
	boolean_t active;
	int res;

	if (req->newlen != sizeof(active)) {
		return EINVAL;
	}

	res = copyin(req->newptr, &active, sizeof(active));
	if (res != 0) {
		return res;
	}

	return set_sched_stats_active(active);
}

SYSCTL_PROC(_kern, OID_AUTO, sched_stats_enable, CTLFLAG_LOCKED | CTLFLAG_WR, 0, 0, sysctl_sched_stats_enable, "-", "");

extern uint32_t sched_debug_flags;
SYSCTL_INT(_debug, OID_AUTO, sched, CTLFLAG_RW | CTLFLAG_LOCKED, &sched_debug_flags, 0, "scheduler debug");

#if (DEBUG || DEVELOPMENT)
extern boolean_t doprnt_hide_pointers;
SYSCTL_INT(_debug, OID_AUTO, hide_kernel_pointers, CTLFLAG_RW | CTLFLAG_LOCKED, &doprnt_hide_pointers, 0, "hide kernel pointers from log");
#endif

extern int get_kernel_symfile(proc_t, char **);

#if COUNT_SYSCALLS
#define KERN_COUNT_SYSCALLS (KERN_OSTYPE + 1000)

extern const unsigned int     nsysent;
extern int syscalls_log[];
extern const char *syscallnames[];

STATIC int
sysctl_docountsyscalls SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;      /* subcommand*/
	__unused int *name = arg1;      /* oid element argument vector */
	__unused int namelen = arg2;    /* number of oid element arguments */
	int error, changed;

	int tmp;

	/* valid values passed in:
	 * = 0 means don't keep called counts for each bsd syscall
	 * > 0 means keep called counts for each bsd syscall
	 * = 2 means dump current counts to the system log
	 * = 3 means reset all counts
	 * for example, to dump current counts:
	 *		sysctl -w kern.count_calls=2
	 */
	error = sysctl_io_number(req, do_count_syscalls,
	    sizeof(do_count_syscalls), &tmp, &changed);

	if (error != 0 || !changed) {
		return error;
	}

	if (tmp == 1) {
		do_count_syscalls = 1;
	} else if (tmp == 0 || tmp == 2 || tmp == 3) {
		for (int i = 0; i < nsysent; i++) {
			if (syscalls_log[i] != 0) {
				if (tmp == 2) {
					printf("%d calls - name %s \n", syscalls_log[i], syscallnames[i]);
				} else {
					syscalls_log[i] = 0;
				}
			}
		}
		do_count_syscalls = (tmp != 0);
	}

	return error;
}
SYSCTL_PROC(_kern, KERN_COUNT_SYSCALLS, count_syscalls, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    0,                          /* Integer argument (arg2) */
    sysctl_docountsyscalls,     /* Handler function */
    NULL,                       /* Data pointer */
    "");
#endif  /* COUNT_SYSCALLS */

/*
 * The following sysctl_* functions should not be used
 * any more, as they can only cope with callers in
 * user mode: Use new-style
 *  sysctl_io_number()
 *  sysctl_io_string()
 *  sysctl_io_opaque()
 * instead.
 */

STATIC int
sysdoproc_filt_KERN_PROC_PID(proc_t p, void * arg)
{
	if (p->p_pid != (pid_t)*(int*)arg) {
		return 0;
	} else {
		return 1;
	}
}

STATIC int
sysdoproc_filt_KERN_PROC_PGRP(proc_t p, void * arg)
{
	if (p->p_pgrpid != (pid_t)*(int*)arg) {
		return 0;
	} else {
		return 1;
	}
}

STATIC int
sysdoproc_filt_KERN_PROC_TTY(proc_t p, void * arg)
{
	int retval;
	struct tty *tp;

	/* This is very racy but list lock is held.. Hmmm. */
	if ((p->p_flag & P_CONTROLT) == 0 ||
	    (p->p_pgrp == NULL) || (p->p_pgrp->pg_session == NULL) ||
	    (tp = SESSION_TP(p->p_pgrp->pg_session)) == TTY_NULL ||
	    tp->t_dev != (dev_t)*(int*)arg) {
		retval = 0;
	} else {
		retval = 1;
	}

	return retval;
}

STATIC int
sysdoproc_filt_KERN_PROC_UID(proc_t p, void * arg)
{
	kauth_cred_t my_cred;
	uid_t uid;

	if (p->p_ucred == NULL) {
		return 0;
	}
	my_cred = kauth_cred_proc_ref(p);
	uid = kauth_cred_getuid(my_cred);
	kauth_cred_unref(&my_cred);

	if (uid != (uid_t)*(int*)arg) {
		return 0;
	} else {
		return 1;
	}
}


STATIC int
sysdoproc_filt_KERN_PROC_RUID(proc_t p, void * arg)
{
	kauth_cred_t my_cred;
	uid_t ruid;

	if (p->p_ucred == NULL) {
		return 0;
	}
	my_cred = kauth_cred_proc_ref(p);
	ruid = kauth_cred_getruid(my_cred);
	kauth_cred_unref(&my_cred);

	if (ruid != (uid_t)*(int*)arg) {
		return 0;
	} else {
		return 1;
	}
}

/*
 * try over estimating by 5 procs
 */
#define KERN_PROCSLOP (5 * sizeof(struct kinfo_proc))
struct sysdoproc_args {
	size_t buflen;
	void *kprocp;
	boolean_t is_64_bit;
	user_addr_t dp;
	size_t needed;
	unsigned int sizeof_kproc;
	int *errorp;
	int uidcheck;
	int ruidcheck;
	int ttycheck;
	int uidval;
};

int
sysdoproc_callback(proc_t p, void *arg)
{
	struct sysdoproc_args *args = arg;

	if (args->buflen >= args->sizeof_kproc) {
		if ((args->ruidcheck != 0) && (sysdoproc_filt_KERN_PROC_RUID(p, &args->uidval) == 0)) {
			return PROC_RETURNED;
		}
		if ((args->uidcheck != 0) && (sysdoproc_filt_KERN_PROC_UID(p, &args->uidval) == 0)) {
			return PROC_RETURNED;
		}
		if ((args->ttycheck != 0) && (sysdoproc_filt_KERN_PROC_TTY(p, &args->uidval) == 0)) {
			return PROC_RETURNED;
		}

		bzero(args->kprocp, args->sizeof_kproc);
		if (args->is_64_bit) {
			fill_user64_proc(p, args->kprocp);
		} else {
			fill_user32_proc(p, args->kprocp);
		}
		int error = copyout(args->kprocp, args->dp, args->sizeof_kproc);
		if (error) {
			*args->errorp = error;
			return PROC_RETURNED_DONE;
		}
		args->dp += args->sizeof_kproc;
		args->buflen -= args->sizeof_kproc;
	}
	args->needed += args->sizeof_kproc;
	return PROC_RETURNED;
}

SYSCTL_NODE(_kern, KERN_PROC, proc, CTLFLAG_RD | CTLFLAG_LOCKED, 0, "");
STATIC int
sysctl_prochandle SYSCTL_HANDLER_ARGS
{
	int cmd = oidp->oid_arg2;       /* subcommand for multiple nodes */
	int *name = arg1;               /* oid element argument vector */
	int namelen = arg2;             /* number of oid element arguments */
	user_addr_t where = req->oldptr;/* user buffer copy out address */

	user_addr_t dp = where;
	size_t needed = 0;
	size_t buflen = where != USER_ADDR_NULL ? req->oldlen : 0;
	int error = 0;
	boolean_t is_64_bit = proc_is64bit(current_proc());
	struct user32_kinfo_proc  user32_kproc;
	struct user64_kinfo_proc  user_kproc;
	int sizeof_kproc;
	void *kprocp;
	int (*filterfn)(proc_t, void *) = 0;
	struct sysdoproc_args args;
	int uidcheck = 0;
	int ruidcheck = 0;
	int ttycheck = 0;

	if (namelen != 1 && !(namelen == 0 && cmd == KERN_PROC_ALL)) {
		return EINVAL;
	}

	if (is_64_bit) {
		sizeof_kproc = sizeof(user_kproc);
		kprocp = &user_kproc;
	} else {
		sizeof_kproc = sizeof(user32_kproc);
		kprocp = &user32_kproc;
	}

	switch (cmd) {
	case KERN_PROC_PID:
		filterfn = sysdoproc_filt_KERN_PROC_PID;
		break;

	case KERN_PROC_PGRP:
		filterfn = sysdoproc_filt_KERN_PROC_PGRP;
		break;

	case KERN_PROC_TTY:
		ttycheck = 1;
		break;

	case KERN_PROC_UID:
		uidcheck = 1;
		break;

	case KERN_PROC_RUID:
		ruidcheck = 1;
		break;

	case KERN_PROC_ALL:
		break;

	default:
		/* must be kern.proc.<unknown> */
		return ENOTSUP;
	}

	error = 0;
	args.buflen = buflen;
	args.kprocp = kprocp;
	args.is_64_bit = is_64_bit;
	args.dp = dp;
	args.needed = needed;
	args.errorp = &error;
	args.uidcheck = uidcheck;
	args.ruidcheck = ruidcheck;
	args.ttycheck = ttycheck;
	args.sizeof_kproc = sizeof_kproc;
	if (namelen) {
		args.uidval = name[0];
	}

	proc_iterate((PROC_ALLPROCLIST | PROC_ZOMBPROCLIST),
	    sysdoproc_callback, &args, filterfn, name);

	if (error) {
		return error;
	}

	dp = args.dp;
	needed = args.needed;

	if (where != USER_ADDR_NULL) {
		req->oldlen = dp - where;
		if (needed > req->oldlen) {
			return ENOMEM;
		}
	} else {
		needed += KERN_PROCSLOP;
		req->oldlen = needed;
	}
	/* adjust index so we return the right required/consumed amount */
	req->oldidx += req->oldlen;
	return 0;
}


/*
 * We specify the subcommand code for multiple nodes as the 'req->arg2' value
 * in the sysctl declaration itself, which comes into the handler function
 * as 'oidp->oid_arg2'.
 *
 * For these particular sysctls, since they have well known OIDs, we could
 * have just obtained it from the '((int *)arg1)[0]' parameter, but that would
 * not demonstrate how to handle multiple sysctls that used OID_AUTO instead
 * of a well known value with a common handler function.  This is desirable,
 * because we want well known values to "go away" at some future date.
 *
 * It should be noted that the value of '((int *)arg1)[1]' is used for many
 * an integer parameter to the subcommand for many of these sysctls; we'd
 * rather have used '((int *)arg1)[0]' for that, or even better, an element
 * in a structure passed in as the the 'newp' argument to sysctlbyname(3),
 * and then use leaf-node permissions enforcement, but that would have
 * necessitated modifying user space code to correspond to the interface
 * change, and we are striving for binary backward compatibility here; even
 * though these are SPI, and not intended for use by user space applications
 * which are not themselves system tools or libraries, some applications
 * have erroneously used them.
 */
SYSCTL_PROC(_kern_proc, KERN_PROC_ALL, all, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    KERN_PROC_ALL,              /* Integer argument (arg2) */
    sysctl_prochandle,          /* Handler function */
    NULL,                       /* Data is size variant on ILP32/LP64 */
    "");
SYSCTL_PROC(_kern_proc, KERN_PROC_PID, pid, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    KERN_PROC_PID,              /* Integer argument (arg2) */
    sysctl_prochandle,          /* Handler function */
    NULL,                       /* Data is size variant on ILP32/LP64 */
    "");
SYSCTL_PROC(_kern_proc, KERN_PROC_TTY, tty, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    KERN_PROC_TTY,              /* Integer argument (arg2) */
    sysctl_prochandle,          /* Handler function */
    NULL,                       /* Data is size variant on ILP32/LP64 */
    "");
SYSCTL_PROC(_kern_proc, KERN_PROC_PGRP, pgrp, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    KERN_PROC_PGRP,             /* Integer argument (arg2) */
    sysctl_prochandle,          /* Handler function */
    NULL,                       /* Data is size variant on ILP32/LP64 */
    "");
SYSCTL_PROC(_kern_proc, KERN_PROC_UID, uid, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    KERN_PROC_UID,              /* Integer argument (arg2) */
    sysctl_prochandle,          /* Handler function */
    NULL,                       /* Data is size variant on ILP32/LP64 */
    "");
SYSCTL_PROC(_kern_proc, KERN_PROC_RUID, ruid, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    KERN_PROC_RUID,             /* Integer argument (arg2) */
    sysctl_prochandle,          /* Handler function */
    NULL,                       /* Data is size variant on ILP32/LP64 */
    "");
SYSCTL_PROC(_kern_proc, KERN_PROC_LCID, lcid, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    KERN_PROC_LCID,             /* Integer argument (arg2) */
    sysctl_prochandle,          /* Handler function */
    NULL,                       /* Data is size variant on ILP32/LP64 */
    "");


/*
 * Fill in non-zero fields of an eproc structure for the specified process.
 */
STATIC void
fill_user32_eproc(proc_t p, struct user32_eproc *__restrict ep)
{
	struct tty *tp;
	struct pgrp *pg;
	struct session *sessp;
	kauth_cred_t my_cred;

	pg = proc_pgrp(p);
	sessp = proc_session(p);

	if (pg != PGRP_NULL) {
		ep->e_pgid = p->p_pgrpid;
		ep->e_jobc = pg->pg_jobc;
		if (sessp != SESSION_NULL && sessp->s_ttyvp) {
			ep->e_flag = EPROC_CTTY;
		}
	}
	ep->e_ppid = p->p_ppid;
	if (p->p_ucred) {
		my_cred = kauth_cred_proc_ref(p);

		/* A fake historical pcred */
		ep->e_pcred.p_ruid = kauth_cred_getruid(my_cred);
		ep->e_pcred.p_svuid = kauth_cred_getsvuid(my_cred);
		ep->e_pcred.p_rgid = kauth_cred_getrgid(my_cred);
		ep->e_pcred.p_svgid = kauth_cred_getsvgid(my_cred);

		/* A fake historical *kauth_cred_t */
		unsigned long refcnt = os_atomic_load(&my_cred->cr_ref, relaxed);
		ep->e_ucred.cr_ref = (uint32_t)MIN(refcnt, UINT32_MAX);
		ep->e_ucred.cr_uid = kauth_cred_getuid(my_cred);
		ep->e_ucred.cr_ngroups = (short)posix_cred_get(my_cred)->cr_ngroups;
		bcopy(posix_cred_get(my_cred)->cr_groups,
		    ep->e_ucred.cr_groups, NGROUPS * sizeof(gid_t));

		kauth_cred_unref(&my_cred);
	}

	if ((p->p_flag & P_CONTROLT) && (sessp != SESSION_NULL) &&
	    (tp = SESSION_TP(sessp))) {
		ep->e_tdev = tp->t_dev;
		ep->e_tpgid = sessp->s_ttypgrpid;
	} else {
		ep->e_tdev = NODEV;
	}

	if (sessp != SESSION_NULL) {
		if (SESS_LEADER(p, sessp)) {
			ep->e_flag |= EPROC_SLEADER;
		}
		session_rele(sessp);
	}
	if (pg != PGRP_NULL) {
		pg_rele(pg);
	}
}

/*
 * Fill in non-zero fields of an LP64 eproc structure for the specified process.
 */
STATIC void
fill_user64_eproc(proc_t p, struct user64_eproc *__restrict ep)
{
	struct tty *tp;
	struct pgrp *pg;
	struct session *sessp;
	kauth_cred_t my_cred;

	pg = proc_pgrp(p);
	sessp = proc_session(p);

	if (pg != PGRP_NULL) {
		ep->e_pgid = p->p_pgrpid;
		ep->e_jobc = pg->pg_jobc;
		if (sessp != SESSION_NULL && sessp->s_ttyvp) {
			ep->e_flag = EPROC_CTTY;
		}
	}
	ep->e_ppid = p->p_ppid;
	if (p->p_ucred) {
		my_cred = kauth_cred_proc_ref(p);

		/* A fake historical pcred */
		ep->e_pcred.p_ruid = kauth_cred_getruid(my_cred);
		ep->e_pcred.p_svuid = kauth_cred_getsvuid(my_cred);
		ep->e_pcred.p_rgid = kauth_cred_getrgid(my_cred);
		ep->e_pcred.p_svgid = kauth_cred_getsvgid(my_cred);

		/* A fake historical *kauth_cred_t */
		unsigned long refcnt = os_atomic_load(&my_cred->cr_ref, relaxed);
		ep->e_ucred.cr_ref = (uint32_t)MIN(refcnt, UINT32_MAX);
		ep->e_ucred.cr_uid = kauth_cred_getuid(my_cred);
		ep->e_ucred.cr_ngroups = (short)posix_cred_get(my_cred)->cr_ngroups;
		bcopy(posix_cred_get(my_cred)->cr_groups,
		    ep->e_ucred.cr_groups, NGROUPS * sizeof(gid_t));

		kauth_cred_unref(&my_cred);
	}

	if ((p->p_flag & P_CONTROLT) && (sessp != SESSION_NULL) &&
	    (tp = SESSION_TP(sessp))) {
		ep->e_tdev = tp->t_dev;
		ep->e_tpgid = sessp->s_ttypgrpid;
	} else {
		ep->e_tdev = NODEV;
	}

	if (sessp != SESSION_NULL) {
		if (SESS_LEADER(p, sessp)) {
			ep->e_flag |= EPROC_SLEADER;
		}
		session_rele(sessp);
	}
	if (pg != PGRP_NULL) {
		pg_rele(pg);
	}
}

/*
 * Fill in an eproc structure for the specified process.
 * bzeroed by our caller, so only set non-zero fields.
 */
STATIC void
fill_user32_externproc(proc_t p, struct user32_extern_proc *__restrict exp)
{
	exp->p_starttime.tv_sec = (user32_time_t)p->p_start.tv_sec;
	exp->p_starttime.tv_usec = p->p_start.tv_usec;
	exp->p_flag = p->p_flag;
	if (p->p_lflag & P_LTRACED) {
		exp->p_flag |= P_TRACED;
	}
	if (p->p_lflag & P_LPPWAIT) {
		exp->p_flag |= P_PPWAIT;
	}
	if (p->p_lflag & P_LEXIT) {
		exp->p_flag |= P_WEXIT;
	}
	exp->p_stat = p->p_stat;
	exp->p_pid = p->p_pid;
	exp->p_oppid = p->p_oppid;
	/* Mach related  */
	exp->p_debugger = p->p_debugger;
	exp->sigwait = p->sigwait;
	/* scheduling */
#ifdef _PROC_HAS_SCHEDINFO_
	exp->p_estcpu = p->p_estcpu;
	exp->p_pctcpu = p->p_pctcpu;
	exp->p_slptime = p->p_slptime;
#endif
	exp->p_realtimer.it_interval.tv_sec =
	    (user32_time_t)p->p_realtimer.it_interval.tv_sec;
	exp->p_realtimer.it_interval.tv_usec =
	    (__int32_t)p->p_realtimer.it_interval.tv_usec;

	exp->p_realtimer.it_value.tv_sec =
	    (user32_time_t)p->p_realtimer.it_value.tv_sec;
	exp->p_realtimer.it_value.tv_usec =
	    (__int32_t)p->p_realtimer.it_value.tv_usec;

	exp->p_rtime.tv_sec = (user32_time_t)p->p_rtime.tv_sec;
	exp->p_rtime.tv_usec = (__int32_t)p->p_rtime.tv_usec;

	exp->p_sigignore = p->p_sigignore;
	exp->p_sigcatch = p->p_sigcatch;
	exp->p_priority = p->p_priority;
	exp->p_nice = p->p_nice;
	bcopy(&p->p_comm, &exp->p_comm, MAXCOMLEN);
	exp->p_xstat = (u_short)MIN(p->p_xstat, USHRT_MAX);
	exp->p_acflag = p->p_acflag;
}

/*
 * Fill in an LP64 version of extern_proc structure for the specified process.
 */
STATIC void
fill_user64_externproc(proc_t p, struct user64_extern_proc *__restrict exp)
{
	exp->p_starttime.tv_sec = p->p_start.tv_sec;
	exp->p_starttime.tv_usec = p->p_start.tv_usec;
	exp->p_flag = p->p_flag;
	if (p->p_lflag & P_LTRACED) {
		exp->p_flag |= P_TRACED;
	}
	if (p->p_lflag & P_LPPWAIT) {
		exp->p_flag |= P_PPWAIT;
	}
	if (p->p_lflag & P_LEXIT) {
		exp->p_flag |= P_WEXIT;
	}
	exp->p_stat = p->p_stat;
	exp->p_pid = p->p_pid;
	exp->p_oppid = p->p_oppid;
	/* Mach related  */
	exp->p_debugger = p->p_debugger;
	exp->sigwait = p->sigwait;
	/* scheduling */
#ifdef _PROC_HAS_SCHEDINFO_
	exp->p_estcpu = p->p_estcpu;
	exp->p_pctcpu = p->p_pctcpu;
	exp->p_slptime = p->p_slptime;
#endif
	exp->p_realtimer.it_interval.tv_sec = p->p_realtimer.it_interval.tv_sec;
	exp->p_realtimer.it_interval.tv_usec = p->p_realtimer.it_interval.tv_usec;

	exp->p_realtimer.it_value.tv_sec = p->p_realtimer.it_value.tv_sec;
	exp->p_realtimer.it_value.tv_usec = p->p_realtimer.it_value.tv_usec;

	exp->p_rtime.tv_sec = p->p_rtime.tv_sec;
	exp->p_rtime.tv_usec = p->p_rtime.tv_usec;

	exp->p_sigignore = p->p_sigignore;
	exp->p_sigcatch = p->p_sigcatch;
	exp->p_priority = p->p_priority;
	exp->p_nice = p->p_nice;
	bcopy(&p->p_comm, &exp->p_comm, MAXCOMLEN);
	exp->p_xstat = (u_short)MIN(p->p_xstat, USHRT_MAX);
	exp->p_acflag = p->p_acflag;
}

STATIC void
fill_user32_proc(proc_t p, struct user32_kinfo_proc *__restrict kp)
{
	/* on a 64 bit kernel, 32 bit users get some truncated information */
	fill_user32_externproc(p, &kp->kp_proc);
	fill_user32_eproc(p, &kp->kp_eproc);
}

STATIC void
fill_user64_proc(proc_t p, struct user64_kinfo_proc *__restrict kp)
{
	fill_user64_externproc(p, &kp->kp_proc);
	fill_user64_eproc(p, &kp->kp_eproc);
}

STATIC int
sysctl_kdebug_ops SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;      /* subcommand*/
	int *name = arg1;               /* oid element argument vector */
	int namelen = arg2;             /* number of oid element arguments */
	user_addr_t oldp = req->oldptr; /* user buffer copy out address */
	size_t *oldlenp = &req->oldlen; /* user buffer copy out size */
//	user_addr_t newp = req->newptr;	/* user buffer copy in address */
//	size_t newlen = req->newlen;	/* user buffer copy in size */

	int ret = 0;

	if (namelen == 0) {
		return ENOTSUP;
	}

	switch (name[0]) {
	case KERN_KDEFLAGS:
	case KERN_KDDFLAGS:
	case KERN_KDENABLE:
	case KERN_KDGETBUF:
	case KERN_KDSETUP:
	case KERN_KDREMOVE:
	case KERN_KDSETREG:
	case KERN_KDGETREG:
	case KERN_KDREADTR:
	case KERN_KDWRITETR:
	case KERN_KDWRITEMAP:
	case KERN_KDTEST:
	case KERN_KDPIDTR:
	case KERN_KDTHRMAP:
	case KERN_KDPIDEX:
	case KERN_KDSETBUF:
	case KERN_KDREADCURTHRMAP:
	case KERN_KDSET_TYPEFILTER:
	case KERN_KDBUFWAIT:
	case KERN_KDCPUMAP:
	case KERN_KDWRITEMAP_V3:
	case KERN_KDWRITETR_V3:
		ret = kdbg_control(name, namelen, oldp, oldlenp);
		break;
	default:
		ret = ENOTSUP;
		break;
	}

	/* adjust index so we return the right required/consumed amount */
	if (!ret) {
		req->oldidx += req->oldlen;
	}

	return ret;
}
SYSCTL_PROC(_kern, KERN_KDEBUG, kdebug, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    0,                          /* Integer argument (arg2) */
    sysctl_kdebug_ops,          /* Handler function */
    NULL,                       /* Data pointer */
    "");


#if defined(XNU_TARGET_OS_OSX)
/*
 * Return the top *sizep bytes of the user stack, or the entire area of the
 * user stack down through the saved exec_path, whichever is smaller.
 */
STATIC int
sysctl_doprocargs SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;      /* subcommand*/
	int *name = arg1;               /* oid element argument vector */
	int namelen = arg2;             /* number of oid element arguments */
	user_addr_t oldp = req->oldptr; /* user buffer copy out address */
	size_t *oldlenp = &req->oldlen; /* user buffer copy out size */
//	user_addr_t newp = req->newptr;	/* user buffer copy in address */
//	size_t newlen = req->newlen;	/* user buffer copy in size */
	int error;

	error =  sysctl_procargsx( name, namelen, oldp, oldlenp, current_proc(), 0);

	/* adjust index so we return the right required/consumed amount */
	if (!error) {
		req->oldidx += req->oldlen;
	}

	return error;
}
SYSCTL_PROC(_kern, KERN_PROCARGS, procargs, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    0,                          /* Integer argument (arg2) */
    sysctl_doprocargs,          /* Handler function */
    NULL,                       /* Data pointer */
    "");
#endif  /* defined(XNU_TARGET_OS_OSX) */

STATIC int
sysctl_doprocargs2 SYSCTL_HANDLER_ARGS
{
	__unused int cmd = oidp->oid_arg2;      /* subcommand*/
	int *name = arg1;               /* oid element argument vector */
	int namelen = arg2;             /* number of oid element arguments */
	user_addr_t oldp = req->oldptr; /* user buffer copy out address */
	size_t *oldlenp = &req->oldlen; /* user buffer copy out size */
//	user_addr_t newp = req->newptr;	/* user buffer copy in address */
//	size_t newlen = req->newlen;	/* user buffer copy in size */
	int error;

	error = sysctl_procargsx( name, namelen, oldp, oldlenp, current_proc(), 1);

	/* adjust index so we return the right required/consumed amount */
	if (!error) {
		req->oldidx += req->oldlen;
	}

	return error;
}
SYSCTL_PROC(_kern, KERN_PROCARGS2, procargs2, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,                          /* Pointer argument (arg1) */
    0,                          /* Integer argument (arg2) */
    sysctl_doprocargs2,         /* Handler function */
    NULL,                       /* Data pointer */
    "");

#define SYSCTL_PROCARGS_READ_ENVVARS_ENTITLEMENT "com.apple.private.read-environment-variables"
STATIC int
sysctl_procargsx(int *name, u_int namelen, user_addr_t where,
    size_t *sizep, proc_t cur_proc, int argc_yes)
{
	assert(sizep != NULL);
	proc_t p = NULL;
	size_t buflen = where != USER_ADDR_NULL ? *sizep : 0;
	int error = 0;
	struct _vm_map *proc_map = NULL;
	struct task * task;
	vm_map_copy_t   tmp = NULL;
	user_addr_t     arg_addr;
	size_t          arg_size;
	caddr_t data;
	size_t argslen = 0;
	size_t size = 0;
	vm_offset_t     copy_start = 0, copy_end;
	vm_offset_t     smallbuffer_start;
	kern_return_t ret;
	int pid;
	kauth_cred_t my_cred;
	uid_t uid;
	int argc = -1;
	size_t argvsize;
	size_t remaining;
	size_t current_arg_index;
	size_t current_arg_len;
	const char * current_arg;
	bool omit_env_vars = true;

	if (namelen < 1) {
		error = EINVAL;
		goto finish;
	}

	if (argc_yes) {
		buflen -= sizeof(int);          /* reserve first word to return argc */
	}
	/* we only care about buflen when where (oldp from sysctl) is not NULL. */
	/* when where (oldp from sysctl) is NULL and sizep (oldlenp from sysctl */
	/* is not NULL then the caller wants us to return the length needed to */
	/* hold the data we would return */
	if (where != USER_ADDR_NULL && (buflen <= 0 || buflen > ARG_MAX)) {
		error = EINVAL;
		goto finish;
	}

	/*
	 *	Lookup process by pid
	 */
	pid = name[0];
	p = proc_find(pid);
	if (p == NULL) {
		error = EINVAL;
		goto finish;
	}

	/* Allow reading environment variables if any of the following are true:
	 * - kernel is DEVELOPMENT || DEBUG
	 * - target process is same as current_proc()
	 * - target process is not cs_restricted
	 * - SIP is off
	 * - caller has an entitlement
	 */

#if DEVELOPMENT || DEBUG
	omit_env_vars = false;
#endif
	if (p == current_proc() ||
	    !cs_restricted(p) ||
#if CONFIG_CSR
	    csr_check(CSR_ALLOW_UNRESTRICTED_DTRACE) == 0 ||
#endif
	    IOTaskHasEntitlement(current_task(), SYSCTL_PROCARGS_READ_ENVVARS_ENTITLEMENT)
	    ) {
		omit_env_vars = false;
	}

	/*
	 *	Copy the top N bytes of the stack.
	 *	On all machines we have so far, the stack grows
	 *	downwards.
	 *
	 *	If the user expects no more than N bytes of
	 *	argument list, use that as a guess for the
	 *	size.
	 */

	if (!p->user_stack) {
		error = EINVAL;
		goto finish;
	}

	/* save off argc before releasing the proc */
	argc = p->p_argc;

	argslen = p->p_argslen;

	/*
	 * When these sysctls were introduced, the first string in the strings
	 * section was just the bare path of the executable.  However, for security
	 * reasons we now prefix this string with executable_path= so it can be
	 * parsed getenv style.  To avoid binary compatability issues with exising
	 * callers of this sysctl, we strip it off here.
	 * (rdar://problem/13746466)
	 */
#define        EXECUTABLE_KEY "executable_path="
	argslen -= strlen(EXECUTABLE_KEY);

	if (where == USER_ADDR_NULL && !omit_env_vars) {
		/* caller only wants to know length of proc args data.
		 * If we don't need to omit environment variables, we can skip
		 * copying the target process stack */
		goto calculate_size;
	}

	my_cred = kauth_cred_proc_ref(p);
	uid = kauth_cred_getuid(my_cred);
	kauth_cred_unref(&my_cred);

	if ((uid != kauth_cred_getuid(kauth_cred_get()))
	    && suser(kauth_cred_get(), &cur_proc->p_acflag)) {
		error = EINVAL;
		goto finish;
	}

	arg_size = round_page(argslen);

	arg_addr = p->user_stack - arg_size;

	/*
	 *	Before we can block (any VM code), make another
	 *	reference to the map to keep it alive.  We do
	 *	that by getting a reference on the task itself.
	 */
	task = p->task;
	if (task == NULL) {
		error = EINVAL;
		goto finish;
	}

	/*
	 * Once we have a task reference we can convert that into a
	 * map reference, which we will use in the calls below.  The
	 * task/process may change its map after we take this reference
	 * (see execve), but the worst that will happen then is a return
	 * of stale info (which is always a possibility).
	 */
	task_reference(task);
	proc_rele(p);
	p = NULL;
	proc_map = get_task_map_reference(task);
	task_deallocate(task);

	if (proc_map == NULL) {
		error = EINVAL;
		goto finish;
	}

	ret = kmem_alloc(kernel_map, &copy_start, arg_size, VM_KERN_MEMORY_BSD);
	if (ret != KERN_SUCCESS) {
		error = ENOMEM;
		goto finish;
	}
	bzero((void *)copy_start, arg_size);

	/* End of buffer should be page aligned */
	assert(copy_start + arg_size == round_page(copy_start + arg_size));
	copy_end = copy_start + arg_size;

	if (vm_map_copyin(proc_map, (vm_map_address_t)arg_addr,
	    (vm_map_size_t)arg_size, FALSE, &tmp) != KERN_SUCCESS) {
		error = EIO;
		goto finish;
	}

	/*
	 *	Now that we've done the copyin from the process'
	 *	map, we can release the reference to it.
	 */
	vm_map_deallocate(proc_map);
	proc_map = NULL;

	if (vm_map_copy_overwrite(kernel_map,
	    (vm_map_address_t)copy_start,
	    tmp, (vm_map_size_t) arg_size, FALSE) != KERN_SUCCESS) {
		error = EIO;
		goto finish;
	}
	/* tmp was consumed */
	tmp = NULL;

	if (omit_env_vars) {
		argvsize = 0;

		/* Iterate over everything in argv, plus one for the bare executable path */
		for (current_arg_index = 0; current_arg_index < argc + 1 && argvsize < argslen; ++current_arg_index) {
			current_arg = (const char *)(copy_end - argslen) + argvsize;
			remaining = argslen - argvsize;
			current_arg_len = strnlen(current_arg, remaining);
			if (current_arg_len < remaining) {
				/* We have space for the null terminator */
				current_arg_len += 1;

				if (current_arg_index == 0) {
					/* The bare executable path may have multiple null bytes after it for alignment */
					while (current_arg_len < remaining && current_arg[current_arg_len] == 0) {
						current_arg_len += 1;
					}
				}
			}
			argvsize += current_arg_len;
		}
		assert(argvsize <= argslen);

		/* Adjust argslen and copy_end to make the copyout range extend to the end of argv */
		copy_end = copy_end - argslen + argvsize;
		argslen = argvsize;
	}

	if (where == USER_ADDR_NULL) {
		/* Skip copyout */
		goto calculate_size;
	}

	if (buflen >= argslen) {
		data = (caddr_t) (copy_end - argslen);
		size = argslen;
	} else {
		/*
		 * Before rdar://25397314, this function contained incorrect logic when buflen is less
		 * than argslen. The problem was that it copied in `buflen` bytes from the end of the target
		 * process user stack into the beginning of a buffer of size round_page(buflen), and then
		 * copied out `buflen` bytes from the end of this buffer. The effect of this was that
		 * the caller of this sysctl would get zeros at the end of their buffer.
		 *
		 * To preserve this behavior, bzero everything from copy_end-round_page(buflen)+buflen to the
		 * end of the buffer. This emulates copying in only `buflen` bytes.
		 *
		 *
		 * In the old code:
		 *
		 *   copy_start     .... size: round_page(buflen) ....        copy_end
		 *      [---copied in data (size: buflen)---|--- zeros ----------]
		 *                           ^
		 *                          data = copy_end - buflen
		 *
		 *
		 * In the new code:
		 *   copy_start        .... size: round_page(p->argslen) ....                full copy_end
		 *      ^         ....................... p->argslen ...............................^
		 *      ^         ^                                         truncated copy_end      ^
		 *      ^         ^                                                 ^               ^
		 *      ^         ................  argslen  ........................               ^
		 *      ^         ^                                                 ^               ^
		 *      [-------copied in data (size: round_page(p->argslen))-------:----env vars---]
		 *                                ^            ^
		 *                                ^         data = copy_end - buflen
		 *                smallbuffer_start = max(copy_end - round_page(buflen), copy_start)
		 *
		 *
		 * Full copy_end: copy_end calculated from copy_start + round_page(p->argslen)
		 * Truncated copy_end: copy_end after truncation to remove environment variables.
		 *
		 * If environment variables were omitted, then we use the truncated copy_end, otherwise
		 * we use full copy_end.
		 *
		 * smallbuffer_start: represents where copy_start would be in the old code.
		 * data: The beginning of the region we copyout
		 */
		smallbuffer_start = copy_end - round_page(buflen);
		if (smallbuffer_start < copy_start) {
			smallbuffer_start = copy_start;
		}
		bzero((void *)(smallbuffer_start + buflen), copy_end - (smallbuffer_start + buflen));
		data = (caddr_t) (copy_end - buflen);
		size = buflen;
	}

	if (argc_yes) {
		/* Put processes argc as the first word in the copyout buffer */
		suword(where, argc);
		error = copyout(data, (where + sizeof(int)), size);
		size += sizeof(int);
	} else {
		error = copyout(data, where, size);

		/*
		 * Make the old PROCARGS work to return the executable's path
		 * But, only if there is enough space in the provided buffer
		 *
		 * on entry: data [possibily] points to the beginning of the path
		 *
		 * Note: we keep all pointers&sizes aligned to word boundries
		 */
		if ((!error) && (buflen > 0 && (u_int)buflen > size)) {
			int binPath_sz, alignedBinPath_sz = 0;
			int extraSpaceNeeded, addThis;
			user_addr_t placeHere;
			char * str = (char *) data;
			size_t max_len = size;

			/* Some apps are really bad about messing up their stacks
			 *  So, we have to be extra careful about getting the length
			 *  of the executing binary.  If we encounter an error, we bail.
			 */

			/* Limit ourselves to PATH_MAX paths */
			if (max_len > PATH_MAX) {
				max_len = PATH_MAX;
			}

			binPath_sz = 0;

			while ((binPath_sz < max_len - 1) && (*str++ != 0)) {
				binPath_sz++;
			}

			/* If we have a NUL terminator, copy it, too */
			if (binPath_sz < max_len - 1) {
				binPath_sz += 1;
			}

			/* Pre-Flight the space requiremnts */

			/* Account for the padding that fills out binPath to the next word */
			alignedBinPath_sz += (binPath_sz & (sizeof(int) - 1)) ? (sizeof(int) - (binPath_sz & (sizeof(int) - 1))) : 0;

			placeHere = where + size;

			/* Account for the bytes needed to keep placeHere word aligned */
			addThis = (placeHere & (sizeof(int) - 1)) ? (sizeof(int) - (placeHere & (sizeof(int) - 1))) : 0;

			/* Add up all the space that is needed */
			extraSpaceNeeded = alignedBinPath_sz + addThis + binPath_sz + (4 * sizeof(int));

			/* is there is room to tack on argv[0]? */
			if ((buflen & ~(sizeof(int) - 1)) >= (size + extraSpaceNeeded)) {
				placeHere += addThis;
				suword(placeHere, 0);
				placeHere += sizeof(int);
				suword(placeHere, 0xBFFF0000);
				placeHere += sizeof(int);
				suword(placeHere, 0);
				placeHere += sizeof(int);
				error = copyout(data, placeHere, binPath_sz);
				if (!error) {
					placeHere += binPath_sz;
					suword(placeHere, 0);
					size += extraSpaceNeeded;
				}
			}
		}
	}

calculate_size:
	/* Size has already been calculated for the where != NULL case */
	if (where == USER_ADDR_NULL) {
		size = argslen;
		if (argc_yes) {
			size += sizeof(int);
		} else {
			/*
			 * old PROCARGS will return the executable's path and plus some
			 * extra space for work alignment and data tags
			 */
			size += PATH_MAX + (6 * sizeof(int));
		}
		size += (size & (sizeof(int) - 1)) ? (sizeof(int) - (size & (sizeof(int) - 1))) : 0;
	}

	*sizep = size;

finish:
	if (p != NULL) {
		proc_rele(p);
	}
	if (tmp != NULL) {
		vm_map_copy_discard(tmp);
	}
	if (proc_map != NULL) {
		vm_map_deallocate(proc_map);
	}
	if (copy_start != (vm_offset_t) 0) {
		kmem_free(kernel_map, copy_start, arg_size);
	}
	return error;
}


/*
 * Max number of concurrent aio requests
 */
STATIC int
sysctl_aiomax
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, aio_max_requests, sizeof(int), &new_value, &changed);
	if (changed) {
		/* make sure the system-wide limit is greater than the per process limit */
		if (new_value >= aio_max_requests_per_process && new_value <= AIO_MAX_REQUESTS) {
			aio_max_requests = new_value;
		} else {
			error = EINVAL;
		}
	}
	return error;
}


/*
 * Max number of concurrent aio requests per process
 */
STATIC int
sysctl_aioprocmax
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, aio_max_requests_per_process, sizeof(int), &new_value, &changed);
	if (changed) {
		/* make sure per process limit is less than the system-wide limit */
		if (new_value <= aio_max_requests && new_value >= AIO_LISTIO_MAX) {
			aio_max_requests_per_process = new_value;
		} else {
			error = EINVAL;
		}
	}
	return error;
}


/*
 * Max number of async IO worker threads
 */
STATIC int
sysctl_aiothreads
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, aio_worker_threads, sizeof(int), &new_value, &changed);
	if (changed) {
		/* we only allow an increase in the number of worker threads */
		if (new_value > aio_worker_threads) {
			_aio_create_worker_threads((new_value - aio_worker_threads));
			aio_worker_threads = new_value;
		} else {
			error = EINVAL;
		}
	}
	return error;
}


/*
 * System-wide limit on the max number of processes
 */
STATIC int
sysctl_maxproc
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, maxproc, sizeof(int), &new_value, &changed);
	if (changed) {
		AUDIT_ARG(value32, new_value);
		/* make sure the system-wide limit is less than the configured hard
		 *  limit set at kernel compilation */
		if (new_value <= hard_maxproc && new_value > 0) {
			maxproc = new_value;
		} else {
			error = EINVAL;
		}
	}
	return error;
}

extern int sched_enable_smt;
STATIC int
sysctl_sched_enable_smt
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, sched_enable_smt, sizeof(int), &new_value, &changed);
	if (error) {
		return error;
	}
	kern_return_t kret = KERN_SUCCESS;
	if (changed) {
		AUDIT_ARG(value32, new_value);
		if (new_value == 0) {
			sched_enable_smt = 0;
			kret = enable_smt_processors(false);
		} else {
			sched_enable_smt = 1;
			kret = enable_smt_processors(true);
		}
	}
	switch (kret) {
	case KERN_SUCCESS:
		error = 0;
		break;
	case KERN_INVALID_ARGUMENT:
		error = EINVAL;
		break;
	case KERN_FAILURE:
		error = EBUSY;
		break;
	default:
		error = ENOENT;
		break;
	}

	return error;
}

SYSCTL_STRING(_kern, KERN_OSTYPE, ostype,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    ostype, 0, "");
SYSCTL_STRING(_kern, KERN_OSRELEASE, osrelease,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    osrelease, 0, "");
SYSCTL_INT(_kern, KERN_OSREV, osrevision,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    (int *)NULL, BSD, "");
SYSCTL_STRING(_kern, KERN_VERSION, version,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    version, 0, "");
SYSCTL_STRING(_kern, OID_AUTO, uuid,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &kernel_uuid_string[0], 0, "");

SYSCTL_STRING(_kern, OID_AUTO, osbuildconfig,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    &osbuild_config[0], 0, "");

STATIC int
sysctl_protoboot(__unused struct sysctl_oid *oidp,
    __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error = -1;
#if CONFIG_LOCKERBOOT
	char protoboot_buff[24];
	size_t protoboot_len = sizeof(protoboot_buff);

	if (vnode_tag(rootvnode) == VT_LOCKERFS) {
		strlcpy(protoboot_buff, kernel_protoboot_mount, protoboot_len);
		error = sysctl_io_string(req, protoboot_buff, protoboot_len, 0, NULL);
	} else {
		error = EFTYPE;
	}

#else
#pragma unused(req)
	error = ENOTSUP;
#endif

	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, protoboot,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_protoboot, "A", "");

#if DEBUG
#ifndef DKPR
#define DKPR 1
#endif
#endif

#if DKPR
int debug_kprint_syscall = 0;
char debug_kprint_syscall_process[MAXCOMLEN + 1];

/* Thread safe: bits and string value are not used to reclaim state */
SYSCTL_INT(_debug, OID_AUTO, kprint_syscall,
    CTLFLAG_RW | CTLFLAG_LOCKED, &debug_kprint_syscall, 0, "kprintf syscall tracing");
SYSCTL_STRING(_debug, OID_AUTO, kprint_syscall_process,
    CTLFLAG_RW | CTLFLAG_LOCKED, debug_kprint_syscall_process, sizeof(debug_kprint_syscall_process),
    "name of process for kprintf syscall tracing");

int
debug_kprint_current_process(const char **namep)
{
	struct proc *p = current_proc();

	if (p == NULL) {
		return 0;
	}

	if (debug_kprint_syscall_process[0]) {
		/* user asked to scope tracing to a particular process name */
		if (0 == strncmp(debug_kprint_syscall_process,
		    p->p_comm, sizeof(debug_kprint_syscall_process))) {
			/* no value in telling the user that we traced what they asked */
			if (namep) {
				*namep = NULL;
			}

			return 1;
		} else {
			return 0;
		}
	}

	/* trace all processes. Tell user what we traced */
	if (namep) {
		*namep = p->p_comm;
	}

	return 1;
}
#endif

/* PR-5293665: need to use a callback function for kern.osversion to set
 * osversion in IORegistry */

STATIC int
sysctl_osversion(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int rval = 0;

	rval = sysctl_handle_string(oidp, arg1, arg2, req);

	if (req->newptr) {
		IORegistrySetOSBuildVersion((char *)arg1);
	}

	return rval;
}

SYSCTL_PROC(_kern, KERN_OSVERSION, osversion,
    CTLFLAG_RW | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    osversion, 256 /* OSVERSIZE*/,
    sysctl_osversion, "A", "");

static bool
_already_set_or_not_launchd(struct sysctl_req *req, char *val)
{
	if (req->newptr != 0) {
		/*
		 * Can only ever be set by launchd, and only once at boot.
		 */
		if (req->p->p_pid != 1 || val[0] != '\0') {
			return true;
		}
	}
	return false;
}

#if XNU_TARGET_OS_OSX
static int
sysctl_system_version_compat
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int oldval = (task_has_system_version_compat_enabled(current_task()));
	int new_value = 0, changed = 0;

	int error = sysctl_io_number(req, oldval, sizeof(int), &new_value, &changed);
	if (changed) {
		task_set_system_version_compat_enabled(current_task(), (new_value));
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, system_version_compat,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    0, 0, sysctl_system_version_compat, "A", "");

char osproductversioncompat[48] = { '\0' };

static int
sysctl_osproductversioncompat(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (_already_set_or_not_launchd(req, osproductversioncompat)) {
		return EPERM;
	}
	return sysctl_handle_string(oidp, arg1, arg2, req);
}


SYSCTL_PROC(_kern, OID_AUTO, osproductversioncompat,
    CTLFLAG_RW | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    osproductversioncompat, sizeof(osproductversioncompat),
    sysctl_osproductversioncompat, "A", "The ProductVersion from SystemVersionCompat.plist");
#endif

char osproductversion[48] = { '\0' };

static int
sysctl_osproductversion(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (_already_set_or_not_launchd(req, osproductversion)) {
		return EPERM;
	}

#if !XNU_TARGET_OS_OSX
	return sysctl_handle_string(oidp, arg1, arg2, req);
#else
	if (task_has_system_version_compat_enabled(current_task()) && (osproductversioncompat[0] != '\0')) {
		return sysctl_handle_string(oidp, osproductversioncompat, arg2, req);
	} else {
		return sysctl_handle_string(oidp, arg1, arg2, req);
	}
#endif
}

#if XNU_TARGET_OS_OSX
static_assert(sizeof(osproductversioncompat) == sizeof(osproductversion),
    "osproductversion size matches osproductversioncompat size");
#endif

SYSCTL_PROC(_kern, OID_AUTO, osproductversion,
    CTLFLAG_RW | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    osproductversion, sizeof(osproductversion),
    sysctl_osproductversion, "A", "The ProductVersion from SystemVersion.plist");

char osreleasetype[48] = { '\0' };

STATIC int
sysctl_osreleasetype(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (_already_set_or_not_launchd(req, osreleasetype)) {
		return EPERM;
	}
	return sysctl_handle_string(oidp, arg1, arg2, req);
}

void reset_osreleasetype(void);

void
reset_osreleasetype(void)
{
	memset(osreleasetype, 0, sizeof(osreleasetype));
}

SYSCTL_PROC(_kern, OID_AUTO, osreleasetype,
    CTLFLAG_RW | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    osreleasetype, sizeof(osreleasetype),
    sysctl_osreleasetype, "A", "The ReleaseType from SystemVersion.plist");

static uint64_t iossupportversion_string[48];

STATIC int
sysctl_iossupportversion(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (req->newptr != 0) {
		/*
		 * Can only ever be set by launchd, and only once at boot.
		 */
		if (req->p->p_pid != 1 || iossupportversion_string[0] != '\0') {
			return EPERM;
		}
	}

	return sysctl_handle_string(oidp, arg1, arg2, req);
}

SYSCTL_PROC(_kern, OID_AUTO, iossupportversion,
    CTLFLAG_RW | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    iossupportversion_string, sizeof(iossupportversion_string),
    sysctl_iossupportversion, "A", "The iOSSupportVersion from SystemVersion.plist");

static uint64_t osvariant_status = 0;

STATIC int
sysctl_osvariant_status(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	if (req->newptr != 0) {
		/*
		 * Can only ever be set by launchd, and only once.
		 * Reset by usrctl() -> reset_osvariant_status() during
		 * userspace reboot, since userspace could reboot into
		 * a different variant.
		 */
		if (req->p->p_pid != 1 || osvariant_status != 0) {
			return EPERM;
		}
	}

	return sysctl_handle_quad(oidp, arg1, arg2, req);
}

SYSCTL_PROC(_kern, OID_AUTO, osvariant_status,
    CTLFLAG_RW | CTLTYPE_QUAD | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    &osvariant_status, sizeof(osvariant_status),
    sysctl_osvariant_status, "Q", "Opaque flags used to cache OS variant information");

void reset_osvariant_status(void);

void
reset_osvariant_status(void)
{
	osvariant_status = 0;
}

extern void commpage_update_dyld_flags(uint64_t);
uint64_t dyld_flags = 0;

STATIC int
sysctl_dyld_flags(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	/*
	 * Can only ever be set by launchd, possibly several times
	 * as dyld may change its mind after a userspace reboot.
	 */
	if (req->newptr != 0 && req->p->p_pid != 1) {
		return EPERM;
	}

	int res = sysctl_handle_quad(oidp, arg1, arg2, req);
	if (req->newptr && res == 0) {
		commpage_update_dyld_flags(dyld_flags);
	}
	return res;
}

SYSCTL_PROC(_kern, OID_AUTO, dyld_flags,
    CTLFLAG_RW | CTLTYPE_QUAD | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    &dyld_flags, sizeof(dyld_flags),
    sysctl_dyld_flags, "Q", "Opaque flags used to cache dyld system-wide configuration");

#if defined(XNU_TARGET_OS_BRIDGE)
char macosproductversion[MACOS_VERS_LEN] = { '\0' };

SYSCTL_STRING(_kern, OID_AUTO, macosproductversion,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &macosproductversion[0], MACOS_VERS_LEN, "The currently running macOS ProductVersion (from SystemVersion.plist on macOS)");

char macosversion[MACOS_VERS_LEN] = { '\0' };

SYSCTL_STRING(_kern, OID_AUTO, macosversion,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &macosversion[0], MACOS_VERS_LEN, "The currently running macOS build version");
#endif

STATIC int
sysctl_sysctl_bootargs
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error;
	char buf[BOOT_LINE_LENGTH];

	strlcpy(buf, PE_boot_args(), BOOT_LINE_LENGTH);
	error = sysctl_io_string(req, buf, BOOT_LINE_LENGTH, 0, NULL);
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, bootargs,
    CTLFLAG_LOCKED | CTLFLAG_RD | CTLFLAG_KERN | CTLTYPE_STRING,
    NULL, 0,
    sysctl_sysctl_bootargs, "A", "bootargs");

STATIC int
sysctl_kernelcacheuuid(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int rval = ENOENT;
	if (kernelcache_uuid_valid) {
		rval = sysctl_handle_string(oidp, arg1, arg2, req);
	}
	return rval;
}

SYSCTL_PROC(_kern, OID_AUTO, kernelcacheuuid,
    CTLFLAG_RD | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    kernelcache_uuid_string, sizeof(kernelcache_uuid_string),
    sysctl_kernelcacheuuid, "A", "");

STATIC int
sysctl_systemfilesetuuid(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int rval = ENOENT;
	if (pageablekc_uuid_valid) {
		rval = sysctl_handle_string(oidp, arg1, arg2, req);
	}
	return rval;
}

SYSCTL_PROC(_kern, OID_AUTO, systemfilesetuuid,
    CTLFLAG_RD | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    pageablekc_uuid_string, sizeof(pageablekc_uuid_string),
    sysctl_systemfilesetuuid, "A", "");

STATIC int
sysctl_auxiliaryfilesetuuid(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int rval = ENOENT;
	if (auxkc_uuid_valid) {
		rval = sysctl_handle_string(oidp, arg1, arg2, req);
	}
	return rval;
}

SYSCTL_PROC(_kern, OID_AUTO, auxiliaryfilesetuuid,
    CTLFLAG_RD | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    auxkc_uuid_string, sizeof(auxkc_uuid_string),
    sysctl_auxiliaryfilesetuuid, "A", "");

STATIC int
sysctl_filesetuuid(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int rval = ENOENT;
	kc_format_t kcformat;
	kernel_mach_header_t *mh;
	void *uuid = NULL;
	unsigned long uuidlen = 0;
	uuid_string_t uuid_str;

	if (!PE_get_primary_kc_format(&kcformat) || kcformat != KCFormatFileset) {
		return rval;
	}

	mh = (kernel_mach_header_t *)PE_get_kc_header(KCKindPrimary);
	uuid = getuuidfromheader(mh, &uuidlen);

	if ((uuid != NULL) && (uuidlen == sizeof(uuid_t))) {
		uuid_unparse_upper(*(uuid_t *)uuid, uuid_str);
		rval = sysctl_io_string(req, (char *)uuid_str, sizeof(uuid_str), 0, NULL);
	}

	return rval;
}

SYSCTL_PROC(_kern, OID_AUTO, filesetuuid,
    CTLFLAG_RD | CTLFLAG_KERN | CTLTYPE_STRING | CTLFLAG_LOCKED,
    NULL, 0,
    sysctl_filesetuuid, "A", "");


SYSCTL_INT(_kern, KERN_MAXFILES, maxfiles,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &maxfiles, 0, "");
SYSCTL_INT(_kern, KERN_ARGMAX, argmax,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    (int *)NULL, ARG_MAX, "");
SYSCTL_INT(_kern, KERN_POSIX1, posix1version,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    (int *)NULL, _POSIX_VERSION, "");
SYSCTL_INT(_kern, KERN_NGROUPS, ngroups,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    (int *)NULL, NGROUPS_MAX, "");
SYSCTL_INT(_kern, KERN_JOB_CONTROL, job_control,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    (int *)NULL, 1, "");
#if 1   /* _POSIX_SAVED_IDS from <unistd.h> */
SYSCTL_INT(_kern, KERN_SAVED_IDS, saved_ids,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    (int *)NULL, 1, "");
#else
SYSCTL_INT(_kern, KERN_SAVED_IDS, saved_ids,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    NULL, 0, "");
#endif
SYSCTL_INT(_kern, OID_AUTO, num_files,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    &nfiles, 0, "");
SYSCTL_COMPAT_INT(_kern, OID_AUTO, num_vnodes,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    &numvnodes, 0, "");
SYSCTL_INT(_kern, OID_AUTO, num_tasks,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    &task_max, 0, "");
SYSCTL_INT(_kern, OID_AUTO, num_threads,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    &thread_max, 0, "");
SYSCTL_INT(_kern, OID_AUTO, num_taskthreads,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    &task_threadmax, 0, "");
SYSCTL_LONG(_kern, OID_AUTO, num_recycledvnodes,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    &num_recycledvnodes, "");
SYSCTL_COMPAT_INT(_kern, OID_AUTO, free_vnodes,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    &freevnodes, 0, "");

STATIC int
sysctl_maxvnodes(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int oldval = desiredvnodes;
	int error = sysctl_io_number(req, desiredvnodes, sizeof(int), &desiredvnodes, NULL);

	if (oldval != desiredvnodes) {
		resize_namecache(desiredvnodes);
	}

	return error;
}

SYSCTL_INT(_kern, OID_AUTO, namecache_disabled,
    CTLFLAG_RW | CTLFLAG_LOCKED,
    &nc_disabled, 0, "");

SYSCTL_PROC(_kern, KERN_MAXVNODES, maxvnodes,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_maxvnodes, "I", "");

SYSCTL_PROC(_kern, KERN_MAXPROC, maxproc,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_maxproc, "I", "");

SYSCTL_PROC(_kern, KERN_AIOMAX, aiomax,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_aiomax, "I", "");

SYSCTL_PROC(_kern, KERN_AIOPROCMAX, aioprocmax,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_aioprocmax, "I", "");

SYSCTL_PROC(_kern, KERN_AIOTHREADS, aiothreads,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_aiothreads, "I", "");

SYSCTL_PROC(_kern, OID_AUTO, sched_enable_smt,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_KERN,
    0, 0, sysctl_sched_enable_smt, "I", "");

extern int sched_allow_NO_SMT_threads;
SYSCTL_INT(_kern, OID_AUTO, sched_allow_NO_SMT_threads,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_allow_NO_SMT_threads, 0, "");

#if (DEVELOPMENT || DEBUG)
extern int smt_sched_bonus_16ths;
SYSCTL_INT(_kern, OID_AUTO, smt_sched_bonus_16ths,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &smt_sched_bonus_16ths, 0, "");

extern int smt_timeshare_enabled;
SYSCTL_INT(_kern, OID_AUTO, sched_smt_timeshare_enable,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &smt_timeshare_enabled, 0, "");

extern int sched_smt_balance;
SYSCTL_INT(_kern, OID_AUTO, sched_smt_balance,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_smt_balance, 0, "");
extern int sched_allow_rt_smt;
SYSCTL_INT(_kern, OID_AUTO, sched_allow_rt_smt,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_allow_rt_smt, 0, "");
extern int sched_avoid_cpu0;
SYSCTL_INT(_kern, OID_AUTO, sched_avoid_cpu0,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_avoid_cpu0, 0, "");
#if __arm__ || __arm64__
extern uint32_t perfcontrol_requested_recommended_cores;
SYSCTL_UINT(_kern, OID_AUTO, sched_recommended_cores,
    CTLFLAG_KERN | CTLFLAG_RD | CTLFLAG_LOCKED,
    &perfcontrol_requested_recommended_cores, 0, "");

/* Scheduler perfcontrol callouts sysctls */
SYSCTL_DECL(_kern_perfcontrol_callout);
SYSCTL_NODE(_kern, OID_AUTO, perfcontrol_callout, CTLFLAG_RW | CTLFLAG_LOCKED, 0,
    "scheduler perfcontrol callouts");

extern int perfcontrol_callout_stats_enabled;
SYSCTL_INT(_kern_perfcontrol_callout, OID_AUTO, stats_enabled,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &perfcontrol_callout_stats_enabled, 0, "");

extern uint64_t perfcontrol_callout_stat_avg(perfcontrol_callout_type_t type,
    perfcontrol_callout_stat_t stat);

/* On-Core Callout */
STATIC int
sysctl_perfcontrol_callout_stat
(__unused struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	perfcontrol_callout_stat_t stat = (perfcontrol_callout_stat_t)arg1;
	perfcontrol_callout_type_t type = (perfcontrol_callout_type_t)arg2;
	return sysctl_io_number(req, (int)perfcontrol_callout_stat_avg(type, stat),
	           sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern_perfcontrol_callout, OID_AUTO, oncore_instr,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *)PERFCONTROL_STAT_INSTRS, PERFCONTROL_CALLOUT_ON_CORE,
    sysctl_perfcontrol_callout_stat, "I", "");
SYSCTL_PROC(_kern_perfcontrol_callout, OID_AUTO, oncore_cycles,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *)PERFCONTROL_STAT_CYCLES, PERFCONTROL_CALLOUT_ON_CORE,
    sysctl_perfcontrol_callout_stat, "I", "");
SYSCTL_PROC(_kern_perfcontrol_callout, OID_AUTO, offcore_instr,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *)PERFCONTROL_STAT_INSTRS, PERFCONTROL_CALLOUT_OFF_CORE,
    sysctl_perfcontrol_callout_stat, "I", "");
SYSCTL_PROC(_kern_perfcontrol_callout, OID_AUTO, offcore_cycles,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *)PERFCONTROL_STAT_CYCLES, PERFCONTROL_CALLOUT_OFF_CORE,
    sysctl_perfcontrol_callout_stat, "I", "");
SYSCTL_PROC(_kern_perfcontrol_callout, OID_AUTO, context_instr,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *)PERFCONTROL_STAT_INSTRS, PERFCONTROL_CALLOUT_CONTEXT,
    sysctl_perfcontrol_callout_stat, "I", "");
SYSCTL_PROC(_kern_perfcontrol_callout, OID_AUTO, context_cycles,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *)PERFCONTROL_STAT_CYCLES, PERFCONTROL_CALLOUT_CONTEXT,
    sysctl_perfcontrol_callout_stat, "I", "");
SYSCTL_PROC(_kern_perfcontrol_callout, OID_AUTO, update_instr,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *)PERFCONTROL_STAT_INSTRS, PERFCONTROL_CALLOUT_STATE_UPDATE,
    sysctl_perfcontrol_callout_stat, "I", "");
SYSCTL_PROC(_kern_perfcontrol_callout, OID_AUTO, update_cycles,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *)PERFCONTROL_STAT_CYCLES, PERFCONTROL_CALLOUT_STATE_UPDATE,
    sysctl_perfcontrol_callout_stat, "I", "");

#if __AMP__
extern int sched_amp_idle_steal;
SYSCTL_INT(_kern, OID_AUTO, sched_amp_idle_steal,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_amp_idle_steal, 0, "");
extern int sched_amp_spill_steal;
SYSCTL_INT(_kern, OID_AUTO, sched_amp_spill_steal,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_amp_spill_steal, 0, "");
extern int sched_amp_spill_count;
SYSCTL_INT(_kern, OID_AUTO, sched_amp_spill_count,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_amp_spill_count, 0, "");
extern int sched_amp_spill_deferred_ipi;
SYSCTL_INT(_kern, OID_AUTO, sched_amp_spill_deferred_ipi,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_amp_spill_deferred_ipi, 0, "");
extern int sched_amp_pcores_preempt_immediate_ipi;
SYSCTL_INT(_kern, OID_AUTO, sched_amp_pcores_preempt_immediate_ipi,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &sched_amp_pcores_preempt_immediate_ipi, 0, "");
#endif /* __AMP__ */
#endif /* __arm__ || __arm64__ */

#if __arm64__
extern int legacy_footprint_entitlement_mode;
SYSCTL_INT(_kern, OID_AUTO, legacy_footprint_entitlement_mode,
    CTLFLAG_KERN | CTLFLAG_RD | CTLFLAG_LOCKED,
    &legacy_footprint_entitlement_mode, 0, "");
#endif /* __arm64__ */

static int
sysctl_kern_sched_rt_n_backup_processors(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int old_value = sched_get_rt_n_backup_processors();
	int error = sysctl_io_number(req, old_value, sizeof(int), &new_value, &changed);
	if (changed) {
		sched_set_rt_n_backup_processors(new_value);
	}

	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, sched_rt_n_backup_processors,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_kern_sched_rt_n_backup_processors, "I", "");

#endif /* (DEVELOPMENT || DEBUG) */

STATIC int
sysctl_securelvl
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, securelevel, sizeof(int), &new_value, &changed);
	if (changed) {
		if (!(new_value < securelevel && req->p->p_pid != 1)) {
			proc_list_lock();
			securelevel = new_value;
			proc_list_unlock();
		} else {
			error = EPERM;
		}
	}
	return error;
}

SYSCTL_PROC(_kern, KERN_SECURELVL, securelevel,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_securelvl, "I", "");


STATIC int
sysctl_domainname
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error, changed;
	char tmpname[MAXHOSTNAMELEN] = {};

	lck_mtx_lock(&domainname_lock);
	strlcpy(tmpname, domainname, sizeof(tmpname));
	lck_mtx_unlock(&domainname_lock);

	error = sysctl_io_string(req, tmpname, sizeof(tmpname), 0, &changed);
	if (!error && changed) {
		lck_mtx_lock(&hostname_lock);
		strlcpy(domainname, tmpname, sizeof(domainname));
		lck_mtx_unlock(&hostname_lock);
	}
	return error;
}

SYSCTL_PROC(_kern, KERN_DOMAINNAME, nisdomainname,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_domainname, "A", "");

SYSCTL_COMPAT_INT(_kern, KERN_HOSTID, hostid,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &hostid, 0, "");

STATIC int
sysctl_hostname
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error, changed;
	char tmpname[MAXHOSTNAMELEN] = {};

	lck_mtx_lock(&hostname_lock);
	strlcpy(tmpname, hostname, sizeof(tmpname));
	lck_mtx_unlock(&hostname_lock);

	error = sysctl_io_string(req, tmpname, sizeof(tmpname), 1, &changed);
	if (!error && changed) {
		lck_mtx_lock(&hostname_lock);
		strlcpy(hostname, tmpname, sizeof(hostname));
		lck_mtx_unlock(&hostname_lock);
	}
	return error;
}

SYSCTL_PROC(_kern, KERN_HOSTNAME, hostname,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_hostname, "A", "");

STATIC int
sysctl_procname
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	/* Original code allowed writing, I'm copying this, although this all makes
	 *  no sense to me. Besides, this sysctl is never used. */
	return sysctl_io_string(req, &req->p->p_name[0], (2 * MAXCOMLEN + 1), 1, NULL);
}

SYSCTL_PROC(_kern, KERN_PROCNAME, procname,
    CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    0, 0, sysctl_procname, "A", "");

SYSCTL_INT(_kern, KERN_SPECULATIVE_READS, speculative_reads_disabled,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &speculative_reads_disabled, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, preheat_max_bytes,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &preheat_max_bytes, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, preheat_min_bytes,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &preheat_min_bytes, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, speculative_prefetch_max,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &speculative_prefetch_max, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, speculative_prefetch_max_iosize,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &speculative_prefetch_max_iosize, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_free_target,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_page_free_target, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_free_min,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_page_free_min, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_free_reserved,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_page_free_reserved, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_speculative_percentage,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_pageout_state.vm_page_speculative_percentage, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_page_speculative_q_age_ms,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_pageout_state.vm_page_speculative_q_age_ms, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_max_delayed_work_limit,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_max_delayed_work_limit, 0, "");

SYSCTL_UINT(_kern, OID_AUTO, vm_max_batch,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_max_batch, 0, "");

SYSCTL_STRING(_kern, OID_AUTO, bootsessionuuid,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    &bootsessionuuid_string, sizeof(bootsessionuuid_string), "");


STATIC int
sysctl_boottime
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	struct timeval tv;
	boottime_timeval(&tv);
	struct proc *p = req->p;

	if (proc_is64bit(p)) {
		struct user64_timeval t = {};
		t.tv_sec = tv.tv_sec;
		t.tv_usec = tv.tv_usec;
		return sysctl_io_opaque(req, &t, sizeof(t), NULL);
	} else {
		struct user32_timeval t = {};
		t.tv_sec = (user32_time_t)tv.tv_sec;
		t.tv_usec = tv.tv_usec;
		return sysctl_io_opaque(req, &t, sizeof(t), NULL);
	}
}

SYSCTL_PROC(_kern, KERN_BOOTTIME, boottime,
    CTLTYPE_STRUCT | CTLFLAG_KERN | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_boottime, "S,timeval", "");

extern const char* IOGetBootUUID(void);

/* non-static: written by imageboot.c */
uuid_string_t fake_bootuuid;

STATIC int
sysctl_bootuuid
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error = ENOENT;

	/* check the first byte to see if the string has been
	 * populated. this is a uuid_STRING_t, this check would
	 * not work with a uuid_t.
	 */
	if (fake_bootuuid[0] != '\0') {
		error = sysctl_io_string(req, fake_bootuuid, 0, 0, NULL);
		goto out;
	}

	const char *uuid_string = IOGetBootUUID();
	if (uuid_string) {
		uuid_t boot_uuid;
		error = uuid_parse(uuid_string, boot_uuid);
		if (!error) {
			error = sysctl_io_string(req, __DECONST(char *, uuid_string), 0, 0, NULL);
		}
	}

out:
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, bootuuid,
    CTLTYPE_STRING | CTLFLAG_KERN | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_bootuuid, "A", "");


extern const char* IOGetApfsPrebootUUID(void);
extern const char *IOGetAssociatedApfsVolgroupUUID(void);

STATIC int
sysctl_apfsprebootuuid
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error = ENOENT;

	const char *uuid_string = IOGetApfsPrebootUUID();
	if (uuid_string) {
		uuid_t apfs_preboot_uuid;
		error = uuid_parse(uuid_string, apfs_preboot_uuid);
		if (!error) {
			error = sysctl_io_string(req, __DECONST(char *, uuid_string), 0, 0, NULL);
		}
	}

	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, apfsprebootuuid,
    CTLTYPE_STRING | CTLFLAG_KERN | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_apfsprebootuuid, "A", "");

STATIC int
sysctl_targetsystemvolgroupuuid
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error = ENOENT;

	const char *uuid_string = IOGetApfsPrebootUUID();
	if (uuid_string) {
		uuid_t apfs_preboot_uuid;
		error = uuid_parse(uuid_string, apfs_preboot_uuid);
		if (!error) {
			error = sysctl_io_string(req, __DECONST(char *, uuid_string), 0, 0, NULL);
		}
	} else {
		/*
		 * In special boot modes, such as kcgen-mode, the
		 * apfs-preboot-uuid property will not be set. Instead, a
		 * different property, associated-volume-group, will be set
		 * which indicates the UUID of the VolumeGroup containing the
		 * system volume into which you will boot.
		 */
		uuid_string = IOGetAssociatedApfsVolgroupUUID();
		if (uuid_string) {
			uuid_t apfs_preboot_uuid;
			error = uuid_parse(uuid_string, apfs_preboot_uuid);
			if (!error) {
				error = sysctl_io_string(req, __DECONST(char *, uuid_string), 0, 0, NULL);
			}
		}
	}

	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, targetsystemvolgroupuuid,
    CTLTYPE_STRING | CTLFLAG_KERN | CTLFLAG_RD | CTLFLAG_MASKED | CTLFLAG_LOCKED,
    0, 0, sysctl_targetsystemvolgroupuuid, "A", "");

STATIC int
sysctl_symfile
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char *str;
	int error = get_kernel_symfile(req->p, &str);
	if (error) {
		return error;
	}
	return sysctl_io_string(req, str, 0, 0, NULL);
}


SYSCTL_PROC(_kern, KERN_SYMFILE, symfile,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_symfile, "A", "");

#if CONFIG_NFS_CLIENT
STATIC int
sysctl_netboot
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, netboot_root(), sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, KERN_NETBOOT, netboot,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_netboot, "I", "");
#endif

#ifdef CONFIG_IMGSRC_ACCESS
/*
 * Legacy--act as if only one layer of nesting is possible.
 */
STATIC int
sysctl_imgsrcdev
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	vfs_context_t ctx = vfs_context_current();
	vnode_t devvp;
	int result;

	if (!vfs_context_issuser(ctx)) {
		return EPERM;
	}

	if (imgsrc_rootvnodes[0] == NULL) {
		return ENOENT;
	}

	result = vnode_getwithref(imgsrc_rootvnodes[0]);
	if (result != 0) {
		return result;
	}

	devvp = vnode_mount(imgsrc_rootvnodes[0])->mnt_devvp;
	result = vnode_getwithref(devvp);
	if (result != 0) {
		goto out;
	}

	result = sysctl_io_number(req, vnode_specrdev(devvp), sizeof(dev_t), NULL, NULL);

	vnode_put(devvp);
out:
	vnode_put(imgsrc_rootvnodes[0]);
	return result;
}

SYSCTL_PROC(_kern, OID_AUTO, imgsrcdev,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_imgsrcdev, "I", "");

STATIC int
sysctl_imgsrcinfo
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int error;
	struct imgsrc_info info[MAX_IMAGEBOOT_NESTING] = {};    /* 2 for now, no problem */
	uint32_t i;
	vnode_t rvp, devvp;

	if (imgsrc_rootvnodes[0] == NULLVP) {
		return ENXIO;
	}

	for (i = 0; i < MAX_IMAGEBOOT_NESTING; i++) {
		/*
		 * Go get the root vnode.
		 */
		rvp = imgsrc_rootvnodes[i];
		if (rvp == NULLVP) {
			break;
		}

		error = vnode_get(rvp);
		if (error != 0) {
			return error;
		}

		/*
		 * For now, no getting at a non-local volume.
		 */
		devvp = vnode_mount(rvp)->mnt_devvp;
		if (devvp == NULL) {
			vnode_put(rvp);
			return EINVAL;
		}

		error = vnode_getwithref(devvp);
		if (error != 0) {
			vnode_put(rvp);
			return error;
		}

		/*
		 * Fill in info.
		 */
		info[i].ii_dev = vnode_specrdev(devvp);
		info[i].ii_flags = 0;
		info[i].ii_height = i;
		bzero(info[i].ii_reserved, sizeof(info[i].ii_reserved));

		vnode_put(devvp);
		vnode_put(rvp);
	}

	return sysctl_io_opaque(req, info, i * sizeof(info[0]), NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, imgsrcinfo,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_imgsrcinfo, "I", "");

#endif /* CONFIG_IMGSRC_ACCESS */


SYSCTL_DECL(_kern_timer);
SYSCTL_NODE(_kern, OID_AUTO, timer, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "timer");


SYSCTL_INT(_kern_timer, OID_AUTO, coalescing_enabled,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &mach_timer_coalescing_enabled, 0, "");

SYSCTL_QUAD(_kern_timer, OID_AUTO, deadline_tracking_bin_1,
    CTLFLAG_RW | CTLFLAG_LOCKED,
    &timer_deadline_tracking_bin_1, "");
SYSCTL_QUAD(_kern_timer, OID_AUTO, deadline_tracking_bin_2,
    CTLFLAG_RW | CTLFLAG_LOCKED,
    &timer_deadline_tracking_bin_2, "");

SYSCTL_DECL(_kern_timer_longterm);
SYSCTL_NODE(_kern_timer, OID_AUTO, longterm, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "longterm");


/* Must match definition in osfmk/kern/timer_call.c */
enum {
	THRESHOLD, QCOUNT,
	ENQUEUES, DEQUEUES, ESCALATES, SCANS, PREEMPTS,
	LATENCY, LATENCY_MIN, LATENCY_MAX, SCAN_LIMIT, SCAN_INTERVAL, PAUSES
};
extern uint64_t timer_sysctl_get(int);
extern int      timer_sysctl_set(int, uint64_t);

STATIC int
sysctl_timer
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int             oid = (int)arg1;
	uint64_t        value = timer_sysctl_get(oid);
	uint64_t        new_value;
	int             error;
	int             changed;

	error = sysctl_io_number(req, value, sizeof(value), &new_value, &changed);
	if (changed) {
		error = timer_sysctl_set(oid, new_value);
	}

	return error;
}

SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, threshold,
    CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED,
    (void *) THRESHOLD, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, scan_limit,
    CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED,
    (void *) SCAN_LIMIT, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, scan_interval,
    CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED,
    (void *) SCAN_INTERVAL, 0, sysctl_timer, "Q", "");

SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, qlen,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) QCOUNT, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, scan_pauses,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) PAUSES, 0, sysctl_timer, "Q", "");

#if  DEBUG
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, enqueues,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) ENQUEUES, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, dequeues,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) DEQUEUES, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, escalates,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) ESCALATES, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, scans,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) SCANS, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, preempts,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) PREEMPTS, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, latency,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) LATENCY, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, latency_min,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) LATENCY_MIN, 0, sysctl_timer, "Q", "");
SYSCTL_PROC(_kern_timer_longterm, OID_AUTO, latency_max,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    (void *) LATENCY_MAX, 0, sysctl_timer, "Q", "");
#endif /* DEBUG */

STATIC int
sysctl_usrstack
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, (int)req->p->user_stack, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, KERN_USRSTACK32, usrstack,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_usrstack, "I", "");

STATIC int
sysctl_usrstack64
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, req->p->user_stack, sizeof(req->p->user_stack), NULL, NULL);
}

SYSCTL_PROC(_kern, KERN_USRSTACK64, usrstack64,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_usrstack64, "Q", "");

#if CONFIG_COREDUMP

SYSCTL_STRING(_kern, KERN_COREFILE, corefile,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    corefilename, sizeof(corefilename), "");

STATIC int
sysctl_coredump
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
#ifdef SECURE_KERNEL
	(void)req;
	return ENOTSUP;
#else
	int new_value, changed;
	int error = sysctl_io_number(req, do_coredump, sizeof(int), &new_value, &changed);
	if (changed) {
		if ((new_value == 0) || (new_value == 1)) {
			do_coredump = new_value;
		} else {
			error = EINVAL;
		}
	}
	return error;
#endif
}

SYSCTL_PROC(_kern, KERN_COREDUMP, coredump,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_coredump, "I", "");

STATIC int
sysctl_suid_coredump
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
#ifdef SECURE_KERNEL
	(void)req;
	return ENOTSUP;
#else
	int new_value, changed;
	int error = sysctl_io_number(req, sugid_coredump, sizeof(int), &new_value, &changed);
	if (changed) {
		if ((new_value == 0) || (new_value == 1)) {
			sugid_coredump = new_value;
		} else {
			error = EINVAL;
		}
	}
	return error;
#endif
}

SYSCTL_PROC(_kern, KERN_SUGID_COREDUMP, sugid_coredump,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_suid_coredump, "I", "");

#endif /* CONFIG_COREDUMP */

STATIC int
sysctl_delayterm
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	struct proc *p = req->p;
	int new_value, changed;
	int error = sysctl_io_number(req, (req->p->p_lflag & P_LDELAYTERM)? 1: 0, sizeof(int), &new_value, &changed);
	if (changed) {
		proc_lock(p);
		if (new_value) {
			req->p->p_lflag |=  P_LDELAYTERM;
		} else {
			req->p->p_lflag &=  ~P_LDELAYTERM;
		}
		proc_unlock(p);
	}
	return error;
}

SYSCTL_PROC(_kern, KERN_PROCDELAYTERM, delayterm,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, sysctl_delayterm, "I", "");


STATIC int
sysctl_rage_vnode
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	struct proc *p = req->p;
	struct  uthread *ut;
	int new_value, old_value, changed;
	int error;

	ut = get_bsdthread_info(current_thread());

	if (ut->uu_flag & UT_RAGE_VNODES) {
		old_value = KERN_RAGE_THREAD;
	} else if (p->p_lflag & P_LRAGE_VNODES) {
		old_value = KERN_RAGE_PROC;
	} else {
		old_value = 0;
	}

	error = sysctl_io_number(req, old_value, sizeof(int), &new_value, &changed);

	if ((error == 0) && (changed != 0)) {
		switch (new_value) {
		case KERN_RAGE_PROC:
			proc_lock(p);
			p->p_lflag |= P_LRAGE_VNODES;
			proc_unlock(p);
			break;
		case KERN_UNRAGE_PROC:
			proc_lock(p);
			p->p_lflag &= ~P_LRAGE_VNODES;
			proc_unlock(p);
			break;

		case KERN_RAGE_THREAD:
			ut->uu_flag |= UT_RAGE_VNODES;
			break;
		case KERN_UNRAGE_THREAD:
			ut = get_bsdthread_info(current_thread());
			ut->uu_flag &= ~UT_RAGE_VNODES;
			break;
		}
	}
	return error;
}

SYSCTL_PROC(_kern, KERN_RAGEVNODE, rage_vnode,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    0, 0, sysctl_rage_vnode, "I", "");

/* XXX until filecoordinationd fixes a bit of inverted logic. */
STATIC int
sysctl_vfsnspace
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int old_value = 0, new_value, changed;

	return sysctl_io_number(req, old_value, sizeof(int), &new_value,
	           &changed);
}

SYSCTL_PROC(_kern, OID_AUTO, vfsnspace,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    0, 0, sysctl_vfsnspace, "I", "");

/* XXX move this interface into libproc and remove this sysctl */
STATIC int
sysctl_setthread_cpupercent
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, old_value;
	int error = 0;
	kern_return_t kret = KERN_SUCCESS;
	uint8_t percent = 0;
	int ms_refill = 0;

	if (!req->newptr) {
		return 0;
	}

	old_value = 0;

	if ((error = sysctl_io_number(req, old_value, sizeof(old_value), &new_value, NULL)) != 0) {
		return error;
	}

	percent = new_value & 0xff;                     /* low 8 bytes for perent */
	ms_refill = (new_value >> 8) & 0xffffff;        /* upper 24bytes represent ms refill value */
	if (percent > 100) {
		return EINVAL;
	}

	/*
	 * If the caller is specifying a percentage of 0, this will unset the CPU limit, if present.
	 */
	if ((kret = thread_set_cpulimit(THREAD_CPULIMIT_BLOCK, percent, ms_refill * (int)NSEC_PER_MSEC)) != 0) {
		return EIO;
	}

	return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, setthread_cpupercent,
    CTLTYPE_INT | CTLFLAG_WR | CTLFLAG_ANYBODY,
    0, 0, sysctl_setthread_cpupercent, "I", "set thread cpu percentage limit");


STATIC int
sysctl_kern_check_openevt
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	struct proc *p = req->p;
	int new_value, old_value, changed;
	int error;

	if (p->p_flag & P_CHECKOPENEVT) {
		old_value = KERN_OPENEVT_PROC;
	} else {
		old_value = 0;
	}

	error = sysctl_io_number(req, old_value, sizeof(int), &new_value, &changed);

	if ((error == 0) && (changed != 0)) {
		switch (new_value) {
		case KERN_OPENEVT_PROC:
			OSBitOrAtomic(P_CHECKOPENEVT, &p->p_flag);
			break;

		case KERN_UNOPENEVT_PROC:
			OSBitAndAtomic(~((uint32_t)P_CHECKOPENEVT), &p->p_flag);
			break;

		default:
			error = EINVAL;
		}
	}
	return error;
}

SYSCTL_PROC(_kern, KERN_CHECKOPENEVT, check_openevt, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED,
    0, 0, sysctl_kern_check_openevt, "I", "set the per-process check-open-evt flag");


#if DEVELOPMENT || DEBUG
STATIC int
sysctl_nx
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
#ifdef SECURE_KERNEL
	(void)req;
	return ENOTSUP;
#else
	int new_value, changed;
	int error;

	error = sysctl_io_number(req, nx_enabled, sizeof(nx_enabled), &new_value, &changed);
	if (error) {
		return error;
	}

	if (changed) {
#if defined(__x86_64__)
		/*
		 * Only allow setting if NX is supported on the chip
		 */
		if (!(cpuid_extfeatures() & CPUID_EXTFEATURE_XD)) {
			return ENOTSUP;
		}
#endif
		nx_enabled = new_value;
	}
	return error;
#endif /* SECURE_KERNEL */
}
#endif

#if DEVELOPMENT || DEBUG
SYSCTL_PROC(_kern, KERN_NX_PROTECTION, nx,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    0, 0, sysctl_nx, "I", "");
#endif

STATIC int
sysctl_loadavg
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	if (proc_is64bit(req->p)) {
		struct user64_loadavg loadinfo64 = {};
		fill_loadavg64(&averunnable, &loadinfo64);
		return sysctl_io_opaque(req, &loadinfo64, sizeof(loadinfo64), NULL);
	} else {
		struct user32_loadavg loadinfo32 = {};
		fill_loadavg32(&averunnable, &loadinfo32);
		return sysctl_io_opaque(req, &loadinfo32, sizeof(loadinfo32), NULL);
	}
}

SYSCTL_PROC(_vm, VM_LOADAVG, loadavg,
    CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_loadavg, "S,loadavg", "");

/*
 * Note:	Thread safe; vm_map_lock protects in  vm_toggle_entry_reuse()
 */
STATIC int
sysctl_vm_toggle_address_reuse(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int old_value = 0, new_value = 0, error = 0;

	if (vm_toggle_entry_reuse( VM_TOGGLE_GETVALUE, &old_value )) {
		return error;
	}
	error = sysctl_io_number(req, old_value, sizeof(int), &new_value, NULL);
	if (!error) {
		return vm_toggle_entry_reuse(new_value, NULL);
	}
	return error;
}

SYSCTL_PROC(_debug, OID_AUTO, toggle_address_reuse, CTLFLAG_ANYBODY | CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_vm_toggle_address_reuse, "I", "");

#ifdef CONFIG_XNUPOST

extern uint32_t xnupost_get_estimated_testdata_size(void);
extern int xnupost_reset_all_tests(void);

STATIC int
sysctl_handle_xnupost_get_tests SYSCTL_HANDLER_ARGS
{
	/* fixup unused arguments warnings */
	__unused int _oa2                  = arg2;
	__unused void * _oa1               = arg1;
	__unused struct sysctl_oid * _oidp = oidp;

	int error          = 0;
	user_addr_t oldp   = 0;
	user_addr_t newp   = 0;
	uint32_t usedbytes = 0;

	oldp = req->oldptr;
	newp = req->newptr;

	if (newp) {
		return ENOTSUP;
	}

	if ((void *)oldp == NULL) {
		/* return estimated size for second call where info can be placed */
		req->oldidx = xnupost_get_estimated_testdata_size();
	} else {
		error       = xnupost_export_testdata((void *)oldp, req->oldlen, &usedbytes);
		req->oldidx = usedbytes;
	}

	return error;
}

SYSCTL_PROC(_debug,
    OID_AUTO,
    xnupost_get_tests,
    CTLFLAG_MASKED | CTLFLAG_ANYBODY | CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0,
    0,
    sysctl_handle_xnupost_get_tests,
    "-",
    "read xnupost test data in kernel");

STATIC int
sysctl_debug_xnupost_ctl SYSCTL_HANDLER_ARGS
{
	/* fixup unused arguments warnings */
	__unused int _oa2                  = arg2;
	__unused void * _oa1               = arg1;
	__unused struct sysctl_oid * _oidp = oidp;

#define ARRCOUNT 4
	/*
	 * INPUT: ACTION,  PARAM1, PARAM2, PARAM3
	 * OUTPUT: RESULTCODE, ADDITIONAL DATA
	 */
	int32_t outval[ARRCOUNT] = {0};
	int32_t input[ARRCOUNT]  = {0};
	int32_t out_size         = sizeof(outval);
	int32_t in_size          = sizeof(input);
	int error                = 0;

	/* if this is NULL call to find out size, send out size info */
	if (!req->newptr) {
		goto out;
	}

	/* pull in provided value from userspace */
	error = SYSCTL_IN(req, &input[0], in_size);
	if (error) {
		return error;
	}

	if (input[0] == XTCTL_RESET_TESTDATA) {
		outval[0] = xnupost_reset_all_tests();
		goto out;
	}

out:
	error = SYSCTL_OUT(req, &outval[0], out_size);
	return error;
}

SYSCTL_PROC(_debug,
    OID_AUTO,
    xnupost_testctl,
    CTLFLAG_MASKED | CTLFLAG_ANYBODY | CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_LOCKED,
    0,
    0,
    sysctl_debug_xnupost_ctl,
    "I",
    "xnupost control for kernel testing");

extern void test_oslog_handleOSLogCtl(int32_t * in, int32_t * out, int32_t arraycount);

STATIC int
sysctl_debug_test_oslog_ctl(__unused struct sysctl_oid * oidp, __unused void * arg1, __unused int arg2, struct sysctl_req * req)
{
#define ARRCOUNT 4
	int32_t outval[ARRCOUNT] = {0};
	int32_t input[ARRCOUNT]  = {0};
	int32_t size_outval      = sizeof(outval);
	int32_t size_inval       = sizeof(input);
	int32_t error;

	/* if this is NULL call to find out size, send out size info */
	if (!req->newptr) {
		error = SYSCTL_OUT(req, &outval[0], size_outval);
		return error;
	}

	/* pull in provided value from userspace */
	error = SYSCTL_IN(req, &input[0], size_inval);
	if (error) {
		return error;
	}

	test_oslog_handleOSLogCtl(input, outval, ARRCOUNT);

	error = SYSCTL_OUT(req, &outval[0], size_outval);

	return error;
}

SYSCTL_PROC(_debug,
    OID_AUTO,
    test_OSLogCtl,
    CTLFLAG_MASKED | CTLFLAG_ANYBODY | CTLTYPE_OPAQUE | CTLFLAG_RW | CTLFLAG_LOCKED,
    0,
    0,
    sysctl_debug_test_oslog_ctl,
    "I",
    "testing oslog in kernel");

#include <mach/task.h>
#include <mach/semaphore.h>

static LCK_GRP_DECLARE(sysctl_debug_test_stackshot_owner_grp, "test-stackshot-owner-grp");
static LCK_MTX_DECLARE(sysctl_debug_test_stackshot_owner_init_mtx,
    &sysctl_debug_test_stackshot_owner_grp);

/* This is a sysctl for testing collection of owner info on a lock in kernel space. A multi-threaded
 * test from userland sets this sysctl in such a way that a thread blocks in kernel mode, and a
 * stackshot is taken to see if the owner of the lock can be identified.
 *
 * We can't return to userland with a kernel lock held, so be sure to unlock before we leave.
 * the semaphores allow us to artificially create cases where the lock is being held and the
 * thread is hanging / taking a long time to do something. */

volatile char      sysctl_debug_test_stackshot_mtx_inited = 0;
semaphore_t        sysctl_debug_test_stackshot_mutex_sem;
lck_mtx_t          sysctl_debug_test_stackshot_owner_lck;

#define SYSCTL_DEBUG_MTX_ACQUIRE_WAIT   1
#define SYSCTL_DEBUG_MTX_ACQUIRE_NOWAIT 2
#define SYSCTL_DEBUG_MTX_SIGNAL         3
#define SYSCTL_DEBUG_MTX_TEARDOWN       4

STATIC int
sysctl_debug_test_stackshot_mutex_owner(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	long long option = -1;
	/* if the user tries to read the sysctl, we tell them what the address of the lock is (to test against stackshot's output) */
	long long mtx_unslid_addr = (long long)VM_KERNEL_UNSLIDE_OR_PERM(&sysctl_debug_test_stackshot_owner_lck);
	int error = sysctl_io_number(req, mtx_unslid_addr, sizeof(long long), (void*)&option, NULL);

	lck_mtx_lock(&sysctl_debug_test_stackshot_owner_init_mtx);
	if (!sysctl_debug_test_stackshot_mtx_inited) {
		lck_mtx_init(&sysctl_debug_test_stackshot_owner_lck,
		    &sysctl_debug_test_stackshot_owner_grp,
		    LCK_ATTR_NULL);
		semaphore_create(kernel_task,
		    &sysctl_debug_test_stackshot_mutex_sem,
		    SYNC_POLICY_FIFO, 0);
		sysctl_debug_test_stackshot_mtx_inited = 1;
	}
	lck_mtx_unlock(&sysctl_debug_test_stackshot_owner_init_mtx);

	if (!error) {
		switch (option) {
		case SYSCTL_DEBUG_MTX_ACQUIRE_NOWAIT:
			lck_mtx_lock(&sysctl_debug_test_stackshot_owner_lck);
			lck_mtx_unlock(&sysctl_debug_test_stackshot_owner_lck);
			break;
		case SYSCTL_DEBUG_MTX_ACQUIRE_WAIT:
			lck_mtx_lock(&sysctl_debug_test_stackshot_owner_lck);
			semaphore_wait(sysctl_debug_test_stackshot_mutex_sem);
			lck_mtx_unlock(&sysctl_debug_test_stackshot_owner_lck);
			break;
		case SYSCTL_DEBUG_MTX_SIGNAL:
			semaphore_signal(sysctl_debug_test_stackshot_mutex_sem);
			break;
		case SYSCTL_DEBUG_MTX_TEARDOWN:
			lck_mtx_lock(&sysctl_debug_test_stackshot_owner_init_mtx);

			lck_mtx_destroy(&sysctl_debug_test_stackshot_owner_lck,
			    &sysctl_debug_test_stackshot_owner_grp);
			semaphore_destroy(kernel_task,
			    sysctl_debug_test_stackshot_mutex_sem);
			sysctl_debug_test_stackshot_mtx_inited = 0;

			lck_mtx_unlock(&sysctl_debug_test_stackshot_owner_init_mtx);
			break;
		case -1:         /* user just wanted to read the value, so do nothing */
			break;
		default:
			error = EINVAL;
			break;
		}
	}
	return error;
}

/* we can't return to userland with a kernel rwlock held, so be sure to unlock before we leave.
 * the semaphores allow us to artificially create cases where the lock is being held and the
 * thread is hanging / taking a long time to do something. */

SYSCTL_PROC(_debug,
    OID_AUTO,
    test_MutexOwnerCtl,
    CTLFLAG_MASKED | CTLFLAG_ANYBODY | CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED,
    0,
    0,
    sysctl_debug_test_stackshot_mutex_owner,
    "-",
    "Testing mutex owner in kernel");

volatile char sysctl_debug_test_stackshot_rwlck_inited = 0;
lck_rw_t      sysctl_debug_test_stackshot_owner_rwlck;
semaphore_t   sysctl_debug_test_stackshot_rwlck_sem;

#define SYSCTL_DEBUG_KRWLCK_RACQUIRE_NOWAIT 1
#define SYSCTL_DEBUG_KRWLCK_RACQUIRE_WAIT   2
#define SYSCTL_DEBUG_KRWLCK_WACQUIRE_NOWAIT 3
#define SYSCTL_DEBUG_KRWLCK_WACQUIRE_WAIT   4
#define SYSCTL_DEBUG_KRWLCK_SIGNAL          5
#define SYSCTL_DEBUG_KRWLCK_TEARDOWN        6

STATIC int
sysctl_debug_test_stackshot_rwlck_owner(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	long long option = -1;
	/* if the user tries to read the sysctl, we tell them what the address of the lock is
	 * (to test against stackshot's output) */
	long long rwlck_unslid_addr = (long long)VM_KERNEL_UNSLIDE_OR_PERM(&sysctl_debug_test_stackshot_owner_rwlck);
	int error = sysctl_io_number(req, rwlck_unslid_addr, sizeof(long long), (void*)&option, NULL);

	lck_mtx_lock(&sysctl_debug_test_stackshot_owner_init_mtx);
	if (!sysctl_debug_test_stackshot_rwlck_inited) {
		lck_rw_init(&sysctl_debug_test_stackshot_owner_rwlck,
		    &sysctl_debug_test_stackshot_owner_grp,
		    LCK_ATTR_NULL);
		semaphore_create(kernel_task,
		    &sysctl_debug_test_stackshot_rwlck_sem,
		    SYNC_POLICY_FIFO,
		    0);
		sysctl_debug_test_stackshot_rwlck_inited = 1;
	}
	lck_mtx_unlock(&sysctl_debug_test_stackshot_owner_init_mtx);

	if (!error) {
		switch (option) {
		case SYSCTL_DEBUG_KRWLCK_RACQUIRE_NOWAIT:
			lck_rw_lock(&sysctl_debug_test_stackshot_owner_rwlck, LCK_RW_TYPE_SHARED);
			lck_rw_unlock(&sysctl_debug_test_stackshot_owner_rwlck, LCK_RW_TYPE_SHARED);
			break;
		case SYSCTL_DEBUG_KRWLCK_RACQUIRE_WAIT:
			lck_rw_lock(&sysctl_debug_test_stackshot_owner_rwlck, LCK_RW_TYPE_SHARED);
			semaphore_wait(sysctl_debug_test_stackshot_rwlck_sem);
			lck_rw_unlock(&sysctl_debug_test_stackshot_owner_rwlck, LCK_RW_TYPE_SHARED);
			break;
		case SYSCTL_DEBUG_KRWLCK_WACQUIRE_NOWAIT:
			lck_rw_lock(&sysctl_debug_test_stackshot_owner_rwlck, LCK_RW_TYPE_EXCLUSIVE);
			lck_rw_unlock(&sysctl_debug_test_stackshot_owner_rwlck, LCK_RW_TYPE_EXCLUSIVE);
			break;
		case SYSCTL_DEBUG_KRWLCK_WACQUIRE_WAIT:
			lck_rw_lock(&sysctl_debug_test_stackshot_owner_rwlck, LCK_RW_TYPE_EXCLUSIVE);
			semaphore_wait(sysctl_debug_test_stackshot_rwlck_sem);
			lck_rw_unlock(&sysctl_debug_test_stackshot_owner_rwlck, LCK_RW_TYPE_EXCLUSIVE);
			break;
		case SYSCTL_DEBUG_KRWLCK_SIGNAL:
			semaphore_signal(sysctl_debug_test_stackshot_rwlck_sem);
			break;
		case SYSCTL_DEBUG_KRWLCK_TEARDOWN:
			lck_mtx_lock(&sysctl_debug_test_stackshot_owner_init_mtx);

			lck_rw_destroy(&sysctl_debug_test_stackshot_owner_rwlck,
			    &sysctl_debug_test_stackshot_owner_grp);
			semaphore_destroy(kernel_task,
			    sysctl_debug_test_stackshot_rwlck_sem);
			sysctl_debug_test_stackshot_rwlck_inited = 0;

			lck_mtx_unlock(&sysctl_debug_test_stackshot_owner_init_mtx);
			break;
		case -1:         /* user just wanted to read the value, so do nothing */
			break;
		default:
			error = EINVAL;
			break;
		}
	}
	return error;
}


SYSCTL_PROC(_debug,
    OID_AUTO,
    test_RWLockOwnerCtl,
    CTLFLAG_MASKED | CTLFLAG_ANYBODY | CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED,
    0,
    0,
    sysctl_debug_test_stackshot_rwlck_owner,
    "-",
    "Testing rwlock owner in kernel");
#endif /* !CONFIG_XNUPOST */

STATIC int
sysctl_swapusage
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int                     error;
	uint64_t                swap_total;
	uint64_t                swap_avail;
	vm_size_t               swap_pagesize;
	boolean_t               swap_encrypted;
	struct xsw_usage        xsu = {};

	error = macx_swapinfo(&swap_total,
	    &swap_avail,
	    &swap_pagesize,
	    &swap_encrypted);
	if (error) {
		return error;
	}

	xsu.xsu_total = swap_total;
	xsu.xsu_avail = swap_avail;
	xsu.xsu_used = swap_total - swap_avail;
	xsu.xsu_pagesize = (u_int32_t)MIN(swap_pagesize, UINT32_MAX);
	xsu.xsu_encrypted = swap_encrypted;
	return sysctl_io_opaque(req, &xsu, sizeof(xsu), NULL);
}



SYSCTL_PROC(_vm, VM_SWAPUSAGE, swapusage,
    CTLTYPE_STRUCT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_swapusage, "S,xsw_usage", "");

#if CONFIG_FREEZE
extern void vm_page_reactivate_all_throttled(void);
extern void memorystatus_disable_freeze(void);

static int
sysctl_freeze_enabled SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error, val = memorystatus_freeze_enabled ? 1 : 0;
	boolean_t disabled;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || !req->newptr) {
		return error;
	}

	if (!VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
		//assert(req->newptr);
		printf("Failed attempt to set vm.freeze_enabled sysctl\n");
		return EINVAL;
	}

	/*
	 * If freeze is being disabled, we need to move dirty pages out from the throttle to the active queue.
	 */
	disabled = (!val && memorystatus_freeze_enabled);

	memorystatus_freeze_enabled = val ? TRUE : FALSE;

	if (disabled) {
		vm_page_reactivate_all_throttled();
		memorystatus_disable_freeze();
	}

	return 0;
}

SYSCTL_PROC(_vm, OID_AUTO, freeze_enabled, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY, &memorystatus_freeze_enabled, 0, sysctl_freeze_enabled, "I", "");
#endif /* CONFIG_FREEZE */

#if DEVELOPMENT || DEBUG
extern int vm_num_swap_files_config;
extern int vm_num_swap_files;
extern lck_mtx_t vm_swap_data_lock;
#define VM_MAX_SWAP_FILE_NUM            100

static int
sysctl_vm_config_num_swap_files SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error = 0, val = vm_num_swap_files_config;

	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || !req->newptr) {
		goto out;
	}

	if (!VM_CONFIG_SWAP_IS_ACTIVE && !VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
		printf("Swap is disabled\n");
		error = EINVAL;
		goto out;
	}

	lck_mtx_lock(&vm_swap_data_lock);

	if (val < vm_num_swap_files) {
		printf("Cannot configure fewer swap files than already exist.\n");
		error = EINVAL;
		lck_mtx_unlock(&vm_swap_data_lock);
		goto out;
	}

	if (val > VM_MAX_SWAP_FILE_NUM) {
		printf("Capping number of swap files to upper bound.\n");
		val = VM_MAX_SWAP_FILE_NUM;
	}

	vm_num_swap_files_config = val;
	lck_mtx_unlock(&vm_swap_data_lock);
out:

	return 0;
}

SYSCTL_PROC(_debug, OID_AUTO, num_swap_files_configured, CTLFLAG_ANYBODY | CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, sysctl_vm_config_num_swap_files, "I", "");
#endif /* DEVELOPMENT || DEBUG */

/* this kernel does NOT implement shared_region_make_private_np() */
SYSCTL_INT(_kern, KERN_SHREG_PRIVATIZABLE, shreg_private,
    CTLFLAG_RD | CTLFLAG_LOCKED,
    (int *)NULL, 0, "");

STATIC int
fetch_process_cputype(
	proc_t cur_proc,
	int *name,
	u_int namelen,
	cpu_type_t *cputype)
{
	proc_t p = PROC_NULL;
	int refheld = 0;
	cpu_type_t ret = 0;
	int error = 0;

	if (namelen == 0) {
		p = cur_proc;
	} else if (namelen == 1) {
		p = proc_find(name[0]);
		if (p == NULL) {
			return EINVAL;
		}
		refheld = 1;
	} else {
		error = EINVAL;
		goto out;
	}

	ret = cpu_type() & ~CPU_ARCH_MASK;
	if (IS_64BIT_PROCESS(p)) {
		ret |= CPU_ARCH_ABI64;
	}

	*cputype = ret;

	if (refheld != 0) {
		proc_rele(p);
	}
out:
	return error;
}


STATIC int
sysctl_sysctl_native(__unused struct sysctl_oid *oidp, void *arg1, int arg2,
    struct sysctl_req *req)
{
	int error;
	cpu_type_t proc_cputype = 0;
	if ((error = fetch_process_cputype(req->p, (int *)arg1, arg2, &proc_cputype)) != 0) {
		return error;
	}
	int res = 1;
	if ((proc_cputype & ~CPU_ARCH_MASK) != (cpu_type() & ~CPU_ARCH_MASK)) {
		res = 0;
	}
	return SYSCTL_OUT(req, &res, sizeof(res));
}
SYSCTL_PROC(_sysctl, OID_AUTO, proc_native, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_sysctl_native, "I", "proc_native");

STATIC int
sysctl_sysctl_cputype(__unused struct sysctl_oid *oidp, void *arg1, int arg2,
    struct sysctl_req *req)
{
	int error;
	cpu_type_t proc_cputype = 0;
	if ((error = fetch_process_cputype(req->p, (int *)arg1, arg2, &proc_cputype)) != 0) {
		return error;
	}
	return SYSCTL_OUT(req, &proc_cputype, sizeof(proc_cputype));
}
SYSCTL_PROC(_sysctl, OID_AUTO, proc_cputype, CTLTYPE_NODE | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, sysctl_sysctl_cputype, "I", "proc_cputype");

STATIC int
sysctl_safeboot
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, boothowto & RB_SAFEBOOT ? 1 : 0, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, KERN_SAFEBOOT, safeboot,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_safeboot, "I", "");

STATIC int
sysctl_singleuser
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, boothowto & RB_SINGLE ? 1 : 0, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, singleuser,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_singleuser, "I", "");

STATIC int
sysctl_minimalboot
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_io_number(req, minimalboot, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, minimalboot,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_minimalboot, "I", "");

/*
 * Controls for debugging affinity sets - see osfmk/kern/affinity.c
 */
extern boolean_t        affinity_sets_enabled;
extern int              affinity_sets_mapping;

SYSCTL_INT(_kern, OID_AUTO, affinity_sets_enabled,
    CTLFLAG_RW | CTLFLAG_LOCKED, (int *) &affinity_sets_enabled, 0, "hinting enabled");
SYSCTL_INT(_kern, OID_AUTO, affinity_sets_mapping,
    CTLFLAG_RW | CTLFLAG_LOCKED, &affinity_sets_mapping, 0, "mapping policy");

/*
 * Boolean indicating if KASLR is active.
 */
STATIC int
sysctl_slide
(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	uint32_t        slide;

	slide = vm_kernel_slide ? 1 : 0;

	return sysctl_io_number( req, slide, sizeof(int), NULL, NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, slide,
    CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_slide, "I", "");

/* User address of the PFZ */
#if DEBUG || DEVELOPMENT
extern user32_addr_t commpage_text32_location;
extern user64_addr_t commpage_text64_location;

STATIC int
sysctl_pfz_start SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)

#ifdef __LP64__
	return sysctl_io_number(req, commpage_text64_location, sizeof(user64_addr_t), NULL, NULL);
#else
	return sysctl_io_number(req, commpage_text32_location, sizeof(user32_addr_t), NULL, NULL);
#endif
}

SYSCTL_PROC(_kern, OID_AUTO, pfz,
    CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_MASKED,
    0, 0, sysctl_pfz_start, "I", "");
#endif


/*
 * Limit on total memory users can wire.
 *
 * vm_global_user_wire_limit - system wide limit on wired memory from all processes combined.
 *
 * vm_per_task_user_wire_limit - per address space limit on wired memory.  This puts a cap on the process's rlimit value.
 *
 * These values are initialized to reasonable defaults at boot time based on the available physical memory in
 * kmem_init().
 *
 * All values are in bytes.
 */

vm_map_size_t   vm_global_user_wire_limit;
vm_map_size_t   vm_per_task_user_wire_limit;
extern uint64_t max_mem_actual, max_mem;

uint64_t        vm_add_wire_count_over_global_limit;
uint64_t        vm_add_wire_count_over_user_limit;
/*
 * We used to have a global in the kernel called vm_global_no_user_wire_limit which was the inverse
 * of vm_global_user_wire_limit. But maintaining both of those is silly, and vm_global_user_wire_limit is the
 * real limit.
 * This function is for backwards compatibility with userspace
 * since we exposed the old global via a sysctl.
 */
STATIC int
sysctl_global_no_user_wire_amount(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	vm_map_size_t old_value;
	vm_map_size_t new_value;
	int changed;
	int error;
	uint64_t config_memsize = max_mem;
#if defined(XNU_TARGET_OS_OSX)
	config_memsize = max_mem_actual;
#endif /* defined(XNU_TARGET_OS_OSX) */

	old_value = (vm_map_size_t)(config_memsize - vm_global_user_wire_limit);
	error = sysctl_io_number(req, old_value, sizeof(vm_map_size_t), &new_value, &changed);
	if (changed) {
		if ((uint64_t)new_value > config_memsize) {
			error = EINVAL;
		} else {
			vm_global_user_wire_limit = (vm_map_size_t)(config_memsize - new_value);
		}
	}
	return error;
}
/*
 * There needs to be a more automatic/elegant way to do this
 */
#if defined(__ARM__)
SYSCTL_INT(_vm, OID_AUTO, global_user_wire_limit, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_global_user_wire_limit, 0, "");
SYSCTL_INT(_vm, OID_AUTO, user_wire_limit, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_per_task_user_wire_limit, 0, "");
SYSCTL_PROC(_vm, OID_AUTO, global_no_user_wire_amount, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, &sysctl_global_no_user_wire_amount, "I", "");
#else
SYSCTL_QUAD(_vm, OID_AUTO, global_user_wire_limit, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_global_user_wire_limit, "");
SYSCTL_QUAD(_vm, OID_AUTO, user_wire_limit, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_per_task_user_wire_limit, "");
SYSCTL_PROC(_vm, OID_AUTO, global_no_user_wire_amount, CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED, 0, 0, &sysctl_global_no_user_wire_amount, "Q", "");
#endif

/*
 * Relaxed atomic RW of a 64bit value via sysctl.
 */
STATIC int
sysctl_r_64bit_atomic(uint64_t *ptr, struct sysctl_req *req)
{
	uint64_t old_value;
	uint64_t new_value;
	int error;

	old_value = os_atomic_load_wide(ptr, relaxed);
	error = sysctl_io_number(req, old_value, sizeof(vm_map_size_t), &new_value, NULL);
	return error;
}
STATIC int
sysctl_add_wire_count_over_global_limit(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_r_64bit_atomic(&vm_add_wire_count_over_global_limit, req);
}
STATIC int
sysctl_add_wire_count_over_user_limit(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	return sysctl_r_64bit_atomic(&vm_add_wire_count_over_user_limit, req);
}

SYSCTL_PROC(_vm, OID_AUTO, add_wire_count_over_global_limit, CTLTYPE_QUAD | CTLFLAG_RD |  CTLFLAG_LOCKED, 0, 0, &sysctl_add_wire_count_over_global_limit, "Q", "");
SYSCTL_PROC(_vm, OID_AUTO, add_wire_count_over_user_limit, CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, &sysctl_add_wire_count_over_user_limit, "Q", "");

#if DEVELOPMENT || DEBUG
/* These sysctls are used to test the wired limit. */
extern unsigned int    vm_page_wire_count;
extern uint32_t        vm_lopage_free_count;
SYSCTL_INT(_vm, OID_AUTO, page_wire_count, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_page_wire_count, 0, "");
SYSCTL_INT(_vm, OID_AUTO, lopage_free_count, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_lopage_free_count, 0, "");
#endif /* DEVELOPMENT */

extern int vm_map_copy_overwrite_aligned_src_not_internal;
extern int vm_map_copy_overwrite_aligned_src_not_symmetric;
extern int vm_map_copy_overwrite_aligned_src_large;
SYSCTL_INT(_vm, OID_AUTO, vm_copy_src_not_internal, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_map_copy_overwrite_aligned_src_not_internal, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_copy_src_not_symmetric, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_map_copy_overwrite_aligned_src_not_symmetric, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_copy_src_large, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_map_copy_overwrite_aligned_src_large, 0, "");


extern uint32_t vm_page_external_count;

SYSCTL_INT(_vm, OID_AUTO, vm_page_external_count, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_page_external_count, 0, "");

SYSCTL_INT(_vm, OID_AUTO, vm_page_filecache_min, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_pageout_state.vm_page_filecache_min, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_page_xpmapped_min, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_pageout_state.vm_page_xpmapped_min, 0, "");

#if DEVELOPMENT || DEBUG
SYSCTL_INT(_vm, OID_AUTO, vm_page_filecache_min_divisor, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_pageout_state.vm_page_filecache_min_divisor, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_page_xpmapped_min_divisor, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_pageout_state.vm_page_xpmapped_min_divisor, 0, "");
#endif

extern int      vm_compressor_mode;
extern int      vm_compressor_is_active;
extern int      vm_compressor_available;
extern uint32_t vm_ripe_target_age;
extern uint32_t swapout_target_age;
extern int64_t  compressor_bytes_used;
extern int64_t  c_segment_input_bytes;
extern int64_t  c_segment_compressed_bytes;
extern uint32_t compressor_eval_period_in_msecs;
extern uint32_t compressor_sample_min_in_msecs;
extern uint32_t compressor_sample_max_in_msecs;
extern uint32_t compressor_thrashing_threshold_per_10msecs;
extern uint32_t compressor_thrashing_min_per_10msecs;
extern uint32_t vm_compressor_time_thread;

#if DEVELOPMENT || DEBUG
extern uint32_t vm_compressor_minorcompact_threshold_divisor;
extern uint32_t vm_compressor_majorcompact_threshold_divisor;
extern uint32_t vm_compressor_unthrottle_threshold_divisor;
extern uint32_t vm_compressor_catchup_threshold_divisor;

extern uint32_t vm_compressor_minorcompact_threshold_divisor_overridden;
extern uint32_t vm_compressor_majorcompact_threshold_divisor_overridden;
extern uint32_t vm_compressor_unthrottle_threshold_divisor_overridden;
extern uint32_t vm_compressor_catchup_threshold_divisor_overridden;

extern vmct_stats_t vmct_stats;


STATIC int
sysctl_minorcompact_threshold_divisor(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, vm_compressor_minorcompact_threshold_divisor, sizeof(int), &new_value, &changed);

	if (changed) {
		vm_compressor_minorcompact_threshold_divisor = new_value;
		vm_compressor_minorcompact_threshold_divisor_overridden = 1;
	}
	return error;
}

SYSCTL_PROC(_vm, OID_AUTO, compressor_minorcompact_threshold_divisor,
    CTLTYPE_INT | CTLFLAG_LOCKED | CTLFLAG_RW,
    0, 0, sysctl_minorcompact_threshold_divisor, "I", "");


STATIC int
sysctl_majorcompact_threshold_divisor(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, vm_compressor_majorcompact_threshold_divisor, sizeof(int), &new_value, &changed);

	if (changed) {
		vm_compressor_majorcompact_threshold_divisor = new_value;
		vm_compressor_majorcompact_threshold_divisor_overridden = 1;
	}
	return error;
}

SYSCTL_PROC(_vm, OID_AUTO, compressor_majorcompact_threshold_divisor,
    CTLTYPE_INT | CTLFLAG_LOCKED | CTLFLAG_RW,
    0, 0, sysctl_majorcompact_threshold_divisor, "I", "");


STATIC int
sysctl_unthrottle_threshold_divisor(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, vm_compressor_unthrottle_threshold_divisor, sizeof(int), &new_value, &changed);

	if (changed) {
		vm_compressor_unthrottle_threshold_divisor = new_value;
		vm_compressor_unthrottle_threshold_divisor_overridden = 1;
	}
	return error;
}

SYSCTL_PROC(_vm, OID_AUTO, compressor_unthrottle_threshold_divisor,
    CTLTYPE_INT | CTLFLAG_LOCKED | CTLFLAG_RW,
    0, 0, sysctl_unthrottle_threshold_divisor, "I", "");


STATIC int
sysctl_catchup_threshold_divisor(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, vm_compressor_catchup_threshold_divisor, sizeof(int), &new_value, &changed);

	if (changed) {
		vm_compressor_catchup_threshold_divisor = new_value;
		vm_compressor_catchup_threshold_divisor_overridden = 1;
	}
	return error;
}

SYSCTL_PROC(_vm, OID_AUTO, compressor_catchup_threshold_divisor,
    CTLTYPE_INT | CTLFLAG_LOCKED | CTLFLAG_RW,
    0, 0, sysctl_catchup_threshold_divisor, "I", "");
#endif


SYSCTL_QUAD(_vm, OID_AUTO, compressor_input_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &c_segment_input_bytes, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_compressed_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &c_segment_compressed_bytes, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_bytes_used, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_bytes_used, "");

SYSCTL_INT(_vm, OID_AUTO, compressor_mode, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_compressor_mode, 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_is_active, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_compressor_is_active, 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_swapout_target_age, CTLFLAG_RD | CTLFLAG_LOCKED, &swapout_target_age, 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_available, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_compressor_available, 0, "");

extern int min_csegs_per_major_compaction;
SYSCTL_INT(_vm, OID_AUTO, compressor_min_csegs_per_major_compaction, CTLFLAG_RW | CTLFLAG_LOCKED, &min_csegs_per_major_compaction, 0, "");

SYSCTL_INT(_vm, OID_AUTO, vm_ripe_target_age_in_secs, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_ripe_target_age, 0, "");

SYSCTL_INT(_vm, OID_AUTO, compressor_eval_period_in_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &compressor_eval_period_in_msecs, 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_sample_min_in_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &compressor_sample_min_in_msecs, 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_sample_max_in_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &compressor_sample_max_in_msecs, 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_thrashing_threshold_per_10msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &compressor_thrashing_threshold_per_10msecs, 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_thrashing_min_per_10msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &compressor_thrashing_min_per_10msecs, 0, "");

SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapouts_under_30s, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.unripe_under_30s, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapouts_under_60s, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.unripe_under_60s, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapouts_under_300s, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.unripe_under_300s, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapper_reclaim_swapins, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.reclaim_swapins, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapper_defrag_swapins, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.defrag_swapins, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapper_swapout_threshold_exceeded, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.compressor_swap_threshold_exceeded, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapper_swapout_fileq_throttled, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.external_q_throttled, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapper_swapout_free_count_low, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.free_count_below_reserve, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapper_swapout_thrashing_detected, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.thrashing_detected, "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_swapper_swapout_fragmentation_detected, CTLFLAG_RD | CTLFLAG_LOCKED, &vmcs_stats.fragmentation_detected, "");

SYSCTL_STRING(_vm, OID_AUTO, swapfileprefix, CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED, swapfilename, sizeof(swapfilename) - SWAPFILENAME_INDEX_LEN, "");

SYSCTL_INT(_vm, OID_AUTO, compressor_timing_enabled, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_compressor_time_thread, 0, "");

#if DEVELOPMENT || DEBUG
SYSCTL_QUAD(_vm, OID_AUTO, compressor_thread_runtime0, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_runtimes[0], "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_thread_runtime1, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_runtimes[1], "");

SYSCTL_QUAD(_vm, OID_AUTO, compressor_threads_total, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_cthreads_total, "");

SYSCTL_QUAD(_vm, OID_AUTO, compressor_thread_pages0, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_pages[0], "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_thread_pages1, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_pages[1], "");

SYSCTL_QUAD(_vm, OID_AUTO, compressor_thread_iterations0, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_iterations[0], "");
SYSCTL_QUAD(_vm, OID_AUTO, compressor_thread_iterations1, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_iterations[1], "");

SYSCTL_INT(_vm, OID_AUTO, compressor_thread_minpages0, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_minpages[0], 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_thread_minpages1, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_minpages[1], 0, "");

SYSCTL_INT(_vm, OID_AUTO, compressor_thread_maxpages0, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_maxpages[0], 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_thread_maxpages1, CTLFLAG_RD | CTLFLAG_LOCKED, &vmct_stats.vmct_maxpages[1], 0, "");

int vm_compressor_injected_error_count;

SYSCTL_INT(_vm, OID_AUTO, compressor_injected_error_count, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_compressor_injected_error_count, 0, "");

static int
sysctl_compressor_inject_error(__unused struct sysctl_oid *oidp,
    __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int result;
	vm_address_t va = 0;
	int changed;

	result = sysctl_io_number(req, va, sizeof(va), &va, &changed);
	if (result == 0 && changed) {
		result = vm_map_inject_error(current_map(), va);
		if (result == 0) {
			/*
			 * Count the number of errors injected successfully to detect
			 * situations where corruption was caused by improper use of this
			 * sysctl.
			 */
			os_atomic_inc(&vm_compressor_injected_error_count, relaxed);
		}
	}
	return result;
}

SYSCTL_PROC(_vm, OID_AUTO, compressor_inject_error, CTLTYPE_QUAD | CTLFLAG_LOCKED | CTLFLAG_RW,
    0, 0, sysctl_compressor_inject_error, "Q", "flips a bit in a compressed page for the current task");

#endif

SYSCTL_QUAD(_vm, OID_AUTO, lz4_compressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.lz4_compressions, "");
SYSCTL_QUAD(_vm, OID_AUTO, lz4_compression_failures, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.lz4_compression_failures, "");
SYSCTL_QUAD(_vm, OID_AUTO, lz4_compressed_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.lz4_compressed_bytes, "");
SYSCTL_QUAD(_vm, OID_AUTO, lz4_wk_compression_delta, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.lz4_wk_compression_delta, "");
SYSCTL_QUAD(_vm, OID_AUTO, lz4_wk_compression_negative_delta, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.lz4_wk_compression_negative_delta, "");

SYSCTL_QUAD(_vm, OID_AUTO, lz4_decompressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.lz4_decompressions, "");
SYSCTL_QUAD(_vm, OID_AUTO, lz4_decompressed_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.lz4_decompressed_bytes, "");

SYSCTL_QUAD(_vm, OID_AUTO, uc_decompressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.uc_decompressions, "");

SYSCTL_QUAD(_vm, OID_AUTO, wk_compressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_compressions, "");

SYSCTL_QUAD(_vm, OID_AUTO, wk_catime, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_cabstime, "");

SYSCTL_QUAD(_vm, OID_AUTO, wkh_catime, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wkh_cabstime, "");
SYSCTL_QUAD(_vm, OID_AUTO, wkh_compressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wkh_compressions, "");

SYSCTL_QUAD(_vm, OID_AUTO, wks_catime, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wks_cabstime, "");
SYSCTL_QUAD(_vm, OID_AUTO, wks_compressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wks_compressions, "");

SYSCTL_QUAD(_vm, OID_AUTO, wk_compressions_exclusive, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_compressions_exclusive, "");
SYSCTL_QUAD(_vm, OID_AUTO, wk_sv_compressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_sv_compressions, "");
SYSCTL_QUAD(_vm, OID_AUTO, wk_mzv_compressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_mzv_compressions, "");
SYSCTL_QUAD(_vm, OID_AUTO, wk_compression_failures, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_compression_failures, "");
SYSCTL_QUAD(_vm, OID_AUTO, wk_compressed_bytes_exclusive, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_compressed_bytes_exclusive, "");
SYSCTL_QUAD(_vm, OID_AUTO, wk_compressed_bytes_total, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_compressed_bytes_total, "");

SYSCTL_QUAD(_vm, OID_AUTO, wks_compressed_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wks_compressed_bytes, "");
SYSCTL_QUAD(_vm, OID_AUTO, wks_compression_failures, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wks_compression_failures, "");
SYSCTL_QUAD(_vm, OID_AUTO, wks_sv_compressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wks_sv_compressions, "");


SYSCTL_QUAD(_vm, OID_AUTO, wk_decompressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_decompressions, "");

SYSCTL_QUAD(_vm, OID_AUTO, wk_datime, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_dabstime, "");

SYSCTL_QUAD(_vm, OID_AUTO, wkh_datime, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wkh_dabstime, "");
SYSCTL_QUAD(_vm, OID_AUTO, wkh_decompressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wkh_decompressions, "");

SYSCTL_QUAD(_vm, OID_AUTO, wks_datime, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wks_dabstime, "");
SYSCTL_QUAD(_vm, OID_AUTO, wks_decompressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wks_decompressions, "");

SYSCTL_QUAD(_vm, OID_AUTO, wk_decompressed_bytes, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_decompressed_bytes, "");
SYSCTL_QUAD(_vm, OID_AUTO, wk_sv_decompressions, CTLFLAG_RD | CTLFLAG_LOCKED, &compressor_stats.wk_sv_decompressions, "");

SYSCTL_INT(_vm, OID_AUTO, lz4_threshold, CTLFLAG_RW | CTLFLAG_LOCKED, &vmctune.lz4_threshold, 0, "");
SYSCTL_INT(_vm, OID_AUTO, wkdm_reeval_threshold, CTLFLAG_RW | CTLFLAG_LOCKED, &vmctune.wkdm_reeval_threshold, 0, "");
SYSCTL_INT(_vm, OID_AUTO, lz4_max_failure_skips, CTLFLAG_RW | CTLFLAG_LOCKED, &vmctune.lz4_max_failure_skips, 0, "");
SYSCTL_INT(_vm, OID_AUTO, lz4_max_failure_run_length, CTLFLAG_RW | CTLFLAG_LOCKED, &vmctune.lz4_max_failure_run_length, 0, "");
SYSCTL_INT(_vm, OID_AUTO, lz4_max_preselects, CTLFLAG_RW | CTLFLAG_LOCKED, &vmctune.lz4_max_preselects, 0, "");
SYSCTL_INT(_vm, OID_AUTO, lz4_run_preselection_threshold, CTLFLAG_RW | CTLFLAG_LOCKED, &vmctune.lz4_run_preselection_threshold, 0, "");
SYSCTL_INT(_vm, OID_AUTO, lz4_run_continue_bytes, CTLFLAG_RW | CTLFLAG_LOCKED, &vmctune.lz4_run_continue_bytes, 0, "");
SYSCTL_INT(_vm, OID_AUTO, lz4_profitable_bytes, CTLFLAG_RW | CTLFLAG_LOCKED, &vmctune.lz4_profitable_bytes, 0, "");
#if DEVELOPMENT || DEBUG
extern int vm_compressor_current_codec;
extern int vm_compressor_test_seg_wp;
extern boolean_t vm_compressor_force_sw_wkdm;
SYSCTL_INT(_vm, OID_AUTO, compressor_codec, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_compressor_current_codec, 0, "");
SYSCTL_INT(_vm, OID_AUTO, compressor_test_wp, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_compressor_test_seg_wp, 0, "");

SYSCTL_INT(_vm, OID_AUTO, wksw_force, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_compressor_force_sw_wkdm, 0, "");
extern int precompy, wkswhw;

SYSCTL_INT(_vm, OID_AUTO, precompy, CTLFLAG_RW | CTLFLAG_LOCKED, &precompy, 0, "");
SYSCTL_INT(_vm, OID_AUTO, wkswhw, CTLFLAG_RW | CTLFLAG_LOCKED, &wkswhw, 0, "");
extern unsigned int vm_ktrace_enabled;
SYSCTL_INT(_vm, OID_AUTO, vm_ktrace, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_ktrace_enabled, 0, "");
#endif

#if CONFIG_PHANTOM_CACHE
extern uint32_t phantom_cache_thrashing_threshold;
extern uint32_t phantom_cache_eval_period_in_msecs;
extern uint32_t phantom_cache_thrashing_threshold_ssd;


SYSCTL_INT(_vm, OID_AUTO, phantom_cache_eval_period_in_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &phantom_cache_eval_period_in_msecs, 0, "");
SYSCTL_INT(_vm, OID_AUTO, phantom_cache_thrashing_threshold, CTLFLAG_RW | CTLFLAG_LOCKED, &phantom_cache_thrashing_threshold, 0, "");
SYSCTL_INT(_vm, OID_AUTO, phantom_cache_thrashing_threshold_ssd, CTLFLAG_RW | CTLFLAG_LOCKED, &phantom_cache_thrashing_threshold_ssd, 0, "");
#endif

#if CONFIG_BACKGROUND_QUEUE

extern uint32_t vm_page_background_count;
extern uint32_t vm_page_background_target;
extern uint32_t vm_page_background_internal_count;
extern uint32_t vm_page_background_external_count;
extern uint32_t vm_page_background_mode;
extern uint32_t vm_page_background_exclude_external;
extern uint64_t vm_page_background_promoted_count;
extern uint64_t vm_pageout_rejected_bq_internal;
extern uint64_t vm_pageout_rejected_bq_external;

SYSCTL_INT(_vm, OID_AUTO, vm_page_background_mode, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_page_background_mode, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_page_background_exclude_external, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_page_background_exclude_external, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_page_background_target, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_page_background_target, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_page_background_count, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_page_background_count, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_page_background_internal_count, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_page_background_internal_count, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_page_background_external_count, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_page_background_external_count, 0, "");

SYSCTL_QUAD(_vm, OID_AUTO, vm_page_background_promoted_count, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_page_background_promoted_count, "");
SYSCTL_QUAD(_vm, OID_AUTO, vm_pageout_considered_bq_internal, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_pageout_vminfo.vm_pageout_considered_bq_internal, "");
SYSCTL_QUAD(_vm, OID_AUTO, vm_pageout_considered_bq_external, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_pageout_vminfo.vm_pageout_considered_bq_external, "");
SYSCTL_QUAD(_vm, OID_AUTO, vm_pageout_rejected_bq_internal, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_pageout_rejected_bq_internal, "");
SYSCTL_QUAD(_vm, OID_AUTO, vm_pageout_rejected_bq_external, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_pageout_rejected_bq_external, "");

#endif /* CONFIG_BACKGROUND_QUEUE */

extern void vm_update_darkwake_mode(boolean_t);
extern boolean_t vm_darkwake_mode;

STATIC int
sysctl_toggle_darkwake_mode(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value, changed;
	int error = sysctl_io_number(req, vm_darkwake_mode, sizeof(int), &new_value, &changed);

	if (!error && changed) {
		if (new_value != 0 && new_value != 1) {
			printf("Error: Invalid value passed to darkwake sysctl. Acceptable: 0 or 1.\n");
			error = EINVAL;
		} else {
			vm_update_darkwake_mode((boolean_t) new_value);
		}
	}

	return error;
}

SYSCTL_PROC(_vm, OID_AUTO, darkwake_mode,
    CTLTYPE_INT | CTLFLAG_LOCKED | CTLFLAG_RW,
    0, 0, sysctl_toggle_darkwake_mode, "I", "");

#if (DEVELOPMENT || DEBUG)

SYSCTL_UINT(_vm, OID_AUTO, vm_page_creation_throttled_hard,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_page_creation_throttled_hard, 0, "");

SYSCTL_UINT(_vm, OID_AUTO, vm_page_creation_throttled_soft,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &vm_page_creation_throttled_soft, 0, "");

extern uint32_t vm_pageout_memorystatus_fb_factor_nr;
extern uint32_t vm_pageout_memorystatus_fb_factor_dr;
SYSCTL_INT(_vm, OID_AUTO, vm_pageout_memorystatus_fb_factor_nr, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_pageout_memorystatus_fb_factor_nr, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_pageout_memorystatus_fb_factor_dr, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_pageout_memorystatus_fb_factor_dr, 0, "");

extern uint32_t vm_grab_anon_nops;

SYSCTL_INT(_vm, OID_AUTO, vm_grab_anon_overrides, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_pageout_debug.vm_grab_anon_overrides, 0, "");
SYSCTL_INT(_vm, OID_AUTO, vm_grab_anon_nops, CTLFLAG_RW | CTLFLAG_LOCKED, &vm_pageout_debug.vm_grab_anon_nops, 0, "");

extern int vm_page_delayed_work_ctx_needed;
SYSCTL_INT(_vm, OID_AUTO, vm_page_needed_delayed_work_ctx, CTLFLAG_RD | CTLFLAG_LOCKED, &vm_page_delayed_work_ctx_needed, 0, "");

/* log message counters for persistence mode */
SCALABLE_COUNTER_DECLARE(oslog_p_total_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_metadata_saved_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_metadata_dropped_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_error_count);
SCALABLE_COUNTER_DECLARE(oslog_p_saved_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_dropped_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_boot_dropped_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_coprocessor_total_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_coprocessor_dropped_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_unresolved_kc_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_fmt_invalid_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_fmt_max_args_msgcount);
SCALABLE_COUNTER_DECLARE(oslog_p_truncated_msgcount);

/* log message counters for streaming mode */
extern uint32_t oslog_s_total_msgcount;
extern uint32_t oslog_s_metadata_msgcount;
SCALABLE_COUNTER_DECLARE(oslog_s_error_count);
extern uint32_t oslog_s_streamed_msgcount;
extern uint32_t oslog_s_dropped_msgcount;

/* log message counters for msgbuf logging */
extern uint32_t oslog_msgbuf_msgcount;
extern uint32_t oslog_msgbuf_dropped_msgcount;
extern uint32_t oslog_msgbuf_dropped_charcount;

/* log message counters for vaddlog logging */
extern uint32_t vaddlog_msgcount;
extern uint32_t vaddlog_msgcount_dropped;

SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_total_msgcount, oslog_p_total_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_metadata_saved_msgcount, oslog_p_metadata_saved_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_metadata_dropped_msgcount, oslog_p_metadata_dropped_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_error_count, oslog_p_error_count, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_saved_msgcount, oslog_p_saved_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_dropped_msgcount, oslog_p_dropped_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_boot_dropped_msgcount, oslog_p_boot_dropped_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_coprocessor_total_msgcount, oslog_p_coprocessor_total_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_coprocessor_dropped_msgcount, oslog_p_coprocessor_dropped_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_unresolved_kc_msgcount, oslog_p_unresolved_kc_msgcount, "");

SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_fmt_invalid_msgcount, oslog_p_fmt_invalid_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_fmt_max_args_msgcount, oslog_p_fmt_max_args_msgcount, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_p_truncated_msgcount, oslog_p_truncated_msgcount, "");

SYSCTL_UINT(_debug, OID_AUTO, oslog_s_total_msgcount, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &oslog_s_total_msgcount, 0, "");
SYSCTL_UINT(_debug, OID_AUTO, oslog_s_metadata_msgcount, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &oslog_s_metadata_msgcount, 0, "");
SYSCTL_SCALABLE_COUNTER(_debug, oslog_s_error_count, oslog_s_error_count, "");
SYSCTL_UINT(_debug, OID_AUTO, oslog_s_streamed_msgcount, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &oslog_s_streamed_msgcount, 0, "");
SYSCTL_UINT(_debug, OID_AUTO, oslog_s_dropped_msgcount, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &oslog_s_dropped_msgcount, 0, "");

SYSCTL_UINT(_debug, OID_AUTO, oslog_msgbuf_msgcount, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &oslog_msgbuf_msgcount, 0, "");
SYSCTL_UINT(_debug, OID_AUTO, oslog_msgbuf_dropped_msgcount, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &oslog_msgbuf_dropped_msgcount, 0, "");
SYSCTL_UINT(_debug, OID_AUTO, oslog_msgbuf_dropped_charcount, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &oslog_msgbuf_dropped_charcount, 0, "");

SYSCTL_UINT(_debug, OID_AUTO, vaddlog_msgcount, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &vaddlog_msgcount, 0, "");
SYSCTL_UINT(_debug, OID_AUTO, vaddlog_msgcount_dropped, CTLFLAG_ANYBODY | CTLFLAG_RD | CTLFLAG_LOCKED, &vaddlog_msgcount_dropped, 0, "");

#endif /* DEVELOPMENT || DEBUG */

/*
 * Enable tracing of voucher contents
 */
extern uint32_t ipc_voucher_trace_contents;

SYSCTL_INT(_kern, OID_AUTO, ipc_voucher_trace_contents,
    CTLFLAG_RW | CTLFLAG_LOCKED, &ipc_voucher_trace_contents, 0, "Enable tracing voucher contents");

/*
 * Kernel stack size and depth
 */
SYSCTL_INT(_kern, OID_AUTO, stack_size,
    CTLFLAG_RD | CTLFLAG_LOCKED, (int *) &kernel_stack_size, 0, "Kernel stack size");
SYSCTL_INT(_kern, OID_AUTO, stack_depth_max,
    CTLFLAG_RD | CTLFLAG_LOCKED, (int *) &kernel_stack_depth_max, 0, "Max kernel stack depth at interrupt or context switch");

extern unsigned int kern_feature_overrides;
SYSCTL_INT(_kern, OID_AUTO, kern_feature_overrides,
    CTLFLAG_RD | CTLFLAG_LOCKED, &kern_feature_overrides, 0, "Kernel feature override mask");

/*
 * enable back trace for port allocations
 */
extern int ipc_portbt;

SYSCTL_INT(_kern, OID_AUTO, ipc_portbt,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &ipc_portbt, 0, "");

/*
 * Mach message signature validation control and outputs
 */
extern unsigned int ikm_signature_failures;
SYSCTL_INT(_kern, OID_AUTO, ikm_signature_failures,
    CTLFLAG_RD | CTLFLAG_LOCKED, &ikm_signature_failures, 0, "Message signature failure count");
extern unsigned int ikm_signature_failure_id;
SYSCTL_INT(_kern, OID_AUTO, ikm_signature_failure_id,
    CTLFLAG_RD | CTLFLAG_LOCKED, &ikm_signature_failure_id, 0, "Message signature failure count");

#if (DEVELOPMENT || DEBUG)
extern unsigned int ikm_signature_panic_disable;
SYSCTL_INT(_kern, OID_AUTO, ikm_signature_panic_disable,
    CTLFLAG_RW | CTLFLAG_LOCKED, &ikm_signature_panic_disable, 0, "Message signature failure mode");
extern unsigned int ikm_signature_header_failures;
SYSCTL_INT(_kern, OID_AUTO, ikm_signature_header_failures,
    CTLFLAG_RD | CTLFLAG_LOCKED, &ikm_signature_header_failures, 0, "Message header signature failure count");
extern unsigned int ikm_signature_trailer_failures;
SYSCTL_INT(_kern, OID_AUTO, ikm_signature_trailer_failures,
    CTLFLAG_RD | CTLFLAG_LOCKED, &ikm_signature_trailer_failures, 0, "Message trailer signature failure count");
#endif

/*
 * Scheduler sysctls
 */

SYSCTL_STRING(_kern, OID_AUTO, sched,
    CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED,
    sched_string, sizeof(sched_string),
    "Timeshare scheduler implementation");

#if CONFIG_QUIESCE_COUNTER
static int
sysctl_cpu_quiescent_counter_interval SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)

	uint32_t local_min_interval_us = cpu_quiescent_counter_get_min_interval_us();

	int error = sysctl_handle_int(oidp, &local_min_interval_us, 0, req);
	if (error || !req->newptr) {
		return error;
	}

	cpu_quiescent_counter_set_min_interval_us(local_min_interval_us);

	return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, cpu_checkin_interval,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0,
    sysctl_cpu_quiescent_counter_interval, "I",
    "Quiescent CPU checkin interval (microseconds)");
#endif /* CONFIG_QUIESCE_COUNTER */


/*
 * Only support runtime modification on development / debug
 */
#if DEVELOPMENT || DEBUG
extern int precise_user_kernel_time;
SYSCTL_INT(_kern, OID_AUTO, precise_user_kernel_time,
    CTLFLAG_RW | CTLFLAG_LOCKED,
    &precise_user_kernel_time, 0, "Precise accounting of kernel vs. user time");
#endif /* DEVELOPMENT || DEBUG */

/* Parameters related to timer coalescing tuning, to be replaced
 * with a dedicated systemcall in the future.
 */
/* Enable processing pending timers in the context of any other interrupt
 * Coalescing tuning parameters for various thread/task attributes */
STATIC int
sysctl_timer_user_us_kernel_abstime SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp)
	int size = arg2;        /* subcommand*/
	int error;
	int changed = 0;
	uint64_t old_value_ns;
	uint64_t new_value_ns;
	uint64_t value_abstime;
	if (size == sizeof(uint32_t)) {
		value_abstime = *((uint32_t *)arg1);
	} else if (size == sizeof(uint64_t)) {
		value_abstime = *((uint64_t *)arg1);
	} else {
		return ENOTSUP;
	}

	absolutetime_to_nanoseconds(value_abstime, &old_value_ns);
	error = sysctl_io_number(req, old_value_ns, sizeof(old_value_ns), &new_value_ns, &changed);
	if ((error) || (!changed)) {
		return error;
	}

	nanoseconds_to_absolutetime(new_value_ns, &value_abstime);
	if (size == sizeof(uint32_t)) {
		*((uint32_t *)arg1) = (uint32_t)value_abstime;
	} else {
		*((uint64_t *)arg1) = value_abstime;
	}
	return error;
}

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_bg_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_coalesce_bg_shift, 0, "");
SYSCTL_PROC(_kern, OID_AUTO, timer_resort_threshold_ns,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_resort_threshold_abstime,
    sizeof(tcoal_prio_params.timer_resort_threshold_abstime),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");
SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_bg_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_coalesce_bg_abstime_max,
    sizeof(tcoal_prio_params.timer_coalesce_bg_abstime_max),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_kt_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_coalesce_kt_shift, 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_kt_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_coalesce_kt_abstime_max,
    sizeof(tcoal_prio_params.timer_coalesce_kt_abstime_max),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_fp_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_coalesce_fp_shift, 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_fp_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_coalesce_fp_abstime_max,
    sizeof(tcoal_prio_params.timer_coalesce_fp_abstime_max),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_ts_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_coalesce_ts_shift, 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_ts_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.timer_coalesce_ts_abstime_max,
    sizeof(tcoal_prio_params.timer_coalesce_ts_abstime_max),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_tier0_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_scale[0], 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_tier0_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_abstime_max[0],
    sizeof(tcoal_prio_params.latency_qos_abstime_max[0]),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_tier1_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_scale[1], 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_tier1_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_abstime_max[1],
    sizeof(tcoal_prio_params.latency_qos_abstime_max[1]),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_tier2_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_scale[2], 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_tier2_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_abstime_max[2],
    sizeof(tcoal_prio_params.latency_qos_abstime_max[2]),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_tier3_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_scale[3], 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_tier3_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_abstime_max[3],
    sizeof(tcoal_prio_params.latency_qos_abstime_max[3]),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_tier4_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_scale[4], 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_tier4_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_abstime_max[4],
    sizeof(tcoal_prio_params.latency_qos_abstime_max[4]),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

SYSCTL_INT(_kern, OID_AUTO, timer_coalesce_tier5_scale,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_scale[5], 0, "");

SYSCTL_PROC(_kern, OID_AUTO, timer_coalesce_tier5_ns_max,
    CTLTYPE_QUAD | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &tcoal_prio_params.latency_qos_abstime_max[5],
    sizeof(tcoal_prio_params.latency_qos_abstime_max[5]),
    sysctl_timer_user_us_kernel_abstime,
    "Q", "");

/* Communicate the "user idle level" heuristic to the timer layer, and
 * potentially other layers in the future.
 */

static int
timer_user_idle_level(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int new_value = 0, old_value = 0, changed = 0, error;

	old_value = timer_get_user_idle_level();

	error = sysctl_io_number(req, old_value, sizeof(int), &new_value, &changed);

	if (error == 0 && changed) {
		if (timer_set_user_idle_level(new_value) != KERN_SUCCESS) {
			error = ERANGE;
		}
	}

	return error;
}

SYSCTL_PROC(_machdep, OID_AUTO, user_idle_level,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0,
    timer_user_idle_level, "I", "User idle level heuristic, 0-128");

#if HYPERVISOR
SYSCTL_INT(_kern, OID_AUTO, hv_support,
    CTLFLAG_KERN | CTLFLAG_RD | CTLFLAG_LOCKED,
    &hv_support_available, 0, "");

SYSCTL_INT(_kern, OID_AUTO, hv_disable,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &hv_disable, 0, "");
#endif

#if DEVELOPMENT || DEBUG
extern uint64_t driverkit_checkin_timed_out;
SYSCTL_QUAD(_kern, OID_AUTO, driverkit_checkin_timed_out,
    CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &driverkit_checkin_timed_out, "timestamp of dext checkin timeout");
#endif

extern int IOGetVMMPresent(void);

static int
hv_vmm_present SYSCTL_HANDLER_ARGS
{
	__unused struct sysctl_oid *unused_oidp = oidp;
	__unused void *unused_arg1 = arg1;
	__unused int unused_arg2 = arg2;

	int hv_vmm_present = 0;

	hv_vmm_present = IOGetVMMPresent();

	return SYSCTL_OUT(req, &hv_vmm_present, sizeof(hv_vmm_present));
}

SYSCTL_PROC(_kern, OID_AUTO, hv_vmm_present,
    CTLTYPE_INT | CTLFLAG_ANYBODY | CTLFLAG_KERN | CTLFLAG_LOCKED,
    0, 0,
    hv_vmm_present, "I", "");

#if CONFIG_DARKBOOT
STATIC int
sysctl_darkboot SYSCTL_HANDLER_ARGS
{
	int err = 0, value = 0;
#pragma unused(oidp, arg1, arg2, err, value, req)

	/*
	 * Handle the sysctl request.
	 *
	 * If this is a read, the function will set the value to the current darkboot value. Otherwise,
	 * we'll get the request identifier into "value" and then we can honor it.
	 */
	if ((err = sysctl_io_number(req, darkboot, sizeof(int), &value, NULL)) != 0) {
		goto exit;
	}

	/* writing requested, let's process the request */
	if (req->newptr) {
		/* writing is protected by an entitlement */
		if (priv_check_cred(kauth_cred_get(), PRIV_DARKBOOT, 0) != 0) {
			err = EPERM;
			goto exit;
		}

		switch (value) {
		case MEMORY_MAINTENANCE_DARK_BOOT_UNSET:
			/*
			 * If the darkboot sysctl is unset, the NVRAM variable
			 * must be unset too. If that's not the case, it means
			 * someone is doing something crazy and not supported.
			 */
			if (darkboot != 0) {
				int ret = PERemoveNVRAMProperty(MEMORY_MAINTENANCE_DARK_BOOT_NVRAM_NAME);
				if (ret) {
					darkboot = 0;
				} else {
					err = EINVAL;
				}
			}
			break;
		case MEMORY_MAINTENANCE_DARK_BOOT_SET:
			darkboot = 1;
			break;
		case MEMORY_MAINTENANCE_DARK_BOOT_SET_PERSISTENT: {
			/*
			 * Set the NVRAM and update 'darkboot' in case
			 * of success. Otherwise, do not update
			 * 'darkboot' and report the failure.
			 */
			if (PEWriteNVRAMBooleanProperty(MEMORY_MAINTENANCE_DARK_BOOT_NVRAM_NAME, TRUE)) {
				darkboot = 1;
			} else {
				err = EINVAL;
			}

			break;
		}
		default:
			err = EINVAL;
		}
	}

exit:
	return err;
}

SYSCTL_PROC(_kern, OID_AUTO, darkboot,
    CTLFLAG_KERN | CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY,
    0, 0, sysctl_darkboot, "I", "");
#endif /* CONFIG_DARKBOOT */

#if DEVELOPMENT || DEBUG
#include <sys/sysent.h>
/* This should result in a fatal exception, verifying that "sysent" is
 * write-protected.
 */
static int
kern_sysent_write(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	uint64_t new_value = 0, old_value = 0;
	int changed = 0, error;

	error = sysctl_io_number(req, old_value, sizeof(uint64_t), &new_value, &changed);
	if ((error == 0) && changed) {
		volatile uint32_t *wraddr = __DECONST(uint32_t *, &sysent[0]);
		*wraddr = 0;
		printf("sysent[0] write succeeded\n");
	}
	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, sysent_const_check,
    CTLTYPE_QUAD | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0,
    kern_sysent_write, "I", "Attempt sysent[0] write");

#endif

#if DEVELOPMENT || DEBUG
SYSCTL_COMPAT_INT(_kern, OID_AUTO, development, CTLFLAG_RD | CTLFLAG_MASKED | CTLFLAG_KERN, NULL, 1, "");
#else
SYSCTL_COMPAT_INT(_kern, OID_AUTO, development, CTLFLAG_RD | CTLFLAG_MASKED, NULL, 0, "");
#endif


#if DEVELOPMENT || DEBUG

static int
sysctl_panic_test SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int rval = 0;
	char str[32] = "entry prelog postlog postcore";

	rval = sysctl_handle_string(oidp, str, sizeof(str), req);

	if (rval == 0 && req->newptr) {
		if (strncmp("entry", str, strlen("entry")) == 0) {
			panic_with_options(0, NULL, DEBUGGER_OPTION_RECURPANIC_ENTRY, "test recursive panic at entry");
		} else if (strncmp("prelog", str, strlen("prelog")) == 0) {
			panic_with_options(0, NULL, DEBUGGER_OPTION_RECURPANIC_PRELOG, "test recursive panic prior to writing a paniclog");
		} else if (strncmp("postlog", str, strlen("postlog")) == 0) {
			panic_with_options(0, NULL, DEBUGGER_OPTION_RECURPANIC_POSTLOG, "test recursive panic subsequent to paniclog");
		} else if (strncmp("postcore", str, strlen("postcore")) == 0) {
			panic_with_options(0, NULL, DEBUGGER_OPTION_RECURPANIC_POSTCORE, "test recursive panic subsequent to on-device core");
		}
	}

	return rval;
}

static int
sysctl_debugger_test SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int rval = 0;
	char str[32] = "entry prelog postlog postcore";

	rval = sysctl_handle_string(oidp, str, sizeof(str), req);

	if (rval == 0 && req->newptr) {
		if (strncmp("entry", str, strlen("entry")) == 0) {
			DebuggerWithContext(0, NULL, "test recursive panic via debugger at entry", DEBUGGER_OPTION_RECURPANIC_ENTRY);
		} else if (strncmp("prelog", str, strlen("prelog")) == 0) {
			DebuggerWithContext(0, NULL, "test recursive panic via debugger prior to writing a paniclog", DEBUGGER_OPTION_RECURPANIC_PRELOG);
		} else if (strncmp("postlog", str, strlen("postlog")) == 0) {
			DebuggerWithContext(0, NULL, "test recursive panic via debugger subsequent to paniclog", DEBUGGER_OPTION_RECURPANIC_POSTLOG);
		} else if (strncmp("postcore", str, strlen("postcore")) == 0) {
			DebuggerWithContext(0, NULL, "test recursive panic via debugger subsequent to on-device core", DEBUGGER_OPTION_RECURPANIC_POSTCORE);
		}
	}

	return rval;
}

decl_lck_spin_data(, spinlock_panic_test_lock);

__attribute__((noreturn))
static void
spinlock_panic_test_acquire_spinlock(void * arg __unused, wait_result_t wres __unused)
{
	lck_spin_lock(&spinlock_panic_test_lock);
	while (1) {
		;
	}
}

static int
sysctl_spinlock_panic_test SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (req->newlen == 0) {
		return EINVAL;
	}

	thread_t panic_spinlock_thread;
	/* Initialize panic spinlock */
	lck_grp_t * panic_spinlock_grp;
	lck_grp_attr_t * panic_spinlock_grp_attr;
	lck_attr_t * panic_spinlock_attr;

	panic_spinlock_grp_attr = lck_grp_attr_alloc_init();
	panic_spinlock_grp = lck_grp_alloc_init("panic_spinlock", panic_spinlock_grp_attr);
	panic_spinlock_attr = lck_attr_alloc_init();

	lck_spin_init(&spinlock_panic_test_lock, panic_spinlock_grp, panic_spinlock_attr);


	/* Create thread to acquire spinlock */
	if (kernel_thread_start(spinlock_panic_test_acquire_spinlock, NULL, &panic_spinlock_thread) != KERN_SUCCESS) {
		return EBUSY;
	}

	/* Try to acquire spinlock -- should panic eventually */
	lck_spin_lock(&spinlock_panic_test_lock);
	while (1) {
		;
	}
}

__attribute__((noreturn))
static void
simultaneous_panic_worker
(void * arg, wait_result_t wres __unused)
{
	atomic_int *start_panic = (atomic_int *)arg;

	while (!atomic_load(start_panic)) {
		;
	}
	panic("SIMULTANEOUS PANIC TEST: INITIATING PANIC FROM CPU %d", cpu_number());
	__builtin_unreachable();
}

static int
sysctl_simultaneous_panic_test SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (req->newlen == 0) {
		return EINVAL;
	}

	int i = 0, threads_to_create = 2 * processor_count;
	atomic_int start_panic = 0;
	unsigned int threads_created = 0;
	thread_t new_panic_thread;

	for (i = threads_to_create; i > 0; i--) {
		if (kernel_thread_start(simultaneous_panic_worker, (void *) &start_panic, &new_panic_thread) == KERN_SUCCESS) {
			threads_created++;
		}
	}

	/* FAIL if we couldn't create at least processor_count threads */
	if (threads_created < processor_count) {
		panic("SIMULTANEOUS PANIC TEST: FAILED TO CREATE ENOUGH THREADS, ONLY CREATED %d (of %d)",
		    threads_created, threads_to_create);
	}

	atomic_exchange(&start_panic, 1);
	while (1) {
		;
	}
}

SYSCTL_PROC(_debug, OID_AUTO, panic_test, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_MASKED, 0, 0, sysctl_panic_test, "A", "panic test");
SYSCTL_PROC(_debug, OID_AUTO, debugger_test, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_MASKED, 0, 0, sysctl_debugger_test, "A", "debugger test");
SYSCTL_PROC(_debug, OID_AUTO, spinlock_panic_test, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_MASKED, 0, 0, sysctl_spinlock_panic_test, "A", "spinlock panic test");
SYSCTL_PROC(_debug, OID_AUTO, simultaneous_panic_test, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_MASKED, 0, 0, sysctl_simultaneous_panic_test, "A", "simultaneous panic test");

extern int exc_resource_threads_enabled;

SYSCTL_INT(_kern, OID_AUTO, exc_resource_threads_enabled, CTLFLAG_RD | CTLFLAG_LOCKED, &exc_resource_threads_enabled, 0, "exc_resource thread limit enabled");


#endif /* DEVELOPMENT || DEBUG */

#if CONFIG_THREAD_GROUPS
#if DEVELOPMENT || DEBUG

static int
sysctl_get_thread_group_id SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp)
	uint64_t thread_group_id = thread_group_get_id(thread_group_get(current_thread()));
	return SYSCTL_OUT(req, &thread_group_id, sizeof(thread_group_id));
}

SYSCTL_PROC(_kern, OID_AUTO, thread_group_id, CTLFLAG_RD | CTLFLAG_LOCKED | CTLTYPE_QUAD,
    0, 0, &sysctl_get_thread_group_id, "I", "thread group id of the thread");

STATIC int
sysctl_thread_group_count(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = thread_group_count();
	return sysctl_io_number(req, value, sizeof(value), NULL, NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, thread_group_count, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
    0, 0, &sysctl_thread_group_count, "I", "count of thread groups");

#endif /* DEVELOPMENT || DEBUG */
const uint32_t thread_groups_supported = 1;
#else /* CONFIG_THREAD_GROUPS */
const uint32_t thread_groups_supported = 0;
#endif /* CONFIG_THREAD_GROUPS */

STATIC int
sysctl_thread_groups_supported(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = thread_groups_supported;
	return sysctl_io_number(req, value, sizeof(value), NULL, NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, thread_groups_supported, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN,
    0, 0, &sysctl_thread_groups_supported, "I", "thread groups supported");

static int
sysctl_grade_cputype SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp)
	int error = 0;
	int type_tuple[2] = {};
	int return_value = 0;

	error = SYSCTL_IN(req, &type_tuple, sizeof(type_tuple));

	if (error) {
		return error;
	}

	return_value = grade_binary(type_tuple[0], type_tuple[1] & ~CPU_SUBTYPE_MASK, type_tuple[1] & CPU_SUBTYPE_MASK, FALSE);

	error = SYSCTL_OUT(req, &return_value, sizeof(return_value));

	if (error) {
		return error;
	}

	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, grade_cputype,
    CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_MASKED | CTLFLAG_LOCKED | CTLTYPE_OPAQUE,
    0, 0, &sysctl_grade_cputype, "S",
    "grade value of cpu_type_t+cpu_sub_type_t");

extern boolean_t allow_direct_handoff;
SYSCTL_INT(_kern, OID_AUTO, direct_handoff,
    CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED,
    &allow_direct_handoff, 0, "Enable direct handoff for realtime threads");

#if DEVELOPMENT || DEBUG

SYSCTL_LONG(_kern, OID_AUTO, phys_carveout_pa, CTLFLAG_RD | CTLFLAG_LOCKED,
    &phys_carveout_pa,
    "base physical address of the phys_carveout_mb boot-arg region");
SYSCTL_LONG(_kern, OID_AUTO, phys_carveout_size, CTLFLAG_RD | CTLFLAG_LOCKED,
    &phys_carveout_size,
    "size in bytes of the phys_carveout_mb boot-arg region");

extern void do_cseg_wedge_thread(void);
extern void do_cseg_unwedge_thread(void);

static int
cseg_wedge_thread SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)

	int error, val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || val == 0) {
		return error;
	}

	do_cseg_wedge_thread();
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, cseg_wedge_thread, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_MASKED, 0, 0, cseg_wedge_thread, "I", "wedge c_seg thread");

static int
cseg_unwedge_thread SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)

	int error, val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || val == 0) {
		return error;
	}

	do_cseg_unwedge_thread();
	return 0;
}
SYSCTL_PROC(_kern, OID_AUTO, cseg_unwedge_thread, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_MASKED, 0, 0, cseg_unwedge_thread, "I", "unstuck c_seg thread");

static atomic_int wedge_thread_should_wake = 0;

static int
unwedge_thread SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error, val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || val == 0) {
		return error;
	}

	atomic_store(&wedge_thread_should_wake, 1);
	return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, unwedge_thread, CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0, unwedge_thread, "I", "unwedge the thread wedged by kern.wedge_thread");

static int
wedge_thread SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)

	int error, val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || val == 0) {
		return error;
	}

	uint64_t interval = 1;
	nanoseconds_to_absolutetime(1000 * 1000 * 50, &interval);

	atomic_store(&wedge_thread_should_wake, 0);
	while (!atomic_load(&wedge_thread_should_wake)) {
		tsleep1(NULL, 0, "wedge_thread", mach_absolute_time() + interval, NULL);
	}

	return 0;
}

SYSCTL_PROC(_kern, OID_AUTO, wedge_thread,
    CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0, wedge_thread, "I",
    "wedge this thread so it cannot be cleaned up");

static int
sysctl_total_corpses_count SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	extern unsigned long total_corpses_count(void);

	unsigned long corpse_count_long = total_corpses_count();
	unsigned int corpse_count = (unsigned int)MIN(corpse_count_long, UINT_MAX);
	return sysctl_io_opaque(req, &corpse_count, sizeof(corpse_count), NULL);
}

SYSCTL_PROC(_kern, OID_AUTO, total_corpses_count,
    CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, 0, 0,
    sysctl_total_corpses_count, "I", "total corpses on the system");

static int
sysctl_turnstile_test_prim_lock SYSCTL_HANDLER_ARGS;
static int
sysctl_turnstile_test_prim_unlock SYSCTL_HANDLER_ARGS;
int
tstile_test_prim_lock(boolean_t use_hashtable);
int
tstile_test_prim_unlock(boolean_t use_hashtable);

static int
sysctl_turnstile_test_prim_lock SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error, val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || val == 0) {
		return error;
	}
	switch (val) {
	case SYSCTL_TURNSTILE_TEST_USER_DEFAULT:
	case SYSCTL_TURNSTILE_TEST_USER_HASHTABLE:
	case SYSCTL_TURNSTILE_TEST_KERNEL_DEFAULT:
	case SYSCTL_TURNSTILE_TEST_KERNEL_HASHTABLE:
		return tstile_test_prim_lock(val);
	default:
		return error;
	}
}

static int
sysctl_turnstile_test_prim_unlock SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error, val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || val == 0) {
		return error;
	}
	switch (val) {
	case SYSCTL_TURNSTILE_TEST_USER_DEFAULT:
	case SYSCTL_TURNSTILE_TEST_USER_HASHTABLE:
	case SYSCTL_TURNSTILE_TEST_KERNEL_DEFAULT:
	case SYSCTL_TURNSTILE_TEST_KERNEL_HASHTABLE:
		return tstile_test_prim_unlock(val);
	default:
		return error;
	}
}

SYSCTL_PROC(_kern, OID_AUTO, turnstiles_test_lock, CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_KERN | CTLFLAG_LOCKED,
    0, 0, sysctl_turnstile_test_prim_lock, "I", "turnstiles test lock");

SYSCTL_PROC(_kern, OID_AUTO, turnstiles_test_unlock, CTLFLAG_WR | CTLFLAG_ANYBODY | CTLFLAG_KERN | CTLFLAG_LOCKED,
    0, 0, sysctl_turnstile_test_prim_unlock, "I", "turnstiles test unlock");

int
turnstile_get_boost_stats_sysctl(void *req);
int
turnstile_get_unboost_stats_sysctl(void *req);
static int
sysctl_turnstile_boost_stats SYSCTL_HANDLER_ARGS;
static int
sysctl_turnstile_unboost_stats SYSCTL_HANDLER_ARGS;
extern uint64_t thread_block_on_turnstile_count;
extern uint64_t thread_block_on_regular_waitq_count;

static int
sysctl_turnstile_boost_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp)
	return turnstile_get_boost_stats_sysctl(req);
}

static int
sysctl_turnstile_unboost_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2, oidp)
	return turnstile_get_unboost_stats_sysctl(req);
}

SYSCTL_PROC(_kern, OID_AUTO, turnstile_boost_stats, CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_KERN | CTLFLAG_LOCKED | CTLTYPE_STRUCT,
    0, 0, sysctl_turnstile_boost_stats, "S", "turnstiles boost stats");
SYSCTL_PROC(_kern, OID_AUTO, turnstile_unboost_stats, CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_KERN | CTLFLAG_LOCKED | CTLTYPE_STRUCT,
    0, 0, sysctl_turnstile_unboost_stats, "S", "turnstiles unboost stats");
SYSCTL_QUAD(_kern, OID_AUTO, thread_block_count_on_turnstile,
    CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &thread_block_on_turnstile_count, "thread blocked on turnstile count");
SYSCTL_QUAD(_kern, OID_AUTO, thread_block_count_on_reg_waitq,
    CTLFLAG_RD | CTLFLAG_ANYBODY | CTLFLAG_KERN | CTLFLAG_LOCKED,
    &thread_block_on_regular_waitq_count, "thread blocked on regular waitq count");

static int
sysctl_erase_all_test_mtx_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error, val = 0;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error || val == 0) {
		return error;
	}

	if (val == 1) {
		lck_mtx_test_init();
		erase_all_test_mtx_stats();
	}

	return 0;
}

static int
sysctl_get_test_mtx_stats SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char* buffer;
	int size, buffer_size, error;

	buffer_size = 1000;
	buffer = kheap_alloc(KHEAP_TEMP, buffer_size, Z_WAITOK);
	if (!buffer) {
		panic("Impossible to allocate memory for %s\n", __func__);
	}

	lck_mtx_test_init();

	size = get_test_mtx_stats_string(buffer, buffer_size);

	error = sysctl_io_string(req, buffer, size, 0, NULL);

	kheap_free(KHEAP_TEMP, buffer, buffer_size);

	return error;
}

static int
sysctl_test_mtx_uncontended SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char* buffer;
	int buffer_size, offset, error, iter;
	char input_val[40];

	if (!req->newptr) {
		return 0;
	}

	if (!req->oldptr) {
		return EINVAL;
	}

	if (req->newlen >= sizeof(input_val)) {
		return EINVAL;
	}

	error = SYSCTL_IN(req, input_val, req->newlen);
	if (error) {
		return error;
	}
	input_val[req->newlen] = '\0';

	iter = 0;
	error = sscanf(input_val, "%d", &iter);
	if (error != 1) {
		printf("%s invalid input\n", __func__);
		return EINVAL;
	}

	if (iter <= 0) {
		printf("%s requested %d iterations, not starting the test\n", __func__, iter);
		return EINVAL;
	}

	lck_mtx_test_init();

	buffer_size = 2000;
	offset = 0;
	buffer = kheap_alloc(KHEAP_TEMP, buffer_size, Z_WAITOK);
	if (!buffer) {
		panic("Impossible to allocate memory for %s\n", __func__);
	}
	memset(buffer, 0, buffer_size);

	printf("%s starting uncontended mutex test with %d iterations\n", __func__, iter);

	offset = scnprintf(buffer, buffer_size, "STATS INNER LOOP");
	offset += lck_mtx_test_mtx_uncontended(iter, &buffer[offset], buffer_size - offset);

	offset += scnprintf(&buffer[offset], buffer_size - offset, "\nSTATS OUTER LOOP");
	offset += lck_mtx_test_mtx_uncontended_loop_time(iter, &buffer[offset], buffer_size - offset);

	error = SYSCTL_OUT(req, buffer, offset);

	kheap_free(KHEAP_TEMP, buffer, buffer_size);
	return error;
}

static int
sysctl_test_mtx_contended SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char* buffer;
	int buffer_size, offset, error, iter;
	char input_val[40];

	if (!req->newptr) {
		return 0;
	}

	if (!req->oldptr) {
		return EINVAL;
	}

	if (req->newlen >= sizeof(input_val)) {
		return EINVAL;
	}

	error = SYSCTL_IN(req, input_val, req->newlen);
	if (error) {
		return error;
	}
	input_val[req->newlen] = '\0';

	iter = 0;
	error = sscanf(input_val, "%d", &iter);
	if (error != 1) {
		printf("%s invalid input\n", __func__);
		return EINVAL;
	}

	if (iter <= 0) {
		printf("%s requested %d iterations, not starting the test\n", __func__, iter);
		return EINVAL;
	}

	lck_mtx_test_init();

	erase_all_test_mtx_stats();

	buffer_size = 2000;
	offset = 0;
	buffer = kheap_alloc(KHEAP_TEMP, buffer_size, Z_WAITOK | Z_ZERO);
	if (!buffer) {
		panic("Impossible to allocate memory for %s\n", __func__);
	}

	printf("%s starting contended mutex test with %d iterations FULL_CONTENDED\n", __func__, iter);

	offset = scnprintf(buffer, buffer_size, "STATS INNER LOOP");
	offset += lck_mtx_test_mtx_contended(iter, &buffer[offset], buffer_size - offset, FULL_CONTENDED);

	printf("%s starting contended mutex loop test with %d iterations FULL_CONTENDED\n", __func__, iter);

	offset += scnprintf(&buffer[offset], buffer_size - offset, "\nSTATS OUTER LOOP");
	offset += lck_mtx_test_mtx_contended_loop_time(iter, &buffer[offset], buffer_size - offset, FULL_CONTENDED);

	printf("%s starting contended mutex test with %d iterations HALF_CONTENDED\n", __func__, iter);

	offset += scnprintf(&buffer[offset], buffer_size - offset, "STATS INNER LOOP");
	offset += lck_mtx_test_mtx_contended(iter, &buffer[offset], buffer_size - offset, HALF_CONTENDED);

	printf("%s starting contended mutex loop test with %d iterations HALF_CONTENDED\n", __func__, iter);

	offset += scnprintf(&buffer[offset], buffer_size - offset, "\nSTATS OUTER LOOP");
	offset += lck_mtx_test_mtx_contended_loop_time(iter, &buffer[offset], buffer_size - offset, HALF_CONTENDED);

	error = SYSCTL_OUT(req, buffer, offset);

	printf("\n%s\n", buffer);
	kheap_free(KHEAP_TEMP, buffer, buffer_size);

	return error;
}

SYSCTL_PROC(_kern, OID_AUTO, erase_all_test_mtx_stats,
    CTLFLAG_WR | CTLFLAG_MASKED | CTLFLAG_ANYBODY | CTLFLAG_KERN | CTLFLAG_LOCKED,
    0, 0, sysctl_erase_all_test_mtx_stats, "I", "erase test_mtx statistics");

SYSCTL_PROC(_kern, OID_AUTO, get_test_mtx_stats,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MASKED | CTLFLAG_KERN | CTLFLAG_LOCKED,
    0, 0, sysctl_get_test_mtx_stats, "A", "get test_mtx statistics");

SYSCTL_PROC(_kern, OID_AUTO, test_mtx_contended,
    CTLTYPE_STRING | CTLFLAG_MASKED | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    0, 0, sysctl_test_mtx_contended, "A", "get statistics for contended mtx test");

SYSCTL_PROC(_kern, OID_AUTO, test_mtx_uncontended,
    CTLTYPE_STRING | CTLFLAG_MASKED | CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED,
    0, 0, sysctl_test_mtx_uncontended, "A", "get statistics for uncontended mtx test");

extern uint64_t MutexSpin;

SYSCTL_QUAD(_kern, OID_AUTO, mutex_spin_abs, CTLFLAG_RW, &MutexSpin,
    "Spin time in abs for acquiring a kernel mutex");

extern uint64_t low_MutexSpin;
extern int64_t high_MutexSpin;
extern unsigned int real_ncpus;

SYSCTL_QUAD(_kern, OID_AUTO, low_mutex_spin_abs, CTLFLAG_RW, &low_MutexSpin,
    "Low spin threshold in abs for acquiring a kernel mutex");

static int
sysctl_high_mutex_spin_ns SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int error;
	int64_t val = 0;
	int64_t res;

	/* Check if the user is writing to high_MutexSpin, or just reading it */
	if (req->newptr) {
		error = SYSCTL_IN(req, &val, sizeof(val));
		if (error || (val < 0 && val != -1)) {
			return error;
		}
		high_MutexSpin = val;
	}

	if (high_MutexSpin >= 0) {
		res = high_MutexSpin;
	} else {
		res = low_MutexSpin * real_ncpus;
	}
	return SYSCTL_OUT(req, &res, sizeof(res));
}
SYSCTL_PROC(_kern, OID_AUTO, high_mutex_spin_abs, CTLFLAG_RW | CTLTYPE_QUAD, 0, 0, sysctl_high_mutex_spin_ns, "I",
    "High spin threshold in abs for acquiring a kernel mutex");


#if defined (__x86_64__)

semaphore_t sysctl_test_panic_with_thread_sem;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winfinite-recursion" /* rdar://38801963 */
__attribute__((noreturn))
static void
panic_thread_test_child_spin(void * arg, wait_result_t wres)
{
	static int panic_thread_recurse_count = 5;

	if (panic_thread_recurse_count > 0) {
		panic_thread_recurse_count--;
		panic_thread_test_child_spin(arg, wres);
	}

	semaphore_signal(sysctl_test_panic_with_thread_sem);
	while (1) {
		;
	}
}
#pragma clang diagnostic pop

static void
panic_thread_test_child_park(void * arg __unused, wait_result_t wres __unused)
{
	int event;

	assert_wait(&event, THREAD_UNINT);
	semaphore_signal(sysctl_test_panic_with_thread_sem);
	thread_block(panic_thread_test_child_park);
}

static int
sysctl_test_panic_with_thread SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int rval = 0;
	char str[16] = { '\0' };
	thread_t child_thread = THREAD_NULL;

	rval = sysctl_handle_string(oidp, str, sizeof(str), req);
	if (rval != 0 || !req->newptr) {
		return EINVAL;
	}

	semaphore_create(kernel_task, &sysctl_test_panic_with_thread_sem, SYNC_POLICY_FIFO, 0);

	/* Create thread to spin or park in continuation */
	if (strncmp("spin", str, strlen("spin")) == 0) {
		if (kernel_thread_start(panic_thread_test_child_spin, NULL, &child_thread) != KERN_SUCCESS) {
			semaphore_destroy(kernel_task, sysctl_test_panic_with_thread_sem);
			return EBUSY;
		}
	} else if (strncmp("continuation", str, strlen("continuation")) == 0) {
		if (kernel_thread_start(panic_thread_test_child_park, NULL, &child_thread) != KERN_SUCCESS) {
			semaphore_destroy(kernel_task, sysctl_test_panic_with_thread_sem);
			return EBUSY;
		}
	} else {
		semaphore_destroy(kernel_task, sysctl_test_panic_with_thread_sem);
		return EINVAL;
	}

	semaphore_wait(sysctl_test_panic_with_thread_sem);

	panic_with_thread_context(0, NULL, 0, child_thread, "testing panic_with_thread_context for thread %p", child_thread);

	/* Not reached */
	return EINVAL;
}

SYSCTL_PROC(_kern, OID_AUTO, test_panic_with_thread,
    CTLFLAG_MASKED | CTLFLAG_KERN | CTLFLAG_LOCKED | CTLFLAG_WR | CTLTYPE_STRING,
    0, 0, sysctl_test_panic_with_thread, "A", "test panic flow for backtracing a different thread");
#endif /* defined (__x86_64__) */

#endif /* DEVELOPMENT || DEBUG */

static int
sysctl_get_owned_vmobjects SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)

	/* validate */
	if (req->newlen != sizeof(mach_port_name_t) || req->newptr == USER_ADDR_NULL ||
    req->oldidx != 0 || req->newidx != 0 || req->p == NULL) {
		return EINVAL;
	}

	int error;
	mach_port_name_t task_port_name;
	task_t task;
	size_t buffer_size = (req->oldptr != USER_ADDR_NULL) ? req->oldlen : 0;
	vmobject_list_output_t buffer = NULL;
	size_t output_size;
	size_t entries;

	/* we have a "newptr" (for write) we get a task port name from the caller. */
	error = SYSCTL_IN(req, &task_port_name, sizeof(mach_port_name_t));

	if (error != 0) {
		goto sysctl_get_vmobject_list_exit;
	}

	task = port_name_to_task_read(task_port_name);
	if (task == TASK_NULL) {
		error = ESRCH;
		goto sysctl_get_vmobject_list_exit;
	}

	/* get the current size */
	task_copy_vmobjects(task, NULL, 0, &entries);
	size_t max_size = (entries > 0) ? entries * sizeof(vm_object_query_data_t) + sizeof(*buffer) : 0;

	/* if buffer_size is specified clamp to the current size then allcoate the kernel buffer */
	if (buffer_size) {
		if (buffer_size < sizeof(*buffer) + sizeof(vm_object_query_data_t)) {
			error = ENOMEM;
			goto sysctl_get_vmobject_list_deallocate_and_exit;
		}

		buffer_size = (buffer_size > max_size) ? max_size : buffer_size;
		buffer = kheap_alloc(KHEAP_TEMP, buffer_size, Z_WAITOK);

		if (!buffer) {
			error = ENOMEM;
			goto sysctl_get_vmobject_list_deallocate_and_exit;
		}
	} else {
		buffer = NULL;
	}

	/* copy the vmobjects and vmobject data out of the task */
	if (buffer_size == 0) {
		output_size = max_size;
	} else {
		task_copy_vmobjects(task, &buffer->data[0], buffer_size - sizeof(*buffer), &entries);
		buffer->entries = (uint64_t)entries;
		output_size = entries * sizeof(vm_object_query_data_t) + sizeof(*buffer);
	}

	error = SYSCTL_OUT(req, (char*) buffer, output_size);

sysctl_get_vmobject_list_deallocate_and_exit:
	task_deallocate(task);

sysctl_get_vmobject_list_exit:
	if (buffer) {
		kheap_free(KHEAP_TEMP, buffer, buffer_size);
	}

	return error;
}

SYSCTL_PROC(_vm, OID_AUTO, get_owned_vmobjects,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_WR | CTLFLAG_MASKED | CTLFLAG_KERN | CTLFLAG_LOCKED | CTLFLAG_ANYBODY,
    0, 0, sysctl_get_owned_vmobjects, "A", "get owned vmobjects in task");

extern uint64_t num_static_scalable_counters;
SYSCTL_QUAD(_kern, OID_AUTO, num_static_scalable_counters, CTLFLAG_RD | CTLFLAG_LOCKED, &num_static_scalable_counters, "");

uuid_string_t trial_treatment_id;
uuid_string_t trial_experiment_id;
int trial_deployment_id = -1;

SYSCTL_STRING(_kern, OID_AUTO, trial_treatment_id, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY | CTLFLAG_EXPERIMENT, trial_treatment_id, sizeof(trial_treatment_id), "");
SYSCTL_STRING(_kern, OID_AUTO, trial_experiment_id, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY | CTLFLAG_EXPERIMENT, trial_experiment_id, sizeof(trial_experiment_id), "");
SYSCTL_INT(_kern, OID_AUTO, trial_deployment_id, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY | CTLFLAG_EXPERIMENT, &trial_deployment_id, 0, "");

#if DEVELOPMENT || DEBUG
/* For unit testing setting factors & limits. */
unsigned int testing_experiment_factor;
EXPERIMENT_FACTOR_UINT(_kern, testing_experiment_factor, &testing_experiment_factor, 5, 10, "");
#endif /* DEVELOPMENT || DEBUG */
