#ifndef INC_RequirementLexer_hpp_
#define INC_RequirementLexer_hpp_

#include <antlr/config.hpp>
/* $ANTLR 2.7.7 (20120228): "requirements.grammar" -> "RequirementLexer.hpp"$ */
#include <antlr/CommonToken.hpp>
#include <antlr/InputBuffer.hpp>
#include <antlr/BitSet.hpp>
#include "RequirementParserTokenTypes.hpp"
#include <antlr/CharScanner.hpp>

#include "requirement.h"
using namespace CodeSigning;
typedef Requirement::Maker Maker;

ANTLR_BEGIN_NAMESPACE(Security_CodeSigning)
class CUSTOM_API RequirementLexer : public antlr::CharScanner, public RequirementParserTokenTypes
{
private:
	void initLiterals();
public:
	bool getCaseSensitiveLiterals() const
	{
		return true;
	}
public:
	RequirementLexer(std::istream& in);
	RequirementLexer(antlr::InputBuffer& ib);
	RequirementLexer(const antlr::LexerSharedInputState& state);
	antlr::RefToken nextToken();
	protected: void mIDENT(bool _createToken);
	public: void mDOTKEY(bool _createToken);
	public: void mINTEGER(bool _createToken);
	public: void mPATHNAME(bool _createToken);
	public: void mHASHCONSTANT(bool _createToken);
	protected: void mHEX(bool _createToken);
	public: void mHEXCONSTANT(bool _createToken);
	public: void mSTRING(bool _createToken);
	public: void mARROW(bool _createToken);
	public: void mSEMI(bool _createToken);
	public: void mLPAREN(bool _createToken);
	public: void mRPAREN(bool _createToken);
	public: void mLBRACK(bool _createToken);
	public: void mRBRACK(bool _createToken);
	public: void mLESS(bool _createToken);
	public: void mGT(bool _createToken);
	public: void mLE(bool _createToken);
	public: void mGE(bool _createToken);
	public: void mCOMMA(bool _createToken);
	public: void mEQL(bool _createToken);
	public: void mEQQL(bool _createToken);
	public: void mSUBS(bool _createToken);
	public: void mNEG(bool _createToken);
	public: void mNOT(bool _createToken);
	public: void mSTAR(bool _createToken);
	public: void mWS(bool _createToken);
	public: void mSHELLCOMMENT(bool _createToken);
	public: void mC_COMMENT(bool _createToken);
	public: void mCPP_COMMENT(bool _createToken);
private:
	
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
};

ANTLR_END_NAMESPACE
#endif /*INC_RequirementLexer_hpp_*/
