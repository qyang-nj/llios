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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)kern_proc.c	8.4 (Berkeley) 1/4/94
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */
/* HISTORY
 *  04-Aug-97  Umesh Vaishampayan (umeshv@apple.com)
 *	Added current_proc_EXTERNAL() function for the use of kernel
 *      lodable modules.
 *
 *  05-Jun-95 Mac Gillon (mgillon) at NeXT
 *	New version based on 3.3NS and 4.4
 */


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc_internal.h>
#include <sys/acct.h>
#include <sys/wait.h>
#include <sys/file_internal.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mbuf.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
#include <sys/signalvar.h>
#include <sys/syslog.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/kauth.h>
#include <sys/codesign.h>
#include <sys/kernel_types.h>
#include <sys/ubc.h>
#include <kern/kalloc.h>
#include <kern/task.h>
#include <kern/coalition.h>
#include <sys/coalition.h>
#include <kern/assert.h>
#include <vm/vm_protos.h>
#include <vm/vm_map.h>          /* vm_map_switch_protect() */
#include <vm/vm_pageout.h>
#include <mach/task.h>
#include <mach/message.h>
#include <sys/priv.h>
#include <sys/proc_info.h>
#include <sys/bsdtask_info.h>
#include <sys/persona.h>
#include <sys/sysent.h>
#include <sys/reason.h>
#include <sys/proc_require.h>
#include <IOKit/IOBSD.h>        /* IOTaskHasEntitlement() */
#include <kern/ipc_kobject.h>   /* ipc_kobject_set_kobjidx() */

#ifdef CONFIG_32BIT_TELEMETRY
#include <sys/kasl.h>
#endif /* CONFIG_32BIT_TELEMETRY */

#if CONFIG_CSR
#include <sys/csr.h>
#endif

#if CONFIG_MEMORYSTATUS
#include <sys/kern_memorystatus.h>
#endif

#if CONFIG_MACF
#include <security/mac_framework.h>
#include <security/mac_mach_internal.h>
#endif

#include <libkern/crypto/sha1.h>

#ifdef CONFIG_32BIT_TELEMETRY
#define MAX_32BIT_EXEC_SIG_SIZE 160
#endif /* CONFIG_32BIT_TELEMETRY */

/*
 * Structure associated with user cacheing.
 */
struct uidinfo {
	LIST_ENTRY(uidinfo) ui_hash;
	uid_t   ui_uid;
	size_t    ui_proccnt;
};
#define UIHASH(uid)     (&uihashtbl[(uid) & uihash])
LIST_HEAD(uihashhead, uidinfo) * uihashtbl;
u_long uihash;          /* size of hash table - 1 */

/*
 * Other process lists
 */
struct pidhashhead *pidhashtbl;
u_long pidhash;
struct pgrphashhead *pgrphashtbl;
u_long pgrphash;
struct sesshashhead *sesshashtbl;
u_long sesshash;

struct proclist allproc;
struct proclist zombproc;
extern struct tty cons;

extern int cs_debug;

#if DEVELOPMENT || DEBUG
int syscallfilter_disable = 0;
#endif // DEVELOPMENT || DEBUG

#if DEBUG
#define __PROC_INTERNAL_DEBUG 1
#endif
#if CONFIG_COREDUMP
/* Name to give to core files */
#if defined(XNU_TARGET_OS_BRIDGE)
__XNU_PRIVATE_EXTERN char corefilename[MAXPATHLEN + 1] = {"/private/var/internal/%N.core"};
#elif defined(XNU_TARGET_OS_OSX)
__XNU_PRIVATE_EXTERN char corefilename[MAXPATHLEN + 1] = {"/cores/core.%P"};
#else
__XNU_PRIVATE_EXTERN char corefilename[MAXPATHLEN + 1] = {"/private/var/cores/%N.core"};
#endif
#endif

#if PROC_REF_DEBUG
#include <kern/backtrace.h>
#endif

static LCK_MTX_DECLARE_ATTR(proc_klist_mlock, &proc_mlock_grp, &proc_lck_attr);

ZONE_DECLARE(pgrp_zone, "pgrp",
    sizeof(struct pgrp), ZC_ZFREE_CLEARMEM);
ZONE_DECLARE(session_zone, "session",
    sizeof(struct session), ZC_ZFREE_CLEARMEM);
/*
 * If you need accounting for KM_PROC consider using
 * ZONE_VIEW_DEFINE to define a zone view.
 */
#define KM_PROC KHEAP_DEFAULT

typedef uint64_t unaligned_u64 __attribute__((aligned(1)));

static void orphanpg(struct pgrp * pg);
void proc_name_kdp(task_t t, char * buf, int size);
boolean_t proc_binary_uuid_kdp(task_t task, uuid_t uuid);
int proc_threadname_kdp(void * uth, char * buf, size_t size);
void proc_starttime_kdp(void * p, unaligned_u64 *tv_sec, unaligned_u64 *tv_usec, unaligned_u64 *abstime);
void proc_archinfo_kdp(void* p, cpu_type_t* cputype, cpu_subtype_t* cpusubtype);
char * proc_name_address(void * p);
char * proc_longname_address(void *);

static void  pgrp_add(struct pgrp * pgrp, proc_t parent, proc_t child);
static void pgrp_remove(proc_t p);
static void pgrp_replace(proc_t p, struct pgrp *pgrp);
static void pgdelete_dropref(struct pgrp *pgrp);
extern void pg_rele_dropref(struct pgrp * pgrp);
static int csops_internal(pid_t pid, int ops, user_addr_t uaddr, user_size_t usersize, user_addr_t uaddittoken);
static boolean_t proc_parent_is_currentproc(proc_t p);

struct fixjob_iterargs {
	struct pgrp * pg;
	struct session * mysession;
	int entering;
};

int fixjob_callback(proc_t, void *);

uint64_t
get_current_unique_pid(void)
{
	proc_t  p = current_proc();

	if (p) {
		return p->p_uniqueid;
	} else {
		return 0;
	}
}

/*
 * Initialize global process hashing structures.
 */
void
procinit(void)
{
	LIST_INIT(&allproc);
	LIST_INIT(&zombproc);
	pidhashtbl = hashinit(maxproc / 4, M_PROC, &pidhash);
	pgrphashtbl = hashinit(maxproc / 4, M_PROC, &pgrphash);
	sesshashtbl = hashinit(maxproc / 4, M_PROC, &sesshash);
	uihashtbl = hashinit(maxproc / 16, M_PROC, &uihash);
#if CONFIG_PERSONAS
	personas_bootstrap();
#endif
}

/*
 * Change the count associated with number of processes
 * a given user is using. This routine protects the uihash
 * with the list lock
 */
size_t
chgproccnt(uid_t uid, int diff)
{
	struct uidinfo *uip;
	struct uidinfo *newuip = NULL;
	struct uihashhead *uipp;
	size_t retval;

again:
	proc_list_lock();
	uipp = UIHASH(uid);
	for (uip = uipp->lh_first; uip != 0; uip = uip->ui_hash.le_next) {
		if (uip->ui_uid == uid) {
			break;
		}
	}
	if (uip) {
		uip->ui_proccnt += diff;
		if (uip->ui_proccnt > 0) {
			retval = uip->ui_proccnt;
			proc_list_unlock();
			goto out;
		}
		LIST_REMOVE(uip, ui_hash);
		retval = 0;
		proc_list_unlock();
		kheap_free(KM_PROC, uip, sizeof(struct uidinfo));
		goto out;
	}
	if (diff <= 0) {
		if (diff == 0) {
			retval = 0;
			proc_list_unlock();
			goto out;
		}
		panic("chgproccnt: lost user");
	}
	if (newuip != NULL) {
		uip = newuip;
		newuip = NULL;
		LIST_INSERT_HEAD(uipp, uip, ui_hash);
		uip->ui_uid = uid;
		uip->ui_proccnt = diff;
		retval = diff;
		proc_list_unlock();
		goto out;
	}
	proc_list_unlock();
	newuip = kheap_alloc(KM_PROC, sizeof(struct uidinfo), Z_WAITOK);
	if (newuip == NULL) {
		panic("chgproccnt: M_PROC zone depleted");
	}
	goto again;
out:
	kheap_free(KM_PROC, newuip, sizeof(struct uidinfo));
	return retval;
}

/*
 * Is p an inferior of the current process?
 */
int
inferior(proc_t p)
{
	int retval = 0;

	proc_list_lock();
	for (; p != current_proc(); p = p->p_pptr) {
		if (p->p_pid == 0) {
			goto out;
		}
	}
	retval = 1;
out:
	proc_list_unlock();
	return retval;
}

/*
 * Is p an inferior of t ?
 */
int
isinferior(proc_t p, proc_t t)
{
	int retval = 0;
	int nchecked = 0;
	proc_t start = p;

	/* if p==t they are not inferior */
	if (p == t) {
		return 0;
	}

	proc_list_lock();
	for (; p != t; p = p->p_pptr) {
		nchecked++;

		/* Detect here if we're in a cycle */
		if ((p->p_pid == 0) || (p->p_pptr == start) || (nchecked >= nprocs)) {
			goto out;
		}
	}
	retval = 1;
out:
	proc_list_unlock();
	return retval;
}

int
proc_isinferior(int pid1, int pid2)
{
	proc_t p = PROC_NULL;
	proc_t t = PROC_NULL;
	int retval = 0;

	if (((p = proc_find(pid1)) != (proc_t)0) && ((t = proc_find(pid2)) != (proc_t)0)) {
		retval = isinferior(p, t);
	}

	if (p != PROC_NULL) {
		proc_rele(p);
	}
	if (t != PROC_NULL) {
		proc_rele(t);
	}

	return retval;
}

proc_t
proc_find(int pid)
{
	return proc_findinternal(pid, 0);
}

proc_t
proc_findinternal(int pid, int locked)
{
	proc_t p = PROC_NULL;

	if (locked == 0) {
		proc_list_lock();
	}

	p = pfind_locked(pid);
	if ((p == PROC_NULL) || (p != proc_ref_locked(p))) {
		p = PROC_NULL;
	}

	if (locked == 0) {
		proc_list_unlock();
	}

	return p;
}

proc_t
proc_findthread(thread_t thread)
{
	proc_t p = PROC_NULL;
	struct uthread *uth;

	proc_list_lock();
	uth = get_bsdthread_info(thread);
	if (uth && (uth->uu_flag & UT_VFORK)) {
		p = uth->uu_proc;
	} else {
		p = (proc_t)(get_bsdthreadtask_info(thread));
	}
	p = proc_ref_locked(p);
	proc_list_unlock();
	return p;
}

/*
 * Returns process identity of a given process. Calling this function is not
 * racy for a current process or if a reference to the process is held.
 */
struct proc_ident
proc_ident(proc_t p)
{
	struct proc_ident ident = {
		.p_pid = proc_pid(p),
		.p_uniqueid = proc_uniqueid(p),
		.p_idversion = proc_pidversion(p),
	};

	return ident;
}

proc_t
proc_find_ident(struct proc_ident const *ident)
{
	proc_t proc = PROC_NULL;

	proc = proc_find(ident->p_pid);
	if (proc == PROC_NULL) {
		return PROC_NULL;
	}

	if (proc_uniqueid(proc) != ident->p_uniqueid ||
	    proc_pidversion(proc) != ident->p_idversion) {
		proc_rele(proc);
		return PROC_NULL;
	}

	return proc;
}

void
uthread_reset_proc_refcount(void *uthread)
{
	uthread_t uth;

	uth = (uthread_t) uthread;
	uth->uu_proc_refcount = 0;

#if PROC_REF_DEBUG
	if (proc_ref_tracking_disabled) {
		return;
	}

	uth->uu_pindex = 0;
#endif
}

#if PROC_REF_DEBUG
int
uthread_get_proc_refcount(void *uthread)
{
	uthread_t uth;

	if (proc_ref_tracking_disabled) {
		return 0;
	}

	uth = (uthread_t) uthread;

	return uth->uu_proc_refcount;
}
#endif

static void
record_procref(proc_t p __unused, int count)
{
	uthread_t uth;

	uth = current_uthread();
	uth->uu_proc_refcount += count;

#if PROC_REF_DEBUG
	if (proc_ref_tracking_disabled) {
		return;
	}

	if (uth->uu_pindex < NUM_PROC_REFS_TO_TRACK) {
		backtrace((uintptr_t *) &uth->uu_proc_pcs[uth->uu_pindex],
		    PROC_REF_STACK_DEPTH, NULL);

		uth->uu_proc_ps[uth->uu_pindex] = p;
		uth->uu_pindex++;
	}
#endif
}

static boolean_t
uthread_needs_to_wait_in_proc_refwait(void)
{
	uthread_t uth = current_uthread();

	/*
	 * Allow threads holding no proc refs to wait
	 * in proc_refwait, allowing threads holding
	 * proc refs to wait in proc_refwait causes
	 * deadlocks and makes proc_find non-reentrant.
	 */
	if (uth->uu_proc_refcount == 0) {
		return TRUE;
	}

	return FALSE;
}

int
proc_rele(proc_t p)
{
	proc_list_lock();
	proc_rele_locked(p);
	proc_list_unlock();

	return 0;
}

proc_t
proc_self(void)
{
	struct proc * p;

	p = current_proc();

	proc_list_lock();
	if (p != proc_ref_locked(p)) {
		p = PROC_NULL;
	}
	proc_list_unlock();
	return p;
}


proc_t
proc_ref_locked(proc_t p)
{
	proc_t p1 = p;
	int pid = proc_pid(p);

retry:
	/*
	 * if process still in creation or proc got recycled
	 * during msleep then return failure.
	 */
	if ((p == PROC_NULL) || (p1 != p) || ((p->p_listflag & P_LIST_INCREATE) != 0)) {
		return PROC_NULL;
	}

	/*
	 * Do not return process marked for termination
	 * or proc_refdrain called without ref wait.
	 * Wait for proc_refdrain_with_refwait to complete if
	 * process in refdrain and refwait flag is set, unless
	 * the current thread is holding to a proc_ref
	 * for any proc.
	 */
	if ((p->p_stat != SZOMB) &&
	    ((p->p_listflag & P_LIST_EXITED) == 0) &&
	    ((p->p_listflag & P_LIST_DEAD) == 0) &&
	    (((p->p_listflag & (P_LIST_DRAIN | P_LIST_DRAINWAIT)) == 0) ||
	    ((p->p_listflag & P_LIST_REFWAIT) != 0))) {
		if ((p->p_listflag & P_LIST_REFWAIT) != 0 && uthread_needs_to_wait_in_proc_refwait()) {
			msleep(&p->p_listflag, &proc_list_mlock, 0, "proc_refwait", 0);
			/*
			 * the proc might have been recycled since we dropped
			 * the proc list lock, get the proc again.
			 */
			p = pfind_locked(pid);
			goto retry;
		}
		p->p_refcount++;
		record_procref(p, 1);
	} else {
		p1 = PROC_NULL;
	}

	return p1;
}

