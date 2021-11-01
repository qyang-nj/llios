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
#ifndef _H_DETACHEDREP
#define _H_DETACHEDREP

#include "diskrep.h"
#include "sigblob.h"

namespace Security {
namespace CodeSigning {


//
// We use a DetachedRep to interpose (filter) the genuine DiskRep representing
// the code on disk, *if* a detached signature was set on this object. In this
// situation, mRep will point to a (2 element) chain of DiskReps.
//
// This is a neat way of dealing with the (unusual) detached-signature case
// without disturbing things unduly. Consider DetachedDiskRep to be closely
// married to SecStaticCode; it's unlikely to work right if you use it elsewhere.
//
// Note that there's no *writing* code here. Writing detached signatures is handled
// specially in the signing code.
//
class DetachedRep : public FilterRep {
public:
	DetachedRep(CFDataRef sig, DiskRep *orig, const std::string &source); // SuperBlob of all architectures
	DetachedRep(CFDataRef sig, CFDataRef gsig, DiskRep *orig, const std::string &source); // one architecture + globals
	
	CFDataRef component(CodeDirectory::SpecialSlot slot);
	
	const std::string &source() const { return mSource; }

private:
	CFRef<CFDataRef> mSig, mGSig;
	const EmbeddedSignatureBlob *mArch;		// current architecture; points into mSignature
	const EmbeddedSignatureBlob *mGlobal;	// shared elements; points into mSignature
	std::string mSource;					// source description (readable)
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_DETACHEDREP
