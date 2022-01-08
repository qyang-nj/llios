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
 * Copyright (c) 1989, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Rick Macklem at The University of Guelph.
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
 *	@(#)nfs_vfsops.c	8.12 (Berkeley) 5/20/95
 * FreeBSD-Id: nfs_vfsops.c,v 1.52 1997/11/12 05:42:21 julian Exp $
 */

#include <nfs/nfs_conf.h>
#if CONFIG_NFS_CLIENT

/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/ioctl.h>
#include <sys/signal.h>
#include <sys/proc_internal.h> /* for fs rooting to update rootdir in fdp */
#include <sys/kauth.h>
#include <sys/vnode_internal.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/mount_internal.h>
#include <sys/kpi_mbuf.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/socketvar.h>
#include <sys/fcntl.h>
#include <sys/quota.h>
#include <sys/priv.h>
#include <libkern/OSAtomic.h>

#include <sys/vm.h>
#include <sys/vmparam.h>

#if !defined(NO_MOUNT_PRIVATE)
#include <sys/filedesc.h>
#endif /* NO_MOUNT_PRIVATE */

#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>

#include <nfs/rpcv2.h>
#include <nfs/krpc.h>
#include <nfs/nfsproto.h>
#include <nfs/nfs.h>
#include <nfs/nfsnode.h>
#include <nfs/nfs_gss.h>
#include <nfs/nfsmount.h>
#include <nfs/xdr_subs.h>
#include <nfs/nfsm_subs.h>
#include <nfs/nfsdiskless.h>
#include <nfs/nfs_lock.h>
#if CONFIG_MACF
#include <security/mac_framework.h>
#endif

#include <pexpert/pexpert.h>

#define NFS_VFS_DBG(...) NFS_DBG(NFS_FAC_VFS, 7, ## __VA_ARGS__)

/*
 * NFS client globals
 */

ZONE_DECLARE(nfsmnt_zone, "NFS mount",
    sizeof(struct nfsmount), ZC_ZFREE_CLEARMEM);

int nfs_ticks;
static LCK_GRP_DECLARE(nfs_global_grp, "nfs_global");
static LCK_GRP_DECLARE(nfs_mount_grp, "nfs_mount");
LCK_MTX_DECLARE(nfs_global_mutex, &nfs_global_grp);
uint32_t nfs_fs_attr_bitmap[NFS_ATTR_BITMAP_LEN];
uint32_t nfs_object_attr_bitmap[NFS_ATTR_BITMAP_LEN];
uint32_t nfs_getattr_bitmap[NFS_ATTR_BITMAP_LEN];
uint32_t nfs4_getattr_write_bitmap[NFS_ATTR_BITMAP_LEN];
struct nfsclientidlist nfsclientids;

/* NFS requests */
struct nfs_reqqhead nfs_reqq;
LCK_GRP_DECLARE(nfs_request_grp, "nfs_request");
LCK_MTX_DECLARE(nfs_request_mutex, &nfs_request_grp);
thread_call_t nfs_request_timer_call;
int nfs_request_timer_on;
u_int64_t nfs_xid = 0;
u_int64_t nfs_xidwrap = 0;              /* to build a (non-wrapping) 64 bit xid */

thread_call_t nfs_buf_timer_call;

/* NFSv4 */
LCK_GRP_DECLARE(nfs_open_grp, "nfs_open");
uint32_t nfs_open_owner_seqnum = 0;
uint32_t nfs_lock_owner_seqnum = 0;
thread_call_t nfs4_callback_timer_call;
int nfs4_callback_timer_on = 0;
char nfs4_default_domain[MAXPATHLEN];

/* nfsiod */
static LCK_GRP_DECLARE(nfsiod_lck_grp, "nfsiod");
LCK_MTX_DECLARE(nfsiod_mutex, &nfsiod_lck_grp);
struct nfsiodlist nfsiodfree, nfsiodwork;
struct nfsiodmountlist nfsiodmounts;
int nfsiod_thread_count = 0;
int nfsiod_thread_max = NFS_DEFASYNCTHREAD;
int nfs_max_async_writes = NFS_DEFMAXASYNCWRITES;

int nfs_iosize = NFS_IOSIZE;
int nfs_access_cache_timeout = NFS_MAXATTRTIMO;
int nfs_access_delete = 1; /* too many servers get this wrong - workaround on by default */
int nfs_access_dotzfs = 1;
int nfs_access_for_getattr = 0;
int nfs_allow_async = 0;
int nfs_statfs_rate_limit = NFS_DEFSTATFSRATELIMIT;
int nfs_lockd_mounts = 0;
int nfs_lockd_request_sent = 0;
int nfs_idmap_ctrl = NFS_IDMAP_CTRL_USE_IDMAP_SERVICE;
int nfs_callback_port = 0;

int nfs_tprintf_initial_delay = NFS_TPRINTF_INITIAL_DELAY;
int nfs_tprintf_delay = NFS_TPRINTF_DELAY;


int             mountnfs(char *, mount_t, vfs_context_t, vnode_t *);
#if CONFIG_NETBOOT
static int      nfs_mount_diskless(struct nfs_dlmount *, const char *, int, vnode_t *, mount_t *, vfs_context_t);
#if !defined(NO_MOUNT_PRIVATE)
static int      nfs_mount_diskless_private(struct nfs_dlmount *, const char *, int, vnode_t *, mount_t *, vfs_context_t);
#endif /* NO_MOUNT_PRIVATE */
#endif
int             nfs_mount_connect(struct nfsmount *);
void            nfs_mount_drain_and_cleanup(struct nfsmount *);
void            nfs_mount_cleanup(struct nfsmount *);
int             nfs_mountinfo_assemble(struct nfsmount *, struct xdrbuf *);
int             nfs4_mount_update_path_with_symlink(struct nfsmount *, struct nfs_fs_path *, uint32_t, fhandle_t *, int *, fhandle_t *, vfs_context_t);

/*
 * NFS VFS operations.
 */
int     nfs_vfs_mount(mount_t, vnode_t, user_addr_t, vfs_context_t);
int     nfs_vfs_start(mount_t, int, vfs_context_t);
int     nfs_vfs_unmount(mount_t, int, vfs_context_t);
int     nfs_vfs_root(mount_t, vnode_t *, vfs_context_t);
int     nfs_vfs_quotactl(mount_t, int, uid_t, caddr_t, vfs_context_t);
int     nfs_vfs_getattr(mount_t, struct vfs_attr *, vfs_context_t);
int     nfs_vfs_sync(mount_t, int, vfs_context_t);
int     nfs_vfs_vget(mount_t, ino64_t, vnode_t *, vfs_context_t);
int     nfs_vfs_vptofh(vnode_t, int *, unsigned char *, vfs_context_t);
int     nfs_vfs_fhtovp(mount_t, int, unsigned char *, vnode_t *, vfs_context_t);
int     nfs_vfs_init(struct vfsconf *);
int     nfs_vfs_sysctl(int *, u_int, user_addr_t, size_t *, user_addr_t, size_t, vfs_context_t);

const struct vfsops nfs_vfsops = {
	.vfs_mount       = nfs_vfs_mount,
	.vfs_start       = nfs_vfs_start,
	.vfs_unmount     = nfs_vfs_unmount,
	.vfs_root        = nfs_vfs_root,
	.vfs_quotactl    = nfs_vfs_quotactl,
	.vfs_getattr     = nfs_vfs_getattr,
	.vfs_sync        = nfs_vfs_sync,
	.vfs_vget        = nfs_vfs_vget,
	.vfs_fhtovp      = nfs_vfs_fhtovp,
	.vfs_vptofh      = nfs_vfs_vptofh,
	.vfs_init        = nfs_vfs_init,
	.vfs_sysctl      = nfs_vfs_sysctl,
	// We do not support the remaining VFS ops
};


/*
 * version-specific NFS functions
 */
int nfs3_mount(struct nfsmount *, vfs_context_t, nfsnode_t *);
int nfs4_mount(struct nfsmount *, vfs_context_t, nfsnode_t *);
int nfs3_fsinfo(struct nfsmount *, nfsnode_t, vfs_context_t);
int nfs3_update_statfs(struct nfsmount *, vfs_context_t);
int nfs4_update_statfs(struct nfsmount *, vfs_context_t);
#if !QUOTA
#define nfs3_getquota   NULL
#define nfs4_getquota   NULL
#else
int nfs3_getquota(struct nfsmount *, vfs_context_t, uid_t, int, struct dqblk *);
int nfs4_getquota(struct nfsmount *, vfs_context_t, uid_t, int, struct dqblk *);
#endif

const struct nfs_funcs nfs3_funcs = {
	.nf_mount = nfs3_mount,
	.nf_update_statfs = nfs3_update_statfs,
	.nf_getquota = nfs3_getquota,
	.nf_access_rpc = nfs3_access_rpc,
	.nf_getattr_rpc = nfs3_getattr_rpc,
	.nf_setattr_rpc = nfs3_setattr_rpc,
	.nf_read_rpc_async = nfs3_read_rpc_async,
	.nf_read_rpc_async_finish = nfs3_read_rpc_async_finish,
	.nf_readlink_rpc = nfs3_readlink_rpc,
	.nf_write_rpc_async = nfs3_write_rpc_async,
	.nf_write_rpc_async_finish = nfs3_write_rpc_async_finish,
	.nf_commit_rpc = nfs3_commit_rpc,
	.nf_lookup_rpc_async = nfs3_lookup_rpc_async,
	.nf_lookup_rpc_async_finish = nfs3_lookup_rpc_async_finish,
	.nf_remove_rpc = nfs3_remove_rpc,
	.nf_rename_rpc = nfs3_rename_rpc,
	.nf_setlock_rpc = nfs3_setlock_rpc,
	.nf_unlock_rpc = nfs3_unlock_rpc,
	.nf_getlock_rpc = nfs3_getlock_rpc
};
#if CONFIG_NFS4
const struct nfs_funcs nfs4_funcs = {
	.nf_mount = nfs4_mount,
	.nf_update_statfs = nfs4_update_statfs,
	.nf_getquota = nfs4_getquota,
	.nf_access_rpc = nfs4_access_rpc,
	.nf_getattr_rpc = nfs4_getattr_rpc,
	.nf_setattr_rpc = nfs4_setattr_rpc,
	.nf_read_rpc_async = nfs4_read_rpc_async,
	.nf_read_rpc_async_finish = nfs4_read_rpc_async_finish,
	.nf_readlink_rpc = nfs4_readlink_rpc,
	.nf_write_rpc_async = nfs4_write_rpc_async,
	.nf_write_rpc_async_finish = nfs4_write_rpc_async_finish,
	.nf_commit_rpc = nfs4_commit_rpc,
	.nf_lookup_rpc_async = nfs4_lookup_rpc_async,
	.nf_lookup_rpc_async_finish = nfs4_lookup_rpc_async_finish,
	.nf_remove_rpc = nfs4_remove_rpc,
	.nf_rename_rpc = nfs4_rename_rpc,
	.nf_setlock_rpc = nfs4_setlock_rpc,
	.nf_unlock_rpc = nfs4_unlock_rpc,
	.nf_getlock_rpc = nfs4_getlock_rpc
};
#endif

/*
 * Called once to initialize data structures...
 */
int
nfs_vfs_init(__unused struct vfsconf *vfsp)
{
#if CONFIG_NFS4
	int i;
#endif
	/*
	 * Check to see if major data structures haven't bloated.
	 */
	if (sizeof(struct nfsnode) > NFS_NODEALLOC) {
		printf("struct nfsnode bloated (> %dbytes)\n", NFS_NODEALLOC);
		printf("Try reducing NFS_SMALLFH\n");
	}
	if (sizeof(struct nfsmount) > NFS_MNTALLOC) {
		printf("struct nfsmount bloated (> %dbytes)\n", NFS_MNTALLOC);
	}

	nfs_ticks = (hz * NFS_TICKINTVL + 500) / 1000;
	if (nfs_ticks < 1) {
		nfs_ticks = 1;
	}

	/* init async I/O thread pool state */
	TAILQ_INIT(&nfsiodfree);
	TAILQ_INIT(&nfsiodwork);
	TAILQ_INIT(&nfsiodmounts);

	/* initialize NFS request list */
	TAILQ_INIT(&nfs_reqq);

	nfs_nbinit();                   /* Init the nfsbuf table */
#if CONFIG_NFS_GSS
	nfs_gss_init();                 /* Init RPCSEC_GSS security */
#endif

#if CONFIG_NFS4
	/* NFSv4 stuff */
	NFS4_PER_FS_ATTRIBUTES(nfs_fs_attr_bitmap);
	NFS4_PER_OBJECT_ATTRIBUTES(nfs_object_attr_bitmap);
	NFS4_DEFAULT_WRITE_ATTRIBUTES(nfs4_getattr_write_bitmap);
	NFS4_DEFAULT_ATTRIBUTES(nfs_getattr_bitmap);
	for (i = 0; i < NFS_ATTR_BITMAP_LEN; i++) {
		nfs_getattr_bitmap[i] &= nfs_object_attr_bitmap[i];
		nfs4_getattr_write_bitmap[i] &= nfs_object_attr_bitmap[i];
	}
	TAILQ_INIT(&nfsclientids);
#endif

	/* initialize NFS timer callouts */
	nfs_request_timer_call = thread_call_allocate(nfs_request_timer, NULL);
	nfs_buf_timer_call = thread_call_allocate(nfs_buf_timer, NULL);
#if CONFIG_NFS4
	nfs4_callback_timer_call = thread_call_allocate(nfs4_callback_timer, NULL);
#endif
	return 0;
}


/*
 * nfs statfs call
 */
int
nfs3_update_statfs(struct nfsmount *nmp, vfs_context_t ctx)
{
	nfsnode_t np;
	int error = 0, lockerror, status, nfsvers;
	u_int64_t xid;
	struct nfsm_chain nmreq, nmrep;
	uint32_t val = 0;

	nfsvers = nmp->nm_vers;
	np = nmp->nm_dnp;
	if (!np) {
		return ENXIO;
	}
	if ((error = vnode_get(NFSTOV(np)))) {
		return error;
	}

	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);

	nfsm_chain_build_alloc_init(error, &nmreq, NFSX_FH(nfsvers));
	nfsm_chain_add_fh(error, &nmreq, nfsvers, np->n_fhp, np->n_fhsize);
	nfsm_chain_build_done(error, &nmreq);
	nfsmout_if(error);
	error = nfs_request2(np, NULL, &nmreq, NFSPROC_FSSTAT, vfs_context_thread(ctx),
	    vfs_context_ucred(ctx), NULL, R_SOFT, &nmrep, &xid, &status);
	if (error == ETIMEDOUT) {
		goto nfsmout;
	}
	if ((lockerror = nfs_node_lock(np))) {
		error = lockerror;
	}
	if (nfsvers == NFS_VER3) {
		nfsm_chain_postop_attr_update(error, &nmrep, np, &xid);
	}
	if (!lockerror) {
		nfs_node_unlock(np);
	}
	if (!error) {
		error = status;
	}
	nfsm_assert(error, NFSTONMP(np), ENXIO);
	nfsmout_if(error);
	lck_mtx_lock(&nmp->nm_lock);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SPACE_TOTAL);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SPACE_FREE);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SPACE_AVAIL);
	if (nfsvers == NFS_VER3) {
		NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_FILES_AVAIL);
		NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_FILES_TOTAL);
		NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_FILES_FREE);
		nmp->nm_fsattr.nfsa_bsize = NFS_FABLKSIZE;
		nfsm_chain_get_64(error, &nmrep, nmp->nm_fsattr.nfsa_space_total);
		nfsm_chain_get_64(error, &nmrep, nmp->nm_fsattr.nfsa_space_free);
		nfsm_chain_get_64(error, &nmrep, nmp->nm_fsattr.nfsa_space_avail);
		nfsm_chain_get_64(error, &nmrep, nmp->nm_fsattr.nfsa_files_total);
		nfsm_chain_get_64(error, &nmrep, nmp->nm_fsattr.nfsa_files_free);
		nfsm_chain_get_64(error, &nmrep, nmp->nm_fsattr.nfsa_files_avail);
		// skip invarsec
	} else {
		nfsm_chain_adv(error, &nmrep, NFSX_UNSIGNED); // skip tsize?
		nfsm_chain_get_32(error, &nmrep, nmp->nm_fsattr.nfsa_bsize);
		nfsm_chain_get_32(error, &nmrep, val);
		nfsmout_if(error);
		if (nmp->nm_fsattr.nfsa_bsize <= 0) {
			nmp->nm_fsattr.nfsa_bsize = NFS_FABLKSIZE;
		}
		nmp->nm_fsattr.nfsa_space_total = (uint64_t)val * nmp->nm_fsattr.nfsa_bsize;
		nfsm_chain_get_32(error, &nmrep, val);
		nfsmout_if(error);
		nmp->nm_fsattr.nfsa_space_free = (uint64_t)val * nmp->nm_fsattr.nfsa_bsize;
		nfsm_chain_get_32(error, &nmrep, val);
		nfsmout_if(error);
		nmp->nm_fsattr.nfsa_space_avail = (uint64_t)val * nmp->nm_fsattr.nfsa_bsize;
	}
	lck_mtx_unlock(&nmp->nm_lock);
nfsmout:
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
	vnode_put(NFSTOV(np));
	return error;
}

#if CONFIG_NFS4
int
nfs4_update_statfs(struct nfsmount *nmp, vfs_context_t ctx)
{
	nfsnode_t np;
	int error = 0, lockerror, status, nfsvers, numops;
	u_int64_t xid;
	struct nfsm_chain nmreq, nmrep;
	uint32_t bitmap[NFS_ATTR_BITMAP_LEN];
	struct nfs_vattr nvattr;
	struct nfsreq_secinfo_args si;

	nfsvers = nmp->nm_vers;
	np = nmp->nm_dnp;
	if (!np) {
		return ENXIO;
	}
	if ((error = vnode_get(NFSTOV(np)))) {
		return error;
	}

	NFSREQ_SECINFO_SET(&si, np, NULL, 0, NULL, 0);
	NVATTR_INIT(&nvattr);
	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);

	// PUTFH + GETATTR
	numops = 2;
	nfsm_chain_build_alloc_init(error, &nmreq, 15 * NFSX_UNSIGNED);
	nfsm_chain_add_compound_header(error, &nmreq, "statfs", nmp->nm_minor_vers, numops);
	numops--;
	nfsm_chain_add_32(error, &nmreq, NFS_OP_PUTFH);
	nfsm_chain_add_fh(error, &nmreq, nfsvers, np->n_fhp, np->n_fhsize);
	numops--;
	nfsm_chain_add_32(error, &nmreq, NFS_OP_GETATTR);
	NFS_COPY_ATTRIBUTES(nfs_getattr_bitmap, bitmap);
	NFS4_STATFS_ATTRIBUTES(bitmap);
	nfsm_chain_add_bitmap_supported(error, &nmreq, bitmap, nmp, np);
	nfsm_chain_build_done(error, &nmreq);
	nfsm_assert(error, (numops == 0), EPROTO);
	nfsmout_if(error);
	error = nfs_request2(np, NULL, &nmreq, NFSPROC4_COMPOUND,
	    vfs_context_thread(ctx), vfs_context_ucred(ctx),
	    NULL, R_SOFT, &nmrep, &xid, &status);
	nfsm_chain_skip_tag(error, &nmrep);
	nfsm_chain_get_32(error, &nmrep, numops);
	nfsm_chain_op_check(error, &nmrep, NFS_OP_PUTFH);
	nfsm_chain_op_check(error, &nmrep, NFS_OP_GETATTR);
	nfsm_assert(error, NFSTONMP(np), ENXIO);
	nfsmout_if(error);
	lck_mtx_lock(&nmp->nm_lock);
	error = nfs4_parsefattr(&nmrep, &nmp->nm_fsattr, &nvattr, NULL, NULL, NULL);
	lck_mtx_unlock(&nmp->nm_lock);
	nfsmout_if(error);
	if ((lockerror = nfs_node_lock(np))) {
		error = lockerror;
	}
	if (!error) {
		nfs_loadattrcache(np, &nvattr, &xid, 0);
	}
	if (!lockerror) {
		nfs_node_unlock(np);
	}
	nfsm_assert(error, NFSTONMP(np), ENXIO);
	nfsmout_if(error);
	nmp->nm_fsattr.nfsa_bsize = NFS_FABLKSIZE;
nfsmout:
	NVATTR_CLEANUP(&nvattr);
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
	vnode_put(NFSTOV(np));
	return error;
}
#endif /* CONFIG_NFS4 */


/*
 * Return an NFS volume name from the mntfrom name.
 */
static void
nfs_get_volname(struct mount *mp, char *volname, size_t len, __unused vfs_context_t ctx)
{
	const char *ptr, *cptr;
	const char *mntfrom = mp->mnt_vfsstat.f_mntfromname;
	size_t mflen;


	mflen = strnlen(mntfrom, MAXPATHLEN + 1);

	if (mflen > MAXPATHLEN || mflen == 0) {
		strlcpy(volname, "Bad volname", len);
		return;
	}

	/* Move back over trailing slashes */
	for (ptr = &mntfrom[mflen - 1]; ptr != mntfrom && *ptr == '/'; ptr--) {
		mflen--;
	}

	/* Find first character after the last slash */
	cptr = ptr = NULL;
	for (size_t i = 0; i < mflen; i++) {
		if (mntfrom[i] == '/') {
			ptr = &mntfrom[i + 1];
		}
		/* And the first character after the first colon */
		else if (cptr == NULL && mntfrom[i] == ':') {
			cptr = &mntfrom[i + 1];
		}
	}

	/*
	 * No slash or nothing after the last slash
	 * use everything past the first colon
	 */
	if (ptr == NULL || *ptr == '\0') {
		ptr = cptr;
	}
	/* Otherwise use the mntfrom name */
	if (ptr == NULL) {
		ptr = mntfrom;
	}

	mflen = &mntfrom[mflen] - ptr;
	len = mflen + 1 < len ? mflen + 1 : len;

	strlcpy(volname, ptr, len);
}


/*
 * The NFS VFS_GETATTR function: "statfs"-type information is retrieved
 * using the nf_update_statfs() function, and other attributes are cobbled
 * together from whatever sources we can (getattr, fsinfo, pathconf).
 */
int
nfs_vfs_getattr(mount_t mp, struct vfs_attr *fsap, vfs_context_t ctx)
{
	struct nfsmount *nmp;
	uint32_t bsize;
	int error = 0, nfsvers;

	nmp = VFSTONFS(mp);
	if (nfs_mount_gone(nmp)) {
		return ENXIO;
	}
	nfsvers = nmp->nm_vers;

	if (VFSATTR_IS_ACTIVE(fsap, f_bsize) ||
	    VFSATTR_IS_ACTIVE(fsap, f_iosize) ||
	    VFSATTR_IS_ACTIVE(fsap, f_blocks) ||
	    VFSATTR_IS_ACTIVE(fsap, f_bfree) ||
	    VFSATTR_IS_ACTIVE(fsap, f_bavail) ||
	    VFSATTR_IS_ACTIVE(fsap, f_bused) ||
	    VFSATTR_IS_ACTIVE(fsap, f_files) ||
	    VFSATTR_IS_ACTIVE(fsap, f_ffree)) {
		int statfsrate = nfs_statfs_rate_limit;
		int refresh = 1;

		/*
		 * Are we rate-limiting statfs RPCs?
		 * (Treat values less than 1 or greater than 1,000,000 as no limit.)
		 */
		if ((statfsrate > 0) && (statfsrate < 1000000)) {
			struct timeval now;
			time_t stamp;

			microuptime(&now);
			lck_mtx_lock(&nmp->nm_lock);
			stamp = (now.tv_sec * statfsrate) + (now.tv_usec / (1000000 / statfsrate));
			if (stamp != nmp->nm_fsattrstamp) {
				refresh = 1;
				nmp->nm_fsattrstamp = stamp;
			} else {
				refresh = 0;
			}
			lck_mtx_unlock(&nmp->nm_lock);
		}

		if (refresh && !nfs_use_cache(nmp)) {
			error = nmp->nm_funcs->nf_update_statfs(nmp, ctx);
		}
		if ((error == ESTALE) || (error == ETIMEDOUT)) {
			error = 0;
		}
		if (error) {
			return error;
		}

		lck_mtx_lock(&nmp->nm_lock);
		VFSATTR_RETURN(fsap, f_iosize, nfs_iosize);
		VFSATTR_RETURN(fsap, f_bsize, nmp->nm_fsattr.nfsa_bsize);
		bsize = nmp->nm_fsattr.nfsa_bsize;
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SPACE_TOTAL)) {
			VFSATTR_RETURN(fsap, f_blocks, nmp->nm_fsattr.nfsa_space_total / bsize);
		}
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SPACE_FREE)) {
			VFSATTR_RETURN(fsap, f_bfree, nmp->nm_fsattr.nfsa_space_free / bsize);
		}
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SPACE_AVAIL)) {
			VFSATTR_RETURN(fsap, f_bavail, nmp->nm_fsattr.nfsa_space_avail / bsize);
		}
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SPACE_TOTAL) &&
		    NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SPACE_FREE)) {
			VFSATTR_RETURN(fsap, f_bused,
			    (nmp->nm_fsattr.nfsa_space_total / bsize) -
			    (nmp->nm_fsattr.nfsa_space_free / bsize));
		}
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_FILES_TOTAL)) {
			VFSATTR_RETURN(fsap, f_files, nmp->nm_fsattr.nfsa_files_total);
		}
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_FILES_FREE)) {
			VFSATTR_RETURN(fsap, f_ffree, nmp->nm_fsattr.nfsa_files_free);
		}
		lck_mtx_unlock(&nmp->nm_lock);
	}

	if (VFSATTR_IS_ACTIVE(fsap, f_vol_name)) {
		/*%%% IF fail over support is implemented we may need to take nm_lock */
		nfs_get_volname(mp, fsap->f_vol_name, MAXPATHLEN, ctx);
		VFSATTR_SET_SUPPORTED(fsap, f_vol_name);
	}
	if (VFSATTR_IS_ACTIVE(fsap, f_capabilities)
	    ) {
		u_int32_t caps, valid;
		nfsnode_t np = nmp->nm_dnp;

		nfsm_assert(error, VFSTONFS(mp) && np, ENXIO);
		if (error) {
			return error;
		}
		lck_mtx_lock(&nmp->nm_lock);

		/*
		 * The capabilities[] array defines what this volume supports.
		 *
		 * The valid[] array defines which bits this code understands
		 * the meaning of (whether the volume has that capability or
		 * not).  Any zero bits here means "I don't know what you're
		 * asking about" and the caller cannot tell whether that
		 * capability is present or not.
		 */
		caps = valid = 0;
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SYMLINK_SUPPORT)) {
			valid |= VOL_CAP_FMT_SYMBOLICLINKS;
			if (nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_SYMLINK) {
				caps |= VOL_CAP_FMT_SYMBOLICLINKS;
			}
		}
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_LINK_SUPPORT)) {
			valid |= VOL_CAP_FMT_HARDLINKS;
			if (nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_LINK) {
				caps |= VOL_CAP_FMT_HARDLINKS;
			}
		}
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_CASE_INSENSITIVE)) {
			valid |= VOL_CAP_FMT_CASE_SENSITIVE;
			if (!(nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_CASE_INSENSITIVE)) {
				caps |= VOL_CAP_FMT_CASE_SENSITIVE;
			}
		}
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_CASE_PRESERVING)) {
			valid |= VOL_CAP_FMT_CASE_PRESERVING;
			if (nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_CASE_PRESERVING) {
				caps |= VOL_CAP_FMT_CASE_PRESERVING;
			}
		}
		/* Note: VOL_CAP_FMT_2TB_FILESIZE is actually used to test for "large file support" */
		if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_MAXFILESIZE)) {
			/* Is server's max file size at least 4GB? */
			if (nmp->nm_fsattr.nfsa_maxfilesize >= 0x100000000ULL) {
				caps |= VOL_CAP_FMT_2TB_FILESIZE;
			}
		} else if (nfsvers >= NFS_VER3) {
			/*
			 * NFSv3 and up supports 64 bits of file size.
			 * So, we'll just assume maxfilesize >= 4GB
			 */
			caps |= VOL_CAP_FMT_2TB_FILESIZE;
		}
#if CONFIG_NFS4
		if (nfsvers >= NFS_VER4) {
			caps |= VOL_CAP_FMT_HIDDEN_FILES;
			valid |= VOL_CAP_FMT_HIDDEN_FILES;
			// VOL_CAP_FMT_OPENDENYMODES
//			caps |= VOL_CAP_FMT_OPENDENYMODES;
//			valid |= VOL_CAP_FMT_OPENDENYMODES;
		}
#endif
		// no version of nfs supports immutable files
		caps |= VOL_CAP_FMT_NO_IMMUTABLE_FILES;
		valid |= VOL_CAP_FMT_NO_IMMUTABLE_FILES;

		fsap->f_capabilities.capabilities[VOL_CAPABILITIES_FORMAT] =
		    // VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    // VOL_CAP_FMT_SYMBOLICLINKS |
		    // VOL_CAP_FMT_HARDLINKS |
		    // VOL_CAP_FMT_JOURNAL |
		    // VOL_CAP_FMT_JOURNAL_ACTIVE |
		    // VOL_CAP_FMT_NO_ROOT_TIMES |
		    // VOL_CAP_FMT_SPARSE_FILES |
		    // VOL_CAP_FMT_ZERO_RUNS |
		    // VOL_CAP_FMT_CASE_SENSITIVE |
		    // VOL_CAP_FMT_CASE_PRESERVING |
		    // VOL_CAP_FMT_FAST_STATFS |
		    // VOL_CAP_FMT_2TB_FILESIZE |
		    // VOL_CAP_FMT_OPENDENYMODES |
		    // VOL_CAP_FMT_HIDDEN_FILES |
		    caps;
		fsap->f_capabilities.valid[VOL_CAPABILITIES_FORMAT] =
		    VOL_CAP_FMT_PERSISTENTOBJECTIDS |
		    // VOL_CAP_FMT_SYMBOLICLINKS |
		    // VOL_CAP_FMT_HARDLINKS |
		    // VOL_CAP_FMT_JOURNAL |
		    // VOL_CAP_FMT_JOURNAL_ACTIVE |
		    // VOL_CAP_FMT_NO_ROOT_TIMES |
		    // VOL_CAP_FMT_SPARSE_FILES |
		    // VOL_CAP_FMT_ZERO_RUNS |
		    // VOL_CAP_FMT_CASE_SENSITIVE |
		    // VOL_CAP_FMT_CASE_PRESERVING |
		    VOL_CAP_FMT_FAST_STATFS |
		    VOL_CAP_FMT_2TB_FILESIZE |
		    // VOL_CAP_FMT_OPENDENYMODES |
		    // VOL_CAP_FMT_HIDDEN_FILES |
		    valid;

		/*
		 * We don't support most of the interfaces.
		 *
		 * We MAY support locking, but we don't have any easy way of
		 * probing.  We can tell if there's no lockd running or if
		 * locks have been disabled for a mount, so we can definitely
		 * answer NO in that case.  Any attempt to send a request to
		 * lockd to test for locking support may cause the lazily-
		 * launched locking daemons to be started unnecessarily.  So
		 * we avoid that.  However, we do record if we ever successfully
		 * perform a lock operation on a mount point, so if it looks
		 * like lock ops have worked, we do report that we support them.
		 */
		caps = valid = 0;
#if CONFIG_NFS4
		if (nfsvers >= NFS_VER4) {
			caps = VOL_CAP_INT_ADVLOCK | VOL_CAP_INT_FLOCK;
			valid = VOL_CAP_INT_ADVLOCK | VOL_CAP_INT_FLOCK;
			if (nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_ACL) {
				caps |= VOL_CAP_INT_EXTENDED_SECURITY;
			}
			valid |= VOL_CAP_INT_EXTENDED_SECURITY;
			if (nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_NAMED_ATTR) {
				caps |= VOL_CAP_INT_EXTENDED_ATTR;
			}
			valid |= VOL_CAP_INT_EXTENDED_ATTR;
#if NAMEDSTREAMS
			if (nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_NAMED_ATTR) {
				caps |= VOL_CAP_INT_NAMEDSTREAMS;
			}
			valid |= VOL_CAP_INT_NAMEDSTREAMS;
#endif
		} else
