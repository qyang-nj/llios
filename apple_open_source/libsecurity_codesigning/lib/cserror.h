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
#ifndef _H_CSERRORS
#define _H_CSERRORS

#include <security_utilities/cfutilities.h>
#include <security_utilities/debugging.h>


namespace Security {
namespace CodeSigning {


//
// Special tailored exceptions to transmit additional error information
//
class CSError : public MacOSError {
public:
	CSError(OSStatus rc) : MacOSError(rc) { }
	CSError(OSStatus rc, CFDictionaryRef dict) : MacOSError(rc), mInfoDict(dict) { } // takes dict
	~CSError() throw ();
	
    static void throwMe(OSStatus rc) __attribute__((noreturn));
	static void throwMe(OSStatus rc, CFDictionaryRef info) __attribute__ ((noreturn)); // takes dict
    static void throwMe(OSStatus rc, CFStringRef key, CFTypeRef value) __attribute__((noreturn));

	void augment(CFStringRef key, CFTypeRef value);

	CFDictionaryRef infoDict() const { return mInfoDict; }
	
public:
	OSStatus cfError(CFErrorRef *errors) const;
	static OSStatus cfError(CFErrorRef *errors, OSStatus rc);
	
private:
	CFRef<CFDictionaryRef> mInfoDict;
};


}	// CodeSigning
}	// Security

#endif //_H_CSERRORS
