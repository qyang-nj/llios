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
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
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
 * NOTICE: This file was modified by McAfee Research in 2004 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 * Copyright (c) 2005 SPARTA, Inc.
 */
/*
 */
/*
 *	File:	ipc/ipc_kmsg.c
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Operations on kernel messages.
 */


#include <mach/mach_types.h>
#include <mach/boolean.h>
#include <mach/kern_return.h>
#include <mach/message.h>
#include <mach/port.h>
#include <mach/vm_map.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>

#include <kern/kern_types.h>
#include <kern/assert.h>
#include <kern/debug.h>
#include <kern/ipc_kobject.h>
#include <kern/kalloc.h>
#include <kern/zalloc.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/misc_protos.h>
#include <kern/cpu_data.h>
#include <kern/policy_internal.h>
#include <kern/mach_filter.h>

#include <pthread/priority_private.h>

#include <machine/limits.h>

#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_kern.h>

#include <ipc/port.h>
#include <ipc/ipc_types.h>
#include <ipc/ipc_entry.h>
#include <ipc/ipc_kmsg.h>
#include <ipc/ipc_notify.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_right.h>
#include <ipc/ipc_hash.h>
#include <ipc/ipc_table.h>
#include <ipc/ipc_importance.h>
#if MACH_FLIPC
#include <kern/mach_node.h>
#include <ipc/flipc.h>
#endif

#include <os/overflow.h>

#include <security/mac_mach_internal.h>

#include <device/device_server.h>

#include <string.h>

#ifdef ppc
#include <ppc/Firmware.h>
#include <ppc/low_trace.h>
#endif

#if DEBUG
#define DEBUG_MSGS_K64 1
#endif

#include <sys/kdebug.h>
#include <libkern/OSAtomic.h>

#include <libkern/crypto/sha2.h>

#include <ptrauth.h>
#if __has_feature(ptrauth_calls)
#include <libkern/ptrauth_utils.h>
#endif

#pragma pack(4)

typedef struct{
	mach_msg_bits_t       msgh_bits;
	mach_msg_size_t       msgh_size;
	mach_port_name_t      msgh_remote_port;
	mach_port_name_t      msgh_local_port;
	mach_port_name_t      msgh_voucher_port;
	mach_msg_id_t         msgh_id;
} mach_msg_legacy_header_t;

typedef struct{
	mach_msg_legacy_header_t       header;
	mach_msg_body_t         body;
} mach_msg_legacy_base_t;

typedef struct{
	mach_port_name_t                              name;
	mach_msg_size_t                               pad1;
	uint32_t                                              pad2 : 16;
	mach_msg_type_name_t                  disposition : 8;
	mach_msg_descriptor_type_t    type : 8;
} mach_msg_legacy_port_descriptor_t;


typedef union{
	mach_msg_legacy_port_descriptor_t                     port;
	mach_msg_ool_descriptor32_t           out_of_line32;
	mach_msg_ool_ports_descriptor32_t     ool_ports32;
	mach_msg_guarded_port_descriptor32_t  guarded_port32;
	mach_msg_type_descriptor_t                    type;
} mach_msg_legacy_descriptor_t;

#pragma pack()

#define LEGACY_HEADER_SIZE_DELTA ((mach_msg_size_t)(sizeof(mach_msg_header_t) - sizeof(mach_msg_legacy_header_t)))

// END LP64 fixes

#if __has_feature(ptrauth_calls)
typedef uintptr_t ikm_sig_scratch_t;

static void
ikm_init_sig(
	__unused ipc_kmsg_t kmsg,
	ikm_sig_scratch_t *scratchp)
{
	*scratchp = OS_PTRAUTH_DISCRIMINATOR("kmsg.ikm_signature");
}

static void
ikm_chunk_sig(
	ipc_kmsg_t kmsg,
	void *data,
	size_t len,
	ikm_sig_scratch_t *scratchp)
{
	int ptrauth_flags;
	void *trailerp;

	/*
	 * if we happen to be doing the trailer chunk,
	 * diversify with the ptrauth-ed trailer pointer -
	 * as that is unchanging for the kmsg
	 */
	trailerp = (void *)
	    ((vm_offset_t)kmsg->ikm_header +
	    mach_round_msg(kmsg->ikm_header->msgh_size));

	ptrauth_flags = (data == trailerp) ? PTRAUTH_ADDR_DIVERSIFY : 0;
	*scratchp = ptrauth_utils_sign_blob_generic(data, len, *scratchp, ptrauth_flags);
}

static uintptr_t
ikm_finalize_sig(
	__unused ipc_kmsg_t kmsg,
	ikm_sig_scratch_t *scratchp)
{
	return *scratchp;
}

#elif defined(CRYPTO_SHA2) && !defined(__x86_64__) && !defined(__arm__)

typedef SHA256_CTX ikm_sig_scratch_t;

static void
ikm_init_sig(
	__unused ipc_kmsg_t kmsg,
	ikm_sig_scratch_t *scratchp)
{
	SHA256_Init(scratchp);
	SHA256_Update(scratchp, &vm_kernel_addrhash_salt_ext, sizeof(uint64_t));
}

static void
ikm_chunk_sig(
	__unused ipc_kmsg_t kmsg,
	void *data,
	size_t len,
	ikm_sig_scratch_t *scratchp)
{
	SHA256_Update(scratchp, data, len);
}

static uintptr_t
ikm_finalize_sig(
	__unused ipc_kmsg_t kmsg,
	ikm_sig_scratch_t *scratchp)
{
	uintptr_t sha_digest[SHA256_DIGEST_LENGTH / sizeof(uintptr_t)];

	SHA256_Final((uint8_t *)sha_digest, scratchp);

	/*
	 * Only use one uintptr_t sized part of result for space and compat reasons.
	 * Truncation is better than XOR'ing the chunks together in hopes of higher
	 * entropy - because of its lower risk of collisions.
	 */
	return *sha_digest;
}

#else
/* Stubbed out implementation (for __x86_64__, __arm__ for now) */

typedef uintptr_t ikm_sig_scratch_t;

static void
ikm_init_sig(
	__unused ipc_kmsg_t kmsg,
	ikm_sig_scratch_t *scratchp)
{
	*scratchp = 0;
}

static void
ikm_chunk_sig(
	__unused ipc_kmsg_t kmsg,
	__unused void *data,
	__unused size_t len,
	__unused ikm_sig_scratch_t *scratchp)
{
	return;
}

static uintptr_t
ikm_finalize_sig(
	__unused ipc_kmsg_t kmsg,
	ikm_sig_scratch_t *scratchp)
{
	return *scratchp;
}

#endif

static void
ikm_header_sig(
	ipc_kmsg_t kmsg,
	ikm_sig_scratch_t *scratchp)
{
	mach_msg_size_t dsc_count;
	mach_msg_base_t base;
	boolean_t complex;

	/* take a snapshot of the message header/body-count */
	base.header = *kmsg->ikm_header;
	complex = ((base.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0);
	if (complex) {
		dsc_count = ((mach_msg_body_t *)(kmsg->ikm_header + 1))->msgh_descriptor_count;
	} else {
		dsc_count = 0;
	}
	base.body.msgh_descriptor_count = dsc_count;

	/* compute sig of a copy of the header with all varying bits masked off */
	base.header.msgh_bits &= MACH_MSGH_BITS_USER;
	base.header.msgh_bits &= ~MACH_MSGH_BITS_VOUCHER_MASK;
	ikm_chunk_sig(kmsg, &base, sizeof(mach_msg_base_t), scratchp);
}

static void
ikm_trailer_sig(
	ipc_kmsg_t kmsg,
	ikm_sig_scratch_t *scratchp)
{
	mach_msg_max_trailer_t *trailerp;

	/* Add sig of the trailer contents */
	trailerp = (mach_msg_max_trailer_t *)
	    ((vm_offset_t)kmsg->ikm_header +
	    mach_round_msg(kmsg->ikm_header->msgh_size));
	ikm_chunk_sig(kmsg, trailerp, sizeof(*trailerp), scratchp);
}

/* Compute the signature for the body bits of a message */
static void
ikm_body_sig(
	ipc_kmsg_t        kmsg,
	ikm_sig_scratch_t *scratchp)
{
	mach_msg_descriptor_t *kern_dsc;
	mach_msg_size_t dsc_count;
	mach_msg_body_t *body;
	mach_msg_size_t i;

	if ((kmsg->ikm_header->msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0) {
		return;
	}
	body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
	dsc_count = body->msgh_descriptor_count;

	if (dsc_count == 0) {
		return;
	}

	kern_dsc = (mach_msg_descriptor_t *) (body + 1);

	/* Compute the signature for the whole descriptor array */
	ikm_chunk_sig(kmsg, kern_dsc, sizeof(*kern_dsc) * dsc_count, scratchp);

	/* look for descriptor contents that need a signature */
	for (i = 0; i < dsc_count; i++) {
		switch (kern_dsc[i].type.type) {
		case MACH_MSG_PORT_DESCRIPTOR:
		case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
		case MACH_MSG_OOL_DESCRIPTOR:
			break;

		case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
			mach_msg_ool_ports_descriptor_t *ports_dsc;

			/* Compute sig for the port/object pointers */
			ports_dsc = (mach_msg_ool_ports_descriptor_t *)&kern_dsc[i];
			ikm_chunk_sig(kmsg, ports_dsc->address, ports_dsc->count * sizeof(ipc_object_t), scratchp);
			break;
		}
		default: {
			panic("ipc_kmsg_body_sig: invalid message descriptor");
		}
		}
	}
}

static void
ikm_sign(
	ipc_kmsg_t kmsg)
{
	ikm_sig_scratch_t scratch;
	uintptr_t sig;

	zone_require(ipc_kmsg_zone, kmsg);

	ikm_init_sig(kmsg, &scratch);

	ikm_header_sig(kmsg, &scratch);
#if IKM_PARTIAL_SIG
	/* save off partial signature for just header */
	sig = ikm_finalize_sig(kmsg, &scratch);
	kmsg->ikm_header_sig = sig;
#endif

	ikm_trailer_sig(kmsg, &scratch);
#if IKM_PARTIAL_SIG
	/* save off partial signature for header+trailer */
	sig = ikm_finalize_sig(kmsg, &scratch);
	kmsg->ikm_headtrail_sig = sig;
#endif

	ikm_body_sig(kmsg, &scratch);
	sig = ikm_finalize_sig(kmsg, &scratch);
	kmsg->ikm_signature = sig;
}

unsigned int ikm_signature_failures;
unsigned int ikm_signature_failure_id;
#if (DEVELOPMENT || DEBUG)
unsigned int ikm_signature_panic_disable;
unsigned int ikm_signature_header_failures;
unsigned int ikm_signature_trailer_failures;
#endif

static void
ikm_validate_sig(
	ipc_kmsg_t kmsg)
{
	ikm_sig_scratch_t scratch;
	uintptr_t expected;
	uintptr_t sig;
	char *str;

	zone_require(ipc_kmsg_zone, kmsg);

	ikm_init_sig(kmsg, &scratch);

	ikm_header_sig(kmsg, &scratch);
#if IKM_PARTIAL_SIG
	/* Do partial evaluation of just the header signature */
	sig = ikm_finalize_sig(kmsg, &scratch);
	expected = kmsg->ikm_header_sig;
	if (sig != expected) {
		ikm_signature_header_failures++;
		str = "header";
		goto failure;
	}
#endif

	ikm_trailer_sig(kmsg, &scratch);
#if IKM_PARTIAL_SIG
	/* Do partial evaluation of header+trailer signature */
	sig = ikm_finalize_sig(kmsg, &scratch);
	expected = kmsg->ikm_headtrail_sig;
	if (sig != expected) {
		ikm_signature_trailer_failures++;
		str = "trailer";
		goto failure;
	}
#endif

	ikm_body_sig(kmsg, &scratch);
	sig = ikm_finalize_sig(kmsg, &scratch);

	expected = kmsg->ikm_signature;
	if (sig != expected) {
		ikm_signature_failures++;
		str = "full";

#if IKM_PARTIAL_SIG
failure:
#endif
		{
			mach_msg_id_t id = kmsg->ikm_header->msgh_id;

			ikm_signature_failure_id = id;
#if (DEVELOPMENT || DEBUG)
			if (ikm_signature_panic_disable) {
				return;
			}
#endif
			panic("ikm_validate_sig: %s signature mismatch: kmsg=0x%p, id=%d, sig=0x%zx (expected 0x%zx)",
			    str, kmsg, id, sig, expected);
		}
	}
}

#if DEBUG_MSGS_K64
extern void ipc_pset_print64(
	ipc_pset_t      pset);

extern void     ipc_kmsg_print64(
	ipc_kmsg_t      kmsg,
	const char      *str);

extern void     ipc_msg_print64(
	mach_msg_header_t       *msgh);

extern ipc_port_t ipc_name_to_data64(
	task_t                  task,
	mach_port_name_t        name);

/*
 * Forward declarations
 */
void ipc_msg_print_untyped64(
	mach_msg_body_t         *body);

const char * ipc_type_name64(
	int             type_name,
	boolean_t       received);

void ipc_print_type_name64(
	int     type_name);

const char *
msgh_bit_decode64(
	mach_msg_bits_t bit);

const char *
mm_copy_options_string64(
	mach_msg_copy_options_t option);

void db_print_msg_uid64(mach_msg_header_t *);

static void
ipc_msg_body_print64(void *body, int size)
{
	uint32_t        *word = (uint32_t *) body;
	uint32_t        *end  = (uint32_t *)(((uintptr_t) body) + size
	    - sizeof(mach_msg_header_t));
	int             i;

	kprintf("  body(%p-%p):\n    %p: ", body, end, word);
	for (;;) {
		for (i = 0; i < 8; i++, word++) {
			if (word >= end) {
				kprintf("\n");
				return;
			}
			kprintf("%08x ", *word);
		}
		kprintf("\n    %p: ", word);
	}
}


const char *
ipc_type_name64(
	int             type_name,
	boolean_t       received)
{
	switch (type_name) {
	case MACH_MSG_TYPE_PORT_NAME:
		return "port_name";

	case MACH_MSG_TYPE_MOVE_RECEIVE:
		if (received) {
			return "port_receive";
		} else {
			return "move_receive";
		}

	case MACH_MSG_TYPE_MOVE_SEND:
		if (received) {
			return "port_send";
		} else {
			return "move_send";
		}

	case MACH_MSG_TYPE_MOVE_SEND_ONCE:
		if (received) {
			return "port_send_once";
		} else {
			return "move_send_once";
		}

	case MACH_MSG_TYPE_COPY_SEND:
		return "copy_send";

	case MACH_MSG_TYPE_MAKE_SEND:
		return "make_send";

	case MACH_MSG_TYPE_MAKE_SEND_ONCE:
		return "make_send_once";

	default:
		return (char *) 0;
	}
}

void
ipc_print_type_name64(
	int     type_name)
{
	const char *name = ipc_type_name64(type_name, TRUE);
	if (name) {
		kprintf("%s", name);
	} else {
		kprintf("type%d", type_name);
	}
}

/*
 * ipc_kmsg_print64	[ debug ]
 */
void
ipc_kmsg_print64(
	ipc_kmsg_t      kmsg,
	const char      *str)
{
	kprintf("%s kmsg=%p:\n", str, kmsg);
	kprintf("  next=%p, prev=%p, size=%d",
	    kmsg->ikm_next,
	    kmsg->ikm_prev,
	    kmsg->ikm_size);
	kprintf("\n");
	ipc_msg_print64(kmsg->ikm_header);
}

const char *
msgh_bit_decode64(
	mach_msg_bits_t bit)
{
	switch (bit) {
	case MACH_MSGH_BITS_COMPLEX:        return "complex";
	case MACH_MSGH_BITS_CIRCULAR:       return "circular";
	default:                            return (char *) 0;
	}
}

/*
 * ipc_msg_print64	[ debug ]
 */
void
ipc_msg_print64(
	mach_msg_header_t       *msgh)
{
	mach_msg_bits_t mbits;
	unsigned int    bit, i;
	const char      *bit_name;
	int             needs_comma;

	mbits = msgh->msgh_bits;
	kprintf("  msgh_bits=0x%x: l=0x%x,r=0x%x\n",
	    mbits,
	    MACH_MSGH_BITS_LOCAL(msgh->msgh_bits),
	    MACH_MSGH_BITS_REMOTE(msgh->msgh_bits));

	mbits = MACH_MSGH_BITS_OTHER(mbits) & MACH_MSGH_BITS_USED;
	kprintf("  decoded bits:  ");
	needs_comma = 0;
	for (i = 0, bit = 1; i < sizeof(mbits) * 8; ++i, bit <<= 1) {
		if ((mbits & bit) == 0) {
			continue;
		}
		bit_name = msgh_bit_decode64((mach_msg_bits_t)bit);
		if (bit_name) {
			kprintf("%s%s", needs_comma ? "," : "", bit_name);
		} else {
			kprintf("%sunknown(0x%x),", needs_comma ? "," : "", bit);
		}
		++needs_comma;
	}
	if (msgh->msgh_bits & ~MACH_MSGH_BITS_USED) {
		kprintf("%sunused=0x%x,", needs_comma ? "," : "",
		    msgh->msgh_bits & ~MACH_MSGH_BITS_USED);
	}
	kprintf("\n");

	needs_comma = 1;
	if (msgh->msgh_remote_port) {
		kprintf("  remote=%p(", msgh->msgh_remote_port);
		ipc_print_type_name64(MACH_MSGH_BITS_REMOTE(msgh->msgh_bits));
		kprintf(")");
	} else {
		kprintf("  remote=null");
	}

	if (msgh->msgh_local_port) {
		kprintf("%slocal=%p(", needs_comma ? "," : "",
		    msgh->msgh_local_port);
		ipc_print_type_name64(MACH_MSGH_BITS_LOCAL(msgh->msgh_bits));
		kprintf(")\n");
	} else {
		kprintf("local=null\n");
	}

	kprintf("  msgh_id=%d, size=%d\n",
	    msgh->msgh_id,
	    msgh->msgh_size);

	if (mbits & MACH_MSGH_BITS_COMPLEX) {
		ipc_msg_print_untyped64((mach_msg_body_t *) (msgh + 1));
	}

	ipc_msg_body_print64((void *)(msgh + 1), msgh->msgh_size);
}


const char *
mm_copy_options_string64(
	mach_msg_copy_options_t option)
{
	const char      *name;

	switch (option) {
	case MACH_MSG_PHYSICAL_COPY:
		name = "PHYSICAL";
		break;
	case MACH_MSG_VIRTUAL_COPY:
		name = "VIRTUAL";
		break;
	case MACH_MSG_OVERWRITE:
		name = "OVERWRITE(DEPRECATED)";
		break;
	case MACH_MSG_ALLOCATE:
		name = "ALLOCATE";
		break;
	case MACH_MSG_KALLOC_COPY_T:
		name = "KALLOC_COPY_T";
		break;
	default:
		name = "unknown";
		break;
	}
	return name;
}

void
ipc_msg_print_untyped64(
	mach_msg_body_t         *body)
{
	mach_msg_descriptor_t       *saddr, *send;
	mach_msg_descriptor_type_t  type;

	kprintf("  %d descriptors: \n", body->msgh_descriptor_count);

	saddr = (mach_msg_descriptor_t *) (body + 1);
	send = saddr + body->msgh_descriptor_count;

	for (; saddr < send; saddr++) {
		type = saddr->type.type;

		switch (type) {
		case MACH_MSG_PORT_DESCRIPTOR: {
			mach_msg_port_descriptor_t *dsc;

			dsc = &saddr->port;
			kprintf("    PORT name = %p disp = ", dsc->name);
			ipc_print_type_name64(dsc->disposition);
			kprintf("\n");
			break;
		}
		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
		case MACH_MSG_OOL_DESCRIPTOR: {
			mach_msg_ool_descriptor_t *dsc;

			dsc = (mach_msg_ool_descriptor_t *) &saddr->out_of_line;
			kprintf("    OOL%s addr = %p size = 0x%x copy = %s %s\n",
			    type == MACH_MSG_OOL_DESCRIPTOR ? "" : " VOLATILE",
			    dsc->address, dsc->size,
			    mm_copy_options_string64(dsc->copy),
			    dsc->deallocate ? "DEALLOC" : "");
			break;
		}
		case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
			mach_msg_ool_ports_descriptor_t *dsc;

			dsc = (mach_msg_ool_ports_descriptor_t *) &saddr->ool_ports;

			kprintf("    OOL_PORTS addr = %p count = 0x%x ",
			    dsc->address, dsc->count);
			kprintf("disp = ");
			ipc_print_type_name64(dsc->disposition);
			kprintf(" copy = %s %s\n",
			    mm_copy_options_string64(dsc->copy),
			    dsc->deallocate ? "DEALLOC" : "");
			break;
		}
		case MACH_MSG_GUARDED_PORT_DESCRIPTOR: {
			mach_msg_guarded_port_descriptor_t *dsc;

			dsc = (mach_msg_guarded_port_descriptor_t *)&saddr->guarded_port;
			kprintf("    GUARDED_PORT name = %p flags = 0x%x disp = ", dsc->name, dsc->flags);
			ipc_print_type_name64(dsc->disposition);
			kprintf("\n");
			break;
		}
		default: {
			kprintf("    UNKNOWN DESCRIPTOR 0x%x\n", type);
			break;
		}
		}
	}
}

#define DEBUG_IPC_KMSG_PRINT(kmsg, string)       \
	__unreachable_ok_push   \
	if (DEBUG_KPRINT_SYSCALL_PREDICATE(DEBUG_KPRINT_SYSCALL_IPC_MASK)) {    \
	        ipc_kmsg_print64(kmsg, string); \
	}       \
	__unreachable_ok_pop

#define DEBUG_IPC_MSG_BODY_PRINT(body, size)     \
	__unreachable_ok_push   \
	if (DEBUG_KPRINT_SYSCALL_PREDICATE(DEBUG_KPRINT_SYSCALL_IPC_MASK)) {    \
	        ipc_msg_body_print64(body,size);\
	}       \
	__unreachable_ok_pop
#else /* !DEBUG_MSGS_K64 */
#define DEBUG_IPC_KMSG_PRINT(kmsg, string)
#define DEBUG_IPC_MSG_BODY_PRINT(body, size)
#endif  /* !DEBUG_MSGS_K64 */

extern vm_map_t         ipc_kernel_copy_map;
extern vm_size_t        ipc_kmsg_max_space;
extern const vm_size_t  ipc_kmsg_max_vm_space;
extern const vm_size_t  ipc_kmsg_max_body_space;
extern vm_size_t        msg_ool_size_small;

#define MSG_OOL_SIZE_SMALL      msg_ool_size_small

#if defined(__LP64__)
#define MAP_SIZE_DIFFERS(map)   (map->max_offset < MACH_VM_MAX_ADDRESS)
#define OTHER_OOL_DESCRIPTOR    mach_msg_ool_descriptor32_t
#define OTHER_OOL_PORTS_DESCRIPTOR      mach_msg_ool_ports_descriptor32_t
#else
#define MAP_SIZE_DIFFERS(map)   (map->max_offset > VM_MAX_ADDRESS)
#define OTHER_OOL_DESCRIPTOR    mach_msg_ool_descriptor64_t
#define OTHER_OOL_PORTS_DESCRIPTOR      mach_msg_ool_ports_descriptor64_t
#endif

#define DESC_SIZE_ADJUSTMENT    ((mach_msg_size_t)(sizeof(mach_msg_ool_descriptor64_t) - \
	                         sizeof(mach_msg_ool_descriptor32_t)))

/* scatter list macros */

#define SKIP_PORT_DESCRIPTORS(s, c)                                     \
MACRO_BEGIN                                                             \
	if ((s) != MACH_MSG_DESCRIPTOR_NULL) {                          \
	        while ((c) > 0) {                                       \
	                if ((s)->type.type != MACH_MSG_PORT_DESCRIPTOR) \
	                        break;                                  \
	                (s)++; (c)--;                                   \
	        }                                                       \
	        if (c == 0)                                             \
	                (s) = MACH_MSG_DESCRIPTOR_NULL;                 \
	}                                                               \
MACRO_END

#define INCREMENT_SCATTER(s, c, d)                                      \
MACRO_BEGIN                                                             \
	if ((s) != MACH_MSG_DESCRIPTOR_NULL) {                          \
	    s = (d) ? (mach_msg_descriptor_t *)                         \
	        ((OTHER_OOL_DESCRIPTOR *)(s) + 1) :                     \
	        (s + 1);                                                \
	        (c)--;                                                  \
	}                                                               \
MACRO_END

#define KMSG_TRACE_FLAG_TRACED     0x000001
#define KMSG_TRACE_FLAG_COMPLEX    0x000002
#define KMSG_TRACE_FLAG_OOLMEM     0x000004
#define KMSG_TRACE_FLAG_VCPY       0x000008
#define KMSG_TRACE_FLAG_PCPY       0x000010
#define KMSG_TRACE_FLAG_SND64      0x000020
#define KMSG_TRACE_FLAG_RAISEIMP   0x000040
#define KMSG_TRACE_FLAG_APP_SRC    0x000080
#define KMSG_TRACE_FLAG_APP_DST    0x000100
#define KMSG_TRACE_FLAG_DAEMON_SRC 0x000200
#define KMSG_TRACE_FLAG_DAEMON_DST 0x000400
#define KMSG_TRACE_FLAG_DST_NDFLTQ 0x000800
#define KMSG_TRACE_FLAG_SRC_NDFLTQ 0x001000
#define KMSG_TRACE_FLAG_DST_SONCE  0x002000
#define KMSG_TRACE_FLAG_SRC_SONCE  0x004000
#define KMSG_TRACE_FLAG_CHECKIN    0x008000
#define KMSG_TRACE_FLAG_ONEWAY     0x010000
#define KMSG_TRACE_FLAG_IOKIT      0x020000
#define KMSG_TRACE_FLAG_SNDRCV     0x040000
#define KMSG_TRACE_FLAG_DSTQFULL   0x080000
#define KMSG_TRACE_FLAG_VOUCHER    0x100000
#define KMSG_TRACE_FLAG_TIMER      0x200000
#define KMSG_TRACE_FLAG_SEMA       0x400000
#define KMSG_TRACE_FLAG_DTMPOWNER  0x800000
#define KMSG_TRACE_FLAG_GUARDED_DESC 0x1000000

#define KMSG_TRACE_FLAGS_MASK      0x1ffffff
#define KMSG_TRACE_FLAGS_SHIFT     8

#define KMSG_TRACE_PORTS_MASK      0xff
#define KMSG_TRACE_PORTS_SHIFT     0

#if (KDEBUG_LEVEL >= KDEBUG_LEVEL_STANDARD)
#include <stdint.h>

