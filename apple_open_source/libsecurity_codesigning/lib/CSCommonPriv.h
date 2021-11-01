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

/*!
	@header CSCommonPriv
	SecStaticCodePriv is the private counter-part to CSCommon. Its contents are not
	official API, and are subject to change without notice.
*/
#ifndef _H_CSCOMMONPRIV
#define _H_CSCOMMONPRIV

#include <Security/CSCommon.h>

#ifdef __cplusplus
extern "C" {
#endif


/*!
	@typedef SecCodeDirectoryFlagTable
	This constant array can be used to translate between names and values
	of CodeDirectory flag bits. The table ends with an entry with NULL name.
	The elements are in no particular order.
	@field name The official text name of the flag.
	@field value The binary value of the flag.
	@field signable True if the flag can be specified during signing. False if it is set
	internally and can only be read from a signature.
 */
typedef struct {
	const char *name;
	uint32_t value;
	bool signable;
} SecCodeDirectoryFlagTable;

extern const SecCodeDirectoryFlagTable kSecCodeDirectoryFlagTable[];


/*!
	Blob types (magic numbers) for blobs used by Code Signing.
	
	@constant kSecCodeMagicRequirement Magic number for individual code requirements.
	@constant kSecCodeMagicRequirementSet Magic number for a collection of
	individual code requirements, indexed by requirement type. This is used
	for internal requirement sets.
	@constant kSecCodeMagicCodeDirectory Magic number for a CodeDirectory.
	@constant kSecCodeMagicEmbeddedSignature Magic number for a SuperBlob
	containing all the signing components that are usually embedded within
	a main executable.
	@constant kSecCodeMagicDetachedSignature Magic number for a SuperBlob that
	contains all the data for all architectures of a signature, including any
	data that is usually written to separate files. This is the format of
	detached signatures if the program is capable of having multiple architectures.
	@constant kSecCodeMagicEntitlement Magic number for a standard entitlement blob.
	@constant kSecCodeMagicByte The first byte (in NBO) shared by all these magic
	numbers. This is not a valid ASCII character; test for this to distinguish
	between text and binary data if you expect a code signing-related binary blob.
 */

enum {
	kSecCodeMagicRequirement = 0xfade0c00,		/* single requirement */
	kSecCodeMagicRequirementSet = 0xfade0c01,	/* requirement set */
	kSecCodeMagicCodeDirectory = 0xfade0c02,	/* CodeDirectory */
	kSecCodeMagicEmbeddedSignature = 0xfade0cc0, /* single-architecture embedded signature */
	kSecCodeMagicDetachedSignature = 0xfade0cc1, /* detached multi-architecture signature */
	kSecCodeMagicEntitlement = 0xfade7171,		/* entitlement blob */
	
	kSecCodeMagicByte = 0xfa					/* shared first byte */
};


/*!
	Types of cryptographic digests (hashes) used to hold code signatures
	together.

	Each combination of type, length, and other parameters is a separate
	hash type; we don't understand "families" here.

	These type codes govern the digest links that connect a CodeDirectory
	to its subordinate data structures (code pages, resources, etc.)
	They do not directly control other uses of hashes (such as the
	hash-of-CodeDirectory identifiers used in requirements).
 */
enum {
	kSecCodeSignatureNoHash							=  0,	/* null value */
	kSecCodeSignatureHashSHA1						=  1,	/* SHA-1 */
	kSecCodeSignatureHashSHA256						=  2,	/* SHA-256 */
	kSecCodeSignatureHashPrestandardSkein160x256	= 32,	/* Skein, 160 bits, 256 bit pool */
	kSecCodeSignatureHashPrestandardSkein256x512	= 33,	/* Skein, 256 bits, 512 bit pool */
	
	kSecCodeSignatureDefaultDigestAlgorithm = kSecCodeSignatureHashSHA1
};


#ifdef __cplusplus
}
#endif

#endif //_H_CSCOMMON
