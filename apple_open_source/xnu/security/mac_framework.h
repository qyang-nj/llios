/*
 * Copyright (c) 2007 Apple Inc. All rights reserved.
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
/*-
 * Copyright (c) 1999-2002 Robert N. M. Watson
 * Copyright (c) 2001-2005 Networks Associates Technology, Inc.
 * Copyright (c) 2005-2007 SPARTA, Inc.
 * All rights reserved.
 *
 * This software was developed by Robert Watson for the TrustedBSD Project.
 *
 * This software was developed for the FreeBSD Project in part by Network
 * Associates Laboratories, the Security Research Division of Network
 * Associates, Inc. under DARPA/SPAWAR contract N66001-01-C-8035 ("CBOSS"),
 * as part of the DARPA CHATS research program.
 *
 * This software was enhanced by SPARTA ISSO under SPAWAR contract
 * N66001-04-C-6019 ("SEFOS").
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/sys/mac.h,v 1.40 2003/04/18 19:57:37 rwatson Exp $
 *
 */
/*
 * Kernel interface for Mandatory Access Control -- how kernel services
 * interact with the TrustedBSD MAC Framework.
 */

#ifndef _SECURITY_MAC_FRAMEWORK_H_
#define _SECURITY_MAC_FRAMEWORK_H_

#ifndef KERNEL
#error "no user-serviceable parts inside"
#endif

#ifndef PRIVATE
#warning "MAC policy is not KPI, see Technical Q&A QA1574, this header will be removed in next version"
#endif

struct attrlist;
struct auditinfo;
struct componentname;
struct cs_blob;
struct devnode;
struct exception_action;
struct flock;
struct fdescnode;
struct fileglob;
struct fileproc;
struct ifreq;
struct image_params;
struct ipc_port;
struct knote;
struct mac;
struct msg;
struct msqid_kernel;
struct mount;
struct pipe;
struct proc;
struct pseminfo;
struct pshminfo;
struct semid_kernel;
struct shmid_kernel;
struct sockaddr;
struct sockopt;
struct socket;
struct task;
struct thread;
struct timespec;
struct tty;
struct ucred;
struct uio;
struct uthread;
struct vfs_attr;
struct vfs_context;
struct vnode;
struct vnode_attr;
struct vop_setlabel_args;

#include <stdbool.h>
#include <sys/kauth.h>
#include <sys/kernel_types.h>

#if CONFIG_MACF

#ifndef __IOKIT_PORTS_DEFINED__
#define __IOKIT_PORTS_DEFINED__
#ifdef __cplusplus
class OSObject;
typedef OSObject *io_object_t;
#else
struct OSObject;
typedef struct OSObject *io_object_t;
#endif
#endif /* __IOKIT_PORTS_DEFINED__ */

/*@ macros */
#define VNODE_LABEL_CREATE      1

/*@ === */
int     mac_audit_check_postselect(kauth_cred_t cred, unsigned short syscode,
    void *args, int error, int retval, int mac_forced) __result_use_check;
int     mac_audit_check_preselect(kauth_cred_t cred, unsigned short syscode,
    void *args) __result_use_check;
int     mac_cred_check_label_update(kauth_cred_t cred,
    struct label *newlabel) __result_use_check;
int     mac_cred_check_label_update_execve(vfs_context_t ctx,
    struct vnode *vp, off_t offset, struct vnode *scriptvp,
    struct label *scriptvnodelabel, struct label *execlabel,
    proc_t proc, void *macextensions) __result_use_check;
int     mac_cred_check_visible(kauth_cred_t u1, kauth_cred_t u2) __result_use_check;
struct label    *mac_cred_label_alloc(void);
void    mac_cred_label_associate(kauth_cred_t cred_parent,
    kauth_cred_t cred_child);
void    mac_cred_label_associate_fork(kauth_cred_t cred, proc_t child);
void    mac_cred_label_associate_kernel(kauth_cred_t cred);
void    mac_cred_label_associate_user(kauth_cred_t cred);
void    mac_cred_label_destroy(kauth_cred_t cred);
int     mac_cred_label_externalize_audit(proc_t p, struct mac *mac) __result_use_check;
void    mac_cred_label_free(struct label *label);
void    mac_cred_label_init(kauth_cred_t cred);
bool    mac_cred_label_is_equal(const struct label *a, const struct label *b) __result_use_check;
uint32_t mac_cred_label_hash_update(const struct label *a, uint32_t hash);
void    mac_cred_label_update(kauth_cred_t cred, struct label *newlabel);
void    mac_cred_label_update_execve(vfs_context_t ctx, kauth_cred_t newcred,
    struct vnode *vp, off_t offset, struct vnode *scriptvp,
    struct label *scriptvnodelabel, struct label *execlabel, u_int *csflags,
    void *macextensions, int *disjoint, int *labelupdateerror);
