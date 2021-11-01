/*
 * Copyright (c) 2006 Apple Computer, Inc. All Rights Reserved.
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
// sigblob - signature (Super)Blob types
//
#ifndef _H_SIGBLOB
#define _H_SIGBLOB

#include "codedirectory.h"
#include <security_utilities/superblob.h>
#include <CoreFoundation/CFData.h>

namespace Security {
namespace CodeSigning {


//
// An EmbeddedSignatureBlob is a SuperBlob indexed by component slot number.
// This is what we embed in Mach-O images. It is also what we use for detached
// signatures for non-Mach-O binaries.
//
class EmbeddedSignatureBlob : public SuperBlobCore<EmbeddedSignatureBlob, 0xfade0cc0, uint32_t> {
	typedef SuperBlobCore<EmbeddedSignatureBlob, 0xfade0cc0, uint32_t> _Core;
public:
	CFDataRef component(CodeDirectory::SpecialSlot slot) const;
	
	class Maker : public _Core::Maker {
	public:
		void component(CodeDirectory::SpecialSlot type, CFDataRef data);
	};
};


//
// A DetachedSignatureBlob collects multiple architectures' worth of
// EmbeddedSignatureBlobs into one, well, Super-SuperBlob.
// This is what we use for Mach-O detached signatures.
//
typedef SuperBlob<0xfade0cc1> DetachedSignatureBlob;	// indexed by main architecture


//
// The linkers produces a superblob of dependency records from its dylib inputs
//
typedef SuperBlob<0xfade0c05> LibraryDependencyBlob; // indexed sequentially from 0


//
// An entitlement blob is used for embedding entitlement configuration data
//
class EntitlementBlob : public Blob<EntitlementBlob, 0xfade7171> {
public:
	CFDictionaryRef entitlements() const;
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_SIGBLOB
