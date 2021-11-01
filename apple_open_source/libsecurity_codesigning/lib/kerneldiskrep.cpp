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
#include "kerneldiskrep.h"
#include <sys/utsname.h>

namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


//
// Everything about the kernel is pretty much fixed, so there's
// no state to maintain.
//
KernelDiskRep::KernelDiskRep()
{
	CODESIGN_DISKREP_CREATE_KERNEL(this);
}


//
// We can't pull any resources from the kernel.
// And we know where it all is.
//
CFDataRef KernelDiskRep::component(CodeDirectory::SpecialSlot slot)
{
	return NULL;
}

CFDataRef KernelDiskRep::identification()
{
	return NULL;
}


CFURLRef KernelDiskRep::canonicalPath()
{
	return makeCFURL("/mach_kernel");
}

string KernelDiskRep::recommendedIdentifier(const SigningContext &)
{
	utsname names;
	UnixError::check(::uname(&names));
	return string("kernel.") + names.sysname;
}

size_t KernelDiskRep::signingLimit()
{
	return 0;				// don't bother
}

string KernelDiskRep::format()
{
	return "system kernel";
}

UnixPlusPlus::FileDesc &KernelDiskRep::fd()
{
	UnixError::throwMe(EINVAL);		// don't have one
}

string KernelDiskRep::mainExecutablePath()
{
	return "/mach_kernel";
}


} // end namespace CodeSigning
} // end namespace Security
