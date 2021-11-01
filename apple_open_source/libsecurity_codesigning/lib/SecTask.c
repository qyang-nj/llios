/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
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

#include <CoreFoundation/CFRuntime.h>
#include <Carbon/Carbon.h>	/* Yuck! For MacErrors.h */

#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFUnserialize.h>

#include <pthread.h>

#include <bsm/libbsm.h>

/* XXX: convert <Security/xyz.h> -> "xyz.h" for these when in project */
#include "SecCode.h"
#include "SecRequirement.h"

/* #include <debugging.h> */

#include "SecTask.h"

struct __SecTask {
	CFRuntimeBase base;

	audit_token_t token;

	/* Track whether we've loaded entitlements independently since after the
	 * load, entitlements may legitimately be NULL */
	Boolean entitlementsLoaded;
	CFDictionaryRef entitlements;
};

enum {
	kSecCodeMagicEntitlement = 0xfade7171,		/* entitlement blob */
};


CFTypeID _kSecTaskTypeID = _kCFRuntimeNotATypeID;

static void SecTaskFinalize(CFTypeRef cfTask)
{
	SecTaskRef task = (SecTaskRef) cfTask;

	if (task->entitlements != NULL) {
		CFRelease(task->entitlements);
		task->entitlements = NULL;
	}
}

static CFStringRef SecTaskCopyDebugDescription(CFTypeRef cfTask)
{
	SecTaskRef task = (SecTaskRef) cfTask;

	return CFStringCreateWithFormat(CFGetAllocator(task), NULL, CFSTR("<SecTask %p>"), task);
}

static void SecTaskRegisterClass(void)
{
	static const CFRuntimeClass SecTaskClass = {
		.version = 0,
		.className = "SecTask",
		.init = NULL,
		.copy = NULL,
		.finalize = SecTaskFinalize,
		.equal = NULL,
		.hash = NULL,
		.copyFormattingDesc = NULL,
		.copyDebugDesc = SecTaskCopyDebugDescription,
	};

	_kSecTaskTypeID = _CFRuntimeRegisterClass(&SecTaskClass);
}

CFTypeID SecTaskGetTypeID(void)
{
	static pthread_once_t secTaskRegisterClassOnce = PTHREAD_ONCE_INIT;

	/* Register the class with the CF runtime the first time through */
	pthread_once(&secTaskRegisterClassOnce, SecTaskRegisterClass);

	return _kSecTaskTypeID;
}

SecTaskRef SecTaskCreateWithAuditToken(CFAllocatorRef allocator, audit_token_t token)
{
	CFIndex extra = sizeof(struct __SecTask) - sizeof(CFRuntimeBase);
	SecTaskRef task = (SecTaskRef) _CFRuntimeCreateInstance(allocator, SecTaskGetTypeID(), extra, NULL);
	if (task != NULL) {

		memcpy(&task->token, &token, sizeof(token));
		task->entitlementsLoaded = false;
		task->entitlements = NULL;
	}

	return task;
}


static CFDictionaryRef parseEntitlementsFromData(CFDataRef blobData)
{
	const struct theBlob {
		uint32_t magic;  /* kSecCodeMagicEntitlement */
		uint32_t length;
		const uint8_t data[];
	} *blob = NULL;

	CFDictionaryRef entitlements = NULL;

	size_t blobDataLen = CFDataGetLength (blobData);

	/* Make sure we're at least the size of a blob */
	if (blobDataLen <= sizeof(struct theBlob)) goto fin;

	blob = (const struct theBlob *) CFDataGetBytePtr (blobData);

	/* Check the magic */
	if (kSecCodeMagicEntitlement != ntohl(blob->magic)) goto fin;

	/* Make sure we have as much data as the blob says we should */
	if (blobDataLen != ntohl(blob->length)) goto fin;

	/* Convert the blobs payload to a dictionary */
	CFDataRef entitlementData = CFDataCreateWithBytesNoCopy (kCFAllocatorDefault,
			blob->data,
			blobDataLen - sizeof(struct theBlob),
			kCFAllocatorNull);

	if (NULL == entitlementData) goto fin;

	entitlements = CFPropertyListCreateFromXMLData (kCFAllocatorDefault,
			entitlementData,
			kCFPropertyListImmutable,
			NULL);
	if (NULL == entitlements) goto fin;

	if (CFGetTypeID(entitlements) != CFDictionaryGetTypeID()) {
		CFRelease (entitlements);
		entitlements = NULL;
	}
fin:
	if (NULL != entitlementData) { CFRelease (entitlementData); }

	return entitlements;
}


