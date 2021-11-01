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
#include "filediskrep.h"
#include "StaticCode.h"
#include <security_utilities/macho++.h>
#include <cstring>


namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


//
// Everything's lazy in here
//
FileDiskRep::FileDiskRep(const char *path)
	: SingleDiskRep(path)
{
	CODESIGN_DISKREP_CREATE_FILE(this, (char*)path);
}


//
// Produce an extended attribute name from a canonical slot name
//
string FileDiskRep::attrName(const char *name)
{
	static const char prefix[] = "com.apple.cs.";
	return string(prefix) + name;
}


//
// Retrieve an extended attribute by name
//
CFDataRef FileDiskRep::getAttribute(const char *name)
{
	string aname = attrName(name);
	try {
		ssize_t length = fd().getAttrLength(aname);
		if (length < 0)
			return NULL;		// no such attribute
		CFMallocData buffer(length);
		fd().getAttr(aname, buffer, length);
		return buffer;
	} catch (const UnixError &err) {
		// recover some errors that happen in (relatively) benign circumstances
		switch (err.error) {
		case ENOTSUP:	// no extended attributes on this filesystem
		case EPERM:		// filesystem objects to name(?)
			return NULL;
		default:
			throw;
		}
	}
}


//
// Extract and return a component by slot number.
// If we have a Mach-O binary, use embedded components.
// Otherwise, look for and return the extended attribute, if any.
//
CFDataRef FileDiskRep::component(CodeDirectory::SpecialSlot slot)
{
	if (const char *name = CodeDirectory::canonicalSlotName(slot))
		return getAttribute(name);
	else
		return NULL;
}


//
// Generate a suggested set of internal requirements.
// We don't really have to say much. However, if we encounter a file that
// starts with the magic "#!" script marker, we do suggest that this should
// be a valid host if we can reasonably make out what that is.
//
const Requirements *FileDiskRep::defaultRequirements(const Architecture *, const SigningContext &ctx)
{
	// read start of file
	char buffer[256];
	size_t length = fd().read(buffer, sizeof(buffer), 0);
	if (length > 3 && buffer[0] == '#' && buffer[1] == '!' && buffer[2] == '/') {
		// isolate (full) path element in #!/full/path -some -other -stuff
		if (length == sizeof(buffer))
			length--;
		buffer[length] = '\0';
		char *cmd = buffer + 2;
		cmd[strcspn(cmd, " \t\n\r\f")] = '\0';
		secdebug("filediskrep", "looks like a script for %s", cmd);
		if (cmd[1])
			try {
				// find path on disk, get designated requirement (if signed)
				string path = ctx.sdkPath(cmd);
				if (RefPointer<DiskRep> rep = DiskRep::bestFileGuess(path))
					if (SecPointer<SecStaticCode> code = new SecStaticCode(rep))
						if (const Requirement *req = code->designatedRequirement()) {
							CODESIGN_SIGN_DEP_INTERP(this, (char*)cmd, (void*)req);
							// package up as host requirement and return that
							Requirements::Maker maker;
							maker.add(kSecHostRequirementType, req->clone());
							return maker.make();
						}
			} catch (...) {
				secdebug("filediskrep", "exception getting host requirement (ignored)");
			}
	}
	return NULL;
}


string FileDiskRep::format()
{
	return "generic";
}


//
// FileDiskRep::Writers
//
DiskRep::Writer *FileDiskRep::writer()
{
	return new Writer(this);
}


//
// Write a component.
// Note that this isn't concerned with Mach-O writing; this is handled at
// a much higher level. If we're called, it's extended attribute time.
//
void FileDiskRep::Writer::component(CodeDirectory::SpecialSlot slot, CFDataRef data)
{
	try {
		fd().setAttr(attrName(CodeDirectory::canonicalSlotName(slot)),
			CFDataGetBytePtr(data), CFDataGetLength(data));
	} catch (const UnixError &error) {
		if (error.error == ERANGE)
			MacOSError::throwMe(errSecCSCMSTooLarge);
		throw;
	}
}


//
// Clear all signing data
//
void FileDiskRep::Writer::remove()
{
	for (CodeDirectory::SpecialSlot slot = 0; slot < cdSlotCount; slot++)
		if (const char *name = CodeDirectory::canonicalSlotName(slot))
			fd().removeAttr(attrName(name));
	fd().removeAttr(attrName(kSecCS_SIGNATUREFILE));
}


//
// We are NOT the preferred store for components because our approach
// (extended attributes) suffers from some serious limitations.
//
bool FileDiskRep::Writer::preferredStore()
{
	return false;
}


} // end namespace CodeSigning
} // end namespace Security