void    mac_devfs_label_associate_device(dev_t dev, struct devnode *de,
    const char *fullpath);
void    mac_devfs_label_associate_directory(const char *dirname, int dirnamelen,
    struct devnode *de, const char *fullpath);
void    mac_devfs_label_copy(struct label *, struct label *label);
void    mac_devfs_label_destroy(struct devnode *de);
void    mac_devfs_label_init(struct devnode *de);
void    mac_devfs_label_update(struct mount *mp, struct devnode *de,
    struct vnode *vp);
int     mac_execve_enter(user_addr_t mac_p, struct image_params *imgp) __result_use_check;
int     mac_file_check_change_offset(kauth_cred_t cred, struct fileglob *fg) __result_use_check;
int     mac_file_check_create(kauth_cred_t cred) __result_use_check;
int     mac_file_check_dup(kauth_cred_t cred, struct fileglob *fg, int newfd) __result_use_check;
int     mac_file_check_fcntl(kauth_cred_t cred, struct fileglob *fg, int cmd,
    user_long_t arg) __result_use_check;
int     mac_file_check_get(kauth_cred_t cred, struct fileglob *fg,
    char *elements, size_t len) __result_use_check;
int     mac_file_check_get_offset(kauth_cred_t cred, struct fileglob *fg) __result_use_check;
int     mac_file_check_inherit(kauth_cred_t cred, struct fileglob *fg) __result_use_check;
int     mac_file_check_ioctl(kauth_cred_t cred, struct fileglob *fg,
    unsigned long cmd) __result_use_check;
int     mac_file_check_lock(kauth_cred_t cred, struct fileglob *fg, int op,
    struct flock *fl) __result_use_check;
int     mac_file_check_library_validation(struct proc *proc,
    struct fileglob *fg, off_t slice_offset,
    user_long_t error_message, size_t error_message_size) __result_use_check;
int     mac_file_check_mmap(kauth_cred_t cred, struct fileglob *fg,
    int prot, int flags, uint64_t file_pos, int *maxprot) __result_use_check;
void    mac_file_check_mmap_downgrade(kauth_cred_t cred, struct fileglob *fg,
    int *prot);
int     mac_file_check_receive(kauth_cred_t cred, struct fileglob *fg) __result_use_check;
int     mac_file_check_set(kauth_cred_t cred, struct fileglob *fg,
    char *bufp, size_t buflen) __result_use_check;
void    mac_file_notify_close(struct ucred *cred, struct fileglob *fg);
void    mac_file_label_associate(kauth_cred_t cred, struct fileglob *fg);
void    mac_file_label_destroy(struct fileglob *fg);
void    mac_file_label_init(struct fileglob *fg);
int     mac_iokit_check_open_service(kauth_cred_t cred, io_object_t service, unsigned int user_client_type) __result_use_check;
int     mac_iokit_check_open(kauth_cred_t cred, io_object_t user_client, unsigned int user_client_type) __result_use_check;
int     mac_iokit_check_set_properties(kauth_cred_t cred, io_object_t registry_entry, io_object_t properties) __result_use_check;
int     mac_iokit_check_filter_properties(kauth_cred_t cred, io_object_t registry_entry) __result_use_check;
int     mac_iokit_check_get_property(kauth_cred_t cred, io_object_t registry_entry, const char *name) __result_use_check;
#ifdef KERNEL_PRIVATE
int     mac_iokit_check_hid_control(kauth_cred_t cred) __result_use_check;
#endif
int     mac_mount_check_fsctl(vfs_context_t ctx, struct mount *mp,
    unsigned long cmd) __result_use_check;
int     mac_mount_check_getattr(vfs_context_t ctx, struct mount *mp,
    struct vfs_attr *vfa) __result_use_check;
int     mac_mount_check_label_update(vfs_context_t ctx, struct mount *mp) __result_use_check;
int     mac_mount_check_mount(vfs_context_t ctx, struct vnode *vp,
    struct componentname *cnp, const char *vfc_name) __result_use_check;
