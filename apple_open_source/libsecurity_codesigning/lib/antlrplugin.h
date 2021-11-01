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
// Plugin interface for internal Security plug-ins
//
#ifndef _H_ANTLRPLUGIN
#define _H_ANTLRPLUGIN

#include <Security/CodeSigning.h>
#include "requirement.h"
#include <cstdio>
#include <string>

namespace Security {
namespace CodeSigning {


//
// The plugin proxy.
//
// During loading, one instance of this object will be created by the plugin
// and returned through the (one and only) dynamically-linked method of the plugin.
// All further interaction then proceeds through methods of this object.
//
//
class AntlrPlugin {
public:
	typedef const Requirement *FileRequirement(std::FILE *source, std::string &errors);
	FileRequirement *fileRequirement;
	typedef const Requirements *FileRequirements(std::FILE *source, std::string &errors);
	FileRequirements *fileRequirements;
	typedef const BlobCore *FileGeneric(std::FILE *source, std::string &errors);
	FileGeneric *fileGeneric;
	typedef const Requirement *StringRequirement(std::string source, std::string &errors);
	StringRequirement *stringRequirement;
	typedef const Requirements *StringRequirements(std::string source, std::string &errors);
	StringRequirements *stringRequirements;
	typedef const BlobCore *StringGeneric(std::string source, std::string &errors);
	StringGeneric *stringGeneric;
};

extern "C" {
	AntlrPlugin *findAntlrPlugin();
	typedef AntlrPlugin *FindAntlrPlugin();
}

#define FINDANTLRPLUGIN "findAntlrPlugin"


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_ANTLRPLUGIN
