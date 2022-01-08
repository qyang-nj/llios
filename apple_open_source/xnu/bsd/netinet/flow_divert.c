/*
 * Copyright (c) 2012-2017, 2020, 2021 Apple Inc. All rights reserved.
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

#include <string.h>
#include <sys/types.h>
#include <sys/syslog.h>
#include <sys/queue.h>
#include <sys/malloc.h>
#include <sys/socket.h>
#include <sys/kpi_mbuf.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socketvar.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/kern_control.h>
#include <sys/ubc.h>
#include <sys/codesign.h>
#include <libkern/tree.h>
#include <kern/locks.h>
#include <kern/debug.h>
#include <kern/task.h>
#include <mach/task_info.h>
#include <net/if_var.h>
#include <net/route.h>
#include <net/flowhash.h>
#include <net/ntstat.h>
#include <net/content_filter.h>
#include <net/necp.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>
#include <netinet/tcp_fsm.h>
#include <netinet/flow_divert.h>
#include <netinet/flow_divert_proto.h>
#include <netinet6/in6_pcb.h>
#include <netinet6/ip6protosw.h>
#include <dev/random/randomdev.h>
#include <libkern/crypto/sha1.h>
#include <libkern/crypto/crypto_internal.h>
#include <os/log.h>
#include <corecrypto/cc.h>
#if CONTENT_FILTER
#include <net/content_filter.h>
#endif /* CONTENT_FILTER */

#define FLOW_DIVERT_CONNECT_STARTED             0x00000001
#define FLOW_DIVERT_READ_CLOSED                 0x00000002
#define FLOW_DIVERT_WRITE_CLOSED                0x00000004
#define FLOW_DIVERT_TUNNEL_RD_CLOSED    0x00000008
#define FLOW_DIVERT_TUNNEL_WR_CLOSED    0x00000010
#define FLOW_DIVERT_HAS_HMAC            0x00000040
#define FLOW_DIVERT_NOTIFY_ON_RECEIVED  0x00000080
#define FLOW_DIVERT_IMPLICIT_CONNECT    0x00000100
#define FLOW_DIVERT_DID_SET_LOCAL_ADDR  0x00000200
#define FLOW_DIVERT_HAS_TOKEN           0x00000400
#define FLOW_DIVERT_SHOULD_SET_LOCAL_ADDR 0x00000800
#define FLOW_DIVERT_FLOW_IS_TRANSPARENT   0x00001000

#define FDLOG(level, pcb, format, ...) \
	os_log_with_type(OS_LOG_DEFAULT, flow_divert_syslog_type_to_oslog_type(level), "(%u): " format "\n", (pcb)->hash, __VA_ARGS__)

#define FDLOG0(level, pcb, msg) \
	os_log_with_type(OS_LOG_DEFAULT, flow_divert_syslog_type_to_oslog_type(level), "(%u): " msg "\n", (pcb)->hash)

#define FDRETAIN(pcb)                   if ((pcb) != NULL) OSIncrementAtomic(&(pcb)->ref_count)
#define FDRELEASE(pcb)                                                                                                          \
	do {                                                                                                                                    \
	        if ((pcb) != NULL && 1 == OSDecrementAtomic(&(pcb)->ref_count)) {       \
	                flow_divert_pcb_destroy(pcb);                                                                   \
	        }                                                                                                                                       \
	} while (0)

#define FDLOCK(pcb)                                             lck_mtx_lock(&(pcb)->mtx)
#define FDUNLOCK(pcb)                                   lck_mtx_unlock(&(pcb)->mtx)

#define FD_CTL_SENDBUFF_SIZE                    (128 * 1024)
#define FD_CTL_RCVBUFF_SIZE                             (128 * 1024)

#define GROUP_BIT_CTL_ENQUEUE_BLOCKED   0

#define GROUP_COUNT_MAX                                 31
#define FLOW_DIVERT_MAX_NAME_SIZE               4096
#define FLOW_DIVERT_MAX_KEY_SIZE                1024
#define FLOW_DIVERT_MAX_TRIE_MEMORY             (1024 * 1024)

struct flow_divert_trie_node {
	uint16_t start;
	uint16_t length;
	uint16_t child_map;
};

#define CHILD_MAP_SIZE                  256
#define NULL_TRIE_IDX                   0xffff
#define TRIE_NODE(t, i)                 ((t)->nodes[(i)])
#define TRIE_CHILD(t, i, b)             (((t)->child_maps + (CHILD_MAP_SIZE * TRIE_NODE(t, i).child_map))[(b)])
#define TRIE_BYTE(t, i)                 ((t)->bytes[(i)])

static struct flow_divert_pcb           nil_pcb;

decl_lck_rw_data(static, g_flow_divert_group_lck);
static struct flow_divert_group         **g_flow_divert_groups                  = NULL;
static uint32_t                                         g_active_group_count                    = 0;

static  lck_grp_attr_t                          *flow_divert_grp_attr                   = NULL;
static  lck_attr_t                                      *flow_divert_mtx_attr                   = NULL;
static  lck_grp_t                                       *flow_divert_mtx_grp                    = NULL;
static  errno_t                                         g_init_result                                   = 0;

static  kern_ctl_ref                            g_flow_divert_kctl_ref                  = NULL;

static struct protosw                           g_flow_divert_in_protosw;
static struct pr_usrreqs                        g_flow_divert_in_usrreqs;
static struct protosw                           g_flow_divert_in_udp_protosw;
static struct pr_usrreqs                        g_flow_divert_in_udp_usrreqs;
static struct ip6protosw                        g_flow_divert_in6_protosw;
static struct pr_usrreqs                        g_flow_divert_in6_usrreqs;
static struct ip6protosw                        g_flow_divert_in6_udp_protosw;
static struct pr_usrreqs                        g_flow_divert_in6_udp_usrreqs;

static struct protosw                           *g_tcp_protosw                                  = NULL;
static struct ip6protosw                        *g_tcp6_protosw                                 = NULL;
static struct protosw                           *g_udp_protosw                                  = NULL;
static struct ip6protosw                        *g_udp6_protosw                                 = NULL;

ZONE_DECLARE(flow_divert_group_zone, "flow_divert_group",
    sizeof(struct flow_divert_group), ZC_ZFREE_CLEARMEM | ZC_NOENCRYPT);
ZONE_DECLARE(flow_divert_pcb_zone, "flow_divert_pcb",
    sizeof(struct flow_divert_pcb), ZC_ZFREE_CLEARMEM | ZC_NOENCRYPT);

static errno_t
flow_divert_dup_addr(sa_family_t family, struct sockaddr *addr, struct sockaddr **dup);

static boolean_t
flow_divert_is_sockaddr_valid(struct sockaddr *addr);

static int
flow_divert_append_target_endpoint_tlv(mbuf_t connect_packet, struct sockaddr *toaddr);

struct sockaddr *
flow_divert_get_buffered_target_address(mbuf_t buffer);

static void
flow_divert_disconnect_socket(struct socket *so);

static inline uint8_t
flow_divert_syslog_type_to_oslog_type(int syslog_type)
{
	switch (syslog_type) {
	case LOG_ERR: return OS_LOG_TYPE_ERROR;
	case LOG_INFO: return OS_LOG_TYPE_INFO;
	case LOG_DEBUG: return OS_LOG_TYPE_DEBUG;
	default: return OS_LOG_TYPE_DEFAULT;
	}
}

static inline int
flow_divert_pcb_cmp(const struct flow_divert_pcb *pcb_a, const struct flow_divert_pcb *pcb_b)
{
	return memcmp(&pcb_a->hash, &pcb_b->hash, sizeof(pcb_a->hash));
}

RB_PROTOTYPE(fd_pcb_tree, flow_divert_pcb, rb_link, flow_divert_pcb_cmp);
RB_GENERATE(fd_pcb_tree, flow_divert_pcb, rb_link, flow_divert_pcb_cmp);

static const char *
flow_divert_packet_type2str(uint8_t packet_type)
{
	switch (packet_type) {
	case FLOW_DIVERT_PKT_CONNECT:
		return "connect";
	case FLOW_DIVERT_PKT_CONNECT_RESULT:
		return "connect result";
	case FLOW_DIVERT_PKT_DATA:
		return "data";
	case FLOW_DIVERT_PKT_CLOSE:
		return "close";
	case FLOW_DIVERT_PKT_READ_NOTIFY:
		return "read notification";
	case FLOW_DIVERT_PKT_PROPERTIES_UPDATE:
		return "properties update";
	case FLOW_DIVERT_PKT_APP_MAP_CREATE:
		return "app map create";
	default:
		return "unknown";
	}
}

static struct flow_divert_pcb *
flow_divert_pcb_lookup(uint32_t hash, struct flow_divert_group *group)
{
	struct flow_divert_pcb  key_item;
	struct flow_divert_pcb  *fd_cb          = NULL;

	key_item.hash = hash;

	lck_rw_lock_shared(&group->lck);
	fd_cb = RB_FIND(fd_pcb_tree, &group->pcb_tree, &key_item);
	FDRETAIN(fd_cb);
	lck_rw_done(&group->lck);

	return fd_cb;
}

static errno_t
flow_divert_pcb_insert(struct flow_divert_pcb *fd_cb, uint32_t ctl_unit)
{
	errno_t                                                 error                                           = 0;
	struct                                          flow_divert_pcb *exist          = NULL;
	struct flow_divert_group        *group;
	static uint32_t                         g_nextkey                                       = 1;
	static uint32_t                         g_hash_seed                                     = 0;
	int                                                     try_count                                       = 0;

	if (ctl_unit == 0 || ctl_unit >= GROUP_COUNT_MAX) {
		return EINVAL;
	}

	socket_unlock(fd_cb->so, 0);
	lck_rw_lock_shared(&g_flow_divert_group_lck);

	if (g_flow_divert_groups == NULL || g_active_group_count == 0) {
		FDLOG0(LOG_ERR, &nil_pcb, "No active groups, flow divert cannot be used for this socket");
		error = ENETUNREACH;
		goto done;
	}

	group = g_flow_divert_groups[ctl_unit];
	if (group == NULL) {
		FDLOG(LOG_ERR, &nil_pcb, "Group for control unit %u is NULL, flow divert cannot be used for this socket", ctl_unit);
		error = ENETUNREACH;
		goto done;
	}

	socket_lock(fd_cb->so, 0);

	do {
		uint32_t        key[2];
		uint32_t        idx;

		key[0] = g_nextkey++;
		key[1] = RandomULong();

		if (g_hash_seed == 0) {
			g_hash_seed = RandomULong();
		}

		fd_cb->hash = net_flowhash(key, sizeof(key), g_hash_seed);

		for (idx = 1; idx < GROUP_COUNT_MAX; idx++) {
			struct flow_divert_group *curr_group = g_flow_divert_groups[idx];
			if (curr_group != NULL && curr_group != group) {
				lck_rw_lock_shared(&curr_group->lck);
				exist = RB_FIND(fd_pcb_tree, &curr_group->pcb_tree, fd_cb);
				lck_rw_done(&curr_group->lck);
				if (exist != NULL) {
					break;
				}
			}
		}

		if (exist == NULL) {
			lck_rw_lock_exclusive(&group->lck);
			exist = RB_INSERT(fd_pcb_tree, &group->pcb_tree, fd_cb);
			lck_rw_done(&group->lck);
		}
	} while (exist != NULL && try_count++ < 3);

	if (exist == NULL) {
		fd_cb->group = group;
		FDRETAIN(fd_cb);                /* The group now has a reference */
	} else {
		fd_cb->hash = 0;
		error = EEXIST;
	}

	socket_unlock(fd_cb->so, 0);

done:
	lck_rw_done(&g_flow_divert_group_lck);
	socket_lock(fd_cb->so, 0);

	return error;
}

static struct flow_divert_pcb *
flow_divert_pcb_create(socket_t so)
{
	struct flow_divert_pcb  *new_pcb = NULL;

	new_pcb = zalloc_flags(flow_divert_pcb_zone, Z_WAITOK | Z_ZERO);
	lck_mtx_init(&new_pcb->mtx, flow_divert_mtx_grp, flow_divert_mtx_attr);
	new_pcb->so = so;
	new_pcb->log_level = nil_pcb.log_level;

	FDRETAIN(new_pcb);      /* Represents the socket's reference */

	return new_pcb;
}

static void
flow_divert_pcb_destroy(struct flow_divert_pcb *fd_cb)
{
	FDLOG(LOG_INFO, fd_cb, "Destroying, app tx %u, tunnel tx %u, tunnel rx %u",
	    fd_cb->bytes_written_by_app, fd_cb->bytes_sent, fd_cb->bytes_received);

	if (fd_cb->connect_token != NULL) {
		mbuf_freem(fd_cb->connect_token);
	}
	if (fd_cb->connect_packet != NULL) {
		mbuf_freem(fd_cb->connect_packet);
	}
	if (fd_cb->app_data != NULL) {
		FREE(fd_cb->app_data, M_TEMP);
	}
	if (fd_cb->original_remote_endpoint != NULL) {
		FREE(fd_cb->original_remote_endpoint, M_SONAME);
	}
	zfree(flow_divert_pcb_zone, fd_cb);
}

static void
flow_divert_pcb_remove(struct flow_divert_pcb *fd_cb)
{
	if (fd_cb->group != NULL) {
		struct flow_divert_group *group = fd_cb->group;
		lck_rw_lock_exclusive(&group->lck);
		FDLOG(LOG_INFO, fd_cb, "Removing from group %d, ref count = %d", group->ctl_unit, fd_cb->ref_count);
		RB_REMOVE(fd_pcb_tree, &group->pcb_tree, fd_cb);
		fd_cb->group = NULL;
		FDRELEASE(fd_cb);                               /* Release the group's reference */
		lck_rw_done(&group->lck);
	}
}

static int
flow_divert_packet_init(struct flow_divert_pcb *fd_cb, uint8_t packet_type, mbuf_t *packet)
{
	struct flow_divert_packet_header        hdr;
	int                                     error           = 0;

	error = mbuf_gethdr(MBUF_DONTWAIT, MBUF_TYPE_HEADER, packet);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to allocate the header mbuf: %d", error);
		return error;
	}

	hdr.packet_type = packet_type;
	hdr.conn_id = htonl(fd_cb->hash);

	/* Lay down the header */
	error = mbuf_copyback(*packet, 0, sizeof(hdr), &hdr, MBUF_DONTWAIT);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "mbuf_copyback(hdr) failed: %d", error);
		mbuf_freem(*packet);
		*packet = NULL;
		return error;
	}

	return 0;
}

static int
flow_divert_packet_append_tlv(mbuf_t packet, uint8_t type, uint32_t length, const void *value)
{
	uint32_t        net_length      = htonl(length);
	int                     error           = 0;

	error = mbuf_copyback(packet, mbuf_pkthdr_len(packet), sizeof(type), &type, MBUF_DONTWAIT);
	if (error) {
		FDLOG(LOG_ERR, &nil_pcb, "failed to append the type (%d)", type);
		return error;
	}

	error = mbuf_copyback(packet, mbuf_pkthdr_len(packet), sizeof(net_length), &net_length, MBUF_DONTWAIT);
	if (error) {
		FDLOG(LOG_ERR, &nil_pcb, "failed to append the length (%u)", length);
		return error;
	}

	error = mbuf_copyback(packet, mbuf_pkthdr_len(packet), length, value, MBUF_DONTWAIT);
	if (error) {
		FDLOG0(LOG_ERR, &nil_pcb, "failed to append the value");
		return error;
	}

	return error;
}

static int
flow_divert_packet_find_tlv(mbuf_t packet, int offset, uint8_t type, int *err, int next)
{
	size_t          cursor                  = offset;
	int                     error                   = 0;
	uint32_t        curr_length;
	uint8_t         curr_type;

	*err = 0;

	do {
		if (!next) {
			error = mbuf_copydata(packet, cursor, sizeof(curr_type), &curr_type);
			if (error) {
				*err = ENOENT;
				return -1;
			}
		} else {
			next = 0;
			curr_type = FLOW_DIVERT_TLV_NIL;
		}

		if (curr_type != type) {
			cursor += sizeof(curr_type);
			error = mbuf_copydata(packet, cursor, sizeof(curr_length), &curr_length);
			if (error) {
				*err = error;
				return -1;
			}

			cursor += (sizeof(curr_length) + ntohl(curr_length));
		}
	} while (curr_type != type);

	return (int)cursor;
}

static int
flow_divert_packet_get_tlv(mbuf_t packet, int offset, uint8_t type, size_t buff_len, void *buff, uint32_t *val_size)
{
	int                     error           = 0;
	uint32_t        length;
	int                     tlv_offset;

	tlv_offset = flow_divert_packet_find_tlv(packet, offset, type, &error, 0);
	if (tlv_offset < 0) {
		return error;
	}

	error = mbuf_copydata(packet, tlv_offset + sizeof(type), sizeof(length), &length);
	if (error) {
		return error;
	}

	length = ntohl(length);

	uint32_t data_offset = tlv_offset + sizeof(type) + sizeof(length);

	if (length > (mbuf_pkthdr_len(packet) - data_offset)) {
		FDLOG(LOG_ERR, &nil_pcb, "Length of %u TLV (%u) is larger than remaining packet data (%lu)", type, length, (mbuf_pkthdr_len(packet) - data_offset));
		return EINVAL;
	}

	if (val_size != NULL) {
		*val_size = length;
	}

	if (buff != NULL && buff_len > 0) {
		memset(buff, 0, buff_len);
		size_t to_copy = (length < buff_len) ? length : buff_len;
		error = mbuf_copydata(packet, data_offset, to_copy, buff);
		if (error) {
			return error;
		}
	}

	return 0;
}

static int
flow_divert_packet_compute_hmac(mbuf_t packet, struct flow_divert_group *group, uint8_t *hmac)
{
	mbuf_t  curr_mbuf       = packet;

	if (g_crypto_funcs == NULL || group->token_key == NULL) {
		return ENOPROTOOPT;
	}

	cchmac_di_decl(g_crypto_funcs->ccsha1_di, hmac_ctx);
	g_crypto_funcs->cchmac_init_fn(g_crypto_funcs->ccsha1_di, hmac_ctx, group->token_key_size, group->token_key);

	while (curr_mbuf != NULL) {
		g_crypto_funcs->cchmac_update_fn(g_crypto_funcs->ccsha1_di, hmac_ctx, mbuf_len(curr_mbuf), mbuf_data(curr_mbuf));
		curr_mbuf = mbuf_next(curr_mbuf);
	}

	g_crypto_funcs->cchmac_final_fn(g_crypto_funcs->ccsha1_di, hmac_ctx, hmac);

	return 0;
}

static int
flow_divert_packet_verify_hmac(mbuf_t packet, uint32_t ctl_unit)
{
	int                                                     error = 0;
	struct flow_divert_group        *group = NULL;
	int                                                     hmac_offset;
	uint8_t                                         packet_hmac[SHA_DIGEST_LENGTH];
	uint8_t                                         computed_hmac[SHA_DIGEST_LENGTH];
	mbuf_t                                          tail;

	lck_rw_lock_shared(&g_flow_divert_group_lck);

	if (g_flow_divert_groups != NULL && g_active_group_count > 0) {
		group = g_flow_divert_groups[ctl_unit];
	}

	if (group == NULL) {
		lck_rw_done(&g_flow_divert_group_lck);
		return ENOPROTOOPT;
	}

	lck_rw_lock_shared(&group->lck);

	if (group->token_key == NULL) {
		error = ENOPROTOOPT;
		goto done;
	}

	hmac_offset = flow_divert_packet_find_tlv(packet, 0, FLOW_DIVERT_TLV_HMAC, &error, 0);
	if (hmac_offset < 0) {
		goto done;
	}

	error = flow_divert_packet_get_tlv(packet, hmac_offset, FLOW_DIVERT_TLV_HMAC, sizeof(packet_hmac), packet_hmac, NULL);
	if (error) {
		goto done;
	}

	/* Chop off the HMAC TLV */
	error = mbuf_split(packet, hmac_offset, MBUF_WAITOK, &tail);
	if (error) {
		goto done;
	}

	mbuf_free(tail);

	error = flow_divert_packet_compute_hmac(packet, group, computed_hmac);
	if (error) {
		goto done;
	}

	if (cc_cmp_safe(sizeof(packet_hmac), packet_hmac, computed_hmac)) {
		FDLOG0(LOG_WARNING, &nil_pcb, "HMAC in token does not match computed HMAC");
		error = EINVAL;
		goto done;
	}

done:
	lck_rw_done(&group->lck);
	lck_rw_done(&g_flow_divert_group_lck);
	return error;
}

