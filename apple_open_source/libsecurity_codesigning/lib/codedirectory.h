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
// A CodeDirectory is the top level object describing a particular instance
// of (static) code. It contains hashes of other objects that further describe
// parts of that code; these hashes hold the various pieces together.
//
// This means that if you reliably ascertain the contents of a CodeDirectory,
// you can verify the integrity of the entire code object it represents - the
// CodeDirectory can stand as a proxy for that code.
//
// Code signatures usually use CMS to sign the CodeDirectory to form full
// signature blobs; ad-hoc signatures simply record the SHA-1 hash of the
// CodeDirectory directly. The SHA-1 of the CodeDirectory is also widely
// used as concordance for a particular code instance - in essence, for
// different processes (or a process and the kernel) to "compare notes"
// to make sure they refer to the same code.
//
#ifndef _H_CODEDIRECTORY
#define _H_CODEDIRECTORY

#include <security_utilities/unix++.h>
#include <security_utilities/blob.h>
#include <security_utilities/cfutilities.h>
#include <security_utilities/hashing.h>
#include <Security/CSCommonPriv.h>


namespace Security {
namespace CodeSigning {


//
// Conventional string names for various code signature components.
// Depending on storage, these may end up as filenames, extended attribute names, etc.
//
#define kSecCS_CODEDIRECTORYFILE	"CodeDirectory"		// CodeDirectory
#define kSecCS_SIGNATUREFILE		"CodeSignature"		// CMS Signature
#define kSecCS_REQUIREMENTSFILE		"CodeRequirements"	// internal requirements
#define kSecCS_RESOURCEDIRFILE		"CodeResources"		// resource directory
#define kSecCS_APPLICATIONFILE		"CodeApplication"	// application-specific resource
#define kSecCS_ENTITLEMENTFILE		"CodeEntitlements"	// entitlement configuration


//
// Special hash slot values. In a CodeDirectory, these show up at negative slot
// indices. This enumeration is also used widely in various internal APIs, and as
// type values in embedded SuperBlobs.
//
// How to add a new special slot type:
//	1. Add the new name at the end of the primary or virtual slot array (below).
//	2a. For slots representing existing code pieces, follow the ball for cdInfoSlot.
//	2b. For slots representing global signature components, follow the ball for cdResourceDirSlot.
//	2c. For slots representing per-architecture signature components, follow the ball for cdEntitlementSlot.
// ("Follow the ball" -> Global search for that name and do likewise.)
//
enum {
	//
	// Primary slot numbers.
	// These values are potentially present in the CodeDirectory hash array
	// under their negative values. They are also used in APIs and SuperBlobs.
	// Note that zero must not be used for these (it's page 0 of the main code array),
	// and it is important to assign contiguous (very) small values for them.
	//
	cdInfoSlot = 1,						// Info.plist
	cdRequirementsSlot = 2,				// internal requirements
	cdResourceDirSlot = 3,				// resource directory
	cdApplicationSlot = 4,				// Application specific slot
	cdEntitlementSlot = 5,				// embedded entitlement configuration
	// (add further primary slot numbers here)

	cdSlotCount,						// total number of special slots (+1 for slot 0)
	cdSlotMax = cdSlotCount - 1,		// highest special slot number (as a positive number)
	
	//
	// Virtual slot numbers.
	// These values are NOT used in the CodeDirectory hash array. They are used as
	// internal API identifiers and as types in SuperBlobs.
	// Zero is okay to use here; and we assign that to the CodeDirectory itself so
	// it shows up first in (properly sorted) SuperBlob indices. The rest of the
	// numbers is set Far Away so the primary slot set can expand safely.
	// It's okay to have large gaps in these assignments.
	//
	cdCodeDirectorySlot = 0,			// CodeDirectory
	cdSignatureSlot = 0x10000,			// CMS signature
	cdIdentificationSlot,				// identification blob
	// (add further virtual slot numbers here)
};


//
// Special hash slot attributes.
// This is a central description of attributes of each slot.
// Various places in Code Signing pick up those attributes and act accordingly.
//
enum {
	cdComponentPerArchitecture = 1,			// slot value differs for each Mach-O architecture
	cdComponentIsBlob = 2,					// slot value is a Blob (need not be BlobWrapped)
};


//
// A CodeDirectory is a typed Blob describing the secured pieces of a program.
// This structure describes the common header and provides access to the variable-size
// elements packed after it. For help in constructing a CodeDirectory, use the nested
// Builder class.
//
// At the heart of a CodeDirectory lies a packed array of hash digests.
// The array's zero-index element is at offset hashOffset, and the array covers
// elements in the range [-nSpecialSlots .. nCodeSlots-1]. Non-negative indices
// denote pages of the main executable. Negative indices indicate "special" hashes,
// each of a different thing (see cd*Slot constants above).
// Special slots that are in range but not present are zeroed out. Unallocated special
// slots are also presumed absent; this is not an error. (Thus the range of special
// slots can be extended at will.)
//
// HOW TO MANAGE COMPATIBILITY:
// Each CodeDirectory has a format (compatibility) version. Two constants control
// versioning:
//	* currentVersion is the version used for newly created CodeDirectories.
//  * compatibilityLimit is the highest version the code will accept as compatible.
// Test for version < currentVersion to detect old formats that may need special
// handling; this is done in checkIntegrity(). The current code rejects versions
// below earliestVersion.
// Break backward compatibility by rejecting versions that are unsuitable.
// Accept currentVersion < version <= compatibilityLimit as versions newer than
// those understood by this code but engineered (by newer code) to be backward
// compatible. Reject version > compatibilityLimit as incomprehensible gibberish.
//
// When creating a new version, increment currentVersion. When adding new fixed fields,
// just append them; the flex fields will shift to make room. To add new flex fields,
// add a fixed field containing the new field's offset and add suitable computations
// to the Builder to place the new data (right) before the hash array. Remember to check
// for offset in-range in checkIntegrity(). Older code will then simply ignore your
// new fields on load/read.
// Add flag bits to the existing flags field to add features that step outside
// of the linear versioning stream. Leave the 'spare' fields alone unless you need
// something extraordinarily weird - they're meant to be the final escape when everything
// else fails.
// As you create new versions, consider moving the compatibilityLimit out to open up
// new room for backward compatibility.
// To break backward compatibility intentionally, move currentVersion beyond the
// old compatibilityLimit (and move compatibilityLimit further out).
//
class CodeDirectory: public Blob<CodeDirectory, kSecCodeMagicCodeDirectory> {
public:
	Endian<uint32_t> version;		// compatibility version
	Endian<uint32_t> flags;			// setup and mode flags
	Endian<uint32_t> hashOffset;	// offset of hash slot element at index zero
	Endian<uint32_t> identOffset;	// offset of identifier string
	Endian<uint32_t> nSpecialSlots;	// number of special hash slots
	Endian<uint32_t> nCodeSlots;	// number of ordinary (code) hash slots
	Endian<uint32_t> codeLimit;		// limit to main image signature range
	uint8_t hashSize;				// size of each hash digest (bytes)
	uint8_t hashType;				// type of hash (kSecCodeSignatureHash* constants)
	uint8_t spare1;					// unused (must be zero)
	uint8_t	pageSize;				// log2(page size in bytes); 0 => infinite
	Endian<uint32_t> spare2;		// unused (must be zero)
	Endian<uint32_t> scatterOffset;	// offset of optional scatter vector (zero if absent)
	
