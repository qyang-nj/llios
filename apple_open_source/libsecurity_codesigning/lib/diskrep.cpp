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
// diskrep - disk representations of code
//
#include "diskrep.h"
#include <sys/stat.h>
#include <CoreFoundation/CFBundlePriv.h>

// specific disk representations created by the bestGuess() function
#include "filediskrep.h"
#include "bundlediskrep.h"
#include "cfmdiskrep.h"
#include "slcrep.h"


namespace Security {
namespace CodeSigning {

using namespace UnixPlusPlus;


//
// Abstract features
//
DiskRep::DiskRep()
{
}

DiskRep::~DiskRep()
{
	CODESIGN_DISKREP_DESTROY(this);
}


//
// Normal DiskReps are their own base.
//
DiskRep *DiskRep::base()
{
	return this;
}


//
// By default, DiskReps are read-only.
//
DiskRep::Writer *DiskRep::writer()
{
	MacOSError::throwMe(errSecCSUnimplemented);
}


void DiskRep::Writer::addDiscretionary(CodeDirectory::Builder &)
{
	// do nothing
}


//
// Given a file system path, come up with the most likely correct
// disk representation for what's there.
// This is, strictly speaking, a heuristic that could be fooled - there's
// no fool-proof rule for figuring this out. But we'd expect this to work
// fine in ordinary use. If you happen to know what you're looking at
// (say, a bundle), then just create the suitable subclass of DiskRep directly.
// That's quite legal.
// The optional context argument can provide additional information that guides the guess.
//
DiskRep *DiskRep::bestGuess(const char *path, const Context *ctx)
{
	try {
		if (!(ctx && ctx->fileOnly)) {
			struct stat st;
			if (::stat(path, &st))
				UnixError::throwMe();
				
			// if it's a directory, assume it's a bundle
			if ((st.st_mode & S_IFMT) == S_IFDIR)	// directory - assume bundle
				return new BundleDiskRep(path, ctx);
			
			// see if it's the main executable of a recognized bundle
			if (CFRef<CFURLRef> pathURL = makeCFURL(path))
				if (CFRef<CFBundleRef> bundle = _CFBundleCreateWithExecutableURLIfMightBeBundle(NULL, pathURL))
						return new BundleDiskRep(bundle, ctx);
		}
		
		// try the various single-file representations
		AutoFileDesc fd(path, O_RDONLY);
		if (MachORep::candidate(fd))
			return new MachORep(path, ctx);
		if (CFMDiskRep::candidate(fd))
			return new CFMDiskRep(path);
		if (DYLDCacheRep::candidate(fd))
			return new DYLDCacheRep(path);

		// ultimate fallback - the generic file representation
		return new FileDiskRep(path);

	} catch (const CommonError &error) {
		switch (error.unixError()) {
		case ENOENT:
			MacOSError::throwMe(errSecCSStaticCodeNotFound);
		default:
			throw;
		}
	}
}


DiskRep *DiskRep::bestFileGuess(const char *path, const Context *ctx)
{
	Context dctx;
	if (ctx)
		dctx = *ctx;
	dctx.fileOnly = true;
	return bestGuess(path, &dctx);
}


//
// Given a main executable known to be a Mach-O binary, and an offset into
// the file of the actual architecture desired (of a Universal file),
// produce a suitable MachORep.
// This function does not consider non-MachO binaries. It does however handle
// bundles with Mach-O main executables correctly.
//
DiskRep *DiskRep::bestGuess(const char *path, size_t archOffset)
{
	try {
		// is it the main executable of a bundle?
		if (CFRef<CFURLRef> pathURL = makeCFURL(path))
			if (CFRef<CFBundleRef> bundle = _CFBundleCreateWithExecutableURLIfMightBeBundle(NULL, pathURL)) {
				Context ctx; ctx.offset = archOffset;
				return new BundleDiskRep(bundle, &ctx);	// ask bundle to make bundle-with-MachO-at-offset
			}
		// else, must be a Mach-O binary
		Context ctx; ctx.offset = archOffset;
		return new MachORep(path, &ctx);
	} catch (const CommonError &error) {
		switch (error.unixError()) {
		case ENOENT:
			MacOSError::throwMe(errSecCSStaticCodeNotFound);
		default:
			throw;
		}
	}
}


//
// Default behaviors of DiskRep
//
string DiskRep::resourcesRootPath()
{
	return "";		// has no resources directory
}

void DiskRep::adjustResources(ResourceBuilder &builder)
{
	// do nothing
}

Universal *DiskRep::mainExecutableImage()
{
	return NULL;	// no Mach-O executable
}

size_t DiskRep::signingBase()
{
	return 0;		// whole file (start at beginning)
}

CFArrayRef DiskRep::modifiedFiles()
{
	// by default, claim (just) the main executable modified
	CFRef<CFURLRef> mainURL = makeCFURL(mainExecutablePath());
	return makeCFArray(1, mainURL.get());
}

void DiskRep::flush()
{
	// nothing cached
}


CFDictionaryRef DiskRep::defaultResourceRules(const SigningContext &)
{
	return NULL;	// none
}

const Requirements *DiskRep::defaultRequirements(const Architecture *, const SigningContext &)
{
	return NULL;	// none
}

size_t DiskRep::pageSize(const SigningContext &)
{
	return monolithicPageSize;	// unpaged (monolithic)
}


//
// Given some string (usually a pathname), derive a suggested signing identifier
// in a canonical way (so there's some consistency).
//
// This is a heuristic. First we lop off any leading directories and final (non-numeric)
// extension. Then we walk backwards, eliminating numeric extensions except the first one.
// Thus, libfrotz7.3.5.dylib becomes libfrotz7, mumble.77.plugin becomes mumble.77,
// and rumble.rb becomes rumble. This isn't perfect, but it ought to handle 98%+ of
// the common varieties out there. Specify an explicit identifier for the oddballs.
//
// This is called by the various recommendedIdentifier() methods, who are
// free to modify or override it.
//
// Note: We use strchr("...") instead of is*() here because we do not
// wish to be influenced by locale settings.
//
std::string DiskRep::canonicalIdentifier(const std::string &name)
{
	string s = name;
	string::size_type p;
	
	// lop off any directory prefixes
	if ((p = s.rfind('/')) != string::npos)
		s = s.substr(p+1);

	// remove any final extension (last dot) unless it's numeric
	if ((p = s.rfind('.')) != string::npos && !strchr("0123456789", s[p+1]))
		s = s.substr(0, p);
	
	// eat numeric suffixes except the first one; roughly:
	// foo.2.3.4 => foo.2, foo2.3 => foo2, foo.9 => foo.9, foo => foo
	if (strchr("0123456789.", s[0]))			// starts with digit or .
		return s;								// ... so don't mess with it
	p = s.size()-1;
	// foo3.5^, foo.3.5^, foo3^, foo.3^, foo^
	while (strchr("0123456789.", s[p]))
		p--;
	// fo^o3.5, fo^o.3.5, fo^o3, fo^o.3, fo^o
	p++;
	// foo^3.5, foo^.3.5, foo^3, foo^.3, foo^
	if (s[p] == '.')
		p++;
	// foo^3.5, foo.^3.5, foo^3, foo.^3, foo^
	while (p < s.size() && strchr("0123456789", s[p]))
		p++;
	// foo3^.5, foo.3^.5, foo3^, foo.3^, foo^
	return s.substr(0, p);
}


//
// Writers
//
DiskRep::Writer::Writer(uint32_t attrs)
	: mArch(CPU_TYPE_ANY), mAttributes(attrs)
{
}

DiskRep::Writer::~Writer()
{ /* virtual */ }

uint32_t DiskRep::Writer::attributes() const
{ return mAttributes; }

void DiskRep::Writer::flush()
{ /* do nothing */ }

void DiskRep::Writer::remove()
{
	MacOSError::throwMe(errSecCSNotSupported);
}


} // end namespace CodeSigning
} // end namespace Security
