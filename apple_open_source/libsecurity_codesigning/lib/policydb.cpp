/*
 * Copyright (c) 2011-2012 Apple Inc. All Rights Reserved.
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
#include "cs.h"
#include "policydb.h"
#include "policyengine.h"
#include <Security/CodeSigning.h>
#include <security_utilities/cfutilities.h>
#include <security_utilities/cfmunge.h>
#include <security_utilities/blob.h>
#include <security_utilities/logging.h>
#include <security_utilities/simpleprefs.h>
#include <security_utilities/logging.h>
#include "csdatabase.h"

#include <dispatch/dispatch.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <notify.h>

namespace Security {
namespace CodeSigning {


using namespace SQLite;


//
// Determine the database path
//
static const char *dbPath()
{
	if (const char *s = getenv("SYSPOLICYDATABASE"))
		return s;
	return defaultDatabase;
}


//
// Help mapping API-ish CFString keys to more convenient internal enumerations
//
typedef struct {
	const CFStringRef &cstring;
	uint enumeration;
} StringMap;

static uint mapEnum(CFDictionaryRef context, CFStringRef attr, const StringMap *map, uint value = 0)
{
	if (context)
		if (CFTypeRef value = CFDictionaryGetValue(context, attr))
			for (const StringMap *mp = map; mp->cstring; ++mp)
				if (CFEqual(mp->cstring, value))
					return mp->enumeration;
	return value;
}

static const StringMap mapType[] = {
	{ kSecAssessmentOperationTypeExecute, kAuthorityExecute },
	{ kSecAssessmentOperationTypeInstall, kAuthorityInstall },
	{ kSecAssessmentOperationTypeOpenDocument, kAuthorityOpenDoc },
	{ NULL }
};

AuthorityType typeFor(CFDictionaryRef context, AuthorityType type /* = kAuthorityInvalid */)
{
	return mapEnum(context, kSecAssessmentContextKeyOperation, mapType, type);
}

CFStringRef typeNameFor(AuthorityType type)
{
	for (const StringMap *mp = mapType; mp->cstring; ++mp)
		if (type == mp->enumeration)
			return mp->cstring;
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("type %d"), type);
}


//
// Open the database
//
PolicyDatabase::PolicyDatabase(const char *path, int flags)
	: SQLite::Database(path ? path : dbPath(), flags),
	  mLastExplicitCheck(0)
{
	// sqlite3 doesn't do foreign key support by default, have to turn this on per connection
	SQLite::Statement foreign(*this, "PRAGMA foreign_keys = true");
	foreign.execute();
	
	// Try upgrade processing if we may be open for write.
	// Ignore any errors (we may have been downgraded to read-only)
	// and try again later.
	if (openFlags() & SQLITE_OPEN_READWRITE)
		try {
			upgradeDatabase();
			installExplicitSet(gkeAuthFile, gkeSigsFile);
		} catch(...) {
		}
}

PolicyDatabase::~PolicyDatabase()
{ /* virtual */ }


//
// Quick-check the cache for a match.
// Return true on a cache hit, false on failure to confirm a hit for any reason.
//
bool PolicyDatabase::checkCache(CFURLRef path, AuthorityType type, CFMutableDictionaryRef result)
{
	// we currently don't use the cache for anything but execution rules
	if (type != kAuthorityExecute)
		return false;
	
	CFRef<SecStaticCodeRef> code;
	MacOSError::check(SecStaticCodeCreateWithPath(path, kSecCSDefaultFlags, &code.aref()));
	if (SecStaticCodeCheckValidity(code, kSecCSBasicValidateOnly, NULL) != noErr)
		return false;	// quick pass - any error is a cache miss
	CFRef<CFDictionaryRef> info;
	MacOSError::check(SecCodeCopySigningInformation(code, kSecCSDefaultFlags, &info.aref()));
	CFDataRef cdHash = CFDataRef(CFDictionaryGetValue(info, kSecCodeInfoUnique));
	
	// check the cache table for a fast match
	SQLite::Statement cached(*this, "SELECT object.allow, authority.label, authority FROM object, authority"
		" WHERE object.authority = authority.id AND object.type = :type AND object.hash = :hash AND authority.disabled = 0"
		" AND JULIANDAY('now') < object.expires;");
	cached.bind(":type").integer(type);
	cached.bind(":hash") = cdHash;
	if (cached.nextRow()) {
		bool allow = int(cached[0]);
		const char *label = cached[1];
		SQLite::int64 auth = cached[2];
		SYSPOLICY_ASSESS_CACHE_HIT();

		// If its allowed, lets do a full validation unless if
		// we are overriding the assessement, since that force
		// the verdict to 'pass' at the end

		if (allow && !overrideAssessment())
		    MacOSError::check(SecStaticCodeCheckValidity(code, kSecCSDefaultFlags, NULL));

		cfadd(result, "{%O=%B}", kSecAssessmentAssessmentVerdict, allow);
		PolicyEngine::addAuthority(result, label, auth, kCFBooleanTrue);
		return true;
	}
	return false;
}


