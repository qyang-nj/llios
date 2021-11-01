/*
 * Copyright (c) 2006 Apple Computer, Inc. All Rights Reserved.
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
// reqmaker - Requirement assembler
//
#ifndef _H_REQMAKER
#define _H_REQMAKER

#include <security_codesigning/requirement.h>

namespace Security {
namespace CodeSigning {


//
// A Requirement::Maker is a tool for creating a Requirement blob.
// It's primarily an assember for the binary requirements (exprOp) language.
// Initialize it, call put() methods to generate the exprOp program, then
// call make() to get the assembled Requirement blob, malloc'ed for you.
// The Maker is not reusable.
//
class Requirement::Maker {
public:
	Maker(Kind k = exprForm);
	~Maker() { free(mBuffer); }
	
	template <class T>
	T *alloc(size_t size) { return reinterpret_cast<T *>(alloc(size)); }

	template <class T>
	void put(const T &value) { *alloc<Endian<T> >(sizeof(T)) = value; }
	void put(ExprOp op) { put(uint32_t(op)); }
	void put(MatchOperation op) { put(uint32_t(op)); }
	void put(const std::string &s) { putData(s.data(), s.size()); }
	void put(const char *s) { putData(s, strlen(s)); }
	void putData(const void *data, size_t length);
	void putData(CFStringRef s) { put(cfString(s)); }
	
	void anchor(int slot, SHA1::Digest digest);			// given slot/digest
	void anchor(int slot, const void *cert, size_t length); // given slot/cert
	void anchor();										// made-by-Apple
	void anchorGeneric();								// anything drawn from the Apple anchor
	
	void trustedAnchor();
	void trustedAnchor(int slot);
	
	void infoKey(const std::string &key, const std::string &value);
	void ident(const std::string &identHash);
	void cdhash(SHA1::Digest digest);
	
	void copy(const void *data, size_t length)
		{ memcpy(this->alloc(length), data, length); }
	void copy(const Requirement *req);				// inline expand
	
	//
	// Keep labels into exprOp code, and allow for "shifting in"
	// prefix code as needed (exprOp is a prefix-code language).
	//
	struct Label {
		const Offset pos;
		Label(const Maker &maker) : pos(maker.length()) { }
	};
	void *insert(const Label &label, size_t length = sizeof(uint32_t));
	
	template <class T>
	Endian<T> &insert(const Label &label, size_t length = sizeof(T))
	{ return *reinterpret_cast<Endian<T>*>(insert(label, length)); }

	//
	// Help with making operator chains (foo AND bar AND baz...).
	// Note that the empty case (no elements at all) must be resolved by the caller.
	//
	class Chain : public Label {
	public:
		Chain(Maker &myMaker, ExprOp op)
			: Label(myMaker), maker(myMaker), mJoiner(op), mCount(0) { }

		void add()
			{ if (mCount++) maker.insert<ExprOp>(*this) = mJoiner; }
	
		Maker &maker;
		bool empty() const { return mCount == 0; }
		unsigned count() const { return mCount; }

	private:
		ExprOp mJoiner;
		unsigned mCount;
	};
	
	
	//
	// Over-all construction management
	//
	void kind(Kind k) { mBuffer->kind(k); }
	size_t length() const { return mPC; }
	Requirement *make();
	Requirement *operator () () { return make(); }
	
protected:
	void require(size_t size);	
	void *alloc(size_t size);

private:
	Requirement *mBuffer;
	Offset mSize;
	Offset mPC;
};


}	// CodeSigning
}	// Security

#endif //_H_REQMAKER
