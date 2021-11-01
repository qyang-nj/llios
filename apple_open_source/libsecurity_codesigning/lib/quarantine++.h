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
// quarantine++ - interface to XAR-format archive files
//
#ifndef _H_QUARANTINEPLUSPLUS
#define _H_QUARANTINEPLUSPLUS

#include <security_utilities/utilities.h>
#include <CoreFoundation/CoreFoundation.h>

extern "C" {
#include <quarantine.h>
}

namespace Security {
namespace CodeSigning {


//
// A file quarantine object
//
class FileQuarantine {
public:	
	FileQuarantine(const char *path);
	FileQuarantine(int fd);
	virtual ~FileQuarantine();
	
	uint32_t flags() const
		{ return qtn_file_get_flags(mQtn); }
	bool flag(uint32_t f) const
		{ return this->flags() & f; }
	
	void setFlags(uint32_t flags);
	void setFlag(uint32_t flag);
	void clearFlag(uint32_t flag);
	
	void applyTo(const char *path);
	void applyTo(int fd);
	
	operator bool() const { return mQtn != 0; }
	bool quarantined() const { return mQuarantined; }

private:
	void check(int err);
	
private:
	qtn_file_t mQtn;		// qtn handle
	bool mQuarantined;		// has quarantine information
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_QUARANTINEPLUSPLUS
