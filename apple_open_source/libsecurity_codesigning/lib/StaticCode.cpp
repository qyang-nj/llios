/*
 * Copyright (c) 2006-2012 Apple Inc. All Rights Reserved.
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
// StaticCode - SecStaticCode API objects
//
#include "StaticCode.h"
#include "Code.h"
#include "reqmaker.h"
#include "drmaker.h"
#include "reqdumper.h"
#include "sigblob.h"
#include "resources.h"
#include "renum.h"
#include "detachedrep.h"
#include "csdatabase.h"
#include "csutilities.h"
#include "SecCode.h"
#include <CoreFoundation/CFURLAccess.h>
#include <Security/SecPolicyPriv.h>
#include <Security/SecTrustPriv.h>
#include <Security/SecCertificatePriv.h>
#include <Security/CMSPrivate.h>
#include <Security/SecCmsContentInfo.h>
#include <Security/SecCmsSignerInfo.h>
#include <Security/SecCmsSignedData.h>
#include <Security/cssmapplePriv.h>
#include <security_utilities/unix++.h>
#include <security_utilities/cfmunge.h>
#include <Security/CMSDecoder.h>


namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


//
// Construct a SecStaticCode object given a disk representation object
//
SecStaticCode::SecStaticCode(DiskRep *rep)
	: mRep(rep),
	  mValidated(false), mExecutableValidated(false), mResourcesValidated(false), mResourcesValidContext(NULL),
	  mDesignatedReq(NULL), mGotResourceBase(false), mEvalDetails(NULL)
{
	CODESIGN_STATIC_CREATE(this, rep);
	checkForSystemSignature();
}


//
// Clean up a SecStaticCode object
//
SecStaticCode::~SecStaticCode() throw()
try {
	::free(const_cast<Requirement *>(mDesignatedReq));
	if (mResourcesValidContext)
		delete mResourcesValidContext;
} catch (...) {
	return;
}


//
// CF-level comparison of SecStaticCode objects compares CodeDirectory hashes if signed,
// and falls back on comparing canonical paths if (both are) not.
//
bool SecStaticCode::equal(SecCFObject &secOther)
{
	SecStaticCode *other = static_cast<SecStaticCode *>(&secOther);
	CFDataRef mine = this->cdHash();
	CFDataRef his = other->cdHash();
	if (mine || his)
		return mine && his && CFEqual(mine, his);
	else
		return CFEqual(this->canonicalPath(), other->canonicalPath());
}

CFHashCode SecStaticCode::hash()
{
	if (CFDataRef h = this->cdHash())
		return CFHash(h);
	else
		return CFHash(this->canonicalPath());
}


//
// Attach a detached signature.
//
void SecStaticCode::detachedSignature(CFDataRef sigData)
{
	if (sigData) {
		mRep = new DetachedRep(sigData, mRep->base(), "explicit detached");
		CODESIGN_STATIC_ATTACH_EXPLICIT(this, mRep);
	} else {
		mRep = mRep->base();
		CODESIGN_STATIC_ATTACH_EXPLICIT(this, NULL);
	}
}


//
// Consult the system detached signature database to see if it contains
// a detached signature for this StaticCode. If it does, fetch and attach it.
// We do this only if the code has no signature already attached.
//
void SecStaticCode::checkForSystemSignature()
{
	if (!this->isSigned())
		try {
			if (RefPointer<DiskRep> dsig = signatureDatabase().findCode(mRep)) {
				CODESIGN_STATIC_ATTACH_SYSTEM(this, dsig);
				mRep = dsig;
			}
		} catch (...) {
		}
}


//
// Return a descriptive string identifying the source of the code signature
//
string SecStaticCode::signatureSource()
{
	if (!isSigned())
		return "unsigned";
	if (DetachedRep *rep = dynamic_cast<DetachedRep *>(mRep.get()))
		return rep->source();
	return "embedded";
}


//
// Do ::required, but convert incoming SecCodeRefs to their SecStaticCodeRefs
// (if possible).
//
SecStaticCode *SecStaticCode::requiredStatic(SecStaticCodeRef ref)
{
	SecCFObject *object = SecCFObject::required(ref, errSecCSInvalidObjectRef);
	if (SecStaticCode *scode = dynamic_cast<SecStaticCode *>(object))
		return scode;
	else if (SecCode *code = dynamic_cast<SecCode *>(object))
		return code->staticCode();
	else	// neither (a SecSomethingElse)
		MacOSError::throwMe(errSecCSInvalidObjectRef);
}

SecCode *SecStaticCode::optionalDynamic(SecStaticCodeRef ref)
{
	SecCFObject *object = SecCFObject::required(ref, errSecCSInvalidObjectRef);
	if (dynamic_cast<SecStaticCode *>(object))
		return NULL;
	else if (SecCode *code = dynamic_cast<SecCode *>(object))
		return code;
	else	// neither (a SecSomethingElse)
		MacOSError::throwMe(errSecCSInvalidObjectRef);
}


//
// Void all cached validity data.
//
// We also throw out cached components, because the new signature data may have
// a different idea of what components should be present. We could reconcile the
// cached data instead, if performance seems to be impacted.
//
void SecStaticCode::resetValidity()
{
	CODESIGN_EVAL_STATIC_RESET(this);
	mValidated = false;
	mExecutableValidated = false;
	mResourcesValidated = false;
	if (mResourcesValidContext) {
		delete mResourcesValidContext;
		mResourcesValidContext = NULL;
	}
	mDir = NULL;
	mSignature = NULL;
	for (unsigned n = 0; n < cdSlotCount; n++)
		mCache[n] = NULL;
	mInfoDict = NULL;
	mEntitlements = NULL;
	mResourceDict = NULL;
	mDesignatedReq = NULL;
	mGotResourceBase = false;
	mTrust = NULL;
	mCertChain = NULL;
	mEvalDetails = NULL;
	mRep->flush();
	
	// we may just have updated the system database, so check again
	checkForSystemSignature();
}


//
// Retrieve a sealed component by special slot index.
// If the CodeDirectory has already been validated, validate against that.
// Otherwise, retrieve the component without validation (but cache it). Validation
// will go through the cache and validate all cached components.
//
CFDataRef SecStaticCode::component(CodeDirectory::SpecialSlot slot, OSStatus fail /* = errSecCSSignatureFailed */)
{
	assert(slot <= cdSlotMax);
	
	CFRef<CFDataRef> &cache = mCache[slot];
	if (!cache) {
		if (CFRef<CFDataRef> data = mRep->component(slot)) {
			if (validated()) // if the directory has been validated...
				if (!codeDirectory()->validateSlot(CFDataGetBytePtr(data), // ... and it's no good
						CFDataGetLength(data), -slot))
					MacOSError::throwMe(fail); // ... then bail
			cache = data;	// it's okay, cache it
		} else {	// absent, mark so
			if (validated())	// if directory has been validated...
				if (codeDirectory()->slotIsPresent(-slot)) // ... and the slot is NOT missing
					MacOSError::throwMe(fail);	// was supposed to be there
			cache = CFDataRef(kCFNull);		// white lie
		}
	}
	return (cache == CFDataRef(kCFNull)) ? NULL : cache.get();
}


