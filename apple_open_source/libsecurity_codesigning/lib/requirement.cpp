/*
 * Copyright (c) 2006-2012 Apple Inc. All Rights Reserved.
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
// requirement - Code Requirement Blob description
//
#include "requirement.h"
#include "reqinterp.h"
#include "codesigning_dtrace.h"
#include <security_utilities/errors.h>
#include <security_utilities/unix++.h>
#include <security_utilities/logging.h>
#include <security_utilities/cfutilities.h>
#include <security_utilities/hashing.h>

#ifdef DEBUGDUMP
#include <security_codesigning/reqdumper.h>
#endif

namespace Security {
namespace CodeSigning {


//
// The (SHA-1) hash of the canonical Apple certificate root anchor
//
static const SHA1::Digest gAppleAnchorHash =
	{ 0x61, 0x1e, 0x5b, 0x66, 0x2c, 0x59, 0x3a, 0x08, 0xff, 0x58,
	  0xd1, 0x4a, 0xe2, 0x24, 0x52, 0xd1, 0x98, 0xdf, 0x6c, 0x60 };


//
// Canonical names for requirement types
//
const char *const Requirement::typeNames[] = {
	"invalid",
	"host",
	"guest",
	"designated",
	"library",
	"plugin",
};


//
// validate a requirement against a code context
//
void Requirement::validate(const Requirement::Context &ctx, OSStatus failure /* = errSecCSReqFailed */) const
{
	if (!this->validates(ctx, failure))
		MacOSError::throwMe(failure);
}

bool Requirement::validates(const Requirement::Context &ctx, OSStatus failure /* = errSecCSReqFailed */) const
{
	CODESIGN_EVAL_REQINT_START((void*)this, this->length());
	switch (kind()) {
	case exprForm:
		if (Requirement::Interpreter(this, &ctx).evaluate()) {
			CODESIGN_EVAL_REQINT_END(this, 0);
			return true;
		} else {
			CODESIGN_EVAL_REQINT_END(this, failure);
			return false;
		}
	default:
		CODESIGN_EVAL_REQINT_END(this, errSecCSReqUnsupported);
		MacOSError::throwMe(errSecCSReqUnsupported);
	}
}


//
// Retrieve one certificate from the cert chain.
// Positive and negative indices can be used:
//    [ leaf, intermed-1, ..., intermed-n, anchor ]
//        0       1       ...     -2         -1
// Returns NULL if unavailable for any reason.
//	
SecCertificateRef Requirement::Context::cert(int ix) const
{
	if (certs) {
		if (ix < 0)
			ix += certCount();
		if (ix >= CFArrayGetCount(certs))
		    return NULL;
		if (CFTypeRef element = CFArrayGetValueAtIndex(certs, ix))
			return SecCertificateRef(element);
	}
	return NULL;
}

unsigned int Requirement::Context::certCount() const
{
	if (certs)
		return CFArrayGetCount(certs);
	else
		return 0;
}


//
// Return the hash of the canonical Apple certificate root (anchor).
// In a special test mode, also return an alternate root hash for testing.
//
const SHA1::Digest &Requirement::appleAnchorHash()
{
	return gAppleAnchorHash;
}

#if defined(TEST_APPLE_ANCHOR)

const char Requirement::testAppleAnchorEnv[] = "TEST_APPLE_ANCHOR";

const SHA1::Digest &Requirement::testAppleAnchorHash()
{
	static bool tried = false;
	static SHA1::Digest testHash;
	if (!tried) {
		// see if we have one configured
		if (const char *path = getenv(testAppleAnchorEnv))
			try {
				UnixPlusPlus::FileDesc fd(path);
				char buffer[2048];		// arbitrary limit
				size_t size = fd.read(buffer, sizeof(buffer));
				SHA1 hash;
				hash(buffer, size);
				hash.finish(testHash);
				Syslog::alert("ACCEPTING TEST AUTHORITY %s FOR APPLE CODE IDENTITY", path);
			} catch (...) { }
		tried = true;
	}
	return testHash;		// will be zeroes (no match) if not configured
}

#endif //TEST_APPLE_ANCHOR


//
// Debug dump support
//
#ifdef DEBUGDUMP

void Requirement::dump() const
{
	Debug::dump("%s\n", Dumper::dump(this).c_str());
}

#endif //DEBUGDUMP


}	// CodeSigning
}	// Security