#endif
		if (nmp->nm_lockmode == NFS_LOCK_MODE_DISABLED) {
			/* locks disabled on this mount, so they definitely won't work */
			valid = VOL_CAP_INT_ADVLOCK | VOL_CAP_INT_FLOCK;
		} else if (nmp->nm_state & NFSSTA_LOCKSWORK) {
			caps = VOL_CAP_INT_ADVLOCK | VOL_CAP_INT_FLOCK;
			valid = VOL_CAP_INT_ADVLOCK | VOL_CAP_INT_FLOCK;
		}
		fsap->f_capabilities.capabilities[VOL_CAPABILITIES_INTERFACES] =
		    // VOL_CAP_INT_SEARCHFS |
		    // VOL_CAP_INT_ATTRLIST |
		    // VOL_CAP_INT_NFSEXPORT |
		    // VOL_CAP_INT_READDIRATTR |
		    // VOL_CAP_INT_EXCHANGEDATA |
		    // VOL_CAP_INT_COPYFILE |
		    // VOL_CAP_INT_ALLOCATE |
		    // VOL_CAP_INT_VOL_RENAME |
		    // VOL_CAP_INT_ADVLOCK |
		    // VOL_CAP_INT_FLOCK |
		    // VOL_CAP_INT_EXTENDED_SECURITY |
		    // VOL_CAP_INT_USERACCESS |
		    // VOL_CAP_INT_MANLOCK |
		    // VOL_CAP_INT_NAMEDSTREAMS |
		    // VOL_CAP_INT_EXTENDED_ATTR |
		    VOL_CAP_INT_REMOTE_EVENT |
		    caps;
		fsap->f_capabilities.valid[VOL_CAPABILITIES_INTERFACES] =
		    VOL_CAP_INT_SEARCHFS |
		    VOL_CAP_INT_ATTRLIST |
		    VOL_CAP_INT_NFSEXPORT |
		    VOL_CAP_INT_READDIRATTR |
		    VOL_CAP_INT_EXCHANGEDATA |
		    VOL_CAP_INT_COPYFILE |
		    VOL_CAP_INT_ALLOCATE |
		    VOL_CAP_INT_VOL_RENAME |
		    // VOL_CAP_INT_ADVLOCK |
		    // VOL_CAP_INT_FLOCK |
		    // VOL_CAP_INT_EXTENDED_SECURITY |
		    // VOL_CAP_INT_USERACCESS |
		    // VOL_CAP_INT_MANLOCK |
		    // VOL_CAP_INT_NAMEDSTREAMS |
		    // VOL_CAP_INT_EXTENDED_ATTR |
		    VOL_CAP_INT_REMOTE_EVENT |
		    valid;

		fsap->f_capabilities.capabilities[VOL_CAPABILITIES_RESERVED1] = 0;
		fsap->f_capabilities.valid[VOL_CAPABILITIES_RESERVED1] = 0;

		fsap->f_capabilities.capabilities[VOL_CAPABILITIES_RESERVED2] = 0;
		fsap->f_capabilities.valid[VOL_CAPABILITIES_RESERVED2] = 0;

		VFSATTR_SET_SUPPORTED(fsap, f_capabilities);
		lck_mtx_unlock(&nmp->nm_lock);
	}

	if (VFSATTR_IS_ACTIVE(fsap, f_attributes)) {
		fsap->f_attributes.validattr.commonattr = 0;
		fsap->f_attributes.validattr.volattr =
		    ATTR_VOL_NAME | ATTR_VOL_CAPABILITIES | ATTR_VOL_ATTRIBUTES;
		fsap->f_attributes.validattr.dirattr = 0;
		fsap->f_attributes.validattr.fileattr = 0;
		fsap->f_attributes.validattr.forkattr = 0;

		fsap->f_attributes.nativeattr.commonattr = 0;
		fsap->f_attributes.nativeattr.volattr =
		    ATTR_VOL_NAME | ATTR_VOL_CAPABILITIES | ATTR_VOL_ATTRIBUTES;
		fsap->f_attributes.nativeattr.dirattr = 0;
		fsap->f_attributes.nativeattr.fileattr = 0;
		fsap->f_attributes.nativeattr.forkattr = 0;

		VFSATTR_SET_SUPPORTED(fsap, f_attributes);
	}

	return error;
}

/*
 * nfs version 3 fsinfo rpc call
 */
int
nfs3_fsinfo(struct nfsmount *nmp, nfsnode_t np, vfs_context_t ctx)
{
	int error = 0, lockerror, status, nmlocked = 0;
	u_int64_t xid;
	uint32_t val, prefsize, maxsize;
	struct nfsm_chain nmreq, nmrep;

	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);

	nfsm_chain_build_alloc_init(error, &nmreq, NFSX_FH(nmp->nm_vers));
	nfsm_chain_add_fh(error, &nmreq, nmp->nm_vers, np->n_fhp, np->n_fhsize);
	nfsm_chain_build_done(error, &nmreq);
	nfsmout_if(error);
	error = nfs_request(np, NULL, &nmreq, NFSPROC_FSINFO, ctx, NULL, &nmrep, &xid, &status);
	if ((lockerror = nfs_node_lock(np))) {
		error = lockerror;
	}
	nfsm_chain_postop_attr_update(error, &nmrep, np, &xid);
	if (!lockerror) {
		nfs_node_unlock(np);
	}
	if (!error) {
		error = status;
	}
	nfsmout_if(error);

	lck_mtx_lock(&nmp->nm_lock);
	nmlocked = 1;

	nfsm_chain_get_32(error, &nmrep, maxsize);
	nfsm_chain_get_32(error, &nmrep, prefsize);
	nfsmout_if(error);
	nmp->nm_fsattr.nfsa_maxread = maxsize;
	if (prefsize < nmp->nm_rsize) {
		nmp->nm_rsize = (prefsize + NFS_FABLKSIZE - 1) &
		    ~(NFS_FABLKSIZE - 1);
	}
	if ((maxsize > 0) && (maxsize < nmp->nm_rsize)) {
		nmp->nm_rsize = maxsize & ~(NFS_FABLKSIZE - 1);
		if (nmp->nm_rsize == 0) {
			nmp->nm_rsize = maxsize;
		}
	}
	nfsm_chain_adv(error, &nmrep, NFSX_UNSIGNED); // skip rtmult

	nfsm_chain_get_32(error, &nmrep, maxsize);
	nfsm_chain_get_32(error, &nmrep, prefsize);
	nfsmout_if(error);
	nmp->nm_fsattr.nfsa_maxwrite = maxsize;
	if (prefsize < nmp->nm_wsize) {
		nmp->nm_wsize = (prefsize + NFS_FABLKSIZE - 1) &
		    ~(NFS_FABLKSIZE - 1);
	}
	if ((maxsize > 0) && (maxsize < nmp->nm_wsize)) {
		nmp->nm_wsize = maxsize & ~(NFS_FABLKSIZE - 1);
		if (nmp->nm_wsize == 0) {
			nmp->nm_wsize = maxsize;
		}
	}
	nfsm_chain_adv(error, &nmrep, NFSX_UNSIGNED); // skip wtmult

	nfsm_chain_get_32(error, &nmrep, prefsize);
	nfsmout_if(error);
	if ((prefsize > 0) && (prefsize < nmp->nm_readdirsize)) {
		nmp->nm_readdirsize = prefsize;
	}
	if ((nmp->nm_fsattr.nfsa_maxread > 0) &&
	    (nmp->nm_fsattr.nfsa_maxread < nmp->nm_readdirsize)) {
		nmp->nm_readdirsize = nmp->nm_fsattr.nfsa_maxread;
	}

	nfsm_chain_get_64(error, &nmrep, nmp->nm_fsattr.nfsa_maxfilesize);

	nfsm_chain_adv(error, &nmrep, 2 * NFSX_UNSIGNED); // skip time_delta

	/* convert FS properties to our own flags */
	nfsm_chain_get_32(error, &nmrep, val);
	nfsmout_if(error);
	if (val & NFSV3FSINFO_LINK) {
		nmp->nm_fsattr.nfsa_flags |= NFS_FSFLAG_LINK;
	}
	if (val & NFSV3FSINFO_SYMLINK) {
		nmp->nm_fsattr.nfsa_flags |= NFS_FSFLAG_SYMLINK;
	}
	if (val & NFSV3FSINFO_HOMOGENEOUS) {
		nmp->nm_fsattr.nfsa_flags |= NFS_FSFLAG_HOMOGENEOUS;
	}
	if (val & NFSV3FSINFO_CANSETTIME) {
		nmp->nm_fsattr.nfsa_flags |= NFS_FSFLAG_SET_TIME;
	}
	nmp->nm_state |= NFSSTA_GOTFSINFO;
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_MAXREAD);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_MAXWRITE);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_MAXFILESIZE);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_LINK_SUPPORT);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_SYMLINK_SUPPORT);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_HOMOGENEOUS);
	NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_CANSETTIME);
nfsmout:
	if (nmlocked) {
		lck_mtx_unlock(&nmp->nm_lock);
	}
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
	return error;
}

/*
 * Mount a remote root fs via. nfs. This depends on the info in the
 * nfs_diskless structure that has been filled in properly by some primary
 * bootstrap.
 * It goes something like this:
 * - do enough of "ifconfig" by calling ifioctl() so that the system
 *   can talk to the server
 * - If nfs_diskless.mygateway is filled in, use that address as
 *   a default gateway.
 * - hand craft the swap nfs vnode hanging off a fake mount point
 *	if swdevt[0].sw_dev == NODEV
 * - build the rootfs mount point and call mountnfs() to do the rest.
 */
#if CONFIG_NETBOOT
int
nfs_mountroot(void)
{
	struct nfs_diskless nd;
	mount_t mp = NULL;
	vnode_t vp = NULL;
	vfs_context_t ctx;
	int error;
#if !defined(NO_MOUNT_PRIVATE)
	mount_t mppriv = NULL;
	vnode_t vppriv = NULL;
#endif /* NO_MOUNT_PRIVATE */
	int v3, sotype;

	/*
	 * Call nfs_boot_init() to fill in the nfs_diskless struct.
	 * Note: networking must already have been configured before
	 * we're called.
	 */
	bzero((caddr_t) &nd, sizeof(nd));
	error = nfs_boot_init(&nd);
	if (error) {
		panic("nfs_boot_init: unable to initialize NFS root system information, "
		    "error %d, check configuration: %s\n", error, PE_boot_args());
	}

	/*
	 * Try NFSv3 first, then fallback to NFSv2.
	 * Likewise, try TCP first, then fall back to UDP.
	 */
	v3 = 1;
	sotype = SOCK_STREAM;

tryagain:
	error = nfs_boot_getfh(&nd, v3, sotype);
	if (error) {
		if (error == EHOSTDOWN || error == EHOSTUNREACH) {
			if (nd.nd_root.ndm_mntfrom) {
				NFS_ZFREE(ZV_NAMEI, nd.nd_root.ndm_mntfrom);
			}
			if (nd.nd_root.ndm_path) {
				NFS_ZFREE(ZV_NAMEI, nd.nd_root.ndm_path);
			}
			if (nd.nd_private.ndm_mntfrom) {
				NFS_ZFREE(ZV_NAMEI, nd.nd_private.ndm_mntfrom);
			}
			if (nd.nd_private.ndm_path) {
				NFS_ZFREE(ZV_NAMEI, nd.nd_private.ndm_path);
			}
			return error;
		}
		if (v3) {
			if (sotype == SOCK_STREAM) {
				printf("NFS mount (v3,TCP) failed with error %d, trying UDP...\n", error);
				sotype = SOCK_DGRAM;
				goto tryagain;
			}
			printf("NFS mount (v3,UDP) failed with error %d, trying v2...\n", error);
			v3 = 0;
			sotype = SOCK_STREAM;
			goto tryagain;
		} else if (sotype == SOCK_STREAM) {
			printf("NFS mount (v2,TCP) failed with error %d, trying UDP...\n", error);
			sotype = SOCK_DGRAM;
			goto tryagain;
		} else {
			printf("NFS mount (v2,UDP) failed with error %d, giving up...\n", error);
		}
		switch (error) {
		case EPROGUNAVAIL:
			panic("NFS mount failed: NFS server mountd not responding, check server configuration: %s", PE_boot_args());
		case EACCES:
		case EPERM:
			panic("NFS mount failed: NFS server refused mount, check server configuration: %s", PE_boot_args());
		default:
			panic("NFS mount failed with error %d, check configuration: %s", error, PE_boot_args());
		}
	}

	ctx = vfs_context_kernel();

	/*
	 * Create the root mount point.
	 */
#if !defined(NO_MOUNT_PRIVATE)
	{
		//PWC hack until we have a real "mount" tool to remount root rw
		int rw_root = 0;
		int flags = MNT_ROOTFS | MNT_RDONLY;
		PE_parse_boot_argn("-rwroot_hack", &rw_root, sizeof(rw_root));
		if (rw_root) {
			flags = MNT_ROOTFS;
			kprintf("-rwroot_hack in effect: mounting root fs read/write\n");
		}

		if ((error = nfs_mount_diskless(&nd.nd_root, "/", flags, &vp, &mp, ctx)))
#else
	if ((error = nfs_mount_diskless(&nd.nd_root, "/", MNT_ROOTFS, &vp, &mp, ctx)))
#endif /* NO_MOUNT_PRIVATE */
		{
			if (v3) {
				if (sotype == SOCK_STREAM) {
					printf("NFS root mount (v3,TCP) failed with %d, trying UDP...\n", error);
					sotype = SOCK_DGRAM;
					goto tryagain;
				}
				printf("NFS root mount (v3,UDP) failed with %d, trying v2...\n", error);
				v3 = 0;
				sotype = SOCK_STREAM;
				goto tryagain;
			} else if (sotype == SOCK_STREAM) {
				printf("NFS root mount (v2,TCP) failed with %d, trying UDP...\n", error);
				sotype = SOCK_DGRAM;
				goto tryagain;
			} else {
				printf("NFS root mount (v2,UDP) failed with error %d, giving up...\n", error);
			}
			panic("NFS root mount failed with error %d, check configuration: %s\n", error, PE_boot_args());
		}
	}
	printf("root on %s\n", nd.nd_root.ndm_mntfrom);

	vfs_unbusy(mp);
	mount_list_add(mp);
	rootvp = vp;

#if !defined(NO_MOUNT_PRIVATE)
	if (nd.nd_private.ndm_saddr.sin_addr.s_addr) {
		error = nfs_mount_diskless_private(&nd.nd_private, "/private",
		    0, &vppriv, &mppriv, ctx);
		if (error) {
			panic("NFS /private mount failed with error %d, check configuration: %s\n", error, PE_boot_args());
		}
		printf("private on %s\n", nd.nd_private.ndm_mntfrom);

		vfs_unbusy(mppriv);
		mount_list_add(mppriv);
	}

#endif /* NO_MOUNT_PRIVATE */

	if (nd.nd_root.ndm_mntfrom) {
		NFS_ZFREE(ZV_NAMEI, nd.nd_root.ndm_mntfrom);
	}
	if (nd.nd_root.ndm_path) {
		NFS_ZFREE(ZV_NAMEI, nd.nd_root.ndm_path);
	}
	if (nd.nd_private.ndm_mntfrom) {
		NFS_ZFREE(ZV_NAMEI, nd.nd_private.ndm_mntfrom);
	}
	if (nd.nd_private.ndm_path) {
		NFS_ZFREE(ZV_NAMEI, nd.nd_private.ndm_path);
	}

	/* Get root attributes (for the time). */
	error = nfs_getattr(VTONFS(vp), NULL, ctx, NGA_UNCACHED);
	if (error) {
		panic("NFS mount: failed to get attributes for root directory, error %d, check server", error);
	}
	return 0;
}

/*
 * Internal version of mount system call for diskless setup.
 */
static int
nfs_mount_diskless(
	struct nfs_dlmount *ndmntp,
	const char *mntname,
	int mntflag,
	vnode_t *vpp,
	mount_t *mpp,
	vfs_context_t ctx)
{
	mount_t mp;
	int error, numcomps;
	char *xdrbuf, *p, *cp, *frompath, *endserverp;
	char uaddr[MAX_IPv4_STR_LEN];
	struct xdrbuf xb;
	uint32_t mattrs[NFS_MATTR_BITMAP_LEN];
	uint32_t mflags_mask[NFS_MFLAG_BITMAP_LEN];
	uint32_t mflags[NFS_MFLAG_BITMAP_LEN];
	uint64_t argslength_offset, attrslength_offset, end_offset;

	if ((error = vfs_rootmountalloc("nfs", ndmntp->ndm_mntfrom, &mp))) {
		printf("nfs_mount_diskless: NFS not configured\n");
		return error;
	}

	mp->mnt_flag |= mntflag;
	if (!(mntflag & MNT_RDONLY)) {
		mp->mnt_flag &= ~MNT_RDONLY;
	}

	/* find the server-side path being mounted */
	frompath = ndmntp->ndm_mntfrom;
	if (*frompath == '[') {  /* skip IPv6 literal address */
		while (*frompath && (*frompath != ']')) {
			frompath++;
		}
		if (*frompath == ']') {
			frompath++;
		}
	}
	while (*frompath && (*frompath != ':')) {
		frompath++;
	}
	endserverp = frompath;
	while (*frompath && (*frompath == ':')) {
		frompath++;
	}
	/* count fs location path components */
	p = frompath;
	while (*p && (*p == '/')) {
		p++;
	}
	numcomps = 0;
	while (*p) {
		numcomps++;
		while (*p && (*p != '/')) {
			p++;
		}
		while (*p && (*p == '/')) {
			p++;
		}
	}

	/* convert address to universal address string */
	if (inet_ntop(AF_INET, &ndmntp->ndm_saddr.sin_addr, uaddr, sizeof(uaddr)) != uaddr) {
		printf("nfs_mount_diskless: bad address\n");
		return EINVAL;
	}

	/* prepare mount attributes */
	NFS_BITMAP_ZERO(mattrs, NFS_MATTR_BITMAP_LEN);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_VERSION);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_SOCKET_TYPE);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_PORT);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FH);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FS_LOCATIONS);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_MNTFLAGS);

	/* prepare mount flags */
	NFS_BITMAP_ZERO(mflags_mask, NFS_MFLAG_BITMAP_LEN);
	NFS_BITMAP_ZERO(mflags, NFS_MFLAG_BITMAP_LEN);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_RESVPORT);
	NFS_BITMAP_SET(mflags, NFS_MFLAG_RESVPORT);

	/* build xdr buffer */
	xb_init_buffer(&xb, NULL, 0);
	xb_add_32(error, &xb, NFS_ARGSVERSION_XDR);
	argslength_offset = xb_offset(&xb);
	xb_add_32(error, &xb, 0); // args length
	xb_add_32(error, &xb, NFS_XDRARGS_VERSION_0);
	xb_add_bitmap(error, &xb, mattrs, NFS_MATTR_BITMAP_LEN);
	attrslength_offset = xb_offset(&xb);
	xb_add_32(error, &xb, 0); // attrs length
	xb_add_32(error, &xb, ndmntp->ndm_nfsv3 ? 3 : 2); // NFS version
	xb_add_string(error, &xb, ((ndmntp->ndm_sotype == SOCK_DGRAM) ? "udp" : "tcp"), 3);
	xb_add_32(error, &xb, ntohs(ndmntp->ndm_saddr.sin_port)); // NFS port
	xb_add_fh(error, &xb, &ndmntp->ndm_fh[0], ndmntp->ndm_fhlen);
	/* fs location */
	xb_add_32(error, &xb, 1); /* fs location count */
	xb_add_32(error, &xb, 1); /* server count */
	xb_add_string(error, &xb, ndmntp->ndm_mntfrom, (endserverp - ndmntp->ndm_mntfrom)); /* server name */
	xb_add_32(error, &xb, 1); /* address count */
	xb_add_string(error, &xb, uaddr, strlen(uaddr)); /* address */
	xb_add_32(error, &xb, 0); /* empty server info */
	xb_add_32(error, &xb, numcomps); /* pathname component count */
	p = frompath;
	while (*p && (*p == '/')) {
		p++;
	}
	while (*p) {
		cp = p;
		while (*p && (*p != '/')) {
			p++;
		}
		xb_add_string(error, &xb, cp, (p - cp)); /* component */
		if (error) {
			break;
		}
		while (*p && (*p == '/')) {
			p++;
		}
	}
	xb_add_32(error, &xb, 0); /* empty fsl info */
	xb_add_32(error, &xb, mntflag); /* MNT flags */
	xb_build_done(error, &xb);

	/* update opaque counts */
	end_offset = xb_offset(&xb);
	if (!error) {
		error = xb_seek(&xb, argslength_offset);
		xb_add_32(error, &xb, end_offset - argslength_offset + XDRWORD /*version*/);
	}
	if (!error) {
		error = xb_seek(&xb, attrslength_offset);
		xb_add_32(error, &xb, end_offset - attrslength_offset - XDRWORD /*don't include length field*/);
	}
	if (error) {
		printf("nfs_mount_diskless: error %d assembling mount args\n", error);
		xb_cleanup(&xb);
		return error;
	}
	/* grab the assembled buffer */
	xdrbuf = xb_buffer_base(&xb);
	xb.xb_flags &= ~XB_CLEANUP;

	/* do the mount */
	if ((error = mountnfs(xdrbuf, mp, ctx, vpp))) {
		printf("nfs_mountroot: mount %s failed: %d\n", mntname, error);
		// XXX vfs_rootmountfailed(mp);
		mount_list_lock();
		mp->mnt_vtable->vfc_refcount--;
		mount_list_unlock();
		vfs_unbusy(mp);
		mount_lock_destroy(mp);
#if CONFIG_MACF
		mac_mount_label_destroy(mp);
#endif
		NFS_ZFREE(mount_zone, mp);
	} else {
		*mpp = mp;
	}
	xb_cleanup(&xb);
	return error;
}

#if !defined(NO_MOUNT_PRIVATE)
/*
 * Internal version of mount system call to mount "/private"
 * separately in diskless setup
 */
static int
nfs_mount_diskless_private(
	struct nfs_dlmount *ndmntp,
	const char *mntname,
	int mntflag,
	vnode_t *vpp,
	mount_t *mpp,
	vfs_context_t ctx)
{
	mount_t mp;
	int error, numcomps;
	proc_t procp;
	struct vfstable *vfsp;
	struct nameidata nd;
	vnode_t vp;
	char *xdrbuf = NULL, *p, *cp, *frompath, *endserverp;
	char uaddr[MAX_IPv4_STR_LEN];
	struct xdrbuf xb;
	uint32_t mattrs[NFS_MATTR_BITMAP_LEN];
	uint32_t mflags_mask[NFS_MFLAG_BITMAP_LEN], mflags[NFS_MFLAG_BITMAP_LEN];
	uint64_t argslength_offset, attrslength_offset, end_offset;

	procp = current_proc(); /* XXX */
	xb_init(&xb, XDRBUF_NONE);

	{
		/*
		 * mimic main()!. Temporarily set up rootvnode and other stuff so
		 * that namei works. Need to undo this because main() does it, too
		 */
		struct filedesc *fdp;   /* pointer to file descriptor state */
		fdp = procp->p_fd;
		mountlist.tqh_first->mnt_flag |= MNT_ROOTFS;

		/* Get the vnode for '/'. Set fdp->fd_cdir to reference it. */
		if (VFS_ROOT(mountlist.tqh_first, &rootvnode, NULL)) {
			panic("cannot find root vnode");
		}
		error = vnode_ref(rootvnode);
		if (error) {
			printf("nfs_mountroot: vnode_ref() failed on root vnode!\n");
			goto out;
		}
		fdp->fd_cdir = rootvnode;
		fdp->fd_rdir = NULL;
	}

	/*
	 * Get vnode to be covered
	 */
	NDINIT(&nd, LOOKUP, OP_LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE,
	    CAST_USER_ADDR_T(mntname), ctx);
	error = namei(&nd);
	{
		/* undo vnode_ref() in mimic main()! */
		vnode_rele(rootvnode);
	}
	if (error) {
		printf("nfs_mountroot: private namei failed!\n");
		goto out;
	}
	nameidone(&nd);
	vp = nd.ni_vp;

	if ((error = VNOP_FSYNC(vp, MNT_WAIT, ctx)) ||
	    (error = buf_invalidateblks(vp, BUF_WRITE_DATA, 0, 0))) {
		vnode_put(vp);
		goto out;
	}
	if (vnode_vtype(vp) != VDIR) {
		vnode_put(vp);
		error = ENOTDIR;
		goto out;
	}
	for (vfsp = vfsconf; vfsp; vfsp = vfsp->vfc_next) {
		if (!strncmp(vfsp->vfc_name, "nfs", sizeof(vfsp->vfc_name))) {
			break;
		}
	}
	if (vfsp == NULL) {
		printf("nfs_mountroot: private NFS not configured\n");
		vnode_put(vp);
		error = ENODEV;
		goto out;
	}
	if (vnode_mountedhere(vp) != NULL) {
		vnode_put(vp);
		error = EBUSY;
		goto out;
	}

	/*
	 * Allocate and initialize the filesystem.
	 */
	mp = zalloc_flags(mount_zone, Z_WAITOK | Z_ZERO);
	/* Initialize the default IO constraints */
	mp->mnt_maxreadcnt = mp->mnt_maxwritecnt = MAXPHYS;
	mp->mnt_segreadcnt = mp->mnt_segwritecnt = 32;
	mp->mnt_ioflags = 0;
	mp->mnt_realrootvp = NULLVP;
	mp->mnt_authcache_ttl = 0; /* Allways go to our lookup */

	mount_lock_init(mp);
	TAILQ_INIT(&mp->mnt_vnodelist);
	TAILQ_INIT(&mp->mnt_workerqueue);
	TAILQ_INIT(&mp->mnt_newvnodes);
	(void)vfs_busy(mp, LK_NOWAIT);
	TAILQ_INIT(&mp->mnt_vnodelist);
	mount_list_lock();
	vfsp->vfc_refcount++;
	mount_list_unlock();
	mp->mnt_vtable = vfsp;
	mp->mnt_op = vfsp->vfc_vfsops;
	// mp->mnt_stat.f_type = vfsp->vfc_typenum;
	mp->mnt_flag = mntflag;
	mp->mnt_flag |= vfsp->vfc_flags & MNT_VISFLAGMASK;
	strncpy(mp->mnt_vfsstat.f_fstypename, vfsp->vfc_name, MFSNAMELEN - 1);
	vp->v_mountedhere = mp;
	mp->mnt_vnodecovered = vp;
	vp = NULLVP;
	mp->mnt_vfsstat.f_owner = kauth_cred_getuid(kauth_cred_get());
	(void) copystr(mntname, mp->mnt_vfsstat.f_mntonname, MAXPATHLEN - 1, 0);
	(void) copystr(ndmntp->ndm_mntfrom, mp->mnt_vfsstat.f_mntfromname, MAXPATHLEN - 1, 0);
#if CONFIG_MACF
	mac_mount_label_init(mp);
	mac_mount_label_associate(ctx, mp);
#endif

	/* find the server-side path being mounted */
	frompath = ndmntp->ndm_mntfrom;
	if (*frompath == '[') {  /* skip IPv6 literal address */
		while (*frompath && (*frompath != ']')) {
			frompath++;
		}
		if (*frompath == ']') {
			frompath++;
		}
	}
	while (*frompath && (*frompath != ':')) {
		frompath++;
	}
	endserverp = frompath;
	while (*frompath && (*frompath == ':')) {
		frompath++;
	}
	/* count fs location path components */
	p = frompath;
	while (*p && (*p == '/')) {
		p++;
	}
	numcomps = 0;
	while (*p) {
		numcomps++;
		while (*p && (*p != '/')) {
			p++;
		}
		while (*p && (*p == '/')) {
			p++;
		}
	}

	/* convert address to universal address string */
	if (inet_ntop(AF_INET, &ndmntp->ndm_saddr.sin_addr, uaddr, sizeof(uaddr)) != uaddr) {
		printf("nfs_mountroot: bad address\n");
		error = EINVAL;
		goto out;
	}

	/* prepare mount attributes */
	NFS_BITMAP_ZERO(mattrs, NFS_MATTR_BITMAP_LEN);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_VERSION);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_SOCKET_TYPE);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_PORT);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FH);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FS_LOCATIONS);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_MNTFLAGS);

	/* prepare mount flags */
	NFS_BITMAP_ZERO(mflags_mask, NFS_MFLAG_BITMAP_LEN);
	NFS_BITMAP_ZERO(mflags, NFS_MFLAG_BITMAP_LEN);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_RESVPORT);
	NFS_BITMAP_SET(mflags, NFS_MFLAG_RESVPORT);

	/* build xdr buffer */
	xb_init_buffer(&xb, NULL, 0);
	xb_add_32(error, &xb, NFS_ARGSVERSION_XDR);
	argslength_offset = xb_offset(&xb);
	xb_add_32(error, &xb, 0); // args length
	xb_add_32(error, &xb, NFS_XDRARGS_VERSION_0);
	xb_add_bitmap(error, &xb, mattrs, NFS_MATTR_BITMAP_LEN);
	attrslength_offset = xb_offset(&xb);
	xb_add_32(error, &xb, 0); // attrs length
	xb_add_32(error, &xb, ndmntp->ndm_nfsv3 ? 3 : 2); // NFS version
	xb_add_string(error, &xb, ((ndmntp->ndm_sotype == SOCK_DGRAM) ? "udp" : "tcp"), 3);
	xb_add_32(error, &xb, ntohs(ndmntp->ndm_saddr.sin_port)); // NFS port
	xb_add_fh(error, &xb, &ndmntp->ndm_fh[0], ndmntp->ndm_fhlen);
	/* fs location */
	xb_add_32(error, &xb, 1); /* fs location count */
	xb_add_32(error, &xb, 1); /* server count */
	xb_add_string(error, &xb, ndmntp->ndm_mntfrom, (endserverp - ndmntp->ndm_mntfrom)); /* server name */
	xb_add_32(error, &xb, 1); /* address count */
	xb_add_string(error, &xb, uaddr, strlen(uaddr)); /* address */
	xb_add_32(error, &xb, 0); /* empty server info */
	xb_add_32(error, &xb, numcomps); /* pathname component count */
	p = frompath;
	while (*p && (*p == '/')) {
		p++;
	}
	while (*p) {
		cp = p;
		while (*p && (*p != '/')) {
			p++;
		}
		xb_add_string(error, &xb, cp, (p - cp)); /* component */
		if (error) {
			break;
		}
		while (*p && (*p == '/')) {
			p++;
		}
	}
	xb_add_32(error, &xb, 0); /* empty fsl info */
	xb_add_32(error, &xb, mntflag); /* MNT flags */
	xb_build_done(error, &xb);

	/* update opaque counts */
	end_offset = xb_offset(&xb);
	if (!error) {
		error = xb_seek(&xb, argslength_offset);
		xb_add_32(error, &xb, end_offset - argslength_offset + XDRWORD /*version*/);
	}
	if (!error) {
		error = xb_seek(&xb, attrslength_offset);
		xb_add_32(error, &xb, end_offset - attrslength_offset - XDRWORD /*don't include length field*/);
	}
	if (error) {
		printf("nfs_mountroot: error %d assembling mount args\n", error);
		goto out;
	}
	/* grab the assembled buffer */
	xdrbuf = xb_buffer_base(&xb);
	xb.xb_flags &= ~XB_CLEANUP;

	/* do the mount */
	if ((error = mountnfs(xdrbuf, mp, ctx, &vp))) {
		printf("nfs_mountroot: mount %s failed: %d\n", mntname, error);
		vnode_put(mp->mnt_vnodecovered);
		mount_list_lock();
		vfsp->vfc_refcount--;
		mount_list_unlock();
		vfs_unbusy(mp);
		mount_lock_destroy(mp);
#if CONFIG_MACF
		mac_mount_label_destroy(mp);
#endif
		NFS_ZFREE(mount_zone, mp);
		goto out;
	}

	*mpp = mp;
	*vpp = vp;
out:
	xb_cleanup(&xb);
	return error;
}
#endif /* NO_MOUNT_PRIVATE */

#endif

/*
 * Convert old style NFS mount args to XDR.
 */
