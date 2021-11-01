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
// signer - Signing operation supervisor and controller
//
#ifndef _H_SIGNER
#define _H_SIGNER

#include "CodeSigner.h"
#include "cdbuilder.h"
#include "signerutils.h"
#include "StaticCode.h"
#include <security_utilities/utilities.h>

namespace Security {
namespace CodeSigning {


//
// The signer driver class.
// This is a workflow object, containing all the data needed for the various
// signing stages to cooperate. It is not meant to be API visible; that is
// SecCodeSigner's job.
//
class SecCodeSigner::Signer {
public:
	Signer(SecCodeSigner &s, SecStaticCode *c) : state(s), code(c) { }
	void sign(SecCSFlags flags);
	void remove(SecCSFlags flags);
	
	SecCodeSigner &state;
	SecStaticCode * const code;
	
	CodeDirectory::HashAlgorithm digestAlgorithm() const { return state.mDigestAlgorithm; }
	
	std::string path() const { return cfString(rep->canonicalPath()); }
	SecIdentityRef signingIdentity() const { return state.mSigner; }
	std::string signingIdentifier() const { return identifier; }
	
protected:
	void prepare(SecCSFlags flags);				// set up signing parameters
	void signMachO(Universal *fat, const Requirement::Context &context); // sign a Mach-O binary
	void signArchitectureAgnostic(const Requirement::Context &context); // sign anything else

	void populate(DiskRep::Writer &writer);		// global
	void populate(CodeDirectory::Builder &builder, DiskRep::Writer &writer,
		InternalRequirements &ireqs, size_t offset = 0, size_t length = 0);	// per-architecture
	CFDataRef signCodeDirectory(const CodeDirectory *cd);

	uint32_t cdTextFlags(std::string text);		// convert text CodeDirectory flags
	std::string uniqueName() const;				// derive unique string from rep
	
private:
	RefPointer<DiskRep> rep;		// DiskRep of Code being signed
	CFRef<CFDataRef> resourceDirectory;	// resource directory
	std::string identifier;			// signing identifier
	uint32_t cdFlags;				// CodeDirectory flags
	size_t pagesize;				// size of main executable pages
	CFAbsoluteTime signingTime;		// signing time for CMS signature (0 => none)
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_CODESIGNER
