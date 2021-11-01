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
// cskernel - Kernel implementation of the Code Signing Host Interface
//
#ifndef _H_CSKERNEL
#define _H_CSKERNEL

#include "Code.h"
#include "StaticCode.h"
#include <security_utilities/utilities.h>

namespace Security {
namespace CodeSigning {


class ProcessCode;


//
// The nominal StaticCode representing the kernel on disk.
// This is barely used, since we don't validate the kernel (it's the root of trust)
// and we don't activate new kernels at runtime.
//
class KernelStaticCode : public SecStaticCode {
public:
	KernelStaticCode();

private:
};


//
// A SecCode that represents the system's running kernel.
// We usually only have one of those in the system at one time. :-)
//
class KernelCode : public SecCode {
public:
	KernelCode();

	SecCode *locateGuest(CFDictionaryRef attributes);
	SecStaticCode *identifyGuest(SecCode *guest, CFDataRef *cdhash);
	SecCodeStatus getGuestStatus(SecCode *guest);
	void changeGuestStatus(SecCode *guest, SecCodeStatusOperation operation, CFDictionaryRef arguments);
	
	static KernelCode *active()		{ return globals().code; }
	
public:
	struct Globals {
		Globals();
		SecPointer<KernelCode> code;
		SecPointer<KernelStaticCode> staticCode;
	};
	static ModuleNexus<Globals> globals;

protected:
	void identify();
	void csops(ProcessCode *proc, unsigned int op, void *addr = NULL, size_t length = 0);
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_CSKERNEL
