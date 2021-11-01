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
// csdb - system-supported Code Signing related database interfaces
//
#ifndef _H_CSDATABASE
#define _H_CSDATABASE

#include "diskrep.h"
#include "sigblob.h"
#include <Security/Security.h>
#include <security_utilities/globalizer.h>
#include <security_utilities/sqlite++.h>
#include <security_utilities/cfutilities.h>


namespace Security {
namespace CodeSigning {

namespace SQLite = SQLite3;


class SignatureDatabase : public SQLite::Database {
public:
	SignatureDatabase(const char *path = defaultPath,
		int flags = SQLITE_OPEN_READONLY);
	virtual ~SignatureDatabase();
	
	FilterRep *findCode(DiskRep *rep);

public:
	static const char defaultPath[];
};


class SignatureDatabaseWriter : public SignatureDatabase {
public:
	SignatureDatabaseWriter(const char *path = defaultPath,
		int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
		: SignatureDatabase(path, flags) { }

	void storeCode(const BlobCore *sig, const char *location);
	
private:
	SQLite::int64 insertGlobal(const char *location, const BlobCore *blob);
	void insertCode(SQLite::int64 globid, int arch, const EmbeddedSignatureBlob *sig);
};


extern ModuleNexus<SignatureDatabase> signatureDatabase;
extern ModuleNexus<SignatureDatabaseWriter> signatureDatabaseWriter;


} // end namespace CodeSigning
} // end namespace Security

#endif // !_H_CSDATABASE
