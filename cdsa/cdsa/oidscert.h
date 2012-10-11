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
 File:      oidscert.h

 Contains:  Object Identifiers for X509 Certificate Library

 Copyright: (c) 1999-2000 Apple Computer, Inc., all rights reserved.
*/

#ifndef _OIDSCERT_H_
#define _OIDSCERT_H_  1

#include <Security/cssmconfig.h>
#include <Security/cssmtype.h>
#include <Security/oidsbase.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INTEL_X509V3_CERT_R08 INTEL_SEC_FORMATS, 1, 1
#define INTEL_X509V3_CERT_R08_LENGTH INTEL_SEC_FORMATS_LENGTH + 2

/* Prefix for defining Certificate Extension field OIDs */
#define INTEL_X509V3_CERT_PRIVATE_EXTENSIONS INTEL_X509V3_CERT_R08, 50
#define INTEL_X509V3_CERT_PRIVATE_EXTENSIONS_LENGTH INTEL_X509V3_CERT_R08_LENGTH + 1

/* Prefix for defining signature field OIDs */
#define INTEL_X509V3_SIGN_R08 INTEL_SEC_FORMATS, 3, 2
#define INTEL_X509V3_SIGN_R08_LENGTH INTEL_SEC_FORMATS_LENGTH + 2

/* Suffix specifying format or representation of a field value                                      */
/* Note that if a format suffix is not specified, a flat data representation is implied. */

#define INTEL_X509_C_DATATYPE					1
#define INTEL_X509_LDAPSTRING_DATATYPE		2

/* Certificate OIDS */
extern const CSSM_OID

	CSSMOID_X509V3SignedCertificate,
	CSSMOID_X509V3SignedCertificateCStruct,
	CSSMOID_X509V3Certificate,
	CSSMOID_X509V3CertificateCStruct,
	CSSMOID_X509V1Version,
	CSSMOID_X509V1SerialNumber,
	CSSMOID_X509V1IssuerName,
	CSSMOID_X509V1IssuerNameCStruct,
	CSSMOID_X509V1IssuerNameLDAP,
	CSSMOID_X509V1ValidityNotBefore,
	CSSMOID_X509V1ValidityNotAfter,
	CSSMOID_X509V1SubjectName,
	CSSMOID_X509V1SubjectNameCStruct,
	CSSMOID_X509V1SubjectNameLDAP,
	CSSMOID_CSSMKeyStruct,
	CSSMOID_X509V1SubjectPublicKeyCStruct,
	CSSMOID_X509V1SubjectPublicKeyAlgorithm,
	CSSMOID_X509V1SubjectPublicKeyAlgorithmParameters,
	CSSMOID_X509V1SubjectPublicKey,
	CSSMOID_X509V1CertificateIssuerUniqueId,
	CSSMOID_X509V1CertificateSubjectUniqueId,
	CSSMOID_X509V3CertificateExtensionsStruct,
	CSSMOID_X509V3CertificateExtensionsCStruct,
	CSSMOID_X509V3CertificateNumberOfExtensions,
	CSSMOID_X509V3CertificateExtensionStruct,
	CSSMOID_X509V3CertificateExtensionCStruct,
	CSSMOID_X509V3CertificateExtensionId,
	CSSMOID_X509V3CertificateExtensionCritical,
	CSSMOID_X509V3CertificateExtensionType,
	CSSMOID_X509V3CertificateExtensionValue,
	
	/* Signature OID Fields */
	CSSMOID_X509V1SignatureStruct,
	CSSMOID_X509V1SignatureCStruct,
	CSSMOID_X509V1SignatureAlgorithm,
	CSSMOID_X509V1SignatureAlgorithmTBS,
	CSSMOID_X509V1SignatureAlgorithmParameters,
	CSSMOID_X509V1Signature,
	
	/* Extension OID Fields */
	CSSMOID_SubjectSignatureBitmap,
	CSSMOID_SubjectPicture,
	CSSMOID_SubjectEmailAddress,
	CSSMOID_UseExemptions;

/*** 
 *** Apple addenda
 ***/
 
/* 
 * Standard Cert extensions.
 */
extern const CSSM_OID
	CSSMOID_SubjectDirectoryAttributes,
	CSSMOID_SubjectKeyIdentifier,
	CSSMOID_KeyUsage,
	CSSMOID_PrivateKeyUsagePeriod ,
	CSSMOID_SubjectAltName,
	CSSMOID_IssuerAltName,
	CSSMOID_BasicConstraints,
	CSSMOID_CrlNumber,
	CSSMOID_CrlReason,
	CSSMOID_HoldInstructionCode,
	CSSMOID_InvalidityDate,
	CSSMOID_DeltaCrlIndicator,
	CSSMOID_IssuingDistributionPoints,
	CSSMOID_NameConstraints,
	CSSMOID_CrlDistributionPoints,
	CSSMOID_CertificatePolicies,
	CSSMOID_PolicyMappings,
	CSSMOID_PolicyConstraints,
	CSSMOID_AuthorityKeyIdentifier,
	CSSMOID_ExtendedKeyUsage,
	CSSMOID_ExtendedUseCodeSigning;

/*
 * Netscape extensions.
 */
extern const CSSM_OID	CSSMOID_NetscapeCertType;

/*
 * Field values for CSSMOID_NetscapeCertType, a bit string.
 * Assumes a 16 bit field, even though currently only 8 bits
 * are defined. 
 */
#define CE_NCT_SSL_Client	0x8000
#define CE_NCT_SSL_Server	0x4000
#define CE_NCT_SMIME		0x2000
#define CE_NCT_ObjSign		0x1000
#define CE_NCT_Reserved		0x0800
#define CE_NCT_SSL_CA		0x0400
#define CE_NCT_SMIME_CA		0x0200
#define CE_NCT_ObjSignCA	0x0100

#ifdef __cplusplus
}
#endif

#endif /* _OIDSCERT_H_ */
