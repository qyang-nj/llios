/*
 * Copyright (c) 2011-2012 Apple Inc. All Rights Reserved.
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
#ifndef _H_POLICYENGINE
#define _H_POLICYENGINE

#include "SecAssessment.h"
#include "policydb.h"
#include <security_utilities/globalizer.h>
#include <security_utilities/cfutilities.h>
#include <security_utilities/hashing.h>
#include <security_utilities/sqlite++.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/CodeSigning.h>

namespace Security {
namespace CodeSigning {


typedef uint EngineOperation;
enum {
	opInvalid = 0,
	opEvaluate,
	opAddAuthority,
	opRemoveAuthority,
};


class PolicyEngine : public PolicyDatabase {
public:
	PolicyEngine();
	virtual ~PolicyEngine();

public:
	void evaluate(CFURLRef path, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context, CFMutableDictionaryRef result);

	CFDictionaryRef update(CFTypeRef target, SecAssessmentFlags flags, CFDictionaryRef context);
	CFDictionaryRef add(CFTypeRef target, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context);
	CFDictionaryRef remove(CFTypeRef target, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context);
	CFDictionaryRef enable(CFTypeRef target, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context);
	CFDictionaryRef disable(CFTypeRef target, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context);
	CFDictionaryRef find(CFTypeRef target, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context);

public:
	static void addAuthority(CFMutableDictionaryRef parent, const char *label, SQLite::int64 row = 0, CFTypeRef cacheInfo = NULL);
	static void addToAuthority(CFMutableDictionaryRef parent, CFStringRef key, CFTypeRef value);

private:
	void evaluateCode(CFURLRef path, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context, CFMutableDictionaryRef result);
	void evaluateInstall(CFURLRef path, SecAssessmentFlags flags, CFDictionaryRef context, CFMutableDictionaryRef result);
	void evaluateDocOpen(CFURLRef path, SecAssessmentFlags flags, CFDictionaryRef context, CFMutableDictionaryRef result);
	
	void selectRules(SQLite::Statement &action, std::string stanza, std::string table,
		CFTypeRef inTarget, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context, std::string suffix = "");
	CFDictionaryRef manipulateRules(const std::string &stanza,
		CFTypeRef target, AuthorityType type, SecAssessmentFlags flags, CFDictionaryRef context);

	void setOrigin(CFArrayRef chain, CFMutableDictionaryRef result);

	void recordOutcome(SecStaticCodeRef code, bool allow, AuthorityType type, double expires, SQLite::int64 authority);
};


} // end namespace CodeSigning
} // end namespace Security

#endif //_H_POLICYENGINE