void
ipc_kmsg_trace_send(ipc_kmsg_t kmsg,
    mach_msg_option_t option)
{
	task_t send_task = TASK_NULL;
	ipc_port_t dst_port, src_port;
	boolean_t is_task_64bit;
	mach_msg_header_t *msg;
	mach_msg_trailer_t *trailer;

	int kotype = 0;
	uint32_t msg_size = 0;
	uint64_t msg_flags = KMSG_TRACE_FLAG_TRACED;
	uint32_t num_ports = 0;
	uint32_t send_pid, dst_pid;

	/*
	 * check to see not only if ktracing is enabled, but if we will
	 * _actually_ emit the KMSG_INFO tracepoint. This saves us a
	 * significant amount of processing (and a port lock hold) in
	 * the non-tracing case.
	 */
	if (__probable((kdebug_enable & KDEBUG_TRACE) == 0)) {
		return;
	}
	if (!kdebug_debugid_enabled(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_INFO))) {
		return;
	}

	msg = kmsg->ikm_header;

	dst_port = msg->msgh_remote_port;
	if (!IPC_PORT_VALID(dst_port)) {
		return;
	}

	/*
	 * Message properties / options
	 */
	if ((option & (MACH_SEND_MSG | MACH_RCV_MSG)) == (MACH_SEND_MSG | MACH_RCV_MSG)) {
		msg_flags |= KMSG_TRACE_FLAG_SNDRCV;
	}

	if (msg->msgh_id >= is_iokit_subsystem.start &&
	    msg->msgh_id < is_iokit_subsystem.end + 100) {
		msg_flags |= KMSG_TRACE_FLAG_IOKIT;
	}
	/* magic XPC checkin message id (XPC_MESSAGE_ID_CHECKIN) from libxpc */
	else if (msg->msgh_id == 0x77303074u /* w00t */) {
		msg_flags |= KMSG_TRACE_FLAG_CHECKIN;
	}

	if (msg->msgh_bits & MACH_MSGH_BITS_RAISEIMP) {
		msg_flags |= KMSG_TRACE_FLAG_RAISEIMP;
	}

	if (unsafe_convert_port_to_voucher(kmsg->ikm_voucher)) {
		msg_flags |= KMSG_TRACE_FLAG_VOUCHER;
	}

	/*
	 * Sending task / port
	 */
	send_task = current_task();
	send_pid = task_pid(send_task);

	if (send_pid != 0) {
		if (task_is_daemon(send_task)) {
			msg_flags |= KMSG_TRACE_FLAG_DAEMON_SRC;
		} else if (task_is_app(send_task)) {
			msg_flags |= KMSG_TRACE_FLAG_APP_SRC;
		}
	}

	is_task_64bit = (send_task->map->max_offset > VM_MAX_ADDRESS);
	if (is_task_64bit) {
		msg_flags |= KMSG_TRACE_FLAG_SND64;
	}

	src_port = msg->msgh_local_port;
	if (src_port) {
		if (src_port->ip_messages.imq_qlimit != MACH_PORT_QLIMIT_DEFAULT) {
			msg_flags |= KMSG_TRACE_FLAG_SRC_NDFLTQ;
		}
		switch (MACH_MSGH_BITS_LOCAL(msg->msgh_bits)) {
		case MACH_MSG_TYPE_MOVE_SEND_ONCE:
			msg_flags |= KMSG_TRACE_FLAG_SRC_SONCE;
			break;
		default:
			break;
		}
	} else {
		msg_flags |= KMSG_TRACE_FLAG_ONEWAY;
	}


	/*
	 * Destination task / port
	 */
	ip_lock(dst_port);
	if (!ip_active(dst_port)) {
		/* dst port is being torn down */
		dst_pid = (uint32_t)0xfffffff0;
	} else if (dst_port->ip_tempowner) {
		msg_flags |= KMSG_TRACE_FLAG_DTMPOWNER;
		if (IIT_NULL != dst_port->ip_imp_task) {
			dst_pid = task_pid(dst_port->ip_imp_task->iit_task);
		} else {
			dst_pid = (uint32_t)0xfffffff1;
		}
	} else if (dst_port->ip_receiver_name == MACH_PORT_NULL) {
		/* dst_port is otherwise in-transit */
		dst_pid = (uint32_t)0xfffffff2;
	} else {
		if (dst_port->ip_receiver == ipc_space_kernel) {
			dst_pid = 0;
		} else {
			ipc_space_t dst_space;
			dst_space = dst_port->ip_receiver;
			if (dst_space && is_active(dst_space)) {
				dst_pid = task_pid(dst_space->is_task);
				if (task_is_daemon(dst_space->is_task)) {
					msg_flags |= KMSG_TRACE_FLAG_DAEMON_DST;
				} else if (task_is_app(dst_space->is_task)) {
					msg_flags |= KMSG_TRACE_FLAG_APP_DST;
				}
			} else {
				/* receiving task is being torn down */
				dst_pid = (uint32_t)0xfffffff3;
			}
		}
	}

	if (dst_port->ip_messages.imq_qlimit != MACH_PORT_QLIMIT_DEFAULT) {
		msg_flags |= KMSG_TRACE_FLAG_DST_NDFLTQ;
	}
	if (imq_full(&dst_port->ip_messages)) {
		msg_flags |= KMSG_TRACE_FLAG_DSTQFULL;
	}

	kotype = ip_kotype(dst_port);

	ip_unlock(dst_port);

	switch (kotype) {
	case IKOT_SEMAPHORE:
		msg_flags |= KMSG_TRACE_FLAG_SEMA;
		break;
	case IKOT_TIMER:
	case IKOT_CLOCK:
		msg_flags |= KMSG_TRACE_FLAG_TIMER;
		break;
	case IKOT_MASTER_DEVICE:
	case IKOT_IOKIT_CONNECT:
	case IKOT_IOKIT_OBJECT:
	case IKOT_IOKIT_IDENT:
	case IKOT_UEXT_OBJECT:
		msg_flags |= KMSG_TRACE_FLAG_IOKIT;
		break;
	default:
		break;
	}

	switch (MACH_MSGH_BITS_REMOTE(msg->msgh_bits)) {
	case MACH_MSG_TYPE_PORT_SEND_ONCE:
		msg_flags |= KMSG_TRACE_FLAG_DST_SONCE;
		break;
	default:
		break;
	}


	/*
	 * Message size / content
	 */
	msg_size = msg->msgh_size - sizeof(mach_msg_header_t);

	if (msg->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
		mach_msg_body_t *msg_body;
		mach_msg_descriptor_t *kern_dsc;
		int dsc_count;

		msg_flags |= KMSG_TRACE_FLAG_COMPLEX;

		msg_body = (mach_msg_body_t *)(kmsg->ikm_header + 1);
		dsc_count = (int)msg_body->msgh_descriptor_count;
		kern_dsc = (mach_msg_descriptor_t *)(msg_body + 1);

		/* this is gross: see ipc_kmsg_copyin_body()... */
		if (!is_task_64bit) {
			msg_size -= (dsc_count * 12);
		}

		for (int i = 0; i < dsc_count; i++) {
			switch (kern_dsc[i].type.type) {
			case MACH_MSG_PORT_DESCRIPTOR:
				num_ports++;
				if (is_task_64bit) {
					msg_size -= 12;
				}
				break;
			case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
			case MACH_MSG_OOL_DESCRIPTOR: {
				mach_msg_ool_descriptor_t *dsc;
				dsc = (mach_msg_ool_descriptor_t *)&kern_dsc[i];
				msg_flags |= KMSG_TRACE_FLAG_OOLMEM;
				msg_size += dsc->size;
				if ((dsc->size >= MSG_OOL_SIZE_SMALL) &&
				    (dsc->copy == MACH_MSG_PHYSICAL_COPY) &&
				    !dsc->deallocate) {
					msg_flags |= KMSG_TRACE_FLAG_PCPY;
				} else if (dsc->size <= MSG_OOL_SIZE_SMALL) {
					msg_flags |= KMSG_TRACE_FLAG_PCPY;
				} else {
					msg_flags |= KMSG_TRACE_FLAG_VCPY;
				}
				if (is_task_64bit) {
					msg_size -= 16;
				}
			} break;
			case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
				mach_msg_ool_ports_descriptor_t *dsc;
				dsc = (mach_msg_ool_ports_descriptor_t *)&kern_dsc[i];
				num_ports += dsc->count;
				if (is_task_64bit) {
					msg_size -= 16;
				}
			} break;
			case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
				num_ports++;
				msg_flags |= KMSG_TRACE_FLAG_GUARDED_DESC;
				if (is_task_64bit) {
					msg_size -= 16;
				}
				break;
			default:
				break;
			}
		}
	}

	/*
	 * Trailer contents
	 */
	trailer = (mach_msg_trailer_t *)((vm_offset_t)msg +
	    (vm_offset_t)mach_round_msg(msg->msgh_size));
	if (trailer->msgh_trailer_size <= sizeof(mach_msg_security_trailer_t)) {
		extern const security_token_t KERNEL_SECURITY_TOKEN;
		mach_msg_security_trailer_t *strailer;
		strailer = (mach_msg_security_trailer_t *)trailer;
		/*
		 * verify the sender PID: replies from the kernel often look
		 * like self-talk because the sending port is not reset.
		 */
		if (memcmp(&strailer->msgh_sender,
		    &KERNEL_SECURITY_TOKEN,
		    sizeof(KERNEL_SECURITY_TOKEN)) == 0) {
			send_pid = 0;
			msg_flags &= ~(KMSG_TRACE_FLAG_APP_SRC | KMSG_TRACE_FLAG_DAEMON_SRC);
		}
	}

	KDBG(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_INFO) | DBG_FUNC_END,
	    (uintptr_t)send_pid,
	    (uintptr_t)dst_pid,
	    (uintptr_t)msg_size,
	    (uintptr_t)(
		    ((msg_flags & KMSG_TRACE_FLAGS_MASK) << KMSG_TRACE_FLAGS_SHIFT) |
		    ((num_ports & KMSG_TRACE_PORTS_MASK) << KMSG_TRACE_PORTS_SHIFT)
		    )
	    );
}
#endif

/* zone for cached ipc_kmsg_t structures */
ZONE_DECLARE(ipc_kmsg_zone, "ipc kmsgs", IKM_SAVED_KMSG_SIZE,
    ZC_CACHING | ZC_ZFREE_CLEARMEM);
static TUNABLE(bool, enforce_strict_reply, "ipc_strict_reply", false);

/*
 * Forward declarations
 */

void ipc_kmsg_clean(
	ipc_kmsg_t      kmsg);

void ipc_kmsg_clean_body(
	ipc_kmsg_t      kmsg,
	mach_msg_type_number_t  number,
	mach_msg_descriptor_t   *desc);

void ipc_kmsg_clean_partial(
	ipc_kmsg_t              kmsg,
	mach_msg_type_number_t  number,
	mach_msg_descriptor_t   *desc,
	vm_offset_t             paddr,
	vm_size_t               length);

mach_msg_return_t ipc_kmsg_copyin_body(
	ipc_kmsg_t          kmsg,
	ipc_space_t         space,
	vm_map_t            map,
	mach_msg_option_t   *optionp);


static void
ipc_kmsg_link_reply_context_locked(
	ipc_port_t reply_port,
	ipc_port_t voucher_port);

static kern_return_t
ipc_kmsg_validate_reply_port_locked(
	ipc_port_t reply_port,
	mach_msg_option_t options);

static mach_msg_return_t
ipc_kmsg_validate_reply_context_locked(
	mach_msg_option_t option,
	ipc_port_t dest_port,
	ipc_voucher_t voucher,
	mach_port_name_t voucher_name);

/* we can't include the BSD <sys/persona.h> header here... */
#ifndef PERSONA_ID_NONE
#define PERSONA_ID_NONE ((uint32_t)-1)
#endif

/*
 *	We keep a per-processor cache of kernel message buffers.
 *	The cache saves the overhead/locking of using kalloc/kfree.
 *	The per-processor cache seems to miss less than a per-thread cache,
 *	and it also uses less memory.  Access to the cache doesn't
 *	require locking.
 */

/*
 *	Routine:	ikm_set_header
 *	Purpose:
 *		Set the header (and data) pointers for a message. If the
 *      message is small, the data pointer is NULL and all the
 *      data resides within the fixed
 *		the cache, that is best.  Otherwise, allocate a new one.
 *	Conditions:
 *		Nothing locked.
 */
static void
ikm_set_header(
	ipc_kmsg_t kmsg,
	void *data,
	mach_msg_size_t mtsize)
{
	if (data) {
		kmsg->ikm_data = data;
		kmsg->ikm_header = (mach_msg_header_t *)(data + kmsg->ikm_size - mtsize);
	} else {
		assert(kmsg->ikm_size == IKM_SAVED_MSG_SIZE);
		kmsg->ikm_header = (mach_msg_header_t *)
		    ((vm_offset_t)(kmsg + 1) + kmsg->ikm_size - mtsize);
	}
}

/*
 *	Routine:	ipc_kmsg_alloc
 *	Purpose:
 *		Allocate a kernel message structure.  If we can get one from
 *		the cache, that is best.  Otherwise, allocate a new one.
 *	Conditions:
 *		Nothing locked.
 */
ipc_kmsg_t
ipc_kmsg_alloc(
	mach_msg_size_t msg_and_trailer_size)
{
	mach_msg_size_t max_expanded_size;
	ipc_kmsg_t kmsg;
	void *data;

	/*
	 * LP64support -
	 * Pad the allocation in case we need to expand the
	 * message descriptors for user spaces with pointers larger than
	 * the kernel's own, or vice versa.  We don't know how many descriptors
	 * there are yet, so just assume the whole body could be
	 * descriptors (if there could be any at all).
	 *
	 * The expansion space is left in front of the header,
	 * because it is easier to pull the header and descriptors
	 * forward as we process them than it is to push all the
	 * data backwards.
	 */
	mach_msg_size_t size = msg_and_trailer_size - MAX_TRAILER_SIZE;

	/* compare against implementation upper limit for the body */
	if (size > ipc_kmsg_max_body_space) {
		return IKM_NULL;
	}

	if (size > sizeof(mach_msg_base_t)) {
		mach_msg_size_t max_desc = (mach_msg_size_t)(((size - sizeof(mach_msg_base_t)) /
		    sizeof(mach_msg_ool_descriptor32_t)) *
		    DESC_SIZE_ADJUSTMENT);

		/* make sure expansion won't cause wrap */
		if (msg_and_trailer_size > MACH_MSG_SIZE_MAX - max_desc) {
			return IKM_NULL;
		}

		max_expanded_size = msg_and_trailer_size + max_desc;
	} else {
		max_expanded_size = msg_and_trailer_size;
	}

	if (max_expanded_size > IKM_SAVED_MSG_SIZE) {
		data = kheap_alloc(KHEAP_DATA_BUFFERS, max_expanded_size, Z_WAITOK);
		if (data == NULL) {
			return IKM_NULL;
		}
	} else {
		data = NULL;
		max_expanded_size = IKM_SAVED_MSG_SIZE;
	}

	kmsg = zalloc_flags(ipc_kmsg_zone, Z_WAITOK | Z_ZERO | Z_NOFAIL);
	kmsg->ikm_size = max_expanded_size;
	ikm_qos_init(kmsg);
	ikm_set_header(kmsg, data, msg_and_trailer_size);
	assert((kmsg->ikm_prev = kmsg->ikm_next = IKM_BOGUS));

	return kmsg;
}

/*
 *	Routine:	ipc_kmsg_free
 *	Purpose:
 *		Free a kernel message buffer.  If the kms is preallocated
 *		to a port, just "put it back (marked unused)."  We have to
 *		do this with the port locked.  The port may have its hold
 *		on our message released.  In that case, we have to just
 *		revert the message to a traditional one and free it normally.
 *	Conditions:
 *		Nothing locked.
 */

void
ipc_kmsg_free(
	ipc_kmsg_t      kmsg)
{
	mach_msg_size_t size = kmsg->ikm_size;
	ipc_port_t port;

	assert(!IP_VALID(kmsg->ikm_voucher));

	KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_FREE) | DBG_FUNC_NONE,
	    VM_KERNEL_ADDRPERM((uintptr_t)kmsg),
	    0, 0, 0, 0);

	/*
	 * Check to see if the message is bound to the port.  If so,
	 * mark it not in use.  If the port isn't already dead, then
	 * leave the message associated with it.  Otherwise, free it.
	 */
	if (size == IKM_SAVED_MSG_SIZE) {
		if ((void *)kmsg->ikm_header < (void *)(kmsg + 1) ||
		    (void *)kmsg->ikm_header >= (void *)(kmsg + 1) + IKM_SAVED_MSG_SIZE) {
			panic("ipc_kmsg_free");
		}
		port = ikm_prealloc_inuse_port(kmsg);
		if (port != IP_NULL) {
			ip_lock(port);
			ikm_prealloc_clear_inuse(kmsg, port);
			if (ip_active(port) && (port->ip_premsg == kmsg)) {
				assert(IP_PREALLOC(port));
				ip_unlock(port);
				ip_release(port);
				return;
			}
			ip_unlock(port);
			ip_release(port); /* May be last reference */
		}
	} else {
		void *data = kmsg->ikm_data;
		if ((void *)kmsg->ikm_header < data ||
		    (void *)kmsg->ikm_header >= data + size) {
			panic("ipc_kmsg_free");
		}
		kheap_free(KHEAP_DATA_BUFFERS, data, size);
	}
	zfree(ipc_kmsg_zone, kmsg);
}


/*
 *	Routine:	ipc_kmsg_enqueue
 *	Purpose:
 *		Enqueue a kmsg.
 */

void
ipc_kmsg_enqueue(
	ipc_kmsg_queue_t        queue,
	ipc_kmsg_t              kmsg)
{
	ipc_kmsg_t first = queue->ikmq_base;
	ipc_kmsg_t last;

	if (first == IKM_NULL) {
		queue->ikmq_base = kmsg;
		kmsg->ikm_next = kmsg;
		kmsg->ikm_prev = kmsg;
	} else {
		last = first->ikm_prev;
		kmsg->ikm_next = first;
		kmsg->ikm_prev = last;
		first->ikm_prev = kmsg;
		last->ikm_next = kmsg;
	}
}

/*
 *	Routine:	ipc_kmsg_enqueue_qos
 *	Purpose:
 *		Enqueue a kmsg, propagating qos
 *		overrides towards the head of the queue.
 *
 *	Returns:
 *		whether the head of the queue had
 *		it's override-qos adjusted because
 *		of this insertion.
 */

boolean_t
ipc_kmsg_enqueue_qos(
	ipc_kmsg_queue_t        queue,
	ipc_kmsg_t              kmsg)
{
	ipc_kmsg_t first = queue->ikmq_base;
	ipc_kmsg_t prev;
	mach_msg_qos_t qos_ovr;

	if (first == IKM_NULL) {
		/* insert a first message */
		queue->ikmq_base = kmsg;
		kmsg->ikm_next = kmsg;
		kmsg->ikm_prev = kmsg;
		return TRUE;
	}

	/* insert at the tail */
	prev = first->ikm_prev;
	kmsg->ikm_next = first;
	kmsg->ikm_prev = prev;
	first->ikm_prev = kmsg;
	prev->ikm_next = kmsg;

	/* apply QoS overrides towards the head */
	qos_ovr = kmsg->ikm_qos_override;
	while (prev != kmsg &&
	    qos_ovr > prev->ikm_qos_override) {
		prev->ikm_qos_override = qos_ovr;
		prev = prev->ikm_prev;
	}

	/* did we adjust everything? */
	return prev == kmsg;
}

/*
 *	Routine:	ipc_kmsg_override_qos
 *	Purpose:
 *		Update the override for a given kmsg already
 *		enqueued, propagating qos override adjustments
 *		towards	the head of the queue.
 *
 *	Returns:
 *		whether the head of the queue had
 *		it's override-qos adjusted because
 *		of this insertion.
 */

boolean_t
ipc_kmsg_override_qos(
	ipc_kmsg_queue_t    queue,
	ipc_kmsg_t          kmsg,
	mach_msg_qos_t      qos_ovr)
{
	ipc_kmsg_t first = queue->ikmq_base;
	ipc_kmsg_t cur = kmsg;

	/* apply QoS overrides towards the head */
	while (qos_ovr > cur->ikm_qos_override) {
		cur->ikm_qos_override = qos_ovr;
		if (cur == first) {
			return TRUE;
		}
		cur = cur->ikm_prev;
	}
	return FALSE;
}

/*
 *	Routine:	ipc_kmsg_dequeue
 *	Purpose:
 *		Dequeue and return a kmsg.
 */

ipc_kmsg_t
ipc_kmsg_dequeue(
	ipc_kmsg_queue_t        queue)
{
	ipc_kmsg_t first;

	first = ipc_kmsg_queue_first(queue);

	if (first != IKM_NULL) {
		ipc_kmsg_rmqueue(queue, first);
	}

	return first;
}

/*
 *	Routine:	ipc_kmsg_rmqueue
 *	Purpose:
 *		Pull a kmsg out of a queue.
 */

void
ipc_kmsg_rmqueue(
	ipc_kmsg_queue_t        queue,
	ipc_kmsg_t              kmsg)
{
	ipc_kmsg_t next, prev;

	assert(queue->ikmq_base != IKM_NULL);

	next = kmsg->ikm_next;
	prev = kmsg->ikm_prev;

	if (next == kmsg) {
		assert(prev == kmsg);
		assert(queue->ikmq_base == kmsg);

		queue->ikmq_base = IKM_NULL;
	} else {
		if (__improbable(next->ikm_prev != kmsg || prev->ikm_next != kmsg)) {
			panic("ipc_kmsg_rmqueue: inconsistent prev/next pointers. "
			    "(prev->next: %p, next->prev: %p, kmsg: %p)",
			    prev->ikm_next, next->ikm_prev, kmsg);
		}

		if (queue->ikmq_base == kmsg) {
			queue->ikmq_base = next;
		}

		next->ikm_prev = prev;
		prev->ikm_next = next;
	}
	/* XXX Temporary debug logic */
	assert((kmsg->ikm_next = IKM_BOGUS) == IKM_BOGUS);
	assert((kmsg->ikm_prev = IKM_BOGUS) == IKM_BOGUS);
}

/*
 *	Routine:	ipc_kmsg_queue_next
 *	Purpose:
 *		Return the kmsg following the given kmsg.
 *		(Or IKM_NULL if it is the last one in the queue.)
 */

ipc_kmsg_t
ipc_kmsg_queue_next(
	ipc_kmsg_queue_t        queue,
	ipc_kmsg_t              kmsg)
{
	ipc_kmsg_t next;

	assert(queue->ikmq_base != IKM_NULL);

	next = kmsg->ikm_next;
	if (queue->ikmq_base == next) {
		next = IKM_NULL;
	}

	return next;
}

/*
 *	Routine:	ipc_kmsg_destroy
 *	Purpose:
 *		Destroys a kernel message.  Releases all rights,
 *		references, and memory held by the message.
 *		Frees the message.
 *	Conditions:
 *		No locks held.
 */

void
ipc_kmsg_destroy(
	ipc_kmsg_t      kmsg)
{
	/*
	 *	Destroying a message can cause more messages to be destroyed.
	 *	Curtail recursion by putting messages on the deferred
	 *	destruction queue.  If this was the first message on the
	 *	queue, this instance must process the full queue.
	 */
	if (ipc_kmsg_delayed_destroy(kmsg)) {
		ipc_kmsg_reap_delayed();
	}
}

/*
 *	Routine:	ipc_kmsg_delayed_destroy
 *	Purpose:
 *		Enqueues a kernel message for deferred destruction.
 *	Returns:
 *		Boolean indicator that the caller is responsible to reap
 *		deferred messages.
 */

boolean_t
ipc_kmsg_delayed_destroy(
	ipc_kmsg_t kmsg)
{
	ipc_kmsg_queue_t queue = &(current_thread()->ith_messages);
	boolean_t first = ipc_kmsg_queue_empty(queue);

	ipc_kmsg_enqueue(queue, kmsg);
	return first;
}

/*
 *	Routine:	ipc_kmsg_destroy_queue
 *	Purpose:
 *		Destroys messages from the per-thread
 *		deferred reaping queue.
 *	Conditions:
 *		No locks held.
 */

void
ipc_kmsg_reap_delayed(void)
{
	ipc_kmsg_queue_t queue = &(current_thread()->ith_messages);
	ipc_kmsg_t kmsg;

	/*
	 * must leave kmsg in queue while cleaning it to assure
	 * no nested calls recurse into here.
	 */
	while ((kmsg = ipc_kmsg_queue_first(queue)) != IKM_NULL) {
		ipc_kmsg_clean(kmsg);
		ipc_kmsg_rmqueue(queue, kmsg);
		ipc_kmsg_free(kmsg);
	}
}

/*
 *	Routine:	ipc_kmsg_clean_body
 *	Purpose:
 *		Cleans the body of a kernel message.
 *		Releases all rights, references, and memory.
 *
 *	Conditions:
 *		No locks held.
 */
static unsigned int _ipc_kmsg_clean_invalid_desc = 0;
void
ipc_kmsg_clean_body(
	__unused ipc_kmsg_t     kmsg,
	mach_msg_type_number_t  number,
	mach_msg_descriptor_t   *saddr)
{
	mach_msg_type_number_t      i;

	if (number == 0) {
		return;
	}

	for (i = 0; i < number; i++, saddr++) {
		switch (saddr->type.type) {
		case MACH_MSG_PORT_DESCRIPTOR: {
			mach_msg_port_descriptor_t *dsc;

			dsc = &saddr->port;

			/*
			 * Destroy port rights carried in the message
			 */
			if (!IP_VALID(dsc->name)) {
				continue;
			}
			ipc_object_destroy(ip_to_object(dsc->name), dsc->disposition);
			break;
		}
		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
		case MACH_MSG_OOL_DESCRIPTOR: {
			mach_msg_ool_descriptor_t *dsc;

			dsc = (mach_msg_ool_descriptor_t *)&saddr->out_of_line;

			/*
			 * Destroy memory carried in the message
			 */
			if (dsc->size == 0) {
				assert(dsc->address == (void *) 0);
			} else {
				vm_map_copy_discard((vm_map_copy_t) dsc->address);
			}
			break;
		}
		case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
			ipc_object_t                    *objects;
			mach_msg_type_number_t          j;
			mach_msg_ool_ports_descriptor_t *dsc;

			dsc = (mach_msg_ool_ports_descriptor_t  *)&saddr->ool_ports;
			objects = (ipc_object_t *) dsc->address;

			if (dsc->count == 0) {
				break;
			}

			assert(objects != (ipc_object_t *) 0);

			/* destroy port rights carried in the message */

			for (j = 0; j < dsc->count; j++) {
				ipc_object_t object = objects[j];

				if (!IO_VALID(object)) {
					continue;
				}

				ipc_object_destroy(object, dsc->disposition);
			}

			/* destroy memory carried in the message */

			assert(dsc->count != 0);

			kfree(dsc->address,
			    (vm_size_t) dsc->count * sizeof(mach_port_t));
			break;
		}
		case MACH_MSG_GUARDED_PORT_DESCRIPTOR: {
			mach_msg_guarded_port_descriptor_t *dsc = (typeof(dsc)) & saddr->guarded_port;

			/*
			 * Destroy port rights carried in the message
			 */
			if (!IP_VALID(dsc->name)) {
				continue;
			}
			ipc_object_destroy(ip_to_object(dsc->name), dsc->disposition);
			break;
		}
		default: {
			_ipc_kmsg_clean_invalid_desc++;         /* don't understand this type of descriptor */
		}
		}
	}
}

/*
 *	Routine:	ipc_kmsg_clean_partial
 *	Purpose:
 *		Cleans a partially-acquired kernel message.
 *		number is the index of the type descriptor
 *		in the body of the message that contained the error.
 *		If dolast, the memory and port rights in this last
 *		type spec are also cleaned.  In that case, number
 *		specifies the number of port rights to clean.
 *	Conditions:
 *		Nothing locked.
 */

