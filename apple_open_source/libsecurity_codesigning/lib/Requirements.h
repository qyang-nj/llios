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
// Requirements - SecRequirement API objects
//
#ifndef _H_REQUIREMENTS
#define _H_REQUIREMENTS

#include "cs.h"
#include "requirement.h"

namespace Security {
namespace CodeSigning {


//
// A SecRequirement object acts as the API representation for a code
// requirement. All its semantics are within the Requirement object it holds.
// The SecRequirement just manages the API appearances.
//
class SecRequirement : public SecCFObject {
	NOCOPY(SecRequirement)
public:
	SECCFFUNCTIONS(SecRequirement, SecRequirementRef, errSecCSInvalidObjectRef, gCFObjects().Requirement)

	SecRequirement(const void *data, size_t length);
	SecRequirement(const Requirement *req, bool transferOwnership = false);
    virtual ~SecRequirement() throw();
	
    bool equal(SecCFObject &other);
    CFHashCode hash();
	
	const Requirement *requirement() const { return mReq; }

private:
	const Requirement *mReq;
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_REQUIREMENTS
