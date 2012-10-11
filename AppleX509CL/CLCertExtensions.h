/*
 * Copyright (c) 2000-2001 Apple Computer, Inc. All Rights Reserved.
 * 
 * The contents of this file constitute Original Code as defined in and are
 * subject to the Apple Public Source License Version 1.2 (the 'License').
 * You may not use this file except in compliance with the License. Please obtain
 * a copy of the License at http://www.apple.com/publicsource and read it before
 * using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS
 * OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for the
 * specific language governing rights and limitations under the License.
 */


/*
 * CLCertExtensions.h - extern declarations of get/set/free functions implemented in
 *                    CertExtensions,cpp and used only in CertFields.cpp.
 *
 * Created 9/8/2000 by Doug Mitchell. 
 * Copyright (c) 2000 by Apple Computer. 
 */

#ifndef	_CL_CERT_EXTENSIONS_H_
#define _CL_CERT_EXTENSIONS_H_

#include "DecodedCert.h"

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Functions to map OID --> {get,set,free}field
 */
typedef bool (getFieldFcn) (
	const DecodedCert 	&cert,
	unsigned			index,			// which occurrence (0 = first)
	uint32				&numFields,		// RETURNED
	CssmOwnedData		&fieldValue);	// RETURNED
typedef void (setFieldFcn) (
	DecodedCert			&cert,
	const CssmData		&fieldValue);
typedef void (freeFieldFcn) (
	CssmOwnedData		&fieldValue);

getFieldFcn getFieldKeyUsage, getFieldBasicConstraints, getFieldExtKeyUsage,
	getFieldSubjectKeyId, getFieldAuthorityKeyId, getFieldSubjAltName,
	getFieldCertPolicies, getFieldNetscapeCertType, getFieldUnknownExt;
setFieldFcn setFieldKeyUsage, setFieldBasicConstraints, setFieldExtKeyUsage,
	setFieldSubjectKeyId, setFieldAuthorityKeyId, setFieldSubjAltName,
	setFieldCertPolicies, setFieldNetscapeCertType, setFieldUnknownExt;
freeFieldFcn freeFieldSimpleExtension, freeFieldExtKeyUsage, freeFieldSubjectKeyId,
	freeFieldAuthorityKeyId, freeFieldSubjAltName, freeFieldCertPolicies, 
	freeFieldUnknownExt;
	
#ifdef	__cplusplus
}
#endif

#endif	/* _CERT_EXTENSIONS_H_*/