void
ipc_kmsg_clean_partial(
	ipc_kmsg_t              kmsg,
	mach_msg_type_number_t  number,
	mach_msg_descriptor_t   *desc,
	vm_offset_t             paddr,
	vm_size_t               length)
{
	ipc_object_t object;
	mach_msg_bits_t mbits = kmsg->ikm_header->msgh_bits;

	/* deal with importance chain while we still have dest and voucher references */
	ipc_importance_clean(kmsg);

	object = ip_to_object(kmsg->ikm_header->msgh_remote_port);
	assert(IO_VALID(object));
	ipc_object_destroy_dest(object, MACH_MSGH_BITS_REMOTE(mbits));

	object = ip_to_object(kmsg->ikm_header->msgh_local_port);
	if (IO_VALID(object)) {
		ipc_object_destroy(object, MACH_MSGH_BITS_LOCAL(mbits));
	}

	object = ip_to_object(kmsg->ikm_voucher);
	if (IO_VALID(object)) {
		assert(MACH_MSGH_BITS_VOUCHER(mbits) == MACH_MSG_TYPE_MOVE_SEND);
		ipc_object_destroy(object, MACH_MSG_TYPE_PORT_SEND);
		kmsg->ikm_voucher = IP_NULL;
	}

	if (paddr) {
		(void) vm_deallocate(ipc_kernel_copy_map, paddr, length);
	}

	ipc_kmsg_clean_body(kmsg, number, desc);
}

/*
 *	Routine:	ipc_kmsg_clean
 *	Purpose:
 *		Cleans a kernel message.  Releases all rights,
 *		references, and memory held by the message.
 *	Conditions:
 *		No locks held.
 */

void
ipc_kmsg_clean(
	ipc_kmsg_t      kmsg)
{
	ipc_object_t object;
	mach_msg_bits_t mbits;

	/* deal with importance chain while we still have dest and voucher references */
	ipc_importance_clean(kmsg);

	mbits = kmsg->ikm_header->msgh_bits;
	object = ip_to_object(kmsg->ikm_header->msgh_remote_port);
	if (IO_VALID(object)) {
		ipc_object_destroy_dest(object, MACH_MSGH_BITS_REMOTE(mbits));
	}

	object = ip_to_object(kmsg->ikm_header->msgh_local_port);
	if (IO_VALID(object)) {
		ipc_object_destroy(object, MACH_MSGH_BITS_LOCAL(mbits));
	}

	object = ip_to_object(kmsg->ikm_voucher);
	if (IO_VALID(object)) {
		assert(MACH_MSGH_BITS_VOUCHER(mbits) == MACH_MSG_TYPE_MOVE_SEND);
		ipc_object_destroy(object, MACH_MSG_TYPE_PORT_SEND);
		kmsg->ikm_voucher = IP_NULL;
	}

	if (mbits & MACH_MSGH_BITS_COMPLEX) {
		mach_msg_body_t *body;

		body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
		ipc_kmsg_clean_body(kmsg, body->msgh_descriptor_count,
		    (mach_msg_descriptor_t *)(body + 1));
	}
}

/*
 *	Routine:	ipc_kmsg_set_prealloc
 *	Purpose:
 *		Assign a kmsg as a preallocated message buffer to a port.
 *	Conditions:
 *		port locked.
 */

void
ipc_kmsg_set_prealloc(
	ipc_kmsg_t              kmsg,
	ipc_port_t              port)
{
	assert(kmsg->ikm_prealloc == IP_NULL);

	kmsg->ikm_prealloc = IP_NULL;

	assert(port_send_turnstile(port) == TURNSTILE_NULL);
	kmsg->ikm_turnstile = TURNSTILE_NULL;
	IP_SET_PREALLOC(port, kmsg);
}

/*
 *	Routine:	ipc_kmsg_clear_prealloc
 *	Purpose:
 *		Release the Assignment of a preallocated message buffer from a port.
 *	Conditions:
 *		port locked.
 */
void
ipc_kmsg_clear_prealloc(
	ipc_kmsg_t              kmsg,
	ipc_port_t              port)
{
	/* take the mqueue lock since the turnstile is protected under it */
	imq_lock(&port->ip_messages);

	IP_CLEAR_PREALLOC(port, kmsg);
	set_port_send_turnstile(port, kmsg->ikm_turnstile);
	imq_unlock(&port->ip_messages);
}

/*
 *	Routine:	ipc_kmsg_prealloc
 *	Purpose:
 *		Wraper to ipc_kmsg_alloc() to account for
 *		header expansion requirements.
 */
ipc_kmsg_t
ipc_kmsg_prealloc(mach_msg_size_t size)
{
#if defined(__LP64__)
	if (size > IKM_SAVED_MSG_SIZE - LEGACY_HEADER_SIZE_DELTA) {
		panic("ipc_kmsg_prealloc");
	}

	size += LEGACY_HEADER_SIZE_DELTA;
#endif
	return ipc_kmsg_alloc(size);
}


/*
 *	Routine:	ipc_kmsg_get
 *	Purpose:
 *		Allocates a kernel message buffer.
 *		Copies a user message to the message buffer.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	Acquired a message buffer.
 *		MACH_SEND_MSG_TOO_SMALL	Message smaller than a header.
 *		MACH_SEND_MSG_TOO_SMALL	Message size not long-word multiple.
 *		MACH_SEND_TOO_LARGE	Message too large to ever be sent.
 *		MACH_SEND_NO_BUFFER	Couldn't allocate a message buffer.
 *		MACH_SEND_INVALID_DATA	Couldn't copy message data.
 */

mach_msg_return_t
ipc_kmsg_get(
	mach_vm_address_t       msg_addr,
	mach_msg_size_t size,
	ipc_kmsg_t              *kmsgp)
{
	mach_msg_size_t                 msg_and_trailer_size;
	ipc_kmsg_t                      kmsg;
	mach_msg_max_trailer_t          *trailer;
	mach_msg_legacy_base_t      legacy_base;
	mach_msg_size_t             len_copied;
	legacy_base.body.msgh_descriptor_count = 0;

	if ((size < sizeof(mach_msg_legacy_header_t)) || (size & 3)) {
		return MACH_SEND_MSG_TOO_SMALL;
	}

	if (size > ipc_kmsg_max_body_space) {
		return MACH_SEND_TOO_LARGE;
	}

	if (size == sizeof(mach_msg_legacy_header_t)) {
		len_copied = sizeof(mach_msg_legacy_header_t);
	} else {
		len_copied = sizeof(mach_msg_legacy_base_t);
	}

	if (copyinmsg(msg_addr, (char *)&legacy_base, len_copied)) {
		return MACH_SEND_INVALID_DATA;
	}

	/*
	 * If the message claims to be complex, it must at least
	 * have the length of a "base" message (header + dsc_count).
	 */
	if (len_copied < sizeof(mach_msg_legacy_base_t) &&
	    (legacy_base.header.msgh_bits & MACH_MSGH_BITS_COMPLEX)) {
		return MACH_SEND_MSG_TOO_SMALL;
	}

	msg_addr += sizeof(legacy_base.header);
#if defined(__LP64__)
	size += LEGACY_HEADER_SIZE_DELTA;
#endif
	/* unreachable if !DEBUG */
	__unreachable_ok_push
	if (DEBUG_KPRINT_SYSCALL_PREDICATE(DEBUG_KPRINT_SYSCALL_IPC_MASK)) {
		unsigned int j;
		for (j = 0; j < sizeof(legacy_base.header); j++) {
			kprintf("%02x\n", ((unsigned char*)&legacy_base.header)[j]);
		}
	}
	__unreachable_ok_pop

	    msg_and_trailer_size = size + MAX_TRAILER_SIZE;
	kmsg = ipc_kmsg_alloc(msg_and_trailer_size);
	if (kmsg == IKM_NULL) {
		return MACH_SEND_NO_BUFFER;
	}

	kmsg->ikm_header->msgh_size                     = size;
	kmsg->ikm_header->msgh_bits                     = legacy_base.header.msgh_bits;
	kmsg->ikm_header->msgh_remote_port      = CAST_MACH_NAME_TO_PORT(legacy_base.header.msgh_remote_port);
	kmsg->ikm_header->msgh_local_port       = CAST_MACH_NAME_TO_PORT(legacy_base.header.msgh_local_port);
	kmsg->ikm_header->msgh_voucher_port             = legacy_base.header.msgh_voucher_port;
	kmsg->ikm_header->msgh_id                       = legacy_base.header.msgh_id;

	DEBUG_KPRINT_SYSCALL_IPC("ipc_kmsg_get header:\n"
	    "  size:		0x%.8x\n"
	    "  bits:		0x%.8x\n"
	    "  remote_port:	%p\n"
	    "  local_port:	%p\n"
	    "  voucher_port:	0x%.8x\n"
	    "  id:		%.8d\n",
	    kmsg->ikm_header->msgh_size,
	    kmsg->ikm_header->msgh_bits,
	    kmsg->ikm_header->msgh_remote_port,
	    kmsg->ikm_header->msgh_local_port,
	    kmsg->ikm_header->msgh_voucher_port,
	    kmsg->ikm_header->msgh_id);

	if (copyinmsg(msg_addr, (char *)(kmsg->ikm_header + 1), size - (mach_msg_size_t)sizeof(mach_msg_header_t))) {
		ipc_kmsg_free(kmsg);
		return MACH_SEND_INVALID_DATA;
	}

	/* unreachable if !DEBUG */
	__unreachable_ok_push
	if (DEBUG_KPRINT_SYSCALL_PREDICATE(DEBUG_KPRINT_SYSCALL_IPC_MASK)) {
		kprintf("body: size: %lu\n", (size - sizeof(mach_msg_header_t)));
		uint32_t i;
		for (i = 0; i * 4 < (size - sizeof(mach_msg_header_t)); i++) {
			kprintf("%.4x\n", ((uint32_t *)(kmsg->ikm_header + 1))[i]);
		}
	}
	__unreachable_ok_pop
	DEBUG_IPC_KMSG_PRINT(kmsg, "ipc_kmsg_get()");

	/*
	 * I reserve for the trailer the largest space (MAX_TRAILER_SIZE)
	 * However, the internal size field of the trailer (msgh_trailer_size)
	 * is initialized to the minimum (sizeof(mach_msg_trailer_t)), to optimize
	 * the cases where no implicit data is requested.
	 */
	trailer = (mach_msg_max_trailer_t *) ((vm_offset_t)kmsg->ikm_header + size);
	bzero(trailer, sizeof(*trailer));
	trailer->msgh_sender = current_thread()->task->sec_token;
	trailer->msgh_audit = current_thread()->task->audit_token;
	trailer->msgh_trailer_type = MACH_MSG_TRAILER_FORMAT_0;
	trailer->msgh_trailer_size = MACH_MSG_TRAILER_MINIMUM_SIZE;

#ifdef ppc
	if (trcWork.traceMask) {
		dbgTrace(0x1100, (unsigned int)kmsg->ikm_header->msgh_id,
		    (unsigned int)kmsg->ikm_header->msgh_remote_port,
		    (unsigned int)kmsg->ikm_header->msgh_local_port, 0);
	}
#endif

	trailer->msgh_labels.sender = 0;
	*kmsgp = kmsg;
	return MACH_MSG_SUCCESS;
}

/*
 *	Routine:	ipc_kmsg_get_from_kernel
 *	Purpose:
 *		First checks for a preallocated message
 *		reserved for kernel clients.  If not found -
 *		allocates a new kernel message buffer.
 *		Copies a kernel message to the message buffer.
 *		Only resource errors are allowed.
 *	Conditions:
 *		Nothing locked.
 *		Ports in header are ipc_port_t.
 *	Returns:
 *		MACH_MSG_SUCCESS	Acquired a message buffer.
 *		MACH_SEND_NO_BUFFER	Couldn't allocate a message buffer.
 */

mach_msg_return_t
ipc_kmsg_get_from_kernel(
	mach_msg_header_t       *msg,
	mach_msg_size_t size,
	ipc_kmsg_t              *kmsgp)
{
	ipc_kmsg_t      kmsg;
	mach_msg_size_t msg_and_trailer_size;
	mach_msg_max_trailer_t *trailer;
	ipc_port_t      dest_port;

	assert(size >= sizeof(mach_msg_header_t));
	assert((size & 3) == 0);

	dest_port = msg->msgh_remote_port;

	msg_and_trailer_size = size + MAX_TRAILER_SIZE;

	/*
	 * See if the port has a pre-allocated kmsg for kernel
	 * clients.  These are set up for those kernel clients
	 * which cannot afford to wait.
	 */
	if (IP_VALID(dest_port) && IP_PREALLOC(dest_port)) {
		mach_msg_size_t max_desc = 0;

		ip_lock(dest_port);
		if (!ip_active(dest_port)) {
			ip_unlock(dest_port);
			return MACH_SEND_NO_BUFFER;
		}
		assert(IP_PREALLOC(dest_port));
		kmsg = dest_port->ip_premsg;
		if (ikm_prealloc_inuse(kmsg)) {
			ip_unlock(dest_port);
			return MACH_SEND_NO_BUFFER;
		}
#if !defined(__LP64__)
		if (msg->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
			assert(size > sizeof(mach_msg_base_t));
			max_desc = ((mach_msg_base_t *)msg)->body.msgh_descriptor_count *
			    DESC_SIZE_ADJUSTMENT;
		}
#endif
		if (msg_and_trailer_size > kmsg->ikm_size - max_desc) {
			ip_unlock(dest_port);
			return MACH_SEND_TOO_LARGE;
		}
		ikm_prealloc_set_inuse(kmsg, dest_port);
		ikm_set_header(kmsg, NULL, msg_and_trailer_size);
		ip_unlock(dest_port);
	} else {
		kmsg = ipc_kmsg_alloc(msg_and_trailer_size);
		if (kmsg == IKM_NULL) {
			return MACH_SEND_NO_BUFFER;
		}
	}

	(void) memcpy((void *) kmsg->ikm_header, (const void *) msg, size);

	ikm_qos_init(kmsg);

	kmsg->ikm_header->msgh_size = size;

	/*
	 * I reserve for the trailer the largest space (MAX_TRAILER_SIZE)
	 * However, the internal size field of the trailer (msgh_trailer_size)
	 * is initialized to the minimum (sizeof(mach_msg_trailer_t)), to
	 * optimize the cases where no implicit data is requested.
	 */
	trailer = (mach_msg_max_trailer_t *)
	    ((vm_offset_t)kmsg->ikm_header + size);
	bzero(trailer, sizeof(*trailer));
	trailer->msgh_sender = KERNEL_SECURITY_TOKEN;
	trailer->msgh_audit = KERNEL_AUDIT_TOKEN;
	trailer->msgh_trailer_type = MACH_MSG_TRAILER_FORMAT_0;
	trailer->msgh_trailer_size = MACH_MSG_TRAILER_MINIMUM_SIZE;

	trailer->msgh_labels.sender = 0;

	*kmsgp = kmsg;
	return MACH_MSG_SUCCESS;
}

/*
 *	Routine:	ipc_kmsg_send
 *	Purpose:
 *		Send a message.  The message holds a reference
 *		for the destination port in the msgh_remote_port field.
 *
 *		If unsuccessful, the caller still has possession of
 *		the message and must do something with it.  If successful,
 *		the message is queued, given to a receiver, destroyed,
 *		or handled directly by the kernel via mach_msg.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	The message was accepted.
 *		MACH_SEND_TIMED_OUT	Caller still has message.
 *		MACH_SEND_INTERRUPTED	Caller still has message.
 *		MACH_SEND_INVALID_DEST	Caller still has message.
 */
mach_msg_return_t
ipc_kmsg_send(
	ipc_kmsg_t              kmsg,
	mach_msg_option_t       option,
	mach_msg_timeout_t      send_timeout)
{
	ipc_port_t port;
	thread_t th = current_thread();
	mach_msg_return_t error = MACH_MSG_SUCCESS;
	boolean_t kernel_reply = FALSE;

	/* Check if honor qlimit flag is set on thread. */
	if ((th->options & TH_OPT_HONOR_QLIMIT) == TH_OPT_HONOR_QLIMIT) {
		/* Remove the MACH_SEND_ALWAYS flag to honor queue limit. */
		option &= (~MACH_SEND_ALWAYS);
		/* Add the timeout flag since the message queue might be full. */
		option |= MACH_SEND_TIMEOUT;
		th->options &= (~TH_OPT_HONOR_QLIMIT);
	}

#if IMPORTANCE_INHERITANCE
	bool did_importance = false;
#if IMPORTANCE_TRACE
	mach_msg_id_t imp_msgh_id = -1;
	int           sender_pid  = -1;
#endif /* IMPORTANCE_TRACE */
#endif /* IMPORTANCE_INHERITANCE */

	/* don't allow the creation of a circular loop */
	if (kmsg->ikm_header->msgh_bits & MACH_MSGH_BITS_CIRCULAR) {
		ipc_kmsg_destroy(kmsg);
		KDBG(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_INFO) | DBG_FUNC_END, MACH_MSGH_BITS_CIRCULAR);
		return MACH_MSG_SUCCESS;
	}

	ipc_voucher_send_preprocessing(kmsg);

	port = kmsg->ikm_header->msgh_remote_port;
	assert(IP_VALID(port));
	ip_lock(port);

	/*
	 * If the destination has been guarded with a reply context, and the
	 * sender is consuming a send-once right, then assume this is a reply
	 * to an RPC and we need to validate that this sender is currently in
	 * the correct context.
	 */
	if (enforce_strict_reply && port->ip_reply_context != 0 &&
	    ((option & MACH_SEND_KERNEL) == 0) &&
	    MACH_MSGH_BITS_REMOTE(kmsg->ikm_header->msgh_bits) == MACH_MSG_TYPE_PORT_SEND_ONCE) {
		error = ipc_kmsg_validate_reply_context_locked(option, port, th->ith_voucher, th->ith_voucher_name);
		if (error != MACH_MSG_SUCCESS) {
			ip_unlock(port);
			return error;
		}
	}

#if IMPORTANCE_INHERITANCE
retry:
#endif /* IMPORTANCE_INHERITANCE */
	/*
	 *	Can't deliver to a dead port.
	 *	However, we can pretend it got sent
	 *	and was then immediately destroyed.
	 */
	if (!ip_active(port)) {
		ip_unlock(port);
#if MACH_FLIPC
		if (MACH_NODE_VALID(kmsg->ikm_node) && FPORT_VALID(port->ip_messages.imq_fport)) {
			flipc_msg_ack(kmsg->ikm_node, &port->ip_messages, FALSE);
		}
#endif
		if (did_importance) {
			/*
			 * We're going to pretend we delivered this message
			 * successfully, and just eat the kmsg. However, the
			 * kmsg is actually visible via the importance_task!
			 * We need to cleanup this linkage before we destroy
			 * the message, and more importantly before we set the
			 * msgh_remote_port to NULL. See: 34302571
			 */
			ipc_importance_clean(kmsg);
		}
		ip_release(port);  /* JMM - Future: release right, not just ref */
		kmsg->ikm_header->msgh_remote_port = MACH_PORT_NULL;
		ipc_kmsg_destroy(kmsg);
		KDBG(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_INFO) | DBG_FUNC_END, MACH_SEND_INVALID_DEST);
		return MACH_MSG_SUCCESS;
	}

	if (port->ip_receiver == ipc_space_kernel) {
		/*
		 *	We can check ip_receiver == ipc_space_kernel
		 *	before checking that the port is active because
		 *	ipc_port_dealloc_kernel clears ip_receiver
		 *	before destroying a kernel port.
		 */
		require_ip_active(port);
		port->ip_messages.imq_seqno++;
		ip_unlock(port);

		current_task()->messages_sent++;

		/*
		 * Call the server routine, and get the reply message to send.
		 */
		kmsg = ipc_kobject_server(kmsg, option);
		if (kmsg == IKM_NULL) {
			return MACH_MSG_SUCCESS;
		}

		/* sign the reply message */
		ikm_sign(kmsg);

		/* restart the KMSG_INFO tracing for the reply message */
		KDBG(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_INFO) | DBG_FUNC_START);
		port = kmsg->ikm_header->msgh_remote_port;
		assert(IP_VALID(port));
		ip_lock(port);
		/* fall thru with reply - same options */
		kernel_reply = TRUE;
		if (!ip_active(port)) {
			error = MACH_SEND_INVALID_DEST;
		}
	}

#if IMPORTANCE_INHERITANCE
	/*
	 * Need to see if this message needs importance donation and/or
	 * propagation.  That routine can drop the port lock temporarily.
	 * If it does we'll have to revalidate the destination.
	 */
	if (!did_importance) {
		did_importance = true;
		if (ipc_importance_send(kmsg, option)) {
			goto retry;
		}
	}
#endif /* IMPORTANCE_INHERITANCE */

	if (error != MACH_MSG_SUCCESS) {
		ip_unlock(port);
	} else {
		/*
		 * We have a valid message and a valid reference on the port.
		 * we can unlock the port and call mqueue_send() on its message
		 * queue. Lock message queue while port is locked.
		 */
		imq_lock(&port->ip_messages);

		ipc_special_reply_port_msg_sent(port);

		ip_unlock(port);

		error = ipc_mqueue_send(&port->ip_messages, kmsg, option,
		    send_timeout);
	}

#if IMPORTANCE_INHERITANCE
	if (did_importance) {
		__unused int importance_cleared = 0;
		switch (error) {
		case MACH_SEND_TIMED_OUT:
		case MACH_SEND_NO_BUFFER:
		case MACH_SEND_INTERRUPTED:
		case MACH_SEND_INVALID_DEST:
			/*
			 * We still have the kmsg and its
			 * reference on the port.  But we
			 * have to back out the importance
			 * boost.
			 *
			 * The port could have changed hands,
			 * be inflight to another destination,
			 * etc...  But in those cases our
			 * back-out will find the new owner
			 * (and all the operations that
			 * transferred the right should have
			 * applied their own boost adjustments
			 * to the old owner(s)).
			 */
			importance_cleared = 1;
			ipc_importance_clean(kmsg);
			break;

		case MACH_MSG_SUCCESS:
		default:
			break;
		}
#if IMPORTANCE_TRACE
		KERNEL_DEBUG_CONSTANT_IST(KDEBUG_TRACE, (IMPORTANCE_CODE(IMP_MSG, IMP_MSG_SEND)) | DBG_FUNC_END,
		    task_pid(current_task()), sender_pid, imp_msgh_id, importance_cleared, 0);
#endif /* IMPORTANCE_TRACE */
	}
#endif /* IMPORTANCE_INHERITANCE */

	/*
	 * If the port has been destroyed while we wait, treat the message
	 * as a successful delivery (like we do for an inactive port).
	 */
	if (error == MACH_SEND_INVALID_DEST) {
#if MACH_FLIPC
		if (MACH_NODE_VALID(kmsg->ikm_node) && FPORT_VALID(port->ip_messages.imq_fport)) {
			flipc_msg_ack(kmsg->ikm_node, &port->ip_messages, FALSE);
		}
#endif
		ip_release(port); /* JMM - Future: release right, not just ref */
		kmsg->ikm_header->msgh_remote_port = MACH_PORT_NULL;
		ipc_kmsg_destroy(kmsg);
		KDBG(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_INFO) | DBG_FUNC_END, MACH_SEND_INVALID_DEST);
		return MACH_MSG_SUCCESS;
	}

	if (error != MACH_MSG_SUCCESS && kernel_reply) {
		/*
		 * Kernel reply messages that fail can't be allowed to
		 * pseudo-receive on error conditions. We need to just treat
		 * the message as a successful delivery.
		 */
#if MACH_FLIPC
		if (MACH_NODE_VALID(kmsg->ikm_node) && FPORT_VALID(port->ip_messages.imq_fport)) {
			flipc_msg_ack(kmsg->ikm_node, &port->ip_messages, FALSE);
		}
#endif
		ip_release(port); /* JMM - Future: release right, not just ref */
		kmsg->ikm_header->msgh_remote_port = MACH_PORT_NULL;
		ipc_kmsg_destroy(kmsg);
		KDBG(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_INFO) | DBG_FUNC_END, error);
		return MACH_MSG_SUCCESS;
	}
	return error;
}

/*
 *	Routine:	ipc_kmsg_put
 *	Purpose:
 *		Copies a message buffer to a user message.
 *		Copies only the specified number of bytes.
 *		Frees the message buffer.
 *	Conditions:
 *		Nothing locked.  The message buffer must have clean
 *		header fields.
 *	Returns:
 *		MACH_MSG_SUCCESS	Copied data out of message buffer.
 *		MACH_RCV_INVALID_DATA	Couldn't copy to user message.
 */

mach_msg_return_t
ipc_kmsg_put(
	ipc_kmsg_t              kmsg,
	mach_msg_option_t       option,
	mach_vm_address_t       rcv_addr,
	mach_msg_size_t         rcv_size,
	mach_msg_size_t         trailer_size,
	mach_msg_size_t         *sizep)
{
	mach_msg_size_t size = kmsg->ikm_header->msgh_size + trailer_size;
	mach_msg_return_t mr;

	DEBUG_IPC_KMSG_PRINT(kmsg, "ipc_kmsg_put()");


	DEBUG_KPRINT_SYSCALL_IPC("ipc_kmsg_put header:\n"
	    "  size:		0x%.8x\n"
	    "  bits:		0x%.8x\n"
	    "  remote_port:	%p\n"
	    "  local_port:	%p\n"
	    "  voucher_port:	0x%.8x\n"
	    "  id:		%.8d\n",
	    kmsg->ikm_header->msgh_size,
	    kmsg->ikm_header->msgh_bits,
	    kmsg->ikm_header->msgh_remote_port,
	    kmsg->ikm_header->msgh_local_port,
	    kmsg->ikm_header->msgh_voucher_port,
	    kmsg->ikm_header->msgh_id);

#if defined(__LP64__)
	if (current_task() != kernel_task) { /* don't if receiver expects fully-cooked in-kernel msg; */
		mach_msg_legacy_header_t *legacy_header =
		    (mach_msg_legacy_header_t *)((vm_offset_t)(kmsg->ikm_header) + LEGACY_HEADER_SIZE_DELTA);

		mach_msg_bits_t         bits            = kmsg->ikm_header->msgh_bits;
		mach_msg_size_t         msg_size        = kmsg->ikm_header->msgh_size;
		mach_port_name_t        remote_port     = CAST_MACH_PORT_TO_NAME(kmsg->ikm_header->msgh_remote_port);
		mach_port_name_t        local_port      = CAST_MACH_PORT_TO_NAME(kmsg->ikm_header->msgh_local_port);
		mach_port_name_t        voucher_port    = kmsg->ikm_header->msgh_voucher_port;
		mach_msg_id_t           id                      = kmsg->ikm_header->msgh_id;

		legacy_header->msgh_id                  = id;
		legacy_header->msgh_local_port = local_port;
		legacy_header->msgh_remote_port = remote_port;
		legacy_header->msgh_voucher_port = voucher_port;
		legacy_header->msgh_size                = msg_size - LEGACY_HEADER_SIZE_DELTA;
		legacy_header->msgh_bits                = bits;

		size -= LEGACY_HEADER_SIZE_DELTA;
		kmsg->ikm_header = (mach_msg_header_t *)legacy_header;
	}
#endif

	/* unreachable if !DEBUG */
	__unreachable_ok_push
	if (DEBUG_KPRINT_SYSCALL_PREDICATE(DEBUG_KPRINT_SYSCALL_IPC_MASK)) {
		kprintf("ipc_kmsg_put header+body: %d\n", (size));
		uint32_t i;
		for (i = 0; i * 4 < size; i++) {
			kprintf("%.4x\n", ((uint32_t *)kmsg->ikm_header)[i]);
		}
		kprintf("type: %d\n", ((mach_msg_type_descriptor_t *)(((mach_msg_base_t *)kmsg->ikm_header) + 1))->type);
	}
	__unreachable_ok_pop

	/* Re-Compute target address if using stack-style delivery */
	if (option & MACH_RCV_STACK) {
		rcv_addr += rcv_size - size;
	}

	if (copyoutmsg((const char *) kmsg->ikm_header, rcv_addr, size)) {
		mr = MACH_RCV_INVALID_DATA;
		size = 0;
	} else {
		mr = MACH_MSG_SUCCESS;
	}

	KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_KMSG_LINK) | DBG_FUNC_NONE,
	    (rcv_addr >= VM_MIN_KERNEL_AND_KEXT_ADDRESS ||
	    rcv_addr + size >= VM_MIN_KERNEL_AND_KEXT_ADDRESS) ? (uintptr_t)0 : (uintptr_t)rcv_addr,
	    VM_KERNEL_ADDRPERM((uintptr_t)kmsg),
	    1 /* this is on the receive/copyout path */,
	    0,
	    0);
	ipc_kmsg_free(kmsg);

	if (sizep) {
		*sizep = size;
	}
	return mr;
}

