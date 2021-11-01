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
// reqmaker - Requirement assembler
//
#include "reqmaker.h"

namespace Security {
namespace CodeSigning {


//
// Requirement::Makers
//
Requirement::Maker::Maker(Kind k)
	: mSize(1024)
{
	mBuffer = (Requirement *)malloc(mSize);
	mBuffer->initialize();
	mBuffer->kind(k);
	mPC = sizeof(Requirement);
}

// need at least (size) bytes in the creation buffer
void Requirement::Maker::require(size_t size)
{
	if (mPC + size > mSize) {
		mSize *= 2;
		if (mPC + size > mSize)
			mSize = mPC + size;
		if (!(mBuffer = (Requirement *)realloc(mBuffer, mSize)))
			UnixError::throwMe(ENOMEM);
	}
}

// allocate (size) bytes at end of buffer and return pointer to that
void *Requirement::Maker::alloc(size_t size)
{
	// round size up to preserve alignment
	size_t usedSize = LowLevelMemoryUtilities::alignUp(size, baseAlignment);
	require(usedSize);
	void *data = mBuffer->at<void>(mPC);
	mPC += usedSize;
	
	// clear any padding (avoid random bytes in code image)
	const uint32_t zero = 0;
	memcpy(mBuffer->at<void>(mPC - usedSize + size), &zero, usedSize - size);
	
	// all done
	return data;
}

// put contiguous data blob
void Requirement::Maker::putData(const void *data, size_t length)
{
	put(uint32_t(length));
	memcpy(alloc(length), data, length);
}

// Specialized Maker put operations
void Requirement::Maker::anchor()
{
	put(opAppleAnchor);
}

void Requirement::Maker::anchorGeneric()
{
	put(opAppleGenericAnchor);
}

void Requirement::Maker::anchor(int slot, SHA1::Digest digest)
{
	put(opAnchorHash);
	put(slot);
	putData(digest, SHA1::digestLength);
}

void Requirement::Maker::anchor(int slot, const void *cert, size_t length)
{
	SHA1 hasher;
	hasher(cert, length);
	SHA1::Digest digest;
	hasher.finish(digest);
	anchor(slot, digest);
}

void Requirement::Maker::trustedAnchor()
{
	put(opTrustedCerts);
}

void Requirement::Maker::trustedAnchor(int slot)
{
	put(opTrustedCert);
	put(slot);
}

void Requirement::Maker::infoKey(const string &key, const string &value)
{
	put(opInfoKeyValue);
	put(key);
	put(value);
}

void Requirement::Maker::ident(const string &identifier)
{
	put(opIdent);
	put(identifier);
}

void Requirement::Maker::cdhash(SHA1::Digest digest)
{
	put(opCDHash);
	putData(digest, SHA1::digestLength);
}



void Requirement::Maker::copy(const Requirement *req)
{
	assert(req);
	if (req->kind() != exprForm)		// don't know how to embed this
		MacOSError::throwMe(errSecCSReqUnsupported);
	this->copy(req->at<const void>(sizeof(Requirement)), req->length() - sizeof(Requirement));
}


void *Requirement::Maker::insert(const Label &label, size_t length)
{
	require(length);
	memmove(mBuffer->at<void>(label.pos + length),
		mBuffer->at<void>(label.pos), mPC - label.pos);
	mPC += length;
	return mBuffer->at<void>(label.pos);
}


Requirement *Requirement::Maker::make()
{
	mBuffer->length(mPC);
	Requirement *result = mBuffer;
	mBuffer = NULL;
	return result;
}


}	// CodeSigning
}	// Security
