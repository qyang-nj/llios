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
// SecRequirement - API frame for SecRequirement objects
//
#include "cs.h"
#include "Requirements.h"
#include "reqparser.h"
#include "reqmaker.h"
#include "reqdumper.h"
#include <Security/SecCertificate.h>
#include <security_utilities/cfutilities.h>

using namespace CodeSigning;


//
// CF-standard type code function
//
CFTypeID SecRequirementGetTypeID(void)
{
	BEGIN_CSAPI
	return gCFObjects().Requirement.typeID;
    END_CSAPI1(_kCFRuntimeNotATypeID)
}


//
// Create a Requirement from data
//
OSStatus SecRequirementCreateWithData(CFDataRef data, SecCSFlags flags,
	SecRequirementRef *requirementRef)
{
	BEGIN_CSAPI
	
	checkFlags(flags);
	CodeSigning::Required(requirementRef) = (new SecRequirement(CFDataGetBytePtr(data), CFDataGetLength(data)))->handle();

	END_CSAPI
}
	

//
// Create a Requirement from data in a file
//
OSStatus SecRequirementCreateWithResource(CFURLRef resource, SecCSFlags flags,
	SecRequirementRef *requirementRef)
{
	BEGIN_CSAPI
	
	checkFlags(flags);
	CFRef<CFDataRef> data = cfLoadFile(resource);
	CodeSigning::Required(requirementRef) =
		(new SecRequirement(CFDataGetBytePtr(data), CFDataGetLength(data)))->handle();

	END_CSAPI
}


//
// Create a Requirement from source text (compiling it)
//
OSStatus SecRequirementCreateWithString(CFStringRef text, SecCSFlags flags,
	SecRequirementRef *requirementRef)
{
	return SecRequirementCreateWithStringAndErrors(text, flags, NULL, requirementRef);
}

OSStatus SecRequirementCreateWithStringAndErrors(CFStringRef text, SecCSFlags flags,
	CFErrorRef *errors, SecRequirementRef *requirementRef)
{
	BEGIN_CSAPI
	
	checkFlags(flags);
	CodeSigning::Required(requirementRef) = (new SecRequirement(parseRequirement(cfString(text)), true))->handle();

	END_CSAPI_ERRORS
}


//
// Create a Requirement group.
// This is the canonical point where "application group" is defined.
//
OSStatus SecRequirementCreateGroup(CFStringRef groupName, SecCertificateRef anchorRef,
	SecCSFlags flags, SecRequirementRef *requirementRef)
{
	BEGIN_CSAPI
	
	checkFlags(flags);
	Requirement::Maker maker;
	maker.put(opAnd);		// both of...
	maker.infoKey("Application-Group", cfString(groupName));
	if (anchorRef) {
		CSSM_DATA certData;
		MacOSError::check(SecCertificateGetData(anchorRef, &certData));
		maker.anchor(0, certData.Data, certData.Length);
	} else {
		maker.anchor();			// canonical Apple anchor
	}
	CodeSigning::Required(requirementRef) = (new SecRequirement(maker.make(), true))->handle();

	END_CSAPI
}


//
// Extract the stable binary from from a SecRequirementRef
//
OSStatus SecRequirementCopyData(SecRequirementRef requirementRef, SecCSFlags flags,
	CFDataRef *data)
{
	BEGIN_CSAPI
	
	const Requirement *req = SecRequirement::required(requirementRef)->requirement();
	checkFlags(flags);
	CodeSigning::Required(data);
	*data = makeCFData(*req);

	END_CSAPI
}


//
// Generate source form for a SecRequirement (decompile/disassemble)
//
OSStatus SecRequirementCopyString(SecRequirementRef requirementRef, SecCSFlags flags,
	CFStringRef *text)
{
	BEGIN_CSAPI
	
	const Requirement *req = SecRequirement::required(requirementRef)->requirement();
	checkFlags(flags);
	CodeSigning::Required(text);
	*text = makeCFString(Dumper::dump(req));

	END_CSAPI
}


//
CFStringRef kSecRequirementKeyInfoPlist = CFSTR("requirement:eval:info");
CFStringRef kSecRequirementKeyEntitlements = CFSTR("requirement:eval:entitlements");
CFStringRef kSecRequirementKeyIdentifier = CFSTR("requirement:eval:identifier");

OSStatus SecRequirementEvaluate(SecRequirementRef requirementRef,
	CFArrayRef certificateChain, CFDictionaryRef context,
	SecCSFlags flags)
{
	BEGIN_CSAPI

	const Requirement *req = SecRequirement::required(requirementRef)->requirement();
	checkFlags(flags);
	CodeSigning::Required(certificateChain);
	
	Requirement::Context ctx(certificateChain,		// mandatory
		context ? CFDictionaryRef(CFDictionaryGetValue(context, kSecRequirementKeyInfoPlist)) : NULL,
		context ? CFDictionaryRef(CFDictionaryGetValue(context, kSecRequirementKeyEntitlements)) : NULL,
		(context && CFDictionaryGetValue(context, kSecRequirementKeyIdentifier)) ?
			cfString(CFStringRef(CFDictionaryGetValue(context, kSecRequirementKeyIdentifier))) : "",
		NULL	// can't specify a CodeDirectory here
	);
	req->validate(ctx);
	
	END_CSAPI
}


