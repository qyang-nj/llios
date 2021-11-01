/*
 * Copyright (c) 2006 Apple Computer, Inc. All Rights Reserved.
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
// sigblob - signature (Super)Blob types
//
#include "sigblob.h"
#include "CSCommon.h"


namespace Security {
namespace CodeSigning {


CFDataRef EmbeddedSignatureBlob::component(CodeDirectory::SpecialSlot slot) const
{
	if (const BlobCore *blob = this->find(slot))
		if (CodeDirectory::slotAttributes(slot) & cdComponentIsBlob)
			return makeCFData(*blob);	// is a native Blob
		else if (const BlobWrapper *wrap = BlobWrapper::specific(blob))
			return makeCFData(*wrap);
		else
			MacOSError::throwMe(errSecCSSignatureInvalid);
	return NULL;
}


void EmbeddedSignatureBlob::Maker::component(CodeDirectory::SpecialSlot slot, CFDataRef data)
{
	if (CodeDirectory::slotAttributes(slot) & cdComponentIsBlob)
		add(slot, reinterpret_cast<const BlobCore *>(CFDataGetBytePtr(data))->clone());
	else
		add(slot, BlobWrapper::alloc(CFDataGetBytePtr(data), CFDataGetLength(data)));
}


CFDictionaryRef EntitlementBlob::entitlements() const
{
	return makeCFDictionaryFrom(this->at<const UInt8 *>(sizeof(EntitlementBlob)),
		this->length() - sizeof(EntitlementBlob));
}


} // end namespace CodeSigning
} // end namespace Security
