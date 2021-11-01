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
// requirement - Code Requirement Blob description
//
#ifndef _H_REQUIREMENT
#define _H_REQUIREMENT

#include <security_utilities/blob.h>
#include <security_utilities/superblob.h>
#include <security_utilities/hashing.h>
#include <Security/CodeSigning.h>
#include "codedirectory.h"
#include <map>

namespace Security {
namespace CodeSigning {


//
// Single requirement.
// This is a contiguous binary blob, starting with this header
// and followed by binary expr-code. All links within the blob
// are offset-relative to the start of the header.
// This is designed to be a binary stable format. Note that we restrict
// outselves to 4GB maximum size (4 byte size/offset), and we expect real
// Requirement blobs to be fairly small (a few kilobytes at most).
//
// The "kind" field allows for adding different kinds of Requirements altogether
// in the future. We expect to stay within the framework of "opExpr" requirements,
// but it never hurts to have a way out.
//
class Requirement: public Blob<Requirement, 0xfade0c00> {
public:	
	class Maker;				// makes Requirement blobs
	class Context;				// evaluation context
	class Reader;				// structured reader
	class Interpreter;			// evaluation engine

	// different forms of Requirements. Right now, we only support exprForm ("opExprs")
	enum Kind {
		exprForm = 1			// prefix expr form
	};
	
	void kind(Kind k) { mKind = k; }
	Kind kind() const { return Kind(uint32_t(mKind)); }
	
	// validate this requirement against a code context
	void validate(const Context &ctx, OSStatus failure = errSecCSReqFailed) const;	// throws on all failures
	bool validates(const Context &ctx, OSStatus failure = errSecCSReqFailed) const;	// returns on clean miss
	
	// certificate positions (within a standard certificate chain)
	static const int leafCert = 0;		// index for leaf (first in chain)
	static const int anchorCert = -1;	// index for anchor (last in chain)
	
	// the SHA1 hash of the canonical "Apple Anchor", i.e. the X509 Anchor
	// that is considered "Apple's anchor certificate", as defined by hashOfCertificate().
	static const SHA1::Digest &appleAnchorHash();
#if defined(TEST_APPLE_ANCHOR)
	static const char testAppleAnchorEnv[];
	static const SHA1::Digest &testAppleAnchorHash();
#endif //TEST_APPLE_ANCHOR
	
	// common alignment rule for all requirement forms
	static const size_t baseAlignment = sizeof(uint32_t); // (we might as well say "four")
	
    // canonical (source) names of Requirement types (matched to SecRequirementType in CSCommon.h)
    static const char *const typeNames[];
	
	IFDUMP(void dump() const);

private:
	Endian<uint32_t> mKind;			// expression kind
};


//
// An interpretation context
//
class Requirement::Context {
protected:
	Context()
		: certs(NULL), info(NULL), entitlements(NULL), identifier(""), directory(NULL) { }

public:
	Context(CFArrayRef certChain, CFDictionaryRef infoDict, CFDictionaryRef entitlementDict,
			const std::string &ident, const CodeDirectory *dir)
		: certs(certChain), info(infoDict), entitlements(entitlementDict), identifier(ident), directory(dir) { }

	CFArrayRef certs;								// certificate chain
	CFDictionaryRef info;							// Info.plist
	CFDictionaryRef entitlements;					// entitlement plist
	std::string identifier;						// signing identifier
	const CodeDirectory *directory;				// CodeDirectory

	SecCertificateRef cert(int ix) const;			// get a cert from the cert chain (NULL if not found)
	unsigned int certCount() const;				// length of cert chain (including root)
};


//
// exprForm opcodes.
//
// Opcodes are broken into flags in the (HBO) high byte, and an opcode value
// in the remaining 24 bits. Note that opcodes will remain fairly small
// (almost certainly <60000), so we have the third byte to play around with
// in the future, if needed. For now, small opcodes effective reserve this byte
// as zero.
// The flag byte allows for limited understanding of unknown opcodes. It allows
// the interpreter to use the known opcode parts of the program while semi-creatively
// disregarding the parts it doesn't know about. An unrecognized opcode with zero
// flag byte causes evaluation to categorically fail, since the semantics of such
// an opcode cannot safely be predicted.
//
enum {
	// semantic bits or'ed into the opcode
	opFlagMask =	 0xFF000000,	// high bit flags
	opGenericFalse = 0x80000000,	// has size field; okay to default to false
	opGenericSkip =  0x40000000,	// has size field; skip and continue
};

enum ExprOp {
	opFalse,						// unconditionally false
	opTrue,							// unconditionally true
	opIdent,						// match canonical code [string]
	opAppleAnchor,					// signed by Apple as Apple's product
	opAnchorHash,					// match anchor [cert hash]
	opInfoKeyValue,					// *legacy* - use opInfoKeyField [key; value]
	opAnd,							// binary prefix expr AND expr [expr; expr]
	opOr,							// binary prefix expr OR expr [expr; expr]
	opCDHash,						// match hash of CodeDirectory directly [cd hash]
	opNot,							// logical inverse [expr]
	opInfoKeyField,					// Info.plist key field [string; match suffix]
	opCertField,					// Certificate field [cert index; field name; match suffix]
	opTrustedCert,					// require trust settings to approve one particular cert [cert index]
	opTrustedCerts,					// require trust settings to approve the cert chain
	opCertGeneric,					// Certificate component by OID [cert index; oid; match suffix]
	opAppleGenericAnchor,			// signed by Apple in any capacity
	opEntitlementField,				// entitlement dictionary field [string; match suffix]
	opCertPolicy,					// Certificate policy by OID [cert index; oid; match suffix]
	opNamedAnchor,					// named anchor type
	opNamedCode,					// named subroutine
	exprOpCount						// (total opcode count in use)
};

// match suffix opcodes
enum MatchOperation {
	matchExists,					// anything but explicit "false" - no value stored
	matchEqual,						// equal (CFEqual)
	matchContains,					// partial match (substring)
	matchBeginsWith,				// partial match (initial substring)
	matchEndsWith,					// partial match (terminal substring)
	matchLessThan,					// less than (string with numeric comparison)
	matchGreaterThan,				// greater than (string with numeric comparison)
	matchLessEqual,					// less or equal (string with numeric comparison)
	matchGreaterEqual,				// greater or equal (string with numeric comparison)
};


//
// We keep Requirement groups in SuperBlobs, indexed by SecRequirementType
//
typedef SuperBlob<0xfade0c01> Requirements;


//
// Byte order flippers
//
inline CodeSigning::ExprOp h2n(CodeSigning::ExprOp op)
{
	uint32_t intOp = (uint32_t) op;
	return (CodeSigning::ExprOp) ::h2n(intOp);
}

inline CodeSigning::ExprOp n2h(CodeSigning::ExprOp op)
{
	uint32_t intOp = (uint32_t) op;
	return (CodeSigning::ExprOp) ::n2h(intOp);
}


inline CodeSigning::MatchOperation h2n(CodeSigning::MatchOperation op)
{
	return CodeSigning::MatchOperation(::h2n((uint32_t) op));
}

inline CodeSigning::MatchOperation n2h(CodeSigning::MatchOperation op)
{
	return CodeSigning::MatchOperation(::n2h((uint32_t) op));
}


}	// CodeSigning
}	// Security

#endif //_H_REQUIREMENT
