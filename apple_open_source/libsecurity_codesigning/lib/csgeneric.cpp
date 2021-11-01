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
// csgeneric - generic Code representative
//
#include "csgeneric.h"
#include "cs.h"
#include "StaticCode.h"
#include <securityd_client/cshosting.h>
#include <sys/param.h>

namespace Security {
namespace CodeSigning {

using MachPlusPlus::Port;


//
// Common call-out code for cshosting RPC service
//
#define CALL(host, name, args...) \
	OSStatus result; \
	if (cshosting_client_ ## name (host, mig_get_reply_port(), &result, args)) \
		MacOSError::throwMe(errSecCSNotAHost); \
	MacOSError::check(result);


//
// Construct a running process representation
//
GenericCode::GenericCode(SecCode *host, SecGuestRef guestRef)
	: SecCode(host), mGuestRef(guestRef)
{
}


//
// Identify a guest by attribute set, and return a new GenericCode representing it.
// This uses cshosting RPCs to ask the host (or its proxy).
//
SecCode *GenericCode::locateGuest(CFDictionaryRef attributes)
{
	if (Port host = hostingPort()) {
		CFRef<CFDataRef> attrData;
		void *attrPtr = NULL; size_t attrLength = 0;
		if (attributes) {
			attrData.take(CFPropertyListCreateXMLData(NULL, attributes));
			attrPtr = (void *)CFDataGetBytePtr(attrData);
			attrLength = CFDataGetLength(attrData);
		}
		GuestChain guestPath;
		mach_msg_type_number_t guestPathLength;
		mach_port_t subport;
		CALL(host, findGuest, guestRef(), attrPtr, attrLength,
			&guestPath, &guestPathLength, &subport);
		CODESIGN_GUEST_LOCATE_GENERIC(this, guestPath, guestPathLength, subport);
		SecPointer<SecCode> code = this;
		for (unsigned n = 0; n < guestPathLength; n++)
			code = new GenericCode(code, guestPath[n]);
		return code.yield();
	} else
		return NULL;		// not found, no error
}


//
// Identify a guest by returning its StaticCode and running CodeDirectory hash.
// This uses cshosting RPCs to ask the host (or its proxy).
//
SecStaticCode *GenericCode::identifyGuest(SecCode *guest, CFDataRef *cdhashOut)
{
	if (GenericCode *iguest = dynamic_cast<GenericCode *>(guest)) {
		FilePathOut path;
		CFRef<CFDataRef> cdhash;
		CFDictionary attributes(errSecCSHostProtocolInvalidAttribute);
		identifyGuest(iguest->guestRef(), path, cdhash.aref(), attributes.aref());
		DiskRep::Context ctx;
		if (CFNumberRef architecture = attributes.get<CFNumberRef>(kSecGuestAttributeArchitecture)) {
			cpu_type_t cpu = cfNumber<cpu_type_t>(architecture);
			if (CFNumberRef subarchitecture = attributes.get<CFNumberRef>(kSecGuestAttributeSubarchitecture))
				ctx.arch = Architecture(cpu, cfNumber<cpu_subtype_t>(subarchitecture));
			else
				ctx.arch = Architecture(cpu);
		}
		SecPointer<GenericStaticCode> code = new GenericStaticCode(DiskRep::bestGuess(path, &ctx));
		CODESIGN_GUEST_IDENTIFY_GENERIC(iguest, iguest->guestRef(), code);
		if (cdhash) {
			CODESIGN_GUEST_CDHASH_GENERIC(iguest, (void *)CFDataGetBytePtr(cdhash), CFDataGetLength(cdhash));
			*cdhashOut = cdhash.yield();
		}
		return code.yield();
	} else
		MacOSError::throwMe(errSecCSNotAHost);
}

// helper to drive the identifyGuest hosting IPC and return results as CF objects
void GenericCode::identifyGuest(SecGuestRef guest, char *path, CFDataRef &cdhash, CFDictionaryRef &attributes)
{
	if (Port host = hostingPort()) {
		HashDataOut hash;
		uint32_t hashLength;
		XMLBlobOut attr;
		uint32_t attrLength;
		CALL(host, identifyGuest, guest, path, hash, &hashLength, &attr, &attrLength);
		if (hashLength)
			cdhash = makeCFData(hash, hashLength);
		if (attrLength) {
			CFRef<CFDataRef> attrData = makeCFData(attr, attrLength);
			attributes = makeCFDictionaryFrom(attrData);
#if ROSETTA_TEST_HACK
			CFMutableDictionaryRef hattr = makeCFMutableDictionary(attributes);
			CFDictionaryAddValue(hattr, kSecGuestAttributeArchitecture, CFTempNumber(CPU_TYPE_POWERPC));
			CFRelease(attributes);
			attributes = hattr;
#endif
		}
	} else
		MacOSError::throwMe(errSecCSNotAHost);
}


//
// Get the Code Signing Status Word for a Code.
// This uses cshosting RPCs to ask the host (or its proxy).
//
SecCodeStatus GenericCode::getGuestStatus(SecCode *guest)
{
	if (Port host = hostingPort()) {
		uint32_t status;
		CALL(host, guestStatus, safe_cast<GenericCode *>(guest)->guestRef(), &status);
		return status;
	} else
		MacOSError::throwMe(errSecCSNotAHost);
}


//
// Status changes are transmitted through the cshosting RPCs.
//
void GenericCode::changeGuestStatus(SecCode *iguest, SecCodeStatusOperation operation, CFDictionaryRef arguments)
{
	if (/* GenericCode *guest = */dynamic_cast<GenericCode *>(iguest))
		switch (operation) {
		case kSecCodeOperationNull:
			break;
		case kSecCodeOperationInvalidate:
		case kSecCodeOperationSetHard:
		case kSecCodeOperationSetKill:
			MacOSError::throwMe(errSecCSUnimplemented);
			break;
		default:
			MacOSError::throwMe(errSecCSUnimplemented);
		}
	else
		MacOSError::throwMe(errSecCSNoSuchCode);
}


//
// Return the Hosting Port for this Code.
// May return MACH_PORT_NULL if the code is not a code host.
// Throws if we can't get the hosting port for some reason (and can't positively
// determine that there is none).
//
// Note that we do NOT cache negative outcomes. Being a host is a dynamic property,
// and this Code may not have commenced hosting operations yet. For non- (or not-yet-)hosts
// we simply return NULL.
//
Port GenericCode::hostingPort()
{
	if (!mHostingPort) {
		if (staticCode()->codeDirectory()->flags & kSecCodeSignatureHost) {
			mHostingPort = getHostingPort();
			CODESIGN_GUEST_HOSTINGPORT(this, mHostingPort);
		}
	}
	return mHostingPort;	
}


//
// A pure GenericHost has no idea where to get a hosting port from.
// This method must be overridden to get one.
// However, we do handle a contiguous chain of GenericCodes by deferring
// to our next-higher host for it.
//
mach_port_t GenericCode::getHostingPort()
{
	if (GenericCode *genericHost = dynamic_cast<GenericCode *>(host()))
		return genericHost->getHostingPort();
	else
		MacOSError::throwMe(errSecCSNotAHost);
}


} // CodeSigning
} // Security