static void
flow_divert_add_data_statistics(struct flow_divert_pcb *fd_cb, size_t data_len, Boolean send)
{
	struct inpcb *inp = NULL;
	struct ifnet *ifp = NULL;
	Boolean cell = FALSE;
	Boolean wifi = FALSE;
	Boolean wired = FALSE;

	inp = sotoinpcb(fd_cb->so);
	if (inp == NULL) {
		return;
	}

	if (inp->inp_vflag & INP_IPV4) {
		ifp = inp->inp_last_outifp;
	} else if (inp->inp_vflag & INP_IPV6) {
		ifp = inp->in6p_last_outifp;
	}
	if (ifp != NULL) {
		cell = IFNET_IS_CELLULAR(ifp);
		wifi = (!cell && IFNET_IS_WIFI(ifp));
		wired = (!wifi && IFNET_IS_WIRED(ifp));
	}

	if (send) {
		INP_ADD_STAT(inp, cell, wifi, wired, txpackets, 1);
		INP_ADD_STAT(inp, cell, wifi, wired, txbytes, data_len);
	} else {
		INP_ADD_STAT(inp, cell, wifi, wired, rxpackets, 1);
		INP_ADD_STAT(inp, cell, wifi, wired, rxbytes, data_len);
	}
	inp_set_activity_bitmap(inp);
}

static errno_t
flow_divert_check_no_cellular(struct flow_divert_pcb *fd_cb)
{
	struct inpcb *inp = sotoinpcb(fd_cb->so);
	if (INP_NO_CELLULAR(inp)) {
		struct ifnet *ifp = NULL;
		if (inp->inp_vflag & INP_IPV4) {
			ifp = inp->inp_last_outifp;
		} else if (inp->inp_vflag & INP_IPV6) {
			ifp = inp->in6p_last_outifp;
		}
		if (ifp != NULL && IFNET_IS_CELLULAR(ifp)) {
			FDLOG0(LOG_ERR, fd_cb, "Cellular is denied");
			return EHOSTUNREACH;
		}
	}
	return 0;
}

static errno_t
flow_divert_check_no_expensive(struct flow_divert_pcb *fd_cb)
{
	struct inpcb *inp = sotoinpcb(fd_cb->so);
	if (INP_NO_EXPENSIVE(inp)) {
		struct ifnet *ifp = NULL;
		if (inp->inp_vflag & INP_IPV4) {
			ifp = inp->inp_last_outifp;
		} else if (inp->inp_vflag & INP_IPV6) {
			ifp = inp->in6p_last_outifp;
		}
		if (ifp != NULL && IFNET_IS_EXPENSIVE(ifp)) {
			FDLOG0(LOG_ERR, fd_cb, "Expensive is denied");
			return EHOSTUNREACH;
		}
	}
	return 0;
}

static errno_t
flow_divert_check_no_constrained(struct flow_divert_pcb *fd_cb)
{
	struct inpcb *inp = sotoinpcb(fd_cb->so);
	if (INP_NO_CONSTRAINED(inp)) {
		struct ifnet *ifp = NULL;
		if (inp->inp_vflag & INP_IPV4) {
			ifp = inp->inp_last_outifp;
		} else if (inp->inp_vflag & INP_IPV6) {
			ifp = inp->in6p_last_outifp;
		}
		if (ifp != NULL && IFNET_IS_CONSTRAINED(ifp)) {
			FDLOG0(LOG_ERR, fd_cb, "Constrained is denied");
			return EHOSTUNREACH;
		}
	}
	return 0;
}

static void
flow_divert_update_closed_state(struct flow_divert_pcb *fd_cb, int how, Boolean tunnel)
{
	if (how != SHUT_RD) {
		fd_cb->flags |= FLOW_DIVERT_WRITE_CLOSED;
		if (tunnel || !(fd_cb->flags & FLOW_DIVERT_CONNECT_STARTED)) {
			fd_cb->flags |= FLOW_DIVERT_TUNNEL_WR_CLOSED;
			/* If the tunnel is not accepting writes any more, then flush the send buffer */
			sbflush(&fd_cb->so->so_snd);
		}
	}
	if (how != SHUT_WR) {
		fd_cb->flags |= FLOW_DIVERT_READ_CLOSED;
		if (tunnel || !(fd_cb->flags & FLOW_DIVERT_CONNECT_STARTED)) {
			fd_cb->flags |= FLOW_DIVERT_TUNNEL_RD_CLOSED;
		}
	}
}

static uint16_t
trie_node_alloc(struct flow_divert_trie *trie)
{
	if (trie->nodes_free_next < trie->nodes_count) {
		uint16_t node_idx = trie->nodes_free_next++;
		TRIE_NODE(trie, node_idx).child_map = NULL_TRIE_IDX;
		return node_idx;
	} else {
		return NULL_TRIE_IDX;
	}
}

static uint16_t
trie_child_map_alloc(struct flow_divert_trie *trie)
{
	if (trie->child_maps_free_next < trie->child_maps_count) {
		return trie->child_maps_free_next++;
	} else {
		return NULL_TRIE_IDX;
	}
}

static uint16_t
trie_bytes_move(struct flow_divert_trie *trie, uint16_t bytes_idx, size_t bytes_size)
{
	uint16_t start = trie->bytes_free_next;
	if (start + bytes_size <= trie->bytes_count) {
		if (start != bytes_idx) {
			memmove(&TRIE_BYTE(trie, start), &TRIE_BYTE(trie, bytes_idx), bytes_size);
		}
		trie->bytes_free_next += bytes_size;
		return start;
	} else {
		return NULL_TRIE_IDX;
	}
}

static uint16_t
flow_divert_trie_insert(struct flow_divert_trie *trie, uint16_t string_start, size_t string_len)
{
	uint16_t current = trie->root;
	uint16_t child = trie->root;
	uint16_t string_end = string_start + (uint16_t)string_len;
	uint16_t string_idx = string_start;
	uint16_t string_remainder = (uint16_t)string_len;

	while (child != NULL_TRIE_IDX) {
		uint16_t parent = current;
		uint16_t node_idx;
		uint16_t current_end;

		current = child;
		child = NULL_TRIE_IDX;

		current_end = TRIE_NODE(trie, current).start + TRIE_NODE(trie, current).length;

		for (node_idx = TRIE_NODE(trie, current).start;
		    node_idx < current_end &&
		    string_idx < string_end &&
		    TRIE_BYTE(trie, node_idx) == TRIE_BYTE(trie, string_idx);
		    node_idx++, string_idx++) {
			;
		}

		string_remainder = string_end - string_idx;

		if (node_idx < (TRIE_NODE(trie, current).start + TRIE_NODE(trie, current).length)) {
			/*
			 * We did not reach the end of the current node's string.
			 * We need to split the current node into two:
			 *   1. A new node that contains the prefix of the node that matches
			 *      the prefix of the string being inserted.
			 *   2. The current node modified to point to the remainder
			 *      of the current node's string.
			 */
			uint16_t prefix = trie_node_alloc(trie);
			if (prefix == NULL_TRIE_IDX) {
				FDLOG0(LOG_ERR, &nil_pcb, "Ran out of trie nodes while splitting an existing node");
				return NULL_TRIE_IDX;
			}

			/*
			 * Prefix points to the portion of the current nodes's string that has matched
			 * the input string thus far.
			 */
			TRIE_NODE(trie, prefix).start = TRIE_NODE(trie, current).start;
			TRIE_NODE(trie, prefix).length = (node_idx - TRIE_NODE(trie, current).start);

			/*
			 * Prefix has the current node as the child corresponding to the first byte
			 * after the split.
			 */
			TRIE_NODE(trie, prefix).child_map = trie_child_map_alloc(trie);
			if (TRIE_NODE(trie, prefix).child_map == NULL_TRIE_IDX) {
				FDLOG0(LOG_ERR, &nil_pcb, "Ran out of child maps while splitting an existing node");
				return NULL_TRIE_IDX;
			}
			TRIE_CHILD(trie, prefix, TRIE_BYTE(trie, node_idx)) = current;

			/* Parent has the prefix as the child correspoding to the first byte in the prefix */
			TRIE_CHILD(trie, parent, TRIE_BYTE(trie, TRIE_NODE(trie, prefix).start)) = prefix;

			/* Current node is adjusted to point to the remainder */
			TRIE_NODE(trie, current).start = node_idx;
			TRIE_NODE(trie, current).length -= TRIE_NODE(trie, prefix).length;

			/* We want to insert the new leaf (if any) as a child of the prefix */
			current = prefix;
		}

		if (string_remainder > 0) {
			/*
			 * We still have bytes in the string that have not been matched yet.
			 * If the current node has children, iterate to the child corresponding
			 * to the next byte in the string.
			 */
			if (TRIE_NODE(trie, current).child_map != NULL_TRIE_IDX) {
				child = TRIE_CHILD(trie, current, TRIE_BYTE(trie, string_idx));
			}
		}
	} /* while (child != NULL_TRIE_IDX) */

	if (string_remainder > 0) {
		/* Add a new leaf containing the remainder of the string */
		uint16_t leaf = trie_node_alloc(trie);
		if (leaf == NULL_TRIE_IDX) {
			FDLOG0(LOG_ERR, &nil_pcb, "Ran out of trie nodes while inserting a new leaf");
			return NULL_TRIE_IDX;
		}

		TRIE_NODE(trie, leaf).start = trie_bytes_move(trie, string_idx, string_remainder);
		if (TRIE_NODE(trie, leaf).start == NULL_TRIE_IDX) {
			FDLOG0(LOG_ERR, &nil_pcb, "Ran out of bytes while inserting a new leaf");
			return NULL_TRIE_IDX;
		}
		TRIE_NODE(trie, leaf).length = string_remainder;

		/* Set the new leaf as the child of the current node */
		if (TRIE_NODE(trie, current).child_map == NULL_TRIE_IDX) {
			TRIE_NODE(trie, current).child_map = trie_child_map_alloc(trie);
			if (TRIE_NODE(trie, current).child_map == NULL_TRIE_IDX) {
				FDLOG0(LOG_ERR, &nil_pcb, "Ran out of child maps while inserting a new leaf");
				return NULL_TRIE_IDX;
			}
		}
		TRIE_CHILD(trie, current, TRIE_BYTE(trie, TRIE_NODE(trie, leaf).start)) = leaf;
		current = leaf;
	} /* else duplicate or this string is a prefix of one of the existing strings */

	return current;
}

#define APPLE_WEBCLIP_ID_PREFIX "com.apple.webapp"
static uint16_t
flow_divert_trie_search(struct flow_divert_trie *trie, const uint8_t *string_bytes)
{
	uint16_t current = trie->root;
	uint16_t string_idx = 0;

	while (current != NULL_TRIE_IDX) {
		uint16_t next = NULL_TRIE_IDX;
		uint16_t node_end = TRIE_NODE(trie, current).start + TRIE_NODE(trie, current).length;
		uint16_t node_idx;

		for (node_idx = TRIE_NODE(trie, current).start;
		    node_idx < node_end && string_bytes[string_idx] != '\0' && string_bytes[string_idx] == TRIE_BYTE(trie, node_idx);
		    node_idx++, string_idx++) {
			;
		}

		if (node_idx == node_end) {
			if (string_bytes[string_idx] == '\0') {
				return current; /* Got an exact match */
			} else if (string_idx == strlen(APPLE_WEBCLIP_ID_PREFIX) &&
			    0 == strncmp((const char *)string_bytes, APPLE_WEBCLIP_ID_PREFIX, string_idx)) {
				return current; /* Got an apple webclip id prefix match */
			} else if (TRIE_NODE(trie, current).child_map != NULL_TRIE_IDX) {
				next = TRIE_CHILD(trie, current, string_bytes[string_idx]);
			}
		}
		current = next;
	}

	return NULL_TRIE_IDX;
}

struct uuid_search_info {
	uuid_t target_uuid;
	char *found_signing_id;
	boolean_t found_multiple_signing_ids;
	proc_t found_proc;
};

static int
flow_divert_find_proc_by_uuid_callout(proc_t p, void *arg)
{
	struct uuid_search_info *info = (struct uuid_search_info *)arg;
	int result = PROC_RETURNED_DONE; /* By default, we didn't find the process */

	if (info->found_signing_id != NULL) {
		if (!info->found_multiple_signing_ids) {
			/* All processes that were found had the same signing identifier, so just claim this first one and be done. */
			info->found_proc = p;
			result = PROC_CLAIMED_DONE;
		} else {
			uuid_string_t uuid_str;
			uuid_unparse(info->target_uuid, uuid_str);
			FDLOG(LOG_WARNING, &nil_pcb, "Found multiple processes with UUID %s with different signing identifiers", uuid_str);
		}
		FREE(info->found_signing_id, M_TEMP);
		info->found_signing_id = NULL;
	}

	if (result == PROC_RETURNED_DONE) {
		uuid_string_t uuid_str;
		uuid_unparse(info->target_uuid, uuid_str);
		FDLOG(LOG_WARNING, &nil_pcb, "Failed to find a process with UUID %s", uuid_str);
	}

	return result;
}

static int
flow_divert_find_proc_by_uuid_filter(proc_t p, void *arg)
{
	struct uuid_search_info *info = (struct uuid_search_info *)arg;
	int include = 0;

	if (info->found_multiple_signing_ids) {
		return include;
	}

	include = (uuid_compare(p->p_uuid, info->target_uuid) == 0);
	if (include) {
		const char *signing_id = cs_identity_get(p);
		if (signing_id != NULL) {
			FDLOG(LOG_INFO, &nil_pcb, "Found process %d with signing identifier %s", p->p_pid, signing_id);
			size_t signing_id_size = strlen(signing_id) + 1;
			if (info->found_signing_id == NULL) {
				MALLOC(info->found_signing_id, char *, signing_id_size, M_TEMP, M_WAITOK);
				memcpy(info->found_signing_id, signing_id, signing_id_size);
			} else if (memcmp(signing_id, info->found_signing_id, signing_id_size)) {
				info->found_multiple_signing_ids = TRUE;
			}
		} else {
			info->found_multiple_signing_ids = TRUE;
		}
		include = !info->found_multiple_signing_ids;
	}

	return include;
}

static proc_t
flow_divert_find_proc_by_uuid(uuid_t uuid)
{
	struct uuid_search_info info;

	if (LOG_INFO <= nil_pcb.log_level) {
		uuid_string_t uuid_str;
		uuid_unparse(uuid, uuid_str);
		FDLOG(LOG_INFO, &nil_pcb, "Looking for process with UUID %s", uuid_str);
	}

	memset(&info, 0, sizeof(info));
	info.found_proc = PROC_NULL;
	uuid_copy(info.target_uuid, uuid);

	proc_iterate(PROC_ALLPROCLIST, flow_divert_find_proc_by_uuid_callout, &info, flow_divert_find_proc_by_uuid_filter, &info);

	return info.found_proc;
}

static int
flow_divert_add_proc_info(struct flow_divert_pcb *fd_cb, proc_t proc, const char *signing_id, mbuf_t connect_packet, bool is_effective)
{
	int error = 0;
	uint8_t *cdhash = NULL;
	audit_token_t audit_token = {};
	const char *proc_cs_id = signing_id;

	proc_lock(proc);

	if (proc_cs_id == NULL) {
		if (proc->p_csflags & (CS_VALID | CS_DEBUGGED)) {
			proc_cs_id = cs_identity_get(proc);
		} else {
			FDLOG0(LOG_ERR, fd_cb, "Signature of proc is invalid");
		}
	}

	if (is_effective) {
		lck_rw_lock_shared(&fd_cb->group->lck);
		if (!(fd_cb->group->flags & FLOW_DIVERT_GROUP_FLAG_NO_APP_MAP)) {
			if (proc_cs_id != NULL) {
				uint16_t result = flow_divert_trie_search(&fd_cb->group->signing_id_trie, (const uint8_t *)proc_cs_id);
				if (result == NULL_TRIE_IDX) {
					FDLOG(LOG_WARNING, fd_cb, "%s did not match", proc_cs_id);
					error = EPERM;
				} else {
					FDLOG(LOG_INFO, fd_cb, "%s matched", proc_cs_id);
				}
			} else {
				error = EPERM;
			}
		}
		lck_rw_done(&fd_cb->group->lck);
	}

	if (error != 0) {
		goto done;
	}

	/*
	 * If signing_id is not NULL then it came from the flow divert token and will be added
	 * as part of the token, so there is no need to add it here.
	 */
	if (signing_id == NULL && proc_cs_id != NULL) {
		error = flow_divert_packet_append_tlv(connect_packet,
		    (is_effective ? FLOW_DIVERT_TLV_SIGNING_ID : FLOW_DIVERT_TLV_APP_REAL_SIGNING_ID),
		    (uint32_t)strlen(proc_cs_id),
		    proc_cs_id);
		if (error != 0) {
			FDLOG(LOG_ERR, fd_cb, "failed to append the signing ID: %d", error);
			goto done;
		}
	}

	cdhash = cs_get_cdhash(proc);
	if (cdhash != NULL) {
		error = flow_divert_packet_append_tlv(connect_packet,
		    (is_effective ? FLOW_DIVERT_TLV_CDHASH : FLOW_DIVERT_TLV_APP_REAL_CDHASH),
		    SHA1_RESULTLEN,
		    cdhash);
		if (error) {
			FDLOG(LOG_ERR, fd_cb, "failed to append the cdhash: %d", error);
			goto done;
		}
	} else {
		FDLOG0(LOG_ERR, fd_cb, "failed to get the cdhash");
	}

	task_t task = proc_task(proc);
	if (task != TASK_NULL) {
		mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
		kern_return_t rc = task_info(task, TASK_AUDIT_TOKEN, (task_info_t)&audit_token, &count);
		if (rc == KERN_SUCCESS) {
			int append_error = flow_divert_packet_append_tlv(connect_packet,
			    (is_effective ? FLOW_DIVERT_TLV_APP_AUDIT_TOKEN : FLOW_DIVERT_TLV_APP_REAL_AUDIT_TOKEN),
			    sizeof(audit_token_t),
			    &audit_token);
			if (append_error) {
				FDLOG(LOG_ERR, fd_cb, "failed to append app audit token: %d", append_error);
			}
		}
	}

done:
	proc_unlock(proc);

	return error;
}

static int
flow_divert_add_all_proc_info(struct flow_divert_pcb *fd_cb, struct socket *so, proc_t proc, const char *signing_id, mbuf_t connect_packet)
{
	int error = 0;
	proc_t effective_proc = PROC_NULL;
	proc_t responsible_proc = PROC_NULL;
	proc_t real_proc = proc_find(so->last_pid);
	bool release_real_proc = true;

	proc_t src_proc = PROC_NULL;
	proc_t real_src_proc = PROC_NULL;

	if (real_proc == PROC_NULL) {
		FDLOG(LOG_ERR, fd_cb, "failed to find the real proc record for %d", so->last_pid);
		release_real_proc = false;
		real_proc = proc;
		if (real_proc == PROC_NULL) {
			real_proc = current_proc();
		}
	}

	if (so->so_flags & SOF_DELEGATED) {
		if (real_proc->p_pid != so->e_pid) {
			effective_proc = proc_find(so->e_pid);
		} else if (uuid_compare(real_proc->p_uuid, so->e_uuid)) {
			effective_proc = flow_divert_find_proc_by_uuid(so->e_uuid);
		}
	}

#if defined(XNU_TARGET_OS_OSX)
	lck_rw_lock_shared(&fd_cb->group->lck);
	if (!(fd_cb->group->flags & FLOW_DIVERT_GROUP_FLAG_NO_APP_MAP)) {
		if (so->so_rpid > 0) {
			responsible_proc = proc_find(so->so_rpid);
		}
	}
	lck_rw_done(&fd_cb->group->lck);
#endif

	real_src_proc = real_proc;

	if (responsible_proc != PROC_NULL) {
		src_proc = responsible_proc;
		if (effective_proc != NULL) {
			real_src_proc = effective_proc;
		}
	} else if (effective_proc != PROC_NULL) {
		src_proc = effective_proc;
	} else {
		src_proc = real_proc;
	}

	error = flow_divert_add_proc_info(fd_cb, src_proc, signing_id, connect_packet, true);
	if (error != 0) {
		goto done;
	}

	if (real_src_proc != NULL && real_src_proc != src_proc) {
		error = flow_divert_add_proc_info(fd_cb, real_src_proc, NULL, connect_packet, false);
		if (error != 0) {
			goto done;
		}
	}

done:
	if (responsible_proc != PROC_NULL) {
		proc_rele(responsible_proc);
	}

	if (effective_proc != PROC_NULL) {
		proc_rele(effective_proc);
	}

	if (real_proc != PROC_NULL && release_real_proc) {
		proc_rele(real_proc);
	}

	return error;
}

