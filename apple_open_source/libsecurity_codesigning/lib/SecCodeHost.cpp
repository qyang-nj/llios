/*
 * Copyright (c) 2006-2007 Apple Inc. All Rights Reserved.
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
// SecCodeHost - Host Code API
//
#include "cs.h"
#include "SecCodeHost.h"
#include <security_utilities/cfutilities.h>
#include <security_utilities/globalizer.h>
#include <securityd_client/ssclient.h>

using namespace CodeSigning;


//
// Munge a CFDictionary into a CssmData representing its plist
//
class DictData : public CFRef<CFDataRef> {
public:
	DictData(CFDictionaryRef dict) : CFRef<CFDataRef>(makeCFData(dict)) { }

	operator CssmData() const
	{
		if (*this)
			return CssmData::wrap(CFDataGetBytePtr(*this), CFDataGetLength(*this));
		else
			return CssmData();
	}
};


OSStatus SecHostCreateGuest(SecGuestRef host,
	uint32_t status, CFURLRef path, CFDictionaryRef attributes,
	SecCSFlags flags, SecGuestRef *newGuest)
{
	BEGIN_CSAPI
	
	checkFlags(flags, kSecCSDedicatedHost | kSecCSGenerateGuestHash);
	CodeSigning::Required(newGuest) = SecurityServer::ClientSession().createGuest(host,
		status, cfString(path).c_str(), CssmData(), DictData(attributes), flags);
	
	END_CSAPI
}

OSStatus SecHostRemoveGuest(SecGuestRef host, SecGuestRef guest, SecCSFlags flags)
{
	BEGIN_CSAPI

	checkFlags(flags);
	SecurityServer::ClientSession().removeGuest(host, guest);
	
	END_CSAPI
}

OSStatus SecHostSelectGuest(SecGuestRef guestRef, SecCSFlags flags)
{
	BEGIN_CSAPI

	checkFlags(flags);
	SecurityServer::ClientSession().selectGuest(guestRef);
	
	END_CSAPI
}


OSStatus SecHostSelectedGuest(SecCSFlags flags, SecGuestRef *guestRef)
{
	BEGIN_CSAPI
	
	checkFlags(flags);
	CodeSigning::Required(guestRef) = SecurityServer::ClientSession().selectedGuest();
	
	END_CSAPI
}

OSStatus SecHostSetGuestStatus(SecGuestRef guestRef,
	uint32_t status, CFDictionaryRef attributes,
	SecCSFlags flags)
{
	BEGIN_CSAPI

	checkFlags(flags);
	SecurityServer::ClientSession().setGuestStatus(guestRef, status, DictData(attributes));
		
	END_CSAPI
}

OSStatus SecHostSetHostingPort(mach_port_t hostingPort, SecCSFlags flags)
{
	BEGIN_CSAPI

	checkFlags(flags);
	SecurityServer::ClientSession().registerHosting(hostingPort, flags);
		
	END_CSAPI
}
