/*
 * Copyright (c) 2012-2020 Apple Inc. All rights reserved.
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

#include <kern/locks.h>
#include <kern/policy_internal.h>
#include <kern/zalloc.h>

#include <mach/sdt.h>

#include <sys/domain.h>
#include <sys/kdebug.h>
#include <sys/kern_control.h>
#include <sys/kernel.h>
#include <sys/mbuf.h>
#include <sys/mcache.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/resourcevar.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/systm.h>

#include <net/content_filter.h>
#include <net/if.h>
#include <net/if_var.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcp_var.h>
#include <netinet/mptcp_var.h>
#include <netinet/mptcp.h>
#include <netinet/mptcp_opt.h>
#include <netinet/mptcp_seq.h>
#include <netinet/mptcp_timer.h>
#include <libkern/crypto/sha1.h>
#include <netinet6/in6_pcb.h>
#include <netinet6/ip6protosw.h>
#include <dev/random/randomdev.h>

/*
 * Notes on MPTCP implementation.
 *
 * MPTCP is implemented as <SOCK_STREAM,IPPROTO_TCP> protocol in PF_MULTIPATH
 * communication domain.  The structure mtcbinfo describes the MPTCP instance
 * of a Multipath protocol in that domain.  It is used to keep track of all
 * MPTCP PCB instances in the system, and is protected by the global lock
 * mppi_lock.
 *
 * An MPTCP socket is opened by calling socket(PF_MULTIPATH, SOCK_STREAM,
 * IPPROTO_TCP).  Upon success, a Multipath PCB gets allocated and along with
 * it comes an MPTCP Session and an MPTCP PCB.  All three structures are
 * allocated from the same memory block, and each structure has a pointer
 * to the adjacent ones.  The layout is defined by the mpp_mtp structure.
 * The socket lock (mpp_lock) is used to protect accesses to the Multipath
 * PCB (mppcb) as well as the MPTCP Session (mptses).
 *
 * The MPTCP Session is an MPTCP-specific extension to the Multipath PCB;
 *
 * A functioning MPTCP Session consists of one or more subflow sockets.  Each
 * subflow socket is essentially a regular PF_INET/PF_INET6 TCP socket, and is
 * represented by the mptsub structure.  Because each subflow requires access
 * to the MPTCP Session, the MPTCP socket's so_usecount is bumped up for each
 * subflow.  This gets decremented prior to the subflow's destruction.
 *
 * To handle events (read, write, control) from the subflows, we do direct
 * upcalls into the specific function.
 *
 * The whole MPTCP connection is protected by a single lock, the MPTCP socket's
 * lock. Incoming data on a subflow also ends up taking this single lock. To
 * achieve the latter, tcp_lock/unlock has been changed to rather use the lock
 * of the MPTCP-socket.
 *
 * An MPTCP socket will be destroyed when its so_usecount drops to zero; this
 * work is done by the MPTCP garbage collector which is invoked on demand by
 * the PF_MULTIPATH garbage collector.  This process will take place once all
 * of the subflows have been destroyed.
 */

static void mptcp_attach_to_subf(struct socket *, struct mptcb *, uint8_t);
static void mptcp_detach_mptcb_from_subf(struct mptcb *, struct socket *);

static uint32_t mptcp_gc(struct mppcbinfo *);
static int mptcp_subflow_soreceive(struct socket *, struct sockaddr **,
    struct uio *, struct mbuf **, struct mbuf **, int *);
static int mptcp_subflow_sosend(struct socket *, struct sockaddr *,
    struct uio *, struct mbuf *, struct mbuf *, int);
static void mptcp_subflow_wupcall(struct socket *, void *, int);
static void mptcp_subflow_eupcall1(struct socket *so, void *arg, long events);
static void mptcp_update_last_owner(struct socket *so, struct socket *mp_so);
static void mptcp_drop_tfo_data(struct mptses *, struct mptsub *);

static void mptcp_subflow_abort(struct mptsub *, int);

static void mptcp_send_dfin(struct socket *so);
static void mptcp_set_cellicon(struct mptses *mpte, struct mptsub *mpts);
static int mptcp_freeq(struct mptcb *mp_tp);

/*
 * Possible return values for subflow event handlers.  Note that success
 * values must be greater or equal than MPTS_EVRET_OK.  Values less than that
 * indicate errors or actions which require immediate attention; they will
 * prevent the rest of the handlers from processing their respective events
 * until the next round of events processing.
 */
typedef enum {
	MPTS_EVRET_DELETE               = 1,    /* delete this subflow */
	MPTS_EVRET_OK                   = 2,    /* OK */
	MPTS_EVRET_CONNECT_PENDING      = 3,    /* resume pended connects */
	MPTS_EVRET_DISCONNECT_FALLBACK  = 4,    /* abort all but preferred */
} ev_ret_t;

static ev_ret_t mptcp_subflow_propagate_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_nosrcaddr_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_failover_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_ifdenied_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_connected_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_disconnected_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_mpstatus_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_mustrst_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_mpcantrcvmore_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_mpsuberror_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_adaptive_rtimo_ev(struct mptses *, struct mptsub *, long *, long);
static ev_ret_t mptcp_subflow_adaptive_wtimo_ev(struct mptses *, struct mptsub *, long *, long);

static void mptcp_do_sha1(mptcp_key_t *, char *);
static void mptcp_init_local_parms(struct mptses *);

static ZONE_DECLARE(mptsub_zone, "mptsub", sizeof(struct mptsub), ZC_ZFREE_CLEARMEM);
static ZONE_DECLARE(mptopt_zone, "mptopt", sizeof(struct mptopt), ZC_ZFREE_CLEARMEM);
static ZONE_DECLARE(mpt_subauth_zone, "mptauth",
    sizeof(struct mptcp_subf_auth_entry), ZC_NONE);

struct mppcbinfo mtcbinfo;

SYSCTL_DECL(_net_inet);

SYSCTL_NODE(_net_inet, OID_AUTO, mptcp, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "MPTCP");

uint32_t mptcp_dbg_area = 31;           /* more noise if greater than 1 */
SYSCTL_UINT(_net_inet_mptcp, OID_AUTO, dbg_area, CTLFLAG_RW | CTLFLAG_LOCKED,
    &mptcp_dbg_area, 0, "MPTCP debug area");

uint32_t mptcp_dbg_level = 1;
SYSCTL_INT(_net_inet_mptcp, OID_AUTO, dbg_level, CTLFLAG_RW | CTLFLAG_LOCKED,
    &mptcp_dbg_level, 0, "MPTCP debug level");

SYSCTL_UINT(_net_inet_mptcp, OID_AUTO, pcbcount, CTLFLAG_RD | CTLFLAG_LOCKED,
    &mtcbinfo.mppi_count, 0, "Number of active PCBs");


static int mptcp_alternate_port = 0;
SYSCTL_INT(_net_inet_mptcp, OID_AUTO, alternate_port, CTLFLAG_RW | CTLFLAG_LOCKED,
    &mptcp_alternate_port, 0, "Set alternate port for MPTCP connections");

static struct protosw mptcp_subflow_protosw;
static struct pr_usrreqs mptcp_subflow_usrreqs;
static struct ip6protosw mptcp_subflow_protosw6;
static struct pr_usrreqs mptcp_subflow_usrreqs6;

static uint8_t  mptcp_create_subflows_scheduled;

typedef struct mptcp_subflow_event_entry {
	long        sofilt_hint_mask;
	ev_ret_t    (*sofilt_hint_ev_hdlr)(
		struct mptses *mpte,
		struct mptsub *mpts,
		long *p_mpsofilt_hint,
		long event);
} mptsub_ev_entry_t;

/* Using Symptoms Advisory to detect poor WiFi or poor Cell */
static kern_ctl_ref mptcp_kern_ctrl_ref = NULL;
static uint32_t mptcp_kern_skt_inuse = 0;
static uint32_t mptcp_kern_skt_unit;
static symptoms_advisory_t mptcp_advisory;

uint32_t mptcp_cellicon_refcount = 0;

/*
 * XXX The order of the event handlers below is really
 * really important. Think twice before changing it.
 */
static mptsub_ev_entry_t mpsub_ev_entry_tbl[] = {
	{
		.sofilt_hint_mask = SO_FILT_HINT_MP_SUB_ERROR,
		.sofilt_hint_ev_hdlr = mptcp_subflow_mpsuberror_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_MPCANTRCVMORE,
		.sofilt_hint_ev_hdlr =  mptcp_subflow_mpcantrcvmore_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_MPFAILOVER,
		.sofilt_hint_ev_hdlr = mptcp_subflow_failover_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_CONNRESET,
		.sofilt_hint_ev_hdlr = mptcp_subflow_propagate_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_MUSTRST,
		.sofilt_hint_ev_hdlr = mptcp_subflow_mustrst_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_CANTRCVMORE,
		.sofilt_hint_ev_hdlr = mptcp_subflow_propagate_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_TIMEOUT,
		.sofilt_hint_ev_hdlr = mptcp_subflow_propagate_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_NOSRCADDR,
		.sofilt_hint_ev_hdlr = mptcp_subflow_nosrcaddr_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_IFDENIED,
		.sofilt_hint_ev_hdlr = mptcp_subflow_ifdenied_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_CONNECTED,
		.sofilt_hint_ev_hdlr = mptcp_subflow_connected_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_MPSTATUS,
		.sofilt_hint_ev_hdlr = mptcp_subflow_mpstatus_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_DISCONNECTED,
		.sofilt_hint_ev_hdlr = mptcp_subflow_disconnected_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_ADAPTIVE_RTIMO,
		.sofilt_hint_ev_hdlr = mptcp_subflow_adaptive_rtimo_ev,
	},
	{
		.sofilt_hint_mask = SO_FILT_HINT_ADAPTIVE_WTIMO,
		.sofilt_hint_ev_hdlr = mptcp_subflow_adaptive_wtimo_ev,
	},
};

os_log_t mptcp_log_handle;

/*
 * Protocol pr_init callback.
 */
void
mptcp_init(struct protosw *pp, struct domain *dp)
{
#pragma unused(dp)
	static int mptcp_initialized = 0;
	struct protosw *prp;
	struct ip6protosw *prp6;

	VERIFY((pp->pr_flags & (PR_INITIALIZED | PR_ATTACHED)) == PR_ATTACHED);

	/* do this only once */
	if (mptcp_initialized) {
		return;
	}
	mptcp_initialized = 1;

	mptcp_advisory.sa_wifi_status = SYMPTOMS_ADVISORY_WIFI_OK;

	/*
	 * Since PF_MULTIPATH gets initialized after PF_INET/INET6,
	 * we must be able to find IPPROTO_TCP entries for both.
	 */
	prp = pffindproto_locked(PF_INET, IPPROTO_TCP, SOCK_STREAM);
	VERIFY(prp != NULL);
	bcopy(prp, &mptcp_subflow_protosw, sizeof(*prp));
	bcopy(prp->pr_usrreqs, &mptcp_subflow_usrreqs,
	    sizeof(mptcp_subflow_usrreqs));
	mptcp_subflow_protosw.pr_entry.tqe_next = NULL;
	mptcp_subflow_protosw.pr_entry.tqe_prev = NULL;
	mptcp_subflow_protosw.pr_usrreqs = &mptcp_subflow_usrreqs;
	mptcp_subflow_usrreqs.pru_soreceive = mptcp_subflow_soreceive;
	mptcp_subflow_usrreqs.pru_sosend = mptcp_subflow_sosend;
	mptcp_subflow_usrreqs.pru_rcvoob = pru_rcvoob_notsupp;
	/*
	 * Socket filters shouldn't attach/detach to/from this protosw
	 * since pr_protosw is to be used instead, which points to the
	 * real protocol; if they do, it is a bug and we should panic.
	 */
	mptcp_subflow_protosw.pr_filter_head.tqh_first =
	    (struct socket_filter *)(uintptr_t)0xdeadbeefdeadbeef;
	mptcp_subflow_protosw.pr_filter_head.tqh_last =
	    (struct socket_filter **)(uintptr_t)0xdeadbeefdeadbeef;

	prp6 = (struct ip6protosw *)pffindproto_locked(PF_INET6,
	    IPPROTO_TCP, SOCK_STREAM);
	VERIFY(prp6 != NULL);
	bcopy(prp6, &mptcp_subflow_protosw6, sizeof(*prp6));
	bcopy(prp6->pr_usrreqs, &mptcp_subflow_usrreqs6,
	    sizeof(mptcp_subflow_usrreqs6));
	mptcp_subflow_protosw6.pr_entry.tqe_next = NULL;
	mptcp_subflow_protosw6.pr_entry.tqe_prev = NULL;
	mptcp_subflow_protosw6.pr_usrreqs = &mptcp_subflow_usrreqs6;
	mptcp_subflow_usrreqs6.pru_soreceive = mptcp_subflow_soreceive;
	mptcp_subflow_usrreqs6.pru_sosend = mptcp_subflow_sosend;
	mptcp_subflow_usrreqs6.pru_rcvoob = pru_rcvoob_notsupp;
	/*
	 * Socket filters shouldn't attach/detach to/from this protosw
	 * since pr_protosw is to be used instead, which points to the
	 * real protocol; if they do, it is a bug and we should panic.
	 */
	mptcp_subflow_protosw6.pr_filter_head.tqh_first =
	    (struct socket_filter *)(uintptr_t)0xdeadbeefdeadbeef;
	mptcp_subflow_protosw6.pr_filter_head.tqh_last =
	    (struct socket_filter **)(uintptr_t)0xdeadbeefdeadbeef;

	bzero(&mtcbinfo, sizeof(mtcbinfo));
	TAILQ_INIT(&mtcbinfo.mppi_pcbs);
	mtcbinfo.mppi_size = sizeof(struct mpp_mtp);
	mtcbinfo.mppi_zone = zone_create("mptc", mtcbinfo.mppi_size,
	    ZC_NONE);

	mtcbinfo.mppi_lock_grp_attr = lck_grp_attr_alloc_init();
	mtcbinfo.mppi_lock_grp = lck_grp_alloc_init("mppcb",
	    mtcbinfo.mppi_lock_grp_attr);
	mtcbinfo.mppi_lock_attr = lck_attr_alloc_init();
	lck_mtx_init(&mtcbinfo.mppi_lock, mtcbinfo.mppi_lock_grp,
	    mtcbinfo.mppi_lock_attr);

	mtcbinfo.mppi_gc = mptcp_gc;
	mtcbinfo.mppi_timer = mptcp_timer;

	/* attach to MP domain for garbage collection to take place */
	mp_pcbinfo_attach(&mtcbinfo);

	mptcp_log_handle = os_log_create("com.apple.xnu.net.mptcp", "mptcp");
}

int
mptcpstats_get_index_by_ifindex(struct mptcp_itf_stats *stats, u_short ifindex, boolean_t create)
{
	int i, index = -1;

	for (i = 0; i < MPTCP_ITFSTATS_SIZE; i++) {
		if (create && stats[i].ifindex == IFSCOPE_NONE) {
			if (index < 0) {
				index = i;
			}
			continue;
		}

		if (stats[i].ifindex == ifindex) {
			index = i;
			return index;
		}
	}

	if (index != -1) {
		stats[index].ifindex = ifindex;
	}

	return index;
}

static int
mptcpstats_get_index(struct mptcp_itf_stats *stats, const struct mptsub *mpts)
{
	const struct ifnet *ifp = sotoinpcb(mpts->mpts_socket)->inp_last_outifp;
	int index;

	if (ifp == NULL) {
		os_log_error(mptcp_log_handle, "%s - %lx: no ifp on subflow, state %u flags %#x\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpts->mpts_mpte),
		    sototcpcb(mpts->mpts_socket)->t_state, mpts->mpts_flags);
		return -1;
	}

	index = mptcpstats_get_index_by_ifindex(stats, ifp->if_index, true);

	if (index != -1) {
		if (stats[index].is_expensive == 0) {
			stats[index].is_expensive = IFNET_IS_CELLULAR(ifp);
		}
	}

	return index;
}

void
mptcpstats_inc_switch(struct mptses *mpte, const struct mptsub *mpts)
{
	int index;

	tcpstat.tcps_mp_switches++;
	mpte->mpte_subflow_switches++;

	index = mptcpstats_get_index(mpte->mpte_itfstats, mpts);

	if (index != -1) {
		mpte->mpte_itfstats[index].switches++;
	}
}

/*
 * Flushes all recorded socket options from an MP socket.
 */
static void
mptcp_flush_sopts(struct mptses *mpte)
{
	struct mptopt *mpo, *tmpo;

	TAILQ_FOREACH_SAFE(mpo, &mpte->mpte_sopts, mpo_entry, tmpo) {
		mptcp_sopt_remove(mpte, mpo);
		mptcp_sopt_free(mpo);
	}
	VERIFY(TAILQ_EMPTY(&mpte->mpte_sopts));
}

/*
 * Create an MPTCP session, called as a result of opening a MPTCP socket.
 */
int
mptcp_session_create(struct mppcb *mpp)
{
	struct mppcbinfo *mppi;
	struct mptses *mpte;
	struct mptcb *mp_tp;

	VERIFY(mpp != NULL);
	mppi = mpp->mpp_pcbinfo;
	VERIFY(mppi != NULL);

	__IGNORE_WCASTALIGN(mpte = &((struct mpp_mtp *)mpp)->mpp_ses);
	__IGNORE_WCASTALIGN(mp_tp = &((struct mpp_mtp *)mpp)->mtcb);

	/* MPTCP Multipath PCB Extension */
	bzero(mpte, sizeof(*mpte));
	VERIFY(mpp->mpp_pcbe == NULL);
	mpp->mpp_pcbe = mpte;
	mpte->mpte_mppcb = mpp;
	mpte->mpte_mptcb = mp_tp;

	TAILQ_INIT(&mpte->mpte_sopts);
	TAILQ_INIT(&mpte->mpte_subflows);
	mpte->mpte_associd = SAE_ASSOCID_ANY;
	mpte->mpte_connid_last = SAE_CONNID_ANY;

	mptcp_init_urgency_timer(mpte);

	mpte->mpte_itfinfo = &mpte->_mpte_itfinfo[0];
	mpte->mpte_itfinfo_size = MPTE_ITFINFO_SIZE;

	if (mptcp_alternate_port > 0 && mptcp_alternate_port < UINT16_MAX) {
		mpte->mpte_alternate_port = htons((uint16_t)mptcp_alternate_port);
	}

	mpte->mpte_last_cellicon_set = tcp_now;

	/* MPTCP Protocol Control Block */
	bzero(mp_tp, sizeof(*mp_tp));
	mp_tp->mpt_mpte = mpte;
	mp_tp->mpt_state = MPTCPS_CLOSED;

	DTRACE_MPTCP1(session__create, struct mppcb *, mpp);

	return 0;
}

struct sockaddr *
mptcp_get_session_dst(struct mptses *mpte, boolean_t ipv6, boolean_t ipv4)
{
	if (ipv6 && mpte->mpte_sub_dst_v6.sin6_family == AF_INET6) {
		return (struct sockaddr *)&mpte->mpte_sub_dst_v6;
	}

	if (ipv4 && mpte->mpte_sub_dst_v4.sin_family == AF_INET) {
		return (struct sockaddr *)&mpte->mpte_sub_dst_v4;
	}

	/* The interface has neither IPv4 nor IPv6 routes. Give our best guess,
	 * meaning we prefer IPv6 over IPv4.
	 */
	if (mpte->mpte_sub_dst_v6.sin6_family == AF_INET6) {
		return (struct sockaddr *)&mpte->mpte_sub_dst_v6;
	}

	if (mpte->mpte_sub_dst_v4.sin_family == AF_INET) {
		return (struct sockaddr *)&mpte->mpte_sub_dst_v4;
	}

	/* We don't yet have a unicast IP */
	return NULL;
}

static void
mptcpstats_get_bytes(struct mptses *mpte, boolean_t initial_cell,
    uint64_t *cellbytes, uint64_t *allbytes)
{
	int64_t mycellbytes = 0;
	uint64_t myallbytes = 0;
	int i;

	for (i = 0; i < MPTCP_ITFSTATS_SIZE; i++) {
		if (mpte->mpte_itfstats[i].is_expensive) {
			mycellbytes += mpte->mpte_itfstats[i].mpis_txbytes;
			mycellbytes += mpte->mpte_itfstats[i].mpis_rxbytes;
		}

		myallbytes += mpte->mpte_itfstats[i].mpis_txbytes;
		myallbytes += mpte->mpte_itfstats[i].mpis_rxbytes;
	}

	if (initial_cell) {
		mycellbytes -= mpte->mpte_init_txbytes;
		mycellbytes -= mpte->mpte_init_rxbytes;
	}

	if (mycellbytes < 0) {
		os_log_error(mptcp_log_handle, "%s - %lx: cellbytes is %lld\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mycellbytes);
		*cellbytes = 0;
		*allbytes = 0;
	} else {
		*cellbytes = mycellbytes;
		*allbytes = myallbytes;
	}
}

static void
mptcpstats_session_wrapup(struct mptses *mpte)
{
	boolean_t cell = mpte->mpte_initial_cell;

	switch (mpte->mpte_svctype) {
	case MPTCP_SVCTYPE_HANDOVER:
		if (mpte->mpte_flags & MPTE_FIRSTPARTY) {
			tcpstat.tcps_mptcp_fp_handover_attempt++;

			if (cell && mpte->mpte_handshake_success) {
				tcpstat.tcps_mptcp_fp_handover_success_cell++;

				if (mpte->mpte_used_wifi) {
					tcpstat.tcps_mptcp_handover_wifi_from_cell++;
				}
			} else if (mpte->mpte_handshake_success) {
				tcpstat.tcps_mptcp_fp_handover_success_wifi++;

				if (mpte->mpte_used_cell) {
					tcpstat.tcps_mptcp_handover_cell_from_wifi++;
				}
			}
		} else {
			tcpstat.tcps_mptcp_handover_attempt++;

			if (cell && mpte->mpte_handshake_success) {
				tcpstat.tcps_mptcp_handover_success_cell++;

				if (mpte->mpte_used_wifi) {
					tcpstat.tcps_mptcp_handover_wifi_from_cell++;
				}
			} else if (mpte->mpte_handshake_success) {
				tcpstat.tcps_mptcp_handover_success_wifi++;

				if (mpte->mpte_used_cell) {
					tcpstat.tcps_mptcp_handover_cell_from_wifi++;
				}
			}
		}

		if (mpte->mpte_handshake_success) {
			uint64_t cellbytes;
			uint64_t allbytes;

			mptcpstats_get_bytes(mpte, cell, &cellbytes, &allbytes);

			tcpstat.tcps_mptcp_handover_cell_bytes += cellbytes;
			tcpstat.tcps_mptcp_handover_all_bytes += allbytes;
		}
		break;
	case MPTCP_SVCTYPE_INTERACTIVE:
		if (mpte->mpte_flags & MPTE_FIRSTPARTY) {
			tcpstat.tcps_mptcp_fp_interactive_attempt++;

			if (mpte->mpte_handshake_success) {
				tcpstat.tcps_mptcp_fp_interactive_success++;

				if (!cell && mpte->mpte_used_cell) {
					tcpstat.tcps_mptcp_interactive_cell_from_wifi++;
				}
			}
		} else {
			tcpstat.tcps_mptcp_interactive_attempt++;

			if (mpte->mpte_handshake_success) {
				tcpstat.tcps_mptcp_interactive_success++;

				if (!cell && mpte->mpte_used_cell) {
					tcpstat.tcps_mptcp_interactive_cell_from_wifi++;
				}
			}
		}

		if (mpte->mpte_handshake_success) {
			uint64_t cellbytes;
			uint64_t allbytes;

			mptcpstats_get_bytes(mpte, cell, &cellbytes, &allbytes);

			tcpstat.tcps_mptcp_interactive_cell_bytes += cellbytes;
			tcpstat.tcps_mptcp_interactive_all_bytes += allbytes;
		}
		break;
	case MPTCP_SVCTYPE_AGGREGATE:
		if (mpte->mpte_flags & MPTE_FIRSTPARTY) {
			tcpstat.tcps_mptcp_fp_aggregate_attempt++;

			if (mpte->mpte_handshake_success) {
				tcpstat.tcps_mptcp_fp_aggregate_success++;
			}
		} else {
			tcpstat.tcps_mptcp_aggregate_attempt++;

			if (mpte->mpte_handshake_success) {
				tcpstat.tcps_mptcp_aggregate_success++;
			}
		}

		if (mpte->mpte_handshake_success) {
			uint64_t cellbytes;
			uint64_t allbytes;

			mptcpstats_get_bytes(mpte, cell, &cellbytes, &allbytes);

			tcpstat.tcps_mptcp_aggregate_cell_bytes += cellbytes;
			tcpstat.tcps_mptcp_aggregate_all_bytes += allbytes;
		}
		break;
	}

	if (cell && mpte->mpte_handshake_success && mpte->mpte_used_wifi) {
		tcpstat.tcps_mptcp_back_to_wifi++;
	}

	if (mpte->mpte_triggered_cell) {
		tcpstat.tcps_mptcp_triggered_cell++;
	}
}

/*
 * Destroy an MPTCP session.
 */
static void
mptcp_session_destroy(struct mptses *mpte)
{
	struct mptcb *mp_tp = mpte->mpte_mptcb;

	VERIFY(mp_tp != NULL);
	VERIFY(TAILQ_EMPTY(&mpte->mpte_subflows) && mpte->mpte_numflows == 0);

	mptcpstats_session_wrapup(mpte);
	mptcp_unset_cellicon(mpte, NULL, mpte->mpte_cellicon_increments);
	mptcp_flush_sopts(mpte);

	if (mpte->mpte_itfinfo_size > MPTE_ITFINFO_SIZE) {
		_FREE(mpte->mpte_itfinfo, M_TEMP);
	}
	mpte->mpte_itfinfo = NULL;

	mptcp_freeq(mp_tp);
	m_freem_list(mpte->mpte_reinjectq);

	os_log(mptcp_log_handle, "%s - %lx: Destroying session\n",
	    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));
}

boolean_t
mptcp_ok_to_create_subflows(struct mptcb *mp_tp)
{
	return mp_tp->mpt_state >= MPTCPS_ESTABLISHED &&
	       mp_tp->mpt_state < MPTCPS_FIN_WAIT_1 &&
	       !(mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP);
}

static int
mptcp_synthesize_nat64(struct in6_addr *addr, uint32_t len,
    const struct in_addr *addrv4)
{
	static const struct in6_addr well_known_prefix = {
		.__u6_addr.__u6_addr8 = {0x00, 0x64, 0xff, 0x9b, 0x00, 0x00,
			                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			                 0x00, 0x00, 0x00, 0x00},
	};
	const char *ptrv4 = (const char *)addrv4;
	char *ptr = (char *)addr;

	if (IN_ZERONET(ntohl(addrv4->s_addr)) || // 0.0.0.0/8 Source hosts on local network
	    IN_LOOPBACK(ntohl(addrv4->s_addr)) || // 127.0.0.0/8 Loopback
	    IN_LINKLOCAL(ntohl(addrv4->s_addr)) || // 169.254.0.0/16 Link Local
	    IN_DS_LITE(ntohl(addrv4->s_addr)) || // 192.0.0.0/29 DS-Lite
	    IN_6TO4_RELAY_ANYCAST(ntohl(addrv4->s_addr)) || // 192.88.99.0/24 6to4 Relay Anycast
	    IN_MULTICAST(ntohl(addrv4->s_addr)) || // 224.0.0.0/4 Multicast
	    INADDR_BROADCAST == addrv4->s_addr) { // 255.255.255.255/32 Limited Broadcast
		return -1;
	}

	/* Check for the well-known prefix */
	if (len == NAT64_PREFIX_LEN_96 &&
	    IN6_ARE_ADDR_EQUAL(addr, &well_known_prefix)) {
		if (IN_PRIVATE(ntohl(addrv4->s_addr)) || // 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16 Private-Use
		    IN_SHARED_ADDRESS_SPACE(ntohl(addrv4->s_addr))) { // 100.64.0.0/10 Shared Address Space
			return -1;
		}
	}

	switch (len) {
	case NAT64_PREFIX_LEN_96:
		memcpy(ptr + 12, ptrv4, 4);
		break;
	case NAT64_PREFIX_LEN_64:
		memcpy(ptr + 9, ptrv4, 4);
		break;
	case NAT64_PREFIX_LEN_56:
		memcpy(ptr + 7, ptrv4, 1);
		memcpy(ptr + 9, ptrv4 + 1, 3);
		break;
	case NAT64_PREFIX_LEN_48:
		memcpy(ptr + 6, ptrv4, 2);
		memcpy(ptr + 9, ptrv4 + 2, 2);
		break;
	case NAT64_PREFIX_LEN_40:
		memcpy(ptr + 5, ptrv4, 3);
		memcpy(ptr + 9, ptrv4 + 3, 1);
		break;
	case NAT64_PREFIX_LEN_32:
		memcpy(ptr + 4, ptrv4, 4);
		break;
	default:
		panic("NAT64-prefix len is wrong: %u\n", len);
	}

	return 0;
}

