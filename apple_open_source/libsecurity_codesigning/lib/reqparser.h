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

//
// reqparser - interface to Requirement language parser/compiler
//
#ifndef _H_REQPARSER
#define _H_REQPARSER

#include "requirement.h"

namespace Security {
namespace CodeSigning {


//
// Generic parser interface
//
template <class ReqType>
class RequirementParser {
public:
	const ReqType *operator () (std::FILE *file);
	const ReqType *operator () (const std::string &text);
};


//
// Specifics for easier readability
//
template <class Input>
inline const Requirement *parseRequirement(const Input &source)
{ return RequirementParser<Requirement>()(source); }

template <class Input>
inline const Requirements *parseRequirements(const Input &source)
{ return RequirementParser<Requirements>()(source); }

template <class Input>
inline const BlobCore *parseGeneric(const Input &source)
{ return RequirementParser<BlobCore>()(source); }


}	// CodeSigning
}	// Security

#endif //_H_REQPARSER
