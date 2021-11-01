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
#include "cs.h"
#include "SecAssessment.h"
#include "policydb.h"
#include "policyengine.h"
#include "xpcengine.h"
#include "csutilities.h"
#include <CoreFoundation/CFRuntime.h>
#include <security_utilities/globalizer.h>
#include <security_utilities/unix++.h>
#include <security_utilities/cfmunge.h>
#include <notify.h>

using namespace CodeSigning;


//
// CF Objects
//
struct _SecAssessment : private CFRuntimeBase {
public:
	_SecAssessment(CFURLRef p, CFDictionaryRef r) : path(p), result(r) { }
	
	CFCopyRef<CFURLRef> path;
	CFRef<CFDictionaryRef> result;

public:
	static _SecAssessment &ref(SecAssessmentRef r)
		{ return *(_SecAssessment *)r; }

	// CF Boiler-plate
	void *operator new (size_t size)
	{
		return (void *)_CFRuntimeCreateInstance(NULL, SecAssessmentGetTypeID(),
			sizeof(_SecAssessment) - sizeof(CFRuntimeBase), NULL);
	}
	
	static void finalize(CFTypeRef obj)
	{ ((_SecAssessment *)obj)->~_SecAssessment(); }
};

typedef _SecAssessment SecAssessment;


static const CFRuntimeClass assessmentClass = {
	0,								// version
	"SecAssessment",				// name
	NULL,							// init
	NULL,							// copy
	SecAssessment::finalize,		// finalize
	NULL,							// equal
	NULL,							// hash
	NULL,							// formatting
	NULL							// debug string
};


static dispatch_once_t assessmentOnce;
CFTypeID assessmentType = _kCFRuntimeNotATypeID;
	
CFTypeID SecAssessmentGetTypeID()
{
	
	dispatch_once(&assessmentOnce, ^void() {
		if ((assessmentType = _CFRuntimeRegisterClass(&assessmentClass)) == _kCFRuntimeNotATypeID)
			abort();
	});
	return assessmentType;
}


//
// Common dictionary constants
//
const CFStringRef kSecAssessmentContextKeyOperation = CFSTR("operation");
const CFStringRef kSecAssessmentOperationTypeExecute = CFSTR("operation:execute");
const CFStringRef kSecAssessmentOperationTypeInstall = CFSTR("operation:install");
const CFStringRef kSecAssessmentOperationTypeOpenDocument = CFSTR("operation:lsopen");


//
// Read-only in-process access to the policy database
//
class ReadPolicy : public PolicyDatabase {
public:
	ReadPolicy() : PolicyDatabase(defaultDatabase) { }
};
ModuleNexus<ReadPolicy> gDatabase;


//
// An on-demand instance of the policy engine
//
ModuleNexus<PolicyEngine> gEngine;


//
// Policy evaluation ("assessment") operations
//
const CFStringRef kSecAssessmentAssessmentVerdict = CFSTR("assessment:verdict");
const CFStringRef kSecAssessmentAssessmentOriginator = CFSTR("assessment:originator");
const CFStringRef kSecAssessmentAssessmentAuthority = CFSTR("assessment:authority");
const CFStringRef kSecAssessmentAssessmentSource = CFSTR("assessment:authority:source");
const CFStringRef kSecAssessmentAssessmentAuthorityRow = CFSTR("assessment:authority:row");
const CFStringRef kSecAssessmentAssessmentAuthorityOverride = CFSTR("assessment:authority:override");
const CFStringRef kSecAssessmentAssessmentFromCache = CFSTR("assessment:authority:cached");

const CFStringRef kDisabledOverride = CFSTR("security disabled");

const CFStringRef kSecAssessmentContextKeyCertificates = CFSTR("context:certificates");	// obsolete