//
// Purge the object cache of all expired entries.
// These are meant to run within the caller's transaction.
//
void PolicyDatabase::purgeAuthority()
{
	SQLite::Statement cleaner(*this,
		"DELETE FROM authority WHERE expires <= JULIANDAY('now');");
	cleaner.execute();
}

void PolicyDatabase::purgeObjects()
{
	SQLite::Statement cleaner(*this,
		"DELETE FROM object WHERE expires <= JULIANDAY('now');");
	cleaner.execute();
}

void PolicyDatabase::purgeObjects(double priority)
{
	SQLite::Statement cleaner(*this,
		"DELETE FROM object WHERE expires <= JULIANDAY('now') OR (SELECT priority FROM authority WHERE id = object.authority) <= :priority;");
	cleaner.bind(":priority") = priority;
	cleaner.execute();
}

    
//
// Database migration
//
std::string PolicyDatabase::featureLevel(const char *name)
{
	SQLite::Statement feature(*this, "SELECT value FROM feature WHERE name=:name");
	feature.bind(":name") = name;
	if (feature.nextRow())
		return feature[0].string();
	else
		return "";		// new feature (no level)
}
    
void PolicyDatabase::addFeature(const char *name, const char *value, const char *remarks)
{
	SQLite::Statement feature(*this, "INSERT OR REPLACE INTO feature (name,value,remarks) VALUES(:name, :value, :remarks)");
	feature.bind(":name") = name;
	feature.bind(":value") = value;
	feature.bind(":remarks") = remarks;
	feature.execute();
}

void PolicyDatabase::simpleFeature(const char *feature, void (^perform)())
{
	if (!hasFeature(feature)) {
		SQLite::Transaction update(*this);
		addFeature(feature, "upgraded", "upgraded");
		perform();
		update.commit();
	}
}

void PolicyDatabase::simpleFeature(const char *feature, const char *sql)
{
	if (!hasFeature(feature)) {
		SQLite::Transaction update(*this);
		addFeature(feature, "upgraded", "upgraded");
		SQLite::Statement perform(*this, sql);
		perform.execute();
		update.commit();
	}
}


void PolicyDatabase::upgradeDatabase()
{
	simpleFeature("bookmarkhints",
		"CREATE TABLE bookmarkhints ("
			"  id INTEGER PRIMARY KEY AUTOINCREMENT, "
			"  bookmark BLOB,"
			"  authority INTEGER NOT NULL"
			"     REFERENCES authority(id) ON DELETE CASCADE"
			")");

	if (!hasFeature("codesignedpackages")) {
		SQLite::Transaction update(*this);
		addFeature("codesignedpackages", "upgraded", "upgraded");
		SQLite::Statement updates(*this,
                                 "UPDATE authority"
                                 " SET requirement = 'anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and "
                                 "(certificate leaf[field.1.2.840.113635.100.6.1.14] or certificate leaf[field.1.2.840.113635.100.6.1.13])'"
                                 " WHERE type = 2 and label = 'Developer ID' and flags & :flag");
		updates.bind(":flag") = kAuthorityFlagDefault;
		updates.execute();
		update.commit();
	}
}


