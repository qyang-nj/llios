#ifndef INC_RequirementParser_hpp_
#define INC_RequirementParser_hpp_

#include <antlr/config.hpp>
/* $ANTLR 2.7.7 (20120228): "requirements.grammar" -> "RequirementParser.hpp"$ */
#include <antlr/TokenStream.hpp>
#include <antlr/TokenBuffer.hpp>
#include "RequirementParserTokenTypes.hpp"
#include <antlr/LLkParser.hpp>


#include "requirement.h"
using namespace CodeSigning;
typedef Requirement::Maker Maker;

ANTLR_BEGIN_NAMESPACE(Security_CodeSigning)
class CUSTOM_API RequirementParser : public antlr::LLkParser, public RequirementParserTokenTypes
{

public:
	std::string errors;
	void reportError(const antlr::RecognitionException &ex);
	void reportError(const std::string &s);

private:
	static string hexString(const string &s);
	static void hashString(const string &s, SHA1::Digest hash);
	void certMatchOperation(Maker &maker, int32_t slot, string key);
public:
	void initializeASTFactory( antlr::ASTFactory& factory );
protected:
	RequirementParser(antlr::TokenBuffer& tokenBuf, int k);
public:
	RequirementParser(antlr::TokenBuffer& tokenBuf);
protected:
	RequirementParser(antlr::TokenStream& lexer, int k);
public:
	RequirementParser(antlr::TokenStream& lexer);
	RequirementParser(const antlr::ParserSharedInputState& state);
	int getNumTokens() const
	{
		return RequirementParser::NUM_TOKENS;
	}
	const char* getTokenName( int type ) const
	{
		if( type > getNumTokens() ) return 0;
		return RequirementParser::tokenNames[type];
	}
	const char* const* getTokenNames() const
	{
		return RequirementParser::tokenNames;
	}
	public: BlobCore * autosense();
	public: Requirement * requirement();
	public: Requirements * requirementSet();
	public: uint32_t  requirementType();
	public: Requirement * requirementElement();
	public: void expr(
		Maker &maker
	);
	public: void fluff();
	public: void term(
		Maker &maker
	);
	public: void primary(
		Maker &maker
	);
	public: void certspec(
		Maker &maker
	);
	public: void infospec(
		Maker &maker
	);
	public: void entitlementspec(
		Maker &maker
	);
	public: void eql();
	public: string  identifierString();
	public: void hash(
		SHA1::Digest digest
	);
	public: void appleanchor(
		Maker &maker
	);
	public: int32_t  certSlot();
	public: void certslotspec(
		Maker &maker, int32_t slot
	);
	public: void empty();
	public: void certificateDigest(
		SHA1::Digest digest
	);
	public: string  bracketKey();
	public: void match_suffix(
		Maker &maker
	);
	public: string  datavalue();
	public: string  stringvalue();
	public: string  pathstring();
public:
	antlr::RefAST getAST()
	{
		return returnAST;
	}
	
protected:
	antlr::RefAST returnAST;
private:
	static const char* tokenNames[];
#ifndef NO_STATIC_CONSTS
	static const int NUM_TOKENS = 57;
#else
	enum {
		NUM_TOKENS = 57
	};
#endif
	
	static const unsigned long _tokenSet_0_data_[];
	static const antlr::BitSet _tokenSet_0;
	static const unsigned long _tokenSet_1_data_[];
	static const antlr::BitSet _tokenSet_1;
	static const unsigned long _tokenSet_2_data_[];
	static const antlr::BitSet _tokenSet_2;
	static const unsigned long _tokenSet_3_data_[];
	static const antlr::BitSet _tokenSet_3;
	static const unsigned long _tokenSet_4_data_[];
	static const antlr::BitSet _tokenSet_4;
	static const unsigned long _tokenSet_5_data_[];
	static const antlr::BitSet _tokenSet_5;
	static const unsigned long _tokenSet_6_data_[];
	static const antlr::BitSet _tokenSet_6;
	static const unsigned long _tokenSet_7_data_[];
	static const antlr::BitSet _tokenSet_7;
	static const unsigned long _tokenSet_8_data_[];
	static const antlr::BitSet _tokenSet_8;
	static const unsigned long _tokenSet_9_data_[];
	static const antlr::BitSet _tokenSet_9;
	static const unsigned long _tokenSet_10_data_[];
	static const antlr::BitSet _tokenSet_10;
	static const unsigned long _tokenSet_11_data_[];
	static const antlr::BitSet _tokenSet_11;
	static const unsigned long _tokenSet_12_data_[];
	static const antlr::BitSet _tokenSet_12;
	static const unsigned long _tokenSet_13_data_[];
	static const antlr::BitSet _tokenSet_13;
	static const unsigned long _tokenSet_14_data_[];
	static const antlr::BitSet _tokenSet_14;
	static const unsigned long _tokenSet_15_data_[];
	static const antlr::BitSet _tokenSet_15;
};

ANTLR_END_NAMESPACE
#endif /*INC_RequirementParser_hpp_*/