/*
 *	Routine:	ipc_kmsg_put_to_kernel
 *	Purpose:
 *		Copies a message buffer to a kernel message.
 *		Frees the message buffer.
 *		No errors allowed.
 *	Conditions:
 *		Nothing locked.
 */

void
ipc_kmsg_put_to_kernel(
	mach_msg_header_t       *msg,
	ipc_kmsg_t              kmsg,
	mach_msg_size_t         size)
{
	(void) memcpy((void *) msg, (const void *) kmsg->ikm_header, size);

	ipc_kmsg_free(kmsg);
}

static pthread_priority_compact_t
ipc_get_current_thread_priority(void)
{
	thread_t thread = current_thread();
	thread_qos_t qos;
	int relpri;

	qos = thread_get_requested_qos(thread, &relpri);
	if (!qos) {
		qos = thread_user_promotion_qos_for_pri(thread->base_pri);
		relpri = 0;
	}
	return _pthread_priority_make_from_thread_qos(qos, relpri, 0);
}

static kern_return_t
ipc_kmsg_set_qos(
	ipc_kmsg_t kmsg,
	mach_msg_option_t options,
	mach_msg_priority_t priority)
{
	kern_return_t kr;
	ipc_port_t special_reply_port = kmsg->ikm_header->msgh_local_port;
	ipc_port_t dest_port = kmsg->ikm_header->msgh_remote_port;

	if ((options & MACH_SEND_OVERRIDE) &&
	    !mach_msg_priority_is_pthread_priority(priority)) {
		mach_msg_qos_t qos = mach_msg_priority_qos(priority);
		int relpri = mach_msg_priority_relpri(priority);
		mach_msg_qos_t ovr = mach_msg_priority_overide_qos(priority);

		kmsg->ikm_ppriority = _pthread_priority_make_from_thread_qos(qos, relpri, 0);
		kmsg->ikm_qos_override = MAX(qos, ovr);
	} else {
		kr = ipc_get_pthpriority_from_kmsg_voucher(kmsg, &kmsg->ikm_ppriority);
		if (kr != KERN_SUCCESS) {
			if (options & MACH_SEND_PROPAGATE_QOS) {
				kmsg->ikm_ppriority = ipc_get_current_thread_priority();
			} else {
				kmsg->ikm_ppriority = MACH_MSG_PRIORITY_UNSPECIFIED;
			}
		}

		if (options & MACH_SEND_OVERRIDE) {
			mach_msg_qos_t qos = _pthread_priority_thread_qos(kmsg->ikm_ppriority);
			mach_msg_qos_t ovr = _pthread_priority_thread_qos(priority);
			kmsg->ikm_qos_override = MAX(qos, ovr);
		} else {
			kmsg->ikm_qos_override = _pthread_priority_thread_qos(kmsg->ikm_ppriority);
		}
	}

	kr = KERN_SUCCESS;

	if (IP_VALID(special_reply_port) &&
	    MACH_MSGH_BITS_LOCAL(kmsg->ikm_header->msgh_bits) == MACH_MSG_TYPE_PORT_SEND_ONCE) {
		if ((options & MACH_SEND_SYNC_OVERRIDE)) {
			boolean_t sync_bootstrap_checkin = !!(options & MACH_SEND_SYNC_BOOTSTRAP_CHECKIN);
			/*
			 * Link the destination port to special reply port and make sure that
			 * dest port has a send turnstile, else allocate one.
			 */
			ipc_port_link_special_reply_port(special_reply_port, dest_port, sync_bootstrap_checkin);
		}
	}
	return kr;
}

/*
 *	Routine:	ipc_kmsg_link_reply_context_locked
 *	Purpose:
 *		Link any required context from the sending voucher
 *		to the reply port. The ipc_kmsg_copyin function will
 *		enforce that the sender calls mach_msg in this context.
 *	Conditions:
 *		reply port is locked
 */
static void
ipc_kmsg_link_reply_context_locked(
	ipc_port_t reply_port,
	ipc_port_t voucher_port)
{
	kern_return_t __assert_only kr;
	uint32_t persona_id = 0;
	ipc_voucher_t voucher;

	ip_lock_held(reply_port);

	if (!ip_active(reply_port)) {
		return;
	}

	voucher = convert_port_to_voucher(voucher_port);

	kr = bank_get_bank_ledger_thread_group_and_persona(voucher, NULL, NULL, &persona_id);
	assert(kr == KERN_SUCCESS);
	ipc_voucher_release(voucher);

	if (persona_id == 0 || persona_id == PERSONA_ID_NONE) {
		/* there was no persona context to record */
		return;
	}

	/*
	 * Set the persona_id as the context on the reply port.
	 * This will force the thread that replies to have adopted a voucher
	 * with a matching persona.
	 */
	reply_port->ip_reply_context = persona_id;

	return;
}

static kern_return_t
ipc_kmsg_validate_reply_port_locked(ipc_port_t reply_port, mach_msg_option_t options)
{
	ip_lock_held(reply_port);

	if (!ip_active(reply_port)) {
		/*
		 * Ideally, we would enforce that the reply receive right is
		 * active, but asynchronous XPC cancellation destroys the
		 * receive right, so we just have to return success here.
		 */
		return KERN_SUCCESS;
	}

	if (options & MACH_SEND_MSG) {
		/*
		 * If the rely port is active, then it should not be
		 * in-transit, and the receive right should be in the caller's
		 * IPC space.
		 */
		if (!reply_port->ip_receiver_name || reply_port->ip_receiver != current_task()->itk_space) {
			return KERN_INVALID_CAPABILITY;
		}

		/*
		 * A port used as a reply port in an RPC should have exactly 1
		 * extant send-once right which we either just made or are
		 * moving as part of the IPC.
		 */
		if (reply_port->ip_sorights != 1) {
			return KERN_INVALID_CAPABILITY;
		}
		/*
		 * XPC uses an extra send-right to keep the name of the reply
		 * right around through cancellation.  That makes it harder to
		 * enforce a particular semantic kere, so for now, we say that
		 * you can have a maximum of 1 send right (in addition to your
		 * send once right). In the future, it would be great to lock
		 * this down even further.
		 */
		if (reply_port->ip_srights > 1) {
			return KERN_INVALID_CAPABILITY;
		}

		/*
		 * The sender can also specify that the receive right should
		 * be immovable. Note that this check only applies to
		 * send-only operations. Combined send/receive or rcv-only
		 * operations can specify an immovable receive right by
		 * opt-ing into guarded descriptors (MACH_RCV_GUARDED_DESC)
		 * and using the MACH_MSG_STRICT_REPLY options flag.
		 */
		if (MACH_SEND_REPLY_IS_IMMOVABLE(options)) {
			if (!reply_port->ip_immovable_receive) {
				return KERN_INVALID_CAPABILITY;
			}
		}
	}

	/*
	 * don't enforce this yet: need a better way of indicating the
	 * receiver wants this...
	 */
#if 0
	if (MACH_RCV_WITH_IMMOVABLE_REPLY(options)) {
		if (!reply_port->ip_immovable_receive) {
			return KERN_INVALID_CAPABILITY;
		}
	}
#endif /* 0  */

	return KERN_SUCCESS;
}

/*
 *	Routine:	ipc_kmsg_validate_reply_context_locked
 *	Purpose:
 *		Validate that the current thread is running in the context
 *		required by the destination port.
 *	Conditions:
 *		dest_port is locked
 *	Returns:
 *		MACH_MSG_SUCCESS on success.
 *		On error, an EXC_GUARD exception is also raised.
 *		This function *always* resets the port reply context.
 */
static mach_msg_return_t
ipc_kmsg_validate_reply_context_locked(
	mach_msg_option_t option,
	ipc_port_t dest_port,
	ipc_voucher_t voucher,
	mach_port_name_t voucher_name)
{
	uint32_t dest_ctx = dest_port->ip_reply_context;
	dest_port->ip_reply_context = 0;

	if (!ip_active(dest_port)) {
		return MACH_MSG_SUCCESS;
	}

	if (voucher == IPC_VOUCHER_NULL || !MACH_PORT_VALID(voucher_name)) {
		if ((option & MACH_SEND_KERNEL) == 0) {
			mach_port_guard_exception(voucher_name, 0,
			    (MPG_FLAGS_STRICT_REPLY_INVALID_VOUCHER | dest_ctx),
			    kGUARD_EXC_STRICT_REPLY);
		}
		return MACH_SEND_INVALID_CONTEXT;
	}

	kern_return_t __assert_only kr;
	uint32_t persona_id = 0;
	kr = bank_get_bank_ledger_thread_group_and_persona(voucher, NULL, NULL, &persona_id);
	assert(kr == KERN_SUCCESS);

	if (dest_ctx != persona_id) {
		if ((option & MACH_SEND_KERNEL) == 0) {
			mach_port_guard_exception(voucher_name, 0,
			    (MPG_FLAGS_STRICT_REPLY_MISMATCHED_PERSONA | ((((uint64_t)persona_id << 32) & MPG_FLAGS_STRICT_REPLY_MASK) | dest_ctx)),
			    kGUARD_EXC_STRICT_REPLY);
		}
		return MACH_SEND_INVALID_CONTEXT;
	}

	return MACH_MSG_SUCCESS;
}

/*
 *	Routine:	ipc_kmsg_copyin_header
 *	Purpose:
 *		"Copy-in" port rights in the header of a message.
 *		Operates atomically; if it doesn't succeed the
 *		message header and the space are left untouched.
 *		If it does succeed the remote/local port fields
 *		contain object pointers instead of port names,
 *		and the bits field is updated.  The destination port
 *		will be a valid port pointer.
 *
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	Successful copyin.
 *		MACH_SEND_INVALID_HEADER
 *			Illegal value in the message header bits.
 *		MACH_SEND_INVALID_DEST	The space is dead.
 *		MACH_SEND_INVALID_DEST	Can't copyin destination port.
 *			(Either KERN_INVALID_NAME or KERN_INVALID_RIGHT.)
 *		MACH_SEND_INVALID_REPLY	Can't copyin reply port.
 *			(Either KERN_INVALID_NAME or KERN_INVALID_RIGHT.)
 */

mach_msg_return_t
ipc_kmsg_copyin_header(
	ipc_kmsg_t              kmsg,
	ipc_space_t             space,
	mach_msg_priority_t     priority,
	mach_msg_option_t       *optionp)
{
	mach_msg_header_t *msg = kmsg->ikm_header;
	mach_msg_bits_t mbits = msg->msgh_bits & MACH_MSGH_BITS_USER;
	mach_port_name_t dest_name = CAST_MACH_PORT_TO_NAME(msg->msgh_remote_port);
	mach_port_name_t reply_name = CAST_MACH_PORT_TO_NAME(msg->msgh_local_port);
	mach_port_name_t voucher_name = MACH_PORT_NULL;
	kern_return_t kr;

	mach_msg_type_name_t dest_type = MACH_MSGH_BITS_REMOTE(mbits);
	mach_msg_type_name_t reply_type = MACH_MSGH_BITS_LOCAL(mbits);
	mach_msg_type_name_t voucher_type = MACH_MSGH_BITS_VOUCHER(mbits);
	ipc_object_t dest_port = IO_NULL;
	ipc_object_t reply_port = IO_NULL;
	ipc_port_t dest_soright = IP_NULL;
	ipc_port_t reply_soright = IP_NULL;
	ipc_port_t voucher_soright = IP_NULL;
	ipc_port_t release_port = IP_NULL;
	ipc_port_t voucher_port = IP_NULL;
	ipc_port_t voucher_release_port = IP_NULL;
	ipc_entry_t dest_entry = IE_NULL;
	ipc_entry_t reply_entry = IE_NULL;
	ipc_entry_t voucher_entry = IE_NULL;

	int assertcnt = 0;
#if IMPORTANCE_INHERITANCE
	boolean_t needboost = FALSE;
#endif /* IMPORTANCE_INHERITANCE */

	if ((mbits != msg->msgh_bits) ||
	    (!MACH_MSG_TYPE_PORT_ANY_SEND(dest_type)) ||
	    ((reply_type == 0) ?
	    (reply_name != MACH_PORT_NULL) :
	    !MACH_MSG_TYPE_PORT_ANY_SEND(reply_type))) {
		return MACH_SEND_INVALID_HEADER;
	}

	if (!MACH_PORT_VALID(dest_name)) {
		return MACH_SEND_INVALID_DEST;
	}

	is_write_lock(space);
	if (!is_active(space)) {
		is_write_unlock(space);
		return MACH_SEND_INVALID_DEST;
	}
	/* space locked and active */

	/*
	 *	If there is a voucher specified, make sure the disposition is
	 *	valid and the entry actually refers to a voucher port.  Don't
	 *	actually copy in until we validate destination and reply.
	 */
	if (voucher_type != MACH_MSGH_BITS_ZERO) {
		voucher_name = msg->msgh_voucher_port;

		if (voucher_name == MACH_PORT_DEAD ||
		    (voucher_type != MACH_MSG_TYPE_MOVE_SEND &&
		    voucher_type != MACH_MSG_TYPE_COPY_SEND)) {
			is_write_unlock(space);
			if ((*optionp & MACH_SEND_KERNEL) == 0) {
				mach_port_guard_exception(voucher_name, 0, 0, kGUARD_EXC_SEND_INVALID_VOUCHER);
			}
			return MACH_SEND_INVALID_VOUCHER;
		}

		if (voucher_name != MACH_PORT_NULL) {
			voucher_entry = ipc_entry_lookup(space, voucher_name);
			if (voucher_entry == IE_NULL ||
			    (voucher_entry->ie_bits & MACH_PORT_TYPE_SEND) == 0 ||
			    io_kotype(voucher_entry->ie_object) != IKOT_VOUCHER) {
				is_write_unlock(space);
				if ((*optionp & MACH_SEND_KERNEL) == 0) {
					mach_port_guard_exception(voucher_name, 0, 0, kGUARD_EXC_SEND_INVALID_VOUCHER);
				}
				return MACH_SEND_INVALID_VOUCHER;
			}
		} else {
			voucher_type = MACH_MSG_TYPE_MOVE_SEND;
		}
	}

	if (enforce_strict_reply && MACH_SEND_WITH_STRICT_REPLY(*optionp) &&
	    (!MACH_PORT_VALID(reply_name) ||
	    ((reply_type != MACH_MSG_TYPE_MAKE_SEND_ONCE) && (reply_type != MACH_MSG_TYPE_MOVE_SEND_ONCE))
	    )) {
		/*
		 * The caller cannot enforce a reply context with an invalid
		 * reply port name, or a non-send_once reply disposition.
		 */
		is_write_unlock(space);
		if ((*optionp & MACH_SEND_KERNEL) == 0) {
			mach_port_guard_exception(reply_name, 0,
			    (MPG_FLAGS_STRICT_REPLY_INVALID_REPLY_DISP | reply_type),
			    kGUARD_EXC_STRICT_REPLY);
		}
		return MACH_SEND_INVALID_REPLY;
	}

	/*
	 *	Handle combinations of validating destination and reply; along
	 *	with copying in destination, reply, and voucher in an atomic way.
	 */

	if (dest_name == voucher_name) {
		/*
		 *	If the destination name is the same as the voucher name,
		 *	the voucher_entry must already be known.  Either that or
		 *	the destination name is MACH_PORT_NULL (i.e. invalid).
		 */
		dest_entry = voucher_entry;
		if (dest_entry == IE_NULL) {
			goto invalid_dest;
		}

		/*
		 *	Make sure a future copyin of the reply port will succeed.
		 *	Once we start copying in the dest/voucher pair, we can't
		 *	back out.
		 */
		if (MACH_PORT_VALID(reply_name)) {
			assert(reply_type != 0); /* because reply_name not null */

			/* It is just WRONG if dest, voucher, and reply are all the same. */
			if (voucher_name == reply_name) {
				goto invalid_reply;
			}
			reply_entry = ipc_entry_lookup(space, reply_name);
			if (reply_entry == IE_NULL) {
				goto invalid_reply;
			}
			assert(dest_entry != reply_entry); /* names are not equal */
			if (!ipc_right_copyin_check_reply(space, reply_name, reply_entry, reply_type)) {
				goto invalid_reply;
			}
		}

		/*
		 *	Do the joint copyin of the dest disposition and
		 *	voucher disposition from the one entry/port.  We
		 *	already validated that the voucher copyin would
		 *	succeed (above).  So, any failure in combining
		 *	the copyins can be blamed on the destination.
		 */
		kr = ipc_right_copyin_two(space, dest_name, dest_entry,
		    dest_type, voucher_type, &dest_port, &dest_soright,
		    &release_port);
		if (kr != KERN_SUCCESS) {
			assert(kr != KERN_INVALID_CAPABILITY);
			goto invalid_dest;
		}
		voucher_port = ip_object_to_port(dest_port);

		/*
		 * could not have been one of these dispositions,
		 * validated the port was a true kernel voucher port above,
		 * AND was successfully able to copyin both dest and voucher.
		 */
		assert(dest_type != MACH_MSG_TYPE_MAKE_SEND);
		assert(dest_type != MACH_MSG_TYPE_MAKE_SEND_ONCE);
		assert(dest_type != MACH_MSG_TYPE_MOVE_SEND_ONCE);

		/*
		 *	Perform the delayed reply right copyin (guaranteed success).
		 */
		if (reply_entry != IE_NULL) {
			kr = ipc_right_copyin(space, reply_name, reply_entry,
			    reply_type, IPC_OBJECT_COPYIN_FLAGS_DEADOK,
			    &reply_port, &reply_soright,
			    &release_port, &assertcnt, 0, NULL);
			assert(assertcnt == 0);
			assert(kr == KERN_SUCCESS);
		}
	} else {
		if (dest_name == reply_name) {
			/*
			 *	Destination and reply ports are the same!
			 *	This is very similar to the case where the
			 *	destination and voucher ports were the same
			 *	(except the reply port disposition is not
			 *	previously validated).
			 */
			dest_entry = ipc_entry_lookup(space, dest_name);
			if (dest_entry == IE_NULL) {
				goto invalid_dest;
			}

			reply_entry = dest_entry;
			assert(reply_type != 0); /* because name not null */

			/*
			 *	Pre-validate that the reply right can be copied in by itself
			 */
			if (!ipc_right_copyin_check_reply(space, reply_name, reply_entry, reply_type)) {
				goto invalid_reply;
			}

			/*
			 *	Do the joint copyin of the dest disposition and
			 *	reply disposition from the one entry/port.
			 */
			kr = ipc_right_copyin_two(space, dest_name, dest_entry,
			    dest_type, reply_type, &dest_port, &dest_soright,
			    &release_port);
			if (kr == KERN_INVALID_CAPABILITY) {
				goto invalid_reply;
			} else if (kr != KERN_SUCCESS) {
				goto invalid_dest;
			}
			reply_port = dest_port;
		} else {
			/*
			 *	Handle destination and reply independently, as
			 *	they are independent entries (even if the entries
			 *	refer to the same port).
			 *
			 *	This can be the tough case to make atomic.
			 *
			 *	The difficult problem is serializing with port death.
			 *	The bad case is when dest_port dies after its copyin,
			 *	reply_port dies before its copyin, and dest_port dies before
			 *	reply_port.  Then the copyins operated as if dest_port was
			 *	alive and reply_port was dead, which shouldn't have happened
			 *	because they died in the other order.
			 *
			 *	Note that it is easy for a user task to tell if
			 *	a copyin happened before or after a port died.
			 *	If a port dies before copyin, a dead-name notification
			 *	is generated and the dead name's urefs are incremented,
			 *	and if the copyin happens first, a port-deleted
			 *	notification is generated.
			 *
			 *	Even so, avoiding that potentially detectable race is too
			 *	expensive - and no known code cares about it.  So, we just
			 *	do the expedient thing and copy them in one after the other.
			 */

			dest_entry = ipc_entry_lookup(space, dest_name);
			if (dest_entry == IE_NULL) {
				goto invalid_dest;
			}
			assert(dest_entry != voucher_entry);

			/*
			 *	Make sure reply port entry is valid before dest copyin.
			 */
			if (MACH_PORT_VALID(reply_name)) {
				if (reply_name == voucher_name) {
					goto invalid_reply;
				}
				reply_entry = ipc_entry_lookup(space, reply_name);
				if (reply_entry == IE_NULL) {
					goto invalid_reply;
				}
				assert(dest_entry != reply_entry); /* names are not equal */
				assert(reply_type != 0); /* because reply_name not null */

				if (!ipc_right_copyin_check_reply(space, reply_name, reply_entry, reply_type)) {
					goto invalid_reply;
				}
			}

			/*
			 *	copyin the destination.
			 */
			kr = ipc_right_copyin(space, dest_name, dest_entry,
			    dest_type, (IPC_OBJECT_COPYIN_FLAGS_ALLOW_IMMOVABLE_SEND |
			    IPC_OBJECT_COPYIN_FLAGS_ALLOW_DEAD_SEND_ONCE),
			    &dest_port, &dest_soright,
			    &release_port, &assertcnt, 0, NULL);
			assert(assertcnt == 0);
			if (kr != KERN_SUCCESS) {
				goto invalid_dest;
			}
			assert(IO_VALID(dest_port));
			assert(!IP_VALID(release_port));

			/*
			 *	Copyin the pre-validated reply right.
			 *	It's OK if the reply right has gone dead in the meantime.
			 */
			if (MACH_PORT_VALID(reply_name)) {
				kr = ipc_right_copyin(space, reply_name, reply_entry,
				    reply_type, IPC_OBJECT_COPYIN_FLAGS_DEADOK,
				    &reply_port, &reply_soright,
				    &release_port, &assertcnt, 0, NULL);
				assert(assertcnt == 0);
				assert(kr == KERN_SUCCESS);
			} else {
				/* convert invalid name to equivalent ipc_object type */
				reply_port = ip_to_object(CAST_MACH_NAME_TO_PORT(reply_name));
			}
		}

		/*
		 * Finally can copyin the voucher right now that dest and reply
		 * are fully copied in (guaranteed success).
		 */
		if (IE_NULL != voucher_entry) {
			kr = ipc_right_copyin(space, voucher_name, voucher_entry,
			    voucher_type, IPC_OBJECT_COPYIN_FLAGS_NONE,
			    (ipc_object_t *)&voucher_port,
			    &voucher_soright,
			    &voucher_release_port,
			    &assertcnt, 0, NULL);
			assert(assertcnt == 0);
			assert(KERN_SUCCESS == kr);
			assert(IP_VALID(voucher_port));
			require_ip_active(voucher_port);
		}
	}

	/*
	 * The entries might need to be deallocated.
	 *
	 * Each entry should be deallocated only once,
	 * even if it was specified in more than one slot in the header.
	 * Note that dest can be the same entry as reply or voucher,
	 * but reply and voucher must be distinct entries.
	 */
	assert(IE_NULL != dest_entry);
	if (IE_NULL != reply_entry) {
		assert(reply_entry != voucher_entry);
	}

	if (IE_BITS_TYPE(dest_entry->ie_bits) == MACH_PORT_TYPE_NONE) {
		ipc_entry_dealloc(space, dest_name, dest_entry);

		if (dest_entry == reply_entry) {
			reply_entry = IE_NULL;
		}

		if (dest_entry == voucher_entry) {
			voucher_entry = IE_NULL;
		}

		dest_entry = IE_NULL;
	}
	if (IE_NULL != reply_entry &&
	    IE_BITS_TYPE(reply_entry->ie_bits) == MACH_PORT_TYPE_NONE) {
		ipc_entry_dealloc(space, reply_name, reply_entry);
		reply_entry = IE_NULL;
	}
	if (IE_NULL != voucher_entry &&
	    IE_BITS_TYPE(voucher_entry->ie_bits) == MACH_PORT_TYPE_NONE) {
		ipc_entry_dealloc(space, voucher_name, voucher_entry);
		voucher_entry = IE_NULL;
	}

	dest_type = ipc_object_copyin_type(dest_type);
	reply_type = ipc_object_copyin_type(reply_type);

	/*
	 *	If the dest port is a kobject AND its receive right belongs to kernel, allow
	 *  copyin of immovable send rights in the message body (port descriptor) to
	 *  succeed since those send rights are simply "moved" or "copied" into kernel.
	 *
	 *  See: ipc_object_copyin().
	 */
	if (io_is_kobject(dest_port) &&
	    ip_object_to_port(dest_port)->ip_receiver == ipc_space_kernel) {
		assert(io_kotype(dest_port) != IKOT_HOST_NOTIFY && io_kotype(dest_port) != IKOT_TIMER);
		kmsg->ikm_flags |= IPC_OBJECT_COPYIN_FLAGS_ALLOW_IMMOVABLE_SEND;
	}

	/*
	 * JMM - Without rdar://problem/6275821, this is the last place we can
	 * re-arm the send-possible notifications.  It may trigger unexpectedly
	 * early (send may NOT have failed), but better than missing.  We assure
	 * we won't miss by forcing MACH_SEND_ALWAYS if we got past arming.
	 */
	if (((*optionp & MACH_SEND_NOTIFY) != 0) &&
	    dest_type != MACH_MSG_TYPE_PORT_SEND_ONCE &&
	    dest_entry != IE_NULL && dest_entry->ie_request != IE_REQ_NONE) {
		ipc_port_t dport = ip_object_to_port(dest_port);

		assert(dport != IP_NULL);
		ip_lock(dport);
		if (ip_active(dport) && dport->ip_receiver != ipc_space_kernel) {
			if (ip_full(dport)) {
#if IMPORTANCE_INHERITANCE
				needboost = ipc_port_request_sparm(dport, dest_name,
				    dest_entry->ie_request,
				    *optionp,
				    priority);
				if (needboost == FALSE) {
					ip_unlock(dport);
				}
#else
				ipc_port_request_sparm(dport, dest_name,
				    dest_entry->ie_request,
				    *optionp,
				    priority);
				ip_unlock(dport);
#endif /* IMPORTANCE_INHERITANCE */
			} else {
				*optionp |= MACH_SEND_ALWAYS;
				ip_unlock(dport);
			}
		} else {
			ip_unlock(dport);
		}
	}

	is_write_unlock(space);

#if IMPORTANCE_INHERITANCE
	/*
	 * If our request is the first boosting send-possible
	 * notification this cycle, push the boost down the
	 * destination port.
	 */
	if (needboost == TRUE) {
		ipc_port_t dport = ip_object_to_port(dest_port);

		/* dport still locked from above */
		if (ipc_port_importance_delta(dport, IPID_OPTION_SENDPOSSIBLE, 1) == FALSE) {
			ip_unlock(dport);
		}
	}
#endif /* IMPORTANCE_INHERITANCE */

	if (dest_soright != IP_NULL) {
		ipc_notify_port_deleted(dest_soright, dest_name);
	}
	if (reply_soright != IP_NULL) {
		ipc_notify_port_deleted(reply_soright, reply_name);
	}
	if (voucher_soright != IP_NULL) {
		ipc_notify_port_deleted(voucher_soright, voucher_name);
	}

	/*
	 * No room to store voucher port in in-kernel msg header,
	 * so we store it back in the kmsg itself.  Extract the
	 * qos, and apply any override before we enqueue the kmsg.
	 */
	if (IP_VALID(voucher_port)) {
		kmsg->ikm_voucher = voucher_port;
		voucher_type = MACH_MSG_TYPE_MOVE_SEND;
	}

	msg->msgh_bits = MACH_MSGH_BITS_SET(dest_type, reply_type, voucher_type, mbits);
	msg->msgh_remote_port = ip_object_to_port(dest_port);
	msg->msgh_local_port = ip_object_to_port(reply_port);

	/* capture the qos value(s) for the kmsg */
	ipc_kmsg_set_qos(kmsg, *optionp, priority);

	if (release_port != IP_NULL) {
		ip_release(release_port);
	}

	if (voucher_release_port != IP_NULL) {
		ip_release(voucher_release_port);
	}

	if (enforce_strict_reply && MACH_SEND_WITH_STRICT_REPLY(*optionp) && IP_VALID(msg->msgh_local_port)) {
		/*
		 * We've already validated that the reply disposition is a
		 * [make/move] send-once. Ideally, we should enforce that the
		 * reply port is also not dead, but XPC asynchronous
		 * cancellation can make the reply port dead before we
		 * actually make it to the mach_msg send.
		 *
		 * Here, we ensure that if we have a non-dead reply port, then
		 * the reply port's receive right should not be in-transit,
		 * and should live in the caller's IPC space.
		 */
		ipc_port_t rport = msg->msgh_local_port;
		ip_lock(rport);
		kr = ipc_kmsg_validate_reply_port_locked(rport, *optionp);
		ip_unlock(rport);
		if (kr != KERN_SUCCESS) {
			/*
			 * no descriptors have been copied in yet, but the
			 * full header has been copied in: clean it up
			 */
			ipc_kmsg_clean_partial(kmsg, 0, NULL, 0, 0);
			if ((*optionp & MACH_SEND_KERNEL) == 0) {
				mach_port_guard_exception(reply_name, 0,
				    (MPG_FLAGS_STRICT_REPLY_INVALID_REPLY_PORT | kr),
				    kGUARD_EXC_STRICT_REPLY);
			}
			return MACH_SEND_INVALID_REPLY;
		}
	}

	return MACH_MSG_SUCCESS;

invalid_reply:
	is_write_unlock(space);

	if (release_port != IP_NULL) {
		ip_release(release_port);
	}

	assert(voucher_port == IP_NULL);
	assert(voucher_soright == IP_NULL);

	if ((*optionp & MACH_SEND_KERNEL) == 0) {
		mach_port_guard_exception(reply_name, 0, 0, kGUARD_EXC_SEND_INVALID_REPLY);
	}
	return MACH_SEND_INVALID_REPLY;

invalid_dest:
	is_write_unlock(space);

	if (release_port != IP_NULL) {
		ip_release(release_port);
	}

	if (reply_soright != IP_NULL) {
		ipc_notify_port_deleted(reply_soright, reply_name);
	}

	assert(voucher_port == IP_NULL);
	assert(voucher_soright == IP_NULL);

	return MACH_SEND_INVALID_DEST;
}

