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
#include "SecCodeHost.h"
#include "SecCodeHostLib.h"
#include <Security/Security.h>
#include <Security/AuthSession.h>
#include <securityd_client/ucsp.h>
#include <servers/bootstrap.h>


//
// Global state
//
mach_port_t gServerPort;
SecCSFlags gInitFlags;


//
// Framing macros and facilities
//
#define UCSP_ARGS	gServerPort, mig_get_reply_port(), &securitydCreds, &rcode
#define ATTRDATA(attr) (void *)(attr), (attr) ? strlen((attr)) : 0

#define CALL(func) \
	security_token_t securitydCreds; \
	CSSM_RETURN rcode; \
	if (KERN_SUCCESS != func) \
		return errSecCSInternalError; \
	if (securitydCreds.val[0] != 0) \
		return CSSM_ERRCODE_VERIFICATION_FAILURE; \
	return rcode



//
// Mandatory initialization call
//
OSStatus SecHostLibInit(SecCSFlags flags)
{
	if (gServerPort != MACH_PORT_NULL)	// re-initialization attempt
		return errSecCSInternalError;

	mach_port_t bootstrapPort;
	if (KERN_SUCCESS != task_get_bootstrap_port(mach_task_self(), &bootstrapPort))
		return errSecCSInternalError;
	static char serverName[BOOTSTRAP_MAX_NAME_LEN] = SECURITYSERVER_BOOTSTRAP_NAME;
	if (KERN_SUCCESS != bootstrap_look_up(bootstrapPort, serverName, &gServerPort))
		return errSecCSInternalError;

	ClientSetupInfo info = { 0x1234, SSPROTOVERSION };
	CALL(ucsp_client_setup(UCSP_ARGS, mach_task_self(), info, "?:unspecified"));
}


//
// Guest creation.
// At this time, this ONLY supports the creation of (one) dedicated guest.
//
OSStatus SecHostLibCreateGuest(SecGuestRef host,
	uint32_t status, const char *path, const char *attributeXML,
	SecCSFlags flags, SecGuestRef *newGuest)
{
	return SecHostLibCreateGuest2(host, status, path, "", 0, attributeXML, flags, newGuest);
}

OSStatus SecHostLibCreateGuest2(SecGuestRef host,
	uint32_t status, const char *path, const void *cdhash, size_t cdhashLength, const char *attributeXML,
	SecCSFlags flags, SecGuestRef *newGuest)
{
	if (flags != kSecCSDedicatedHost)
		return errSecCSInvalidFlags;
	
	CALL(ucsp_client_createGuest(UCSP_ARGS, host, status, path,
		(void *)cdhash, cdhashLength, ATTRDATA(attributeXML), flags, newGuest));
}


//
// Update the status of a guest.
//
OSStatus SecHostLibSetGuestStatus(SecGuestRef guestRef,
	uint32_t status, const char *attributeXML,
	SecCSFlags flags)
{
	CALL(ucsp_client_setGuestStatus(UCSP_ARGS, guestRef, status, ATTRDATA(attributeXML)));
}


//
// Enable dynamic hosting mode.
//
OSStatus SecHostSetHostingPort(mach_port_t hostingPort, SecCSFlags flags)
{
	CALL(ucsp_client_registerHosting(UCSP_ARGS, hostingPort, flags));
}


//
// Helper for checked incorporation of code.
//
OSStatus SecHostLibCheckLoad(const char *path, SecRequirementType type)
{
	CALL(ucsp_client_helpCheckLoad(UCSP_ARGS, path, type));
}