static void
mptcp_trigger_cell_bringup(struct mptses *mpte)
{
	struct socket *mp_so = mptetoso(mpte);

	if (!uuid_is_null(mpsotomppcb(mp_so)->necp_client_uuid)) {
		uuid_string_t uuidstr;
		int err;

		socket_unlock(mp_so, 0);
		err = necp_client_assert_bb_radio_manager(mpsotomppcb(mp_so)->necp_client_uuid,
		    TRUE);
		socket_lock(mp_so, 0);

		if (err == 0) {
			mpte->mpte_triggered_cell = 1;
		}

		uuid_unparse_upper(mpsotomppcb(mp_so)->necp_client_uuid, uuidstr);
		os_log_info(mptcp_log_handle, "%s - %lx: asked irat to bringup cell for uuid %s, err %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), uuidstr, err);
	} else {
		os_log_info(mptcp_log_handle, "%s - %lx: UUID is already null\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));
	}
}

static boolean_t
mptcp_subflow_disconnecting(struct mptsub *mpts)
{
	if (mpts->mpts_socket->so_state & SS_ISDISCONNECTED) {
		return true;
	}

	if (mpts->mpts_flags & (MPTSF_DISCONNECTING | MPTSF_DISCONNECTED | MPTSF_CLOSE_REQD)) {
		return true;
	}

	if (sototcpcb(mpts->mpts_socket)->t_state == TCPS_CLOSED) {
		return true;
	}

	return false;
}

/*
 * In Handover mode, only create cell subflow if
 * - Symptoms marked WiFi as weak:
 *   Here, if we are sending data, then we can check the RTO-state. That is a
 *   stronger signal of WiFi quality than the Symptoms indicator.
 *   If however we are not sending any data, the only thing we can do is guess
 *   and thus bring up Cell.
 *
 * - Symptoms marked WiFi as unknown:
 *   In this state we don't know what the situation is and thus remain
 *   conservative, only bringing up cell if there are retransmissions going on.
 */
static boolean_t
mptcp_handover_use_cellular(struct mptses *mpte, struct tcpcb *tp)
{
	int unusable_state = mptcp_is_wifi_unusable_for_session(mpte);

	if (unusable_state == 0) {
		/* WiFi is good - don't use cell */
		return false;
	}

	if (unusable_state == -1) {
		/*
		 * We are in unknown state, only use Cell if we have confirmed
		 * that WiFi is bad.
		 */
		if (mptetoso(mpte)->so_snd.sb_cc != 0 && tp->t_rxtshift >= mptcp_fail_thresh * 2) {
			return true;
		} else {
			return false;
		}
	}

	if (unusable_state == 1) {
		/*
		 * WiFi is confirmed to be bad from Symptoms-Framework.
		 * If we are sending data, check the RTOs.
		 * Otherwise, be pessimistic and use Cell.
		 */
		if (mptetoso(mpte)->so_snd.sb_cc != 0) {
			if (tp->t_rxtshift >= mptcp_fail_thresh * 2) {
				return true;
			} else {
				return false;
			}
		} else {
			return true;
		}
	}

	return false;
}

void
mptcp_check_subflows_and_add(struct mptses *mpte)
{
	struct mptcb *mp_tp = mpte->mpte_mptcb;
	boolean_t cellular_viable = FALSE;
	boolean_t want_cellular = TRUE;
	uint32_t i;

	if (!mptcp_ok_to_create_subflows(mp_tp)) {
		os_log_debug(mptcp_log_handle, "%s - %lx: not a good time for subflows, state %u flags %#x",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mp_tp->mpt_state, mp_tp->mpt_flags);
		return;
	}

	/* Just to see if we have an IP-address available */
	if (mptcp_get_session_dst(mpte, false, false) == NULL) {
		return;
	}

	for (i = 0; i < mpte->mpte_itfinfo_size; i++) {
		boolean_t need_to_ask_symptoms = FALSE, found = FALSE;
		struct mpt_itf_info *info;
		struct sockaddr_in6 nat64pre;
		struct sockaddr *dst;
		struct mptsub *mpts;
		struct ifnet *ifp;
		uint32_t ifindex;

		info = &mpte->mpte_itfinfo[i];

		ifindex = info->ifindex;
		if (ifindex == IFSCOPE_NONE) {
			continue;
		}

		os_log(mptcp_log_handle, "%s - %lx: itf %u no support %u hasv4 %u has v6 %u hasnat64 %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), info->ifindex, info->no_mptcp_support,
		    info->has_v4_conn, info->has_v6_conn, info->has_nat64_conn);

		if (info->no_mptcp_support) {
			continue;
		}

		ifnet_head_lock_shared();
		ifp = ifindex2ifnet[ifindex];
		ifnet_head_done();

		if (ifp == NULL) {
			continue;
		}

		if (IFNET_IS_CELLULAR(ifp)) {
			cellular_viable = TRUE;

			if (mpte->mpte_svctype == MPTCP_SVCTYPE_HANDOVER ||
			    mpte->mpte_svctype == MPTCP_SVCTYPE_PURE_HANDOVER) {
				if (!mptcp_is_wifi_unusable_for_session(mpte)) {
					continue;
				}
			}
		}

		TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
			const struct ifnet *subifp = sotoinpcb(mpts->mpts_socket)->inp_last_outifp;
			struct tcpcb *tp = sototcpcb(mpts->mpts_socket);

			if (subifp == NULL) {
				continue;
			}

			/*
			 * If there is at least one functioning subflow on WiFi
			 * and we are checking for the cell interface, then
			 * we always need to ask symptoms for permission as
			 * cell is triggered even if WiFi is available.
			 */
			if (!IFNET_IS_CELLULAR(subifp) &&
			    !mptcp_subflow_disconnecting(mpts) &&
			    IFNET_IS_CELLULAR(ifp)) {
				need_to_ask_symptoms = TRUE;
			}

			if (mpte->mpte_svctype == MPTCP_SVCTYPE_HANDOVER || mpte->mpte_svctype == MPTCP_SVCTYPE_PURE_HANDOVER) {
				os_log(mptcp_log_handle,
				    "%s - %lx: %s: cell %u wifi-state %d flags %#x rxt %u first-party %u sb_cc %u ifindex %u this %u rtt %u rttvar %u rto %u\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
				    mpte->mpte_svctype == MPTCP_SVCTYPE_HANDOVER ? "handover" : "pure-handover",
				    IFNET_IS_CELLULAR(subifp),
				    mptcp_is_wifi_unusable_for_session(mpte),
				    mpts->mpts_flags,
				    tp->t_rxtshift,
				    !!(mpte->mpte_flags & MPTE_FIRSTPARTY),
				    mptetoso(mpte)->so_snd.sb_cc,
				    ifindex, subifp->if_index,
				    tp->t_srtt >> TCP_RTT_SHIFT,
				    tp->t_rttvar >> TCP_RTTVAR_SHIFT,
				    tp->t_rxtcur);

				if (!IFNET_IS_CELLULAR(subifp) &&
				    !mptcp_subflow_disconnecting(mpts) &&
				    (mpts->mpts_flags & MPTSF_CONNECTED) &&
				    !mptcp_handover_use_cellular(mpte, tp)) {
					found = TRUE;

					/* We found a proper subflow on WiFi - no need for cell */
					want_cellular = FALSE;
					break;
				}
			} else if (mpte->mpte_svctype == MPTCP_SVCTYPE_TARGET_BASED) {
				uint64_t time_now = mach_continuous_time();

				os_log(mptcp_log_handle,
				    "%s - %lx: target-based: %llu now %llu unusable? %d cell %u sostat %#x mpts_flags %#x tcp-state %u\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpte->mpte_time_target,
				    time_now, mptcp_is_wifi_unusable_for_session(mpte),
				    IFNET_IS_CELLULAR(subifp), mpts->mpts_socket->so_state,
				    mpts->mpts_flags, sototcpcb(mpts->mpts_socket)->t_state);

				if (!IFNET_IS_CELLULAR(subifp) &&
				    !mptcp_subflow_disconnecting(mpts) &&
				    (mpte->mpte_time_target == 0 ||
				    (int64_t)(mpte->mpte_time_target - time_now) > 0 ||
				    !mptcp_is_wifi_unusable_for_session(mpte))) {
					found = TRUE;

					want_cellular = FALSE;
					break;
				}
			}

			if (subifp->if_index == ifindex &&
			    !mptcp_subflow_disconnecting(mpts)) {
				/*
				 * We found a subflow on this interface.
				 * No need to create a new one.
				 */
				found = TRUE;
				break;
			}
		}

		if (found) {
			continue;
		}

		if (need_to_ask_symptoms &&
		    !(mpte->mpte_flags & MPTE_FIRSTPARTY) &&
		    !(mpte->mpte_flags & MPTE_ACCESS_GRANTED) &&
		    mptcp_developer_mode == 0) {
			mptcp_ask_symptoms(mpte);
			return;
		}

		dst = mptcp_get_session_dst(mpte, info->has_v6_conn, info->has_v4_conn);

		if (dst->sa_family == AF_INET &&
		    !info->has_v4_conn && info->has_nat64_conn) {
			struct ipv6_prefix nat64prefixes[NAT64_MAX_NUM_PREFIXES];
			int error, j;

			bzero(&nat64pre, sizeof(struct sockaddr_in6));

			error = ifnet_get_nat64prefix(ifp, nat64prefixes);
			if (error) {
				os_log_error(mptcp_log_handle, "%s - %lx: no NAT64-prefix on itf %s, error %d\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), ifp->if_name, error);
				continue;
			}

			for (j = 0; j < NAT64_MAX_NUM_PREFIXES; j++) {
				if (nat64prefixes[j].prefix_len != 0) {
					break;
				}
			}

			VERIFY(j < NAT64_MAX_NUM_PREFIXES);

			error = mptcp_synthesize_nat64(&nat64prefixes[j].ipv6_prefix,
			    nat64prefixes[j].prefix_len,
			    &((struct sockaddr_in *)(void *)dst)->sin_addr);
			if (error != 0) {
				os_log_error(mptcp_log_handle, "%s - %lx: cannot synthesize this addr\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));
				continue;
			}

			memcpy(&nat64pre.sin6_addr,
			    &nat64prefixes[j].ipv6_prefix,
			    sizeof(nat64pre.sin6_addr));
			nat64pre.sin6_len = sizeof(struct sockaddr_in6);
			nat64pre.sin6_family = AF_INET6;
			nat64pre.sin6_port = ((struct sockaddr_in *)(void *)dst)->sin_port;
			nat64pre.sin6_flowinfo = 0;
			nat64pre.sin6_scope_id = 0;

			dst = (struct sockaddr *)&nat64pre;
		}

		if (dst->sa_family == AF_INET && !info->has_v4_conn) {
			continue;
		}
		if (dst->sa_family == AF_INET6 && !info->has_v6_conn) {
			continue;
		}

		mptcp_subflow_add(mpte, NULL, dst, ifindex, NULL);
	}

	if (!cellular_viable && want_cellular) {
		/* Trigger Cell Bringup */
		mptcp_trigger_cell_bringup(mpte);
	}
}

static void
mptcp_remove_cell_subflows(struct mptses *mpte)
{
	struct mptsub *mpts, *tmpts;

	TAILQ_FOREACH_SAFE(mpts, &mpte->mpte_subflows, mpts_entry, tmpts) {
		const struct ifnet *ifp = sotoinpcb(mpts->mpts_socket)->inp_last_outifp;

		if (ifp == NULL || !IFNET_IS_CELLULAR(ifp)) {
			continue;
		}

		os_log(mptcp_log_handle, "%s - %lx: removing cell subflow\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));

		soevent(mpts->mpts_socket, SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);
	}

	return;
}

static void
mptcp_remove_wifi_subflows(struct mptses *mpte)
{
	struct mptsub *mpts, *tmpts;

	TAILQ_FOREACH_SAFE(mpts, &mpte->mpte_subflows, mpts_entry, tmpts) {
		const struct ifnet *ifp = sotoinpcb(mpts->mpts_socket)->inp_last_outifp;

		if (ifp == NULL || IFNET_IS_CELLULAR(ifp)) {
			continue;
		}

		os_log(mptcp_log_handle, "%s - %lx: removing wifi subflow\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));

		soevent(mpts->mpts_socket, SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);
	}

	return;
}

static void
mptcp_pure_handover_subflows_remove(struct mptses *mpte)
{
	int wifi_unusable = mptcp_is_wifi_unusable_for_session(mpte);
	boolean_t found_working_wifi_subflow = false;
	boolean_t found_working_cell_subflow = false;

	struct mptsub *mpts;

	/*
	 * Look for a subflow that is on a non-cellular interface in connected
	 * state.
	 *
	 * In that case, remove all cellular subflows.
	 *
	 * If however there is no connected subflow
	 */
	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		const struct ifnet *ifp = sotoinpcb(mpts->mpts_socket)->inp_last_outifp;
		struct socket *so;
		struct tcpcb *tp;

		if (ifp == NULL) {
			continue;
		}

		so = mpts->mpts_socket;
		tp = sototcpcb(so);

		if (!(mpts->mpts_flags & MPTSF_CONNECTED) ||
		    tp->t_state != TCPS_ESTABLISHED ||
		    mptcp_subflow_disconnecting(mpts)) {
			continue;
		}

		if (IFNET_IS_CELLULAR(ifp)) {
			found_working_cell_subflow = true;
		} else {
			os_log_debug(mptcp_log_handle, "%s - %lx: rxt %u sb_cc %u unusable %d\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), tp->t_rxtshift, mptetoso(mpte)->so_snd.sb_cc, wifi_unusable);
			if (!mptcp_handover_use_cellular(mpte, tp)) {
				found_working_wifi_subflow = true;
			}
		}
	}

	/*
	 * Couldn't find a working subflow, let's not remove those on a cellular
	 * interface.
	 */
	os_log_debug(mptcp_log_handle, "%s - %lx: Found Wi-Fi: %u Found Cellular %u",
	    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
	    found_working_wifi_subflow, found_working_cell_subflow);
	if (!found_working_wifi_subflow && wifi_unusable) {
		if (found_working_cell_subflow) {
			mptcp_remove_wifi_subflows(mpte);
		}
		return;
	}

	mptcp_remove_cell_subflows(mpte);
}

static void
mptcp_handover_subflows_remove(struct mptses *mpte)
{
	int wifi_unusable = mptcp_is_wifi_unusable_for_session(mpte);
	boolean_t found_working_subflow = false;
	struct mptsub *mpts;

	/*
	 * Look for a subflow that is on a non-cellular interface
	 * and actually works (aka, no retransmission timeout).
	 */
	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		const struct ifnet *ifp = sotoinpcb(mpts->mpts_socket)->inp_last_outifp;
		struct socket *so;
		struct tcpcb *tp;

		if (ifp == NULL || IFNET_IS_CELLULAR(ifp)) {
			continue;
		}

		so = mpts->mpts_socket;
		tp = sototcpcb(so);

		if (!(mpts->mpts_flags & MPTSF_CONNECTED) ||
		    tp->t_state != TCPS_ESTABLISHED) {
			continue;
		}

		os_log_debug(mptcp_log_handle, "%s - %lx: rxt %u sb_cc %u unusable %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), tp->t_rxtshift, mptetoso(mpte)->so_snd.sb_cc, wifi_unusable);

		if (!mptcp_handover_use_cellular(mpte, tp)) {
			found_working_subflow = true;
			break;
		}
	}

	/*
	 * Couldn't find a working subflow, let's not remove those on a cellular
	 * interface.
	 */
	if (!found_working_subflow) {
		return;
	}

	mptcp_remove_cell_subflows(mpte);
}

static void
mptcp_targetbased_subflows_remove(struct mptses *mpte)
{
	uint64_t time_now = mach_continuous_time();
	struct mptsub *mpts;

	if (mpte->mpte_time_target != 0 &&
	    (int64_t)(mpte->mpte_time_target - time_now) <= 0 &&
	    mptcp_is_wifi_unusable_for_session(mpte)) {
		/* WiFi is bad and we are below the target - don't remove any subflows */
		return;
	}

	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		const struct ifnet *ifp = sotoinpcb(mpts->mpts_socket)->inp_last_outifp;

		if (ifp == NULL || IFNET_IS_CELLULAR(ifp)) {
			continue;
		}

		/* We have a functioning subflow on WiFi. No need for cell! */
		if (mpts->mpts_flags & MPTSF_CONNECTED &&
		    !mptcp_subflow_disconnecting(mpts)) {
			mptcp_remove_cell_subflows(mpte);
			break;
		}
	}
}

/*
 * Based on the MPTCP Service-type and the state of the subflows, we
 * will destroy subflows here.
 */
void
mptcp_check_subflows_and_remove(struct mptses *mpte)
{
	if (!mptcp_ok_to_create_subflows(mpte->mpte_mptcb)) {
		return;
	}

	socket_lock_assert_owned(mptetoso(mpte));

	if (mpte->mpte_svctype == MPTCP_SVCTYPE_PURE_HANDOVER) {
		mptcp_pure_handover_subflows_remove(mpte);
	}

	if (mpte->mpte_svctype == MPTCP_SVCTYPE_HANDOVER) {
		mptcp_handover_subflows_remove(mpte);
	}

	if (mpte->mpte_svctype == MPTCP_SVCTYPE_TARGET_BASED) {
		mptcp_targetbased_subflows_remove(mpte);
	}
}

static void
mptcp_remove_subflows(struct mptses *mpte)
{
	struct mptsub *mpts, *tmpts;

	if (!mptcp_ok_to_create_subflows(mpte->mpte_mptcb)) {
		return;
	}

	TAILQ_FOREACH_SAFE(mpts, &mpte->mpte_subflows, mpts_entry, tmpts) {
		const struct ifnet *ifp = sotoinpcb(mpts->mpts_socket)->inp_last_outifp;
		boolean_t found = false;
		uint32_t ifindex;
		uint32_t i;

		if (mpts->mpts_flags & MPTSF_CLOSE_REQD) {
			mpts->mpts_flags &= ~MPTSF_CLOSE_REQD;

			os_log(mptcp_log_handle, "%s - %lx: itf %u close_reqd last itf %d\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpts->mpts_ifscope,
			    ifp ? ifp->if_index : -1);
			soevent(mpts->mpts_socket,
			    SO_FILT_HINT_LOCKED | SO_FILT_HINT_NOSRCADDR);

			continue;
		}

		if (ifp == NULL && mpts->mpts_ifscope == IFSCOPE_NONE) {
			continue;
		}

		if (ifp) {
			ifindex = ifp->if_index;
		} else {
			ifindex = mpts->mpts_ifscope;
		}

		for (i = 0; i < mpte->mpte_itfinfo_size; i++) {
			if (mpte->mpte_itfinfo[i].ifindex == IFSCOPE_NONE) {
				continue;
			}

			if (mpte->mpte_itfinfo[i].ifindex == ifindex) {
				if (mpts->mpts_dst.sa_family == AF_INET6 &&
				    (mpte->mpte_itfinfo[i].has_v6_conn || mpte->mpte_itfinfo[i].has_nat64_conn)) {
					found = true;
					break;
				}

				if (mpts->mpts_dst.sa_family == AF_INET &&
				    mpte->mpte_itfinfo[i].has_v4_conn) {
					found = true;
					break;
				}
			}
		}

		if (!found) {
			os_log(mptcp_log_handle, "%s - %lx: itf %u killing %#x\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    ifindex, mpts->mpts_flags);

			soevent(mpts->mpts_socket,
			    SO_FILT_HINT_LOCKED | SO_FILT_HINT_NOSRCADDR);
		}
	}
}

static void
mptcp_create_subflows(__unused void *arg)
{
	struct mppcb *mpp;

	/*
	 * Start with clearing, because we might be processing connections
	 * while a new event comes in.
	 */
	if (OSTestAndClear(0x01, &mptcp_create_subflows_scheduled)) {
		os_log_error(mptcp_log_handle, "%s: bit was already cleared!\n", __func__);
	}

	/* Iterate over all MPTCP connections */

	lck_mtx_lock(&mtcbinfo.mppi_lock);

	TAILQ_FOREACH(mpp, &mtcbinfo.mppi_pcbs, mpp_entry) {
		struct socket *mp_so = mpp->mpp_socket;
		struct mptses *mpte = mpp->mpp_pcbe;

		if (!(mpp->mpp_flags & MPP_CREATE_SUBFLOWS)) {
			continue;
		}

		socket_lock(mp_so, 1);
		VERIFY(mp_so->so_usecount > 0);

		mpp->mpp_flags &= ~MPP_CREATE_SUBFLOWS;

		mptcp_check_subflows_and_add(mpte);
		mptcp_remove_subflows(mpte);

		mp_so->so_usecount--; /* See mptcp_sched_create_subflows */
		socket_unlock(mp_so, 1);
	}

	lck_mtx_unlock(&mtcbinfo.mppi_lock);
}

/*
 * We need this because we are coming from an NECP-event. This event gets posted
 * while holding NECP-locks. The creation of the subflow however leads us back
 * into NECP (e.g., to add the necp_cb and also from tcp_connect).
 * So, we would deadlock there as we already hold the NECP-lock.
 *
 * So, let's schedule this separately. It also gives NECP the chance to make
 * progress, without having to wait for MPTCP to finish its subflow creation.
 */
void
mptcp_sched_create_subflows(struct mptses *mpte)
{
	struct mppcb *mpp = mpte->mpte_mppcb;
	struct mptcb *mp_tp = mpte->mpte_mptcb;
	struct socket *mp_so = mpp->mpp_socket;

	if (!mptcp_ok_to_create_subflows(mp_tp)) {
		os_log_debug(mptcp_log_handle, "%s - %lx: not a good time for subflows, state %u flags %#x",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mp_tp->mpt_state, mp_tp->mpt_flags);
		return;
	}

	if (!(mpp->mpp_flags & MPP_CREATE_SUBFLOWS)) {
		mp_so->so_usecount++; /* To prevent it from being free'd in-between */
		mpp->mpp_flags |= MPP_CREATE_SUBFLOWS;
	}

	if (OSTestAndSet(0x01, &mptcp_create_subflows_scheduled)) {
		return;
	}

	/* Do the call in 100ms to allow NECP to schedule it on all sockets */
	timeout(mptcp_create_subflows, NULL, hz / 10);
}

/*
 * Allocate an MPTCP socket option structure.
 */
struct mptopt *
mptcp_sopt_alloc(zalloc_flags_t how)
{
	return zalloc_flags(mptopt_zone, how | Z_ZERO);
}

/*
 * Free an MPTCP socket option structure.
 */
void
mptcp_sopt_free(struct mptopt *mpo)
{
	VERIFY(!(mpo->mpo_flags & MPOF_ATTACHED));

	zfree(mptopt_zone, mpo);
}

/*
 * Add a socket option to the MPTCP socket option list.
 */
void
mptcp_sopt_insert(struct mptses *mpte, struct mptopt *mpo)
{
	socket_lock_assert_owned(mptetoso(mpte));
	mpo->mpo_flags |= MPOF_ATTACHED;
	TAILQ_INSERT_TAIL(&mpte->mpte_sopts, mpo, mpo_entry);
}

/*
 * Remove a socket option from the MPTCP socket option list.
 */
void
mptcp_sopt_remove(struct mptses *mpte, struct mptopt *mpo)
{
	socket_lock_assert_owned(mptetoso(mpte));
	VERIFY(mpo->mpo_flags & MPOF_ATTACHED);
	mpo->mpo_flags &= ~MPOF_ATTACHED;
	TAILQ_REMOVE(&mpte->mpte_sopts, mpo, mpo_entry);
}

/*
 * Search for an existing <sopt_level,sopt_name> socket option.
 */
struct mptopt *
mptcp_sopt_find(struct mptses *mpte, struct sockopt *sopt)
{
	struct mptopt *mpo;

	socket_lock_assert_owned(mptetoso(mpte));

	TAILQ_FOREACH(mpo, &mpte->mpte_sopts, mpo_entry) {
		if (mpo->mpo_level == sopt->sopt_level &&
		    mpo->mpo_name == sopt->sopt_name) {
			break;
		}
	}
	return mpo;
}

/*
 * Allocate a MPTCP subflow structure.
 */
static struct mptsub *
mptcp_subflow_alloc(void)
{
	return zalloc_flags(mptsub_zone, Z_WAITOK | Z_ZERO);
}

/*
 * Deallocate a subflow structure, called when all of the references held
 * on it have been released.  This implies that the subflow has been deleted.
 */
static void
mptcp_subflow_free(struct mptsub *mpts)
{
	VERIFY(mpts->mpts_refcnt == 0);
	VERIFY(!(mpts->mpts_flags & MPTSF_ATTACHED));
	VERIFY(mpts->mpts_mpte == NULL);
	VERIFY(mpts->mpts_socket == NULL);

	if (mpts->mpts_src != NULL) {
		FREE(mpts->mpts_src, M_SONAME);
		mpts->mpts_src = NULL;
	}

	zfree(mptsub_zone, mpts);
}

static void
mptcp_subflow_addref(struct mptsub *mpts)
{
	if (++mpts->mpts_refcnt == 0) {
		panic("%s: mpts %p wraparound refcnt\n", __func__, mpts);
	}
	/* NOTREACHED */
}

static void
mptcp_subflow_remref(struct mptsub *mpts)
{
	if (mpts->mpts_refcnt == 0) {
		panic("%s: mpts %p negative refcnt\n", __func__, mpts);
		/* NOTREACHED */
	}
	if (--mpts->mpts_refcnt > 0) {
		return;
	}

	/* callee will unlock and destroy lock */
	mptcp_subflow_free(mpts);
}

static void
mptcp_subflow_attach(struct mptses *mpte, struct mptsub *mpts, struct socket *so)
{
	struct socket *mp_so = mpte->mpte_mppcb->mpp_socket;
	struct tcpcb *tp = sototcpcb(so);

	/*
	 * From this moment on, the subflow is linked to the MPTCP-connection.
	 * Locking,... happens now at the MPTCP-layer
	 */
	tp->t_mptcb = mpte->mpte_mptcb;
	so->so_flags |= SOF_MP_SUBFLOW;
	mp_so->so_usecount++;

	/*
	 * Insert the subflow into the list, and associate the MPTCP PCB
	 * as well as the the subflow socket.  From this point on, removing
	 * the subflow needs to be done via mptcp_subflow_del().
	 */
	TAILQ_INSERT_TAIL(&mpte->mpte_subflows, mpts, mpts_entry);
	mpte->mpte_numflows++;

	atomic_bitset_32(&mpts->mpts_flags, MPTSF_ATTACHED);
	mpts->mpts_mpte = mpte;
	mpts->mpts_socket = so;
	tp->t_mpsub = mpts;
	mptcp_subflow_addref(mpts);     /* for being in MPTCP subflow list */
	mptcp_subflow_addref(mpts);     /* for subflow socket */
}

static void
mptcp_subflow_necp_cb(void *handle, __unused int action,
    __unused uint32_t interface_index,
    uint32_t necp_flags, bool *viable)
{
	boolean_t low_power = !!(necp_flags & NECP_CLIENT_RESULT_FLAG_INTERFACE_LOW_POWER);
	struct inpcb *inp = (struct inpcb *)handle;
	struct socket *so = inp->inp_socket;
	struct mptsub *mpts;
	struct mptses *mpte;

	if (low_power) {
		action = NECP_CLIENT_CBACTION_NONVIABLE;
	}

	if (action != NECP_CLIENT_CBACTION_NONVIABLE) {
		return;
	}

	/*
	 * The socket is being garbage-collected. There is nothing to be done
	 * here.
	 */
	if (in_pcb_checkstate(inp, WNT_ACQUIRE, 0) == WNT_STOPUSING) {
		return;
	}

	socket_lock(so, 1);

	/* Check again after we acquired the lock. */
	if (in_pcb_checkstate(inp, WNT_RELEASE, 1) == WNT_STOPUSING) {
		goto out;
	}

	mpte = tptomptp(sototcpcb(so))->mpt_mpte;
	mpts = sototcpcb(so)->t_mpsub;

	os_log_debug(mptcp_log_handle, "%s - %lx: Subflow on itf %u became non-viable, power %u",
	    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpts->mpts_ifscope, low_power);

	mpts->mpts_flags |= MPTSF_CLOSE_REQD;

	mptcp_sched_create_subflows(mpte);

	if ((mpte->mpte_svctype == MPTCP_SVCTYPE_HANDOVER ||
	    mpte->mpte_svctype == MPTCP_SVCTYPE_PURE_HANDOVER ||
	    mpte->mpte_svctype == MPTCP_SVCTYPE_TARGET_BASED) &&
	    viable != NULL) {
		*viable = 1;
	}

out:
	socket_unlock(so, 1);
}

/*
 * Create an MPTCP subflow socket.
 */
static int
mptcp_subflow_socreate(struct mptses *mpte, struct mptsub *mpts, int dom,
    struct socket **so)
{
	lck_mtx_t *subflow_mtx;
	struct mptopt smpo, *mpo, *tmpo;
	struct proc *p;
	struct socket *mp_so;
	int error;

	*so = NULL;

	mp_so = mptetoso(mpte);

	p = proc_find(mp_so->last_pid);
	if (p == PROC_NULL) {
		os_log_error(mptcp_log_handle, "%s - %lx: Couldn't find proc for pid %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mp_so->last_pid);

		mptcp_subflow_free(mpts);
		return ESRCH;
	}

	/*
	 * Create the subflow socket (multipath subflow, non-blocking.)
	 *
	 * This will cause SOF_MP_SUBFLOW socket flag to be set on the subflow
	 * socket; it will be cleared when the socket is peeled off or closed.
	 * It also indicates to the underlying TCP to handle MPTCP options.
	 * A multipath subflow socket implies SS_NOFDREF state.
	 */

	/*
	 * Unlock, because tcp_usr_attach ends up in in_pcballoc, which takes
	 * the ipi-lock. We cannot hold the socket-lock at that point.
	 */
	socket_unlock(mp_so, 0);
	error = socreate_internal(dom, so, SOCK_STREAM, IPPROTO_TCP, p,
	    SOCF_MPTCP, PROC_NULL);
	socket_lock(mp_so, 0);
	if (error) {
		os_log_error(mptcp_log_handle, "%s - %lx: unable to create subflow socket error %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), error);

		proc_rele(p);

		mptcp_subflow_free(mpts);
		return error;
	}

	/*
	 * We need to protect the setting of SOF_MP_SUBFLOW with a lock, because
	 * this marks the moment of lock-switch from the TCP-lock to the MPTCP-lock.
	 * Which is why we also need to get the lock with pr_getlock, as after
	 * setting the flag, socket_unlock will work on the MPTCP-level lock.
	 */
	subflow_mtx = ((*so)->so_proto->pr_getlock)(*so, 0);
	lck_mtx_lock(subflow_mtx);

	/*
	 * Must be the first thing we do, to make sure all pointers for this
	 * subflow are set.
	 */
	mptcp_subflow_attach(mpte, mpts, *so);

	/*
	 * A multipath subflow socket is used internally in the kernel,
	 * therefore it does not have a file desciptor associated by
	 * default.
	 */
	(*so)->so_state |= SS_NOFDREF;

	lck_mtx_unlock(subflow_mtx);

	/* prevent the socket buffers from being compressed */
	(*so)->so_rcv.sb_flags |= SB_NOCOMPRESS;
	(*so)->so_snd.sb_flags |= SB_NOCOMPRESS;

	/* Inherit preconnect and TFO data flags */
	if (mp_so->so_flags1 & SOF1_PRECONNECT_DATA) {
		(*so)->so_flags1 |= SOF1_PRECONNECT_DATA;
	}
	if (mp_so->so_flags1 & SOF1_DATA_IDEMPOTENT) {
		(*so)->so_flags1 |= SOF1_DATA_IDEMPOTENT;
	}
	if (mp_so->so_flags1 & SOF1_DATA_AUTHENTICATED) {
		(*so)->so_flags1 |= SOF1_DATA_AUTHENTICATED;
	}

	/* Inherit uuid and create the related flow. */
	if (!uuid_is_null(mpsotomppcb(mp_so)->necp_client_uuid)) {
		struct mptcb *mp_tp = mpte->mpte_mptcb;

		sotoinpcb(*so)->necp_cb = mptcp_subflow_necp_cb;

		/*
		 * A note on the unlock: With MPTCP, we do multiple times a
		 * necp_client_register_socket_flow. This is problematic,
		 * because now the lock-ordering guarantee (first necp-locks,
		 * then socket-locks) is no more respected. So, we need to
		 * unlock here.
		 */
		socket_unlock(mp_so, 0);
		error = necp_client_register_socket_flow(mp_so->last_pid,
		    mpsotomppcb(mp_so)->necp_client_uuid, sotoinpcb(*so));
		socket_lock(mp_so, 0);

		if (error) {
			os_log_error(mptcp_log_handle, "%s - %lx: necp_client_register_socket_flow failed with error %d\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), error);

			goto out_err;
		}

		/* Possible state-change during the unlock above */
		if (mp_tp->mpt_state >= MPTCPS_TIME_WAIT ||
		    (mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP)) {
			os_log_error(mptcp_log_handle, "%s - %lx: state changed during unlock: %u flags %#x\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    mp_tp->mpt_state, mp_tp->mpt_flags);

			error = EINVAL;
			goto out_err;
		}

		uuid_copy(sotoinpcb(*so)->necp_client_uuid, mpsotomppcb(mp_so)->necp_client_uuid);
	}

	/* Needs to happen prior to the delegation! */
	(*so)->last_pid = mp_so->last_pid;

	if (mp_so->so_flags & SOF_DELEGATED) {
		if (mpte->mpte_epid) {
			error = so_set_effective_pid(*so, mpte->mpte_epid, p, false);
			if (error) {
				os_log_error(mptcp_log_handle, "%s - %lx: so_set_effective_pid failed with error %d\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), error);
				goto out_err;
			}
		}
		if (!uuid_is_null(mpte->mpte_euuid)) {
			error = so_set_effective_uuid(*so, mpte->mpte_euuid, p, false);
			if (error) {
				os_log_error(mptcp_log_handle, "%s - %lx: so_set_effective_uuid failed with error %d\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), error);
				goto out_err;
			}
		}
	}

	/* inherit the other socket options */
	bzero(&smpo, sizeof(smpo));
	smpo.mpo_flags |= MPOF_SUBFLOW_OK;
	smpo.mpo_level = SOL_SOCKET;
	smpo.mpo_intval = 1;

	/* disable SIGPIPE */
	smpo.mpo_name = SO_NOSIGPIPE;
	if ((error = mptcp_subflow_sosetopt(mpte, mpts, &smpo)) != 0) {
		goto out_err;
	}

	/* find out if the subflow's source address goes away */
	smpo.mpo_name = SO_NOADDRERR;
	if ((error = mptcp_subflow_sosetopt(mpte, mpts, &smpo)) != 0) {
		goto out_err;
	}

	if (mpte->mpte_mptcb->mpt_state >= MPTCPS_ESTABLISHED) {
		/*
		 * On secondary subflows we might need to set the cell-fallback
		 * flag (see conditions in mptcp_subflow_sosetopt).
		 */
		smpo.mpo_level = SOL_SOCKET;
		smpo.mpo_name = SO_MARK_CELLFALLBACK;
		smpo.mpo_intval = 1;
		if ((error = mptcp_subflow_sosetopt(mpte, mpts, &smpo)) != 0) {
			goto out_err;
		}
	}

	/* replay setsockopt(2) on the subflow sockets for eligible options */
	TAILQ_FOREACH_SAFE(mpo, &mpte->mpte_sopts, mpo_entry, tmpo) {
		int interim;

		if (!(mpo->mpo_flags & MPOF_SUBFLOW_OK)) {
			continue;
		}

		/*
		 * Skip those that are handled internally; these options
		 * should not have been recorded and marked with the
		 * MPOF_SUBFLOW_OK by mptcp_setopt(), but just in case.
		 */
		if (mpo->mpo_level == SOL_SOCKET &&
		    (mpo->mpo_name == SO_NOSIGPIPE ||
		    mpo->mpo_name == SO_NOADDRERR ||
		    mpo->mpo_name == SO_KEEPALIVE)) {
			continue;
		}

		interim = (mpo->mpo_flags & MPOF_INTERIM);
		if (mptcp_subflow_sosetopt(mpte, mpts, mpo) != 0 && interim) {
			os_log_error(mptcp_log_handle, "%s - %lx: sopt %s val %d interim record removed\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    mptcp_sopt2str(mpo->mpo_level, mpo->mpo_name),
			    mpo->mpo_intval);
			mptcp_sopt_remove(mpte, mpo);
			mptcp_sopt_free(mpo);
			continue;
		}
	}

	/*
	 * We need to receive everything that the subflow socket has,
	 * so use a customized socket receive function.  We will undo
	 * this when the socket is peeled off or closed.
	 */
	switch (dom) {
	case PF_INET:
		(*so)->so_proto = &mptcp_subflow_protosw;
		break;
	case PF_INET6:
		(*so)->so_proto = (struct protosw *)&mptcp_subflow_protosw6;
		break;
	default:
		VERIFY(0);
		/* NOTREACHED */
	}

	proc_rele(p);

	DTRACE_MPTCP3(subflow__create, struct mptses *, mpte,
	    int, dom, int, error);

	return 0;

out_err:
	mptcp_subflow_abort(mpts, error);

	proc_rele(p);

	return error;
}

/*
 * Close an MPTCP subflow socket.
 *
 * Note that this may be called on an embryonic subflow, and the only
 * thing that is guaranteed valid is the protocol-user request.
 */
static void
mptcp_subflow_soclose(struct mptsub *mpts)
{
	struct socket *so = mpts->mpts_socket;

	if (mpts->mpts_flags & MPTSF_CLOSED) {
		return;
	}

	VERIFY(so != NULL);
	VERIFY(so->so_flags & SOF_MP_SUBFLOW);
	VERIFY((so->so_state & (SS_NBIO | SS_NOFDREF)) == (SS_NBIO | SS_NOFDREF));

	DTRACE_MPTCP5(subflow__close, struct mptsub *, mpts,
	    struct socket *, so,
	    struct sockbuf *, &so->so_rcv,
	    struct sockbuf *, &so->so_snd,
	    struct mptses *, mpts->mpts_mpte);

	mpts->mpts_flags |= MPTSF_CLOSED;

	if (so->so_retaincnt == 0) {
		soclose_locked(so);

		return;
	} else {
		VERIFY(so->so_usecount > 0);
		so->so_usecount--;
	}

	return;
}

/*
 * Connect an MPTCP subflow socket.
 *
 * Note that in the pending connect case, the subflow socket may have been
 * bound to an interface and/or a source IP address which may no longer be
 * around by the time this routine is called; in that case the connect attempt
 * will most likely fail.
 */
static int
mptcp_subflow_soconnectx(struct mptses *mpte, struct mptsub *mpts)
{
	char dbuf[MAX_IPv6_STR_LEN];
	struct socket *mp_so, *so;
	struct mptcb *mp_tp;
	struct sockaddr *dst;
	struct proc *p;
	int af, error, dport;

	mp_so = mptetoso(mpte);
	mp_tp = mpte->mpte_mptcb;
	so = mpts->mpts_socket;
	af = mpts->mpts_dst.sa_family;
	dst = &mpts->mpts_dst;

	VERIFY((mpts->mpts_flags & (MPTSF_CONNECTING | MPTSF_CONNECTED)) == MPTSF_CONNECTING);
	VERIFY(mpts->mpts_socket != NULL);
	VERIFY(af == AF_INET || af == AF_INET6);

	if (af == AF_INET) {
		inet_ntop(af, &SIN(dst)->sin_addr.s_addr, dbuf, sizeof(dbuf));
		dport = ntohs(SIN(dst)->sin_port);
	} else {
		inet_ntop(af, &SIN6(dst)->sin6_addr, dbuf, sizeof(dbuf));
		dport = ntohs(SIN6(dst)->sin6_port);
	}

	os_log(mptcp_log_handle,
	    "%s - %lx: ifindex %u dst %s:%d pended %u\n", __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
	    mpts->mpts_ifscope, dbuf, dport, !!(mpts->mpts_flags & MPTSF_CONNECT_PENDING));

	p = proc_find(mp_so->last_pid);
	if (p == PROC_NULL) {
		os_log_error(mptcp_log_handle, "%s - %lx: Couldn't find proc for pid %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mp_so->last_pid);

		return ESRCH;
	}

	mpts->mpts_flags &= ~MPTSF_CONNECT_PENDING;

	mptcp_attach_to_subf(so, mpte->mpte_mptcb, mpte->mpte_addrid_last);

	/* connect the subflow socket */
	error = soconnectxlocked(so, mpts->mpts_src, &mpts->mpts_dst,
	    p, mpts->mpts_ifscope,
	    mpte->mpte_associd, NULL, 0, NULL, 0, NULL, NULL);

	mpts->mpts_iss = sototcpcb(so)->iss;

	/* See tcp_connect_complete */
	if (mp_tp->mpt_state < MPTCPS_ESTABLISHED &&
	    (mp_so->so_flags1 & SOF1_PRECONNECT_DATA)) {
		mp_tp->mpt_sndwnd = sototcpcb(so)->snd_wnd;
	}

	/* Allocate a unique address id per subflow */
	mpte->mpte_addrid_last++;
	if (mpte->mpte_addrid_last == 0) {
		mpte->mpte_addrid_last++;
	}

	proc_rele(p);

	DTRACE_MPTCP3(subflow__connect, struct mptses *, mpte,
	    struct mptsub *, mpts, int, error);
	if (error) {
		os_log_error(mptcp_log_handle, "%s - %lx: connectx failed with error %d ifscope %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), error, mpts->mpts_ifscope);
	}

	return error;
}

static int
mptcp_adj_rmap(struct socket *so, struct mbuf *m, int off, uint64_t dsn,
    uint32_t rseq, uint16_t dlen, uint8_t dfin)
{
	struct mptsub *mpts = sototcpcb(so)->t_mpsub;

	if (m_pktlen(m) == 0) {
		return 0;
	}

	if (!(m->m_flags & M_PKTHDR)) {
		return 0;
	}

	if (m->m_pkthdr.pkt_flags & PKTF_MPTCP) {
		if (off && (dsn != m->m_pkthdr.mp_dsn ||
		    rseq != m->m_pkthdr.mp_rseq ||
		    dlen != m->m_pkthdr.mp_rlen ||
		    dfin != !!(m->m_pkthdr.pkt_flags & PKTF_MPTCP_DFIN))) {
			os_log_error(mptcp_log_handle, "%s - %lx: Received incorrect second mapping: DSN: %u - %u , SSN: %u - %u, DLEN: %u - %u, DFIN: %u - %u\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpts->mpts_mpte),
			    (uint32_t)dsn, (uint32_t)m->m_pkthdr.mp_dsn,
			    rseq, m->m_pkthdr.mp_rseq,
			    dlen, m->m_pkthdr.mp_rlen,
			    dfin, !!(m->m_pkthdr.pkt_flags & PKTF_MPTCP_DFIN));

			soevent(mpts->mpts_socket, SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);
			return -1;
		}
	}

	/* If mbuf is beyond right edge of the mapping, we need to split */
	if (m_pktlen(m) > dlen - dfin - off) {
		struct mbuf *new = m_split(m, dlen - dfin - off, M_DONTWAIT);
		if (new == NULL) {
			os_log_error(mptcp_log_handle, "%s - %lx: m_split failed dlen %u dfin %u off %d pktlen %d, killing subflow %d",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpts->mpts_mpte),
			    dlen, dfin, off, m_pktlen(m),
			    mpts->mpts_connid);

			soevent(mpts->mpts_socket, SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);
			return -1;
		}

		m->m_next = new;
		sballoc(&so->so_rcv, new);
		/* Undo, as sballoc will add to it as well */
		so->so_rcv.sb_cc -= new->m_len;

		if (so->so_rcv.sb_mbtail == m) {
			so->so_rcv.sb_mbtail = new;
		}
	}

	m->m_pkthdr.pkt_flags |= PKTF_MPTCP;
	m->m_pkthdr.mp_dsn = dsn + off;
	m->m_pkthdr.mp_rseq = rseq + off;
	VERIFY(m_pktlen(m) < UINT16_MAX);
	m->m_pkthdr.mp_rlen = (uint16_t)m_pktlen(m);

	/* Only put the DATA_FIN-flag on the last mbuf of this mapping */
	if (dfin) {
		if (m->m_pkthdr.mp_dsn + m->m_pkthdr.mp_rlen < dsn + dlen - dfin) {
			m->m_pkthdr.pkt_flags &= ~PKTF_MPTCP_DFIN;
		} else {
			m->m_pkthdr.pkt_flags |= PKTF_MPTCP_DFIN;
		}
	}


	mpts->mpts_flags |= MPTSF_FULLY_ESTABLISHED;

	return 0;
}

/*
 * MPTCP subflow socket receive routine, derived from soreceive().
 */
static int
mptcp_subflow_soreceive(struct socket *so, struct sockaddr **psa,
    struct uio *uio, struct mbuf **mp0, struct mbuf **controlp, int *flagsp)
{
#pragma unused(uio)
	struct socket *mp_so;
	struct mptses *mpte;
	struct mptcb *mp_tp;
	int flags, error = 0;
	struct mbuf *m, **mp = mp0;

	mpte = tptomptp(sototcpcb(so))->mpt_mpte;
	mp_so = mptetoso(mpte);
	mp_tp = mpte->mpte_mptcb;

	VERIFY(so->so_proto->pr_flags & PR_CONNREQUIRED);

#ifdef MORE_LOCKING_DEBUG
	if (so->so_usecount == 1) {
		panic("%s: so=%x no other reference on socket\n", __func__, so);
		/* NOTREACHED */
	}
#endif
	/*
	 * We return all that is there in the subflow's socket receive buffer
	 * to the MPTCP layer, so we require that the caller passes in the
	 * expected parameters.
	 */
	if (mp == NULL || controlp != NULL) {
		return EINVAL;
	}

	*mp = NULL;
	if (psa != NULL) {
		*psa = NULL;
	}
	if (flagsp != NULL) {
		flags = *flagsp & ~MSG_EOR;
	} else {
		flags = 0;
	}

	if (flags & (MSG_PEEK | MSG_OOB | MSG_NEEDSA | MSG_WAITALL | MSG_WAITSTREAM)) {
		return EOPNOTSUPP;
	}

	flags |= (MSG_DONTWAIT | MSG_NBIO);

	/*
	 * If a recv attempt is made on a previously-accepted socket
	 * that has been marked as inactive (disconnected), reject
	 * the request.
	 */
	if (so->so_flags & SOF_DEFUNCT) {
		struct sockbuf *sb = &so->so_rcv;

		error = ENOTCONN;
		/*
		 * This socket should have been disconnected and flushed
		 * prior to being returned from sodefunct(); there should
		 * be no data on its receive list, so panic otherwise.
		 */
		if (so->so_state & SS_DEFUNCT) {
			sb_empty_assert(sb, __func__);
		}
		return error;
	}

	/*
	 * See if the socket has been closed (SS_NOFDREF|SS_CANTRCVMORE)
	 * and if so just return to the caller.  This could happen when
	 * soreceive() is called by a socket upcall function during the
	 * time the socket is freed.  The socket buffer would have been
	 * locked across the upcall, therefore we cannot put this thread
	 * to sleep (else we will deadlock) or return EWOULDBLOCK (else
	 * we may livelock), because the lock on the socket buffer will
	 * only be released when the upcall routine returns to its caller.
	 * Because the socket has been officially closed, there can be
	 * no further read on it.
	 *
	 * A multipath subflow socket would have its SS_NOFDREF set by
	 * default, so check for SOF_MP_SUBFLOW socket flag; when the
	 * socket is closed for real, SOF_MP_SUBFLOW would be cleared.
	 */
	if ((so->so_state & (SS_NOFDREF | SS_CANTRCVMORE)) ==
	    (SS_NOFDREF | SS_CANTRCVMORE) && !(so->so_flags & SOF_MP_SUBFLOW)) {
		return 0;
	}

	/*
	 * For consistency with soreceive() semantics, we need to obey
	 * SB_LOCK in case some other code path has locked the buffer.
	 */
	error = sblock(&so->so_rcv, 0);
	if (error != 0) {
		return error;
	}

	m = so->so_rcv.sb_mb;
	if (m == NULL) {
		/*
		 * Panic if we notice inconsistencies in the socket's
		 * receive list; both sb_mb and sb_cc should correctly
		 * reflect the contents of the list, otherwise we may
		 * end up with false positives during select() or poll()
		 * which could put the application in a bad state.
		 */
		SB_MB_CHECK(&so->so_rcv);

		if (so->so_error != 0) {
			error = so->so_error;
			so->so_error = 0;
			goto release;
		}

		if (so->so_state & SS_CANTRCVMORE) {
			goto release;
		}

		if (!(so->so_state & (SS_ISCONNECTED | SS_ISCONNECTING))) {
			error = ENOTCONN;
			goto release;
		}

		/*
		 * MSG_DONTWAIT is implicitly defined and this routine will
		 * never block, so return EWOULDBLOCK when there is nothing.
		 */
		error = EWOULDBLOCK;
		goto release;
	}

	mptcp_update_last_owner(so, mp_so);

	SBLASTRECORDCHK(&so->so_rcv, "mptcp_subflow_soreceive 1");
	SBLASTMBUFCHK(&so->so_rcv, "mptcp_subflow_soreceive 1");

	while (m != NULL) {
		int dlen = 0, error_out = 0, off = 0;
		uint8_t dfin = 0;
		struct mbuf *start = m;
		uint64_t dsn;
		uint32_t sseq;
		uint16_t orig_dlen;
		uint16_t csum;

		VERIFY(m->m_nextpkt == NULL);

		if (mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP) {
fallback:
			/* Just move mbuf to MPTCP-level */

			sbfree(&so->so_rcv, m);

			if (mp != NULL) {
				*mp = m;
				mp = &m->m_next;
				so->so_rcv.sb_mb = m = m->m_next;
				*mp = NULL;
			}

			if (m != NULL) {
				so->so_rcv.sb_lastrecord = m;
			} else {
				SB_EMPTY_FIXUP(&so->so_rcv);
			}

			continue;
		} else if (!(m->m_flags & M_PKTHDR) || !(m->m_pkthdr.pkt_flags & PKTF_MPTCP)) {
			struct mptsub *mpts = sototcpcb(so)->t_mpsub;
			boolean_t found_mapping = false;
			int parsed_length = 0;
			struct mbuf *m_iter;

			/*
			 * No MPTCP-option in the header. Either fallback or
			 * wait for additional mappings.
			 */
			if (!(mpts->mpts_flags & MPTSF_FULLY_ESTABLISHED)) {
				/* data arrived without a DSS option mapping */

				/* initial subflow can fallback right after SYN handshake */
				if (mpts->mpts_flags & MPTSF_INITIAL_SUB) {
					mptcp_notify_mpfail(so);

					goto fallback;
				} else {
					os_log_error(mptcp_log_handle, "%s - %lx: No DSS on secondary subflow. Killing %d\n",
					    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
					    mpts->mpts_connid);
					soevent(mpts->mpts_socket, SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);

					error = EIO;
					*mp0 = NULL;
					goto release;
				}
			}

			/* Thus, let's look for an mbuf with the mapping */
			m_iter = m->m_next;
			parsed_length = m->m_len;
			while (m_iter != NULL && parsed_length < UINT16_MAX) {
				if (!(m_iter->m_flags & M_PKTHDR) || !(m_iter->m_pkthdr.pkt_flags & PKTF_MPTCP)) {
					parsed_length += m_iter->m_len;
					m_iter = m_iter->m_next;
					continue;
				}

				found_mapping = true;

				/* Found an mbuf with a DSS-mapping */
				orig_dlen = dlen = m_iter->m_pkthdr.mp_rlen;
				dsn = m_iter->m_pkthdr.mp_dsn;
				sseq = m_iter->m_pkthdr.mp_rseq;
				csum = m_iter->m_pkthdr.mp_csum;

				if (m_iter->m_pkthdr.pkt_flags & PKTF_MPTCP_DFIN) {
					dfin = 1;
					dlen--;
				}

				break;
			}

			if (!found_mapping && parsed_length < UINT16_MAX) {
				/* Mapping not yet present, we can wait! */
				if (*mp0 == NULL) {
					error = EWOULDBLOCK;
				}
				goto release;
			} else if (!found_mapping && parsed_length >= UINT16_MAX) {
				os_log_error(mptcp_log_handle, "%s - %lx: Received more than 64KB without DSS mapping. Killing %d\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
				    mpts->mpts_connid);
				/* Received 64KB without DSS-mapping. We should kill the subflow */
				soevent(mpts->mpts_socket, SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);

				error = EIO;
				*mp0 = NULL;
				goto release;
			}
		} else {
			orig_dlen = dlen = m->m_pkthdr.mp_rlen;
			dsn = m->m_pkthdr.mp_dsn;
			sseq = m->m_pkthdr.mp_rseq;
			csum = m->m_pkthdr.mp_csum;

			if (m->m_pkthdr.pkt_flags & PKTF_MPTCP_DFIN) {
				dfin = 1;
				dlen--;
			}
		}

		/*
		 * Check if the full mapping is now present
		 */
		if ((int)so->so_rcv.sb_cc < dlen) {
			if (*mp0 == NULL) {
				error = EWOULDBLOCK;
			}
			goto release;
		}

		/* Now, get the full mapping */
		off = 0;
		while (dlen > 0) {
			if (mptcp_adj_rmap(so, m, off, dsn, sseq, orig_dlen, dfin)) {
				error_out = 1;
				error = EIO;
				dlen = 0;
				*mp0 = NULL;
				break;
			}

			dlen -= m->m_len;
			off += m->m_len;
			sbfree(&so->so_rcv, m);

			if (mp != NULL) {
				*mp = m;
				mp = &m->m_next;
				so->so_rcv.sb_mb = m = m->m_next;
				*mp = NULL;
			}

			VERIFY(dlen == 0 || m);
		}

		VERIFY(dlen == 0);

		if (m != NULL) {
			so->so_rcv.sb_lastrecord = m;
		} else {
			SB_EMPTY_FIXUP(&so->so_rcv);
		}

		if (error_out) {
			goto release;
		}

		if (mptcp_validate_csum(sototcpcb(so), start, dsn, sseq, orig_dlen, csum, dfin)) {
			error = EIO;
			*mp0 = NULL;
			goto release;
		}

		SBLASTRECORDCHK(&so->so_rcv, "mptcp_subflow_soreceive 2");
		SBLASTMBUFCHK(&so->so_rcv, "mptcp_subflow_soreceive 2");
	}

	DTRACE_MPTCP3(subflow__receive, struct socket *, so,
	    struct sockbuf *, &so->so_rcv, struct sockbuf *, &so->so_snd);

	if (flagsp != NULL) {
		*flagsp |= flags;
	}

release:
	sbunlock(&so->so_rcv, TRUE);

	return error;
}

/*
 * MPTCP subflow socket send routine, derived from sosend().
 */
static int
mptcp_subflow_sosend(struct socket *so, struct sockaddr *addr, struct uio *uio,
    struct mbuf *top, struct mbuf *control, int flags)
{
	struct socket *mp_so = mptetoso(tptomptp(sototcpcb(so))->mpt_mpte);
	boolean_t en_tracing = FALSE, proc_held = FALSE;
	struct proc *p = current_proc();
	int en_tracing_val;
	int sblocked = 1; /* Pretend as if it is already locked, so we won't relock it */
	int error;

	VERIFY(control == NULL);
	VERIFY(addr == NULL);
	VERIFY(uio == NULL);
	VERIFY(flags == 0);
	VERIFY((so->so_flags & SOF_CONTENT_FILTER) == 0);

	VERIFY(top->m_pkthdr.len > 0 && top->m_pkthdr.len <= UINT16_MAX);
	VERIFY(top->m_pkthdr.pkt_flags & PKTF_MPTCP);

	/*
	 * trace if tracing & network (vs. unix) sockets & and
	 * non-loopback
	 */
	if (ENTR_SHOULDTRACE &&
	    (SOCK_CHECK_DOM(so, AF_INET) || SOCK_CHECK_DOM(so, AF_INET6))) {
		struct inpcb *inp = sotoinpcb(so);
		if (inp->inp_last_outifp != NULL &&
		    !(inp->inp_last_outifp->if_flags & IFF_LOOPBACK)) {
			en_tracing = TRUE;
			en_tracing_val = top->m_pkthdr.len;
			KERNEL_ENERGYTRACE(kEnTrActKernSockWrite, DBG_FUNC_START,
			    (unsigned long)VM_KERNEL_ADDRPERM(so),
			    ((so->so_state & SS_NBIO) ? kEnTrFlagNonBlocking : 0),
			    (int64_t)en_tracing_val);
		}
	}

	mptcp_update_last_owner(so, mp_so);

	if (mp_so->last_pid != proc_pid(p)) {
		p = proc_find(mp_so->last_pid);
		if (p == PROC_NULL) {
			p = current_proc();
		} else {
			proc_held = TRUE;
		}
	}

#if NECP
	inp_update_necp_policy(sotoinpcb(so), NULL, NULL, 0);
#endif /* NECP */

	error = sosendcheck(so, NULL, top->m_pkthdr.len, 0, 1, 0, &sblocked);
	if (error) {
		goto out;
	}

	error = (*so->so_proto->pr_usrreqs->pru_send)(so, 0, top, NULL, NULL, p);
	top = NULL;

out:
	if (top != NULL) {
		m_freem(top);
	}

	if (proc_held) {
		proc_rele(p);
	}

	soclearfastopen(so);

	if (en_tracing) {
		KERNEL_ENERGYTRACE(kEnTrActKernSockWrite, DBG_FUNC_END,
		    (unsigned long)VM_KERNEL_ADDRPERM(so),
		    ((error == EWOULDBLOCK) ? kEnTrFlagNoWork : 0),
		    (int64_t)en_tracing_val);
	}

	return error;
}

/*
 * Establish an initial MPTCP connection (if first subflow and not yet
 * connected), or add a subflow to an existing MPTCP connection.
 */
int
mptcp_subflow_add(struct mptses *mpte, struct sockaddr *src,
    struct sockaddr *dst, uint32_t ifscope, sae_connid_t *pcid)
{
	struct socket *mp_so, *so = NULL;
	struct mptcb *mp_tp;
	struct mptsub *mpts = NULL;
	int af, error = 0;

	mp_so = mptetoso(mpte);
	mp_tp = mpte->mpte_mptcb;

	socket_lock_assert_owned(mp_so);

	if (mp_tp->mpt_state >= MPTCPS_CLOSE_WAIT) {
		/* If the remote end sends Data FIN, refuse subflow adds */
		os_log_error(mptcp_log_handle, "%s - %lx: state %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mp_tp->mpt_state);
		error = ENOTCONN;
		goto out_err;
	}

	if (mpte->mpte_numflows > MPTCP_MAX_NUM_SUBFLOWS) {
		error = EOVERFLOW;
		goto out_err;
	}

	mpts = mptcp_subflow_alloc();
	if (mpts == NULL) {
		os_log_error(mptcp_log_handle, "%s - %lx: malloc subflow failed\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));
		error = ENOMEM;
		goto out_err;
	}

	if (src) {
		if (src->sa_family != AF_INET && src->sa_family != AF_INET6) {
			error = EAFNOSUPPORT;
			goto out_err;
		}

		if (src->sa_family == AF_INET &&
		    src->sa_len != sizeof(struct sockaddr_in)) {
			error = EINVAL;
			goto out_err;
		}

		if (src->sa_family == AF_INET6 &&
		    src->sa_len != sizeof(struct sockaddr_in6)) {
			error = EINVAL;
			goto out_err;
		}

		MALLOC(mpts->mpts_src, struct sockaddr *, src->sa_len, M_SONAME,
		    M_WAITOK | M_ZERO);
		if (mpts->mpts_src == NULL) {
			error = ENOMEM;
			goto out_err;
		}
		bcopy(src, mpts->mpts_src, src->sa_len);
	}

	if (dst->sa_family != AF_INET && dst->sa_family != AF_INET6) {
		error = EAFNOSUPPORT;
		goto out_err;
	}

	if (dst->sa_family == AF_INET &&
	    dst->sa_len != sizeof(mpts->__mpts_dst_v4)) {
		error = EINVAL;
		goto out_err;
	}

	if (dst->sa_family == AF_INET6 &&
	    dst->sa_len != sizeof(mpts->__mpts_dst_v6)) {
		error = EINVAL;
		goto out_err;
	}

	memcpy(&mpts->mpts_u_dst, dst, dst->sa_len);

	af = mpts->mpts_dst.sa_family;

	ifnet_head_lock_shared();
	if ((ifscope > (unsigned)if_index)) {
		ifnet_head_done();
		error = ENXIO;
		goto out_err;
	}
	ifnet_head_done();

	mpts->mpts_ifscope = ifscope;

	/* create the subflow socket */
	if ((error = mptcp_subflow_socreate(mpte, mpts, af, &so)) != 0) {
		/*
		 * Returning (error) and not cleaning up, because up to here
		 * all we did is creating mpts.
		 *
		 * And the contract is that the call to mptcp_subflow_socreate,
		 * moves ownership of mpts to mptcp_subflow_socreate.
		 */
		return error;
	}

	/*
	 * We may be called from within the kernel. Still need to account this
	 * one to the real app.
	 */
	mptcp_update_last_owner(mpts->mpts_socket, mp_so);

	/*
	 * Increment the counter, while avoiding 0 (SAE_CONNID_ANY) and
	 * -1 (SAE_CONNID_ALL).
	 */
	mpte->mpte_connid_last++;
	if (mpte->mpte_connid_last == SAE_CONNID_ALL ||
	    mpte->mpte_connid_last == SAE_CONNID_ANY) {
		mpte->mpte_connid_last++;
	}

	mpts->mpts_connid = mpte->mpte_connid_last;

	mpts->mpts_rel_seq = 1;

	/* Allocate a unique address id per subflow */
	mpte->mpte_addrid_last++;
	if (mpte->mpte_addrid_last == 0) {
		mpte->mpte_addrid_last++;
	}

	/* register for subflow socket read/write events */
	sock_setupcalls_locked(so, NULL, NULL, mptcp_subflow_wupcall, mpts, 1);

	/* Register for subflow socket control events */
	sock_catchevents_locked(so, mptcp_subflow_eupcall1, mpts,
	    SO_FILT_HINT_CONNRESET | SO_FILT_HINT_CANTRCVMORE |
	    SO_FILT_HINT_TIMEOUT | SO_FILT_HINT_NOSRCADDR |
	    SO_FILT_HINT_IFDENIED | SO_FILT_HINT_CONNECTED |
	    SO_FILT_HINT_DISCONNECTED | SO_FILT_HINT_MPFAILOVER |
	    SO_FILT_HINT_MPSTATUS | SO_FILT_HINT_MUSTRST |
	    SO_FILT_HINT_MPCANTRCVMORE | SO_FILT_HINT_ADAPTIVE_RTIMO |
	    SO_FILT_HINT_ADAPTIVE_WTIMO | SO_FILT_HINT_MP_SUB_ERROR);

	/* sanity check */
	VERIFY(!(mpts->mpts_flags &
	    (MPTSF_CONNECTING | MPTSF_CONNECTED | MPTSF_CONNECT_PENDING)));

	/*
	 * Indicate to the TCP subflow whether or not it should establish
	 * the initial MPTCP connection, or join an existing one.  Fill
	 * in the connection request structure with additional info needed
	 * by the underlying TCP (to be used in the TCP options, etc.)
	 */
	if (mp_tp->mpt_state < MPTCPS_ESTABLISHED && mpte->mpte_numflows == 1) {
		mpts->mpts_flags |= MPTSF_INITIAL_SUB;

		if (mp_tp->mpt_state == MPTCPS_CLOSED) {
			mptcp_init_local_parms(mpte);
		}
		soisconnecting(mp_so);

		/* If fastopen is requested, set state in mpts */
		if (so->so_flags1 & SOF1_PRECONNECT_DATA) {
			mpts->mpts_flags |= MPTSF_TFO_REQD;
		}
	} else {
		if (!(mp_tp->mpt_flags & MPTCPF_JOIN_READY)) {
			mpts->mpts_flags |= MPTSF_CONNECT_PENDING;
		}
	}

	mpts->mpts_flags |= MPTSF_CONNECTING;

	/* connect right away if first attempt, or if join can be done now */
	if (!(mpts->mpts_flags & MPTSF_CONNECT_PENDING)) {
		error = mptcp_subflow_soconnectx(mpte, mpts);
	}

	if (error) {
		goto out_err_close;
	}

	if (pcid) {
		*pcid = mpts->mpts_connid;
	}

	return 0;

out_err_close:
	mptcp_subflow_abort(mpts, error);

	return error;

out_err:
	if (mpts) {
		mptcp_subflow_free(mpts);
	}

	return error;
}

void
mptcpstats_update(struct mptcp_itf_stats *stats, const struct mptsub *mpts)
{
	int index = mptcpstats_get_index(stats, mpts);

	if (index != -1) {
		struct inpcb *inp = sotoinpcb(mpts->mpts_socket);

		stats[index].mpis_txbytes += inp->inp_stat->txbytes;
		stats[index].mpis_rxbytes += inp->inp_stat->rxbytes;

		stats[index].mpis_wifi_txbytes += inp->inp_wstat->txbytes;
		stats[index].mpis_wifi_rxbytes += inp->inp_wstat->rxbytes;

		stats[index].mpis_wired_txbytes += inp->inp_Wstat->txbytes;
		stats[index].mpis_wired_rxbytes += inp->inp_Wstat->rxbytes;

		stats[index].mpis_cell_txbytes += inp->inp_cstat->txbytes;
		stats[index].mpis_cell_rxbytes += inp->inp_cstat->rxbytes;
	}
}

/*
 * Delete/remove a subflow from an MPTCP.  The underlying subflow socket
 * will no longer be accessible after a subflow is deleted, thus this
 * should occur only after the subflow socket has been disconnected.
 */
void
mptcp_subflow_del(struct mptses *mpte, struct mptsub *mpts)
{
	struct socket *mp_so = mptetoso(mpte);
	struct socket *so = mpts->mpts_socket;
	struct tcpcb *tp = sototcpcb(so);

	socket_lock_assert_owned(mp_so);
	VERIFY(mpts->mpts_mpte == mpte);
	VERIFY(mpts->mpts_flags & MPTSF_ATTACHED);
	VERIFY(mpte->mpte_numflows != 0);
	VERIFY(mp_so->so_usecount > 0);

	mptcpstats_update(mpte->mpte_itfstats, mpts);

	mptcp_unset_cellicon(mpte, mpts, 1);

	mpte->mpte_init_rxbytes = sotoinpcb(so)->inp_stat->rxbytes;
	mpte->mpte_init_txbytes = sotoinpcb(so)->inp_stat->txbytes;

	atomic_bitclear_32(&mpts->mpts_flags, MPTSF_ATTACHED);
	TAILQ_REMOVE(&mpte->mpte_subflows, mpts, mpts_entry);
	mpte->mpte_numflows--;
	if (mpte->mpte_active_sub == mpts) {
		mpte->mpte_active_sub = NULL;
	}

	/*
	 * Drop references held by this subflow socket; there
	 * will be no further upcalls made from this point.
	 */
	sock_setupcalls_locked(so, NULL, NULL, NULL, NULL, 0);
	sock_catchevents_locked(so, NULL, NULL, 0);

	mptcp_detach_mptcb_from_subf(mpte->mpte_mptcb, so);

	mp_so->so_usecount--;           /* for subflow socket */
	mpts->mpts_mpte = NULL;
	mpts->mpts_socket = NULL;

	mptcp_subflow_remref(mpts);             /* for MPTCP subflow list */
	mptcp_subflow_remref(mpts);             /* for subflow socket */

	so->so_flags &= ~SOF_MP_SUBFLOW;
	tp->t_mptcb = NULL;
	tp->t_mpsub = NULL;
}

void
mptcp_subflow_shutdown(struct mptses *mpte, struct mptsub *mpts)
{
	struct socket *so = mpts->mpts_socket;
	struct mptcb *mp_tp = mpte->mpte_mptcb;
	int send_dfin = 0;

	if (mp_tp->mpt_state > MPTCPS_CLOSE_WAIT) {
		send_dfin = 1;
	}

	if (!(so->so_state & (SS_ISDISCONNECTING | SS_ISDISCONNECTED)) &&
	    (so->so_state & SS_ISCONNECTED)) {
		mptcplog((LOG_DEBUG, "MPTCP subflow shutdown %s: cid %d fin %d\n",
		    __func__, mpts->mpts_connid, send_dfin),
		    MPTCP_SOCKET_DBG, MPTCP_LOGLVL_VERBOSE);

		if (send_dfin) {
			mptcp_send_dfin(so);
		}
		soshutdownlock(so, SHUT_WR);
	}
}

static void
mptcp_subflow_abort(struct mptsub *mpts, int error)
{
	struct socket *so = mpts->mpts_socket;
	struct tcpcb *tp = sototcpcb(so);

	if (mpts->mpts_flags & MPTSF_DISCONNECTED) {
		return;
	}

	mptcplog((LOG_DEBUG, "%s aborting connection state %u\n", __func__, tp->t_state),
	    MPTCP_SOCKET_DBG, MPTCP_LOGLVL_VERBOSE);

	if (tp->t_state != TCPS_CLOSED) {
		tcp_drop(tp, error);
	}

	mptcp_subflow_eupcall1(so, mpts, SO_FILT_HINT_DISCONNECTED);
}

/*
 * Disconnect a subflow socket.
 */
void
mptcp_subflow_disconnect(struct mptses *mpte, struct mptsub *mpts)
{
	struct socket *so, *mp_so;
	struct mptcb *mp_tp;
	int send_dfin = 0;

	so = mpts->mpts_socket;
	mp_tp = mpte->mpte_mptcb;
	mp_so = mptetoso(mpte);

	socket_lock_assert_owned(mp_so);

	if (mpts->mpts_flags & (MPTSF_DISCONNECTING | MPTSF_DISCONNECTED)) {
		return;
	}

	mptcp_unset_cellicon(mpte, mpts, 1);

	mpts->mpts_flags |= MPTSF_DISCONNECTING;

	if (mp_tp->mpt_state > MPTCPS_CLOSE_WAIT) {
		send_dfin = 1;
	}

	if (mp_so->so_flags & SOF_DEFUNCT) {
		errno_t ret;

		ret = sosetdefunct(NULL, so, SHUTDOWN_SOCKET_LEVEL_DISCONNECT_ALL, TRUE);
		if (ret == 0) {
			ret = sodefunct(NULL, so, SHUTDOWN_SOCKET_LEVEL_DISCONNECT_ALL);

			if (ret != 0) {
				os_log_error(mptcp_log_handle, "%s - %lx: sodefunct failed with %d\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), ret);
			}
		} else {
			os_log_error(mptcp_log_handle, "%s - %lx: sosetdefunct failed with %d\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), ret);
		}
	}

	if (!(so->so_state & (SS_ISDISCONNECTING | SS_ISDISCONNECTED)) &&
	    (so->so_state & SS_ISCONNECTED)) {
		mptcplog((LOG_DEBUG, "%s: cid %d fin %d\n",
		    __func__, mpts->mpts_connid, send_dfin),
		    MPTCP_SOCKET_DBG, MPTCP_LOGLVL_VERBOSE);

		if (send_dfin) {
			mptcp_send_dfin(so);
		}

		(void) soshutdownlock(so, SHUT_RD);
		(void) soshutdownlock(so, SHUT_WR);
		(void) sodisconnectlocked(so);
	}

	/*
	 * Generate a disconnect event for this subflow socket, in case
	 * the lower layer doesn't do it; this is needed because the
	 * subflow socket deletion relies on it.
	 */
	mptcp_subflow_eupcall1(so, mpts, SO_FILT_HINT_DISCONNECTED);
}

/*
 * Subflow socket input.
 */
static void
mptcp_subflow_input(struct mptses *mpte, struct mptsub *mpts)
{
	struct socket *mp_so = mptetoso(mpte);
	struct mbuf *m = NULL;
	struct socket *so;
	int error, wakeup = 0;

	VERIFY(!(mpte->mpte_mppcb->mpp_flags & MPP_INSIDE_INPUT));
	mpte->mpte_mppcb->mpp_flags |= MPP_INSIDE_INPUT;

	DTRACE_MPTCP2(subflow__input, struct mptses *, mpte,
	    struct mptsub *, mpts);

	if (!(mpts->mpts_flags & MPTSF_CONNECTED)) {
		goto out;
	}

	so = mpts->mpts_socket;

	error = sock_receive_internal(so, NULL, &m, 0, NULL);
	if (error != 0 && error != EWOULDBLOCK) {
		os_log_error(mptcp_log_handle, "%s - %lx: cid %d error %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpts->mpts_connid, error);
		if (error == ENODATA) {
			/*
			 * Don't ignore ENODATA so as to discover
			 * nasty middleboxes.
			 */
			mp_so->so_error = ENODATA;

			wakeup = 1;
			goto out;
		}
	} else if (error == 0) {
		mptcplog((LOG_DEBUG, "%s: cid %d \n", __func__, mpts->mpts_connid),
		    MPTCP_RECEIVER_DBG, MPTCP_LOGLVL_VERBOSE);
	}

	/* In fallback, make sure to accept data on all but one subflow */
	if (m && (mpts->mpts_flags & MPTSF_MP_DEGRADED) &&
	    !(mpts->mpts_flags & MPTSF_ACTIVE)) {
		mptcplog((LOG_DEBUG, "%s: degraded and got data on non-active flow\n",
		    __func__), MPTCP_RECEIVER_DBG, MPTCP_LOGLVL_VERBOSE);
		m_freem(m);
		goto out;
	}

	if (m != NULL) {
		if (IFNET_IS_CELLULAR(sotoinpcb(so)->inp_last_outifp)) {
			mptcp_set_cellicon(mpte, mpts);

			mpte->mpte_used_cell = 1;
		} else {
			/*
			 * If during the past MPTCP_CELLICON_TOGGLE_RATE seconds we didn't
			 * explicitly set the cellicon, then we unset it again.
			 */
			if (TSTMP_LT(mpte->mpte_last_cellicon_set + MPTCP_CELLICON_TOGGLE_RATE, tcp_now)) {
				mptcp_unset_cellicon(mpte, NULL, 1);
			}

			mpte->mpte_used_wifi = 1;
		}

		mptcp_input(mpte, m);
	}

out:
	if (wakeup) {
		mpte->mpte_mppcb->mpp_flags |= MPP_SHOULD_RWAKEUP;
	}

	mptcp_handle_deferred_upcalls(mpte->mpte_mppcb, MPP_INSIDE_INPUT);
}

void
mptcp_handle_input(struct socket *so)
{
	struct mptsub *mpts, *tmpts;
	struct mptses *mpte;

	if (!(so->so_flags & SOF_MP_SUBFLOW)) {
		return;
	}

	mpts = sototcpcb(so)->t_mpsub;
	mpte = mpts->mpts_mpte;

	socket_lock_assert_owned(mptetoso(mpte));

	if (mptcp_should_defer_upcall(mpte->mpte_mppcb)) {
		if (!(mpte->mpte_mppcb->mpp_flags & MPP_INPUT_HANDLE)) {
			mpte->mpte_mppcb->mpp_flags |= MPP_SHOULD_RWAKEUP;
		}
		return;
	}

	mpte->mpte_mppcb->mpp_flags |= MPP_INPUT_HANDLE;
	TAILQ_FOREACH_SAFE(mpts, &mpte->mpte_subflows, mpts_entry, tmpts) {
		if (mpts->mpts_socket->so_usecount == 0) {
			/* Will be removed soon by tcp_garbage_collect */
			continue;
		}

		mptcp_subflow_addref(mpts);
		mpts->mpts_socket->so_usecount++;

		mptcp_subflow_input(mpte, mpts);

		mptcp_subflow_remref(mpts);             /* ours */

		VERIFY(mpts->mpts_socket->so_usecount != 0);
		mpts->mpts_socket->so_usecount--;
	}

	mptcp_handle_deferred_upcalls(mpte->mpte_mppcb, MPP_INPUT_HANDLE);
}

/*
 * Subflow socket write upcall.
 *
 * Called when the associated subflow socket posted a read event.
 */
static void
mptcp_subflow_wupcall(struct socket *so, void *arg, int waitf)
{
#pragma unused(so, waitf)
	struct mptsub *mpts = arg;
	struct mptses *mpte = mpts->mpts_mpte;

	VERIFY(mpte != NULL);

	if (mptcp_should_defer_upcall(mpte->mpte_mppcb)) {
		if (!(mpte->mpte_mppcb->mpp_flags & MPP_WUPCALL)) {
			mpte->mpte_mppcb->mpp_flags |= MPP_SHOULD_WWAKEUP;
		}
		return;
	}

	mptcp_output(mpte);
}

static boolean_t
mptcp_search_seq_in_sub(struct mbuf *m, struct socket *so)
{
	struct mbuf *so_m = so->so_snd.sb_mb;
	uint64_t dsn = m->m_pkthdr.mp_dsn;

	while (so_m) {
		VERIFY(so_m->m_flags & M_PKTHDR);
		VERIFY(so_m->m_pkthdr.pkt_flags & PKTF_MPTCP);

		/* Part of the segment is covered, don't reinject here */
		if (so_m->m_pkthdr.mp_dsn <= dsn &&
		    so_m->m_pkthdr.mp_dsn + so_m->m_pkthdr.mp_rlen > dsn) {
			return TRUE;
		}

		so_m = so_m->m_next;
	}

	return FALSE;
}

/*
 * Subflow socket output.
 *
 * Called for sending data from MPTCP to the underlying subflow socket.
 */
int
mptcp_subflow_output(struct mptses *mpte, struct mptsub *mpts, int flags)
{
	struct mptcb *mp_tp = mpte->mpte_mptcb;
	struct mbuf *sb_mb, *m, *mpt_mbuf = NULL, *head = NULL, *tail = NULL;
	struct socket *mp_so, *so;
	struct tcpcb *tp;
	uint64_t mpt_dsn = 0, off = 0;
	int sb_cc = 0, error = 0, wakeup = 0;
	uint16_t dss_csum;
	uint16_t tot_sent = 0;
	boolean_t reinjected = FALSE;

	mp_so = mptetoso(mpte);
	so = mpts->mpts_socket;
	tp = sototcpcb(so);

	socket_lock_assert_owned(mp_so);

	VERIFY(!(mpte->mpte_mppcb->mpp_flags & MPP_INSIDE_OUTPUT));
	mpte->mpte_mppcb->mpp_flags |= MPP_INSIDE_OUTPUT;

	VERIFY(!INP_WAIT_FOR_IF_FEEDBACK(sotoinpcb(so)));
	VERIFY((mpts->mpts_flags & MPTSF_MP_CAPABLE) ||
	    (mpts->mpts_flags & MPTSF_MP_DEGRADED) ||
	    (mpts->mpts_flags & MPTSF_TFO_REQD));
	VERIFY(mptcp_subflow_cwnd_space(mpts->mpts_socket) > 0);

	mptcplog((LOG_DEBUG, "%s mpts_flags %#x, mpte_flags %#x cwnd_space %u\n",
	    __func__, mpts->mpts_flags, mpte->mpte_flags,
	    mptcp_subflow_cwnd_space(so)),
	    MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);
	DTRACE_MPTCP2(subflow__output, struct mptses *, mpte,
	    struct mptsub *, mpts);

	/* Remove Addr Option is not sent reliably as per I-D */
	if (mpte->mpte_flags & MPTE_SND_REM_ADDR) {
		tp->t_rem_aid = mpte->mpte_lost_aid;
		tp->t_mpflags |= TMPF_SND_REM_ADDR;
		mpte->mpte_flags &= ~MPTE_SND_REM_ADDR;
	}

	/*
	 * The mbuf chains containing the metadata (as well as pointing to
	 * the user data sitting at the MPTCP output queue) would then be
	 * sent down to the subflow socket.
	 *
	 * Some notes on data sequencing:
	 *
	 *   a. Each mbuf must be a M_PKTHDR.
	 *   b. MPTCP metadata is stored in the mptcp_pktinfo structure
	 *	in the mbuf pkthdr structure.
	 *   c. Each mbuf containing the MPTCP metadata must have its
	 *	pkt_flags marked with the PKTF_MPTCP flag.
	 */

	if (mpte->mpte_reinjectq) {
		sb_mb = mpte->mpte_reinjectq;
	} else {
		sb_mb = mp_so->so_snd.sb_mb;
	}

	if (sb_mb == NULL) {
		os_log_error(mptcp_log_handle, "%s - %lx: No data in MPTCP-sendbuffer! smax %u snxt %u suna %u state %u flags %#x\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
		    (uint32_t)mp_tp->mpt_sndmax, (uint32_t)mp_tp->mpt_sndnxt,
		    (uint32_t)mp_tp->mpt_snduna, mp_tp->mpt_state, mp_so->so_flags1);

		/* Fix it to prevent looping */
		if (MPTCP_SEQ_LT(mp_tp->mpt_sndnxt, mp_tp->mpt_snduna)) {
			mp_tp->mpt_sndnxt = mp_tp->mpt_snduna;
		}
		goto out;
	}

	VERIFY(sb_mb->m_pkthdr.pkt_flags & PKTF_MPTCP);

	if (sb_mb->m_pkthdr.mp_rlen == 0 &&
	    !(so->so_state & SS_ISCONNECTED) &&
	    (so->so_flags1 & SOF1_PRECONNECT_DATA)) {
		tp->t_mpflags |= TMPF_TFO_REQUEST;

		/* Opting to call pru_send as no mbuf at subflow level */
		error = (*so->so_proto->pr_usrreqs->pru_send)(so, 0, NULL, NULL,
		    NULL, current_proc());

		goto done_sending;
	}

	mpt_dsn = sb_mb->m_pkthdr.mp_dsn;

	/* First, drop acknowledged data */
	if (MPTCP_SEQ_LT(mpt_dsn, mp_tp->mpt_snduna)) {
		os_log_error(mptcp_log_handle, "%s - %lx: dropping data, should have been done earlier "
		    "dsn %u suna %u reinject? %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), (uint32_t)mpt_dsn,
		    (uint32_t)mp_tp->mpt_snduna, !!mpte->mpte_reinjectq);
		if (mpte->mpte_reinjectq) {
			mptcp_clean_reinjectq(mpte);
		} else {
			uint64_t len = 0;
			len = mp_tp->mpt_snduna - mpt_dsn;
			sbdrop(&mp_so->so_snd, (int)len);
			wakeup = 1;
		}
	}

	/* Check again because of above sbdrop */
	if (mp_so->so_snd.sb_mb == NULL && mpte->mpte_reinjectq == NULL) {
		os_log_error(mptcp_log_handle, "%s - $%lx: send-buffer is empty\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));
		goto out;
	}

	/*
	 * In degraded mode, we don't receive data acks, so force free
	 * mbufs less than snd_nxt
	 */
	if ((mpts->mpts_flags & MPTSF_MP_DEGRADED) &&
	    (mp_tp->mpt_flags & MPTCPF_POST_FALLBACK_SYNC) &&
	    mp_so->so_snd.sb_mb) {
		mpt_dsn = mp_so->so_snd.sb_mb->m_pkthdr.mp_dsn;
		if (MPTCP_SEQ_LT(mpt_dsn, mp_tp->mpt_snduna)) {
			uint64_t len = 0;
			len = mp_tp->mpt_snduna - mpt_dsn;
			sbdrop(&mp_so->so_snd, (int)len);
			wakeup = 1;

			os_log_error(mptcp_log_handle, "%s - %lx: dropping data in degraded mode, should have been done earlier dsn %u sndnxt %u suna %u\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    (uint32_t)mpt_dsn, (uint32_t)mp_tp->mpt_sndnxt, (uint32_t)mp_tp->mpt_snduna);
		}
	}

	if ((mpts->mpts_flags & MPTSF_MP_DEGRADED) &&
	    !(mp_tp->mpt_flags & MPTCPF_POST_FALLBACK_SYNC)) {
		mp_tp->mpt_flags |= MPTCPF_POST_FALLBACK_SYNC;
		so->so_flags1 |= SOF1_POST_FALLBACK_SYNC;
	}

	/*
	 * Adjust the top level notion of next byte used for retransmissions
	 * and sending FINs.
	 */
	if (MPTCP_SEQ_LT(mp_tp->mpt_sndnxt, mp_tp->mpt_snduna)) {
		mp_tp->mpt_sndnxt = mp_tp->mpt_snduna;
	}

	/* Now determine the offset from which to start transmitting data */
	if (mpte->mpte_reinjectq) {
		sb_mb = mpte->mpte_reinjectq;
	} else {
dont_reinject:
		sb_mb = mp_so->so_snd.sb_mb;
	}
	if (sb_mb == NULL) {
		os_log_error(mptcp_log_handle, "%s - %lx: send-buffer is still empty\n", __func__,
		    (unsigned long)VM_KERNEL_ADDRPERM(mpte));
		goto out;
	}

	if (sb_mb == mpte->mpte_reinjectq) {
		sb_cc = sb_mb->m_pkthdr.mp_rlen;
		off = 0;

		if (mptcp_search_seq_in_sub(sb_mb, so)) {
			if (mptcp_can_send_more(mp_tp, TRUE)) {
				goto dont_reinject;
			}

			error = ECANCELED;
			goto out;
		}

		reinjected = TRUE;
	} else if (flags & MPTCP_SUBOUT_PROBING) {
		sb_cc = sb_mb->m_pkthdr.mp_rlen;
		off = 0;
	} else {
		sb_cc = min(mp_so->so_snd.sb_cc, mp_tp->mpt_sndwnd);

		/*
		 * With TFO, there might be no data at all, thus still go into this
		 * code-path here.
		 */
		if ((mp_so->so_flags1 & SOF1_PRECONNECT_DATA) ||
		    MPTCP_SEQ_LT(mp_tp->mpt_sndnxt, mp_tp->mpt_sndmax)) {
			off = mp_tp->mpt_sndnxt - mp_tp->mpt_snduna;
			sb_cc -= off;
		} else {
			os_log_error(mptcp_log_handle, "%s - %lx: this should not happen: sndnxt %u sndmax %u\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), (uint32_t)mp_tp->mpt_sndnxt,
			    (uint32_t)mp_tp->mpt_sndmax);

			goto out;
		}
	}

	sb_cc = min(sb_cc, mptcp_subflow_cwnd_space(so));
	if (sb_cc <= 0) {
		os_log_error(mptcp_log_handle, "%s - %lx: sb_cc is %d, mp_so->sb_cc %u, sndwnd %u,sndnxt %u sndmax %u cwnd %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), sb_cc, mp_so->so_snd.sb_cc, mp_tp->mpt_sndwnd,
		    (uint32_t)mp_tp->mpt_sndnxt, (uint32_t)mp_tp->mpt_sndmax,
		    mptcp_subflow_cwnd_space(so));
	}

	sb_cc = min(sb_cc, UINT16_MAX);

	/*
	 * Create a DSN mapping for the data we are about to send. It all
	 * has the same mapping.
	 */
	if (reinjected) {
		mpt_dsn = sb_mb->m_pkthdr.mp_dsn;
	} else {
		mpt_dsn = mp_tp->mpt_snduna + off;
	}

	mpt_mbuf = sb_mb;
	while (mpt_mbuf && reinjected == FALSE &&
	    (mpt_mbuf->m_pkthdr.mp_rlen == 0 ||
	    mpt_mbuf->m_pkthdr.mp_rlen <= (uint32_t)off)) {
		off -= mpt_mbuf->m_pkthdr.mp_rlen;
		mpt_mbuf = mpt_mbuf->m_next;
	}
	if (mpts->mpts_flags & MPTSF_MP_DEGRADED) {
		mptcplog((LOG_DEBUG, "%s: %u snduna = %u sndnxt = %u probe %d\n",
		    __func__, mpts->mpts_connid, (uint32_t)mp_tp->mpt_snduna, (uint32_t)mp_tp->mpt_sndnxt,
		    mpts->mpts_probecnt),
		    MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);
	}

	VERIFY((mpt_mbuf == NULL) || (mpt_mbuf->m_pkthdr.pkt_flags & PKTF_MPTCP));

	head = tail = NULL;

	while (tot_sent < sb_cc) {
		int32_t mlen;

		mlen = mpt_mbuf->m_len;
		mlen -= off;
		mlen = MIN(mlen, sb_cc - tot_sent);

		if (mlen < 0) {
			os_log_error(mptcp_log_handle, "%s - %lx: mlen %d mp_rlen %u off %u sb_cc %u tot_sent %u\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mlen, mpt_mbuf->m_pkthdr.mp_rlen,
			    (uint32_t)off, sb_cc, tot_sent);
			goto out;
		}

		if (mlen == 0) {
			goto next;
		}

		m = m_copym_mode(mpt_mbuf, (int)off, mlen, M_DONTWAIT,
		    M_COPYM_MUST_COPY_HDR);
		if (m == NULL) {
			os_log_error(mptcp_log_handle, "%s - %lx: m_copym_mode failed\n", __func__,
			    (unsigned long)VM_KERNEL_ADDRPERM(mpte));
			error = ENOBUFS;
			break;
		}

		/* Create a DSN mapping for the data (m_copym does it) */
		VERIFY(m->m_flags & M_PKTHDR);
		VERIFY(m->m_next == NULL);

		m->m_pkthdr.pkt_flags |= PKTF_MPTCP;
		m->m_pkthdr.pkt_flags &= ~PKTF_MPSO;
		m->m_pkthdr.mp_dsn = mpt_dsn;
		m->m_pkthdr.mp_rseq = mpts->mpts_rel_seq;
		m->m_pkthdr.len = mlen;

		if (head == NULL) {
			head = tail = m;
		} else {
			tail->m_next = m;
			tail = m;
		}

		tot_sent += mlen;
		off = 0;
next:
		mpt_mbuf = mpt_mbuf->m_next;
	}

	if (reinjected) {
		if (sb_cc < sb_mb->m_pkthdr.mp_rlen) {
			struct mbuf *n = sb_mb;

			while (n) {
				n->m_pkthdr.mp_dsn += sb_cc;
				n->m_pkthdr.mp_rlen -= sb_cc;
				n = n->m_next;
			}
			m_adj(sb_mb, sb_cc);
		} else {
			mpte->mpte_reinjectq = sb_mb->m_nextpkt;
			m_freem(sb_mb);
		}
	}

	mptcplog((LOG_DEBUG, "%s: Queued dsn %u ssn %u len %u on sub %u\n",
	    __func__, (uint32_t)mpt_dsn, mpts->mpts_rel_seq,
	    tot_sent, mpts->mpts_connid), MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);

	if (head && (mp_tp->mpt_flags & MPTCPF_CHECKSUM)) {
		dss_csum = mptcp_output_csum(head, mpt_dsn, mpts->mpts_rel_seq,
		    tot_sent);
	}

	/* Now, let's update rel-seq and the data-level length */
	mpts->mpts_rel_seq += tot_sent;
	m = head;
	while (m) {
		if (mp_tp->mpt_flags & MPTCPF_CHECKSUM) {
			m->m_pkthdr.mp_csum = dss_csum;
		}
		m->m_pkthdr.mp_rlen = tot_sent;
		m = m->m_next;
	}

	if (head != NULL) {
		if ((mpts->mpts_flags & MPTSF_TFO_REQD) &&
		    (tp->t_tfo_stats == 0)) {
			tp->t_mpflags |= TMPF_TFO_REQUEST;
		}

		error = sock_sendmbuf(so, NULL, head, 0, NULL);
		head = NULL;
	}

done_sending:
	if (error == 0 ||
	    (error == EWOULDBLOCK && (tp->t_mpflags & TMPF_TFO_REQUEST))) {
		uint64_t new_sndnxt = mp_tp->mpt_sndnxt + tot_sent;

		if (mpts->mpts_probesoon && mpts->mpts_maxseg && tot_sent) {
			tcpstat.tcps_mp_num_probes++;
			if ((uint32_t)tot_sent < mpts->mpts_maxseg) {
				mpts->mpts_probecnt += 1;
			} else {
				mpts->mpts_probecnt +=
				    tot_sent / mpts->mpts_maxseg;
			}
		}

		if (!reinjected && !(flags & MPTCP_SUBOUT_PROBING)) {
			if (MPTCP_DATASEQ_HIGH32(new_sndnxt) >
			    MPTCP_DATASEQ_HIGH32(mp_tp->mpt_sndnxt)) {
				mp_tp->mpt_flags |= MPTCPF_SND_64BITDSN;
			}
			mp_tp->mpt_sndnxt = new_sndnxt;
		}

		mptcp_cancel_timer(mp_tp, MPTT_REXMT);

		/* Must be here as mptcp_can_send_more() checks for this */
		soclearfastopen(mp_so);

		if ((mpts->mpts_flags & MPTSF_MP_DEGRADED) ||
		    (mpts->mpts_probesoon != 0)) {
			mptcplog((LOG_DEBUG, "%s %u degraded %u wrote %d %d probe %d probedelta %d\n",
			    __func__, mpts->mpts_connid,
			    !!(mpts->mpts_flags & MPTSF_MP_DEGRADED),
			    tot_sent, (int) sb_cc, mpts->mpts_probecnt,
			    (tcp_now - mpts->mpts_probesoon)),
			    MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);
		}

		if (IFNET_IS_CELLULAR(sotoinpcb(so)->inp_last_outifp)) {
			mptcp_set_cellicon(mpte, mpts);

			mpte->mpte_used_cell = 1;
		} else {
			/*
			 * If during the past MPTCP_CELLICON_TOGGLE_RATE seconds we didn't
			 * explicitly set the cellicon, then we unset it again.
			 */
			if (TSTMP_LT(mpte->mpte_last_cellicon_set + MPTCP_CELLICON_TOGGLE_RATE, tcp_now)) {
				mptcp_unset_cellicon(mpte, NULL, 1);
			}

			mpte->mpte_used_wifi = 1;
		}

		/*
		 * Don't propagate EWOULDBLOCK - it's already taken care of
		 * in mptcp_usr_send for TFO.
		 */
		error = 0;
	} else {
		/* We need to revert our change to mpts_rel_seq */
		mpts->mpts_rel_seq -= tot_sent;

		os_log_error(mptcp_log_handle, "%s - %lx: %u error %d len %d subflags %#x sostate %#x soerror %u hiwat %u lowat %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpts->mpts_connid, error, tot_sent, so->so_flags, so->so_state, so->so_error, so->so_snd.sb_hiwat, so->so_snd.sb_lowat);
	}
out:

	if (head != NULL) {
		m_freem(head);
	}

	if (wakeup) {
		mpte->mpte_mppcb->mpp_flags |= MPP_SHOULD_WWAKEUP;
	}

	mptcp_handle_deferred_upcalls(mpte->mpte_mppcb, MPP_INSIDE_OUTPUT);
	return error;
}

static void
mptcp_add_reinjectq(struct mptses *mpte, struct mbuf *m)
{
	struct mbuf *n, *prev = NULL;

	mptcplog((LOG_DEBUG, "%s reinjecting dsn %u dlen %u rseq %u\n",
	    __func__, (uint32_t)m->m_pkthdr.mp_dsn, m->m_pkthdr.mp_rlen,
	    m->m_pkthdr.mp_rseq),
	    MPTCP_SOCKET_DBG, MPTCP_LOGLVL_VERBOSE);

	n = mpte->mpte_reinjectq;

	/* First, look for an mbuf n, whose data-sequence-number is bigger or
	 * equal than m's sequence number.
	 */
	while (n) {
		if (MPTCP_SEQ_GEQ(n->m_pkthdr.mp_dsn, m->m_pkthdr.mp_dsn)) {
			break;
		}

		prev = n;

		n = n->m_nextpkt;
	}

	if (n) {
		/* m is already fully covered by the next mbuf in the queue */
		if (n->m_pkthdr.mp_dsn == m->m_pkthdr.mp_dsn &&
		    n->m_pkthdr.mp_rlen >= m->m_pkthdr.mp_rlen) {
			os_log(mptcp_log_handle, "%s - %lx: dsn %u dlen %u rseq %u fully covered with len %u\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    (uint32_t)m->m_pkthdr.mp_dsn, m->m_pkthdr.mp_rlen,
			    m->m_pkthdr.mp_rseq, n->m_pkthdr.mp_rlen);
			goto dont_queue;
		}

		/* m is covering the next mbuf entirely, thus we remove this guy */
		if (m->m_pkthdr.mp_dsn + m->m_pkthdr.mp_rlen >= n->m_pkthdr.mp_dsn + n->m_pkthdr.mp_rlen) {
			struct mbuf *tmp = n->m_nextpkt;

			os_log(mptcp_log_handle, "%s - %lx: m (dsn %u len %u) is covering existing mbuf (dsn %u len %u)\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    (uint32_t)m->m_pkthdr.mp_dsn, m->m_pkthdr.mp_rlen,
			    (uint32_t)n->m_pkthdr.mp_dsn, n->m_pkthdr.mp_rlen);

			m->m_nextpkt = NULL;
			if (prev == NULL) {
				mpte->mpte_reinjectq = tmp;
			} else {
				prev->m_nextpkt = tmp;
			}

			m_freem(n);
			n = tmp;
		}
	}

	if (prev) {
		/* m is already fully covered by the previous mbuf in the queue */
		if (prev->m_pkthdr.mp_dsn + prev->m_pkthdr.mp_rlen >= m->m_pkthdr.mp_dsn + m->m_pkthdr.len) {
			os_log(mptcp_log_handle, "%s - %lx: prev (dsn %u len %u) covers us (dsn %u len %u)\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    (uint32_t)prev->m_pkthdr.mp_dsn, prev->m_pkthdr.mp_rlen,
			    (uint32_t)m->m_pkthdr.mp_dsn, m->m_pkthdr.mp_rlen);
			goto dont_queue;
		}
	}

	if (prev == NULL) {
		mpte->mpte_reinjectq = m;
	} else {
		prev->m_nextpkt = m;
	}

	m->m_nextpkt = n;

	return;

dont_queue:
	m_freem(m);
	return;
}

static struct mbuf *
mptcp_lookup_dsn(struct mptses *mpte, uint64_t dsn)
{
	struct socket *mp_so = mptetoso(mpte);
	struct mbuf *m;

	m = mp_so->so_snd.sb_mb;

	while (m) {
		/* If this segment covers what we are looking for, return it. */
		if (MPTCP_SEQ_LEQ(m->m_pkthdr.mp_dsn, dsn) &&
		    MPTCP_SEQ_GT(m->m_pkthdr.mp_dsn + m->m_pkthdr.mp_rlen, dsn)) {
			break;
		}


		/* Segment is no more in the queue */
		if (MPTCP_SEQ_GT(m->m_pkthdr.mp_dsn, dsn)) {
			return NULL;
		}

		m = m->m_next;
	}

	return m;
}

static struct mbuf *
mptcp_copy_mbuf_list(struct mptses *mpte, struct mbuf *m, int len)
{
	struct mbuf *top = NULL, *tail = NULL;
	uint64_t dsn;
	uint32_t dlen, rseq;

	dsn = m->m_pkthdr.mp_dsn;
	dlen = m->m_pkthdr.mp_rlen;
	rseq = m->m_pkthdr.mp_rseq;

	while (len > 0) {
		struct mbuf *n;

		VERIFY((m->m_flags & M_PKTHDR) && (m->m_pkthdr.pkt_flags & PKTF_MPTCP));

		n = m_copym_mode(m, 0, m->m_len, M_DONTWAIT, M_COPYM_MUST_COPY_HDR);
		if (n == NULL) {
			os_log_error(mptcp_log_handle, "%s - %lx: m_copym_mode returned NULL\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));
			goto err;
		}

		VERIFY(n->m_flags & M_PKTHDR);
		VERIFY(n->m_next == NULL);
		VERIFY(n->m_pkthdr.mp_dsn == dsn);
		VERIFY(n->m_pkthdr.mp_rlen == dlen);
		VERIFY(n->m_pkthdr.mp_rseq == rseq);
		VERIFY(n->m_len == m->m_len);

		n->m_pkthdr.pkt_flags |= (PKTF_MPSO | PKTF_MPTCP);

		if (top == NULL) {
			top = n;
		}

		if (tail != NULL) {
			tail->m_next = n;
		}

		tail = n;

		len -= m->m_len;
		m = m->m_next;
	}

	return top;

err:
	if (top) {
		m_freem(top);
	}

	return NULL;
}

static void
mptcp_reinject_mbufs(struct socket *so)
{
	struct tcpcb *tp = sototcpcb(so);
	struct mptsub *mpts = tp->t_mpsub;
	struct mptcb *mp_tp = tptomptp(tp);
	struct mptses *mpte = mp_tp->mpt_mpte;;
	struct sockbuf *sb = &so->so_snd;
	struct mbuf *m;

	m = sb->sb_mb;
	while (m) {
		struct mbuf *n = m->m_next, *orig = m;
		bool set_reinject_flag = false;

		mptcplog((LOG_DEBUG, "%s working on suna %u relseq %u iss %u len %u pktflags %#x\n",
		    __func__, tp->snd_una, m->m_pkthdr.mp_rseq, mpts->mpts_iss,
		    m->m_pkthdr.mp_rlen, m->m_pkthdr.pkt_flags),
		    MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);

		VERIFY((m->m_flags & M_PKTHDR) && (m->m_pkthdr.pkt_flags & PKTF_MPTCP));

		if (m->m_pkthdr.pkt_flags & PKTF_MPTCP_REINJ) {
			goto next;
		}

		/* Has it all already been acknowledged at the data-level? */
		if (MPTCP_SEQ_GEQ(mp_tp->mpt_snduna, m->m_pkthdr.mp_dsn + m->m_pkthdr.mp_rlen)) {
			goto next;
		}

		/* Part of this has already been acknowledged - lookup in the
		 * MPTCP-socket for the segment.
		 */
		if (SEQ_GT(tp->snd_una - mpts->mpts_iss, m->m_pkthdr.mp_rseq)) {
			m = mptcp_lookup_dsn(mpte, m->m_pkthdr.mp_dsn);
			if (m == NULL) {
				goto next;
			}
		}

		/* Copy the mbuf with headers (aka, DSN-numbers) */
		m = mptcp_copy_mbuf_list(mpte, m, m->m_pkthdr.mp_rlen);
		if (m == NULL) {
			break;
		}

		VERIFY(m->m_nextpkt == NULL);

		/* Now, add to the reinject-queue, eliminating overlapping
		 * segments
		 */
		mptcp_add_reinjectq(mpte, m);

		set_reinject_flag = true;
		orig->m_pkthdr.pkt_flags |= PKTF_MPTCP_REINJ;

next:
		/* mp_rlen can cover multiple mbufs, so advance to the end of it. */
		while (n) {
			VERIFY((n->m_flags & M_PKTHDR) && (n->m_pkthdr.pkt_flags & PKTF_MPTCP));

			if (n->m_pkthdr.mp_dsn != orig->m_pkthdr.mp_dsn) {
				break;
			}

			if (set_reinject_flag) {
				n->m_pkthdr.pkt_flags |= PKTF_MPTCP_REINJ;
			}
			n = n->m_next;
		}

		m = n;
	}
}

void
mptcp_clean_reinjectq(struct mptses *mpte)
{
	struct mptcb *mp_tp = mpte->mpte_mptcb;

	socket_lock_assert_owned(mptetoso(mpte));

	while (mpte->mpte_reinjectq) {
		struct mbuf *m = mpte->mpte_reinjectq;

		if (MPTCP_SEQ_GEQ(m->m_pkthdr.mp_dsn, mp_tp->mpt_snduna) ||
		    MPTCP_SEQ_GT(m->m_pkthdr.mp_dsn + m->m_pkthdr.mp_rlen, mp_tp->mpt_snduna)) {
			break;
		}

		mpte->mpte_reinjectq = m->m_nextpkt;
		m->m_nextpkt = NULL;
		m_freem(m);
	}
}

/*
 * Subflow socket control event upcall.
 */
static void
mptcp_subflow_eupcall1(struct socket *so, void *arg, long events)
{
#pragma unused(so)
	struct mptsub *mpts = arg;
	struct mptses *mpte = mpts->mpts_mpte;

	socket_lock_assert_owned(mptetoso(mpte));

	if ((mpts->mpts_evctl & events) == events) {
		return;
	}

	mpts->mpts_evctl |= events;

	if (mptcp_should_defer_upcall(mpte->mpte_mppcb)) {
		mpte->mpte_mppcb->mpp_flags |= MPP_SHOULD_WORKLOOP;
		return;
	}

	mptcp_subflow_workloop(mpte);
}

/*
 * Subflow socket control events.
 *
 * Called for handling events related to the underlying subflow socket.
 */
static ev_ret_t
mptcp_subflow_events(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint)
{
	ev_ret_t ret = MPTS_EVRET_OK;
	int i, mpsub_ev_entry_count = sizeof(mpsub_ev_entry_tbl) /
	    sizeof(mpsub_ev_entry_tbl[0]);

	/* bail if there's nothing to process */
	if (!mpts->mpts_evctl) {
		return ret;
	}

	if (mpts->mpts_evctl & (SO_FILT_HINT_CONNRESET | SO_FILT_HINT_MUSTRST |
	    SO_FILT_HINT_CANTSENDMORE | SO_FILT_HINT_TIMEOUT |
	    SO_FILT_HINT_NOSRCADDR | SO_FILT_HINT_IFDENIED |
	    SO_FILT_HINT_DISCONNECTED)) {
		mpts->mpts_evctl |= SO_FILT_HINT_MPFAILOVER;
	}

	DTRACE_MPTCP3(subflow__events, struct mptses *, mpte,
	    struct mptsub *, mpts, uint32_t, mpts->mpts_evctl);

	/*
	 * Process all the socket filter hints and reset the hint
	 * once it is handled
	 */
	for (i = 0; i < mpsub_ev_entry_count && mpts->mpts_evctl; i++) {
		/*
		 * Always execute the DISCONNECTED event, because it will wakeup
		 * the app.
		 */
		if ((mpts->mpts_evctl & mpsub_ev_entry_tbl[i].sofilt_hint_mask) &&
		    (ret >= MPTS_EVRET_OK ||
		    mpsub_ev_entry_tbl[i].sofilt_hint_mask == SO_FILT_HINT_DISCONNECTED)) {
			mpts->mpts_evctl &= ~mpsub_ev_entry_tbl[i].sofilt_hint_mask;
			ev_ret_t error =
			    mpsub_ev_entry_tbl[i].sofilt_hint_ev_hdlr(mpte, mpts, p_mpsofilt_hint, mpsub_ev_entry_tbl[i].sofilt_hint_mask);
			ret = ((error >= MPTS_EVRET_OK) ? MAX(error, ret) : error);
		}
	}

	return ret;
}

static ev_ret_t
mptcp_subflow_propagate_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
	struct socket *mp_so, *so;
	struct mptcb *mp_tp;

	mp_so = mptetoso(mpte);
	mp_tp = mpte->mpte_mptcb;
	so = mpts->mpts_socket;

	/*
	 * We got an event for this subflow that might need to be propagated,
	 * based on the state of the MPTCP connection.
	 */
	if (mp_tp->mpt_state < MPTCPS_ESTABLISHED ||
	    (!(mp_tp->mpt_flags & MPTCPF_JOIN_READY) && !(mpts->mpts_flags & MPTSF_MP_READY)) ||
	    ((mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP) && (mpts->mpts_flags & MPTSF_ACTIVE))) {
		mp_so->so_error = so->so_error;
		*p_mpsofilt_hint |= event;
	}

	return MPTS_EVRET_OK;
}

/*
 * Handle SO_FILT_HINT_NOSRCADDR subflow socket event.
 */
static ev_ret_t
mptcp_subflow_nosrcaddr_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(p_mpsofilt_hint, event)
	struct socket *mp_so;
	struct tcpcb *tp;

	mp_so = mptetoso(mpte);
	tp = intotcpcb(sotoinpcb(mpts->mpts_socket));

	/*
	 * This overwrites any previous mpte_lost_aid to avoid storing
	 * too much state when the typical case has only two subflows.
	 */
	mpte->mpte_flags |= MPTE_SND_REM_ADDR;
	mpte->mpte_lost_aid = tp->t_local_aid;

	mptcplog((LOG_DEBUG, "%s cid %d\n", __func__, mpts->mpts_connid),
	    MPTCP_EVENTS_DBG, MPTCP_LOGLVL_LOG);

	/*
	 * The subflow connection has lost its source address.
	 */
	mptcp_subflow_abort(mpts, EADDRNOTAVAIL);

	if (mp_so->so_flags & SOF_NOADDRAVAIL) {
		mptcp_subflow_propagate_ev(mpte, mpts, p_mpsofilt_hint, event);
	}

	return MPTS_EVRET_DELETE;
}

static ev_ret_t
mptcp_subflow_mpsuberror_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event, p_mpsofilt_hint)
	struct socket *so, *mp_so;

	so = mpts->mpts_socket;

	if (so->so_error != ENODATA) {
		return MPTS_EVRET_OK;
	}


	mp_so = mptetoso(mpte);

	mp_so->so_error = ENODATA;

	sorwakeup(mp_so);
	sowwakeup(mp_so);

	return MPTS_EVRET_OK;
}


/*
 * Handle SO_FILT_HINT_MPCANTRCVMORE subflow socket event that
 * indicates that the remote side sent a Data FIN
 */
static ev_ret_t
mptcp_subflow_mpcantrcvmore_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event)
	struct mptcb *mp_tp = mpte->mpte_mptcb;

	mptcplog((LOG_DEBUG, "%s: cid %d\n", __func__, mpts->mpts_connid),
	    MPTCP_EVENTS_DBG, MPTCP_LOGLVL_LOG);

	/*
	 * We got a Data FIN for the MPTCP connection.
	 * The FIN may arrive with data. The data is handed up to the
	 * mptcp socket and the user is notified so that it may close
	 * the socket if needed.
	 */
	if (mp_tp->mpt_state == MPTCPS_CLOSE_WAIT) {
		*p_mpsofilt_hint |= SO_FILT_HINT_CANTRCVMORE;
	}

	return MPTS_EVRET_OK; /* keep the subflow socket around */
}

/*
 * Handle SO_FILT_HINT_MPFAILOVER subflow socket event
 */
static ev_ret_t
mptcp_subflow_failover_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event, p_mpsofilt_hint)
	struct mptsub *mpts_alt = NULL;
	struct socket *alt_so = NULL;
	struct socket *mp_so;
	int altpath_exists = 0;

	mp_so = mptetoso(mpte);
	os_log_info(mptcp_log_handle, "%s - %lx\n", __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));

	mptcp_reinject_mbufs(mpts->mpts_socket);

	mpts_alt = mptcp_get_subflow(mpte, NULL);

	/* If there is no alternate eligible subflow, ignore the failover hint. */
	if (mpts_alt == NULL || mpts_alt == mpts) {
		os_log(mptcp_log_handle, "%s - %lx no alternate path\n", __func__,
		    (unsigned long)VM_KERNEL_ADDRPERM(mpte));

		goto done;
	}

	altpath_exists = 1;
	alt_so = mpts_alt->mpts_socket;
	if (mpts_alt->mpts_flags & MPTSF_FAILINGOVER) {
		/* All data acknowledged and no RTT spike */
		if (alt_so->so_snd.sb_cc == 0 && mptcp_no_rto_spike(alt_so)) {
			mpts_alt->mpts_flags &= ~MPTSF_FAILINGOVER;
		} else {
			/* no alternate path available */
			altpath_exists = 0;
		}
	}

	if (altpath_exists) {
		mpts_alt->mpts_flags |= MPTSF_ACTIVE;

		mpte->mpte_active_sub = mpts_alt;
		mpts->mpts_flags |= MPTSF_FAILINGOVER;
		mpts->mpts_flags &= ~MPTSF_ACTIVE;

		os_log_info(mptcp_log_handle, "%s - %lx: switched from %d to %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpts->mpts_connid, mpts_alt->mpts_connid);

		mptcpstats_inc_switch(mpte, mpts);

		sowwakeup(alt_so);
	} else {
		mptcplog((LOG_DEBUG, "%s: no alt cid = %d\n", __func__,
		    mpts->mpts_connid),
		    MPTCP_EVENTS_DBG, MPTCP_LOGLVL_LOG);
done:
		mpts->mpts_socket->so_flags &= ~SOF_MP_TRYFAILOVER;
	}

	return MPTS_EVRET_OK;
}

/*
 * Handle SO_FILT_HINT_IFDENIED subflow socket event.
 */
static ev_ret_t
mptcp_subflow_ifdenied_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
	mptcplog((LOG_DEBUG, "%s: cid %d\n", __func__,
	    mpts->mpts_connid), MPTCP_EVENTS_DBG, MPTCP_LOGLVL_LOG);

	/*
	 * The subflow connection cannot use the outgoing interface, let's
	 * close this subflow.
	 */
	mptcp_subflow_abort(mpts, EPERM);

	mptcp_subflow_propagate_ev(mpte, mpts, p_mpsofilt_hint, event);

	return MPTS_EVRET_DELETE;
}

/*
 * https://tools.ietf.org/html/rfc6052#section-2
 * https://tools.ietf.org/html/rfc6147#section-5.2
 */
static boolean_t
mptcp_desynthesize_ipv6_addr(const struct in6_addr *addr,
    const struct ipv6_prefix *prefix,
    struct in_addr *addrv4)
{
	char buf[MAX_IPv4_STR_LEN];
	char *ptrv4 = (char *)addrv4;
	const char *ptr = (const char *)addr;

	if (memcmp(addr, &prefix->ipv6_prefix, prefix->prefix_len) != 0) {
		return false;
	}

	switch (prefix->prefix_len) {
	case NAT64_PREFIX_LEN_96:
		memcpy(ptrv4, ptr + 12, 4);
		break;
	case NAT64_PREFIX_LEN_64:
		memcpy(ptrv4, ptr + 9, 4);
		break;
	case NAT64_PREFIX_LEN_56:
		memcpy(ptrv4, ptr + 7, 1);
		memcpy(ptrv4 + 1, ptr + 9, 3);
		break;
	case NAT64_PREFIX_LEN_48:
		memcpy(ptrv4, ptr + 6, 2);
		memcpy(ptrv4 + 2, ptr + 9, 2);
		break;
	case NAT64_PREFIX_LEN_40:
		memcpy(ptrv4, ptr + 5, 3);
		memcpy(ptrv4 + 3, ptr + 9, 1);
		break;
	case NAT64_PREFIX_LEN_32:
		memcpy(ptrv4, ptr + 4, 4);
		break;
	default:
		panic("NAT64-prefix len is wrong: %u\n",
		    prefix->prefix_len);
	}

	os_log_info(mptcp_log_handle, "%s desynthesized to %s\n", __func__,
	    inet_ntop(AF_INET, (void *)addrv4, buf, sizeof(buf)));

	return true;
}

static void
mptcp_handle_ipv6_connection(struct mptses *mpte, const struct mptsub *mpts)
{
	struct ipv6_prefix nat64prefixes[NAT64_MAX_NUM_PREFIXES];
	struct socket *so = mpts->mpts_socket;
	struct ifnet *ifp;
	int j;

	/* Subflow IPs will be steered directly by the server - no need to
	 * desynthesize.
	 */
	if (mpte->mpte_flags & MPTE_UNICAST_IP) {
		return;
	}

	ifp = sotoinpcb(so)->inp_last_outifp;

	if (ifnet_get_nat64prefix(ifp, nat64prefixes) == ENOENT) {
		return;
	}

	for (j = 0; j < NAT64_MAX_NUM_PREFIXES; j++) {
		int success;

		if (nat64prefixes[j].prefix_len == 0) {
			continue;
		}

		success = mptcp_desynthesize_ipv6_addr(&mpte->__mpte_dst_v6.sin6_addr,
		    &nat64prefixes[j],
		    &mpte->mpte_sub_dst_v4.sin_addr);
		if (success) {
			mpte->mpte_sub_dst_v4.sin_len = sizeof(mpte->mpte_sub_dst_v4);
			mpte->mpte_sub_dst_v4.sin_family = AF_INET;
			mpte->mpte_sub_dst_v4.sin_port = mpte->__mpte_dst_v6.sin6_port;
			break;
		}
	}
}

static void
mptcp_try_alternate_port(struct mptses *mpte, struct mptsub *mpts)
{
	struct inpcb *inp;

	if (!mptcp_ok_to_create_subflows(mpte->mpte_mptcb)) {
		return;
	}

	inp = sotoinpcb(mpts->mpts_socket);
	if (inp == NULL) {
		return;
	}

	/* Should we try the alternate port? */
	if (mpte->mpte_alternate_port &&
	    inp->inp_fport != mpte->mpte_alternate_port) {
		union sockaddr_in_4_6 dst;
		struct sockaddr_in *dst_in = (struct sockaddr_in *)&dst;

		memcpy(&dst, &mpts->mpts_dst, mpts->mpts_dst.sa_len);

		dst_in->sin_port = mpte->mpte_alternate_port;

		mptcp_subflow_add(mpte, NULL, (struct sockaddr *)&dst,
		    mpts->mpts_ifscope, NULL);
	} else { /* Else, we tried all we could, mark this interface as non-MPTCP */
		unsigned int i;

		if (inp->inp_last_outifp == NULL) {
			return;
		}

		for (i = 0; i < mpte->mpte_itfinfo_size; i++) {
			struct mpt_itf_info *info =  &mpte->mpte_itfinfo[i];

			if (inp->inp_last_outifp->if_index == info->ifindex) {
				info->no_mptcp_support = 1;
				break;
			}
		}
	}
}

/*
 * Handle SO_FILT_HINT_CONNECTED subflow socket event.
 */
static ev_ret_t
mptcp_subflow_connected_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event, p_mpsofilt_hint)
	struct socket *mp_so, *so;
	struct inpcb *inp;
	struct tcpcb *tp;
	struct mptcb *mp_tp;
	int af;
	boolean_t mpok = FALSE;

	mp_so = mptetoso(mpte);
	mp_tp = mpte->mpte_mptcb;
	so = mpts->mpts_socket;
	tp = sototcpcb(so);
	af = mpts->mpts_dst.sa_family;

	if (mpts->mpts_flags & MPTSF_CONNECTED) {
		return MPTS_EVRET_OK;
	}

	if ((mpts->mpts_flags & MPTSF_DISCONNECTED) ||
	    (mpts->mpts_flags & MPTSF_DISCONNECTING)) {
		if (!(so->so_state & (SS_ISDISCONNECTING | SS_ISDISCONNECTED)) &&
		    (so->so_state & SS_ISCONNECTED)) {
			mptcplog((LOG_DEBUG, "%s: cid %d disconnect before tcp connect\n",
			    __func__, mpts->mpts_connid),
			    MPTCP_EVENTS_DBG, MPTCP_LOGLVL_LOG);
			(void) soshutdownlock(so, SHUT_RD);
			(void) soshutdownlock(so, SHUT_WR);
			(void) sodisconnectlocked(so);
		}
		return MPTS_EVRET_OK;
	}

	/*
	 * The subflow connection has been connected.  Find out whether it
	 * is connected as a regular TCP or as a MPTCP subflow.  The idea is:
	 *
	 *   a. If MPTCP connection is not yet established, then this must be
	 *	the first subflow connection.  If MPTCP failed to negotiate,
	 *	fallback to regular TCP by degrading this subflow.
	 *
	 *   b. If MPTCP connection has been established, then this must be
	 *	one of the subsequent subflow connections. If MPTCP failed
	 *	to negotiate, disconnect the connection.
	 *
	 * Right now, we simply unblock any waiters at the MPTCP socket layer
	 * if the MPTCP connection has not been established.
	 */

	if (so->so_state & SS_ISDISCONNECTED) {
		/*
		 * With MPTCP joins, a connection is connected at the subflow
		 * level, but the 4th ACK from the server elevates the MPTCP
		 * subflow to connected state. So there is a small window
		 * where the subflow could get disconnected before the
		 * connected event is processed.
		 */
		return MPTS_EVRET_OK;
	}

	if (mpts->mpts_flags & MPTSF_TFO_REQD) {
		mptcp_drop_tfo_data(mpte, mpts);
	}

	mpts->mpts_flags &= ~(MPTSF_CONNECTING | MPTSF_TFO_REQD);
	mpts->mpts_flags |= MPTSF_CONNECTED;

	if (tp->t_mpflags & TMPF_MPTCP_TRUE) {
		mpts->mpts_flags |= MPTSF_MP_CAPABLE;
	}

	tp->t_mpflags &= ~TMPF_TFO_REQUEST;

	/* get/verify the outbound interface */
	inp = sotoinpcb(so);

	mpts->mpts_maxseg = tp->t_maxseg;

	mptcplog((LOG_DEBUG, "%s: cid %d outif %s is %s\n", __func__, mpts->mpts_connid,
	    ((inp->inp_last_outifp != NULL) ? inp->inp_last_outifp->if_xname : "NULL"),
	    ((mpts->mpts_flags & MPTSF_MP_CAPABLE) ? "MPTCP capable" : "a regular TCP")),
	    (MPTCP_SOCKET_DBG | MPTCP_EVENTS_DBG), MPTCP_LOGLVL_LOG);

	mpok = (mpts->mpts_flags & MPTSF_MP_CAPABLE);

	if (mp_tp->mpt_state < MPTCPS_ESTABLISHED) {
		mp_tp->mpt_state = MPTCPS_ESTABLISHED;
		mpte->mpte_associd = mpts->mpts_connid;
		DTRACE_MPTCP2(state__change,
		    struct mptcb *, mp_tp,
		    uint32_t, 0 /* event */);

		if (SOCK_DOM(so) == AF_INET) {
			in_getsockaddr_s(so, &mpte->__mpte_src_v4);
		} else {
			in6_getsockaddr_s(so, &mpte->__mpte_src_v6);
		}

		mpts->mpts_flags |= MPTSF_ACTIVE;

		/* case (a) above */
		if (!mpok) {
			tcpstat.tcps_mpcap_fallback++;

			tp->t_mpflags |= TMPF_INFIN_SENT;
			mptcp_notify_mpfail(so);
		} else {
			if (IFNET_IS_CELLULAR(inp->inp_last_outifp) &&
			    mptcp_subflows_need_backup_flag(mpte)) {
				tp->t_mpflags |= (TMPF_BACKUP_PATH | TMPF_SND_MPPRIO);
			} else {
				mpts->mpts_flags |= MPTSF_PREFERRED;
			}
			mpts->mpts_flags |= MPTSF_MPCAP_CTRSET;
			mpte->mpte_nummpcapflows++;

			if (SOCK_DOM(so) == AF_INET6) {
				mptcp_handle_ipv6_connection(mpte, mpts);
			}

			mptcp_check_subflows_and_add(mpte);

			if (IFNET_IS_CELLULAR(inp->inp_last_outifp)) {
				mpte->mpte_initial_cell = 1;
			}

			mpte->mpte_handshake_success = 1;
		}

		mp_tp->mpt_sndwnd = tp->snd_wnd;
		mp_tp->mpt_sndwl1 = mp_tp->mpt_rcvnxt;
		mp_tp->mpt_sndwl2 = mp_tp->mpt_snduna;
		soisconnected(mp_so);
	} else if (mpok) {
		/*
		 * case (b) above
		 * In case of additional flows, the MPTCP socket is not
		 * MPTSF_MP_CAPABLE until an ACK is received from server
		 * for 3-way handshake.  TCP would have guaranteed that this
		 * is an MPTCP subflow.
		 */
		if (IFNET_IS_CELLULAR(inp->inp_last_outifp) &&
		    !(tp->t_mpflags & TMPF_BACKUP_PATH) &&
		    mptcp_subflows_need_backup_flag(mpte)) {
			tp->t_mpflags |= (TMPF_BACKUP_PATH | TMPF_SND_MPPRIO);
			mpts->mpts_flags &= ~MPTSF_PREFERRED;
		} else {
			mpts->mpts_flags |= MPTSF_PREFERRED;
		}

		mpts->mpts_flags |= MPTSF_MPCAP_CTRSET;
		mpte->mpte_nummpcapflows++;

		mpts->mpts_rel_seq = 1;

		mptcp_check_subflows_and_remove(mpte);
	} else {
		mptcp_try_alternate_port(mpte, mpts);

		tcpstat.tcps_join_fallback++;
		if (IFNET_IS_CELLULAR(inp->inp_last_outifp)) {
			tcpstat.tcps_mptcp_cell_proxy++;
		} else {
			tcpstat.tcps_mptcp_wifi_proxy++;
		}

		soevent(mpts->mpts_socket, SO_FILT_HINT_LOCKED | SO_FILT_HINT_MUSTRST);

		return MPTS_EVRET_OK;
	}

	/* This call, just to "book" an entry in the stats-table for this ifindex */
	mptcpstats_get_index(mpte->mpte_itfstats, mpts);

	mptcp_output(mpte);

	return MPTS_EVRET_OK; /* keep the subflow socket around */
}

/*
 * Handle SO_FILT_HINT_DISCONNECTED subflow socket event.
 */
static ev_ret_t
mptcp_subflow_disconnected_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event, p_mpsofilt_hint)
	struct socket *mp_so, *so;
	struct mptcb *mp_tp;

	mp_so = mptetoso(mpte);
	mp_tp = mpte->mpte_mptcb;
	so = mpts->mpts_socket;

	mptcplog((LOG_DEBUG, "%s: cid %d, so_err %d, mpt_state %u fallback %u active %u flags %#x\n",
	    __func__, mpts->mpts_connid, so->so_error, mp_tp->mpt_state,
	    !!(mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP),
	    !!(mpts->mpts_flags & MPTSF_ACTIVE), sototcpcb(so)->t_mpflags),
	    MPTCP_EVENTS_DBG, MPTCP_LOGLVL_LOG);

	if (mpts->mpts_flags & MPTSF_DISCONNECTED) {
		return MPTS_EVRET_DELETE;
	}

	mpts->mpts_flags |= MPTSF_DISCONNECTED;

	/* The subflow connection has been disconnected. */

	if (mpts->mpts_flags & MPTSF_MPCAP_CTRSET) {
		mpte->mpte_nummpcapflows--;
		if (mpte->mpte_active_sub == mpts) {
			mpte->mpte_active_sub = NULL;
			mptcplog((LOG_DEBUG, "%s: resetting active subflow \n",
			    __func__), MPTCP_EVENTS_DBG, MPTCP_LOGLVL_LOG);
		}
		mpts->mpts_flags &= ~MPTSF_MPCAP_CTRSET;
	} else {
		if (so->so_flags & SOF_MP_SEC_SUBFLOW &&
		    !(mpts->mpts_flags & MPTSF_CONNECTED)) {
			mptcp_try_alternate_port(mpte, mpts);
		}
	}

	if (mp_tp->mpt_state < MPTCPS_ESTABLISHED ||
	    ((mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP) && (mpts->mpts_flags & MPTSF_ACTIVE))) {
		mptcp_drop(mpte, mp_tp, so->so_error);
	}

	/*
	 * Clear flags that are used by getconninfo to return state.
	 * Retain like MPTSF_DELETEOK for internal purposes.
	 */
	mpts->mpts_flags &= ~(MPTSF_CONNECTING | MPTSF_CONNECT_PENDING |
	    MPTSF_CONNECTED | MPTSF_DISCONNECTING | MPTSF_PREFERRED |
	    MPTSF_MP_CAPABLE | MPTSF_MP_READY | MPTSF_MP_DEGRADED | MPTSF_ACTIVE);

	return MPTS_EVRET_DELETE;
}

/*
 * Handle SO_FILT_HINT_MPSTATUS subflow socket event
 */
static ev_ret_t
mptcp_subflow_mpstatus_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event, p_mpsofilt_hint)
	ev_ret_t ret = MPTS_EVRET_OK;
	struct socket *mp_so, *so;
	struct mptcb *mp_tp;

	mp_so = mptetoso(mpte);
	mp_tp = mpte->mpte_mptcb;
	so = mpts->mpts_socket;

	if (sototcpcb(so)->t_mpflags & TMPF_MPTCP_TRUE) {
		mpts->mpts_flags |= MPTSF_MP_CAPABLE;
	} else {
		mpts->mpts_flags &= ~MPTSF_MP_CAPABLE;
	}

	if (sototcpcb(so)->t_mpflags & TMPF_TCP_FALLBACK) {
		if (mpts->mpts_flags & MPTSF_MP_DEGRADED) {
			goto done;
		}
		mpts->mpts_flags |= MPTSF_MP_DEGRADED;
	} else {
		mpts->mpts_flags &= ~MPTSF_MP_DEGRADED;
	}

	if (sototcpcb(so)->t_mpflags & TMPF_MPTCP_READY) {
		mpts->mpts_flags |= MPTSF_MP_READY;
	} else {
		mpts->mpts_flags &= ~MPTSF_MP_READY;
	}

	if (mpts->mpts_flags & MPTSF_MP_DEGRADED) {
		mp_tp->mpt_flags |= MPTCPF_FALLBACK_TO_TCP;
		mp_tp->mpt_flags &= ~MPTCPF_JOIN_READY;
	}

	if (mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP) {
		ret = MPTS_EVRET_DISCONNECT_FALLBACK;

		m_freem_list(mpte->mpte_reinjectq);
		mpte->mpte_reinjectq = NULL;
	} else if (mpts->mpts_flags & MPTSF_MP_READY) {
		mp_tp->mpt_flags |= MPTCPF_JOIN_READY;
		ret = MPTS_EVRET_CONNECT_PENDING;
	}

done:
	return ret;
}

/*
 * Handle SO_FILT_HINT_MUSTRST subflow socket event
 */
static ev_ret_t
mptcp_subflow_mustrst_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event)
	struct socket *mp_so, *so;
	struct mptcb *mp_tp;
	boolean_t is_fastclose;

	mp_so = mptetoso(mpte);
	mp_tp = mpte->mpte_mptcb;
	so = mpts->mpts_socket;

	/* We got an invalid option or a fast close */
	struct inpcb *inp = sotoinpcb(so);
	struct tcpcb *tp = NULL;

	tp = intotcpcb(inp);
	so->so_error = ECONNABORTED;

	is_fastclose = !!(tp->t_mpflags & TMPF_FASTCLOSERCV);

	tp->t_mpflags |= TMPF_RESET;

	if (tp->t_state != TCPS_CLOSED) {
		struct tcptemp *t_template = tcp_maketemplate(tp);

		if (t_template) {
			struct tcp_respond_args tra;

			bzero(&tra, sizeof(tra));
			if (inp->inp_flags & INP_BOUND_IF) {
				tra.ifscope = inp->inp_boundifp->if_index;
			} else {
				tra.ifscope = IFSCOPE_NONE;
			}
			tra.awdl_unrestricted = 1;

			tcp_respond(tp, t_template->tt_ipgen,
			    &t_template->tt_t, (struct mbuf *)NULL,
			    tp->rcv_nxt, tp->snd_una, TH_RST, &tra);
			(void) m_free(dtom(t_template));
		}
	}

	if (!(mp_tp->mpt_flags & MPTCPF_FALLBACK_TO_TCP) && is_fastclose) {
		struct mptsub *iter, *tmp;

		*p_mpsofilt_hint |= SO_FILT_HINT_CONNRESET;

		mp_so->so_error = ECONNRESET;

		TAILQ_FOREACH_SAFE(iter, &mpte->mpte_subflows, mpts_entry, tmp) {
			if (iter == mpts) {
				continue;
			}
			mptcp_subflow_abort(iter, ECONNABORTED);
		}

		/*
		 * mptcp_drop is being called after processing the events, to fully
		 * close the MPTCP connection
		 */
		mptcp_drop(mpte, mp_tp, mp_so->so_error);
	}

	mptcp_subflow_abort(mpts, ECONNABORTED);

	if (mp_tp->mpt_gc_ticks == MPT_GC_TICKS) {
		mp_tp->mpt_gc_ticks = MPT_GC_TICKS_FAST;
	}

	return MPTS_EVRET_DELETE;
}

static ev_ret_t
mptcp_subflow_adaptive_rtimo_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event)
	bool found_active = false;

	mpts->mpts_flags |= MPTSF_READ_STALL;

	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		struct tcpcb *tp = sototcpcb(mpts->mpts_socket);

		if (!TCPS_HAVEESTABLISHED(tp->t_state) ||
		    TCPS_HAVERCVDFIN2(tp->t_state)) {
			continue;
		}

		if (!(mpts->mpts_flags & MPTSF_READ_STALL)) {
			found_active = true;
			break;
		}
	}

	if (!found_active) {
		*p_mpsofilt_hint |= SO_FILT_HINT_ADAPTIVE_RTIMO;
	}

	return MPTS_EVRET_OK;
}

static ev_ret_t
mptcp_subflow_adaptive_wtimo_ev(struct mptses *mpte, struct mptsub *mpts,
    long *p_mpsofilt_hint, long event)
{
#pragma unused(event)
	bool found_active = false;

	mpts->mpts_flags |= MPTSF_WRITE_STALL;

	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		struct tcpcb *tp = sototcpcb(mpts->mpts_socket);

		if (!TCPS_HAVEESTABLISHED(tp->t_state) ||
		    tp->t_state > TCPS_CLOSE_WAIT) {
			continue;
		}

		if (!(mpts->mpts_flags & MPTSF_WRITE_STALL)) {
			found_active = true;
			break;
		}
	}

	if (!found_active) {
		*p_mpsofilt_hint |= SO_FILT_HINT_ADAPTIVE_WTIMO;
	}

	return MPTS_EVRET_OK;
}

