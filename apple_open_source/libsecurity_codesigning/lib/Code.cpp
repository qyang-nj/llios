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
// Code - SecCode API objects
//
#include "Code.h"
#include "StaticCode.h"
#include <Security/SecCodeHost.h>
#include "cskernel.h"
#include <security_utilities/cfmunge.h>
#include <security_utilities/debugging.h>

namespace Security {
namespace CodeSigning {


//
// Construction
//
SecCode::SecCode(SecCode *host)
	: mHost(host), mIdentified(false)
{
	CODESIGN_DYNAMIC_CREATE(this, host);
}


//
// Clean up a SecCode object
//
SecCode::~SecCode() throw()
try {
} catch (...) {
	return;
}


//
// CF-level comparison of SecStaticCode objects compares CodeDirectory hashes if signed,
// and falls back on comparing canonical paths if (both are) not.
//
bool SecCode::equal(SecCFObject &secOther)
{
	SecCode *other = static_cast<SecCode *>(&secOther);
	CFDataRef mine = this->cdHash();
	CFDataRef his = other->cdHash();
	if (mine || his)
		return mine && his && CFEqual(mine, his);
	else
		return this->staticCode()->equal(*other->staticCode());
}

CFHashCode SecCode::hash()
{
	if (CFDataRef h = this->cdHash())
		return CFHash(h);
	else
		return this->staticCode()->hash();
}


//
// Yield the host Code
//
SecCode *SecCode::host() const
{
	return mHost;
}


//
// Yield the static code. This is cached.
// The caller does not own the object returned; it lives (at least) as long
// as the SecCode it was derived from.
//
SecStaticCode *SecCode::staticCode()
{
	if (!mIdentified) {
		this->identify();
		mIdentified = true;
	}
	assert(mStaticCode);
	return mStaticCode;
}


//
// Yield the CodeDirectory hash as presented by our host.
// This usually is the same as the hash of staticCode().codeDirectory(), but might not
// if files are changing on disk while code is running.
//
CFDataRef SecCode::cdHash()
{
	if (!mIdentified) {
		this->identify();
		mIdentified = true;
	}
	return mCDHash;		// can be NULL (host has no dynamic identity for guest)
}


//
// Retrieve current dynamic status.
//
SecCodeStatus SecCode::status()
{
	if (this->isRoot())
		return kSecCodeStatusValid;			// root of trust, presumed valid
	else
		return this->host()->getGuestStatus(this);
}

void SecCode::status(SecCodeStatusOperation operation, CFDictionaryRef arguments)
{
	if (this->isRoot())
		MacOSError::throwMe(errSecCSHostProtocolStateError);
	else
		this->host()->changeGuestStatus(this, operation, arguments);
}


//
// By default, we have no guests
//
SecCode *SecCode::locateGuest(CFDictionaryRef)
{
	return NULL;
}


//
// By default, we self-identify by asking our host to identify us.
// (This is currently only overridden in the root-of-trust (kernel) implementation.)
//
void SecCode::identify()
{
	mStaticCode.take(host()->identifyGuest(this, &mCDHash.aref()));
}


//
// The default implementation cannot map guests to disk
//
SecStaticCode *SecCode::identifyGuest(SecCode *, CFDataRef *)
{
	MacOSError::throwMe(errSecCSNoSuchCode);
}


//
// Master validation function.
//
// This is the most important function in all of Code Signing. It performs
// dynamic validation on running code. Despite its simple structure, it does
// everything that's needed to establish whether a Code is currently valid...
// with a little help from StaticCode, format drivers, type drivers, and so on.
//
// This function validates internal requirements in the hosting chain. It does
// not validate external requirements - the caller needs to do that with a separate call.
//
void SecCode::checkValidity(SecCSFlags flags)
{
	if (this->isRoot()) {
		// the root-of-trust is valid by definition
		CODESIGN_EVAL_DYNAMIC_ROOT(this);
		return;
	}
	DTRACK(CODESIGN_EVAL_DYNAMIC, this, (char*)this->staticCode()->mainExecutablePath().c_str());
	
	//
	// Do not reorder the operations below without thorough cogitation. There are
	// interesting dependencies and significant performance issues. There is also
	// client code that relies on errors being noticed in a particular order.
	//
	// For the most part, failure of (reliable) identity will cause exceptions to be
	// thrown, and success is indicated by survival. If you make it to the end,
	// you have won the validity race. (Good rat.)
	//

	// check my host first, recursively
	this->host()->checkValidity(flags);

	SecStaticCode *myDisk = this->staticCode();
	SecStaticCode *hostDisk = this->host()->staticCode();

	// check my static state
	myDisk->validateDirectory();

	// check my own dynamic state
	if (!(this->host()->getGuestStatus(this) & kSecCodeStatusValid))
		MacOSError::throwMe(errSecCSGuestInvalid);
	
	// check that static and dynamic views are consistent
	if (this->cdHash() && !CFEqual(this->cdHash(), myDisk->cdHash()))
		MacOSError::throwMe(errSecCSStaticCodeChanged);

	// check host/guest constraints
	if (!this->host()->isRoot()) {	// not hosted by root of trust
		myDisk->validateRequirements(kSecHostRequirementType, hostDisk, errSecCSHostReject);
		hostDisk->validateRequirements(kSecGuestRequirementType, myDisk);
	}
}


//
// By default, we track no validity for guests (we don't have any)
//
uint32_t SecCode::getGuestStatus(SecCode *guest)
{
	MacOSError::throwMe(errSecCSNoSuchCode);
}

void SecCode::changeGuestStatus(SecCode *guest, SecCodeStatusOperation operation, CFDictionaryRef arguments)
{
	MacOSError::throwMe(errSecCSNoSuchCode);
}


//
// Given a bag of attribute values, automagically come up with a SecCode
// without any other information.
// This is meant to be the "just do what makes sense" generic call, for callers
// who don't want to engage in the fascinating dance of manual guest enumeration.
//
// Note that we expect the logic embedded here to change over time (in backward
// compatible fashion, one hopes), and that it's all right to use heuristics here
// as long as it's done sensibly.
//
// Be warned that the present logic is quite a bit ad-hoc, and will likely not
// handle arbitrary combinations of proxy hosting, dynamic hosting, and dedicated
// hosting all that well.
//
SecCode *SecCode::autoLocateGuest(CFDictionaryRef attributes, SecCSFlags flags)
{
	// special case: with no attributes at all, return the root of trust
	if (CFDictionaryGetCount(attributes) == 0)
		return KernelCode::active()->retain();
	
	// main logic: we need a pid, and we'll take a canonical guest id as an option
	int pid = 0;
	if (!cfscan(attributes, "{%O=%d}", kSecGuestAttributePid, &pid))
		CSError::throwMe(errSecCSUnsupportedGuestAttributes, kSecCFErrorGuestAttributes, attributes);
	if (SecCode *process =
			KernelCode::active()->locateGuest(CFTemp<CFDictionaryRef>("{%O=%d}", kSecGuestAttributePid, pid))) {
		SecPointer<SecCode> code;
		code.take(process);		// locateGuest gave us a retained object
		if (code->staticCode()->flag(kSecCodeSignatureHost)) {
			// might be a code host. Let's find out
			CFRef<CFMutableDictionaryRef> rest = makeCFMutableDictionary(attributes);
			CFDictionaryRemoveValue(rest, kSecGuestAttributePid);
			if (SecCode *guest = code->locateGuest(rest))
				return guest;
		}
		if (!CFDictionaryGetValue(attributes, kSecGuestAttributeCanonical)) {
			// only "soft" attributes, and no hosting is happening. Return the (non-)host itself
			return code.yield();
		}
	}
	MacOSError::throwMe(errSecCSNoSuchCode);
}


} // end namespace CodeSigning
} // end namespace Security