static int
flow_divert_send_packet(struct flow_divert_pcb *fd_cb, mbuf_t packet, Boolean enqueue)
{
	int             error;

	if (fd_cb->group == NULL) {
		fd_cb->so->so_error = ECONNABORTED;
		flow_divert_disconnect_socket(fd_cb->so);
		return ECONNABORTED;
	}

	lck_rw_lock_shared(&fd_cb->group->lck);

	if (MBUFQ_EMPTY(&fd_cb->group->send_queue)) {
		error = ctl_enqueuembuf(g_flow_divert_kctl_ref, fd_cb->group->ctl_unit, packet, CTL_DATA_EOR);
	} else {
		error = ENOBUFS;
	}

	if (error == ENOBUFS) {
		if (enqueue) {
			if (!lck_rw_lock_shared_to_exclusive(&fd_cb->group->lck)) {
				lck_rw_lock_exclusive(&fd_cb->group->lck);
			}
			MBUFQ_ENQUEUE(&fd_cb->group->send_queue, packet);
			error = 0;
		}
		OSTestAndSet(GROUP_BIT_CTL_ENQUEUE_BLOCKED, &fd_cb->group->atomic_bits);
	}

	lck_rw_done(&fd_cb->group->lck);

	return error;
}

static int
flow_divert_create_connect_packet(struct flow_divert_pcb *fd_cb, struct sockaddr *to, struct socket *so, proc_t p, mbuf_t *out_connect_packet)
{
	int                     error                   = 0;
	int                     flow_type               = 0;
	char                    *signing_id = NULL;
	mbuf_t                  connect_packet = NULL;
	cfil_sock_id_t          cfil_sock_id            = CFIL_SOCK_ID_NONE;
	const void              *cfil_id                = NULL;
	size_t                  cfil_id_size            = 0;
	struct inpcb            *inp = sotoinpcb(so);
	struct ifnet *ifp = NULL;
	uint32_t flags = 0;

	error = flow_divert_packet_init(fd_cb, FLOW_DIVERT_PKT_CONNECT, &connect_packet);
	if (error) {
		goto done;
	}

	if (fd_cb->connect_token != NULL && (fd_cb->flags & FLOW_DIVERT_HAS_HMAC)) {
		uint32_t sid_size = 0;
		int find_error = flow_divert_packet_get_tlv(fd_cb->connect_token, 0, FLOW_DIVERT_TLV_SIGNING_ID, 0, NULL, &sid_size);
		if (find_error == 0 && sid_size > 0) {
			MALLOC(signing_id, char *, sid_size + 1, M_TEMP, M_WAITOK | M_ZERO);
			if (signing_id != NULL) {
				flow_divert_packet_get_tlv(fd_cb->connect_token, 0, FLOW_DIVERT_TLV_SIGNING_ID, sid_size, signing_id, NULL);
				FDLOG(LOG_INFO, fd_cb, "Got %s from token", signing_id);
			}
		}
	}

	socket_unlock(so, 0);

	error = flow_divert_add_all_proc_info(fd_cb, so, p, signing_id, connect_packet);

	socket_lock(so, 0);

	if (signing_id != NULL) {
		FREE(signing_id, M_TEMP);
	}

	if (error) {
		FDLOG(LOG_ERR, fd_cb, "Failed to add source proc info: %d", error);
		goto done;
	}

	error = flow_divert_packet_append_tlv(connect_packet,
	    FLOW_DIVERT_TLV_TRAFFIC_CLASS,
	    sizeof(fd_cb->so->so_traffic_class),
	    &fd_cb->so->so_traffic_class);
	if (error) {
		goto done;
	}

	if (SOCK_TYPE(fd_cb->so) == SOCK_STREAM) {
		flow_type = FLOW_DIVERT_FLOW_TYPE_TCP;
	} else if (SOCK_TYPE(fd_cb->so) == SOCK_DGRAM) {
		flow_type = FLOW_DIVERT_FLOW_TYPE_UDP;
	} else {
		error = EINVAL;
		goto done;
	}
	error = flow_divert_packet_append_tlv(connect_packet,
	    FLOW_DIVERT_TLV_FLOW_TYPE,
	    sizeof(flow_type),
	    &flow_type);

	if (error) {
		goto done;
	}

	if (fd_cb->connect_token != NULL) {
		unsigned int token_len = m_length(fd_cb->connect_token);
		mbuf_concatenate(connect_packet, fd_cb->connect_token);
		mbuf_pkthdr_adjustlen(connect_packet, token_len);
		fd_cb->connect_token = NULL;
	} else {
		error = flow_divert_append_target_endpoint_tlv(connect_packet, to);
		if (error) {
			goto done;
		}
	}

	if (fd_cb->local_endpoint.sa.sa_family == AF_INET || fd_cb->local_endpoint.sa.sa_family == AF_INET6) {
		error = flow_divert_packet_append_tlv(connect_packet, FLOW_DIVERT_TLV_LOCAL_ADDR, fd_cb->local_endpoint.sa.sa_len, &(fd_cb->local_endpoint.sa));
		if (error) {
			goto done;
		}
	}

	if (inp->inp_vflag & INP_IPV4) {
		ifp = inp->inp_last_outifp;
	} else if (inp->inp_vflag & INP_IPV6) {
		ifp = inp->in6p_last_outifp;
	}
	if (ifp != NULL) {
		uint32_t flow_if_index = ifp->if_index;
		error = flow_divert_packet_append_tlv(connect_packet, FLOW_DIVERT_TLV_OUT_IF_INDEX,
		    sizeof(flow_if_index), &flow_if_index);
		if (error) {
			goto done;
		}
	}

	if (so->so_flags1 & SOF1_DATA_IDEMPOTENT) {
		flags |= FLOW_DIVERT_TOKEN_FLAG_TFO;
	}

	if ((inp->inp_flags & INP_BOUND_IF) ||
	    ((inp->inp_vflag & INP_IPV6) && !IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_laddr)) ||
	    ((inp->inp_vflag & INP_IPV4) && inp->inp_laddr.s_addr != INADDR_ANY)) {
		flags |= FLOW_DIVERT_TOKEN_FLAG_BOUND;
	}

	if (flags != 0) {
		error = flow_divert_packet_append_tlv(connect_packet, FLOW_DIVERT_TLV_FLAGS, sizeof(flags), &flags);
		if (error) {
			goto done;
		}
	}

	if (SOCK_TYPE(so) == SOCK_DGRAM) {
		cfil_sock_id = cfil_sock_id_from_datagram_socket(so, NULL, to);
	} else {
		cfil_sock_id = cfil_sock_id_from_socket(so);
	}

	if (cfil_sock_id != CFIL_SOCK_ID_NONE) {
		cfil_id = &cfil_sock_id;
		cfil_id_size = sizeof(cfil_sock_id);
	} else if (so->so_flags1 & SOF1_CONTENT_FILTER_SKIP) {
		cfil_id = &inp->necp_client_uuid;
		cfil_id_size = sizeof(inp->necp_client_uuid);
	}

	if (cfil_id != NULL && cfil_id_size > 0 && cfil_id_size <= sizeof(uuid_t)) {
		error = flow_divert_packet_append_tlv(connect_packet, FLOW_DIVERT_TLV_CFIL_ID, (uint32_t)cfil_id_size, cfil_id);
		if (error) {
			goto done;
		}
	}

done:
	if (!error) {
		*out_connect_packet = connect_packet;
	} else if (connect_packet != NULL) {
		mbuf_freem(connect_packet);
	}

	return error;
}

static int
flow_divert_send_connect_packet(struct flow_divert_pcb *fd_cb)
{
	int error = 0;
	mbuf_t connect_packet = fd_cb->connect_packet;
	mbuf_t saved_connect_packet = NULL;

	if (connect_packet != NULL) {
		error = mbuf_copym(connect_packet, 0, mbuf_pkthdr_len(connect_packet), MBUF_DONTWAIT, &saved_connect_packet);
		if (error) {
			FDLOG0(LOG_ERR, fd_cb, "Failed to copy the connect packet");
			goto done;
		}

		error = flow_divert_send_packet(fd_cb, connect_packet, TRUE);
		if (error) {
			goto done;
		}

		fd_cb->connect_packet = saved_connect_packet;
		saved_connect_packet = NULL;
	} else {
		error = ENOENT;
	}
done:
	if (saved_connect_packet != NULL) {
		mbuf_freem(saved_connect_packet);
	}

	return error;
}

static int
flow_divert_send_connect_result(struct flow_divert_pcb *fd_cb)
{
	int             error                   = 0;
	mbuf_t  packet                  = NULL;
	int             rbuff_space             = 0;

	error = flow_divert_packet_init(fd_cb, FLOW_DIVERT_PKT_CONNECT_RESULT, &packet);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to create a connect result packet: %d", error);
		goto done;
	}

	rbuff_space = fd_cb->so->so_rcv.sb_hiwat;
	if (rbuff_space < 0) {
		rbuff_space = 0;
	}
	rbuff_space = htonl(rbuff_space);
	error = flow_divert_packet_append_tlv(packet,
	    FLOW_DIVERT_TLV_SPACE_AVAILABLE,
	    sizeof(rbuff_space),
	    &rbuff_space);
	if (error) {
		goto done;
	}

	if (fd_cb->local_endpoint.sa.sa_family == AF_INET || fd_cb->local_endpoint.sa.sa_family == AF_INET6) {
		error = flow_divert_packet_append_tlv(packet, FLOW_DIVERT_TLV_LOCAL_ADDR, fd_cb->local_endpoint.sa.sa_len, &(fd_cb->local_endpoint.sa));
		if (error) {
			goto done;
		}
	}

	error = flow_divert_send_packet(fd_cb, packet, TRUE);
	if (error) {
		goto done;
	}

done:
	if (error && packet != NULL) {
		mbuf_freem(packet);
	}

	return error;
}

static int
flow_divert_send_close(struct flow_divert_pcb *fd_cb, int how)
{
	int             error   = 0;
	mbuf_t  packet  = NULL;
	uint32_t        zero    = 0;

	error = flow_divert_packet_init(fd_cb, FLOW_DIVERT_PKT_CLOSE, &packet);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to create a close packet: %d", error);
		goto done;
	}

	error = flow_divert_packet_append_tlv(packet, FLOW_DIVERT_TLV_ERROR_CODE, sizeof(zero), &zero);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to add the error code TLV: %d", error);
		goto done;
	}

	how = htonl(how);
	error = flow_divert_packet_append_tlv(packet, FLOW_DIVERT_TLV_HOW, sizeof(how), &how);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to add the how flag: %d", error);
		goto done;
	}

	error = flow_divert_send_packet(fd_cb, packet, TRUE);
	if (error) {
		goto done;
	}

done:
	if (error && packet != NULL) {
		mbuf_free(packet);
	}

	return error;
}

static int
flow_divert_tunnel_how_closed(struct flow_divert_pcb *fd_cb)
{
	if ((fd_cb->flags & (FLOW_DIVERT_TUNNEL_RD_CLOSED | FLOW_DIVERT_TUNNEL_WR_CLOSED)) ==
	    (FLOW_DIVERT_TUNNEL_RD_CLOSED | FLOW_DIVERT_TUNNEL_WR_CLOSED)) {
		return SHUT_RDWR;
	} else if (fd_cb->flags & FLOW_DIVERT_TUNNEL_RD_CLOSED) {
		return SHUT_RD;
	} else if (fd_cb->flags & FLOW_DIVERT_TUNNEL_WR_CLOSED) {
		return SHUT_WR;
	}

	return -1;
}

/*
 * Determine what close messages if any need to be sent to the tunnel. Returns TRUE if the tunnel is closed for both reads and
 * writes. Returns FALSE otherwise.
 */
static void
flow_divert_send_close_if_needed(struct flow_divert_pcb *fd_cb)
{
	int             how             = -1;

	/* Do not send any close messages if there is still data in the send buffer */
	if (fd_cb->so->so_snd.sb_cc == 0) {
		if ((fd_cb->flags & (FLOW_DIVERT_READ_CLOSED | FLOW_DIVERT_TUNNEL_RD_CLOSED)) == FLOW_DIVERT_READ_CLOSED) {
			/* Socket closed reads, but tunnel did not. Tell tunnel to close reads */
			how = SHUT_RD;
		}
		if ((fd_cb->flags & (FLOW_DIVERT_WRITE_CLOSED | FLOW_DIVERT_TUNNEL_WR_CLOSED)) == FLOW_DIVERT_WRITE_CLOSED) {
			/* Socket closed writes, but tunnel did not. Tell tunnel to close writes */
			if (how == SHUT_RD) {
				how = SHUT_RDWR;
			} else {
				how = SHUT_WR;
			}
		}
	}

	if (how != -1) {
		FDLOG(LOG_INFO, fd_cb, "sending close, how = %d", how);
		if (flow_divert_send_close(fd_cb, how) != ENOBUFS) {
			/* Successfully sent the close packet. Record the ways in which the tunnel has been closed */
			if (how != SHUT_RD) {
				fd_cb->flags |= FLOW_DIVERT_TUNNEL_WR_CLOSED;
			}
			if (how != SHUT_WR) {
				fd_cb->flags |= FLOW_DIVERT_TUNNEL_RD_CLOSED;
			}
		}
	}

	if (flow_divert_tunnel_how_closed(fd_cb) == SHUT_RDWR) {
		flow_divert_disconnect_socket(fd_cb->so);
	}
}

static errno_t
flow_divert_send_data_packet(struct flow_divert_pcb *fd_cb, mbuf_t data, size_t data_len, struct sockaddr *toaddr, Boolean force)
{
	mbuf_t  packet = NULL;
	mbuf_t  last = NULL;
	int             error   = 0;

	error = flow_divert_packet_init(fd_cb, FLOW_DIVERT_PKT_DATA, &packet);
	if (error || packet == NULL) {
		FDLOG(LOG_ERR, fd_cb, "flow_divert_packet_init failed: %d", error);
		goto done;
	}

	if (toaddr != NULL) {
		error = flow_divert_append_target_endpoint_tlv(packet, toaddr);
		if (error) {
			FDLOG(LOG_ERR, fd_cb, "flow_divert_append_target_endpoint_tlv() failed: %d", error);
			goto done;
		}
	}

	if (data_len > 0 && data_len <= INT_MAX && data != NULL) {
		last = m_last(packet);
		mbuf_setnext(last, data);
		mbuf_pkthdr_adjustlen(packet, (int)data_len);
	} else {
		data_len = 0;
	}
	error = flow_divert_send_packet(fd_cb, packet, force);
	if (error == 0 && data_len > 0) {
		fd_cb->bytes_sent += data_len;
		flow_divert_add_data_statistics(fd_cb, data_len, TRUE);
	}

done:
	if (error) {
		if (last != NULL) {
			mbuf_setnext(last, NULL);
		}
		if (packet != NULL) {
			mbuf_freem(packet);
		}
	}

	return error;
}

static void
flow_divert_send_buffered_data(struct flow_divert_pcb *fd_cb, Boolean force)
{
	size_t  to_send;
	size_t  sent    = 0;
	int             error   = 0;
	mbuf_t  buffer;

	to_send = fd_cb->so->so_snd.sb_cc;
	buffer = fd_cb->so->so_snd.sb_mb;

	if (buffer == NULL && to_send > 0) {
		FDLOG(LOG_ERR, fd_cb, "Send buffer is NULL, but size is supposed to be %lu", to_send);
		return;
	}

	/* Ignore the send window if force is enabled */
	if (!force && (to_send > fd_cb->send_window)) {
		to_send = fd_cb->send_window;
	}

	if (SOCK_TYPE(fd_cb->so) == SOCK_STREAM) {
		while (sent < to_send) {
			mbuf_t  data;
			size_t  data_len;

			data_len = to_send - sent;
			if (data_len > FLOW_DIVERT_CHUNK_SIZE) {
				data_len = FLOW_DIVERT_CHUNK_SIZE;
			}

			error = mbuf_copym(buffer, sent, data_len, MBUF_DONTWAIT, &data);
			if (error) {
				FDLOG(LOG_ERR, fd_cb, "mbuf_copym failed: %d", error);
				break;
			}

			error = flow_divert_send_data_packet(fd_cb, data, data_len, NULL, force);
			if (error) {
				if (data != NULL) {
					mbuf_freem(data);
				}
				break;
			}

			sent += data_len;
		}
		sbdrop(&fd_cb->so->so_snd, (int)sent);
		sowwakeup(fd_cb->so);
	} else if (SOCK_TYPE(fd_cb->so) == SOCK_DGRAM) {
		mbuf_t data;
		mbuf_t m;
		size_t data_len;

		while (buffer) {
			struct sockaddr *toaddr = flow_divert_get_buffered_target_address(buffer);

			m = buffer;
			if (toaddr != NULL) {
				/* look for data in the chain */
				do {
					m = m->m_next;
					if (m != NULL && m->m_type == MT_DATA) {
						break;
					}
				} while (m);
				if (m == NULL) {
					/* unexpected */
					FDLOG0(LOG_ERR, fd_cb, "failed to find type MT_DATA in the mbuf chain.");
					goto move_on;
				}
			}
			data_len = mbuf_pkthdr_len(m);
			if (data_len > 0) {
				FDLOG(LOG_DEBUG, fd_cb, "mbuf_copym() data_len = %lu", data_len);
				error = mbuf_copym(m, 0, data_len, MBUF_DONTWAIT, &data);
				if (error) {
					FDLOG(LOG_ERR, fd_cb, "mbuf_copym failed: %d", error);
					break;
				}
			} else {
				data = NULL;
			}
			error = flow_divert_send_data_packet(fd_cb, data, data_len, toaddr, force);
			if (error) {
				if (data != NULL) {
					mbuf_freem(data);
				}
				break;
			}
			sent += data_len;
move_on:
			buffer = buffer->m_nextpkt;
			(void) sbdroprecord(&(fd_cb->so->so_snd));
		}
	}

	if (sent > 0) {
		FDLOG(LOG_DEBUG, fd_cb, "sent %lu bytes of buffered data", sent);
		if (fd_cb->send_window >= sent) {
			fd_cb->send_window -= sent;
		} else {
			fd_cb->send_window = 0;
		}
	}
}