SecAssessmentRef SecAssessmentCreate(CFURLRef path,
	SecAssessmentFlags flags,
	CFDictionaryRef context,
	CFErrorRef *errors)
{
	BEGIN_CSAPI
	
	if (flags & kSecAssessmentFlagAsynchronous)
		MacOSError::throwMe(errSecCSUnimplemented);
	
	AuthorityType type = typeFor(context, kAuthorityExecute);
	CFRef<CFMutableDictionaryRef> result = makeCFMutableDictionary();

	SYSPOLICY_ASSESS_API(cfString(path).c_str(), int(type), flags);

	try {
		// check the object cache first unless caller denied that or we need extended processing
		if (!(flags & (kSecAssessmentFlagRequestOrigin | kSecAssessmentFlagIgnoreCache))) {
			if (gDatabase().checkCache(path, type, result))
				return new SecAssessment(path, result.yield());
		}
		
		if (flags & kSecAssessmentFlagDirect) {
			// ask the engine right here to do its thing
			SYSPOLICY_ASSESS_LOCAL();
			gEngine().evaluate(path, type, flags, context, result);
		} else {
			// relay the question to our daemon for consideration
			SYSPOLICY_ASSESS_REMOTE();
			xpcEngineAssess(path, flags, context, result);
		}
	} catch (CommonError &error) {
		switch (error.osStatus()) {
		case CSSMERR_TP_CERT_REVOKED:
			throw;
		default:
			if (!overrideAssessment())
				throw;		// let it go as an error
			break;
		}
		// record the error we would have returned
		cfadd(result, "{%O=#F,'assessment:error'=%d}}", kSecAssessmentAssessmentVerdict, error.osStatus());
	} catch (...) {
		// catch stray errors not conforming to the CommonError scheme
		if (!overrideAssessment())
			throw;		// let it go as an error
		cfadd(result, "{%O=#F}", kSecAssessmentAssessmentVerdict);
	}
	return new SecAssessment(path, result.yield());

	END_CSAPI_ERRORS1(NULL)
}


static void traceResult(SecAssessment &assessment, CFDictionaryRef result)
{
	if (CFDictionaryGetValue(result, CFSTR("assessment:remote")))
		return;		// just traced in syspolicyd

	CFRef<CFURLRef> url = CFURLCopyAbsoluteURL(assessment.path);
	string sanitized = cfString(url);
	string::size_type rslash = sanitized.rfind('/');
	if (rslash != string::npos)
		sanitized = sanitized.substr(rslash+1);
	string::size_type dot = sanitized.rfind('.');
	if (dot != string::npos)
		sanitized = sanitized.substr(dot+1);
	else
		sanitized = "(none)";

	string identifier = "UNBUNDLED";
	if (CFRef<CFBundleRef> bundle = CFBundleCreate(NULL, assessment.path))
		if (CFStringRef ident = CFBundleGetIdentifier(bundle))
			identifier = cfString(ident);
	
	string authority = "UNSPECIFIED";
	bool overridden = false;
	if (CFDictionaryRef authdict = CFDictionaryRef(CFDictionaryGetValue(result, kSecAssessmentAssessmentAuthority))) {
		if (CFStringRef auth = CFStringRef(CFDictionaryGetValue(authdict, kSecAssessmentAssessmentSource)))
			authority = cfString(auth);
		else
			authority = "no authority";
		if (CFTypeRef override = CFDictionaryGetValue(authdict, kSecAssessmentAssessmentAuthorityOverride))
			if (CFEqual(override, kDisabledOverride))
				overridden = true;
	}
	
	MessageTrace trace("com.apple.security.assessment.outcome", NULL);
	trace.add("signature2", "bundle:%s", identifier.c_str());
	if (CFDictionaryGetValue(result, kSecAssessmentAssessmentVerdict) == kCFBooleanFalse) {
		trace.add("signature", "denied:%s", authority.c_str());
		trace.add("signature3", sanitized.c_str());
		trace.send("assessment denied for %s", sanitized.c_str());
	} else if (overridden) {
		trace.add("signature", "override:%s", authority.c_str());
		trace.add("signature3", sanitized.c_str());
		trace.send("assessment denied for %s but overridden", sanitized.c_str());
	} else {
		trace.add("signature", "granted:%s", authority.c_str());
		trace.add("signature3", sanitized.c_str());
		trace.send("assessment granted for %s by %s", sanitized.c_str(), authority.c_str());
	}
}


