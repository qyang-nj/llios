/* $ANTLR 2.7.7 (20120228): "requirements.grammar" -> "RequirementLexer.cpp"$ */
#include "RequirementLexer.hpp"
#include <antlr/CharBuffer.hpp>
#include <antlr/TokenStreamException.hpp>
#include <antlr/TokenStreamIOException.hpp>
#include <antlr/TokenStreamRecognitionException.hpp>
#include <antlr/CharStreamException.hpp>
#include <antlr/CharStreamIOException.hpp>
#include <antlr/NoViableAltForCharException.hpp>


#include "requirement.h"
#include "reqmaker.h"
#include "csutilities.h"
#include <security_utilities/cfutilities.h>
#include <security_utilities/hashing.h>
#include <security_cdsa_utilities/cssmdata.h>	// OID coding
using namespace CodeSigning;
typedef Requirement::Maker Maker;

ANTLR_BEGIN_NAMESPACE(Security_CodeSigning)
RequirementLexer::RequirementLexer(std::istream& in)
	: antlr::CharScanner(new antlr::CharBuffer(in),true)
{
	initLiterals();
}

RequirementLexer::RequirementLexer(antlr::InputBuffer& ib)
	: antlr::CharScanner(ib,true)
{
	initLiterals();
}

RequirementLexer::RequirementLexer(const antlr::LexerSharedInputState& state)
	: antlr::CharScanner(state,true)
{
	initLiterals();
}

void RequirementLexer::initLiterals()
{
	literals["certificate"] = 25;
	literals["always"] = 16;
	literals["host"] = 6;
	literals["guest"] = 5;
	literals["cdhash"] = 21;
	literals["entitlement"] = 29;
	literals["library"] = 8;
	literals["never"] = 18;
	literals["cert"] = 26;
	literals["plugin"] = 9;
	literals["or"] = 11;
	literals["leaf"] = 42;
	literals["info"] = 28;
	literals["designated"] = 7;
	literals["apple"] = 23;
	literals["trusted"] = 27;
	literals["true"] = 17;
	literals["and"] = 12;
	literals["root"] = 43;
	literals["anchor"] = 22;
	literals["false"] = 19;
	literals["generic"] = 24;
	literals["identifier"] = 20;
	literals["exists"] = 30;
}