static int
nfs_convert_old_nfs_args(mount_t mp, user_addr_t data, vfs_context_t ctx, int argsversion, int inkernel, char **xdrbufp)
{
	int error = 0, args64bit, argsize, numcomps;
	struct user_nfs_args args;
	struct nfs_args tempargs;
	caddr_t argsp;
	size_t len;
	u_char nfh[NFS4_FHSIZE];
	char *mntfrom, *endserverp, *frompath, *p, *cp;
	struct sockaddr_storage ss;
	void *sinaddr = NULL;
	char uaddr[MAX_IPv6_STR_LEN];
	uint32_t mattrs[NFS_MATTR_BITMAP_LEN];
	uint32_t mflags_mask[NFS_MFLAG_BITMAP_LEN], mflags[NFS_MFLAG_BITMAP_LEN];
	uint32_t nfsvers, nfslockmode = 0;
	size_t argslength_offset, attrslength_offset, end_offset;
	struct xdrbuf xb;

	*xdrbufp = NULL;

	/* allocate a temporary buffer for mntfrom */
	mntfrom = zalloc(ZV_NAMEI);

	args64bit = (inkernel || vfs_context_is64bit(ctx));
	argsp = args64bit ? (void*)&args : (void*)&tempargs;

	argsize = args64bit ? sizeof(args) : sizeof(tempargs);
	switch (argsversion) {
	case 3:
		argsize -= NFS_ARGSVERSION4_INCSIZE;
		OS_FALLTHROUGH;
	case 4:
		argsize -= NFS_ARGSVERSION5_INCSIZE;
		OS_FALLTHROUGH;
	case 5:
		argsize -= NFS_ARGSVERSION6_INCSIZE;
		OS_FALLTHROUGH;
	case 6:
		break;
	default:
		error = EPROGMISMATCH;
		goto nfsmout;
	}

	/* read in the structure */
	if (inkernel) {
		bcopy(CAST_DOWN(void *, data), argsp, argsize);
	} else {
		error = copyin(data, argsp, argsize);
	}
	nfsmout_if(error);

	if (!args64bit) {
		args.addrlen = tempargs.addrlen;
		args.sotype = tempargs.sotype;
		args.proto = tempargs.proto;
		args.fhsize = tempargs.fhsize;
		args.flags = tempargs.flags;
		args.wsize = tempargs.wsize;
		args.rsize = tempargs.rsize;
		args.readdirsize = tempargs.readdirsize;
		args.timeo = tempargs.timeo;
		args.retrans = tempargs.retrans;
		args.maxgrouplist = tempargs.maxgrouplist;
		args.readahead = tempargs.readahead;
		args.leaseterm = tempargs.leaseterm;
		args.deadthresh = tempargs.deadthresh;
		args.addr = CAST_USER_ADDR_T(tempargs.addr);
		args.fh = CAST_USER_ADDR_T(tempargs.fh);
		args.hostname = CAST_USER_ADDR_T(tempargs.hostname);
		if (args.version >= 4) {
			args.acregmin = tempargs.acregmin;
			args.acregmax = tempargs.acregmax;
			args.acdirmin = tempargs.acdirmin;
			args.acdirmax = tempargs.acdirmax;
		}
		if (args.version >= 5) {
			args.auth = tempargs.auth;
		}
		if (args.version >= 6) {
			args.deadtimeout = tempargs.deadtimeout;
		}
	}

	if ((args.fhsize < 0) || (args.fhsize > NFS4_FHSIZE)) {
		error = EINVAL;
		goto nfsmout;
	}
	if (args.fhsize > 0) {
		if (inkernel) {
			bcopy(CAST_DOWN(void *, args.fh), (caddr_t)nfh, args.fhsize);
		} else {
			error = copyin(args.fh, (caddr_t)nfh, args.fhsize);
		}
		nfsmout_if(error);
	}

	if (inkernel) {
		error = copystr(CAST_DOWN(void *, args.hostname), mntfrom, MAXPATHLEN - 1, &len);
	} else {
		error = copyinstr(args.hostname, mntfrom, MAXPATHLEN - 1, &len);
	}
	nfsmout_if(error);
	bzero(&mntfrom[len], MAXPATHLEN - len);

	/* find the server-side path being mounted */
	frompath = mntfrom;
	if (*frompath == '[') {  /* skip IPv6 literal address */
		while (*frompath && (*frompath != ']')) {
			frompath++;
		}
		if (*frompath == ']') {
			frompath++;
		}
	}
	while (*frompath && (*frompath != ':')) {
		frompath++;
	}
	endserverp = frompath;
	while (*frompath && (*frompath == ':')) {
		frompath++;
	}
	/* count fs location path components */
	p = frompath;
	while (*p && (*p == '/')) {
		p++;
	}
	numcomps = 0;
	while (*p) {
		numcomps++;
		while (*p && (*p != '/')) {
			p++;
		}
		while (*p && (*p == '/')) {
			p++;
		}
	}

	/* copy socket address */
	if (inkernel) {
		bcopy(CAST_DOWN(void *, args.addr), &ss, args.addrlen);
	} else {
		if (args.addrlen > sizeof(struct sockaddr_storage)) {
			error = EINVAL;
		} else {
			error = copyin(args.addr, &ss, args.addrlen);
		}
	}
	nfsmout_if(error);
	ss.ss_len = args.addrlen;

	/* convert address to universal address string */
	if (ss.ss_family == AF_INET) {
		if (ss.ss_len != sizeof(struct sockaddr_in)) {
			error = EINVAL;
		} else {
			sinaddr = &((struct sockaddr_in*)&ss)->sin_addr;
		}
	} else if (ss.ss_family == AF_INET6) {
		if (ss.ss_len != sizeof(struct sockaddr_in6)) {
			error = EINVAL;
		} else {
			sinaddr = &((struct sockaddr_in6*)&ss)->sin6_addr;
		}
	} else {
		sinaddr = NULL;
	}
	nfsmout_if(error);

	if (!sinaddr || (inet_ntop(ss.ss_family, sinaddr, uaddr, sizeof(uaddr)) != uaddr)) {
		error = EINVAL;
		goto nfsmout;
	}

	/* prepare mount flags */
	NFS_BITMAP_ZERO(mflags_mask, NFS_MFLAG_BITMAP_LEN);
	NFS_BITMAP_ZERO(mflags, NFS_MFLAG_BITMAP_LEN);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_SOFT);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_INTR);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_RESVPORT);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NOCONNECT);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_DUMBTIMER);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_CALLUMNT);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_RDIRPLUS);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NONEGNAMECACHE);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_MUTEJUKEBOX);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NOQUOTA);
	if (args.flags & NFSMNT_SOFT) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_SOFT);
	}
	if (args.flags & NFSMNT_INT) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_INTR);
	}
	if (args.flags & NFSMNT_RESVPORT) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_RESVPORT);
	}
	if (args.flags & NFSMNT_NOCONN) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_NOCONNECT);
	}
	if (args.flags & NFSMNT_DUMBTIMR) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_DUMBTIMER);
	}
	if (args.flags & NFSMNT_CALLUMNT) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_CALLUMNT);
	}
	if (args.flags & NFSMNT_RDIRPLUS) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_RDIRPLUS);
	}
	if (args.flags & NFSMNT_NONEGNAMECACHE) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_NONEGNAMECACHE);
	}
	if (args.flags & NFSMNT_MUTEJUKEBOX) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_MUTEJUKEBOX);
	}
	if (args.flags & NFSMNT_NOQUOTA) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_NOQUOTA);
	}

	/* prepare mount attributes */
	NFS_BITMAP_ZERO(mattrs, NFS_MATTR_BITMAP_LEN);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FLAGS);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_VERSION);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_SOCKET_TYPE);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_PORT);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FH);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FS_LOCATIONS);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_MNTFLAGS);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_MNTFROM);
	if (args.flags & NFSMNT_NFSV4) {
		nfsvers = 4;
	} else if (args.flags & NFSMNT_NFSV3) {
		nfsvers = 3;
	} else {
		nfsvers = 2;
	}
	if ((args.flags & NFSMNT_RSIZE) && (args.rsize > 0)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_READ_SIZE);
	}
	if ((args.flags & NFSMNT_WSIZE) && (args.wsize > 0)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_WRITE_SIZE);
	}
	if ((args.flags & NFSMNT_TIMEO) && (args.timeo > 0)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_REQUEST_TIMEOUT);
	}
	if ((args.flags & NFSMNT_RETRANS) && (args.retrans > 0)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_SOFT_RETRY_COUNT);
	}
	if ((args.flags & NFSMNT_MAXGRPS) && (args.maxgrouplist > 0)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_MAX_GROUP_LIST);
	}
	if ((args.flags & NFSMNT_READAHEAD) && (args.readahead > 0)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_READAHEAD);
	}
	if ((args.flags & NFSMNT_READDIRSIZE) && (args.readdirsize > 0)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_READDIR_SIZE);
	}
	if ((args.flags & NFSMNT_NOLOCKS) ||
	    (args.flags & NFSMNT_LOCALLOCKS)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_LOCK_MODE);
		if (args.flags & NFSMNT_NOLOCKS) {
			nfslockmode = NFS_LOCK_MODE_DISABLED;
		} else if (args.flags & NFSMNT_LOCALLOCKS) {
			nfslockmode = NFS_LOCK_MODE_LOCAL;
		} else {
			nfslockmode = NFS_LOCK_MODE_ENABLED;
		}
	}
	if (args.version >= 4) {
		if ((args.flags & NFSMNT_ACREGMIN) && (args.acregmin > 0)) {
			NFS_BITMAP_SET(mattrs, NFS_MATTR_ATTRCACHE_REG_MIN);
		}
		if ((args.flags & NFSMNT_ACREGMAX) && (args.acregmax > 0)) {
			NFS_BITMAP_SET(mattrs, NFS_MATTR_ATTRCACHE_REG_MAX);
		}
		if ((args.flags & NFSMNT_ACDIRMIN) && (args.acdirmin > 0)) {
			NFS_BITMAP_SET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MIN);
		}
		if ((args.flags & NFSMNT_ACDIRMAX) && (args.acdirmax > 0)) {
			NFS_BITMAP_SET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MAX);
		}
	}
	if (args.version >= 5) {
		if ((args.flags & NFSMNT_SECFLAVOR) || (args.flags & NFSMNT_SECSYSOK)) {
			NFS_BITMAP_SET(mattrs, NFS_MATTR_SECURITY);
		}
	}
	if (args.version >= 6) {
		if ((args.flags & NFSMNT_DEADTIMEOUT) && (args.deadtimeout > 0)) {
			NFS_BITMAP_SET(mattrs, NFS_MATTR_DEAD_TIMEOUT);
		}
	}

	/* build xdr buffer */
	xb_init_buffer(&xb, NULL, 0);
	xb_add_32(error, &xb, args.version);
	argslength_offset = xb_offset(&xb);
	xb_add_32(error, &xb, 0); // args length
	xb_add_32(error, &xb, NFS_XDRARGS_VERSION_0);
	xb_add_bitmap(error, &xb, mattrs, NFS_MATTR_BITMAP_LEN);
	attrslength_offset = xb_offset(&xb);
	xb_add_32(error, &xb, 0); // attrs length
	xb_add_bitmap(error, &xb, mflags_mask, NFS_MFLAG_BITMAP_LEN); /* mask */
	xb_add_bitmap(error, &xb, mflags, NFS_MFLAG_BITMAP_LEN); /* value */
	xb_add_32(error, &xb, nfsvers);
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READ_SIZE)) {
		xb_add_32(error, &xb, args.rsize);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_WRITE_SIZE)) {
		xb_add_32(error, &xb, args.wsize);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READDIR_SIZE)) {
		xb_add_32(error, &xb, args.readdirsize);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READAHEAD)) {
		xb_add_32(error, &xb, args.readahead);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_REG_MIN)) {
		xb_add_32(error, &xb, args.acregmin);
		xb_add_32(error, &xb, 0);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_REG_MAX)) {
		xb_add_32(error, &xb, args.acregmax);
		xb_add_32(error, &xb, 0);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MIN)) {
		xb_add_32(error, &xb, args.acdirmin);
		xb_add_32(error, &xb, 0);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MAX)) {
		xb_add_32(error, &xb, args.acdirmax);
		xb_add_32(error, &xb, 0);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCK_MODE)) {
		xb_add_32(error, &xb, nfslockmode);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SECURITY)) {
		uint32_t flavors[2], i = 0;
		if (args.flags & NFSMNT_SECFLAVOR) {
			flavors[i++] = args.auth;
		}
		if ((args.flags & NFSMNT_SECSYSOK) && ((i == 0) || (flavors[0] != RPCAUTH_SYS))) {
			flavors[i++] = RPCAUTH_SYS;
		}
		xb_add_word_array(error, &xb, flavors, i);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MAX_GROUP_LIST)) {
		xb_add_32(error, &xb, args.maxgrouplist);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SOCKET_TYPE)) {
		xb_add_string(error, &xb, ((args.sotype == SOCK_DGRAM) ? "udp" : "tcp"), 3);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_PORT)) {
		xb_add_32(error, &xb, ((ss.ss_family == AF_INET) ?
		    ntohs(((struct sockaddr_in*)&ss)->sin_port) :
		    ntohs(((struct sockaddr_in6*)&ss)->sin6_port)));
	}
	/* NFS_MATTR_MOUNT_PORT (not available in old args) */
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_REQUEST_TIMEOUT)) {
		/* convert from .1s increments to time */
		xb_add_32(error, &xb, args.timeo / 10);
		xb_add_32(error, &xb, (args.timeo % 10) * 100000000);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SOFT_RETRY_COUNT)) {
		xb_add_32(error, &xb, args.retrans);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_DEAD_TIMEOUT)) {
		xb_add_32(error, &xb, args.deadtimeout);
		xb_add_32(error, &xb, 0);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_FH)) {
		xb_add_fh(error, &xb, &nfh[0], args.fhsize);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_FS_LOCATIONS)) {
		xb_add_32(error, &xb, 1); /* fs location count */
		xb_add_32(error, &xb, 1); /* server count */
		xb_add_string(error, &xb, mntfrom, (endserverp - mntfrom)); /* server name */
		xb_add_32(error, &xb, 1); /* address count */
		xb_add_string(error, &xb, uaddr, strlen(uaddr)); /* address */
		xb_add_32(error, &xb, 0); /* empty server info */
		xb_add_32(error, &xb, numcomps); /* pathname component count */
		nfsmout_if(error);
		p = frompath;
		while (*p && (*p == '/')) {
			p++;
		}
		while (*p) {
			cp = p;
			while (*p && (*p != '/')) {
				p++;
			}
			xb_add_string(error, &xb, cp, (p - cp)); /* component */
			nfsmout_if(error);
			while (*p && (*p == '/')) {
				p++;
			}
		}
		xb_add_32(error, &xb, 0); /* empty fsl info */
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MNTFLAGS)) {
		xb_add_32(error, &xb, (vfs_flags(mp) & MNT_VISFLAGMASK)); /* VFS MNT_* flags */
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MNTFROM)) {
		xb_add_string(error, &xb, mntfrom, strlen(mntfrom)); /* fixed f_mntfromname */
	}
	xb_build_done(error, &xb);

	/* update opaque counts */
	end_offset = xb_offset(&xb);
	error = xb_seek(&xb, argslength_offset);
	xb_add_32(error, &xb, end_offset - argslength_offset + XDRWORD /*version*/);
	nfsmout_if(error);
	error = xb_seek(&xb, attrslength_offset);
	xb_add_32(error, &xb, end_offset - attrslength_offset - XDRWORD /*don't include length field*/);

	if (!error) {
		/* grab the assembled buffer */
		*xdrbufp = xb_buffer_base(&xb);
		xb.xb_flags &= ~XB_CLEANUP;
	}
nfsmout:
	xb_cleanup(&xb);
	NFS_ZFREE(ZV_NAMEI, mntfrom);
	return error;
}

/*
 * VFS Operations.
 *
 * mount system call
 */
int
nfs_vfs_mount(mount_t mp, vnode_t vp, user_addr_t data, vfs_context_t ctx)
{
	int error = 0, inkernel = vfs_iskernelmount(mp);
	uint32_t argsversion, argslength;
	char *xdrbuf = NULL;

	/* read in version */
	if (inkernel) {
		bcopy(CAST_DOWN(void *, data), &argsversion, sizeof(argsversion));
	} else if ((error = copyin(data, &argsversion, sizeof(argsversion)))) {
		return error;
	}

	/* If we have XDR args, then all values in the buffer are in network order */
	if (argsversion == htonl(NFS_ARGSVERSION_XDR)) {
		argsversion = NFS_ARGSVERSION_XDR;
	}

	switch (argsversion) {
	case 3:
	case 4:
	case 5:
	case 6:
		/* convert old-style args to xdr */
		error = nfs_convert_old_nfs_args(mp, data, ctx, argsversion, inkernel, &xdrbuf);
		break;
	case NFS_ARGSVERSION_XDR:
		/* copy in xdr buffer */
		if (inkernel) {
			bcopy(CAST_DOWN(void *, (data + XDRWORD)), &argslength, XDRWORD);
		} else {
			error = copyin((data + XDRWORD), &argslength, XDRWORD);
		}
		if (error) {
			break;
		}
		argslength = ntohl(argslength);
		/* put a reasonable limit on the size of the XDR args */
		if (argslength > 16 * 1024) {
			error = E2BIG;
			break;
		}
		/* allocate xdr buffer */
		xdrbuf = xb_malloc(xdr_rndup(argslength));
		if (!xdrbuf) {
			error = ENOMEM;
			break;
		}
		if (inkernel) {
			bcopy(CAST_DOWN(void *, data), xdrbuf, argslength);
		} else {
			error = copyin(data, xdrbuf, argslength);
		}

		if (!inkernel) {
			/* Recheck buffer size to avoid double fetch vulnerability */
			struct xdrbuf xb;
			uint32_t _version, _length;
			xb_init_buffer(&xb, xdrbuf, 2 * XDRWORD);
			xb_get_32(error, &xb, _version); /* version */
			xb_get_32(error, &xb, _length); /* args length */
			if (_length != argslength) {
				printf("nfs: actual buffer length (%u) does not match the initial value (%u)\n", _length, argslength);
				error = EINVAL;
				break;
			}
		}

		break;
	default:
		error = EPROGMISMATCH;
	}

	if (error) {
		if (xdrbuf) {
			xb_free(xdrbuf);
		}
		return error;
	}
	error = mountnfs(xdrbuf, mp, ctx, &vp);
	return error;
}

/*
 * Common code for mount and mountroot
 */

/* Set up an NFSv2/v3 mount */
int
nfs3_mount(
	struct nfsmount *nmp,
	vfs_context_t ctx,
	nfsnode_t *npp)
{
	int error = 0;
	struct nfs_vattr nvattr;
	u_int64_t xid;

	*npp = NULL;

	if (!nmp->nm_fh) {
		return EINVAL;
	}

	/*
	 * Get file attributes for the mountpoint.  These are needed
	 * in order to properly create the root vnode.
	 */
	error = nfs3_getattr_rpc(NULL, nmp->nm_mountp, nmp->nm_fh->fh_data, nmp->nm_fh->fh_len, 0,
	    ctx, &nvattr, &xid);
	if (error) {
		goto out;
	}

	error = nfs_nget(nmp->nm_mountp, NULL, NULL, nmp->nm_fh->fh_data, nmp->nm_fh->fh_len,
	    &nvattr, &xid, RPCAUTH_UNKNOWN, NG_MARKROOT, npp);
	if (*npp) {
		nfs_node_unlock(*npp);
	}
	if (error) {
		goto out;
	}

	/*
	 * Try to make sure we have all the general info from the server.
	 */
	if (nmp->nm_vers == NFS_VER2) {
		NFS_BITMAP_SET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_MAXNAME);
		nmp->nm_fsattr.nfsa_maxname = NFS_MAXNAMLEN;
	} else if (nmp->nm_vers == NFS_VER3) {
		/* get the NFSv3 FSINFO */
		error = nfs3_fsinfo(nmp, *npp, ctx);
		if (error) {
			goto out;
		}
		/* grab a copy of root info now (even if server does not support FSF_HOMOGENEOUS) */
		struct nfs_fsattr nfsa;
		if (!nfs3_pathconf_rpc(*npp, &nfsa, ctx)) {
			/* cache a copy of the results */
			lck_mtx_lock(&nmp->nm_lock);
			nfs3_pathconf_cache(nmp, &nfsa);
			lck_mtx_unlock(&nmp->nm_lock);
		}
	}
out:
	if (*npp && error) {
		vnode_put(NFSTOV(*npp));
		vnode_recycle(NFSTOV(*npp));
		*npp = NULL;
	}
	return error;
}

#if CONFIG_NFS4
/*
 * Update an NFSv4 mount path with the contents of the symlink.
 *
 * Read the link for the given file handle.
 * Insert the link's components into the path.
 */
int
nfs4_mount_update_path_with_symlink(struct nfsmount *nmp, struct nfs_fs_path *nfsp, uint32_t curcomp, fhandle_t *dirfhp, int *depthp, fhandle_t *fhp, vfs_context_t ctx)
{
	int error = 0, status, numops;
	uint32_t len = 0, comp, newcomp, linkcompcount;
	u_int64_t xid;
	struct nfsm_chain nmreq, nmrep;
	struct nfsreq rq, *req = &rq;
	struct nfsreq_secinfo_args si;
	char *link = NULL, *p, *q, ch;
	struct nfs_fs_path nfsp2;

	bzero(&nfsp2, sizeof(nfsp2));
	if (dirfhp->fh_len) {
		NFSREQ_SECINFO_SET(&si, NULL, dirfhp->fh_data, dirfhp->fh_len, nfsp->np_components[curcomp], 0);
	} else {
		NFSREQ_SECINFO_SET(&si, NULL, NULL, 0, nfsp->np_components[curcomp], 0);
	}
	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);

	link = zalloc(ZV_NAMEI);

	// PUTFH, READLINK
	numops = 2;
	nfsm_chain_build_alloc_init(error, &nmreq, 12 * NFSX_UNSIGNED);
	nfsm_chain_add_compound_header(error, &nmreq, "readlink", nmp->nm_minor_vers, numops);
	numops--;
	nfsm_chain_add_32(error, &nmreq, NFS_OP_PUTFH);
	nfsm_chain_add_fh(error, &nmreq, NFS_VER4, fhp->fh_data, fhp->fh_len);
	numops--;
	nfsm_chain_add_32(error, &nmreq, NFS_OP_READLINK);
	nfsm_chain_build_done(error, &nmreq);
	nfsm_assert(error, (numops == 0), EPROTO);
	nfsmout_if(error);

	error = nfs_request_async(NULL, nmp->nm_mountp, &nmreq, NFSPROC4_COMPOUND,
	    vfs_context_thread(ctx), vfs_context_ucred(ctx), &si, 0, NULL, &req);
	if (!error) {
		error = nfs_request_async_finish(req, &nmrep, &xid, &status);
	}

	nfsm_chain_skip_tag(error, &nmrep);
	nfsm_chain_get_32(error, &nmrep, numops);
	nfsm_chain_op_check(error, &nmrep, NFS_OP_PUTFH);
	nfsm_chain_op_check(error, &nmrep, NFS_OP_READLINK);
	nfsm_chain_get_32(error, &nmrep, len);
	nfsmout_if(error);
	if (len == 0) {
		error = ENOENT;
	} else if (len >= MAXPATHLEN) {
		len = MAXPATHLEN - 1;
	}
	nfsm_chain_get_opaque(error, &nmrep, len, link);
	nfsmout_if(error);
	/* make sure link string is terminated properly */
	link[len] = '\0';

	/* count the number of components in link */
	p = link;
	while (*p && (*p == '/')) {
		p++;
	}
	linkcompcount = 0;
	while (*p) {
		linkcompcount++;
		while (*p && (*p != '/')) {
			p++;
		}
		while (*p && (*p == '/')) {
			p++;
		}
	}

	/* free up used components */
	for (comp = 0; comp <= curcomp; comp++) {
		if (nfsp->np_components[comp]) {
			FREE(nfsp->np_components[comp], M_TEMP);
			nfsp->np_components[comp] = NULL;
		}
	}

	/* set up new path */
	nfsp2.np_compcount = nfsp->np_compcount - curcomp - 1 + linkcompcount;
	MALLOC(nfsp2.np_components, char **, nfsp2.np_compcount * sizeof(char*), M_TEMP, M_WAITOK | M_ZERO);
	if (!nfsp2.np_components) {
		error = ENOMEM;
		goto nfsmout;
	}

	/* add link components */
	p = link;
	while (*p && (*p == '/')) {
		p++;
	}
	for (newcomp = 0; newcomp < linkcompcount; newcomp++) {
		/* find end of component */
		q = p;
		while (*q && (*q != '/')) {
			q++;
		}
		MALLOC(nfsp2.np_components[newcomp], char *, q - p + 1, M_TEMP, M_WAITOK | M_ZERO);
		if (!nfsp2.np_components[newcomp]) {
			error = ENOMEM;
			break;
		}
		ch = *q;
		*q = '\0';
		strlcpy(nfsp2.np_components[newcomp], p, q - p + 1);
		*q = ch;
		p = q;
		while (*p && (*p == '/')) {
			p++;
		}
	}
	nfsmout_if(error);

	/* add remaining components */
	for (comp = curcomp + 1; comp < nfsp->np_compcount; comp++, newcomp++) {
		nfsp2.np_components[newcomp] = nfsp->np_components[comp];
		nfsp->np_components[comp] = NULL;
	}

	/* move new path into place */
	FREE(nfsp->np_components, M_TEMP);
	nfsp->np_components = nfsp2.np_components;
	nfsp->np_compcount = nfsp2.np_compcount;
	nfsp2.np_components = NULL;

	/* for absolute link, let the caller now that the next dirfh is root */
	if (link[0] == '/') {
		dirfhp->fh_len = 0;
		*depthp = 0;
	}
nfsmout:
	if (link) {
		NFS_ZFREE(ZV_NAMEI, link);
	}
	if (nfsp2.np_components) {
		for (comp = 0; comp < nfsp2.np_compcount; comp++) {
			if (nfsp2.np_components[comp]) {
				FREE(nfsp2.np_components[comp], M_TEMP);
			}
		}
		FREE(nfsp2.np_components, M_TEMP);
	}
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
	return error;
}

/* Set up an NFSv4 mount */
int
nfs4_mount(
	struct nfsmount *nmp,
	vfs_context_t ctx,
	nfsnode_t *npp)
{
	struct nfsm_chain nmreq, nmrep;
	int error = 0, numops, status, interval, isdotdot, loopcnt = 0, depth = 0;
	struct nfs_fs_path fspath, *nfsp, fspath2;
	uint32_t bitmap[NFS_ATTR_BITMAP_LEN], comp, comp2;
	fhandle_t fh, dirfh;
	struct nfs_vattr nvattr;
	u_int64_t xid;
	struct nfsreq rq, *req = &rq;
	struct nfsreq_secinfo_args si;
	struct nfs_sec sec;
	struct nfs_fs_locations nfsls;

	*npp = NULL;
	fh.fh_len = dirfh.fh_len = 0;
	TAILQ_INIT(&nmp->nm_open_owners);
	TAILQ_INIT(&nmp->nm_delegations);
	TAILQ_INIT(&nmp->nm_dreturnq);
	nmp->nm_stategenid = 1;
	NVATTR_INIT(&nvattr);
	bzero(&nfsls, sizeof(nfsls));
	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);

	/*
	 * If no security flavors were specified we'll want to default to the server's
	 * preferred flavor.  For NFSv4.0 we need a file handle and name to get that via
	 * SECINFO, so we'll do that on the last component of the server path we are
	 * mounting.  If we are mounting the server's root, we'll need to defer the
	 * SECINFO call to the first successful LOOKUP request.
	 */
	if (!nmp->nm_sec.count) {
		nmp->nm_state |= NFSSTA_NEEDSECINFO;
	}

	/* make a copy of the current location's path */
	nfsp = &nmp->nm_locations.nl_locations[nmp->nm_locations.nl_current.nli_loc]->nl_path;
	bzero(&fspath, sizeof(fspath));
	fspath.np_compcount = nfsp->np_compcount;
	if (fspath.np_compcount > 0) {
		MALLOC(fspath.np_components, char **, fspath.np_compcount * sizeof(char*), M_TEMP, M_WAITOK | M_ZERO);
		if (!fspath.np_components) {
			error = ENOMEM;
			goto nfsmout;
		}
		for (comp = 0; comp < nfsp->np_compcount; comp++) {
			size_t slen = strlen(nfsp->np_components[comp]);
			MALLOC(fspath.np_components[comp], char *, slen + 1, M_TEMP, M_WAITOK | M_ZERO);
			if (!fspath.np_components[comp]) {
				error = ENOMEM;
				break;
			}
			strlcpy(fspath.np_components[comp], nfsp->np_components[comp], slen + 1);
		}
		if (error) {
			goto nfsmout;
		}
	}

	/* for mirror mounts, we can just use the file handle passed in */
	if (nmp->nm_fh) {
		dirfh.fh_len = nmp->nm_fh->fh_len;
		bcopy(nmp->nm_fh->fh_data, dirfh.fh_data, dirfh.fh_len);
		NFSREQ_SECINFO_SET(&si, NULL, dirfh.fh_data, dirfh.fh_len, NULL, 0);
		goto gotfh;
	}

	/* otherwise, we need to get the fh for the directory we are mounting */

	/* if no components, just get root */
	if (fspath.np_compcount == 0) {
nocomponents:
		// PUTROOTFH + GETATTR(FH)
		NFSREQ_SECINFO_SET(&si, NULL, NULL, 0, NULL, 0);
		numops = 2;
		nfsm_chain_build_alloc_init(error, &nmreq, 9 * NFSX_UNSIGNED);
		nfsm_chain_add_compound_header(error, &nmreq, "mount", nmp->nm_minor_vers, numops);
		numops--;
		nfsm_chain_add_32(error, &nmreq, NFS_OP_PUTROOTFH);
		numops--;
		nfsm_chain_add_32(error, &nmreq, NFS_OP_GETATTR);
		NFS_CLEAR_ATTRIBUTES(bitmap);
		NFS4_DEFAULT_ATTRIBUTES(bitmap);
		NFS_BITMAP_SET(bitmap, NFS_FATTR_FILEHANDLE);
		nfsm_chain_add_bitmap(error, &nmreq, bitmap, NFS_ATTR_BITMAP_LEN);
		nfsm_chain_build_done(error, &nmreq);
		nfsm_assert(error, (numops == 0), EPROTO);
		nfsmout_if(error);
		error = nfs_request_async(NULL, nmp->nm_mountp, &nmreq, NFSPROC4_COMPOUND,
		    vfs_context_thread(ctx), vfs_context_ucred(ctx), &si, 0, NULL, &req);
		if (!error) {
			error = nfs_request_async_finish(req, &nmrep, &xid, &status);
		}
		nfsm_chain_skip_tag(error, &nmrep);
		nfsm_chain_get_32(error, &nmrep, numops);
		nfsm_chain_op_check(error, &nmrep, NFS_OP_PUTROOTFH);
		nfsm_chain_op_check(error, &nmrep, NFS_OP_GETATTR);
		nfsmout_if(error);
		NFS_CLEAR_ATTRIBUTES(nmp->nm_fsattr.nfsa_bitmap);
		error = nfs4_parsefattr(&nmrep, &nmp->nm_fsattr, &nvattr, &dirfh, NULL, NULL);
		if (!error && !NFS_BITMAP_ISSET(&nvattr.nva_bitmap, NFS_FATTR_FILEHANDLE)) {
			printf("nfs: mount didn't return filehandle?\n");
			error = EBADRPC;
		}
		nfsmout_if(error);
		nfsm_chain_cleanup(&nmrep);
		nfsm_chain_null(&nmreq);
		NVATTR_CLEANUP(&nvattr);
		goto gotfh;
	}

	/* look up each path component */
	for (comp = 0; comp < fspath.np_compcount;) {
		isdotdot = 0;
		if (fspath.np_components[comp][0] == '.') {
			if (fspath.np_components[comp][1] == '\0') {
				/* skip "." */
				comp++;
				continue;
			}
			/* treat ".." specially */
			if ((fspath.np_components[comp][1] == '.') &&
			    (fspath.np_components[comp][2] == '\0')) {
				isdotdot = 1;
			}
			if (isdotdot && (dirfh.fh_len == 0)) {
				/* ".." in root directory is same as "." */
				comp++;
				continue;
			}
		}
		// PUT(ROOT)FH + LOOKUP(P) + GETFH + GETATTR
		if (dirfh.fh_len == 0) {
			NFSREQ_SECINFO_SET(&si, NULL, NULL, 0, isdotdot ? NULL : fspath.np_components[comp], 0);
		} else {
			NFSREQ_SECINFO_SET(&si, NULL, dirfh.fh_data, dirfh.fh_len, isdotdot ? NULL : fspath.np_components[comp], 0);
		}
		numops = 4;
		nfsm_chain_build_alloc_init(error, &nmreq, 18 * NFSX_UNSIGNED);
		nfsm_chain_add_compound_header(error, &nmreq, "mount", nmp->nm_minor_vers, numops);
		numops--;
		if (dirfh.fh_len) {
			nfsm_chain_add_32(error, &nmreq, NFS_OP_PUTFH);
			nfsm_chain_add_fh(error, &nmreq, NFS_VER4, dirfh.fh_data, dirfh.fh_len);
		} else {
			nfsm_chain_add_32(error, &nmreq, NFS_OP_PUTROOTFH);
		}
		numops--;
		if (isdotdot) {
			nfsm_chain_add_32(error, &nmreq, NFS_OP_LOOKUPP);
		} else {
			nfsm_chain_add_32(error, &nmreq, NFS_OP_LOOKUP);
			nfsm_chain_add_name(error, &nmreq,
			    fspath.np_components[comp], strlen(fspath.np_components[comp]), nmp);
		}
		numops--;
		nfsm_chain_add_32(error, &nmreq, NFS_OP_GETFH);
		numops--;
		nfsm_chain_add_32(error, &nmreq, NFS_OP_GETATTR);
		NFS_CLEAR_ATTRIBUTES(bitmap);
		NFS4_DEFAULT_ATTRIBUTES(bitmap);
		/* if no namedattr support or component is ".zfs", clear NFS_FATTR_NAMED_ATTR */
		if (!NMFLAG(nmp, NAMEDATTR) || !strcmp(fspath.np_components[comp], ".zfs")) {
			NFS_BITMAP_CLR(bitmap, NFS_FATTR_NAMED_ATTR);
		}
		nfsm_chain_add_bitmap(error, &nmreq, bitmap, NFS_ATTR_BITMAP_LEN);
		nfsm_chain_build_done(error, &nmreq);
		nfsm_assert(error, (numops == 0), EPROTO);
		nfsmout_if(error);
		error = nfs_request_async(NULL, nmp->nm_mountp, &nmreq, NFSPROC4_COMPOUND,
		    vfs_context_thread(ctx), vfs_context_ucred(ctx), &si, 0, NULL, &req);
		if (!error) {
			error = nfs_request_async_finish(req, &nmrep, &xid, &status);
		}
		nfsm_chain_skip_tag(error, &nmrep);
		nfsm_chain_get_32(error, &nmrep, numops);
		nfsm_chain_op_check(error, &nmrep, dirfh.fh_len ? NFS_OP_PUTFH : NFS_OP_PUTROOTFH);
		nfsm_chain_op_check(error, &nmrep, isdotdot ? NFS_OP_LOOKUPP : NFS_OP_LOOKUP);
		nfsmout_if(error);
		nfsm_chain_op_check(error, &nmrep, NFS_OP_GETFH);
		nfsm_chain_get_32(error, &nmrep, fh.fh_len);
		if (fh.fh_len > sizeof(fh.fh_data)) {
			error = EBADRPC;
		}
		nfsmout_if(error);
		nfsm_chain_get_opaque(error, &nmrep, fh.fh_len, fh.fh_data);
		nfsm_chain_op_check(error, &nmrep, NFS_OP_GETATTR);
		if (!error) {
			NFS_CLEAR_ATTRIBUTES(nmp->nm_fsattr.nfsa_bitmap);
			error = nfs4_parsefattr(&nmrep, &nmp->nm_fsattr, &nvattr, NULL, NULL, &nfsls);
		}
		nfsm_chain_cleanup(&nmrep);
		nfsm_chain_null(&nmreq);
		if (error) {
			/* LOOKUP succeeded but GETATTR failed?  This could be a referral. */
			/* Try the lookup again with a getattr for fs_locations. */
			nfs_fs_locations_cleanup(&nfsls);
			error = nfs4_get_fs_locations(nmp, NULL, dirfh.fh_data, dirfh.fh_len, fspath.np_components[comp], ctx, &nfsls);
			if (!error && (nfsls.nl_numlocs < 1)) {
				error = ENOENT;
			}
			nfsmout_if(error);
			if (++loopcnt > MAXSYMLINKS) {
				/* too many symlink/referral redirections */
				error = ELOOP;
				goto nfsmout;
			}
			/* tear down the current connection */
			nfs_disconnect(nmp);
			/* replace fs locations */
			nfs_fs_locations_cleanup(&nmp->nm_locations);
			nmp->nm_locations = nfsls;
			bzero(&nfsls, sizeof(nfsls));
			/* initiate a connection using the new fs locations */
			error = nfs_mount_connect(nmp);
			if (!error && !(nmp->nm_locations.nl_current.nli_flags & NLI_VALID)) {
				error = EIO;
			}
			nfsmout_if(error);
			/* add new server's remote path to beginning of our path and continue */
			nfsp = &nmp->nm_locations.nl_locations[nmp->nm_locations.nl_current.nli_loc]->nl_path;
			bzero(&fspath2, sizeof(fspath2));
			fspath2.np_compcount = (fspath.np_compcount - comp - 1) + nfsp->np_compcount;
			if (fspath2.np_compcount > 0) {
				MALLOC(fspath2.np_components, char **, fspath2.np_compcount * sizeof(char*), M_TEMP, M_WAITOK | M_ZERO);
				if (!fspath2.np_components) {
					error = ENOMEM;
					goto nfsmout;
				}
				for (comp2 = 0; comp2 < nfsp->np_compcount; comp2++) {
					size_t slen = strlen(nfsp->np_components[comp2]);
					MALLOC(fspath2.np_components[comp2], char *, slen + 1, M_TEMP, M_WAITOK | M_ZERO);
					if (!fspath2.np_components[comp2]) {
						/* clean up fspath2, then error out */
						while (comp2 > 0) {
							comp2--;
							FREE(fspath2.np_components[comp2], M_TEMP);
						}
						FREE(fspath2.np_components, M_TEMP);
						error = ENOMEM;
						goto nfsmout;
					}
					strlcpy(fspath2.np_components[comp2], nfsp->np_components[comp2], slen + 1);
				}
				if ((fspath.np_compcount - comp - 1) > 0) {
					bcopy(&fspath.np_components[comp + 1], &fspath2.np_components[nfsp->np_compcount], (fspath.np_compcount - comp - 1) * sizeof(char*));
				}
				/* free up unused parts of old path (prior components and component array) */
				do {
					FREE(fspath.np_components[comp], M_TEMP);
				} while (comp-- > 0);
				FREE(fspath.np_components, M_TEMP);
				/* put new path in place */
				fspath = fspath2;
			}
			/* reset dirfh and component index */
			dirfh.fh_len = 0;
			comp = 0;
			NVATTR_CLEANUP(&nvattr);
			if (fspath.np_compcount == 0) {
				goto nocomponents;
			}
			continue;
		}
		nfsmout_if(error);
		/* if file handle is for a symlink, then update the path with the symlink contents */
		if (NFS_BITMAP_ISSET(&nvattr.nva_bitmap, NFS_FATTR_TYPE) && (nvattr.nva_type == VLNK)) {
			if (++loopcnt > MAXSYMLINKS) {
				error = ELOOP;
			} else {
				error = nfs4_mount_update_path_with_symlink(nmp, &fspath, comp, &dirfh, &depth, &fh, ctx);
			}
			nfsmout_if(error);
			/* directory file handle is either left the same or reset to root (if link was absolute) */
			/* path traversal starts at beginning of the path again */
			comp = 0;
			NVATTR_CLEANUP(&nvattr);
			nfs_fs_locations_cleanup(&nfsls);
			continue;
		}
		NVATTR_CLEANUP(&nvattr);
		nfs_fs_locations_cleanup(&nfsls);
		/* not a symlink... */
		if ((nmp->nm_state & NFSSTA_NEEDSECINFO) && (comp == (fspath.np_compcount - 1)) && !isdotdot) {
			/* need to get SECINFO for the directory being mounted */
			if (dirfh.fh_len == 0) {
				NFSREQ_SECINFO_SET(&si, NULL, NULL, 0, isdotdot ? NULL : fspath.np_components[comp], 0);
			} else {
				NFSREQ_SECINFO_SET(&si, NULL, dirfh.fh_data, dirfh.fh_len, isdotdot ? NULL : fspath.np_components[comp], 0);
			}
			sec.count = NX_MAX_SEC_FLAVORS;
			error = nfs4_secinfo_rpc(nmp, &si, vfs_context_ucred(ctx), sec.flavors, &sec.count);
			/* [sigh] some implementations return "illegal" error for unsupported ops */
			if (error == NFSERR_OP_ILLEGAL) {
				error = 0;
			}
			nfsmout_if(error);
			/* set our default security flavor to the first in the list */
			if (sec.count) {
				nmp->nm_auth = sec.flavors[0];
			}
			nmp->nm_state &= ~NFSSTA_NEEDSECINFO;
		}
		/* advance directory file handle, component index, & update depth */
		dirfh = fh;
		comp++;
		if (!isdotdot) { /* going down the hierarchy */
			depth++;
		} else if (--depth <= 0) { /* going up the hierarchy */
			dirfh.fh_len = 0; /* clear dirfh when we hit root */
		}
	}

