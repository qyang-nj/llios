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
// renum - enumerator for code (usually bundle) resources
//
#include "renum.h"
#include <security_utilities/unix++.h>
#include <security_utilities/debugging.h>

namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


ResourceEnumerator::ResourceEnumerator(string path)
	: mPath(path)
{
	assert(!mPath.empty());
	const char * paths[2] = { path.c_str(), NULL };
	mFTS = fts_open((char * const *)paths, FTS_PHYSICAL | FTS_COMFOLLOW | FTS_NOCHDIR, NULL);
	if (!mFTS)
		UnixError::throwMe();
}

ResourceEnumerator::~ResourceEnumerator()
{
	checkError(fts_close(mFTS));
}


FTSENT *ResourceEnumerator::next(string &path)
{
	while (FTSENT *ent = fts_read(mFTS)) {
		switch (ent->fts_info) {
		case FTS_F:
			path = ent->fts_path + mPath.size() + 1;	// skip prefix + "/"
			return ent;
		case FTS_D:
			secdebug("rdirenum", "entering %s", ent->fts_path);
			break;
		case FTS_DP:
			secdebug("rdirenum", "leaving %s", ent->fts_path);
			break;
		case FTS_SL:
			secdebug("rdirenum", "symlink ignored: %s", ent->fts_path);
			break;
		default:
			secdebug("rdirenum", "type %d (errno %d): %s",
				ent->fts_info, ent->fts_errno, ent->fts_path);
			break;
		}
	}
	return NULL;
}


} // end namespace CodeSigning
} // end namespace Security
