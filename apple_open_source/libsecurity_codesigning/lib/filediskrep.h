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
// filediskrep - single-file executable disk representation
//
#ifndef _H_FILEDISKREP
#define _H_FILEDISKREP

#include "singlediskrep.h"
#include "machorep.h"
#include <security_utilities/cfutilities.h>

namespace Security {
namespace CodeSigning {


//
// A FileDiskRep represents a single code file on disk. We assume nothing about
// the format or contents of the file and impose no structure on it, other than
// assuming that all relevant code is contained in the file's data bytes.
// By default, we seal the entire file data as a single page.
//
// This is the ultimate fallback disk format. It is used if no other pattern
// applies. As such it is important that we do not introduce any assumptions
// here. Know that you do not know what any of the file means.
//
// FileDiskrep stores components in extended file attributes, one attribute
// per component. Note that this imposes size limitations on component size
// that may well be prohibitive in some applications.
//
// This DiskRep does not support resource sealing.
//
class FileDiskRep : public SingleDiskRep {
public:
	FileDiskRep(const char *path);
	
	CFDataRef component(CodeDirectory::SpecialSlot slot);
	std::string format();
	
	const Requirements *defaultRequirements(const Architecture *arch, const SigningContext &ctx);
	
public:
	DiskRep::Writer *writer();
	class Writer;
	friend class Writer;
	
protected:
	CFDataRef getAttribute(const char *name);
	static std::string attrName(const char *name);
};


//
// The write side of a FileDiskRep
//
class FileDiskRep::Writer : public SingleDiskRep::Writer {
	friend class FileDiskRep;
public:
	void component(CodeDirectory::SpecialSlot slot, CFDataRef data);
	void remove();
	bool preferredStore();

protected:
	Writer(FileDiskRep *r) : SingleDiskRep::Writer(r, writerLastResort) { }
	RefPointer<FileDiskRep> rep;
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_FILEDISKREP