void
proc_rele_locked(proc_t p)
{
	if (p->p_refcount > 0) {
		p->p_refcount--;
		record_procref(p, -1);
		if ((p->p_refcount == 0) && ((p->p_listflag & P_LIST_DRAINWAIT) == P_LIST_DRAINWAIT)) {
			p->p_listflag &= ~P_LIST_DRAINWAIT;
			wakeup(&p->p_refcount);
		}
	} else {
		panic("proc_rele_locked  -ve ref\n");
	}
}

proc_t
proc_find_zombref(int pid)
{
	proc_t p;

	proc_list_lock();

again:
	p = pfind_locked(pid);

	/* should we bail? */
	if ((p == PROC_NULL)                                    /* not found */
	    || ((p->p_listflag & P_LIST_INCREATE) != 0)         /* not created yet */
	    || ((p->p_listflag & P_LIST_EXITED) == 0)) {        /* not started exit */
		proc_list_unlock();
		return PROC_NULL;
	}

	/* If someone else is controlling the (unreaped) zombie - wait */
	if ((p->p_listflag & P_LIST_WAITING) != 0) {
		(void)msleep(&p->p_stat, &proc_list_mlock, PWAIT, "waitcoll", 0);
		goto again;
	}
	p->p_listflag |=  P_LIST_WAITING;

	proc_list_unlock();

	return p;
}

void
proc_drop_zombref(proc_t p)
{
	proc_list_lock();
	if ((p->p_listflag & P_LIST_WAITING) == P_LIST_WAITING) {
		p->p_listflag &= ~P_LIST_WAITING;
		wakeup(&p->p_stat);
	}
	proc_list_unlock();
}


void
proc_refdrain(proc_t p)
{
	proc_refdrain_with_refwait(p, FALSE);
}

proc_t
proc_refdrain_with_refwait(proc_t p, boolean_t get_ref_and_allow_wait)
{
	boolean_t initexec = FALSE;
	proc_list_lock();

	p->p_listflag |= P_LIST_DRAIN;
	if (get_ref_and_allow_wait) {
		/*
		 * All the calls to proc_ref_locked will wait
		 * for the flag to get cleared before returning a ref,
		 * unless the current thread is holding to a proc ref
		 * for any proc.
		 */
		p->p_listflag |= P_LIST_REFWAIT;
		if (p == initproc) {
			initexec = TRUE;
		}
	}

	/* Do not wait in ref drain for launchd exec */
	while (p->p_refcount && !initexec) {
		p->p_listflag |= P_LIST_DRAINWAIT;
		msleep(&p->p_refcount, &proc_list_mlock, 0, "proc_refdrain", 0);
	}

	p->p_listflag &= ~P_LIST_DRAIN;
	if (!get_ref_and_allow_wait) {
		p->p_listflag |= P_LIST_DEAD;
	} else {
		/* Return a ref to the caller */
		p->p_refcount++;
		record_procref(p, 1);
	}

	proc_list_unlock();

	if (get_ref_and_allow_wait) {
		return p;
	}
	return NULL;
}

void
proc_refwake(proc_t p)
{
	proc_list_lock();
	p->p_listflag &= ~P_LIST_REFWAIT;
	wakeup(&p->p_listflag);
	proc_list_unlock();
}

proc_t
proc_parentholdref(proc_t p)
{
	proc_t parent = PROC_NULL;
	proc_t pp;
	int loopcnt = 0;


	proc_list_lock();
loop:
	pp = p->p_pptr;
	if ((pp == PROC_NULL) || (pp->p_stat == SZOMB) || ((pp->p_listflag & (P_LIST_CHILDDRSTART | P_LIST_CHILDDRAINED)) == (P_LIST_CHILDDRSTART | P_LIST_CHILDDRAINED))) {
		parent = PROC_NULL;
		goto out;
	}

	if ((pp->p_listflag & (P_LIST_CHILDDRSTART | P_LIST_CHILDDRAINED)) == P_LIST_CHILDDRSTART) {
		pp->p_listflag |= P_LIST_CHILDDRWAIT;
		msleep(&pp->p_childrencnt, &proc_list_mlock, 0, "proc_parent", 0);
		loopcnt++;
		if (loopcnt == 5) {
			parent = PROC_NULL;
			goto out;
		}
		goto loop;
	}

	if ((pp->p_listflag & (P_LIST_CHILDDRSTART | P_LIST_CHILDDRAINED)) == 0) {
		pp->p_parentref++;
		parent = pp;
		goto out;
	}

out:
	proc_list_unlock();
	return parent;
}
int
proc_parentdropref(proc_t p, int listlocked)
{
	if (listlocked == 0) {
		proc_list_lock();
	}

	if (p->p_parentref > 0) {
		p->p_parentref--;
		if ((p->p_parentref == 0) && ((p->p_listflag & P_LIST_PARENTREFWAIT) == P_LIST_PARENTREFWAIT)) {
			p->p_listflag &= ~P_LIST_PARENTREFWAIT;
			wakeup(&p->p_parentref);
		}
	} else {
		panic("proc_parentdropref  -ve ref\n");
	}
	if (listlocked == 0) {
		proc_list_unlock();
	}

	return 0;
}

void
proc_childdrainstart(proc_t p)
{
#if __PROC_INTERNAL_DEBUG
	if ((p->p_listflag & P_LIST_CHILDDRSTART) == P_LIST_CHILDDRSTART) {
		panic("proc_childdrainstart: childdrain already started\n");
	}
#endif
	p->p_listflag |= P_LIST_CHILDDRSTART;
	/* wait for all that hold parentrefs to drop */
	while (p->p_parentref > 0) {
		p->p_listflag |= P_LIST_PARENTREFWAIT;
		msleep(&p->p_parentref, &proc_list_mlock, 0, "proc_childdrainstart", 0);
	}
}


void
proc_childdrainend(proc_t p)
{
#if __PROC_INTERNAL_DEBUG
	if (p->p_childrencnt > 0) {
		panic("exiting: children stil hanging around\n");
	}
#endif
	p->p_listflag |= P_LIST_CHILDDRAINED;
	if ((p->p_listflag & (P_LIST_CHILDLKWAIT | P_LIST_CHILDDRWAIT)) != 0) {
		p->p_listflag &= ~(P_LIST_CHILDLKWAIT | P_LIST_CHILDDRWAIT);
		wakeup(&p->p_childrencnt);
	}
}

void
proc_checkdeadrefs(__unused proc_t p)
{
#if __PROC_INTERNAL_DEBUG
	if ((p->p_listflag  & P_LIST_INHASH) != 0) {
		panic("proc being freed and still in hash %p: %u\n", p, p->p_listflag);
	}
	if (p->p_childrencnt != 0) {
		panic("proc being freed and pending children cnt %p:%d\n", p, p->p_childrencnt);
	}
	if (p->p_refcount != 0) {
		panic("proc being freed and pending refcount %p:%d\n", p, p->p_refcount);
	}
	if (p->p_parentref != 0) {
		panic("proc being freed and pending parentrefs %p:%d\n", p, p->p_parentref);
	}
#endif
}


__attribute__((always_inline, visibility("hidden")))
void
proc_require(proc_t proc, proc_require_flags_t flags)
{
	if ((flags & PROC_REQUIRE_ALLOW_NULL) && proc == PROC_NULL) {
		return;
	}
	if ((flags & PROC_REQUIRE_ALLOW_KERNPROC) && proc == &proc0) {
		return;
	}
	zone_id_require(ZONE_ID_PROC, sizeof(struct proc), proc);
}

int
proc_pid(proc_t p)
{
	if (p != NULL) {
		proc_require(p, PROC_REQUIRE_ALLOW_KERNPROC);
		return p->p_pid;
	}
	return -1;
}

int
proc_ppid(proc_t p)
{
	if (p != NULL) {
		proc_require(p, PROC_REQUIRE_ALLOW_KERNPROC);
		return p->p_ppid;
	}
	return -1;
}

int
proc_original_ppid(proc_t p)
{
	if (p != NULL) {
		proc_require(p, PROC_REQUIRE_ALLOW_KERNPROC);
		return p->p_original_ppid;
	}
	return -1;
}

int
proc_starttime(proc_t p, struct timeval *tv)
{
	if (p != NULL && tv != NULL) {
		tv->tv_sec = p->p_start.tv_sec;
		tv->tv_usec = p->p_start.tv_usec;
		return 0;
	}
	return EINVAL;
}

int
proc_selfpid(void)
{
	return current_proc()->p_pid;
}

int
proc_selfppid(void)
{
	return current_proc()->p_ppid;
}

uint64_t
proc_selfcsflags(void)
{
	return (uint64_t)current_proc()->p_csflags;
}

int
proc_csflags(proc_t p, uint64_t *flags)
{
	if (p && flags) {
		proc_require(p, PROC_REQUIRE_ALLOW_KERNPROC);
		*flags = (uint64_t)p->p_csflags;
		return 0;
	}
	return EINVAL;
}

uint32_t
proc_platform(const proc_t p)
{
	if (p != NULL) {
		return p->p_platform;
	}
	return (uint32_t)-1;
}

uint32_t
proc_min_sdk(proc_t p)
{
	if (p != NULL) {
		return p->p_min_sdk;
	}
	return (uint32_t)-1;
}

uint32_t
proc_sdk(proc_t p)
{
	if (p != NULL) {
		return p->p_sdk;
	}
	return (uint32_t)-1;
}

#if CONFIG_DTRACE
static proc_t
dtrace_current_proc_vforking(void)
{
	thread_t th = current_thread();
	struct uthread *ut = get_bsdthread_info(th);

	if (ut &&
	    ((ut->uu_flag & (UT_VFORK | UT_VFORKING)) == (UT_VFORK | UT_VFORKING))) {
		/*
		 * Handle the narrow window where we're in the vfork syscall,
		 * but we're not quite ready to claim (in particular, to DTrace)
		 * that we're running as the child.
		 */
		return get_bsdtask_info(get_threadtask(th));
	}
	return current_proc();
}

int
dtrace_proc_selfpid(void)
{
	return dtrace_current_proc_vforking()->p_pid;
}

int
dtrace_proc_selfppid(void)
{
	return dtrace_current_proc_vforking()->p_ppid;
}

uid_t
dtrace_proc_selfruid(void)
{
	return dtrace_current_proc_vforking()->p_ruid;
}
#endif /* CONFIG_DTRACE */

proc_t
proc_parent(proc_t p)
{
	proc_t parent;
	proc_t pp;

	proc_list_lock();
loop:
	pp = p->p_pptr;
	parent =  proc_ref_locked(pp);
	if ((parent == PROC_NULL) && (pp != PROC_NULL) && (pp->p_stat != SZOMB) && ((pp->p_listflag & P_LIST_EXITED) != 0) && ((pp->p_listflag & P_LIST_CHILDDRAINED) == 0)) {
		pp->p_listflag |= P_LIST_CHILDLKWAIT;
		msleep(&pp->p_childrencnt, &proc_list_mlock, 0, "proc_parent", 0);
		goto loop;
	}
	proc_list_unlock();
	return parent;
}

static boolean_t
proc_parent_is_currentproc(proc_t p)
{
	boolean_t ret = FALSE;

	proc_list_lock();
	if (p->p_pptr == current_proc()) {
		ret = TRUE;
	}

	proc_list_unlock();
	return ret;
}

void
proc_name(int pid, char * buf, int size)
{
	proc_t p;

	if (size <= 0) {
		return;
	}

	bzero(buf, size);

	if ((p = proc_find(pid)) != PROC_NULL) {
		strlcpy(buf, &p->p_comm[0], size);
		proc_rele(p);
	}
}

void
proc_name_kdp(task_t t, char * buf, int size)
{
	proc_t p = get_bsdtask_info(t);
	if (p == PROC_NULL) {
		return;
	}

	if ((size_t)size > sizeof(p->p_comm)) {
		strlcpy(buf, &p->p_name[0], MIN((int)sizeof(p->p_name), size));
	} else {
		strlcpy(buf, &p->p_comm[0], MIN((int)sizeof(p->p_comm), size));
	}
}

boolean_t
proc_binary_uuid_kdp(task_t task, uuid_t uuid)
{
	proc_t p = get_bsdtask_info(task);
	if (p == PROC_NULL) {
		return FALSE;
	}

	proc_getexecutableuuid(p, uuid, sizeof(uuid_t));

	return TRUE;
}

int
proc_threadname_kdp(void * uth, char * buf, size_t size)
{
	if (size < MAXTHREADNAMESIZE) {
		/* this is really just a protective measure for the future in
		 * case the thread name size in stackshot gets out of sync with
		 * the BSD max thread name size. Note that bsd_getthreadname
		 * doesn't take input buffer size into account. */
		return -1;
	}

	if (uth != NULL) {
		bsd_getthreadname(uth, buf);
	}
	return 0;
}


/* note that this function is generally going to be called from stackshot,
 * and the arguments will be coming from a struct which is declared packed
 * thus the input arguments will in general be unaligned. We have to handle
 * that here. */
void
proc_starttime_kdp(void *p, unaligned_u64 *tv_sec, unaligned_u64 *tv_usec, unaligned_u64 *abstime)
{
	proc_t pp = (proc_t)p;
	if (pp != PROC_NULL) {
		if (tv_sec != NULL) {
			*tv_sec = pp->p_start.tv_sec;
		}
		if (tv_usec != NULL) {
			*tv_usec = pp->p_start.tv_usec;
		}
		if (abstime != NULL) {
			if (pp->p_stats != NULL) {
				*abstime = pp->p_stats->ps_start;
			} else {
				*abstime = 0;
			}
		}
	}
}

