/*
 * Copyright (c) 2006-2010 Apple Inc. All Rights Reserved.
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
// CodeSigner - SecCodeSigner API objects
//
#ifndef _H_CODESIGNER
#define _H_CODESIGNER

#include "cs.h"
#include "StaticCode.h"
#include "cdbuilder.h"
#include <Security/SecIdentity.h>
#include <security_utilities/utilities.h>

namespace Security {
namespace CodeSigning {


//
// A SecCode object represents running code in the system. It must be subclassed
// to implement a particular notion of code.
//
class SecCodeSigner : public SecCFObject, public DiskRep::SigningContext {
	NOCOPY(SecCodeSigner)
public:
	class Parser;
	class Signer;

public:
	SECCFFUNCTIONS(SecCodeSigner, SecCodeSignerRef, errSecCSInvalidObjectRef, gCFObjects().CodeSigner)

	SecCodeSigner(SecCSFlags flags);
    virtual ~SecCodeSigner() throw();
	
	void parameters(CFDictionaryRef args);	// parse and set parameters
	bool valid() const;
	
	void sign(SecStaticCode *code, SecCSFlags flags);
	void remove(SecStaticCode *code, SecCSFlags flags);
	
	void returnDetachedSignature(BlobCore *blob, Signer &signer);
	
protected:
	std::string sdkPath(const std::string &path) const;
	bool isAdhoc() const;
	
private:
	// parsed parameter set
	SecCSFlags mOpFlags;			// operation flags
	CFRef<SecIdentityRef> mSigner;	// signing identity
	CFRef<CFTypeRef> mDetached;		// detached-signing information (NULL => attached)
	CFRef<CFDictionaryRef> mResourceRules; // explicit resource collection rules (override)
	CFRef<CFDateRef> mSigningTime;	// signing time desired (kCFNull for none)
	CFRef<CFDataRef> mApplicationData; // contents of application slot
	CFRef<CFDataRef> mEntitlementData; // entitlement configuration data
	CFRef<CFURLRef> mSDKRoot;		// substitute filesystem root for sub-component lookup
	const Requirements *mRequirements; // internal code requirements
	size_t mCMSSize;				// size estimate for CMS blob
	uint32_t mCdFlags;				// CodeDirectory flags
	bool mCdFlagsGiven;				// CodeDirectory flags were specified
	CodeDirectory::HashAlgorithm mDigestAlgorithm; // interior digest (hash) algorithm
	std::string mIdentifier;		// unique identifier override
	std::string mIdentifierPrefix;	// prefix for un-dotted default identifiers
	bool mNoMachO;					// override to perform non-Mach-O signing
	bool mDryRun;					// dry run (do not change target)
	CFRef<CFNumberRef> mPageSize;	// main executable page size
	CFRef<SecIdentityRef> mTimestampAuthentication;	// identity for client-side authentication to the Timestamp server
	CFRef<CFURLRef> mTimestampService;		// URL for Timestamp server
    bool mWantTimeStamp;          // use a Timestamp server
    bool mNoTimeStampCerts;       // don't request certificates with timestamping request
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_CODESIGNER
