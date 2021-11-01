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

/*!
	@header SecRequirement
	SecRequirementPriv is the private counter-part to SecRequirement. Its contents are not
	official API, and are subject to change without notice.
*/
#ifndef _H_SECREQUIREMENTPRIV
#define _H_SECREQUIREMENTPRIV

#include <Security/SecRequirement.h>

#ifdef __cplusplus
extern "C" {
#endif


/*!
	@function SecRequirementsCreateFromRequirements
	Take a dictionary of requirement objects and package them up as a requirement set.
	
	@param requirements A dictionary of requirements to combine into a set.
	Dictionary keys are CFNumbers representing the index keys. Values are SecRequirementRefs.
	NULL requirements are not allowed in the dictionary.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param requirementSet Upon success, receives a CFData object 
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
*/
OSStatus SecRequirementsCreateFromRequirements(CFDictionaryRef requirements, SecCSFlags flags,
	CFDataRef *requirementSet);


/*!
	@function SecRequirementsCopyRequirements
	Create a SecRequirement object from binary form obtained from a file.
	This call is functionally equivalent to reading the entire contents of a file
	into a CFDataRef and then calling SecRequirementCreateWithData with that.
	
	@param requirementSet A CFData containing a requirement set.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param requirements Upon success, a dictionary containing each requirement contained
	in requirementSet. The keys are CFNumbers indicating the requirement type.
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
*/
OSStatus SecRequirementsCopyRequirements(CFDataRef requirementSet, SecCSFlags flags,
	CFDictionaryRef *requirements);

	
/*!
	@function SecRequirementsCreateWithString
	Create a SecRequirement object or requirement set based on the string provided.
	
	@param text A CFString containing the text form of a (single) Code Requirement.
	@param flags Optional flags. Pass kSecCSDefaultFlags to accept any supported input form.
	Pass a combination of individual flags to select what forms to accept; other forms will result
	in an error.
	@param result Upon success, a CoreFoundation object of some kind representing
	the result of parsing text. Depending on the input string and flags, the result
	can be a SecRequirementRef (for a single requirement) or a CFDataRef for a requirement set.
	@param errors An optional pointer to a CFErrorRef variable. If the call fails
	(and something other than noErr is returned), and this argument is non-NULL,
	a CFErrorRef is stored there further describing the nature and circumstances
	of the failure. The caller must CFRelease() this error object when done with it.
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
*/
enum {
	kSecCSParseRequirement = 0x0001,		// accept single requirements
	kSecCSParseRequirementSet = 0x0002,		// accept requirement sets
};

OSStatus SecRequirementsCreateWithString(CFStringRef text, SecCSFlags flags,
	CFTypeRef *result, CFErrorRef *errors);


/*!
	@function SecRequirementsCopyString
	Converts a requirement object of some kind into text form.
	This is the effective inverse of SecRequirementsCreateWithString.

	This function can process individual requirements (SecRequirementRefs)
	and requirement sets (represented as CFDataRefs).
	
	Repeated application of this function may produce text that differs in
	formatting, may contain different source comments, and may perform its
	validation functions in different order. However, it is guaranteed that
	recompiling the text using SecRequirementCreateWithString will produce a
	SecRequirement object that behaves identically to the one you start with.
	
	@param requirements A SecRequirementRef, or a CFDataRef containing a valid requirement set.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param text On successful return, contains a reference to a CFString object
	containing a text representation of the requirement.
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
*/
OSStatus SecRequirementsCopyString(CFTypeRef input, SecCSFlags flags, CFStringRef *text);
	

/*!
	@function SecRequirementCreateWithResource
	Create a SecRequirement object from binary form obtained from a file.
	This call is functionally equivalent to reading the entire contents of a file
	into a CFDataRef and then calling SecRequirementCreateWithData with that.
	
	@param resource A CFURL identifying a file containing a (binary) requirement blob.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param requirement On successful return, contains a reference to a SecRequirement
	object that behaves identically to the one the data blob was obtained from.
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
*/
OSStatus SecRequirementCreateWithResource(CFURLRef resource, SecCSFlags flags,
	SecRequirementRef *requirement);

	
/*!
	@function SecRequirementCreateGroup
	Create a SecRequirement object that represents membership in a developer-defined
	application	group. Group membership is defined by an entry in the code's
	Info.plist, and sealed to a particular signing authority.

	This is not an API-track function. Don't call it if you don't already do.
	
	@param groupName A CFString containing the name of the desired application group.
	@param anchor A reference to a digital certificate representing the signing
	authority that asserts group membership. If NULL, indicates Apple's authority.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param requirement On successful return, contains a reference to a SecRequirement
	object that requires group membership to pass validation.
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
*/
OSStatus SecRequirementCreateGroup(CFStringRef groupName, SecCertificateRef anchor,
	SecCSFlags flags, SecRequirementRef *requirement);


	
/*!
	@function SecRequirementEvaluate
	Explicitly evaluate a SecRequirementRef against context provided in the call.
	This allows evaluation of a code requirement outside the context of a code signature.

	@param requirement A valid SecRequirement object.
	@param certificateChain A CFArray of SecCertificate objects describing the certificate
	chain of the object being validated. This must be a full chain terminating in an anchor
	certificate that is cryptographically valid.
	@param context An optional CFDictionary containing additional context made available
	to the requirement program's evaluation. NULL is equivalent to an empty dictionary.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@result Upon success, noErr. Failure to pass the check returns errSecCSReqFailed.
	All other returns indicate errors as documented in CSCommon.h or certain other
	Security framework headers.
	
	@constant kSecRequirementKeyInfoPlist A context key providing an CFDictionary denoting
	an Info.plist. If this key is missing, all references to Info.plist contents will fail.
	@constant kSecRequirementKeyEntitlements A context key providing an CFDictionary describing
	an entitlement dictionary. If this key is missing, all references to entitlements will fail.
	@constant kSecRequirementKeyIdentifier A context key providing the signing identifier as a CFString.
*/
extern CFStringRef kSecRequirementKeyInfoPlist;
extern CFStringRef kSecRequirementKeyEntitlements;
extern CFStringRef kSecRequirementKeyIdentifier;

OSStatus SecRequirementEvaluate(SecRequirementRef requirement,
	CFArrayRef certificateChain, CFDictionaryRef context,
	SecCSFlags flags);


#ifdef __cplusplus
}
#endif

#endif //_H_SECREQUIREMENTPRIV