//
// At present, CopyResult simply retrieves the result already formed by Create.
// In the future, this will be more lazy.
//
CFDictionaryRef SecAssessmentCopyResult(SecAssessmentRef assessmentRef,
	SecAssessmentFlags flags,
	CFErrorRef *errors)
{
	BEGIN_CSAPI

	SecAssessment &assessment = SecAssessment::ref(assessmentRef);
	CFCopyRef<CFDictionaryRef> result = assessment.result;
	if (!(flags & kSecAssessmentFlagEnforce) && overrideAssessment()) {
		// turn rejections into approvals, but note that we did that
		if (CFDictionaryGetValue(result, kSecAssessmentAssessmentVerdict) == kCFBooleanFalse) {
			CFRef<CFMutableDictionaryRef> adulterated = makeCFMutableDictionary(result.get());
			CFDictionarySetValue(adulterated, kSecAssessmentAssessmentVerdict, kCFBooleanTrue);
			if (CFDictionaryRef authority = CFDictionaryRef(CFDictionaryGetValue(adulterated, kSecAssessmentAssessmentAuthority))) {
				CFRef<CFMutableDictionaryRef> authority2 = makeCFMutableDictionary(authority);
				CFDictionarySetValue(authority2, kSecAssessmentAssessmentAuthorityOverride, kDisabledOverride);
				CFDictionarySetValue(adulterated, kSecAssessmentAssessmentAuthority, authority2);
			} else {
				cfadd(adulterated, "{%O={%O=%O}}",
					kSecAssessmentAssessmentAuthority, kSecAssessmentAssessmentAuthorityOverride, kDisabledOverride);
			}
			result = adulterated.get();
		}
	}
	traceResult(assessment, result);
	return result.yield();

	END_CSAPI_ERRORS1(NULL)
}


//
// Policy editing operations.
// These all make permanent changes to the system-wide authority records.
//
const CFStringRef kSecAssessmentContextKeyUpdate = CFSTR("update");
const CFStringRef kSecAssessmentUpdateOperationAdd = CFSTR("update:add");
const CFStringRef kSecAssessmentUpdateOperationRemove = CFSTR("update:remove");
const CFStringRef kSecAssessmentUpdateOperationEnable = CFSTR("update:enable");
const CFStringRef kSecAssessmentUpdateOperationDisable = CFSTR("update:disable");
const CFStringRef kSecAssessmentUpdateOperationFind = CFSTR("update:find");

const CFStringRef kSecAssessmentUpdateKeyAuthorization = CFSTR("update:authorization");
const CFStringRef kSecAssessmentUpdateKeyPriority = CFSTR("update:priority");
const CFStringRef kSecAssessmentUpdateKeyLabel = CFSTR("update:label");
const CFStringRef kSecAssessmentUpdateKeyExpires = CFSTR("update:expires");
const CFStringRef kSecAssessmentUpdateKeyAllow = CFSTR("update:allow");
const CFStringRef kSecAssessmentUpdateKeyRemarks = CFSTR("update:remarks");

const CFStringRef kSecAssessmentUpdateKeyRow = CFSTR("update:row");
const CFStringRef kSecAssessmentUpdateKeyCount = CFSTR("update:count");
const CFStringRef kSecAssessmentUpdateKeyFound = CFSTR("update:found");