/*
 * Issues SOPT_SET on an MPTCP subflow socket; socket must already be locked,
 * caller must ensure that the option can be issued on subflow sockets, via
 * MPOF_SUBFLOW_OK flag.
 */
int
mptcp_subflow_sosetopt(struct mptses *mpte, struct mptsub *mpts, struct mptopt *mpo)
{
	struct socket *mp_so, *so;
	struct sockopt sopt;
	int error;

	VERIFY(mpo->mpo_flags & MPOF_SUBFLOW_OK);

	mp_so = mptetoso(mpte);
	so = mpts->mpts_socket;

	socket_lock_assert_owned(mp_so);

	if (mpte->mpte_mptcb->mpt_state >= MPTCPS_ESTABLISHED &&
	    mpo->mpo_level == SOL_SOCKET &&
	    mpo->mpo_name == SO_MARK_CELLFALLBACK) {
		struct ifnet *ifp = ifindex2ifnet[mpts->mpts_ifscope];

		mptcplog((LOG_DEBUG, "%s Setting CELL_FALLBACK, mpte_flags %#x, svctype %u wifi unusable %d lastcell? %d boundcell? %d\n",
		    __func__, mpte->mpte_flags, mpte->mpte_svctype, mptcp_is_wifi_unusable_for_session(mpte),
		    sotoinpcb(so)->inp_last_outifp ? IFNET_IS_CELLULAR(sotoinpcb(so)->inp_last_outifp) : -1,
		    mpts->mpts_ifscope != IFSCOPE_NONE && ifp ? IFNET_IS_CELLULAR(ifp) : -1),
		    MPTCP_SOCKET_DBG, MPTCP_LOGLVL_VERBOSE);

		/*
		 * When we open a new subflow, mark it as cell fallback, if
		 * this subflow goes over cell.
		 *
		 * (except for first-party apps)
		 */

		if (mpte->mpte_flags & MPTE_FIRSTPARTY) {
			return 0;
		}

		if (sotoinpcb(so)->inp_last_outifp &&
		    !IFNET_IS_CELLULAR(sotoinpcb(so)->inp_last_outifp)) {
			return 0;
		}

		/*
		 * This here is an OR, because if the app is not binding to the
		 * interface, then it definitely is not a cell-fallback
		 * connection.
		 */
		if (mpts->mpts_ifscope == IFSCOPE_NONE || ifp == NULL ||
		    !IFNET_IS_CELLULAR(ifp)) {
			return 0;
		}
	}

	mpo->mpo_flags &= ~MPOF_INTERIM;

	bzero(&sopt, sizeof(sopt));
	sopt.sopt_dir = SOPT_SET;
	sopt.sopt_level = mpo->mpo_level;
	sopt.sopt_name = mpo->mpo_name;
	sopt.sopt_val = CAST_USER_ADDR_T(&mpo->mpo_intval);
	sopt.sopt_valsize = sizeof(int);
	sopt.sopt_p = kernproc;

	error = sosetoptlock(so, &sopt, 0);
	if (error) {
		os_log_error(mptcp_log_handle, "%s - %lx: sopt %s "
		    "val %d set error %d\n", __func__,
		    (unsigned long)VM_KERNEL_ADDRPERM(mpte),
		    mptcp_sopt2str(mpo->mpo_level, mpo->mpo_name),
		    mpo->mpo_intval, error);
	}
	return error;
}