//
// Assemble a requirement set (as a CFData) from a dictionary of requirement objects.
// An empty set is allowed.
//
OSStatus SecRequirementsCreateFromRequirements(CFDictionaryRef requirements, SecCSFlags flags,
	CFDataRef *requirementSet)
{
	BEGIN_CSAPI
	
	checkFlags(flags);
	if (requirements == NULL)
		return errSecCSObjectRequired;
	CFIndex count = CFDictionaryGetCount(requirements);
	CFNumberRef keys[count];
	SecRequirementRef reqs[count];
	CFDictionaryGetKeysAndValues(requirements, (const void **)keys, (const void **)reqs);
	Requirements::Maker maker;
	for (CFIndex n = 0; n < count; n++) {
		const Requirement *req = SecRequirement::required(reqs[n])->requirement();
		maker.add(cfNumber<Requirements::Type>(keys[n]), req->clone());
	}
	Requirements *reqset = maker.make();					// malloc'ed
	CodeSigning::Required(requirementSet) = makeCFDataMalloc(*reqset);	// takes ownership of reqs

	END_CSAPI
}


//
// Break a requirement set (given as a CFData) into its constituent requirements
// and return it as a CFDictionary.
//
OSStatus SecRequirementsCopyRequirements(CFDataRef requirementSet, SecCSFlags flags,
	CFDictionaryRef *requirements)
{
	BEGIN_CSAPI
	
	checkFlags(flags);
	if (requirementSet == NULL)
		return errSecCSObjectRequired;
	const Requirements *reqs = (const Requirements *)CFDataGetBytePtr(requirementSet);
	CFRef<CFMutableDictionaryRef> dict = makeCFMutableDictionary();
	unsigned count = reqs->count();
	for (unsigned n = 0; n < count; n++) {
		CFRef<SecRequirementRef> req = (new SecRequirement(reqs->blob<Requirement>(n)))->handle();
		CFDictionaryAddValue(dict, CFTempNumber(reqs->type(n)), req);
	}
	CodeSigning::Required(requirements) = dict.yield();

	END_CSAPI
}

	
//
// Generically parse a string as some kind of requirement-related source form.
// If properly recognized, return the result as a CF object:
//	SecRequirementRef for a single requirement
//	CFDataRef for a requirement set
//
OSStatus SecRequirementsCreateWithString(CFStringRef text, SecCSFlags flags,
	CFTypeRef *result, CFErrorRef *errors)
{
	BEGIN_CSAPI
	
	checkFlags(flags, kSecCSParseRequirement | kSecCSParseRequirementSet);
	if (text == NULL || result == NULL)
		return errSecCSObjectRequired;
	std::string s = cfString(text);
	switch (flags & (kSecCSParseRequirement | kSecCSParseRequirementSet)) {
	case kSecCSParseRequirement:		// single only
		*result = (new SecRequirement(parseRequirement(s), true))->handle();
		break;
	case kSecCSParseRequirementSet:		// single only
		{
			const Requirements *reqs = parseRequirements(s);
			*result = makeCFDataMalloc(*reqs);
			break;
		}
	case 0:
	case kSecCSParseRequirement | kSecCSParseRequirementSet:
		{
			const BlobCore *any = parseGeneric(s);
			if (any->is<Requirement>())
				*result = (new SecRequirement(Requirement::specific(any), true))->handle();
			else
				*result = makeCFDataMalloc(*any);
			break;
		}
	}

	END_CSAPI_ERRORS
}

	
//
// Convert a SecRequirementRef or a CFDataRef containing a requirement set to text.
// Requirement sets will be formatted as multiple lines (one per requirement). They can be empty.
// A single requirement will return a single line that is NOT newline-terminated.
//
OSStatus SecRequirementsCopyString(CFTypeRef input, SecCSFlags flags, CFStringRef *text)
{
	BEGIN_CSAPI
	
	checkFlags(flags);
	if (input == NULL)
		return errSecCSObjectRequired;
	if (CFGetTypeID(input) == SecRequirementGetTypeID()) {
		return SecRequirementCopyString(SecRequirementRef(input), flags, text);
	} else if (CFGetTypeID(input) == CFDataGetTypeID()) {
		const Requirements *reqs = (const Requirements *)CFDataGetBytePtr(CFDataRef(input));
		if (!reqs->validateBlob(CFDataGetLength(CFDataRef(input))))
			return errSecCSReqInvalid;
		CodeSigning::Required(text) = makeCFString(Dumper::dump(reqs, false));
	} else
		return errSecCSInvalidObjectRef;

	END_CSAPI
}