//
// Get the CodeDirectory.
// Throws (if check==true) or returns NULL (check==false) if there is none.
// Always throws if the CodeDirectory exists but is invalid.
// NEVER validates against the signature.
//
const CodeDirectory *SecStaticCode::codeDirectory(bool check /* = true */)
{
	if (!mDir) {
		if (mDir.take(mRep->codeDirectory())) {
			const CodeDirectory *dir = reinterpret_cast<const CodeDirectory *>(CFDataGetBytePtr(mDir));
			dir->checkIntegrity();
		}
	}
	if (mDir)
		return reinterpret_cast<const CodeDirectory *>(CFDataGetBytePtr(mDir));
	if (check)
		MacOSError::throwMe(errSecCSUnsigned);
	return NULL;
}


//
// Get the hash of the CodeDirectory.
// Returns NULL if there is none.
//
CFDataRef SecStaticCode::cdHash()
{
	if (!mCDHash) {
		if (const CodeDirectory *cd = codeDirectory(false)) {
			SHA1 hash;
			hash(cd, cd->length());
			SHA1::Digest digest;
			hash.finish(digest);
			mCDHash.take(makeCFData(digest, sizeof(digest)));
			CODESIGN_STATIC_CDHASH(this, digest, sizeof(digest));
		}
	}
	return mCDHash;
}


//
// Return the CMS signature blob; NULL if none found.
//
CFDataRef SecStaticCode::signature()
{
	if (!mSignature)
		mSignature.take(mRep->signature());
	if (mSignature)
		return mSignature;
	MacOSError::throwMe(errSecCSUnsigned);
}


//
// Verify the signature on the CodeDirectory.
// If this succeeds (doesn't throw), the CodeDirectory is statically trustworthy.
// Any outcome (successful or not) is cached for the lifetime of the StaticCode.
//
void SecStaticCode::validateDirectory()
{
	// echo previous outcome, if any
	if (!validated())
		try {
			// perform validation (or die trying)
			CODESIGN_EVAL_STATIC_DIRECTORY(this);
			mValidationExpired = verifySignature();
			component(cdInfoSlot, errSecCSInfoPlistFailed);	// force load of Info Dictionary (if any)
			for (CodeDirectory::SpecialSlot slot = codeDirectory()->maxSpecialSlot(); slot >= 1; --slot)
				if (mCache[slot])	// if we already loaded that resource...
					validateComponent(slot); // ... then check it now
			mValidated = true;			// we've done the deed...
			mValidationResult = noErr;	// ... and it was good
		} catch (const CommonError &err) {
			mValidated = true;
			mValidationResult = err.osStatus();
			throw;
		} catch (...) {
			secdebug("staticCode", "%p validation threw non-common exception", this);
			mValidated = true;
			mValidationResult = errSecCSInternalError;
			throw;
		}
	assert(validated());
	if (mValidationResult == noErr) {
		if (mValidationExpired)
			if ((apiFlags() & kSecCSConsiderExpiration)
					|| (codeDirectory()->flags & kSecCodeSignatureForceExpiration))
				MacOSError::throwMe(CSSMERR_TP_CERT_EXPIRED);
	} else
		MacOSError::throwMe(mValidationResult);
}


//
// Load and validate the CodeDirectory and all components *except* those related to the resource envelope.
// Those latter components are checked by validateResources().
//
void SecStaticCode::validateNonResourceComponents()
{
	this->validateDirectory();
	for (CodeDirectory::SpecialSlot slot = codeDirectory()->maxSpecialSlot(); slot >= 1; --slot)
		switch (slot) {
		case cdResourceDirSlot:		// validated by validateResources
			break;
		default:
			this->component(slot);		// loads and validates
			break;
		}
}