int     mac_mount_check_mount_late(vfs_context_t ctx, struct mount *mp) __result_use_check;
int     mac_mount_check_quotactl(vfs_context_t ctx, struct mount *mp,
    int cmd, int id) __result_use_check;
int     mac_mount_check_snapshot_create(vfs_context_t ctx, struct mount *mp,
    const char *name) __result_use_check;
int     mac_mount_check_snapshot_delete(vfs_context_t ctx, struct mount *mp,
    const char *name) __result_use_check;
#ifdef KERNEL_PRIVATE
int     mac_mount_check_snapshot_mount(vfs_context_t ctx, struct vnode *rvp,
    struct vnode *vp, struct componentname *cnp, const char *name,
    const char *vfc_name) __result_use_check;
#endif
int     mac_mount_check_snapshot_revert(vfs_context_t ctx, struct mount *mp,
    const char *name) __result_use_check;
int     mac_mount_check_remount(vfs_context_t ctx, struct mount *mp) __result_use_check;
int     mac_mount_check_setattr(vfs_context_t ctx, struct mount *mp,
    struct vfs_attr *vfa) __result_use_check;
int     mac_mount_check_stat(vfs_context_t ctx, struct mount *mp) __result_use_check;
int     mac_mount_check_umount(vfs_context_t ctx, struct mount *mp) __result_use_check;
void    mac_mount_label_associate(vfs_context_t ctx, struct mount *mp);
void    mac_mount_label_destroy(struct mount *mp);
int     mac_mount_label_externalize(struct label *label, char *elements,
    char *outbuf, size_t outbuflen) __result_use_check;
int     mac_mount_label_get(struct mount *mp, user_addr_t mac_p) __result_use_check;
void    mac_mount_label_init(struct mount *);
int     mac_mount_label_internalize(struct label *, char *string) __result_use_check;
int     mac_necp_check_open(proc_t proc, int flags) __result_use_check;
int     mac_necp_check_client_action(proc_t proc, struct fileglob *fg, uint32_t action) __result_use_check;
int     mac_pipe_check_ioctl(kauth_cred_t cred, struct pipe *cpipe,
    unsigned long cmd) __result_use_check;
int     mac_pipe_check_kqfilter(kauth_cred_t cred, struct knote *kn,
    struct pipe *cpipe) __result_use_check;
int     mac_pipe_check_read(kauth_cred_t cred, struct pipe *cpipe) __result_use_check;
int     mac_pipe_check_select(kauth_cred_t cred, struct pipe *cpipe,
    int which) __result_use_check;
int     mac_pipe_check_stat(kauth_cred_t cred, struct pipe *cpipe) __result_use_check;
int     mac_pipe_check_write(kauth_cred_t cred, struct pipe *cpipe) __result_use_check;
struct label    *mac_pipe_label_alloc(void);
void    mac_pipe_label_associate(kauth_cred_t cred, struct pipe *cpipe);
void    mac_pipe_label_destroy(struct pipe *cpipe);
void    mac_pipe_label_free(struct label *label);
void    mac_pipe_label_init(struct pipe *cpipe);
void    mac_policy_initbsd(void);
int     mac_posixsem_check_create(kauth_cred_t cred, const char *name) __result_use_check;
int     mac_posixsem_check_open(kauth_cred_t cred, struct pseminfo *psem) __result_use_check;
int     mac_posixsem_check_post(kauth_cred_t cred, struct pseminfo *psem) __result_use_check;
int     mac_posixsem_check_unlink(kauth_cred_t cred, struct pseminfo *psem,
    const char *name) __result_use_check;
int     mac_posixsem_check_wait(kauth_cred_t cred, struct pseminfo *psem) __result_use_check;
void    mac_posixsem_vnode_label_associate(kauth_cred_t cred,
    struct pseminfo *psem, struct label *plabel,
    vnode_t vp, struct label *vlabel);
void    mac_posixsem_label_associate(kauth_cred_t cred,
    struct pseminfo *psem, const char *name);
void    mac_posixsem_label_destroy(struct pseminfo *psem);
void    mac_posixsem_label_init(struct pseminfo *psem);
int     mac_posixshm_check_create(kauth_cred_t cred, const char *name) __result_use_check;
int     mac_posixshm_check_mmap(kauth_cred_t cred, struct pshminfo *pshm,
    int prot, int flags) __result_use_check;
int     mac_posixshm_check_open(kauth_cred_t cred, struct pshminfo *pshm,
    int fflags) __result_use_check;