static int
flow_divert_send_app_data(struct flow_divert_pcb *fd_cb, mbuf_t data, struct sockaddr *toaddr)
{
	size_t  to_send         = mbuf_pkthdr_len(data);
	int     error           = 0;

	if (to_send > fd_cb->send_window) {
		to_send = fd_cb->send_window;
	}

	if (fd_cb->so->so_snd.sb_cc > 0) {
		to_send = 0;    /* If the send buffer is non-empty, then we can't send anything */
	}

	if (SOCK_TYPE(fd_cb->so) == SOCK_STREAM) {
		size_t  sent            = 0;
		mbuf_t  remaining_data  = data;
		mbuf_t  pkt_data        = NULL;
		while (sent < to_send && remaining_data != NULL) {
			size_t  pkt_data_len;

			pkt_data = remaining_data;

			if ((to_send - sent) > FLOW_DIVERT_CHUNK_SIZE) {
				pkt_data_len = FLOW_DIVERT_CHUNK_SIZE;
			} else {
				pkt_data_len = to_send - sent;
			}

			if (pkt_data_len < mbuf_pkthdr_len(pkt_data)) {
				error = mbuf_split(pkt_data, pkt_data_len, MBUF_DONTWAIT, &remaining_data);
				if (error) {
					FDLOG(LOG_ERR, fd_cb, "mbuf_split failed: %d", error);
					pkt_data = NULL;
					break;
				}
			} else {
				remaining_data = NULL;
			}

			error = flow_divert_send_data_packet(fd_cb, pkt_data, pkt_data_len, NULL, FALSE);

			if (error) {
				break;
			}

			pkt_data = NULL;
			sent += pkt_data_len;
		}

		fd_cb->send_window -= sent;

		error = 0;

		if (pkt_data != NULL) {
			if (sbspace(&fd_cb->so->so_snd) > 0) {
				if (!sbappendstream(&fd_cb->so->so_snd, pkt_data)) {
					FDLOG(LOG_ERR, fd_cb, "sbappendstream failed with pkt_data, send buffer size = %u, send_window = %u\n",
					    fd_cb->so->so_snd.sb_cc, fd_cb->send_window);
				}
			} else {
				mbuf_freem(pkt_data);
				error = ENOBUFS;
			}
		}

		if (remaining_data != NULL) {
			if (sbspace(&fd_cb->so->so_snd) > 0) {
				if (!sbappendstream(&fd_cb->so->so_snd, remaining_data)) {
					FDLOG(LOG_ERR, fd_cb, "sbappendstream failed with remaining_data, send buffer size = %u, send_window = %u\n",
					    fd_cb->so->so_snd.sb_cc, fd_cb->send_window);
				}
			} else {
				mbuf_freem(remaining_data);
				error = ENOBUFS;
			}
		}
	} else if (SOCK_TYPE(fd_cb->so) == SOCK_DGRAM) {
		if (to_send || mbuf_pkthdr_len(data) == 0) {
			error = flow_divert_send_data_packet(fd_cb, data, to_send, toaddr, FALSE);
			if (error) {
				FDLOG(LOG_ERR, fd_cb, "flow_divert_send_data_packet failed. send data size = %lu", to_send);
				if (data != NULL) {
					mbuf_freem(data);
				}
			} else {
				fd_cb->send_window -= to_send;
			}
		} else {
			/* buffer it */
			if (sbspace(&fd_cb->so->so_snd) >= (int)mbuf_pkthdr_len(data)) {
				if (toaddr != NULL) {
					if (!sbappendaddr(&fd_cb->so->so_snd, toaddr, data, NULL, &error)) {
						FDLOG(LOG_ERR, fd_cb,
						    "sbappendaddr failed. send buffer size = %u, send_window = %u, error = %d\n",
						    fd_cb->so->so_snd.sb_cc, fd_cb->send_window, error);
					}
					error = 0;
				} else {
					if (!sbappendrecord(&fd_cb->so->so_snd, data)) {
						FDLOG(LOG_ERR, fd_cb,
						    "sbappendrecord failed. send buffer size = %u, send_window = %u, error = %d\n",
						    fd_cb->so->so_snd.sb_cc, fd_cb->send_window, error);
					}
				}
			} else {
				if (data != NULL) {
					mbuf_freem(data);
				}
				error = ENOBUFS;
			}
		}
	}

	return error;
}

static int
flow_divert_send_read_notification(struct flow_divert_pcb *fd_cb)
{
	int error = 0;
	mbuf_t packet = NULL;

	error = flow_divert_packet_init(fd_cb, FLOW_DIVERT_PKT_READ_NOTIFY, &packet);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to create a read notification packet: %d", error);
		goto done;
	}

	error = flow_divert_send_packet(fd_cb, packet, TRUE);
	if (error) {
		goto done;
	}

done:
	if (error && packet != NULL) {
		mbuf_free(packet);
	}

	return error;
}

static int
flow_divert_send_traffic_class_update(struct flow_divert_pcb *fd_cb, int traffic_class)
{
	int             error           = 0;
	mbuf_t  packet          = NULL;

	error = flow_divert_packet_init(fd_cb, FLOW_DIVERT_PKT_PROPERTIES_UPDATE, &packet);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to create a properties update packet: %d", error);
		goto done;
	}

	error = flow_divert_packet_append_tlv(packet, FLOW_DIVERT_TLV_TRAFFIC_CLASS, sizeof(traffic_class), &traffic_class);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to add the traffic class: %d", error);
		goto done;
	}

	error = flow_divert_send_packet(fd_cb, packet, TRUE);
	if (error) {
		goto done;
	}

done:
	if (error && packet != NULL) {
		mbuf_free(packet);
	}

	return error;
}

static void
flow_divert_set_local_endpoint(struct flow_divert_pcb *fd_cb, struct sockaddr *local_endpoint)
{
	struct inpcb *inp = sotoinpcb(fd_cb->so);

	if (local_endpoint->sa_family == AF_INET6) {
		if (IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_laddr) && (fd_cb->flags & FLOW_DIVERT_SHOULD_SET_LOCAL_ADDR)) {
			fd_cb->flags |= FLOW_DIVERT_DID_SET_LOCAL_ADDR;
			inp->in6p_laddr = (satosin6(local_endpoint))->sin6_addr;
		}
		if (inp->inp_lport == 0) {
			inp->inp_lport = (satosin6(local_endpoint))->sin6_port;
		}
	} else if (local_endpoint->sa_family == AF_INET) {
		if (inp->inp_laddr.s_addr == INADDR_ANY && (fd_cb->flags & FLOW_DIVERT_SHOULD_SET_LOCAL_ADDR)) {
			fd_cb->flags |= FLOW_DIVERT_DID_SET_LOCAL_ADDR;
			inp->inp_laddr = (satosin(local_endpoint))->sin_addr;
		}
		if (inp->inp_lport == 0) {
			inp->inp_lport = (satosin(local_endpoint))->sin_port;
		}
	}
}

static void
flow_divert_set_remote_endpoint(struct flow_divert_pcb *fd_cb, struct sockaddr *remote_endpoint)
{
	struct inpcb *inp = sotoinpcb(fd_cb->so);

	if (remote_endpoint->sa_family == AF_INET6) {
		if (IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_faddr)) {
			inp->in6p_faddr = (satosin6(remote_endpoint))->sin6_addr;
		}
		if (inp->inp_fport == 0) {
			inp->inp_fport = (satosin6(remote_endpoint))->sin6_port;
		}
	} else if (remote_endpoint->sa_family == AF_INET) {
		if (inp->inp_laddr.s_addr == INADDR_ANY) {
			inp->inp_faddr = (satosin(remote_endpoint))->sin_addr;
		}
		if (inp->inp_fport == 0) {
			inp->inp_fport = (satosin(remote_endpoint))->sin_port;
		}
	}
}

static uint32_t
flow_divert_derive_kernel_control_unit(uint32_t ctl_unit, uint32_t *aggregate_unit, bool *is_aggregate)
{
	*is_aggregate = false;
	if (aggregate_unit != NULL && *aggregate_unit != 0) {
		uint32_t counter;
		for (counter = 0; counter < (GROUP_COUNT_MAX - 1); counter++) {
			if ((*aggregate_unit) & (1 << counter)) {
				break;
			}
		}
		if (counter < (GROUP_COUNT_MAX - 1)) {
			*aggregate_unit &= ~(1 << counter);
			*is_aggregate = true;
			return counter + 1;
		} else {
			return ctl_unit;
		}
	} else {
		return ctl_unit;
	}
}

static int
flow_divert_try_next(struct flow_divert_pcb *fd_cb)
{
	uint32_t current_ctl_unit = 0;
	uint32_t next_ctl_unit = 0;
	struct flow_divert_group *current_group = NULL;
	struct flow_divert_group *next_group = NULL;
	int error = 0;
	bool is_aggregate = false;

	next_ctl_unit = flow_divert_derive_kernel_control_unit(fd_cb->policy_control_unit, &(fd_cb->aggregate_unit), &is_aggregate);
	current_ctl_unit = fd_cb->control_group_unit;

	if (current_ctl_unit == next_ctl_unit) {
		FDLOG0(LOG_NOTICE, fd_cb, "Next control unit is the same as the current control unit, disabling flow divert");
		error = EALREADY;
		goto done;
	}

	if (next_ctl_unit == 0 || next_ctl_unit >= GROUP_COUNT_MAX) {
		FDLOG0(LOG_NOTICE, fd_cb, "No more valid control units, disabling flow divert");
		error = ENOENT;
		goto done;
	}

	if (g_flow_divert_groups == NULL || g_active_group_count == 0) {
		FDLOG0(LOG_NOTICE, fd_cb, "No active groups, disabling flow divert");
		error = ENOENT;
		goto done;
	}

	next_group = g_flow_divert_groups[next_ctl_unit];
	if (next_group == NULL) {
		FDLOG(LOG_NOTICE, fd_cb, "Group for control unit %u does not exist", next_ctl_unit);
		error = ENOENT;
		goto done;
	}

	current_group = fd_cb->group;

	lck_rw_lock_exclusive(&(current_group->lck));
	lck_rw_lock_exclusive(&(next_group->lck));

	FDLOG(LOG_NOTICE, fd_cb, "Moving from %u to %u", current_ctl_unit, next_ctl_unit);

	RB_REMOVE(fd_pcb_tree, &(current_group->pcb_tree), fd_cb);
	if (RB_INSERT(fd_pcb_tree, &(next_group->pcb_tree), fd_cb) != NULL) {
		panic("group with unit %u already contains a connection with hash %u", next_ctl_unit, fd_cb->hash);
	}

	fd_cb->group = next_group;
	fd_cb->control_group_unit = next_ctl_unit;
	if (is_aggregate) {
		fd_cb->flags |= FLOW_DIVERT_FLOW_IS_TRANSPARENT;
	} else {
		fd_cb->flags &= ~FLOW_DIVERT_FLOW_IS_TRANSPARENT;
	}

	lck_rw_done(&(next_group->lck));
	lck_rw_done(&(current_group->lck));

	error = flow_divert_send_connect_packet(fd_cb);
	if (error) {
		FDLOG(LOG_NOTICE, fd_cb, "Failed to send the connect packet to %u, disabling flow divert", next_ctl_unit);
		error = ENOENT;
		goto done;
	}

done:
	return error;
}

static void
flow_divert_disable(struct flow_divert_pcb *fd_cb)
{
	struct socket *so = NULL;
	mbuf_t  buffer;
	int error = 0;
	proc_t last_proc = NULL;
	struct sockaddr *remote_endpoint = fd_cb->original_remote_endpoint;
	bool do_connect = !(fd_cb->flags & FLOW_DIVERT_IMPLICIT_CONNECT);
	struct inpcb *inp = NULL;

	so = fd_cb->so;
	if (so == NULL) {
		goto done;
	}

	FDLOG0(LOG_NOTICE, fd_cb, "Skipped all flow divert services, disabling flow divert");

	/* Restore the IP state */
	inp = sotoinpcb(so);
	inp->inp_vflag = fd_cb->original_vflag;
	inp->inp_faddr.s_addr = INADDR_ANY;
	inp->inp_fport = 0;
	memset(&(inp->in6p_faddr), 0, sizeof(inp->in6p_faddr));
	inp->in6p_fport = 0;
	/* If flow divert set the local address, clear it out */
	if (fd_cb->flags & FLOW_DIVERT_DID_SET_LOCAL_ADDR) {
		inp->inp_laddr.s_addr = INADDR_ANY;
		memset(&(inp->in6p_laddr), 0, sizeof(inp->in6p_laddr));
	}
	inp->inp_last_outifp = fd_cb->original_last_outifp;
	inp->in6p_last_outifp = fd_cb->original_last_outifp6;

	/* Dis-associate the socket */
	so->so_flags &= ~SOF_FLOW_DIVERT;
	so->so_flags1 |= SOF1_FLOW_DIVERT_SKIP;
	so->so_fd_pcb = NULL;
	fd_cb->so = NULL;

	/* Remove from the group */
	flow_divert_pcb_remove(fd_cb);

	FDRELEASE(fd_cb); /* Release the socket's reference */

	/* Revert back to the original protocol */
	so->so_proto = pffindproto(SOCK_DOM(so), SOCK_PROTO(so), SOCK_TYPE(so));

	last_proc = proc_find(so->last_pid);

	if (do_connect) {
		/* Connect using the original protocol */
		error = (*so->so_proto->pr_usrreqs->pru_connect)(so, remote_endpoint, (last_proc != NULL ? last_proc : current_proc()));
		if (error) {
			FDLOG(LOG_ERR, fd_cb, "Failed to connect using the socket's original protocol: %d", error);
			goto done;
		}
	}

	buffer = so->so_snd.sb_mb;
	if (buffer == NULL) {
		/* No buffered data, done */
		goto done;
	}

	/* Send any buffered data using the original protocol */
	if (SOCK_TYPE(so) == SOCK_STREAM) {
		mbuf_t data_to_send = NULL;
		size_t data_len = so->so_snd.sb_cc;

		error = mbuf_copym(buffer, 0, data_len, MBUF_DONTWAIT, &data_to_send);
		if (error) {
			FDLOG0(LOG_ERR, fd_cb, "Failed to copy the mbuf chain in the socket's send buffer");
			goto done;
		}

		sbflush(&so->so_snd);

		if (data_to_send->m_flags & M_PKTHDR) {
			mbuf_pkthdr_setlen(data_to_send, data_len);
		}

		error = (*so->so_proto->pr_usrreqs->pru_send)(so,
		    0,
		    data_to_send,
		    NULL,
		    NULL,
		    (last_proc != NULL ? last_proc : current_proc()));

		if (error && error != EWOULDBLOCK) {
			FDLOG(LOG_ERR, fd_cb, "Failed to send queued data using the socket's original protocol: %d", error);
		} else {
			error = 0;
		}
	} else if (SOCK_TYPE(so) == SOCK_DGRAM) {
		struct sockbuf *sb = &so->so_snd;
		MBUFQ_HEAD(send_queue_head) send_queue;
		MBUFQ_INIT(&send_queue);

		/* Flush the send buffer, moving all records to a temporary queue */
		while (sb->sb_mb != NULL) {
			mbuf_t record = sb->sb_mb;
			mbuf_t m = record;
			sb->sb_mb = sb->sb_mb->m_nextpkt;
			while (m != NULL) {
				sbfree(sb, m);
				m = m->m_next;
			}
			record->m_nextpkt = NULL;
			MBUFQ_ENQUEUE(&send_queue, record);
		}
		SB_EMPTY_FIXUP(sb);

		while (!MBUFQ_EMPTY(&send_queue)) {
			mbuf_t next_record = MBUFQ_FIRST(&send_queue);
			mbuf_t addr = NULL;
			mbuf_t control = NULL;
			mbuf_t last_control = NULL;
			mbuf_t data = NULL;
			mbuf_t m = next_record;
			struct sockaddr *to_endpoint = NULL;

			MBUFQ_DEQUEUE(&send_queue, next_record);

			while (m != NULL) {
				if (m->m_type == MT_SONAME) {
					addr = m;
				} else if (m->m_type == MT_CONTROL) {
					if (control == NULL) {
						control = m;
					}
					last_control = m;
				} else if (m->m_type == MT_DATA) {
					data = m;
					break;
				}
				m = m->m_next;
			}

			if (addr != NULL) {
				to_endpoint = flow_divert_get_buffered_target_address(addr);
				if (to_endpoint == NULL) {
					FDLOG0(LOG_NOTICE, fd_cb, "Failed to get the remote address from the buffer");
				}
			}

			if (data == NULL) {
				FDLOG0(LOG_ERR, fd_cb, "Buffered record does not contain any data");
				mbuf_freem(next_record);
				continue;
			}

			if (!(data->m_flags & M_PKTHDR)) {
				FDLOG0(LOG_ERR, fd_cb, "Buffered data does not have a packet header");
				mbuf_freem(next_record);
				continue;
			}

			if (addr != NULL) {
				addr->m_next = NULL;
			}

			if (last_control != NULL) {
				last_control->m_next = NULL;
			}

			error = (*so->so_proto->pr_usrreqs->pru_send)(so,
			    0,
			    data,
			    to_endpoint,
			    control,
			    (last_proc != NULL ? last_proc : current_proc()));

			if (addr != NULL) {
				mbuf_freem(addr);
			}

			if (error) {
				FDLOG(LOG_ERR, fd_cb, "Failed to send queued data using the socket's original protocol: %d", error);
			}
		}
	}
done:
	if (last_proc != NULL) {
		proc_rele(last_proc);
	}

	if (error) {
		so->so_error = (uint16_t)error;
		flow_divert_disconnect_socket(so);
	}
}

static void
flow_divert_scope(struct flow_divert_pcb *fd_cb, int out_if_index, bool derive_new_address)
{
	struct socket *so = NULL;
	struct inpcb *inp = NULL;
	struct ifnet *current_ifp = NULL;
	struct ifnet *new_ifp = NULL;
	int error = 0;

	so = fd_cb->so;
	if (so == NULL) {
		return;
	}

	inp = sotoinpcb(so);

	if (out_if_index <= 0) {
		return;
	}

	if (inp->inp_vflag & INP_IPV6) {
		current_ifp = inp->in6p_last_outifp;
	} else {
		current_ifp = inp->inp_last_outifp;
	}

	if (current_ifp != NULL) {
		if (current_ifp->if_index == out_if_index) {
			/* No change */
			return;
		}

		/* Scope the socket to the given interface */
		error = inp_bindif(inp, out_if_index, &new_ifp);
		if (error != 0) {
			FDLOG(LOG_ERR, fd_cb, "failed to scope to %d because inp_bindif returned %d", out_if_index, error);
			return;
		}

		if (derive_new_address && fd_cb->original_remote_endpoint != NULL) {
			/* Get the appropriate address for the given interface */
			if (inp->inp_vflag & INP_IPV6) {
				inp->in6p_laddr = sa6_any.sin6_addr;
				error = in6_pcbladdr(inp, fd_cb->original_remote_endpoint, &(fd_cb->local_endpoint.sin6.sin6_addr), NULL);
			} else {
				inp->inp_laddr.s_addr = INADDR_ANY;
				error = in_pcbladdr(inp, fd_cb->original_remote_endpoint, &(fd_cb->local_endpoint.sin.sin_addr), IFSCOPE_NONE, NULL, 0);
			}

			if (error != 0) {
				FDLOG(LOG_WARNING, fd_cb, "failed to derive a new local address from %d because in_pcbladdr returned %d", out_if_index, error);
			}
		}
	} else {
		ifnet_head_lock_shared();
		if (out_if_index <= if_index) {
			new_ifp = ifindex2ifnet[out_if_index];
		}
		ifnet_head_done();
	}

	/* Update the "last interface" of the socket */
	if (new_ifp != NULL) {
		if (inp->inp_vflag & INP_IPV6) {
			inp->in6p_last_outifp = new_ifp;
		} else {
			inp->inp_last_outifp = new_ifp;
		}

	}
}

