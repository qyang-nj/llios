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

/*!
	@header SecCodePriv
	SecCodePriv is the private counter-part to SecCode. Its contents are not
	official API, and are subject to change without notice.
*/
#ifndef _H_SECCODEPRIV
#define _H_SECCODEPRIV

#include <Security/SecCode.h>

#ifdef __cplusplus
extern "C" {
#endif


/*
 *	Private constants for SecCodeCopySigningInformation.
 *	These are returned with the 
 */
extern const CFStringRef kSecCodeInfoCodeDirectory;			/* Internal */
extern const CFStringRef kSecCodeInfoCodeOffset;			/* Internal */
extern const CFStringRef kSecCodeInfoResourceDirectory;		/* Internal */


/*!
	@function SecCodeGetStatus
	Retrieves the dynamic status for a SecCodeRef.
	
	The dynamic status of a code can change at any time; the value returned is a snapshot
	in time that is inherently stale by the time it is received by the caller. However,
	since the status bits can only change in certain ways, some information is indefinitely
	valid. For example, an indication of invalidity (kSecCodeStatusValid bit off) is permanent
	since the valid bit cannot be set once clear, while an indication of validity (bit set)
	may already be out of date.
	Use this call with caution; it is usually wiser to call the validation API functions
	and let then consider the status as part of their holistic computation. However,
	SecCodeGetStatus is useful at times to capture persistent (sticky) status configurations.

	@param code A valid SecCode object reference representing code running
	on the system.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param status Upon successful return, contains the dynamic status of code as
	determined by its host.
	
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
 */
OSStatus SecCodeGetStatus(SecCodeRef code, SecCSFlags flags, SecCodeStatus *status);


/*!
	@function SecCodeSetStatus
	Change the dynamic status of a SecCodeRef.
	
	@param code A valid SecCode object reference representing code running
	on the system.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param status Upon successful return, contains the dynamic status of code as
	determined by its host.
	
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
 */
typedef uint32_t SecCodeStatusOperation;
enum {
	kSecCodeOperationNull = 0,
	kSecCodeOperationInvalidate = 1,
	kSecCodeOperationSetHard = 2,
	kSecCodeOperationSetKill = 3,
};

OSStatus SecCodeSetStatus(SecCodeRef code, SecCodeStatusOperation operation,
	CFDictionaryRef arguments, SecCSFlags flags);


/*!
	@function SecCodeCopyInternalRequirement
	For a given Code or StaticCode object, retrieves a particular kind of internal
	requirement that was sealed during signing.

	This function will always fail for unsigned code. Requesting a type of internal
	requirement that was not given during signing is not an error.
	
	Specifying a type of kSecDesignatedRequirementType is not the same as calling
	SecCodeCopyDesignatedRequirement. This function will only return an explicit
	Designated Requirement if one was specified during signing. SecCodeCopyDesignatedRequirement
	will synthesize a suitable Designated Requirement if one was not given explicitly.
	
	@param code The Code or StaticCode object to be interrogated. For a Code
		argument, its StaticCode is processed as per SecCodeCopyStaticCode.
	@param type A SecRequirementType specifying which internal requirement is being
		requested.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param requirement On successful return, contains a copy of the internal requirement
		of the given type included in the given code. If the code has no such internal
		requirement, this argument is set to NULL (with no error).
	@result On success, noErr. On error, an OSStatus value
		documented in CSCommon.h or certain other Security framework headers.
*/
OSStatus SecCodeCopyInternalRequirement(SecStaticCodeRef code, SecRequirementType type,
	SecCSFlags flags, SecRequirementRef *requirement);


/*!
	@function SecCodeCreateWithPID
	Asks the kernel to return a SecCode object for a process identified
	by a UNIX process id (pid). This is a shorthand for asking SecGetRootCode()
	for a guest whose "pid" attribute has the given pid value.
	
	This is a deprecated convenience function.
	Call SecCodeCopyGuestWithAttributes instead.
	
	@param pid A process id for an existing UNIX process on the system.
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
	@param process On successful return, a SecCode object reference identifying
	the requesteed process.
	@result Upon success, noErr. Upon error, an OSStatus value documented in
	CSCommon.h or certain other Security framework headers.
*/
OSStatus SecCodeCreateWithPID(pid_t pid, SecCSFlags flags, SecCodeRef *process)
	AVAILABLE_MAC_OS_X_VERSION_10_5_AND_LATER_BUT_DEPRECATED_IN_MAC_OS_X_VERSION_10_6;


/*
	@function SecCodeSetDetachedSignature
	For a given Code or StaticCode object, explicitly specify the detached signature
	data used to verify it.
	This call unconditionally overrides any signature embedded in the Code and any
	previously specified detached signature; only the signature data specified here
	will be used from now on for this Code object. If NULL data is specified, the
	code object is returned to its natural signing state (before a detached
	signature was first attached to it).
	Any call to this function voids all cached validations for the Code object.
	Validations will be performed again as needed in the future. This call does not,
	by itself, perform or trigger any validations.
	Please note that it is possible to have multiple Code objects for the same static
	or dynamic code entity in the system. This function only attaches signature data
	to the particular SecStaticCodeRef involved. It is your responsibility to understand
	the object graph and pick the right one(s).
	
	@param code A Code or StaticCode object whose signature information is to be changed.
	@param signature A CFDataRef containing the signature data to be used for validating
		the given Code. This must be exactly the data previously generated as a detached
		signature by the SecCodeSignerAddSignature API or the codesign(1) command with
		the -D/--detached option.
		If signature is NULL, discards any previously set signature data and reverts
		to using the embedded signature, if any. If not NULL, the data is retained and used
		for future validation operations.
		The data may be retained or copied. Behavior is undefined if this object
		is modified after this call before it is replaced through another call to this
		function).
	@param flags Optional flags. Pass kSecCSDefaultFlags for standard behavior.
 */
OSStatus SecCodeSetDetachedSignature(SecStaticCodeRef code, CFDataRef signature,
	SecCSFlags flags);


#ifdef __cplusplus
}
#endif

#endif //_H_SECCODE
