--
-- Copyright (c) 2011-2012 Apple Inc. All Rights Reserved.
-- 
-- @APPLE_LICENSE_HEADER_START@
-- 
-- This file contains Original Code and/or Modifications of Original Code
-- as defined in and that are subject to the Apple Public Source License
-- Version 2.0 (the 'License'). You may not use this file except in
-- compliance with the License. Please obtain a copy of the License at
-- http://www.opensource.apple.com/apsl/ and read it before using this
-- file.
--
-- The Original Code and all software distributed under the License are
-- distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
-- EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
-- INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
-- FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
-- Please see the License for the specific language governing rights and
-- limitations under the License.
-- 
-- @APPLE_LICENSE_HEADER_END@
--
--
-- System Policy master database - file format and initial contents
--
-- This is currently for sqlite3
--
-- NOTES:
-- Dates are uniformly in julian form. We use 5000000 as the canonical "never" expiration
-- value; that's a day in the year 8977.
--
PRAGMA user_version = 1;
PRAGMA foreign_keys = true;
PRAGMA legacy_file_format = false;
PRAGMA recursive_triggers = true;


--
-- The feature table hold configuration features and options
--
CREATE TABLE feature (
	id INTEGER PRIMARY KEY,				-- canononical
	name TEXT NOT NULL UNIQUE,			-- name of option
	value TEXT NULL,					-- value of option, if any
	remarks TEXT NULL					-- optional remarks string
);


--
-- The primary authority. This table is conceptually scanned
-- in priority order, with the highest-priority matching enabled record
-- determining the outcome.
-- 
CREATE TABLE authority (
	id INTEGER PRIMARY KEY AUTOINCREMENT,				-- canonical
	version INTEGER NOT NULL DEFAULT (1)				-- semantic version of this rule
		CHECK (version > 0),
	type INTEGER NOT NULL,								-- operation type
	requirement TEXT NULL								-- code requirement
		CHECK ((requirement IS NULL) = ((flags & 1) != 0)),
	allow INTEGER NOT NULL DEFAULT (1)					-- allow (1) or deny (0)
		CHECK (allow = 0 OR allow = 1),
	disabled INTEGER NOT NULL DEFAULT (0)				-- disable count (stacks; enabled if zero)
		CHECK (disabled >= 0),
	expires FLOAT NOT NULL DEFAULT (5000000),			-- expiration of rule authority (Julian date)
	priority REAL NOT NULL DEFAULT (0),					-- rule priority (full float)
	label TEXT NULL,									-- text label for authority rule
	flags INTEGER NOT NULL DEFAULT (0),					-- amalgamated binary flags
	-- following fields are for documentation only
	ctime FLOAT NOT NULL DEFAULT (JULIANDAY('now')),	-- rule creation time (Julian)
	mtime FLOAT NOT NULL DEFAULT (JULIANDAY('now')),	-- time rule was last changed (Julian)
	user TEXT NULL,										-- user requesting this rule (NULL if unknown)
	remarks TEXT NULL									-- optional remarks string
);

-- index
CREATE INDEX authority_type ON authority (type);
CREATE INDEX authority_priority ON authority (priority);
CREATE INDEX authority_expires ON authority (expires);

-- update mtime if a record is changed
CREATE TRIGGER authority_update AFTER UPDATE ON authority
BEGIN
	UPDATE authority SET mtime = JULIANDAY('now') WHERE id = old.id;
END;

-- rules that are actively considered
CREATE VIEW active_authority AS
SELECT * from authority
WHERE disabled = 0 AND JULIANDAY('now') < expires AND (flags & 1) = 0;

-- rules subject to priority scan: active_authority but including disabled rules
CREATE VIEW scan_authority AS
SELECT * from authority
WHERE JULIANDAY('now') < expires AND (flags & 1) = 0;


--
-- A table to carry (potentially large-ish) filesystem data stored as a bookmark blob.
--
CREATE TABLE bookmarkhints (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	bookmark BLOB NOT NULL,
	authority INTEGER NOT NULL
		REFERENCES authority(id) ON DELETE CASCADE
);


