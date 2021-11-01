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
// Code - SecCode API objects
//
#ifndef _H_CODE
#define _H_CODE

#include "cs.h"
#include "Requirements.h"
#include <security_utilities/utilities.h>

namespace Security {
namespace CodeSigning {


class SecStaticCode;


//
// A SecCode object represents running code in the system. It must be subclassed
// to implement a particular notion of code.
//
class SecCode : public SecCFObject {
	NOCOPY(SecCode)
	friend class KernelCode;	// overrides identify() to set mStaticCode/mCDHash
public:
	SECCFFUNCTIONS(SecCode, SecCodeRef, errSecCSInvalidObjectRef, gCFObjects().Code)

	SecCode(SecCode *host);
    virtual ~SecCode() throw();
	
    bool equal(SecCFObject &other);
    CFHashCode hash();
	
	SecCode *host() const;
	bool isRoot() const { return host() == NULL; }
	SecStaticCode *staticCode();	// cached. Result lives as long as this SecCode
	CFDataRef cdHash();
	
	SecCodeStatus status();				// dynamic status
	void status(SecCodeStatusOperation operation, CFDictionaryRef arguments);

	// primary virtual drivers. Caller owns the result
	virtual void identify();
	virtual SecCode *locateGuest(CFDictionaryRef attributes);
	virtual SecStaticCode *identifyGuest(SecCode *guest, CFDataRef *cdhash);
	
	void checkValidity(SecCSFlags flags);
	virtual SecCodeStatus getGuestStatus(SecCode *guest);
	virtual void changeGuestStatus(SecCode *guest, SecCodeStatusOperation operation, CFDictionaryRef arguments);
	
public:
	// perform "autolocation" (root-based heuristic). Caller owns the result
	static SecCode *autoLocateGuest(CFDictionaryRef attributes, SecCSFlags flags);

private:
	SecPointer<SecCode> mHost;
	bool mIdentified;							// called identify(), mStaticCode & mCDHash are valid
	SecPointer<SecStaticCode> mStaticCode;		// (static) code origin
	CFRef<CFDataRef> mCDHash;					// (dynamic) CodeDirectory hash as per host
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_CODE