gotfh:
	/* get attrs for mount point root */
	numops = NMFLAG(nmp, NAMEDATTR) ? 3 : 2; // PUTFH + GETATTR + OPENATTR
	nfsm_chain_build_alloc_init(error, &nmreq, 25 * NFSX_UNSIGNED);
	nfsm_chain_add_compound_header(error, &nmreq, "mount", nmp->nm_minor_vers, numops);
	numops--;
	nfsm_chain_add_32(error, &nmreq, NFS_OP_PUTFH);
	nfsm_chain_add_fh(error, &nmreq, NFS_VER4, dirfh.fh_data, dirfh.fh_len);
	numops--;
	nfsm_chain_add_32(error, &nmreq, NFS_OP_GETATTR);
	NFS_CLEAR_ATTRIBUTES(bitmap);
	NFS4_DEFAULT_ATTRIBUTES(bitmap);
	/* if no namedattr support or last component is ".zfs", clear NFS_FATTR_NAMED_ATTR */
	if (!NMFLAG(nmp, NAMEDATTR) || ((fspath.np_compcount > 0) && !strcmp(fspath.np_components[fspath.np_compcount - 1], ".zfs"))) {
		NFS_BITMAP_CLR(bitmap, NFS_FATTR_NAMED_ATTR);
	}
	nfsm_chain_add_bitmap(error, &nmreq, bitmap, NFS_ATTR_BITMAP_LEN);
	if (NMFLAG(nmp, NAMEDATTR)) {
		numops--;
		nfsm_chain_add_32(error, &nmreq, NFS_OP_OPENATTR);
		nfsm_chain_add_32(error, &nmreq, 0);
	}
	nfsm_chain_build_done(error, &nmreq);
	nfsm_assert(error, (numops == 0), EPROTO);
	nfsmout_if(error);
	error = nfs_request_async(NULL, nmp->nm_mountp, &nmreq, NFSPROC4_COMPOUND,
	    vfs_context_thread(ctx), vfs_context_ucred(ctx), &si, 0, NULL, &req);
	if (!error) {
		error = nfs_request_async_finish(req, &nmrep, &xid, &status);
	}
	nfsm_chain_skip_tag(error, &nmrep);
	nfsm_chain_get_32(error, &nmrep, numops);
	nfsm_chain_op_check(error, &nmrep, NFS_OP_PUTFH);
	nfsm_chain_op_check(error, &nmrep, NFS_OP_GETATTR);
	nfsmout_if(error);
	NFS_CLEAR_ATTRIBUTES(nmp->nm_fsattr.nfsa_bitmap);
	error = nfs4_parsefattr(&nmrep, &nmp->nm_fsattr, &nvattr, NULL, NULL, NULL);
	nfsmout_if(error);
	if (NMFLAG(nmp, NAMEDATTR)) {
		nfsm_chain_op_check(error, &nmrep, NFS_OP_OPENATTR);
		if (error == ENOENT) {
			error = 0;
		}
		/* [sigh] some implementations return "illegal" error for unsupported ops */
		if (error || !NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_supp_attr, NFS_FATTR_NAMED_ATTR)) {
			nmp->nm_fsattr.nfsa_flags &= ~NFS_FSFLAG_NAMED_ATTR;
		} else {
			nmp->nm_fsattr.nfsa_flags |= NFS_FSFLAG_NAMED_ATTR;
		}
	} else {
		nmp->nm_fsattr.nfsa_flags &= ~NFS_FSFLAG_NAMED_ATTR;
	}
	if (NMFLAG(nmp, NOACL)) { /* make sure ACL support is turned off */
		nmp->nm_fsattr.nfsa_flags &= ~NFS_FSFLAG_ACL;
	}
	if (NMFLAG(nmp, ACLONLY) && !(nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_ACL)) {
		NFS_BITMAP_CLR(nmp->nm_flags, NFS_MFLAG_ACLONLY);
	}
	if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_supp_attr, NFS_FATTR_FH_EXPIRE_TYPE)) {
		uint32_t fhtype = ((nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_FHTYPE_MASK) >> NFS_FSFLAG_FHTYPE_SHIFT);
		if (fhtype != NFS_FH_PERSISTENT) {
			printf("nfs: warning: non-persistent file handles! for %s\n", vfs_statfs(nmp->nm_mountp)->f_mntfromname);
		}
	}

	/* make sure it's a directory */
	if (!NFS_BITMAP_ISSET(&nvattr.nva_bitmap, NFS_FATTR_TYPE) || (nvattr.nva_type != VDIR)) {
		error = ENOTDIR;
		goto nfsmout;
	}

	/* save the NFS fsid */
	nmp->nm_fsid = nvattr.nva_fsid;

	/* create the root node */
	error = nfs_nget(nmp->nm_mountp, NULL, NULL, dirfh.fh_data, dirfh.fh_len, &nvattr, &xid, rq.r_auth, NG_MARKROOT, npp);
	nfsmout_if(error);

	if (nmp->nm_fsattr.nfsa_flags & NFS_FSFLAG_ACL) {
		vfs_setextendedsecurity(nmp->nm_mountp);
	}

	/* adjust I/O sizes to server limits */
	if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_MAXREAD) && (nmp->nm_fsattr.nfsa_maxread > 0)) {
		if (nmp->nm_fsattr.nfsa_maxread < (uint64_t)nmp->nm_rsize) {
			nmp->nm_rsize = nmp->nm_fsattr.nfsa_maxread & ~(NFS_FABLKSIZE - 1);
			if (nmp->nm_rsize == 0) {
				nmp->nm_rsize = nmp->nm_fsattr.nfsa_maxread;
			}
		}
	}
	if (NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_bitmap, NFS_FATTR_MAXWRITE) && (nmp->nm_fsattr.nfsa_maxwrite > 0)) {
		if (nmp->nm_fsattr.nfsa_maxwrite < (uint64_t)nmp->nm_wsize) {
			nmp->nm_wsize = nmp->nm_fsattr.nfsa_maxwrite & ~(NFS_FABLKSIZE - 1);
			if (nmp->nm_wsize == 0) {
				nmp->nm_wsize = nmp->nm_fsattr.nfsa_maxwrite;
			}
		}
	}

	/* set up lease renew timer */
	nmp->nm_renew_timer = thread_call_allocate_with_options(nfs4_renew_timer, nmp, THREAD_CALL_PRIORITY_HIGH, THREAD_CALL_OPTIONS_ONCE);
	interval = nmp->nm_fsattr.nfsa_lease / 2;
	if (interval < 1) {
		interval = 1;
	}
	nfs_interval_timer_start(nmp->nm_renew_timer, interval * 1000);

nfsmout:
	if (fspath.np_components) {
		for (comp = 0; comp < fspath.np_compcount; comp++) {
			if (fspath.np_components[comp]) {
				FREE(fspath.np_components[comp], M_TEMP);
			}
		}
		FREE(fspath.np_components, M_TEMP);
	}
	NVATTR_CLEANUP(&nvattr);
	nfs_fs_locations_cleanup(&nfsls);
	if (*npp) {
		nfs_node_unlock(*npp);
	}
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
	return error;
}
#endif /* CONFIG_NFS4 */

/*
 * Thread to handle initial NFS mount connection.
 */
void
nfs_mount_connect_thread(void *arg, __unused wait_result_t wr)
{
	struct nfsmount *nmp = arg;
	int error = 0, savederror = 0, slpflag = (NMFLAG(nmp, INTR) ? PCATCH : 0);
	int done = 0, timeo, tries, maxtries;

	if (NM_OMFLAG(nmp, MNTQUICK)) {
		timeo = 8;
		maxtries = 1;
	} else {
		timeo = 30;
		maxtries = 2;
	}

	for (tries = 0; tries < maxtries; tries++) {
		error = nfs_connect(nmp, 1, timeo);
		switch (error) {
		case ETIMEDOUT:
		case EAGAIN:
		case EPIPE:
		case EADDRNOTAVAIL:
		case ENETDOWN:
		case ENETUNREACH:
		case ENETRESET:
		case ECONNABORTED:
		case ECONNRESET:
		case EISCONN:
		case ENOTCONN:
		case ESHUTDOWN:
		case ECONNREFUSED:
		case EHOSTDOWN:
		case EHOSTUNREACH:
			/* just keep retrying on any of these errors */
			break;
		case 0:
		default:
			/* looks like we got an answer... */
			done = 1;
			break;
		}

		/* save the best error */
		if (nfs_connect_error_class(error) >= nfs_connect_error_class(savederror)) {
			savederror = error;
		}
		if (done) {
			error = savederror;
			break;
		}

		/* pause before next attempt */
		if ((error = nfs_sigintr(nmp, NULL, current_thread(), 0))) {
			break;
		}
		error = tsleep(nmp, PSOCK | slpflag, "nfs_mount_connect_retry", 2 * hz);
		if (error && (error != EWOULDBLOCK)) {
			break;
		}
		error = savederror;
	}

	/* update status of mount connect */
	lck_mtx_lock(&nmp->nm_lock);
	if (!nmp->nm_mounterror) {
		nmp->nm_mounterror = error;
	}
	nmp->nm_state &= ~NFSSTA_MOUNT_THREAD;
	lck_mtx_unlock(&nmp->nm_lock);
	wakeup(&nmp->nm_nss);
}

int
nfs_mount_connect(struct nfsmount *nmp)
{
	int error = 0, slpflag;
	thread_t thd;
	struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };

	/*
	 * Set up the socket.  Perform initial search for a location/server/address to
	 * connect to and negotiate any unspecified mount parameters.  This work is
	 * done on a kernel thread to satisfy reserved port usage needs.
	 */
	slpflag = NMFLAG(nmp, INTR) ? PCATCH : 0;
	lck_mtx_lock(&nmp->nm_lock);
	/* set flag that the thread is running */
	nmp->nm_state |= NFSSTA_MOUNT_THREAD;
	if (kernel_thread_start(nfs_mount_connect_thread, nmp, &thd) != KERN_SUCCESS) {
		nmp->nm_state &= ~NFSSTA_MOUNT_THREAD;
		nmp->nm_mounterror = EIO;
		printf("nfs mount %s start socket connect thread failed\n", vfs_statfs(nmp->nm_mountp)->f_mntfromname);
	} else {
		thread_deallocate(thd);
	}

	/* wait until mount connect thread is finished/gone */
	while (nmp->nm_state & NFSSTA_MOUNT_THREAD) {
		error = msleep(&nmp->nm_nss, &nmp->nm_lock, slpflag | PSOCK, "nfsconnectthread", &ts);
		if ((error && (error != EWOULDBLOCK)) || ((error = nfs_sigintr(nmp, NULL, current_thread(), 1)))) {
			/* record error */
			if (!nmp->nm_mounterror) {
				nmp->nm_mounterror = error;
			}
			/* signal the thread that we are aborting */
			nmp->nm_sockflags |= NMSOCK_UNMOUNT;
			if (nmp->nm_nss) {
				wakeup(nmp->nm_nss);
			}
			/* and continue waiting on it to finish */
			slpflag = 0;
		}
	}
	lck_mtx_unlock(&nmp->nm_lock);

	/* grab mount connect status */
	error = nmp->nm_mounterror;

	return error;
}

/* Table of maximum minor version for a given version */
uint32_t maxminorverstab[] = {
	0, /* Version 0 (does not exist) */
	0, /* Version 1 (does not exist) */
	0, /* Version 2 */
	0, /* Version 3 */
	0, /* Version 4 */
};

#define NFS_MAX_SUPPORTED_VERSION  ((long)(sizeof (maxminorverstab) / sizeof (uint32_t) - 1))
#define NFS_MAX_SUPPORTED_MINOR_VERSION(v) ((long)(maxminorverstab[(v)]))

#define DEFAULT_NFS_MIN_VERS VER2PVER(2, 0)
#define DEFAULT_NFS_MAX_VERS VER2PVER(3, 0)

/*
 * Common code to mount an NFS file system.
 */
int
mountnfs(
	char *xdrbuf,
	mount_t mp,
	vfs_context_t ctx,
	vnode_t *vpp)
{
	struct nfsmount *nmp;
	nfsnode_t np;
	int error = 0;
	struct vfsstatfs *sbp;
	struct xdrbuf xb;
	uint32_t i, val, maxio, iosize, len;
	uint32_t *mattrs;
	uint32_t *mflags_mask;
	uint32_t *mflags;
	uint32_t argslength, attrslength;
	uid_t set_owner = 0;
	struct nfs_location_index firstloc = {
		.nli_flags = NLI_VALID,
		.nli_loc = 0,
		.nli_serv = 0,
		.nli_addr = 0
	};
	static const struct nfs_etype nfs_default_etypes = {
		.count = NFS_MAX_ETYPES,
		.selected = NFS_MAX_ETYPES,
		.etypes = { NFS_AES256_CTS_HMAC_SHA1_96,
			    NFS_AES128_CTS_HMAC_SHA1_96,
			    NFS_DES3_CBC_SHA1_KD}
	};

	/* make sure mbuf constants are set up */
	if (!nfs_mbuf_mhlen) {
		nfs_mbuf_init();
	}

	if (vfs_flags(mp) & MNT_UPDATE) {
		nmp = VFSTONFS(mp);
		/* update paths, file handles, etc, here	XXX */
		xb_free(xdrbuf);
		return 0;
	} else {
		/* allocate an NFS mount structure for this mount */
		nmp = zalloc_flags(nfsmnt_zone, Z_WAITOK | Z_ZERO);
		lck_mtx_init(&nmp->nm_lock, &nfs_mount_grp, LCK_ATTR_NULL);
		TAILQ_INIT(&nmp->nm_resendq);
		TAILQ_INIT(&nmp->nm_iodq);
		TAILQ_INIT(&nmp->nm_gsscl);
		LIST_INIT(&nmp->nm_monlist);
		vfs_setfsprivate(mp, nmp);
		vfs_getnewfsid(mp);
		nmp->nm_mountp = mp;
		vfs_setauthopaque(mp);
		/*
		 * Disable cache_lookup_path for NFS.  NFS lookup always needs
		 * to be called to check if the directory attribute cache is
		 * valid and possibly purge the directory before calling
		 * cache_lookup.
		 */
		vfs_setauthcache_ttl(mp, 0);

		nfs_nhinit_finish();

		nmp->nm_args = xdrbuf;

		/* set up defaults */
		nmp->nm_ref = 0;
		nmp->nm_vers = 0;
		nmp->nm_min_vers = DEFAULT_NFS_MIN_VERS;
		nmp->nm_max_vers = DEFAULT_NFS_MAX_VERS;
		nmp->nm_timeo = NFS_TIMEO;
		nmp->nm_retry = NFS_RETRANS;
		nmp->nm_sotype = 0;
		nmp->nm_sofamily = 0;
		nmp->nm_nfsport = 0;
		nmp->nm_wsize = NFS_WSIZE;
		nmp->nm_rsize = NFS_RSIZE;
		nmp->nm_readdirsize = NFS_READDIRSIZE;
		nmp->nm_numgrps = NFS_MAXGRPS;
		nmp->nm_readahead = NFS_DEFRAHEAD;
		nmp->nm_tprintf_delay = nfs_tprintf_delay;
		if (nmp->nm_tprintf_delay < 0) {
			nmp->nm_tprintf_delay = 0;
		}
		nmp->nm_tprintf_initial_delay = nfs_tprintf_initial_delay;
		if (nmp->nm_tprintf_initial_delay < 0) {
			nmp->nm_tprintf_initial_delay = 0;
		}
		nmp->nm_acregmin = NFS_MINATTRTIMO;
		nmp->nm_acregmax = NFS_MAXATTRTIMO;
		nmp->nm_acdirmin = NFS_MINDIRATTRTIMO;
		nmp->nm_acdirmax = NFS_MAXDIRATTRTIMO;
		nmp->nm_etype = nfs_default_etypes;
		nmp->nm_auth = RPCAUTH_SYS;
		nmp->nm_iodlink.tqe_next = NFSNOLIST;
		nmp->nm_deadtimeout = 0;
		nmp->nm_curdeadtimeout = 0;
		NFS_BITMAP_SET(nmp->nm_flags, NFS_MFLAG_RDIRPLUS); /* enable RDIRPLUS by default. It will be reverted later in case NFSv2 is used */
		NFS_BITMAP_SET(nmp->nm_flags, NFS_MFLAG_NOACL);
		nmp->nm_realm = NULL;
		nmp->nm_principal = NULL;
		nmp->nm_sprinc = NULL;
	}

	mattrs = nmp->nm_mattrs;
	mflags = nmp->nm_mflags;
	mflags_mask = nmp->nm_mflags_mask;

	/* set up NFS mount with args */
	xb_init_buffer(&xb, xdrbuf, 2 * XDRWORD);
	xb_get_32(error, &xb, val); /* version */
	xb_get_32(error, &xb, argslength); /* args length */
	nfsmerr_if(error);
	xb_init_buffer(&xb, xdrbuf, argslength);        /* restart parsing with actual buffer length */
	xb_get_32(error, &xb, val); /* version */
	xb_get_32(error, &xb, argslength); /* args length */
	xb_get_32(error, &xb, val); /* XDR args version */
	if (val != NFS_XDRARGS_VERSION_0 || argslength < ((4 + NFS_MATTR_BITMAP_LEN + 1) * XDRWORD)) {
		error = EINVAL;
	}
	len = NFS_MATTR_BITMAP_LEN;
	xb_get_bitmap(error, &xb, mattrs, len); /* mount attribute bitmap */
	attrslength = 0;
	xb_get_32(error, &xb, attrslength); /* attrs length */
	if (!error && (attrslength > (argslength - ((4 + NFS_MATTR_BITMAP_LEN + 1) * XDRWORD)))) {
		error = EINVAL;
	}
	nfsmerr_if(error);
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_FLAGS)) {
		len = NFS_MFLAG_BITMAP_LEN;
		xb_get_bitmap(error, &xb, mflags_mask, len); /* mount flag mask */
		len = NFS_MFLAG_BITMAP_LEN;
		xb_get_bitmap(error, &xb, mflags, len); /* mount flag values */
		if (!error) {
			/* clear all mask bits and OR in all the ones that are set */
			nmp->nm_flags[0] &= ~mflags_mask[0];
			nmp->nm_flags[0] |= (mflags_mask[0] & mflags[0]);
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_VERSION)) {
		/* Can't specify a single version and a range */
		if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_VERSION_RANGE)) {
			error = EINVAL;
		}
		xb_get_32(error, &xb, nmp->nm_vers);
		if (nmp->nm_vers > NFS_MAX_SUPPORTED_VERSION ||
		    nmp->nm_vers < NFS_VER2) {
			error = EINVAL;
		}
		if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_MINOR_VERSION)) {
			xb_get_32(error, &xb, nmp->nm_minor_vers);
		} else {
			nmp->nm_minor_vers = maxminorverstab[nmp->nm_vers];
		}
		if (nmp->nm_minor_vers > maxminorverstab[nmp->nm_vers]) {
			error = EINVAL;
		}
		nmp->nm_max_vers = nmp->nm_min_vers =
		    VER2PVER(nmp->nm_vers, nmp->nm_minor_vers);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_MINOR_VERSION)) {
		/* should have also gotten NFS version (and already gotten minor version) */
		if (!NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_VERSION)) {
			error = EINVAL;
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_VERSION_RANGE)) {
		xb_get_32(error, &xb, nmp->nm_min_vers);
		xb_get_32(error, &xb, nmp->nm_max_vers);
		if ((nmp->nm_min_vers > nmp->nm_max_vers) ||
		    (PVER2MAJOR(nmp->nm_max_vers) > NFS_MAX_SUPPORTED_VERSION) ||
		    (PVER2MINOR(nmp->nm_min_vers) > maxminorverstab[PVER2MAJOR(nmp->nm_min_vers)]) ||
		    (PVER2MINOR(nmp->nm_max_vers) > maxminorverstab[PVER2MAJOR(nmp->nm_max_vers)])) {
			error = EINVAL;
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READ_SIZE)) {
		xb_get_32(error, &xb, nmp->nm_rsize);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_WRITE_SIZE)) {
		xb_get_32(error, &xb, nmp->nm_wsize);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READDIR_SIZE)) {
		xb_get_32(error, &xb, nmp->nm_readdirsize);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READAHEAD)) {
		xb_get_32(error, &xb, nmp->nm_readahead);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_REG_MIN)) {
		xb_get_32(error, &xb, nmp->nm_acregmin);
		xb_skip(error, &xb, XDRWORD);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_REG_MAX)) {
		xb_get_32(error, &xb, nmp->nm_acregmax);
		xb_skip(error, &xb, XDRWORD);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MIN)) {
		xb_get_32(error, &xb, nmp->nm_acdirmin);
		xb_skip(error, &xb, XDRWORD);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MAX)) {
		xb_get_32(error, &xb, nmp->nm_acdirmax);
		xb_skip(error, &xb, XDRWORD);
	}
	nfsmerr_if(error);
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCK_MODE)) {
		xb_get_32(error, &xb, val);
		switch (val) {
		case NFS_LOCK_MODE_DISABLED:
		case NFS_LOCK_MODE_LOCAL:
#if CONFIG_NFS4
			if (nmp->nm_vers >= NFS_VER4) {
				/* disabled/local lock mode only allowed on v2/v3 */
				error = EINVAL;
				break;
			}
#endif
			OS_FALLTHROUGH;
		case NFS_LOCK_MODE_ENABLED:
			nmp->nm_lockmode = val;
			break;
		default:
			error = EINVAL;
		}
	}
	nfsmerr_if(error);
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SECURITY)) {
		uint32_t seccnt;
		xb_get_32(error, &xb, seccnt);
		if (!error && ((seccnt < 1) || (seccnt > NX_MAX_SEC_FLAVORS))) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		nmp->nm_sec.count = seccnt;
		for (i = 0; i < seccnt; i++) {
			xb_get_32(error, &xb, nmp->nm_sec.flavors[i]);
			/* Check for valid security flavor */
			switch (nmp->nm_sec.flavors[i]) {
			case RPCAUTH_NONE:
			case RPCAUTH_SYS:
			case RPCAUTH_KRB5:
			case RPCAUTH_KRB5I:
			case RPCAUTH_KRB5P:
				break;
			default:
				error = EINVAL;
			}
		}
		/* start with the first flavor */
		nmp->nm_auth = nmp->nm_sec.flavors[0];
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_KERB_ETYPE)) {
		uint32_t etypecnt;
		xb_get_32(error, &xb, etypecnt);
		if (!error && ((etypecnt < 1) || (etypecnt > NFS_MAX_ETYPES))) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		nmp->nm_etype.count = etypecnt;
		xb_get_32(error, &xb, nmp->nm_etype.selected);
		nfsmerr_if(error);
		if (etypecnt) {
			nmp->nm_etype.selected = etypecnt; /* Nothing is selected yet, so set selected to count */
			for (i = 0; i < etypecnt; i++) {
				xb_get_32(error, &xb, nmp->nm_etype.etypes[i]);
				/* Check for valid encryption type */
				switch (nmp->nm_etype.etypes[i]) {
				case NFS_DES3_CBC_SHA1_KD:
				case NFS_AES128_CTS_HMAC_SHA1_96:
				case NFS_AES256_CTS_HMAC_SHA1_96:
					break;
				default:
					error = EINVAL;
				}
			}
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MAX_GROUP_LIST)) {
		xb_get_32(error, &xb, nmp->nm_numgrps);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SOCKET_TYPE)) {
		char sotype[16];

		*sotype = '\0';
		xb_get_32(error, &xb, val);
		if (!error && ((val < 3) || (val > sizeof(sotype)))) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		error = xb_get_bytes(&xb, sotype, val, 0);
		nfsmerr_if(error);
		sotype[val] = '\0';
		if (!strcmp(sotype, "tcp")) {
			nmp->nm_sotype = SOCK_STREAM;
		} else if (!strcmp(sotype, "udp")) {
			nmp->nm_sotype = SOCK_DGRAM;
		} else if (!strcmp(sotype, "tcp4")) {
			nmp->nm_sotype = SOCK_STREAM;
			nmp->nm_sofamily = AF_INET;
		} else if (!strcmp(sotype, "udp4")) {
			nmp->nm_sotype = SOCK_DGRAM;
			nmp->nm_sofamily = AF_INET;
		} else if (!strcmp(sotype, "tcp6")) {
			nmp->nm_sotype = SOCK_STREAM;
			nmp->nm_sofamily = AF_INET6;
		} else if (!strcmp(sotype, "udp6")) {
			nmp->nm_sotype = SOCK_DGRAM;
			nmp->nm_sofamily = AF_INET6;
		} else if (!strcmp(sotype, "inet4")) {
			nmp->nm_sofamily = AF_INET;
		} else if (!strcmp(sotype, "inet6")) {
			nmp->nm_sofamily = AF_INET6;
		} else if (!strcmp(sotype, "inet")) {
			nmp->nm_sofamily = 0; /* ok */
		} else if (!strcmp(sotype, "ticotsord")) {
			nmp->nm_sofamily = AF_LOCAL;
			nmp->nm_sotype = SOCK_STREAM;
		} else if (!strcmp(sotype, "ticlts")) {
			nmp->nm_sofamily = AF_LOCAL;
			nmp->nm_sotype = SOCK_DGRAM;
		} else {
			error = EINVAL;
		}
#if CONFIG_NFS4
		if (!error && (nmp->nm_vers >= NFS_VER4) && nmp->nm_sotype &&
		    (nmp->nm_sotype != SOCK_STREAM)) {
			error = EINVAL;         /* NFSv4 is only allowed over TCP. */
		}
#endif
		if (error) {
			NFS_VFS_DBG("EINVAL sotype = \"%s\"\n", sotype);
		}
		nfsmerr_if(error);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_PORT)) {
		xb_get_32(error, &xb, val);
		if (NFS_PORT_INVALID(val)) {
			error = EINVAL;
			nfsmerr_if(error);
		}
		nmp->nm_nfsport = (in_port_t)val;
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MOUNT_PORT)) {
		xb_get_32(error, &xb, val);
		if (NFS_PORT_INVALID(val)) {
			error = EINVAL;
			nfsmerr_if(error);
		}
		nmp->nm_mountport = (in_port_t)val;
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_REQUEST_TIMEOUT)) {
		/* convert from time to 0.1s units */
		xb_get_32(error, &xb, nmp->nm_timeo);
		xb_get_32(error, &xb, val);
		nfsmerr_if(error);
		if (val >= 1000000000) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		nmp->nm_timeo *= 10;
		nmp->nm_timeo += (val + 100000000 - 1) / 100000000;
		/* now convert to ticks */
		nmp->nm_timeo = (nmp->nm_timeo * NFS_HZ + 5) / 10;
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SOFT_RETRY_COUNT)) {
		xb_get_32(error, &xb, val);
		if (!error && (val > 1)) {
			nmp->nm_retry = val;
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_DEAD_TIMEOUT)) {
		xb_get_32(error, &xb, nmp->nm_deadtimeout);
		xb_skip(error, &xb, XDRWORD);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_FH)) {
		nfsmerr_if(error);
		nmp->nm_fh = zalloc(nfs_fhandle_zone);
		xb_get_32(error, &xb, nmp->nm_fh->fh_len);
		nfsmerr_if(error);
		if ((size_t)nmp->nm_fh->fh_len > sizeof(nmp->nm_fh->fh_data)) {
			error = EINVAL;
		} else {
			error = xb_get_bytes(&xb, (char*)&nmp->nm_fh->fh_data[0], nmp->nm_fh->fh_len, 0);
		}
	}
	nfsmerr_if(error);
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_FS_LOCATIONS)) {
		uint32_t loc, serv, addr, comp;
		struct nfs_fs_location *fsl;
		struct nfs_fs_server *fss;
		struct nfs_fs_path *fsp;

		xb_get_32(error, &xb, nmp->nm_locations.nl_numlocs); /* fs location count */
		/* sanity check location count */
		if (!error && ((nmp->nm_locations.nl_numlocs < 1) || (nmp->nm_locations.nl_numlocs > 256))) {
			NFS_VFS_DBG("Invalid number of fs_locations: %d", nmp->nm_locations.nl_numlocs);
			error = EINVAL;
		}
		nfsmerr_if(error);
		MALLOC(nmp->nm_locations.nl_locations, struct nfs_fs_location **, nmp->nm_locations.nl_numlocs * sizeof(struct nfs_fs_location*), M_TEMP, M_WAITOK | M_ZERO);
		if (!nmp->nm_locations.nl_locations) {
			error = ENOMEM;
		}
		for (loc = 0; loc < nmp->nm_locations.nl_numlocs; loc++) {
			nfsmerr_if(error);
			MALLOC(fsl, struct nfs_fs_location *, sizeof(struct nfs_fs_location), M_TEMP, M_WAITOK | M_ZERO);
			if (!fsl) {
				error = ENOMEM;
			}
			nmp->nm_locations.nl_locations[loc] = fsl;
			xb_get_32(error, &xb, fsl->nl_servcount); /* server count */
			/* sanity check server count */
			if (!error && ((fsl->nl_servcount < 1) || (fsl->nl_servcount > 256))) {
				NFS_VFS_DBG("Invalid server count %d", fsl->nl_servcount);
				error = EINVAL;
			}
			nfsmerr_if(error);
			MALLOC(fsl->nl_servers, struct nfs_fs_server **, fsl->nl_servcount * sizeof(struct nfs_fs_server*), M_TEMP, M_WAITOK | M_ZERO);
			if (!fsl->nl_servers) {
				error = ENOMEM;
				NFS_VFS_DBG("Server count = %d, error = %d\n", fsl->nl_servcount, error);
			}
			for (serv = 0; serv < fsl->nl_servcount; serv++) {
				nfsmerr_if(error);
				MALLOC(fss, struct nfs_fs_server *, sizeof(struct nfs_fs_server), M_TEMP, M_WAITOK | M_ZERO);
				if (!fss) {
					error = ENOMEM;
				}
				fsl->nl_servers[serv] = fss;
				xb_get_32(error, &xb, val); /* server name length */
				/* sanity check server name length */
				if (!error && (val > MAXPATHLEN)) {
					NFS_VFS_DBG("Invalid server name length %d", val);
					error = EINVAL;
				}
				nfsmerr_if(error);
				MALLOC(fss->ns_name, char *, val + 1, M_TEMP, M_WAITOK | M_ZERO);
				if (!fss->ns_name) {
					error = ENOMEM;
				}
				nfsmerr_if(error);
				error = xb_get_bytes(&xb, fss->ns_name, val, 0); /* server name */
				xb_get_32(error, &xb, fss->ns_addrcount); /* address count */
				/* sanity check address count (OK to be zero) */
				if (!error && (fss->ns_addrcount > 256)) {
					NFS_VFS_DBG("Invalid address count %d", fss->ns_addrcount);
					error = EINVAL;
				}
				nfsmerr_if(error);
				if (fss->ns_addrcount > 0) {
					MALLOC(fss->ns_addresses, char **, fss->ns_addrcount * sizeof(char *), M_TEMP, M_WAITOK | M_ZERO);
					if (!fss->ns_addresses) {
						error = ENOMEM;
					}
					for (addr = 0; addr < fss->ns_addrcount; addr++) {
						xb_get_32(error, &xb, val); /* address length */
						/* sanity check address length */
						if (!error && val > 128) {
							NFS_VFS_DBG("Invalid address length %d", val);
							error = EINVAL;
						}
						nfsmerr_if(error);
						MALLOC(fss->ns_addresses[addr], char *, val + 1, M_TEMP, M_WAITOK | M_ZERO);
						if (!fss->ns_addresses[addr]) {
							error = ENOMEM;
						}
						nfsmerr_if(error);
						error = xb_get_bytes(&xb, fss->ns_addresses[addr], val, 0); /* address */
					}
				}
				xb_get_32(error, &xb, val); /* server info length */
				xb_skip(error, &xb, val); /* skip server info */
			}
			/* get pathname */
			fsp = &fsl->nl_path;
			xb_get_32(error, &xb, fsp->np_compcount); /* component count */
			/* sanity check component count */
			if (!error && (fsp->np_compcount > MAXPATHLEN)) {
				NFS_VFS_DBG("Invalid component count %d", fsp->np_compcount);
				error = EINVAL;
			}
			nfsmerr_if(error);
			if (fsp->np_compcount) {
				MALLOC(fsp->np_components, char **, fsp->np_compcount * sizeof(char*), M_TEMP, M_WAITOK | M_ZERO);
				if (!fsp->np_components) {
					error = ENOMEM;
				}
			}
			for (comp = 0; comp < fsp->np_compcount; comp++) {
				xb_get_32(error, &xb, val); /* component length */
				/* sanity check component length */
				if (!error && (val == 0)) {
					/*
					 * Apparently some people think a path with zero components should
					 * be encoded with one zero-length component.  So, just ignore any
					 * zero length components.
					 */
					comp--;
					fsp->np_compcount--;
					if (fsp->np_compcount == 0) {
						FREE(fsp->np_components, M_TEMP);
						fsp->np_components = NULL;
					}
					continue;
				}
				if (!error && ((val < 1) || (val > MAXPATHLEN))) {
					NFS_VFS_DBG("Invalid component path length %d", val);
					error = EINVAL;
				}
				nfsmerr_if(error);
				MALLOC(fsp->np_components[comp], char *, val + 1, M_TEMP, M_WAITOK | M_ZERO);
				if (!fsp->np_components[comp]) {
					error = ENOMEM;
				}
				nfsmerr_if(error);
				error = xb_get_bytes(&xb, fsp->np_components[comp], val, 0); /* component */
			}
			xb_get_32(error, &xb, val); /* fs location info length */
			NFS_VFS_DBG("Skipping fs location info bytes %d", val);
			xb_skip(error, &xb, xdr_rndup(val)); /* skip fs location info */
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MNTFLAGS)) {
		xb_skip(error, &xb, XDRWORD);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MNTFROM)) {
		xb_get_32(error, &xb, len);
		nfsmerr_if(error);
		val = len;
		if (val >= sizeof(vfs_statfs(mp)->f_mntfromname)) {
			val = sizeof(vfs_statfs(mp)->f_mntfromname) - 1;
		}
		error = xb_get_bytes(&xb, vfs_statfs(mp)->f_mntfromname, val, 0);
		if ((len - val) > 0) {
			xb_skip(error, &xb, len - val);
		}
		nfsmerr_if(error);
		vfs_statfs(mp)->f_mntfromname[val] = '\0';
	}
	nfsmerr_if(error);

	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_REALM)) {
		xb_get_32(error, &xb, len);
		if (!error && ((len < 1) || (len > MAXPATHLEN))) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		/* allocate an extra byte for a leading '@' if its not already prepended to the realm */
		MALLOC(nmp->nm_realm, char *, len + 2, M_TEMP, M_WAITOK | M_ZERO);
		if (!nmp->nm_realm) {
			error = ENOMEM;
		}
		nfsmerr_if(error);
		error = xb_get_bytes(&xb, nmp->nm_realm, len, 0);
		if (error == 0 && *nmp->nm_realm != '@') {
			bcopy(nmp->nm_realm, &nmp->nm_realm[1], len);
			nmp->nm_realm[0] = '@';
		}
	}
	nfsmerr_if(error);

	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_PRINCIPAL)) {
		xb_get_32(error, &xb, len);
		if (!error && ((len < 1) || (len > MAXPATHLEN))) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		MALLOC(nmp->nm_principal, char *, len + 1, M_TEMP, M_WAITOK | M_ZERO);
		if (!nmp->nm_principal) {
			error = ENOMEM;
		}
		nfsmerr_if(error);
		error = xb_get_bytes(&xb, nmp->nm_principal, len, 0);
	}
	nfsmerr_if(error);

	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SVCPRINCIPAL)) {
		xb_get_32(error, &xb, len);
		if (!error && ((len < 1) || (len > MAXPATHLEN))) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		MALLOC(nmp->nm_sprinc, char *, len + 1, M_TEMP, M_WAITOK | M_ZERO);
		if (!nmp->nm_sprinc) {
			error = ENOMEM;
		}
		nfsmerr_if(error);
		error = xb_get_bytes(&xb, nmp->nm_sprinc, len, 0);
	}
	nfsmerr_if(error);

	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCAL_NFS_PORT)) {
		if (nmp->nm_nfsport) {
			error = EINVAL;
			NFS_VFS_DBG("Can't have ports specified over incompatible socket families");
		}
		nfsmerr_if(error);
		xb_get_32(error, &xb, len);
		if (!error && ((len < 1) || (len > sizeof(((struct sockaddr_un *)0)->sun_path)))) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		MALLOC(nmp->nm_nfs_localport, char *, len + 1, M_TEMP, M_WAITOK | M_ZERO);
		if (!nmp->nm_nfs_localport) {
			error = ENOMEM;
		}
		nfsmerr_if(error);
		error = xb_get_bytes(&xb, nmp->nm_nfs_localport, len, 0);
		nmp->nm_sofamily = AF_LOCAL;
		nmp->nm_nfsport = 1; /* We use the now deprecated tpcmux port to indcate that we have an AF_LOCAL port */
		NFS_VFS_DBG("Setting nfs local port %s (%d)\n", nmp->nm_nfs_localport, nmp->nm_nfsport);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCAL_MOUNT_PORT)) {
		if (nmp->nm_mountport) {
			error = EINVAL;
			NFS_VFS_DBG("Can't have ports specified over mulitple socket families");
		}
		nfsmerr_if(error);
		xb_get_32(error, &xb, len);
		if (!error && ((len < 1) || (len > sizeof(((struct sockaddr_un *)0)->sun_path)))) {
			error = EINVAL;
		}
		nfsmerr_if(error);
		MALLOC(nmp->nm_mount_localport, char *, len + 1, M_TEMP, M_WAITOK | M_ZERO);
		if (!nmp->nm_mount_localport) {
			error = ENOMEM;
		}
		nfsmerr_if(error);
		error = xb_get_bytes(&xb, nmp->nm_mount_localport, len, 0);
		nmp->nm_sofamily = AF_LOCAL;
		nmp->nm_mountport = 1; /* We use the now deprecated tpcmux port to indcate that we have an AF_LOCAL port */
		NFS_VFS_DBG("Setting mount local port %s (%d)\n", nmp->nm_mount_localport, nmp->nm_mountport);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SET_MOUNT_OWNER)) {
		xb_get_32(error, &xb, set_owner);
		nfsmerr_if(error);
		error = vfs_context_suser(ctx);
		/*
		 * root can set owner to whatever, user can set owner to self
		 */
		if ((error) && (set_owner == kauth_cred_getuid(vfs_context_ucred(ctx)))) {
			/* ok for non-root can set owner to self */
			error = 0;
		}
		nfsmerr_if(error);
	}

	/*
	 * Sanity check/finalize settings.
	 */

	if (nmp->nm_timeo < NFS_MINTIMEO) {
		nmp->nm_timeo = NFS_MINTIMEO;
	} else if (nmp->nm_timeo > NFS_MAXTIMEO) {
		nmp->nm_timeo = NFS_MAXTIMEO;
	}
	if (nmp->nm_retry > NFS_MAXREXMIT) {
		nmp->nm_retry = NFS_MAXREXMIT;
	}

	if (nmp->nm_numgrps > NFS_MAXGRPS) {
		nmp->nm_numgrps = NFS_MAXGRPS;
	}
	if (nmp->nm_readahead > NFS_MAXRAHEAD) {
		nmp->nm_readahead = NFS_MAXRAHEAD;
	}
	if (nmp->nm_acregmin > nmp->nm_acregmax) {
		nmp->nm_acregmin = nmp->nm_acregmax;
	}
	if (nmp->nm_acdirmin > nmp->nm_acdirmax) {
		nmp->nm_acdirmin = nmp->nm_acdirmax;
	}

	/* need at least one fs location */
	if (nmp->nm_locations.nl_numlocs < 1) {
		error = EINVAL;
	}
	nfsmerr_if(error);

	if (!NM_OMATTR_GIVEN(nmp, MNTFROM)) {
		/* init mount's mntfromname to first location */
		nfs_location_mntfromname(&nmp->nm_locations, firstloc,
		    vfs_statfs(mp)->f_mntfromname,
		    sizeof(vfs_statfs(mp)->f_mntfromname), 0);
	}

	/* Need to save the mounting credential for v4. */
	nmp->nm_mcred = vfs_context_ucred(ctx);
	if (IS_VALID_CRED(nmp->nm_mcred)) {
		kauth_cred_ref(nmp->nm_mcred);
	}

	/*
	 * If a reserved port is required, check for that privilege.
	 * (Note that mirror mounts are exempt because the privilege was
	 * already checked for the original mount.)
	 */
	if (NMFLAG(nmp, RESVPORT) && !vfs_iskernelmount(mp)) {
		error = priv_check_cred(nmp->nm_mcred, PRIV_NETINET_RESERVEDPORT, 0);
	}
	nfsmerr_if(error);

	/* set up the version-specific function tables */
	if (nmp->nm_vers < NFS_VER4) {
		nmp->nm_funcs = &nfs3_funcs;
	} else {
#if CONFIG_NFS4
		nmp->nm_funcs = &nfs4_funcs;
#else
		/* don't go any further if we don't support NFS4 */
		nmp->nm_funcs = NULL;
		error = ENOTSUP;
		nfsmerr_if(error);
#endif
	}

	/* do mount's initial socket connection */
	error = nfs_mount_connect(nmp);
	nfsmerr_if(error);

	/* sanity check settings now that version/connection is set */
	if (nmp->nm_vers == NFS_VER2) {         /* ignore RDIRPLUS on NFSv2 */
		NFS_BITMAP_CLR(nmp->nm_flags, NFS_MFLAG_RDIRPLUS);
	}
