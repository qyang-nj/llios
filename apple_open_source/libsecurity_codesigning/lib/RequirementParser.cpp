/* $ANTLR 2.7.7 (20120228): "requirements.grammar" -> "RequirementParser.cpp"$ */
#include "RequirementParser.hpp"
#include <antlr/NoViableAltException.hpp>
#include <antlr/SemanticException.hpp>
#include <antlr/ASTFactory.hpp>

#include "requirement.h"
#include "reqmaker.h"
#include "csutilities.h"
#include <security_utilities/cfutilities.h>
#include <security_utilities/hashing.h>
#include <security_cdsa_utilities/cssmdata.h>	// OID coding
using namespace CodeSigning;
typedef Requirement::Maker Maker;

ANTLR_BEGIN_NAMESPACE(Security_CodeSigning)

	//
	// Collect error messages.
	// Note that the immediate caller takes the absence of collected error messages
	// to indicate compilation success.
	//
	void RequirementParser::reportError(const antlr::RecognitionException &ex)
	{
		errors += ex.toString() + "\n";
	}
	
	void RequirementParser::reportError(const std::string &s)
	{
		errors += s + "\n";
	}

	
	//
	// Parser helper functions
	//
	string RequirementParser::hexString(const string &s)
	{
		if (s.size() % 2)
			throw antlr::SemanticException("odd number of digits");
		const char *p = s.data();
		string result;
		for (unsigned n = 0; n < s.length(); n += 2) {
			char c;
			sscanf(p+n, "%2hhx", &c);
			result.push_back(c);
		}
		return result;
	}

	void RequirementParser::hashString(const string &s, SHA1::Digest hash)
	{
		if (s.size() != 2 * SHA1::digestLength)
			throw antlr::SemanticException("invalid hash length");
		memcpy(hash, hexString(s).data(), SHA1::digestLength);
	}
	
	static const char *matchPrefix(const string &key, const char *prefix)
	{
		unsigned pLength = strlen(prefix);
		if (!key.compare(0, pLength, prefix, 0, pLength))
			return key.c_str() + pLength;
		else
			return NULL;
	}
	
	void RequirementParser::certMatchOperation(Maker &maker, int32_t slot, string key)
	{
		if (matchPrefix(key, "subject.")) {
			maker.put(opCertField);
			maker.put(slot);
			maker.put(key);
		} else if (const char *oids = matchPrefix(key, "field.")) {
			maker.put(opCertGeneric);
			maker.put(slot);
			CssmAutoData oid(Allocator::standard()); oid.fromOid(oids);
			maker.putData(oid.data(), oid.length());
		} else if (const char *oids = matchPrefix(key, "extension.")) {
			maker.put(opCertGeneric);
			maker.put(slot);
			CssmAutoData oid(Allocator::standard()); oid.fromOid(oids);
			maker.putData(oid.data(), oid.length());
		} else if (const char *oids = matchPrefix(key, "policy.")) {
			maker.put(opCertPolicy);
			maker.put(slot);
			CssmAutoData oid(Allocator::standard()); oid.fromOid(oids);
			maker.putData(oid.data(), oid.length());
		} else {
			throw antlr::SemanticException(key + ": unrecognized certificate field");
		}
	}

RequirementParser::RequirementParser(antlr::TokenBuffer& tokenBuf, int k)
: antlr::LLkParser(tokenBuf,k)
{
}

RequirementParser::RequirementParser(antlr::TokenBuffer& tokenBuf)
: antlr::LLkParser(tokenBuf,2)
{
}

RequirementParser::RequirementParser(antlr::TokenStream& lexer, int k)
: antlr::LLkParser(lexer,k)
{
}

RequirementParser::RequirementParser(antlr::TokenStream& lexer)
: antlr::LLkParser(lexer,2)
{
}

RequirementParser::RequirementParser(const antlr::ParserSharedInputState& state)
: antlr::LLkParser(state,2)
{
}

