/*
 * Copyright (c) 2006-2010 Apple Inc. All Rights Reserved.
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
// csutilities - miscellaneous utilities for the code signing implementation
//
// This is a collection of odds and ends that wouldn't fit anywhere else.
// The common theme is that the contents are otherwise naturally homeless.
//
#ifndef _H_CSUTILITIES
#define _H_CSUTILITIES

#include <Security/Security.h>
#include <security_utilities/hashing.h>
#include <security_utilities/unix++.h>
#include <security_cdsa_utilities/cssmdata.h>
#include <copyfile.h>
#include <asl.h>
#include <cstdarg>

namespace Security {
namespace CodeSigning {


//
// Calculate canonical hashes of certificate.
// This is simply defined as (always) the SHA1 hash of the DER.
//
void hashOfCertificate(const void *certData, size_t certLength, SHA1::Digest digest);
void hashOfCertificate(SecCertificateRef cert, SHA1::Digest digest);


//
// Calculate hashes of (a section of) a file.
// Starts at the current file position.
// Extends to end of file, or (if limit > 0) at most limit bytes.
// Returns number of bytes digested.
//
template <class _Hash>
size_t hashFileData(const char *path, _Hash *hasher)
{
	UnixPlusPlus::AutoFileDesc fd(path);
	return hashFileData(fd, hasher);
}

template <class _Hash>
size_t hashFileData(UnixPlusPlus::FileDesc fd, _Hash *hasher, size_t limit = 0)
{
	unsigned char buffer[4096];
	size_t total = 0;
	for (;;) {
		size_t size = sizeof(buffer);
		if (limit && limit < size)
			size = limit;
		size_t got = fd.read(buffer, size);
		total += got;
		if (fd.atEnd())
			break;
		hasher->update(buffer, got);
		if (limit && (limit -= got) == 0)
			break;
	}
	return total;
}


//
// Check to see if a certificate contains a particular field, by OID. This works for extensions,
// even ones not recognized by the local CL. It does not return any value, only presence.
//
bool certificateHasField(SecCertificateRef cert, const CSSM_OID &oid);
bool certificateHasPolicy(SecCertificateRef cert, const CSSM_OID &policyOid);


//
// Encapsulation of the copyfile(3) API.
// This is slated to go into utilities once stable.
//
class Copyfile {
public:
	Copyfile();
	~Copyfile()	{ copyfile_state_free(mState); }
	
	operator copyfile_state_t () const { return mState; }
	
	void set(uint32_t flag, const void *value);
	void get(uint32_t flag, void *value);
	
	void operator () (const char *src, const char *dst, copyfile_flags_t flags);

private:
	void check(int rc);
	
private:
	copyfile_state_t mState;
};


//
// MessageTracer support
//
class MessageTrace {
public:
	MessageTrace(const char *domain, const char *signature);
	void add(const char *key, const char *format, ...);
	void send(const char *format, ...);

private:
	aslmsg mAsl;
};


//
// A reliable uid set/reset bracket
//
class UidGuard {
public:
	UidGuard() : mPrevious(-1) { }
	UidGuard(uid_t uid) : mPrevious(-1) { seteuid(uid); }
	~UidGuard()
	{
		if (active())
			UnixError::check(::seteuid(mPrevious));
	}
	
	bool seteuid(uid_t uid)
	{
		if (uid == geteuid())
			return true;	// no change, don't bother the kernel
		if (!active())
			mPrevious = ::geteuid();
		return ::seteuid(uid) == 0;
	}
	
	bool active() const { return mPrevious != uid_t(-1); }
	operator bool () const { return active(); }
	uid_t saved() const { assert(active()); return mPrevious; }

private:
	uid_t mPrevious;
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_CSUTILITIES
