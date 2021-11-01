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
// drmaker - create automatic Designated Requirements
//
#include "drmaker.h"
#include "csutilities.h"
#include <Security/oidsbase.h>
#include <Security/SecCertificatePriv.h>
//#include <Security/cssmapplePriv.h>

namespace Security {
namespace CodeSigning {


static const uint8_t adcSdkMarker[] = { APPLE_EXTENSION_OID, 2, 1 };		// iOS intermediate marker
const CSSM_DATA adcSdkMarkerOID = { sizeof(adcSdkMarker), (uint8_t *)adcSdkMarker };

static const uint8_t caspianSdkMarker[] = { APPLE_EXTENSION_OID, 2, 6 }; // Caspian intermediate marker
const CSSM_DATA devIdSdkMarkerOID = { sizeof(caspianSdkMarker), (uint8_t *)caspianSdkMarker };
static const uint8_t caspianLeafMarker[] = { APPLE_EXTENSION_OID, 1, 13 }; // Caspian leaf certificate marker
const CSSM_DATA devIdLeafMarkerOID = { sizeof(caspianLeafMarker), (uint8_t *)caspianLeafMarker };



DRMaker::DRMaker(const Requirement::Context &context)
	: ctx(context)
{
}

DRMaker::~DRMaker()
{
}


//
// Generate the default (implicit) Designated Requirement for this StaticCode.
// This is a heuristic of sorts, and may change over time (for the better, we hope).
//
Requirement *DRMaker::make()
{
	// we can't make an explicit DR for a (proposed) ad-hoc signing because that requires the CodeDirectory (which we ain't got yet)
	if (ctx.certCount() == 0)
		return NULL;

	// always require the identifier
	this->put(opAnd);
	this->ident(ctx.identifier);
	
	SHA1::Digest anchorHash;
	hashOfCertificate(ctx.cert(Requirement::anchorCert), anchorHash);
	if (!memcmp(anchorHash, Requirement::appleAnchorHash(), SHA1::digestLength)
#if	defined(TEST_APPLE_ANCHOR)
		|| !memcmp(anchorHash, Requirement::testAppleAnchorHash(), SHA1::digestLength)
#endif
		)
		appleAnchor();
	else
		nonAppleAnchor();
	
	return Maker::make();
}


void DRMaker::nonAppleAnchor()
{
	// get the Organization DN element for the leaf
	CFRef<CFStringRef> leafOrganization;
	MacOSError::check(SecCertificateCopySubjectComponent(ctx.cert(Requirement::leafCert),
		&CSSMOID_OrganizationName, &leafOrganization.aref()));

	// now step up the cert chain looking for the first cert with a different one
	int slot = Requirement::leafCert;						// start at leaf
	if (leafOrganization) {
		while (SecCertificateRef ca = ctx.cert(slot+1)) {		// NULL if you over-run the anchor slot
			CFRef<CFStringRef> caOrganization;
			MacOSError::check(SecCertificateCopySubjectComponent(ca, &CSSMOID_OrganizationName, &caOrganization.aref()));
			if (!caOrganization || CFStringCompare(leafOrganization, caOrganization, 0) != kCFCompareEqualTo)
				break;
			slot++;
		}
		if (slot == ctx.certCount() - 1)		// went all the way to the anchor...
			slot = Requirement::anchorCert;					// ... so say that
	}
	
	// nail the last cert with the leaf's Organization value
	SHA1::Digest authorityHash;
	hashOfCertificate(ctx.cert(slot), authorityHash);
	this->anchor(slot, authorityHash);
}


void DRMaker::appleAnchor()
{
	if (isIOSSignature()) {
		// get the Common Name DN element for the leaf
		CFRef<CFStringRef> leafCN;
		MacOSError::check(SecCertificateCopySubjectComponent(ctx.cert(Requirement::leafCert),
			&CSSMOID_CommonName, &leafCN.aref()));
		
		// apple anchor generic and ...
		this->put(opAnd);
		this->anchorGeneric();			// apple generic anchor and...
		// ... leaf[subject.CN] = <leaf's subject> and ...
		this->put(opAnd);
		this->put(opCertField);			// certificate
		this->put(0);					// leaf
		this->put("subject.CN");		// [subject.CN]
		this->put(matchEqual);			// =
		this->putData(leafCN);			// <leaf CN>
		// ... cert 1[field.<marker>] exists
		this->put(opCertGeneric);		// certificate
		this->put(1);					// 1
		this->putData(adcSdkMarkerOID.Data, adcSdkMarkerOID.Length); // [field.<marker>]
		this->put(matchExists);			// exists
		return;
	}
	
	if (isDeveloperIDSignature()) {
		// get the Organizational Unit DN element for the leaf (it contains the TEAMID)
		CFRef<CFStringRef> teamID;
		MacOSError::check(SecCertificateCopySubjectComponent(ctx.cert(Requirement::leafCert),
			&CSSMOID_OrganizationalUnitName, &teamID.aref()));

		// apple anchor generic and ...
		this->put(opAnd);
		this->anchorGeneric();			// apple generic anchor and...
		
		// ... certificate 1[intermediate marker oid] exists and ...
		this->put(opAnd);
		this->put(opCertGeneric);		// certificate
		this->put(1);					// 1
		this->putData(caspianSdkMarker, sizeof(caspianSdkMarker));
		this->put(matchExists);			// exists
		
		// ... certificate leaf[Caspian cert oid] exists and ...
		this->put(opAnd);
		this->put(opCertGeneric);		// certificate
		this->put(0);					// leaf
		this->putData(caspianLeafMarker, sizeof(caspianLeafMarker));
		this->put(matchExists);			// exists

		// ... leaf[subject.OU] = <leaf's subject>
		this->put(opCertField);			// certificate
		this->put(0);					// leaf
		this->put("subject.OU");		// [subject.OU]
		this->put(matchEqual);			// =
		this->putData(teamID);			// TEAMID
		return;
	}

	// otherwise, claim this program for Apple Proper
	this->anchor();
}

bool DRMaker::isIOSSignature()
{
	if (ctx.certCount() == 3)		// leaf, one intermediate, anchor
		if (SecCertificateRef intermediate = ctx.cert(1)) // get intermediate
			if (certificateHasField(intermediate, CssmOid::overlay(adcSdkMarkerOID)))
				return true;
	return false;
}

bool DRMaker::isDeveloperIDSignature()
{
	if (ctx.certCount() == 3)		// leaf, one intermediate, anchor
		if (SecCertificateRef intermediate = ctx.cert(1)) // get intermediate
			if (certificateHasField(intermediate, CssmOid::overlay(devIdSdkMarkerOID)))
				return true;
	return false;
}


} // end namespace CodeSigning
} // end namespace Security
