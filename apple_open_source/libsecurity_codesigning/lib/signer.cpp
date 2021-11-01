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
// signer - Signing operation supervisor and controller
//
#include "signer.h"
#include "resources.h"
#include "signerutils.h"
#include "SecCodeSigner.h"
#include <Security/SecIdentity.h>
#include <Security/CMSEncoder.h>
#include <Security/CMSPrivate.h>
#include <Security/CSCommonPriv.h>
#include <CoreFoundation/CFBundlePriv.h>
#include "renum.h"
#include "machorep.h"
#include "csutilities.h"
#include <security_utilities/unix++.h>
#include <security_utilities/unixchild.h>
#include <security_utilities/cfmunge.h>

namespace Security {
namespace CodeSigning {


//
// Sign some code.
//
void SecCodeSigner::Signer::sign(SecCSFlags flags)
{
	rep = code->diskRep()->base();
	this->prepare(flags);
	PreSigningContext context(*this);
	if (Universal *fat = state.mNoMachO ? NULL : rep->mainExecutableImage()) {
		signMachO(fat, context);
	} else {
		signArchitectureAgnostic(context);
	}
}


//
// Remove any existing code signature from code
//
void SecCodeSigner::Signer::remove(SecCSFlags flags)
{
	// can't remove a detached signature
	if (state.mDetached)
		MacOSError::throwMe(errSecCSNotSupported);

	rep = code->diskRep();
	if (Universal *fat = state.mNoMachO ? NULL : rep->mainExecutableImage()) {
		// architecture-sensitive removal
		MachOEditor editor(rep->writer(), *fat, kSecCodeSignatureNoHash, rep->mainExecutablePath());
		editor.allocate();		// create copy
		editor.commit();		// commit change
	} else {
		// architecture-agnostic removal
		RefPointer<DiskRep::Writer> writer = rep->writer();
		writer->remove();
		writer->flush();
	}
}


//
// Contemplate the object-to-be-signed and set up the Signer state accordingly.
//
void SecCodeSigner::Signer::prepare(SecCSFlags flags)
{
	// get the Info.plist out of the rep for some creative defaulting
	CFRef<CFDictionaryRef> infoDict;
	if (CFRef<CFDataRef> infoData = rep->component(cdInfoSlot))
		infoDict.take(makeCFDictionaryFrom(infoData));

	// work out the canonical identifier
	identifier = state.mIdentifier;
	if (identifier.empty()) {
		identifier = rep->recommendedIdentifier(state);
		if (identifier.find('.') == string::npos)
			identifier = state.mIdentifierPrefix + identifier;
		if (identifier.find('.') == string::npos && state.isAdhoc())
			identifier = identifier + "-" + uniqueName();
		secdebug("signer", "using default identifier=%s", identifier.c_str());
	} else
		secdebug("signer", "using explicit identifier=%s", identifier.c_str());
	
	// work out the CodeDirectory flags word
	if (state.mCdFlagsGiven) {
		cdFlags = state.mCdFlags;
		secdebug("signer", "using explicit cdFlags=0x%x", cdFlags);
	} else {
		cdFlags = 0;
		if (infoDict)
			if (CFTypeRef csflags = CFDictionaryGetValue(infoDict, CFSTR("CSFlags"))) {
				if (CFGetTypeID(csflags) == CFNumberGetTypeID()) {
					cdFlags = cfNumber<uint32_t>(CFNumberRef(csflags));
					secdebug("signer", "using numeric cdFlags=0x%x from Info.plist", cdFlags);
				} else if (CFGetTypeID(csflags) == CFStringGetTypeID()) {
					cdFlags = cdTextFlags(cfString(CFStringRef(csflags)));
					secdebug("signer", "using text cdFlags=0x%x from Info.plist", cdFlags);
				} else
					MacOSError::throwMe(errSecCSBadDictionaryFormat);
			}
	}
	if (state.mSigner == SecIdentityRef(kCFNull))	// ad-hoc signing requested...
		cdFlags |= kSecCodeSignatureAdhoc;	// ... so note that
	
	// prepare the resource directory, if any
	string rpath = rep->resourcesRootPath();
	if (!rpath.empty()) {
		// explicitly given resource rules always win
		CFCopyRef<CFDictionaryRef> resourceRules = state.mResourceRules;
		
		// embedded resource rules come next
		if (!resourceRules && infoDict)
			if (CFTypeRef spec = CFDictionaryGetValue(infoDict, _kCFBundleResourceSpecificationKey)) {
				if (CFGetTypeID(spec) == CFStringGetTypeID())
					if (CFRef<CFDataRef> data = cfLoadFile(rpath + "/" + cfString(CFStringRef(spec))))
						if (CFDictionaryRef dict = makeCFDictionaryFrom(data))
							resourceRules.take(dict);
				if (!resourceRules)	// embedded rules present but unacceptable
					MacOSError::throwMe(errSecCSResourceRulesInvalid);
			}

		// finally, ask the DiskRep for its default
		if (!resourceRules)
			resourceRules.take(rep->defaultResourceRules(state));
		
		// build the resource directory
		ResourceBuilder resources(rpath, cfget<CFDictionaryRef>(resourceRules, "rules"), digestAlgorithm());
		rep->adjustResources(resources);	// DiskRep-specific adjustments
		CFRef<CFDictionaryRef> rdir = resources.build();
		resourceDirectory.take(CFPropertyListCreateXMLData(NULL, rdir));
	}
	
	// screen and set the signing time
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (state.mSigningTime == CFDateRef(kCFNull)) {
		signingTime = 0;		// no time at all
	} else if (!state.mSigningTime) {
		signingTime = now;		// default
	} else {
		CFAbsoluteTime time = CFDateGetAbsoluteTime(state.mSigningTime);
		if (time > now)	// not allowed to post-date a signature
			MacOSError::throwMe(errSecCSBadDictionaryFormat);
		signingTime = time;
	}
	
	pagesize = state.mPageSize ? cfNumber<size_t>(state.mPageSize) : rep->pageSize(state);
    
    // Timestamping setup
    CFRef<SecIdentityRef> mTSAuth;	// identity for client-side authentication to the Timestamp server
}


//
// Sign a Mach-O binary, using liberal dollops of that special Mach-O magic sauce.
// Note that this will deal just fine with non-fat Mach-O binaries, but it will
// treat them as architectural binaries containing (only) one architecture - that
// interpretation is courtesy of the Universal/MachO support classes.
//
void SecCodeSigner::Signer::signMachO(Universal *fat, const Requirement::Context &context)
{
	// Mach-O executable at the core - perform multi-architecture signing
	auto_ptr<ArchEditor> editor(state.mDetached
		? static_cast<ArchEditor *>(new BlobEditor(*fat, *this))
		: new MachOEditor(rep->writer(), *fat, this->digestAlgorithm(), rep->mainExecutablePath()));
	assert(editor->count() > 0);
	if (!editor->attribute(writerNoGlobal))	// can store architecture-common components
		populate(*editor);
	
	// pass 1: prepare signature blobs and calculate sizes
	for (MachOEditor::Iterator it = editor->begin(); it != editor->end(); ++it) {
		MachOEditor::Arch &arch = *it->second;
		arch.source.reset(fat->architecture(it->first));
		arch.ireqs(state.mRequirements, rep->defaultRequirements(&arch.architecture, state), context);
		if (editor->attribute(writerNoGlobal))	// can't store globally, add per-arch
			populate(arch);
		populate(arch.cdbuilder, arch, arch.ireqs,
			arch.source->offset(), arch.source->signingExtent());
	
		// add identification blob (made from this architecture) only if we're making a detached signature
		if (state.mDetached) {
			CFRef<CFDataRef> identification = MachORep::identificationFor(arch.source.get());
			arch.add(cdIdentificationSlot, BlobWrapper::alloc(
				CFDataGetBytePtr(identification), CFDataGetLength(identification)));
		}
		
		// prepare SuperBlob size estimate
		size_t cdSize = arch.cdbuilder.size();
		arch.blobSize = arch.size(cdSize, state.mCMSSize, 0);
	}
	
	editor->allocate();
	
	// pass 2: Finish and generate signatures, and write them
	for (MachOEditor::Iterator it = editor->begin(); it != editor->end(); ++it) {
		MachOEditor::Arch &arch = *it->second;
		editor->reset(arch);

		// finish CodeDirectory (off new binary) and sign it
		CodeDirectory *cd = arch.cdbuilder.build();
		CFRef<CFDataRef> signature = signCodeDirectory(cd);
		
		// complete the SuperBlob
		arch.add(cdCodeDirectorySlot, cd);	// takes ownership
		arch.add(cdSignatureSlot, BlobWrapper::alloc(
			CFDataGetBytePtr(signature), CFDataGetLength(signature)));
		if (!state.mDryRun) {
			EmbeddedSignatureBlob *blob = arch.make();
			editor->write(arch, blob);	// takes ownership of blob
		}
	}
	
	// done: write edit copy back over the original
	if (!state.mDryRun)
		editor->commit();
}


//
// Sign a binary that has no notion of architecture.
// That currently means anything that isn't Mach-O format.
//
void SecCodeSigner::Signer::signArchitectureAgnostic(const Requirement::Context &context)
{
	// non-Mach-O executable - single-instance signing
	RefPointer<DiskRep::Writer> writer = state.mDetached ?
		(new DetachedBlobWriter(*this)) : rep->writer();
	CodeDirectory::Builder builder(state.mDigestAlgorithm);
	InternalRequirements ireqs;
	ireqs(state.mRequirements, rep->defaultRequirements(NULL, state), context);
	populate(*writer);
	populate(builder, *writer, ireqs, rep->signingBase(), rep->signingLimit());
	
	// add identification blob (made from this architecture) only if we're making a detached signature
	if (state.mDetached) {
		CFRef<CFDataRef> identification = rep->identification();
		writer->component(cdIdentificationSlot, identification);
	}
	
	CodeDirectory *cd = builder.build();
	CFRef<CFDataRef> signature = signCodeDirectory(cd);
	if (!state.mDryRun) {
		writer->codeDirectory(cd);
		writer->signature(signature);
		writer->flush();
	}
	::free(cd);
}


//
// Global populate - send components to destination buffers ONCE
//
void SecCodeSigner::Signer::populate(DiskRep::Writer &writer)
{
	if (resourceDirectory)
		writer.component(cdResourceDirSlot, resourceDirectory);
}


//
// Per-architecture populate - send components to per-architecture buffers
// and populate the CodeDirectory for an architecture. In architecture-agnostic
// signing operations, the non-architectural binary is considered one (arbitrary) architecture
// for the purposes of this call.
//
void SecCodeSigner::Signer::populate(CodeDirectory::Builder &builder, DiskRep::Writer &writer,
	InternalRequirements &ireqs, size_t offset /* = 0 */, size_t length /* = 0 */)
{
	// fill the CodeDirectory
	builder.executable(rep->mainExecutablePath(), pagesize, offset, length);
	builder.flags(cdFlags);
	builder.identifier(identifier);
	
	if (CFRef<CFDataRef> data = rep->component(cdInfoSlot))
		builder.specialSlot(cdInfoSlot, data);
	if (ireqs) {
		CFRef<CFDataRef> data = makeCFData(*ireqs);
		writer.component(cdRequirementsSlot, data);
		builder.specialSlot(cdRequirementsSlot, data);
	}
	if (resourceDirectory)
		builder.specialSlot(cdResourceDirSlot, resourceDirectory);
#if NOT_YET
	if (state.mApplicationData)
		builder.specialSlot(cdApplicationSlot, state.mApplicationData);
#endif
	if (state.mEntitlementData) {
		writer.component(cdEntitlementSlot, state.mEntitlementData);
		builder.specialSlot(cdEntitlementSlot, state.mEntitlementData);
	}
	
	writer.addDiscretionary(builder);
}

#include <Security/tsaSupport.h>

//
// Generate the CMS signature for a (finished) CodeDirectory.
//
CFDataRef SecCodeSigner::Signer::signCodeDirectory(const CodeDirectory *cd)
{
	assert(state.mSigner);
	CFRef<CFMutableDictionaryRef> defaultTSContext = NULL;
    
	// a null signer generates a null signature blob
	if (state.mSigner == SecIdentityRef(kCFNull))
		return CFDataCreate(NULL, NULL, 0);
	
	// generate CMS signature
	CFRef<CMSEncoderRef> cms;
	MacOSError::check(CMSEncoderCreate(&cms.aref()));
	MacOSError::check(CMSEncoderSetCertificateChainMode(cms, kCMSCertificateChainWithRoot));
	CMSEncoderAddSigners(cms, state.mSigner);
	MacOSError::check(CMSEncoderSetHasDetachedContent(cms, true));
	
	if (signingTime) {
		MacOSError::check(CMSEncoderAddSignedAttributes(cms, kCMSAttrSigningTime));
		MacOSError::check(CMSEncoderSetSigningTime(cms, signingTime));
	}
	
	MacOSError::check(CMSEncoderUpdateContent(cms, cd, cd->length()));
    
    // Set up to call Timestamp server if requested
    
    if (state.mWantTimeStamp)
    {
        CFRef<CFErrorRef> error = NULL;
        defaultTSContext = SecCmsTSAGetDefaultContext(&error.aref());
        if (error)
            MacOSError::throwMe(errSecDataNotAvailable);
            
        if (state.mNoTimeStampCerts || state.mTimestampService) {
            if (state.mTimestampService)
                CFDictionarySetValue(defaultTSContext, kTSAContextKeyURL, state.mTimestampService);
            if (state.mNoTimeStampCerts)
                CFDictionarySetValue(defaultTSContext, kTSAContextKeyNoCerts, kCFBooleanTrue);
       }
            
        CmsMessageSetTSAContext(cms, defaultTSContext);
    }
    
	CFDataRef signature;
	MacOSError::check(CMSEncoderCopyEncodedContent(cms, &signature));

	return signature;
}


//
// Parse a text of the form
//	flag,...,flag
// where each flag is the canonical name of a signable CodeDirectory flag.
// No abbreviations are allowed, and internally set flags are not accepted.
//
uint32_t SecCodeSigner::Signer::cdTextFlags(std::string text)
{
	uint32_t flags = 0;
	for (string::size_type comma = text.find(','); ; text = text.substr(comma+1), comma = text.find(',')) {
		string word = (comma == string::npos) ? text : text.substr(0, comma);
		const SecCodeDirectoryFlagTable *item;
		for (item = kSecCodeDirectoryFlagTable; item->name; item++)
			if (item->signable && word == item->name) {
				flags |= item->value;
				break;
			}
		if (!item->name)	// not found
			MacOSError::throwMe(errSecCSInvalidFlags);
		if (comma == string::npos)	// last word
			break;
	}
	return flags;
}


//
// Generate a unique string from our underlying DiskRep.
// We could get 90%+ of the uniquing benefit by just generating
// a random string here. Instead, we pick the (hex string encoding of)
// the source rep's unique identifier blob. For universal binaries,
// this is the canonical local architecture, which is a bit arbitrary.
// This provides us with a consistent unique string for all architectures
// of a fat binary, *and* (unlike a random string) is reproducible
// for identical inputs, even upon resigning.
//
std::string SecCodeSigner::Signer::uniqueName() const
{
	CFRef<CFDataRef> identification = rep->identification();
	const UInt8 *ident = CFDataGetBytePtr(identification);
	const unsigned int length = CFDataGetLength(identification);
	string result;
	for (unsigned int n = 0; n < length; n++) {
		char hex[3];
		snprintf(hex, sizeof(hex), "%02x", ident[n]);
		result += hex;
	}
	return result;
}


} // end namespace CodeSigning
} // end namespace Security
