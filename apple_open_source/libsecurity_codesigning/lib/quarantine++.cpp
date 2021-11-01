/*
 * Copyright (c) 2011 Apple Inc. All Rights Reserved.
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
// xar++ - interface to XAR-format archive files
//
#include "quarantine++.h"


namespace Security {
namespace CodeSigning {


//
// Check the int result of a qtn API call.
// If the error is "not quarantined," note in the object (no error).
// Other qtn-specific errors are arbitrarily mapped to ENOSYS (this isn't
// important enough to subclass CommonError).
//
void FileQuarantine::check(int err)
{
	switch (err) {
	case 0:
		mQuarantined = true;
		break;
	case QTN_NOT_QUARANTINED:
		mQuarantined = false;
		return;
	default:	// some flavor of quarantine-not-available
		UnixError::throwMe(err);
	}
}


FileQuarantine::~FileQuarantine()
{
	if (mQtn)
		qtn_file_free(mQtn);
}


FileQuarantine::FileQuarantine(const char *path)
{
	if (!(mQtn = qtn_file_alloc()))
		UnixError::throwMe();
	check(qtn_file_init_with_path(mQtn, path));
}

FileQuarantine::FileQuarantine(int fd)
{
	if (!(mQtn = qtn_file_alloc()))
		UnixError::throwMe();
	check(qtn_file_init_with_fd(mQtn, fd));
}


void FileQuarantine::setFlags(uint32_t flags)
{
	if (mQuarantined)
		check(qtn_file_set_flags(mQtn, flags));
}

void FileQuarantine::setFlag(uint32_t flag)
{
	if (mQuarantined)
		setFlags(flags() | flag);
}

void FileQuarantine::clearFlag(uint32_t flag)
{
	if (mQuarantined)
		setFlags(flags() & ~flag);
}

void FileQuarantine::applyTo(const char *path)
{
	check(qtn_file_apply_to_path(mQtn, path));
}

void FileQuarantine::applyTo(int fd)
{
	check(qtn_file_apply_to_fd(mQtn, fd));
}


} // end namespace CodeSigning
} // end namespace Security