/*
 * Issues SOPT_GET on an MPTCP subflow socket; socket must already be locked,
 * caller must ensure that the option can be issued on subflow sockets, via
 * MPOF_SUBFLOW_OK flag.
 */
int
mptcp_subflow_sogetopt(struct mptses *mpte, struct socket *so,
    struct mptopt *mpo)
{
	struct socket *mp_so;
	struct sockopt sopt;
	int error;

	VERIFY(mpo->mpo_flags & MPOF_SUBFLOW_OK);
	mp_so = mptetoso(mpte);

	socket_lock_assert_owned(mp_so);

	bzero(&sopt, sizeof(sopt));
	sopt.sopt_dir = SOPT_GET;
	sopt.sopt_level = mpo->mpo_level;
	sopt.sopt_name = mpo->mpo_name;
	sopt.sopt_val = CAST_USER_ADDR_T(&mpo->mpo_intval);
	sopt.sopt_valsize = sizeof(int);
	sopt.sopt_p = kernproc;

	error = sogetoptlock(so, &sopt, 0);     /* already locked */
	if (error) {
		os_log_error(mptcp_log_handle,
		    "%s - %lx: sopt %s get error %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte),
		    mptcp_sopt2str(mpo->mpo_level, mpo->mpo_name), error);
	}
	return error;
}