#if CONFIG_NFS4
	if (nmp->nm_vers >= NFS_VER4) {
		if (NFS_BITMAP_ISSET(nmp->nm_flags, NFS_MFLAG_ACLONLY)) { /* aclonly trumps noacl */
			NFS_BITMAP_CLR(nmp->nm_flags, NFS_MFLAG_NOACL);
		}
		NFS_BITMAP_CLR(nmp->nm_flags, NFS_MFLAG_CALLUMNT);
		if (nmp->nm_lockmode != NFS_LOCK_MODE_ENABLED) {
			error = EINVAL; /* disabled/local lock mode only allowed on v2/v3 */
		}
	} else {
#endif
	/* ignore these if not v4 */
	NFS_BITMAP_CLR(nmp->nm_flags, NFS_MFLAG_NOCALLBACK);
	NFS_BITMAP_CLR(nmp->nm_flags, NFS_MFLAG_NAMEDATTR);
	NFS_BITMAP_CLR(nmp->nm_flags, NFS_MFLAG_NOACL);
	NFS_BITMAP_CLR(nmp->nm_flags, NFS_MFLAG_ACLONLY);
#if CONFIG_NFS4
}
#endif
	nfsmerr_if(error);

	if (nmp->nm_sotype == SOCK_DGRAM) {
		/* I/O size defaults for UDP are different */
		if (!NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READ_SIZE)) {
			nmp->nm_rsize = NFS_DGRAM_RSIZE;
		}
		if (!NFS_BITMAP_ISSET(mattrs, NFS_MATTR_WRITE_SIZE)) {
			nmp->nm_wsize = NFS_DGRAM_WSIZE;
		}
	}

	/* round down I/O sizes to multiple of NFS_FABLKSIZE */
	nmp->nm_rsize &= ~(NFS_FABLKSIZE - 1);
	if (nmp->nm_rsize <= 0) {
		nmp->nm_rsize = NFS_FABLKSIZE;
	}
	nmp->nm_wsize &= ~(NFS_FABLKSIZE - 1);
	if (nmp->nm_wsize <= 0) {
		nmp->nm_wsize = NFS_FABLKSIZE;
	}

	/* and limit I/O sizes to maximum allowed */
	maxio = (nmp->nm_vers == NFS_VER2) ? NFS_V2MAXDATA :
	    (nmp->nm_sotype == SOCK_DGRAM) ? NFS_MAXDGRAMDATA : NFS_MAXDATA;
	if (maxio > NFS_MAXBSIZE) {
		maxio = NFS_MAXBSIZE;
	}
	if (nmp->nm_rsize > maxio) {
		nmp->nm_rsize = maxio;
	}
	if (nmp->nm_wsize > maxio) {
		nmp->nm_wsize = maxio;
	}

	if (nmp->nm_readdirsize > maxio) {
		nmp->nm_readdirsize = maxio;
	}
	if (nmp->nm_readdirsize > nmp->nm_rsize) {
		nmp->nm_readdirsize = nmp->nm_rsize;
	}

	/* Set up the sockets and related info */
	if (nmp->nm_sotype == SOCK_DGRAM) {
		TAILQ_INIT(&nmp->nm_cwndq);
	}

	if (nmp->nm_saddr->sa_family == AF_LOCAL) {
		struct sockaddr_un *un = (struct sockaddr_un *)nmp->nm_saddr;
		size_t size;
		int n = snprintf(vfs_statfs(mp)->f_mntfromname, sizeof(vfs_statfs(mp)->f_mntfromname), "<%s>:", un->sun_path);

		if (n > 0 && (size_t)n < sizeof(vfs_statfs(mp)->f_mntfromname)) {
			size = sizeof(vfs_statfs(mp)->f_mntfromname) - n;
			nfs_location_mntfromname(&nmp->nm_locations, firstloc,
			    &vfs_statfs(mp)->f_mntfromname[n], size, 1);
		}
	}


	/*
	 * Get the root node/attributes from the NFS server and
	 * do any basic, version-specific setup.
	 */
	error = nmp->nm_funcs->nf_mount(nmp, ctx, &np);
	nfsmerr_if(error);

	/*
	 * A reference count is needed on the node representing the
	 * remote root.  If this object is not persistent, then backward
	 * traversals of the mount point (i.e. "..") will not work if
	 * the node gets flushed out of the cache.
	 */
	nmp->nm_dnp = np;
	*vpp = NFSTOV(np);


	/* get usecount and drop iocount */
	error = vnode_ref(*vpp);
	vnode_put(*vpp);
	if (error) {
		vnode_recycle(*vpp);
		goto nfsmerr;
	}

	/*
	 * Do statfs to ensure static info gets set to reasonable values.
	 */
	if ((error = nmp->nm_funcs->nf_update_statfs(nmp, ctx))) {
		int error2 = vnode_getwithref(*vpp);
		vnode_rele(*vpp);
		if (!error2) {
			vnode_put(*vpp);
		}
		vnode_recycle(*vpp);
		goto nfsmerr;
	}
	sbp = vfs_statfs(mp);
	sbp->f_bsize = nmp->nm_fsattr.nfsa_bsize;
	sbp->f_blocks = nmp->nm_fsattr.nfsa_space_total / sbp->f_bsize;
	sbp->f_bfree = nmp->nm_fsattr.nfsa_space_free / sbp->f_bsize;
	sbp->f_bavail = nmp->nm_fsattr.nfsa_space_avail / sbp->f_bsize;
	sbp->f_bused = (nmp->nm_fsattr.nfsa_space_total / sbp->f_bsize) -
	    (nmp->nm_fsattr.nfsa_space_free / sbp->f_bsize);
	sbp->f_files = nmp->nm_fsattr.nfsa_files_total;
	sbp->f_ffree = nmp->nm_fsattr.nfsa_files_free;
	sbp->f_iosize = nfs_iosize;

	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SET_MOUNT_OWNER)) {
		sbp->f_owner = set_owner;
	}

	/*
	 * Calculate the size used for I/O buffers.  Use the larger
	 * of the two sizes to minimise NFS requests but make sure
	 * that it is at least one VM page to avoid wasting buffer
	 * space and to allow easy mmapping of I/O buffers.
	 * The read/write RPC calls handle the splitting up of
	 * buffers into multiple requests if the buffer size is
	 * larger than the I/O size.
	 */
	iosize = max(nmp->nm_rsize, nmp->nm_wsize);
	if (iosize < PAGE_SIZE) {
		iosize = PAGE_SIZE;
	}
	nmp->nm_biosize = trunc_page_32(iosize);

	/* For NFSv3 and greater, there is a (relatively) reliable ACCESS call. */
	if (nmp->nm_vers > NFS_VER2 && !NMFLAG(nmp, NOOPAQUE_AUTH)
	    ) {
		vfs_setauthopaqueaccess(mp);
	}

	switch (nmp->nm_lockmode) {
	case NFS_LOCK_MODE_DISABLED:
		break;
	case NFS_LOCK_MODE_LOCAL:
		vfs_setlocklocal(nmp->nm_mountp);
		break;
	case NFS_LOCK_MODE_ENABLED:
	default:
		if (nmp->nm_vers <= NFS_VER3) {
			nfs_lockd_mount_register(nmp);
		}
		break;
	}


	/* success! */
	lck_mtx_lock(&nmp->nm_lock);
	nmp->nm_state |= NFSSTA_MOUNTED;
	lck_mtx_unlock(&nmp->nm_lock);
	return 0;
nfsmerr:
	nfs_mount_drain_and_cleanup(nmp);
	return error;
}

#if CONFIG_TRIGGERS

/*
 * We've detected a file system boundary on the server and
 * need to mount a new file system so that our file systems
 * MIRROR the file systems on the server.
 *
 * Build the mount arguments for the new mount and call kernel_mount().
 */
int
nfs_mirror_mount_domount(vnode_t dvp, vnode_t vp, vfs_context_t ctx)
{
	nfsnode_t np = VTONFS(vp);
#if CONFIG_NFS4
	nfsnode_t dnp = VTONFS(dvp);
#endif
	struct nfsmount *nmp = NFSTONMP(np);
	char fstype[MFSTYPENAMELEN], *mntfromname = NULL, *path = NULL, *relpath, *p, *cp;
	int error = 0, pathbuflen = MAXPATHLEN, i, mntflags = 0, referral, skipcopy = 0;
	size_t nlen, rlen, mlen, mlen2, count;
	struct xdrbuf xb, xbnew;
	uint32_t mattrs[NFS_MATTR_BITMAP_LEN];
	uint32_t newmattrs[NFS_MATTR_BITMAP_LEN];
	uint32_t newmflags[NFS_MFLAG_BITMAP_LEN];
	uint32_t newmflags_mask[NFS_MFLAG_BITMAP_LEN];
	uint32_t val, relpathcomps;
	uint64_t argslength = 0, argslength_offset, attrslength_offset, end_offset;
	uint32_t numlocs, loc, numserv, serv, numaddr, addr, numcomp, comp;
	char buf[XDRWORD];
	struct nfs_fs_locations nfsls;

	referral = (np->n_vattr.nva_flags & NFS_FFLAG_TRIGGER_REFERRAL);
	if (referral) {
		bzero(&nfsls, sizeof(nfsls));
	}

	xb_init(&xbnew, XDRBUF_NONE);

	if (!nmp || (nmp->nm_state & (NFSSTA_FORCE | NFSSTA_DEAD))) {
		return ENXIO;
	}

	/* allocate a couple path buffers we need */
	mntfromname = zalloc(ZV_NAMEI);
	path = zalloc(ZV_NAMEI);

	/* get the path for the directory being mounted on */
	error = vn_getpath(vp, path, &pathbuflen);
	if (error) {
		error = ENOMEM;
		goto nfsmerr;
	}

	/*
	 * Set up the mntfromname for the new mount based on the
	 * current mount's mntfromname and the directory's path
	 * relative to the current mount's mntonname.
	 * Set up relpath to point at the relative path on the current mount.
	 * Also, count the number of components in relpath.
	 * We'll be adding those to each fs location path in the new args.
	 */
	nlen = strlcpy(mntfromname, vfs_statfs(nmp->nm_mountp)->f_mntfromname, MAXPATHLEN);
	if ((nlen > 0) && (mntfromname[nlen - 1] == '/')) { /* avoid double '/' in new name */
		mntfromname[nlen - 1] = '\0';
		nlen--;
	}
	relpath = mntfromname + nlen;
	nlen = strlcat(mntfromname, path + strlen(vfs_statfs(nmp->nm_mountp)->f_mntonname), MAXPATHLEN);
	if (nlen >= MAXPATHLEN) {
		error = ENAMETOOLONG;
		goto nfsmerr;
	}
	/* count the number of components in relpath */
	p = relpath;
	while (*p && (*p == '/')) {
		p++;
	}
	relpathcomps = 0;
	while (*p) {
		relpathcomps++;
		while (*p && (*p != '/')) {
			p++;
		}
		while (*p && (*p == '/')) {
			p++;
		}
	}

	/* grab a copy of the file system type */
	vfs_name(vnode_mount(vp), fstype);

	/* for referrals, fetch the fs locations */
	if (referral) {
		const char *vname = vnode_getname(NFSTOV(np));
		if (!vname) {
			error = ENOENT;
		}
#if CONFIG_NFS4
		else {
			error = nfs4_get_fs_locations(nmp, dnp, NULL, 0, vname, ctx, &nfsls);
			vnode_putname(vname);
			if (!error && (nfsls.nl_numlocs < 1)) {
				error = ENOENT;
			}
		}
#endif
		nfsmerr_if(error);
	}

	/* set up NFS mount args based on current mount args */

#define xb_copy_32(E, XBSRC, XBDST, V) \
	do { \
	        if (E) break; \
	        xb_get_32((E), (XBSRC), (V)); \
	        if (skipcopy) break; \
	        xb_add_32((E), (XBDST), (V)); \
	} while (0)
#define xb_copy_opaque(E, XBSRC, XBDST) \
	do { \
	        uint32_t __count = 0, __val; \
	        xb_copy_32((E), (XBSRC), (XBDST), __count); \
	        if (E) break; \
	        __count = nfsm_rndup(__count); \
	        __count /= XDRWORD; \
	        while (__count-- > 0) \
	                xb_copy_32((E), (XBSRC), (XBDST), __val); \
	} while (0)

	xb_init_buffer(&xb, nmp->nm_args, 2 * XDRWORD);
	xb_get_32(error, &xb, val); /* version */
	xb_get_32(error, &xb, argslength); /* args length */
	xb_init_buffer(&xb, nmp->nm_args, argslength);

	xb_init_buffer(&xbnew, NULL, 0);
	xb_copy_32(error, &xb, &xbnew, val); /* version */
	argslength_offset = xb_offset(&xbnew);
	xb_copy_32(error, &xb, &xbnew, val); /* args length */
	xb_copy_32(error, &xb, &xbnew, val); /* XDR args version */
	count = NFS_MATTR_BITMAP_LEN;
	xb_get_bitmap(error, &xb, mattrs, count); /* mount attribute bitmap */
	nfsmerr_if(error);
	for (i = 0; i < NFS_MATTR_BITMAP_LEN; i++) {
		newmattrs[i] = mattrs[i];
	}
	if (referral) {
		NFS_BITMAP_SET(newmattrs, NFS_MATTR_FS_LOCATIONS);
		NFS_BITMAP_CLR(newmattrs, NFS_MATTR_MNTFROM);
	} else {
		NFS_BITMAP_SET(newmattrs, NFS_MATTR_FH);
	}
	NFS_BITMAP_SET(newmattrs, NFS_MATTR_FLAGS);
	NFS_BITMAP_SET(newmattrs, NFS_MATTR_MNTFLAGS);
	NFS_BITMAP_SET(newmattrs, NFS_MATTR_SET_MOUNT_OWNER);
	xb_add_bitmap(error, &xbnew, newmattrs, NFS_MATTR_BITMAP_LEN);
	attrslength_offset = xb_offset(&xbnew);
	xb_copy_32(error, &xb, &xbnew, val); /* attrs length */
	NFS_BITMAP_ZERO(newmflags_mask, NFS_MFLAG_BITMAP_LEN);
	NFS_BITMAP_ZERO(newmflags, NFS_MFLAG_BITMAP_LEN);
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_FLAGS)) {
		count = NFS_MFLAG_BITMAP_LEN;
		xb_get_bitmap(error, &xb, newmflags_mask, count); /* mount flag mask bitmap */
		count = NFS_MFLAG_BITMAP_LEN;
		xb_get_bitmap(error, &xb, newmflags, count); /* mount flag bitmap */
	}
	NFS_BITMAP_SET(newmflags_mask, NFS_MFLAG_EPHEMERAL);
	NFS_BITMAP_SET(newmflags, NFS_MFLAG_EPHEMERAL);
	xb_add_bitmap(error, &xbnew, newmflags_mask, NFS_MFLAG_BITMAP_LEN);
	xb_add_bitmap(error, &xbnew, newmflags, NFS_MFLAG_BITMAP_LEN);
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_VERSION)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_MINOR_VERSION)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_VERSION_RANGE)) {
		xb_copy_32(error, &xb, &xbnew, val);
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READ_SIZE)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_WRITE_SIZE)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READDIR_SIZE)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_READAHEAD)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_REG_MIN)) {
		xb_copy_32(error, &xb, &xbnew, val);
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_REG_MAX)) {
		xb_copy_32(error, &xb, &xbnew, val);
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MIN)) {
		xb_copy_32(error, &xb, &xbnew, val);
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MAX)) {
		xb_copy_32(error, &xb, &xbnew, val);
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCK_MODE)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SECURITY)) {
		xb_copy_32(error, &xb, &xbnew, count);
		while (!error && (count-- > 0)) {
			xb_copy_32(error, &xb, &xbnew, val);
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_KERB_ETYPE)) {
		xb_copy_32(error, &xb, &xbnew, count);
		xb_add_32(error, &xbnew, -1);
		while (!error && (count-- > 0)) {
			xb_copy_32(error, &xb, &xbnew, val);
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MAX_GROUP_LIST)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SOCKET_TYPE)) {
		xb_copy_opaque(error, &xb, &xbnew);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_NFS_PORT)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MOUNT_PORT)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_REQUEST_TIMEOUT)) {
		xb_copy_32(error, &xb, &xbnew, val);
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SOFT_RETRY_COUNT)) {
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_DEAD_TIMEOUT)) {
		xb_copy_32(error, &xb, &xbnew, val);
		xb_copy_32(error, &xb, &xbnew, val);
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_FH)) {
		xb_get_32(error, &xb, count);
		xb_skip(error, &xb, count);
	}
	if (!referral) {
		/* set the initial file handle to the directory's file handle */
		xb_add_fh(error, &xbnew, np->n_fhp, np->n_fhsize);
	}
	/* copy/extend/skip fs locations */
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_FS_LOCATIONS)) {
		numlocs = numserv = numaddr = numcomp = 0;
		if (referral) { /* don't copy the fs locations for a referral */
			skipcopy = 1;
		}
		xb_copy_32(error, &xb, &xbnew, numlocs); /* location count */
		for (loc = 0; !error && (loc < numlocs); loc++) {
			xb_copy_32(error, &xb, &xbnew, numserv); /* server count */
			for (serv = 0; !error && (serv < numserv); serv++) {
				xb_copy_opaque(error, &xb, &xbnew); /* server name */
				xb_copy_32(error, &xb, &xbnew, numaddr); /* address count */
				for (addr = 0; !error && (addr < numaddr); addr++) {
					xb_copy_opaque(error, &xb, &xbnew); /* address */
				}
				xb_copy_opaque(error, &xb, &xbnew); /* server info */
			}
			/* pathname */
			xb_get_32(error, &xb, numcomp); /* component count */
			if (!skipcopy) {
				uint64_t totalcomps = numcomp + relpathcomps;

				/* set error to ERANGE in the event of overflow */
				if (totalcomps > UINT32_MAX) {
					nfsmerr_if((error = ERANGE));
				}

				xb_add_32(error, &xbnew, (uint32_t) totalcomps); /* new component count */
			}
			for (comp = 0; !error && (comp < numcomp); comp++) {
				xb_copy_opaque(error, &xb, &xbnew); /* component */
			}
			/* add additional components */
			p = relpath;
			while (*p && (*p == '/')) {
				p++;
			}
			while (*p && !error) {
				cp = p;
				while (*p && (*p != '/')) {
					p++;
				}
				xb_add_string(error, &xbnew, cp, (p - cp)); /* component */
				while (*p && (*p == '/')) {
					p++;
				}
			}
			xb_copy_opaque(error, &xb, &xbnew); /* fs location info */
		}
		if (referral) {
			skipcopy = 0;
		}
	}
	if (referral) {
		/* add referral's fs locations */
		xb_add_32(error, &xbnew, nfsls.nl_numlocs);                     /* FS_LOCATIONS */
		for (loc = 0; !error && (loc < nfsls.nl_numlocs); loc++) {
			xb_add_32(error, &xbnew, nfsls.nl_locations[loc]->nl_servcount);
			for (serv = 0; !error && (serv < nfsls.nl_locations[loc]->nl_servcount); serv++) {
				xb_add_string(error, &xbnew, nfsls.nl_locations[loc]->nl_servers[serv]->ns_name,
				    strlen(nfsls.nl_locations[loc]->nl_servers[serv]->ns_name));
				xb_add_32(error, &xbnew, nfsls.nl_locations[loc]->nl_servers[serv]->ns_addrcount);
				for (addr = 0; !error && (addr < nfsls.nl_locations[loc]->nl_servers[serv]->ns_addrcount); addr++) {
					xb_add_string(error, &xbnew, nfsls.nl_locations[loc]->nl_servers[serv]->ns_addresses[addr],
					    strlen(nfsls.nl_locations[loc]->nl_servers[serv]->ns_addresses[addr]));
				}
				xb_add_32(error, &xbnew, 0); /* empty server info */
			}
			xb_add_32(error, &xbnew, nfsls.nl_locations[loc]->nl_path.np_compcount);
			for (comp = 0; !error && (comp < nfsls.nl_locations[loc]->nl_path.np_compcount); comp++) {
				xb_add_string(error, &xbnew, nfsls.nl_locations[loc]->nl_path.np_components[comp],
				    strlen(nfsls.nl_locations[loc]->nl_path.np_components[comp]));
			}
			xb_add_32(error, &xbnew, 0); /* empty fs location info */
		}
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MNTFLAGS)) {
		xb_get_32(error, &xb, mntflags);
	}
	/*
	 * We add the following mount flags to the ones for the mounted-on mount:
	 * MNT_DONTBROWSE - to keep the mount from showing up as a separate volume
	 * MNT_AUTOMOUNTED - to keep DiskArb from retriggering the mount after
	 *                   an unmount (looking for /.autodiskmounted)
	 */
	mntflags |= (MNT_AUTOMOUNTED | MNT_DONTBROWSE);
	xb_add_32(error, &xbnew, mntflags);
	if (!referral && NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MNTFROM)) {
		/* copy mntfrom string and add relpath */
		rlen = strlen(relpath);
		xb_get_32(error, &xb, mlen);
		nfsmerr_if(error);
		mlen2 = mlen + ((relpath[0] != '/') ? 1 : 0) + rlen;
		xb_add_32(error, &xbnew, mlen2);
		count = mlen / XDRWORD;
		/* copy the original string */
		while (count-- > 0) {
			xb_copy_32(error, &xb, &xbnew, val);
		}
		if (!error && (mlen % XDRWORD)) {
			error = xb_get_bytes(&xb, buf, mlen % XDRWORD, 0);
			if (!error) {
				error = xb_add_bytes(&xbnew, buf, mlen % XDRWORD, 1);
			}
		}
		/* insert a '/' if the relative path doesn't start with one */
		if (!error && (relpath[0] != '/')) {
			buf[0] = '/';
			error = xb_add_bytes(&xbnew, buf, 1, 1);
		}
		/* add the additional relative path */
		if (!error) {
			error = xb_add_bytes(&xbnew, relpath, rlen, 1);
		}
		/* make sure the resulting string has the right number of pad bytes */
		if (!error && (mlen2 != nfsm_rndup(mlen2))) {
			bzero(buf, sizeof(buf));
			count = nfsm_rndup(mlen2) - mlen2;
			error = xb_add_bytes(&xbnew, buf, count, 1);
		}
	}
	/*
	 * The following string copies rely on the fact that we already validated
	 * these data when creating the initial mount point.
	 */
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_REALM)) {
		xb_add_string(error, &xbnew, nmp->nm_realm, strlen(nmp->nm_realm));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_PRINCIPAL)) {
		xb_add_string(error, &xbnew, nmp->nm_principal, strlen(nmp->nm_principal));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SVCPRINCIPAL)) {
		xb_add_string(error, &xbnew, nmp->nm_sprinc, strlen(nmp->nm_sprinc));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCAL_NFS_PORT)) {
		xb_add_string(error, &xbnew, nmp->nm_nfs_localport, strlen(nmp->nm_nfs_localport));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCAL_MOUNT_PORT)) {
		xb_add_string(error, &xbnew, nmp->nm_mount_localport, strlen(nmp->nm_mount_localport));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SET_MOUNT_OWNER)) {
		/* drop embedded owner value */
		xb_get_32(error, &xb, count);
	}
	/* New mount always gets same owner as this mount */
	xb_add_32(error, &xbnew, vnode_mount(vp)->mnt_vfsstat.f_owner);
	xb_build_done(error, &xbnew);

	/* update opaque counts */
	end_offset = xb_offset(&xbnew);
	if (!error) {
		error = xb_seek(&xbnew, argslength_offset);
		argslength = end_offset - argslength_offset + XDRWORD /*version*/;
		xb_add_32(error, &xbnew, argslength);
	}
	if (!error) {
		error = xb_seek(&xbnew, attrslength_offset);
		xb_add_32(error, &xbnew, end_offset - attrslength_offset - XDRWORD /*don't include length field*/);
	}
	nfsmerr_if(error);

	/*
	 * For kernel_mount() call, use the existing mount flags (instead of the
	 * original flags) because flags like MNT_NOSUID and MNT_NODEV may have
	 * been silently enforced. Also, in terms of MACF, the _kernel_ is
	 * performing the mount (and enforcing all of the mount options), so we
	 * use the kernel context for the mount call.
	 */
	mntflags = vnode_vfsvisflags(vp);
	mntflags |= (MNT_AUTOMOUNTED | MNT_DONTBROWSE);
	ctx = vfs_context_kernel();

	/* do the mount */
	error = kernel_mount(fstype, dvp, vp, path, xb_buffer_base(&xbnew), argslength,
	    mntflags, KERNEL_MOUNT_PERMIT_UNMOUNT | KERNEL_MOUNT_NOAUTH, ctx);

