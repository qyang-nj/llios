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
// signerutils - utilities for signature generation
//
#ifndef _H_SIGNERUTILS
#define _H_SIGNERUTILS

#include "CodeSigner.h"
#include "sigblob.h"
#include "cdbuilder.h"
#include <security_utilities/utilities.h>
#include <security_utilities/blob.h>
#include <security_utilities/unix++.h>
#include <security_utilities/unixchild.h>

namespace Security {
namespace CodeSigning {


//
// A helper to deal with the magic merger logic of internal requirements
//
class InternalRequirements : public Requirements::Maker {
public:
	InternalRequirements() : mReqs(NULL) { }
	~InternalRequirements() { ::free((void *)mReqs); }
	void operator () (const Requirements *given, const Requirements *defaulted, const Requirement::Context &context);
	operator const Requirements * () const { return mReqs; }

private:
	const Requirements *mReqs;
};


//
// A DiskRep::Writer that assembles data in a SuperBlob (in memory)
//
class BlobWriter : public DiskRep::Writer, public EmbeddedSignatureBlob::Maker {
public:	
	void component(CodeDirectory::SpecialSlot slot, CFDataRef data);
};


class DetachedBlobWriter : public BlobWriter {
public:
	DetachedBlobWriter(SecCodeSigner::Signer &s) : signer(s) { }

	SecCodeSigner::Signer &signer;
	
	void flush();
};


//
// A multi-architecture editing assistant.
// ArchEditor collects (Mach-O) architectures in use, and maintains per-archtitecture
// data structures. It must be subclassed to express a particular way to handle the signing
// data.
//
class ArchEditor : public DiskRep::Writer {
public:
	ArchEditor(Universal &fat, CodeDirectory::HashAlgorithm hashType, uint32_t attrs);
	virtual ~ArchEditor();

public:
	//
	// One architecture's signing construction element.
	// This also implements DispRep::Writer so generic writing code
	// can work with both Mach-O and other files.
	//
	struct Arch : public BlobWriter {
		Architecture architecture;		// our architecture
		auto_ptr<MachO> source;			// Mach-O object to be signed
		CodeDirectory::Builder cdbuilder; // builder for CodeDirectory
		InternalRequirements ireqs;		// consolidated internal requirements
		size_t blobSize;				// calculated SuperBlob size
		
		Arch(const Architecture &arch, CodeDirectory::HashAlgorithm hashType)
			: architecture(arch), cdbuilder(hashType) { }
	};

	//
	// Our callers access the architectural universe through a map
	// from Architectures to Arch objects.
	//
	typedef std::map<Architecture, Arch *> ArchMap;
	typedef ArchMap::iterator Iterator;
	ArchMap::iterator begin()		{ return architecture.begin(); }
	ArchMap::iterator end()			{ return architecture.end(); }
	unsigned count() const			{ return architecture.size(); }
	
	// methods needed for an actual implementation
	virtual void allocate() = 0;			// interpass allocations
	virtual void reset(Arch &arch) = 0;		// pass 2 prep
	virtual void write(Arch &arch, EmbeddedSignatureBlob *blob) = 0; // takes ownership of blob
	virtual void commit() = 0;				// write/flush result
	
protected:
	ArchMap architecture;
};


//
// An ArchEditor that collects all architectures into a single SuperBlob,
// usually for writing a detached multi-architecture signature.
//
class BlobEditor : public ArchEditor {
public:
	BlobEditor(Universal &fat, SecCodeSigner::Signer &s);
	
	SecCodeSigner::Signer &signer;
	
	void component(CodeDirectory::SpecialSlot slot, CFDataRef data);
	void allocate() { }
	void reset(Arch &arch) { }
	void write(Arch &arch, EmbeddedSignatureBlob *blob);
	void commit();
	
private:
	DetachedSignatureBlob::Maker mMaker;
	EmbeddedSignatureBlob::Maker mGlobal;
};


//
// An ArchEditor that writes its signatures into a (fat) binary file.
// We do this by forking a helper tool (codesign_allocate) and asking
// it to make a copy with suitable space "opened up" in the right spots.
//
class MachOEditor : public ArchEditor, private UnixPlusPlus::Child {
public:
	MachOEditor(DiskRep::Writer *w, Universal &code, CodeDirectory::HashAlgorithm hashType, std::string srcPath);
	~MachOEditor();

	const RefPointer<DiskRep::Writer> writer;
	const std::string sourcePath;
	const std::string tempPath;
	
	void component(CodeDirectory::SpecialSlot slot, CFDataRef data);
	void allocate();
	void reset(Arch &arch);
	void write(Arch &arch, EmbeddedSignatureBlob *blob);
	void commit();
	
private:
	// fork operation
	void childAction();
	void parentAction();
	
	// controlling the temporary file copy
	Universal *mNewCode;
	UnixPlusPlus::AutoFileDesc mFd;
	bool mTempMayExist;
	
	// finding and managing the helper tool
	const char *mHelperPath;
	bool mHelperOverridden;
};


//
// A Requirement::Context populated from a signing request.
// We use this to help generate the explicit Designated Requirement
// during signing ops, and thus this must be constructed BEFORE we
// actually have a signed object.
//
class PreSigningContext : public Requirement::Context {
public:
	PreSigningContext(const SecCodeSigner::Signer &signer);

private:
	CFRef<CFArrayRef> mCerts;		// hold cert chain
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_SIGNERUTILS
