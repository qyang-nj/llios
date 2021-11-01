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
#ifndef _H_SLCREP
#define _H_SLCREP

#include "singlediskrep.h"
#include "sigblob.h"
#include <security_utilities/unix++.h>
#include <security_utilities/macho++.h>
#include <security_utilities/dyldcache.h>

namespace Security {
namespace CodeSigning {


//
// DYLDCacheRep implements the on-disk format for the Mac OS X
// Shared Library Cache, which coalesces a set of system libraries
// and frameworks into one big (mappable) code blob in the sky.
//
class DYLDCacheRep : public SingleDiskRep {
public:
	DYLDCacheRep(const Context *ctx = NULL);
	DYLDCacheRep(const char *path);
	
	CFDataRef component(CodeDirectory::SpecialSlot slot);
	size_t pageSize(const SigningContext &ctx);
	std::string format();
	
	static bool candidate(UnixPlusPlus::FileDesc &fd);
	
public:
	static CFDataRef identificationFor(MachO *macho);
	
public:
	DiskRep::Writer *writer();
	class Writer;
	friend class Writer;

private:
	void setup();

private:
	DYLDCache mCache;
	const EmbeddedSignatureBlob *mSigningData;	// pointer to signature SuperBlob (in mapped memory)
};


//
// The write side of a FileDiskRep
//
class DYLDCacheRep::Writer : public SingleDiskRep::Writer, private EmbeddedSignatureBlob::Maker {
	friend class FileDiskRep;
public:
	Writer(DYLDCacheRep *r) : SingleDiskRep::Writer(r, writerNoGlobal), rep(r), mSigningData(NULL) { }
	void component(CodeDirectory::SpecialSlot slot, CFDataRef data);
	void flush();
	void addDiscretionary(CodeDirectory::Builder &builder);
	
private:
	DYLDCacheRep *rep;
	EmbeddedSignatureBlob *mSigningData;
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_SLCREP