nfsmerr:
	if (error) {
		printf("nfs: mirror mount of %s on %s failed (%d)\n",
		    mntfromname, path, error);
	}
	/* clean up */
	xb_cleanup(&xbnew);
	if (referral) {
		nfs_fs_locations_cleanup(&nfsls);
	}
	if (path) {
		NFS_ZFREE(ZV_NAMEI, path);
	}
	if (mntfromname) {
		NFS_ZFREE(ZV_NAMEI, mntfromname);
	}
	if (!error) {
		nfs_ephemeral_mount_harvester_start();
	}
	return error;
}

/*
 * trigger vnode functions
 */
#define NFS_TRIGGER_DEBUG 1

resolver_result_t
nfs_mirror_mount_trigger_resolve(
	vnode_t vp,
	const struct componentname *cnp,
	enum path_operation pop,
	__unused int flags,
	__unused void *data,
	vfs_context_t ctx)
{
	nfsnode_t         np = VTONFS(vp);
	vnode_t           pvp = NULLVP;
	int               error = 0;
	int               didBusy = 0;
	resolver_result_t result;

	/*
	 * We have a trigger node that doesn't have anything mounted on it yet.
	 * We'll do the mount if either:
	 * (a) this isn't the last component of the path OR
	 * (b) this is an op that looks like it should trigger the mount.
	 */
	if (cnp->cn_flags & ISLASTCN) {
		switch (pop) {
		case OP_MOUNT:
		case OP_UNMOUNT:
		case OP_STATFS:
		case OP_LINK:
		case OP_UNLINK:
		case OP_RENAME:
		case OP_MKNOD:
		case OP_MKFIFO:
		case OP_SYMLINK:
		case OP_ACCESS:
		case OP_GETATTR:
		case OP_MKDIR:
		case OP_RMDIR:
		case OP_REVOKE:
		case OP_GETXATTR:
		case OP_LISTXATTR:
			/* don't perform the mount for these operations */
			result = vfs_resolver_result(np->n_trigseq, RESOLVER_NOCHANGE, 0);
#ifdef NFS_TRIGGER_DEBUG
			NP(np, "nfs trigger RESOLVE: no change, last %d nameiop %d, seq %d",
			    (cnp->cn_flags & ISLASTCN) ? 1 : 0, cnp->cn_nameiop, np->n_trigseq);
#endif
			return result;
		case OP_OPEN:
		case OP_CHDIR:
		case OP_CHROOT:
		case OP_TRUNCATE:
		case OP_COPYFILE:
		case OP_PATHCONF:
		case OP_READLINK:
		case OP_SETATTR:
		case OP_EXCHANGEDATA:
		case OP_SEARCHFS:
		case OP_FSCTL:
		case OP_SETXATTR:
		case OP_REMOVEXATTR:
		default:
			/* go ahead and do the mount */
			break;
		}
	}

	if (vnode_mountedhere(vp) != NULL) {
		/*
		 * Um... there's already something mounted.
		 * Been there.  Done that.  Let's just say it succeeded.
		 */
		error = 0;
		goto skipmount;
	}

	if ((error = nfs_node_set_busy(np, vfs_context_thread(ctx)))) {
		result = vfs_resolver_result(np->n_trigseq, RESOLVER_ERROR, error);
#ifdef NFS_TRIGGER_DEBUG
		NP(np, "nfs trigger RESOLVE: busy error %d, last %d nameiop %d, seq %d",
		    error, (cnp->cn_flags & ISLASTCN) ? 1 : 0, cnp->cn_nameiop, np->n_trigseq);
#endif
		return result;
	}
	didBusy = 1;

	/* Check again, in case the mount happened while we were setting busy */
	if (vnode_mountedhere(vp) != NULL) {
		/* Been there.  Done that.  Let's just say it succeeded.  */
		error = 0;
		goto skipmount;
	}
	nfs_node_lock_force(np);
	if (np->n_flag & NDISARMTRIGGER) {
		error = ECANCELED;
		nfs_node_unlock(np);
		goto skipmount;
	}
	nfs_node_unlock(np);

	pvp = vnode_getparent(vp);
	if (pvp == NULLVP) {
		error = EINVAL;
	}
	if (!error) {
		error = nfs_mirror_mount_domount(pvp, vp, ctx);
	}
skipmount:
	if (!error) {
		np->n_trigseq++;
	}
	result = vfs_resolver_result(np->n_trigseq, error ? RESOLVER_ERROR : RESOLVER_RESOLVED, error);
#ifdef NFS_TRIGGER_DEBUG
	NP(np, "nfs trigger RESOLVE: %s %d, last %d nameiop %d, seq %d",
	    error ? "error" : "resolved", error,
	    (cnp->cn_flags & ISLASTCN) ? 1 : 0, cnp->cn_nameiop, np->n_trigseq);
#endif

	if (pvp != NULLVP) {
		vnode_put(pvp);
	}
	if (didBusy) {
		nfs_node_clear_busy(np);
	}
	return result;
}

resolver_result_t
nfs_mirror_mount_trigger_unresolve(
	vnode_t vp,
	int flags,
	__unused void *data,
	vfs_context_t ctx)
{
	nfsnode_t np = VTONFS(vp);
	mount_t mp;
	int error;
	resolver_result_t result;

	if ((error = nfs_node_set_busy(np, vfs_context_thread(ctx)))) {
		result = vfs_resolver_result(np->n_trigseq, RESOLVER_ERROR, error);
#ifdef NFS_TRIGGER_DEBUG
		NP(np, "nfs trigger UNRESOLVE: busy error %d, seq %d", error, np->n_trigseq);
#endif
		return result;
	}

	mp = vnode_mountedhere(vp);
	if (!mp) {
		error = EINVAL;
	}
	if (!error) {
		error = vfs_unmountbyfsid(&(vfs_statfs(mp)->f_fsid), flags, ctx);
	}
	if (!error) {
		np->n_trigseq++;
	}
	result = vfs_resolver_result(np->n_trigseq, error ? RESOLVER_ERROR : RESOLVER_UNRESOLVED, error);
#ifdef NFS_TRIGGER_DEBUG
	NP(np, "nfs trigger UNRESOLVE: %s %d, seq %d",
	    error ? "error" : "unresolved", error, np->n_trigseq);
#endif
	nfs_node_clear_busy(np);
	return result;
}

resolver_result_t
nfs_mirror_mount_trigger_rearm(
	vnode_t vp,
	__unused int flags,
	__unused void *data,
	vfs_context_t ctx)
{
	nfsnode_t np = VTONFS(vp);
	int error;
	resolver_result_t result;

	if ((error = nfs_node_set_busy(np, vfs_context_thread(ctx)))) {
		result = vfs_resolver_result(np->n_trigseq, RESOLVER_ERROR, error);
#ifdef NFS_TRIGGER_DEBUG
		NP(np, "nfs trigger REARM: busy error %d, seq %d", error, np->n_trigseq);
#endif
		return result;
	}

	np->n_trigseq++;
	result = vfs_resolver_result(np->n_trigseq,
	    vnode_mountedhere(vp) ? RESOLVER_RESOLVED : RESOLVER_UNRESOLVED, 0);
#ifdef NFS_TRIGGER_DEBUG
	NP(np, "nfs trigger REARM: %s, seq %d",
	    vnode_mountedhere(vp) ? "resolved" : "unresolved", np->n_trigseq);
#endif
	nfs_node_clear_busy(np);
	return result;
}

/*
 * Periodically attempt to unmount ephemeral (mirror) mounts in an attempt to limit
 * the number of unused mounts.
 */

#define NFS_EPHEMERAL_MOUNT_HARVEST_INTERVAL    120     /* how often the harvester runs */
struct nfs_ephemeral_mount_harvester_info {
	fsid_t          fsid;           /* FSID that we need to try to unmount */
	uint32_t        mountcount;     /* count of ephemeral mounts seen in scan */
};
/* various globals for the harvester */
static thread_call_t nfs_ephemeral_mount_harvester_timer = NULL;
static int nfs_ephemeral_mount_harvester_on = 0;

kern_return_t thread_terminate(thread_t);

static int
nfs_ephemeral_mount_harvester_callback(mount_t mp, void *arg)
{
	struct nfs_ephemeral_mount_harvester_info *hinfo = arg;
	struct nfsmount *nmp;
	struct timeval now;

	if (strcmp(mp->mnt_vfsstat.f_fstypename, "nfs")) {
		return VFS_RETURNED;
	}
	nmp = VFSTONFS(mp);
	if (!nmp || !NMFLAG(nmp, EPHEMERAL)
	    ) {
		return VFS_RETURNED;
	}
	hinfo->mountcount++;

	/* avoid unmounting mounts that have been triggered within the last harvest interval */
	microtime(&now);
	if ((nmp->nm_mounttime >> 32) > ((uint32_t)now.tv_sec - NFS_EPHEMERAL_MOUNT_HARVEST_INTERVAL)) {
		return VFS_RETURNED;
	}

	if (hinfo->fsid.val[0] || hinfo->fsid.val[1]) {
		/* attempt to unmount previously-found ephemeral mount */
		vfs_unmountbyfsid(&hinfo->fsid, 0, vfs_context_kernel());
		hinfo->fsid.val[0] = hinfo->fsid.val[1] = 0;
	}

	/*
	 * We can't call unmount here since we hold a mount iter ref
	 * on mp so save its fsid for the next call iteration to unmount.
	 */
	hinfo->fsid.val[0] = mp->mnt_vfsstat.f_fsid.val[0];
	hinfo->fsid.val[1] = mp->mnt_vfsstat.f_fsid.val[1];

	return VFS_RETURNED;
}

/*
 * Spawn a thread to do the ephemeral mount harvesting.
 */
static void
nfs_ephemeral_mount_harvester_timer_func(void)
{
	thread_t thd;

	if (kernel_thread_start(nfs_ephemeral_mount_harvester, NULL, &thd) == KERN_SUCCESS) {
		thread_deallocate(thd);
	}
}

/*
 * Iterate all mounts looking for NFS ephemeral mounts to try to unmount.
 */
