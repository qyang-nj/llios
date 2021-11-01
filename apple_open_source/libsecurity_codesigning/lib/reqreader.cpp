/*
 * Copyright (c) 2007 Apple Inc. All Rights Reserved.
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
// reqreader - Requirement language (exprOp) reader/scanner
//
#include "reqreader.h"
#include <Security/SecTrustSettingsPriv.h>
#include <security_utilities/memutils.h>
#include <security_cdsa_utilities/cssmdata.h>	// for hex encoding
#include "csutilities.h"

namespace Security {
namespace CodeSigning {


//
// Requirement::Reader
//
Requirement::Reader::Reader(const Requirement *req)
	: mReq(req), mPC(sizeof(Requirement))
{
	assert(req);
	if (req->kind() != exprForm)
		MacOSError::throwMe(errSecCSReqUnsupported);
}


//
// Access helpers to retrieve various data types from the data stream
//
void Requirement::Reader::getData(const void *&data, size_t &length)
{
	length = get<uint32_t>();
	checkSize(length);
	data = (mReq->at<void>(mPC));
	mPC += LowLevelMemoryUtilities::alignUp(length, baseAlignment);
}

string Requirement::Reader::getString()
{
	const char *s; size_t length;
	getData(s, length);
	return string(s, length);
}

const unsigned char *Requirement::Reader::getHash()
{
	const unsigned char *s; size_t length;
	getData(s, length);
	if (length != SHA1::digestLength)
		MacOSError::throwMe(errSecCSReqInvalid);
	return s;
}

const unsigned char *Requirement::Reader::getSHA1()
{
	const unsigned char *digest; size_t length;
	getData(digest, length);
	if (length != CC_SHA1_DIGEST_LENGTH)
		MacOSError::throwMe(errSecCSReqInvalid);
	return digest;
}

void Requirement::Reader::skip(size_t length)
{
	checkSize(length);
	mPC += length;
}


}	// CodeSigning
}	// Security
