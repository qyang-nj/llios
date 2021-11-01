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
// csdb - system-supported Code Signing related database interfaces
//
#include "csdatabase.h"
#include "detachedrep.h"

namespace Security {
namespace CodeSigning {

using namespace SQLite;


//
// The one and only SignatureDatabase object.
// It auto-adapts to readonly vs. writable use.
//
ModuleNexus<SignatureDatabase> signatureDatabase;
ModuleNexus<SignatureDatabaseWriter> signatureDatabaseWriter;


//
// Default path to the signature database.
//
const char SignatureDatabase::defaultPath[] = "/var/db/DetachedSignatures";


//
// Creation commands to initialize the system database.
//
const char schema[] = "\
	create table if not exists code ( \n\
		id integer primary key on conflict replace autoincrement not null, \n\
		global integer null references global (id), \n\
		identifier text not null, \n\
		architecture integer, \n\
		identification blob not null unique on conflict replace, \n\
		signature blob not null, \n\
		created text default current_timestamp \n\
	); \n\
	create index if not exists identifier_index on code (identifier); \n\
	create index if not exists architecture_index on code (architecture); \n\
	create index if not exists id_index on code (identification); \n\
	\n\
	create table if not exists global ( \n\
		id integer primary key on conflict replace autoincrement not null, \n\
		sign_location text not null, \n\
		signature blob null \n\
	); \n\
	create index if not exists location_index on global (sign_location); \n\
";



//
// Open the database (creating it if necessary and possible).
// Note that this isn't creating the schema; we do that on first write.
//
SignatureDatabase::SignatureDatabase(const char *path, int flags)
	: SQLite::Database(path, flags)
{
}

SignatureDatabase::~SignatureDatabase()
{ /* virtual */ }


//
// Consult the database to find code by identification blob.
// Return the signature and (optional) global data blobs.
//
FilterRep *SignatureDatabase::findCode(DiskRep *rep)
{
	if (CFRef<CFDataRef> identification = rep->identification())
		if (!this->empty()) {
			SQLite::Statement query(*this,
				"select code.signature, global.signature from code, global \
				 where code.identification = ?1 and code.global = global.id;");
			query.bind(1) = identification.get();
			if (query.nextRow())
				return new DetachedRep(query[0].data(), query[1].data(), rep, "system");
		}

	// no joy
	return NULL;
}


//
// Given a unified detached signature blob, store its data in the database.
// This writes exactly one Global record, plus one Code record per architecture
// (where non-architectural code is treated as single-architecture).
//
void SignatureDatabaseWriter::storeCode(const BlobCore *sig, const char *location)
{
	Transaction xa(*this, Transaction::exclusive);	// lock out everyone
	if (this->empty())
		this->execute(schema);					// initialize schema
	if (const EmbeddedSignatureBlob *esig = EmbeddedSignatureBlob::specific(sig)) {	// architecture-less
		int64 globid = insertGlobal(location, NULL);
		insertCode(globid, 0, esig);
		xa.commit();
		return;
	} else if (const DetachedSignatureBlob *dsblob = DetachedSignatureBlob::specific(sig)) {
		int64 globid = insertGlobal(location, dsblob->find(0));
		unsigned count = dsblob->count();
		for (unsigned n = 0; n < count; n++)
			if (uint32_t arch = dsblob->type(n))
				insertCode(globid, arch, EmbeddedSignatureBlob::specific(dsblob->blob(n)));
		xa.commit();
		return;
	}
	
	MacOSError::throwMe(errSecCSSignatureInvalid);

}

int64 SignatureDatabaseWriter::insertGlobal(const char *location, const BlobCore *blob)
{
	Statement insert(*this, "insert into global (sign_location, signature) values (?1, ?2);");
	insert.bind(1) = location;
	if (blob)
		insert.bind(2).blob(blob, blob->length(), true);
	insert();
	return lastInsert();
}

void SignatureDatabaseWriter::insertCode(int64 globid, int arch, const EmbeddedSignatureBlob *sig)
{
	// retrieve binary identifier (was added by signer)
	const BlobWrapper *ident = BlobWrapper::specific(sig->find(cdIdentificationSlot));
	assert(ident);
	
	// extract CodeDirectory to get some information from it
	const CodeDirectory *cd = CodeDirectory::specific(sig->find(cdCodeDirectorySlot));
	assert(cd);

	// write the record
	Statement insert(*this,
		"insert into code (global, identifier, architecture, identification, signature) values (?1, ?2, ?3, ?4, ?5);");
	insert.bind(1) = globid;
	insert.bind(2) = cd->identifier();
	if (arch)
		insert.bind(3) = arch;
	insert.bind(4).blob(ident->data(), ident->length(), true);
	insert.bind(5).blob(sig, sig->length(), true);
	insert();
}



} // end namespace CodeSigning
} // end namespace Security