void
nfs_ephemeral_mount_harvester(__unused void *arg, __unused wait_result_t wr)
{
	struct nfs_ephemeral_mount_harvester_info hinfo;
	uint64_t deadline;

	hinfo.mountcount = 0;
	hinfo.fsid.val[0] = hinfo.fsid.val[1] = 0;
	vfs_iterate(VFS_ITERATE_TAIL_FIRST, nfs_ephemeral_mount_harvester_callback, &hinfo);
	if (hinfo.fsid.val[0] || hinfo.fsid.val[1]) {
		/* attempt to unmount last found ephemeral mount */
		vfs_unmountbyfsid(&hinfo.fsid, 0, vfs_context_kernel());
	}

	lck_mtx_lock(&nfs_global_mutex);
	if (!hinfo.mountcount) {
		/* no more ephemeral mounts - don't need timer */
		nfs_ephemeral_mount_harvester_on = 0;
	} else {
		/* re-arm the timer */
		clock_interval_to_deadline(NFS_EPHEMERAL_MOUNT_HARVEST_INTERVAL, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(nfs_ephemeral_mount_harvester_timer, deadline);
		nfs_ephemeral_mount_harvester_on = 1;
	}
	lck_mtx_unlock(&nfs_global_mutex);

	/* thread done */
	thread_terminate(current_thread());
}

/*
 * Make sure the NFS ephemeral mount harvester timer is running.
 */
void
nfs_ephemeral_mount_harvester_start(void)
{
	uint64_t deadline;

	lck_mtx_lock(&nfs_global_mutex);
	if (nfs_ephemeral_mount_harvester_on) {
		lck_mtx_unlock(&nfs_global_mutex);
		return;
	}
	if (nfs_ephemeral_mount_harvester_timer == NULL) {
		nfs_ephemeral_mount_harvester_timer = thread_call_allocate((thread_call_func_t)nfs_ephemeral_mount_harvester_timer_func, NULL);
	}
	clock_interval_to_deadline(NFS_EPHEMERAL_MOUNT_HARVEST_INTERVAL, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(nfs_ephemeral_mount_harvester_timer, deadline);
	nfs_ephemeral_mount_harvester_on = 1;
	lck_mtx_unlock(&nfs_global_mutex);
}

#endif

/*
 *  Send a STAT protocol request to the server to verify statd is running.
 *  rpc-statd service, which responsible to provide locks for the NFS server, is disabled by default on Ubuntu.
 *  Please see Radar 45969553 for more info.
 */
int
nfs3_check_lockmode(struct nfsmount *nmp, struct sockaddr *sa, int sotype, int timeo)
{
	struct sockaddr_storage ss;
	int error, port = 0;

	if (nmp->nm_lockmode == NFS_LOCK_MODE_ENABLED) {
		if (sa->sa_len > sizeof(ss)) {
			return EINVAL;
		}
		bcopy(sa, &ss, MIN(sa->sa_len, sizeof(ss)));
		error = nfs_portmap_lookup(nmp, vfs_context_current(), (struct sockaddr*)&ss, NULL, RPCPROG_STAT, RPCMNT_VER1, NM_OMFLAG(nmp, MNTUDP) ? SOCK_DGRAM : sotype, timeo);
		if (!error) {
			if (ss.ss_family == AF_INET) {
				port = ntohs(((struct sockaddr_in*)&ss)->sin_port);
			} else if (ss.ss_family == AF_INET6) {
				port = ntohs(((struct sockaddr_in6*)&ss)->sin6_port);
			} else if (ss.ss_family == AF_LOCAL) {
				port = (((struct sockaddr_un*)&ss)->sun_path[0] != '\0');
			}

			if (!port) {
				printf("nfs: STAT(NSM) rpc service is not available, unable to mount with current lock mode.\n");
				return EPROGUNAVAIL;
			}
		}
	}
	return 0;
}

/*
 * Send a MOUNT protocol MOUNT request to the server to get the initial file handle (and security).
 */
int
nfs3_mount_rpc(struct nfsmount *nmp, struct sockaddr *sa, int sotype, int nfsvers, char *path, vfs_context_t ctx, int timeo, fhandle_t *fh, struct nfs_sec *sec)
{
	int error = 0, mntproto;
	thread_t thd = vfs_context_thread(ctx);
	kauth_cred_t cred = vfs_context_ucred(ctx);
	uint64_t xid = 0;
	size_t slen;
	struct nfsm_chain nmreq, nmrep;
	mbuf_t mreq;
	uint32_t mntvers, mntport, val;
	struct sockaddr_storage ss;
	struct sockaddr *saddr = (struct sockaddr*)&ss;
	struct sockaddr_un *sun = (struct sockaddr_un*)saddr;

	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);

	mntvers = (nfsvers == NFS_VER2) ? RPCMNT_VER1 : RPCMNT_VER3;
	mntproto = (NM_OMFLAG(nmp, MNTUDP) || (sotype == SOCK_DGRAM)) ? IPPROTO_UDP : IPPROTO_TCP;
	sec->count = 0;

	bcopy(sa, saddr, min(sizeof(ss), sa->sa_len));
	if (saddr->sa_family == AF_INET) {
		if (nmp->nm_mountport) {
			((struct sockaddr_in*)saddr)->sin_port = htons(nmp->nm_mountport);
		}
		mntport = ntohs(((struct sockaddr_in*)saddr)->sin_port);
	} else if (saddr->sa_family == AF_INET6) {
		if (nmp->nm_mountport) {
			((struct sockaddr_in6*)saddr)->sin6_port = htons(nmp->nm_mountport);
		}
		mntport = ntohs(((struct sockaddr_in6*)saddr)->sin6_port);
	} else {  /* Local domain socket */
		mntport = ((struct sockaddr_un *)saddr)->sun_path[0]; /* Do we have and address? */
		mntproto = IPPROTO_TCP;  /* XXX rpcbind only listens on streams sockets for now */
	}

	while (!mntport) {
		error = nfs_portmap_lookup(nmp, ctx, saddr, NULL, RPCPROG_MNT, mntvers,
		    mntproto == IPPROTO_UDP ? SOCK_DGRAM : SOCK_STREAM, timeo);
		nfsmout_if(error);
		if (saddr->sa_family == AF_INET) {
			mntport = ntohs(((struct sockaddr_in*)saddr)->sin_port);
		} else if (saddr->sa_family == AF_INET6) {
			mntport = ntohs(((struct sockaddr_in6*)saddr)->sin6_port);
		} else if (saddr->sa_family == AF_LOCAL) {
			mntport = ((struct sockaddr_un*)saddr)->sun_path[0];
		}
		if (!mntport) {
			/* if not found and TCP, then retry with UDP */
			if (mntproto == IPPROTO_UDP) {
				error = EPROGUNAVAIL;
				break;
			}
			mntproto = IPPROTO_UDP;
			bcopy(sa, saddr, min(sizeof(ss), sa->sa_len));
			if (saddr->sa_family == AF_LOCAL) {
				strlcpy(sun->sun_path, RPCB_TICLTS_PATH, sizeof(sun->sun_path));
			}
		}
	}
	nfsmout_if(error || !mntport);

	/* MOUNT protocol MOUNT request */
	slen = strlen(path);
	nfsm_chain_build_alloc_init(error, &nmreq, NFSX_UNSIGNED + nfsm_rndup(slen));
	nfsm_chain_add_name(error, &nmreq, path, slen, nmp);
	nfsm_chain_build_done(error, &nmreq);
	nfsmout_if(error);
	error = nfsm_rpchead2(nmp, (mntproto == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM,
	    RPCPROG_MNT, mntvers, RPCMNT_MOUNT,
	    RPCAUTH_SYS, cred, NULL, nmreq.nmc_mhead, &xid, &mreq);
	nfsmout_if(error);
	nmreq.nmc_mhead = NULL;
	error = nfs_aux_request(nmp, thd, saddr, NULL,
	    ((mntproto == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM),
	    mreq, R_XID32(xid), 1, timeo, &nmrep);
	nfsmout_if(error);
	nfsm_chain_get_32(error, &nmrep, val);
	if (!error && val) {
		error = val;
	}
	nfsmout_if(error);
	nfsm_chain_get_fh(error, &nmrep, nfsvers, fh);
	if (!error && (nfsvers > NFS_VER2)) {
		sec->count = NX_MAX_SEC_FLAVORS;
		error = nfsm_chain_get_secinfo(&nmrep, &sec->flavors[0], &sec->count);
	}
nfsmout:
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
	return error;
}


/*
 * Send a MOUNT protocol UNMOUNT request to tell the server we've unmounted it.
 */
void
nfs3_umount_rpc(struct nfsmount *nmp, vfs_context_t ctx, int timeo)
{
	int error = 0, mntproto;
	thread_t thd = vfs_context_thread(ctx);
	kauth_cred_t cred = vfs_context_ucred(ctx);
	char *path;
	uint64_t xid = 0;
	size_t slen;
	struct nfsm_chain nmreq, nmrep;
	mbuf_t mreq;
	uint32_t mntvers;
	in_port_t mntport;
	struct sockaddr_storage ss;
	struct sockaddr *saddr = (struct sockaddr*)&ss;

	if (!nmp->nm_saddr) {
		return;
	}

	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);

	mntvers = (nmp->nm_vers == NFS_VER2) ? RPCMNT_VER1 : RPCMNT_VER3;
	mntproto = (NM_OMFLAG(nmp, MNTUDP) || (nmp->nm_sotype == SOCK_DGRAM)) ? IPPROTO_UDP : IPPROTO_TCP;
	mntport = nmp->nm_mountport;

	bcopy(nmp->nm_saddr, saddr, min(sizeof(ss), nmp->nm_saddr->sa_len));
	if (saddr->sa_family == AF_INET) {
		((struct sockaddr_in*)saddr)->sin_port = htons(mntport);
	} else if (saddr->sa_family == AF_INET6) {
		((struct sockaddr_in6*)saddr)->sin6_port = htons(mntport);
	} else { /* Local domain socket */
		mntport = ((struct sockaddr_un *)saddr)->sun_path[0]; /* Do we have and address? */
	}

	while (!mntport) {
		error = nfs_portmap_lookup(nmp, ctx, saddr, NULL, RPCPROG_MNT, mntvers, mntproto, timeo);
		nfsmout_if(error);
		if (saddr->sa_family == AF_INET) {
			mntport = ntohs(((struct sockaddr_in*)saddr)->sin_port);
		} else if (saddr->sa_family == AF_INET6) {
			mntport = ntohs(((struct sockaddr_in6*)saddr)->sin6_port);
		} else { /* Local domain socket */
			mntport = ((struct sockaddr_un *)saddr)->sun_path[0]; /* Do we have and address? */
		}
		/* if not found and mntvers > VER1, then retry with VER1 */
		if (!mntport) {
			if (mntvers > RPCMNT_VER1) {
				mntvers = RPCMNT_VER1;
			} else if (mntproto == IPPROTO_TCP) {
				mntproto = IPPROTO_UDP;
				mntvers = (nmp->nm_vers == NFS_VER2) ? RPCMNT_VER1 : RPCMNT_VER3;
			} else {
				break;
			}
			bcopy(nmp->nm_saddr, saddr, min(sizeof(ss), nmp->nm_saddr->sa_len));
		}
	}
	nfsmout_if(!mntport);

	/* MOUNT protocol UNMOUNT request */
	path = &vfs_statfs(nmp->nm_mountp)->f_mntfromname[0];
	while (*path && (*path != '/')) {
		path++;
	}
	slen = strlen(path);
	nfsm_chain_build_alloc_init(error, &nmreq, NFSX_UNSIGNED + nfsm_rndup(slen));
	nfsm_chain_add_name(error, &nmreq, path, slen, nmp);
	nfsm_chain_build_done(error, &nmreq);
	nfsmout_if(error);
	error = nfsm_rpchead2(nmp, (mntproto == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM,
	    RPCPROG_MNT, RPCMNT_VER1, RPCMNT_UMOUNT,
	    RPCAUTH_SYS, cred, NULL, nmreq.nmc_mhead, &xid, &mreq);
	nfsmout_if(error);
	nmreq.nmc_mhead = NULL;
	error = nfs_aux_request(nmp, thd, saddr, NULL,
	    ((mntproto == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM),
	    mreq, R_XID32(xid), 1, timeo, &nmrep);
nfsmout:
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
}

/*
 * unmount system call
 */
int
nfs_vfs_unmount(
	mount_t mp,
	int mntflags,
	__unused vfs_context_t ctx)
{
	struct nfsmount *nmp;
	vnode_t vp;
	int error, flags = 0;
	struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

	nmp = VFSTONFS(mp);
	lck_mtx_lock(&nmp->nm_lock);
	/*
	 * Set the flag indicating that an unmount attempt is in progress.
	 */
	nmp->nm_state |= NFSSTA_UNMOUNTING;
	/*
	 * During a force unmount we want to...
	 *   Mark that we are doing a force unmount.
	 *   Make the mountpoint soft.
	 */
	if (mntflags & MNT_FORCE) {
		flags |= FORCECLOSE;
		nmp->nm_state |= NFSSTA_FORCE;
		NFS_BITMAP_SET(nmp->nm_flags, NFS_MFLAG_SOFT);
	}
	/*
	 * Wait for any in-progress monitored node scan to complete.
	 */
	while (nmp->nm_state & NFSSTA_MONITOR_SCAN) {
		msleep(&nmp->nm_state, &nmp->nm_lock, PZERO - 1, "nfswaitmonscan", &ts);
	}
	/*
	 * Goes something like this..
	 * - Call vflush() to clear out vnodes for this file system,
	 *   except for the swap files. Deal with them in 2nd pass.
	 * - Decrement reference on the vnode representing remote root.
	 * - Clean up the NFS mount structure.
	 */
	vp = NFSTOV(nmp->nm_dnp);
	lck_mtx_unlock(&nmp->nm_lock);

	/*
	 * vflush will check for busy vnodes on mountpoint.
	 * Will do the right thing for MNT_FORCE. That is, we should
	 * not get EBUSY back.
	 */
	error = vflush(mp, vp, SKIPSWAP | flags);
	if (mntflags & MNT_FORCE) {
		error = vflush(mp, NULLVP, flags); /* locks vp in the process */
	} else {
		if (vnode_isinuse(vp, 1)) {
			error = EBUSY;
		} else {
			error = vflush(mp, vp, flags);
		}
	}
	if (error) {
		lck_mtx_lock(&nmp->nm_lock);
		nmp->nm_state &= ~NFSSTA_UNMOUNTING;
		lck_mtx_unlock(&nmp->nm_lock);
		return error;
	}

	lck_mtx_lock(&nmp->nm_lock);
	nmp->nm_dnp = NULL;
	lck_mtx_unlock(&nmp->nm_lock);

	/*
	 * Release the root vnode reference held by mountnfs()
	 */
	error = vnode_get(vp);
	vnode_rele(vp);
	if (!error) {
		vnode_put(vp);
	}

	vflush(mp, NULLVP, FORCECLOSE);

	/* Wait for all other references to be released and free the mount */
	nfs_mount_drain_and_cleanup(nmp);

	return 0;
}

/*
 * cleanup/destroy NFS fs locations structure
 */
void
nfs_fs_locations_cleanup(struct nfs_fs_locations *nfslsp)
{
	struct nfs_fs_location *fsl;
	struct nfs_fs_server *fss;
	struct nfs_fs_path *fsp;
	uint32_t loc, serv, addr, comp;

	/* free up fs locations */
	if (!nfslsp->nl_numlocs || !nfslsp->nl_locations) {
		return;
	}

	for (loc = 0; loc < nfslsp->nl_numlocs; loc++) {
		fsl = nfslsp->nl_locations[loc];
		if (!fsl) {
			continue;
		}
		if ((fsl->nl_servcount > 0) && fsl->nl_servers) {
			for (serv = 0; serv < fsl->nl_servcount; serv++) {
				fss = fsl->nl_servers[serv];
				if (!fss) {
					continue;
				}
				if ((fss->ns_addrcount > 0) && fss->ns_addresses) {
					for (addr = 0; addr < fss->ns_addrcount; addr++) {
						FREE(fss->ns_addresses[addr], M_TEMP);
					}
					FREE(fss->ns_addresses, M_TEMP);
				}
				FREE(fss->ns_name, M_TEMP);
				FREE(fss, M_TEMP);
			}
			FREE(fsl->nl_servers, M_TEMP);
		}
		fsp = &fsl->nl_path;
		if (fsp->np_compcount && fsp->np_components) {
			for (comp = 0; comp < fsp->np_compcount; comp++) {
				if (fsp->np_components[comp]) {
					FREE(fsp->np_components[comp], M_TEMP);
				}
			}
			FREE(fsp->np_components, M_TEMP);
		}
		FREE(fsl, M_TEMP);
	}
	FREE(nfslsp->nl_locations, M_TEMP);
	nfslsp->nl_numlocs = 0;
	nfslsp->nl_locations = NULL;
}

void
nfs_mount_rele(struct nfsmount *nmp)
{
	int wup = 0;

	lck_mtx_lock(&nmp->nm_lock);
	if (nmp->nm_ref < 1) {
		panic("nfs zombie mount underflow\n");
	}
	nmp->nm_ref--;
	if (nmp->nm_ref == 0) {
		wup = nmp->nm_state & NFSSTA_MOUNT_DRAIN;
	}
	lck_mtx_unlock(&nmp->nm_lock);
	if (wup) {
		wakeup(&nmp->nm_ref);
	}
}

void
nfs_mount_drain_and_cleanup(struct nfsmount *nmp)
{
	lck_mtx_lock(&nmp->nm_lock);
	nmp->nm_state |= NFSSTA_MOUNT_DRAIN;
	while (nmp->nm_ref > 0) {
		msleep(&nmp->nm_ref, &nmp->nm_lock, PZERO - 1, "nfs_mount_drain", NULL);
	}
	assert(nmp->nm_ref == 0);
	lck_mtx_unlock(&nmp->nm_lock);
	nfs_mount_cleanup(nmp);
}

/*
 * nfs_mount_zombie
 */
void
nfs_mount_zombie(struct nfsmount *nmp, int nm_state_flags)
{
	struct nfsreq *req, *treq;
	struct nfs_reqqhead iodq, resendq;
	struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
	struct nfs_open_owner *noop, *nextnoop;
	nfsnode_t np;
	int docallback;

	lck_mtx_lock(&nmp->nm_lock);
	nmp->nm_state |= nm_state_flags;
	nmp->nm_ref++;
	lck_mtx_unlock(&nmp->nm_lock);
#if CONFIG_NFS4
	/* stop callbacks */
	if ((nmp->nm_vers >= NFS_VER4) && !NMFLAG(nmp, NOCALLBACK) && nmp->nm_cbid) {
		nfs4_mount_callback_shutdown(nmp);
	}
#endif
#if CONFIG_NFS_GSS
	/* Destroy any RPCSEC_GSS contexts */
	nfs_gss_clnt_ctx_unmount(nmp);
#endif

	/* mark the socket for termination */
	lck_mtx_lock(&nmp->nm_lock);
	nmp->nm_sockflags |= NMSOCK_UNMOUNT;

	/* Have the socket thread send the unmount RPC, if requested/appropriate. */
	if ((nmp->nm_vers < NFS_VER4) && (nmp->nm_state & NFSSTA_MOUNTED) &&
	    !(nmp->nm_state & (NFSSTA_FORCE | NFSSTA_DEAD)) && NMFLAG(nmp, CALLUMNT)) {
		nfs_mount_sock_thread_wake(nmp);
	}

	/* wait for the socket thread to terminate */
	while (nmp->nm_sockthd && current_thread() != nmp->nm_sockthd) {
		wakeup(&nmp->nm_sockthd);
		msleep(&nmp->nm_sockthd, &nmp->nm_lock, PZERO - 1, "nfswaitsockthd", &ts);
	}
	lck_mtx_unlock(&nmp->nm_lock);

	/* tear down the socket */
	nfs_disconnect(nmp);

	lck_mtx_lock(&nmp->nm_lock);

#if CONFIG_NFS4
	if ((nmp->nm_vers >= NFS_VER4) && !NMFLAG(nmp, NOCALLBACK) && nmp->nm_cbid) {
		/* clear out any pending delegation return requests */
		while ((np = TAILQ_FIRST(&nmp->nm_dreturnq))) {
			TAILQ_REMOVE(&nmp->nm_dreturnq, np, n_dreturn);
			np->n_dreturn.tqe_next = NFSNOLIST;
		}
	}

	/* cancel any renew timer */
	if ((nmp->nm_vers >= NFS_VER4) && nmp->nm_renew_timer) {
		thread_call_cancel(nmp->nm_renew_timer);
		thread_call_free(nmp->nm_renew_timer);
		nmp->nm_renew_timer = NULL;
	}

#endif
	lck_mtx_unlock(&nmp->nm_lock);

	if (nmp->nm_state & NFSSTA_MOUNTED) {
		switch (nmp->nm_lockmode) {
		case NFS_LOCK_MODE_DISABLED:
		case NFS_LOCK_MODE_LOCAL:
			break;
		case NFS_LOCK_MODE_ENABLED:
		default:
			if (nmp->nm_vers <= NFS_VER3) {
				nfs_lockd_mount_unregister(nmp);
				nmp->nm_lockmode = NFS_LOCK_MODE_DISABLED;
			}
			break;
		}
	}

#if CONFIG_NFS4
	if ((nmp->nm_vers >= NFS_VER4) && nmp->nm_longid) {
		/* remove/deallocate the client ID data */
		lck_mtx_lock(&nfs_global_mutex);
		TAILQ_REMOVE(&nfsclientids, nmp->nm_longid, nci_link);
		if (nmp->nm_longid->nci_id) {
			FREE(nmp->nm_longid->nci_id, M_TEMP);
		}
		FREE(nmp->nm_longid, M_TEMP);
		nmp->nm_longid = NULL;
		lck_mtx_unlock(&nfs_global_mutex);
	}
#endif
	/*
	 * Be sure all requests for this mount are completed
	 * and removed from the resend queue.
	 */
	TAILQ_INIT(&resendq);
	lck_mtx_lock(&nfs_request_mutex);
	TAILQ_FOREACH(req, &nfs_reqq, r_chain) {
		if (req->r_nmp == nmp) {
			lck_mtx_lock(&req->r_mtx);
			if (!req->r_error && req->r_nmrep.nmc_mhead == NULL) {
				req->r_error = EIO;
			}
			if (req->r_flags & R_RESENDQ) {
				lck_mtx_lock(&nmp->nm_lock);
				if ((req->r_flags & R_RESENDQ) && req->r_rchain.tqe_next != NFSREQNOLIST) {
					TAILQ_REMOVE(&nmp->nm_resendq, req, r_rchain);
					req->r_flags &= ~R_RESENDQ;
					req->r_rchain.tqe_next = NFSREQNOLIST;
					/*
					 * Queue up the request so that we can unreference them
					 * with out holding nfs_request_mutex
					 */
					TAILQ_INSERT_TAIL(&resendq, req, r_rchain);
				}
				lck_mtx_unlock(&nmp->nm_lock);
			}
			wakeup(req);
			lck_mtx_unlock(&req->r_mtx);
		}
	}
	lck_mtx_unlock(&nfs_request_mutex);

	/* Since we've drop the request mutex we can now safely unreference the request */
	TAILQ_FOREACH_SAFE(req, &resendq, r_rchain, treq) {
		TAILQ_REMOVE(&resendq, req, r_rchain);
		/* Make sure we don't try and remove again in nfs_request_destroy */
		req->r_rchain.tqe_next = NFSREQNOLIST;
		nfs_request_rele(req);
	}

	/*
	 * Now handle and outstanding async requests. We need to walk the
	 * request queue again this time with the nfsiod_mutex held. No
	 * other iods can grab our requests until we've put them on our own
	 * local iod queue for processing.
	 */
	TAILQ_INIT(&iodq);
	lck_mtx_lock(&nfs_request_mutex);
	lck_mtx_lock(&nfsiod_mutex);
	TAILQ_FOREACH(req, &nfs_reqq, r_chain) {
		if (req->r_nmp == nmp) {
			lck_mtx_lock(&req->r_mtx);
			if (req->r_callback.rcb_func
			    && !(req->r_flags & R_WAITSENT) && !(req->r_flags & R_IOD)) {
				/*
				 * Since R_IOD is not set then we need to handle it. If
				 * we're not on a list add it to our iod queue. Otherwise
				 * we must already be on nm_iodq which is added to our
				 * local queue below.
				 * %%% We should really keep a back pointer to our iod queue
				 * that we're on.
				 */
				req->r_flags |= R_IOD;
				if (req->r_achain.tqe_next == NFSREQNOLIST) {
					TAILQ_INSERT_TAIL(&iodq, req, r_achain);
				}
			}
			lck_mtx_unlock(&req->r_mtx);
		}
	}

	/* finish any async I/O RPCs queued up */
	if (nmp->nm_iodlink.tqe_next != NFSNOLIST) {
		TAILQ_REMOVE(&nfsiodmounts, nmp, nm_iodlink);
	}
	TAILQ_CONCAT(&iodq, &nmp->nm_iodq, r_achain);
	lck_mtx_unlock(&nfsiod_mutex);
	lck_mtx_unlock(&nfs_request_mutex);

	TAILQ_FOREACH_SAFE(req, &iodq, r_achain, treq) {
		TAILQ_REMOVE(&iodq, req, r_achain);
		req->r_achain.tqe_next = NFSREQNOLIST;
		lck_mtx_lock(&req->r_mtx);
		docallback = !(req->r_flags & R_WAITSENT);
		lck_mtx_unlock(&req->r_mtx);
		if (docallback) {
			req->r_callback.rcb_func(req);
		}
	}

	/* clean up common state */
	lck_mtx_lock(&nmp->nm_lock);
	while ((np = LIST_FIRST(&nmp->nm_monlist))) {
		LIST_REMOVE(np, n_monlink);
		np->n_monlink.le_next = NFSNOLIST;
	}
	TAILQ_FOREACH_SAFE(noop, &nmp->nm_open_owners, noo_link, nextnoop) {
		os_ref_count_t newcount;

		TAILQ_REMOVE(&nmp->nm_open_owners, noop, noo_link);
		noop->noo_flags &= ~NFS_OPEN_OWNER_LINK;
		newcount = os_ref_release_locked(&noop->noo_refcnt);

		if (newcount) {
			continue;
		}
		nfs_open_owner_destroy(noop);
	}
	lck_mtx_unlock(&nmp->nm_lock);

#if CONFIG_NFS4
	/* clean up NFSv4 state */
	if (nmp->nm_vers >= NFS_VER4) {
		lck_mtx_lock(&nmp->nm_lock);
		while ((np = TAILQ_FIRST(&nmp->nm_delegations))) {
			TAILQ_REMOVE(&nmp->nm_delegations, np, n_dlink);
			np->n_dlink.tqe_next = NFSNOLIST;
		}
		lck_mtx_unlock(&nmp->nm_lock);
	}
#endif
	nfs_mount_rele(nmp);
}

/*
 * cleanup/destroy an nfsmount
 */
void
nfs_mount_cleanup(struct nfsmount *nmp)
{
	if (!nmp) {
		return;
	}

	nfs_mount_zombie(nmp, 0);

	NFS_VFS_DBG("Unmounting %s from %s\n",
	    vfs_statfs(nmp->nm_mountp)->f_mntfromname,
	    vfs_statfs(nmp->nm_mountp)->f_mntonname);
	NFS_VFS_DBG("nfs state = 0x%8.8x\n", nmp->nm_state);
	NFS_VFS_DBG("nfs socket flags = 0x%8.8x\n", nmp->nm_sockflags);
	NFS_VFS_DBG("nfs mount ref count is %d\n", nmp->nm_ref);
	NFS_VFS_DBG("mount ref count is %d\n", nmp->nm_mountp->mnt_count);

	if (nmp->nm_mountp) {
		vfs_setfsprivate(nmp->nm_mountp, NULL);
	}

	lck_mtx_lock(&nmp->nm_lock);
	if (nmp->nm_ref) {
		panic("Some one has grabbed a ref %d state flags = 0x%8.8x\n", nmp->nm_ref, nmp->nm_state);
	}

	if (nmp->nm_saddr) {
		FREE(nmp->nm_saddr, M_SONAME);
	}
	if ((nmp->nm_vers < NFS_VER4) && nmp->nm_rqsaddr) {
		FREE(nmp->nm_rqsaddr, M_SONAME);
	}

	if (IS_VALID_CRED(nmp->nm_mcred)) {
		kauth_cred_unref(&nmp->nm_mcred);
	}

	nfs_fs_locations_cleanup(&nmp->nm_locations);

	if (nmp->nm_realm) {
		FREE(nmp->nm_realm, M_TEMP);
	}
	if (nmp->nm_principal) {
		FREE(nmp->nm_principal, M_TEMP);
	}
	if (nmp->nm_sprinc) {
		FREE(nmp->nm_sprinc, M_TEMP);
	}

	if (nmp->nm_args) {
		xb_free(nmp->nm_args);
	}

	lck_mtx_unlock(&nmp->nm_lock);

	lck_mtx_destroy(&nmp->nm_lock, &nfs_mount_grp);
	if (nmp->nm_fh) {
		NFS_ZFREE(nfs_fhandle_zone, nmp->nm_fh);
	}


	NFS_ZFREE(nfsmnt_zone, nmp);
}

/*
 * Return root of a filesystem
 */
int
nfs_vfs_root(mount_t mp, vnode_t *vpp, __unused vfs_context_t ctx)
{
	vnode_t vp;
	struct nfsmount *nmp;
	int error;
	u_int32_t vpid;

	nmp = VFSTONFS(mp);
	if (!nmp || !nmp->nm_dnp) {
		return ENXIO;
	}
	vp = NFSTOV(nmp->nm_dnp);
	vpid = vnode_vid(vp);
	while ((error = vnode_getwithvid(vp, vpid))) {
		/* vnode_get() may return ENOENT if the dir changes. */
		/* If that happens, just try it again, else return the error. */
		if ((error != ENOENT) || (vnode_vid(vp) == vpid)) {
			return error;
		}
		vpid = vnode_vid(vp);
	}
	*vpp = vp;
	return 0;
}

/*
 * Do operations associated with quotas
 */
#if !QUOTA
int
nfs_vfs_quotactl(
	__unused mount_t mp,
	__unused int cmds,
	__unused uid_t uid,
	__unused caddr_t datap,
	__unused vfs_context_t context)
{
	return ENOTSUP;
}
#else

static in_port_t
nfs_sa_getport(struct sockaddr *sa, int *error)
{
	in_port_t port = 0;

	if (sa->sa_family == AF_INET6) {
		port = ntohs(((struct sockaddr_in6*)sa)->sin6_port);
	} else if (sa->sa_family == AF_INET) {
		port = ntohs(((struct sockaddr_in*)sa)->sin_port);
	} else if (error) {
		*error = EIO;
	}

	return port;
}

static void
nfs_sa_setport(struct sockaddr *sa, in_port_t port)
{
	if (sa->sa_family == AF_INET6) {
		((struct sockaddr_in6*)sa)->sin6_port = htons(port);
	} else if (sa->sa_family == AF_INET) {
		((struct sockaddr_in*)sa)->sin_port = htons(port);
	}
}

int
nfs3_getquota(struct nfsmount *nmp, vfs_context_t ctx, uid_t id, int type, struct dqblk *dqb)
{
	int error = 0, timeo;
	int rqproto, rqvers = (type == GRPQUOTA) ? RPCRQUOTA_EXT_VER : RPCRQUOTA_VER;
	in_port_t rqport = 0;
	thread_t thd = vfs_context_thread(ctx);
	kauth_cred_t cred = vfs_context_ucred(ctx);
	char *path;
	uint64_t slen, xid = 0;
	struct nfsm_chain nmreq, nmrep;
	mbuf_t mreq;
	uint32_t val = 0, bsize = 0;
	struct sockaddr *rqsaddr;
	struct timeval now;
	struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };

	if (!nmp->nm_saddr) {
		return ENXIO;
	}

	if (NMFLAG(nmp, NOQUOTA) || nmp->nm_saddr->sa_family == AF_LOCAL /* XXX for now */) {
		return ENOTSUP;
	}

	/*
	 * Allocate an address for rquotad if needed
	 */
	if (!nmp->nm_rqsaddr) {
		int need_free = 0;

		MALLOC(rqsaddr, struct sockaddr *, sizeof(struct sockaddr_storage), M_SONAME, M_WAITOK | M_ZERO);
		bcopy(nmp->nm_saddr, rqsaddr, min(sizeof(struct sockaddr_storage), nmp->nm_saddr->sa_len));
		/* Set the port to zero, will call rpcbind to get the port below */
		nfs_sa_setport(rqsaddr, 0);
		microuptime(&now);

		lck_mtx_lock(&nmp->nm_lock);
		if (!nmp->nm_rqsaddr) {
			nmp->nm_rqsaddr = rqsaddr;
			nmp->nm_rqsaddrstamp = now.tv_sec;
		} else {
			need_free = 1;
		}
		lck_mtx_unlock(&nmp->nm_lock);
		if (need_free) {
			FREE(rqsaddr, M_SONAME);
		}
	}

	timeo = NMFLAG(nmp, SOFT) ? 10 : 60;
	rqproto = IPPROTO_UDP; /* XXX should prefer TCP if mount is TCP */

	/* check if we have a recently cached rquota port */
	microuptime(&now);
	lck_mtx_lock(&nmp->nm_lock);
	rqsaddr = nmp->nm_rqsaddr;
	rqport = nfs_sa_getport(rqsaddr, &error);
	while (!error && (!rqport || ((nmp->nm_rqsaddrstamp + 60) <= (uint32_t)now.tv_sec))) {
		error = nfs_sigintr(nmp, NULL, thd, 1);
		if (error) {
			lck_mtx_unlock(&nmp->nm_lock);
			return error;
		}
		if (nmp->nm_state & NFSSTA_RQUOTAINPROG) {
			nmp->nm_state |= NFSSTA_WANTRQUOTA;
			msleep(&nmp->nm_rqsaddr, &nmp->nm_lock, PZERO - 1, "nfswaitrquotaaddr", &ts);
			rqport = nfs_sa_getport(rqsaddr, &error);
			continue;
		}
		nmp->nm_state |= NFSSTA_RQUOTAINPROG;
		lck_mtx_unlock(&nmp->nm_lock);

		/* send portmap request to get rquota port */
		error = nfs_portmap_lookup(nmp, ctx, rqsaddr, NULL, RPCPROG_RQUOTA, rqvers, rqproto, timeo);
		if (error) {
			goto out;
		}
		rqport = nfs_sa_getport(rqsaddr, &error);
		if (error) {
			goto out;
		}

		if (!rqport) {
			/*
			 * We overload PMAPPORT for the port if rquotad is not
			 * currently registered or up at the server.  In the
			 * while loop above, port will be set and we will defer
			 * for a bit.  Perhaps the service isn't online yet.
			 *
			 * Note that precludes using indirect, but we're not doing
			 * that here.
			 */
			rqport = PMAPPORT;
			nfs_sa_setport(rqsaddr, rqport);
		}
		microuptime(&now);
		nmp->nm_rqsaddrstamp = now.tv_sec;
out:
		lck_mtx_lock(&nmp->nm_lock);
		nmp->nm_state &= ~NFSSTA_RQUOTAINPROG;
		if (nmp->nm_state & NFSSTA_WANTRQUOTA) {
			nmp->nm_state &= ~NFSSTA_WANTRQUOTA;
			wakeup(&nmp->nm_rqsaddr);
		}
	}
	lck_mtx_unlock(&nmp->nm_lock);
	if (error) {
		return error;
	}

	/* Using PMAPPORT for unavailabe rquota service */
	if (rqport == PMAPPORT) {
		return ENOTSUP;
	}

	/* rquota request */
	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);
	path = &vfs_statfs(nmp->nm_mountp)->f_mntfromname[0];
	while (*path && (*path != '/')) {
		path++;
	}
	slen = strlen(path);
	nfsm_chain_build_alloc_init(error, &nmreq, 3 * NFSX_UNSIGNED + nfsm_rndup(slen));
	nfsm_chain_add_name(error, &nmreq, path, slen, nmp);
	if (type == GRPQUOTA) {
		nfsm_chain_add_32(error, &nmreq, type);
	}
	nfsm_chain_add_32(error, &nmreq, id);
	nfsm_chain_build_done(error, &nmreq);
	nfsmout_if(error);
	error = nfsm_rpchead2(nmp, (rqproto == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM,
	    RPCPROG_RQUOTA, rqvers, RPCRQUOTA_GET,
	    RPCAUTH_SYS, cred, NULL, nmreq.nmc_mhead, &xid, &mreq);
	nfsmout_if(error);
	nmreq.nmc_mhead = NULL;
	error = nfs_aux_request(nmp, thd, rqsaddr, NULL,
	    (rqproto == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM,
	    mreq, R_XID32(xid), 0, timeo, &nmrep);
	nfsmout_if(error);

	/* parse rquota response */
	nfsm_chain_get_32(error, &nmrep, val);
	if (!error && (val != RQUOTA_STAT_OK)) {
		if (val == RQUOTA_STAT_NOQUOTA) {
			error = ENOENT;
		} else if (val == RQUOTA_STAT_EPERM) {
			error = EPERM;
		} else {
			error = EIO;
		}
	}
	nfsm_chain_get_32(error, &nmrep, bsize);
	nfsm_chain_adv(error, &nmrep, NFSX_UNSIGNED);
	nfsm_chain_get_32(error, &nmrep, val);
	nfsmout_if(error);
	dqb->dqb_bhardlimit = (uint64_t)val * bsize;
	nfsm_chain_get_32(error, &nmrep, val);
	nfsmout_if(error);
	dqb->dqb_bsoftlimit = (uint64_t)val * bsize;
	nfsm_chain_get_32(error, &nmrep, val);
	nfsmout_if(error);
	dqb->dqb_curbytes = (uint64_t)val * bsize;
	nfsm_chain_get_32(error, &nmrep, dqb->dqb_ihardlimit);
	nfsm_chain_get_32(error, &nmrep, dqb->dqb_isoftlimit);
	nfsm_chain_get_32(error, &nmrep, dqb->dqb_curinodes);
	nfsm_chain_get_32(error, &nmrep, dqb->dqb_btime);
	nfsm_chain_get_32(error, &nmrep, dqb->dqb_itime);
	nfsmout_if(error);
	dqb->dqb_id = id;
nfsmout:
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
	return error;
}
#if CONFIG_NFS4
int
nfs4_getquota(struct nfsmount *nmp, vfs_context_t ctx, uid_t id, int type, struct dqblk *dqb)
{
	nfsnode_t np;
	int error = 0, status, nfsvers, numops;
	u_int64_t xid;
	struct nfsm_chain nmreq, nmrep;
	uint32_t bitmap[NFS_ATTR_BITMAP_LEN];
	thread_t thd = vfs_context_thread(ctx);
	kauth_cred_t cred = vfs_context_ucred(ctx);
	struct nfsreq_secinfo_args si;

	if (type != USRQUOTA) { /* NFSv4 only supports user quotas */
		return ENOTSUP;
	}

	/* first check that the server supports any of the quota attributes */
	if (!NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_supp_attr, NFS_FATTR_QUOTA_AVAIL_HARD) &&
	    !NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_supp_attr, NFS_FATTR_QUOTA_AVAIL_SOFT) &&
	    !NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_supp_attr, NFS_FATTR_QUOTA_USED)) {
		return ENOTSUP;
	}

	/*
	 * The credential passed to the server needs to have
	 * an effective uid that matches the given uid.
	 */
	if (id != kauth_cred_getuid(cred)) {
		struct posix_cred temp_pcred;
		posix_cred_t pcred = posix_cred_get(cred);
		bzero(&temp_pcred, sizeof(temp_pcred));
		temp_pcred.cr_uid = id;
		temp_pcred.cr_ngroups = pcred->cr_ngroups;
		bcopy(pcred->cr_groups, temp_pcred.cr_groups, sizeof(temp_pcred.cr_groups));
		cred = posix_cred_create(&temp_pcred);
		if (!IS_VALID_CRED(cred)) {
			return ENOMEM;
		}
	} else {
		kauth_cred_ref(cred);
	}

	nfsvers = nmp->nm_vers;
	np = nmp->nm_dnp;
	if (!np) {
		error = ENXIO;
	}
	if (error || ((error = vnode_get(NFSTOV(np))))) {
		kauth_cred_unref(&cred);
		return error;
	}

	NFSREQ_SECINFO_SET(&si, np, NULL, 0, NULL, 0);
	nfsm_chain_null(&nmreq);
	nfsm_chain_null(&nmrep);

	// PUTFH + GETATTR
	numops = 2;
	nfsm_chain_build_alloc_init(error, &nmreq, 15 * NFSX_UNSIGNED);
	nfsm_chain_add_compound_header(error, &nmreq, "quota", nmp->nm_minor_vers, numops);
	numops--;
	nfsm_chain_add_32(error, &nmreq, NFS_OP_PUTFH);
	nfsm_chain_add_fh(error, &nmreq, nfsvers, np->n_fhp, np->n_fhsize);
	numops--;
	nfsm_chain_add_32(error, &nmreq, NFS_OP_GETATTR);
	NFS_CLEAR_ATTRIBUTES(bitmap);
	NFS_BITMAP_SET(bitmap, NFS_FATTR_QUOTA_AVAIL_HARD);
	NFS_BITMAP_SET(bitmap, NFS_FATTR_QUOTA_AVAIL_SOFT);
	NFS_BITMAP_SET(bitmap, NFS_FATTR_QUOTA_USED);
	nfsm_chain_add_bitmap_supported(error, &nmreq, bitmap, nmp, NULL);
	nfsm_chain_build_done(error, &nmreq);
	nfsm_assert(error, (numops == 0), EPROTO);
	nfsmout_if(error);
	error = nfs_request2(np, NULL, &nmreq, NFSPROC4_COMPOUND, thd, cred, &si, 0, &nmrep, &xid, &status);
	nfsm_chain_skip_tag(error, &nmrep);
	nfsm_chain_get_32(error, &nmrep, numops);
	nfsm_chain_op_check(error, &nmrep, NFS_OP_PUTFH);
	nfsm_chain_op_check(error, &nmrep, NFS_OP_GETATTR);
	nfsm_assert(error, NFSTONMP(np), ENXIO);
	nfsmout_if(error);
	error = nfs4_parsefattr(&nmrep, NULL, NULL, NULL, dqb, NULL);
	nfsmout_if(error);
	nfsm_assert(error, NFSTONMP(np), ENXIO);
nfsmout:
	nfsm_chain_cleanup(&nmreq);
	nfsm_chain_cleanup(&nmrep);
	vnode_put(NFSTOV(np));
	kauth_cred_unref(&cred);
	return error;
}
#endif /* CONFIG_NFS4 */
int
nfs_vfs_quotactl(mount_t mp, int cmds, uid_t uid, caddr_t datap, vfs_context_t ctx)
{
	struct nfsmount *nmp;
	int cmd, type, error, nfsvers;
	uid_t euid = kauth_cred_getuid(vfs_context_ucred(ctx));
	struct dqblk *dqb = (struct dqblk*)datap;

	nmp = VFSTONFS(mp);
	if (nfs_mount_gone(nmp)) {
		return ENXIO;
	}
	nfsvers = nmp->nm_vers;

	if (uid == ~0U) {
		uid = euid;
	}

	/* we can only support Q_GETQUOTA */
	cmd = cmds >> SUBCMDSHIFT;
	switch (cmd) {
	case Q_GETQUOTA:
		break;
	case Q_QUOTAON:
	case Q_QUOTAOFF:
	case Q_SETQUOTA:
	case Q_SETUSE:
	case Q_SYNC:
	case Q_QUOTASTAT:
		return ENOTSUP;
	default:
		return EINVAL;
	}

	type = cmds & SUBCMDMASK;
	if ((u_int)type >= MAXQUOTAS) {
		return EINVAL;
	}
	if ((uid != euid) && ((error = vfs_context_suser(ctx)))) {
		return error;
	}

	if (vfs_busy(mp, LK_NOWAIT)) {
		return 0;
	}
	bzero(dqb, sizeof(*dqb));
	error = nmp->nm_funcs->nf_getquota(nmp, ctx, uid, type, dqb);
	vfs_unbusy(mp);
	return error;
}
#endif

/*
 * Flush out the buffer cache
 */
int nfs_sync_callout(vnode_t, void *);

struct nfs_sync_cargs {
	vfs_context_t   ctx;
	int             waitfor;
	int             error;
};

int
nfs_sync_callout(vnode_t vp, void *arg)
{
	struct nfs_sync_cargs *cargs = (struct nfs_sync_cargs*)arg;
	nfsnode_t np = VTONFS(vp);
	int error;

	if (np->n_flag & NREVOKE) {
		vn_revoke(vp, REVOKEALL, cargs->ctx);
		return VNODE_RETURNED;
	}

	if (LIST_EMPTY(&np->n_dirtyblkhd)) {
		return VNODE_RETURNED;
	}
	if (np->n_wrbusy > 0) {
		return VNODE_RETURNED;
	}
	if (np->n_bflag & (NBFLUSHINPROG | NBINVALINPROG)) {
		return VNODE_RETURNED;
	}

	error = nfs_flush(np, cargs->waitfor, vfs_context_thread(cargs->ctx), 0);
	if (error) {
		cargs->error = error;
	}

	return VNODE_RETURNED;
}

int
nfs_vfs_sync(mount_t mp, int waitfor, vfs_context_t ctx)
{
	struct nfs_sync_cargs cargs;

	cargs.waitfor = waitfor;
	cargs.ctx = ctx;
	cargs.error = 0;

	vnode_iterate(mp, 0, nfs_sync_callout, &cargs);

	return cargs.error;
}

/*
 * NFS flat namespace lookup.
 * Currently unsupported.
 */
/*ARGSUSED*/
int
nfs_vfs_vget(
	__unused mount_t mp,
	__unused ino64_t ino,
	__unused vnode_t *vpp,
	__unused vfs_context_t ctx)
{
	return ENOTSUP;
}

/*
 * At this point, this should never happen
 */
/*ARGSUSED*/
int
nfs_vfs_fhtovp(
	__unused mount_t mp,
	__unused int fhlen,
	__unused unsigned char *fhp,
	__unused vnode_t *vpp,
	__unused vfs_context_t ctx)
{
	return ENOTSUP;
}

/*
 * Vnode pointer to File handle, should never happen either
 */
/*ARGSUSED*/
int
nfs_vfs_vptofh(
	__unused vnode_t vp,
	__unused int *fhlenp,
	__unused unsigned char *fhp,
	__unused vfs_context_t ctx)
{
	return ENOTSUP;
}

/*
 * Vfs start routine, a no-op.
 */
/*ARGSUSED*/
int
nfs_vfs_start(
	__unused mount_t mp,
	__unused int flags,
	__unused vfs_context_t ctx)
{
	return 0;
}

/*
 * Build the mount info buffer for NFS_MOUNTINFO.
 */
int
nfs_mountinfo_assemble(struct nfsmount *nmp, struct xdrbuf *xb)
{
	struct xdrbuf xbinfo, xborig;
	char sotype[16];
	uint32_t origargsvers, origargslength;
	size_t infolength_offset, curargsopaquelength_offset, curargslength_offset, attrslength_offset, curargs_end_offset, end_offset;
	uint32_t miattrs[NFS_MIATTR_BITMAP_LEN];
	uint32_t miflags_mask[NFS_MIFLAG_BITMAP_LEN];
	uint32_t miflags[NFS_MIFLAG_BITMAP_LEN];
	uint32_t mattrs[NFS_MATTR_BITMAP_LEN];
	uint32_t mflags_mask[NFS_MFLAG_BITMAP_LEN];
	uint32_t mflags[NFS_MFLAG_BITMAP_LEN];
	uint32_t loc, serv, addr, comp;
	int i, timeo, error = 0;

	/* set up mount info attr and flag bitmaps */
	NFS_BITMAP_ZERO(miattrs, NFS_MIATTR_BITMAP_LEN);
	NFS_BITMAP_SET(miattrs, NFS_MIATTR_FLAGS);
	NFS_BITMAP_SET(miattrs, NFS_MIATTR_ORIG_ARGS);
	NFS_BITMAP_SET(miattrs, NFS_MIATTR_CUR_ARGS);
	NFS_BITMAP_SET(miattrs, NFS_MIATTR_CUR_LOC_INDEX);
	NFS_BITMAP_ZERO(miflags_mask, NFS_MIFLAG_BITMAP_LEN);
	NFS_BITMAP_ZERO(miflags, NFS_MIFLAG_BITMAP_LEN);
	NFS_BITMAP_SET(miflags_mask, NFS_MIFLAG_DEAD);
	NFS_BITMAP_SET(miflags_mask, NFS_MIFLAG_NOTRESP);
	NFS_BITMAP_SET(miflags_mask, NFS_MIFLAG_RECOVERY);
	if (nmp->nm_state & NFSSTA_DEAD) {
		NFS_BITMAP_SET(miflags, NFS_MIFLAG_DEAD);
	}
	if ((nmp->nm_state & (NFSSTA_TIMEO | NFSSTA_JUKEBOXTIMEO)) ||
	    ((nmp->nm_state & NFSSTA_LOCKTIMEO) && (nmp->nm_lockmode == NFS_LOCK_MODE_ENABLED))) {
		NFS_BITMAP_SET(miflags, NFS_MIFLAG_NOTRESP);
	}
	if (nmp->nm_state & NFSSTA_RECOVER) {
		NFS_BITMAP_SET(miflags, NFS_MIFLAG_RECOVERY);
	}

	/* get original mount args length */
	xb_init_buffer(&xborig, nmp->nm_args, 2 * XDRWORD);
	xb_get_32(error, &xborig, origargsvers); /* version */
	xb_get_32(error, &xborig, origargslength); /* args length */
	nfsmerr_if(error);

	/* set up current mount attributes bitmap */
	NFS_BITMAP_ZERO(mattrs, NFS_MATTR_BITMAP_LEN);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FLAGS);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_VERSION);
#if CONFIG_NFS4
	if (nmp->nm_vers >= NFS_VER4) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_MINOR_VERSION);
	}
#endif
	NFS_BITMAP_SET(mattrs, NFS_MATTR_READ_SIZE);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_WRITE_SIZE);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_READDIR_SIZE);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_READAHEAD);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_ATTRCACHE_REG_MIN);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_ATTRCACHE_REG_MAX);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MIN);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_ATTRCACHE_DIR_MAX);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_LOCK_MODE);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_SECURITY);
	if (nmp->nm_etype.selected < nmp->nm_etype.count) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_KERB_ETYPE);
	}
	NFS_BITMAP_SET(mattrs, NFS_MATTR_MAX_GROUP_LIST);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_SOCKET_TYPE);
	if (nmp->nm_saddr->sa_family != AF_LOCAL) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_NFS_PORT);
	}
	if ((nmp->nm_vers < NFS_VER4) && nmp->nm_mountport && !nmp->nm_mount_localport) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_MOUNT_PORT);
	}
	NFS_BITMAP_SET(mattrs, NFS_MATTR_REQUEST_TIMEOUT);
	if (NMFLAG(nmp, SOFT)) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_SOFT_RETRY_COUNT);
	}
	if (nmp->nm_deadtimeout) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_DEAD_TIMEOUT);
	}
	if (nmp->nm_fh) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_FH);
	}
	NFS_BITMAP_SET(mattrs, NFS_MATTR_FS_LOCATIONS);
	NFS_BITMAP_SET(mattrs, NFS_MATTR_MNTFLAGS);
	if (origargsvers < NFS_ARGSVERSION_XDR) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_MNTFROM);
	}
	if (nmp->nm_realm) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_REALM);
	}
	if (nmp->nm_principal) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_PRINCIPAL);
	}
	if (nmp->nm_sprinc) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_SVCPRINCIPAL);
	}
	if (nmp->nm_nfs_localport) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_LOCAL_NFS_PORT);
	}
	if ((nmp->nm_vers < NFS_VER4) && nmp->nm_mount_localport) {
		NFS_BITMAP_SET(mattrs, NFS_MATTR_LOCAL_MOUNT_PORT);
	}

	/* set up current mount flags bitmap */
	/* first set the flags that we will be setting - either on OR off */
	NFS_BITMAP_ZERO(mflags_mask, NFS_MFLAG_BITMAP_LEN);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_SOFT);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_INTR);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_RESVPORT);
	if (nmp->nm_sotype == SOCK_DGRAM) {
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NOCONNECT);
	}
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_DUMBTIMER);
	if (nmp->nm_vers < NFS_VER4) {
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_CALLUMNT);
	}
	if (nmp->nm_vers >= NFS_VER3) {
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_RDIRPLUS);
	}
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NONEGNAMECACHE);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_MUTEJUKEBOX);
#if CONFIG_NFS4
	if (nmp->nm_vers >= NFS_VER4) {
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_EPHEMERAL);
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NOCALLBACK);
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NAMEDATTR);
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NOACL);
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_ACLONLY);
	}
#endif
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NFC);
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_NOQUOTA);
	if (nmp->nm_vers < NFS_VER4) {
		NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_MNTUDP);
	}
	NFS_BITMAP_SET(mflags_mask, NFS_MFLAG_MNTQUICK);
	/* now set the flags that should be set */
	NFS_BITMAP_ZERO(mflags, NFS_MFLAG_BITMAP_LEN);
	if (NMFLAG(nmp, SOFT)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_SOFT);
	}
	if (NMFLAG(nmp, INTR)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_INTR);
	}
	if (NMFLAG(nmp, RESVPORT)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_RESVPORT);
	}
	if ((nmp->nm_sotype == SOCK_DGRAM) && NMFLAG(nmp, NOCONNECT)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_NOCONNECT);
	}
	if (NMFLAG(nmp, DUMBTIMER)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_DUMBTIMER);
	}
	if ((nmp->nm_vers < NFS_VER4) && NMFLAG(nmp, CALLUMNT)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_CALLUMNT);
	}
	if ((nmp->nm_vers >= NFS_VER3) && NMFLAG(nmp, RDIRPLUS)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_RDIRPLUS);
	}
	if (NMFLAG(nmp, NONEGNAMECACHE)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_NONEGNAMECACHE);
	}
	if (NMFLAG(nmp, MUTEJUKEBOX)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_MUTEJUKEBOX);
	}
#if CONFIG_NFS4
	if (nmp->nm_vers >= NFS_VER4) {
		if (NMFLAG(nmp, EPHEMERAL)) {
			NFS_BITMAP_SET(mflags, NFS_MFLAG_EPHEMERAL);
		}
		if (NMFLAG(nmp, NOCALLBACK)) {
			NFS_BITMAP_SET(mflags, NFS_MFLAG_NOCALLBACK);
		}
		if (NMFLAG(nmp, NAMEDATTR)) {
			NFS_BITMAP_SET(mflags, NFS_MFLAG_NAMEDATTR);
		}
		if (NMFLAG(nmp, NOACL)) {
			NFS_BITMAP_SET(mflags, NFS_MFLAG_NOACL);
		}
		if (NMFLAG(nmp, ACLONLY)) {
			NFS_BITMAP_SET(mflags, NFS_MFLAG_ACLONLY);
		}
	}
