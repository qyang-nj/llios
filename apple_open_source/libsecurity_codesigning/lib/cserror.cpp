/*
 * Copyright (c) 2007 Apple Inc. All Rights Reserved.
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
// cserror.h - extended-diagnostics Code Signing errors
//
#include "cs.h"
#include <security_utilities/cfmunge.h>

namespace Security {
namespace CodeSigning {


//
// We need a nothrow destructor
//
CSError::~CSError() throw ()
{ }


//
// Create and throw various forms of CSError
//
void CSError::throwMe(OSStatus rc)
{
	throw CSError(rc);
}

void CSError::throwMe(OSStatus rc, CFDictionaryRef dict)
{
	throw CSError(rc, dict);
}

void CSError::throwMe(OSStatus rc, CFStringRef key, CFTypeRef value)
{
	throw CSError(rc, cfmake<CFDictionaryRef>("{%O=%O}", key, value));
}


//
// Add a key/value pair to the dictionary
//
void CSError::augment(CFStringRef key, CFTypeRef value)
{
	mInfoDict.take(cfmake<CFDictionaryRef>("{+%O,%O=%O}", mInfoDict.get(), key, value));
}


//
// Convert exception-carried error information to CFError form
//
OSStatus CSError::cfError(CFErrorRef *errors) const
{
	if (errors)		// errors argument was specified
		*errors = CFErrorCreate(NULL, kCFErrorDomainOSStatus, this->osStatus(), this->infoDict());
	return this->osStatus();
}

OSStatus CSError::cfError(CFErrorRef *errors, OSStatus rc)
{
	if (errors)
		*errors = CFErrorCreate(NULL, kCFErrorDomainOSStatus, rc, NULL);
	return rc;
}


}	// CodeSigning
}	// Security