int     mac_posixshm_check_stat(kauth_cred_t cred, struct pshminfo *pshm) __result_use_check;
int     mac_posixshm_check_truncate(kauth_cred_t cred, struct pshminfo *pshm,
    off_t s) __result_use_check;
int     mac_posixshm_check_unlink(kauth_cred_t cred, struct pshminfo *pshm,
    const char *name) __result_use_check;
void    mac_posixshm_vnode_label_associate(kauth_cred_t cred,
    struct pshminfo *pshm, struct label *plabel,
    vnode_t vp, struct label *vlabel);
void    mac_posixshm_label_associate(kauth_cred_t cred,
    struct pshminfo *pshm, const char *name);
void    mac_posixshm_label_destroy(struct pshminfo *pshm);
void    mac_posixshm_label_init(struct pshminfo *pshm);
int     mac_priv_check(kauth_cred_t cred, int priv) __result_use_check;
int     mac_priv_grant(kauth_cred_t cred, int priv) __result_use_check;
int     mac_proc_check_debug(proc_ident_t tracing_ident, kauth_cred_t tracing_cred, proc_ident_t traced_ident) __result_use_check;
int     mac_proc_check_dump_core(proc_t proc) __result_use_check;
int     mac_proc_check_proc_info(proc_t curp, proc_t target, int callnum, int flavor) __result_use_check;
int     mac_proc_check_get_cs_info(proc_t curp, proc_t target, unsigned int op) __result_use_check;
int     mac_proc_check_set_cs_info(proc_t curp, proc_t target, unsigned int op) __result_use_check;
int     mac_proc_check_fork(proc_t proc) __result_use_check;
int     mac_proc_check_suspend_resume(proc_t proc, int sr) __result_use_check;
int     mac_proc_check_get_task(kauth_cred_t cred, proc_ident_t pident, mach_task_flavor_t flavor) __result_use_check;
int     mac_proc_check_expose_task(kauth_cred_t cred, proc_ident_t pident, mach_task_flavor_t flavor) __result_use_check;
int     mac_proc_check_get_movable_control_port(void) __result_use_check;
int     mac_proc_check_inherit_ipc_ports(struct proc *p, struct vnode *cur_vp, off_t cur_offset, struct vnode *img_vp, off_t img_offset, struct vnode *scriptvp) __result_use_check;
int     mac_proc_check_getaudit(proc_t proc) __result_use_check;
int     mac_proc_check_getauid(proc_t proc) __result_use_check;
int     mac_proc_check_getlcid(proc_t proc1, proc_t proc2,
    pid_t pid) __result_use_check;
int     mac_proc_check_dyld_process_info_notify_register(void) __result_use_check;
int     mac_proc_check_ledger(proc_t curp, proc_t target, int op) __result_use_check;
int     mac_proc_check_map_anon(proc_t proc, user_addr_t u_addr,
    user_size_t u_size, int prot, int flags, int *maxprot) __result_use_check;
int     mac_proc_check_memorystatus_control(proc_t proc, uint32_t command, pid_t pid) __result_use_check;
int     mac_proc_check_mprotect(proc_t proc,
    user_addr_t addr, user_size_t size, int prot) __result_use_check;
int     mac_proc_check_run_cs_invalid(proc_t proc) __result_use_check;
void    mac_proc_notify_cs_invalidated(proc_t proc);
int     mac_proc_check_sched(proc_t proc, proc_t proc2) __result_use_check;
int     mac_proc_check_setaudit(proc_t proc, struct auditinfo_addr *ai) __result_use_check;
int     mac_proc_check_setauid(proc_t proc, uid_t auid) __result_use_check;
int     mac_proc_check_setlcid(proc_t proc1, proc_t proc2,
    pid_t pid1, pid_t pid2) __result_use_check;
int     mac_proc_check_signal(proc_t proc1, proc_t proc2,
    int signum) __result_use_check;
int     mac_proc_check_syscall_unix(proc_t proc, int scnum) __result_use_check;
int     mac_proc_check_wait(proc_t proc1, proc_t proc2) __result_use_check;
int     mac_proc_check_work_interval_ctl(proc_t proc, uint32_t operation) __result_use_check;
void    mac_proc_notify_exit(proc_t proc);
int     mac_socket_check_accept(kauth_cred_t cred, struct socket *so) __result_use_check;
int     mac_socket_check_accepted(kauth_cred_t cred, struct socket *so) __result_use_check;
int     mac_socket_check_bind(kauth_cred_t cred, struct socket *so,
    struct sockaddr *addr) __result_use_check;