static mach_msg_descriptor_t *
ipc_kmsg_copyin_port_descriptor(
	mach_msg_port_descriptor_t *dsc,
	mach_msg_legacy_port_descriptor_t *user_dsc_in,
	ipc_space_t space,
	ipc_object_t dest,
	ipc_kmsg_t kmsg,
	mach_msg_option_t *optionp,
	mach_msg_return_t *mr)
{
	mach_msg_legacy_port_descriptor_t user_dsc = *user_dsc_in;
	mach_msg_type_name_t        user_disp;
	mach_msg_type_name_t        result_disp;
	mach_port_name_t            name;
	ipc_object_t                        object;

	user_disp = user_dsc.disposition;
	result_disp = ipc_object_copyin_type(user_disp);

	name = (mach_port_name_t)user_dsc.name;
	if (MACH_PORT_VALID(name)) {
		kern_return_t kr = ipc_object_copyin(space, name, user_disp, &object, 0, NULL, kmsg->ikm_flags);
		if (kr != KERN_SUCCESS) {
			if (((*optionp & MACH_SEND_KERNEL) == 0) && (kr == KERN_INVALID_RIGHT)) {
				mach_port_guard_exception(name, 0, 0, kGUARD_EXC_SEND_INVALID_RIGHT);
			}
			*mr = MACH_SEND_INVALID_RIGHT;
			return NULL;
		}

		if ((result_disp == MACH_MSG_TYPE_PORT_RECEIVE) &&
		    ipc_port_check_circularity(ip_object_to_port(object),
		    ip_object_to_port(dest))) {
			kmsg->ikm_header->msgh_bits |= MACH_MSGH_BITS_CIRCULAR;
		}
		dsc->name = ip_object_to_port(object);
	} else {
		dsc->name = CAST_MACH_NAME_TO_PORT(name);
	}
	dsc->disposition = result_disp;
	dsc->type = MACH_MSG_PORT_DESCRIPTOR;

	dsc->pad_end = 0;         // debug, unnecessary

	return (mach_msg_descriptor_t *)(user_dsc_in + 1);
}

static mach_msg_descriptor_t *
ipc_kmsg_copyin_ool_descriptor(
	mach_msg_ool_descriptor_t *dsc,
	mach_msg_descriptor_t *user_dsc,
	int is_64bit,
	vm_offset_t *paddr,
	vm_map_copy_t *copy,
	vm_size_t *space_needed,
	vm_map_t map,
	__unused mach_msg_option_t *optionp,
	mach_msg_return_t *mr)
{
	vm_size_t                           length;
	boolean_t                           dealloc;
	mach_msg_copy_options_t             copy_options;
	mach_vm_offset_t            addr;
	mach_msg_descriptor_type_t  dsc_type;

	if (is_64bit) {
		mach_msg_ool_descriptor64_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;

		addr = (mach_vm_offset_t) user_ool_dsc->address;
		length = user_ool_dsc->size;
		dealloc = user_ool_dsc->deallocate;
		copy_options = user_ool_dsc->copy;
		dsc_type = user_ool_dsc->type;

		user_dsc = (typeof(user_dsc))(user_ool_dsc + 1);
	} else {
		mach_msg_ool_descriptor32_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;

		addr = CAST_USER_ADDR_T(user_ool_dsc->address);
		dealloc = user_ool_dsc->deallocate;
		copy_options = user_ool_dsc->copy;
		dsc_type = user_ool_dsc->type;
		length = user_ool_dsc->size;

		user_dsc = (typeof(user_dsc))(user_ool_dsc + 1);
	}

	dsc->size = (mach_msg_size_t)length;
	dsc->deallocate = dealloc;
	dsc->copy = copy_options;
	dsc->type = dsc_type;

	if (length == 0) {
		dsc->address = NULL;
	} else if ((length >= MSG_OOL_SIZE_SMALL) &&
	    (copy_options == MACH_MSG_PHYSICAL_COPY) && !dealloc) {
		/*
		 * If the request is a physical copy and the source
		 * is not being deallocated, then allocate space
		 * in the kernel's pageable ipc copy map and copy
		 * the data in.  The semantics guarantee that the
		 * data will have been physically copied before
		 * the send operation terminates.  Thus if the data
		 * is not being deallocated, we must be prepared
		 * to page if the region is sufficiently large.
		 */
		if (copyin(addr, (char *)*paddr, length)) {
			*mr = MACH_SEND_INVALID_MEMORY;
			return NULL;
		}

		/*
		 * The kernel ipc copy map is marked no_zero_fill.
		 * If the transfer is not a page multiple, we need
		 * to zero fill the balance.
		 */
		if (!page_aligned(length)) {
			(void) memset((void *) (*paddr + length), 0,
			    round_page(length) - length);
		}
		if (vm_map_copyin(ipc_kernel_copy_map, (vm_map_address_t)*paddr,
		    (vm_map_size_t)length, TRUE, copy) != KERN_SUCCESS) {
			*mr = MACH_MSG_VM_KERNEL;
			return NULL;
		}
		dsc->address = (void *)*copy;
		*paddr += round_page(length);
		*space_needed -= round_page(length);
	} else {
		/*
		 * Make a vm_map_copy_t of the of the data.  If the
		 * data is small, this will do an optimized physical
		 * copy.  Otherwise, it will do a virtual copy.
		 *
		 * NOTE: A virtual copy is OK if the original is being
		 * deallocted, even if a physical copy was requested.
		 */
		kern_return_t kr = vm_map_copyin(map, addr,
		    (vm_map_size_t)length, dealloc, copy);
		if (kr != KERN_SUCCESS) {
			*mr = (kr == KERN_RESOURCE_SHORTAGE) ?
			    MACH_MSG_VM_KERNEL :
			    MACH_SEND_INVALID_MEMORY;
			return NULL;
		}
		dsc->address = (void *)*copy;
	}

	return user_dsc;
}

static mach_msg_descriptor_t *
ipc_kmsg_copyin_ool_ports_descriptor(
	mach_msg_ool_ports_descriptor_t *dsc,
	mach_msg_descriptor_t *user_dsc,
	int is_64bit,
	vm_map_t map,
	ipc_space_t space,
	ipc_object_t dest,
	ipc_kmsg_t kmsg,
	mach_msg_option_t *optionp,
	mach_msg_return_t *mr)
{
	void *data;
	ipc_object_t *objects;
	unsigned int i;
	mach_vm_offset_t addr;
	mach_msg_type_name_t user_disp;
	mach_msg_type_name_t result_disp;
	mach_msg_type_number_t count;
	mach_msg_copy_options_t copy_option;
	boolean_t deallocate;
	mach_msg_descriptor_type_t type;
	vm_size_t ports_length, names_length;

	if (is_64bit) {
		mach_msg_ool_ports_descriptor64_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;

		addr = (mach_vm_offset_t)user_ool_dsc->address;
		count = user_ool_dsc->count;
		deallocate = user_ool_dsc->deallocate;
		copy_option = user_ool_dsc->copy;
		user_disp = user_ool_dsc->disposition;
		type = user_ool_dsc->type;

		user_dsc = (typeof(user_dsc))(user_ool_dsc + 1);
	} else {
		mach_msg_ool_ports_descriptor32_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;

		addr = CAST_USER_ADDR_T(user_ool_dsc->address);
		count = user_ool_dsc->count;
		deallocate = user_ool_dsc->deallocate;
		copy_option = user_ool_dsc->copy;
		user_disp = user_ool_dsc->disposition;
		type = user_ool_dsc->type;

		user_dsc = (typeof(user_dsc))(user_ool_dsc + 1);
	}

	dsc->deallocate = deallocate;
	dsc->copy = copy_option;
	dsc->type = type;
	dsc->count = count;
	dsc->address = NULL; /* for now */

	result_disp = ipc_object_copyin_type(user_disp);
	dsc->disposition = result_disp;

	/* We always do a 'physical copy', but you have to specify something valid */
	if (copy_option != MACH_MSG_PHYSICAL_COPY &&
	    copy_option != MACH_MSG_VIRTUAL_COPY) {
		*mr = MACH_SEND_INVALID_TYPE;
		return NULL;
	}

	/* calculate length of data in bytes, rounding up */

	if (os_mul_overflow(count, sizeof(mach_port_t), &ports_length)) {
		*mr = MACH_SEND_TOO_LARGE;
		return NULL;
	}

	if (os_mul_overflow(count, sizeof(mach_port_name_t), &names_length)) {
		*mr = MACH_SEND_TOO_LARGE;
		return NULL;
	}

	if (ports_length == 0) {
		return user_dsc;
	}

	data = kalloc(ports_length);

	if (data == NULL) {
		*mr = MACH_SEND_NO_BUFFER;
		return NULL;
	}

#ifdef __LP64__
	mach_port_name_t *names = &((mach_port_name_t *)data)[count];
#else
	mach_port_name_t *names = ((mach_port_name_t *)data);
#endif

	if (copyinmap(map, addr, names, names_length) != KERN_SUCCESS) {
		kfree(data, ports_length);
		*mr = MACH_SEND_INVALID_MEMORY;
		return NULL;
	}

	if (deallocate) {
		(void) mach_vm_deallocate(map, addr, (mach_vm_size_t)names_length);
	}

	objects = (ipc_object_t *) data;
	dsc->address = data;

	for (i = 0; i < count; i++) {
		mach_port_name_t name = names[i];
		ipc_object_t object;

		if (!MACH_PORT_VALID(name)) {
			objects[i] = ip_to_object(CAST_MACH_NAME_TO_PORT(name));
			continue;
		}

		kern_return_t kr = ipc_object_copyin(space, name, user_disp, &object, 0, NULL, kmsg->ikm_flags);

		if (kr != KERN_SUCCESS) {
			unsigned int j;

			for (j = 0; j < i; j++) {
				object = objects[j];
				if (IPC_OBJECT_VALID(object)) {
					ipc_object_destroy(object, result_disp);
				}
			}
			kfree(data, ports_length);
			dsc->address = NULL;
			if (((*optionp & MACH_SEND_KERNEL) == 0) && (kr == KERN_INVALID_RIGHT)) {
				mach_port_guard_exception(name, 0, 0, kGUARD_EXC_SEND_INVALID_RIGHT);
			}
			*mr = MACH_SEND_INVALID_RIGHT;
			return NULL;
		}

		if ((dsc->disposition == MACH_MSG_TYPE_PORT_RECEIVE) &&
		    ipc_port_check_circularity(ip_object_to_port(object),
		    ip_object_to_port(dest))) {
			kmsg->ikm_header->msgh_bits |= MACH_MSGH_BITS_CIRCULAR;
		}

		objects[i] = object;
	}

	return user_dsc;
}

static mach_msg_descriptor_t *
ipc_kmsg_copyin_guarded_port_descriptor(
	mach_msg_guarded_port_descriptor_t *dsc,
	mach_msg_descriptor_t *user_addr,
	int is_64bit,
	ipc_space_t space,
	ipc_object_t dest,
	ipc_kmsg_t kmsg,
	mach_msg_option_t *optionp,
	mach_msg_return_t *mr)
{
	mach_msg_descriptor_t       *user_dsc;
	mach_msg_type_name_t        disp;
	mach_msg_type_name_t        result_disp;
	mach_port_name_t            name;
	mach_msg_guard_flags_t      guard_flags;
	ipc_object_t                object;
	mach_port_context_t         context;

	if (!is_64bit) {
		mach_msg_guarded_port_descriptor32_t *user_gp_dsc = (typeof(user_gp_dsc))user_addr;
		name = user_gp_dsc->name;
		guard_flags = user_gp_dsc->flags;
		disp = user_gp_dsc->disposition;
		context = user_gp_dsc->context;
		user_dsc = (mach_msg_descriptor_t *)(user_gp_dsc + 1);
	} else {
		mach_msg_guarded_port_descriptor64_t *user_gp_dsc = (typeof(user_gp_dsc))user_addr;
		name = user_gp_dsc->name;
		guard_flags = user_gp_dsc->flags;
		disp = user_gp_dsc->disposition;
		context = user_gp_dsc->context;
		user_dsc = (mach_msg_descriptor_t *)(user_gp_dsc + 1);
	}

	guard_flags &= MACH_MSG_GUARD_FLAGS_MASK;
	result_disp = ipc_object_copyin_type(disp);

	if (MACH_PORT_VALID(name)) {
		kern_return_t kr = ipc_object_copyin(space, name, disp, &object, context, &guard_flags, kmsg->ikm_flags);
		if (kr != KERN_SUCCESS) {
			if (((*optionp & MACH_SEND_KERNEL) == 0) && (kr == KERN_INVALID_RIGHT)) {
				mach_port_guard_exception(name, 0, 0, kGUARD_EXC_SEND_INVALID_RIGHT);
			}
			*mr = MACH_SEND_INVALID_RIGHT;
			return NULL;
		}

		if ((result_disp == MACH_MSG_TYPE_PORT_RECEIVE) &&
		    ipc_port_check_circularity(ip_object_to_port(object),
		    ip_object_to_port(dest))) {
			kmsg->ikm_header->msgh_bits |= MACH_MSGH_BITS_CIRCULAR;
		}
		dsc->name = ip_object_to_port(object);
	} else {
		dsc->name = CAST_MACH_NAME_TO_PORT(name);
	}
	dsc->flags = guard_flags;
	dsc->disposition = result_disp;
	dsc->type = MACH_MSG_GUARDED_PORT_DESCRIPTOR;

#if __LP64__
	dsc->pad_end = 0;         // debug, unnecessary
#endif

	return user_dsc;
}


/*
 *	Routine:	ipc_kmsg_copyin_body
 *	Purpose:
 *		"Copy-in" port rights and out-of-line memory
 *		in the message body.
 *
 *		In all failure cases, the message is left holding
 *		no rights or memory.  However, the message buffer
 *		is not deallocated.  If successful, the message
 *		contains a valid destination port.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	Successful copyin.
 *		MACH_SEND_INVALID_MEMORY	Can't grab out-of-line memory.
 *		MACH_SEND_INVALID_RIGHT	Can't copyin port right in body.
 *		MACH_SEND_INVALID_TYPE	Bad type specification.
 *		MACH_SEND_MSG_TOO_SMALL	Body is too small for types/data.
 *		MACH_SEND_INVALID_RT_OOL_SIZE OOL Buffer too large for RT
 *		MACH_MSG_INVALID_RT_DESCRIPTOR Dealloc and RT are incompatible
 *		MACH_SEND_NO_GRANT_DEST	Dest port doesn't accept ports in body
 */

mach_msg_return_t
ipc_kmsg_copyin_body(
	ipc_kmsg_t      kmsg,
	ipc_space_t     space,
	vm_map_t    map,
	mach_msg_option_t *optionp)
{
	ipc_object_t                dest;
	mach_msg_body_t             *body;
	mach_msg_descriptor_t       *daddr, *naddr, *end;
	mach_msg_descriptor_t       *user_addr, *kern_addr;
	mach_msg_type_number_t      dsc_count;
	boolean_t                   is_task_64bit = (map->max_offset > VM_MAX_ADDRESS);
	boolean_t                   complex = FALSE;
	boolean_t                   contains_port_desc = FALSE;
	vm_size_t                   space_needed = 0;
	vm_offset_t                 paddr = 0;
	vm_map_copy_t               copy = VM_MAP_COPY_NULL;
	mach_msg_type_number_t      i;
	mach_msg_return_t           mr = MACH_MSG_SUCCESS;
	ipc_port_t                  remote_port = kmsg->ikm_header->msgh_remote_port;

	vm_size_t           descriptor_size = 0;

	mach_msg_type_number_t total_ool_port_count = 0;
	mach_msg_guard_flags_t guard_flags = 0;
	mach_port_context_t context;
	mach_msg_type_name_t disp;

	/*
	 * Determine if the target is a kernel port.
	 */
	dest = ip_to_object(remote_port);
	body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
	naddr = (mach_msg_descriptor_t *) (body + 1);
	end = (mach_msg_descriptor_t *) ((vm_offset_t)kmsg->ikm_header + kmsg->ikm_header->msgh_size);

	dsc_count = body->msgh_descriptor_count;
	if (dsc_count == 0) {
		return MACH_MSG_SUCCESS;
	}

	/*
	 * Make an initial pass to determine kernal VM space requirements for
	 * physical copies and possible contraction of the descriptors from
	 * processes with pointers larger than the kernel's.
	 */
	daddr = NULL;
	for (i = 0; i < dsc_count; i++) {
		mach_msg_size_t size;
		mach_msg_type_number_t ool_port_count = 0;

		daddr = naddr;

		/* make sure the descriptor fits in the message */
		if (is_task_64bit) {
			if ((mach_msg_descriptor_t*)((vm_offset_t)daddr + 12) > end) {
				mr = MACH_SEND_MSG_TOO_SMALL;
				goto clean_message;
			}

			switch (daddr->type.type) {
			case MACH_MSG_OOL_DESCRIPTOR:
			case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
			case MACH_MSG_OOL_PORTS_DESCRIPTOR:
			case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
				descriptor_size += 16;
				naddr = (typeof(naddr))((vm_offset_t)daddr + 16);
				break;
			default:
				descriptor_size += 12;
				naddr = (typeof(naddr))((vm_offset_t)daddr + 12);
				break;
			}
		} else {
			descriptor_size += 12;
			naddr = (typeof(naddr))((vm_offset_t)daddr + 12);
		}

		if (naddr > end) {
			mr = MACH_SEND_MSG_TOO_SMALL;
			goto clean_message;
		}

		switch (daddr->type.type) {
		case MACH_MSG_OOL_DESCRIPTOR:
		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
			size = (is_task_64bit) ?
			    ((mach_msg_ool_descriptor64_t *)daddr)->size :
			    daddr->out_of_line.size;

			if (daddr->out_of_line.copy != MACH_MSG_PHYSICAL_COPY &&
			    daddr->out_of_line.copy != MACH_MSG_VIRTUAL_COPY) {
				/*
				 * Invalid copy option
				 */
				mr = MACH_SEND_INVALID_TYPE;
				goto clean_message;
			}

			if ((size >= MSG_OOL_SIZE_SMALL) &&
			    (daddr->out_of_line.copy == MACH_MSG_PHYSICAL_COPY) &&
			    !(daddr->out_of_line.deallocate)) {
				/*
				 * Out-of-line memory descriptor, accumulate kernel
				 * memory requirements
				 */
				if (space_needed + round_page(size) <= space_needed) {
					/* Overflow dectected */
					mr = MACH_MSG_VM_KERNEL;
					goto clean_message;
				}

				space_needed += round_page(size);
				if (space_needed > ipc_kmsg_max_vm_space) {
					/* Per message kernel memory limit exceeded */
					mr = MACH_MSG_VM_KERNEL;
					goto clean_message;
				}
			}
			break;
		case MACH_MSG_PORT_DESCRIPTOR:
			if (os_add_overflow(total_ool_port_count, 1, &total_ool_port_count)) {
				/* Overflow detected */
				mr = MACH_SEND_TOO_LARGE;
				goto clean_message;
			}
			contains_port_desc = TRUE;
			break;
		case MACH_MSG_OOL_PORTS_DESCRIPTOR:
			ool_port_count = (is_task_64bit) ?
			    ((mach_msg_ool_ports_descriptor64_t *)daddr)->count :
			    daddr->ool_ports.count;

			if (os_add_overflow(total_ool_port_count, ool_port_count, &total_ool_port_count)) {
				/* Overflow detected */
				mr = MACH_SEND_TOO_LARGE;
				goto clean_message;
			}

			if (ool_port_count > (ipc_kmsg_max_vm_space / sizeof(mach_port_t))) {
				/* Per message kernel memory limit exceeded */
				mr = MACH_SEND_TOO_LARGE;
				goto clean_message;
			}
			contains_port_desc = TRUE;
			break;
		case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
			guard_flags = (is_task_64bit) ?
			    ((mach_msg_guarded_port_descriptor64_t *)daddr)->flags :
			    ((mach_msg_guarded_port_descriptor32_t *)daddr)->flags;
			context = (is_task_64bit) ?
			    ((mach_msg_guarded_port_descriptor64_t *)daddr)->context :
			    ((mach_msg_guarded_port_descriptor32_t *)daddr)->context;
			disp = (is_task_64bit) ?
			    ((mach_msg_guarded_port_descriptor64_t *)daddr)->disposition :
			    ((mach_msg_guarded_port_descriptor32_t *)daddr)->disposition;

			/* Only MACH_MSG_TYPE_MOVE_RECEIVE is supported for now */
			if (!guard_flags || ((guard_flags & ~MACH_MSG_GUARD_FLAGS_MASK) != 0) ||
			    ((guard_flags & MACH_MSG_GUARD_FLAGS_UNGUARDED_ON_SEND) && (context != 0)) ||
			    (disp != MACH_MSG_TYPE_MOVE_RECEIVE)) {
				/*
				 * Invalid guard flags, context or disposition
				 */
				mr = MACH_SEND_INVALID_TYPE;
				goto clean_message;
			}
			if (os_add_overflow(total_ool_port_count, 1, &total_ool_port_count)) {
				/* Overflow detected */
				mr = MACH_SEND_TOO_LARGE;
				goto clean_message;
			}
			contains_port_desc = TRUE;
			break;
		}
	}

	/* Sending more than 16383 rights in one message seems crazy */
	if (total_ool_port_count >= (MACH_PORT_UREFS_MAX / 4)) {
		mr = MACH_SEND_TOO_LARGE;
		goto clean_message;
	}

	/*
	 * Check if dest is a no-grant port; Since this bit is set only on
	 * port construction and cannot be unset later, we can peek at the
	 * bit without paying the cost of locking the port.
	 */
	if (contains_port_desc && remote_port->ip_no_grant) {
		mr = MACH_SEND_NO_GRANT_DEST;
		goto clean_message;
	}

	/*
	 * Allocate space in the pageable kernel ipc copy map for all the
	 * ool data that is to be physically copied.  Map is marked wait for
	 * space.
	 */
	if (space_needed) {
		if (vm_allocate_kernel(ipc_kernel_copy_map, &paddr, space_needed,
		    VM_FLAGS_ANYWHERE, VM_KERN_MEMORY_IPC) != KERN_SUCCESS) {
			mr = MACH_MSG_VM_KERNEL;
			goto clean_message;
		}
	}

	/* user_addr = just after base as it was copied in */
	user_addr = (mach_msg_descriptor_t *)((vm_offset_t)kmsg->ikm_header + sizeof(mach_msg_base_t));

	/* Shift the mach_msg_base_t down to make room for dsc_count*16bytes of descriptors on 64 bit kernels
	 */
	if (descriptor_size != 16 * dsc_count) {
		vm_offset_t dsc_adjust = 16 * dsc_count - descriptor_size;

		memmove((char *)(((vm_offset_t)kmsg->ikm_header) - dsc_adjust), kmsg->ikm_header, sizeof(mach_msg_base_t));
		kmsg->ikm_header = (mach_msg_header_t *)((vm_offset_t)kmsg->ikm_header - dsc_adjust);

		/* Update the message size for the larger in-kernel representation */
		kmsg->ikm_header->msgh_size += (mach_msg_size_t)dsc_adjust;
	}


	/* kern_addr = just after base after it has been (conditionally) moved */
	kern_addr = (mach_msg_descriptor_t *)((vm_offset_t)kmsg->ikm_header + sizeof(mach_msg_base_t));

	/* handle the OOL regions and port descriptors. */
	for (i = 0; i < dsc_count; i++) {
		switch (user_addr->type.type) {
		case MACH_MSG_PORT_DESCRIPTOR:
			user_addr = ipc_kmsg_copyin_port_descriptor((mach_msg_port_descriptor_t *)kern_addr,
			    (mach_msg_legacy_port_descriptor_t *)user_addr, space, dest, kmsg, optionp, &mr);
			kern_addr++;
			complex = TRUE;
			break;
		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
		case MACH_MSG_OOL_DESCRIPTOR:
			user_addr = ipc_kmsg_copyin_ool_descriptor((mach_msg_ool_descriptor_t *)kern_addr,
			    user_addr, is_task_64bit, &paddr, &copy, &space_needed, map, optionp, &mr);
			kern_addr++;
			complex = TRUE;
			break;
		case MACH_MSG_OOL_PORTS_DESCRIPTOR:
			user_addr = ipc_kmsg_copyin_ool_ports_descriptor((mach_msg_ool_ports_descriptor_t *)kern_addr,
			    user_addr, is_task_64bit, map, space, dest, kmsg, optionp, &mr);
			kern_addr++;
			complex = TRUE;
			break;
		case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
			user_addr = ipc_kmsg_copyin_guarded_port_descriptor((mach_msg_guarded_port_descriptor_t *)kern_addr,
			    user_addr, is_task_64bit, space, dest, kmsg, optionp, &mr);
			kern_addr++;
			complex = TRUE;
			break;
		default:
			/* Invalid descriptor */
			mr = MACH_SEND_INVALID_TYPE;
			break;
		}

		if (MACH_MSG_SUCCESS != mr) {
			/* clean from start of message descriptors to i */
			ipc_kmsg_clean_partial(kmsg, i,
			    (mach_msg_descriptor_t *)((mach_msg_base_t *)kmsg->ikm_header + 1),
			    paddr, space_needed);
			goto out;
		}
	}         /* End of loop */

	if (!complex) {
		kmsg->ikm_header->msgh_bits &= ~MACH_MSGH_BITS_COMPLEX;
	}
out:
	return mr;

clean_message:
	/* no descriptors have been copied in yet */
	ipc_kmsg_clean_partial(kmsg, 0, NULL, 0, 0);
	return mr;
}


