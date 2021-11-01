/*
 * Copyright (c) 2007 Apple Inc. All Rights Reserved.
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

#include "antlrplugin.h"
#include "cserror.h"
#include "RequirementLexer.hpp"
#include "RequirementParser.hpp"
#include <antlr/TokenStreamException.hpp>


namespace Security {
namespace CodeSigning {

namespace Parser = Security_CodeSigning;


//
// Lexer input adapters
//
class StdioInputStream : public antlr::InputBuffer {
public:
	StdioInputStream(FILE *fp) : mFile(fp) { }	
	int getChar() { return fgetc(mFile); }

private:
	FILE *mFile;
};

class StringInputStream : public antlr::InputBuffer {
public:
	StringInputStream(const string &s) : mInput(s), mPos(mInput.begin()) { }
	int getChar() { return (mPos == mInput.end()) ? EOF : *mPos++; }

private:
	string mInput;
	string::const_iterator mPos;
};


//
// Generic parser driver
//
template <class Input, class Source, class Result>
const Result *parse(Source source, Result *(Parser::RequirementParser::*rule)(), std::string &errors)
{
	Input input(source);
	Parser::RequirementLexer lexer(input);
	Parser::RequirementParser parser(lexer);
	try {
		const Result *result = (parser.*rule)();
		errors = parser.errors;
		if (errors.empty())
			return result;
		else
			::free((void *)result);
	} catch (const antlr::TokenStreamException &ex) {
		errors = ex.toString() + "\n";
	}
	return NULL;			// signal failure
}


//
// Hook up each supported parsing action to the plugin interface
//
const Requirement *fileRequirement(FILE *source, string &errors)
{ return parse<StdioInputStream>(source, &Parser::RequirementParser::requirement, errors); }

const Requirement *stringRequirement(string source, string &errors)
{ return parse<StringInputStream>(source, &Parser::RequirementParser::requirement, errors); }

const Requirements *fileRequirements(FILE *source, string &errors)
{ return parse<StdioInputStream>(source, &Parser::RequirementParser::requirementSet, errors); }

const Requirements *stringRequirements(string source, string &errors)
{ return parse<StringInputStream>(source, &Parser::RequirementParser::requirementSet, errors); }

const BlobCore *fileGeneric(FILE *source, string &errors)
{ return parse<StdioInputStream>(source, &Parser::RequirementParser::autosense, errors); }

const BlobCore *stringGeneric(string source, string &errors)
{ return parse<StringInputStream>(source, &Parser::RequirementParser::autosense, errors); }


//
// Basic plugin hookup
//
static AntlrPlugin plugin = {
	fileRequirement,
	fileRequirements,
	fileGeneric,
	stringRequirement,
	stringRequirements,
	stringGeneric
};

AntlrPlugin *findAntlrPlugin()
{
	return &plugin;
}


} // end namespace CodeSigning
} // end namespace Security
