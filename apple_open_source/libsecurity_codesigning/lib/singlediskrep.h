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
// singlediskrep - semi-abstract diskrep for a single file of some kind
//
#ifndef _H_SINGLEDISKREP
#define _H_SINGLEDISKREP

#include "diskrep.h"
#include <security_utilities/unix++.h>

namespace Security {
namespace CodeSigning {


//
// A slight specialization of DiskRep that knows that it's working with a single
// file at a path that is both the canonical and main executable path. This is a common
// pattern.
//
// A SingleDiskRep is not a fully formed DiskRep in its own right. It must be further
// subclassed.
//
class SingleDiskRep : public DiskRep {
public:
	SingleDiskRep(const std::string &path);

	CFDataRef identification();								// partial file hash
	std::string mainExecutablePath();						// base path
	CFURLRef canonicalPath();								// base path
	size_t signingLimit();									// size of file
	UnixPlusPlus::FileDesc &fd();							// readable fd for this file
	void flush();											// close cached fd
	
	std::string recommendedIdentifier(const SigningContext &ctx); // basename(path)
	
public:
	class Writer;
	
protected:
	std::string path() const { return mPath; }

private:
	std::string mPath;
	UnixPlusPlus::AutoFileDesc mFd;							// open file (cached)
};


//
// A Writer for a SingleDiskRep
//
class SingleDiskRep::Writer : public DiskRep::Writer {
public:
	Writer(SingleDiskRep *r, uint32_t attrs = 0) : DiskRep::Writer(attrs), rep(r) { }

	UnixPlusPlus::FileDesc &fd();

private:
	RefPointer<SingleDiskRep> rep;							// underlying SingleDiskRep
	UnixPlusPlus::AutoFileDesc mFd;							// cached writable fd
};



} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_SINGLEDISKREP