/*
 *	Routine:	ipc_kmsg_copyin
 *	Purpose:
 *		"Copy-in" port rights and out-of-line memory
 *		in the message.
 *
 *		In all failure cases, the message is left holding
 *		no rights or memory.  However, the message buffer
 *		is not deallocated.  If successful, the message
 *		contains a valid destination port.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	Successful copyin.
 *		MACH_SEND_INVALID_HEADER Illegal value in the message header bits.
 *		MACH_SEND_INVALID_DEST	Can't copyin destination port.
 *		MACH_SEND_INVALID_REPLY	Can't copyin reply port.
 *		MACH_SEND_INVALID_MEMORY	Can't grab out-of-line memory.
 *		MACH_SEND_INVALID_RIGHT	Can't copyin port right in body.
 *		MACH_SEND_INVALID_TYPE	Bad type specification.
 *		MACH_SEND_MSG_TOO_SMALL	Body is too small for types/data.
 */

mach_msg_return_t
ipc_kmsg_copyin(
	ipc_kmsg_t              kmsg,
	ipc_space_t             space,
	vm_map_t                map,
	mach_msg_priority_t     priority,
	mach_msg_option_t       *optionp)
{
	mach_msg_return_t           mr;
	mach_port_name_t dest_name = CAST_MACH_PORT_TO_NAME(kmsg->ikm_header->msgh_remote_port);

	kmsg->ikm_header->msgh_bits &= MACH_MSGH_BITS_USER;

	mr = ipc_kmsg_copyin_header(kmsg, space, priority, optionp);

	if (mr != MACH_MSG_SUCCESS) {
		return mr;
	}

	/* Get the message filter policy if the task and port support filtering */
	mach_msg_filter_id fid = 0;
	if (ip_enforce_msg_filtering(kmsg->ikm_header->msgh_remote_port) &&
	    task_get_filter_msg_flag(current_task())) {
		/* port label is yet to be supported */
		boolean_t allow_kmsg = mach_msg_fetch_filter_policy(NULL, kmsg->ikm_header->msgh_id, &fid);
		if (!allow_kmsg) {
			mach_port_guard_exception(dest_name, 0, 0, kGUARD_EXC_MSG_FILTERED);
			/* no descriptors have been copied in yet */
			ipc_kmsg_clean_partial(kmsg, 0, NULL, 0, 0);
			return MACH_SEND_MSG_FILTERED;
		}
		kmsg->ikm_filter_policy_id = fid;
	}

	KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_MSG_SEND) | DBG_FUNC_NONE,
	    VM_KERNEL_ADDRPERM((uintptr_t)kmsg),
	    (uintptr_t)kmsg->ikm_header->msgh_bits,
	    (uintptr_t)kmsg->ikm_header->msgh_id,
	    VM_KERNEL_ADDRPERM((uintptr_t)unsafe_convert_port_to_voucher(kmsg->ikm_voucher)),
	    0);

	DEBUG_KPRINT_SYSCALL_IPC("ipc_kmsg_copyin header:\n%.8x\n%.8x\n%p\n%p\n%p\n%.8x\n",
	    kmsg->ikm_header->msgh_size,
	    kmsg->ikm_header->msgh_bits,
	    kmsg->ikm_header->msgh_remote_port,
	    kmsg->ikm_header->msgh_local_port,
	    kmsg->ikm_voucher,
	    kmsg->ikm_header->msgh_id);

	if (kmsg->ikm_header->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
		mr = ipc_kmsg_copyin_body( kmsg, space, map, optionp);

		/* unreachable if !DEBUG */
		__unreachable_ok_push
		if (DEBUG_KPRINT_SYSCALL_PREDICATE(DEBUG_KPRINT_SYSCALL_IPC_MASK)) {
			kprintf("body:\n");
			uint32_t i;
			for (i = 0; i * 4 < (kmsg->ikm_header->msgh_size - sizeof(mach_msg_header_t)); i++) {
				kprintf("%.4x\n", ((uint32_t *)(kmsg->ikm_header + 1))[i]);
			}
		}
		__unreachable_ok_pop
	}

	/* Sign the message contents */
	if (mr == MACH_MSG_SUCCESS) {
		ikm_sign(kmsg);
	}

	return mr;
}

/*
 *	Routine:	ipc_kmsg_copyin_from_kernel
 *	Purpose:
 *		"Copy-in" port rights and out-of-line memory
 *		in a message sent from the kernel.
 *
 *		Because the message comes from the kernel,
 *		the implementation assumes there are no errors
 *		or peculiarities in the message.
 *	Conditions:
 *		Nothing locked.
 */

mach_msg_return_t
ipc_kmsg_copyin_from_kernel(
	ipc_kmsg_t      kmsg)
{
	mach_msg_bits_t bits = kmsg->ikm_header->msgh_bits;
	mach_msg_type_name_t rname = MACH_MSGH_BITS_REMOTE(bits);
	mach_msg_type_name_t lname = MACH_MSGH_BITS_LOCAL(bits);
	ipc_object_t remote = ip_to_object(kmsg->ikm_header->msgh_remote_port);
	ipc_object_t local = ip_to_object(kmsg->ikm_header->msgh_local_port);
	ipc_port_t dest = kmsg->ikm_header->msgh_remote_port;

	/* translate the destination and reply ports */
	if (!IO_VALID(remote)) {
		return MACH_SEND_INVALID_DEST;
	}

	ipc_object_copyin_from_kernel(remote, rname);
	if (IO_VALID(local)) {
		ipc_object_copyin_from_kernel(local, lname);
	}

	/*
	 *	The common case is a complex message with no reply port,
	 *	because that is what the memory_object interface uses.
	 */

	if (bits == (MACH_MSGH_BITS_COMPLEX |
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0))) {
		bits = (MACH_MSGH_BITS_COMPLEX |
		    MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND, 0));

		kmsg->ikm_header->msgh_bits = bits;
	} else {
		bits = (MACH_MSGH_BITS_OTHER(bits) |
		    MACH_MSGH_BITS(ipc_object_copyin_type(rname),
		    ipc_object_copyin_type(lname)));

		kmsg->ikm_header->msgh_bits = bits;
	}

	if (bits & MACH_MSGH_BITS_COMPLEX) {
		/*
		 * Check if the remote port accepts ports in the body.
		 */
		if (dest->ip_no_grant) {
			mach_msg_descriptor_t   *saddr;
			mach_msg_body_t         *body;
			mach_msg_type_number_t  i, count;

			body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
			saddr = (mach_msg_descriptor_t *) (body + 1);
			count = body->msgh_descriptor_count;

			for (i = 0; i < count; i++, saddr++) {
				switch (saddr->type.type) {
				case MACH_MSG_PORT_DESCRIPTOR:
				case MACH_MSG_OOL_PORTS_DESCRIPTOR:
				case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
					/* no descriptors have been copied in yet */
					ipc_kmsg_clean_partial(kmsg, 0, NULL, 0, 0);
					return MACH_SEND_NO_GRANT_DEST;
				}
			}
		}

		mach_msg_descriptor_t   *saddr;
		mach_msg_body_t         *body;
		mach_msg_type_number_t  i, count;

		body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
		saddr = (mach_msg_descriptor_t *) (body + 1);
		count = body->msgh_descriptor_count;

		for (i = 0; i < count; i++, saddr++) {
			switch (saddr->type.type) {
			case MACH_MSG_PORT_DESCRIPTOR: {
				mach_msg_type_name_t        name;
				ipc_object_t                object;
				mach_msg_port_descriptor_t  *dsc;

				dsc = &saddr->port;

				/* this is really the type SEND, SEND_ONCE, etc. */
				name = dsc->disposition;
				object = ip_to_object(dsc->name);
				dsc->disposition = ipc_object_copyin_type(name);

				if (!IO_VALID(object)) {
					break;
				}

				ipc_object_copyin_from_kernel(object, name);

				/* CDY avoid circularity when the destination is also */
				/* the kernel.  This check should be changed into an  */
				/* assert when the new kobject model is in place since*/
				/* ports will not be used in kernel to kernel chats   */

				if (ip_object_to_port(remote)->ip_receiver != ipc_space_kernel) {
					if ((dsc->disposition == MACH_MSG_TYPE_PORT_RECEIVE) &&
					    ipc_port_check_circularity(ip_object_to_port(object),
					    ip_object_to_port(remote))) {
						kmsg->ikm_header->msgh_bits |=
						    MACH_MSGH_BITS_CIRCULAR;
					}
				}
				break;
			}
			case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
			case MACH_MSG_OOL_DESCRIPTOR: {
				/*
				 * The sender should supply ready-made memory, i.e.
				 * a vm_map_copy_t, so we don't need to do anything.
				 */
				break;
			}
			case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
				ipc_object_t                        *objects;
				unsigned int                        j;
				mach_msg_type_name_t                name;
				mach_msg_ool_ports_descriptor_t     *dsc;

				dsc = (mach_msg_ool_ports_descriptor_t *)&saddr->ool_ports;

				/* this is really the type SEND, SEND_ONCE, etc. */
				name = dsc->disposition;
				dsc->disposition = ipc_object_copyin_type(name);

				objects = (ipc_object_t *) dsc->address;

				for (j = 0; j < dsc->count; j++) {
					ipc_object_t object = objects[j];

					if (!IO_VALID(object)) {
						continue;
					}

					ipc_object_copyin_from_kernel(object, name);

					if ((dsc->disposition == MACH_MSG_TYPE_PORT_RECEIVE) &&
					    ipc_port_check_circularity(ip_object_to_port(object),
					    ip_object_to_port(remote))) {
						kmsg->ikm_header->msgh_bits |= MACH_MSGH_BITS_CIRCULAR;
					}
				}
				break;
			}
			case MACH_MSG_GUARDED_PORT_DESCRIPTOR: {
				mach_msg_guarded_port_descriptor_t *dsc = (typeof(dsc)) & saddr->guarded_port;
				mach_msg_type_name_t disp = dsc->disposition;
				ipc_object_t object = ip_to_object(dsc->name);
				dsc->disposition = ipc_object_copyin_type(disp);
				assert(dsc->flags == 0);

				if (!IO_VALID(object)) {
					break;
				}

				ipc_object_copyin_from_kernel(object, disp);
				/*
				 * avoid circularity when the destination is also
				 * the kernel.  This check should be changed into an
				 * assert when the new kobject model is in place since
				 * ports will not be used in kernel to kernel chats
				 */

				if (ip_object_to_port(remote)->ip_receiver != ipc_space_kernel) {
					if ((dsc->disposition == MACH_MSG_TYPE_PORT_RECEIVE) &&
					    ipc_port_check_circularity(ip_object_to_port(object),
					    ip_object_to_port(remote))) {
						kmsg->ikm_header->msgh_bits |= MACH_MSGH_BITS_CIRCULAR;
					}
				}
				break;
			}
			default: {
#if     MACH_ASSERT
				panic("ipc_kmsg_copyin_from_kernel:  bad descriptor");
#endif  /* MACH_ASSERT */
			}
			}
		}
	}

	/* Add the signature to the message */
	ikm_sign(kmsg);

	return MACH_MSG_SUCCESS;
}

#if IKM_SUPPORT_LEGACY
mach_msg_return_t
ipc_kmsg_copyin_from_kernel_legacy(
	ipc_kmsg_t      kmsg)
{
	mach_msg_bits_t bits = kmsg->ikm_header->msgh_bits;
	mach_msg_type_name_t rname = MACH_MSGH_BITS_REMOTE(bits);
	mach_msg_type_name_t lname = MACH_MSGH_BITS_LOCAL(bits);
	ipc_object_t remote = ip_to_object(kmsg->ikm_header->msgh_remote_port);
	ipc_object_t local = ip_to_object(kmsg->ikm_header->msgh_local_port);
	ipc_port_t dest = kmsg->ikm_header->msgh_remote_port;

	/* translate the destination and reply ports */
	if (!IO_VALID(remote)) {
		return MACH_SEND_INVALID_DEST;
	}

	ipc_object_copyin_from_kernel(remote, rname);
	if (IO_VALID(local)) {
		ipc_object_copyin_from_kernel(local, lname);
	}

	/*
	 *	The common case is a complex message with no reply port,
	 *	because that is what the memory_object interface uses.
	 */

	if (bits == (MACH_MSGH_BITS_COMPLEX |
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0))) {
		bits = (MACH_MSGH_BITS_COMPLEX |
		    MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND, 0));

		kmsg->ikm_header->msgh_bits = bits;
	} else {
		bits = (MACH_MSGH_BITS_OTHER(bits) |
		    MACH_MSGH_BITS(ipc_object_copyin_type(rname),
		    ipc_object_copyin_type(lname)));

		kmsg->ikm_header->msgh_bits = bits;
	}

	if (bits & MACH_MSGH_BITS_COMPLEX) {
		if (dest->ip_no_grant) {
			mach_msg_descriptor_t   *saddr;
			mach_msg_body_t         *body;
			mach_msg_type_number_t  i, count;

			body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
			saddr = (mach_msg_descriptor_t *) (body + 1);
			count = body->msgh_descriptor_count;

			for (i = 0; i < count; i++, saddr++) {
				switch (saddr->type.type) {
				case MACH_MSG_PORT_DESCRIPTOR:
				case MACH_MSG_OOL_PORTS_DESCRIPTOR:
				case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
					/* no descriptors have been copied in yet */
					ipc_kmsg_clean_partial(kmsg, 0, NULL, 0, 0);
					return MACH_SEND_NO_GRANT_DEST;
				}
			}
		}

		mach_msg_legacy_descriptor_t    *saddr;
		mach_msg_descriptor_t   *daddr;
		mach_msg_body_t         *body;
		mach_msg_type_number_t  i, count;

		body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
		saddr = (typeof(saddr))(body + 1);
		count = body->msgh_descriptor_count;

		if (count) {
			vm_offset_t dsc_adjust = 4 * count;
			memmove((char *)(((vm_offset_t)kmsg->ikm_header) - dsc_adjust), kmsg->ikm_header, sizeof(mach_msg_base_t));
			kmsg->ikm_header = (mach_msg_header_t *)((vm_offset_t)kmsg->ikm_header - dsc_adjust);
			/* Update the message size for the larger in-kernel representation */
			kmsg->ikm_header->msgh_size += dsc_adjust;
		}
		daddr = (mach_msg_descriptor_t *)((vm_offset_t)kmsg->ikm_header + sizeof(mach_msg_base_t));

		for (i = 0; i < count; i++, saddr++, daddr++) {
			switch (saddr->type.type) {
			case MACH_MSG_PORT_DESCRIPTOR: {
				mach_msg_type_name_t        name;
				ipc_object_t                object;
				mach_msg_legacy_port_descriptor_t   *dsc;
				mach_msg_port_descriptor_t  *dest_dsc;

				dsc = (typeof(dsc)) & saddr->port;
				dest_dsc = &daddr->port;

				/* this is really the type SEND, SEND_ONCE, etc. */
				name = dsc->disposition;
				object = ip_to_object(CAST_MACH_NAME_TO_PORT(dsc->name));
				dest_dsc->disposition = ipc_object_copyin_type(name);
				dest_dsc->name = ip_object_to_port(object);
				dest_dsc->type = MACH_MSG_PORT_DESCRIPTOR;

				if (!IO_VALID(object)) {
					break;
				}

				ipc_object_copyin_from_kernel(object, name);

				/* CDY avoid circularity when the destination is also */
				/* the kernel.  This check should be changed into an  */
				/* assert when the new kobject model is in place since*/
				/* ports will not be used in kernel to kernel chats   */

				if (ip_object_to_port(remote)->ip_receiver != ipc_space_kernel) {
					if ((dest_dsc->disposition == MACH_MSG_TYPE_PORT_RECEIVE) &&
					    ipc_port_check_circularity(ip_object_to_port(object),
					    ip_object_to_port(remote))) {
						kmsg->ikm_header->msgh_bits |=
						    MACH_MSGH_BITS_CIRCULAR;
					}
				}
				break;
			}
			case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
			case MACH_MSG_OOL_DESCRIPTOR: {
				/* The sender should supply ready-made memory, i.e. a vm_map_copy_t
				 * so we don't need to do anything special. */

				mach_msg_ool_descriptor32_t *source_dsc = &saddr->out_of_line32;
				mach_msg_ool_descriptor_t   *dest_dsc = (typeof(dest_dsc)) & daddr->out_of_line;

				vm_offset_t             address = source_dsc->address;
				vm_size_t                           size = source_dsc->size;
				boolean_t                           deallocate = source_dsc->deallocate;
				mach_msg_copy_options_t             copy = source_dsc->copy;
				mach_msg_descriptor_type_t  type = source_dsc->type;

				dest_dsc->address = (void *)address;
				dest_dsc->size = size;
				dest_dsc->deallocate = deallocate;
				dest_dsc->copy = copy;
				dest_dsc->type = type;
				break;
			}
			case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
				ipc_object_t                        *objects;
				unsigned int                        j;
				mach_msg_type_name_t                name;
				mach_msg_ool_ports_descriptor_t     *dest_dsc;

				mach_msg_ool_ports_descriptor32_t   *source_dsc = &saddr->ool_ports32;
				dest_dsc = (typeof(dest_dsc)) & daddr->ool_ports;

				boolean_t deallocate = source_dsc->deallocate;
				mach_msg_copy_options_t copy = source_dsc->copy;
				mach_msg_size_t port_count = source_dsc->count;
				mach_msg_type_name_t disposition = source_dsc->disposition;

				/* this is really the type SEND, SEND_ONCE, etc. */
				name = disposition;
				disposition = ipc_object_copyin_type(name);

				objects = (ipc_object_t *) (uintptr_t)source_dsc->address;

				for (j = 0; j < port_count; j++) {
					ipc_object_t object = objects[j];

					if (!IO_VALID(object)) {
						continue;
					}

					ipc_object_copyin_from_kernel(object, name);

					if ((disposition == MACH_MSG_TYPE_PORT_RECEIVE) &&
					    ipc_port_check_circularity(ip_object_to_port(object),
					    ip_object_to_port(remote))) {
						kmsg->ikm_header->msgh_bits |= MACH_MSGH_BITS_CIRCULAR;
					}
				}

				dest_dsc->address = objects;
				dest_dsc->deallocate = deallocate;
				dest_dsc->copy = copy;
				dest_dsc->disposition = disposition;
				dest_dsc->type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
				dest_dsc->count = port_count;
				break;
			}
			case MACH_MSG_GUARDED_PORT_DESCRIPTOR: {
				mach_msg_type_name_t  disp;
				ipc_object_t object;
				mach_msg_guarded_port_descriptor32_t   *dsc;
				mach_msg_guarded_port_descriptor_t  *dest_dsc;

				dsc = (typeof(dsc)) & saddr->guarded_port32;
				dest_dsc = &daddr->guarded_port;

				disp = dsc->disposition;
				object = ip_to_object(CAST_MACH_NAME_TO_PORT(dsc->name));
				assert(dsc->flags == 0);
				assert(dsc->context == 0);

				dest_dsc->disposition = ipc_object_copyin_type(disp);
				dest_dsc->name = ip_object_to_port(object);
				dest_dsc->type = MACH_MSG_GUARDED_PORT_DESCRIPTOR;
				dest_dsc->flags = 0;

				if (!IO_VALID(object)) {
					break;
				}

				ipc_object_copyin_from_kernel(object, disp);

				/* CDY avoid circularity when the destination is also */
				/* the kernel.  This check should be changed into an  */
				/* assert when the new kobject model is in place since*/
				/* ports will not be used in kernel to kernel chats   */

				if (ip_object_to_port(remote)->ip_receiver != ipc_space_kernel) {
					if ((dest_dsc->disposition == MACH_MSG_TYPE_PORT_RECEIVE) &&
					    ipc_port_check_circularity(ip_object_to_port(object),
					    ip_object_to_port(remote))) {
						kmsg->ikm_header->msgh_bits |=
						    MACH_MSGH_BITS_CIRCULAR;
					}
				}
				break;
			}
			default: {
#if     MACH_ASSERT
				panic("ipc_kmsg_copyin_from_kernel:  bad descriptor");
#endif  /* MACH_ASSERT */
			}
			}
		}
	}

	ikm_sign(kmsg);

	return MACH_MSG_SUCCESS;
}
#endif /* IKM_SUPPORT_LEGACY */

/*
 *	Routine:	ipc_kmsg_copyout_header
 *	Purpose:
 *		"Copy-out" port rights in the header of a message.
 *		Operates atomically; if it doesn't succeed the
 *		message header and the space are left untouched.
 *		If it does succeed the remote/local port fields
 *		contain port names instead of object pointers,
 *		and the bits field is updated.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	Copied out port rights.
 *		MACH_RCV_INVALID_NOTIFY
 *			Notify is non-null and doesn't name a receive right.
 *			(Either KERN_INVALID_NAME or KERN_INVALID_RIGHT.)
 *		MACH_RCV_HEADER_ERROR|MACH_MSG_IPC_SPACE
 *			The space is dead.
 *		MACH_RCV_HEADER_ERROR|MACH_MSG_IPC_SPACE
 *			No room in space for another name.
 *		MACH_RCV_HEADER_ERROR|MACH_MSG_IPC_KERNEL
 *			Couldn't allocate memory for the reply port.
 *		MACH_RCV_HEADER_ERROR|MACH_MSG_IPC_KERNEL
 *			Couldn't allocate memory for the dead-name request.
 */

