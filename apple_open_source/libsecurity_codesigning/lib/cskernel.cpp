/*
 * Copyright (c) 2006-2007 Apple Inc. All Rights Reserved.
 * 
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

//
// cskernel - Kernel implementation of the Code Signing Host Interface.
//
// The kernel host currently supports only UNIX processes as guests.
// It tracks then by their pid. Perhaps one day we'll get a more stable
// means of tracking processes that doesn't involve reusing identifiers.
//
// The kernel host could represent non-process guests one day. One candidate
// are Kernel Extensions.
//
#include "cskernel.h"
#include "csprocess.h"
#include "kerneldiskrep.h"
#include "machorep.h"
#include <libproc.h>
#include <sys/codesign.h>
#include <sys/param.h>	// MAXPATHLEN

namespace Security {
namespace CodeSigning {


//
// The running-kernel singletons
//
ModuleNexus<KernelCode::Globals> KernelCode::globals;

KernelCode::Globals::Globals()
{
	code = new KernelCode;
	staticCode = new KernelStaticCode;
}

KernelCode::KernelCode()
	: SecCode(NULL)
{
}

KernelStaticCode::KernelStaticCode()
	: SecStaticCode(new KernelDiskRep())
{
}


//
// Identify our guests (UNIX processes) by attribute.
// The only supported lookup attribute is currently the pid. (We could support
// task ports, but those can easily be mapped to pids.)
// Note that we don't actually validate the pid here; if it's invalid, we'll notice
// when we try to ask the kernel about it later.
//
SecCode *KernelCode::locateGuest(CFDictionaryRef attributes)
{
	if (CFTypeRef attr = CFDictionaryGetValue(attributes, kSecGuestAttributePid)) {
		if (CFDictionaryGetCount(attributes) != 1)
			MacOSError::throwMe(errSecCSUnsupportedGuestAttributes); // had more
		if (CFGetTypeID(attr) == CFNumberGetTypeID())
			return (new ProcessCode(cfNumber<pid_t>(CFNumberRef(attr))))->retain();
		MacOSError::throwMe(errSecCSInvalidAttributeValues);
	} else
		MacOSError::throwMe(errSecCSUnsupportedGuestAttributes);
}


//
// We map guests to disk by calling a kernel service.
// It is here that we verify that our user-space concept of the code identity
// matches the kernel's idea (to defeat just-in-time switching attacks).
//
SecStaticCode *KernelCode::identifyGuest(SecCode *iguest, CFDataRef *cdhash)
{
	if (ProcessCode *guest = dynamic_cast<ProcessCode *>(iguest)) {
		char path[2 * MAXPATHLEN];	// reasonable upper limit
		if (::proc_pidpath(guest->pid(), path, sizeof(path))) {
			off_t offset;
			csops(guest, CS_OPS_PIDOFFSET, &offset, sizeof(offset));
			SecPointer<SecStaticCode> code = new ProcessStaticCode(DiskRep::bestGuess(path, offset));
			CODESIGN_GUEST_IDENTIFY_PROCESS(guest, guest->pid(), code);
			if (cdhash) {
				SHA1::Digest kernelHash;
				if (::csops(guest->pid(), CS_OPS_CDHASH, kernelHash, sizeof(kernelHash)) == -1)
					switch (errno) {
					case EBADEXEC:		// means "no CodeDirectory hash for this program"
						*cdhash = NULL;
						break;
					case ESRCH:
						MacOSError::throwMe(errSecCSNoSuchCode);
					default:
						UnixError::throwMe();
					}
				else	// succeeded
					*cdhash = makeCFData(kernelHash, sizeof(kernelHash));
				CODESIGN_GUEST_CDHASH_PROCESS(guest, kernelHash, sizeof(kernelHash));
			}
			return code.yield();
		} else
			UnixError::throwMe();
	}
	MacOSError::throwMe(errSecCSNoSuchCode);
}


//
// We obtain the guest's status by asking the kernel
//
SecCodeStatus KernelCode::getGuestStatus(SecCode *iguest)
{
	if (ProcessCode *guest = dynamic_cast<ProcessCode *>(iguest)) {
		uint32_t pFlags;
		csops(guest, CS_OPS_STATUS, &pFlags);
		secdebug("kcode", "guest %p(%d) kernel status 0x%x", guest, guest->pid(), pFlags);
		return pFlags;
	} else
		MacOSError::throwMe(errSecCSNoSuchCode);
}


//
// We tell the kernel to make status changes
//
void KernelCode::changeGuestStatus(SecCode *iguest, SecCodeStatusOperation operation, CFDictionaryRef arguments)
{
	if (ProcessCode *guest = dynamic_cast<ProcessCode *>(iguest))
		switch (operation) {
		case kSecCodeOperationNull:
			break;
		case kSecCodeOperationInvalidate:
			csops(guest, CS_OPS_MARKINVALID);
			break;
		case kSecCodeOperationSetHard:
			csops(guest, CS_OPS_MARKHARD);
			break;
		case kSecCodeOperationSetKill:
			csops(guest, CS_OPS_MARKKILL);
			break;
		default:
			MacOSError::throwMe(errSecCSUnimplemented);
		}
	else
		MacOSError::throwMe(errSecCSNoSuchCode);
}


//
// The StaticCode for the running kernel is explicit.
// We can't ask our own host for it, naturally.
//
void KernelCode::identify()
{
	mStaticCode.take(globals().staticCode->retain());
	// the kernel isn't currently signed, so we don't get a cdHash for it
}


//
// Interface to kernel csops() system call.
//
void KernelCode::csops(ProcessCode *proc, unsigned int op, void *addr, size_t length)
{
	if (::csops(proc->pid(), op, addr, length) == -1) {
		switch (errno) {
		case ESRCH:
			MacOSError::throwMe(errSecCSNoSuchCode);
		default:
			UnixError::throwMe();
		}
	}
}


} // CodeSigning
} // Security
