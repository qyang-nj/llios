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
// resource directory construction and verification
//
#include "resources.h"
#include "csutilities.h"
#include <Security/CSCommon.h>
#include <security_utilities/unix++.h>
#include <security_utilities/cfmunge.h>

namespace Security {
namespace CodeSigning {


//
// Construction and maintainance
//
ResourceBuilder::ResourceBuilder(const std::string &root, CFDictionaryRef rulesDict, CodeDirectory::HashAlgorithm hashType)
	: ResourceEnumerator(root), mHashType(hashType)
{
	CFDictionary rules(rulesDict, errSecCSResourceRulesInvalid);
	rules.apply(this, &ResourceBuilder::addRule);
	mRawRules = rules;
}

ResourceBuilder::~ResourceBuilder()
{
	for (Rules::iterator it = mRules.begin(); it != mRules.end(); ++it)
		delete *it;
}


//
// Parse and add one matching rule
//
void ResourceBuilder::addRule(CFTypeRef key, CFTypeRef value)
{
	string pattern = cfString(key, errSecCSResourceRulesInvalid);
	unsigned weight = 1;
	uint32_t flags = 0;
	if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		if (value == kCFBooleanFalse)
			flags |= omitted;
	} else {
		CFDictionary rule(value, errSecCSResourceRulesInvalid);
		if (CFNumberRef weightRef = rule.get<CFNumberRef>("weight"))
			weight = cfNumber<unsigned int>(weightRef);
		if (CFBooleanRef omitRef = rule.get<CFBooleanRef>("omit"))
			if (omitRef == kCFBooleanTrue)
				flags |= omitted;
		if (CFBooleanRef optRef = rule.get<CFBooleanRef>("optional"))
			if (optRef == kCFBooleanTrue)
				flags |= optional;
	}
	addRule(new Rule(pattern, weight, flags));
}


//
// Locate the next non-ignored file, look up its rule, and return it.
// Returns NULL when we're out of files.
//
FTSENT *ResourceBuilder::next(string &path, Rule * &rule)
{
	while (FTSENT *ent = ResourceEnumerator::next(path)) {
		// find best matching rule
		Rule *bestRule = NULL;
		for (Rules::const_iterator it = mRules.begin(); it != mRules.end(); ++it) {
			Rule *rule = *it;
			if (rule->match(path.c_str())) {
				if (rule->flags & exclusion) {
					bestRule = NULL;
					break;
				}
				if (!bestRule || rule->weight > bestRule->weight)
					bestRule = rule;
		}
		}
		if (!bestRule || (bestRule->flags & omitted))
			continue;
		rule = bestRule;
		return ent;
	}
	return NULL;
}


//
// Build the ResourceDirectory given the currently established rule set.
//
CFDictionaryRef ResourceBuilder::build()
{
	secdebug("codesign", "start building resource directory");
	CFRef<CFMutableDictionaryRef> files = makeCFMutableDictionary();

	string path;
	Rule *rule;
	while (FTSENT *ent = next(path, rule)) {
		assert(rule);
		CFRef<CFDataRef> hash = hashFile(ent->fts_accpath);
		if (rule->flags == 0) {	// default case - plain hash
			cfadd(files, "{%s=%O}", path.c_str(), hash.get());
			secdebug("csresource", "%s added simple (rule %p)", path.c_str(), rule);
		} else {	// more complicated - use a sub-dictionary
			cfadd(files, "{%s={hash=%O,optional=%B}}",
				path.c_str(), hash.get(), rule->flags & optional);
			secdebug("csresource", "%s added complex (rule %p)", path.c_str(), rule);
		}
	}
	secdebug("codesign", "finished code directory with %d entries",
		int(CFDictionaryGetCount(files)));
	
	return cfmake<CFDictionaryRef>("{rules=%O,files=%O}", mRawRules.get(), files.get());
}


//
// Hash a file and return a CFDataRef with the hash
//
CFDataRef ResourceBuilder::hashFile(const char *path)
{
	UnixPlusPlus::AutoFileDesc fd(path);
	fd.fcntl(F_NOCACHE, true);		// turn off page caching (one-pass)
	MakeHash<ResourceBuilder> hasher(this);
	hashFileData(fd, hasher.get());
	Hashing::Byte digest[hasher->digestLength()];
	hasher->finish(digest);
	return CFDataCreate(NULL, digest, sizeof(digest));
}


//
// Regex matching objects
//
ResourceBuilder::Rule::Rule(const std::string &pattern, unsigned w, uint32_t f)
	: weight(w), flags(f)
{
	if (::regcomp(this, pattern.c_str(), REG_EXTENDED | REG_NOSUB))	//@@@ REG_ICASE?
		MacOSError::throwMe(errSecCSResourceRulesInvalid);
	secdebug("csresource", "%p rule %s added (weight %d, flags 0x%x)",
		this, pattern.c_str(), w, f);
}

ResourceBuilder::Rule::~Rule()
{
	::regfree(this);
}

bool ResourceBuilder::Rule::match(const char *s) const
{
	switch (::regexec(this, s, 0, NULL, 0)) {
	case 0:
		return true;
	case REG_NOMATCH:
		return false;
	default:
		MacOSError::throwMe(errSecCSResourceRulesInvalid);
	}
}


std::string ResourceBuilder::escapeRE(const std::string &s)
{
	string r;
	for (string::const_iterator it = s.begin(); it != s.end(); ++it) {
		char c = *it;
		if (strchr("\\[]{}().+*", c))
			r.push_back('\\');
		r.push_back(c);
	}
	return r;
}


//
// Resource Seals
//
ResourceSeal::ResourceSeal(CFTypeRef it)
{
	if (it == NULL)
		MacOSError::throwMe(errSecCSResourcesInvalid);
	if (CFGetTypeID(it) == CFDataGetTypeID()) {
		mHash = CFDataRef(it);
		mOptional = false;
	} else {
		mOptional = false;
		if (!cfscan(it, "{hash=%XO,?optional=%B}", &mHash, &mOptional))
			MacOSError::throwMe(errSecCSResourcesInvalid);
	}
}


} // end namespace CodeSigning
} // end namespace Security