void
proc_archinfo_kdp(void* p, cpu_type_t* cputype, cpu_subtype_t* cpusubtype)
{
	proc_t pp = (proc_t)p;
	if (pp != PROC_NULL) {
		*cputype = pp->p_cputype;
		*cpusubtype = pp->p_cpusubtype;
	}
}

char *
proc_name_address(void *p)
{
	return &((proc_t)p)->p_comm[0];
}

char *
proc_longname_address(void *p)
{
	return &((proc_t)p)->p_name[0];
}

char *
proc_best_name(proc_t p)
{
	if (p->p_name[0] != '\0') {
		return &p->p_name[0];
	}
	return &p->p_comm[0];
}

void
proc_selfname(char * buf, int  size)
{
	proc_t p;

	if ((p = current_proc()) != (proc_t)0) {
		strlcpy(buf, &p->p_comm[0], size);
	}
}

void
proc_signal(int pid, int signum)
{
	proc_t p;

	if ((p = proc_find(pid)) != PROC_NULL) {
		psignal(p, signum);
		proc_rele(p);
	}
}

int
proc_issignal(int pid, sigset_t mask)
{
	proc_t p;
	int error = 0;

	if ((p = proc_find(pid)) != PROC_NULL) {
		error = proc_pendingsignals(p, mask);
		proc_rele(p);
	}

	return error;
}

int
proc_noremotehang(proc_t p)
{
	int retval = 0;

	if (p) {
		retval = p->p_flag & P_NOREMOTEHANG;
	}
	return retval? 1: 0;
}

int
proc_exiting(proc_t p)
{
	int retval = 0;

	if (p) {
		retval = p->p_lflag & P_LEXIT;
	}
	return retval? 1: 0;
}

int
proc_in_teardown(proc_t p)
{
	int retval = 0;

	if (p) {
		retval = p->p_lflag & P_LPEXIT;
	}
	return retval? 1: 0;
}

int
proc_forcequota(proc_t p)
{
	int retval = 0;

	if (p) {
		retval = p->p_flag & P_FORCEQUOTA;
	}
	return retval? 1: 0;
}

int
proc_suser(proc_t p)
{
	kauth_cred_t my_cred;
	int error;

	my_cred = kauth_cred_proc_ref(p);
	error = suser(my_cred, &p->p_acflag);
	kauth_cred_unref(&my_cred);
	return error;
}

task_t
proc_task(proc_t proc)
{
	return (task_t)proc->task;
}

/*
 * Obtain the first thread in a process
 *
 * XXX This is a bad thing to do; it exists predominantly to support the
 * XXX use of proc_t's in places that should really be using
 * XXX thread_t's instead.  This maintains historical behaviour, but really
 * XXX needs an audit of the context (proxy vs. not) to clean up.
 */
thread_t
proc_thread(proc_t proc)
{
	LCK_MTX_ASSERT(&proc->p_mlock, LCK_MTX_ASSERT_OWNED);

	uthread_t uth = TAILQ_FIRST(&proc->p_uthlist);

	if (uth != NULL) {
		return uth->uu_context.vc_thread;
	}

	return NULL;
}

kauth_cred_t
proc_ucred(proc_t p)
{
	return p->p_ucred;
}

struct uthread *
current_uthread()
{
	thread_t th = current_thread();

	return (struct uthread *)get_bsdthread_info(th);
}


int
proc_is64bit(proc_t p)
{
	return IS_64BIT_PROCESS(p);
}

int
proc_is64bit_data(proc_t p)
{
	assert(p->task);
	return (int)task_get_64bit_data(p->task);
}

int
proc_isinitproc(proc_t p)
{
	if (initproc == NULL) {
		return 0;
	}
	return p == initproc;
}

int
proc_pidversion(proc_t p)
{
	return p->p_idversion;
}

uint32_t
proc_persona_id(proc_t p)
{
	return (uint32_t)persona_id_from_proc(p);
}

uint32_t
proc_getuid(proc_t p)
{
	return p->p_uid;
}

uint32_t
proc_getgid(proc_t p)
{
	return p->p_gid;
}

uint64_t
proc_uniqueid(proc_t p)
{
	return p->p_uniqueid;
}

uint64_t proc_uniqueid_task(void *p_arg, void *t);
/*
 * During exec, two tasks point at the proc.  This function is used
 * to gives tasks a unique ID; we make the matching task have the
 * proc's uniqueid, and any other task gets the high-bit flipped.
 * (We need to try to avoid returning UINT64_MAX, which is the
 * which is the uniqueid of a task without a proc. (e.g. while exiting))
 *
 * Only used by get_task_uniqueid(); do not add additional callers.
 */
uint64_t
proc_uniqueid_task(void *p_arg, void *t)
{
	proc_t p = p_arg;
	uint64_t uniqueid = p->p_uniqueid;
	return uniqueid ^ (__probable(t == (void *)p->task) ? 0 : (1ull << 63));
}

uint64_t
proc_puniqueid(proc_t p)
{
	return p->p_puniqueid;
}

void
proc_coalitionids(__unused proc_t p, __unused uint64_t ids[COALITION_NUM_TYPES])
{
#if CONFIG_COALITIONS
	task_coalition_ids(p->task, ids);
#else
	memset(ids, 0, sizeof(uint64_t[COALITION_NUM_TYPES]));
#endif
	return;
}

uint64_t
proc_was_throttled(proc_t p)
{
	return p->was_throttled;
}

uint64_t
proc_did_throttle(proc_t p)
{
	return p->did_throttle;
}

int
proc_getcdhash(proc_t p, unsigned char *cdhash)
{
	return vn_getcdhash(p->p_textvp, p->p_textoff, cdhash);
}

int
proc_exitstatus(proc_t p)
{
	return p->p_xstat & 0xffff;
}

void
proc_getexecutableuuid(proc_t p, unsigned char *uuidbuf, unsigned long size)
{
	if (size >= sizeof(p->p_uuid)) {
		memcpy(uuidbuf, p->p_uuid, sizeof(p->p_uuid));
	}
}

/* Return vnode for executable with an iocount. Must be released with vnode_put() */
vnode_t
proc_getexecutablevnode(proc_t p)
{
	vnode_t tvp  = p->p_textvp;

	if (tvp != NULLVP) {
		if (vnode_getwithref(tvp) == 0) {
			return tvp;
		}
	}

	return NULLVP;
}

int
proc_gettty(proc_t p, vnode_t *vp)
{
	if (!p || !vp) {
		return EINVAL;
	}

	struct session *procsp = proc_session(p);
	int err = EINVAL;

	if (procsp != SESSION_NULL) {
		session_lock(procsp);
		vnode_t ttyvp = procsp->s_ttyvp;
		int ttyvid = procsp->s_ttyvid;
		session_unlock(procsp);

		if (ttyvp) {
			if (vnode_getwithvid(ttyvp, ttyvid) == 0) {
				*vp = ttyvp;
				err = 0;
			}
		} else {
			err = ENOENT;
		}

		session_rele(procsp);
	}

	return err;
}

int
proc_gettty_dev(proc_t p, dev_t *dev)
{
	struct session *procsp = proc_session(p);
	boolean_t has_tty = FALSE;

	if (procsp != SESSION_NULL) {
		session_lock(procsp);

		struct tty * tp = SESSION_TP(procsp);
		if (tp != TTY_NULL) {
			*dev = tp->t_dev;
			has_tty = TRUE;
		}

		session_unlock(procsp);
		session_rele(procsp);
	}

	if (has_tty) {
		return 0;
	} else {
		return EINVAL;
	}
}

int
proc_selfexecutableargs(uint8_t *buf, size_t *buflen)
{
	proc_t p = current_proc();

	// buflen must always be provided
	if (buflen == NULL) {
		return EINVAL;
	}

	// If a buf is provided, there must be at least enough room to fit argc
	if (buf && *buflen < sizeof(p->p_argc)) {
		return EINVAL;
	}

	if (!p->user_stack) {
		return EINVAL;
	}

	if (buf == NULL) {
		*buflen = p->p_argslen + sizeof(p->p_argc);
		return 0;
	}

	// Copy in argc to the first 4 bytes
	memcpy(buf, &p->p_argc, sizeof(p->p_argc));

	if (*buflen > sizeof(p->p_argc) && p->p_argslen > 0) {
		// See memory layout comment in kern_exec.c:exec_copyout_strings()
		// We want to copy starting from `p_argslen` bytes away from top of stack
		return copyin(p->user_stack - p->p_argslen,
		           buf + sizeof(p->p_argc),
		           MIN(p->p_argslen, *buflen - sizeof(p->p_argc)));
	} else {
		return 0;
	}
}

off_t
proc_getexecutableoffset(proc_t p)
{
	return p->p_textoff;
}

void
bsd_set_dependency_capable(task_t task)
{
	proc_t p = get_bsdtask_info(task);

	if (p) {
		OSBitOrAtomic(P_DEPENDENCY_CAPABLE, &p->p_flag);
	}
}


#ifndef __arm__
int
IS_64BIT_PROCESS(proc_t p)
{
	if (p && (p->p_flag & P_LP64)) {
		return 1;
	} else {
		return 0;
	}
}
#endif

/*
 * Locate a process by number
 */
proc_t
pfind_locked(pid_t pid)
{
	proc_t p;
#if DEBUG
	proc_t q;
#endif

	if (!pid) {
		return kernproc;
	}

	for (p = PIDHASH(pid)->lh_first; p != 0; p = p->p_hash.le_next) {
		if (p->p_pid == pid) {
#if DEBUG
			for (q = p->p_hash.le_next; q != 0; q = q->p_hash.le_next) {
				if ((p != q) && (q->p_pid == pid)) {
					panic("two procs with same pid %p:%p:%d:%d\n", p, q, p->p_pid, q->p_pid);
				}
			}
#endif
			return p;
		}
	}
	return NULL;
}

/*
 * Locate a zombie by PID
 */
__private_extern__ proc_t
pzfind(pid_t pid)
{
	proc_t p;


	proc_list_lock();

	for (p = zombproc.lh_first; p != 0; p = p->p_list.le_next) {
		if (p->p_pid == pid) {
			break;
		}
	}

	proc_list_unlock();

	return p;
}

/*
 * Locate a process group by number
 */

struct pgrp *
pgfind(pid_t pgid)
{
	struct pgrp * pgrp;

	proc_list_lock();
	pgrp = pgfind_internal(pgid);
	if ((pgrp == NULL) || ((pgrp->pg_listflags & PGRP_FLAG_TERMINATE) != 0)) {
		pgrp = PGRP_NULL;
	} else {
		pgrp->pg_refcount++;
	}
	proc_list_unlock();
	return pgrp;
}



struct pgrp *
pgfind_internal(pid_t pgid)
{
	struct pgrp *pgrp;

	for (pgrp = PGRPHASH(pgid)->lh_first; pgrp != 0; pgrp = pgrp->pg_hash.le_next) {
		if (pgrp->pg_id == pgid) {
			return pgrp;
		}
	}
	return NULL;
}

void
pg_rele(struct pgrp * pgrp)
{
	if (pgrp == PGRP_NULL) {
		return;
	}
	pg_rele_dropref(pgrp);
}

void
pg_rele_dropref(struct pgrp * pgrp)
{
	proc_list_lock();
	if ((pgrp->pg_refcount == 1) && ((pgrp->pg_listflags & PGRP_FLAG_TERMINATE) == PGRP_FLAG_TERMINATE)) {
		proc_list_unlock();
		pgdelete_dropref(pgrp);
		return;
	}

	pgrp->pg_refcount--;
	proc_list_unlock();
}

struct session *
session_find_internal(pid_t sessid)
{
	struct session *sess;

	for (sess = SESSHASH(sessid)->lh_first; sess != 0; sess = sess->s_hash.le_next) {
		if (sess->s_sid == sessid) {
			return sess;
		}
	}
	return NULL;
}


/*
 * Make a new process ready to become a useful member of society by making it
 * visible in all the right places and initialize its own lists to empty.
 *
 * Parameters:	parent			The parent of the process to insert
 *		child			The child process to insert
 *
 * Returns:	(void)
 *
 * Notes:	Insert a child process into the parents process group, assign
 *		the child the parent process pointer and PPID of the parent,
 *		place it on the parents p_children list as a sibling,
 *		initialize its own child list, place it in the allproc list,
 *		insert it in the proper hash bucket, and initialize its
 *		event list.
 */
void
pinsertchild(proc_t parent, proc_t child)
{
	struct pgrp * pg;

	LIST_INIT(&child->p_children);
	child->p_pptr = parent;
	child->p_ppid = parent->p_pid;
	child->p_original_ppid = parent->p_pid;
	child->p_puniqueid = parent->p_uniqueid;
	child->p_xhighbits = 0;

	pg = proc_pgrp(parent);
	pgrp_add(pg, parent, child);
	pg_rele(pg);

	proc_list_lock();

#if CONFIG_MEMORYSTATUS
	memorystatus_add(child, TRUE);
#endif

	parent->p_childrencnt++;
	LIST_INSERT_HEAD(&parent->p_children, child, p_sibling);

	LIST_INSERT_HEAD(&allproc, child, p_list);
	/* mark the completion of proc creation */
	child->p_listflag &= ~P_LIST_INCREATE;

	proc_list_unlock();
}

/*
 * Move p to a new or existing process group (and session)
 *
 * Returns:	0			Success
 *		ESRCH			No such process
 */
