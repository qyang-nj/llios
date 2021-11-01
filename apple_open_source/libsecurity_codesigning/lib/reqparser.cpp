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
// reqparser - interface to Requirement language parser/compiler
//
#include "reqparser.h"
#include "antlrplugin.h"
#include "cserror.h"
#include "codesigning_dtrace.h"
#include <CoreFoundation/CoreFoundation.h>
#include <security_utilities/osxcode.h>

namespace Security {
namespace CodeSigning {


struct PluginHost {
	PluginHost();
	RefPointer<LoadableBundle> plugin;
	AntlrPlugin *antlr;
};

ModuleNexus<PluginHost> plugin;


//
// The PluginHost constructor runs under the protection of ModuleNexus's constructor,
// so it doesn't have to worry about thread safety and such.
//
PluginHost::PluginHost()
{
	if (CFBundleRef securityFramework = CFBundleGetBundleWithIdentifier(CFSTR("com.apple.security")))
		if (CFRef<CFURLRef> plugins = CFBundleCopyBuiltInPlugInsURL(securityFramework))
			if (CFRef<CFURLRef> pluginURL = makeCFURL("csparser.bundle", true, plugins)) {
				plugin = new LoadableBundle(cfString(pluginURL).c_str());
				plugin->load();
				CODESIGN_LOAD_ANTLR();
				antlr = reinterpret_cast<FindAntlrPlugin *>(plugin->lookupSymbol(FINDANTLRPLUGIN))();
				return;
			}
				
	// can't load plugin - fail
	MacOSError::throwMe(errSecCSInternalError);
}


//
// Drive a parsing function through the plugin harness and translate any errors
// into a CFError exception.
//
template <class Result, class Source>
const Result *parse(Source source, const Result *(*AntlrPlugin::*func)(Source, string &))
{
	string errors;
	if (const Result *result = (plugin().antlr->*func)(source, errors))
		return result;
	else
		CSError::throwMe(errSecCSReqInvalid, kSecCFErrorRequirementSyntax, CFTempString(errors));		
}


//
// Implement the template instances by passing them through the plugin's eye-of-the-needle.
// Any other combination of input and output types will cause linker errors.
//
template <>
const Requirement *RequirementParser<Requirement>::operator () (std::FILE *source)
{
	return parse(source, &AntlrPlugin::fileRequirement);
}

template <>
const Requirement *RequirementParser<Requirement>::operator () (const std::string &source)
{
	return parse(source, &AntlrPlugin::stringRequirement);
}

template <>
const Requirements *RequirementParser<Requirements>::operator () (std::FILE *source)
{
	return parse(source, &AntlrPlugin::fileRequirements);
}

template <>
const Requirements *RequirementParser<Requirements>::operator () (const std::string &source)
{
	return parse(source, &AntlrPlugin::stringRequirements);
}

template <>
const BlobCore *RequirementParser<BlobCore>::operator () (std::FILE *source)
{
	return parse(source, &AntlrPlugin::fileGeneric);
}

template <>
const BlobCore *RequirementParser<BlobCore>::operator () (const std::string &source)
{
	return parse(source, &AntlrPlugin::stringGeneric);
}


}	// CodeSigning
}	// Security