antlr::RefToken RequirementLexer::nextToken()
{
	antlr::RefToken theRetToken;
	for (;;) {
		antlr::RefToken theRetToken;
		int _ttype = antlr::Token::INVALID_TYPE;
		resetText();
		try {   // for lexical and char stream error handling
			switch ( LA(1)) {
			case 0x22 /* '\"' */ :
			{
				mSTRING(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x3b /* ';' */ :
			{
				mSEMI(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x28 /* '(' */ :
			{
				mLPAREN(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x29 /* ')' */ :
			{
				mRPAREN(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x5b /* '[' */ :
			{
				mLBRACK(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x5d /* ']' */ :
			{
				mRBRACK(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x2c /* ',' */ :
			{
				mCOMMA(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x7e /* '~' */ :
			{
				mSUBS(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x2d /* '-' */ :
			{
				mNEG(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x21 /* '!' */ :
			{
				mNOT(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x2a /* '*' */ :
			{
				mSTAR(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x9 /* '\t' */ :
			case 0xa /* '\n' */ :
			case 0x20 /* ' ' */ :
			{
				mWS(true);
				theRetToken=_returnToken;
				break;
			}
			case 0x23 /* '#' */ :
			{
				mSHELLCOMMENT(true);
				theRetToken=_returnToken;
				break;
			}
			default:
				if ((LA(1) == 0x2f /* '/' */ ) && (_tokenSet_0.member(LA(2)))) {
					mPATHNAME(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x48 /* 'H' */ ) && (LA(2) == 0x22 /* '\"' */ )) {
					mHASHCONSTANT(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x30 /* '0' */ ) && (LA(2) == 0x78 /* 'x' */ )) {
					mHEXCONSTANT(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x3d /* '=' */ ) && (LA(2) == 0x3e /* '>' */ )) {
					mARROW(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x3c /* '<' */ ) && (LA(2) == 0x3d /* '=' */ )) {
					mLE(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x3e /* '>' */ ) && (LA(2) == 0x3d /* '=' */ )) {
					mGE(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x3d /* '=' */ ) && (LA(2) == 0x3d /* '=' */ )) {
					mEQQL(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x2f /* '/' */ ) && (LA(2) == 0x2a /* '*' */ )) {
					mC_COMMENT(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x2f /* '/' */ ) && (LA(2) == 0x2f /* '/' */ )) {
					mCPP_COMMENT(true);
					theRetToken=_returnToken;
				}
				else if ((_tokenSet_0.member(LA(1))) && (true)) {
					mDOTKEY(true);
					theRetToken=_returnToken;
				}
				else if (((LA(1) >= 0x30 /* '0' */  && LA(1) <= 0x39 /* '9' */ )) && (true)) {
					mINTEGER(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x3c /* '<' */ ) && (true)) {
					mLESS(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x3e /* '>' */ ) && (true)) {
					mGT(true);
					theRetToken=_returnToken;
				}
				else if ((LA(1) == 0x3d /* '=' */ ) && (true)) {
					mEQL(true);
					theRetToken=_returnToken;
				}
			else {
				if (LA(1)==EOF_CHAR)
				{
					uponEOF();
					_returnToken = makeToken(antlr::Token::EOF_TYPE);
				}
				else {throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());}
			}
			}
			if ( !_returnToken )
				goto tryAgain; // found SKIP token

			_ttype = _returnToken->getType();
			_returnToken->setType(_ttype);
			return _returnToken;
		}
		catch (antlr::RecognitionException& e) {
				throw antlr::TokenStreamRecognitionException(e);
		}
		catch (antlr::CharStreamIOException& csie) {
			throw antlr::TokenStreamIOException(csie.io);
		}
		catch (antlr::CharStreamException& cse) {
			throw antlr::TokenStreamException(cse.getMessage());
		}
tryAgain:;
	}
}

void RequirementLexer::mIDENT(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = IDENT;
	std::string::size_type _saveIndex;
	
	{
	switch ( LA(1)) {
	case 0x41 /* 'A' */ :
	case 0x42 /* 'B' */ :
	case 0x43 /* 'C' */ :
	case 0x44 /* 'D' */ :
	case 0x45 /* 'E' */ :
	case 0x46 /* 'F' */ :
	case 0x47 /* 'G' */ :
	case 0x48 /* 'H' */ :
	case 0x49 /* 'I' */ :
	case 0x4a /* 'J' */ :
	case 0x4b /* 'K' */ :
	case 0x4c /* 'L' */ :
	case 0x4d /* 'M' */ :
	case 0x4e /* 'N' */ :
	case 0x4f /* 'O' */ :
	case 0x50 /* 'P' */ :
	case 0x51 /* 'Q' */ :
	case 0x52 /* 'R' */ :
	case 0x53 /* 'S' */ :
	case 0x54 /* 'T' */ :
	case 0x55 /* 'U' */ :
	case 0x56 /* 'V' */ :
	case 0x57 /* 'W' */ :
	case 0x58 /* 'X' */ :
	case 0x59 /* 'Y' */ :
	case 0x5a /* 'Z' */ :
	{
		matchRange('A','Z');
		break;
	}
	case 0x61 /* 'a' */ :
	case 0x62 /* 'b' */ :
	case 0x63 /* 'c' */ :
	case 0x64 /* 'd' */ :
	case 0x65 /* 'e' */ :
	case 0x66 /* 'f' */ :
	case 0x67 /* 'g' */ :
	case 0x68 /* 'h' */ :
	case 0x69 /* 'i' */ :
	case 0x6a /* 'j' */ :
	case 0x6b /* 'k' */ :
	case 0x6c /* 'l' */ :
	case 0x6d /* 'm' */ :
	case 0x6e /* 'n' */ :
	case 0x6f /* 'o' */ :
	case 0x70 /* 'p' */ :
	case 0x71 /* 'q' */ :
	case 0x72 /* 'r' */ :
	case 0x73 /* 's' */ :
	case 0x74 /* 't' */ :
	case 0x75 /* 'u' */ :
	case 0x76 /* 'v' */ :
	case 0x77 /* 'w' */ :
	case 0x78 /* 'x' */ :
	case 0x79 /* 'y' */ :
	case 0x7a /* 'z' */ :
	{
		matchRange('a','z');
		break;
	}
	default:
	{
		throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());
	}
	}
	}
	{ // ( ... )*
	for (;;) {
		switch ( LA(1)) {
		case 0x41 /* 'A' */ :
		case 0x42 /* 'B' */ :
		case 0x43 /* 'C' */ :
		case 0x44 /* 'D' */ :
		case 0x45 /* 'E' */ :
		case 0x46 /* 'F' */ :
		case 0x47 /* 'G' */ :
		case 0x48 /* 'H' */ :
		case 0x49 /* 'I' */ :
		case 0x4a /* 'J' */ :
		case 0x4b /* 'K' */ :
		case 0x4c /* 'L' */ :
		case 0x4d /* 'M' */ :
		case 0x4e /* 'N' */ :
		case 0x4f /* 'O' */ :
		case 0x50 /* 'P' */ :
		case 0x51 /* 'Q' */ :
		case 0x52 /* 'R' */ :
		case 0x53 /* 'S' */ :
		case 0x54 /* 'T' */ :
		case 0x55 /* 'U' */ :
		case 0x56 /* 'V' */ :
		case 0x57 /* 'W' */ :
		case 0x58 /* 'X' */ :
		case 0x59 /* 'Y' */ :
		case 0x5a /* 'Z' */ :
		{
			matchRange('A','Z');
			break;
		}
		case 0x61 /* 'a' */ :
		case 0x62 /* 'b' */ :
		case 0x63 /* 'c' */ :
		case 0x64 /* 'd' */ :
		case 0x65 /* 'e' */ :
		case 0x66 /* 'f' */ :
		case 0x67 /* 'g' */ :
		case 0x68 /* 'h' */ :
		case 0x69 /* 'i' */ :
		case 0x6a /* 'j' */ :
		case 0x6b /* 'k' */ :
		case 0x6c /* 'l' */ :
		case 0x6d /* 'm' */ :
		case 0x6e /* 'n' */ :
		case 0x6f /* 'o' */ :
		case 0x70 /* 'p' */ :
		case 0x71 /* 'q' */ :
		case 0x72 /* 'r' */ :
		case 0x73 /* 's' */ :
		case 0x74 /* 't' */ :
		case 0x75 /* 'u' */ :
		case 0x76 /* 'v' */ :
		case 0x77 /* 'w' */ :
		case 0x78 /* 'x' */ :
		case 0x79 /* 'y' */ :
		case 0x7a /* 'z' */ :
		{
			matchRange('a','z');
			break;
		}
		case 0x30 /* '0' */ :
		case 0x31 /* '1' */ :
		case 0x32 /* '2' */ :
		case 0x33 /* '3' */ :
		case 0x34 /* '4' */ :
		case 0x35 /* '5' */ :
		case 0x36 /* '6' */ :
		case 0x37 /* '7' */ :
		case 0x38 /* '8' */ :
		case 0x39 /* '9' */ :
		{
			matchRange('0','9');
			break;
		}
		default:
		{
			goto _loop46;
		}
		}
	}
	_loop46:;
	} // ( ... )*
	_ttype = testLiteralsTable(text.substr(_begin, text.length()-_begin),_ttype);
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mDOTKEY(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = DOTKEY;
	std::string::size_type _saveIndex;
	
	mIDENT(false);
	{ // ( ... )*
	for (;;) {
		if ((LA(1) == 0x2e /* '.' */ )) {
			match(".");
			{
			switch ( LA(1)) {
			case 0x41 /* 'A' */ :
			case 0x42 /* 'B' */ :
			case 0x43 /* 'C' */ :
			case 0x44 /* 'D' */ :
			case 0x45 /* 'E' */ :
			case 0x46 /* 'F' */ :
			case 0x47 /* 'G' */ :
			case 0x48 /* 'H' */ :
			case 0x49 /* 'I' */ :
			case 0x4a /* 'J' */ :
			case 0x4b /* 'K' */ :
			case 0x4c /* 'L' */ :
			case 0x4d /* 'M' */ :
			case 0x4e /* 'N' */ :
			case 0x4f /* 'O' */ :
			case 0x50 /* 'P' */ :
			case 0x51 /* 'Q' */ :
			case 0x52 /* 'R' */ :
			case 0x53 /* 'S' */ :
			case 0x54 /* 'T' */ :
			case 0x55 /* 'U' */ :
			case 0x56 /* 'V' */ :
			case 0x57 /* 'W' */ :
			case 0x58 /* 'X' */ :
			case 0x59 /* 'Y' */ :
			case 0x5a /* 'Z' */ :
			case 0x61 /* 'a' */ :
			case 0x62 /* 'b' */ :
			case 0x63 /* 'c' */ :
			case 0x64 /* 'd' */ :
			case 0x65 /* 'e' */ :
			case 0x66 /* 'f' */ :
			case 0x67 /* 'g' */ :
			case 0x68 /* 'h' */ :
			case 0x69 /* 'i' */ :
			case 0x6a /* 'j' */ :
			case 0x6b /* 'k' */ :
			case 0x6c /* 'l' */ :
			case 0x6d /* 'm' */ :
			case 0x6e /* 'n' */ :
			case 0x6f /* 'o' */ :
			case 0x70 /* 'p' */ :
			case 0x71 /* 'q' */ :
			case 0x72 /* 'r' */ :
			case 0x73 /* 's' */ :
			case 0x74 /* 't' */ :
			case 0x75 /* 'u' */ :
			case 0x76 /* 'v' */ :
			case 0x77 /* 'w' */ :
			case 0x78 /* 'x' */ :
			case 0x79 /* 'y' */ :
			case 0x7a /* 'z' */ :
			{
				mIDENT(false);
				break;
			}
			case 0x30 /* '0' */ :
			case 0x31 /* '1' */ :
			case 0x32 /* '2' */ :
			case 0x33 /* '3' */ :
			case 0x34 /* '4' */ :
			case 0x35 /* '5' */ :
			case 0x36 /* '6' */ :
			case 0x37 /* '7' */ :
			case 0x38 /* '8' */ :
			case 0x39 /* '9' */ :
			{
				mINTEGER(false);
				break;
			}
			default:
			{
				throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());
			}
			}
			}
		}
		else {
			goto _loop50;
		}
		
	}
	_loop50:;
	} // ( ... )*
	_ttype = testLiteralsTable(_ttype);
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mINTEGER(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = INTEGER;
	std::string::size_type _saveIndex;
	
	{ // ( ... )+
	int _cnt68=0;
	for (;;) {
		if (((LA(1) >= 0x30 /* '0' */  && LA(1) <= 0x39 /* '9' */ ))) {
			matchRange('0','9');
		}
		else {
			if ( _cnt68>=1 ) { goto _loop68; } else {throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());}
		}
		
		_cnt68++;
	}
	_loop68:;
	}  // ( ... )+
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mPATHNAME(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = PATHNAME;
	std::string::size_type _saveIndex;
	
	match("/");
	mIDENT(false);
	{ // ( ... )+
	int _cnt53=0;
	for (;;) {
		if ((LA(1) == 0x2f /* '/' */ )) {
			match("/");
			mIDENT(false);
		}
		else {
			if ( _cnt53>=1 ) { goto _loop53; } else {throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());}
		}
		
		_cnt53++;
	}
	_loop53:;
	}  // ( ... )+
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mHASHCONSTANT(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = HASHCONSTANT;
	std::string::size_type _saveIndex;
	
	_saveIndex = text.length();
	match('H' /* charlit */ );
	text.erase(_saveIndex);
	_saveIndex = text.length();
	match('\"' /* charlit */ );
	text.erase(_saveIndex);
	{ // ( ... )+
	int _cnt56=0;
	for (;;) {
		if ((_tokenSet_1.member(LA(1)))) {
			mHEX(false);
		}
		else {
			if ( _cnt56>=1 ) { goto _loop56; } else {throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());}
		}
		
		_cnt56++;
	}
	_loop56:;
	}  // ( ... )+
	_saveIndex = text.length();
	match('\"' /* charlit */ );
	text.erase(_saveIndex);
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mHEX(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = HEX;
	std::string::size_type _saveIndex;
	
	switch ( LA(1)) {
	case 0x30 /* '0' */ :
	case 0x31 /* '1' */ :
	case 0x32 /* '2' */ :
	case 0x33 /* '3' */ :
	case 0x34 /* '4' */ :
	case 0x35 /* '5' */ :
	case 0x36 /* '6' */ :
	case 0x37 /* '7' */ :
	case 0x38 /* '8' */ :
	case 0x39 /* '9' */ :
	{
		matchRange('0','9');
		break;
	}
	case 0x61 /* 'a' */ :
	case 0x62 /* 'b' */ :
	case 0x63 /* 'c' */ :
	case 0x64 /* 'd' */ :
	case 0x65 /* 'e' */ :
	case 0x66 /* 'f' */ :
	{
		matchRange('a','f');
		break;
	}
	case 0x41 /* 'A' */ :
	case 0x42 /* 'B' */ :
	case 0x43 /* 'C' */ :
	case 0x44 /* 'D' */ :
	case 0x45 /* 'E' */ :
	case 0x46 /* 'F' */ :
	{
		matchRange('A','F');
		break;
	}
	default:
	{
		throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());
	}
	}
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mHEXCONSTANT(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = HEXCONSTANT;
	std::string::size_type _saveIndex;
	
	_saveIndex = text.length();
	match('0' /* charlit */ );
	text.erase(_saveIndex);
	_saveIndex = text.length();
	match('x' /* charlit */ );
	text.erase(_saveIndex);
	{ // ( ... )+
	int _cnt59=0;
	for (;;) {
		if ((_tokenSet_1.member(LA(1)))) {
			mHEX(false);
		}
		else {
			if ( _cnt59>=1 ) { goto _loop59; } else {throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());}
		}
		
		_cnt59++;
	}
	_loop59:;
	}  // ( ... )+
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mSTRING(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = STRING;
	std::string::size_type _saveIndex;
	
	_saveIndex = text.length();
	match('\"' /* charlit */ );
	text.erase(_saveIndex);
	{ // ( ... )*
	for (;;) {
		switch ( LA(1)) {
		case 0x5c /* '\\' */ :
		{
			{
			_saveIndex = text.length();
			match('\\' /* charlit */ );
			text.erase(_saveIndex);
			match('\"' /* charlit */ );
			}
			break;
		}
		case 0x0 /* '\0' */ :
		case 0x1 /* '\1' */ :
		case 0x2 /* '\2' */ :
		case 0x3 /* '\3' */ :
		case 0x4 /* '\4' */ :
		case 0x5 /* '\5' */ :
		case 0x6 /* '\6' */ :
		case 0x7 /* '\7' */ :
		case 0x8 /* '\10' */ :
		case 0x9 /* '\t' */ :
		case 0xa /* '\n' */ :
		case 0xb /* '\13' */ :
		case 0xc /* '\14' */ :
		case 0xd /* '\r' */ :
		case 0xe /* '\16' */ :
		case 0xf /* '\17' */ :
		case 0x10 /* '\20' */ :
		case 0x11 /* '\21' */ :
		case 0x12 /* '\22' */ :
		case 0x13 /* '\23' */ :
		case 0x14 /* '\24' */ :
		case 0x15 /* '\25' */ :
		case 0x16 /* '\26' */ :
		case 0x17 /* '\27' */ :
		case 0x18 /* '\30' */ :
		case 0x19 /* '\31' */ :
		case 0x1a /* '\32' */ :
		case 0x1b /* '\33' */ :
		case 0x1c /* '\34' */ :
		case 0x1d /* '\35' */ :
		case 0x1e /* '\36' */ :
		case 0x1f /* '\37' */ :
		case 0x20 /* ' ' */ :
		case 0x21 /* '!' */ :
		case 0x23 /* '#' */ :
		case 0x24 /* '$' */ :
		case 0x25 /* '%' */ :
		case 0x26 /* '&' */ :
		case 0x27 /* '\'' */ :
		case 0x28 /* '(' */ :
		case 0x29 /* ')' */ :
		case 0x2a /* '*' */ :
		case 0x2b /* '+' */ :
		case 0x2c /* ',' */ :
		case 0x2d /* '-' */ :
		case 0x2e /* '.' */ :
		case 0x2f /* '/' */ :
		case 0x30 /* '0' */ :
		case 0x31 /* '1' */ :
		case 0x32 /* '2' */ :
		case 0x33 /* '3' */ :
		case 0x34 /* '4' */ :
		case 0x35 /* '5' */ :
		case 0x36 /* '6' */ :
		case 0x37 /* '7' */ :
		case 0x38 /* '8' */ :
		case 0x39 /* '9' */ :
		case 0x3a /* ':' */ :
		case 0x3b /* ';' */ :
		case 0x3c /* '<' */ :
		case 0x3d /* '=' */ :
		case 0x3e /* '>' */ :
		case 0x3f /* '?' */ :
		case 0x40 /* '@' */ :
		case 0x41 /* 'A' */ :
		case 0x42 /* 'B' */ :
		case 0x43 /* 'C' */ :
		case 0x44 /* 'D' */ :
		case 0x45 /* 'E' */ :
		case 0x46 /* 'F' */ :
		case 0x47 /* 'G' */ :
		case 0x48 /* 'H' */ :
		case 0x49 /* 'I' */ :
		case 0x4a /* 'J' */ :
		case 0x4b /* 'K' */ :
		case 0x4c /* 'L' */ :
		case 0x4d /* 'M' */ :
		case 0x4e /* 'N' */ :
		case 0x4f /* 'O' */ :
		case 0x50 /* 'P' */ :
		case 0x51 /* 'Q' */ :
		case 0x52 /* 'R' */ :
		case 0x53 /* 'S' */ :
		case 0x54 /* 'T' */ :
		case 0x55 /* 'U' */ :
		case 0x56 /* 'V' */ :
		case 0x57 /* 'W' */ :
		case 0x58 /* 'X' */ :
		case 0x59 /* 'Y' */ :
		case 0x5a /* 'Z' */ :
		case 0x5b /* '[' */ :
		case 0x5d /* ']' */ :
		case 0x5e /* '^' */ :
		case 0x5f /* '_' */ :
		case 0x60 /* '`' */ :
		case 0x61 /* 'a' */ :
		case 0x62 /* 'b' */ :
		case 0x63 /* 'c' */ :
		case 0x64 /* 'd' */ :
		case 0x65 /* 'e' */ :
		case 0x66 /* 'f' */ :
		case 0x67 /* 'g' */ :
		case 0x68 /* 'h' */ :
		case 0x69 /* 'i' */ :
		case 0x6a /* 'j' */ :
		case 0x6b /* 'k' */ :
		case 0x6c /* 'l' */ :
		case 0x6d /* 'm' */ :
		case 0x6e /* 'n' */ :
		case 0x6f /* 'o' */ :
		case 0x70 /* 'p' */ :
		case 0x71 /* 'q' */ :
		case 0x72 /* 'r' */ :
		case 0x73 /* 's' */ :
		case 0x74 /* 't' */ :
		case 0x75 /* 'u' */ :
		case 0x76 /* 'v' */ :
		case 0x77 /* 'w' */ :
		case 0x78 /* 'x' */ :
		case 0x79 /* 'y' */ :
		case 0x7a /* 'z' */ :
		case 0x7b /* '{' */ :
		case 0x7c /* '|' */ :
		case 0x7d /* '}' */ :
		case 0x7e /* '~' */ :
		case 0x7f:
		{
			{
			{
			match(_tokenSet_2);
			}
			}
			break;
		}
		default:
		{
			goto _loop65;
		}
		}
	}
	_loop65:;
	} // ( ... )*
	_saveIndex = text.length();
	match('\"' /* charlit */ );
	text.erase(_saveIndex);
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mARROW(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = ARROW;
	std::string::size_type _saveIndex;
	
	match("=>");
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mSEMI(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = SEMI;
	std::string::size_type _saveIndex;
	
	match(';' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mLPAREN(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = LPAREN;
	std::string::size_type _saveIndex;
	
	match('(' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mRPAREN(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = RPAREN;
	std::string::size_type _saveIndex;
	
	match(')' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mLBRACK(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = LBRACK;
	std::string::size_type _saveIndex;
	
	match('[' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mRBRACK(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = RBRACK;
	std::string::size_type _saveIndex;
	
	match(']' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mLESS(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = LESS;
	std::string::size_type _saveIndex;
	
	match('<' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mGT(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = GT;
	std::string::size_type _saveIndex;
	
	match('>' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mLE(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = LE;
	std::string::size_type _saveIndex;
	
	match("<=");
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mGE(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = GE;
	std::string::size_type _saveIndex;
	
	match(">=");
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mCOMMA(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = COMMA;
	std::string::size_type _saveIndex;
	
	match(',' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mEQL(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = EQL;
	std::string::size_type _saveIndex;
	
	match('=' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mEQQL(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = EQQL;
	std::string::size_type _saveIndex;
	
	match("==");
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mSUBS(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = SUBS;
	std::string::size_type _saveIndex;
	
	match('~' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mNEG(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = NEG;
	std::string::size_type _saveIndex;
	
	match('-' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mNOT(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = NOT;
	std::string::size_type _saveIndex;
	
	match('!' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mSTAR(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = STAR;
	std::string::size_type _saveIndex;
	
	match('*' /* charlit */ );
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mWS(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = WS;
	std::string::size_type _saveIndex;
	
	{ // ( ... )+
	int _cnt89=0;
	for (;;) {
		switch ( LA(1)) {
		case 0x20 /* ' ' */ :
		{
			match(' ' /* charlit */ );
			break;
		}
		case 0xa /* '\n' */ :
		{
			match('\n' /* charlit */ );
			newline();
			break;
		}
		case 0x9 /* '\t' */ :
		{
			match('\t' /* charlit */ );
			break;
		}
		default:
		{
			if ( _cnt89>=1 ) { goto _loop89; } else {throw antlr::NoViableAltForCharException(LA(1), getFilename(), getLine(), getColumn());}
		}
		}
		_cnt89++;
	}
	_loop89:;
	}  // ( ... )+
	_ttype = antlr::Token::SKIP;
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mSHELLCOMMENT(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = SHELLCOMMENT;
	std::string::size_type _saveIndex;
	
	match('#' /* charlit */ );
	{ // ( ... )*
	for (;;) {
		if ((_tokenSet_3.member(LA(1)))) {
			matchNot('\n' /* charlit */ );
		}
		else {
			goto _loop92;
		}
		
	}
	_loop92:;
	} // ( ... )*
	_ttype = antlr::Token::SKIP;
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mC_COMMENT(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = C_COMMENT;
	std::string::size_type _saveIndex;
	
	match("/*");
	{ // ( ... )*
	for (;;) {
		if ((LA(1) == 0x2a /* '*' */ ) && (_tokenSet_4.member(LA(2)))) {
			{
			match('*' /* charlit */ );
			{
			matchNot('/' /* charlit */ );
			}
			}
		}
		else if ((_tokenSet_5.member(LA(1)))) {
			{
			matchNot('*' /* charlit */ );
			}
		}
		else {
			goto _loop98;
		}
		
	}
	_loop98:;
	} // ( ... )*
	match("*/");
	_ttype = antlr::Token::SKIP;
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}

void RequirementLexer::mCPP_COMMENT(bool _createToken) {
	int _ttype; antlr::RefToken _token; std::string::size_type _begin = text.length();
	_ttype = CPP_COMMENT;
	std::string::size_type _saveIndex;
	
	match("//");
	{ // ( ... )*
	for (;;) {
		if ((_tokenSet_3.member(LA(1)))) {
			matchNot('\n' /* charlit */ );
		}
		else {
			goto _loop101;
		}
		
	}
	_loop101:;
	} // ( ... )*
	_ttype = antlr::Token::SKIP;
	if ( _createToken && _token==antlr::nullToken && _ttype!=antlr::Token::SKIP ) {
	   _token = makeToken(_ttype);
	   _token->setText(text.substr(_begin, text.length()-_begin));
	}
	_returnToken = _token;
	_saveIndex=0;
}


const unsigned long RequirementLexer::_tokenSet_0_data_[] = { 0UL, 0UL, 134217726UL, 134217726UL, 0UL, 0UL, 0UL, 0UL };
const antlr::BitSet RequirementLexer::_tokenSet_0(_tokenSet_0_data_,8);
const unsigned long RequirementLexer::_tokenSet_1_data_[] = { 0UL, 67043328UL, 126UL, 126UL, 0UL, 0UL, 0UL, 0UL };
// 0 1 2 3 4 5 6 7 8 
const antlr::BitSet RequirementLexer::_tokenSet_1(_tokenSet_1_data_,8);
const unsigned long RequirementLexer::_tokenSet_2_data_[] = { 4294967295UL, 4294967291UL, 4026531839UL, 4294967295UL, 0UL, 0UL, 0UL, 0UL };
// 0x0 0x1 0x2 0x3 0x4 0x5 0x6 0x7 0x8 0x9 0xa 0xb 0xc 0xd 0xe 0xf 0x10 
// 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x18 0x19 0x1a 0x1b 0x1c 0x1d 0x1e 
// 0x1f   ! # $ % & \' ( ) * + , - . / 0 1 2 3 4 5 6 7 8 
const antlr::BitSet RequirementLexer::_tokenSet_2(_tokenSet_2_data_,8);
const unsigned long RequirementLexer::_tokenSet_3_data_[] = { 4294966271UL, 4294967295UL, 4294967295UL, 4294967295UL, 0UL, 0UL, 0UL, 0UL };
// 0x0 0x1 0x2 0x3 0x4 0x5 0x6 0x7 0x8 0x9 0xb 0xc 0xd 0xe 0xf 0x10 0x11 
// 0x12 0x13 0x14 0x15 0x16 0x17 0x18 0x19 0x1a 0x1b 0x1c 0x1d 0x1e 0x1f 
//   ! \" # $ % & \' ( ) * + , - . / 0 1 2 3 4 5 6 7 8 
const antlr::BitSet RequirementLexer::_tokenSet_3(_tokenSet_3_data_,8);
const unsigned long RequirementLexer::_tokenSet_4_data_[] = { 4294967295UL, 4294934527UL, 4294967295UL, 4294967295UL, 0UL, 0UL, 0UL, 0UL };
// 0x0 0x1 0x2 0x3 0x4 0x5 0x6 0x7 0x8 0x9 0xa 0xb 0xc 0xd 0xe 0xf 0x10 
// 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x18 0x19 0x1a 0x1b 0x1c 0x1d 0x1e 
// 0x1f   ! \" # $ % & \' ( ) * + , - . 0 1 2 3 4 5 6 7 8 
const antlr::BitSet RequirementLexer::_tokenSet_4(_tokenSet_4_data_,8);
const unsigned long RequirementLexer::_tokenSet_5_data_[] = { 4294967295UL, 4294966271UL, 4294967295UL, 4294967295UL, 0UL, 0UL, 0UL, 0UL };
// 0x0 0x1 0x2 0x3 0x4 0x5 0x6 0x7 0x8 0x9 0xa 0xb 0xc 0xd 0xe 0xf 0x10 
// 0x11 0x12 0x13 0x14 0x15 0x16 0x17 0x18 0x19 0x1a 0x1b 0x1c 0x1d 0x1e 
// 0x1f   ! \" # $ % & \' ( ) + , - . / 0 1 2 3 4 5 6 7 8 
const antlr::BitSet RequirementLexer::_tokenSet_5(_tokenSet_5_data_,8);

ANTLR_END_NAMESPACE