int     mac_socket_check_connect(kauth_cred_t cred, struct socket *so,
    struct sockaddr *addr) __result_use_check;
int     mac_socket_check_create(kauth_cred_t cred, int domain,
    int type, int protocol) __result_use_check;
int     mac_socket_check_ioctl(kauth_cred_t cred, struct socket *so,
    unsigned long cmd) __result_use_check;
int     mac_socket_check_listen(kauth_cred_t cred, struct socket *so) __result_use_check;
int     mac_socket_check_receive(kauth_cred_t cred, struct socket *so) __result_use_check;
int     mac_socket_check_received(kauth_cred_t cred, struct socket *so,
    struct sockaddr *saddr) __result_use_check;
int     mac_socket_check_send(kauth_cred_t cred, struct socket *so,
    struct sockaddr *addr) __result_use_check;
int     mac_socket_check_getsockopt(kauth_cred_t cred, struct socket *so,
    struct sockopt *sopt) __result_use_check;
int     mac_socket_check_setsockopt(kauth_cred_t cred, struct socket *so,
    struct sockopt *sopt) __result_use_check;
int     mac_socket_check_stat(kauth_cred_t cred, struct socket *so) __result_use_check;
void    mac_socket_label_associate(kauth_cred_t cred, struct socket *so);
void    mac_socket_label_associate_accept(struct socket *oldsocket,
    struct socket *newsocket);
void    mac_socket_label_copy(struct label *from, struct label *to);
void    mac_socket_label_destroy(struct socket *);
int     mac_socket_label_get(kauth_cred_t cred, struct socket *so,
    struct mac *extmac) __result_use_check;
int     mac_socket_label_init(struct socket *, int waitok) __result_use_check;
void    mac_socketpeer_label_associate_socket(struct socket *peersocket,
    struct socket *socket_to_modify);
int     mac_socketpeer_label_get(kauth_cred_t cred, struct socket *so,
    struct mac *extmac) __result_use_check;
int     mac_system_check_acct(kauth_cred_t cred, struct vnode *vp) __result_use_check;
int     mac_system_check_audit(kauth_cred_t cred, void *record, int length) __result_use_check;
int     mac_system_check_auditctl(kauth_cred_t cred, struct vnode *vp) __result_use_check;
int     mac_system_check_auditon(kauth_cred_t cred, int cmd) __result_use_check;
int     mac_system_check_host_priv(kauth_cred_t cred) __result_use_check;
int     mac_system_check_info(kauth_cred_t, const char *info_type) __result_use_check;
int     mac_system_check_nfsd(kauth_cred_t cred) __result_use_check;
int     mac_system_check_reboot(kauth_cred_t cred, int howto) __result_use_check;
int     mac_system_check_settime(kauth_cred_t cred) __result_use_check;
int     mac_system_check_swapoff(kauth_cred_t cred, struct vnode *vp) __result_use_check;
int     mac_system_check_swapon(kauth_cred_t cred, struct vnode *vp) __result_use_check;
int     mac_system_check_sysctlbyname(kauth_cred_t cred, const char *namestring, int *name,
    size_t namelen, user_addr_t oldctl, size_t oldlen,
    user_addr_t newctl, size_t newlen) __result_use_check;
int     mac_system_check_kas_info(kauth_cred_t cred, int selector) __result_use_check;
void    mac_sysvmsg_label_associate(kauth_cred_t cred,
    struct msqid_kernel *msqptr, struct msg *msgptr);
void    mac_sysvmsg_label_init(struct msg *msgptr);
void    mac_sysvmsg_label_recycle(struct msg *msgptr);
int     mac_sysvmsq_check_enqueue(kauth_cred_t cred, struct msg *msgptr,
    struct msqid_kernel *msqptr) __result_use_check;
int     mac_sysvmsq_check_msgrcv(kauth_cred_t cred, struct msg *msgptr) __result_use_check;
int     mac_sysvmsq_check_msgrmid(kauth_cred_t cred, struct msg *msgptr) __result_use_check;
int     mac_sysvmsq_check_msqctl(kauth_cred_t cred,
    struct msqid_kernel *msqptr, int cmd) __result_use_check;
int     mac_sysvmsq_check_msqget(kauth_cred_t cred,
    struct msqid_kernel *msqptr) __result_use_check;
int     mac_sysvmsq_check_msqrcv(kauth_cred_t cred,
    struct msqid_kernel *msqptr) __result_use_check;
int     mac_sysvmsq_check_msqsnd(kauth_cred_t cred,
    struct msqid_kernel *msqptr) __result_use_check;