static void SecTaskLoadEntitlements(SecTaskRef task, CFErrorRef *error)
{
	pid_t	pid;
	CFNumberRef targetPID = NULL;
	CFDictionaryRef guestAttributes = NULL;
	CFDictionaryRef targetInfo = NULL;
	CFDataRef entitlementData = NULL;
	SecCodeRef target = NULL;
	CFErrorRef cfErr = NULL;
	OSStatus ret = noErr;

	/* XXX: Check for NULL == task->token? */

	audit_token_to_au32 (task->token,
			/* auidp */ NULL,
			/* euidp */ NULL,
			/* egidp */ NULL,
			/* ruidp */ NULL,
			/* rgidp */ NULL,
			/* pidp  */ &pid,
			/* asidp */ NULL,
			/* tidp  */ NULL);

	/*
	 * Ref: /usr/include/sys/_types.h
	 * typedef __int32_t       __darwin_pid_t;
	 */

	targetPID = CFNumberCreate (kCFAllocatorDefault, kCFNumberSInt32Type, &pid);
	if (NULL == targetPID) {
		ret = kPOSIXErrorENOMEM;
		goto err;
	}

	guestAttributes = CFDictionaryCreate (kCFAllocatorDefault,
			(const void **)&kSecGuestAttributePid,
			(const void **)&targetPID,
			1,
			NULL,
			NULL);
	if (NULL == guestAttributes) goto err;

	ret = SecCodeCopyGuestWithAttributes (NULL,
			guestAttributes,
			kSecCSDefaultFlags,
			&target);
	if (noErr != ret) goto err;

	ret = SecCodeCopySigningInformation (target,
			kSecCSRequirementInformation,
			&targetInfo);
	if (noErr != ret || NULL == targetInfo) goto err;

	bool gotKey = CFDictionaryGetValueIfPresent (targetInfo,
			(const void *)kSecCodeInfoEntitlements,
			(const void **)&entitlementData);
	if (false == gotKey || NULL == entitlementData) {
		ret = kIOReturnInvalid;
		goto err;
	}

	task->entitlements = parseEntitlementsFromData (entitlementData);
	if (NULL == task->entitlements) goto err;

	/* secdebug("entitlements", "entitlements %@", task->entitlements); */

	task->entitlementsLoaded = true;
err:
	if (noErr != ret && NULL != error) {
		if (NULL != cfErr) {
			*error = cfErr;
		} else {
			*error = CFErrorCreate(CFGetAllocator(task), kCFErrorDomainMach, ret, NULL);
		}
	}

	/* Free up any allocated things now! */
	if (NULL != targetPID) CFRelease (targetPID);

	if (NULL != guestAttributes) CFRelease (guestAttributes);

	if (NULL != target) CFRelease (target);

	if (NULL != targetInfo) CFRelease (targetInfo);
}

CFTypeRef SecTaskCopyValueForEntitlement(SecTaskRef task, CFStringRef entitlement, CFErrorRef *error)
{
	/* Load entitlements if necessary */
	if (task->entitlementsLoaded == false) {
		SecTaskLoadEntitlements(task, error);
	}

	CFTypeRef value = NULL;
	if (task->entitlements != NULL) {
		value = CFDictionaryGetValue(task->entitlements, entitlement);

		/* Return something the caller must release */
		if (value != NULL) {
			CFRetain(value);
		}
	}

	return value;
}

CFDictionaryRef SecTaskCopyValuesForEntitlements(SecTaskRef task, CFArrayRef entitlements, CFErrorRef *error)
{
	/* Load entitlements if necessary */
	if (task->entitlementsLoaded == false) {
		SecTaskLoadEntitlements(task, error);
	}

	/* Iterate over the passed in entitlements, populating the dictionary
	 * If entitlements were loaded but none were present, return an empty
	 * dictionary */
	CFMutableDictionaryRef values = NULL;
	if (task->entitlementsLoaded == true) {

		CFIndex i, count = CFArrayGetCount(entitlements);
		values = CFDictionaryCreateMutable(CFGetAllocator(task), count, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		if (task->entitlements != NULL) {
			for (i = 0; i < count; i++) {
				CFStringRef entitlement = CFArrayGetValueAtIndex(entitlements, i);
				CFTypeRef value = CFDictionaryGetValue(task->entitlements, entitlement);
				if (value != NULL) {
					CFDictionarySetValue(values, entitlement, value);
				}
			}
		}
	}

	return values;
}