/*
 * MPTCP garbage collector.
 *
 * This routine is called by the MP domain on-demand, periodic callout,
 * which is triggered when a MPTCP socket is closed.  The callout will
 * repeat as long as this routine returns a non-zero value.
 */
static uint32_t
mptcp_gc(struct mppcbinfo *mppi)
{
	struct mppcb *mpp, *tmpp;
	uint32_t active = 0;

	LCK_MTX_ASSERT(&mppi->mppi_lock, LCK_MTX_ASSERT_OWNED);

	TAILQ_FOREACH_SAFE(mpp, &mppi->mppi_pcbs, mpp_entry, tmpp) {
		struct socket *mp_so;
		struct mptses *mpte;
		struct mptcb *mp_tp;

		mp_so = mpp->mpp_socket;
		mpte = mptompte(mpp);
		mp_tp = mpte->mpte_mptcb;

		if (!mpp_try_lock(mpp)) {
			active++;
			continue;
		}

		VERIFY(mpp->mpp_flags & MPP_ATTACHED);

		/* check again under the lock */
		if (mp_so->so_usecount > 0) {
			boolean_t wakeup = FALSE;
			struct mptsub *mpts, *tmpts;

			if (mp_tp->mpt_state >= MPTCPS_FIN_WAIT_1) {
				if (mp_tp->mpt_gc_ticks > 0) {
					mp_tp->mpt_gc_ticks--;
				}
				if (mp_tp->mpt_gc_ticks == 0) {
					wakeup = TRUE;
				}
			}
			if (wakeup) {
				TAILQ_FOREACH_SAFE(mpts,
				    &mpte->mpte_subflows, mpts_entry, tmpts) {
					mptcp_subflow_eupcall1(mpts->mpts_socket,
					    mpts, SO_FILT_HINT_DISCONNECTED);
				}
			}
			socket_unlock(mp_so, 0);
			active++;
			continue;
		}

		if (mpp->mpp_state != MPPCB_STATE_DEAD) {
			panic("%s - %lx: skipped state "
			    "[u=%d,r=%d,s=%d]\n", __func__,
			    (unsigned long)VM_KERNEL_ADDRPERM(mpte),
			    mp_so->so_usecount, mp_so->so_retaincnt,
			    mpp->mpp_state);
		}

		if (mp_tp->mpt_state == MPTCPS_TIME_WAIT) {
			mptcp_close(mpte, mp_tp);
		}

		mptcp_session_destroy(mpte);

		DTRACE_MPTCP4(dispose, struct socket *, mp_so,
		    struct sockbuf *, &mp_so->so_rcv,
		    struct sockbuf *, &mp_so->so_snd,
		    struct mppcb *, mpp);

		mptcp_pcbdispose(mpp);
		sodealloc(mp_so);
	}

	return active;
}