static void
flow_divert_handle_connect_result(struct flow_divert_pcb *fd_cb, mbuf_t packet, int offset)
{
	uint32_t                                        connect_error = 0;
	uint32_t                                        ctl_unit                        = 0;
	int                                                     error                           = 0;
	struct flow_divert_group        *grp                            = NULL;
	union sockaddr_in_4_6 local_endpoint = {};
	union sockaddr_in_4_6 remote_endpoint = {};
	int                                                     out_if_index            = 0;
	uint32_t                                        send_window;
	uint32_t                                        app_data_length         = 0;

	memset(&local_endpoint, 0, sizeof(local_endpoint));
	memset(&remote_endpoint, 0, sizeof(remote_endpoint));

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_ERROR_CODE, sizeof(connect_error), &connect_error, NULL);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to get the connect result: %d", error);
		return;
	}

	connect_error = ntohl(connect_error);
	FDLOG(LOG_INFO, fd_cb, "received connect result %u", connect_error);

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_SPACE_AVAILABLE, sizeof(send_window), &send_window, NULL);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to get the send window: %d", error);
		return;
	}

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_CTL_UNIT, sizeof(ctl_unit), &ctl_unit, NULL);
	if (error) {
		FDLOG0(LOG_INFO, fd_cb, "No control unit provided in the connect result");
	}

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_LOCAL_ADDR, sizeof(local_endpoint), &(local_endpoint.sa), NULL);
	if (error) {
		FDLOG0(LOG_INFO, fd_cb, "No local address provided");
	}

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_REMOTE_ADDR, sizeof(remote_endpoint), &(remote_endpoint.sa), NULL);
	if (error) {
		FDLOG0(LOG_INFO, fd_cb, "No remote address provided");
	}

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_OUT_IF_INDEX, sizeof(out_if_index), &out_if_index, NULL);
	if (error) {
		FDLOG0(LOG_INFO, fd_cb, "No output if index provided");
	}

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_APP_DATA, 0, NULL, &app_data_length);
	if (error) {
		FDLOG0(LOG_INFO, fd_cb, "No application data provided in connect result");
	}

	error = 0;
	ctl_unit                = ntohl(ctl_unit);

	lck_rw_lock_shared(&g_flow_divert_group_lck);

	if (connect_error == 0 && ctl_unit > 0) {
		if (ctl_unit >= GROUP_COUNT_MAX) {
			FDLOG(LOG_ERR, fd_cb, "Connect result contains an invalid control unit: %u", ctl_unit);
			error = EINVAL;
		} else if (g_flow_divert_groups == NULL || g_active_group_count == 0) {
			FDLOG0(LOG_ERR, fd_cb, "No active groups, dropping connection");
			error = EINVAL;
		} else {
			grp = g_flow_divert_groups[ctl_unit];
			if (grp == NULL) {
				error = ECONNRESET;
			}
		}
	}

	FDLOCK(fd_cb);
	if (fd_cb->so != NULL) {
		struct inpcb                            *inp = NULL;
		struct flow_divert_group        *old_group;
		struct socket *so = fd_cb->so;
		bool local_address_is_valid = false;

		socket_lock(so, 0);

		if (!(so->so_flags & SOF_FLOW_DIVERT)) {
			FDLOG0(LOG_NOTICE, fd_cb, "socket is not attached any more, ignoring connect result");
			goto done;
		}

		if (SOCK_TYPE(so) == SOCK_STREAM && !(so->so_state & SS_ISCONNECTING)) {
			FDLOG0(LOG_ERR, fd_cb, "TCP socket is not in the connecting state, ignoring connect result");
			goto done;
		}

		inp = sotoinpcb(so);

		if (connect_error || error) {
			goto set_socket_state;
		}

		if (flow_divert_is_sockaddr_valid(&(local_endpoint.sa))) {
			if (local_endpoint.sa.sa_family == AF_INET) {
				local_endpoint.sa.sa_len = sizeof(struct sockaddr_in);
				if ((inp->inp_vflag & INP_IPV4) && local_endpoint.sin.sin_addr.s_addr != INADDR_ANY) {
					local_address_is_valid = true;
					fd_cb->local_endpoint = local_endpoint;
					inp->inp_laddr.s_addr = INADDR_ANY;
				} else {
					fd_cb->local_endpoint.sin.sin_port = local_endpoint.sin.sin_port;
				}
			} else if (local_endpoint.sa.sa_family == AF_INET6) {
				local_endpoint.sa.sa_len = sizeof(struct sockaddr_in6);
				if ((inp->inp_vflag & INP_IPV6) && !IN6_IS_ADDR_UNSPECIFIED(&local_endpoint.sin6.sin6_addr)) {
					local_address_is_valid = true;
					fd_cb->local_endpoint = local_endpoint;
					inp->in6p_laddr = sa6_any.sin6_addr;
				} else {
					fd_cb->local_endpoint.sin6.sin6_port = local_endpoint.sin6.sin6_port;
				}
			}
		}

		flow_divert_scope(fd_cb, out_if_index, !local_address_is_valid);
		flow_divert_set_local_endpoint(fd_cb, &(fd_cb->local_endpoint.sa));

		if (flow_divert_is_sockaddr_valid(&(remote_endpoint.sa)) && SOCK_TYPE(so) == SOCK_STREAM) {
			if (remote_endpoint.sa.sa_family == AF_INET) {
				remote_endpoint.sa.sa_len = sizeof(struct sockaddr_in);
			} else if (remote_endpoint.sa.sa_family == AF_INET6) {
				remote_endpoint.sa.sa_len = sizeof(struct sockaddr_in6);
			}
			flow_divert_set_remote_endpoint(fd_cb, &(remote_endpoint.sa));
		}

		if (app_data_length > 0) {
			uint8_t *app_data = NULL;
			MALLOC(app_data, uint8_t *, app_data_length, M_TEMP, M_WAITOK);
			if (app_data != NULL) {
				error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_APP_DATA, app_data_length, app_data, NULL);
				if (error == 0) {
					FDLOG(LOG_INFO, fd_cb, "Got %u bytes of app data from the connect result", app_data_length);
					if (fd_cb->app_data != NULL) {
						FREE(fd_cb->app_data, M_TEMP);
					}
					fd_cb->app_data = app_data;
					fd_cb->app_data_length = app_data_length;
				} else {
					FDLOG(LOG_ERR, fd_cb, "Failed to copy %u bytes of application data from the connect result packet", app_data_length);
					FREE(app_data, M_TEMP);
				}
			} else {
				FDLOG(LOG_ERR, fd_cb, "Failed to allocate a buffer of size %u to hold the application data from the connect result", app_data_length);
			}
		}

		if (error) {
			goto set_socket_state;
		}

		if (fd_cb->group == NULL) {
			error = EINVAL;
			goto set_socket_state;
		}

		if (grp != NULL) {
			old_group = fd_cb->group;

			lck_rw_lock_exclusive(&old_group->lck);
			lck_rw_lock_exclusive(&grp->lck);

			RB_REMOVE(fd_pcb_tree, &old_group->pcb_tree, fd_cb);
			if (RB_INSERT(fd_pcb_tree, &grp->pcb_tree, fd_cb) != NULL) {
				panic("group with unit %u already contains a connection with hash %u", grp->ctl_unit, fd_cb->hash);
			}

			fd_cb->group = grp;

			lck_rw_done(&grp->lck);
			lck_rw_done(&old_group->lck);
		}

		fd_cb->send_window = ntohl(send_window);

set_socket_state:
		if (!connect_error && !error) {
			FDLOG0(LOG_INFO, fd_cb, "sending connect result");
			error = flow_divert_send_connect_result(fd_cb);
		}

		if (connect_error || error) {
			if (connect_error && fd_cb->control_group_unit != fd_cb->policy_control_unit) {
				error = flow_divert_try_next(fd_cb);
				if (error) {
					flow_divert_disable(fd_cb);
				}
				goto done;
			}

			if (!connect_error) {
				flow_divert_update_closed_state(fd_cb, SHUT_RDWR, FALSE);
				so->so_error = (uint16_t)error;
				flow_divert_send_close_if_needed(fd_cb);
			} else {
				flow_divert_update_closed_state(fd_cb, SHUT_RDWR, TRUE);
				so->so_error = (uint16_t)connect_error;
			}
			flow_divert_disconnect_socket(so);
		} else {
#if NECP
			/* Update NECP client with connected five-tuple */
			if (!uuid_is_null(inp->necp_client_uuid)) {
				socket_unlock(so, 0);
				necp_client_assign_from_socket(so->last_pid, inp->necp_client_uuid, inp);
				socket_lock(so, 0);
			}
#endif /* NECP */

			flow_divert_send_buffered_data(fd_cb, FALSE);
			soisconnected(so);
		}

		/* We don't need the connect packet any more */
		if (fd_cb->connect_packet != NULL) {
			mbuf_freem(fd_cb->connect_packet);
			fd_cb->connect_packet = NULL;
		}

		/* We don't need the original remote endpoint any more */
		if (fd_cb->original_remote_endpoint != NULL) {
			FREE(fd_cb->original_remote_endpoint, M_SONAME);
			fd_cb->original_remote_endpoint = NULL;
		}
done:
		socket_unlock(so, 0);
	}
	FDUNLOCK(fd_cb);

	lck_rw_done(&g_flow_divert_group_lck);
}

static void
flow_divert_handle_close(struct flow_divert_pcb *fd_cb, mbuf_t packet, int offset)
{
	uint32_t        close_error;
	int                     error                   = 0;
	int                     how;

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_ERROR_CODE, sizeof(close_error), &close_error, NULL);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to get the close error: %d", error);
		return;
	}

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_HOW, sizeof(how), &how, NULL);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to get the close how flag: %d", error);
		return;
	}

	how = ntohl(how);

	FDLOG(LOG_INFO, fd_cb, "close received, how = %d", how);

	FDLOCK(fd_cb);
	if (fd_cb->so != NULL) {
		socket_lock(fd_cb->so, 0);

		if (!(fd_cb->so->so_flags & SOF_FLOW_DIVERT)) {
			FDLOG0(LOG_NOTICE, fd_cb, "socket is not attached any more, ignoring close from provider");
			goto done;
		}

		fd_cb->so->so_error = (uint16_t)ntohl(close_error);

		flow_divert_update_closed_state(fd_cb, how, TRUE);

		how = flow_divert_tunnel_how_closed(fd_cb);
		if (how == SHUT_RDWR) {
			flow_divert_disconnect_socket(fd_cb->so);
		} else if (how == SHUT_RD) {
			socantrcvmore(fd_cb->so);
		} else if (how == SHUT_WR) {
			socantsendmore(fd_cb->so);
		}
done:
		socket_unlock(fd_cb->so, 0);
	}
	FDUNLOCK(fd_cb);
}

static mbuf_t
flow_divert_create_control_mbuf(struct flow_divert_pcb *fd_cb)
{
	struct inpcb *inp = sotoinpcb(fd_cb->so);
	bool is_cfil_enabled = false;
#if CONTENT_FILTER
	/* Content Filter needs to see the local address */
	is_cfil_enabled = (inp->inp_socket && inp->inp_socket->so_cfil_db != NULL);
#endif
	if ((inp->inp_vflag & INP_IPV4) &&
	    fd_cb->local_endpoint.sa.sa_family == AF_INET &&
	    ((inp->inp_flags & INP_RECVDSTADDR) || is_cfil_enabled)) {
		return sbcreatecontrol((caddr_t)&(fd_cb->local_endpoint.sin.sin_addr), sizeof(struct in_addr), IP_RECVDSTADDR, IPPROTO_IP);
	} else if ((inp->inp_vflag & INP_IPV6) &&
	    fd_cb->local_endpoint.sa.sa_family == AF_INET6 &&
	    ((inp->inp_flags & IN6P_PKTINFO) || is_cfil_enabled)) {
		struct in6_pktinfo pi6;
		memset(&pi6, 0, sizeof(pi6));
		pi6.ipi6_addr = fd_cb->local_endpoint.sin6.sin6_addr;

		return sbcreatecontrol((caddr_t)&pi6, sizeof(pi6), IPV6_PKTINFO, IPPROTO_IPV6);
	}
	return NULL;
}

static int
flow_divert_handle_data(struct flow_divert_pcb *fd_cb, mbuf_t packet, size_t offset)
{
	int error = 0;

	FDLOCK(fd_cb);
	if (fd_cb->so != NULL) {
		mbuf_t  data            = NULL;
		size_t  data_size;
		struct sockaddr_storage remote_address;
		boolean_t got_remote_sa = FALSE;
		boolean_t appended = FALSE;
		boolean_t append_success = FALSE;

		socket_lock(fd_cb->so, 0);

		if (!(fd_cb->so->so_flags & SOF_FLOW_DIVERT)) {
			FDLOG0(LOG_NOTICE, fd_cb, "socket is not attached any more, ignoring inbound data");
			goto done;
		}

		if (sbspace(&fd_cb->so->so_rcv) == 0) {
			error = ENOBUFS;
			fd_cb->flags |= FLOW_DIVERT_NOTIFY_ON_RECEIVED;
			FDLOG0(LOG_INFO, fd_cb, "Receive buffer is full, will send read notification when app reads some data");
			goto done;
		}

		if (SOCK_TYPE(fd_cb->so) == SOCK_DGRAM) {
			uint32_t val_size = 0;

			/* check if we got remote address with data */
			memset(&remote_address, 0, sizeof(remote_address));
			error = flow_divert_packet_get_tlv(packet, (int)offset, FLOW_DIVERT_TLV_REMOTE_ADDR, sizeof(remote_address), &remote_address, &val_size);
			if (error || val_size > sizeof(remote_address)) {
				FDLOG0(LOG_INFO, fd_cb, "No remote address provided");
				error = 0;
			} else {
				if (remote_address.ss_len > sizeof(remote_address)) {
					remote_address.ss_len = sizeof(remote_address);
				}
				/* validate the address */
				if (flow_divert_is_sockaddr_valid((struct sockaddr *)&remote_address)) {
					got_remote_sa = TRUE;
				} else {
					FDLOG0(LOG_INFO, fd_cb, "Remote address is invalid");
				}
				offset += (sizeof(uint8_t) + sizeof(uint32_t) + val_size);
			}
		}

		data_size = (mbuf_pkthdr_len(packet) - offset);

		if (fd_cb->so->so_state & SS_CANTRCVMORE) {
			FDLOG(LOG_NOTICE, fd_cb, "app cannot receive any more data, dropping %lu bytes of data", data_size);
			goto done;
		}

		if (SOCK_TYPE(fd_cb->so) != SOCK_STREAM && SOCK_TYPE(fd_cb->so) != SOCK_DGRAM) {
			FDLOG(LOG_ERR, fd_cb, "socket has an unsupported type: %d", SOCK_TYPE(fd_cb->so));
			goto done;
		}

		FDLOG(LOG_DEBUG, fd_cb, "received %lu bytes of data", data_size);

		error = mbuf_split(packet, offset, MBUF_DONTWAIT, &data);
		if (error || data == NULL) {
			FDLOG(LOG_ERR, fd_cb, "mbuf_split failed: %d", error);
			goto done;
		}

		if (SOCK_TYPE(fd_cb->so) == SOCK_STREAM) {
			appended = (sbappendstream(&fd_cb->so->so_rcv, data) != 0);
			append_success = TRUE;
		} else {
			struct sockaddr *append_sa = NULL;
			mbuf_t mctl;

			if (got_remote_sa == TRUE) {
				error = flow_divert_dup_addr(remote_address.ss_family, (struct sockaddr *)&remote_address, &append_sa);
			} else {
				if (fd_cb->so->so_proto->pr_domain->dom_family == AF_INET6) {
					error = in6_mapped_peeraddr(fd_cb->so, &append_sa);
				} else {
					error = in_getpeeraddr(fd_cb->so, &append_sa);
				}
			}
			if (error) {
				FDLOG0(LOG_ERR, fd_cb, "failed to dup the socket address.");
			}

			mctl = flow_divert_create_control_mbuf(fd_cb);
			int append_error = 0;
			if (sbappendaddr(&fd_cb->so->so_rcv, append_sa, data, mctl, &append_error) || append_error == EJUSTRETURN) {
				append_success = TRUE;
				appended = (append_error == 0);
			} else {
				FDLOG(LOG_ERR, fd_cb, "failed to append %lu bytes of data: %d", data_size, append_error);
			}

			if (append_sa != NULL) {
				FREE(append_sa, M_SONAME);
			}
		}

		if (append_success) {
			fd_cb->bytes_received += data_size;
			flow_divert_add_data_statistics(fd_cb, data_size, FALSE);
		}

		if (appended) {
			sorwakeup(fd_cb->so);
		}
done:
		socket_unlock(fd_cb->so, 0);
	}
	FDUNLOCK(fd_cb);

	return error;
}

static void
flow_divert_handle_read_notification(struct flow_divert_pcb *fd_cb, mbuf_t packet, int offset)
{
	uint32_t        read_count;
	int             error                   = 0;

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_READ_COUNT, sizeof(read_count), &read_count, NULL);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to get the read count: %d", error);
		return;
	}

	FDLOG(LOG_DEBUG, fd_cb, "received a read notification for %u bytes", ntohl(read_count));

	FDLOCK(fd_cb);
	if (fd_cb->so != NULL) {
		socket_lock(fd_cb->so, 0);

		if (!(fd_cb->so->so_flags & SOF_FLOW_DIVERT)) {
			FDLOG0(LOG_NOTICE, fd_cb, "socket is not attached any more, ignoring read notification");
			goto done;
		}

		fd_cb->send_window += ntohl(read_count);
		flow_divert_send_buffered_data(fd_cb, FALSE);
done:
		socket_unlock(fd_cb->so, 0);
	}
	FDUNLOCK(fd_cb);
}

static void
flow_divert_handle_group_init(struct flow_divert_group *group, mbuf_t packet, int offset)
{
	int error = 0;
	uint32_t key_size = 0;
	int log_level;
	uint32_t flags = 0;

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_TOKEN_KEY, 0, NULL, &key_size);
	if (error) {
		FDLOG(LOG_ERR, &nil_pcb, "failed to get the key size: %d", error);
		return;
	}

	if (key_size == 0 || key_size > FLOW_DIVERT_MAX_KEY_SIZE) {
		FDLOG(LOG_ERR, &nil_pcb, "Invalid key size: %u", key_size);
		return;
	}

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_LOG_LEVEL, sizeof(log_level), &log_level, NULL);
	if (!error) {
		nil_pcb.log_level = (uint8_t)log_level;
	}

	lck_rw_lock_exclusive(&group->lck);

	if (group->token_key != NULL) {
		FREE(group->token_key, M_TEMP);
		group->token_key = NULL;
	}

	MALLOC(group->token_key, uint8_t *, key_size, M_TEMP, M_WAITOK);
	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_TOKEN_KEY, key_size, group->token_key, NULL);
	if (error) {
		FDLOG(LOG_ERR, &nil_pcb, "failed to get the token key: %d", error);
		FREE(group->token_key, M_TEMP);
		group->token_key = NULL;
		lck_rw_done(&group->lck);
		return;
	}

	group->token_key_size = key_size;

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_FLAGS, sizeof(flags), &flags, NULL);
	if (!error) {
		group->flags = flags;
	}

	lck_rw_done(&group->lck);
}

static void
flow_divert_handle_properties_update(struct flow_divert_pcb *fd_cb, mbuf_t packet, int offset)
{
	int                                                     error                           = 0;
	int                                                     out_if_index            = 0;
	uint32_t                                        app_data_length         = 0;

	FDLOG0(LOG_INFO, fd_cb, "received a properties update");

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_OUT_IF_INDEX, sizeof(out_if_index), &out_if_index, NULL);
	if (error) {
		FDLOG0(LOG_INFO, fd_cb, "No output if index provided in properties update");
	}

	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_APP_DATA, 0, NULL, &app_data_length);
	if (error) {
		FDLOG0(LOG_INFO, fd_cb, "No application data provided in properties update");
	}

	FDLOCK(fd_cb);
	if (fd_cb->so != NULL) {
		socket_lock(fd_cb->so, 0);

		if (!(fd_cb->so->so_flags & SOF_FLOW_DIVERT)) {
			FDLOG0(LOG_NOTICE, fd_cb, "socket is not attached any more, ignoring properties update");
			goto done;
		}

		if (out_if_index > 0) {
			flow_divert_scope(fd_cb, out_if_index, true);
			flow_divert_set_local_endpoint(fd_cb, &(fd_cb->local_endpoint.sa));
		}

		if (app_data_length > 0) {
			uint8_t *app_data = NULL;
			MALLOC(app_data, uint8_t *, app_data_length, M_TEMP, M_WAITOK);
			if (app_data != NULL) {
				error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_APP_DATA, app_data_length, app_data, NULL);
				if (error == 0) {
					if (fd_cb->app_data != NULL) {
						FREE(fd_cb->app_data, M_TEMP);
					}
					fd_cb->app_data = app_data;
					fd_cb->app_data_length = app_data_length;
				} else {
					FDLOG(LOG_ERR, fd_cb, "Failed to copy %u bytes of application data from the properties update packet", app_data_length);
					FREE(app_data, M_TEMP);
				}
			} else {
				FDLOG(LOG_ERR, fd_cb, "Failed to allocate a buffer of size %u to hold the application data from the properties update", app_data_length);
			}
		}
done:
		socket_unlock(fd_cb->so, 0);
	}
	FDUNLOCK(fd_cb);
}

