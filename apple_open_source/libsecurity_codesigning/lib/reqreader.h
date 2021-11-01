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

//
// reqreader - Requirement language (exprOp) reader/scanner
//
#ifndef _H_REQREADER
#define _H_REQREADER

#include <security_codesigning/requirement.h>
#include <Security/SecCertificate.h>

namespace Security {
namespace CodeSigning {


//
// The Reader class provides structured access to a opExpr-type code requirement.
//
class Requirement::Reader {
public:
	Reader(const Requirement *req);
	
	const Requirement *requirement() const { return mReq; }
	
	template <class T> T get();
	void getData(const void *&data, size_t &length);
	
	std::string getString();
	const unsigned char *getHash();
	const unsigned char *getSHA1();
	
	template <class T> void getData(T *&data, size_t &length)
	{ return getData(reinterpret_cast<const void *&>(data), length); }

protected:
	void checkSize(size_t length)
	{
		if (mPC + length > mReq->length())
			MacOSError::throwMe(errSecCSReqInvalid);
	}
	
	void skip(size_t length);
	
	Offset pc() const { return mPC; }
	bool atEnd() const { return mPC >= mReq->length(); }
	
private:
	const Requirement * const mReq;
	Offset mPC;
};

template <class T>
T Requirement::Reader::get()
{
	checkSize(sizeof(T));
	const Endian<const T> *value = mReq->at<Endian<const T> >(mPC);
	mPC += sizeof(T);
	return *value;
}


}	// CodeSigning
}	// Security

#endif //_H_REQREADER