//
// Get the (signed) signing date from the code signature.
// Sadly, we need to validate the signature to get the date (as a side benefit).
// This means that you can't get the signing time for invalidly signed code.
//
// We could run the decoder "almost to" verification to avoid this, but there seems
// little practical point to such a duplication of effort.
//
CFAbsoluteTime SecStaticCode::signingTime()
{
	validateDirectory();
	return mSigningTime;
}

CFAbsoluteTime SecStaticCode::signingTimestamp()
{
	validateDirectory();
	return mSigningTimestamp;
}


//
// Verify the CMS signature on the CodeDirectory.
// This performs the cryptographic tango. It returns if the signature is valid,
// or throws if it is not. As a side effect, a successful return sets up the
// cached certificate chain for future use.
// Returns true if the signature is expired (the X.509 sense), false if it's not.
//
bool SecStaticCode::verifySignature()
{
	// ad-hoc signed code is considered validly signed by definition
	if (flag(kSecCodeSignatureAdhoc)) {
		CODESIGN_EVAL_STATIC_SIGNATURE_ADHOC(this);
		return false;
	}
	
	DTRACK(CODESIGN_EVAL_STATIC_SIGNATURE, this, (char*)this->mainExecutablePath().c_str());

	// decode CMS and extract SecTrust for verification
	CFRef<CMSDecoderRef> cms;
	MacOSError::check(CMSDecoderCreate(&cms.aref())); // create decoder
	CFDataRef sig = this->signature();
	MacOSError::check(CMSDecoderUpdateMessage(cms, CFDataGetBytePtr(sig), CFDataGetLength(sig)));
	this->codeDirectory();	// load CodeDirectory (sets mDir)
	MacOSError::check(CMSDecoderSetDetachedContent(cms, mDir));
	MacOSError::check(CMSDecoderFinalizeMessage(cms));
	MacOSError::check(CMSDecoderSetSearchKeychain(cms, cfEmptyArray()));
	CFRef<CFTypeRef> policy = verificationPolicy(apiFlags());
    CMSSignerStatus status;
    MacOSError::check(CMSDecoderCopySignerStatus(cms, 0, policy,
		false, &status, &mTrust.aref(), NULL));
	if (status != kCMSSignerValid)
		MacOSError::throwMe(errSecCSSignatureFailed);
	
	// internal signing time (as specified by the signer; optional)
	mSigningTime = 0;       // "not present" marker (nobody could code sign on Jan 1, 2001 :-)
	switch (OSStatus rc = CMSDecoderCopySignerSigningTime(cms, 0, &mSigningTime)) {
	case noErr:
	case errSecSigningTimeMissing:
		break;
	default:
		MacOSError::throwMe(rc);
	}

	// certified signing time (as specified by a TSA; optional)
	mSigningTimestamp = 0;
	switch (OSStatus rc = CMSDecoderCopySignerTimestamp(cms, 0, &mSigningTimestamp)) {
	case noErr:
	case errSecTimestampMissing:
		break;
	default:
		MacOSError::throwMe(rc);
	}
    
	// set up the environment for SecTrust
	MacOSError::check(SecTrustSetAnchorCertificates(mTrust, cfEmptyArray())); // no anchors
    MacOSError::check(SecTrustSetKeychains(mTrust, cfEmptyArray())); // no keychains
	CSSM_APPLE_TP_ACTION_DATA actionData = {
		CSSM_APPLE_TP_ACTION_VERSION,	// version of data structure
		CSSM_TP_ACTION_IMPLICIT_ANCHORS	// action flags
	};
	
	for (;;) {	// at most twice
		MacOSError::check(SecTrustSetParameters(mTrust,
			CSSM_TP_ACTION_DEFAULT, CFTempData(&actionData, sizeof(actionData))));
	
		// evaluate trust and extract results
		SecTrustResultType trustResult;
		MacOSError::check(SecTrustEvaluate(mTrust, &trustResult));
		MacOSError::check(SecTrustGetResult(mTrust, &trustResult, &mCertChain.aref(), &mEvalDetails));
		CODESIGN_EVAL_STATIC_SIGNATURE_RESULT(this, trustResult, mCertChain ? CFArrayGetCount(mCertChain) : 0);
		switch (trustResult) {
		case kSecTrustResultProceed:
		case kSecTrustResultUnspecified:
			break;				// success
		case kSecTrustResultDeny:
			MacOSError::throwMe(CSSMERR_APPLETP_TRUST_SETTING_DENY);	// user reject
		case kSecTrustResultInvalid:
			assert(false);		// should never happen
			MacOSError::throwMe(CSSMERR_TP_NOT_TRUSTED);
		default:
			{
				OSStatus result;
				MacOSError::check(SecTrustGetCssmResultCode(mTrust, &result));
				// if we have a valid timestamp, CMS validates against (that) signing time and all is well.
				// If we don't have one, may validate against *now*, and must be able to tolerate expiration.
				if (mSigningTimestamp == 0) // no timestamp available
					if (((result == CSSMERR_TP_CERT_EXPIRED) || (result == CSSMERR_TP_CERT_NOT_VALID_YET))
							&& !(actionData.ActionFlags & CSSM_TP_ACTION_ALLOW_EXPIRED)) {
						CODESIGN_EVAL_STATIC_SIGNATURE_EXPIRED(this);
						actionData.ActionFlags |= CSSM_TP_ACTION_ALLOW_EXPIRED; // (this also allows postdated certs)
						continue;		// retry validation while tolerating expiration
					}
				MacOSError::throwMe(result);
			}
		}
		return actionData.ActionFlags & CSSM_TP_ACTION_ALLOW_EXPIRED;
	}
}


