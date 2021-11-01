#ifndef INC_RequirementParserTokenTypes_hpp_
#define INC_RequirementParserTokenTypes_hpp_

ANTLR_BEGIN_NAMESPACE(Security_CodeSigning)
/* $ANTLR 2.7.7 (20120228): "requirements.grammar" -> "RequirementParserTokenTypes.hpp"$ */

#ifndef CUSTOM_API
# define CUSTOM_API
#endif

#ifdef __cplusplus
struct CUSTOM_API RequirementParserTokenTypes {
#endif
	enum {
		EOF_ = 1,
		ARROW = 4,
		LITERAL_guest = 5,
		LITERAL_host = 6,
		LITERAL_designated = 7,
		LITERAL_library = 8,
		LITERAL_plugin = 9,
		INTEGER = 10,
		LITERAL_or = 11,
		LITERAL_and = 12,
		LPAREN = 13,
		RPAREN = 14,
		NOT = 15,
		LITERAL_always = 16,
		LITERAL_true = 17,
		LITERAL_never = 18,
		LITERAL_false = 19,
		LITERAL_identifier = 20,
		LITERAL_cdhash = 21,
		LITERAL_anchor = 22,
		LITERAL_apple = 23,
		LITERAL_generic = 24,
		LITERAL_certificate = 25,
		LITERAL_cert = 26,
		LITERAL_trusted = 27,
		LITERAL_info = 28,
		LITERAL_entitlement = 29,
		LITERAL_exists = 30,
		EQL = 31,
		EQQL = 32,
		STAR = 33,
		SUBS = 34,
		LESS = 35,
		GT = 36,
		LE = 37,
		GE = 38,
		LBRACK = 39,
		RBRACK = 40,
		NEG = 41,
		LITERAL_leaf = 42,
		LITERAL_root = 43,
		HASHCONSTANT = 44,
		HEXCONSTANT = 45,
		DOTKEY = 46,
		STRING = 47,
		PATHNAME = 48,
		SEMI = 49,
		IDENT = 50,
		HEX = 51,
		COMMA = 52,
		WS = 53,
		SHELLCOMMENT = 54,
		C_COMMENT = 55,
		CPP_COMMENT = 56,
		NULL_TREE_LOOKAHEAD = 3
	};
#ifdef __cplusplus
};
#endif
ANTLR_END_NAMESPACE
#endif /*INC_RequirementParserTokenTypes_hpp_*/