	// works with the version field; see comments above
	static const uint32_t currentVersion = 0x20100;		// "version 2.1"
	static const uint32_t compatibilityLimit = 0x2F000;	// "version 3 with wiggle room"
	
	static const uint32_t earliestVersion = 0x20001;	// earliest supported version
	static const uint32_t supportsScatter = 0x20100;	// first version to support scatter option
	
	void checkIntegrity() const;	// throws if inconsistent or unsupported version

	typedef uint32_t HashAlgorithm;	// types of internal glue hashes
	typedef int Slot;				// slot index (negative for special slots)
	typedef unsigned int SpecialSlot; // positive special slot index (not for code slots)
	
	const char *identifier() const { return at<const char>(identOffset); }
	char *identifier() { return at<char>(identOffset); }

	// main hash array access
	SpecialSlot maxSpecialSlot() const;
		
	unsigned char *operator [] (Slot slot)
	{
		assert(slot >= int(-nSpecialSlots) && slot < int(nCodeSlots));
		return at<unsigned char>(hashOffset) + hashSize * slot;
	}
	
	const unsigned char *operator [] (Slot slot) const
	{
		assert(slot >= int(-nSpecialSlots) && slot < int(nCodeSlots));
		return at<unsigned char>(hashOffset) + hashSize * slot;
	}
	
	//
	// The main page hash array can be "scattered" across the code file
	// by specifying an array of Scatter elements, terminated with an
	// element whose count field is zero.
	// The scatter vector is optional; if absent, the hash array covers
	// a single contiguous range of pages. CodeDirectory versions below
	// supportsScatter never have scatter vectors (they lack the scatterOffset field).
	// 
	struct Scatter {
		Endian<uint32_t> count;			// number of pages; zero for sentinel (only)
		Endian<uint32_t> base;			// first page number
		Endian<uint64_t> targetOffset;	// byte offset in target
		Endian<uint64_t> spare;			// reserved (must be zero)
	};
	Scatter *scatterVector()	// first scatter vector element (NULL if none)
		{ return (version >= supportsScatter && scatterOffset) ? at<Scatter>(scatterOffset) : NULL; }
	const Scatter *scatterVector() const
		{ return (version >= supportsScatter && scatterOffset) ? at<const Scatter>(scatterOffset) : NULL; }
	
public:
	bool validateSlot(const void *data, size_t size, Slot slot) const;			// validate memory buffer against page slot
	bool validateSlot(UnixPlusPlus::FileDesc fd, size_t size, Slot slot) const;	// read and validate file
	bool slotIsPresent(Slot slot) const;
	
	class Builder;

public:
	static DynamicHash *hashFor(HashAlgorithm hashType);		// create a DynamicHash subclass for (hashType) digests
	DynamicHash *getHash() const { return hashFor(this->hashType); } // make one for me
	
protected:
	static size_t generateHash(DynamicHash *hash, UnixPlusPlus::FileDesc fd, Hashing::Byte *digest, size_t limit = 0); // hash to count or end of file
	static size_t generateHash(DynamicHash *hash, const void *data, size_t length, Hashing::Byte *digest); // hash data buffer
	
public:
	//
	// Information about SpecialSlots.
	// This specifies meta-data about slots themselves;
	// it does not work with the contents of hash slots.
	//
	static const char *canonicalSlotName(SpecialSlot slot);
	static unsigned slotAttributes(SpecialSlot slot);
	IFDEBUG(static const char * const debugSlotName[]);
};


}	// CodeSigning
}	// Security


#endif //_H_CODEDIRECTORY