mach_msg_return_t
ipc_kmsg_copyout_header(
	ipc_kmsg_t              kmsg,
	ipc_space_t             space,
	mach_msg_option_t       option)
{
	mach_msg_header_t *msg = kmsg->ikm_header;
	mach_msg_bits_t mbits = msg->msgh_bits;
	ipc_port_t dest = msg->msgh_remote_port;

	assert(IP_VALID(dest));

	/*
	 * While we still hold a reference on the received-from port,
	 * process all send-possible notfications we received along with
	 * the message.
	 */
	ipc_port_spnotify(dest);

	{
		mach_msg_type_name_t dest_type = MACH_MSGH_BITS_REMOTE(mbits);
		mach_msg_type_name_t reply_type = MACH_MSGH_BITS_LOCAL(mbits);
		mach_msg_type_name_t voucher_type = MACH_MSGH_BITS_VOUCHER(mbits);
		ipc_port_t reply = msg->msgh_local_port;
		ipc_port_t release_reply_port = IP_NULL;
		mach_port_name_t dest_name, reply_name;

		ipc_port_t voucher = kmsg->ikm_voucher;
		ipc_port_t release_voucher_port = IP_NULL;
		mach_port_name_t voucher_name;

		uint32_t entries_held = 0;
		boolean_t need_write_lock = FALSE;
		ipc_object_copyout_flags_t reply_copyout_options = IPC_OBJECT_COPYOUT_FLAGS_NONE;
		kern_return_t kr;

		/*
		 * Reserve any potentially needed entries in the target space.
		 * We'll free any unused before unlocking the space.
		 */
		if (IP_VALID(reply)) {
			entries_held++;
			need_write_lock = TRUE;
		}
		if (IP_VALID(voucher)) {
			assert(voucher_type == MACH_MSG_TYPE_MOVE_SEND);

			if ((option & MACH_RCV_VOUCHER) != 0) {
				entries_held++;
			}
			need_write_lock = TRUE;
		}

		if (need_write_lock) {
handle_reply_again:
			is_write_lock(space);

			while (entries_held) {
				if (!is_active(space)) {
					is_write_unlock(space);
					return MACH_RCV_HEADER_ERROR |
					       MACH_MSG_IPC_SPACE;
				}

				kr = ipc_entries_hold(space, entries_held);
				if (KERN_SUCCESS == kr) {
					break;
				}

				kr = ipc_entry_grow_table(space, ITS_SIZE_NONE);
				if (KERN_SUCCESS != kr) {
					return MACH_RCV_HEADER_ERROR |
					       MACH_MSG_IPC_SPACE;
				}
				/* space was unlocked and relocked - retry */
			}

			/* Handle reply port. */
			if (IP_VALID(reply)) {
				ipc_port_t reply_subst = IP_NULL;
				ipc_entry_t entry;

				ip_lock(reply);

				/* Is the reply port still active and allowed to be copied out? */
				if (!ip_active(reply) ||
				    !ip_label_check(space, reply, reply_type,
				    &reply_copyout_options, &reply_subst)) {
					/* clear the context value */
					reply->ip_reply_context = 0;
					ip_unlock(reply);

					assert(reply_subst == IP_NULL);
					release_reply_port = reply;
					reply = IP_DEAD;
					reply_name = MACH_PORT_DEAD;
					goto done_with_reply;
				}

				/* is the kolabel requesting a substitution */
				if (reply_subst != IP_NULL) {
					/*
					 * port is unlocked, its right consumed
					 * space is unlocked
					 */
					assert(reply_type == MACH_MSG_TYPE_PORT_SEND);
					msg->msgh_local_port = reply = reply_subst;
					goto handle_reply_again;
				}


				/* Is there already an entry we can use? */
				if ((reply_type != MACH_MSG_TYPE_PORT_SEND_ONCE) &&
				    ipc_right_reverse(space, ip_to_object(reply), &reply_name, &entry)) {
					assert(entry->ie_bits & MACH_PORT_TYPE_SEND_RECEIVE);
				} else {
					/* claim a held entry for the reply port */
					assert(entries_held > 0);
					entries_held--;
					ipc_entry_claim(space, &reply_name, &entry);
					assert(!ipc_right_inuse(entry));
					assert(entry->ie_object == IO_NULL);
					entry->ie_object = ip_to_object(reply);
				}

				/* space and reply port are locked and active */
				ip_reference(reply);         /* hold onto the reply port */

				/*
				 * If the receiver would like to enforce strict reply
				 * semantics, and the message looks like it expects a reply,
				 * and contains a voucher, then link the context in the
				 * voucher with the reply port so that the next message sent
				 * to the reply port must come from a thread that has a
				 * matching context (voucher).
				 */
				if (enforce_strict_reply && MACH_RCV_WITH_STRICT_REPLY(option) && IP_VALID(voucher)) {
					if (ipc_kmsg_validate_reply_port_locked(reply, option) != KERN_SUCCESS) {
						/* if the receiver isn't happy with the reply port: fail the receive. */
						ip_unlock(reply);
						ipc_entry_dealloc(space, reply_name, entry);
						is_write_unlock(space);
						ip_release(reply);
						return MACH_RCV_INVALID_REPLY;
					}
					ipc_kmsg_link_reply_context_locked(reply, voucher);
				} else {
					/*
					 * if the receive did not choose to participate
					 * in the strict reply/RPC, then don't enforce
					 * anything (as this could lead to booby-trapped
					 * messages that kill the server).
					 */
					reply->ip_reply_context = 0;
				}

				kr = ipc_right_copyout(space, reply_name, entry,
				    reply_type, IPC_OBJECT_COPYOUT_FLAGS_NONE, NULL, NULL,
				    ip_to_object(reply));
				assert(kr == KERN_SUCCESS);
				/* reply port is unlocked */
			} else {
				reply_name = CAST_MACH_PORT_TO_NAME(reply);
			}

done_with_reply:

			/* Handle voucher port. */
			if (voucher_type != MACH_MSGH_BITS_ZERO) {
				assert(voucher_type == MACH_MSG_TYPE_MOVE_SEND);

				if (!IP_VALID(voucher)) {
					if ((option & MACH_RCV_VOUCHER) == 0) {
						voucher_type = MACH_MSGH_BITS_ZERO;
					}
					voucher_name = MACH_PORT_NULL;
					goto done_with_voucher;
				}

				/* clear voucher from its hiding place back in the kmsg */
				kmsg->ikm_voucher = IP_NULL;

				if ((option & MACH_RCV_VOUCHER) != 0) {
					ipc_entry_t entry;

					ip_lock(voucher);

					if (ipc_right_reverse(space, ip_to_object(voucher),
					    &voucher_name, &entry)) {
						assert(entry->ie_bits & MACH_PORT_TYPE_SEND);
					} else {
						assert(entries_held > 0);
						entries_held--;
						ipc_entry_claim(space, &voucher_name, &entry);
						assert(!ipc_right_inuse(entry));
						assert(entry->ie_object == IO_NULL);
						entry->ie_object = ip_to_object(voucher);
					}
					/* space is locked and active */

					assert(ip_kotype(voucher) == IKOT_VOUCHER);
					kr = ipc_right_copyout(space, voucher_name, entry,
					    MACH_MSG_TYPE_MOVE_SEND, IPC_OBJECT_COPYOUT_FLAGS_NONE,
					    NULL, NULL, ip_to_object(voucher));
					/* voucher port is unlocked */
				} else {
					voucher_type = MACH_MSGH_BITS_ZERO;
					release_voucher_port = voucher;
					voucher_name = MACH_PORT_NULL;
				}
			} else {
				voucher_name = msg->msgh_voucher_port;
			}

done_with_voucher:

			ip_lock(dest);
			is_write_unlock(space);
		} else {
			/*
			 *	No reply or voucher port!  This is an easy case.
			 *	We only need to have the space locked
			 *	when locking the destination.
			 */

			is_read_lock(space);
			if (!is_active(space)) {
				is_read_unlock(space);
				return MACH_RCV_HEADER_ERROR | MACH_MSG_IPC_SPACE;
			}

			ip_lock(dest);
			is_read_unlock(space);

			reply_name = CAST_MACH_PORT_TO_NAME(reply);

			if (voucher_type != MACH_MSGH_BITS_ZERO) {
				assert(voucher_type == MACH_MSG_TYPE_MOVE_SEND);
				if ((option & MACH_RCV_VOUCHER) == 0) {
					voucher_type = MACH_MSGH_BITS_ZERO;
				}
				voucher_name = MACH_PORT_NULL;
			} else {
				voucher_name = msg->msgh_voucher_port;
			}
		}

		/*
		 *	At this point, the space is unlocked and the destination
		 *	port is locked.  (Lock taken while space was locked.)
		 *	reply_name is taken care of; we still need dest_name.
		 *	We still hold a ref for reply (if it is valid).
		 *
		 *	If the space holds receive rights for the destination,
		 *	we return its name for the right.  Otherwise the task
		 *	managed to destroy or give away the receive right between
		 *	receiving the message and this copyout.  If the destination
		 *	is dead, return MACH_PORT_DEAD, and if the receive right
		 *	exists somewhere else (another space, in transit)
		 *	return MACH_PORT_NULL.
		 *
		 *	Making this copyout operation atomic with the previous
		 *	copyout of the reply port is a bit tricky.  If there was
		 *	no real reply port (it wasn't IP_VALID) then this isn't
		 *	an issue.  If the reply port was dead at copyout time,
		 *	then we are OK, because if dest is dead we serialize
		 *	after the death of both ports and if dest is alive
		 *	we serialize after reply died but before dest's (later) death.
		 *	So assume reply was alive when we copied it out.  If dest
		 *	is alive, then we are OK because we serialize before
		 *	the ports' deaths.  So assume dest is dead when we look at it.
		 *	If reply dies/died after dest, then we are OK because
		 *	we serialize after dest died but before reply dies.
		 *	So the hard case is when reply is alive at copyout,
		 *	dest is dead at copyout, and reply died before dest died.
		 *	In this case pretend that dest is still alive, so
		 *	we serialize while both ports are alive.
		 *
		 *	Because the space lock is held across the copyout of reply
		 *	and locking dest, the receive right for dest can't move
		 *	in or out of the space while the copyouts happen, so
		 *	that isn't an atomicity problem.  In the last hard case
		 *	above, this implies that when dest is dead that the
		 *	space couldn't have had receive rights for dest at
		 *	the time reply was copied-out, so when we pretend
		 *	that dest is still alive, we can return MACH_PORT_NULL.
		 *
		 *	If dest == reply, then we have to make it look like
		 *	either both copyouts happened before the port died,
		 *	or both happened after the port died.  This special
		 *	case works naturally if the timestamp comparison
		 *	is done correctly.
		 */

		if (ip_active(dest)) {
			ipc_object_copyout_dest(space, ip_to_object(dest),
			    dest_type, &dest_name);
			/* dest is unlocked */
		} else {
			ipc_port_timestamp_t timestamp;

			timestamp = dest->ip_timestamp;
			ip_unlock(dest);
			ip_release(dest);

			if (IP_VALID(reply)) {
				ip_lock(reply);
				if (ip_active(reply) ||
				    IP_TIMESTAMP_ORDER(timestamp,
				    reply->ip_timestamp)) {
					dest_name = MACH_PORT_DEAD;
				} else {
					dest_name = MACH_PORT_NULL;
				}
				ip_unlock(reply);
			} else {
				dest_name = MACH_PORT_DEAD;
			}
		}

		if (IP_VALID(reply)) {
			ip_release(reply);
		}

		if (IP_VALID(release_reply_port)) {
			if (reply_type == MACH_MSG_TYPE_PORT_SEND_ONCE) {
				ipc_port_release_sonce(release_reply_port);
			} else {
				ipc_port_release_send(release_reply_port);
			}
		}

		if ((option & MACH_RCV_VOUCHER) != 0) {
			KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_MSG_RECV) | DBG_FUNC_NONE,
			    VM_KERNEL_ADDRPERM((uintptr_t)kmsg),
			    (uintptr_t)kmsg->ikm_header->msgh_bits,
			    (uintptr_t)kmsg->ikm_header->msgh_id,
			    VM_KERNEL_ADDRPERM((uintptr_t)unsafe_convert_port_to_voucher(voucher)),
			    0);
		} else {
			KERNEL_DEBUG_CONSTANT(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_MSG_RECV_VOUCHER_REFUSED) | DBG_FUNC_NONE,
			    VM_KERNEL_ADDRPERM((uintptr_t)kmsg),
			    (uintptr_t)kmsg->ikm_header->msgh_bits,
			    (uintptr_t)kmsg->ikm_header->msgh_id,
			    VM_KERNEL_ADDRPERM((uintptr_t)unsafe_convert_port_to_voucher(voucher)),
			    0);
		}

		if (IP_VALID(release_voucher_port)) {
			ipc_port_release_send(release_voucher_port);
		}

		msg->msgh_bits = MACH_MSGH_BITS_SET(reply_type, dest_type,
		    voucher_type, mbits);
		msg->msgh_local_port = CAST_MACH_NAME_TO_PORT(dest_name);
		msg->msgh_remote_port = CAST_MACH_NAME_TO_PORT(reply_name);
		msg->msgh_voucher_port = voucher_name;
	}

	return MACH_MSG_SUCCESS;
}

/*
 *	Routine:	ipc_kmsg_copyout_object
 *	Purpose:
 *		Copy-out a port right.  Always returns a name,
 *		even for unsuccessful return codes.  Always
 *		consumes the supplied object.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	The space acquired the right
 *			(name is valid) or the object is dead (MACH_PORT_DEAD).
 *		MACH_MSG_IPC_SPACE	No room in space for the right,
 *			or the space is dead.  (Name is MACH_PORT_NULL.)
 *		MACH_MSG_IPC_KERNEL	Kernel resource shortage.
 *			(Name is MACH_PORT_NULL.)
 */
static mach_msg_return_t
ipc_kmsg_copyout_object(
	ipc_space_t             space,
	ipc_object_t            object,
	mach_msg_type_name_t    msgt_name,
	mach_port_context_t     *context,
	mach_msg_guard_flags_t  *guard_flags,
	mach_port_name_t        *namep)
{
	kern_return_t kr;

	if (!IO_VALID(object)) {
		*namep = CAST_MACH_PORT_TO_NAME(object);
		return MACH_MSG_SUCCESS;
	}

	kr = ipc_object_copyout(space, object, msgt_name, IPC_OBJECT_COPYOUT_FLAGS_NONE,
	    context, guard_flags, namep);
	if (kr != KERN_SUCCESS) {
		if (kr == KERN_INVALID_CAPABILITY) {
			*namep = MACH_PORT_DEAD;
		} else {
			*namep = MACH_PORT_NULL;

			if (kr == KERN_RESOURCE_SHORTAGE) {
				return MACH_MSG_IPC_KERNEL;
			} else {
				return MACH_MSG_IPC_SPACE;
			}
		}
	}

	return MACH_MSG_SUCCESS;
}

static mach_msg_descriptor_t *
ipc_kmsg_copyout_port_descriptor(
	mach_msg_descriptor_t   *dsc,
	mach_msg_descriptor_t   *dest_dsc,
	ipc_space_t             space,
	kern_return_t           *mr)
{
	mach_port_t             port;
	mach_port_name_t        name;
	mach_msg_type_name_t    disp;

	/* Copyout port right carried in the message */
	port = dsc->port.name;
	disp = dsc->port.disposition;
	*mr |= ipc_kmsg_copyout_object(space,
	    ip_to_object(port), disp, NULL, NULL, &name);

	if (current_task() == kernel_task) {
		mach_msg_port_descriptor_t *user_dsc = (typeof(user_dsc))dest_dsc;
		user_dsc--;         // point to the start of this port descriptor
		bzero((void *)user_dsc, sizeof(*user_dsc));
		user_dsc->name = CAST_MACH_NAME_TO_PORT(name);
		user_dsc->disposition = disp;
		user_dsc->type = MACH_MSG_PORT_DESCRIPTOR;
		dest_dsc = (typeof(dest_dsc))user_dsc;
	} else {
		mach_msg_legacy_port_descriptor_t *user_dsc = (typeof(user_dsc))dest_dsc;
		user_dsc--;         // point to the start of this port descriptor
		bzero((void *)user_dsc, sizeof(*user_dsc));
		user_dsc->name = CAST_MACH_PORT_TO_NAME(name);
		user_dsc->disposition = disp;
		user_dsc->type = MACH_MSG_PORT_DESCRIPTOR;
		dest_dsc = (typeof(dest_dsc))user_dsc;
	}

	return (mach_msg_descriptor_t *)dest_dsc;
}

static mach_msg_descriptor_t *
ipc_kmsg_copyout_ool_descriptor(
	mach_msg_ool_descriptor_t   *dsc,
	mach_msg_descriptor_t       *user_dsc,
	int                         is_64bit,
	vm_map_t                    map,
	mach_msg_return_t           *mr)
{
	vm_map_copy_t               copy;
	vm_map_address_t            rcv_addr;
	mach_msg_copy_options_t     copy_options;
	vm_map_size_t               size;
	mach_msg_descriptor_type_t  dsc_type;
	boolean_t                   misaligned = FALSE;

	//SKIP_PORT_DESCRIPTORS(saddr, sdsc_count);

	copy = (vm_map_copy_t)dsc->address;
	size = (vm_map_size_t)dsc->size;
	copy_options = dsc->copy;
	assert(copy_options != MACH_MSG_KALLOC_COPY_T);
	dsc_type = dsc->type;

	if (copy != VM_MAP_COPY_NULL) {
		kern_return_t kr;

		rcv_addr = 0;
		if (vm_map_copy_validate_size(map, copy, &size) == FALSE) {
			panic("Inconsistent OOL/copyout size on %p: expected %d, got %lld @%p",
			    dsc, dsc->size, (unsigned long long)copy->size, copy);
		}

		if ((copy->type == VM_MAP_COPY_ENTRY_LIST) &&
		    (trunc_page(copy->offset) != copy->offset ||
		    round_page(dsc->size) != dsc->size)) {
			misaligned = TRUE;
		}

		if (misaligned) {
			vm_map_address_t        rounded_addr;
			vm_map_size_t   rounded_size;
			vm_map_offset_t effective_page_mask, effective_page_size;

			effective_page_mask = VM_MAP_PAGE_MASK(map);
			effective_page_size = effective_page_mask + 1;

			rounded_size = vm_map_round_page(copy->offset + size, effective_page_mask) - vm_map_trunc_page(copy->offset, effective_page_mask);

			kr = vm_allocate_kernel(map, (vm_offset_t*)&rounded_addr, rounded_size, VM_FLAGS_ANYWHERE, 0);

			if (kr == KERN_SUCCESS) {
				/*
				 * vm_map_copy_overwrite does a full copy
				 * if size is too small to optimize.
				 * So we tried skipping the offset adjustment
				 * if we fail the 'size' test.
				 *
				 * if (size >= VM_MAP_COPY_OVERWRITE_OPTIMIZATION_THRESHOLD_PAGES * effective_page_size) {
				 *
				 * This resulted in leaked memory especially on the
				 * older watches (16k user - 4k kernel) because we
				 * would do a physical copy into the start of this
				 * rounded range but could leak part of it
				 * on deallocation if the 'size' being deallocated
				 * does not cover the full range. So instead we do
				 * the misalignment adjustment always so that on
				 * deallocation we will remove the full range.
				 */
				if ((rounded_addr & effective_page_mask) !=
				    (copy->offset & effective_page_mask)) {
					/*
					 * Need similar mis-alignment of source and destination...
					 */
					rounded_addr += (copy->offset & effective_page_mask);

					assert((rounded_addr & effective_page_mask) == (copy->offset & effective_page_mask));
				}
				rcv_addr = rounded_addr;

				kr = vm_map_copy_overwrite(map, rcv_addr, copy, size, FALSE);
			}
		} else {
			kr = vm_map_copyout_size(map, &rcv_addr, copy, size);
		}
		if (kr != KERN_SUCCESS) {
			if (kr == KERN_RESOURCE_SHORTAGE) {
				*mr |= MACH_MSG_VM_KERNEL;
			} else {
				*mr |= MACH_MSG_VM_SPACE;
			}
			vm_map_copy_discard(copy);
			rcv_addr = 0;
			size = 0;
		}
	} else {
		rcv_addr = 0;
		size = 0;
	}

	/*
	 * Now update the descriptor as the user would see it.
	 * This may require expanding the descriptor to the user
	 * visible size.  There is already space allocated for
	 * this in what naddr points to.
	 */
	if (current_task() == kernel_task) {
		mach_msg_ool_descriptor_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;
		user_ool_dsc--;
		bzero((void *)user_ool_dsc, sizeof(*user_ool_dsc));

		user_ool_dsc->address = (void *)(uintptr_t)rcv_addr;
		user_ool_dsc->deallocate = (copy_options == MACH_MSG_VIRTUAL_COPY) ?
		    TRUE : FALSE;
		user_ool_dsc->copy = copy_options;
		user_ool_dsc->type = dsc_type;
		user_ool_dsc->size = (mach_msg_size_t)size;

		user_dsc = (typeof(user_dsc))user_ool_dsc;
	} else if (is_64bit) {
		mach_msg_ool_descriptor64_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;
		user_ool_dsc--;
		bzero((void *)user_ool_dsc, sizeof(*user_ool_dsc));

		user_ool_dsc->address = rcv_addr;
		user_ool_dsc->deallocate = (copy_options == MACH_MSG_VIRTUAL_COPY) ?
		    TRUE : FALSE;
		user_ool_dsc->copy = copy_options;
		user_ool_dsc->type = dsc_type;
		user_ool_dsc->size = (mach_msg_size_t)size;

		user_dsc = (typeof(user_dsc))user_ool_dsc;
	} else {
		mach_msg_ool_descriptor32_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;
		user_ool_dsc--;
		bzero((void *)user_ool_dsc, sizeof(*user_ool_dsc));

		user_ool_dsc->address = CAST_DOWN_EXPLICIT(uint32_t, rcv_addr);
		user_ool_dsc->size = (mach_msg_size_t)size;
		user_ool_dsc->deallocate = (copy_options == MACH_MSG_VIRTUAL_COPY) ?
		    TRUE : FALSE;
		user_ool_dsc->copy = copy_options;
		user_ool_dsc->type = dsc_type;

		user_dsc = (typeof(user_dsc))user_ool_dsc;
	}
	return user_dsc;
}

mach_msg_descriptor_t *
ipc_kmsg_copyout_ool_ports_descriptor(mach_msg_ool_ports_descriptor_t *dsc,
    mach_msg_descriptor_t *user_dsc,
    int is_64bit,
    vm_map_t map,
    ipc_space_t space,
    ipc_kmsg_t kmsg,
    mach_msg_return_t *mr);
mach_msg_descriptor_t *
ipc_kmsg_copyout_ool_ports_descriptor(mach_msg_ool_ports_descriptor_t *dsc,
    mach_msg_descriptor_t *user_dsc,
    int is_64bit,
    vm_map_t map,
    ipc_space_t space,
    ipc_kmsg_t kmsg,
    mach_msg_return_t *mr)
{
	mach_vm_offset_t            rcv_addr = 0;
	mach_msg_type_name_t                disp;
	mach_msg_type_number_t              count, i;
	vm_size_t                           ports_length, names_length;

	mach_msg_copy_options_t copy_options = MACH_MSG_VIRTUAL_COPY;

	//SKIP_PORT_DESCRIPTORS(saddr, sdsc_count);

	count = dsc->count;
	disp = dsc->disposition;
	ports_length = count * sizeof(mach_port_t);
	names_length = count * sizeof(mach_port_name_t);

	if (ports_length != 0 && dsc->address != 0) {
		/*
		 * Check to see if there is an overwrite descriptor
		 * specified in the scatter list for this ool data.
		 * The descriptor has already been verified.
		 */
#if 0
		if (saddr != MACH_MSG_DESCRIPTOR_NULL) {
			if (differs) {
				OTHER_OOL_DESCRIPTOR *scatter_dsc;

				scatter_dsc = (OTHER_OOL_DESCRIPTOR *)saddr;
				rcv_addr = (mach_vm_offset_t) scatter_dsc->address;
				copy_options = scatter_dsc->copy;
			} else {
				mach_msg_ool_descriptor_t *scatter_dsc;

				scatter_dsc = &saddr->out_of_line;
				rcv_addr = CAST_USER_ADDR_T(scatter_dsc->address);
				copy_options = scatter_dsc->copy;
			}
			INCREMENT_SCATTER(saddr, sdsc_count, differs);
		}
#endif

		if (copy_options == MACH_MSG_VIRTUAL_COPY) {
			/*
			 * Dynamically allocate the region
			 */
			vm_tag_t tag;
			if (vm_kernel_map_is_kernel(map)) {
				tag = VM_KERN_MEMORY_IPC;
			} else {
				tag = VM_MEMORY_MACH_MSG;
			}

			kern_return_t kr;
			if ((kr = mach_vm_allocate_kernel(map, &rcv_addr,
			    (mach_vm_size_t)names_length,
			    VM_FLAGS_ANYWHERE, tag)) != KERN_SUCCESS) {
				ipc_kmsg_clean_body(kmsg, 1, (mach_msg_descriptor_t *)dsc);
				rcv_addr = 0;

				if (kr == KERN_RESOURCE_SHORTAGE) {
					*mr |= MACH_MSG_VM_KERNEL;
				} else {
					*mr |= MACH_MSG_VM_SPACE;
				}
			}
		}

		/*
		 * Handle the port rights and copy out the names
		 * for those rights out to user-space.
		 */
		if (rcv_addr != 0) {
			ipc_object_t *objects = (ipc_object_t *) dsc->address;
			mach_port_name_t *names = (mach_port_name_t *) dsc->address;

			/* copyout port rights carried in the message */

			for (i = 0; i < count; i++) {
				ipc_object_t object = objects[i];

				*mr |= ipc_kmsg_copyout_object(space, object,
				    disp, NULL, NULL, &names[i]);
			}

			/* copyout to memory allocated above */
			void *data = dsc->address;
			if (copyoutmap(map, data, rcv_addr, names_length) != KERN_SUCCESS) {
				*mr |= MACH_MSG_VM_SPACE;
			}
			kfree(data, ports_length);
		}
	} else {
		rcv_addr = 0;
	}

	/*
	 * Now update the descriptor based on the information
	 * calculated above.
	 */
	if (current_task() == kernel_task) {
		mach_msg_ool_ports_descriptor_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;
		user_ool_dsc--;
		bzero((void *)user_ool_dsc, sizeof(*user_ool_dsc));

		user_ool_dsc->address = (void *)(uintptr_t)rcv_addr;
		user_ool_dsc->deallocate = (copy_options == MACH_MSG_VIRTUAL_COPY) ?
		    TRUE : FALSE;
		user_ool_dsc->copy = copy_options;
		user_ool_dsc->disposition = disp;
		user_ool_dsc->type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
		user_ool_dsc->count = count;

		user_dsc = (typeof(user_dsc))user_ool_dsc;
	} else if (is_64bit) {
		mach_msg_ool_ports_descriptor64_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;
		user_ool_dsc--;
		bzero((void *)user_ool_dsc, sizeof(*user_ool_dsc));

		user_ool_dsc->address = rcv_addr;
		user_ool_dsc->deallocate = (copy_options == MACH_MSG_VIRTUAL_COPY) ?
		    TRUE : FALSE;
		user_ool_dsc->copy = copy_options;
		user_ool_dsc->disposition = disp;
		user_ool_dsc->type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
		user_ool_dsc->count = count;

		user_dsc = (typeof(user_dsc))user_ool_dsc;
	} else {
		mach_msg_ool_ports_descriptor32_t *user_ool_dsc = (typeof(user_ool_dsc))user_dsc;
		user_ool_dsc--;
		bzero((void *)user_ool_dsc, sizeof(*user_ool_dsc));

		user_ool_dsc->address = CAST_DOWN_EXPLICIT(uint32_t, rcv_addr);
		user_ool_dsc->count = count;
		user_ool_dsc->deallocate = (copy_options == MACH_MSG_VIRTUAL_COPY) ?
		    TRUE : FALSE;
		user_ool_dsc->copy = copy_options;
		user_ool_dsc->disposition = disp;
		user_ool_dsc->type = MACH_MSG_OOL_PORTS_DESCRIPTOR;

		user_dsc = (typeof(user_dsc))user_ool_dsc;
	}
	return user_dsc;
}

static mach_msg_descriptor_t *
ipc_kmsg_copyout_guarded_port_descriptor(
	mach_msg_guarded_port_descriptor_t *dsc,
	mach_msg_descriptor_t *dest_dsc,
	int is_64bit,
	__unused ipc_kmsg_t  kmsg,
	ipc_space_t space,
	mach_msg_option_t option,
	kern_return_t *mr)
{
	mach_port_t                 port;
	mach_port_name_t            name = MACH_PORT_NULL;
	mach_msg_type_name_t        disp;
	mach_msg_guard_flags_t      guard_flags;
	mach_port_context_t         context;

	/* Copyout port right carried in the message */
	port = dsc->name;
	disp = dsc->disposition;
	guard_flags = dsc->flags;
	context = 0;

	/* Currently kernel_task doesnt support receiving guarded port descriptors */
	struct knote *kn = current_thread()->ith_knote;
	if ((kn != ITH_KNOTE_PSEUDO) && (((option & MACH_RCV_GUARDED_DESC) == 0) ||
	    (current_task() == kernel_task))) {
#if DEVELOPMENT || DEBUG
		if (current_task() != kernel_task) {
			/*
			 * Simulated crash needed for debugging, notifies the receiver to opt into receiving
			 * guarded descriptors.
			 */
			mach_port_guard_exception(current_thread()->ith_receiver_name, 0, 0, kGUARD_EXC_RCV_GUARDED_DESC);
		}
#endif
		KDBG(MACHDBG_CODE(DBG_MACH_IPC, MACH_IPC_DESTROY_GUARDED_DESC), current_thread()->ith_receiver_name,
		    VM_KERNEL_ADDRPERM(port), disp, guard_flags);
		ipc_object_destroy(ip_to_object(port), disp);
		mach_msg_legacy_port_descriptor_t *user_dsc = (typeof(user_dsc))dest_dsc;
		user_dsc--;         // point to the start of this port descriptor
		bzero((void *)user_dsc, sizeof(*user_dsc));
		user_dsc->name = name;
		user_dsc->disposition = disp;
		user_dsc->type = MACH_MSG_PORT_DESCRIPTOR;
		dest_dsc = (typeof(dest_dsc))user_dsc;
	} else {
		*mr |= ipc_kmsg_copyout_object(space,
		    ip_to_object(port), disp, &context, &guard_flags, &name);

		if (!is_64bit) {
			mach_msg_guarded_port_descriptor32_t *user_dsc = (typeof(user_dsc))dest_dsc;
			user_dsc--;         // point to the start of this port descriptor
			bzero((void *)user_dsc, sizeof(*user_dsc));
			user_dsc->name = name;
			user_dsc->flags = guard_flags;
			user_dsc->disposition = disp;
			user_dsc->type = MACH_MSG_GUARDED_PORT_DESCRIPTOR;
			user_dsc->context = CAST_DOWN_EXPLICIT(uint32_t, context);
			dest_dsc = (typeof(dest_dsc))user_dsc;
		} else {
			mach_msg_guarded_port_descriptor64_t *user_dsc = (typeof(user_dsc))dest_dsc;
			user_dsc--;         // point to the start of this port descriptor
			bzero((void *)user_dsc, sizeof(*user_dsc));
			user_dsc->name = name;
			user_dsc->flags = guard_flags;
			user_dsc->disposition = disp;
			user_dsc->type = MACH_MSG_GUARDED_PORT_DESCRIPTOR;
			user_dsc->context = context;
			dest_dsc = (typeof(dest_dsc))user_dsc;
		}
	}

	return (mach_msg_descriptor_t *)dest_dsc;
}


