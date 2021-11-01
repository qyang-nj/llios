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

/*!
	@header SecCodeHostLib
	This header provides a subset of the hosting API for Code Signing.
	This subset functionality is implemented as a static library written
	entirely in C, and depends on nothing except the system library and the
	C runtime. It is thus suitable to be used by low-level libraries and
	other such system facilities. On the other hand, it does not provide the
	full functionality of <Security/SecCodeHost.h>.
	
	This file is documented as a delta to <Security/SecCodeHost.h>, which
	you should consult as a baseline.
*/
#ifndef _H_SECCODEHOSTLIB
#define _H_SECCODEHOSTLIB

#include <Security/SecCodeHost.h>

#ifdef __cplusplus
extern "C" {
#endif


/*!
	@function SecHostLibInit
	This function must be called first to use the SecCodeHostLib facility.
 */
OSStatus SecHostLibInit(SecCSFlags flags);


/*!
	@function SecHostLibCreateGuest
	This function declares a code host, engages hosting proxy services for it,
	and creates a guest with given attributes and state.
	
	NOTE: This version of the function currently only supports dedicated hosting.
	If you do not pass the kSecCSDedicatedHost flag, the call will fail.
 */
OSStatus SecHostLibCreateGuest(SecGuestRef host,
	uint32_t status, const char *path, const char *attributeXML,
	SecCSFlags flags, SecGuestRef *newGuest) DEPRECATED_ATTRIBUTE;
	
OSStatus SecHostLibCreateGuest2(SecGuestRef host,
	uint32_t status, const char *path, const void *cdhash, size_t cdhashLength, const char *attributeXML,
	SecCSFlags flags, SecGuestRef *newGuest);


/*!
	@function SecHostLibSetGuestStatus
	This function can change the state or attributes (or both) of a given guest.
	It performs all the work of SecHostSetGuestStatus.
 */	
OSStatus SecHostLibSetGuestStatus(SecGuestRef guestRef,
	uint32_t status, const char *attributeXML,
	SecCSFlags flags);


/*!
	@function SecHostLibSetHostingPort
	Register a Mach port to receive hosting queries on. This enables (and locks)
	dynamic hosting mode, and is incompatible with all proxy-mode calls.
	You still must call SecHostLibInit first.
 */
OSStatus SecHostSetHostingPort(mach_port_t hostingPort, SecCSFlags flags);


/*
	Functionality from SecCodeHost.h that is genuinely missing here:

OSStatus SecHostRemoveGuest(SecGuestRef host, SecGuestRef guest, SecCSFlags flags);

OSStatus SecHostSelectGuest(SecGuestRef guestRef, SecCSFlags flags);

OSStatus SecHostSelectedGuest(SecCSFlags flags, SecGuestRef *guestRef);

*/


/*!
 */
OSStatus SecHostLibCheckLoad(const char *path, SecRequirementType type);


#ifdef __cplusplus
}
#endif

#endif //_H_SECCODEHOSTLIB
