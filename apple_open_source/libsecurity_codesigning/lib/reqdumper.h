/*
 * Copyright (c) 2006-2007 Apple Inc. All Rights Reserved.
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
// reqdumper - Requirement un-parsing (disassembly)
//
#ifndef _H_REQDUMPER
#define _H_REQDUMPER

#include "reqreader.h"
#include <ctype.h>


namespace Security {
namespace CodeSigning {


//
// A decompiler for (compiled) requirements programs.
// This is intended to produce compiler-ready source, and the
// (decompile . compile) cycle is meant to be loss-less.
//
// Note that a Dumper is a type of Interpreter, so it can use the program stream
// accessors of the Interpreter. However, the evaluaton Context is absent, so
// actual validation functions must not be called.
//
class Dumper : public Requirement::Reader {
public:
	explicit Dumper(const Requirement *req, bool debug = false)
		: Reader(req), mDebug(debug) { }
	
	enum SyntaxLevel {
		slPrimary,		// syntax primary
		slAnd,			// conjunctive
		slOr,			// disjunctive
		slTop			// where we start
	};
	
	void dump();		// decompile this (entire) requirement
	void expr(SyntaxLevel level = slTop); // decompile one requirement expression
	
	std::string value() const { return mOutput; }
	operator std::string () const { return value(); }
	
	typedef unsigned char Byte;
	
public:
	// all-in-one dumping
	static string dump(const Requirements *reqs, bool debug = false);
	static string dump(const Requirement *req, bool debug = false);
	static string dump(const BlobCore *req, bool debug = false);	// dumps either

protected:
	enum PrintMode {
		isSimple,		// printable and does not require quotes
		isPrintable,	// can be quoted safely
		isBinary		// contains binary bytes (use 0xnnn form)
	};
	void data(PrintMode bestMode = isSimple, bool dotOkay = false);
	void dotString() { data(isSimple, true); }
	void quotedString() { data(isPrintable); }
	void hashData();	// H"bytes"
	void certSlot();	// symbolic certificate slot indicator (explicit)
	void match();		// a match suffix (op + value)
	
	void print(const char *format, ...);

private:
	void printBytes(const Byte *data, size_t length); // just write hex bytes
	
private:
	std::string mOutput;		// output accumulator
	bool mDebug;				// include debug output in mOutput
};


}	// CodeSigning
}	// Security

#endif //_H_REQDUMPER