void    mac_sysvmsq_label_associate(kauth_cred_t cred,
    struct msqid_kernel *msqptr);
void    mac_sysvmsq_label_init(struct msqid_kernel *msqptr);
void    mac_sysvmsq_label_recycle(struct msqid_kernel *msqptr);
int     mac_sysvsem_check_semctl(kauth_cred_t cred,
    struct semid_kernel *semakptr, int cmd) __result_use_check;
int     mac_sysvsem_check_semget(kauth_cred_t cred,
    struct semid_kernel *semakptr) __result_use_check;
int     mac_sysvsem_check_semop(kauth_cred_t cred,
    struct semid_kernel *semakptr, size_t accesstype) __result_use_check;
void    mac_sysvsem_label_associate(kauth_cred_t cred,
    struct semid_kernel *semakptr);
void    mac_sysvsem_label_destroy(struct semid_kernel *semakptr);
void    mac_sysvsem_label_init(struct semid_kernel *semakptr);
void    mac_sysvsem_label_recycle(struct semid_kernel *semakptr);
int     mac_sysvshm_check_shmat(kauth_cred_t cred,
    struct shmid_kernel *shmsegptr, int shmflg) __result_use_check;
int     mac_sysvshm_check_shmctl(kauth_cred_t cred,
    struct shmid_kernel *shmsegptr, int cmd) __result_use_check;
int     mac_sysvshm_check_shmdt(kauth_cred_t cred,
    struct shmid_kernel *shmsegptr) __result_use_check;
int     mac_sysvshm_check_shmget(kauth_cred_t cred,
    struct shmid_kernel *shmsegptr, int shmflg) __result_use_check;
void    mac_sysvshm_label_associate(kauth_cred_t cred,
    struct shmid_kernel *shmsegptr);
void    mac_sysvshm_label_destroy(struct shmid_kernel *shmsegptr);
void    mac_sysvshm_label_init(struct shmid_kernel* shmsegptr);
void    mac_sysvshm_label_recycle(struct shmid_kernel *shmsegptr);
int     mac_vnode_check_access(vfs_context_t ctx, struct vnode *vp,
    int acc_mode) __result_use_check;
int     mac_vnode_check_chdir(vfs_context_t ctx, struct vnode *dvp) __result_use_check;
int     mac_vnode_check_chroot(vfs_context_t ctx, struct vnode *dvp,
    struct componentname *cnp) __result_use_check;
int     mac_vnode_check_clone(vfs_context_t ctx, struct vnode *dvp,
    struct vnode *vp, struct componentname *cnp) __result_use_check;
int     mac_vnode_check_create(vfs_context_t ctx, struct vnode *dvp,
    struct componentname *cnp, struct vnode_attr *vap) __result_use_check;
int     mac_vnode_check_deleteextattr(vfs_context_t ctx, struct vnode *vp,
    const char *name) __result_use_check;
int     mac_vnode_check_exchangedata(vfs_context_t ctx, struct vnode *v1,
    struct vnode *v2) __result_use_check;
int     mac_vnode_check_exec(vfs_context_t ctx, struct vnode *vp,
    struct image_params *imgp) __result_use_check;
int     mac_vnode_check_fsgetpath(vfs_context_t ctx, struct vnode *vp) __result_use_check;
int     mac_vnode_check_getattr(vfs_context_t ctx, struct ucred *file_cred,
    struct vnode *vp, struct vnode_attr *va) __result_use_check;
int     mac_vnode_check_getattrlist(vfs_context_t ctx, struct vnode *vp,
    struct attrlist *alist, uint64_t options) __result_use_check;
int     mac_vnode_check_getattrlistbulk(vfs_context_t ctx, struct vnode *dvp,
    struct attrlist *alist, uint64_t options) __result_use_check;
int     mac_vnode_check_getextattr(vfs_context_t ctx, struct vnode *vp,
    const char *name, struct uio *uio) __result_use_check;
int     mac_vnode_check_ioctl(vfs_context_t ctx, struct vnode *vp,
    unsigned long cmd) __result_use_check;
int     mac_vnode_check_kqfilter(vfs_context_t ctx,
    kauth_cred_t file_cred, struct knote *kn, struct vnode *vp) __result_use_check;
int     mac_vnode_check_label_update(vfs_context_t ctx, struct vnode *vp,
    struct label *newlabel); __result_use_check
