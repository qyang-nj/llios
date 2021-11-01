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
#include "machorep.h"
#include "StaticCode.h"
#include "reqmaker.h"


namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


//
// Object management.
// We open the main executable lazily, so nothing much happens on construction.
// If the context specifies a file offset, we directly pick that Mach-O binary (only).
// if it specifies an architecture, we try to pick that. Otherwise, we deliver the whole
// Universal object (which will usually deliver the "native" architecture later).
//
MachORep::MachORep(const char *path, const Context *ctx)
	: SingleDiskRep(path), mSigningData(NULL)
{
	if (ctx)
		if (ctx->offset)
			mExecutable = new Universal(fd(), ctx->offset);
		else if (ctx->arch) {
			auto_ptr<Universal> full(new Universal(fd()));
			mExecutable = new Universal(fd(), full->archOffset(ctx->arch));
		} else
			mExecutable = new Universal(fd());
	else
		mExecutable = new Universal(fd());
	assert(mExecutable);
	CODESIGN_DISKREP_CREATE_MACHO(this, (char*)path, (void*)ctx);
}

MachORep::~MachORep()
{
	delete mExecutable;
	::free(mSigningData);
}


//
// Sniffer function for "plausible Mach-O binary"
//
bool MachORep::candidate(FileDesc &fd)
{
	switch (Universal::typeOf(fd)) {
	case MH_EXECUTE:
	case MH_DYLIB:
	case MH_DYLINKER:
	case MH_BUNDLE:
	case MH_PRELOAD:
		return true;		// dynamic image; supported
	case MH_OBJECT:
		return false;		// maybe later...
	default:
		return false;		// not Mach-O (or too exotic)
	}
}



//
// Nowadays, the main executable object is created upon construction.
//
Universal *MachORep::mainExecutableImage()
{
	return mExecutable;
}


//
// Signing base is the start of the Mach-O architecture we're using
//
size_t MachORep::signingBase()
{
	return mainExecutableImage()->archOffset();
}


//
// We choose the binary identifier for a Mach-O binary as follows:
//	- If the Mach-O headers have a UUID command, use the UUID.
//	- Otherwise, use the SHA-1 hash of the (entire) load commands.
//
CFDataRef MachORep::identification()
{
	std::auto_ptr<MachO> macho(mainExecutableImage()->architecture());
	return identificationFor(macho.get());
}

CFDataRef MachORep::identificationFor(MachO *macho)
{
	// if there is a LC_UUID load command, use the UUID contained therein
	if (const load_command *cmd = macho->findCommand(LC_UUID)) {
		const uuid_command *uuidc = reinterpret_cast<const uuid_command *>(cmd);
		char result[4 + sizeof(uuidc->uuid)];
		memcpy(result, "UUID", 4);
		memcpy(result+4, uuidc->uuid, sizeof(uuidc->uuid));
		return makeCFData(result, sizeof(result));
	}
	
	// otherwise, use the SHA-1 hash of the entire load command area
	SHA1 hash;
	hash(&macho->header(), sizeof(mach_header));
	hash(macho->loadCommands(), macho->commandLength());
	SHA1::Digest digest;
	hash.finish(digest);
	return makeCFData(digest, sizeof(digest));
}


//
// Retrieve a component from the executable.
// This reads the entire signing SuperBlob when first called for an executable,
// and then caches it for further use.
// Note that we could read individual components directly off disk and only cache
// the SuperBlob Index directory. Our caller (usually SecStaticCode) is expected
// to cache the pieces anyway.
//
CFDataRef MachORep::component(CodeDirectory::SpecialSlot slot)
{
	switch (slot) {
	case cdInfoSlot:
		return infoPlist();
	default:
		return embeddedComponent(slot);
	}
}


// Retrieve a component from the embedded signature SuperBlob (if present).
// This reads the entire signing SuperBlob when first called for an executable,
// and then caches it for further use.
// Note that we could read individual components directly off disk and only cache
// the SuperBlob Index directory. Our caller (usually SecStaticCode) is expected
// to cache the pieces anyway. But it's not clear that the resulting multiple I/O
// calls wouldn't be slower in the end.
//
CFDataRef MachORep::embeddedComponent(CodeDirectory::SpecialSlot slot)
{
	if (!mSigningData) {		// fetch and cache
		auto_ptr<MachO> macho(mainExecutableImage()->architecture());
		if (macho.get())
			if (const linkedit_data_command *cs = macho->findCodeSignature()) {
				size_t offset = macho->flip(cs->dataoff);
				size_t length = macho->flip(cs->datasize);
				if ((mSigningData = EmbeddedSignatureBlob::readBlob(macho->fd(), macho->offset() + offset, length))) {
					secdebug("machorep", "%zd signing bytes in %d blob(s) from %s(%s)",
						mSigningData->length(), mSigningData->count(),
						mainExecutablePath().c_str(), macho->architecture().name());
				} else {
					secdebug("machorep", "failed to read signing bytes from %s(%s)",
						mainExecutablePath().c_str(), macho->architecture().name());
					MacOSError::throwMe(errSecCSSignatureInvalid);
				}
			}
	}
	if (mSigningData)
		return mSigningData->component(slot);
	
	// not found
	return NULL;
}


//
// Extract an embedded Info.plist from the file.
// Returns NULL if none is found.
//
CFDataRef MachORep::infoPlist()
{
	CFRef<CFDataRef> info;
	try {
		auto_ptr<MachO> macho(mainExecutableImage()->architecture());
		if (const section *sect = macho->findSection("__TEXT", "__info_plist")) {
			if (macho->is64()) {
				const section_64 *sect64 = reinterpret_cast<const section_64 *>(sect);
				info.take(macho->dataAt(macho->flip(sect64->offset), macho->flip(sect64->size)));
			} else {
				info.take(macho->dataAt(macho->flip(sect->offset), macho->flip(sect->size)));
			}
		}
	} catch (...) {
		secdebug("machorep", "exception reading embedded Info.plist");
	}
	return info.yield();
}