/*
 *	Routine:	ipc_kmsg_copyout_body
 *	Purpose:
 *		"Copy-out" port rights and out-of-line memory
 *		in the body of a message.
 *
 *		The error codes are a combination of special bits.
 *		The copyout proceeds despite errors.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	Successful copyout.
 *		MACH_MSG_IPC_SPACE	No room for port right in name space.
 *		MACH_MSG_VM_SPACE	No room for memory in address space.
 *		MACH_MSG_IPC_KERNEL	Resource shortage handling port right.
 *		MACH_MSG_VM_KERNEL	Resource shortage handling memory.
 *		MACH_MSG_INVALID_RT_DESCRIPTOR Descriptor incompatible with RT
 */

mach_msg_return_t
ipc_kmsg_copyout_body(
	ipc_kmsg_t              kmsg,
	ipc_space_t             space,
	vm_map_t                map,
	mach_msg_option_t       option,
	mach_msg_body_t         *slist)
{
	mach_msg_body_t             *body;
	mach_msg_descriptor_t       *kern_dsc, *user_dsc;
	mach_msg_descriptor_t       *saddr;
	mach_msg_type_number_t      dsc_count, sdsc_count;
	int i;
	mach_msg_return_t           mr = MACH_MSG_SUCCESS;
	boolean_t                   is_task_64bit = (map->max_offset > VM_MAX_ADDRESS);

	body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
	dsc_count = body->msgh_descriptor_count;
	kern_dsc = (mach_msg_descriptor_t *) (body + 1);
	/* Point user_dsc just after the end of all the descriptors */
	user_dsc = &kern_dsc[dsc_count];

	/* Do scatter list setup */
	if (slist != MACH_MSG_BODY_NULL) {
		panic("Scatter lists disabled");
		saddr = (mach_msg_descriptor_t *) (slist + 1);
		sdsc_count = slist->msgh_descriptor_count;
	} else {
		saddr = MACH_MSG_DESCRIPTOR_NULL;
		sdsc_count = 0;
	}

	/* Now process the descriptors - in reverse order */
	for (i = dsc_count - 1; i >= 0; i--) {
		switch (kern_dsc[i].type.type) {
		case MACH_MSG_PORT_DESCRIPTOR:
			user_dsc = ipc_kmsg_copyout_port_descriptor(&kern_dsc[i],
			    user_dsc, space, &mr);
			break;
		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
		case MACH_MSG_OOL_DESCRIPTOR:
			user_dsc = ipc_kmsg_copyout_ool_descriptor(
				(mach_msg_ool_descriptor_t *)&kern_dsc[i],
				user_dsc, is_task_64bit, map, &mr);
			break;
		case MACH_MSG_OOL_PORTS_DESCRIPTOR:
			user_dsc = ipc_kmsg_copyout_ool_ports_descriptor(
				(mach_msg_ool_ports_descriptor_t *)&kern_dsc[i],
				user_dsc, is_task_64bit, map, space, kmsg, &mr);
			break;
		case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
			user_dsc = ipc_kmsg_copyout_guarded_port_descriptor(
				(mach_msg_guarded_port_descriptor_t *)&kern_dsc[i],
				user_dsc, is_task_64bit, kmsg, space, option, &mr);
			break;
		default: {
			panic("untyped IPC copyout body: invalid message descriptor");
		}
		}
	}

	if (user_dsc != kern_dsc) {
		vm_offset_t dsc_adjust = (vm_offset_t)user_dsc - (vm_offset_t)kern_dsc;
		memmove((char *)((vm_offset_t)kmsg->ikm_header + dsc_adjust), kmsg->ikm_header, sizeof(mach_msg_base_t));
		kmsg->ikm_header = (mach_msg_header_t *)((vm_offset_t)kmsg->ikm_header + dsc_adjust);
		/* Update the message size for the smaller user representation */
		kmsg->ikm_header->msgh_size -= (mach_msg_size_t)dsc_adjust;
	}

	return mr;
}

/*
 *	Routine:	ipc_kmsg_copyout_size
 *	Purpose:
 *		Compute the size of the message as copied out to the given
 *		map. If the destination map's pointers are a different size
 *		than the kernel's, we have to allow for expansion/
 *		contraction of the descriptors as appropriate.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		size of the message as it would be received.
 */

mach_msg_size_t
ipc_kmsg_copyout_size(
	ipc_kmsg_t              kmsg,
	vm_map_t                map)
{
	mach_msg_size_t             send_size;

	send_size = kmsg->ikm_header->msgh_size;

	boolean_t is_task_64bit = (map->max_offset > VM_MAX_ADDRESS);

#if defined(__LP64__)
	send_size -= LEGACY_HEADER_SIZE_DELTA;
#endif

	if (kmsg->ikm_header->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
		mach_msg_body_t *body;
		mach_msg_descriptor_t *saddr, *eaddr;

		body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
		saddr = (mach_msg_descriptor_t *) (body + 1);
		eaddr = saddr + body->msgh_descriptor_count;

		for (; saddr < eaddr; saddr++) {
			switch (saddr->type.type) {
			case MACH_MSG_OOL_DESCRIPTOR:
			case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
			case MACH_MSG_OOL_PORTS_DESCRIPTOR:
			case MACH_MSG_GUARDED_PORT_DESCRIPTOR:
				if (!is_task_64bit) {
					send_size -= DESC_SIZE_ADJUSTMENT;
				}
				break;
			case MACH_MSG_PORT_DESCRIPTOR:
				send_size -= DESC_SIZE_ADJUSTMENT;
				break;
			default:
				break;
			}
		}
	}
	return send_size;
}

/*
 *	Routine:	ipc_kmsg_copyout
 *	Purpose:
 *		"Copy-out" port rights and out-of-line memory
 *		in the message.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	Copied out all rights and memory.
 *		MACH_RCV_HEADER_ERROR + special bits
 *			Rights and memory in the message are intact.
 *		MACH_RCV_BODY_ERROR + special bits
 *			The message header was successfully copied out.
 *			As much of the body was handled as possible.
 */

mach_msg_return_t
ipc_kmsg_copyout(
	ipc_kmsg_t              kmsg,
	ipc_space_t             space,
	vm_map_t                map,
	mach_msg_body_t         *slist,
	mach_msg_option_t      option)
{
	mach_msg_return_t mr;

	ikm_validate_sig(kmsg);

	mr = ipc_kmsg_copyout_header(kmsg, space, option);
	if (mr != MACH_MSG_SUCCESS) {
		return mr;
	}

	if (kmsg->ikm_header->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
		mr = ipc_kmsg_copyout_body(kmsg, space, map, option, slist);

		if (mr != MACH_MSG_SUCCESS) {
			mr |= MACH_RCV_BODY_ERROR;
		}
	}

	return mr;
}

/*
 *	Routine:	ipc_kmsg_copyout_pseudo
 *	Purpose:
 *		Does a pseudo-copyout of the message.
 *		This is like a regular copyout, except
 *		that the ports in the header are handled
 *		as if they are in the body.  They aren't reversed.
 *
 *		The error codes are a combination of special bits.
 *		The copyout proceeds despite errors.
 *	Conditions:
 *		Nothing locked.
 *	Returns:
 *		MACH_MSG_SUCCESS	Successful copyout.
 *		MACH_MSG_IPC_SPACE	No room for port right in name space.
 *		MACH_MSG_VM_SPACE	No room for memory in address space.
 *		MACH_MSG_IPC_KERNEL	Resource shortage handling port right.
 *		MACH_MSG_VM_KERNEL	Resource shortage handling memory.
 */

mach_msg_return_t
ipc_kmsg_copyout_pseudo(
	ipc_kmsg_t              kmsg,
	ipc_space_t             space,
	vm_map_t                map,
	mach_msg_body_t         *slist)
{
	mach_msg_bits_t mbits = kmsg->ikm_header->msgh_bits;
	ipc_object_t dest = ip_to_object(kmsg->ikm_header->msgh_remote_port);
	ipc_object_t reply = ip_to_object(kmsg->ikm_header->msgh_local_port);
	ipc_object_t voucher = ip_to_object(kmsg->ikm_voucher);
	mach_msg_type_name_t dest_type = MACH_MSGH_BITS_REMOTE(mbits);
	mach_msg_type_name_t reply_type = MACH_MSGH_BITS_LOCAL(mbits);
	mach_msg_type_name_t voucher_type = MACH_MSGH_BITS_VOUCHER(mbits);
	mach_port_name_t voucher_name = kmsg->ikm_header->msgh_voucher_port;
	mach_port_name_t dest_name, reply_name;
	mach_msg_return_t mr;

	/* Set ith_knote to ITH_KNOTE_PSEUDO */
	current_thread()->ith_knote = ITH_KNOTE_PSEUDO;

	ikm_validate_sig(kmsg);

	assert(IO_VALID(dest));

#if 0
	/*
	 * If we did this here, it looks like we wouldn't need the undo logic
	 * at the end of ipc_kmsg_send() in the error cases.  Not sure which
	 * would be more elegant to keep.
	 */
	ipc_importance_clean(kmsg);
#else
	/* just assert it is already clean */
	ipc_importance_assert_clean(kmsg);
#endif

	mr = (ipc_kmsg_copyout_object(space, dest, dest_type, NULL, NULL, &dest_name) |
	    ipc_kmsg_copyout_object(space, reply, reply_type, NULL, NULL, &reply_name));

	kmsg->ikm_header->msgh_bits = mbits & MACH_MSGH_BITS_USER;
	kmsg->ikm_header->msgh_remote_port = CAST_MACH_NAME_TO_PORT(dest_name);
	kmsg->ikm_header->msgh_local_port = CAST_MACH_NAME_TO_PORT(reply_name);

	if (IO_VALID(voucher)) {
		assert(voucher_type == MACH_MSG_TYPE_MOVE_SEND);

		kmsg->ikm_voucher = IP_NULL;
		mr |= ipc_kmsg_copyout_object(space, voucher, voucher_type, NULL, NULL, &voucher_name);
		kmsg->ikm_header->msgh_voucher_port = voucher_name;
	}

	if (mbits & MACH_MSGH_BITS_COMPLEX) {
		mr |= ipc_kmsg_copyout_body(kmsg, space, map, 0, slist);
	}

	return mr;
}

/*
 *	Routine:	ipc_kmsg_copyout_dest
 *	Purpose:
 *		Copies out the destination port in the message.
 *		Destroys all other rights and memory in the message.
 *	Conditions:
 *		Nothing locked.
 */

void
ipc_kmsg_copyout_dest(
	ipc_kmsg_t      kmsg,
	ipc_space_t     space)
{
	mach_msg_bits_t mbits;
	ipc_object_t dest;
	ipc_object_t reply;
	ipc_object_t voucher;
	mach_msg_type_name_t dest_type;
	mach_msg_type_name_t reply_type;
	mach_msg_type_name_t voucher_type;
	mach_port_name_t dest_name, reply_name, voucher_name;

	ikm_validate_sig(kmsg);

	mbits = kmsg->ikm_header->msgh_bits;
	dest = ip_to_object(kmsg->ikm_header->msgh_remote_port);
	reply = ip_to_object(kmsg->ikm_header->msgh_local_port);
	voucher = ip_to_object(kmsg->ikm_voucher);
	voucher_name = kmsg->ikm_header->msgh_voucher_port;
	dest_type = MACH_MSGH_BITS_REMOTE(mbits);
	reply_type = MACH_MSGH_BITS_LOCAL(mbits);
	voucher_type = MACH_MSGH_BITS_VOUCHER(mbits);

	assert(IO_VALID(dest));

	ipc_importance_assert_clean(kmsg);

	io_lock(dest);
	if (io_active(dest)) {
		ipc_object_copyout_dest(space, dest, dest_type, &dest_name);
		/* dest is unlocked */
	} else {
		io_unlock(dest);
		io_release(dest);
		dest_name = MACH_PORT_DEAD;
	}

	if (IO_VALID(reply)) {
		ipc_object_destroy(reply, reply_type);
		reply_name = MACH_PORT_NULL;
	} else {
		reply_name = CAST_MACH_PORT_TO_NAME(reply);
	}

	if (IO_VALID(voucher)) {
		assert(voucher_type == MACH_MSG_TYPE_MOVE_SEND);

		kmsg->ikm_voucher = IP_NULL;
		ipc_object_destroy(voucher, voucher_type);
		voucher_name = MACH_PORT_NULL;
	}

	kmsg->ikm_header->msgh_bits = MACH_MSGH_BITS_SET(reply_type, dest_type,
	    voucher_type, mbits);
	kmsg->ikm_header->msgh_local_port = CAST_MACH_NAME_TO_PORT(dest_name);
	kmsg->ikm_header->msgh_remote_port = CAST_MACH_NAME_TO_PORT(reply_name);
	kmsg->ikm_header->msgh_voucher_port = voucher_name;

	if (mbits & MACH_MSGH_BITS_COMPLEX) {
		mach_msg_body_t *body;

		body = (mach_msg_body_t *) (kmsg->ikm_header + 1);
		ipc_kmsg_clean_body(kmsg, body->msgh_descriptor_count,
		    (mach_msg_descriptor_t *)(body + 1));
	}
}

/*
 *	Routine:	ipc_kmsg_copyout_to_kernel
 *	Purpose:
 *		Copies out the destination and reply ports in the message.
 *		Leaves all other rights and memory in the message alone.
 *	Conditions:
 *		Nothing locked.
 *
 *	Derived from ipc_kmsg_copyout_dest.
 *	Use by mach_msg_rpc_from_kernel (which used to use copyout_dest).
 *	We really do want to save rights and memory.
 */

void
ipc_kmsg_copyout_to_kernel(
	ipc_kmsg_t      kmsg,
	ipc_space_t     space)
{
	ipc_object_t dest;
	mach_port_t reply;
	mach_msg_type_name_t dest_type;
	mach_msg_type_name_t reply_type;
	mach_port_name_t dest_name;

	ikm_validate_sig(kmsg);

	dest = ip_to_object(kmsg->ikm_header->msgh_remote_port);
	reply = kmsg->ikm_header->msgh_local_port;
	dest_type = MACH_MSGH_BITS_REMOTE(kmsg->ikm_header->msgh_bits);
	reply_type = MACH_MSGH_BITS_LOCAL(kmsg->ikm_header->msgh_bits);

	assert(IO_VALID(dest));

	io_lock(dest);
	if (io_active(dest)) {
		ipc_object_copyout_dest(space, dest, dest_type, &dest_name);
		/* dest is unlocked */
	} else {
		io_unlock(dest);
		io_release(dest);
		dest_name = MACH_PORT_DEAD;
	}

	/*
	 * While MIG kernel users don't receive vouchers, the
	 * msgh_voucher_port field is intended to be round-tripped through the
	 * kernel if there is no voucher disposition set. Here we check for a
	 * non-zero voucher disposition, and consume the voucher send right as
	 * there is no possible way to specify MACH_RCV_VOUCHER semantics.
	 */
	mach_msg_type_name_t voucher_type;
	voucher_type = MACH_MSGH_BITS_VOUCHER(kmsg->ikm_header->msgh_bits);
	if (voucher_type != MACH_MSGH_BITS_ZERO) {
		assert(voucher_type == MACH_MSG_TYPE_MOVE_SEND);
		/*
		 * someone managed to send this kernel routine a message with
		 * a voucher in it. Cleanup the reference in
		 * kmsg->ikm_voucher.
		 */
		if (IP_VALID(kmsg->ikm_voucher)) {
			ipc_port_release_send(kmsg->ikm_voucher);
		}
		kmsg->ikm_voucher = IP_NULL;
		kmsg->ikm_header->msgh_voucher_port = 0;
	}

	kmsg->ikm_header->msgh_bits =
	    (MACH_MSGH_BITS_OTHER(kmsg->ikm_header->msgh_bits) |
	    MACH_MSGH_BITS(reply_type, dest_type));
	kmsg->ikm_header->msgh_local_port =  CAST_MACH_NAME_TO_PORT(dest_name);
	kmsg->ikm_header->msgh_remote_port = reply;
}

#if IKM_SUPPORT_LEGACY
void
ipc_kmsg_copyout_to_kernel_legacy(
	ipc_kmsg_t      kmsg,
	ipc_space_t     space)
{
	ipc_object_t dest;
	mach_port_t  reply;
	mach_msg_type_name_t dest_type;
	mach_msg_type_name_t reply_type;
	mach_port_name_t dest_name;

	ikm_validate_sig(kmsg);

	dest = ip_to_object(kmsg->ikm_header->msgh_remote_port);
	reply = kmsg->ikm_header->msgh_local_port;
	dest_type = MACH_MSGH_BITS_REMOTE(kmsg->ikm_header->msgh_bits);
	reply_type = MACH_MSGH_BITS_LOCAL(kmsg->ikm_header->msgh_bits);

	assert(IO_VALID(dest));

	io_lock(dest);
	if (io_active(dest)) {
		ipc_object_copyout_dest(space, dest, dest_type, &dest_name);
		/* dest is unlocked */
	} else {
		io_unlock(dest);
		io_release(dest);
		dest_name = MACH_PORT_DEAD;
	}

	mach_msg_type_name_t voucher_type;
	voucher_type = MACH_MSGH_BITS_VOUCHER(kmsg->ikm_header->msgh_bits);
	if (voucher_type != MACH_MSGH_BITS_ZERO) {
		assert(voucher_type == MACH_MSG_TYPE_MOVE_SEND);
		assert(IP_VALID(kmsg->ikm_voucher));
		/*
		 * someone managed to send this kernel routine a message with
		 * a voucher in it. Cleanup the reference in
		 * kmsg->ikm_voucher.
		 */
		ipc_port_release_send(kmsg->ikm_voucher);
		kmsg->ikm_voucher = IP_NULL;
		kmsg->ikm_header->msgh_voucher_port = 0;
	}

	kmsg->ikm_header->msgh_bits =
	    (MACH_MSGH_BITS_OTHER(kmsg->ikm_header->msgh_bits) |
	    MACH_MSGH_BITS(reply_type, dest_type));
	kmsg->ikm_header->msgh_local_port =  CAST_MACH_NAME_TO_PORT(dest_name);
	kmsg->ikm_header->msgh_remote_port = reply;

	mach_msg_descriptor_t *saddr;
	mach_msg_legacy_descriptor_t *daddr;
	mach_msg_type_number_t i, count = ((mach_msg_base_t *)kmsg->ikm_header)->body.msgh_descriptor_count;
	saddr = (mach_msg_descriptor_t *) (((mach_msg_base_t *)kmsg->ikm_header) + 1);
	saddr = &saddr[count - 1];
	daddr = (mach_msg_legacy_descriptor_t *)&saddr[count];
	daddr--;

	vm_offset_t dsc_adjust = 0;

	for (i = 0; i < count; i++, saddr--, daddr--) {
		switch (saddr->type.type) {
		case MACH_MSG_PORT_DESCRIPTOR: {
			mach_msg_port_descriptor_t *dsc = &saddr->port;
			mach_msg_legacy_port_descriptor_t *dest_dsc = &daddr->port;

			mach_port_t name = dsc->name;
			mach_msg_type_name_t disposition = dsc->disposition;

			dest_dsc->name = CAST_MACH_PORT_TO_NAME(name);
			dest_dsc->disposition = disposition;
			dest_dsc->type = MACH_MSG_PORT_DESCRIPTOR;
			break;
		}
		case MACH_MSG_OOL_VOLATILE_DESCRIPTOR:
		case MACH_MSG_OOL_DESCRIPTOR: {
			/* The sender should supply ready-made memory, i.e. a vm_map_copy_t
			 * so we don't need to do anything special. */

			mach_msg_ool_descriptor_t       *source_dsc = (typeof(source_dsc)) & saddr->out_of_line;

			mach_msg_ool_descriptor32_t *dest_dsc = &daddr->out_of_line32;

			vm_offset_t                         address = (vm_offset_t)source_dsc->address;
			vm_size_t                       size = source_dsc->size;
			boolean_t                       deallocate = source_dsc->deallocate;
			mach_msg_copy_options_t         copy = source_dsc->copy;
			mach_msg_descriptor_type_t  type = source_dsc->type;

			dest_dsc->address = address;
			dest_dsc->size = size;
			dest_dsc->deallocate = deallocate;
			dest_dsc->copy = copy;
			dest_dsc->type = type;
			break;
		}
		case MACH_MSG_OOL_PORTS_DESCRIPTOR: {
			mach_msg_ool_ports_descriptor_t         *source_dsc = (typeof(source_dsc)) & saddr->ool_ports;

			mach_msg_ool_ports_descriptor32_t   *dest_dsc = &daddr->ool_ports32;

			vm_offset_t                         address = (vm_offset_t)source_dsc->address;
			vm_size_t                       port_count = source_dsc->count;
			boolean_t                       deallocate = source_dsc->deallocate;
			mach_msg_copy_options_t         copy = source_dsc->copy;
			mach_msg_descriptor_type_t  type = source_dsc->type;

			dest_dsc->address = address;
			dest_dsc->count = port_count;
			dest_dsc->deallocate = deallocate;
			dest_dsc->copy = copy;
			dest_dsc->type = type;
			break;
		}
		case MACH_MSG_GUARDED_PORT_DESCRIPTOR: {
			mach_msg_guarded_port_descriptor_t *source_dsc = (typeof(source_dsc)) & saddr->guarded_port;
			mach_msg_guarded_port_descriptor32_t *dest_dsc = &daddr->guarded_port32;

			dest_dsc->name = CAST_MACH_PORT_TO_NAME(source_dsc->name);
			dest_dsc->disposition = source_dsc->disposition;
			dest_dsc->flags = 0;
			dest_dsc->type = MACH_MSG_GUARDED_PORT_DESCRIPTOR;
			dest_dsc->context = 0;
			break;
		}
		default: {
#if     MACH_ASSERT
			panic("ipc_kmsg_copyout_to_kernel_legacy: bad descriptor");
#endif  /* MACH_ASSERT */
		}
		}
	}

	if (count) {
		dsc_adjust = 4 * count;
		memmove((char *)((vm_offset_t)kmsg->ikm_header + dsc_adjust), kmsg->ikm_header, sizeof(mach_msg_base_t));
		kmsg->ikm_header = (mach_msg_header_t *)((vm_offset_t)kmsg->ikm_header + dsc_adjust);
		/* Update the message size for the smaller user representation */
		kmsg->ikm_header->msgh_size -= dsc_adjust;
	}
}
#endif /* IKM_SUPPORT_LEGACY */

#ifdef __arm64__
/*
 * Just sets those parts of the trailer that aren't set up at allocation time.
 */
static void
ipc_kmsg_munge_trailer(mach_msg_max_trailer_t *in, void *_out, boolean_t is64bit)
{
	if (is64bit) {
		mach_msg_max_trailer64_t *out = (mach_msg_max_trailer64_t*)_out;
		out->msgh_seqno = in->msgh_seqno;
		out->msgh_context = in->msgh_context;
		out->msgh_trailer_size = in->msgh_trailer_size;
		out->msgh_ad = in->msgh_ad;
	} else {
		mach_msg_max_trailer32_t *out = (mach_msg_max_trailer32_t*)_out;
		out->msgh_seqno = in->msgh_seqno;
		out->msgh_context = (mach_port_context32_t)in->msgh_context;
		out->msgh_trailer_size = in->msgh_trailer_size;
		out->msgh_ad = in->msgh_ad;
	}
}
#endif /* __arm64__ */

mach_msg_trailer_size_t
ipc_kmsg_trailer_size(
	mach_msg_option_t option,
	__unused thread_t thread)
{
	if (!(option & MACH_RCV_TRAILER_MASK)) {
		return MACH_MSG_TRAILER_MINIMUM_SIZE;
	} else {
		return REQUESTED_TRAILER_SIZE(thread_is_64bit_addr(thread), option);
	}
}

void
ipc_kmsg_add_trailer(ipc_kmsg_t kmsg, ipc_space_t space __unused,
    mach_msg_option_t option, __unused thread_t thread,
    mach_port_seqno_t seqno, boolean_t minimal_trailer,
    mach_vm_offset_t context)
{
	mach_msg_max_trailer_t *trailer;

#ifdef __arm64__
	mach_msg_max_trailer_t tmp_trailer; /* This accommodates U64, and we'll munge */
	void *real_trailer_out = (void*)(mach_msg_max_trailer_t *)
	    ((vm_offset_t)kmsg->ikm_header +
	    mach_round_msg(kmsg->ikm_header->msgh_size));

	/*
	 * Populate scratch with initial values set up at message allocation time.
	 * After, we reinterpret the space in the message as the right type
	 * of trailer for the address space in question.
	 */
	bcopy(real_trailer_out, &tmp_trailer, MAX_TRAILER_SIZE);
	trailer = &tmp_trailer;
#else /* __arm64__ */
	(void)thread;
	trailer = (mach_msg_max_trailer_t *)
	    ((vm_offset_t)kmsg->ikm_header +
	    mach_round_msg(kmsg->ikm_header->msgh_size));
#endif /* __arm64__ */

	if (!(option & MACH_RCV_TRAILER_MASK)) {
		return;
	}

	trailer->msgh_seqno = seqno;
	trailer->msgh_context = context;
	trailer->msgh_trailer_size = REQUESTED_TRAILER_SIZE(thread_is_64bit_addr(thread), option);

	if (minimal_trailer) {
		goto done;
	}

	if (GET_RCV_ELEMENTS(option) >= MACH_RCV_TRAILER_AV) {
		trailer->msgh_ad = kmsg->ikm_filter_policy_id;
	}

	/*
	 * The ipc_kmsg_t holds a reference to the label of a label
	 * handle, not the port. We must get a reference to the port
	 * and a send right to copyout to the receiver.
	 */

	if (option & MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_LABELS)) {
		trailer->msgh_labels.sender = 0;
	}

done:
#ifdef __arm64__
	ipc_kmsg_munge_trailer(trailer, real_trailer_out, thread_is_64bit_addr(thread));
#endif /* __arm64__ */
	return;
}

mach_msg_header_t *
ipc_kmsg_msg_header(ipc_kmsg_t kmsg)
{
	if (NULL == kmsg) {
		return NULL;
	}
	return kmsg->ikm_header;
}