//
// Install Gatekeeper override (GKE) data.
// The arguments are paths to the authority and signature files.
//
void PolicyDatabase::installExplicitSet(const char *authfile, const char *sigfile)
{
	// only try this every gkeCheckInterval seconds
	time_t now = time(NULL);
	if (mLastExplicitCheck + gkeCheckInterval > now)
		return;
	mLastExplicitCheck = now;

	try {
		if (CFRef<CFDataRef> authData = cfLoadFile(authfile)) {
			CFDictionary auth(CFRef<CFDictionaryRef>(makeCFDictionaryFrom(authData)), errSecCSDbCorrupt);
			CFDictionaryRef content = auth.get<CFDictionaryRef>(CFSTR("authority"));
			std::string authUUID = cfString(auth.get<CFStringRef>(CFSTR("uuid")));
			if (authUUID.empty()) {
				secdebug("gkupgrade", "no uuid in auth file; ignoring gke.auth");
				return;
			}
			std::string dbUUID;
			SQLite::Statement uuidQuery(*this, "SELECT value FROM feature WHERE name='gke'");
			if (uuidQuery.nextRow())
				dbUUID = (const char *)uuidQuery[0];
			if (dbUUID == authUUID) {
				secdebug("gkupgrade", "gke.auth already present, ignoring");
				return;
			}
			Syslog::notice("loading GKE %s (replacing %s)", authUUID.c_str(), dbUUID.empty() ? "nothing" : dbUUID.c_str());

			// first, load code signatures. This is pretty much idempotent
			if (sigfile)
				if (FILE *sigs = fopen(sigfile, "r")) {
					unsigned count = 0;
					while (const BlobCore *blob = BlobCore::readBlob(sigs)) {
						signatureDatabaseWriter().storeCode(blob, "<remote>");
						count++;
					}
					secdebug("gkupgrade", "%d detached signature(s) loaded from override data", count);
					fclose(sigs);
				}
			
			// start transaction (atomic from here on out)
			SQLite::Transaction loadAuth(*this, SQLite::Transaction::exclusive, "GKE_Upgrade");
			
			// purge prior authority data
			SQLite::Statement purge(*this, "DELETE FROM authority WHERE flags & :flag");
			purge.bind(":flag") = kAuthorityFlagWhitelist;
			purge();
			
			// load new data
			CFIndex count = CFDictionaryGetCount(content);
			CFStringRef keys[count];
			CFDictionaryRef values[count];
			CFDictionaryGetKeysAndValues(content, (const void **)keys, (const void **)values);
			
			SQLite::Statement insert(*this, "INSERT INTO authority (type, allow, requirement, label, flags, remarks)"
				" VALUES (:type, 1, :requirement, 'GKE', :flags, :path)");
			for (CFIndex n = 0; n < count; n++) {
				CFDictionary info(values[n], errSecCSDbCorrupt);
				insert.reset();
				insert.bind(":type") = cfString(info.get<CFStringRef>(CFSTR("type")));
				insert.bind(":path") = cfString(info.get<CFStringRef>(CFSTR("path")));
				insert.bind(":requirement") = "cdhash H\"" + cfString(info.get<CFStringRef>(CFSTR("cdhash"))) + "\"";
				insert.bind(":flags") = kAuthorityFlagWhitelist;
				insert();
			}
			
			// update version and commit
			addFeature("gke", authUUID.c_str(), "gke loaded");
			loadAuth.commit();
		}
	} catch (...) {
		secdebug("gkupgrade", "exception during GKE upgrade");
	}
}


//
// Check the override-enable master flag
//
#define SP_ENABLE_KEY CFSTR("enabled")
#define SP_ENABLED CFSTR("yes")
#define SP_DISABLED CFSTR("no")

bool overrideAssessment()
{
	static bool enabled = false;
	static dispatch_once_t once;
	static int token = -1;
	static int have_token = 0;
	static dispatch_queue_t queue;
	int check;

	if (have_token && notify_check(token, &check) == NOTIFY_STATUS_OK && !check)
		return !enabled;

	dispatch_once(&once, ^{
		if (notify_register_check(kNotifySecAssessmentMasterSwitch, &token) == NOTIFY_STATUS_OK)
			have_token = 1;
		queue = dispatch_queue_create("com.apple.SecAssessment.assessment", NULL);
             });

	dispatch_sync(queue, ^{
		/* upgrade configuration from emir, ignore all error since we might not be able to write to */
		if (::access(visibleSecurityFlagFile, F_OK) == 0) {
			try {
				setAssessment(true);
				::unlink(visibleSecurityFlagFile);
			} catch (...) {
			}
			enabled = true;
			return;
		}

		try {
			Dictionary * prefsDict = Dictionary::CreateDictionary(prefsFile);
			if (prefsDict == NULL)
				return;
			
			CFStringRef value = prefsDict->getStringValue(SP_ENABLE_KEY);
			if (value && CFStringCompare(value, SP_DISABLED, 0) == 0)
				enabled = false;
			else
				enabled = true;
			delete prefsDict;
		} catch(...) {
		}
	});

	return !enabled;
}

void setAssessment(bool masterSwitch)
{
	MutableDictionary *prefsDict = MutableDictionary::CreateMutableDictionary(prefsFile);
	if (prefsDict == NULL)
		prefsDict = new MutableDictionary::MutableDictionary();
	prefsDict->setValue(SP_ENABLE_KEY, masterSwitch ? SP_ENABLED : SP_DISABLED);
	prefsDict->writePlistToFile(prefsFile);
	delete prefsDict;

	/* make sure permissions is right */
	::chmod(prefsFile, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

	notify_post(kNotifySecAssessmentMasterSwitch);
}


} // end namespace CodeSigning
} // end namespace Security
