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
#include "xpcengine.h"
#include <xpc/connection.h>
#include <syslog.h>
#include <CoreFoundation/CoreFoundation.h>
#include <security_utilities/cfutilities.h>
#include <Security/SecRequirement.h>


namespace Security {
namespace CodeSigning {

static const char serviceName[] = "com.apple.security.syspolicy";


static dispatch_once_t dispatchInit;		// one-time init marker
static xpc_connection_t service;			// connection to spd
static dispatch_queue_t queue;				// dispatch queue for service

static void init()
{
	dispatch_once(&dispatchInit, ^void(void) {
		const char *name = serviceName;
		if (const char *env = getenv("SYSPOLICYNAME"))
			name = env;
		queue = dispatch_queue_create("spd-client", 0);
		service = xpc_connection_create_mach_service(name, queue, XPC_CONNECTION_MACH_SERVICE_PRIVILEGED);
		xpc_connection_set_event_handler(service, ^(xpc_object_t ev) {
		});
		xpc_connection_resume(service);
	});
}


//
// Your standard XPC client-side machinery
//
class Message {
public:
	xpc_object_t obj;
	
	Message(const char *function)
	{
		init();
		obj = xpc_dictionary_create(NULL, NULL, 0);
		xpc_dictionary_set_string(obj, "function", function);
	}
	~Message()
	{
		if (obj)
			xpc_release(obj);
	}
	operator xpc_object_t () { return obj; }

	void send()
	{
		xpc_object_t reply = xpc_connection_send_message_with_reply_sync(service, obj);
		xpc_release(obj);
		obj = NULL;
		xpc_type_t type = xpc_get_type(reply);
		if (type == XPC_TYPE_DICTIONARY) {
			obj = reply;
			if (int64_t error = xpc_dictionary_get_int64(obj, "error"))
				MacOSError::throwMe(error);
		} else if (type == XPC_TYPE_ERROR) {
			const char *s = xpc_copy_description(reply);
			printf("Error returned: %s\n", s);
			free((char*)s);
			MacOSError::throwMe(errSecCSInternalError);
		} else {
			const char *s = xpc_copy_description(reply);
			printf("Unexpected type of return object: %s\n", s);
			free((char*)s);
		}
	}
};



static void copyCFDictionary(const void *key, const void *value, void *ctx)
{
	CFMutableDictionaryRef target = CFMutableDictionaryRef(ctx);
	if (CFEqual(key, kSecAssessmentContextKeyCertificates))	// obsolete
		return;
	if (CFGetTypeID(value) == CFURLGetTypeID()) {
		CFRef<CFStringRef> path = CFURLCopyFileSystemPath(CFURLRef(value), kCFURLPOSIXPathStyle);
		CFDictionaryAddValue(target, key, path);
	} else {
		CFDictionaryAddValue(target, key, value);
	}
}

void xpcEngineAssess(CFURLRef path, uint flags, CFDictionaryRef context, CFMutableDictionaryRef result)
{
	Message msg("assess");
	xpc_dictionary_set_string(msg, "path", cfString(path).c_str());
	xpc_dictionary_set_int64(msg, "flags", flags);
	CFRef<CFMutableDictionaryRef> ctx = makeCFMutableDictionary();
	if (context)
		CFDictionaryApplyFunction(context, copyCFDictionary, ctx);
	CFRef<CFDataRef> contextData = makeCFData(CFDictionaryRef(ctx));
	xpc_dictionary_set_data(msg, "context", CFDataGetBytePtr(contextData), CFDataGetLength(contextData));
	
	msg.send();
	
	if (int64_t error = xpc_dictionary_get_int64(msg, "error"))
		MacOSError::throwMe(error);

	size_t resultLength;
	const void *resultData = xpc_dictionary_get_data(msg, "result", &resultLength);
	CFRef<CFDictionaryRef> resultDict = makeCFDictionaryFrom(resultData, resultLength);
	CFDictionaryApplyFunction(resultDict, copyCFDictionary, result);
	CFDictionaryAddValue(result, CFSTR("assessment:remote"), kCFBooleanTrue);
}


CFDictionaryRef xpcEngineUpdate(CFTypeRef target, uint flags, CFDictionaryRef context)
{
	Message msg("update");
	// target can be NULL, a CFURLRef, a SecRequirementRef, or a CFNumberRef
	if (target) {
		if (CFGetTypeID(target) == CFNumberGetTypeID())
			xpc_dictionary_set_uint64(msg, "rule", cfNumber<int64_t>(CFNumberRef(target)));
		else if (CFGetTypeID(target) == CFURLGetTypeID())
			xpc_dictionary_set_string(msg, "url", cfString(CFURLRef(target)).c_str());
		else if (CFGetTypeID(target) == SecRequirementGetTypeID()) {
			CFRef<CFDataRef> data;
			MacOSError::check(SecRequirementCopyData(SecRequirementRef(target), kSecCSDefaultFlags, &data.aref()));
			xpc_dictionary_set_data(msg, "requirement", CFDataGetBytePtr(data), CFDataGetLength(data));
		} else
			MacOSError::throwMe(errSecCSInvalidObjectRef);
	}
	xpc_dictionary_set_int64(msg, "flags", flags);
	CFRef<CFMutableDictionaryRef> ctx = makeCFMutableDictionary();
	if (context)
		CFDictionaryApplyFunction(context, copyCFDictionary, ctx);
	AuthorizationRef localAuthorization = NULL;
	if (CFDictionaryGetValue(ctx, kSecAssessmentUpdateKeyAuthorization) == NULL) {	// no caller-provided authorization
		MacOSError::check(AuthorizationCreate(NULL, NULL, kAuthorizationFlagDefaults, &localAuthorization));
		AuthorizationExternalForm extForm;
		MacOSError::check(AuthorizationMakeExternalForm(localAuthorization, &extForm));
		CFDictionaryAddValue(ctx, kSecAssessmentUpdateKeyAuthorization, CFTempData(&extForm, sizeof(extForm)));
	}
	CFRef<CFDataRef> contextData = makeCFData(CFDictionaryRef(ctx));
	xpc_dictionary_set_data(msg, "context", CFDataGetBytePtr(contextData), CFDataGetLength(contextData));
	
	msg.send();

	if (localAuthorization)
		AuthorizationFree(localAuthorization, kAuthorizationFlagDefaults);
	
	if (int64_t error = xpc_dictionary_get_int64(msg, "error"))
		MacOSError::throwMe(error);
	
	size_t resultLength;
	const void *resultData = xpc_dictionary_get_data(msg, "result", &resultLength);
	return makeCFDictionaryFrom(resultData, resultLength);
}


bool xpcEngineControl(const char *control)
{
	Message msg("control");
	xpc_dictionary_set_string(msg, "control", control);
	msg.send();
	return true;
}


} // end namespace CodeSigning
} // end namespace Security
