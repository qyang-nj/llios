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
// codedirectory - format and operations for code signing "code directory" structures
//
#include "codedirectory.h"
#include "csutilities.h"
#include "CSCommonPriv.h"

using namespace UnixPlusPlus;


namespace Security {
namespace CodeSigning {


//
// Highest understood special slot in this CodeDirectory.
//
CodeDirectory::SpecialSlot CodeDirectory::maxSpecialSlot() const
{
	SpecialSlot slot = this->nSpecialSlots;
	if (slot > cdSlotMax)
		slot = cdSlotMax;
	return slot;
}


//
// Canonical filesystem names for select slot numbers.
// These are variously used for filenames, extended attribute names, etc.
// to get some consistency in naming. These are for storing signing-related
// data; they have no bearing on the actual hash slots in the CodeDirectory.
//
const char *CodeDirectory::canonicalSlotName(SpecialSlot slot)
{
	switch (slot) {
	case cdRequirementsSlot:
		return kSecCS_REQUIREMENTSFILE;
	case cdResourceDirSlot:
		return kSecCS_RESOURCEDIRFILE;
	case cdCodeDirectorySlot:
		return kSecCS_CODEDIRECTORYFILE;
	case cdSignatureSlot:
		return kSecCS_SIGNATUREFILE;
	case cdApplicationSlot:
		return kSecCS_APPLICATIONFILE;
	case cdEntitlementSlot:
		return kSecCS_ENTITLEMENTFILE;
	default:
		return NULL;
	}
}


//
// Canonical attributes of SpecialSlots.
//
unsigned CodeDirectory::slotAttributes(SpecialSlot slot)
{
	switch (slot) {
	case cdRequirementsSlot:
		return cdComponentIsBlob; // global
	case cdCodeDirectorySlot:
		return cdComponentPerArchitecture | cdComponentIsBlob;
	case cdSignatureSlot:
		return cdComponentPerArchitecture; // raw
	case cdEntitlementSlot:
		return cdComponentIsBlob; // global
	case cdIdentificationSlot:
		return cdComponentPerArchitecture; // raw
	default:
		return 0; // global, raw
	}
}


//
// Symbolic names for code directory special slots.
// These are only used for debug output. They are not API-official.
// Needs to be coordinated with the cd*Slot enumeration in codedirectory.h.
//
#if !defined(NDEBUG)
const char * const CodeDirectory::debugSlotName[] = {
	"codedirectory",
	"info",
	"requirements",
	"resources",
	"application",
	"entitlement"
};
#endif //NDEBUG


//
// Check a CodeDirectory for basic integrity. This should ensure that the
// version is understood by our code, and that the internal structure
// (offsets etc.) is intact. In particular, it must make sure that no offsets
// point outside the CodeDirectory.
// Throws if the directory is corrupted or out of versioning bounds.
// Returns if the version is usable (perhaps with degraded features due to
// compatibility hacks).
//
// Note: There are some things we don't bother checking because they won't
// cause crashes, and will just be flagged as nonsense later. For example,
// a Bad Guy could overlap the identifier and hash fields, which is nonsense
// but not dangerous.
//
void CodeDirectory::checkIntegrity() const
{
	// check version for support
	if (!this->validateBlob())
		MacOSError::throwMe(errSecCSSignatureInvalid);	// busted
	if (version > compatibilityLimit)
		MacOSError::throwMe(errSecCSSignatureUnsupported);	// too new - no clue
	if (version < earliestVersion)
		MacOSError::throwMe(errSecCSSignatureUnsupported);	// too old - can't support
	if (version > currentVersion)
		secdebug("codedir", "%p version 0x%x newer than current 0x%x",
			this, uint32_t(version), currentVersion);
	
	// now check interior offsets for validity
	if (!stringAt(identOffset))
		MacOSError::throwMe(errSecCSSignatureFailed); // identifier out of blob range
	if (!contains(hashOffset - uint64_t(hashSize) * nSpecialSlots, hashSize * (uint64_t(nSpecialSlots) + nCodeSlots)))
		MacOSError::throwMe(errSecCSSignatureFailed); // hash array out of blob range
	if (const Scatter *scatter = this->scatterVector()) {
		// the optional scatter vector is terminated with an element having (count == 0)
		unsigned int pagesConsumed = 0;
		while (scatter->count) {
			if (!contains(scatter, sizeof(Scatter)))
				MacOSError::throwMe(errSecCSSignatureFailed);
			pagesConsumed += scatter->count;
			scatter++;
		}
		if (!contains(scatter, sizeof(Scatter)))			// (even sentinel must be in range)
			MacOSError::throwMe(errSecCSSignatureFailed);
		if (!contains((*this)[pagesConsumed-1], hashSize))	// referenced too many main hash slots
			MacOSError::throwMe(errSecCSSignatureFailed);
	}
}


//
// Validate a slot against data in memory.
//
bool CodeDirectory::validateSlot(const void *data, size_t length, Slot slot) const
{
	secdebug("codedir", "%p validating slot %d", this, int(slot));
	MakeHash<CodeDirectory> hasher(this);
	Hashing::Byte digest[hasher->digestLength()];
	generateHash(hasher, data, length, digest);
	return memcmp(digest, (*this)[slot], hasher->digestLength()) == 0;
}


//
// Validate a slot against the contents of an open file. At most 'length' bytes
// will be read from the file.
//
bool CodeDirectory::validateSlot(FileDesc fd, size_t length, Slot slot) const
{
	MakeHash<CodeDirectory> hasher(this);
	Hashing::Byte digest[hasher->digestLength()];
	generateHash(hasher, fd, digest, length);
	return memcmp(digest, (*this)[slot], hasher->digestLength()) == 0;
}


//
// Check whether a particular slot is present.
// Absense is indicated by either a zero hash, or by lying outside
// the slot range.
//
bool CodeDirectory::slotIsPresent(Slot slot) const
{
	if (slot >= -Slot(nSpecialSlots) && slot < Slot(nCodeSlots)) {
		const Hashing::Byte *digest = (*this)[slot];
		for (unsigned n = 0; n < hashSize; n++)
			if (digest[n])
				return true;	// non-zero digest => present
	}
	return false;	// absent
}


//
// Given a hash type code, create an appropriate subclass of DynamicHash
// and return it. The caller owns the object and  must delete it when done.
// This function never returns NULL. It throws if the hashType is unsuupported,
// or if there's an error creating the hasher.
//
DynamicHash *CodeDirectory::hashFor(HashAlgorithm hashType)
{
	CCDigestAlg alg;
	switch (hashType) {
	case kSecCodeSignatureHashSHA1:						alg = kCCDigestSHA1; break;
	case kSecCodeSignatureHashSHA256:					alg = kCCDigestSHA256; break;
	case kSecCodeSignatureHashPrestandardSkein160x256:	alg = kCCDigestSkein160; break;
	case kSecCodeSignatureHashPrestandardSkein256x512:	alg = kCCDigestSkein256; break;
	default:
		MacOSError::throwMe(errSecCSSignatureUnsupported);
	}
	return new CCHashInstance(alg);
}


//
// Hash the next limit bytes of a file and return the digest.
// If the file is shorter, hash as much as you can.
// Limit==0 means unlimited (to end of file).
// Return how many bytes were actually hashed.
// Throw on any errors.
//
size_t CodeDirectory::generateHash(DynamicHash *hasher, FileDesc fd, Hashing::Byte *digest, size_t limit)
{
	size_t size = hashFileData(fd, hasher, limit);
	hasher->finish(digest);
	return size;
}


//
// Ditto, but hash a memory buffer instead.
//
size_t CodeDirectory::generateHash(DynamicHash *hasher, const void *data, size_t length, Hashing::Byte *digest)
{
	hasher->update(data, length);
	hasher->finish(digest);
	return length;
}


}	// CodeSigning
}	// Security


//
// Canonical text form for user-settable code directory flags.
// Note: This table is actually exported from Security.framework.
//
const SecCodeDirectoryFlagTable kSecCodeDirectoryFlagTable[] = {
	{ "host",		kSecCodeSignatureHost,			true },
	{ "adhoc",		kSecCodeSignatureAdhoc,			false },
	{ "hard",		kSecCodeSignatureForceHard,		true },
	{ "kill",		kSecCodeSignatureForceKill,		true },
	{ "expires",	kSecCodeSignatureForceExpiration, true },
	{ NULL }
};