//
// Return the TP policy used for signature verification.
// This may be a simple SecPolicyRef or a CFArray of policies.
// The caller owns the return value.
//
static SecPolicyRef makeCRLPolicy()
{
	CFRef<SecPolicyRef> policy;
	MacOSError::check(SecPolicyCopy(CSSM_CERT_X_509v3, &CSSMOID_APPLE_TP_REVOCATION_CRL, &policy.aref()));
	CSSM_APPLE_TP_CRL_OPTIONS options;
	memset(&options, 0, sizeof(options));
	options.Version = CSSM_APPLE_TP_CRL_OPTS_VERSION;
	options.CrlFlags = CSSM_TP_ACTION_FETCH_CRL_FROM_NET | CSSM_TP_ACTION_CRL_SUFFICIENT;
	CSSM_DATA optData = { sizeof(options), (uint8 *)&options };
	MacOSError::check(SecPolicySetValue(policy, &optData));
	return policy.yield();
}

static SecPolicyRef makeOCSPPolicy()
{
	CFRef<SecPolicyRef> policy;
	MacOSError::check(SecPolicyCopy(CSSM_CERT_X_509v3, &CSSMOID_APPLE_TP_REVOCATION_OCSP, &policy.aref()));
	CSSM_APPLE_TP_OCSP_OPTIONS options;
	memset(&options, 0, sizeof(options));
	options.Version = CSSM_APPLE_TP_OCSP_OPTS_VERSION;
	options.Flags = CSSM_TP_ACTION_OCSP_SUFFICIENT;
	CSSM_DATA optData = { sizeof(options), (uint8 *)&options };
	MacOSError::check(SecPolicySetValue(policy, &optData));
	return policy.yield();
}

CFTypeRef SecStaticCode::verificationPolicy(SecCSFlags flags)
{
	CFRef<SecPolicyRef> core;
	MacOSError::check(SecPolicyCopy(CSSM_CERT_X_509v3,
			&CSSMOID_APPLE_TP_CODE_SIGNING, &core.aref()));
	if (flags & kSecCSEnforceRevocationChecks) {
		CFRef<SecPolicyRef> crl = makeCRLPolicy();
		CFRef<SecPolicyRef> ocsp = makeOCSPPolicy();
		return makeCFArray(3, core.get(), crl.get(), ocsp.get());
	} else {
		return core.yield();
	}
}


//
// Validate a particular sealed, cached resource against its (special) CodeDirectory slot.
// The resource must already have been placed in the cache.
// This does NOT perform basic validation.
//
void SecStaticCode::validateComponent(CodeDirectory::SpecialSlot slot, OSStatus fail /* = errSecCSSignatureFailed */)
{
	assert(slot <= cdSlotMax);
	CFDataRef data = mCache[slot];
	assert(data);		// must be cached
	if (data == CFDataRef(kCFNull)) {
		if (codeDirectory()->slotIsPresent(-slot)) // was supposed to be there...
				MacOSError::throwMe(fail);	// ... and is missing
	} else {
		if (!codeDirectory()->validateSlot(CFDataGetBytePtr(data), CFDataGetLength(data), -slot))
			MacOSError::throwMe(fail);
	}
}


//
// Perform static validation of the main executable.
// This reads the main executable from disk and validates it against the
// CodeDirectory code slot array.
// Note that this is NOT an in-memory validation, and is thus potentially
// subject to timing attacks.
//
void SecStaticCode::validateExecutable()
{
	if (!validatedExecutable()) {
		try {
			DTRACK(CODESIGN_EVAL_STATIC_EXECUTABLE, this,
				(char*)this->mainExecutablePath().c_str(), codeDirectory()->nCodeSlots);
			const CodeDirectory *cd = this->codeDirectory();
			if (!cd) 
				MacOSError::throwMe(errSecCSUnsigned);
			AutoFileDesc fd(mainExecutablePath(), O_RDONLY);
			fd.fcntl(F_NOCACHE, true);		// turn off page caching (one-pass)
			if (Universal *fat = mRep->mainExecutableImage())
				fd.seek(fat->archOffset());
			size_t pageSize = cd->pageSize ? (1 << cd->pageSize) : 0;
			size_t remaining = cd->codeLimit;
			for (size_t slot = 0; slot < cd->nCodeSlots; ++slot) {
				size_t size = min(remaining, pageSize);
				if (!cd->validateSlot(fd, size, slot)) {
					CODESIGN_EVAL_STATIC_EXECUTABLE_FAIL(this, slot);
					MacOSError::throwMe(errSecCSSignatureFailed);
				}
				remaining -= size;
			}
			mExecutableValidated = true;
			mExecutableValidResult = noErr;
		} catch (const CommonError &err) {
			mExecutableValidated = true;
			mExecutableValidResult = err.osStatus();
			throw;
		} catch (...) {
			secdebug("staticCode", "%p executable validation threw non-common exception", this);
			mExecutableValidated = true;
			mExecutableValidResult = errSecCSInternalError;
			throw;
		}
	}
	assert(validatedExecutable());
	if (mExecutableValidResult != noErr)
		MacOSError::throwMe(mExecutableValidResult);
}