int     mac_vnode_check_link(vfs_context_t ctx, struct vnode *dvp,
    struct vnode *vp, struct componentname *cnp) __result_use_check;
int     mac_vnode_check_listextattr(vfs_context_t ctx, struct vnode *vp) __result_use_check;
int     mac_vnode_check_lookup(vfs_context_t ctx, struct vnode *dvp,
    struct componentname *cnp) __result_use_check;
int     mac_vnode_check_lookup_preflight(vfs_context_t ctx, struct vnode *dvp,
    const char *path, size_t pathlen) __result_use_check;
int     mac_vnode_check_open(vfs_context_t ctx, struct vnode *vp,
    int acc_mode) __result_use_check;
int     mac_vnode_check_read(vfs_context_t ctx,
    kauth_cred_t file_cred, struct vnode *vp) __result_use_check;
int     mac_vnode_check_readdir(vfs_context_t ctx, struct vnode *vp) __result_use_check;
int     mac_vnode_check_readlink(vfs_context_t ctx, struct vnode *vp) __result_use_check;
int     mac_vnode_check_rename(vfs_context_t ctx, struct vnode *dvp,
    struct vnode *vp, struct componentname *cnp, struct vnode *tdvp,
    struct vnode *tvp, struct componentname *tcnp) __result_use_check;
int     mac_vnode_check_revoke(vfs_context_t ctx, struct vnode *vp) __result_use_check;
int     mac_vnode_check_searchfs(vfs_context_t ctx, struct vnode *vp,
    struct attrlist *returnattrs, struct attrlist *searchattrs) __result_use_check;
int     mac_vnode_check_select(vfs_context_t ctx, struct vnode *vp,
    int which) __result_use_check;
int     mac_vnode_check_setacl(vfs_context_t ctx, struct vnode *vp,
    struct kauth_acl *acl) __result_use_check;
int     mac_vnode_check_setattrlist(vfs_context_t ctxd, struct vnode *vp,
    struct attrlist *alist) __result_use_check;
int     mac_vnode_check_setextattr(vfs_context_t ctx, struct vnode *vp,
    const char *name, struct uio *uio) __result_use_check;
int     mac_vnode_check_setflags(vfs_context_t ctx, struct vnode *vp,
    u_long flags) __result_use_check;
int     mac_vnode_check_setmode(vfs_context_t ctx, struct vnode *vp,
    mode_t mode) __result_use_check;
int     mac_vnode_check_setowner(vfs_context_t ctx, struct vnode *vp,
    uid_t uid, gid_t gid) __result_use_check;
int     mac_vnode_check_setutimes(vfs_context_t ctx, struct vnode *vp,
    struct timespec atime, struct timespec mtime) __result_use_check;
int     mac_vnode_check_signature(struct vnode *vp,
    struct cs_blob *cs_blob, struct image_params *imgp,
    unsigned int *cs_flags, unsigned int *signer_type,
    int flags, unsigned int platform) __result_use_check;
int     mac_vnode_check_supplemental_signature(struct vnode *vp,
    struct cs_blob *cs_blob, struct vnode *linked_vp,
    struct cs_blob *linked_cs_blob, unsigned int *signer_type) __result_use_check;
int     mac_vnode_check_stat(vfs_context_t ctx,
    kauth_cred_t file_cred, struct vnode *vp) __result_use_check;
#ifdef KERNEL_PRIVATE
int     mac_vnode_check_trigger_resolve(vfs_context_t ctx, struct vnode *dvp,
    struct componentname *cnp) __result_use_check;
#endif
int     mac_vnode_check_truncate(vfs_context_t ctx,
    kauth_cred_t file_cred, struct vnode *vp) __result_use_check;
int     mac_vnode_check_uipc_bind(vfs_context_t ctx, struct vnode *dvp,
    struct componentname *cnp, struct vnode_attr *vap) __result_use_check;
int     mac_vnode_check_uipc_connect(vfs_context_t ctx, struct vnode *vp, struct socket *so) __result_use_check;
int     mac_vnode_check_unlink(vfs_context_t ctx, struct vnode *dvp,
    struct vnode *vp, struct componentname *cnp) __result_use_check;
int     mac_vnode_check_write(vfs_context_t ctx,
    kauth_cred_t file_cred, struct vnode *vp) __result_use_check;
struct label    *mac_vnode_label_alloc(void);
int     mac_vnode_label_associate(struct mount *mp, struct vnode *vp,
    vfs_context_t ctx) __result_use_check;
