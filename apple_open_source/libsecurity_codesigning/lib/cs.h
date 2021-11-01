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
// cs.h - code signing core header
//
#ifndef _H_CS
#define _H_CS

#include "cserror.h"
#include "codesigning_dtrace.h"
#include <Security/CSCommonPriv.h>
#include <Security/SecCodePriv.h>
#include <Security/SecStaticCodePriv.h>
#include <Security/SecRequirementPriv.h>
#include <Security/SecCodeSigner.h>
#include <Security/SecBasePriv.h>
#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacErrors.h>
#include <security_utilities/globalizer.h>
#include <security_utilities/seccfobject.h>
#include <security_utilities/cfclass.h>
#include <security_utilities/errors.h>
#include <security_utilities/sqlite++.h>
#include <security_utilities/cfutilities.h>


namespace Security {
namespace CodeSigning {


//
// API per-thread globals
//
struct PerThread {
	SecCSFlags flags;				// flags of pending API call
};


//
// API globals
//
struct CFObjects {
	CFObjects();
	CFClass Code;
	CFClass StaticCode;
	CFClass Requirement;
	CFClass CodeSigner;

	ThreadNexus<PerThread> perThread;
	
	SecCSFlags &flags() { return perThread().flags; }
};

extern ModuleNexus<CFObjects> gCFObjects;

static inline SecCSFlags apiFlags() { return gCFObjects().flags(); }

OSStatus dbError(const SQLite3::Error &err);


//
// Code Signing API brackets
//
#define BEGIN_CSAPI \
    try {
	
#define END_CSAPI \
    } \
	catch (const UnixError &err) { \
		switch (err.error) { \
		case ENOEXEC: return errSecCSBadObjectFormat; \
		default: return err.osStatus(); \
		}} \
    catch (const MacOSError &err) { return err.osStatus(); } \
    catch (const SQLite3::Error &err) { return dbError(err); } \
    catch (const CommonError &err) { return SecKeychainErrFromOSStatus(err.osStatus()); } \
    catch (const std::bad_alloc &) { return memFullErr; } \
    catch (...) { return errSecCSInternalError; } \
	return noErr;
	
#define END_CSAPI_ERRORS \
	} \
	catch (const CSError &err) { return err.cfError(errors); } \
	catch (const UnixError &err) { \
		switch (err.error) { \
		case ENOEXEC: return CSError::cfError(errors, errSecCSBadObjectFormat); \
		default: return CSError::cfError(errors, err.osStatus()); \
		}} \
    catch (const MacOSError &err) { return CSError::cfError(errors, err.osStatus()); } \
    catch (const SQLite3::Error &err) { return CSError::cfError(errors, dbError(err)); } \
    catch (const CommonError &err) { return CSError::cfError(errors, SecKeychainErrFromOSStatus(err.osStatus())); } \
    catch (const std::bad_alloc &) { return CSError::cfError(errors, memFullErr); } \
    catch (...) { return CSError::cfError(errors, errSecCSInternalError); } \
	return noErr;
	
#define END_CSAPI1(bad)    } catch (...) { return bad; }

	
#define END_CSAPI_ERRORS1(bad) \
	} \
	catch (const CSError &err) { err.cfError(errors); } \
	catch (const UnixError &err) { \
		switch (err.error) { \
		case ENOEXEC: CSError::cfError(errors, errSecCSBadObjectFormat); \
		default: CSError::cfError(errors, err.osStatus()); \
		}} \
    catch (const MacOSError &err) { CSError::cfError(errors, err.osStatus()); } \
    catch (const SQLite3::Error &err) { CSError::cfError(errors, dbError(err)); } \
    catch (const CommonError &err) { CSError::cfError(errors, SecKeychainErrFromOSStatus(err.osStatus())); } \
    catch (const std::bad_alloc &) { CSError::cfError(errors, memFullErr); } \
    catch (...) { CSError::cfError(errors, errSecCSInternalError); } \
	return bad;


//
// A version of CodeSigning::Required
//
template <class T>
static inline T &Required(T *ptr)
{
	if (ptr == NULL)
		MacOSError::throwMe(errSecCSObjectRequired);
	return *ptr;
}

static inline void Required(const void *ptr)
{
	if (ptr == NULL)
		MacOSError::throwMe(errSecCSObjectRequired);
}


//
// Check flags against a validity mask
//
static inline void checkFlags(SecCSFlags flags, SecCSFlags acceptable = 0)
{
	if (flags & ~acceptable)
		MacOSError::throwMe(errSecCSInvalidFlags);
	gCFObjects().flags() = flags;
}


//
// DTrace USDT function bracket.
// Use like this:
//	DTRACK(PROVIDER_PROBE_PREFIX, arguments-after-this);
// which will call
//	PROVIDER_PROBE_PREFIX_START(this, arguments-after-this)
// and
//	PROVIDER_PROBE_PREFIX_END(this)
//
#define DTRACK(_prefix, _obj, _args...) \
	if (_prefix ## _START_ENABLED()) _prefix ## _START((_obj), ## _args); \
	struct _DTFrame ## _prefix { void *me; \
		_DTFrame ## _prefix(void *m) : me(m) { } \
		~_DTFrame ## _prefix() { _prefix ## _END(me); } \
	} _dtframe##_prefix((_obj));


}	// CodeSigning
}	// Security

#endif //_H_CS