//
// Perform static validation of sealed resources.
//
// This performs a whole-code static resource scan and effectively
// computes a concordance between what's on disk and what's in the ResourceDirectory.
// Any unsanctioned difference causes an error.
//
void SecStaticCode::validateResources()
{
	if (!validatedResources()) {
		try {
			// sanity first
			CFDictionaryRef sealedResources = resourceDictionary();
			if (this->resourceBase())		// disk has resources
				if (sealedResources)
					/* go to work below */;
				else
					MacOSError::throwMe(errSecCSResourcesNotFound);
			else							// disk has no resources
				if (sealedResources)
					MacOSError::throwMe(errSecCSResourcesNotFound);
				else
					return;					// no resources, not sealed - fine (no work)
		
			// found resources, and they are sealed
			CFDictionaryRef rules = cfget<CFDictionaryRef>(sealedResources, "rules");
			CFDictionaryRef files = cfget<CFDictionaryRef>(sealedResources, "files");
			DTRACK(CODESIGN_EVAL_STATIC_RESOURCES, this,
				(char*)this->mainExecutablePath().c_str(), int(CFDictionaryGetCount(files)));
		
			// make a shallow copy of the ResourceDirectory so we can "check off" what we find
			CFRef<CFMutableDictionaryRef> resourceMap = makeCFMutableDictionary(files);
		
			// scan through the resources on disk, checking each against the resourceDirectory
			mResourcesValidContext = new CollectingContext(*this);		// collect all failures in here
			ResourceBuilder resources(cfString(this->resourceBase()), rules, codeDirectory()->hashType);
			mRep->adjustResources(resources);
			string path;
			ResourceBuilder::Rule *rule;
		
			while (resources.next(path, rule)) {
				validateResource(path, *mResourcesValidContext);
				CFDictionaryRemoveValue(resourceMap, CFTempString(path));
			}
			
			if (CFDictionaryGetCount(resourceMap) > 0) {
				secdebug("staticCode", "%p sealed resource(s) not found in code", this);
				CFDictionaryApplyFunction(resourceMap, SecStaticCode::checkOptionalResource, mResourcesValidContext);
			}
			
			// now check for any errors found in the reporting context
			mResourcesValidated = true;
			if (mResourcesValidContext->osStatus() != noErr)
				mResourcesValidContext->throwMe();

		} catch (const CommonError &err) {
			mResourcesValidated = true;
			mResourcesValidResult = err.osStatus();
			throw;
		} catch (...) {
			secdebug("staticCode", "%p executable validation threw non-common exception", this);
			mResourcesValidated = true;
			mResourcesValidResult = errSecCSInternalError;
			throw;
		}
	}
	assert(validatedResources());
	if (mResourcesValidResult)
		MacOSError::throwMe(mResourcesValidResult);
	if (mResourcesValidContext->osStatus() != noErr)
		mResourcesValidContext->throwMe();
}


void SecStaticCode::checkOptionalResource(CFTypeRef key, CFTypeRef value, void *context)
{
	CollectingContext *ctx = static_cast<CollectingContext *>(context);
	ResourceSeal seal(value);
	if (!seal.optional()) {
		if (key && CFGetTypeID(key) == CFStringGetTypeID()) {
			ctx->reportProblem(errSecCSBadResource, kSecCFErrorResourceMissing,
				CFTempURL(CFStringRef(key), false, ctx->code.resourceBase()));
		} else {
			ctx->reportProblem(errSecCSBadResource, kSecCFErrorResourceSeal, key);
		}
	}
}


//
// Load, validate, cache, and return CFDictionary forms of sealed resources.
//
CFDictionaryRef SecStaticCode::infoDictionary()
{
	if (!mInfoDict) {
		mInfoDict.take(getDictionary(cdInfoSlot, errSecCSInfoPlistFailed));
		secdebug("staticCode", "%p loaded InfoDict %p", this, mInfoDict.get());
	}
	return mInfoDict;
}

CFDictionaryRef SecStaticCode::entitlements()
{
	if (!mEntitlements) {
		validateDirectory();
		if (CFDataRef entitlementData = component(cdEntitlementSlot)) {
			validateComponent(cdEntitlementSlot);
			const EntitlementBlob *blob = reinterpret_cast<const EntitlementBlob *>(CFDataGetBytePtr(entitlementData));
			if (blob->validateBlob()) {
				mEntitlements.take(blob->entitlements());
				secdebug("staticCode", "%p loaded Entitlements %p", this, mEntitlements.get());
			}
			// we do not consider a different blob type to be an error. We think it's a new format we don't understand
		}
	}
	return mEntitlements;
}

CFDictionaryRef SecStaticCode::resourceDictionary()
{
	if (mResourceDict)	// cached
		return mResourceDict;
	if (CFRef<CFDictionaryRef> dict = getDictionary(cdResourceDirSlot, errSecCSSignatureFailed))
		if (cfscan(dict, "{rules=%Dn,files=%Dn}")) {
			secdebug("staticCode", "%p loaded ResourceDict %p",
				this, mResourceDict.get());
			return mResourceDict = dict;
		}
	// bad format
	return NULL;
}


//
// Load and cache the resource directory base.
// Note that the base is optional for each DiskRep.
//
CFURLRef SecStaticCode::resourceBase()
{
	if (!mGotResourceBase) {
		string base = mRep->resourcesRootPath();
		if (!base.empty())
			mResourceBase.take(makeCFURL(base, true));
		mGotResourceBase = true;
	}
	return mResourceBase;
}


