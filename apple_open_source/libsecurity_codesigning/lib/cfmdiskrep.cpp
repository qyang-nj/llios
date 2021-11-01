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
// cfmdiskrep - single-file CFM (PEF) executable disk representation
//
#include "cfmdiskrep.h"
#include <cstring>


namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


//
// Everything's lazy in here
//
CFMDiskRep::CFMDiskRep(const char *path)
	: SingleDiskRep(path), mTriedRead(false)
{
	CODESIGN_DISKREP_CREATE_CFM(this, (char*)path);
}

CFMDiskRep::~CFMDiskRep()
{
	if (mTriedRead)
		delete mSigningData;
}


//
// CFM filter heuristic.
// We look for the PEF header within the first scanLength bytes
// of the file's data fork, at certain alignment boundaries (probably
// conservative).
//
bool CFMDiskRep::candidate(FileDesc &fd)
{
	static const char magicMarker[] = "Joy!peffpwpc";
	static const size_t magicLength = 12;
	static const size_t scanLength = 128;
	static const size_t scanAlignment = 4;
	
	char marker[scanLength];
	if (fd.read(marker, scanLength, 0) == scanLength)
		for (size_t p = 0; p <= scanLength - magicLength; p += scanAlignment)
			if (!memcmp(marker+p, magicMarker, magicLength))
				return true;
	return false;
}


//
// Extract and return a component by slot number.
// If we have a Mach-O binary, use embedded components.
// Otherwise, look for and return the extended attribute, if any.
//
CFDataRef CFMDiskRep::component(CodeDirectory::SpecialSlot slot)
{
	if (!mTriedRead)
		readSigningData();
	if (mSigningData)
		return mSigningData->component(slot);
	else
		return NULL;
}


//
// The signing limit is the start of the signature if present,
// or the end of the file otherwise.
//
size_t CFMDiskRep::signingLimit()
{
	readSigningData();
	if (mSigningData)
		return mSigningOffset;
	else
		return fd().fileSize();
}


//
// Various other aspects of our DiskRep personality.
//
string CFMDiskRep::format()
{
	return "CFM/PEF binary";
}


//
// Discard cached information
//
void CFMDiskRep::flush()
{
	mTriedRead = false;
	::free(mSigningData);
}


//
// In Mac OS X, a CFM binary must always be managed by the LaunchCFMApp
// system tool. Thus, we recommend that this be required as a host.
//
static const uint8_t cfm_ireqs[] = {	// host => anchor apple and identifier com.apple.LaunchCFMApp
	0xfa, 0xde, 0x0c, 0x01, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x14, 0xfa, 0xde, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x16,
	0x63, 0x6f, 0x6d, 0x2e, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x2e, 0x4c, 0x61, 0x75, 0x6e, 0x63, 0x68,
	0x43, 0x46, 0x4d, 0x41, 0x70, 0x70, 0x00, 0x00,
};

const Requirements *CFMDiskRep::defaultRequirements(const Architecture *, const SigningContext &)
{
	return ((const Requirements *)cfm_ireqs)->clone();	// need to pass ownership
}


//
// Default to system-paged signing
//
size_t CFMDiskRep::pageSize(const SigningContext &)
{
	return segmentedPageSize;
}


//
// Locate, read, and cache embedded signing data from the CFM binary.
//
void CFMDiskRep::readSigningData()
{
	if (!mTriedRead) {				// try it once
		mSigningData = NULL;		// preset failure
		mTriedRead = true;			// we've tried (and perhaps failed)
		
		FileDesc &fd = this->fd();
		fd.seek(fd.fileSize() - sizeof(Sentinel));
		Sentinel sentinel;
		if (fd.read(&sentinel, sizeof(sentinel), fd.fileSize() - sizeof(Sentinel)) == sizeof(Sentinel))
			if (sentinel.magic == EmbeddedSignatureBlob::typeMagic) {
				mSigningOffset = sentinel.offset;
				if ((mSigningData = EmbeddedSignatureBlob::readBlob(fd, mSigningOffset)))
					secdebug("cfmrep", "%zd signing bytes in %d blob(s) from %s(CFM)",
						mSigningData->length(), mSigningData->count(),
						mainExecutablePath().c_str());
				else
					secdebug("cfmrep", "failed to read signing bytes from %s(CFM)",
						mainExecutablePath().c_str());
			}
	}
}


//
// CFMDiskRep::Writers
//
DiskRep::Writer *CFMDiskRep::writer()
{
	return new Writer(this);
}

CFMDiskRep::Writer::~Writer()
{
	delete mSigningData;
}


//
// Write a component.
// Note that this isn't concerned with Mach-O writing; this is handled at
// a much higher level. If we're called, it's extended attribute time.
//
void CFMDiskRep::Writer::component(CodeDirectory::SpecialSlot slot, CFDataRef data)
{
	EmbeddedSignatureBlob::Maker::component(slot, data);
}


//
// Append the superblob we built to the CFM binary.
// Note: Aligning the signing blob to a 16-byte boundary is not strictly necessary,
// but it's what the Mach-O case does, and it probably improves performance a bit.
//
void CFMDiskRep::Writer::flush()
{
	delete mSigningData;			// ditch previous blob just in case
	mSigningData = Maker::make();	// assemble new signature SuperBlob
	size_t start = LowLevelMemoryUtilities::alignUp(rep->signingLimit(), 16);
	Sentinel sentinel;
	sentinel.magic = EmbeddedSignatureBlob::typeMagic;
	sentinel.offset = start;
	AutoFileDesc fd(rep->path(), O_RDWR);
	fd.seek(start);
	fd.writeAll(mSigningData, mSigningData->length());
	fd.writeAll(&sentinel, sizeof(sentinel));
}


} // end namespace CodeSigning
} // end namespace Security
