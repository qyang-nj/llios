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
// kerneldiskrep - the kernel's own disk representation.
//
// This is a very special case.
// It's here primarily so we don't have to add special cases for the kernel
// all over the higher layers.
//
#ifndef _H_KERNELDISKREP
#define _H_KERNELDISKREP

#include "diskrep.h"

namespace Security {
namespace CodeSigning {


//
// A KernelDiskRep represents a (the) kernel on disk.
// It has no write support, so we can't sign the kernel,
// which is fine since we unconditionally trust it anyway.
//
class KernelDiskRep : public DiskRep {
public:
	KernelDiskRep();
	
	CFDataRef component(CodeDirectory::SpecialSlot slot);
	CFDataRef identification();
	std::string mainExecutablePath();
	CFURLRef canonicalPath();
	size_t signingLimit();
	std::string format();
	UnixPlusPlus::FileDesc &fd();
	
	std::string recommendedIdentifier(const SigningContext &ctx);
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_KERNELDISKREP