void    mac_vnode_label_associate_devfs(struct mount *mp, struct devnode *de,
    struct vnode *vp);
int     mac_vnode_label_associate_extattr(struct mount *mp, struct vnode *vp) __result_use_check;
int     mac_vnode_label_associate_fdesc(struct mount *mp, struct fdescnode *fnp,
    struct vnode *vp, vfs_context_t ctx) __result_use_check;
void    mac_vnode_label_associate_singlelabel(struct mount *mp,
    struct vnode *vp);
void    mac_vnode_label_copy(struct label *l1, struct label *l2);
void    mac_vnode_label_destroy(struct vnode *vp);
int     mac_vnode_label_externalize_audit(struct vnode *vp, struct mac *mac) __result_use_check;
void    mac_vnode_label_free(struct label *label);
void    mac_vnode_label_init(struct vnode *vp);
int     mac_vnode_label_init_needed(struct vnode *vp) __result_use_check;
#ifdef KERNEL_PRIVATE
struct label *mac_vnode_label_allocate(vnode_t vp);
#endif
void    mac_vnode_label_recycle(struct vnode *vp);
void    mac_vnode_label_update(vfs_context_t ctx, struct vnode *vp,
    struct label *newlabel);
void    mac_vnode_label_update_extattr(struct mount *mp, struct vnode *vp,
    const char *name);
int     mac_vnode_notify_create(vfs_context_t ctx, struct mount *mp,
    struct vnode *dvp, struct vnode *vp, struct componentname *cnp) __result_use_check;
void    mac_vnode_notify_deleteextattr(vfs_context_t ctx, struct vnode *vp, const char *name);
void    mac_vnode_notify_link(vfs_context_t ctx, struct vnode *vp,
    struct vnode *dvp, struct componentname *cnp);
void    mac_vnode_notify_open(vfs_context_t ctx, struct vnode *vp, int acc_flags);
void    mac_vnode_notify_rename(vfs_context_t ctx, struct vnode *vp,
    struct vnode *dvp, struct componentname *cnp);
void    mac_vnode_notify_setacl(vfs_context_t ctx, struct vnode *vp, struct kauth_acl *acl);
void    mac_vnode_notify_setattrlist(vfs_context_t ctx, struct vnode *vp, struct attrlist *alist);
void    mac_vnode_notify_setextattr(vfs_context_t ctx, struct vnode *vp, const char *name, struct uio *uio);
void    mac_vnode_notify_setflags(vfs_context_t ctx, struct vnode *vp, u_long flags);
void    mac_vnode_notify_setmode(vfs_context_t ctx, struct vnode *vp, mode_t mode);
void    mac_vnode_notify_setowner(vfs_context_t ctx, struct vnode *vp, uid_t uid, gid_t gid);
void    mac_vnode_notify_setutimes(vfs_context_t ctx, struct vnode *vp, struct timespec atime, struct timespec mtime);
void    mac_vnode_notify_truncate(vfs_context_t ctx, kauth_cred_t file_cred, struct vnode *vp);
int     mac_vnode_find_sigs(struct proc *p, struct vnode *vp, off_t offsetInMacho) __result_use_check;
int     vnode_label(struct mount *mp, struct vnode *dvp, struct vnode *vp,
    struct componentname *cnp, int flags, vfs_context_t ctx) __result_use_check;
void    vnode_relabel(struct vnode *vp);
void    mac_pty_notify_grant(proc_t p, struct tty *tp, dev_t dev, struct label *label);
void    mac_pty_notify_close(proc_t p, struct tty *tp, dev_t dev, struct label *label);
int     mac_kext_check_load(kauth_cred_t cred, const char *identifier) __result_use_check;
int     mac_kext_check_unload(kauth_cred_t cred, const char *identifier) __result_use_check;
int     mac_kext_check_query(kauth_cred_t cred) __result_use_check;
int     mac_skywalk_flow_check_connect(proc_t p, void *flow, const struct sockaddr *addr, int type, int protocol) __result_use_check;
int     mac_skywalk_flow_check_listen(proc_t p, void *flow, const struct sockaddr *addr, int type, int protocol) __result_use_check;
void    mac_vnode_notify_reclaim(vnode_t vp);

void psem_label_associate(struct fileproc *fp, struct vnode *vp, struct vfs_context *ctx);
void pshm_label_associate(struct fileproc *fp, struct vnode *vp, struct vfs_context *ctx);

#endif  /* CONFIG_MACF */

#endif /* !_SECURITY_MAC_FRAMEWORK_H_ */