--
-- Upgradable features already contained in this baseline.
-- See policydatabase.cpp for upgrade code.
--
INSERT INTO feature (name, value, remarks)
	VALUES ('bookmarkhints', 'value', 'builtin');
INSERT INTO feature (name, value, remarks)
	VALUES ('codesignedpackages', 'value', 'builtin');


--
-- Initial canonical contents of a fresh database
--

-- virtual rule anchoring negative cache entries (no rule found)
insert into authority (type, allow, priority, flags, label)
	values (1, 0, -1.0E100, 1, 'No Matching Rule');

-- any Apple-signed installers except Developer ID
insert into authority (type, allow, priority, flags, label, requirement)
	values (2, 1, -1, 2, 'Apple Installer', 'anchor apple generic and ! certificate 1[field.1.2.840.113635.100.6.2.6]');

-- Apple code signing
insert into authority (type, allow, flags, label, requirement)
	values (1, 1, 2, 'Apple System', 'anchor apple');

-- Mac App Store signing
insert into authority (type, allow, flags, label, requirement)
	values (1, 1, 2, 'Mac App Store', 'anchor apple generic and certificate leaf[field.1.2.840.113635.100.6.1.9] exists');

-- Caspian code and archive signing
insert into authority (type, allow, flags, label, requirement)
	values (1, 1, 2, 'Developer ID', 'anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists');
insert into authority (type, allow, flags, label, requirement)
	values (2, 1, 2, 'Developer ID', 'anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and (certificate leaf[field.1.2.840.113635.100.6.1.14] or certificate leaf[field.1.2.840.113635.100.6.1.13])');


--
-- The cache table lists previously determined outcomes
-- for individual objects (by object hash). Entries come from
-- full evaluations of authority records, or by explicitly inserting
-- override rules that preempt the normal authority.
-- EACH object record must have a parent authority record from which it is derived;
-- this may be a normal authority rule or an override rule. If the parent rule is deleted,
-- all objects created from it are automatically removed (by sqlite itself).
--
CREATE TABLE object (
	id INTEGER PRIMARY KEY,								-- canonical
	type INTEGER NOT NULL,									-- operation type
	hash CDHASH NOT NULL,									-- canonical hash of object
	allow INTEGER NOT NULL,								-- allow (1) or deny (0)
	expires FLOAT NOT NULL DEFAULT (5000000),				-- expiration of object entry
	authority INTEGER NOT NULL								-- governing authority rule
		REFERENCES authority(id) ON DELETE CASCADE,
	-- following fields are for documentation only
	path TEXT NULL,											-- path of object at record creation time
	ctime FLOAT NOT NULL DEFAULT (JULIANDAY('now')),		-- record creation time
	mtime FLOAT NOT NULL DEFAULT (JULIANDAY('now')),		-- record modification time
	remarks TEXT NULL										-- optional remarks string
);

-- index
CREATE INDEX object_type ON object (type);
CREATE INDEX object_expires ON object (expires);
CREATE UNIQUE INDEX object_hash ON object (hash);

-- update mtime if a record is changed
CREATE TRIGGER object_update AFTER UPDATE ON object
BEGIN
	UPDATE object SET mtime = JULIANDAY('now') WHERE id = old.id;
END;


--
-- Some useful views on objects. These are for administration; they are not used by the assessor.
--
CREATE VIEW object_state AS
SELECT object.id, object.type, object.allow,
	CASE object.expires WHEN 5000000 THEN NULL ELSE STRFTIME('%Y-%m-%d %H:%M:%f', object.expires, 'localtime') END AS expiration,
	(object.expires - JULIANDAY('now')) * 86400 as remaining,
	authority.label,
	object.authority,
	object.path,
	object.ctime,
	authority.requirement,
	authority.disabled,
	object.remarks
FROM object, authority
WHERE object.authority = authority.id;