const CFStringRef kSecAssessmentRuleKeyID = CFSTR("rule:id");
const CFStringRef kSecAssessmentRuleKeyPriority = CFSTR("rule:priority");
const CFStringRef kSecAssessmentRuleKeyAllow = CFSTR("rule:allow");
const CFStringRef kSecAssessmentRuleKeyLabel = CFSTR("rule:label");
const CFStringRef kSecAssessmentRuleKeyRemarks = CFSTR("rule:remarks");
const CFStringRef kSecAssessmentRuleKeyRequirement = CFSTR("rule:requirement");
const CFStringRef kSecAssessmentRuleKeyType = CFSTR("rule:type");
const CFStringRef kSecAssessmentRuleKeyExpires = CFSTR("rule:expires");
const CFStringRef kSecAssessmentRuleKeyDisabled = CFSTR("rule:disabled");
const CFStringRef kSecAssessmentRuleKeyBookmark = CFSTR("rule:bookmark");


Boolean SecAssessmentUpdate(CFTypeRef target,
	SecAssessmentFlags flags,
	CFDictionaryRef context,
	CFErrorRef *errors)
{
	if (CFDictionaryRef outcome = SecAssessmentCopyUpdate(target, flags, context, errors)) {
		CFRelease(outcome);
		return true;
	} else {
		return false;
	}
}

CFDictionaryRef SecAssessmentCopyUpdate(CFTypeRef target,
	SecAssessmentFlags flags,
	CFDictionaryRef context,
	CFErrorRef *errors)
{
	BEGIN_CSAPI

	CFDictionary ctx(context, errSecCSInvalidAttributeValues);

	if (flags & kSecAssessmentFlagDirect) {
		// ask the engine right here to do its thing
		return gEngine().update(target, flags, ctx);
	} else {
		// relay the question to our daemon for consideration
		return xpcEngineUpdate(target, flags, ctx);
	}

	END_CSAPI_ERRORS1(false)
}


//
// The fcntl of System Policies.
// For those very special requests.
//
Boolean SecAssessmentControl(CFStringRef control, void *arguments, CFErrorRef *errors)
{
	BEGIN_CSAPI
	
	if (CFEqual(control, CFSTR("ui-enable"))) {
		setAssessment(true);
		MessageTrace trace("com.apple.security.assessment.state", "enable");
		trace.send("enable assessment outcomes");
		return true;
	} else if (CFEqual(control, CFSTR("ui-disable"))) {
		setAssessment(false);
		MessageTrace trace("com.apple.security.assessment.state", "disable");
		trace.send("disable assessment outcomes");
		return true;
	} else if (CFEqual(control, CFSTR("ui-status"))) {
		CFBooleanRef &result = *(CFBooleanRef*)(arguments);
		if (overrideAssessment())
			result = kCFBooleanFalse;
		else
			result = kCFBooleanTrue;
		return true;
	} else if (CFEqual(control, CFSTR("ui-enable-devid"))) {
		CFTemp<CFDictionaryRef> ctx("{%O=%s}", kSecAssessmentUpdateKeyLabel, "Developer ID");
		if (CFDictionaryRef result = gEngine().enable(NULL, kAuthorityInvalid, kSecCSDefaultFlags, ctx))
			CFRelease(result);
		return true;
	} else if (CFEqual(control, CFSTR("ui-disable-devid"))) {
		CFTemp<CFDictionaryRef> ctx("{%O=%s}", kSecAssessmentUpdateKeyLabel, "Developer ID");
		if (CFDictionaryRef result = gEngine().disable(NULL, kAuthorityInvalid, kSecCSDefaultFlags, ctx))
			CFRelease(result);
		return true;
	} else if (CFEqual(control, CFSTR("ui-get-devid"))) {
		CFBooleanRef &result = *(CFBooleanRef*)(arguments);
		if (gEngine().value<int>("SELECT disabled FROM authority WHERE label = 'Developer ID';", true))
			result = kCFBooleanFalse;
		else
			result = kCFBooleanTrue;
		return true;
	} else
		MacOSError::throwMe(errSecCSInvalidAttributeValues);

	END_CSAPI_ERRORS1(false)
}
