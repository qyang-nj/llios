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
#ifndef _H_XPCENGINE
#define _H_XPCENGINE

#include "SecAssessment.h"
#include "policydb.h"
#include <xpc/private.h>
#include <Security/Security.h>

namespace Security {
namespace CodeSigning {


void xpcEngineAssess(CFURLRef path, uint flags, CFDictionaryRef context, CFMutableDictionaryRef result);
CFDictionaryRef xpcEngineUpdate(CFTypeRef target, uint flags, CFDictionaryRef context)
    CF_RETURNS_RETAINED;
bool xpcEngineControl(const char *name);


} // end namespace CodeSigning
} // end namespace Security

#endif //_H_XPCENGINE