//
// Load a component, validate it, convert it to a CFDictionary, and return that.
// This will force load and validation, which means that it will perform basic
// validation if it hasn't been done yet.
//
CFDictionaryRef SecStaticCode::getDictionary(CodeDirectory::SpecialSlot slot, OSStatus fail /* = errSecCSSignatureFailed */)
{
	validateDirectory();
	if (CFDataRef infoData = component(slot, fail)) {
		validateComponent(slot, fail);
		if (CFDictionaryRef dict = makeCFDictionaryFrom(infoData))
			return dict;
		else
			MacOSError::throwMe(errSecCSBadDictionaryFormat);
	}
	return NULL;
}


//
// Load, validate, and return a sealed resource.
// The resource data (loaded in to memory as a blob) is returned and becomes
// the responsibility of the caller; it is NOT cached by SecStaticCode.
//
// A resource that is not sealed will not be returned, and an error will be thrown.
// A missing resource will cause an error unless it's marked optional in the Directory.
// Under no circumstances will a corrupt resource be returned.
// NULL will only be returned for a resource that is neither sealed nor present
// (or that is sealed, absent, and marked optional).
// If the ResourceDictionary itself is not sealed, this function will always fail.
//
// There is currently no interface for partial retrieval of the resource data.
// (Since the ResourceDirectory does not currently support segmentation, all the
// data would have to be read anyway, but it could be read into a reusable buffer.)
//
CFDataRef SecStaticCode::resource(string path, ValidationContext &ctx)
{
	if (CFDictionaryRef rdict = resourceDictionary()) {
		if (CFTypeRef file = cfget(rdict, "files.%s", path.c_str())) {
			ResourceSeal seal = file;
			if (!resourceBase())	// no resources in DiskRep
				MacOSError::throwMe(errSecCSResourcesNotFound);
			CFRef<CFURLRef> fullpath = makeCFURL(path, false, resourceBase());
			if (CFRef<CFDataRef> data = cfLoadFile(fullpath)) {
				MakeHash<CodeDirectory> hasher(this->codeDirectory());
				hasher->update(CFDataGetBytePtr(data), CFDataGetLength(data));
				if (hasher->verify(seal.hash()))
					return data.yield();	// good
				else
					ctx.reportProblem(errSecCSBadResource, kSecCFErrorResourceAltered, fullpath); // altered
			} else {
				if (!seal.optional())
					ctx.reportProblem(errSecCSBadResource, kSecCFErrorResourceMissing, fullpath); // was sealed but is now missing
				else
					return NULL;	// validly missing
			}
		} else
			ctx.reportProblem(errSecCSBadResource, kSecCFErrorResourceAdded, CFTempURL(path, false, resourceBase()));
		return NULL;
	} else
		MacOSError::throwMe(errSecCSResourcesNotSealed);
}

CFDataRef SecStaticCode::resource(string path)
{
	ValidationContext ctx;
	return resource(path, ctx);
}


void SecStaticCode::validateResource(string path, ValidationContext &ctx)
{
	if (CFDictionaryRef rdict = resourceDictionary()) {
		if (CFTypeRef file = cfget(rdict, "files.%s", path.c_str())) {
			ResourceSeal seal = file;
			if (!resourceBase())	// no resources in DiskRep
				MacOSError::throwMe(errSecCSResourcesNotFound);
			CFRef<CFURLRef> fullpath = makeCFURL(path, false, resourceBase());
			AutoFileDesc fd(cfString(fullpath), O_RDONLY, FileDesc::modeMissingOk);	// open optional filee
			if (fd) {
				MakeHash<CodeDirectory> hasher(this->codeDirectory());
				hashFileData(fd, hasher.get());
				if (hasher->verify(seal.hash()))
					return;			// verify good
				else
					ctx.reportProblem(errSecCSBadResource, kSecCFErrorResourceAltered, fullpath); // altered
			} else {
				if (!seal.optional())
					ctx.reportProblem(errSecCSBadResource, kSecCFErrorResourceMissing, fullpath); // was sealed but is now missing
				else
					return;			// validly missing
			}
		} else
			ctx.reportProblem(errSecCSBadResource, kSecCFErrorResourceAdded, CFTempURL(path, false, resourceBase()));
	} else
		MacOSError::throwMe(errSecCSResourcesNotSealed);
}


//
// Test a CodeDirectory flag.
// Returns false if there is no CodeDirectory.
// May throw if the CodeDirectory is present but somehow invalid.
//
bool SecStaticCode::flag(uint32_t tested)
{
	if (const CodeDirectory *cd = this->codeDirectory(false))
		return cd->flags & tested;
	else
		return false;
}


//
// Retrieve the full SuperBlob containing all internal requirements.
//
const Requirements *SecStaticCode::internalRequirements()
{
	if (CFDataRef req = component(cdRequirementsSlot))
		return (const Requirements *)CFDataGetBytePtr(req);
	else
		return NULL;
}


//
// Retrieve a particular internal requirement by type.
//
const Requirement *SecStaticCode::internalRequirement(SecRequirementType type)
{
	if (const Requirements *reqs = internalRequirements())
		return reqs->find<Requirement>(type);
	else
		return NULL;
}


//
// Return the Designated Requirement (DR). This can be either explicit in the
// Internal Requirements component, or implicitly generated on demand here.
// Note that an explicit DR may have been implicitly generated at signing time;
// we don't distinguish this case.
//
const Requirement *SecStaticCode::designatedRequirement()
{
	if (const Requirement *req = internalRequirement(kSecDesignatedRequirementType)) {
		return req;		// explicit in signing data
	} else {
		if (!mDesignatedReq)
			mDesignatedReq = defaultDesignatedRequirement();
		return mDesignatedReq;
	}
}


