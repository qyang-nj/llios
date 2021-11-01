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
#include "cs.h"
#include <security_utilities/cfmunge.h>

namespace Security {
namespace CodeSigning {


ModuleNexus<CFObjects> gCFObjects;

CFObjects::CFObjects()
	: Code("SecCode"),
	  StaticCode("SecStaticCode"),
	  Requirement("SecRequirements"),
	  CodeSigner("SecCodeSigner")
{
}


OSStatus dbError(const SQLite3::Error &err)
{
	switch (err.error) {
	case SQLITE_PERM:
	case SQLITE_READONLY:
	case SQLITE_AUTH:
		return errSecCSSigDBDenied;
	case SQLITE_CANTOPEN:
	case SQLITE_EMPTY:
	case SQLITE_NOTADB:
		return errSecCSSigDBAccess;
	default:
		return SecKeychainErrFromOSStatus(err.osStatus());
	}
}


}	// CodeSigning
}	// Security