/*
 * Drop a MPTCP connection, reporting the specified error.
 */
struct mptses *
mptcp_drop(struct mptses *mpte, struct mptcb *mp_tp, u_short errno)
{
	struct socket *mp_so = mptetoso(mpte);

	VERIFY(mpte->mpte_mptcb == mp_tp);

	socket_lock_assert_owned(mp_so);

	DTRACE_MPTCP2(state__change, struct mptcb *, mp_tp,
	    uint32_t, 0 /* event */);

	if (errno == ETIMEDOUT && mp_tp->mpt_softerror != 0) {
		errno = mp_tp->mpt_softerror;
	}
	mp_so->so_error = errno;

	return mptcp_close(mpte, mp_tp);
}

/*
 * Close a MPTCP control block.
 */
struct mptses *
mptcp_close(struct mptses *mpte, struct mptcb *mp_tp)
{
	struct mptsub *mpts = NULL, *tmpts = NULL;
	struct socket *mp_so = mptetoso(mpte);

	socket_lock_assert_owned(mp_so);
	VERIFY(mpte->mpte_mptcb == mp_tp);

	mp_tp->mpt_state = MPTCPS_TERMINATE;

	mptcp_freeq(mp_tp);

	soisdisconnected(mp_so);

	/* Clean up all subflows */
	TAILQ_FOREACH_SAFE(mpts, &mpte->mpte_subflows, mpts_entry, tmpts) {
		mptcp_subflow_disconnect(mpte, mpts);
	}

	return NULL;
}

void
mptcp_notify_close(struct socket *so)
{
	soevent(so, (SO_FILT_HINT_LOCKED | SO_FILT_HINT_DISCONNECTED));
}

/*
 * MPTCP workloop.
 */
void
mptcp_subflow_workloop(struct mptses *mpte)
{
	boolean_t connect_pending = FALSE, disconnect_fallback = FALSE;
	long mpsofilt_hint_mask = SO_FILT_HINT_LOCKED;
	struct mptsub *mpts, *tmpts;
	struct socket *mp_so;

	mp_so = mptetoso(mpte);

	socket_lock_assert_owned(mp_so);

	if (mpte->mpte_flags & MPTE_IN_WORKLOOP) {
		mpte->mpte_flags |= MPTE_WORKLOOP_RELAUNCH;
		return;
	}
	mpte->mpte_flags |= MPTE_IN_WORKLOOP;

relaunch:
	mpte->mpte_flags &= ~MPTE_WORKLOOP_RELAUNCH;

	TAILQ_FOREACH_SAFE(mpts, &mpte->mpte_subflows, mpts_entry, tmpts) {
		ev_ret_t ret;

		if (mpts->mpts_socket->so_usecount == 0) {
			/* Will be removed soon by tcp_garbage_collect */
			continue;
		}

		mptcp_subflow_addref(mpts);
		mpts->mpts_socket->so_usecount++;

		ret = mptcp_subflow_events(mpte, mpts, &mpsofilt_hint_mask);

		/*
		 * If MPTCP socket is closed, disconnect all subflows.
		 * This will generate a disconnect event which will
		 * be handled during the next iteration, causing a
		 * non-zero error to be returned above.
		 */
		if (mp_so->so_flags & SOF_PCBCLEARING) {
			mptcp_subflow_disconnect(mpte, mpts);
		}

		switch (ret) {
		case MPTS_EVRET_OK:
			/* nothing to do */
			break;
		case MPTS_EVRET_DELETE:
			mptcp_subflow_soclose(mpts);
			break;
		case MPTS_EVRET_CONNECT_PENDING:
			connect_pending = TRUE;
			break;
		case MPTS_EVRET_DISCONNECT_FALLBACK:
			disconnect_fallback = TRUE;
			break;
		default:
			mptcplog((LOG_DEBUG,
			    "MPTCP Socket: %s: mptcp_subflow_events "
			    "returned invalid value: %d\n", __func__,
			    ret),
			    MPTCP_SOCKET_DBG, MPTCP_LOGLVL_VERBOSE);
			break;
		}
		mptcp_subflow_remref(mpts);             /* ours */

		VERIFY(mpts->mpts_socket->so_usecount != 0);
		mpts->mpts_socket->so_usecount--;
	}

	if (mpsofilt_hint_mask != SO_FILT_HINT_LOCKED) {
		VERIFY(mpsofilt_hint_mask & SO_FILT_HINT_LOCKED);

		if (mpsofilt_hint_mask & SO_FILT_HINT_CANTRCVMORE) {
			mp_so->so_state |= SS_CANTRCVMORE;
			sorwakeup(mp_so);
		}

		soevent(mp_so, mpsofilt_hint_mask);
	}

	if (!connect_pending && !disconnect_fallback) {
		goto exit;
	}

	TAILQ_FOREACH_SAFE(mpts, &mpte->mpte_subflows, mpts_entry, tmpts) {
		if (disconnect_fallback) {
			struct socket *so = NULL;
			struct inpcb *inp = NULL;
			struct tcpcb *tp = NULL;

			if (mpts->mpts_flags & MPTSF_MP_DEGRADED) {
				continue;
			}

			mpts->mpts_flags |= MPTSF_MP_DEGRADED;

			if (mpts->mpts_flags & (MPTSF_DISCONNECTING |
			    MPTSF_DISCONNECTED)) {
				continue;
			}

			so = mpts->mpts_socket;

			/*
			 * The MPTCP connection has degraded to a fallback
			 * mode, so there is no point in keeping this subflow
			 * regardless of its MPTCP-readiness state, unless it
			 * is the primary one which we use for fallback.  This
			 * assumes that the subflow used for fallback is the
			 * ACTIVE one.
			 */

			inp = sotoinpcb(so);
			tp = intotcpcb(inp);
			tp->t_mpflags &=
			    ~(TMPF_MPTCP_READY | TMPF_MPTCP_TRUE);
			tp->t_mpflags |= TMPF_TCP_FALLBACK;

			soevent(so, SO_FILT_HINT_MUSTRST);
		} else if (connect_pending) {
			/*
			 * The MPTCP connection has progressed to a state
			 * where it supports full multipath semantics; allow
			 * additional joins to be attempted for all subflows
			 * that are in the PENDING state.
			 */
			if (mpts->mpts_flags & MPTSF_CONNECT_PENDING) {
				int error = mptcp_subflow_soconnectx(mpte, mpts);

				if (error) {
					mptcp_subflow_abort(mpts, error);
				}
			}
		}
	}

exit:
	if (mpte->mpte_flags & MPTE_WORKLOOP_RELAUNCH) {
		goto relaunch;
	}

	mpte->mpte_flags &= ~MPTE_IN_WORKLOOP;
}

/*
 * Protocol pr_lock callback.
 */
int
mptcp_lock(struct socket *mp_so, int refcount, void *lr)
{
	struct mppcb *mpp = mpsotomppcb(mp_so);
	void *lr_saved;

	if (lr == NULL) {
		lr_saved = __builtin_return_address(0);
	} else {
		lr_saved = lr;
	}

	if (mpp == NULL) {
		panic("%s: so=%p NO PCB! lr=%p lrh= %s\n", __func__,
		    mp_so, lr_saved, solockhistory_nr(mp_so));
		/* NOTREACHED */
	}
	mpp_lock(mpp);

	if (mp_so->so_usecount < 0) {
		panic("%s: so=%p so_pcb=%p lr=%p ref=%x lrh= %s\n", __func__,
		    mp_so, mp_so->so_pcb, lr_saved, mp_so->so_usecount,
		    solockhistory_nr(mp_so));
		/* NOTREACHED */
	}
	if (refcount != 0) {
		mp_so->so_usecount++;
		mpp->mpp_inside++;
	}
	mp_so->lock_lr[mp_so->next_lock_lr] = lr_saved;
	mp_so->next_lock_lr = (mp_so->next_lock_lr + 1) % SO_LCKDBG_MAX;

	return 0;
}

/*
 * Protocol pr_unlock callback.
 */
int
mptcp_unlock(struct socket *mp_so, int refcount, void *lr)
{
	struct mppcb *mpp = mpsotomppcb(mp_so);
	void *lr_saved;

	if (lr == NULL) {
		lr_saved = __builtin_return_address(0);
	} else {
		lr_saved = lr;
	}

	if (mpp == NULL) {
		panic("%s: so=%p NO PCB usecount=%x lr=%p lrh= %s\n", __func__,
		    mp_so, mp_so->so_usecount, lr_saved,
		    solockhistory_nr(mp_so));
		/* NOTREACHED */
	}
	socket_lock_assert_owned(mp_so);

	if (refcount != 0) {
		mp_so->so_usecount--;
		mpp->mpp_inside--;
	}

	if (mp_so->so_usecount < 0) {
		panic("%s: so=%p usecount=%x lrh= %s\n", __func__,
		    mp_so, mp_so->so_usecount, solockhistory_nr(mp_so));
		/* NOTREACHED */
	}
	if (mpp->mpp_inside < 0) {
		panic("%s: mpp=%p inside=%x lrh= %s\n", __func__,
		    mpp, mpp->mpp_inside, solockhistory_nr(mp_so));
		/* NOTREACHED */
	}
	mp_so->unlock_lr[mp_so->next_unlock_lr] = lr_saved;
	mp_so->next_unlock_lr = (mp_so->next_unlock_lr + 1) % SO_LCKDBG_MAX;
	mpp_unlock(mpp);

	return 0;
}

/*
 * Protocol pr_getlock callback.
 */
lck_mtx_t *
mptcp_getlock(struct socket *mp_so, int flags)
{
	struct mppcb *mpp = mpsotomppcb(mp_so);

	if (mpp == NULL) {
		panic("%s: so=%p NULL so_pcb %s\n", __func__, mp_so,
		    solockhistory_nr(mp_so));
		/* NOTREACHED */
	}
	if (mp_so->so_usecount < 0) {
		panic("%s: so=%p usecount=%x lrh= %s\n", __func__,
		    mp_so, mp_so->so_usecount, solockhistory_nr(mp_so));
		/* NOTREACHED */
	}
	return mpp_getlock(mpp, flags);
}

/*
 * MPTCP Join support
 */

static void
mptcp_attach_to_subf(struct socket *so, struct mptcb *mp_tp, uint8_t addr_id)
{
	struct tcpcb *tp = sototcpcb(so);
	struct mptcp_subf_auth_entry *sauth_entry;

	/*
	 * The address ID of the first flow is implicitly 0.
	 */
	if (mp_tp->mpt_state == MPTCPS_CLOSED) {
		tp->t_local_aid = 0;
	} else {
		tp->t_local_aid = addr_id;
		tp->t_mpflags |= (TMPF_PREESTABLISHED | TMPF_JOINED_FLOW);
		so->so_flags |= SOF_MP_SEC_SUBFLOW;
	}
	sauth_entry = zalloc(mpt_subauth_zone);
	sauth_entry->msae_laddr_id = tp->t_local_aid;
	sauth_entry->msae_raddr_id = 0;
	sauth_entry->msae_raddr_rand = 0;
try_again:
	sauth_entry->msae_laddr_rand = RandomULong();
	if (sauth_entry->msae_laddr_rand == 0) {
		goto try_again;
	}
	LIST_INSERT_HEAD(&mp_tp->mpt_subauth_list, sauth_entry, msae_next);
}

static void
mptcp_detach_mptcb_from_subf(struct mptcb *mp_tp, struct socket *so)
{
	struct mptcp_subf_auth_entry *sauth_entry;
	struct tcpcb *tp = NULL;
	int found = 0;

	tp = sototcpcb(so);
	if (tp == NULL) {
		return;
	}

	LIST_FOREACH(sauth_entry, &mp_tp->mpt_subauth_list, msae_next) {
		if (sauth_entry->msae_laddr_id == tp->t_local_aid) {
			found = 1;
			break;
		}
	}
	if (found) {
		LIST_REMOVE(sauth_entry, msae_next);
	}

	if (found) {
		zfree(mpt_subauth_zone, sauth_entry);
	}
}

void
mptcp_get_rands(mptcp_addr_id addr_id, struct mptcb *mp_tp, u_int32_t *lrand,
    u_int32_t *rrand)
{
	struct mptcp_subf_auth_entry *sauth_entry;

	LIST_FOREACH(sauth_entry, &mp_tp->mpt_subauth_list, msae_next) {
		if (sauth_entry->msae_laddr_id == addr_id) {
			if (lrand) {
				*lrand = sauth_entry->msae_laddr_rand;
			}
			if (rrand) {
				*rrand = sauth_entry->msae_raddr_rand;
			}
			break;
		}
	}
}

void
mptcp_set_raddr_rand(mptcp_addr_id laddr_id, struct mptcb *mp_tp,
    mptcp_addr_id raddr_id, u_int32_t raddr_rand)
{
	struct mptcp_subf_auth_entry *sauth_entry;

	LIST_FOREACH(sauth_entry, &mp_tp->mpt_subauth_list, msae_next) {
		if (sauth_entry->msae_laddr_id == laddr_id) {
			if ((sauth_entry->msae_raddr_id != 0) &&
			    (sauth_entry->msae_raddr_id != raddr_id)) {
				os_log_error(mptcp_log_handle, "%s - %lx: mismatched"
				    " address ids %d %d \n", __func__, (unsigned long)VM_KERNEL_ADDRPERM(mp_tp->mpt_mpte),
				    raddr_id, sauth_entry->msae_raddr_id);
				return;
			}
			sauth_entry->msae_raddr_id = raddr_id;
			if ((sauth_entry->msae_raddr_rand != 0) &&
			    (sauth_entry->msae_raddr_rand != raddr_rand)) {
				os_log_error(mptcp_log_handle, "%s - %lx: "
				    "dup SYN_ACK %d %d \n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mp_tp->mpt_mpte),
				    raddr_rand, sauth_entry->msae_raddr_rand);
				return;
			}
			sauth_entry->msae_raddr_rand = raddr_rand;
			return;
		}
	}
}