//
// Provide a (vaguely) human readable characterization of this code
//
string MachORep::format()
{
	if (Universal *fat = mainExecutableImage()) {
		Universal::Architectures archs;
		fat->architectures(archs);
		if (fat->isUniversal()) {
			string s = "Mach-O universal (";
			for (Universal::Architectures::const_iterator it = archs.begin();
					it != archs.end(); ++it) {
				if (it != archs.begin())
					s += " ";
				s += it->displayName();
			}
			return s + ")";
		} else {
			assert(archs.size() == 1);
			return string("Mach-O thin (") + archs.begin()->displayName() + ")";
		}
	} else
		return "Mach-O (unrecognized format)";
}


//
// Flush cached data
//
void MachORep::flush()
{
	delete mExecutable;
	mExecutable = NULL;
	::free(mSigningData);
	mSigningData = NULL;
	SingleDiskRep::flush();
	mExecutable = new Universal(fd());
}


//
// Return a recommended unique identifier.
// If our file has an embedded Info.plist, use the CFBundleIdentifier from that.
// Otherwise, use the default.
//
string MachORep::recommendedIdentifier(const SigningContext &ctx)
{
	if (CFDataRef info = infoPlist()) {
		if (CFRef<CFDictionaryRef> dict = makeCFDictionaryFrom(info)) {
			CFStringRef code = CFStringRef(CFDictionaryGetValue(dict, kCFBundleIdentifierKey));
			if (code && CFGetTypeID(code) != CFStringGetTypeID())
				MacOSError::throwMe(errSecCSBadDictionaryFormat);
			if (code)
				return cfString(code);
		} else
			MacOSError::throwMe(errSecCSBadDictionaryFormat);
	}
	
	// ah well. Use the default
	return SingleDiskRep::recommendedIdentifier(ctx);
}


//
// The default suggested requirements for Mach-O binaries are as follows:
// Library requirement: Composed from dynamic load commands.
//
const Requirements *MachORep::defaultRequirements(const Architecture *arch, const SigningContext &ctx)
{
	assert(arch);		// enforced by signing infrastructure
	Requirements::Maker maker;
		
	// add library requirements from DYLIB commands (if any)
	if (Requirement *libreq = libraryRequirements(arch, ctx))
		maker.add(kSecLibraryRequirementType, libreq);	// takes ownership

	// that's all
	return maker.make();
}

Requirement *MachORep::libraryRequirements(const Architecture *arch, const SigningContext &ctx)
{
	auto_ptr<MachO> macho(mainExecutableImage()->architecture(*arch));
	Requirement::Maker maker;
	Requirement::Maker::Chain chain(maker, opOr);

	if (macho.get())
		if (const linkedit_data_command *ldep = macho->findLibraryDependencies()) {
			size_t offset = macho->flip(ldep->dataoff);
			size_t length = macho->flip(ldep->datasize);
			if (LibraryDependencyBlob *deplist = LibraryDependencyBlob::readBlob(macho->fd(), macho->offset() + offset, length)) {
				try {
					secdebug("machorep", "%zd library dependency bytes in %d blob(s) from %s(%s)",
						deplist->length(), deplist->count(),
						mainExecutablePath().c_str(), macho->architecture().name());
					unsigned count = deplist->count();
					// we could walk through DYLIB load commands in parallel. We just don't need anything from them so far
					for (unsigned n = 0; n < count; n++) {
						const Requirement *req = NULL;
						if (const BlobCore *dep = deplist->blob(n)) {
							if ((req = Requirement::specific(dep))) {
								// binary code requirement; good to go
							} else if (const BlobWrapper *wrap = BlobWrapper::specific(dep)) {
								// blob-wrapped text form - convert to binary requirement
								std::string reqString = std::string((const char *)wrap->data(), wrap->length());
								CFRef<SecRequirementRef> areq;
								MacOSError::check(SecRequirementCreateWithString(CFTempString(reqString), kSecCSDefaultFlags, &areq.aref()));
								CFRef<CFDataRef> reqData;
								MacOSError::check(SecRequirementCopyData(areq, kSecCSDefaultFlags, &reqData.aref()));
								req = Requirement::specific((const BlobCore *)CFDataGetBytePtr(reqData));
							} else {
								secdebug("machorep", "unexpected blob type 0x%x in slot %d of binary dependencies", dep->magic(), n);
								continue;
							}
							chain.add();
							maker.copy(req);
						} else
							secdebug("machorep", "missing DR info for library index %d", n);
					}
					::free(deplist);
				} catch (...) {
					::free(deplist);
					throw;
				}
			}
		}
	if (chain.empty())
		return NULL;
	else
		return maker.make();
}


//
// Default to system page size for segmented (paged) signatures
//
size_t MachORep::pageSize(const SigningContext &)
{
	return segmentedPageSize;
}


//
// FileDiskRep::Writers
//
DiskRep::Writer *MachORep::writer()
{
	return new Writer(this);
}


//
// Write a component.
// MachORep::Writers don't write to components directly; the signing code uses special
// knowledge of the Mach-O format to build embedded signatures and blasts them directly
// to disk. Thus this implementation will never be called (and, if called, will simply fail).
//
void MachORep::Writer::component(CodeDirectory::SpecialSlot slot, CFDataRef data)
{
	assert(false);
	MacOSError::throwMe(errSecCSInternalError);
}


} // end namespace CodeSigning
} // end namespace Security
