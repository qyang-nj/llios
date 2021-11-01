/*
 * Copyright (c) 2006-2011 Apple Inc. All Rights Reserved.
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
// bundlediskrep - bundle directory disk representation
//
#ifndef _H_BUNDLEDISKREP
#define _H_BUNDLEDISKREP

#include "diskrep.h"
#include "machorep.h"

namespace Security {
namespace CodeSigning {


#define BUNDLEDISKREP_DIRECTORY		"_CodeSignature"
#define STORE_RECEIPT_DIRECTORY		"_MASReceipt"


//
// A BundleDiskRep represents a standard Mac OS X bundle on disk.
// The bundle is expected to have an Info.plist, and a "main executable file"
// of some sort (as indicated therein).
// The BundleDiskRep stores the necessary components in the main executable
// if it is in Mach-O format, or in files in a _CodeSignature directory if not.
// This DiskRep supports resource sealing.
//
class BundleDiskRep : public DiskRep {
public:
	BundleDiskRep(const char *path, const Context *ctx = NULL);
	BundleDiskRep(CFBundleRef ref, const Context *ctx = NULL);
	
	CFDataRef component(CodeDirectory::SpecialSlot slot);
	CFDataRef identification();
	std::string mainExecutablePath();
	CFURLRef canonicalPath();
	std::string resourcesRootPath();
	void adjustResources(ResourceBuilder &builder);
	Universal *mainExecutableImage();
	size_t signingBase();
	size_t signingLimit();
	std::string format();
	CFArrayRef modifiedFiles();
	UnixPlusPlus::FileDesc &fd();
	void flush();
	
	std::string recommendedIdentifier(const SigningContext &ctx);
	CFDictionaryRef defaultResourceRules(const SigningContext &ctx);
	const Requirements *defaultRequirements(const Architecture *arch, const SigningContext &ctx);
	size_t pageSize(const SigningContext &ctx);

	CFBundleRef bundle() const { return mBundle; }
	
public:
	Writer *writer();
	class Writer;
	friend class Writer;
	
protected:
	std::string metaPath(const char *name);
	CFDataRef metaData(const char *name) { return cfLoadFile(CFTempURL(metaPath(name))); }
	void createMeta();						// (try to) create the meta-file directory
	
private:
	void setup(const Context *ctx);			// shared init
	void checkModifiedFile(CFMutableArrayRef files, CodeDirectory::SpecialSlot slot);

private:
	CFRef<CFBundleRef> mBundle;
	std::string mMetaPath;					// path to directory containing signing files
	bool mMetaExists;						// separate meta-file directory exists
	CFRef<CFURLRef> mMainExecutableURL;		// chosen main executable URL
	bool mInstallerPackage;					// is an installer (not executable) bundle
	string mFormat;							// format description string
	RefPointer<DiskRep> mExecRep;			// DiskRep for main executable file
};


//
// Writers
//
//
class BundleDiskRep::Writer : public DiskRep::Writer {
	friend class BundleDiskRep;
public:
	Writer(BundleDiskRep *r);
	
	void component(CodeDirectory::SpecialSlot slot, CFDataRef data);
	void remove();
	void flush();
	
protected:
	DiskRep *execRep() { return rep->mExecRep; }
	void remove(CodeDirectory::SpecialSlot slot);

protected:
	RefPointer<BundleDiskRep> rep;
	RefPointer<DiskRep::Writer> execWriter;
	bool mMadeMetaDirectory;
};


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_BUNDLEDISKREP