/*
 * SHA1 support for MPTCP
 */
static void
mptcp_do_sha1(mptcp_key_t *key, char *sha_digest)
{
	SHA1_CTX sha1ctxt;
	const unsigned char *sha1_base;
	int sha1_size;

	sha1_base = (const unsigned char *) key;
	sha1_size = sizeof(mptcp_key_t);
	SHA1Init(&sha1ctxt);
	SHA1Update(&sha1ctxt, sha1_base, sha1_size);
	SHA1Final(sha_digest, &sha1ctxt);
}

void
mptcp_hmac_sha1(mptcp_key_t key1, mptcp_key_t key2,
    u_int32_t rand1, u_int32_t rand2, u_char *digest)
{
	SHA1_CTX  sha1ctxt;
	mptcp_key_t key_ipad[8] = {0}; /* key XOR'd with inner pad */
	mptcp_key_t key_opad[8] = {0}; /* key XOR'd with outer pad */
	u_int32_t data[2];
	int i;

	bzero(digest, SHA1_RESULTLEN);

	/* Set up the Key for HMAC */
	key_ipad[0] = key1;
	key_ipad[1] = key2;

	key_opad[0] = key1;
	key_opad[1] = key2;

	/* Set up the message for HMAC */
	data[0] = rand1;
	data[1] = rand2;

	/* Key is 512 block length, so no need to compute hash */

	/* Compute SHA1(Key XOR opad, SHA1(Key XOR ipad, data)) */

	for (i = 0; i < 8; i++) {
		key_ipad[i] ^= 0x3636363636363636;
		key_opad[i] ^= 0x5c5c5c5c5c5c5c5c;
	}

	/* Perform inner SHA1 */
	SHA1Init(&sha1ctxt);
	SHA1Update(&sha1ctxt, (unsigned char *)key_ipad, sizeof(key_ipad));
	SHA1Update(&sha1ctxt, (unsigned char *)data, sizeof(data));
	SHA1Final(digest, &sha1ctxt);

	/* Perform outer SHA1 */
	SHA1Init(&sha1ctxt);
	SHA1Update(&sha1ctxt, (unsigned char *)key_opad, sizeof(key_opad));
	SHA1Update(&sha1ctxt, (unsigned char *)digest, SHA1_RESULTLEN);
	SHA1Final(digest, &sha1ctxt);
}

/*
 * corresponds to MAC-B = MAC (Key=(Key-B+Key-A), Msg=(R-B+R-A))
 * corresponds to MAC-A = MAC (Key=(Key-A+Key-B), Msg=(R-A+R-B))
 */
void
mptcp_get_hmac(mptcp_addr_id aid, struct mptcb *mp_tp, u_char *digest)
{
	uint32_t lrand, rrand;

	lrand = rrand = 0;
	mptcp_get_rands(aid, mp_tp, &lrand, &rrand);
	mptcp_hmac_sha1(mp_tp->mpt_localkey, mp_tp->mpt_remotekey, lrand, rrand,
	    digest);
}

/*
 * Authentication data generation
 */
static void
mptcp_generate_token(char *sha_digest, int sha_digest_len, caddr_t token,
    int token_len)
{
	VERIFY(token_len == sizeof(u_int32_t));
	VERIFY(sha_digest_len == SHA1_RESULTLEN);

	/* Most significant 32 bits of the SHA1 hash */
	bcopy(sha_digest, token, sizeof(u_int32_t));
	return;
}

static void
mptcp_generate_idsn(char *sha_digest, int sha_digest_len, caddr_t idsn,
    int idsn_len)
{
	VERIFY(idsn_len == sizeof(u_int64_t));
	VERIFY(sha_digest_len == SHA1_RESULTLEN);

	/*
	 * Least significant 64 bits of the SHA1 hash
	 */

	idsn[7] = sha_digest[12];
	idsn[6] = sha_digest[13];
	idsn[5] = sha_digest[14];
	idsn[4] = sha_digest[15];
	idsn[3] = sha_digest[16];
	idsn[2] = sha_digest[17];
	idsn[1] = sha_digest[18];
	idsn[0] = sha_digest[19];
	return;
}

static void
mptcp_conn_properties(struct mptcb *mp_tp)
{
	/* There is only Version 0 at this time */
	mp_tp->mpt_version = MPTCP_STD_VERSION_0;

	/* Set DSS checksum flag */
	if (mptcp_dss_csum) {
		mp_tp->mpt_flags |= MPTCPF_CHECKSUM;
	}

	/* Set up receive window */
	mp_tp->mpt_rcvwnd = mptcp_sbspace(mp_tp);

	/* Set up gc ticks */
	mp_tp->mpt_gc_ticks = MPT_GC_TICKS;
}

static void
mptcp_init_local_parms(struct mptses *mpte)
{
	struct mptcb *mp_tp = mpte->mpte_mptcb;
	char key_digest[SHA1_RESULTLEN];

	read_frandom(&mp_tp->mpt_localkey, sizeof(mp_tp->mpt_localkey));
	mptcp_do_sha1(&mp_tp->mpt_localkey, key_digest);

	mptcp_generate_token(key_digest, SHA1_RESULTLEN,
	    (caddr_t)&mp_tp->mpt_localtoken, sizeof(mp_tp->mpt_localtoken));
	mptcp_generate_idsn(key_digest, SHA1_RESULTLEN,
	    (caddr_t)&mp_tp->mpt_local_idsn, sizeof(u_int64_t));

	/* The subflow SYN is also first MPTCP byte */
	mp_tp->mpt_snduna = mp_tp->mpt_sndmax = mp_tp->mpt_local_idsn + 1;
	mp_tp->mpt_sndnxt = mp_tp->mpt_snduna;

	mptcp_conn_properties(mp_tp);
}

int
mptcp_init_remote_parms(struct mptcb *mp_tp)
{
	char remote_digest[SHA1_RESULTLEN];

	/* Only Version 0 is supported for auth purposes */
	if (mp_tp->mpt_version != MPTCP_STD_VERSION_0) {
		return -1;
	}

	/* Setup local and remote tokens and Initial DSNs */
	mptcp_do_sha1(&mp_tp->mpt_remotekey, remote_digest);
	mptcp_generate_token(remote_digest, SHA1_RESULTLEN,
	    (caddr_t)&mp_tp->mpt_remotetoken, sizeof(mp_tp->mpt_remotetoken));
	mptcp_generate_idsn(remote_digest, SHA1_RESULTLEN,
	    (caddr_t)&mp_tp->mpt_remote_idsn, sizeof(u_int64_t));
	mp_tp->mpt_rcvnxt = mp_tp->mpt_remote_idsn + 1;
	mp_tp->mpt_rcvadv = mp_tp->mpt_rcvnxt + mp_tp->mpt_rcvwnd;

	return 0;
}

static void
mptcp_send_dfin(struct socket *so)
{
	struct tcpcb *tp = NULL;
	struct inpcb *inp = NULL;

	inp = sotoinpcb(so);
	if (!inp) {
		return;
	}

	tp = intotcpcb(inp);
	if (!tp) {
		return;
	}

	if (!(tp->t_mpflags & TMPF_RESET)) {
		tp->t_mpflags |= TMPF_SEND_DFIN;
	}
}

/*
 * Data Sequence Mapping routines
 */
void
mptcp_insert_dsn(struct mppcb *mpp, struct mbuf *m)
{
	struct mptcb *mp_tp;

	if (m == NULL) {
		return;
	}

	__IGNORE_WCASTALIGN(mp_tp = &((struct mpp_mtp *)mpp)->mtcb);

	while (m) {
		VERIFY(m->m_flags & M_PKTHDR);
		m->m_pkthdr.pkt_flags |= (PKTF_MPTCP | PKTF_MPSO);
		m->m_pkthdr.mp_dsn = mp_tp->mpt_sndmax;
		VERIFY(m_pktlen(m) >= 0 && m_pktlen(m) < UINT16_MAX);
		m->m_pkthdr.mp_rlen = (uint16_t)m_pktlen(m);
		mp_tp->mpt_sndmax += m_pktlen(m);
		m = m->m_next;
	}
}

void
mptcp_fallback_sbdrop(struct socket *so, struct mbuf *m, int len)
{
	struct mptcb *mp_tp = tptomptp(sototcpcb(so));
	uint64_t data_ack;
	uint64_t dsn;

	VERIFY(len >= 0);

	if (!m || len == 0) {
		return;
	}

	while (m && len > 0) {
		VERIFY(m->m_flags & M_PKTHDR);
		VERIFY(m->m_pkthdr.pkt_flags & PKTF_MPTCP);

		data_ack = m->m_pkthdr.mp_dsn + m->m_pkthdr.mp_rlen;
		dsn = m->m_pkthdr.mp_dsn;

		len -= m->m_len;
		m = m->m_next;
	}

	if (m && len == 0) {
		/*
		 * If there is one more mbuf in the chain, it automatically means
		 * that up to m->mp_dsn has been ack'ed.
		 *
		 * This means, we actually correct data_ack back down (compared
		 * to what we set inside the loop - dsn + data_len). Because in
		 * the loop we are "optimistic" and assume that the full mapping
		 * will be acked. If that's not the case and we get out of the
		 * loop with m != NULL, it means only up to m->mp_dsn has been
		 * really acked.
		 */
		data_ack = m->m_pkthdr.mp_dsn;
	}

	if (len < 0) {
		/*
		 * If len is negative, meaning we acked in the middle of an mbuf,
		 * only up to this mbuf's data-sequence number has been acked
		 * at the MPTCP-level.
		 */
		data_ack = dsn;
	}

	mptcplog((LOG_DEBUG, "%s inferred ack up to %u\n", __func__, (uint32_t)data_ack),
	    MPTCP_SOCKET_DBG, MPTCP_LOGLVL_VERBOSE);

	/* We can have data in the subflow's send-queue that is being acked,
	 * while the DATA_ACK has already advanced. Thus, we should check whether
	 * or not the DATA_ACK is actually new here.
	 */
	if (MPTCP_SEQ_LEQ(data_ack, mp_tp->mpt_sndmax) &&
	    MPTCP_SEQ_GEQ(data_ack, mp_tp->mpt_snduna)) {
		mptcp_data_ack_rcvd(mp_tp, sototcpcb(so), data_ack);
	}
}

void
mptcp_preproc_sbdrop(struct socket *so, struct mbuf *m, unsigned int len)
{
	int rewinding = 0;

	/* TFO makes things complicated. */
	if (so->so_flags1 & SOF1_TFO_REWIND) {
		rewinding = 1;
		so->so_flags1 &= ~SOF1_TFO_REWIND;
	}

	while (m && (!(so->so_flags & SOF_MP_SUBFLOW) || rewinding)) {
		u_int32_t sub_len;
		VERIFY(m->m_flags & M_PKTHDR);
		VERIFY(m->m_pkthdr.pkt_flags & PKTF_MPTCP);

		sub_len = m->m_pkthdr.mp_rlen;

		if (sub_len < len) {
			m->m_pkthdr.mp_dsn += sub_len;
			if (!(m->m_pkthdr.pkt_flags & PKTF_MPSO)) {
				m->m_pkthdr.mp_rseq += sub_len;
			}
			m->m_pkthdr.mp_rlen = 0;
			len -= sub_len;
		} else {
			/* sub_len >= len */
			if (rewinding == 0) {
				m->m_pkthdr.mp_dsn += len;
			}
			if (!(m->m_pkthdr.pkt_flags & PKTF_MPSO)) {
				if (rewinding == 0) {
					m->m_pkthdr.mp_rseq += len;
				}
			}
			mptcplog((LOG_DEBUG, "%s: dsn %u ssn %u len %d %d\n",
			    __func__, (u_int32_t)m->m_pkthdr.mp_dsn,
			    m->m_pkthdr.mp_rseq, m->m_pkthdr.mp_rlen, len),
			    MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);
			m->m_pkthdr.mp_rlen -= len;
			break;
		}
		m = m->m_next;
	}

	if (so->so_flags & SOF_MP_SUBFLOW &&
	    !(sototcpcb(so)->t_mpflags & TMPF_TFO_REQUEST) &&
	    !(sototcpcb(so)->t_mpflags & TMPF_RCVD_DACK)) {
		/*
		 * Received an ack without receiving a DATA_ACK.
		 * Need to fallback to regular TCP (or destroy this subflow).
		 */
		sototcpcb(so)->t_mpflags |= TMPF_INFIN_SENT;
		mptcp_notify_mpfail(so);
	}
}

/* Obtain the DSN mapping stored in the mbuf */
void
mptcp_output_getm_dsnmap32(struct socket *so, int off,
    uint32_t *dsn, uint32_t *relseq, uint16_t *data_len, uint16_t *dss_csum)
{
	u_int64_t dsn64;

	mptcp_output_getm_dsnmap64(so, off, &dsn64, relseq, data_len, dss_csum);
	*dsn = (u_int32_t)MPTCP_DATASEQ_LOW32(dsn64);
}

void
mptcp_output_getm_dsnmap64(struct socket *so, int off, uint64_t *dsn,
    uint32_t *relseq, uint16_t *data_len,
    uint16_t *dss_csum)
{
	struct mbuf *m = so->so_snd.sb_mb;
	int off_orig = off;

	VERIFY(off >= 0);

	if (m == NULL && (so->so_flags & SOF_DEFUNCT)) {
		*dsn = 0;
		*relseq = 0;
		*data_len = 0;
		*dss_csum = 0;
		return;
	}

	/*
	 * In the subflow socket, the DSN sequencing can be discontiguous,
	 * but the subflow sequence mapping is contiguous. Use the subflow
	 * sequence property to find the right mbuf and corresponding dsn
	 * mapping.
	 */

	while (m) {
		VERIFY(m->m_flags & M_PKTHDR);
		VERIFY(m->m_pkthdr.pkt_flags & PKTF_MPTCP);

		if (off >= m->m_len) {
			off -= m->m_len;
			m = m->m_next;
		} else {
			break;
		}
	}

	VERIFY(off >= 0);
	VERIFY(m->m_pkthdr.mp_rlen <= UINT16_MAX);

	*dsn = m->m_pkthdr.mp_dsn;
	*relseq = m->m_pkthdr.mp_rseq;
	*data_len = m->m_pkthdr.mp_rlen;
	*dss_csum = m->m_pkthdr.mp_csum;

	mptcplog((LOG_DEBUG, "%s: dsn %u ssn %u data_len %d off %d off_orig %d\n",
	    __func__, (u_int32_t)(*dsn), *relseq, *data_len, off, off_orig),
	    MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);
}

/*
 * Note that this is called only from tcp_input() via mptcp_input_preproc()
 * tcp_input() may trim data after the dsn mapping is inserted into the mbuf.
 * When it trims data tcp_input calls m_adj() which does not remove the
 * m_pkthdr even if the m_len becomes 0 as a result of trimming the mbuf.
 * The dsn map insertion cannot be delayed after trim, because data can be in
 * the reassembly queue for a while and the DSN option info in tp will be
 * overwritten for every new packet received.
 * The dsn map will be adjusted just prior to appending to subflow sockbuf
 * with mptcp_adj_rmap()
 */
void
mptcp_insert_rmap(struct tcpcb *tp, struct mbuf *m, struct tcphdr *th)
{
	VERIFY(m->m_flags & M_PKTHDR);
	VERIFY(!(m->m_pkthdr.pkt_flags & PKTF_MPTCP));

	if (tp->t_mpflags & TMPF_EMBED_DSN) {
		m->m_pkthdr.mp_dsn = tp->t_rcv_map.mpt_dsn;
		m->m_pkthdr.mp_rseq = tp->t_rcv_map.mpt_sseq;
		m->m_pkthdr.mp_rlen = tp->t_rcv_map.mpt_len;
		m->m_pkthdr.mp_csum = tp->t_rcv_map.mpt_csum;
		if (tp->t_rcv_map.mpt_dfin) {
			m->m_pkthdr.pkt_flags |= PKTF_MPTCP_DFIN;
		}

		m->m_pkthdr.pkt_flags |= PKTF_MPTCP;

		tp->t_mpflags &= ~TMPF_EMBED_DSN;
		tp->t_mpflags |= TMPF_MPTCP_ACKNOW;
	} else if (tp->t_mpflags & TMPF_TCP_FALLBACK) {
		if (th->th_flags & TH_FIN) {
			m->m_pkthdr.pkt_flags |= PKTF_MPTCP_DFIN;
		}
	}
}

/*
 * Following routines help with failure detection and failover of data
 * transfer from one subflow to another.
 */
void
mptcp_act_on_txfail(struct socket *so)
{
	struct tcpcb *tp = NULL;
	struct inpcb *inp = sotoinpcb(so);

	if (inp == NULL) {
		return;
	}

	tp = intotcpcb(inp);
	if (tp == NULL) {
		return;
	}

	if (so->so_flags & SOF_MP_TRYFAILOVER) {
		return;
	}

	so->so_flags |= SOF_MP_TRYFAILOVER;
	soevent(so, (SO_FILT_HINT_LOCKED | SO_FILT_HINT_MPFAILOVER));
}

/*
 * Support for MP_FAIL option
 */
int
mptcp_get_map_for_dsn(struct socket *so, uint64_t dsn_fail, uint32_t *tcp_seq)
{
	struct mbuf *m = so->so_snd.sb_mb;
	uint16_t datalen;
	uint64_t dsn;
	int off = 0;

	if (m == NULL) {
		return -1;
	}

	while (m != NULL) {
		VERIFY(m->m_pkthdr.pkt_flags & PKTF_MPTCP);
		VERIFY(m->m_flags & M_PKTHDR);
		dsn = m->m_pkthdr.mp_dsn;
		datalen = m->m_pkthdr.mp_rlen;
		if (MPTCP_SEQ_LEQ(dsn, dsn_fail) &&
		    (MPTCP_SEQ_GEQ(dsn + datalen, dsn_fail))) {
			off = (int)(dsn_fail - dsn);
			*tcp_seq = m->m_pkthdr.mp_rseq + off;
			return 0;
		}

		m = m->m_next;
	}

	/*
	 * If there was no mbuf data and a fallback to TCP occurred, there's
	 * not much else to do.
	 */

	os_log_error(mptcp_log_handle, "%s: %llu not found \n", __func__, dsn_fail);
	return -1;
}

/*
 * Support for sending contiguous MPTCP bytes in subflow
 * Also for preventing sending data with ACK in 3-way handshake
 */
int32_t
mptcp_adj_sendlen(struct socket *so, int32_t off)
{
	struct tcpcb *tp = sototcpcb(so);
	struct mptsub *mpts = tp->t_mpsub;
	uint64_t mdss_dsn;
	uint32_t mdss_subflow_seq;
	int mdss_subflow_off;
	uint16_t mdss_data_len;
	uint16_t dss_csum;

	if (so->so_snd.sb_mb == NULL && (so->so_flags & SOF_DEFUNCT)) {
		return 0;
	}

	mptcp_output_getm_dsnmap64(so, off, &mdss_dsn, &mdss_subflow_seq,
	    &mdss_data_len, &dss_csum);

	/*
	 * We need to compute how much of the mapping still remains.
	 * So, we compute the offset in the send-buffer of the dss-sub-seq.
	 */
	mdss_subflow_off = (mdss_subflow_seq + mpts->mpts_iss) - tp->snd_una;

	/*
	 * When TFO is used, we are sending the mpts->mpts_iss although the relative
	 * seq has been set to 1 (while it should be 0).
	 */
	if (tp->t_mpflags & TMPF_TFO_REQUEST) {
		mdss_subflow_off--;
	}

	VERIFY(off >= mdss_subflow_off);

	return mdss_data_len - (off - mdss_subflow_off);
}

static uint32_t
mptcp_get_maxseg(struct mptses *mpte)
{
	struct mptsub *mpts;
	uint32_t maxseg = 0;

	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		struct tcpcb *tp = sototcpcb(mpts->mpts_socket);

		if (!TCPS_HAVEESTABLISHED(tp->t_state) ||
		    TCPS_HAVERCVDFIN2(tp->t_state)) {
			continue;
		}

		if (tp->t_maxseg > maxseg) {
			maxseg = tp->t_maxseg;
		}
	}

	return maxseg;
}

static uint8_t
mptcp_get_rcvscale(struct mptses *mpte)
{
	struct mptsub *mpts;
	uint8_t rcvscale = UINT8_MAX;

	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		struct tcpcb *tp = sototcpcb(mpts->mpts_socket);

		if (!TCPS_HAVEESTABLISHED(tp->t_state) ||
		    TCPS_HAVERCVDFIN2(tp->t_state)) {
			continue;
		}

		if (tp->rcv_scale < rcvscale) {
			rcvscale = tp->rcv_scale;
		}
	}

	return rcvscale;
}

/* Similar to tcp_sbrcv_reserve */
static void
mptcp_sbrcv_reserve(struct mptcb *mp_tp, struct sockbuf *sbrcv,
    u_int32_t newsize, u_int32_t idealsize)
{
	uint8_t rcvscale = mptcp_get_rcvscale(mp_tp->mpt_mpte);

	/* newsize should not exceed max */
	newsize = min(newsize, tcp_autorcvbuf_max);

	/* The receive window scale negotiated at the
	 * beginning of the connection will also set a
	 * limit on the socket buffer size
	 */
	newsize = min(newsize, TCP_MAXWIN << rcvscale);

	/* Set new socket buffer size */
	if (newsize > sbrcv->sb_hiwat &&
	    (sbreserve(sbrcv, newsize) == 1)) {
		sbrcv->sb_idealsize = min(max(sbrcv->sb_idealsize,
		    (idealsize != 0) ? idealsize : newsize), tcp_autorcvbuf_max);

		/* Again check the limit set by the advertised
		 * window scale
		 */
		sbrcv->sb_idealsize = min(sbrcv->sb_idealsize,
		    TCP_MAXWIN << rcvscale);
	}
}

void
mptcp_sbrcv_grow(struct mptcb *mp_tp)
{
	struct mptses *mpte = mp_tp->mpt_mpte;
	struct socket *mp_so = mpte->mpte_mppcb->mpp_socket;
	struct sockbuf *sbrcv = &mp_so->so_rcv;
	uint32_t hiwat_sum = 0;
	uint32_t ideal_sum = 0;
	struct mptsub *mpts;

	/*
	 * Do not grow the receive socket buffer if
	 * - auto resizing is disabled, globally or on this socket
	 * - the high water mark already reached the maximum
	 * - the stream is in background and receive side is being
	 * throttled
	 * - if there are segments in reassembly queue indicating loss,
	 * do not need to increase recv window during recovery as more
	 * data is not going to be sent. A duplicate ack sent during
	 * recovery should not change the receive window
	 */
	if (tcp_do_autorcvbuf == 0 ||
	    (sbrcv->sb_flags & SB_AUTOSIZE) == 0 ||
	    tcp_cansbgrow(sbrcv) == 0 ||
	    sbrcv->sb_hiwat >= tcp_autorcvbuf_max ||
	    (mp_so->so_flags1 & SOF1_EXTEND_BK_IDLE_WANTED) ||
	    !LIST_EMPTY(&mp_tp->mpt_segq)) {
		/* Can not resize the socket buffer, just return */
		return;
	}

	/*
	 * Ideally, we want the rbuf to be (sum_i {bw_i} * rtt_max * 2)
	 *
	 * But, for this we first need accurate receiver-RTT estimations, which
	 * we currently don't have.
	 *
	 * Let's use a dummy algorithm for now, just taking the sum of all
	 * subflow's receive-buffers. It's too low, but that's all we can get
	 * for now.
	 */

	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		hiwat_sum += mpts->mpts_socket->so_rcv.sb_hiwat;
		ideal_sum += mpts->mpts_socket->so_rcv.sb_idealsize;
	}

	mptcp_sbrcv_reserve(mp_tp, sbrcv, hiwat_sum, ideal_sum);
}

/*
 * Determine if we can grow the recieve socket buffer to avoid sending
 * a zero window update to the peer. We allow even socket buffers that
 * have fixed size (set by the application) to grow if the resource
 * constraints are met. They will also be trimmed after the application
 * reads data.
 *
 * Similar to tcp_sbrcv_grow_rwin
 */
static void
mptcp_sbrcv_grow_rwin(struct mptcb *mp_tp, struct sockbuf *sb)
{
	struct socket *mp_so = mp_tp->mpt_mpte->mpte_mppcb->mpp_socket;
	u_int32_t rcvbufinc = mptcp_get_maxseg(mp_tp->mpt_mpte) << 4;
	u_int32_t rcvbuf = sb->sb_hiwat;

	if (tcp_recv_bg == 1 || IS_TCP_RECV_BG(mp_so)) {
		return;
	}

	if (tcp_do_autorcvbuf == 1 &&
	    tcp_cansbgrow(sb) &&
	    /* Diff to tcp_sbrcv_grow_rwin */
	    (mp_so->so_flags1 & SOF1_EXTEND_BK_IDLE_WANTED) == 0 &&
	    (rcvbuf - sb->sb_cc) < rcvbufinc &&
	    rcvbuf < tcp_autorcvbuf_max &&
	    (sb->sb_idealsize > 0 &&
	    sb->sb_hiwat <= (sb->sb_idealsize + rcvbufinc))) {
		sbreserve(sb, min((sb->sb_hiwat + rcvbufinc), tcp_autorcvbuf_max));
	}
}

/* Similar to tcp_sbspace */
int32_t
mptcp_sbspace(struct mptcb *mp_tp)
{
	struct sockbuf *sb = &mp_tp->mpt_mpte->mpte_mppcb->mpp_socket->so_rcv;
	uint32_t rcvbuf;
	int32_t space;
	int32_t pending = 0;

	socket_lock_assert_owned(mptetoso(mp_tp->mpt_mpte));

	mptcp_sbrcv_grow_rwin(mp_tp, sb);

	/* hiwat might have changed */
	rcvbuf = sb->sb_hiwat;

	space =  ((int32_t) imin((rcvbuf - sb->sb_cc),
	    (sb->sb_mbmax - sb->sb_mbcnt)));
	if (space < 0) {
		space = 0;
	}

#if CONTENT_FILTER
	/* Compensate for data being processed by content filters */
	pending = cfil_sock_data_space(sb);
#endif /* CONTENT_FILTER */
	if (pending > space) {
		space = 0;
	} else {
		space -= pending;
	}

	return space;
}

/*
 * Support Fallback to Regular TCP
 */
void
mptcp_notify_mpready(struct socket *so)
{
	struct tcpcb *tp = NULL;

	if (so == NULL) {
		return;
	}

	tp = intotcpcb(sotoinpcb(so));

	if (tp == NULL) {
		return;
	}

	DTRACE_MPTCP4(multipath__ready, struct socket *, so,
	    struct sockbuf *, &so->so_rcv, struct sockbuf *, &so->so_snd,
	    struct tcpcb *, tp);

	if (!(tp->t_mpflags & TMPF_MPTCP_TRUE)) {
		return;
	}

	if (tp->t_mpflags & TMPF_MPTCP_READY) {
		return;
	}

	tp->t_mpflags &= ~TMPF_TCP_FALLBACK;
	tp->t_mpflags |= TMPF_MPTCP_READY;

	soevent(so, (SO_FILT_HINT_LOCKED | SO_FILT_HINT_MPSTATUS));
}

void
mptcp_notify_mpfail(struct socket *so)
{
	struct tcpcb *tp = NULL;

	if (so == NULL) {
		return;
	}

	tp = intotcpcb(sotoinpcb(so));

	if (tp == NULL) {
		return;
	}

	DTRACE_MPTCP4(multipath__failed, struct socket *, so,
	    struct sockbuf *, &so->so_rcv, struct sockbuf *, &so->so_snd,
	    struct tcpcb *, tp);

	if (tp->t_mpflags & TMPF_TCP_FALLBACK) {
		return;
	}

	tp->t_mpflags &= ~(TMPF_MPTCP_READY | TMPF_MPTCP_TRUE);
	tp->t_mpflags |= TMPF_TCP_FALLBACK;

	soevent(so, (SO_FILT_HINT_LOCKED | SO_FILT_HINT_MPSTATUS));
}

/*
 * Keepalive helper function
 */
boolean_t
mptcp_ok_to_keepalive(struct mptcb *mp_tp)
{
	boolean_t ret = 1;

	socket_lock_assert_owned(mptetoso(mp_tp->mpt_mpte));

	if (mp_tp->mpt_state >= MPTCPS_CLOSE_WAIT) {
		ret = 0;
	}
	return ret;
}

/*
 * MPTCP t_maxseg adjustment function
 */
int
mptcp_adj_mss(struct tcpcb *tp, boolean_t mtudisc)
{
	int mss_lower = 0;
	struct mptcb *mp_tp = tptomptp(tp);

#define MPTCP_COMPUTE_LEN {                             \
	mss_lower = sizeof (struct mptcp_dss_ack_opt);  \
	if (mp_tp->mpt_flags & MPTCPF_CHECKSUM)         \
	        mss_lower += 2;                         \
	else                                            \
	/* adjust to 32-bit boundary + EOL */   \
	        mss_lower += 2;                         \
}
	if (mp_tp == NULL) {
		return 0;
	}

	socket_lock_assert_owned(mptetoso(mp_tp->mpt_mpte));

	/*
	 * For the first subflow and subsequent subflows, adjust mss for
	 * most common MPTCP option size, for case where tcp_mss is called
	 * during option processing and MTU discovery.
	 */
	if (!mtudisc) {
		if (tp->t_mpflags & TMPF_MPTCP_TRUE &&
		    !(tp->t_mpflags & TMPF_JOINED_FLOW)) {
			MPTCP_COMPUTE_LEN;
		}

		if (tp->t_mpflags & TMPF_PREESTABLISHED &&
		    tp->t_mpflags & TMPF_SENT_JOIN) {
			MPTCP_COMPUTE_LEN;
		}
	} else {
		if (tp->t_mpflags & TMPF_MPTCP_TRUE) {
			MPTCP_COMPUTE_LEN;
		}
	}

	return mss_lower;
}

/*
 * Update the pid, upid, uuid of the subflow so, based on parent so
 */