static void
flow_divert_handle_app_map_create(struct flow_divert_group *group, mbuf_t packet, int offset)
{
	size_t bytes_mem_size;
	size_t child_maps_mem_size;
	size_t nodes_mem_size;
	size_t trie_memory_size = 0;
	int cursor;
	int error = 0;
	struct flow_divert_trie new_trie;
	int insert_error = 0;
	int prefix_count = -1;
	int signing_id_count = 0;
	size_t bytes_count = 0;
	size_t nodes_count = 0;
	size_t maps_count = 0;

	lck_rw_lock_exclusive(&group->lck);

	/* Re-set the current trie */
	if (group->signing_id_trie.memory != NULL) {
		FREE(group->signing_id_trie.memory, M_TEMP);
	}
	memset(&group->signing_id_trie, 0, sizeof(group->signing_id_trie));
	group->signing_id_trie.root = NULL_TRIE_IDX;

	memset(&new_trie, 0, sizeof(new_trie));

	/* Get the number of shared prefixes in the new set of signing ID strings */
	error = flow_divert_packet_get_tlv(packet, offset, FLOW_DIVERT_TLV_PREFIX_COUNT, sizeof(prefix_count), &prefix_count, NULL);

	if (prefix_count < 0 || error) {
		FDLOG(LOG_ERR, &nil_pcb, "Invalid prefix count (%d) or an error occurred while reading the prefix count: %d", prefix_count, error);
		lck_rw_done(&group->lck);
		return;
	}

	/* Compute the number of signing IDs and the total amount of bytes needed to store them */
	for (cursor = flow_divert_packet_find_tlv(packet, offset, FLOW_DIVERT_TLV_SIGNING_ID, &error, 0);
	    cursor >= 0;
	    cursor = flow_divert_packet_find_tlv(packet, cursor, FLOW_DIVERT_TLV_SIGNING_ID, &error, 1)) {
		uint32_t sid_size = 0;
		error = flow_divert_packet_get_tlv(packet, cursor, FLOW_DIVERT_TLV_SIGNING_ID, 0, NULL, &sid_size);
		if (error || sid_size == 0) {
			FDLOG(LOG_ERR, &nil_pcb, "Failed to get the length of the signing identifier at offset %d: %d", cursor, error);
			signing_id_count = 0;
			break;
		}
		if (os_add_overflow(bytes_count, sid_size, &bytes_count)) {
			FDLOG0(LOG_ERR, &nil_pcb, "Overflow while incrementing number of bytes");
			signing_id_count = 0;
			break;
		}
		signing_id_count++;
	}

	if (signing_id_count == 0) {
		lck_rw_done(&group->lck);
		FDLOG0(LOG_NOTICE, &nil_pcb, "No signing identifiers");
		return;
	}

	if (os_add3_overflow(prefix_count, signing_id_count, 1, &nodes_count)) { /* + 1 for the root node */
		lck_rw_done(&group->lck);
		FDLOG0(LOG_ERR, &nil_pcb, "Overflow while computing the number of nodes");
		return;
	}

	if (os_add_overflow(prefix_count, 1, &maps_count)) { /* + 1 for the root node */
		lck_rw_done(&group->lck);
		FDLOG0(LOG_ERR, &nil_pcb, "Overflow while computing the number of maps");
		return;
	}

	if (bytes_count > UINT16_MAX || nodes_count > UINT16_MAX || maps_count > UINT16_MAX) {
		lck_rw_done(&group->lck);
		FDLOG(LOG_NOTICE, &nil_pcb, "Invalid bytes count (%lu), nodes count (%lu) or maps count (%lu)", bytes_count, nodes_count, maps_count);
		return;
	}

	FDLOG(LOG_INFO, &nil_pcb, "Nodes count = %lu, child maps count = %lu, bytes_count = %lu",
	    nodes_count, maps_count, bytes_count);

	if (os_mul_overflow(sizeof(*new_trie.nodes), (size_t)nodes_count, &nodes_mem_size) ||
	    os_mul3_overflow(sizeof(*new_trie.child_maps), CHILD_MAP_SIZE, (size_t)maps_count, &child_maps_mem_size) ||
	    os_mul_overflow(sizeof(*new_trie.bytes), (size_t)bytes_count, &bytes_mem_size) ||
	    os_add3_overflow(nodes_mem_size, child_maps_mem_size, bytes_mem_size, &trie_memory_size)) {
		FDLOG0(LOG_ERR, &nil_pcb, "Overflow while computing trie memory sizes");
		lck_rw_done(&group->lck);
		return;
	}

	if (trie_memory_size > FLOW_DIVERT_MAX_TRIE_MEMORY) {
		FDLOG(LOG_ERR, &nil_pcb, "Trie memory size (%lu) is too big (maximum is %u)", trie_memory_size, FLOW_DIVERT_MAX_TRIE_MEMORY);
		lck_rw_done(&group->lck);
		return;
	}

	MALLOC(new_trie.memory, void *, trie_memory_size, M_TEMP, M_WAITOK);
	if (new_trie.memory == NULL) {
		FDLOG(LOG_ERR, &nil_pcb, "Failed to allocate %lu bytes of memory for the signing ID trie",
		    nodes_mem_size + child_maps_mem_size + bytes_mem_size);
		lck_rw_done(&group->lck);
		return;
	}

	new_trie.bytes_count = (uint16_t)bytes_count;
	new_trie.nodes_count = (uint16_t)nodes_count;
	new_trie.child_maps_count = (uint16_t)maps_count;

	/* Initialize the free lists */
	new_trie.nodes = (struct flow_divert_trie_node *)new_trie.memory;
	new_trie.nodes_free_next = 0;
	memset(new_trie.nodes, 0, nodes_mem_size);

	new_trie.child_maps = (uint16_t *)(void *)((uint8_t *)new_trie.memory + nodes_mem_size);
	new_trie.child_maps_free_next = 0;
	memset(new_trie.child_maps, 0xff, child_maps_mem_size);

	new_trie.bytes = (uint8_t *)(void *)((uint8_t *)new_trie.memory + nodes_mem_size + child_maps_mem_size);
	new_trie.bytes_free_next = 0;
	memset(new_trie.bytes, 0, bytes_mem_size);

	/* The root is an empty node */
	new_trie.root = trie_node_alloc(&new_trie);

	/* Add each signing ID to the trie */
	for (cursor = flow_divert_packet_find_tlv(packet, offset, FLOW_DIVERT_TLV_SIGNING_ID, &error, 0);
	    cursor >= 0;
	    cursor = flow_divert_packet_find_tlv(packet, cursor, FLOW_DIVERT_TLV_SIGNING_ID, &error, 1)) {
		uint32_t sid_size = 0;
		error = flow_divert_packet_get_tlv(packet, cursor, FLOW_DIVERT_TLV_SIGNING_ID, 0, NULL, &sid_size);
		if (error || sid_size == 0) {
			FDLOG(LOG_ERR, &nil_pcb, "Failed to get the length of the signing identifier at offset %d while building: %d", cursor, error);
			insert_error = EINVAL;
			break;
		}
		if (sid_size <= UINT16_MAX && new_trie.bytes_free_next + (uint16_t)sid_size <= new_trie.bytes_count) {
			uint16_t new_node_idx;
			error = flow_divert_packet_get_tlv(packet, cursor, FLOW_DIVERT_TLV_SIGNING_ID, sid_size, &TRIE_BYTE(&new_trie, new_trie.bytes_free_next), NULL);
			if (error) {
				FDLOG(LOG_ERR, &nil_pcb, "Failed to read the signing identifier at offset %d: %d", cursor, error);
				insert_error = EINVAL;
				break;
			}
			new_node_idx = flow_divert_trie_insert(&new_trie, new_trie.bytes_free_next, sid_size);
			if (new_node_idx == NULL_TRIE_IDX) {
				insert_error = EINVAL;
				break;
			}
		} else {
			FDLOG0(LOG_ERR, &nil_pcb, "No place to put signing ID for insertion");
			insert_error = ENOBUFS;
			break;
		}
	}

	if (!insert_error) {
		group->signing_id_trie = new_trie;
	} else {
		FREE(new_trie.memory, M_TEMP);
	}

	lck_rw_done(&group->lck);
}

static int
flow_divert_input(mbuf_t packet, struct flow_divert_group *group)
{
	struct flow_divert_packet_header        hdr;
	int                                                                     error           = 0;
	struct flow_divert_pcb                          *fd_cb;

	if (mbuf_pkthdr_len(packet) < sizeof(hdr)) {
		FDLOG(LOG_ERR, &nil_pcb, "got a bad packet, length (%lu) < sizeof hdr (%lu)", mbuf_pkthdr_len(packet), sizeof(hdr));
		error = EINVAL;
		goto done;
	}

	if (mbuf_pkthdr_len(packet) > FD_CTL_RCVBUFF_SIZE) {
		FDLOG(LOG_ERR, &nil_pcb, "got a bad packet, length (%lu) > %d", mbuf_pkthdr_len(packet), FD_CTL_RCVBUFF_SIZE);
		error = EINVAL;
		goto done;
	}

	error = mbuf_copydata(packet, 0, sizeof(hdr), &hdr);
	if (error) {
		FDLOG(LOG_ERR, &nil_pcb, "mbuf_copydata failed for the header: %d", error);
		error = ENOBUFS;
		goto done;
	}

	hdr.conn_id = ntohl(hdr.conn_id);

	if (hdr.conn_id == 0) {
		switch (hdr.packet_type) {
		case FLOW_DIVERT_PKT_GROUP_INIT:
			flow_divert_handle_group_init(group, packet, sizeof(hdr));
			break;
		case FLOW_DIVERT_PKT_APP_MAP_CREATE:
			flow_divert_handle_app_map_create(group, packet, sizeof(hdr));
			break;
		default:
			FDLOG(LOG_WARNING, &nil_pcb, "got an unknown message type: %d", hdr.packet_type);
			break;
		}
		goto done;
	}

	fd_cb = flow_divert_pcb_lookup(hdr.conn_id, group);             /* This retains the PCB */
	if (fd_cb == NULL) {
		if (hdr.packet_type != FLOW_DIVERT_PKT_CLOSE && hdr.packet_type != FLOW_DIVERT_PKT_READ_NOTIFY) {
			FDLOG(LOG_NOTICE, &nil_pcb, "got a %s message from group %d for an unknown pcb: %u", flow_divert_packet_type2str(hdr.packet_type), group->ctl_unit, hdr.conn_id);
		}
		goto done;
	}

	switch (hdr.packet_type) {
	case FLOW_DIVERT_PKT_CONNECT_RESULT:
		flow_divert_handle_connect_result(fd_cb, packet, sizeof(hdr));
		break;
	case FLOW_DIVERT_PKT_CLOSE:
		flow_divert_handle_close(fd_cb, packet, sizeof(hdr));
		break;
	case FLOW_DIVERT_PKT_DATA:
		error = flow_divert_handle_data(fd_cb, packet, sizeof(hdr));
		break;
	case FLOW_DIVERT_PKT_READ_NOTIFY:
		flow_divert_handle_read_notification(fd_cb, packet, sizeof(hdr));
		break;
	case FLOW_DIVERT_PKT_PROPERTIES_UPDATE:
		flow_divert_handle_properties_update(fd_cb, packet, sizeof(hdr));
		break;
	default:
		FDLOG(LOG_WARNING, fd_cb, "got an unknown message type: %d", hdr.packet_type);
		break;
	}

	FDRELEASE(fd_cb);

done:
	mbuf_freem(packet);
	return error;
}

static void
flow_divert_close_all(struct flow_divert_group *group)
{
	struct flow_divert_pcb                  *fd_cb;
	SLIST_HEAD(, flow_divert_pcb)   tmp_list;

	SLIST_INIT(&tmp_list);

	lck_rw_lock_exclusive(&group->lck);

	MBUFQ_DRAIN(&group->send_queue);

	RB_FOREACH(fd_cb, fd_pcb_tree, &group->pcb_tree) {
		FDRETAIN(fd_cb);
		SLIST_INSERT_HEAD(&tmp_list, fd_cb, tmp_list_entry);
	}

	lck_rw_done(&group->lck);

	while (!SLIST_EMPTY(&tmp_list)) {
		fd_cb = SLIST_FIRST(&tmp_list);
		FDLOCK(fd_cb);
		SLIST_REMOVE_HEAD(&tmp_list, tmp_list_entry);
		if (fd_cb->so != NULL) {
			socket_lock(fd_cb->so, 0);
			flow_divert_pcb_remove(fd_cb);
			flow_divert_update_closed_state(fd_cb, SHUT_RDWR, TRUE);
			fd_cb->so->so_error = ECONNABORTED;
			flow_divert_disconnect_socket(fd_cb->so);
			socket_unlock(fd_cb->so, 0);
		}
		FDUNLOCK(fd_cb);
		FDRELEASE(fd_cb);
	}
}

void
flow_divert_detach(struct socket *so)
{
	struct flow_divert_pcb  *fd_cb          = so->so_fd_pcb;

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	so->so_flags &= ~SOF_FLOW_DIVERT;
	so->so_fd_pcb = NULL;

	FDLOG(LOG_INFO, fd_cb, "Detaching, ref count = %d", fd_cb->ref_count);

	if (fd_cb->group != NULL) {
		/* Last-ditch effort to send any buffered data */
		flow_divert_send_buffered_data(fd_cb, TRUE);

		flow_divert_update_closed_state(fd_cb, SHUT_RDWR, FALSE);
		flow_divert_send_close_if_needed(fd_cb);
		/* Remove from the group */
		flow_divert_pcb_remove(fd_cb);
	}

	socket_unlock(so, 0);
	FDLOCK(fd_cb);
	fd_cb->so = NULL;
	FDUNLOCK(fd_cb);
	socket_lock(so, 0);

	FDRELEASE(fd_cb);       /* Release the socket's reference */
}

static int
flow_divert_close(struct socket *so)
{
	struct flow_divert_pcb  *fd_cb          = so->so_fd_pcb;

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	FDLOG0(LOG_INFO, fd_cb, "Closing");

	if (SOCK_TYPE(so) == SOCK_STREAM) {
		soisdisconnecting(so);
		sbflush(&so->so_rcv);
	}

	flow_divert_send_buffered_data(fd_cb, TRUE);
	flow_divert_update_closed_state(fd_cb, SHUT_RDWR, FALSE);
	flow_divert_send_close_if_needed(fd_cb);

	/* Remove from the group */
	flow_divert_pcb_remove(fd_cb);

	return 0;
}

static int
flow_divert_disconnectx(struct socket *so, sae_associd_t aid,
    sae_connid_t cid __unused)
{
	if (aid != SAE_ASSOCID_ANY && aid != SAE_ASSOCID_ALL) {
		return EINVAL;
	}

	return flow_divert_close(so);
}

static int
flow_divert_shutdown(struct socket *so)
{
	struct flow_divert_pcb  *fd_cb          = so->so_fd_pcb;

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	FDLOG0(LOG_INFO, fd_cb, "Can't send more");

	socantsendmore(so);

	flow_divert_update_closed_state(fd_cb, SHUT_WR, FALSE);
	flow_divert_send_close_if_needed(fd_cb);

	return 0;
}

static int
flow_divert_rcvd(struct socket *so, int flags __unused)
{
	struct flow_divert_pcb  *fd_cb = so->so_fd_pcb;
	int space = sbspace(&so->so_rcv);

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	FDLOG(LOG_DEBUG, fd_cb, "app read bytes, space = %d", space);
	if ((fd_cb->flags & FLOW_DIVERT_NOTIFY_ON_RECEIVED) &&
	    (space > 0) &&
	    flow_divert_send_read_notification(fd_cb) == 0) {
		FDLOG0(LOG_INFO, fd_cb, "Sent a read notification");
		fd_cb->flags &= ~FLOW_DIVERT_NOTIFY_ON_RECEIVED;
	}

	return 0;
}

static int
flow_divert_append_target_endpoint_tlv(mbuf_t connect_packet, struct sockaddr *toaddr)
{
	int error = 0;
	int port  = 0;

	if (!flow_divert_is_sockaddr_valid(toaddr)) {
		FDLOG(LOG_ERR, &nil_pcb, "Invalid target address, family = %u, length = %u", toaddr->sa_family, toaddr->sa_len);
		error = EINVAL;
		goto done;
	}

	error = flow_divert_packet_append_tlv(connect_packet, FLOW_DIVERT_TLV_TARGET_ADDRESS, toaddr->sa_len, toaddr);
	if (error) {
		goto done;
	}

	if (toaddr->sa_family == AF_INET) {
		port = ntohs((satosin(toaddr))->sin_port);
	} else {
		port = ntohs((satosin6(toaddr))->sin6_port);
	}

	error = flow_divert_packet_append_tlv(connect_packet, FLOW_DIVERT_TLV_TARGET_PORT, sizeof(port), &port);
	if (error) {
		goto done;
	}

done:
	return error;
}

struct sockaddr *
flow_divert_get_buffered_target_address(mbuf_t buffer)
{
	if (buffer != NULL && buffer->m_type == MT_SONAME) {
		struct sockaddr *toaddr = mtod(buffer, struct sockaddr *);
		if (toaddr != NULL && flow_divert_is_sockaddr_valid(toaddr)) {
			return toaddr;
		}
	}
	return NULL;
}

static boolean_t
flow_divert_is_sockaddr_valid(struct sockaddr *addr)
{
	switch (addr->sa_family) {
	case AF_INET:
		if (addr->sa_len < sizeof(struct sockaddr_in)) {
			return FALSE;
		}
		break;
	case AF_INET6:
		if (addr->sa_len < sizeof(struct sockaddr_in6)) {
			return FALSE;
		}
		break;
	default:
		return FALSE;
	}
	return TRUE;
}

static errno_t
flow_divert_dup_addr(sa_family_t family, struct sockaddr *addr,
    struct sockaddr **dup)
{
	int                                             error           = 0;
	struct sockaddr                 *result;
	struct sockaddr_storage ss;

	if (addr != NULL) {
		result = addr;
	} else {
		memset(&ss, 0, sizeof(ss));
		ss.ss_family = family;
		if (ss.ss_family == AF_INET) {
			ss.ss_len = sizeof(struct sockaddr_in);
		} else if (ss.ss_family == AF_INET6) {
			ss.ss_len = sizeof(struct sockaddr_in6);
		} else {
			error = EINVAL;
		}
		result = (struct sockaddr *)&ss;
	}

	if (!error) {
		*dup = dup_sockaddr(result, 1);
		if (*dup == NULL) {
			error = ENOBUFS;
		}
	}

	return error;
}

static void
flow_divert_disconnect_socket(struct socket *so)
{
	soisdisconnected(so);
	if (SOCK_TYPE(so) == SOCK_DGRAM) {
		struct inpcb *inp = NULL;

		inp = sotoinpcb(so);
		if (inp != NULL) {
			if (SOCK_CHECK_DOM(so, PF_INET6)) {
				in6_pcbdetach(inp);
			} else {
				in_pcbdetach(inp);
			}
		}
	}
}

static errno_t
flow_divert_ctloutput(struct socket *so, struct sockopt *sopt)
{
	struct flow_divert_pcb  *fd_cb  = so->so_fd_pcb;

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	if (sopt->sopt_name == SO_TRAFFIC_CLASS) {
		if (sopt->sopt_dir == SOPT_SET && fd_cb->flags & FLOW_DIVERT_CONNECT_STARTED) {
			flow_divert_send_traffic_class_update(fd_cb, so->so_traffic_class);
		}
	}

	if (SOCK_DOM(so) == PF_INET) {
		return g_tcp_protosw->pr_ctloutput(so, sopt);
	} else if (SOCK_DOM(so) == PF_INET6) {
		return g_tcp6_protosw->pr_ctloutput(so, sopt);
	}
	return 0;
}