#endif
	if (NMFLAG(nmp, NFC)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_NFC);
	}
	if (NMFLAG(nmp, NOQUOTA) || ((nmp->nm_vers >= NFS_VER4) &&
	    !NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_supp_attr, NFS_FATTR_QUOTA_AVAIL_HARD) &&
	    !NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_supp_attr, NFS_FATTR_QUOTA_AVAIL_SOFT) &&
	    !NFS_BITMAP_ISSET(nmp->nm_fsattr.nfsa_supp_attr, NFS_FATTR_QUOTA_USED))) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_NOQUOTA);
	}
	if ((nmp->nm_vers < NFS_VER4) && NMFLAG(nmp, MNTUDP)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_MNTUDP);
	}
	if (NMFLAG(nmp, MNTQUICK)) {
		NFS_BITMAP_SET(mflags, NFS_MFLAG_MNTQUICK);
	}

	/* assemble info buffer: */
	xb_init_buffer(&xbinfo, NULL, 0);
	xb_add_32(error, &xbinfo, NFS_MOUNT_INFO_VERSION);
	infolength_offset = xb_offset(&xbinfo);
	xb_add_32(error, &xbinfo, 0);
	xb_add_bitmap(error, &xbinfo, miattrs, NFS_MIATTR_BITMAP_LEN);
	xb_add_bitmap(error, &xbinfo, miflags, NFS_MIFLAG_BITMAP_LEN);
	xb_add_32(error, &xbinfo, origargslength);
	if (!error) {
		error = xb_add_bytes(&xbinfo, nmp->nm_args, origargslength, 0);
	}

	/* the opaque byte count for the current mount args values: */
	curargsopaquelength_offset = xb_offset(&xbinfo);
	xb_add_32(error, &xbinfo, 0);

	/* Encode current mount args values */
	xb_add_32(error, &xbinfo, NFS_ARGSVERSION_XDR);
	curargslength_offset = xb_offset(&xbinfo);
	xb_add_32(error, &xbinfo, 0);
	xb_add_32(error, &xbinfo, NFS_XDRARGS_VERSION_0);
	xb_add_bitmap(error, &xbinfo, mattrs, NFS_MATTR_BITMAP_LEN);
	attrslength_offset = xb_offset(&xbinfo);
	xb_add_32(error, &xbinfo, 0);
	xb_add_bitmap(error, &xbinfo, mflags_mask, NFS_MFLAG_BITMAP_LEN);
	xb_add_bitmap(error, &xbinfo, mflags, NFS_MFLAG_BITMAP_LEN);
	xb_add_32(error, &xbinfo, nmp->nm_vers);                /* NFS_VERSION */
#if CONFIG_NFS4
	if (nmp->nm_vers >= NFS_VER4) {
		xb_add_32(error, &xbinfo, nmp->nm_minor_vers);  /* NFS_MINOR_VERSION */
	}
#endif
	xb_add_32(error, &xbinfo, nmp->nm_rsize);               /* READ_SIZE */
	xb_add_32(error, &xbinfo, nmp->nm_wsize);               /* WRITE_SIZE */
	xb_add_32(error, &xbinfo, nmp->nm_readdirsize);         /* READDIR_SIZE */
	xb_add_32(error, &xbinfo, nmp->nm_readahead);           /* READAHEAD */
	xb_add_32(error, &xbinfo, nmp->nm_acregmin);            /* ATTRCACHE_REG_MIN */
	xb_add_32(error, &xbinfo, 0);                           /* ATTRCACHE_REG_MIN */
	xb_add_32(error, &xbinfo, nmp->nm_acregmax);            /* ATTRCACHE_REG_MAX */
	xb_add_32(error, &xbinfo, 0);                           /* ATTRCACHE_REG_MAX */
	xb_add_32(error, &xbinfo, nmp->nm_acdirmin);            /* ATTRCACHE_DIR_MIN */
	xb_add_32(error, &xbinfo, 0);                           /* ATTRCACHE_DIR_MIN */
	xb_add_32(error, &xbinfo, nmp->nm_acdirmax);            /* ATTRCACHE_DIR_MAX */
	xb_add_32(error, &xbinfo, 0);                           /* ATTRCACHE_DIR_MAX */
	xb_add_32(error, &xbinfo, nmp->nm_lockmode);            /* LOCK_MODE */
	if (nmp->nm_sec.count) {
		xb_add_32(error, &xbinfo, nmp->nm_sec.count);           /* SECURITY */
		nfsmerr_if(error);
		for (i = 0; i < nmp->nm_sec.count; i++) {
			xb_add_32(error, &xbinfo, nmp->nm_sec.flavors[i]);
		}
	} else if (nmp->nm_servsec.count) {
		xb_add_32(error, &xbinfo, nmp->nm_servsec.count);       /* SECURITY */
		nfsmerr_if(error);
		for (i = 0; i < nmp->nm_servsec.count; i++) {
			xb_add_32(error, &xbinfo, nmp->nm_servsec.flavors[i]);
		}
	} else {
		xb_add_32(error, &xbinfo, 1);                           /* SECURITY */
		xb_add_32(error, &xbinfo, nmp->nm_auth);
	}
	if (nmp->nm_etype.selected < nmp->nm_etype.count) {
		xb_add_32(error, &xbinfo, nmp->nm_etype.count);
		xb_add_32(error, &xbinfo, nmp->nm_etype.selected);
		for (uint32_t j = 0; j < nmp->nm_etype.count; j++) {
			xb_add_32(error, &xbinfo, nmp->nm_etype.etypes[j]);
		}
		nfsmerr_if(error);
	}
	xb_add_32(error, &xbinfo, nmp->nm_numgrps);             /* MAX_GROUP_LIST */
	nfsmerr_if(error);

	switch (nmp->nm_saddr->sa_family) {
	case AF_INET:
	case AF_INET6:
		snprintf(sotype, sizeof(sotype), "%s%s", (nmp->nm_sotype == SOCK_DGRAM) ? "udp" : "tcp",
		    nmp->nm_sofamily ? (nmp->nm_sofamily == AF_INET) ? "4" : "6" : "");
		xb_add_string(error, &xbinfo, sotype, strlen(sotype));  /* SOCKET_TYPE */
		xb_add_32(error, &xbinfo, ntohs(((struct sockaddr_in*)nmp->nm_saddr)->sin_port)); /* NFS_PORT */
		if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_MOUNT_PORT)) {
			xb_add_32(error, &xbinfo, nmp->nm_mountport);   /* MOUNT_PORT */
		}
		break;
	case AF_LOCAL:
		strlcpy(sotype, (nmp->nm_sotype == SOCK_DGRAM) ? "ticlts" : "ticotsord", sizeof(sotype));
		xb_add_string(error, &xbinfo, sotype, strlen(sotype));
		break;
	default:
		NFS_VFS_DBG("Unsupported address family %d\n", nmp->nm_saddr->sa_family);
		printf("Unsupported address family %d\n", nmp->nm_saddr->sa_family);
		error = EINVAL;
		break;
	}

	timeo = (nmp->nm_timeo * 10) / NFS_HZ;
	xb_add_32(error, &xbinfo, timeo / 10);                    /* REQUEST_TIMEOUT */
	xb_add_32(error, &xbinfo, (timeo % 10) * 100000000);        /* REQUEST_TIMEOUT */
	if (NMFLAG(nmp, SOFT)) {
		xb_add_32(error, &xbinfo, nmp->nm_retry);       /* SOFT_RETRY_COUNT */
	}
	if (nmp->nm_deadtimeout) {
		xb_add_32(error, &xbinfo, nmp->nm_deadtimeout); /* DEAD_TIMEOUT */
		xb_add_32(error, &xbinfo, 0);                   /* DEAD_TIMEOUT */
	}
	if (nmp->nm_fh) {
		xb_add_fh(error, &xbinfo, &nmp->nm_fh->fh_data[0], nmp->nm_fh->fh_len); /* FH */
	}
	xb_add_32(error, &xbinfo, nmp->nm_locations.nl_numlocs);                        /* FS_LOCATIONS */
	for (loc = 0; !error && (loc < nmp->nm_locations.nl_numlocs); loc++) {
		xb_add_32(error, &xbinfo, nmp->nm_locations.nl_locations[loc]->nl_servcount);
		for (serv = 0; !error && (serv < nmp->nm_locations.nl_locations[loc]->nl_servcount); serv++) {
			xb_add_string(error, &xbinfo, nmp->nm_locations.nl_locations[loc]->nl_servers[serv]->ns_name,
			    strlen(nmp->nm_locations.nl_locations[loc]->nl_servers[serv]->ns_name));
			xb_add_32(error, &xbinfo, nmp->nm_locations.nl_locations[loc]->nl_servers[serv]->ns_addrcount);
			for (addr = 0; !error && (addr < nmp->nm_locations.nl_locations[loc]->nl_servers[serv]->ns_addrcount); addr++) {
				xb_add_string(error, &xbinfo, nmp->nm_locations.nl_locations[loc]->nl_servers[serv]->ns_addresses[addr],
				    strlen(nmp->nm_locations.nl_locations[loc]->nl_servers[serv]->ns_addresses[addr]));
			}
			xb_add_32(error, &xbinfo, 0); /* empty server info */
		}
		xb_add_32(error, &xbinfo, nmp->nm_locations.nl_locations[loc]->nl_path.np_compcount);
		for (comp = 0; !error && (comp < nmp->nm_locations.nl_locations[loc]->nl_path.np_compcount); comp++) {
			xb_add_string(error, &xbinfo, nmp->nm_locations.nl_locations[loc]->nl_path.np_components[comp],
			    strlen(nmp->nm_locations.nl_locations[loc]->nl_path.np_components[comp]));
		}
		xb_add_32(error, &xbinfo, 0); /* empty fs location info */
	}
	xb_add_32(error, &xbinfo, vfs_flags(nmp->nm_mountp));           /* MNTFLAGS */
	if (origargsvers < NFS_ARGSVERSION_XDR) {
		xb_add_string(error, &xbinfo, vfs_statfs(nmp->nm_mountp)->f_mntfromname,
		    strlen(vfs_statfs(nmp->nm_mountp)->f_mntfromname));         /* MNTFROM */
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_REALM)) {
		xb_add_string(error, &xbinfo, nmp->nm_realm, strlen(nmp->nm_realm));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_PRINCIPAL)) {
		xb_add_string(error, &xbinfo, nmp->nm_principal, strlen(nmp->nm_principal));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_SVCPRINCIPAL)) {
		xb_add_string(error, &xbinfo, nmp->nm_sprinc, strlen(nmp->nm_sprinc));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCAL_NFS_PORT)) {
		struct sockaddr_un *un = (struct sockaddr_un *)nmp->nm_saddr;
		xb_add_string(error, &xbinfo, un->sun_path, strlen(un->sun_path));
	}
	if (NFS_BITMAP_ISSET(mattrs, NFS_MATTR_LOCAL_MOUNT_PORT)) {
		xb_add_string(error, &xbinfo, nmp->nm_mount_localport, strlen(nmp->nm_mount_localport));
	}
	curargs_end_offset = xb_offset(&xbinfo);

	/* NFS_MIATTR_CUR_LOC_INDEX */
	xb_add_32(error, &xbinfo, nmp->nm_locations.nl_current.nli_flags);
	xb_add_32(error, &xbinfo, nmp->nm_locations.nl_current.nli_loc);
	xb_add_32(error, &xbinfo, nmp->nm_locations.nl_current.nli_serv);
	xb_add_32(error, &xbinfo, nmp->nm_locations.nl_current.nli_addr);

	xb_build_done(error, &xbinfo);

	/* update opaque counts */
	end_offset = xb_offset(&xbinfo);
	if (!error) {
		error = xb_seek(&xbinfo, attrslength_offset);
		xb_add_32(error, &xbinfo, curargs_end_offset - attrslength_offset - XDRWORD /*don't include length field*/);
	}
	if (!error) {
		error = xb_seek(&xbinfo, curargslength_offset);
		xb_add_32(error, &xbinfo, curargs_end_offset - curargslength_offset + XDRWORD /*version*/);
	}
	if (!error) {
		error = xb_seek(&xbinfo, curargsopaquelength_offset);
		xb_add_32(error, &xbinfo, curargs_end_offset - curargslength_offset + XDRWORD /*version*/);
	}
	if (!error) {
		error = xb_seek(&xbinfo, infolength_offset);
		xb_add_32(error, &xbinfo, end_offset - infolength_offset + XDRWORD /*version*/);
	}
	nfsmerr_if(error);

	/* copy result xdrbuf to caller */
	*xb = xbinfo;

	/* and mark the local copy as not needing cleanup */
	xbinfo.xb_flags &= ~XB_CLEANUP;
nfsmerr:
	xb_cleanup(&xbinfo);
	return error;
}

/*
 * Do that sysctl thang...
 */
int
nfs_vfs_sysctl(int *name, u_int namelen, user_addr_t oldp, size_t *oldlenp,
    user_addr_t newp, size_t newlen, vfs_context_t ctx)
{
	int error = 0, val;
	struct sysctl_req *req = NULL;
	union union_vfsidctl vc;
	mount_t mp;
	struct nfsmount *nmp = NULL;
	struct vfsquery vq;
	struct nfsreq *rq;
	boolean_t is_64_bit;
	fsid_t fsid;
	struct xdrbuf xb;
	struct netfs_status *nsp = NULL;
	int timeoutmask;
	uint totlen, count, numThreads;
#if CONFIG_NFS_SERVER
	uint pos;
	struct nfs_exportfs *nxfs;
	struct nfs_export *nx;
	struct nfs_active_user_list *ulist;
	struct nfs_export_stat_desc stat_desc = {};
	struct nfs_export_stat_rec statrec;
	struct nfs_user_stat_node *unode, *unode_next;
	struct nfs_user_stat_desc ustat_desc = {};
	struct nfs_user_stat_user_rec ustat_rec;
	struct nfs_user_stat_path_rec upath_rec;
	uint bytes_total, recs_copied;
	uint numExports;
	size_t bytes_avail, numRecs;
#endif /* CONFIG_NFS_SERVER */

	/*
	 * All names at this level are terminal.
	 */
	if (namelen > 1) {
		return ENOTDIR;       /* overloaded */
	}
	is_64_bit = vfs_context_is64bit(ctx);

	/* common code for "new style" VFS_CTL sysctl, get the mount. */
	switch (name[0]) {
	case VFS_CTL_TIMEO:
	case VFS_CTL_NOLOCKS:
	case VFS_CTL_NSTATUS:
#if defined(XNU_TARGET_OS_OSX)
	case VFS_CTL_QUERY:
#endif /* XNU_TARGET_OS_OSX */
		req = CAST_DOWN(struct sysctl_req *, oldp);
		if (req == NULL) {
			return EFAULT;
		}
		error = SYSCTL_IN(req, &vc, is_64_bit? sizeof(vc.vc64):sizeof(vc.vc32));
		if (error) {
			return error;
		}
		mp = vfs_getvfs(&vc.vc32.vc_fsid); /* works for 32 and 64 */
		if (mp == NULL) {
			return ENOENT;
		}
		nmp = VFSTONFS(mp);
		if (!nmp) {
			return ENOENT;
		}
		bzero(&vq, sizeof(vq));
		req->newidx = 0;
		if (is_64_bit) {
			req->newptr = vc.vc64.vc_ptr;
			req->newlen = (size_t)vc.vc64.vc_len;
		} else {
			req->newptr = CAST_USER_ADDR_T(vc.vc32.vc_ptr);
			req->newlen = vc.vc32.vc_len;
		}
		break;
#if !defined(XNU_TARGET_OS_OSX)
	case VFS_CTL_QUERY:
		return EPERM;
#endif /* ! XNU_TARGET_OS_OSX */
	}

	switch (name[0]) {
	case NFS_NFSSTATS:
		if (!oldp) {
			*oldlenp = sizeof nfsstats;
			return 0;
		}

		if (*oldlenp < sizeof nfsstats) {
			*oldlenp = sizeof nfsstats;
			return ENOMEM;
		}

		error = copyout(&nfsstats, oldp, sizeof nfsstats);
		if (error) {
			return error;
		}

		if (newp && newlen != sizeof nfsstats) {
			return EINVAL;
		}

		if (newp) {
			return copyin(newp, &nfsstats, sizeof nfsstats);
		}
		return 0;
	case NFS_NFSZEROSTATS:
		bzero(&nfsstats, sizeof nfsstats);
		return 0;
	case NFS_MOUNTINFO:
		/* read in the fsid */
		if (*oldlenp < sizeof(fsid)) {
			return EINVAL;
		}
		if ((error = copyin(oldp, &fsid, sizeof(fsid)))) {
			return error;
		}
		/* swizzle it back to host order */
		fsid.val[0] = ntohl(fsid.val[0]);
		fsid.val[1] = ntohl(fsid.val[1]);
		/* find mount and make sure it's NFS */
		if (((mp = vfs_getvfs(&fsid))) == NULL) {
			return ENOENT;
		}
		if (strcmp(mp->mnt_vfsstat.f_fstypename, "nfs")) {
			return EINVAL;
		}
		if (((nmp = VFSTONFS(mp))) == NULL) {
			return ENOENT;
		}
		xb_init(&xb, XDRBUF_NONE);
		if ((error = nfs_mountinfo_assemble(nmp, &xb))) {
			return error;
		}
		if (*oldlenp < xb.xb_u.xb_buffer.xbb_len) {
			error = ENOMEM;
		} else {
			error = copyout(xb_buffer_base(&xb), oldp, xb.xb_u.xb_buffer.xbb_len);
		}
		*oldlenp = xb.xb_u.xb_buffer.xbb_len;
		xb_cleanup(&xb);
		break;
#if CONFIG_NFS_SERVER
	case NFS_EXPORTSTATS:
		/* setup export stat descriptor */
		stat_desc.rec_vers = NFS_EXPORT_STAT_REC_VERSION;

		if (!nfsrv_is_initialized()) {
			stat_desc.rec_count = 0;
			if (oldp && (*oldlenp >= sizeof(struct nfs_export_stat_desc))) {
				error = copyout(&stat_desc, oldp, sizeof(struct nfs_export_stat_desc));
			}
			*oldlenp = sizeof(struct nfs_export_stat_desc);
			return error;
		}

		/* Count the number of exported directories */
		lck_rw_lock_shared(&nfsrv_export_rwlock);
		numExports = 0;
		LIST_FOREACH(nxfs, &nfsrv_exports, nxfs_next)
		LIST_FOREACH(nx, &nxfs->nxfs_exports, nx_next)
		numExports += 1;

		/* update stat descriptor's export record count */
		stat_desc.rec_count = numExports;

		/* calculate total size of required buffer */
		totlen = sizeof(struct nfs_export_stat_desc) + (numExports * sizeof(struct nfs_export_stat_rec));

		/* Check caller's buffer */
		if (oldp == 0) {
			lck_rw_done(&nfsrv_export_rwlock);
			/* indicate required buffer len */
			*oldlenp = totlen;
			return 0;
		}

		/* We require the caller's buffer to be at least large enough to hold the descriptor */
		if (*oldlenp < sizeof(struct nfs_export_stat_desc)) {
			lck_rw_done(&nfsrv_export_rwlock);
			/* indicate required buffer len */
			*oldlenp = totlen;
			return ENOMEM;
		}

		/* indicate required buffer len */
		*oldlenp = totlen;

		/* check if export table is empty */
		if (!numExports) {
			lck_rw_done(&nfsrv_export_rwlock);
			error = copyout(&stat_desc, oldp, sizeof(struct nfs_export_stat_desc));
			return error;
		}

		/* calculate how many actual export stat records fit into caller's buffer */
		numRecs = (*oldlenp - sizeof(struct nfs_export_stat_desc)) / sizeof(struct nfs_export_stat_rec);

		if (!numRecs) {
			/* caller's buffer can only accomodate descriptor */
			lck_rw_done(&nfsrv_export_rwlock);
			stat_desc.rec_count = 0;
			error = copyout(&stat_desc, oldp, sizeof(struct nfs_export_stat_desc));
			return error;
		}

		/* adjust to actual number of records to copyout to caller's buffer */
		if (numRecs > numExports) {
			numRecs = numExports;
		}

		/* set actual number of records we are returning */
		stat_desc.rec_count = numRecs;

		/* first copy out the stat descriptor */
		pos = 0;
		error = copyout(&stat_desc, oldp + pos, sizeof(struct nfs_export_stat_desc));
		if (error) {
			lck_rw_done(&nfsrv_export_rwlock);
			return error;
		}
		pos += sizeof(struct nfs_export_stat_desc);

		/* Loop through exported directories */
		count = 0;
		LIST_FOREACH(nxfs, &nfsrv_exports, nxfs_next) {
			LIST_FOREACH(nx, &nxfs->nxfs_exports, nx_next) {
				if (count >= numRecs) {
					break;
				}

				/* build exported filesystem path */
				memset(statrec.path, 0, sizeof(statrec.path));
				snprintf(statrec.path, sizeof(statrec.path), "%s%s%s",
				    nxfs->nxfs_path, ((nxfs->nxfs_path[1] && nx->nx_path[0]) ? "/" : ""),
				    nx->nx_path);

				/* build the 64-bit export stat counters */
				statrec.ops = ((uint64_t)nx->nx_stats.ops.hi << 32) |
				    nx->nx_stats.ops.lo;
				statrec.bytes_read = ((uint64_t)nx->nx_stats.bytes_read.hi << 32) |
				    nx->nx_stats.bytes_read.lo;
				statrec.bytes_written = ((uint64_t)nx->nx_stats.bytes_written.hi << 32) |
				    nx->nx_stats.bytes_written.lo;
				error = copyout(&statrec, oldp + pos, sizeof(statrec));
				if (error) {
					lck_rw_done(&nfsrv_export_rwlock);
					return error;
				}
				/* advance buffer position */
				pos += sizeof(statrec);
			}
		}
		lck_rw_done(&nfsrv_export_rwlock);
		break;
	case NFS_USERSTATS:
		/* init structures used for copying out of kernel */
		ustat_desc.rec_vers = NFS_USER_STAT_REC_VERSION;
		ustat_rec.rec_type = NFS_USER_STAT_USER_REC;
		upath_rec.rec_type = NFS_USER_STAT_PATH_REC;

		/* initialize counters */
		bytes_total = sizeof(struct nfs_user_stat_desc);
		bytes_avail  = *oldlenp;
		recs_copied = 0;

		if (!nfsrv_is_initialized()) { /* NFS server not initialized, so no stats */
			goto ustat_skip;
		}

		/* reclaim old expired user nodes */
		nfsrv_active_user_list_reclaim();

		/* reserve space for the buffer descriptor */
		if (bytes_avail >= sizeof(struct nfs_user_stat_desc)) {
			bytes_avail -= sizeof(struct nfs_user_stat_desc);
		} else {
			bytes_avail = 0;
		}

		/* put buffer position past the buffer descriptor */
		pos = sizeof(struct nfs_user_stat_desc);

		/* Loop through exported directories */
		lck_rw_lock_shared(&nfsrv_export_rwlock);
		LIST_FOREACH(nxfs, &nfsrv_exports, nxfs_next) {
			LIST_FOREACH(nx, &nxfs->nxfs_exports, nx_next) {
				/* copy out path */
				if (bytes_avail >= sizeof(struct nfs_user_stat_path_rec)) {
					memset(upath_rec.path, 0, sizeof(upath_rec.path));
					snprintf(upath_rec.path, sizeof(upath_rec.path), "%s%s%s",
					    nxfs->nxfs_path, ((nxfs->nxfs_path[1] && nx->nx_path[0]) ? "/" : ""),
					    nx->nx_path);

					error = copyout(&upath_rec, oldp + pos, sizeof(struct nfs_user_stat_path_rec));
					if (error) {
						/* punt */
						goto ustat_done;
					}

					pos += sizeof(struct nfs_user_stat_path_rec);
					bytes_avail -= sizeof(struct nfs_user_stat_path_rec);
					recs_copied++;
				} else {
					/* Caller's buffer is exhausted */
					bytes_avail = 0;
				}

				bytes_total += sizeof(struct nfs_user_stat_path_rec);

				/* Scan through all user nodes of this export */
				ulist = &nx->nx_user_list;
				lck_mtx_lock(&ulist->user_mutex);
				for (unode = TAILQ_FIRST(&ulist->user_lru); unode; unode = unode_next) {
					unode_next = TAILQ_NEXT(unode, lru_link);

					/* copy out node if there is space */
					if (bytes_avail >= sizeof(struct nfs_user_stat_user_rec)) {
						/* prepare a user stat rec for copying out */
						ustat_rec.uid = unode->uid;
						memset(&ustat_rec.sock, 0, sizeof(ustat_rec.sock));
						bcopy(&unode->sock, &ustat_rec.sock, unode->sock.ss_len);
						ustat_rec.ops = unode->ops;
						ustat_rec.bytes_read = unode->bytes_read;
						ustat_rec.bytes_written = unode->bytes_written;
						ustat_rec.tm_start = unode->tm_start;
						ustat_rec.tm_last = unode->tm_last;

						error = copyout(&ustat_rec, oldp + pos, sizeof(struct nfs_user_stat_user_rec));

						if (error) {
							/* punt */
							lck_mtx_unlock(&ulist->user_mutex);
							goto ustat_done;
						}

						pos += sizeof(struct nfs_user_stat_user_rec);
						bytes_avail -= sizeof(struct nfs_user_stat_user_rec);
						recs_copied++;
					} else {
						/* Caller's buffer is exhausted */
						bytes_avail = 0;
					}
					bytes_total += sizeof(struct nfs_user_stat_user_rec);
				}
				/* can unlock this export's list now */
				lck_mtx_unlock(&ulist->user_mutex);
			}
		}

ustat_done:
		/* unlock the export table */
		lck_rw_done(&nfsrv_export_rwlock);

ustat_skip:
		/* indicate number of actual records copied */
		ustat_desc.rec_count = recs_copied;

		if (!error) {
			/* check if there was enough room for the buffer descriptor */
			if (*oldlenp >= sizeof(struct nfs_user_stat_desc)) {
				error = copyout(&ustat_desc, oldp, sizeof(struct nfs_user_stat_desc));
			} else {
				error = ENOMEM;
			}

			/* always indicate required buffer size */
			*oldlenp = bytes_total;
		}
		break;
	case NFS_USERCOUNT:
		if (!oldp) {
			*oldlenp = sizeof(nfsrv_user_stat_node_count);
			return 0;
		}

		if (*oldlenp < sizeof(nfsrv_user_stat_node_count)) {
			*oldlenp = sizeof(nfsrv_user_stat_node_count);
			return ENOMEM;
		}

		if (nfsrv_is_initialized()) {
			/* reclaim old expired user nodes */
			nfsrv_active_user_list_reclaim();
		}

		error = copyout(&nfsrv_user_stat_node_count, oldp, sizeof(nfsrv_user_stat_node_count));
		break;
#endif /* CONFIG_NFS_SERVER */
	case VFS_CTL_NOLOCKS:
		if (req->oldptr != USER_ADDR_NULL) {
			lck_mtx_lock(&nmp->nm_lock);
			val = (nmp->nm_lockmode == NFS_LOCK_MODE_DISABLED) ? 1 : 0;
			lck_mtx_unlock(&nmp->nm_lock);
			error = SYSCTL_OUT(req, &val, sizeof(val));
			if (error) {
				return error;
			}
		}
		if (req->newptr != USER_ADDR_NULL) {
			error = SYSCTL_IN(req, &val, sizeof(val));
			if (error) {
				return error;
			}
			lck_mtx_lock(&nmp->nm_lock);
			if (nmp->nm_lockmode == NFS_LOCK_MODE_LOCAL) {
				/* can't toggle locks when using local locks */
				error = EINVAL;
#if CONFIG_NFS4
			} else if ((nmp->nm_vers >= NFS_VER4) && val) {
				/* can't disable locks for NFSv4 */
				error = EINVAL;
#endif
			} else if (val) {
				if ((nmp->nm_vers <= NFS_VER3) && (nmp->nm_lockmode == NFS_LOCK_MODE_ENABLED)) {
					nfs_lockd_mount_unregister(nmp);
				}
				nmp->nm_lockmode = NFS_LOCK_MODE_DISABLED;
				nmp->nm_state &= ~NFSSTA_LOCKTIMEO;
			} else {
				if ((nmp->nm_vers <= NFS_VER3) && (nmp->nm_lockmode == NFS_LOCK_MODE_DISABLED)) {
					nfs_lockd_mount_register(nmp);
				}
				nmp->nm_lockmode = NFS_LOCK_MODE_ENABLED;
			}
			lck_mtx_unlock(&nmp->nm_lock);
		}
		break;
#if defined(XNU_TARGET_OS_OSX)
	case VFS_CTL_QUERY:
		lck_mtx_lock(&nmp->nm_lock);
		/* XXX don't allow users to know about/disconnect unresponsive, soft, nobrowse mounts */
		int softnobrowse = (NMFLAG(nmp, SOFT) && (vfs_flags(nmp->nm_mountp) & MNT_DONTBROWSE));
		if (!softnobrowse && (nmp->nm_state & NFSSTA_TIMEO)) {
			vq.vq_flags |= VQ_NOTRESP;
		}
		if (!softnobrowse && (nmp->nm_state & NFSSTA_JUKEBOXTIMEO) && !NMFLAG(nmp, MUTEJUKEBOX)) {
			vq.vq_flags |= VQ_NOTRESP;
		}
		if (!softnobrowse && (nmp->nm_state & NFSSTA_LOCKTIMEO) &&
		    (nmp->nm_lockmode == NFS_LOCK_MODE_ENABLED)) {
			vq.vq_flags |= VQ_NOTRESP;
		}
		if (nmp->nm_state & NFSSTA_DEAD) {
			vq.vq_flags |= VQ_DEAD;
		}
		lck_mtx_unlock(&nmp->nm_lock);
		error = SYSCTL_OUT(req, &vq, sizeof(vq));
		break;
#endif /* XNU_TARGET_OS_OSX */
	case VFS_CTL_TIMEO:
		if (req->oldptr != USER_ADDR_NULL) {
			lck_mtx_lock(&nmp->nm_lock);
			val = nmp->nm_tprintf_initial_delay;
			lck_mtx_unlock(&nmp->nm_lock);
			error = SYSCTL_OUT(req, &val, sizeof(val));
			if (error) {
				return error;
			}
		}
		if (req->newptr != USER_ADDR_NULL) {
			error = SYSCTL_IN(req, &val, sizeof(val));
			if (error) {
				return error;
			}
			lck_mtx_lock(&nmp->nm_lock);
			if (val < 0) {
				nmp->nm_tprintf_initial_delay = 0;
			} else {
				nmp->nm_tprintf_initial_delay = val;
			}
			lck_mtx_unlock(&nmp->nm_lock);
		}
		break;
	case VFS_CTL_NSTATUS:
		/*
		 * Return the status of this mount.  This is much more
		 * information than VFS_CTL_QUERY.  In addition to the
		 * vq_flags return the significant mount options along
		 * with the list of threads blocked on the mount and
		 * how long the threads have been waiting.
		 */

		lck_mtx_lock(&nfs_request_mutex);
		lck_mtx_lock(&nmp->nm_lock);

		/*
		 * Count the number of requests waiting for a reply.
		 * Note: there could be multiple requests from the same thread.
		 */
		numThreads = 0;
		TAILQ_FOREACH(rq, &nfs_reqq, r_chain) {
			if (rq->r_nmp == nmp) {
				numThreads++;
			}
		}

		/* Calculate total size of result buffer */
		totlen = sizeof(struct netfs_status) + (numThreads * sizeof(uint64_t));

		if (req->oldptr == USER_ADDR_NULL) {            // Caller is querying buffer size
			lck_mtx_unlock(&nmp->nm_lock);
			lck_mtx_unlock(&nfs_request_mutex);
			return SYSCTL_OUT(req, NULL, totlen);
		}
		if (req->oldlen < totlen) {     // Check if caller's buffer is big enough
			lck_mtx_unlock(&nmp->nm_lock);
			lck_mtx_unlock(&nfs_request_mutex);
			return ERANGE;
		}

		MALLOC(nsp, struct netfs_status *, totlen, M_TEMP, M_WAITOK | M_ZERO);
		if (nsp == NULL) {
			lck_mtx_unlock(&nmp->nm_lock);
			lck_mtx_unlock(&nfs_request_mutex);
			return ENOMEM;
		}
		timeoutmask = NFSSTA_TIMEO | NFSSTA_LOCKTIMEO | NFSSTA_JUKEBOXTIMEO;
		if (nmp->nm_state & timeoutmask) {
			nsp->ns_status |= VQ_NOTRESP;
		}
		if (nmp->nm_state & NFSSTA_DEAD) {
			nsp->ns_status |= VQ_DEAD;
		}

		(void) nfs_mountopts(nmp, nsp->ns_mountopts, sizeof(nsp->ns_mountopts));
		nsp->ns_threadcount = numThreads;

		/*
		 * Get the thread ids of threads waiting for a reply
		 * and find the longest wait time.
		 */
		if (numThreads > 0) {
			struct timeval now;
			time_t sendtime;
			uint64_t waittime;

			microuptime(&now);
			count = 0;
			sendtime = now.tv_sec;
			TAILQ_FOREACH(rq, &nfs_reqq, r_chain) {
				if (rq->r_nmp == nmp) {
					if (rq->r_start < sendtime) {
						sendtime = rq->r_start;
					}
					// A thread_id of zero is used to represent an async I/O request.
					nsp->ns_threadids[count] =
					    rq->r_thread ? thread_tid(rq->r_thread) : 0;
					if (++count >= numThreads) {
						break;
					}
				}
			}
			waittime = now.tv_sec - sendtime;
			nsp->ns_waittime = waittime > UINT32_MAX ? UINT32_MAX : (uint32_t)waittime;
		}

		lck_mtx_unlock(&nmp->nm_lock);
		lck_mtx_unlock(&nfs_request_mutex);

		error = SYSCTL_OUT(req, nsp, totlen);
		FREE(nsp, M_TEMP);
		break;
	default:
		return ENOTSUP;
	}
	return error;
}

#endif /* CONFIG_NFS_CLIENT */