BlobCore * RequirementParser::autosense() {
	BlobCore *result = NULL;
	
	try {      // for error handling
		switch ( LA(1)) {
		case LPAREN:
		case NOT:
		case LITERAL_always:
		case LITERAL_true:
		case LITERAL_never:
		case LITERAL_false:
		case LITERAL_identifier:
		case LITERAL_cdhash:
		case LITERAL_anchor:
		case LITERAL_certificate:
		case LITERAL_cert:
		case LITERAL_info:
		case LITERAL_entitlement:
		{
			result=requirement();
			break;
		}
		case LITERAL_guest:
		case LITERAL_host:
		case LITERAL_designated:
		case LITERAL_library:
		case LITERAL_plugin:
		case INTEGER:
		{
			result=requirementSet();
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_0);
	}
	return result;
}

Requirement * RequirementParser::requirement() {
	Requirement *result = NULL;
	
	try {      // for error handling
		result=requirementElement();
		match(antlr::Token::EOF_TYPE);
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_0);
	}
	return result;
}

Requirements * RequirementParser::requirementSet() {
	Requirements *result = NULL;
	Requirements::Maker maker;
	
	try {      // for error handling
		{ // ( ... )+
		int _cnt4=0;
		for (;;) {
			if (((LA(1) >= LITERAL_guest && LA(1) <= INTEGER))) {
				uint32_t t; Requirement *req;
				t=requirementType();
				match(ARROW);
				req=requirementElement();
				maker.add(t, req);
			}
			else {
				if ( _cnt4>=1 ) { goto _loop4; } else {throw antlr::NoViableAltException(LT(1), getFilename());}
			}
			
			_cnt4++;
		}
		_loop4:;
		}  // ( ... )+
		result = errors.empty() ? maker() : NULL;
		match(antlr::Token::EOF_TYPE);
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_0);
	}
	return result;
}

uint32_t  RequirementParser::requirementType() {
	uint32_t type = kSecInvalidRequirementType;
	antlr::RefToken  stype = antlr::nullToken;
	
	try {      // for error handling
		switch ( LA(1)) {
		case LITERAL_guest:
		{
			match(LITERAL_guest);
			type = kSecGuestRequirementType;
			break;
		}
		case LITERAL_host:
		{
			match(LITERAL_host);
			type = kSecHostRequirementType;
			break;
		}
		case LITERAL_designated:
		{
			match(LITERAL_designated);
			type = kSecDesignatedRequirementType;
			break;
		}
		case LITERAL_library:
		{
			match(LITERAL_library);
			type = kSecLibraryRequirementType;
			break;
		}
		case LITERAL_plugin:
		{
			match(LITERAL_plugin);
			type = kSecPluginRequirementType;
			break;
		}
		case INTEGER:
		{
			stype = LT(1);
			match(INTEGER);
			type = atol(stype->getText().c_str());
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_1);
	}
	return type;
}

Requirement * RequirementParser::requirementElement() {
	Requirement *result = NULL;
	Requirement::Maker maker;
	
	try {      // for error handling
		expr(maker);
		result = maker();
		{ // ( ... )*
		for (;;) {
			if ((LA(1) == SEMI)) {
				fluff();
			}
			else {
				goto _loop9;
			}
			
		}
		_loop9:;
		} // ( ... )*
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_2);
	}
	return result;
}

void RequirementParser::expr(
	Maker &maker
) {
	Maker::Label label(maker);
	
	try {      // for error handling
		term(maker);
		{ // ( ... )*
		for (;;) {
			if ((LA(1) == LITERAL_or)) {
				match(LITERAL_or);
				maker.insert<ExprOp>(label) = opOr;
				term(maker);
			}
			else {
				goto _loop12;
			}
			
		}
		_loop12:;
		} // ( ... )*
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_3);
	}
}

void RequirementParser::fluff() {
	
	try {      // for error handling
		match(SEMI);
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_4);
	}
}