static errno_t
flow_divert_connect_out_internal(struct socket *so, struct sockaddr *to, proc_t p, bool implicit)
{
	struct flow_divert_pcb  *fd_cb  = so->so_fd_pcb;
	int                                             error   = 0;
	struct inpcb                    *inp    = sotoinpcb(so);
	struct sockaddr_in              *sinp;
	mbuf_t                                  connect_packet = NULL;
	int                                             do_send = 1;

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	if (fd_cb->group == NULL) {
		error = ENETUNREACH;
		goto done;
	}

	if (inp == NULL) {
		error = EINVAL;
		goto done;
	} else if (inp->inp_state == INPCB_STATE_DEAD) {
		if (so->so_error) {
			error = so->so_error;
			so->so_error = 0;
		} else {
			error = EINVAL;
		}
		goto done;
	}

	if (fd_cb->flags & FLOW_DIVERT_CONNECT_STARTED) {
		error = EALREADY;
		goto done;
	}

	FDLOG0(LOG_INFO, fd_cb, "Connecting");

	if (fd_cb->connect_packet == NULL) {
		struct sockaddr_in sin = {};
		struct ifnet *ifp = NULL;

		if (to == NULL) {
			FDLOG0(LOG_ERR, fd_cb, "No destination address available when creating connect packet");
			error = EINVAL;
			goto done;
		}

		fd_cb->original_remote_endpoint = dup_sockaddr(to, 0);
		if (fd_cb->original_remote_endpoint == NULL) {
			FDLOG0(LOG_ERR, fd_cb, "Failed to dup the remote endpoint");
			error = ENOMEM;
			goto done;
		}
		fd_cb->original_vflag = inp->inp_vflag;
		fd_cb->original_last_outifp = inp->inp_last_outifp;
		fd_cb->original_last_outifp6 = inp->in6p_last_outifp;

		sinp = (struct sockaddr_in *)(void *)to;
		if (sinp->sin_family == AF_INET && IN_MULTICAST(ntohl(sinp->sin_addr.s_addr))) {
			error = EAFNOSUPPORT;
			goto done;
		}

		if (to->sa_family == AF_INET6 && !(inp->inp_flags & IN6P_IPV6_V6ONLY)) {
			struct sockaddr_in6 sin6 = {};
			sin6.sin6_family = AF_INET6;
			sin6.sin6_len = sizeof(struct sockaddr_in6);
			sin6.sin6_port = satosin6(to)->sin6_port;
			sin6.sin6_addr = satosin6(to)->sin6_addr;
			if (IN6_IS_ADDR_V4MAPPED(&(sin6.sin6_addr))) {
				in6_sin6_2_sin(&sin, &sin6);
				to = (struct sockaddr *)&sin;
			}
		}

		if (to->sa_family == AF_INET6) {
			inp->inp_vflag &= ~INP_IPV4;
			inp->inp_vflag |= INP_IPV6;
			fd_cb->local_endpoint.sin6.sin6_len = sizeof(struct sockaddr_in6);
			fd_cb->local_endpoint.sin6.sin6_family = AF_INET6;
			fd_cb->local_endpoint.sin6.sin6_port = inp->inp_lport;
			error = in6_pcbladdr(inp, to, &(fd_cb->local_endpoint.sin6.sin6_addr), &ifp);
			if (error) {
				FDLOG(LOG_WARNING, fd_cb, "failed to get a local IPv6 address: %d", error);
				if (!(fd_cb->flags & FLOW_DIVERT_FLOW_IS_TRANSPARENT) || IN6_IS_ADDR_UNSPECIFIED(&(satosin6(to)->sin6_addr))) {
					error = 0;
				} else {
					goto done;
				}
			}
			if (ifp != NULL) {
				inp->in6p_last_outifp = ifp;
				ifnet_release(ifp);
			}
		} else if (to->sa_family == AF_INET) {
			inp->inp_vflag |= INP_IPV4;
			inp->inp_vflag &= ~INP_IPV6;
			fd_cb->local_endpoint.sin.sin_len = sizeof(struct sockaddr_in);
			fd_cb->local_endpoint.sin.sin_family = AF_INET;
			fd_cb->local_endpoint.sin.sin_port = inp->inp_lport;
			error = in_pcbladdr(inp, to, &(fd_cb->local_endpoint.sin.sin_addr), IFSCOPE_NONE, &ifp, 0);
			if (error) {
				FDLOG(LOG_WARNING, fd_cb, "failed to get a local IPv4 address: %d", error);
				if (!(fd_cb->flags & FLOW_DIVERT_FLOW_IS_TRANSPARENT) || satosin(to)->sin_addr.s_addr == INADDR_ANY) {
					error = 0;
				} else {
					goto done;
				}
			}
			if (ifp != NULL) {
				inp->inp_last_outifp = ifp;
				ifnet_release(ifp);
			}
		} else {
			FDLOG(LOG_WARNING, fd_cb, "target address has an unsupported family: %d", to->sa_family);
		}

		error = flow_divert_check_no_cellular(fd_cb) ||
		    flow_divert_check_no_expensive(fd_cb) ||
		    flow_divert_check_no_constrained(fd_cb);
		if (error) {
			goto done;
		}

		if (SOCK_TYPE(so) == SOCK_STREAM || /* TCP or */
		    !implicit || /* connect() was called or */
		    ((inp->inp_vflag & INP_IPV6) && !IN6_IS_ADDR_UNSPECIFIED(&inp->in6p_laddr)) || /* local address is not un-specified */
		    ((inp->inp_vflag & INP_IPV4) && inp->inp_laddr.s_addr != INADDR_ANY)) {
			fd_cb->flags |= FLOW_DIVERT_SHOULD_SET_LOCAL_ADDR;
		}

		error = flow_divert_create_connect_packet(fd_cb, to, so, p, &connect_packet);
		if (error) {
			goto done;
		}

		if (!implicit || SOCK_TYPE(so) == SOCK_STREAM) {
			flow_divert_set_remote_endpoint(fd_cb, to);
			flow_divert_set_local_endpoint(fd_cb, &(fd_cb->local_endpoint.sa));
		}

		if (implicit) {
			fd_cb->flags |= FLOW_DIVERT_IMPLICIT_CONNECT;
		}

		if (so->so_flags1 & SOF1_PRECONNECT_DATA) {
			FDLOG0(LOG_INFO, fd_cb, "Delaying sending the connect packet until send or receive");
			do_send = 0;
		}

		fd_cb->connect_packet = connect_packet;
		connect_packet = NULL;
	} else {
		FDLOG0(LOG_INFO, fd_cb, "Sending saved connect packet");
	}

	if (do_send) {
		error = flow_divert_send_connect_packet(fd_cb);
		if (error) {
			goto done;
		}

		fd_cb->flags |= FLOW_DIVERT_CONNECT_STARTED;
	}

	if (SOCK_TYPE(so) == SOCK_DGRAM && !(fd_cb->flags & FLOW_DIVERT_HAS_TOKEN)) {
		soisconnected(so);
	} else {
		soisconnecting(so);
	}

done:
	return error;
}

errno_t
flow_divert_connect_out(struct socket *so, struct sockaddr *to, proc_t p)
{
#if CONTENT_FILTER
	if (SOCK_TYPE(so) == SOCK_STREAM && !(so->so_flags & SOF_CONTENT_FILTER)) {
		int error = cfil_sock_attach(so, NULL, to, CFS_CONNECTION_DIR_OUT);
		if (error != 0) {
			struct flow_divert_pcb  *fd_cb  = so->so_fd_pcb;
			FDLOG(LOG_ERR, fd_cb, "Failed to attach cfil: %d", error);
			return error;
		}
	}
#endif /* CONTENT_FILTER */

	return flow_divert_connect_out_internal(so, to, p, false);
}

static int
flow_divert_connectx_out_common(struct socket *so, struct sockaddr *dst,
    struct proc *p, sae_connid_t *pcid, struct uio *auio, user_ssize_t *bytes_written)
{
	struct inpcb *inp = sotoinpcb(so);
	int error;

	if (inp == NULL) {
		return EINVAL;
	}

	VERIFY(dst != NULL);

	error = flow_divert_connect_out(so, dst, p);

	if (error != 0) {
		return error;
	}

	/* if there is data, send it */
	if (auio != NULL) {
		user_ssize_t datalen = 0;

		socket_unlock(so, 0);

		VERIFY(bytes_written != NULL);

		datalen = uio_resid(auio);
		error = so->so_proto->pr_usrreqs->pru_sosend(so, NULL, (uio_t)auio, NULL, NULL, 0);
		socket_lock(so, 0);

		if (error == 0 || error == EWOULDBLOCK) {
			*bytes_written = datalen - uio_resid(auio);
		}

		/*
		 * sosend returns EWOULDBLOCK if it's a non-blocking
		 * socket or a timeout occured (this allows to return
		 * the amount of queued data through sendit()).
		 *
		 * However, connectx() returns EINPROGRESS in case of a
		 * blocking socket. So we change the return value here.
		 */
		if (error == EWOULDBLOCK) {
			error = EINPROGRESS;
		}
	}

	if (error == 0 && pcid != NULL) {
		*pcid = 1;      /* there is only 1 connection for a TCP */
	}

	return error;
}

static int
flow_divert_connectx_out(struct socket *so, struct sockaddr *src __unused,
    struct sockaddr *dst, struct proc *p, uint32_t ifscope __unused,
    sae_associd_t aid __unused, sae_connid_t *pcid, uint32_t flags __unused, void *arg __unused,
    uint32_t arglen __unused, struct uio *uio, user_ssize_t *bytes_written)
{
	return flow_divert_connectx_out_common(so, dst, p, pcid, uio, bytes_written);
}

static int
flow_divert_connectx6_out(struct socket *so, struct sockaddr *src __unused,
    struct sockaddr *dst, struct proc *p, uint32_t ifscope __unused,
    sae_associd_t aid __unused, sae_connid_t *pcid, uint32_t flags __unused, void *arg __unused,
    uint32_t arglen __unused, struct uio *uio, user_ssize_t *bytes_written)
{
	return flow_divert_connectx_out_common(so, dst, p, pcid, uio, bytes_written);
}

static errno_t
flow_divert_data_out(struct socket *so, int flags, mbuf_t data, struct sockaddr *to, mbuf_t control, struct proc *p)
{
	struct flow_divert_pcb  *fd_cb  = so->so_fd_pcb;
	int                                             error   = 0;
	struct inpcb *inp;
#if CONTENT_FILTER
	struct m_tag *cfil_tag = NULL;
#endif

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	inp = sotoinpcb(so);
	if (inp == NULL || inp->inp_state == INPCB_STATE_DEAD) {
		error = ECONNRESET;
		goto done;
	}

	if (control && mbuf_len(control) > 0) {
		error = EINVAL;
		goto done;
	}

	if (flags & MSG_OOB) {
		error = EINVAL;
		goto done; /* We don't support OOB data */
	}

#if CONTENT_FILTER
	/*
	 * If the socket is subject to a UDP Content Filter and no remote address is passed in,
	 * retrieve the CFIL saved remote address from the mbuf and use it.
	 */
	if (to == NULL && so->so_cfil_db) {
		struct sockaddr *cfil_faddr = NULL;
		cfil_tag = cfil_dgram_get_socket_state(data, NULL, NULL, &cfil_faddr, NULL);
		if (cfil_tag) {
			to = (struct sockaddr *)(void *)cfil_faddr;
		}
		FDLOG(LOG_INFO, fd_cb, "Using remote address from CFIL saved state: %p", to);
	}
#endif

	/* Implicit connect */
	if (!(fd_cb->flags & FLOW_DIVERT_CONNECT_STARTED)) {
		FDLOG0(LOG_INFO, fd_cb, "implicit connect");

		error = flow_divert_connect_out_internal(so, to, p, true);
		if (error) {
			goto done;
		}
	} else {
		error = flow_divert_check_no_cellular(fd_cb) ||
		    flow_divert_check_no_expensive(fd_cb) ||
		    flow_divert_check_no_constrained(fd_cb);
		if (error) {
			goto done;
		}
	}

	FDLOG(LOG_DEBUG, fd_cb, "app wrote %lu bytes", mbuf_pkthdr_len(data));

	fd_cb->bytes_written_by_app += mbuf_pkthdr_len(data);
	error = flow_divert_send_app_data(fd_cb, data, to);

	data = NULL;

	if (error) {
		goto done;
	}

	if (flags & PRUS_EOF) {
		flow_divert_shutdown(so);
	}

done:
	if (data) {
		mbuf_freem(data);
	}
	if (control) {
		mbuf_free(control);
	}
#if CONTENT_FILTER
	if (cfil_tag) {
		m_tag_free(cfil_tag);
	}
#endif

	return error;
}

static int
flow_divert_preconnect(struct socket *so)
{
	int error = 0;
	struct flow_divert_pcb  *fd_cb  = so->so_fd_pcb;

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	if (!(fd_cb->flags & FLOW_DIVERT_CONNECT_STARTED)) {
		FDLOG0(LOG_INFO, fd_cb, "Pre-connect read: sending saved connect packet");
		error = flow_divert_send_connect_packet(so->so_fd_pcb);
		if (error) {
			return error;
		}

		fd_cb->flags |= FLOW_DIVERT_CONNECT_STARTED;
	}

	soclearfastopen(so);

	return error;
}

static void
flow_divert_set_protosw(struct socket *so)
{
	if (SOCK_DOM(so) == PF_INET) {
		so->so_proto = &g_flow_divert_in_protosw;
	} else {
		so->so_proto = (struct protosw *)&g_flow_divert_in6_protosw;
	}
}

static void
flow_divert_set_udp_protosw(struct socket *so)
{
	if (SOCK_DOM(so) == PF_INET) {
		so->so_proto = &g_flow_divert_in_udp_protosw;
	} else {
		so->so_proto = (struct protosw *)&g_flow_divert_in6_udp_protosw;
	}
}

errno_t
flow_divert_implicit_data_out(struct socket *so, int flags, mbuf_t data, struct sockaddr *to, mbuf_t control, struct proc *p)
{
	struct flow_divert_pcb  *fd_cb  = so->so_fd_pcb;
	struct inpcb *inp;
	int error = 0;

	inp = sotoinpcb(so);
	if (inp == NULL) {
		return EINVAL;
	}

	if (fd_cb == NULL) {
		error = flow_divert_pcb_init(so);
		fd_cb  = so->so_fd_pcb;
		if (error != 0 || fd_cb == NULL) {
			goto done;
		}
	}
	return flow_divert_data_out(so, flags, data, to, control, p);

done:
	if (data) {
		mbuf_freem(data);
	}
	if (control) {
		mbuf_free(control);
	}

	return error;
}

static errno_t
flow_divert_pcb_init_internal(struct socket *so, uint32_t ctl_unit, uint32_t aggregate_unit)
{
	errno_t error = 0;
	struct flow_divert_pcb *fd_cb;
	uint32_t agg_unit = aggregate_unit;
	bool is_aggregate = false;
	uint32_t group_unit = flow_divert_derive_kernel_control_unit(ctl_unit, &agg_unit, &is_aggregate);

	if (group_unit == 0) {
		return EINVAL;
	}

	if (so->so_flags & SOF_FLOW_DIVERT) {
		return EALREADY;
	}

	fd_cb = flow_divert_pcb_create(so);
	if (fd_cb != NULL) {
		so->so_fd_pcb = fd_cb;
		so->so_flags |= SOF_FLOW_DIVERT;
		fd_cb->control_group_unit = group_unit;
		fd_cb->policy_control_unit = ctl_unit;
		fd_cb->aggregate_unit = agg_unit;
		if (is_aggregate) {
			fd_cb->flags |= FLOW_DIVERT_FLOW_IS_TRANSPARENT;
		} else {
			fd_cb->flags &= ~FLOW_DIVERT_FLOW_IS_TRANSPARENT;
		}

		error = flow_divert_pcb_insert(fd_cb, group_unit);
		if (error) {
			FDLOG(LOG_ERR, fd_cb, "pcb insert failed: %d", error);
			so->so_fd_pcb = NULL;
			so->so_flags &= ~SOF_FLOW_DIVERT;
			FDRELEASE(fd_cb);
		} else {
			if (SOCK_TYPE(so) == SOCK_STREAM) {
				flow_divert_set_protosw(so);
			} else if (SOCK_TYPE(so) == SOCK_DGRAM) {
				flow_divert_set_udp_protosw(so);
			}

			FDLOG0(LOG_INFO, fd_cb, "Created");
		}
	} else {
		error = ENOMEM;
	}

	return error;
}

errno_t
flow_divert_pcb_init(struct socket *so)
{
	struct inpcb *inp = sotoinpcb(so);
	uint32_t aggregate_units = 0;
	uint32_t ctl_unit = necp_socket_get_flow_divert_control_unit(inp, &aggregate_units);
	return flow_divert_pcb_init_internal(so, ctl_unit, aggregate_units);
}

errno_t
flow_divert_token_set(struct socket *so, struct sockopt *sopt)
{
	uint32_t ctl_unit = 0;
	uint32_t key_unit = 0;
	uint32_t aggregate_unit = 0;
	int error = 0;
	int hmac_error = 0;
	mbuf_t token = NULL;

	if (so->so_flags & SOF_FLOW_DIVERT) {
		error = EALREADY;
		goto done;
	}

	if (g_init_result) {
		FDLOG(LOG_ERR, &nil_pcb, "flow_divert_init failed (%d), cannot use flow divert", g_init_result);
		error = ENOPROTOOPT;
		goto done;
	}

	if ((SOCK_TYPE(so) != SOCK_STREAM && SOCK_TYPE(so) != SOCK_DGRAM) ||
	    (SOCK_PROTO(so) != IPPROTO_TCP && SOCK_PROTO(so) != IPPROTO_UDP) ||
	    (SOCK_DOM(so) != PF_INET && SOCK_DOM(so) != PF_INET6)) {
		error = EINVAL;
		goto done;
	} else {
		if (SOCK_TYPE(so) == SOCK_STREAM && SOCK_PROTO(so) == IPPROTO_TCP) {
			struct tcpcb *tp = sototcpcb(so);
			if (tp == NULL || tp->t_state != TCPS_CLOSED) {
				error = EINVAL;
				goto done;
			}
		}
	}

	error = soopt_getm(sopt, &token);
	if (error) {
		token = NULL;
		goto done;
	}

	error = soopt_mcopyin(sopt, token);
	if (error) {
		token = NULL;
		goto done;
	}

	error = flow_divert_packet_get_tlv(token, 0, FLOW_DIVERT_TLV_KEY_UNIT, sizeof(key_unit), (void *)&key_unit, NULL);
	if (!error) {
		key_unit = ntohl(key_unit);
		if (key_unit >= GROUP_COUNT_MAX) {
			key_unit = 0;
		}
	} else if (error != ENOENT) {
		FDLOG(LOG_ERR, &nil_pcb, "Failed to get the key unit from the token: %d", error);
		goto done;
	} else {
		key_unit = 0;
	}

	error = flow_divert_packet_get_tlv(token, 0, FLOW_DIVERT_TLV_CTL_UNIT, sizeof(ctl_unit), (void *)&ctl_unit, NULL);
	if (error) {
		FDLOG(LOG_ERR, &nil_pcb, "Failed to get the control socket unit from the token: %d", error);
		goto done;
	}

	error = flow_divert_packet_get_tlv(token, 0, FLOW_DIVERT_TLV_AGGREGATE_UNIT, sizeof(aggregate_unit), (void *)&aggregate_unit, NULL);
	if (error && error != ENOENT) {
		FDLOG(LOG_ERR, &nil_pcb, "Failed to get the aggregate unit from the token: %d", error);
		goto done;
	}

	/* A valid kernel control unit is required */
	ctl_unit = ntohl(ctl_unit);
	aggregate_unit = ntohl(aggregate_unit);

	if (ctl_unit > 0 && ctl_unit < GROUP_COUNT_MAX) {
		socket_unlock(so, 0);
		hmac_error = flow_divert_packet_verify_hmac(token, (key_unit != 0 ? key_unit : ctl_unit));
		socket_lock(so, 0);

		if (hmac_error && hmac_error != ENOENT) {
			FDLOG(LOG_ERR, &nil_pcb, "HMAC verfication failed: %d", hmac_error);
			error = hmac_error;
			goto done;
		}
	}

	error = flow_divert_pcb_init_internal(so, ctl_unit, aggregate_unit);
	if (error == 0) {
		struct flow_divert_pcb *fd_cb = so->so_fd_pcb;
		int log_level = LOG_NOTICE;

		error = flow_divert_packet_get_tlv(token, 0, FLOW_DIVERT_TLV_LOG_LEVEL, sizeof(log_level), &log_level, NULL);
		if (error == 0) {
			fd_cb->log_level = (uint8_t)log_level;
		}
		error = 0;

		fd_cb->connect_token = token;
		token = NULL;

		fd_cb->flags |= FLOW_DIVERT_HAS_TOKEN;
	}

	if (hmac_error == 0) {
		struct flow_divert_pcb *fd_cb = so->so_fd_pcb;
		if (fd_cb != NULL) {
			fd_cb->flags |= FLOW_DIVERT_HAS_HMAC;
		}
	}

done:
	if (token != NULL) {
		mbuf_freem(token);
	}

	return error;
}

