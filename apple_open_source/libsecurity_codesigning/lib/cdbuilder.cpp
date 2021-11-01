/*
 * Copyright (c) 2006-2010 Apple Inc. All Rights Reserved.
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
// cdbuilder - constructor for CodeDirectories
//
#include "cdbuilder.h"
#include <security_utilities/memutils.h>
#include <cmath>

using namespace UnixPlusPlus;
using LowLevelMemoryUtilities::alignUp;


namespace Security {
namespace CodeSigning {


//
// Create an (empty) builder
//
CodeDirectory::Builder::Builder(HashAlgorithm digestAlgorithm)
	: mFlags(0),
	  mHashType(digestAlgorithm),
	  mSpecialSlots(0),
	  mCodeSlots(0),
	  mScatter(NULL),
	  mScatterSize(0),
	  mDir(NULL)
{
	mDigestLength = MakeHash<Builder>(this)->digestLength();
	mSpecial = (unsigned char *)calloc(cdSlotMax, mDigestLength);
}

CodeDirectory::Builder::~Builder()
{
	::free(mSpecial);
	::free(mScatter);
}


//
// Set the source of the main executable (i.e. the code pages)
//
void CodeDirectory::Builder::executable(string path,
	size_t pagesize, size_t offset, size_t length)
{
	mExec.close();			// any previously opened one
	mExec.open(path);
	mPageSize = pagesize;
	mExecOffset = offset;
	mExecLength = length;
}

void CodeDirectory::Builder::reopen(string path, size_t offset, size_t length)
{
	assert(mExec);					// already called executable()
	mExec.close();
	mExec.open(path);
	mExecOffset = offset;
	mExecLength = length;
}


//
// Set the source for one special slot
//
void CodeDirectory::Builder::specialSlot(SpecialSlot slot, CFDataRef data)
{
	assert(slot <= cdSlotMax);
	MakeHash<Builder> hash(this);
	hash->update(CFDataGetBytePtr(data), CFDataGetLength(data));
	hash->finish(specialSlot(slot));
	if (slot >= mSpecialSlots)
		mSpecialSlots = slot;
}


//
// Allocate a Scatter vector
//
CodeDirectory::Scatter *CodeDirectory::Builder::scatter(unsigned count)
{
	mScatterSize = (count + 1) * sizeof(Scatter);
	if (!(mScatter = (Scatter *)::realloc(mScatter, mScatterSize)))
		UnixError::throwMe(ENOMEM);
	::memset(mScatter, 0, mScatterSize);
	return mScatter;
}


//
// Calculate the size we'll need for the CodeDirectory as described so far
//
size_t CodeDirectory::Builder::size()
{
	assert(mExec);			// must have called executable()
	if (mExecLength == 0)
		mExecLength = mExec.fileSize() - mExecOffset;

	// how many code pages?
	if (mPageSize == 0) {	// indefinite - one page
		mCodeSlots = (mExecLength > 0);
	} else {				// finite - calculate from file size
		mCodeSlots = (mExecLength + mPageSize - 1) / mPageSize; // round up
	}
		
	size_t offset = sizeof(CodeDirectory);
	offset += mScatterSize;				// scatter vector
	offset += mIdentifier.size() + 1;	// size of identifier (with null byte)
	offset += (mCodeSlots + mSpecialSlots) * mDigestLength; // hash vector
	return offset;
}


//
// Take everything added to date and wrap it up in a shiny new CodeDirectory.
//
// Note that this only constructs a CodeDirectory; it does not touch any subsidiary
// structures (resource tables, etc.), nor does it create any signature to secure
// the CodeDirectory.
// The returned CodeDirectory object is yours, and you may modify it as desired.
// But the memory layout is set here, so the various sizes and counts should be good
// when you call build().
// It's up to us to order the dynamic fields as we wish; but note that we currently
// don't pad them, and so they should be allocated in non-increasing order of required
// alignment. Make sure to keep the code here in sync with the size-calculating code above.
//
CodeDirectory *CodeDirectory::Builder::build()
{
	assert(mExec);			// must have (successfully) called executable()

	// size and allocate
	size_t identLength = mIdentifier.size() + 1;
	size_t total = size();
	if (!(mDir = (CodeDirectory *)calloc(1, total)))	// initialize to zero
		UnixError::throwMe(ENOMEM);
	
	// fill header
	mDir->initialize(total);
	mDir->version = currentVersion;
	mDir->flags = mFlags;
	mDir->nSpecialSlots = mSpecialSlots;
	mDir->nCodeSlots = mCodeSlots;
	mDir->codeLimit = mExecLength;
	mDir->hashType = mHashType;
	mDir->hashSize = mDigestLength;
	if (mPageSize) {
		int pglog;
		assert(frexp(mPageSize, &pglog) == 0.5); // must be power of 2
		frexp(mPageSize, &pglog);
		assert(pglog < 256);
		mDir->pageSize = pglog - 1;
	} else
		mDir->pageSize = 0;	// means infinite page size

	// locate and fill flex fields
	size_t offset = sizeof(CodeDirectory);

	if (mScatter) {
		mDir->scatterOffset = offset;
		memcpy(mDir->scatterVector(), mScatter, mScatterSize);
		offset += mScatterSize;
	}

	mDir->identOffset = offset;
	memcpy(mDir->identifier(), mIdentifier.c_str(), identLength);
	offset += identLength;

	// (add new flexibly-allocated fields here)

	mDir->hashOffset = offset + mSpecialSlots * mDigestLength;
	offset += (mSpecialSlots + mCodeSlots) * mDigestLength;
	assert(offset == total);	// matches allocated size
	
	// fill special slots
	memset((*mDir)[-mSpecialSlots], 0, mDigestLength * mSpecialSlots);
	for (size_t slot = 1; slot <= mSpecialSlots; ++slot)
		memcpy((*mDir)[-slot], specialSlot(slot), mDigestLength);
	
	// fill code slots
	mExec.seek(mExecOffset);
	size_t remaining = mExecLength;
	for (unsigned int slot = 0; slot < mCodeSlots; ++slot) {
		size_t thisPage = min(mPageSize, remaining);
		MakeHash<Builder> hasher(this);
		generateHash(hasher, mExec, (*mDir)[slot], thisPage);
		remaining -= thisPage;
	}
	
	// all done. Pass ownership to caller
	return mDir;
}


}	// CodeSigning
}	// Security