int
enterpgrp(proc_t p, pid_t pgid, int mksess)
{
	struct pgrp *pgrp;
	struct pgrp *mypgrp;
	struct session * procsp;

	pgrp = pgfind(pgid);
	mypgrp = proc_pgrp(p);
	procsp = proc_session(p);

#if DIAGNOSTIC
	if (pgrp != NULL && mksess) {   /* firewalls */
		panic("enterpgrp: setsid into non-empty pgrp");
	}
	if (SESS_LEADER(p, procsp)) {
		panic("enterpgrp: session leader attempted setpgrp");
	}
#endif
	if (pgrp == PGRP_NULL) {
		pid_t savepid = p->p_pid;
		proc_t np = PROC_NULL;
		/*
		 * new process group
		 */
#if DIAGNOSTIC
		if (p->p_pid != pgid) {
			panic("enterpgrp: new pgrp and pid != pgid");
		}
#endif
		pgrp = zalloc_flags(pgrp_zone, Z_WAITOK | Z_ZERO);
		if ((np = proc_find(savepid)) == NULL || np != p) {
			if (np != PROC_NULL) {
				proc_rele(np);
			}
			if (mypgrp != PGRP_NULL) {
				pg_rele(mypgrp);
			}
			if (procsp != SESSION_NULL) {
				session_rele(procsp);
			}
			zfree(pgrp_zone, pgrp);
			return ESRCH;
		}
		proc_rele(np);
		if (mksess) {
			struct session *sess;

			/*
			 * new session
			 */
			sess = zalloc_flags(session_zone, Z_WAITOK | Z_ZERO);
			sess->s_leader = p;
			sess->s_sid = p->p_pid;
			sess->s_count = 1;
			sess->s_ttypgrpid = NO_PID;

			lck_mtx_init(&sess->s_mlock, &proc_mlock_grp, &proc_lck_attr);

			bcopy(procsp->s_login, sess->s_login,
			    sizeof(sess->s_login));
			OSBitAndAtomic(~((uint32_t)P_CONTROLT), &p->p_flag);
			proc_list_lock();
			LIST_INSERT_HEAD(SESSHASH(sess->s_sid), sess, s_hash);
			proc_list_unlock();
			pgrp->pg_session = sess;
			p->p_sessionid = sess->s_sid;
#if DIAGNOSTIC
			if (p != current_proc()) {
				panic("enterpgrp: mksession and p != curproc");
			}
#endif
		} else {
			proc_list_lock();
			pgrp->pg_session = procsp;
			p->p_sessionid  = procsp->s_sid;

			if ((pgrp->pg_session->s_listflags & (S_LIST_TERM | S_LIST_DEAD)) != 0) {
				panic("enterpgrp:  providing ref to terminating session ");
			}
			pgrp->pg_session->s_count++;
			proc_list_unlock();
		}
		pgrp->pg_id = pgid;

		lck_mtx_init(&pgrp->pg_mlock, &proc_mlock_grp, &proc_lck_attr);

		LIST_INIT(&pgrp->pg_members);
		proc_list_lock();
		pgrp->pg_refcount = 1;
		LIST_INSERT_HEAD(PGRPHASH(pgid), pgrp, pg_hash);
		proc_list_unlock();
	} else if (pgrp == mypgrp) {
		pg_rele(pgrp);
		if (mypgrp != NULL) {
			pg_rele(mypgrp);
		}
		if (procsp != SESSION_NULL) {
			session_rele(procsp);
		}
		return 0;
	}

	if (procsp != SESSION_NULL) {
		session_rele(procsp);
	}
	/*
	 * Adjust eligibility of affected pgrps to participate in job control.
	 * Increment eligibility counts before decrementing, otherwise we
	 * could reach 0 spuriously during the first call.
	 */
	fixjobc(p, pgrp, 1);
	fixjobc(p, mypgrp, 0);

	if (mypgrp != PGRP_NULL) {
		pg_rele(mypgrp);
	}
	pgrp_replace(p, pgrp);
	pg_rele(pgrp);

	return 0;
}

/*
 * remove process from process group
 */
int
leavepgrp(proc_t p)
{
	pgrp_remove(p);
	return 0;
}

/*
 * delete a process group
 */
static void
pgdelete_dropref(struct pgrp *pgrp)
{
	struct tty *ttyp;
	int emptypgrp  = 1;
	struct session *sessp;


	pgrp_lock(pgrp);
	if (pgrp->pg_membercnt != 0) {
		emptypgrp = 0;
	}
	pgrp_unlock(pgrp);

	proc_list_lock();
	pgrp->pg_refcount--;
	if ((emptypgrp == 0) || (pgrp->pg_membercnt != 0)) {
		proc_list_unlock();
		return;
	}

	pgrp->pg_listflags |= PGRP_FLAG_TERMINATE;

	if (pgrp->pg_refcount > 0) {
		proc_list_unlock();
		return;
	}

	pgrp->pg_listflags |= PGRP_FLAG_DEAD;
	LIST_REMOVE(pgrp, pg_hash);

	proc_list_unlock();

	ttyp = SESSION_TP(pgrp->pg_session);
	if (ttyp != TTY_NULL) {
		if (ttyp->t_pgrp == pgrp) {
			tty_lock(ttyp);
			/* Re-check after acquiring the lock */
			if (ttyp->t_pgrp == pgrp) {
				ttyp->t_pgrp = NULL;
				pgrp->pg_session->s_ttypgrpid = NO_PID;
			}
			tty_unlock(ttyp);
		}
	}

	proc_list_lock();

	sessp = pgrp->pg_session;
	if ((sessp->s_listflags & (S_LIST_TERM | S_LIST_DEAD)) != 0) {
		panic("pg_deleteref: manipulating refs of already terminating session");
	}
	if (--sessp->s_count == 0) {
		if ((sessp->s_listflags & (S_LIST_TERM | S_LIST_DEAD)) != 0) {
			panic("pg_deleteref: terminating already terminated session");
		}
		sessp->s_listflags |= S_LIST_TERM;
		ttyp = SESSION_TP(sessp);
		LIST_REMOVE(sessp, s_hash);
		proc_list_unlock();
		if (ttyp != TTY_NULL) {
			tty_lock(ttyp);
			if (ttyp->t_session == sessp) {
				ttyp->t_session = NULL;
			}
			tty_unlock(ttyp);
		}
		proc_list_lock();
		sessp->s_listflags |= S_LIST_DEAD;
		if (sessp->s_count != 0) {
			panic("pg_deleteref: freeing session in use");
		}
		proc_list_unlock();
		lck_mtx_destroy(&sessp->s_mlock, &proc_mlock_grp);

		zfree(session_zone, sessp);
	} else {
		proc_list_unlock();
	}
	lck_mtx_destroy(&pgrp->pg_mlock, &proc_mlock_grp);
	zfree(pgrp_zone, pgrp);
}


/*
 * Adjust pgrp jobc counters when specified process changes process group.
 * We count the number of processes in each process group that "qualify"
 * the group for terminal job control (those with a parent in a different
 * process group of the same session).  If that count reaches zero, the
 * process group becomes orphaned.  Check both the specified process'
 * process group and that of its children.
 * entering == 0 => p is leaving specified group.
 * entering == 1 => p is entering specified group.
 */
int
fixjob_callback(proc_t p, void * arg)
{
	struct fixjob_iterargs *fp;
	struct pgrp * pg, *hispg;
	struct session * mysession, *hissess;
	int entering;

	fp = (struct fixjob_iterargs *)arg;
	pg = fp->pg;
	mysession = fp->mysession;
	entering = fp->entering;

	hispg = proc_pgrp(p);
	hissess = proc_session(p);

	if ((hispg != pg) &&
	    (hissess == mysession)) {
		pgrp_lock(hispg);
		if (entering) {
			hispg->pg_jobc++;
			pgrp_unlock(hispg);
		} else if (--hispg->pg_jobc == 0) {
			pgrp_unlock(hispg);
			orphanpg(hispg);
		} else {
			pgrp_unlock(hispg);
		}
	}
	if (hissess != SESSION_NULL) {
		session_rele(hissess);
	}
	if (hispg != PGRP_NULL) {
		pg_rele(hispg);
	}

	return PROC_RETURNED;
}

void
fixjobc(proc_t p, struct pgrp *pgrp, int entering)
{
	struct pgrp *hispgrp = PGRP_NULL;
	struct session *hissess = SESSION_NULL;
	struct session *mysession = pgrp->pg_session;
	proc_t parent;
	struct fixjob_iterargs fjarg;
	boolean_t proc_parent_self;

	/*
	 * Check if p's parent is current proc, if yes then no need to take
	 * a ref; calling proc_parent with current proc as parent may
	 * deadlock if current proc is exiting.
	 */
	proc_parent_self = proc_parent_is_currentproc(p);
	if (proc_parent_self) {
		parent = current_proc();
	} else {
		parent = proc_parent(p);
	}

	if (parent != PROC_NULL) {
		hispgrp = proc_pgrp(parent);
		hissess = proc_session(parent);
		if (!proc_parent_self) {
			proc_rele(parent);
		}
	}


	/*
	 * Check p's parent to see whether p qualifies its own process
	 * group; if so, adjust count for p's process group.
	 */
	if ((hispgrp != pgrp) &&
	    (hissess == mysession)) {
		pgrp_lock(pgrp);
		if (entering) {
			pgrp->pg_jobc++;
			pgrp_unlock(pgrp);
		} else if (--pgrp->pg_jobc == 0) {
			pgrp_unlock(pgrp);
			orphanpg(pgrp);
		} else {
			pgrp_unlock(pgrp);
		}
	}

	if (hissess != SESSION_NULL) {
		session_rele(hissess);
	}
	if (hispgrp != PGRP_NULL) {
		pg_rele(hispgrp);
	}

	/*
	 * Check this process' children to see whether they qualify
	 * their process groups; if so, adjust counts for children's
	 * process groups.
	 */
	fjarg.pg = pgrp;
	fjarg.mysession = mysession;
	fjarg.entering = entering;
	proc_childrenwalk(p, fixjob_callback, &fjarg);
}

/*
 * The pidlist_* routines support the functions in this file that
 * walk lists of processes applying filters and callouts to the
 * elements of the list.
 *
 * A prior implementation used a single linear array, which can be
 * tricky to allocate on large systems. This implementation creates
 * an SLIST of modestly sized arrays of PIDS_PER_ENTRY elements.
 *
 * The array should be sized large enough to keep the overhead of
 * walking the list low, but small enough that blocking allocations of
 * pidlist_entry_t structures always succeed.
 */

#define PIDS_PER_ENTRY 1021

typedef struct pidlist_entry {
	SLIST_ENTRY(pidlist_entry) pe_link;
	u_int pe_nused;
	pid_t pe_pid[PIDS_PER_ENTRY];
} pidlist_entry_t;

typedef struct {
	SLIST_HEAD(, pidlist_entry) pl_head;
	struct pidlist_entry *pl_active;
	u_int pl_nalloc;
} pidlist_t;

static __inline__ pidlist_t *
pidlist_init(pidlist_t *pl)
{
	SLIST_INIT(&pl->pl_head);
	pl->pl_active = NULL;
	pl->pl_nalloc = 0;
	return pl;
}

static u_int
pidlist_alloc(pidlist_t *pl, u_int needed)
{
	while (pl->pl_nalloc < needed) {
		pidlist_entry_t *pe = kheap_alloc(KHEAP_TEMP, sizeof(*pe),
		    Z_WAITOK | Z_ZERO);
		if (NULL == pe) {
			panic("no space for pidlist entry");
		}
		SLIST_INSERT_HEAD(&pl->pl_head, pe, pe_link);
		pl->pl_nalloc += (sizeof(pe->pe_pid) / sizeof(pe->pe_pid[0]));
	}
	return pl->pl_nalloc;
}

static void
pidlist_free(pidlist_t *pl)
{
	pidlist_entry_t *pe;
	while (NULL != (pe = SLIST_FIRST(&pl->pl_head))) {
		SLIST_FIRST(&pl->pl_head) = SLIST_NEXT(pe, pe_link);
		kheap_free(KHEAP_TEMP, pe, sizeof(*pe));
	}
	pl->pl_nalloc = 0;
}

static __inline__ void
pidlist_set_active(pidlist_t *pl)
{
	pl->pl_active = SLIST_FIRST(&pl->pl_head);
	assert(pl->pl_active);
}

static void
pidlist_add_pid(pidlist_t *pl, pid_t pid)
{
	pidlist_entry_t *pe = pl->pl_active;
	if (pe->pe_nused >= sizeof(pe->pe_pid) / sizeof(pe->pe_pid[0])) {
		if (NULL == (pe = SLIST_NEXT(pe, pe_link))) {
			panic("pidlist allocation exhausted");
		}
		pl->pl_active = pe;
	}
	pe->pe_pid[pe->pe_nused++] = pid;
}

static __inline__ u_int
pidlist_nalloc(const pidlist_t *pl)
{
	return pl->pl_nalloc;
}

/*
 * A process group has become orphaned; if there are any stopped processes in
 * the group, hang-up all process in that group.
 */
static void
orphanpg(struct pgrp *pgrp)
{
	pidlist_t pid_list, *pl = pidlist_init(&pid_list);
	u_int pid_count_available = 0;
	proc_t p;

	/* allocate outside of the pgrp_lock */
	for (;;) {
		pgrp_lock(pgrp);

		boolean_t should_iterate = FALSE;
		pid_count_available = 0;

		PGMEMBERS_FOREACH(pgrp, p) {
			pid_count_available++;
			if (p->p_stat == SSTOP) {
				should_iterate = TRUE;
			}
		}
		if (pid_count_available == 0 || !should_iterate) {
			pgrp_unlock(pgrp);
			goto out; /* no orphaned processes OR nothing stopped */
		}
		if (pidlist_nalloc(pl) >= pid_count_available) {
			break;
		}
		pgrp_unlock(pgrp);

		pidlist_alloc(pl, pid_count_available);
	}
	pidlist_set_active(pl);

	u_int pid_count = 0;
	PGMEMBERS_FOREACH(pgrp, p) {
		pidlist_add_pid(pl, proc_pid(p));
		if (++pid_count >= pid_count_available) {
			break;
		}
	}
	pgrp_unlock(pgrp);

	const pidlist_entry_t *pe;
	SLIST_FOREACH(pe, &(pl->pl_head), pe_link) {
		for (u_int i = 0; i < pe->pe_nused; i++) {
			const pid_t pid = pe->pe_pid[i];
			if (0 == pid) {
				continue; /* skip kernproc */
			}
			p = proc_find(pid);
			if (!p) {
				continue;
			}
			proc_transwait(p, 0);
			pt_setrunnable(p);
			psignal(p, SIGHUP);
			psignal(p, SIGCONT);
			proc_rele(p);
		}
	}
out:
	pidlist_free(pl);
}