//
// Generate the default Designated Requirement (DR) for this StaticCode.
// Ignore any explicit DR it may contain.
//
const Requirement *SecStaticCode::defaultDesignatedRequirement()
{
	if (flag(kSecCodeSignatureAdhoc)) {
		// adhoc signature: return a plain cdhash requirement
		Requirement::Maker maker;
		SHA1 hash;
		hash(codeDirectory(), codeDirectory()->length());
		SHA1::Digest digest;
		hash.finish(digest);
		maker.cdhash(digest);
		return maker.make();
	} else {
		// full signature: Gin up full context and let DRMaker do its thing
		validateDirectory();		// need the cert chain
		Requirement::Context context(this->certificates(),
			this->infoDictionary(),
			this->entitlements(),
			this->identifier(),
			this->codeDirectory()
		);
		return DRMaker(context).make();
	}
}


//
// Validate a SecStaticCode against the internal requirement of a particular type.
//
void SecStaticCode::validateRequirements(SecRequirementType type, SecStaticCode *target,
	OSStatus nullError /* = noErr */)
{
	DTRACK(CODESIGN_EVAL_STATIC_INTREQ, this, type, target, nullError);
	if (const Requirement *req = internalRequirement(type))
		target->validateRequirement(req, nullError ? nullError : errSecCSReqFailed);
	else if (nullError)
		MacOSError::throwMe(nullError);
	else
		/* accept it */;
}


//
// Validate this StaticCode against an external Requirement
//
bool SecStaticCode::satisfiesRequirement(const Requirement *req, OSStatus failure)
{
	assert(req);
	validateDirectory();
	return req->validates(Requirement::Context(mCertChain, infoDictionary(), entitlements(), codeDirectory()->identifier(), codeDirectory()), failure);
}

void SecStaticCode::validateRequirement(const Requirement *req, OSStatus failure)
{
	if (!this->satisfiesRequirement(req, failure))
		MacOSError::throwMe(failure);
}


//
// Retrieve one certificate from the cert chain.
// Positive and negative indices can be used:
//    [ leaf, intermed-1, ..., intermed-n, anchor ]
//        0       1       ...     -2         -1
// Returns NULL if unavailable for any reason.
//
SecCertificateRef SecStaticCode::cert(int ix)
{
	validateDirectory();		// need cert chain
	if (mCertChain) {
		CFIndex length = CFArrayGetCount(mCertChain);
		if (ix < 0)
			ix += length;
		if (ix >= 0 && ix < length)
			return SecCertificateRef(CFArrayGetValueAtIndex(mCertChain, ix));
	}
	return NULL;
}

CFArrayRef SecStaticCode::certificates()
{
	validateDirectory();		// need cert chain
	return mCertChain;
}


//
// Gather (mostly) API-official information about this StaticCode.
//
// This method lives in the twilight between the API and internal layers,
// since it generates API objects (Sec*Refs) for return.
//
CFDictionaryRef SecStaticCode::signingInformation(SecCSFlags flags)
{
	//
	// Start with the pieces that we return even for unsigned code.
	// This makes Sec[Static]CodeRefs useful as API-level replacements
	// of our internal OSXCode objects.
	//
	CFRef<CFMutableDictionaryRef> dict = makeCFMutableDictionary(1,
		kSecCodeInfoMainExecutable, CFTempURL(this->mainExecutablePath()).get()
	);
	
	//
	// If we're not signed, this is all you get
	//
	if (!this->isSigned())
		return dict.yield();
	
	//
	// Add the generic attributes that we always include
	//
	CFDictionaryAddValue(dict, kSecCodeInfoIdentifier, CFTempString(this->identifier()));
	CFDictionaryAddValue(dict, kSecCodeInfoFormat, CFTempString(this->format()));
	CFDictionaryAddValue(dict, kSecCodeInfoSource, CFTempString(this->signatureSource()));
	CFDictionaryAddValue(dict, kSecCodeInfoUnique, this->cdHash());
	CFDictionaryAddValue(dict, kSecCodeInfoDigestAlgorithm, CFTempNumber(this->codeDirectory(false)->hashType));

	//
	// Deliver any Info.plist only if it looks intact
	//
	try {
		if (CFDictionaryRef info = this->infoDictionary())
			CFDictionaryAddValue(dict, kSecCodeInfoPList, info);
	} catch (...) { }		// don't deliver Info.plist if questionable

	//
	// kSecCSSigningInformation adds information about signing certificates and chains
	//
	if (flags & kSecCSSigningInformation) {
		if (CFArrayRef certs = this->certificates())
		CFDictionaryAddValue(dict, kSecCodeInfoCertificates, certs);
		if (CFDataRef sig = this->signature())
			CFDictionaryAddValue(dict, kSecCodeInfoCMS, sig);
		if (mTrust)
			CFDictionaryAddValue(dict, kSecCodeInfoTrust, mTrust);
		if (CFAbsoluteTime time = this->signingTime())
			if (CFRef<CFDateRef> date = CFDateCreate(NULL, time))
				CFDictionaryAddValue(dict, kSecCodeInfoTime, date);
		if (CFAbsoluteTime time = this->signingTimestamp())
			if (CFRef<CFDateRef> date = CFDateCreate(NULL, time))
				CFDictionaryAddValue(dict, kSecCodeInfoTimestamp, date);
	}
	
	//
	// kSecCSRequirementInformation adds information on requirements
	//
	if (flags & kSecCSRequirementInformation) {
		if (const Requirements *reqs = this->internalRequirements()) {
			CFDictionaryAddValue(dict, kSecCodeInfoRequirements,
				CFTempString(Dumper::dump(reqs)));
			CFDictionaryAddValue(dict, kSecCodeInfoRequirementData, CFTempData(*reqs));
		}
		
		const Requirement *dreq = this->designatedRequirement();
		CFRef<SecRequirementRef> dreqRef = (new SecRequirement(dreq))->handle();
		CFDictionaryAddValue(dict, kSecCodeInfoDesignatedRequirement, dreqRef);
		if (this->internalRequirement(kSecDesignatedRequirementType)) {	// explicit
			CFRef<SecRequirementRef> ddreqRef = (new SecRequirement(this->defaultDesignatedRequirement(), true))->handle();
			CFDictionaryAddValue(dict, kSecCodeInfoImplicitDesignatedRequirement, ddreqRef);
		} else {	// implicit
			CFDictionaryAddValue(dict, kSecCodeInfoImplicitDesignatedRequirement, dreqRef);
		}
		
	   if (CFDataRef ent = this->component(cdEntitlementSlot)) {
		   CFDictionaryAddValue(dict, kSecCodeInfoEntitlements, ent);
		   if (CFDictionaryRef entdict = this->entitlements())
				CFDictionaryAddValue(dict, kSecCodeInfoEntitlementsDict, entdict);
		}
	}
	
	//
	// kSecCSInternalInformation adds internal information meant to be for Apple internal
	// use (SPI), and not guaranteed to be stable. Primarily, this is data we want
	// to reliably transmit through the API wall so that code outside the Security.framework
	// can use it without having to play nasty tricks to get it.
	//
	if (flags & kSecCSInternalInformation) {
		if (mDir)
			CFDictionaryAddValue(dict, kSecCodeInfoCodeDirectory, mDir);
		CFDictionaryAddValue(dict, kSecCodeInfoCodeOffset, CFTempNumber(mRep->signingBase()));
		if (CFDictionaryRef resources = resourceDictionary())
			CFDictionaryAddValue(dict, kSecCodeInfoResourceDirectory, resources);
	}
	
	
	//
	// kSecCSContentInformation adds more information about the physical layout
	// of the signed code. This is (only) useful for packaging or patching-oriented
	// applications.
	//
	if (flags & kSecCSContentInformation)
		if (CFRef<CFArrayRef> files = mRep->modifiedFiles())
			CFDictionaryAddValue(dict, kSecCodeInfoChangedFiles, files);
	
	return dict.yield();
}


