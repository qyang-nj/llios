/*
 * Copyright (c) 2012 Apple Inc. All Rights Reserved.
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
// drmaker - create Designated Requirements
//
#ifndef _H_DRMAKER
#define _H_DRMAKER

#include "reqmaker.h"

namespace Security {
namespace CodeSigning {


//
// Some useful certificate OID markers
//
extern const CSSM_DATA adcSdkMarkerOID;
extern const CSSM_DATA devIdSdkMarkerOID;
extern const CSSM_DATA devIdLeafMarkerOID;



//
// A Maker of Designated Requirements
//
class DRMaker : public Requirement::Maker {
public:
	DRMaker(const Requirement::Context &context);
	virtual ~DRMaker();
	
	const Requirement::Context &ctx;
	
public:
	Requirement *make();

private:
	void appleAnchor();
	void nonAppleAnchor();
	bool isIOSSignature();
	bool isDeveloperIDSignature();
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_DRMAKER