boolean_t
proc_is_translated(proc_t p __unused)
{
	return 0;
}

int
proc_is_classic(proc_t p __unused)
{
	return 0;
}

bool
proc_is_exotic(
	proc_t p)
{
	if (p == NULL) {
		return false;
	}
	return task_is_exotic(proc_task(p));
}

bool
proc_is_alien(
	proc_t p)
{
	if (p == NULL) {
		return false;
	}
	return task_is_alien(proc_task(p));
}

/* XXX Why does this function exist?  Need to kill it off... */
proc_t
current_proc_EXTERNAL(void)
{
	return current_proc();
}

int
proc_is_forcing_hfs_case_sensitivity(proc_t p)
{
	return (p->p_vfs_iopolicy & P_VFS_IOPOLICY_FORCE_HFS_CASE_SENSITIVITY) ? 1 : 0;
}

bool
proc_ignores_content_protection(proc_t p)
{
	return os_atomic_load(&p->p_vfs_iopolicy, relaxed) & P_VFS_IOPOLICY_IGNORE_CONTENT_PROTECTION;
}

bool
proc_ignores_node_permissions(proc_t p)
{
	return os_atomic_load(&p->p_vfs_iopolicy, relaxed) & P_VFS_IOPOLICY_IGNORE_NODE_PERMISSIONS;
}

bool
proc_skip_mtime_update(proc_t p)
{
	return os_atomic_load(&p->p_vfs_iopolicy, relaxed) & P_VFS_IOPOLICY_SKIP_MTIME_UPDATE;
}

#if CONFIG_COREDUMP
/*
 * proc_core_name(name, uid, pid)
 * Expand the name described in corefilename, using name, uid, and pid.
 * corefilename is a printf-like string, with three format specifiers:
 *	%N	name of process ("name")
 *	%P	process id (pid)
 *	%U	user id (uid)
 * For example, "%N.core" is the default; they can be disabled completely
 * by using "/dev/null", or all core files can be stored in "/cores/%U/%N-%P".
 * This is controlled by the sysctl variable kern.corefile (see above).
 */
__private_extern__ int
proc_core_name(const char *name, uid_t uid, pid_t pid, char *cf_name,
    size_t cf_name_len)
{
	const char *format, *appendstr;
	char id_buf[11];                /* Buffer for pid/uid -- max 4B */
	size_t i, l, n;

	if (cf_name == NULL) {
		goto toolong;
	}

	format = corefilename;
	for (i = 0, n = 0; n < cf_name_len && format[i]; i++) {
		switch (format[i]) {
		case '%':       /* Format character */
			i++;
			switch (format[i]) {
			case '%':
				appendstr = "%";
				break;
			case 'N':       /* process name */
				appendstr = name;
				break;
			case 'P':       /* process id */
				snprintf(id_buf, sizeof(id_buf), "%u", pid);
				appendstr = id_buf;
				break;
			case 'U':       /* user id */
				snprintf(id_buf, sizeof(id_buf), "%u", uid);
				appendstr = id_buf;
				break;
			case '\0': /* format string ended in % symbol */
				goto endofstring;
			default:
				appendstr = "";
				log(LOG_ERR,
				    "Unknown format character %c in `%s'\n",
				    format[i], format);
			}
			l = strlen(appendstr);
			if ((n + l) >= cf_name_len) {
				goto toolong;
			}
			bcopy(appendstr, cf_name + n, l);
			n += l;
			break;
		default:
			cf_name[n++] = format[i];
		}
	}
	if (format[i] != '\0') {
		goto toolong;
	}
	return 0;
toolong:
	log(LOG_ERR, "pid %ld (%s), uid (%u): corename is too long\n",
	    (long)pid, name, (uint32_t)uid);
	return 1;
endofstring:
	log(LOG_ERR, "pid %ld (%s), uid (%u): unexpected end of string after %% token\n",
	    (long)pid, name, (uint32_t)uid);
	return 1;
}
#endif /* CONFIG_COREDUMP */

/* Code Signing related routines */

int
csops(__unused proc_t p, struct csops_args *uap, __unused int32_t *retval)
{
	return csops_internal(uap->pid, uap->ops, uap->useraddr,
	           uap->usersize, USER_ADDR_NULL);
}

int
csops_audittoken(__unused proc_t p, struct csops_audittoken_args *uap, __unused int32_t *retval)
{
	if (uap->uaudittoken == USER_ADDR_NULL) {
		return EINVAL;
	}
	return csops_internal(uap->pid, uap->ops, uap->useraddr,
	           uap->usersize, uap->uaudittoken);
}

static int
csops_copy_token(void *start, size_t length, user_size_t usize, user_addr_t uaddr)
{
	char fakeheader[8] = { 0 };
	int error;

	if (usize < sizeof(fakeheader)) {
		return ERANGE;
	}

	/* if no blob, fill in zero header */
	if (NULL == start) {
		start = fakeheader;
		length = sizeof(fakeheader);
	} else if (usize < length) {
		/* ... if input too short, copy out length of entitlement */
		uint32_t length32 = htonl((uint32_t)length);
		memcpy(&fakeheader[4], &length32, sizeof(length32));

		error = copyout(fakeheader, uaddr, sizeof(fakeheader));
		if (error == 0) {
			return ERANGE; /* input buffer to short, ERANGE signals that */
		}
		return error;
	}
	return copyout(start, uaddr, length);
}

static int
csops_internal(pid_t pid, int ops, user_addr_t uaddr, user_size_t usersize, user_addr_t uaudittoken)
{
	size_t usize = (size_t)CAST_DOWN(size_t, usersize);
	proc_t pt;
	int forself;
	int error;
	vnode_t tvp;
	off_t toff;
	unsigned char cdhash[SHA1_RESULTLEN];
	audit_token_t token;
	unsigned int upid = 0, uidversion = 0;

	forself = error = 0;

	if (pid == 0) {
		pid = proc_selfpid();
	}
	if (pid == proc_selfpid()) {
		forself = 1;
	}


	switch (ops) {
	case CS_OPS_STATUS:
	case CS_OPS_CDHASH:
	case CS_OPS_PIDOFFSET:
	case CS_OPS_ENTITLEMENTS_BLOB:
	case CS_OPS_IDENTITY:
	case CS_OPS_BLOB:
	case CS_OPS_TEAMID:
	case CS_OPS_CLEAR_LV:
		break;          /* not restricted to root */
	default:
		if (forself == 0 && kauth_cred_issuser(kauth_cred_get()) != TRUE) {
			return EPERM;
		}
		break;
	}

	pt = proc_find(pid);
	if (pt == PROC_NULL) {
		return ESRCH;
	}

	upid = pt->p_pid;
	uidversion = pt->p_idversion;
	if (uaudittoken != USER_ADDR_NULL) {
		error = copyin(uaudittoken, &token, sizeof(audit_token_t));
		if (error != 0) {
			goto out;
		}
		/* verify the audit token pid/idversion matches with proc */
		if ((token.val[5] != upid) || (token.val[7] != uidversion)) {
			error = ESRCH;
			goto out;
		}
	}

#if CONFIG_MACF
	switch (ops) {
	case CS_OPS_MARKINVALID:
	case CS_OPS_MARKHARD:
	case CS_OPS_MARKKILL:
	case CS_OPS_MARKRESTRICT:
	case CS_OPS_SET_STATUS:
	case CS_OPS_CLEARINSTALLER:
	case CS_OPS_CLEARPLATFORM:
	case CS_OPS_CLEAR_LV:
		if ((error = mac_proc_check_set_cs_info(current_proc(), pt, ops))) {
			goto out;
		}
		break;
	default:
		if ((error = mac_proc_check_get_cs_info(current_proc(), pt, ops))) {
			goto out;
		}
	}
#endif

	switch (ops) {
	case CS_OPS_STATUS: {
		uint32_t retflags;

		proc_lock(pt);
		retflags = pt->p_csflags;
		if (cs_process_enforcement(pt)) {
			retflags |= CS_ENFORCEMENT;
		}
		if (csproc_get_platform_binary(pt)) {
			retflags |= CS_PLATFORM_BINARY;
		}
		if (csproc_get_platform_path(pt)) {
			retflags |= CS_PLATFORM_PATH;
		}
		//Don't return CS_REQUIRE_LV if we turned it on with CS_FORCED_LV but still report CS_FORCED_LV
		if ((pt->p_csflags & CS_FORCED_LV) == CS_FORCED_LV) {
			retflags &= (~CS_REQUIRE_LV);
		}
		proc_unlock(pt);

		if (uaddr != USER_ADDR_NULL) {
			error = copyout(&retflags, uaddr, sizeof(uint32_t));
		}
		break;
	}
	case CS_OPS_MARKINVALID:
		proc_lock(pt);
		if ((pt->p_csflags & CS_VALID) == CS_VALID) {           /* is currently valid */
			pt->p_csflags &= ~CS_VALID;             /* set invalid */
			cs_process_invalidated(pt);
			if ((pt->p_csflags & CS_KILL) == CS_KILL) {
				pt->p_csflags |= CS_KILLED;
				proc_unlock(pt);
				if (cs_debug) {
					printf("CODE SIGNING: marked invalid by pid %d: "
					    "p=%d[%s] honoring CS_KILL, final status 0x%x\n",
					    proc_selfpid(), pt->p_pid, pt->p_comm, pt->p_csflags);
				}
				psignal(pt, SIGKILL);
			} else {
				proc_unlock(pt);
			}
		} else {
			proc_unlock(pt);
		}

		break;

	case CS_OPS_MARKHARD:
		proc_lock(pt);
		pt->p_csflags |= CS_HARD;
		if ((pt->p_csflags & CS_VALID) == 0) {
			/* @@@ allow? reject? kill? @@@ */
			proc_unlock(pt);
			error = EINVAL;
			goto out;
		} else {
			proc_unlock(pt);
		}
		break;

	case CS_OPS_MARKKILL:
		proc_lock(pt);
		pt->p_csflags |= CS_KILL;
		if ((pt->p_csflags & CS_VALID) == 0) {
			proc_unlock(pt);
			psignal(pt, SIGKILL);
		} else {
			proc_unlock(pt);
		}
		break;

	case CS_OPS_PIDOFFSET:
		toff = pt->p_textoff;
		proc_rele(pt);
		error = copyout(&toff, uaddr, sizeof(toff));
		return error;

	case CS_OPS_CDHASH:

		/* pt already holds a reference on its p_textvp */
		tvp = pt->p_textvp;
		toff = pt->p_textoff;

		if (tvp == NULLVP || usize != SHA1_RESULTLEN) {
			proc_rele(pt);
			return EINVAL;
		}

		error = vn_getcdhash(tvp, toff, cdhash);
		proc_rele(pt);

		if (error == 0) {
			error = copyout(cdhash, uaddr, sizeof(cdhash));
		}

		return error;

	case CS_OPS_ENTITLEMENTS_BLOB: {
		void *start;
		size_t length;

		proc_lock(pt);

		if ((pt->p_csflags & (CS_VALID | CS_DEBUGGED)) == 0) {
			proc_unlock(pt);
			error = EINVAL;
			break;
		}

		error = cs_entitlements_blob_get(pt, &start, &length);
		proc_unlock(pt);
		if (error) {
			break;
		}

		error = csops_copy_token(start, length, usize, uaddr);
		break;
	}
	case CS_OPS_MARKRESTRICT:
		proc_lock(pt);
		pt->p_csflags |= CS_RESTRICT;
		proc_unlock(pt);
		break;

	case CS_OPS_SET_STATUS: {
		uint32_t flags;

		if (usize < sizeof(flags)) {
			error = ERANGE;
			break;
		}

		error = copyin(uaddr, &flags, sizeof(flags));
		if (error) {
			break;
		}

		/* only allow setting a subset of all code sign flags */
		flags &=
		    CS_HARD | CS_EXEC_SET_HARD |
		    CS_KILL | CS_EXEC_SET_KILL |
		    CS_RESTRICT |
		    CS_REQUIRE_LV |
		    CS_ENFORCEMENT | CS_EXEC_SET_ENFORCEMENT;

		proc_lock(pt);
		if (pt->p_csflags & CS_VALID) {
			if ((flags & CS_ENFORCEMENT) &&
			    !(pt->p_csflags & CS_ENFORCEMENT)) {
				vm_map_cs_enforcement_set(get_task_map(pt->task), TRUE);
			}
			pt->p_csflags |= flags;
		} else {
			error = EINVAL;
		}
		proc_unlock(pt);

		break;
	}
	case CS_OPS_CLEAR_LV: {
		/*
		 * This option is used to remove library validation from
		 * a running process. This is used in plugin architectures
		 * when a program needs to load untrusted libraries. This
		 * allows the process to maintain library validation as
		 * long as possible, then drop it only when required.
		 * Once a process has loaded the untrusted library,
		 * relying on library validation in the future will
		 * not be effective. An alternative is to re-exec
		 * your application without library validation, or
		 * fork an untrusted child.
		 */
#if !defined(XNU_TARGET_OS_OSX)
		// We only support dropping library validation on macOS
		error = ENOTSUP;
#else
		/*
		 * if we have the flag set, and the caller wants
		 * to remove it, and they're entitled to, then
		 * we remove it from the csflags
		 *
		 * NOTE: We are fine to poke into the task because
		 * we get a ref to pt when we do the proc_find
		 * at the beginning of this function.
		 *
		 * We also only allow altering ourselves.
		 */
		if (forself == 1 && IOTaskHasEntitlement(pt->task, CLEAR_LV_ENTITLEMENT)) {
			proc_lock(pt);
			pt->p_csflags &= (~(CS_REQUIRE_LV | CS_FORCED_LV));
			proc_unlock(pt);
			error = 0;
		} else {
			error = EPERM;
		}
#endif
		break;
	}
	case CS_OPS_BLOB: {
		void *start;
		size_t length;

		proc_lock(pt);
		if ((pt->p_csflags & (CS_VALID | CS_DEBUGGED)) == 0) {
			proc_unlock(pt);
			error = EINVAL;
			break;
		}

		error = cs_blob_get(pt, &start, &length);
		proc_unlock(pt);
		if (error) {
			break;
		}

		error = csops_copy_token(start, length, usize, uaddr);
		break;
	}
	case CS_OPS_IDENTITY:
	case CS_OPS_TEAMID: {
		const char *identity;
		uint8_t fakeheader[8];
		uint32_t idlen;
		size_t length;

		/*
		 * Make identity have a blob header to make it
		 * easier on userland to guess the identity
		 * length.
		 */
		if (usize < sizeof(fakeheader)) {
			error = ERANGE;
			break;
		}
		memset(fakeheader, 0, sizeof(fakeheader));

		proc_lock(pt);
		if ((pt->p_csflags & (CS_VALID | CS_DEBUGGED)) == 0) {
			proc_unlock(pt);
			error = EINVAL;
			break;
		}

		identity = ops == CS_OPS_TEAMID ? csproc_get_teamid(pt) : cs_identity_get(pt);
		proc_unlock(pt);
		if (identity == NULL) {
			error = ENOENT;
			break;
		}

		length = strlen(identity) + 1;         /* include NUL */
		idlen = htonl((uint32_t)(length + sizeof(fakeheader)));
		memcpy(&fakeheader[4], &idlen, sizeof(idlen));

		error = copyout(fakeheader, uaddr, sizeof(fakeheader));
		if (error) {
			break;
		}

		if (usize < sizeof(fakeheader) + length) {
			error = ERANGE;
		} else if (usize > sizeof(fakeheader)) {
			error = copyout(identity, uaddr + sizeof(fakeheader), length);
		}

		break;
	}

	case CS_OPS_CLEARINSTALLER:
		proc_lock(pt);
		pt->p_csflags &= ~(CS_INSTALLER | CS_DATAVAULT_CONTROLLER | CS_EXEC_INHERIT_SIP);
		proc_unlock(pt);
		break;

	case CS_OPS_CLEARPLATFORM:
#if DEVELOPMENT || DEBUG
		if (cs_process_global_enforcement()) {
			error = ENOTSUP;
			break;
		}

#if CONFIG_CSR
		if (csr_check(CSR_ALLOW_APPLE_INTERNAL) != 0) {
			error = ENOTSUP;
			break;
		}
#endif

		proc_lock(pt);
		pt->p_csflags &= ~(CS_PLATFORM_BINARY | CS_PLATFORM_PATH);
		csproc_clear_platform_binary(pt);
		proc_unlock(pt);
		break;
#else
		error = ENOTSUP;
		break;
#endif /* !DEVELOPMENT || DEBUG */

	default:
		error = EINVAL;
		break;
	}
out:
	proc_rele(pt);
	return error;
}

