/*
 * Copyright (c) 2009 Apple Inc. All Rights Reserved.
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
// slcrep - DiskRep representing the Mac OS Shared Library Cache
//
#include "slcrep.h"


namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


//
// Object management.
// We open the file lazily, so nothing much happens on constructions.
// We can construct directly from a file path, or from an architecture
// (represented by Context), which will find the file in its usual
// location on disk.
//
DYLDCacheRep::DYLDCacheRep(const char *path)
	: SingleDiskRep(path), mCache(path)
{
	this->setup();
}

DYLDCacheRep::DYLDCacheRep(const Context *ctx)
	: SingleDiskRep(DYLDCache::pathFor(((ctx && ctx->arch) ? ctx->arch : Architecture::local()))),
	  mCache(this->path())
{
	this->setup();
}

void DYLDCacheRep::setup()
{
	mSigningData = NULL;
	if (mCache.totalSize() >= mCache.mapSize() + sizeof(BlobCore)) {
		const EmbeddedSignatureBlob *blob = mCache.at<const EmbeddedSignatureBlob>(mCache.mapSize());
		if (mCache.totalSize() >= mCache.mapSize() + blob->length())	// entire blob fits in file
			mSigningData = blob;
	}
	CODESIGN_DISKREP_CREATE_SLC(this, (char*)this->mainExecutablePath().c_str());
}


//
// Sniffer function for "plausible shared library cache file".
//
bool DYLDCacheRep::candidate(FileDesc &fd)
{
	return DYLDCache::validate(fd);
}


//
// Default to system page size for segmented (paged) signatures
//
size_t DYLDCacheRep::pageSize(const SigningContext &)
{
	return segmentedPageSize;
}


//
// Retrieve a component from the executable.
// Our mCache has mapped the entire file, so we just fish the contents out of
// the mapped area as needed.
//
CFDataRef DYLDCacheRep::component(CodeDirectory::SpecialSlot slot)
{
	return mSigningData ? mSigningData->component(slot) : NULL;
}


//
// Provide a (vaguely) human readable characterization of this code
//
string DYLDCacheRep::format()
{
	if (const char *name = mCache.architecture().name()) {
		char result[100];
		snprintf(result, sizeof(result), "OS X Shared Library Cache (%s @ 0x%llx)",
			name, mCache.baseAddress());
		return result;
	} else
		return "OS X Shared Library Cache (unknown type)";
}


//
// DYLDCacheRep::Writers
//
DiskRep::Writer *DYLDCacheRep::writer()
{
	return new Writer(this);
}


//
// Write a component.
//
void DYLDCacheRep::Writer::component(CodeDirectory::SpecialSlot slot, CFDataRef data)
{
	EmbeddedSignatureBlob::Maker::component(slot, data);
}


//
// Append the superblob we built to the cache file.
//
void DYLDCacheRep::Writer::flush()
{
	delete mSigningData;			// ditch previous blob just in case
	mSigningData = Maker::make();	// assemble new signature SuperBlob
	fd().seek(rep->mCache.mapSize()); // end of impage proper
	fd().writeAll(*mSigningData);
}


//
// The discretionary additions insert a Scatter vector describing the file's mapping table.
//
void DYLDCacheRep::Writer::addDiscretionary(CodeDirectory::Builder &builder)
{
	unsigned count = rep->mCache.mappingCount();
	builder.scatter(count);
	for (unsigned n = 0; n < count; n++) {
		const DYLDCache::Mapping dmap = rep->mCache.mapping(n);
		CodeDirectory::Scatter *scatter = builder.scatter() + n;
		scatter->targetOffset = dmap.address();
		scatter->base = dmap.offset() / segmentedPageSize;
		assert(dmap.offset() % segmentedPageSize == 0);
		scatter->count = dmap.size() / segmentedPageSize;
		assert(dmap.size() % segmentedPageSize == 0);
	}
}


} // end namespace CodeSigning
} // end namespace Security
