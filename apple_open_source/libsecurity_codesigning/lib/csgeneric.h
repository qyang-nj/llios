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
// csgeneric - generic Code representative
//
#ifndef _H_CSGENERIC
#define _H_CSGENERIC

#include "Code.h"
#include <Security/SecCodeHost.h>
#include <security_utilities/utilities.h>
#include <security_utilities/mach++.h>

namespace Security {
namespace CodeSigning {


//
// A SecCode that represents "generic" code.
// Generic code is, well, generic. It doesn't have any real resources that define it,
// and so it's defined, de facto, by its host. The Code Signing subsystem has no special
// knowledge as to its nature, and so it just asks the host about everything. The asking
// is done via the cshosting Mach RPC protocol, which can be implemented by hosts in whichever
// way they find reasonable. This code doesn't care, as long as someone is answering.
//
// It is all right to subclass GenericCode to inherit access to the cshosting protocol.
//
class GenericCode : public SecCode {
public:
	GenericCode(SecCode *host, SecGuestRef guestRef = kSecNoGuest);
	
	SecCode *locateGuest(CFDictionaryRef attributes);
	SecStaticCode *identifyGuest(SecCode *guest, CFDataRef *cdhash);
	SecCodeStatus getGuestStatus(SecCode *guest);
	void changeGuestStatus(SecCode *guest, SecCodeStatusOperation operation, CFDictionaryRef arguments);
	
	SecGuestRef guestRef() const { return mGuestRef; }

protected:
	MachPlusPlus::Port hostingPort();
	virtual mach_port_t getHostingPort();

private:
	void identifyGuest(SecGuestRef guest, char *path, CFDataRef &cdhash, CFDictionaryRef &attributes);
	
private:
	MachPlusPlus::Port mHostingPort;	// cached hosting port for this Code
	SecGuestRef mGuestRef;				// guest reference
};


//
// We don't need a GenericCode variant of SecStaticCode
//
typedef SecStaticCode GenericStaticCode;


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_CSGENERIC