void
proc_iterate(
	unsigned int flags,
	proc_iterate_fn_t callout,
	void *arg,
	proc_iterate_fn_t filterfn,
	void *filterarg)
{
	pidlist_t pid_list, *pl = pidlist_init(&pid_list);
	u_int pid_count_available = 0;

	assert(callout != NULL);

	/* allocate outside of the proc_list_lock */
	for (;;) {
		proc_list_lock();
		pid_count_available = nprocs + 1; /* kernel_task not counted in nprocs */
		assert(pid_count_available > 0);
		if (pidlist_nalloc(pl) >= pid_count_available) {
			break;
		}
		proc_list_unlock();

		pidlist_alloc(pl, pid_count_available);
	}
	pidlist_set_active(pl);

	/* filter pids into the pid_list */

	u_int pid_count = 0;
	if (flags & PROC_ALLPROCLIST) {
		proc_t p;
		ALLPROC_FOREACH(p) {
			/* ignore processes that are being forked */
			if (p->p_stat == SIDL) {
				continue;
			}
			if ((filterfn != NULL) && (filterfn(p, filterarg) == 0)) {
				continue;
			}
			pidlist_add_pid(pl, proc_pid(p));
			if (++pid_count >= pid_count_available) {
				break;
			}
		}
	}

	if ((pid_count < pid_count_available) &&
	    (flags & PROC_ZOMBPROCLIST)) {
		proc_t p;
		ZOMBPROC_FOREACH(p) {
			if ((filterfn != NULL) && (filterfn(p, filterarg) == 0)) {
				continue;
			}
			pidlist_add_pid(pl, proc_pid(p));
			if (++pid_count >= pid_count_available) {
				break;
			}
		}
	}

	proc_list_unlock();

	/* call callout on processes in the pid_list */

	const pidlist_entry_t *pe;
	SLIST_FOREACH(pe, &(pl->pl_head), pe_link) {
		for (u_int i = 0; i < pe->pe_nused; i++) {
			const pid_t pid = pe->pe_pid[i];
			proc_t p = proc_find(pid);
			if (p) {
				if ((flags & PROC_NOWAITTRANS) == 0) {
					proc_transwait(p, 0);
				}
				const int callout_ret = callout(p, arg);

				switch (callout_ret) {
				case PROC_RETURNED_DONE:
					proc_rele(p);
					OS_FALLTHROUGH;
				case PROC_CLAIMED_DONE:
					goto out;

				case PROC_RETURNED:
					proc_rele(p);
					OS_FALLTHROUGH;
				case PROC_CLAIMED:
					break;
				default:
					panic("%s: callout =%d for pid %d",
					    __func__, callout_ret, pid);
					break;
				}
			} else if (flags & PROC_ZOMBPROCLIST) {
				p = proc_find_zombref(pid);
				if (!p) {
					continue;
				}
				const int callout_ret = callout(p, arg);

				switch (callout_ret) {
				case PROC_RETURNED_DONE:
					proc_drop_zombref(p);
					OS_FALLTHROUGH;
				case PROC_CLAIMED_DONE:
					goto out;

				case PROC_RETURNED:
					proc_drop_zombref(p);
					OS_FALLTHROUGH;
				case PROC_CLAIMED:
					break;
				default:
					panic("%s: callout =%d for zombie %d",
					    __func__, callout_ret, pid);
					break;
				}
			}
		}
	}
out:
	pidlist_free(pl);
}

void
proc_rebootscan(
	proc_iterate_fn_t callout,
	void *arg,
	proc_iterate_fn_t filterfn,
	void *filterarg)
{
	proc_t p;

	assert(callout != NULL);

	proc_shutdown_exitcount = 0;

restart_foreach:

	proc_list_lock();

	ALLPROC_FOREACH(p) {
		if ((filterfn != NULL) && filterfn(p, filterarg) == 0) {
			continue;
		}
		p = proc_ref_locked(p);
		if (!p) {
			continue;
		}

		proc_list_unlock();

		proc_transwait(p, 0);
		(void)callout(p, arg);
		proc_rele(p);

		goto restart_foreach;
	}

	proc_list_unlock();
}

void
proc_childrenwalk(
	proc_t parent,
	proc_iterate_fn_t callout,
	void *arg)
{
	pidlist_t pid_list, *pl = pidlist_init(&pid_list);
	u_int pid_count_available = 0;

	assert(parent != NULL);
	assert(callout != NULL);

	for (;;) {
		proc_list_lock();
		pid_count_available = parent->p_childrencnt;
		if (pid_count_available == 0) {
			proc_list_unlock();
			goto out;
		}
		if (pidlist_nalloc(pl) >= pid_count_available) {
			break;
		}
		proc_list_unlock();

		pidlist_alloc(pl, pid_count_available);
	}
	pidlist_set_active(pl);

	u_int pid_count = 0;
	proc_t p;
	PCHILDREN_FOREACH(parent, p) {
		if (p->p_stat == SIDL) {
			continue;
		}
		pidlist_add_pid(pl, proc_pid(p));
		if (++pid_count >= pid_count_available) {
			break;
		}
	}

	proc_list_unlock();

	const pidlist_entry_t *pe;
	SLIST_FOREACH(pe, &(pl->pl_head), pe_link) {
		for (u_int i = 0; i < pe->pe_nused; i++) {
			const pid_t pid = pe->pe_pid[i];
			p = proc_find(pid);
			if (!p) {
				continue;
			}
			const int callout_ret = callout(p, arg);

			switch (callout_ret) {
			case PROC_RETURNED_DONE:
				proc_rele(p);
				OS_FALLTHROUGH;
			case PROC_CLAIMED_DONE:
				goto out;

			case PROC_RETURNED:
				proc_rele(p);
				OS_FALLTHROUGH;
			case PROC_CLAIMED:
				break;
			default:
				panic("%s: callout =%d for pid %d",
				    __func__, callout_ret, pid);
				break;
			}
		}
	}
out:
	pidlist_free(pl);
}

void
pgrp_iterate(
	struct pgrp *pgrp,
	unsigned int flags,
	proc_iterate_fn_t callout,
	void * arg,
	proc_iterate_fn_t filterfn,
	void * filterarg)
{
	pidlist_t pid_list, *pl = pidlist_init(&pid_list);
	u_int pid_count_available = 0;

	assert(pgrp != NULL);
	assert(callout != NULL);

	for (;;) {
		pgrp_lock(pgrp);
		pid_count_available = pgrp->pg_membercnt;
		if (pid_count_available == 0) {
			pgrp_unlock(pgrp);
			if (flags & PGRP_DROPREF) {
				pg_rele(pgrp);
			}
			goto out;
		}
		if (pidlist_nalloc(pl) >= pid_count_available) {
			break;
		}
		pgrp_unlock(pgrp);

		pidlist_alloc(pl, pid_count_available);
	}
	pidlist_set_active(pl);

	const pid_t pgid = pgrp->pg_id;
	u_int pid_count = 0;
	proc_t p;
	PGMEMBERS_FOREACH(pgrp, p) {
		if ((filterfn != NULL) && (filterfn(p, filterarg) == 0)) {
			continue;;
		}
		pidlist_add_pid(pl, proc_pid(p));
		if (++pid_count >= pid_count_available) {
			break;
		}
	}

	pgrp_unlock(pgrp);

	if (flags & PGRP_DROPREF) {
		pg_rele(pgrp);
	}

	const pidlist_entry_t *pe;
	SLIST_FOREACH(pe, &(pl->pl_head), pe_link) {
		for (u_int i = 0; i < pe->pe_nused; i++) {
			const pid_t pid = pe->pe_pid[i];
			if (0 == pid) {
				continue; /* skip kernproc */
			}
			p = proc_find(pid);
			if (!p) {
				continue;
			}
			if (p->p_pgrpid != pgid) {
				proc_rele(p);
				continue;
			}
			const int callout_ret = callout(p, arg);

			switch (callout_ret) {
			case PROC_RETURNED:
				proc_rele(p);
				OS_FALLTHROUGH;
			case PROC_CLAIMED:
				break;
			case PROC_RETURNED_DONE:
				proc_rele(p);
				OS_FALLTHROUGH;
			case PROC_CLAIMED_DONE:
				goto out;

			default:
				panic("%s: callout =%d for pid %d",
				    __func__, callout_ret, pid);
			}
		}
	}

out:
	pidlist_free(pl);
}

static void
pgrp_add(struct pgrp * pgrp, struct proc * parent, struct proc * child)
{
	proc_list_lock();
	child->p_pgrp = pgrp;
	child->p_pgrpid = pgrp->pg_id;
	child->p_sessionid = pgrp->pg_session->s_sid;
	child->p_listflag |= P_LIST_INPGRP;
	/*
	 * When pgrp is being freed , a process can still
	 * request addition using setpgid from bash when
	 * login is terminated (login cycler) return ESRCH
	 * Safe to hold lock due to refcount on pgrp
	 */
	if ((pgrp->pg_listflags & (PGRP_FLAG_TERMINATE | PGRP_FLAG_DEAD)) == PGRP_FLAG_TERMINATE) {
		pgrp->pg_listflags &= ~PGRP_FLAG_TERMINATE;
	}

	if ((pgrp->pg_listflags & PGRP_FLAG_DEAD) == PGRP_FLAG_DEAD) {
		panic("pgrp_add : pgrp is dead adding process");
	}
	proc_list_unlock();

	pgrp_lock(pgrp);
	pgrp->pg_membercnt++;
	if (parent != PROC_NULL) {
		LIST_INSERT_AFTER(parent, child, p_pglist);
	} else {
		LIST_INSERT_HEAD(&pgrp->pg_members, child, p_pglist);
	}
	pgrp_unlock(pgrp);

	proc_list_lock();
	if (((pgrp->pg_listflags & (PGRP_FLAG_TERMINATE | PGRP_FLAG_DEAD)) == PGRP_FLAG_TERMINATE) && (pgrp->pg_membercnt != 0)) {
		pgrp->pg_listflags &= ~PGRP_FLAG_TERMINATE;
	}
	proc_list_unlock();
}

static void
pgrp_remove(struct proc * p)
{
	struct pgrp * pg;

	pg = proc_pgrp(p);

	proc_list_lock();
#if __PROC_INTERNAL_DEBUG
	if ((p->p_listflag & P_LIST_INPGRP) == 0) {
		panic("removing from pglist but no named ref\n");
	}
#endif
	p->p_pgrpid = PGRPID_DEAD;
	p->p_listflag &= ~P_LIST_INPGRP;
	p->p_pgrp = NULL;
	proc_list_unlock();

	if (pg == PGRP_NULL) {
		panic("pgrp_remove: pg is NULL");
	}
	pgrp_lock(pg);
	pg->pg_membercnt--;

	if (pg->pg_membercnt < 0) {
		panic("pgprp: -ve membercnt pgprp:%p p:%p\n", pg, p);
	}

	LIST_REMOVE(p, p_pglist);
	if (pg->pg_members.lh_first == 0) {
		pgrp_unlock(pg);
		pgdelete_dropref(pg);
	} else {
		pgrp_unlock(pg);
		pg_rele(pg);
	}
}