void RequirementParser::term(
	Maker &maker
) {
	Maker::Label label(maker);
	
	try {      // for error handling
		primary(maker);
		{ // ( ... )*
		for (;;) {
			if ((LA(1) == LITERAL_and)) {
				match(LITERAL_and);
				maker.insert<ExprOp>(label) = opAnd;
				primary(maker);
			}
			else {
				goto _loop15;
			}
			
		}
		_loop15:;
		} // ( ... )*
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_5);
	}
}

void RequirementParser::primary(
	Maker &maker
) {
	
	try {      // for error handling
		switch ( LA(1)) {
		case NOT:
		{
			match(NOT);
			maker.put(opNot);
			primary(maker);
			break;
		}
		case LITERAL_always:
		case LITERAL_true:
		{
			{
			switch ( LA(1)) {
			case LITERAL_always:
			{
				match(LITERAL_always);
				break;
			}
			case LITERAL_true:
			{
				match(LITERAL_true);
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
			maker.put(opTrue);
			break;
		}
		case LITERAL_never:
		case LITERAL_false:
		{
			{
			switch ( LA(1)) {
			case LITERAL_never:
			{
				match(LITERAL_never);
				break;
			}
			case LITERAL_false:
			{
				match(LITERAL_false);
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
			maker.put(opFalse);
			break;
		}
		case LITERAL_anchor:
		case LITERAL_certificate:
		case LITERAL_cert:
		{
			certspec(maker);
			break;
		}
		case LITERAL_info:
		{
			infospec(maker);
			break;
		}
		case LITERAL_entitlement:
		{
			entitlementspec(maker);
			break;
		}
		case LITERAL_identifier:
		{
			match(LITERAL_identifier);
			string code;
			eql();
			code=identifierString();
			maker.ident(code);
			break;
		}
		case LITERAL_cdhash:
		{
			match(LITERAL_cdhash);
			SHA1::Digest digest;
			eql();
			hash(digest);
			maker.cdhash(digest);
			break;
		}
		default:
			if ((LA(1) == LPAREN) && (_tokenSet_6.member(LA(2)))) {
				match(LPAREN);
				expr(maker);
				match(RPAREN);
			}
			else if ((LA(1) == LPAREN) && (LA(2) == DOTKEY || LA(2) == STRING)) {
				match(LPAREN);
				string name;
				name=identifierString();
				match(RPAREN);
				maker.put(opNamedCode); maker.put(name);
			}
		else {
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

void RequirementParser::certspec(
	Maker &maker
) {
	
	try {      // for error handling
		if ((LA(1) == LITERAL_anchor) && (LA(2) == LITERAL_apple)) {
			match(LITERAL_anchor);
			match(LITERAL_apple);
			appleanchor(maker);
		}
		else if ((LA(1) == LITERAL_anchor) && (LA(2) == LITERAL_generic)) {
			match(LITERAL_anchor);
			match(LITERAL_generic);
			match(LITERAL_apple);
			maker.put(opAppleGenericAnchor);
		}
		else if ((LA(1) == LITERAL_anchor || LA(1) == LITERAL_certificate || LA(1) == LITERAL_cert) && (LA(2) == LITERAL_trusted)) {
			{
			switch ( LA(1)) {
			case LITERAL_certificate:
			{
				match(LITERAL_certificate);
				break;
			}
			case LITERAL_cert:
			{
				match(LITERAL_cert);
				break;
			}
			case LITERAL_anchor:
			{
				match(LITERAL_anchor);
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
			match(LITERAL_trusted);
			maker.trustedAnchor();
		}
		else if ((LA(1) == LITERAL_certificate || LA(1) == LITERAL_cert) && (_tokenSet_8.member(LA(2)))) {
			{
			switch ( LA(1)) {
			case LITERAL_certificate:
			{
				match(LITERAL_certificate);
				break;
			}
			case LITERAL_cert:
			{
				match(LITERAL_cert);
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
			int32_t slot;
			slot=certSlot();
			{
			switch ( LA(1)) {
			case EQL:
			case EQQL:
			case LBRACK:
			case HASHCONSTANT:
			case DOTKEY:
			case STRING:
			case PATHNAME:
			{
				certslotspec(maker, slot);
				break;
			}
			case LITERAL_trusted:
			{
				match(LITERAL_trusted);
				maker.trustedAnchor(slot);
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
		}
		else if ((LA(1) == LITERAL_anchor) && (_tokenSet_9.member(LA(2)))) {
			match(LITERAL_anchor);
			certslotspec(maker, Requirement::anchorCert);
		}
		else {
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

void RequirementParser::infospec(
	Maker &maker
) {
	string key;
	
	try {      // for error handling
		match(LITERAL_info);
		key=bracketKey();
		maker.put(opInfoKeyField); maker.put(key);
		match_suffix(maker);
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

void RequirementParser::entitlementspec(
	Maker &maker
) {
	string key;
	
	try {      // for error handling
		match(LITERAL_entitlement);
		key=bracketKey();
		maker.put(opEntitlementField); maker.put(key);
		match_suffix(maker);
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

void RequirementParser::eql() {
	
	try {      // for error handling
		switch ( LA(1)) {
		case EQL:
		{
			match(EQL);
			break;
		}
		case EQQL:
		{
			match(EQQL);
			break;
		}
		case HASHCONSTANT:
		case DOTKEY:
		case STRING:
		case PATHNAME:
		{
			empty();
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_10);
	}
}

string  RequirementParser::identifierString() {
	string result;
	antlr::RefToken  dk = antlr::nullToken;
	antlr::RefToken  s = antlr::nullToken;
	
	try {      // for error handling
		switch ( LA(1)) {
		case DOTKEY:
		{
			dk = LT(1);
			match(DOTKEY);
			result = dk->getText();
			break;
		}
		case STRING:
		{
			s = LT(1);
			match(STRING);
			result = s->getText();
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
	return result;
}

void RequirementParser::hash(
	SHA1::Digest digest
) {
	antlr::RefToken  hash = antlr::nullToken;
	
	try {      // for error handling
		hash = LT(1);
		match(HASHCONSTANT);
		hashString(hash->getText(), digest);
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

void RequirementParser::appleanchor(
	Maker &maker
) {
	
	try {      // for error handling
		switch ( LA(1)) {
		case antlr::Token::EOF_TYPE:
		case LITERAL_guest:
		case LITERAL_host:
		case LITERAL_designated:
		case LITERAL_library:
		case LITERAL_plugin:
		case INTEGER:
		case LITERAL_or:
		case LITERAL_and:
		case RPAREN:
		case SEMI:
		{
			empty();
			maker.put(opAppleAnchor);
			break;
		}
		case LITERAL_generic:
		{
			match(LITERAL_generic);
			maker.put(opAppleGenericAnchor);
			break;
		}
		case DOTKEY:
		case STRING:
		{
			string name;
			name=identifierString();
			maker.put(opNamedAnchor); maker.put(name);
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

int32_t  RequirementParser::certSlot() {
	int32_t slot = 0;
	antlr::RefToken  s = antlr::nullToken;
	antlr::RefToken  ss = antlr::nullToken;
	
	try {      // for error handling
		switch ( LA(1)) {
		case INTEGER:
		{
			s = LT(1);
			match(INTEGER);
			slot = atol(s->getText().c_str());
			break;
		}
		case NEG:
		{
			match(NEG);
			ss = LT(1);
			match(INTEGER);
			slot = -atol(ss->getText().c_str());
			break;
		}
		case LITERAL_leaf:
		{
			match(LITERAL_leaf);
			slot = Requirement::leafCert;
			break;
		}
		case LITERAL_root:
		{
			match(LITERAL_root);
			slot = Requirement::anchorCert;
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_11);
	}
	return slot;
}

void RequirementParser::certslotspec(
	Maker &maker, int32_t slot
) {
	string key;
	
	try {      // for error handling
		switch ( LA(1)) {
		case EQL:
		case EQQL:
		case HASHCONSTANT:
		case DOTKEY:
		case STRING:
		case PATHNAME:
		{
			eql();
			SHA1::Digest digest;
			certificateDigest(digest);
			maker.anchor(slot, digest);
			break;
		}
		case LBRACK:
		{
			key=bracketKey();
			certMatchOperation(maker, slot, key);
			match_suffix(maker);
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

void RequirementParser::empty() {
	
	try {      // for error handling
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_12);
	}
}

void RequirementParser::certificateDigest(
	SHA1::Digest digest
) {
	
	try {      // for error handling
		switch ( LA(1)) {
		case HASHCONSTANT:
		{
			hash(digest);
			break;
		}
		case DOTKEY:
		case STRING:
		case PATHNAME:
		{
			string path;
			path=pathstring();
			if (CFRef<CFDataRef> certData = cfLoadFile(path))
							hashOfCertificate(CFDataGetBytePtr(certData), CFDataGetLength(certData), digest);
						  else
							throw antlr::SemanticException(path + ": not found");
						
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

string  RequirementParser::bracketKey() {
	string key;
	
	try {      // for error handling
		match(LBRACK);
		key=stringvalue();
		match(RBRACK);
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_13);
	}
	return key;
}

void RequirementParser::match_suffix(
	Maker &maker
) {
	
	try {      // for error handling
		switch ( LA(1)) {
		case antlr::Token::EOF_TYPE:
		case LITERAL_guest:
		case LITERAL_host:
		case LITERAL_designated:
		case LITERAL_library:
		case LITERAL_plugin:
		case INTEGER:
		case LITERAL_or:
		case LITERAL_and:
		case RPAREN:
		case LITERAL_exists:
		case SEMI:
		{
			empty();
			{
			switch ( LA(1)) {
			case LITERAL_exists:
			{
				match(LITERAL_exists);
				break;
			}
			case antlr::Token::EOF_TYPE:
			case LITERAL_guest:
			case LITERAL_host:
			case LITERAL_designated:
			case LITERAL_library:
			case LITERAL_plugin:
			case INTEGER:
			case LITERAL_or:
			case LITERAL_and:
			case RPAREN:
			case SEMI:
			{
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
			maker.put(matchExists);
			break;
		}
		case EQL:
		case EQQL:
		{
			{
			switch ( LA(1)) {
			case EQL:
			{
				match(EQL);
				break;
			}
			case EQQL:
			{
				match(EQQL);
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
			MatchOperation mop = matchEqual; string value;
			{
			switch ( LA(1)) {
			case STAR:
			{
				match(STAR);
				mop = matchEndsWith;
				break;
			}
			case HEXCONSTANT:
			case DOTKEY:
			case STRING:
			{
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
			value=datavalue();
			{
			switch ( LA(1)) {
			case STAR:
			{
				match(STAR);
				mop = (mop == matchEndsWith) ? matchContains : matchBeginsWith;
				break;
			}
			case antlr::Token::EOF_TYPE:
			case LITERAL_guest:
			case LITERAL_host:
			case LITERAL_designated:
			case LITERAL_library:
			case LITERAL_plugin:
			case INTEGER:
			case LITERAL_or:
			case LITERAL_and:
			case RPAREN:
			case SEMI:
			{
				break;
			}
			default:
			{
				throw antlr::NoViableAltException(LT(1), getFilename());
			}
			}
			}
			maker.put(mop); maker.put(value);
			break;
		}
		case SUBS:
		{
			match(SUBS);
			string value;
			value=datavalue();
			maker.put(matchContains); maker.put(value);
			break;
		}
		case LESS:
		{
			match(LESS);
			string value;
			value=datavalue();
			maker.put(matchLessThan); maker.put(value);
			break;
		}
		case GT:
		{
			match(GT);
			string value;
			value=datavalue();
			maker.put(matchGreaterThan); maker.put(value);
			break;
		}
		case LE:
		{
			match(LE);
			string value;
			value=datavalue();
			maker.put(matchLessEqual); maker.put(value);
			break;
		}
		case GE:
		{
			match(GE);
			string value;
			value=datavalue();
			maker.put(matchGreaterEqual); maker.put(value);
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
}

string  RequirementParser::datavalue() {
	string result;
	antlr::RefToken  hex = antlr::nullToken;
	
	try {      // for error handling
		switch ( LA(1)) {
		case DOTKEY:
		case STRING:
		{
			result=stringvalue();
			break;
		}
		case HEXCONSTANT:
		{
			hex = LT(1);
			match(HEXCONSTANT);
			result = hexString(hex->getText());
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_14);
	}
	return result;
}

string  RequirementParser::stringvalue() {
	string result;
	antlr::RefToken  dk = antlr::nullToken;
	antlr::RefToken  s = antlr::nullToken;
	
	try {      // for error handling
		switch ( LA(1)) {
		case DOTKEY:
		{
			dk = LT(1);
			match(DOTKEY);
			result = dk->getText();
			break;
		}
		case STRING:
		{
			s = LT(1);
			match(STRING);
			result = s->getText();
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_15);
	}
	return result;
}

string  RequirementParser::pathstring() {
	string result;
	antlr::RefToken  dk = antlr::nullToken;
	antlr::RefToken  s = antlr::nullToken;
	antlr::RefToken  pn = antlr::nullToken;
	
	try {      // for error handling
		switch ( LA(1)) {
		case DOTKEY:
		{
			dk = LT(1);
			match(DOTKEY);
			result = dk->getText();
			break;
		}
		case STRING:
		{
			s = LT(1);
			match(STRING);
			result = s->getText();
			break;
		}
		case PATHNAME:
		{
			pn = LT(1);
			match(PATHNAME);
			result = pn->getText();
			break;
		}
		default:
		{
			throw antlr::NoViableAltException(LT(1), getFilename());
		}
		}
	}
	catch (antlr::RecognitionException& ex) {
		reportError(ex);
		recover(ex,_tokenSet_7);
	}
	return result;
}

void RequirementParser::initializeASTFactory( antlr::ASTFactory& )
{
}
const char* RequirementParser::tokenNames[] = {
	"<0>",
	"EOF",
	"<2>",
	"NULL_TREE_LOOKAHEAD",
	"ARROW",
	"\"guest\"",
	"\"host\"",
	"\"designated\"",
	"\"library\"",
	"\"plugin\"",
	"INTEGER",
	"\"or\"",
	"\"and\"",
	"LPAREN",
	"RPAREN",
	"NOT",
	"\"always\"",
	"\"true\"",
	"\"never\"",
	"\"false\"",
	"\"identifier\"",
	"\"cdhash\"",
	"\"anchor\"",
	"\"apple\"",
	"\"generic\"",
	"\"certificate\"",
	"\"cert\"",
	"\"trusted\"",
	"\"info\"",
	"\"entitlement\"",
	"\"exists\"",
	"EQL",
	"EQQL",
	"STAR",
	"SUBS",
	"LESS",
	"GT",
	"LE",
	"GE",
	"LBRACK",
	"RBRACK",
	"NEG",
	"\"leaf\"",
	"\"root\"",
	"HASHCONSTANT",
	"HEXCONSTANT",
	"DOTKEY",
	"STRING",
	"PATHNAME",
	"SEMI",
	"IDENT",
	"HEX",
	"COMMA",
	"WS",
	"SHELLCOMMENT",
	"C_COMMENT",
	"CPP_COMMENT",
	0
};

const unsigned long RequirementParser::_tokenSet_0_data_[] = { 2UL, 0UL, 0UL, 0UL };
// EOF 
const antlr::BitSet RequirementParser::_tokenSet_0(_tokenSet_0_data_,4);
const unsigned long RequirementParser::_tokenSet_1_data_[] = { 16UL, 0UL, 0UL, 0UL };
// ARROW 
const antlr::BitSet RequirementParser::_tokenSet_1(_tokenSet_1_data_,4);
const unsigned long RequirementParser::_tokenSet_2_data_[] = { 2018UL, 0UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER 
const antlr::BitSet RequirementParser::_tokenSet_2(_tokenSet_2_data_,4);
const unsigned long RequirementParser::_tokenSet_3_data_[] = { 18402UL, 131072UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER RPAREN SEMI 
const antlr::BitSet RequirementParser::_tokenSet_3(_tokenSet_3_data_,4);
const unsigned long RequirementParser::_tokenSet_4_data_[] = { 2018UL, 131072UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER SEMI 
const antlr::BitSet RequirementParser::_tokenSet_4(_tokenSet_4_data_,4);
const unsigned long RequirementParser::_tokenSet_5_data_[] = { 20450UL, 131072UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER "or" RPAREN 
// SEMI 
const antlr::BitSet RequirementParser::_tokenSet_5(_tokenSet_5_data_,4);
const unsigned long RequirementParser::_tokenSet_6_data_[] = { 914333696UL, 0UL, 0UL, 0UL };
// LPAREN NOT "always" "true" "never" "false" "identifier" "cdhash" "anchor" 
// "certificate" "cert" "info" "entitlement" 
const antlr::BitSet RequirementParser::_tokenSet_6(_tokenSet_6_data_,4);
const unsigned long RequirementParser::_tokenSet_7_data_[] = { 24546UL, 131072UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER "or" "and" 
// RPAREN SEMI 
const antlr::BitSet RequirementParser::_tokenSet_7(_tokenSet_7_data_,4);
const unsigned long RequirementParser::_tokenSet_8_data_[] = { 1024UL, 3584UL, 0UL, 0UL };
// INTEGER NEG "leaf" "root" 
const antlr::BitSet RequirementParser::_tokenSet_8(_tokenSet_8_data_,4);
const unsigned long RequirementParser::_tokenSet_9_data_[] = { 2147483648UL, 118913UL, 0UL, 0UL };
// EQL EQQL LBRACK HASHCONSTANT DOTKEY STRING PATHNAME 
const antlr::BitSet RequirementParser::_tokenSet_9(_tokenSet_9_data_,4);
const unsigned long RequirementParser::_tokenSet_10_data_[] = { 0UL, 118784UL, 0UL, 0UL };
// HASHCONSTANT DOTKEY STRING PATHNAME 
const antlr::BitSet RequirementParser::_tokenSet_10(_tokenSet_10_data_,4);
const unsigned long RequirementParser::_tokenSet_11_data_[] = { 2281701376UL, 118913UL, 0UL, 0UL };
// "trusted" EQL EQQL LBRACK HASHCONSTANT DOTKEY STRING PATHNAME 
const antlr::BitSet RequirementParser::_tokenSet_11(_tokenSet_11_data_,4);
const unsigned long RequirementParser::_tokenSet_12_data_[] = { 1073766370UL, 249856UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER "or" "and" 
// RPAREN "exists" HASHCONSTANT DOTKEY STRING PATHNAME SEMI 
const antlr::BitSet RequirementParser::_tokenSet_12(_tokenSet_12_data_,4);
const unsigned long RequirementParser::_tokenSet_13_data_[] = { 3221250018UL, 131197UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER "or" "and" 
// RPAREN "exists" EQL EQQL SUBS LESS GT LE GE SEMI 
const antlr::BitSet RequirementParser::_tokenSet_13(_tokenSet_13_data_,4);
const unsigned long RequirementParser::_tokenSet_14_data_[] = { 24546UL, 131074UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER "or" "and" 
// RPAREN STAR SEMI 
const antlr::BitSet RequirementParser::_tokenSet_14(_tokenSet_14_data_,4);
const unsigned long RequirementParser::_tokenSet_15_data_[] = { 24546UL, 131330UL, 0UL, 0UL };
// EOF "guest" "host" "designated" "library" "plugin" INTEGER "or" "and" 
// RPAREN STAR RBRACK SEMI 
const antlr::BitSet RequirementParser::_tokenSet_15(_tokenSet_15_data_,4);


ANTLR_END_NAMESPACE
