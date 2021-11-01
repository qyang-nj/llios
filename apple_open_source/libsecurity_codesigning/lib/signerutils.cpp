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
#include "signerutils.h"
#include "signer.h"
#include "SecCodeSigner.h"
#include <Security/SecIdentity.h>
#include <Security/CMSEncoder.h>
#include "renum.h"
#include "csutilities.h"
#include "drmaker.h"
#include <security_utilities/unix++.h>
#include <security_utilities/unixchild.h>
#include <vector>

// for helper validation
#include "Code.h"
#include <security_utilities/cfmunge.h>
#include <sys/codesign.h>


namespace Security {
namespace CodeSigning {


//
// About the Mach-O allocation helper
//
static const char helperName[] = "codesign_allocate";
static const char helperPath[] = "/usr/bin/codesign_allocate";
static const char helperOverride[] = "CODESIGN_ALLOCATE";
static const size_t csAlign = 16;


//
// BlobWriters
//
void BlobWriter::component(CodeDirectory::SpecialSlot slot, CFDataRef data)
{
	return EmbeddedSignatureBlob::Maker::component(slot, data);
}


void DetachedBlobWriter::flush()
{
	EmbeddedSignatureBlob *blob = this->make();
	signer.code->detachedSignature(makeCFData(*blob));
	signer.state.returnDetachedSignature(blob, signer);
	::free(blob);
}


//
// ArchEditor
//
ArchEditor::ArchEditor(Universal &code, CodeDirectory::HashAlgorithm hashType, uint32_t attrs)
	: DiskRep::Writer(attrs)
{
	Universal::Architectures archList;
	code.architectures(archList);
	for (Universal::Architectures::const_iterator it = archList.begin();
			it != archList.end(); ++it)
		architecture[*it] = new Arch(*it, hashType);
}


ArchEditor::~ArchEditor()
{
	for (ArchMap::iterator it = begin(); it != end(); ++it)
		delete it->second;
}


//
// BlobEditor
//
BlobEditor::BlobEditor(Universal &fat, SecCodeSigner::Signer &s)
	: ArchEditor(fat, s.digestAlgorithm(), 0), signer(s)
{ }


void BlobEditor::component(CodeDirectory::SpecialSlot slot, CFDataRef data)
{
	mGlobal.component(slot, data);
}

void BlobEditor::write(Arch &arch, EmbeddedSignatureBlob *blob)
{
	mMaker.add(arch.architecture.cpuType(), blob);
}


void BlobEditor::commit()
{
	// create the architecture-global blob and store it into the superblob
	mMaker.add(0, mGlobal.make());	// takes ownership of blob

	// finish up the superblob and deliver it
	DetachedSignatureBlob *blob = mMaker.make();
	signer.state.returnDetachedSignature(blob, signer);
	::free(blob);
}


//
// MachOEditor's allocate() method spawns the codesign_allocate helper tool to
// "drill up" the Mach-O binary for insertion of Code Signing signature data.
// After the tool succeeds, we open the new file and are ready to write it.
//
MachOEditor::MachOEditor(DiskRep::Writer *w, Universal &code, CodeDirectory::HashAlgorithm hashType, std::string srcPath)
	: ArchEditor(code, hashType, w->attributes()),
	  writer(w),
	  sourcePath(srcPath),
	  tempPath(srcPath + ".cstemp"),
	  mNewCode(NULL),
	  mTempMayExist(false)
{
	if (const char *path = getenv(helperOverride)) {
		mHelperPath = path;
		mHelperOverridden = true;
	} else {
		mHelperPath = helperPath;
		mHelperOverridden = false;
	}
}

MachOEditor::~MachOEditor()
{
	delete mNewCode;
	if (mTempMayExist)
		::remove(tempPath.c_str());		// ignore error (can't do anything about it)
	this->kill();
}


void MachOEditor::component(CodeDirectory::SpecialSlot slot, CFDataRef data)
{
	writer->component(slot, data);
}


void MachOEditor::allocate()
{
	// note that we may have a temporary file from now on (for cleanup in the error case)
	mTempMayExist = true;

	// run codesign_allocate to make room in the executable file
	fork();
	wait();
	if (!Child::succeeded())
		UnixError::throwMe(ENOEXEC);	//@@@ how to signal "it din' work"?
	
	// open the new (temporary) Universal file
	{
		UidGuard guard(0);
		mFd.open(tempPath, O_RDWR);
	}
	mNewCode = new Universal(mFd);
}

static const unsigned char appleReq[] = {
	// anchor apple and info["Application-Group"] = "com.apple.tool.codesign_allocate"
	0xfa, 0xde, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06,
	0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x11, 0x41, 0x70, 0x70, 0x6c,
	0x69, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x2d, 0x47, 0x72, 0x6f, 0x75, 0x70, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x20, 0x63, 0x6f, 0x6d, 0x2e, 0x61, 0x70, 0x70, 0x6c,
	0x65, 0x2e, 0x74, 0x6f, 0x6f, 0x6c, 0x2e, 0x63, 0x6f, 0x64, 0x65, 0x73, 0x69, 0x67, 0x6e, 0x5f,
	0x61, 0x6c, 0x6c, 0x6f, 0x63, 0x61, 0x74, 0x65,
};

void MachOEditor::parentAction()
{
	if (mHelperOverridden) {
		CODESIGN_ALLOCATE_VALIDATE((char*)mHelperPath, this->pid());
		// check code identity of an overridden allocation helper
		SecPointer<SecStaticCode> code = new SecStaticCode(DiskRep::bestGuess(mHelperPath));
		code->validateDirectory();
		code->validateExecutable();
		code->validateResources();
		code->validateRequirement((const Requirement *)appleReq, errSecCSReqFailed);
	}
}

void MachOEditor::childAction()
{
	vector<const char *> arguments;
	arguments.push_back(helperName);
	arguments.push_back("-i");
	arguments.push_back(sourcePath.c_str());
	arguments.push_back("-o");
	arguments.push_back(tempPath.c_str());
	
	for (Iterator it = architecture.begin(); it != architecture.end(); ++it) {
		size_t size = LowLevelMemoryUtilities::alignUp(it->second->blobSize, csAlign);
		char *ssize;			// we'll leak this (execv is coming soon)
		asprintf(&ssize, "%zd", size);

		if (const char *arch = it->first.name()) {
			CODESIGN_ALLOCATE_ARCH((char*)arch, size);
			arguments.push_back("-a");
			arguments.push_back(arch);
		} else {
			CODESIGN_ALLOCATE_ARCHN(it->first.cpuType(), it->first.cpuSubtype(), size);
			arguments.push_back("-A");
			char *anum;
			asprintf(&anum, "%d", it->first.cpuType());
			arguments.push_back(anum);
			asprintf(&anum, "%d", it->first.cpuSubtype());
			arguments.push_back(anum);
		}
		arguments.push_back(ssize);
	}
	arguments.push_back(NULL);
	
	if (mHelperOverridden)
		::csops(0, CS_EXEC_SET_KILL, NULL, 0);		// force code integrity
	::seteuid(0);	// activate privilege if caller has it; ignore error if not
	execv(mHelperPath, (char * const *)&arguments[0]);
}

void MachOEditor::reset(Arch &arch)
{
	arch.source.reset(mNewCode->architecture(arch.architecture));
	arch.cdbuilder.reopen(tempPath,
		arch.source->offset(), arch.source->signingOffset());
}


//
// MachOEditor's write() method actually writes the blob into the CODESIGNING section
// of the executable image file.
//
void MachOEditor::write(Arch &arch, EmbeddedSignatureBlob *blob)
{
	if (size_t offset = arch.source->signingOffset()) {
		size_t signingLength = arch.source->signingLength();
		CODESIGN_ALLOCATE_WRITE((char*)arch.architecture.name(), offset, blob->length(), signingLength);
		if (signingLength < blob->length())
			MacOSError::throwMe(errSecCSCMSTooLarge);
		arch.source->seek(offset);
		arch.source->writeAll(*blob);
		::free(blob);		// done with it
	} else {
		secdebug("signer", "%p cannot find CODESIGNING section", this);
		MacOSError::throwMe(errSecCSInternalError);
	}
}


//
// Commit the edit.
// This moves the temporary editor copy over the source image file.
// Note that the Universal object returned by allocate() is still open
// and valid; the caller owns it.
//
void MachOEditor::commit()
{
	// if the file's owned by someone else *and* we can become root...
	struct stat st;
	UnixError::check(::stat(sourcePath.c_str(), &st));

	// copy over all the *other* stuff
	Copyfile copy;
	int fd = mFd;
	copy.set(COPYFILE_STATE_DST_FD, &fd);
	{
		// perform copy under root or file-owner privileges if available
		UidGuard guard;
		if (!guard.seteuid(0))
			guard.seteuid(st.st_uid);
		
		// copy metadata from original file...
		copy(sourcePath.c_str(), NULL, COPYFILE_SECURITY | COPYFILE_METADATA);
		
		// ... but explicitly update the timestamps since we did change the file
		char buf;
		mFd.read(&buf, sizeof(buf), 0);
		mFd.write(&buf, sizeof(buf), 0);

		// move the new file into place
		UnixError::check(::rename(tempPath.c_str(), sourcePath.c_str()));
		mTempMayExist = false;		// we renamed it away
	}
}


//
// InternalRequirements
//
void InternalRequirements::operator () (const Requirements *given, const Requirements *defaulted, const Requirement::Context &context)
{
	// first add the default internal requirements
	if (defaulted) {
		this->add(defaulted);
		::free((void *)defaulted);		// was malloc(3)ed by DiskRep
	}
	
	// now override them with any requirements explicitly given by the signer
	if (given)
		this->add(given);

	// now add the Designated Requirement, if we can make it and it's not been provided
	if (!this->contains(kSecDesignatedRequirementType)) {
		DRMaker maker(context);
		if (Requirement *dr = maker.make()) {
			this->add(kSecDesignatedRequirementType, dr);		// takes ownership of dr
		}
	}
	
	// return the result
	mReqs = this->make();
}


//
// Pre-Signing contexts
//
PreSigningContext::PreSigningContext(const SecCodeSigner::Signer &signer)
{
	// construct a cert chain
	if (signer.signingIdentity() != SecIdentityRef(kCFNull)) {
		CFRef<SecCertificateRef> signingCert;
		MacOSError::check(SecIdentityCopyCertificate(signer.signingIdentity(), &signingCert.aref()));
		CFRef<SecPolicyRef> policy = SecPolicyCreateWithOID(kSecPolicyAppleCodeSigning);
		CFRef<SecTrustRef> trust;
		MacOSError::check(SecTrustCreateWithCertificates(CFArrayRef(signingCert.get()), policy, &trust.aref()));
		SecTrustResultType result;
		MacOSError::check(SecTrustEvaluate(trust, &result));
		CSSM_TP_APPLE_EVIDENCE_INFO *info;
		MacOSError::check(SecTrustGetResult(trust, &result, &mCerts.aref(), &info));
		this->certs = mCerts;
	}
	
	// other stuff
	this->identifier = signer.signingIdentifier();
}


} // end namespace CodeSigning
} // end namespace Security