/* cannot use proc_pgrp as it maybe stalled */
static void
pgrp_replace(struct proc * p, struct pgrp * newpg)
{
	struct pgrp * oldpg;



	proc_list_lock();

	while ((p->p_listflag & P_LIST_PGRPTRANS) == P_LIST_PGRPTRANS) {
		p->p_listflag |= P_LIST_PGRPTRWAIT;
		(void)msleep(&p->p_pgrpid, &proc_list_mlock, 0, "proc_pgrp", 0);
	}

	p->p_listflag |= P_LIST_PGRPTRANS;

	oldpg = p->p_pgrp;
	if (oldpg == PGRP_NULL) {
		panic("pgrp_replace: oldpg NULL");
	}
	oldpg->pg_refcount++;
#if __PROC_INTERNAL_DEBUG
	if ((p->p_listflag & P_LIST_INPGRP) == 0) {
		panic("removing from pglist but no named ref\n");
	}
#endif
	p->p_pgrpid = PGRPID_DEAD;
	p->p_listflag &= ~P_LIST_INPGRP;
	p->p_pgrp = NULL;

	proc_list_unlock();

	pgrp_lock(oldpg);
	oldpg->pg_membercnt--;
	if (oldpg->pg_membercnt < 0) {
		panic("pgprp: -ve membercnt pgprp:%p p:%p\n", oldpg, p);
	}
	LIST_REMOVE(p, p_pglist);
	if (oldpg->pg_members.lh_first == 0) {
		pgrp_unlock(oldpg);
		pgdelete_dropref(oldpg);
	} else {
		pgrp_unlock(oldpg);
		pg_rele(oldpg);
	}

	proc_list_lock();
	p->p_pgrp = newpg;
	p->p_pgrpid = newpg->pg_id;
	p->p_sessionid = newpg->pg_session->s_sid;
	p->p_listflag |= P_LIST_INPGRP;
	/*
	 * When pgrp is being freed , a process can still
	 * request addition using setpgid from bash when
	 * login is terminated (login cycler) return ESRCH
	 * Safe to hold lock due to refcount on pgrp
	 */
	if ((newpg->pg_listflags & (PGRP_FLAG_TERMINATE | PGRP_FLAG_DEAD)) == PGRP_FLAG_TERMINATE) {
		newpg->pg_listflags &= ~PGRP_FLAG_TERMINATE;
	}

	if ((newpg->pg_listflags & PGRP_FLAG_DEAD) == PGRP_FLAG_DEAD) {
		panic("pgrp_add : pgrp is dead adding process");
	}
	proc_list_unlock();

	pgrp_lock(newpg);
	newpg->pg_membercnt++;
	LIST_INSERT_HEAD(&newpg->pg_members, p, p_pglist);
	pgrp_unlock(newpg);

	proc_list_lock();
	if (((newpg->pg_listflags & (PGRP_FLAG_TERMINATE | PGRP_FLAG_DEAD)) == PGRP_FLAG_TERMINATE) && (newpg->pg_membercnt != 0)) {
		newpg->pg_listflags &= ~PGRP_FLAG_TERMINATE;
	}

	p->p_listflag &= ~P_LIST_PGRPTRANS;
	if ((p->p_listflag & P_LIST_PGRPTRWAIT) == P_LIST_PGRPTRWAIT) {
		p->p_listflag &= ~P_LIST_PGRPTRWAIT;
		wakeup(&p->p_pgrpid);
	}
	proc_list_unlock();
}

void
pgrp_lock(struct pgrp * pgrp)
{
	lck_mtx_lock(&pgrp->pg_mlock);
}

void
pgrp_unlock(struct pgrp * pgrp)
{
	lck_mtx_unlock(&pgrp->pg_mlock);
}

void
session_lock(struct session * sess)
{
	lck_mtx_lock(&sess->s_mlock);
}


void
session_unlock(struct session * sess)
{
	lck_mtx_unlock(&sess->s_mlock);
}

struct pgrp *
proc_pgrp(proc_t p)
{
	struct pgrp * pgrp;

	if (p == PROC_NULL) {
		return PGRP_NULL;
	}
	proc_list_lock();

	while ((p->p_listflag & P_LIST_PGRPTRANS) == P_LIST_PGRPTRANS) {
		p->p_listflag |= P_LIST_PGRPTRWAIT;
		(void)msleep(&p->p_pgrpid, &proc_list_mlock, 0, "proc_pgrp", 0);
	}

	pgrp = p->p_pgrp;

	assert(pgrp != NULL);

	if (pgrp != PGRP_NULL) {
		pgrp->pg_refcount++;
		if ((pgrp->pg_listflags & (PGRP_FLAG_TERMINATE | PGRP_FLAG_DEAD)) != 0) {
			panic("proc_pgrp: ref being povided for dead pgrp");
		}
	}

	proc_list_unlock();

	return pgrp;
}

struct pgrp *
tty_pgrp(struct tty * tp)
{
	struct pgrp * pg = PGRP_NULL;

	proc_list_lock();
	pg = tp->t_pgrp;

	if (pg != PGRP_NULL) {
		if ((pg->pg_listflags & PGRP_FLAG_DEAD) != 0) {
			panic("tty_pgrp: ref being povided for dead pgrp");
		}
		pg->pg_refcount++;
	}
	proc_list_unlock();

	return pg;
}

struct session *
proc_session(proc_t p)
{
	struct session * sess = SESSION_NULL;

	if (p == PROC_NULL) {
		return SESSION_NULL;
	}

	proc_list_lock();

	/* wait during transitions */
	while ((p->p_listflag & P_LIST_PGRPTRANS) == P_LIST_PGRPTRANS) {
		p->p_listflag |= P_LIST_PGRPTRWAIT;
		(void)msleep(&p->p_pgrpid, &proc_list_mlock, 0, "proc_pgrp", 0);
	}

	if ((p->p_pgrp != PGRP_NULL) && ((sess = p->p_pgrp->pg_session) != SESSION_NULL)) {
		if ((sess->s_listflags & (S_LIST_TERM | S_LIST_DEAD)) != 0) {
			panic("proc_session:returning sesssion ref on terminating session");
		}
		sess->s_count++;
	}
	proc_list_unlock();
	return sess;
}

void
session_rele(struct session *sess)
{
	proc_list_lock();
	if (--sess->s_count == 0) {
		if ((sess->s_listflags & (S_LIST_TERM | S_LIST_DEAD)) != 0) {
			panic("session_rele: terminating already terminated session");
		}
		sess->s_listflags |= S_LIST_TERM;
		LIST_REMOVE(sess, s_hash);
		sess->s_listflags |= S_LIST_DEAD;
		if (sess->s_count != 0) {
			panic("session_rele: freeing session in use");
		}
		proc_list_unlock();
		lck_mtx_destroy(&sess->s_mlock, &proc_mlock_grp);
		zfree(session_zone, sess);
	} else {
		proc_list_unlock();
	}
}

int
proc_transstart(proc_t p, int locked, int non_blocking)
{
	if (locked == 0) {
		proc_lock(p);
	}
	while ((p->p_lflag & P_LINTRANSIT) == P_LINTRANSIT) {
		if (((p->p_lflag & P_LTRANSCOMMIT) == P_LTRANSCOMMIT) || non_blocking) {
			if (locked == 0) {
				proc_unlock(p);
			}
			return EDEADLK;
		}
		p->p_lflag |= P_LTRANSWAIT;
		msleep(&p->p_lflag, &p->p_mlock, 0, "proc_signstart", NULL);
	}
	p->p_lflag |= P_LINTRANSIT;
	p->p_transholder = current_thread();
	if (locked == 0) {
		proc_unlock(p);
	}
	return 0;
}

void
proc_transcommit(proc_t p, int locked)
{
	if (locked == 0) {
		proc_lock(p);
	}

	assert((p->p_lflag & P_LINTRANSIT) == P_LINTRANSIT);
	assert(p->p_transholder == current_thread());
	p->p_lflag |= P_LTRANSCOMMIT;

	if ((p->p_lflag & P_LTRANSWAIT) == P_LTRANSWAIT) {
		p->p_lflag &= ~P_LTRANSWAIT;
		wakeup(&p->p_lflag);
	}
	if (locked == 0) {
		proc_unlock(p);
	}
}

void
proc_transend(proc_t p, int locked)
{
	if (locked == 0) {
		proc_lock(p);
	}

	p->p_lflag &= ~(P_LINTRANSIT | P_LTRANSCOMMIT);
	p->p_transholder = NULL;

	if ((p->p_lflag & P_LTRANSWAIT) == P_LTRANSWAIT) {
		p->p_lflag &= ~P_LTRANSWAIT;
		wakeup(&p->p_lflag);
	}
	if (locked == 0) {
		proc_unlock(p);
	}
}

int
proc_transwait(proc_t p, int locked)
{
	if (locked == 0) {
		proc_lock(p);
	}
	while ((p->p_lflag & P_LINTRANSIT) == P_LINTRANSIT) {
		if ((p->p_lflag & P_LTRANSCOMMIT) == P_LTRANSCOMMIT && current_proc() == p) {
			if (locked == 0) {
				proc_unlock(p);
			}
			return EDEADLK;
		}
		p->p_lflag |= P_LTRANSWAIT;
		msleep(&p->p_lflag, &p->p_mlock, 0, "proc_signstart", NULL);
	}
	if (locked == 0) {
		proc_unlock(p);
	}
	return 0;
}

void
proc_klist_lock(void)
{
	lck_mtx_lock(&proc_klist_mlock);
}

void
proc_klist_unlock(void)
{
	lck_mtx_unlock(&proc_klist_mlock);
}

void
proc_knote(struct proc * p, long hint)
{
	proc_klist_lock();
	KNOTE(&p->p_klist, hint);
	proc_klist_unlock();
}

void
proc_knote_drain(struct proc *p)
{
	struct knote *kn = NULL;

	/*
	 * Clear the proc's klist to avoid references after the proc is reaped.
	 */
	proc_klist_lock();
	while ((kn = SLIST_FIRST(&p->p_klist))) {
		kn->kn_proc = PROC_NULL;
		KNOTE_DETACH(&p->p_klist, kn);
	}
	proc_klist_unlock();
}

void
proc_setregister(proc_t p)
{
	proc_lock(p);
	p->p_lflag |= P_LREGISTER;
	proc_unlock(p);
}

void
proc_resetregister(proc_t p)
{
	proc_lock(p);
	p->p_lflag &= ~P_LREGISTER;
	proc_unlock(p);
}

bool
proc_get_pthread_jit_allowlist(proc_t p)
{
	bool ret = false;

	proc_lock(p);
	ret = (p->p_lflag & P_LPTHREADJITALLOWLIST);
	proc_unlock(p);

	return ret;
}

void
proc_set_pthread_jit_allowlist(proc_t p)
{
	proc_lock(p);
	p->p_lflag |= P_LPTHREADJITALLOWLIST;
	proc_unlock(p);
}

pid_t
proc_pgrpid(proc_t p)
{
	return p->p_pgrpid;
}

pid_t
proc_sessionid(proc_t p)
{
	return p->p_sessionid;
}

pid_t
proc_selfpgrpid()
{
	return current_proc()->p_pgrpid;
}


/* return control and action states */
int
proc_getpcontrol(int pid, int * pcontrolp)
{
	proc_t p;

	p = proc_find(pid);
	if (p == PROC_NULL) {
		return ESRCH;
	}
	if (pcontrolp != NULL) {
		*pcontrolp = p->p_pcaction;
	}

	proc_rele(p);
	return 0;
}

int
proc_dopcontrol(proc_t p)
{
	int pcontrol;
	os_reason_t kill_reason;

	proc_lock(p);

	pcontrol = PROC_CONTROL_STATE(p);

	if (PROC_ACTION_STATE(p) == 0) {
		switch (pcontrol) {
		case P_PCTHROTTLE:
			PROC_SETACTION_STATE(p);
			proc_unlock(p);
			printf("low swap: throttling pid %d (%s)\n", p->p_pid, p->p_comm);
			break;

		case P_PCSUSP:
			PROC_SETACTION_STATE(p);
			proc_unlock(p);
			printf("low swap: suspending pid %d (%s)\n", p->p_pid, p->p_comm);
			task_suspend(p->task);
			break;

		case P_PCKILL:
			PROC_SETACTION_STATE(p);
			proc_unlock(p);
			printf("low swap: killing pid %d (%s)\n", p->p_pid, p->p_comm);
			kill_reason = os_reason_create(OS_REASON_JETSAM, JETSAM_REASON_LOWSWAP);
			psignal_with_reason(p, SIGKILL, kill_reason);
			break;

		default:
			proc_unlock(p);
		}
	} else {
		proc_unlock(p);
	}

	return PROC_RETURNED;
}


/*
 * Resume a throttled or suspended process.  This is an internal interface that's only
 * used by the user level code that presents the GUI when we run out of swap space and
 * hence is restricted to processes with superuser privileges.
 */

int
proc_resetpcontrol(int pid)
{
	proc_t p;
	int pcontrol;
	int error;
	proc_t self = current_proc();

	/* if the process has been validated to handle resource control or root is valid one */
	if (((self->p_lflag & P_LVMRSRCOWNER) == 0) && (error = suser(kauth_cred_get(), 0))) {
		return error;
	}

	p = proc_find(pid);
	if (p == PROC_NULL) {
		return ESRCH;
	}

	proc_lock(p);

	pcontrol = PROC_CONTROL_STATE(p);

	if (PROC_ACTION_STATE(p) != 0) {
		switch (pcontrol) {
		case P_PCTHROTTLE:
			PROC_RESETACTION_STATE(p);
			proc_unlock(p);
			printf("low swap: unthrottling pid %d (%s)\n", p->p_pid, p->p_comm);
			break;

		case P_PCSUSP:
			PROC_RESETACTION_STATE(p);
			proc_unlock(p);
			printf("low swap: resuming pid %d (%s)\n", p->p_pid, p->p_comm);
			task_resume(p->task);
			break;

		case P_PCKILL:
			/* Huh? */
			PROC_SETACTION_STATE(p);
			proc_unlock(p);
			printf("low swap: attempt to unkill pid %d (%s) ignored\n", p->p_pid, p->p_comm);
			break;

		default:
			proc_unlock(p);
		}
	} else {
		proc_unlock(p);
	}

	proc_rele(p);
	return 0;
}



struct no_paging_space {
	uint64_t        pcs_max_size;
	uint64_t        pcs_uniqueid;
	int             pcs_pid;
	int             pcs_proc_count;
	uint64_t        pcs_total_size;

	uint64_t        npcs_max_size;
	uint64_t        npcs_uniqueid;
	int             npcs_pid;
	int             npcs_proc_count;
	uint64_t        npcs_total_size;

	int             apcs_proc_count;
	uint64_t        apcs_total_size;
};


