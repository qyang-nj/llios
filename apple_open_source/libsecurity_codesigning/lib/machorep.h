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
// machorep - DiskRep mix-in for handling Mach-O main executables
//
#ifndef _H_MACHOREP
#define _H_MACHOREP

#include "singlediskrep.h"
#include "sigblob.h"
#include <security_utilities/unix++.h>
#include <security_utilities/macho++.h>

namespace Security {
namespace CodeSigning {


//
// MachORep is a DiskRep class that supports code signatures
// directly embedded in Mach-O binary files.
//
// It does not have write support (for writing signatures);
// writing multi-architecture binaries is complicated enough
// that it's driven directly from the signing code, with no
// abstractions to get in the way.
//
class MachORep : public SingleDiskRep {
public:
	MachORep(const char *path, const Context *ctx = NULL);
	virtual ~MachORep();
	
	CFDataRef component(CodeDirectory::SpecialSlot slot);
	CFDataRef identification();
	Universal *mainExecutableImage();
	size_t signingBase();
	std::string format();
	
	std::string recommendedIdentifier(const SigningContext &ctx);
	const Requirements *defaultRequirements(const Architecture *arch, const SigningContext &ctx);
	size_t pageSize(const SigningContext &ctx);
	
	void flush();		// flush cache
	
	static bool candidate(UnixPlusPlus::FileDesc &fd);
	
public:
	static CFDataRef identificationFor(MachO *macho);
	
public:
	DiskRep::Writer *writer();
	class Writer;
	friend class Writer;
	
protected:
	CFDataRef embeddedComponent(CodeDirectory::SpecialSlot slot);
	CFDataRef infoPlist();
	Requirement *libraryRequirements(const Architecture *arch, const SigningContext &ctx);

private:
	Universal *mExecutable;	// cached Mach-O/Universal reference to mainExecutablePath()
	EmbeddedSignatureBlob *mSigningData; // cached signing data from current architecture
};


//
// The write side of a MachORep.
// This is purposely dysfunctional; Mach-O signatures are written
// by code in signerutils, not by DiskRep::Writers.
//
class MachORep::Writer : public SingleDiskRep::Writer {
	friend class FileDiskRep;
public:
	Writer(MachORep *r) : SingleDiskRep::Writer(r, writerNoGlobal) { }
	void component(CodeDirectory::SpecialSlot slot, CFDataRef data);
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_MACHOREP