void
mptcp_update_last_owner(struct socket *so, struct socket *mp_so)
{
	if (so->last_pid != mp_so->last_pid ||
	    so->last_upid != mp_so->last_upid) {
		so->last_upid = mp_so->last_upid;
		so->last_pid = mp_so->last_pid;
		uuid_copy(so->last_uuid, mp_so->last_uuid);
	}
	so_update_policy(so);
}

static void
fill_mptcp_subflow(struct socket *so, mptcp_flow_t *flow, struct mptsub *mpts)
{
	struct inpcb *inp;

	tcp_getconninfo(so, &flow->flow_ci);
	inp = sotoinpcb(so);
	if ((inp->inp_vflag & INP_IPV6) != 0) {
		flow->flow_src.ss_family = AF_INET6;
		flow->flow_dst.ss_family = AF_INET6;
		flow->flow_src.ss_len = sizeof(struct sockaddr_in6);
		flow->flow_dst.ss_len = sizeof(struct sockaddr_in6);
		SIN6(&flow->flow_src)->sin6_port = inp->in6p_lport;
		SIN6(&flow->flow_dst)->sin6_port = inp->in6p_fport;
		SIN6(&flow->flow_src)->sin6_addr = inp->in6p_laddr;
		SIN6(&flow->flow_dst)->sin6_addr = inp->in6p_faddr;
	} else if ((inp->inp_vflag & INP_IPV4) != 0) {
		flow->flow_src.ss_family = AF_INET;
		flow->flow_dst.ss_family = AF_INET;
		flow->flow_src.ss_len = sizeof(struct sockaddr_in);
		flow->flow_dst.ss_len = sizeof(struct sockaddr_in);
		SIN(&flow->flow_src)->sin_port = inp->inp_lport;
		SIN(&flow->flow_dst)->sin_port = inp->inp_fport;
		SIN(&flow->flow_src)->sin_addr = inp->inp_laddr;
		SIN(&flow->flow_dst)->sin_addr = inp->inp_faddr;
	}
	flow->flow_len = sizeof(*flow);
	flow->flow_tcpci_offset = offsetof(mptcp_flow_t, flow_ci);
	flow->flow_flags = mpts->mpts_flags;
	flow->flow_cid = mpts->mpts_connid;
	flow->flow_relseq = mpts->mpts_rel_seq;
	flow->flow_soerror = mpts->mpts_socket->so_error;
	flow->flow_probecnt = mpts->mpts_probecnt;
}

static int
mptcp_pcblist SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	int error = 0, f;
	size_t len;
	struct mppcb *mpp;
	struct mptses *mpte;
	struct mptcb *mp_tp;
	struct mptsub *mpts;
	struct socket *so;
	conninfo_mptcp_t mptcpci;
	mptcp_flow_t *flows = NULL;

	if (req->newptr != USER_ADDR_NULL) {
		return EPERM;
	}

	lck_mtx_lock(&mtcbinfo.mppi_lock);
	if (req->oldptr == USER_ADDR_NULL) {
		size_t n = mtcbinfo.mppi_count;
		lck_mtx_unlock(&mtcbinfo.mppi_lock);
		req->oldidx = (n + n / 8) * sizeof(conninfo_mptcp_t) +
		    4 * (n + n / 8)  * sizeof(mptcp_flow_t);
		return 0;
	}
	TAILQ_FOREACH(mpp, &mtcbinfo.mppi_pcbs, mpp_entry) {
		flows = NULL;
		socket_lock(mpp->mpp_socket, 1);
		VERIFY(mpp->mpp_flags & MPP_ATTACHED);
		mpte = mptompte(mpp);

		socket_lock_assert_owned(mptetoso(mpte));
		mp_tp = mpte->mpte_mptcb;

		bzero(&mptcpci, sizeof(mptcpci));
		mptcpci.mptcpci_state = mp_tp->mpt_state;
		mptcpci.mptcpci_flags = mp_tp->mpt_flags;
		mptcpci.mptcpci_ltoken = mp_tp->mpt_localtoken;
		mptcpci.mptcpci_rtoken = mp_tp->mpt_remotetoken;
		mptcpci.mptcpci_notsent_lowat = mp_tp->mpt_notsent_lowat;
		mptcpci.mptcpci_snduna = mp_tp->mpt_snduna;
		mptcpci.mptcpci_sndnxt = mp_tp->mpt_sndnxt;
		mptcpci.mptcpci_sndmax = mp_tp->mpt_sndmax;
		mptcpci.mptcpci_lidsn = mp_tp->mpt_local_idsn;
		mptcpci.mptcpci_sndwnd = mp_tp->mpt_sndwnd;
		mptcpci.mptcpci_rcvnxt = mp_tp->mpt_rcvnxt;
		mptcpci.mptcpci_rcvatmark = mp_tp->mpt_rcvnxt;
		mptcpci.mptcpci_ridsn = mp_tp->mpt_remote_idsn;
		mptcpci.mptcpci_rcvwnd = mp_tp->mpt_rcvwnd;

		mptcpci.mptcpci_nflows = mpte->mpte_numflows;
		mptcpci.mptcpci_mpte_flags = mpte->mpte_flags;
		mptcpci.mptcpci_mpte_addrid = mpte->mpte_addrid_last;
		mptcpci.mptcpci_flow_offset =
		    offsetof(conninfo_mptcp_t, mptcpci_flows);

		len = sizeof(*flows) * mpte->mpte_numflows;
		if (mpte->mpte_numflows != 0) {
			flows = _MALLOC(len, M_TEMP, M_WAITOK | M_ZERO);
			if (flows == NULL) {
				socket_unlock(mpp->mpp_socket, 1);
				break;
			}
			mptcpci.mptcpci_len = sizeof(mptcpci) +
			    sizeof(*flows) * (mptcpci.mptcpci_nflows - 1);
			error = SYSCTL_OUT(req, &mptcpci,
			    sizeof(mptcpci) - sizeof(mptcp_flow_t));
		} else {
			mptcpci.mptcpci_len = sizeof(mptcpci);
			error = SYSCTL_OUT(req, &mptcpci, sizeof(mptcpci));
		}
		if (error) {
			socket_unlock(mpp->mpp_socket, 1);
			FREE(flows, M_TEMP);
			break;
		}
		f = 0;
		TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
			so = mpts->mpts_socket;
			fill_mptcp_subflow(so, &flows[f], mpts);
			f++;
		}
		socket_unlock(mpp->mpp_socket, 1);
		if (flows) {
			error = SYSCTL_OUT(req, flows, len);
			FREE(flows, M_TEMP);
			if (error) {
				break;
			}
		}
	}
	lck_mtx_unlock(&mtcbinfo.mppi_lock);

	return error;
}

SYSCTL_PROC(_net_inet_mptcp, OID_AUTO, pcblist, CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, mptcp_pcblist, "S,conninfo_mptcp_t",
    "List of active MPTCP connections");

/*
 * Set notsent lowat mark on the MPTCB
 */
int
mptcp_set_notsent_lowat(struct mptses *mpte, int optval)
{
	struct mptcb *mp_tp = NULL;
	int error = 0;

	if (mpte->mpte_mppcb->mpp_flags & MPP_ATTACHED) {
		mp_tp = mpte->mpte_mptcb;
	}

	if (mp_tp) {
		mp_tp->mpt_notsent_lowat = optval;
	} else {
		error = EINVAL;
	}

	return error;
}

u_int32_t
mptcp_get_notsent_lowat(struct mptses *mpte)
{
	struct mptcb *mp_tp = NULL;

	if (mpte->mpte_mppcb->mpp_flags & MPP_ATTACHED) {
		mp_tp = mpte->mpte_mptcb;
	}

	if (mp_tp) {
		return mp_tp->mpt_notsent_lowat;
	} else {
		return 0;
	}
}

int
mptcp_notsent_lowat_check(struct socket *so)
{
	struct mptses *mpte;
	struct mppcb *mpp;
	struct mptcb *mp_tp;
	struct mptsub *mpts;

	int notsent = 0;

	mpp = mpsotomppcb(so);
	if (mpp == NULL || mpp->mpp_state == MPPCB_STATE_DEAD) {
		return 0;
	}

	mpte = mptompte(mpp);
	socket_lock_assert_owned(mptetoso(mpte));
	mp_tp = mpte->mpte_mptcb;

	notsent = so->so_snd.sb_cc;

	if ((notsent == 0) ||
	    ((notsent - (mp_tp->mpt_sndnxt - mp_tp->mpt_snduna)) <=
	    mp_tp->mpt_notsent_lowat)) {
		mptcplog((LOG_DEBUG, "MPTCP Sender: "
		    "lowat %d notsent %d actual %llu \n",
		    mp_tp->mpt_notsent_lowat, notsent,
		    notsent - (mp_tp->mpt_sndnxt - mp_tp->mpt_snduna)),
		    MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);
		return 1;
	}

	/* When Nagle's algorithm is not disabled, it is better
	 * to wakeup the client even before there is atleast one
	 * maxseg of data to write.
	 */
	TAILQ_FOREACH(mpts, &mpte->mpte_subflows, mpts_entry) {
		int retval = 0;
		if (mpts->mpts_flags & MPTSF_ACTIVE) {
			struct socket *subf_so = mpts->mpts_socket;
			struct tcpcb *tp = intotcpcb(sotoinpcb(subf_so));

			notsent = so->so_snd.sb_cc -
			    (tp->snd_nxt - tp->snd_una);

			if ((tp->t_flags & TF_NODELAY) == 0 &&
			    notsent > 0 && (notsent <= (int)tp->t_maxseg)) {
				retval = 1;
			}
			mptcplog((LOG_DEBUG, "MPTCP Sender: lowat %d notsent %d"
			    " nodelay false \n",
			    mp_tp->mpt_notsent_lowat, notsent),
			    MPTCP_SENDER_DBG, MPTCP_LOGLVL_VERBOSE);
			return retval;
		}
	}
	return 0;
}

static errno_t
mptcp_symptoms_ctl_connect(kern_ctl_ref kctlref, struct sockaddr_ctl *sac,
    void **unitinfo)
{
#pragma unused(kctlref, sac, unitinfo)

	if (OSIncrementAtomic(&mptcp_kern_skt_inuse) > 0) {
		os_log_error(mptcp_log_handle, "%s: MPTCP kernel-control socket for Symptoms already open!", __func__);
	}

	mptcp_kern_skt_unit = sac->sc_unit;

	return 0;
}

static void
mptcp_allow_uuid(uuid_t uuid, int32_t rssi)
{
	struct mppcb *mpp;

	/* Iterate over all MPTCP connections */

	lck_mtx_lock(&mtcbinfo.mppi_lock);

	TAILQ_FOREACH(mpp, &mtcbinfo.mppi_pcbs, mpp_entry) {
		struct socket *mp_so = mpp->mpp_socket;
		struct mptses *mpte = mpp->mpp_pcbe;

		socket_lock(mp_so, 1);

		if (mp_so->so_flags & SOF_DELEGATED &&
		    uuid_compare(uuid, mp_so->e_uuid)) {
			goto next;
		} else if (!(mp_so->so_flags & SOF_DELEGATED) &&
		    uuid_compare(uuid, mp_so->last_uuid)) {
			goto next;
		}

		os_log(mptcp_log_handle, "%s - %lx: Got allowance for useApp with rssi %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), rssi);

		mpte->mpte_flags |= MPTE_ACCESS_GRANTED;

		if (rssi > MPTCP_TARGET_BASED_RSSI_THRESHOLD) {
			mpte->mpte_flags |= MPTE_CELL_PROHIBITED;
		}

		mptcp_check_subflows_and_add(mpte);
		mptcp_remove_subflows(mpte);

		mpte->mpte_flags &= ~(MPTE_ACCESS_GRANTED | MPTE_CELL_PROHIBITED);

next:
		socket_unlock(mp_so, 1);
	}

	lck_mtx_unlock(&mtcbinfo.mppi_lock);
}

static void
mptcp_wifi_status_changed(void)
{
	struct mppcb *mpp;

	/* Iterate over all MPTCP connections */

	lck_mtx_lock(&mtcbinfo.mppi_lock);

	TAILQ_FOREACH(mpp, &mtcbinfo.mppi_pcbs, mpp_entry) {
		struct socket *mp_so = mpp->mpp_socket;
		struct mptses *mpte = mpp->mpp_pcbe;

		socket_lock(mp_so, 1);

		/* Only handover- and urgency-mode are purely driven by Symptom's Wi-Fi status */
		if (mpte->mpte_svctype != MPTCP_SVCTYPE_HANDOVER &&
		    mpte->mpte_svctype != MPTCP_SVCTYPE_PURE_HANDOVER &&
		    mpte->mpte_svctype != MPTCP_SVCTYPE_TARGET_BASED) {
			goto next;
		}

		mptcp_check_subflows_and_add(mpte);
		mptcp_check_subflows_and_remove(mpte);

next:
		socket_unlock(mp_so, 1);
	}

	lck_mtx_unlock(&mtcbinfo.mppi_lock);
}

struct mptcp_uuid_search_info {
	uuid_t target_uuid;
	proc_t found_proc;
	boolean_t is_proc_found;
};

static int
mptcp_find_proc_filter(proc_t p, void *arg)
{
	struct mptcp_uuid_search_info *info = (struct mptcp_uuid_search_info *)arg;
	int found;

	if (info->is_proc_found) {
		return 0;
	}

	/*
	 * uuid_compare returns 0 if the uuids are matching, but the proc-filter
	 * expects != 0 for a matching filter.
	 */
	found = uuid_compare(p->p_uuid, info->target_uuid) == 0;
	if (found) {
		info->is_proc_found = true;
	}

	return found;
}

static int
mptcp_find_proc_callout(proc_t p, void * arg)
{
	struct mptcp_uuid_search_info *info = (struct mptcp_uuid_search_info *)arg;

	if (uuid_compare(p->p_uuid, info->target_uuid) == 0) {
		info->found_proc = p;
		return PROC_CLAIMED_DONE;
	}

	return PROC_RETURNED;
}

static proc_t
mptcp_find_proc(const uuid_t uuid)
{
	struct mptcp_uuid_search_info info;

	uuid_copy(info.target_uuid, uuid);
	info.found_proc = PROC_NULL;
	info.is_proc_found = false;

	proc_iterate(PROC_ALLPROCLIST, mptcp_find_proc_callout, &info,
	    mptcp_find_proc_filter, &info);

	return info.found_proc;
}

void
mptcp_ask_symptoms(struct mptses *mpte)
{
	struct mptcp_symptoms_ask_uuid ask;
	struct socket *mp_so;
	struct proc *p = PROC_NULL;
	int pid, prio, err;

	if (mptcp_kern_skt_unit == 0) {
		os_log_error(mptcp_log_handle, "%s - %lx: skt_unit is still 0\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));
		return;
	}

	mp_so = mptetoso(mpte);

	if (mp_so->so_flags & SOF_DELEGATED) {
		if (mpte->mpte_epid != 0) {
			p = proc_find(mpte->mpte_epid);
			if (p != PROC_NULL) {
				/* We found a pid, check its UUID */
				if (uuid_compare(mp_so->e_uuid, p->p_uuid)) {
					/* It's not the same - we need to look for the real proc */
					proc_rele(p);
					p = PROC_NULL;
				}
			}
		}

		if (p == PROC_NULL) {
			p = mptcp_find_proc(mp_so->e_uuid);
			if (p == PROC_NULL) {
				uuid_string_t uuid_string;
				uuid_unparse(mp_so->e_uuid, uuid_string);

				os_log_error(mptcp_log_handle, "%s - %lx: Couldn't find proc for uuid %s\n",
				    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), uuid_string);

				return;
			}
			mpte->mpte_epid = proc_pid(p);
		}

		pid = mpte->mpte_epid;
		uuid_copy(ask.uuid, mp_so->e_uuid);
	} else {
		pid = mp_so->last_pid;

		p = proc_find(pid);
		if (p == PROC_NULL) {
			os_log_error(mptcp_log_handle, "%s - %lx: Couldn't find proc for pid %u\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), pid);
			return;
		}

		uuid_copy(ask.uuid, mp_so->last_uuid);
	}


	ask.cmd = MPTCP_SYMPTOMS_ASK_UUID;

	prio = proc_get_effective_task_policy(proc_task(p), TASK_POLICY_ROLE);

	if (prio == TASK_BACKGROUND_APPLICATION || prio == TASK_NONUI_APPLICATION ||
	    prio == TASK_DARWINBG_APPLICATION) {
		ask.priority = MPTCP_SYMPTOMS_BACKGROUND;
	} else if (prio == TASK_FOREGROUND_APPLICATION) {
		ask.priority = MPTCP_SYMPTOMS_FOREGROUND;
	} else {
		ask.priority = MPTCP_SYMPTOMS_UNKNOWN;
	}

	err = ctl_enqueuedata(mptcp_kern_ctrl_ref, mptcp_kern_skt_unit,
	    &ask, sizeof(ask), CTL_DATA_EOR);

	os_log(mptcp_log_handle, "%s - %lx: asked symptoms about pid %u, taskprio %u, prio %u, err %d\n",
	    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), pid, prio, ask.priority, err);


	proc_rele(p);
}

static errno_t
mptcp_symptoms_ctl_disconnect(kern_ctl_ref kctlref, u_int32_t kcunit,
    void *unitinfo)
{
#pragma unused(kctlref, kcunit, unitinfo)

	OSDecrementAtomic(&mptcp_kern_skt_inuse);

	return 0;
}

static errno_t
mptcp_symptoms_ctl_send(kern_ctl_ref kctlref, u_int32_t kcunit, void *unitinfo,
    mbuf_t m, int flags)
{
#pragma unused(kctlref, unitinfo, flags)
	symptoms_advisory_t *sa = NULL;

	if (kcunit != mptcp_kern_skt_unit) {
		os_log_error(mptcp_log_handle, "%s: kcunit %u is different from expected one %u\n",
		    __func__, kcunit, mptcp_kern_skt_unit);
	}

	if (mbuf_pkthdr_len(m) < sizeof(*sa)) {
		mbuf_freem(m);
		return EINVAL;
	}

	if (mbuf_len(m) < sizeof(*sa)) {
		os_log_error(mptcp_log_handle, "%s: mbuf is %lu but need %lu\n",
		    __func__, mbuf_len(m), sizeof(*sa));
		mbuf_freem(m);
		return EINVAL;
	}

	sa = mbuf_data(m);

	if (sa->sa_nwk_status != SYMPTOMS_ADVISORY_USEAPP) {
		os_log(mptcp_log_handle, "%s: wifi new,old: %d,%d, cell new, old: %d,%d\n", __func__,
		    sa->sa_wifi_status, mptcp_advisory.sa_wifi_status,
		    sa->sa_cell_status, mptcp_advisory.sa_cell_status);

		if (sa->sa_wifi_status != mptcp_advisory.sa_wifi_status) {
			mptcp_advisory.sa_wifi_status = sa->sa_wifi_status;
			mptcp_wifi_status_changed();
		}
	} else {
		struct mptcp_symptoms_answer answer;
		errno_t err;

		/* We temporarily allow different sizes for ease of submission */
		if (mbuf_len(m) != sizeof(uuid_t) + sizeof(*sa) &&
		    mbuf_len(m) != sizeof(answer)) {
			os_log_error(mptcp_log_handle, "%s: mbuf is %lu but need %lu or %lu\n",
			    __func__, mbuf_len(m), sizeof(uuid_t) + sizeof(*sa),
			    sizeof(answer));
			mbuf_free(m);
			return EINVAL;
		}

		memset(&answer, 0, sizeof(answer));

		err = mbuf_copydata(m, 0, mbuf_len(m), &answer);
		if (err) {
			os_log_error(mptcp_log_handle, "%s: mbuf_copydata returned %d\n", __func__, err);
			mbuf_free(m);
			return err;
		}

		mptcp_allow_uuid(answer.uuid, answer.rssi);
	}

	mbuf_freem(m);
	return 0;
}

void
mptcp_control_register(void)
{
	/* Set up the advisory control socket */
	struct kern_ctl_reg mptcp_kern_ctl;

	bzero(&mptcp_kern_ctl, sizeof(mptcp_kern_ctl));
	strlcpy(mptcp_kern_ctl.ctl_name, MPTCP_KERN_CTL_NAME,
	    sizeof(mptcp_kern_ctl.ctl_name));
	mptcp_kern_ctl.ctl_connect = mptcp_symptoms_ctl_connect;
	mptcp_kern_ctl.ctl_disconnect = mptcp_symptoms_ctl_disconnect;
	mptcp_kern_ctl.ctl_send = mptcp_symptoms_ctl_send;
	mptcp_kern_ctl.ctl_flags = CTL_FLAG_PRIVILEGED;

	(void)ctl_register(&mptcp_kern_ctl, &mptcp_kern_ctrl_ref);
}

/*
 * Three return-values:
 * 1  : WiFi is bad
 * 0  : WiFi is good
 * -1 : WiFi-state is unknown
 */
int
mptcp_is_wifi_unusable_for_session(struct mptses *mpte)
{
	if (mpte->mpte_flags & MPTE_FIRSTPARTY) {
		if (mpte->mpte_svctype != MPTCP_SVCTYPE_HANDOVER &&
		    mptcp_advisory.sa_wifi_status) {
			return symptoms_is_wifi_lossy() ? 1 : 0;
		}

		/*
		 * If it's a first-party app and we don't have any info
		 * about the Wi-Fi state, let's be pessimistic.
		 */
		return -1;
	} else {
		if (mptcp_advisory.sa_wifi_status & SYMPTOMS_ADVISORY_WIFI_BAD) {
			return 1;
		}

		/*
		 * If we are target-based (meaning, we allow to be more lax on
		 * the "unusable" target. We only *know* about the state once
		 * we got the allowance from Symptoms (MPTE_ACCESS_GRANTED).
		 *
		 * If RSSI is not bad enough, MPTE_CELL_PROHIBITED will then
		 * be set.
		 *
		 * In any other case (while in target-mode), consider WiFi bad
		 * and we are going to ask for allowance from Symptoms anyway.
		 */
		if (mpte->mpte_svctype == MPTCP_SVCTYPE_TARGET_BASED) {
			if (mpte->mpte_flags & MPTE_ACCESS_GRANTED &&
			    mpte->mpte_flags & MPTE_CELL_PROHIBITED) {
				return 0;
			}

			return 1;
		}

		return 0;
	}
}

boolean_t
symptoms_is_wifi_lossy(void)
{
	return (mptcp_advisory.sa_wifi_status & SYMPTOMS_ADVISORY_WIFI_OK) ? false : true;
}

/* If TFO data is succesfully acked, it must be dropped from the mptcp so */
static void
mptcp_drop_tfo_data(struct mptses *mpte, struct mptsub *mpts)
{
	struct socket *mp_so = mptetoso(mpte);
	struct socket *so = mpts->mpts_socket;
	struct tcpcb *tp = intotcpcb(sotoinpcb(so));
	struct mptcb *mp_tp = mpte->mpte_mptcb;

	/* If data was sent with SYN, rewind state */
	if (tp->t_tfo_stats & TFO_S_SYN_DATA_ACKED) {
		u_int64_t mp_droplen = mp_tp->mpt_sndnxt - mp_tp->mpt_snduna;
		unsigned int tcp_droplen = tp->snd_una - tp->iss - 1;

		VERIFY(mp_droplen <= (UINT_MAX));
		VERIFY(mp_droplen >= tcp_droplen);

		mpts->mpts_flags &= ~MPTSF_TFO_REQD;
		mpts->mpts_iss += tcp_droplen;
		tp->t_mpflags &= ~TMPF_TFO_REQUEST;

		if (mp_droplen > tcp_droplen) {
			/* handle partial TCP ack */
			mp_so->so_flags1 |= SOF1_TFO_REWIND;
			mp_tp->mpt_sndnxt = mp_tp->mpt_snduna + (mp_droplen - tcp_droplen);
			mp_droplen = tcp_droplen;
		} else {
			/* all data on SYN was acked */
			mpts->mpts_rel_seq = 1;
			mp_tp->mpt_sndnxt = mp_tp->mpt_snduna;
		}
		mp_tp->mpt_sndmax -= tcp_droplen;

		if (mp_droplen != 0) {
			VERIFY(mp_so->so_snd.sb_mb != NULL);
			sbdrop(&mp_so->so_snd, (int)mp_droplen);
		}
	}
}

int
mptcp_freeq(struct mptcb *mp_tp)
{
	struct tseg_qent *q;
	int rv = 0;

	while ((q = LIST_FIRST(&mp_tp->mpt_segq)) != NULL) {
		LIST_REMOVE(q, tqe_q);
		m_freem(q->tqe_m);
		zfree(tcp_reass_zone, q);
		rv = 1;
	}
	mp_tp->mpt_reassqlen = 0;
	return rv;
}

static int
mptcp_post_event(u_int32_t event_code, int value)
{
	struct kev_mptcp_data event_data;
	struct kev_msg ev_msg;

	memset(&ev_msg, 0, sizeof(ev_msg));

	ev_msg.vendor_code      = KEV_VENDOR_APPLE;
	ev_msg.kev_class        = KEV_NETWORK_CLASS;
	ev_msg.kev_subclass     = KEV_MPTCP_SUBCLASS;
	ev_msg.event_code       = event_code;

	event_data.value = value;

	ev_msg.dv[0].data_ptr    = &event_data;
	ev_msg.dv[0].data_length = sizeof(event_data);

	return kev_post_msg(&ev_msg);
}

static void
mptcp_set_cellicon(struct mptses *mpte, struct mptsub *mpts)
{
	struct tcpcb *tp = sototcpcb(mpts->mpts_socket);
	int error;

	/* First-party apps (Siri) don't flip the cellicon */
	if (mpte->mpte_flags & MPTE_FIRSTPARTY) {
		return;
	}

	/* Subflow is disappearing - don't set it on this one */
	if (mpts->mpts_flags & (MPTSF_DISCONNECTING | MPTSF_DISCONNECTED)) {
		return;
	}

	/* Fallen back connections are not triggering the cellicon */
	if (mpte->mpte_mptcb->mpt_flags & MPTCPF_FALLBACK_TO_TCP) {
		return;
	}

	/* Remember the last time we set the cellicon. Needed for debouncing */
	mpte->mpte_last_cellicon_set = tcp_now;

	tp->t_timer[TCPT_CELLICON] = OFFSET_FROM_START(tp, MPTCP_CELLICON_TOGGLE_RATE);
	tcp_sched_timers(tp);

	if (mpts->mpts_flags & MPTSF_CELLICON_SET &&
	    mpte->mpte_cellicon_increments != 0) {
		if (mptcp_cellicon_refcount == 0) {
			os_log_error(mptcp_log_handle, "%s - %lx: Cell should be set (count is %u), but it's zero!\n",
			    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpte->mpte_cellicon_increments);

			/* Continue, so that the icon gets set... */
		} else {
			/*
			 * In this case, the cellicon is already set. No need to bump it
			 * even higher
			 */

			return;
		}
	}

	/* When tearing down this subflow, we need to decrement the
	 * reference counter
	 */
	mpts->mpts_flags |= MPTSF_CELLICON_SET;

	/* This counter, so that when a session gets destroyed we decrement
	 * the reference counter by whatever is left
	 */
	mpte->mpte_cellicon_increments++;

	if (OSIncrementAtomic(&mptcp_cellicon_refcount)) {
		/* If cellicon is already set, get out of here! */
		return;
	}

	error = mptcp_post_event(KEV_MPTCP_CELLUSE, 1);

	if (error) {
		os_log_error(mptcp_log_handle, "%s - %lx: Setting cellicon failed with %d\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), error);
	} else {
		os_log(mptcp_log_handle, "%s - %lx: successfully set the cellicon\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte));
	}
}

void
mptcp_clear_cellicon(void)
{
	int error = mptcp_post_event(KEV_MPTCP_CELLUSE, 0);

	if (error) {
		os_log_error(mptcp_log_handle, "%s: Unsetting cellicon failed with %d\n",
		    __func__, error);
	} else {
		os_log(mptcp_log_handle, "%s: successfully unset the cellicon\n",
		    __func__);
	}
}

/*
 * Returns true if the icon has been flipped to WiFi.
 */
static boolean_t
__mptcp_unset_cellicon(uint32_t val)
{
	VERIFY(val < INT32_MAX);
	if (OSAddAtomic((int32_t)-val, &mptcp_cellicon_refcount) != 1) {
		return false;
	}

	mptcp_clear_cellicon();

	return true;
}

void
mptcp_unset_cellicon(struct mptses *mpte, struct mptsub *mpts, uint32_t val)
{
	/* First-party apps (Siri) don't flip the cellicon */
	if (mpte->mpte_flags & MPTE_FIRSTPARTY) {
		return;
	}

	if (mpte->mpte_cellicon_increments == 0) {
		/* This flow never used cell - get out of here! */
		return;
	}

	if (mptcp_cellicon_refcount == 0) {
		os_log_error(mptcp_log_handle, "%s - %lx: Cell is off, but should be at least %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpte->mpte_cellicon_increments);

		return;
	}

	if (mpts) {
		if (!(mpts->mpts_flags & MPTSF_CELLICON_SET)) {
			return;
		}

		mpts->mpts_flags &= ~MPTSF_CELLICON_SET;
	}

	if (mpte->mpte_cellicon_increments < val) {
		os_log_error(mptcp_log_handle, "%s - %lx: Increments is %u but want to dec by %u.\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpte->mpte_cellicon_increments, val);
		val = mpte->mpte_cellicon_increments;
	}

	mpte->mpte_cellicon_increments -= val;

	if (__mptcp_unset_cellicon(val) == false) {
		return;
	}

	/* All flows are gone - our counter should be at zero too! */
	if (mpte->mpte_cellicon_increments != 0) {
		os_log_error(mptcp_log_handle, "%s - %lx: Inconsistent state! Cell refcount is zero but increments are at %u\n",
		    __func__, (unsigned long)VM_KERNEL_ADDRPERM(mpte), mpte->mpte_cellicon_increments);
	}
}

void
mptcp_reset_rexmit_state(struct tcpcb *tp)
{
	struct mptsub *mpts;
	struct inpcb *inp;
	struct socket *so;

	inp = tp->t_inpcb;
	if (inp == NULL) {
		return;
	}

	so = inp->inp_socket;
	if (so == NULL) {
		return;
	}

	if (!(so->so_flags & SOF_MP_SUBFLOW)) {
		return;
	}

	mpts = tp->t_mpsub;

	mpts->mpts_flags &= ~MPTSF_WRITE_STALL;
	so->so_flags &= ~SOF_MP_TRYFAILOVER;
}

void
mptcp_reset_keepalive(struct tcpcb *tp)
{
	struct mptsub *mpts = tp->t_mpsub;

	mpts->mpts_flags &= ~MPTSF_READ_STALL;
}
