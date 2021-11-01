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
// singlediskrep - semi-abstract diskrep for a single file of some kind
//
#include "singlediskrep.h"
#include "csutilities.h"
#include <security_utilities/cfutilities.h>

namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


//
// Construct a SingleDiskRep
//
SingleDiskRep::SingleDiskRep(const std::string &path)
	: mPath(path)
{
}


//
// The default binary identification of a SingleDiskRep is the (SHA-1) hash
// of the entire file itself.
//
CFDataRef SingleDiskRep::identification()
{
	SHA1 hash;
	this->fd().seek(0);
	hashFileData(this->fd(), &hash);
	SHA1::Digest digest;
	hash.finish(digest);
	return makeCFData(digest, sizeof(digest));
}


//
// Both the canonical and main executable path of a SingleDiskRep is, well, its path.
//
CFURLRef SingleDiskRep::canonicalPath()
{
	return makeCFURL(mPath);
}

string SingleDiskRep::mainExecutablePath()
{
	return mPath;
}


//
// The default signing limit is the size of the file.
// This will do unless the signing data gets creatively stuck in there somewhere.
//
size_t SingleDiskRep::signingLimit()
{
	return fd().fileSize();
}


//
// A lazily opened read-only file descriptor for the path.
//
FileDesc &SingleDiskRep::fd()
{
	if (!mFd)
		mFd.open(mPath, O_RDONLY);
	return mFd;
}


//
// Flush cached state
//
void SingleDiskRep::flush()
{
	mFd.close();
}


//
// The recommended identifier of a SingleDiskRep is, absent any better clue,
// the basename of its path.
//
string SingleDiskRep::recommendedIdentifier(const SigningContext &)
{
	return canonicalIdentifier(mPath);
}


//
// Prototype Writers
//
FileDesc &SingleDiskRep::Writer::fd()
{
	if (!mFd)
		mFd.open(rep->path(), O_RDWR);
	return mFd;
}


} // end namespace CodeSigning
} // end namespace Security
