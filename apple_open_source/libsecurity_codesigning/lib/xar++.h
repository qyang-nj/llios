/*
 * Copyright (c) 2011 Apple Inc. All Rights Reserved.
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
// xar++ - interface to XAR-format archive files
//
#ifndef _H_XARPLUSPLUS
#define _H_XARPLUSPLUS

#include <security_utilities/utilities.h>
#include <CoreFoundation/CoreFoundation.h>

extern "C" {
#include <xar/xar.h>
}

namespace Security {
namespace CodeSigning {


//
// A XAR-format file on disk
//
class Xar {
public:	
	Xar(const char *path = NULL);
	virtual ~Xar();
	void open(const char *path);
	
	operator bool() const { return mXar != 0; }
	bool isSigned() const { return mSigClassic != 0 || mSigCMS != 0; }
	
	CFArrayRef copyCertChain();

private:
	xar_t mXar;
	xar_signature_t mSigClassic;
	xar_signature_t mSigCMS;
};



} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_XARPLUSPLUS