errno_t
flow_divert_token_get(struct socket *so, struct sockopt *sopt)
{
	uint32_t                                        ctl_unit;
	int                                                     error                                           = 0;
	uint8_t                                         hmac[SHA_DIGEST_LENGTH];
	struct flow_divert_pcb          *fd_cb                                          = so->so_fd_pcb;
	mbuf_t                                          token                                           = NULL;
	struct flow_divert_group        *control_group                          = NULL;

	if (!(so->so_flags & SOF_FLOW_DIVERT)) {
		error = EINVAL;
		goto done;
	}

	VERIFY((so->so_flags & SOF_FLOW_DIVERT) && so->so_fd_pcb != NULL);

	if (fd_cb->group == NULL) {
		error = EINVAL;
		goto done;
	}

	error = mbuf_gethdr(MBUF_DONTWAIT, MBUF_TYPE_HEADER, &token);
	if (error) {
		FDLOG(LOG_ERR, fd_cb, "failed to allocate the header mbuf: %d", error);
		goto done;
	}

	ctl_unit = htonl(fd_cb->group->ctl_unit);

	error = flow_divert_packet_append_tlv(token, FLOW_DIVERT_TLV_CTL_UNIT, sizeof(ctl_unit), &ctl_unit);
	if (error) {
		goto done;
	}

	error = flow_divert_packet_append_tlv(token, FLOW_DIVERT_TLV_FLOW_ID, sizeof(fd_cb->hash), &fd_cb->hash);
	if (error) {
		goto done;
	}

	if (fd_cb->app_data != NULL) {
		error = flow_divert_packet_append_tlv(token, FLOW_DIVERT_TLV_APP_DATA, (uint32_t)fd_cb->app_data_length, fd_cb->app_data);
		if (error) {
			goto done;
		}
	}

	socket_unlock(so, 0);
	lck_rw_lock_shared(&g_flow_divert_group_lck);

	if (g_flow_divert_groups != NULL && g_active_group_count > 0 &&
	    fd_cb->control_group_unit > 0 && fd_cb->control_group_unit < GROUP_COUNT_MAX) {
		control_group = g_flow_divert_groups[fd_cb->control_group_unit];
	}

	if (control_group != NULL) {
		lck_rw_lock_shared(&control_group->lck);
		ctl_unit = htonl(control_group->ctl_unit);
		error = flow_divert_packet_append_tlv(token, FLOW_DIVERT_TLV_KEY_UNIT, sizeof(ctl_unit), &ctl_unit);
		if (!error) {
			error = flow_divert_packet_compute_hmac(token, control_group, hmac);
		}
		lck_rw_done(&control_group->lck);
	} else {
		error = ENOPROTOOPT;
	}

	lck_rw_done(&g_flow_divert_group_lck);
	socket_lock(so, 0);

	if (error) {
		goto done;
	}

	error = flow_divert_packet_append_tlv(token, FLOW_DIVERT_TLV_HMAC, sizeof(hmac), hmac);
	if (error) {
		goto done;
	}

	if (sopt->sopt_val == USER_ADDR_NULL) {
		/* If the caller passed NULL to getsockopt, just set the size of the token and return */
		sopt->sopt_valsize = mbuf_pkthdr_len(token);
		goto done;
	}

	error = soopt_mcopyout(sopt, token);
	if (error) {
		token = NULL;   /* For some reason, soopt_mcopyout() frees the mbuf if it fails */
		goto done;
	}

done:
	if (token != NULL) {
		mbuf_freem(token);
	}

	return error;
}

static errno_t
flow_divert_kctl_connect(kern_ctl_ref kctlref __unused, struct sockaddr_ctl *sac, void **unitinfo)
{
	struct flow_divert_group        *new_group      = NULL;
	int                             error           = 0;

	if (sac->sc_unit >= GROUP_COUNT_MAX) {
		error = EINVAL;
		goto done;
	}

	*unitinfo = NULL;

	new_group = zalloc_flags(flow_divert_group_zone, Z_WAITOK | Z_ZERO);
	lck_rw_init(&new_group->lck, flow_divert_mtx_grp, flow_divert_mtx_attr);
	RB_INIT(&new_group->pcb_tree);
	new_group->ctl_unit = sac->sc_unit;
	MBUFQ_INIT(&new_group->send_queue);
	new_group->signing_id_trie.root = NULL_TRIE_IDX;

	lck_rw_lock_exclusive(&g_flow_divert_group_lck);

	if (g_flow_divert_groups == NULL) {
		MALLOC(g_flow_divert_groups,
		    struct flow_divert_group **,
		    GROUP_COUNT_MAX * sizeof(struct flow_divert_group *),
		    M_TEMP,
		    M_WAITOK | M_ZERO);
	}

	if (g_flow_divert_groups == NULL) {
		error = ENOBUFS;
	} else if (g_flow_divert_groups[sac->sc_unit] != NULL) {
		error = EALREADY;
	} else {
		g_flow_divert_groups[sac->sc_unit] = new_group;
		g_active_group_count++;
	}

	lck_rw_done(&g_flow_divert_group_lck);

done:
	if (error == 0) {
		*unitinfo = new_group;
	} else if (new_group != NULL) {
		zfree(flow_divert_group_zone, new_group);
	}
	return error;
}

static errno_t
flow_divert_kctl_disconnect(kern_ctl_ref kctlref __unused, uint32_t unit, void *unitinfo)
{
	struct flow_divert_group        *group  = NULL;
	errno_t                                         error   = 0;

	if (unit >= GROUP_COUNT_MAX) {
		return EINVAL;
	}

	if (unitinfo == NULL) {
		return 0;
	}

	FDLOG(LOG_INFO, &nil_pcb, "disconnecting group %d", unit);

	lck_rw_lock_exclusive(&g_flow_divert_group_lck);

	if (g_flow_divert_groups == NULL || g_active_group_count == 0) {
		panic("flow divert group %u is disconnecting, but no groups are active (groups = %p, active count = %u", unit,
		    g_flow_divert_groups, g_active_group_count);
	}

	group = g_flow_divert_groups[unit];

	if (group != (struct flow_divert_group *)unitinfo) {
		panic("group with unit %d (%p) != unit info (%p)", unit, group, unitinfo);
	}

	g_flow_divert_groups[unit] = NULL;
	g_active_group_count--;

	if (g_active_group_count == 0) {
		FREE(g_flow_divert_groups, M_TEMP);
		g_flow_divert_groups = NULL;
	}

	lck_rw_done(&g_flow_divert_group_lck);

	if (group != NULL) {
		flow_divert_close_all(group);

		lck_rw_lock_exclusive(&group->lck);

		if (group->token_key != NULL) {
			memset(group->token_key, 0, group->token_key_size);
			FREE(group->token_key, M_TEMP);
			group->token_key = NULL;
			group->token_key_size = 0;
		}

		/* Re-set the current trie */
		if (group->signing_id_trie.memory != NULL) {
			FREE(group->signing_id_trie.memory, M_TEMP);
		}
		memset(&group->signing_id_trie, 0, sizeof(group->signing_id_trie));
		group->signing_id_trie.root = NULL_TRIE_IDX;

		lck_rw_done(&group->lck);

		zfree(flow_divert_group_zone, group);
	} else {
		error = EINVAL;
	}

	return error;
}

static errno_t
flow_divert_kctl_send(kern_ctl_ref kctlref __unused, uint32_t unit __unused, void *unitinfo, mbuf_t m, int flags __unused)
{
	return flow_divert_input(m, (struct flow_divert_group *)unitinfo);
}

static void
flow_divert_kctl_rcvd(kern_ctl_ref kctlref __unused, uint32_t unit __unused, void *unitinfo, int flags __unused)
{
	struct flow_divert_group        *group  = (struct flow_divert_group *)unitinfo;

	if (!OSTestAndClear(GROUP_BIT_CTL_ENQUEUE_BLOCKED, &group->atomic_bits)) {
		struct flow_divert_pcb                  *fd_cb;
		SLIST_HEAD(, flow_divert_pcb)   tmp_list;

		lck_rw_lock_shared(&g_flow_divert_group_lck);
		lck_rw_lock_exclusive(&group->lck);

		while (!MBUFQ_EMPTY(&group->send_queue)) {
			mbuf_t next_packet;
			FDLOG0(LOG_DEBUG, &nil_pcb, "trying ctl_enqueuembuf again");
			next_packet = MBUFQ_FIRST(&group->send_queue);
			int error = ctl_enqueuembuf(g_flow_divert_kctl_ref, group->ctl_unit, next_packet, CTL_DATA_EOR);
			if (error) {
				FDLOG(LOG_DEBUG, &nil_pcb, "ctl_enqueuembuf returned an error: %d", error);
				OSTestAndSet(GROUP_BIT_CTL_ENQUEUE_BLOCKED, &group->atomic_bits);
				lck_rw_done(&group->lck);
				lck_rw_done(&g_flow_divert_group_lck);
				return;
			}
			MBUFQ_DEQUEUE(&group->send_queue, next_packet);
		}

		SLIST_INIT(&tmp_list);

		RB_FOREACH(fd_cb, fd_pcb_tree, &group->pcb_tree) {
			FDRETAIN(fd_cb);
			SLIST_INSERT_HEAD(&tmp_list, fd_cb, tmp_list_entry);
		}

		lck_rw_done(&group->lck);

		SLIST_FOREACH(fd_cb, &tmp_list, tmp_list_entry) {
			FDLOCK(fd_cb);
			if (fd_cb->so != NULL) {
				socket_lock(fd_cb->so, 0);
				if (fd_cb->group != NULL) {
					flow_divert_send_buffered_data(fd_cb, FALSE);
				}
				socket_unlock(fd_cb->so, 0);
			}
			FDUNLOCK(fd_cb);
			FDRELEASE(fd_cb);
		}

		lck_rw_done(&g_flow_divert_group_lck);
	}
}

static int
flow_divert_kctl_init(void)
{
	struct kern_ctl_reg     ctl_reg;
	int                     result;

	memset(&ctl_reg, 0, sizeof(ctl_reg));

	strlcpy(ctl_reg.ctl_name, FLOW_DIVERT_CONTROL_NAME, sizeof(ctl_reg.ctl_name));
	ctl_reg.ctl_name[sizeof(ctl_reg.ctl_name) - 1] = '\0';
	ctl_reg.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED;
	ctl_reg.ctl_sendsize = FD_CTL_SENDBUFF_SIZE;
	ctl_reg.ctl_recvsize = FD_CTL_RCVBUFF_SIZE;

	ctl_reg.ctl_connect = flow_divert_kctl_connect;
	ctl_reg.ctl_disconnect = flow_divert_kctl_disconnect;
	ctl_reg.ctl_send = flow_divert_kctl_send;
	ctl_reg.ctl_rcvd = flow_divert_kctl_rcvd;

	result = ctl_register(&ctl_reg, &g_flow_divert_kctl_ref);

	if (result) {
		FDLOG(LOG_ERR, &nil_pcb, "flow_divert_kctl_init - ctl_register failed: %d\n", result);
		return result;
	}

	return 0;
}

void
flow_divert_init(void)
{
	memset(&nil_pcb, 0, sizeof(nil_pcb));
	nil_pcb.log_level = LOG_NOTICE;

	g_tcp_protosw = pffindproto(AF_INET, IPPROTO_TCP, SOCK_STREAM);

	VERIFY(g_tcp_protosw != NULL);

	memcpy(&g_flow_divert_in_protosw, g_tcp_protosw, sizeof(g_flow_divert_in_protosw));
	memcpy(&g_flow_divert_in_usrreqs, g_tcp_protosw->pr_usrreqs, sizeof(g_flow_divert_in_usrreqs));

	g_flow_divert_in_usrreqs.pru_connect = flow_divert_connect_out;
	g_flow_divert_in_usrreqs.pru_connectx = flow_divert_connectx_out;
	g_flow_divert_in_usrreqs.pru_disconnect = flow_divert_close;
	g_flow_divert_in_usrreqs.pru_disconnectx = flow_divert_disconnectx;
	g_flow_divert_in_usrreqs.pru_rcvd = flow_divert_rcvd;
	g_flow_divert_in_usrreqs.pru_send = flow_divert_data_out;
	g_flow_divert_in_usrreqs.pru_shutdown = flow_divert_shutdown;
	g_flow_divert_in_usrreqs.pru_preconnect = flow_divert_preconnect;

	g_flow_divert_in_protosw.pr_usrreqs = &g_flow_divert_in_usrreqs;
	g_flow_divert_in_protosw.pr_ctloutput = flow_divert_ctloutput;

	/*
	 * Socket filters shouldn't attach/detach to/from this protosw
	 * since pr_protosw is to be used instead, which points to the
	 * real protocol; if they do, it is a bug and we should panic.
	 */
	g_flow_divert_in_protosw.pr_filter_head.tqh_first =
	    (struct socket_filter *)(uintptr_t)0xdeadbeefdeadbeef;
	g_flow_divert_in_protosw.pr_filter_head.tqh_last =
	    (struct socket_filter **)(uintptr_t)0xdeadbeefdeadbeef;

	/* UDP */
	g_udp_protosw = pffindproto(AF_INET, IPPROTO_UDP, SOCK_DGRAM);
	VERIFY(g_udp_protosw != NULL);

	memcpy(&g_flow_divert_in_udp_protosw, g_udp_protosw, sizeof(g_flow_divert_in_udp_protosw));
	memcpy(&g_flow_divert_in_udp_usrreqs, g_udp_protosw->pr_usrreqs, sizeof(g_flow_divert_in_udp_usrreqs));

	g_flow_divert_in_udp_usrreqs.pru_connect = flow_divert_connect_out;
	g_flow_divert_in_udp_usrreqs.pru_connectx = flow_divert_connectx_out;
	g_flow_divert_in_udp_usrreqs.pru_disconnect = flow_divert_close;
	g_flow_divert_in_udp_usrreqs.pru_disconnectx = flow_divert_disconnectx;
	g_flow_divert_in_udp_usrreqs.pru_rcvd = flow_divert_rcvd;
	g_flow_divert_in_udp_usrreqs.pru_send = flow_divert_data_out;
	g_flow_divert_in_udp_usrreqs.pru_shutdown = flow_divert_shutdown;
	g_flow_divert_in_udp_usrreqs.pru_sosend_list = pru_sosend_list_notsupp;
	g_flow_divert_in_udp_usrreqs.pru_soreceive_list = pru_soreceive_list_notsupp;
	g_flow_divert_in_udp_usrreqs.pru_preconnect = flow_divert_preconnect;

	g_flow_divert_in_udp_protosw.pr_usrreqs = &g_flow_divert_in_usrreqs;
	g_flow_divert_in_udp_protosw.pr_ctloutput = flow_divert_ctloutput;

	/*
	 * Socket filters shouldn't attach/detach to/from this protosw
	 * since pr_protosw is to be used instead, which points to the
	 * real protocol; if they do, it is a bug and we should panic.
	 */
	g_flow_divert_in_udp_protosw.pr_filter_head.tqh_first =
	    (struct socket_filter *)(uintptr_t)0xdeadbeefdeadbeef;
	g_flow_divert_in_udp_protosw.pr_filter_head.tqh_last =
	    (struct socket_filter **)(uintptr_t)0xdeadbeefdeadbeef;

	g_tcp6_protosw = (struct ip6protosw *)pffindproto(AF_INET6, IPPROTO_TCP, SOCK_STREAM);

	VERIFY(g_tcp6_protosw != NULL);

	memcpy(&g_flow_divert_in6_protosw, g_tcp6_protosw, sizeof(g_flow_divert_in6_protosw));
	memcpy(&g_flow_divert_in6_usrreqs, g_tcp6_protosw->pr_usrreqs, sizeof(g_flow_divert_in6_usrreqs));

	g_flow_divert_in6_usrreqs.pru_connect = flow_divert_connect_out;
	g_flow_divert_in6_usrreqs.pru_connectx = flow_divert_connectx6_out;
	g_flow_divert_in6_usrreqs.pru_disconnect = flow_divert_close;
	g_flow_divert_in6_usrreqs.pru_disconnectx = flow_divert_disconnectx;
	g_flow_divert_in6_usrreqs.pru_rcvd = flow_divert_rcvd;
	g_flow_divert_in6_usrreqs.pru_send = flow_divert_data_out;
	g_flow_divert_in6_usrreqs.pru_shutdown = flow_divert_shutdown;
	g_flow_divert_in6_usrreqs.pru_preconnect = flow_divert_preconnect;

	g_flow_divert_in6_protosw.pr_usrreqs = &g_flow_divert_in6_usrreqs;
	g_flow_divert_in6_protosw.pr_ctloutput = flow_divert_ctloutput;
	/*
	 * Socket filters shouldn't attach/detach to/from this protosw
	 * since pr_protosw is to be used instead, which points to the
	 * real protocol; if they do, it is a bug and we should panic.
	 */
	g_flow_divert_in6_protosw.pr_filter_head.tqh_first =
	    (struct socket_filter *)(uintptr_t)0xdeadbeefdeadbeef;
	g_flow_divert_in6_protosw.pr_filter_head.tqh_last =
	    (struct socket_filter **)(uintptr_t)0xdeadbeefdeadbeef;

	/* UDP6 */
	g_udp6_protosw = (struct ip6protosw *)pffindproto(AF_INET6, IPPROTO_UDP, SOCK_DGRAM);

	VERIFY(g_udp6_protosw != NULL);

	memcpy(&g_flow_divert_in6_udp_protosw, g_udp6_protosw, sizeof(g_flow_divert_in6_udp_protosw));
	memcpy(&g_flow_divert_in6_udp_usrreqs, g_udp6_protosw->pr_usrreqs, sizeof(g_flow_divert_in6_udp_usrreqs));

	g_flow_divert_in6_udp_usrreqs.pru_connect = flow_divert_connect_out;
	g_flow_divert_in6_udp_usrreqs.pru_connectx = flow_divert_connectx6_out;
	g_flow_divert_in6_udp_usrreqs.pru_disconnect = flow_divert_close;
	g_flow_divert_in6_udp_usrreqs.pru_disconnectx = flow_divert_disconnectx;
	g_flow_divert_in6_udp_usrreqs.pru_rcvd = flow_divert_rcvd;
	g_flow_divert_in6_udp_usrreqs.pru_send = flow_divert_data_out;
	g_flow_divert_in6_udp_usrreqs.pru_shutdown = flow_divert_shutdown;
	g_flow_divert_in6_udp_usrreqs.pru_sosend_list = pru_sosend_list_notsupp;
	g_flow_divert_in6_udp_usrreqs.pru_soreceive_list = pru_soreceive_list_notsupp;
	g_flow_divert_in6_udp_usrreqs.pru_preconnect = flow_divert_preconnect;

	g_flow_divert_in6_udp_protosw.pr_usrreqs = &g_flow_divert_in6_udp_usrreqs;
	g_flow_divert_in6_udp_protosw.pr_ctloutput = flow_divert_ctloutput;
	/*
	 * Socket filters shouldn't attach/detach to/from this protosw
	 * since pr_protosw is to be used instead, which points to the
	 * real protocol; if they do, it is a bug and we should panic.
	 */
	g_flow_divert_in6_udp_protosw.pr_filter_head.tqh_first =
	    (struct socket_filter *)(uintptr_t)0xdeadbeefdeadbeef;
	g_flow_divert_in6_udp_protosw.pr_filter_head.tqh_last =
	    (struct socket_filter **)(uintptr_t)0xdeadbeefdeadbeef;

	flow_divert_grp_attr = lck_grp_attr_alloc_init();
	if (flow_divert_grp_attr == NULL) {
		FDLOG0(LOG_ERR, &nil_pcb, "lck_grp_attr_alloc_init failed");
		g_init_result = ENOMEM;
		goto done;
	}

	flow_divert_mtx_grp = lck_grp_alloc_init(FLOW_DIVERT_CONTROL_NAME, flow_divert_grp_attr);
	if (flow_divert_mtx_grp == NULL) {
		FDLOG0(LOG_ERR, &nil_pcb, "lck_grp_alloc_init failed");
		g_init_result = ENOMEM;
		goto done;
	}

	flow_divert_mtx_attr = lck_attr_alloc_init();
	if (flow_divert_mtx_attr == NULL) {
		FDLOG0(LOG_ERR, &nil_pcb, "lck_attr_alloc_init failed");
		g_init_result = ENOMEM;
		goto done;
	}

	g_init_result = flow_divert_kctl_init();
	if (g_init_result) {
		goto done;
	}

	lck_rw_init(&g_flow_divert_group_lck, flow_divert_mtx_grp, flow_divert_mtx_attr);

done:
	if (g_init_result != 0) {
		if (flow_divert_mtx_attr != NULL) {
			lck_attr_free(flow_divert_mtx_attr);
			flow_divert_mtx_attr = NULL;
		}
		if (flow_divert_mtx_grp != NULL) {
			lck_grp_free(flow_divert_mtx_grp);
			flow_divert_mtx_grp = NULL;
		}
		if (flow_divert_grp_attr != NULL) {
			lck_grp_attr_free(flow_divert_grp_attr);
			flow_divert_grp_attr = NULL;
		}

		if (g_flow_divert_kctl_ref != NULL) {
			ctl_deregister(g_flow_divert_kctl_ref);
			g_flow_divert_kctl_ref = NULL;
		}
	}
}