static int
proc_pcontrol_filter(proc_t p, void *arg)
{
	struct no_paging_space *nps;
	uint64_t        compressed;

	nps = (struct no_paging_space *)arg;

	compressed = get_task_compressed(p->task);

	if (PROC_CONTROL_STATE(p)) {
		if (PROC_ACTION_STATE(p) == 0) {
			if (compressed > nps->pcs_max_size) {
				nps->pcs_pid = p->p_pid;
				nps->pcs_uniqueid = p->p_uniqueid;
				nps->pcs_max_size = compressed;
			}
			nps->pcs_total_size += compressed;
			nps->pcs_proc_count++;
		} else {
			nps->apcs_total_size += compressed;
			nps->apcs_proc_count++;
		}
	} else {
		if (compressed > nps->npcs_max_size) {
			nps->npcs_pid = p->p_pid;
			nps->npcs_uniqueid = p->p_uniqueid;
			nps->npcs_max_size = compressed;
		}
		nps->npcs_total_size += compressed;
		nps->npcs_proc_count++;
	}
	return 0;
}


static int
proc_pcontrol_null(__unused proc_t p, __unused void *arg)
{
	return PROC_RETURNED;
}


/*
 * Deal with the low on compressor pool space condition... this function
 * gets called when we are approaching the limits of the compressor pool or
 * we are unable to create a new swap file.
 * Since this eventually creates a memory deadlock situtation, we need to take action to free up
 * memory resources (both compressed and uncompressed) in order to prevent the system from hanging completely.
 * There are 2 categories of processes to deal with.  Those that have an action
 * associated with them by the task itself and those that do not.  Actionable
 * tasks can have one of three categories specified:  ones that
 * can be killed immediately, ones that should be suspended, and ones that should
 * be throttled.  Processes that do not have an action associated with them are normally
 * ignored unless they are utilizing such a large percentage of the compressor pool (currently 50%)
 * that only by killing them can we hope to put the system back into a usable state.
 */

#define NO_PAGING_SPACE_DEBUG   0

extern uint64_t vm_compressor_pages_compressed(void);

struct timeval  last_no_space_action = {.tv_sec = 0, .tv_usec = 0};

#define MB_SIZE (1024 * 1024ULL)
boolean_t       memorystatus_kill_on_VM_compressor_space_shortage(boolean_t);

extern int32_t  max_kill_priority;
extern int      memorystatus_get_proccnt_upto_priority(int32_t max_bucket_index);

int
no_paging_space_action()
{
	proc_t          p;
	struct no_paging_space nps;
	struct timeval  now;
	os_reason_t kill_reason;

	/*
	 * Throttle how often we come through here.  Once every 5 seconds should be plenty.
	 */
	microtime(&now);

	if (now.tv_sec <= last_no_space_action.tv_sec + 5) {
		return 0;
	}

	/*
	 * Examine all processes and find the biggest (biggest is based on the number of pages this
	 * task has in the compressor pool) that has been marked to have some action
	 * taken when swap space runs out... we also find the biggest that hasn't been marked for
	 * action.
	 *
	 * If the biggest non-actionable task is over the "dangerously big" threashold (currently 50% of
	 * the total number of pages held by the compressor, we go ahead and kill it since no other task
	 * can have any real effect on the situation.  Otherwise, we go after the actionable process.
	 */
	bzero(&nps, sizeof(nps));

	proc_iterate(PROC_ALLPROCLIST, proc_pcontrol_null, (void *)NULL, proc_pcontrol_filter, (void *)&nps);

#if NO_PAGING_SPACE_DEBUG
	printf("low swap: npcs_proc_count = %d, npcs_total_size = %qd, npcs_max_size = %qd\n",
	    nps.npcs_proc_count, nps.npcs_total_size, nps.npcs_max_size);
	printf("low swap: pcs_proc_count = %d, pcs_total_size = %qd, pcs_max_size = %qd\n",
	    nps.pcs_proc_count, nps.pcs_total_size, nps.pcs_max_size);
	printf("low swap: apcs_proc_count = %d, apcs_total_size = %qd\n",
	    nps.apcs_proc_count, nps.apcs_total_size);
#endif
	if (nps.npcs_max_size > (vm_compressor_pages_compressed() * 50) / 100) {
		/*
		 * for now we'll knock out any task that has more then 50% of the pages
		 * held by the compressor
		 */
		if ((p = proc_find(nps.npcs_pid)) != PROC_NULL) {
			if (nps.npcs_uniqueid == p->p_uniqueid) {
				/*
				 * verify this is still the same process
				 * in case the proc exited and the pid got reused while
				 * we were finishing the proc_iterate and getting to this point
				 */
				last_no_space_action = now;

				printf("low swap: killing largest compressed process with pid %d (%s) and size %llu MB\n", p->p_pid, p->p_comm, (nps.pcs_max_size / MB_SIZE));
				kill_reason = os_reason_create(OS_REASON_JETSAM, JETSAM_REASON_LOWSWAP);
				psignal_with_reason(p, SIGKILL, kill_reason);

				proc_rele(p);

				return 0;
			}

			proc_rele(p);
		}
	}

	/*
	 * We have some processes within our jetsam bands of consideration and hence can be killed.
	 * So we will invoke the memorystatus thread to go ahead and kill something.
	 */
	if (memorystatus_get_proccnt_upto_priority(max_kill_priority) > 0) {
		last_no_space_action = now;
		memorystatus_kill_on_VM_compressor_space_shortage(TRUE /* async */);
		return 1;
	}

	/*
	 * No eligible processes to kill. So let's suspend/kill the largest
	 * process depending on its policy control specifications.
	 */

	if (nps.pcs_max_size > 0) {
		if ((p = proc_find(nps.pcs_pid)) != PROC_NULL) {
			if (nps.pcs_uniqueid == p->p_uniqueid) {
				/*
				 * verify this is still the same process
				 * in case the proc exited and the pid got reused while
				 * we were finishing the proc_iterate and getting to this point
				 */
				last_no_space_action = now;

				proc_dopcontrol(p);

				proc_rele(p);

				return 1;
			}

			proc_rele(p);
		}
	}
	last_no_space_action = now;

	printf("low swap: unable to find any eligible processes to take action on\n");

	return 0;
}

int
proc_trace_log(__unused proc_t p, struct proc_trace_log_args *uap, __unused int *retval)
{
	int ret = 0;
	proc_t target_proc = PROC_NULL;
	pid_t target_pid = uap->pid;
	uint64_t target_uniqueid = uap->uniqueid;
	task_t target_task = NULL;

	if (priv_check_cred(kauth_cred_get(), PRIV_PROC_TRACE_INSPECT, 0)) {
		ret = EPERM;
		goto out;
	}
	target_proc = proc_find(target_pid);
	if (target_proc != PROC_NULL) {
		if (target_uniqueid != proc_uniqueid(target_proc)) {
			ret = ENOENT;
			goto out;
		}

		target_task = proc_task(target_proc);
		if (task_send_trace_memory(target_task, target_pid, target_uniqueid)) {
			ret = EINVAL;
			goto out;
		}
	} else {
		ret = ENOENT;
	}

out:
	if (target_proc != PROC_NULL) {
		proc_rele(target_proc);
	}
	return ret;
}

#if VM_SCAN_FOR_SHADOW_CHAIN
extern int vm_map_shadow_max(vm_map_t map);
int proc_shadow_max(void);
int
proc_shadow_max(void)
{
	int             retval, max;
	proc_t          p;
	task_t          task;
	vm_map_t        map;

	max = 0;
	proc_list_lock();
	for (p = allproc.lh_first; (p != 0); p = p->p_list.le_next) {
		if (p->p_stat == SIDL) {
			continue;
		}
		task = p->task;
		if (task == NULL) {
			continue;
		}
		map = get_task_map(task);
		if (map == NULL) {
			continue;
		}
		retval = vm_map_shadow_max(map);
		if (retval > max) {
			max = retval;
		}
	}
	proc_list_unlock();
	return max;
}
#endif /* VM_SCAN_FOR_SHADOW_CHAIN */

void proc_set_responsible_pid(proc_t target_proc, pid_t responsible_pid);
void
proc_set_responsible_pid(proc_t target_proc, pid_t responsible_pid)
{
	if (target_proc != NULL) {
		target_proc->p_responsible_pid = responsible_pid;
	}
	return;
}

int
proc_chrooted(proc_t p)
{
	int retval = 0;

	if (p) {
		proc_fdlock(p);
		retval = (p->p_fd->fd_rdir != NULL) ? 1 : 0;
		proc_fdunlock(p);
	}

	return retval;
}

boolean_t
proc_send_synchronous_EXC_RESOURCE(proc_t p)
{
	if (p == PROC_NULL) {
		return FALSE;
	}

	/* Send sync EXC_RESOURCE if the process is traced */
	if (ISSET(p->p_lflag, P_LTRACED)) {
		return TRUE;
	}
	return FALSE;
}

#if CONFIG_MACF
size_t
proc_get_syscall_filter_mask_size(int which)
{
	switch (which) {
	case SYSCALL_MASK_UNIX:
		return nsysent;
	case SYSCALL_MASK_MACH:
		return mach_trap_count;
	case SYSCALL_MASK_KOBJ:
		return mach_kobj_count;
	default:
		return 0;
	}
}

int
proc_set_syscall_filter_mask(proc_t p, int which, unsigned char *maskptr, size_t masklen)
{
#if DEVELOPMENT || DEBUG
	if (syscallfilter_disable) {
		printf("proc_set_syscall_filter_mask: attempt to set policy for pid %d, but disabled by boot-arg\n", proc_pid(p));
		return 0;
	}
#endif // DEVELOPMENT || DEBUG

	switch (which) {
	case SYSCALL_MASK_UNIX:
		if (maskptr != NULL && masklen != nsysent) {
			return EINVAL;
		}
		p->syscall_filter_mask = maskptr;
		break;
	case SYSCALL_MASK_MACH:
		if (maskptr != NULL && masklen != (size_t)mach_trap_count) {
			return EINVAL;
		}
		mac_task_set_mach_filter_mask(p->task, maskptr);
		break;
	case SYSCALL_MASK_KOBJ:
		if (maskptr != NULL && masklen != (size_t)mach_kobj_count) {
			return EINVAL;
		}
		mac_task_set_kobj_filter_mask(p->task, maskptr);
		break;
	default:
		return EINVAL;
	}

	return 0;
}

int
proc_set_syscall_filter_callbacks(syscall_filter_cbs_t cbs)
{
	if (cbs->version != SYSCALL_FILTER_CALLBACK_VERSION) {
		return EINVAL;
	}

	/* XXX register unix filter callback instead of using MACF hook. */

	if (cbs->mach_filter_cbfunc || cbs->kobj_filter_cbfunc) {
		if (mac_task_register_filter_callbacks(cbs->mach_filter_cbfunc,
		    cbs->kobj_filter_cbfunc) != 0) {
			return EPERM;
		}
	}

	return 0;
}

int
proc_set_syscall_filter_index(int which, int num, int index)
{
	switch (which) {
	case SYSCALL_MASK_KOBJ:
		if (ipc_kobject_set_kobjidx(num, index) != 0) {
			return ENOENT;
		}
		break;
	default:
		return EINVAL;
	}

	return 0;
}
#endif /* CONFIG_MACF */

int
proc_set_filter_message_flag(proc_t p, boolean_t flag)
{
	if (p == PROC_NULL) {
		return EINVAL;
	}

	task_set_filter_msg_flag(proc_task(p), flag);

	return 0;
}

int
proc_get_filter_message_flag(proc_t p, boolean_t *flag)
{
	if (p == PROC_NULL || flag == NULL) {
		return EINVAL;
	}

	*flag = task_get_filter_msg_flag(proc_task(p));

	return 0;
}

bool
proc_is_traced(proc_t p)
{
	bool ret = FALSE;
	assert(p != PROC_NULL);
	proc_lock(p);
	if (p->p_lflag & P_LTRACED) {
		ret = TRUE;
	}
	proc_unlock(p);
	return ret;
}

#ifdef CONFIG_32BIT_TELEMETRY
void
proc_log_32bit_telemetry(proc_t p)
{
	/* Gather info */
	char signature_buf[MAX_32BIT_EXEC_SIG_SIZE] = { 0 };
	char * signature_cur_end = &signature_buf[0];
	char * signature_buf_end = &signature_buf[MAX_32BIT_EXEC_SIG_SIZE - 1];
	int bytes_printed = 0;

	const char * teamid = NULL;
	const char * identity = NULL;
	struct cs_blob * csblob = NULL;

	proc_list_lock();

	/*
	 * Get proc name and parent proc name; if the parent execs, we'll get a
	 * garbled name.
	 */
	bytes_printed = scnprintf(signature_cur_end,
	    signature_buf_end - signature_cur_end,
	    "%s,%s,", p->p_name,
	    (p->p_pptr ? p->p_pptr->p_name : ""));

	if (bytes_printed > 0) {
		signature_cur_end += bytes_printed;
	}

	proc_list_unlock();

	/* Get developer info. */
	vnode_t v = proc_getexecutablevnode(p);

	if (v) {
		csblob = csvnode_get_blob(v, 0);

		if (csblob) {
			teamid = csblob_get_teamid(csblob);
			identity = csblob_get_identity(csblob);
		}
	}

	if (teamid == NULL) {
		teamid = "";
	}

	if (identity == NULL) {
		identity = "";
	}

	bytes_printed = scnprintf(signature_cur_end,
	    signature_buf_end - signature_cur_end,
	    "%s,%s", teamid, identity);

	if (bytes_printed > 0) {
		signature_cur_end += bytes_printed;
	}

	if (v) {
		vnode_put(v);
	}

	/*
	 * We may want to rate limit here, although the SUMMARIZE key should
	 * help us aggregate events in userspace.
	 */

	/* Emit log */
	kern_asl_msg(LOG_DEBUG, "messagetracer", 3,
	    /* 0 */ "com.apple.message.domain", "com.apple.kernel.32bit_exec",
	    /* 1 */ "com.apple.message.signature", signature_buf,
	    /* 2 */ "com.apple.message.summarize", "YES",
	    NULL);
}
#endif /* CONFIG_32BIT_TELEMETRY */
