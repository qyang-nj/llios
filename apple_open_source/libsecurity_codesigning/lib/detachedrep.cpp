/*
 * Copyright (c) 2006-2008 Apple Inc. All Rights Reserved.
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
// detachedrep - prefix diskrep representing a detached signature stored in a file
//
#include "detachedrep.h"


namespace Security {
namespace CodeSigning {


//
// We construct a DetachedRep from the data blob of the detached signature
// and a reference of the original DiskRep we chain to.
// We accept an EmbeddedSignatureBlob (for a non-architected signature)
// or a DetachedSignatureBlob (for architected signatures) that is a SuperBlob
// of EmbeddedSignatureBlobs.
//
DetachedRep::DetachedRep(CFDataRef sig, DiskRep *orig, const std::string &source)
	: FilterRep(orig), mSig(sig), mSource(source)
{
	const BlobCore *sigBlob = reinterpret_cast<const BlobCore *>(CFDataGetBytePtr(sig));
	if (sigBlob->is<EmbeddedSignatureBlob>()) {		// architecture-less
		if ((mArch = EmbeddedSignatureBlob::specific(sigBlob))) {
			mGlobal = NULL;
			CODESIGN_DISKREP_CREATE_DETACHED(this, orig, (char*)source.c_str(), NULL);
			return;
		}
	} else if (sigBlob->is<DetachedSignatureBlob>())	// architecture collection
		if (const DetachedSignatureBlob *dsblob = DetachedSignatureBlob::specific(sigBlob))
			if (Universal *fat = orig->mainExecutableImage())
				if (const BlobCore *blob = dsblob->find(fat->bestNativeArch().cpuType()))
					if ((mArch = EmbeddedSignatureBlob::specific(blob)))
						if ((mGlobal = EmbeddedSignatureBlob::specific(dsblob->find(0)))) {
							CODESIGN_DISKREP_CREATE_DETACHED(this, orig, (char*)source.c_str(), (void*)mGlobal);
							return;
						}
	MacOSError::throwMe(errSecCSSignatureInvalid);
}


//
// Here's a version to construct a DetachedRep if we already have the right architecture
// and (optional) associated global blob. Just take them.
//
DetachedRep::DetachedRep(CFDataRef sig, CFDataRef gsig, DiskRep *orig, const std::string &source)
	: FilterRep(orig), mSig(sig), mGSig(gsig), mSource(source)
{
	const BlobCore *sigBlob = reinterpret_cast<const BlobCore *>(CFDataGetBytePtr(sig));
	mArch = EmbeddedSignatureBlob::specific(sigBlob);
	if (!mArch)
		MacOSError::throwMe(errSecCSSignatureInvalid);
	if (gsig) {
		const BlobCore *gsigBlob = reinterpret_cast<const BlobCore *>(CFDataGetBytePtr(gsig));
		mGlobal = EmbeddedSignatureBlob::specific(gsigBlob);
		if (!mGlobal)
			MacOSError::throwMe(errSecCSSignatureInvalid);
	} else
		mGlobal = NULL;
	CODESIGN_DISKREP_CREATE_DETACHED(this, orig, (char*)source.c_str(), (void*)mGlobal);
}


//
// We look up components by first checking for a per-architecture item,
// then for a global item in the detached signature, and finally falling
// back on the original DiskRep (for static components).
//
CFDataRef DetachedRep::component(CodeDirectory::SpecialSlot slot)
{
	if (CFDataRef result = mArch->component(slot))
		return result;
	if (mGlobal)
		if (CFDataRef result = mGlobal->component(slot))
			return result;
	return this->base()->component(slot);
}


} // end namespace CodeSigning
} // end namespace Security