//
// Resource validation contexts.
// The default context simply throws a CSError, rudely terminating the operation.
//
SecStaticCode::ValidationContext::~ValidationContext()
{ /* virtual */ }

void SecStaticCode::ValidationContext::reportProblem(OSStatus rc, CFStringRef type, CFTypeRef value)
{
	CSError::throwMe(rc, type, value);
}

void SecStaticCode::CollectingContext::reportProblem(OSStatus rc, CFStringRef type, CFTypeRef value)
{
	if (mStatus == noErr)
		mStatus = rc;			// record first failure for eventual error return
	if (type) {
		if (!mCollection)
			mCollection.take(makeCFMutableDictionary());
		CFMutableArrayRef element = CFMutableArrayRef(CFDictionaryGetValue(mCollection, type));
		if (!element) {
			element = makeCFMutableArray(0);
			if (!element)
				CFError::throwMe();
			CFDictionaryAddValue(mCollection, type, element);
			CFRelease(element);
		}
		CFArrayAppendValue(element, value);
	}
}

void SecStaticCode::CollectingContext::throwMe()
{
	assert(mStatus != noErr);
	throw CSError(mStatus, mCollection.retain());
}


//
// SecStaticCode::AllArchitectures produces SecStaticCode objects separately
// for each architecture represented by a base object.
//
// Performance note: This is a simple, straight-forward implementation that
// does not heroically try to share resources between the code objects produced.
// In practice, this means we'll re-open files and re-read resource files.
// In exchange, we enter all the code paths in the normal way, and do not have
// special sharing paths to worry about.
// If a performance tool brings you here because you have *proof* of a performance
// problem, consider digging up MachO and Universal (for sharing file descriptors),
// and SecStaticCode (for sharing resource iterators). That ought to cover most of
// the big chunks. If you're just offended by the simplicity of this implementation,
// go play somewhere else.
//
SecStaticCode::AllArchitectures::AllArchitectures(SecStaticCode *code)
	: mBase(code)
{
	if (Universal *fat = code->diskRep()->mainExecutableImage()) {
		fat->architectures(mArchitectures);
		mCurrent = mArchitectures.begin();
		mState = fatBinary;
	} else {
		mState = firstNonFat;
	}
}

SecStaticCode *SecStaticCode::AllArchitectures::operator () ()
{
	switch (mState) {
	case firstNonFat:
		mState = atEnd;
		return mBase;
	case fatBinary:
		{
			if (mCurrent == mArchitectures.end())
				return NULL;
			Architecture arch = *mCurrent++;
			if (arch == mBase->diskRep()->mainExecutableImage()->bestNativeArch()) {
				return mBase;
			} else {
				DiskRep::Context ctx;
				ctx.arch = arch;
				return new SecStaticCode(DiskRep::bestGuess(mBase->mainExecutablePath(), &ctx));
			}
		}
	default:
		return NULL;
	}
}


} // end namespace CodeSigning
} // end namespace Security
