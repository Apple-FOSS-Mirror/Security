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
   File:      x509defs.h

   Contains:  Data structures for X509 Certificate Library field values

   Copyright: (c) 1999-2000 Apple Computer, Inc., all rights reserved.
*/

#ifndef _X509DEFS_H_
#define _X509DEFS_H_  1

#include <Security/cssmtype.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8 CSSM_BER_TAG;
#define BER_TAG_UNKNOWN 0
#define BER_TAG_BOOLEAN 1
#define BER_TAG_INTEGER 2
#define BER_TAG_BIT_STRING 3
#define BER_TAG_OCTET_STRING 4
#define BER_TAG_NULL 5
#define BER_TAG_OID 6
#define BER_TAG_OBJECT_DESCRIPTOR 7
#define BER_TAG_EXTERNAL 8
#define BER_TAG_REAL 9
#define BER_TAG_ENUMERATED 10
/* 12 to 15 are reserved for future versions of the recommendation */
#define BER_TAG_PKIX_UTF8_STRING 12
#define BER_TAG_SEQUENCE 16
#define BER_TAG_SET 17
#define BER_TAG_NUMERIC_STRING 18
#define BER_TAG_PRINTABLE_STRING 19
#define BER_TAG_T61_STRING 20
#define BER_TAG_TELETEX_STRING BER_TAG_T61_STRING
#define BER_TAG_VIDEOTEX_STRING 21
#define BER_TAG_IA5_STRING 22
#define BER_TAG_UTC_TIME 23
#define BER_TAG_GENERALIZED_TIME 24
#define BER_TAG_GRAPHIC_STRING 25
#define BER_TAG_ISO646_STRING 26
#define BER_TAG_GENERAL_STRING 27
#define BER_TAG_VISIBLE_STRING BER_TAG_ISO646_STRING
/* 28 - are reserved for future versions of the recommendation */
#define BER_TAG_PKIX_UNIVERSAL_STRING 28
#define BER_TAG_PKIX_BMP_STRING 30


/* Data Structures for X.509 Certificates */

typedef struct cssm_x509_algorithm_identifier {
    CSSM_OID algorithm;
    CSSM_DATA parameters;
} CSSM_X509_ALGORITHM_IDENTIFIER, *CSSM_X509_ALGORITHM_IDENTIFIER_PTR;

/* X509 Distinguished name structure */
typedef struct cssm_x509_type_value_pair {
    CSSM_OID type;
    CSSM_BER_TAG valueType; /* The Tag to be used when */
    /*this value is BER encoded */
    CSSM_DATA value;
} CSSM_X509_TYPE_VALUE_PAIR, *CSSM_X509_TYPE_VALUE_PAIR_PTR;

typedef struct cssm_x509_rdn {
    uint32 numberOfPairs;
    CSSM_X509_TYPE_VALUE_PAIR_PTR AttributeTypeAndValue;
} CSSM_X509_RDN, *CSSM_X509_RDN_PTR;

typedef struct cssm_x509_name {
    uint32 numberOfRDNs;
    CSSM_X509_RDN_PTR RelativeDistinguishedName;
} CSSM_X509_NAME, *CSSM_X509_NAME_PTR;

/* Public key info struct */
typedef struct cssm_x509_subject_public_key_info {
    CSSM_X509_ALGORITHM_IDENTIFIER algorithm;
    CSSM_DATA subjectPublicKey;
} CSSM_X509_SUBJECT_PUBLIC_KEY_INFO, *CSSM_X509_SUBJECT_PUBLIC_KEY_INFO_PTR;

typedef struct cssm_x509_time {
    CSSM_BER_TAG timeType;
    CSSM_DATA time;
} CSSM_X509_TIME, *CSSM_X509_TIME_PTR;

/* Validity struct */
typedef struct x509_validity {
    CSSM_X509_TIME notBefore;
    CSSM_X509_TIME notAfter;
} CSSM_X509_VALIDITY, *CSSM_X509_VALIDITY_PTR;

#define CSSM_X509_OPTION_PRESENT CSSM_TRUE
#define CSSM_X509_OPTION_NOT_PRESENT CSSM_FALSE
typedef CSSM_BOOL CSSM_X509_OPTION;

typedef struct cssm_x509ext_basicConstraints {
    CSSM_BOOL cA;
    CSSM_X509_OPTION pathLenConstraintPresent;
    uint32 pathLenConstraint;
} CSSM_X509EXT_BASICCONSTRAINTS, *CSSM_X509EXT_BASICCONSTRAINTS_PTR;

typedef enum extension_data_format {
    CSSM_X509_DATAFORMAT_ENCODED = 0,
    CSSM_X509_DATAFORMAT_PARSED,
    CSSM_X509_DATAFORMAT_PAIR,
} CSSM_X509EXT_DATA_FORMAT;

typedef struct cssm_x509_extensionTagAndValue {
    CSSM_BER_TAG type;
    CSSM_DATA value;
} CSSM_X509EXT_TAGandVALUE, *CSSM_X509EXT_TAGandVALUE_PTR;

typedef struct cssm_x509ext_pair {
    CSSM_X509EXT_TAGandVALUE tagAndValue;
    void *parsedValue;
} CSSM_X509EXT_PAIR, *CSSM_X509EXT_PAIR_PTR;

/* Extension structure */
typedef struct cssm_x509_extension {
    CSSM_OID extnId;
    CSSM_BOOL critical;
    CSSM_X509EXT_DATA_FORMAT format;
    union cssm_x509ext_value {
        CSSM_X509EXT_TAGandVALUE *tagAndValue;
        void *parsedValue;
        CSSM_X509EXT_PAIR *valuePair;
    } value;
    CSSM_DATA BERvalue;
} CSSM_X509_EXTENSION, *CSSM_X509_EXTENSION_PTR;

typedef struct cssm_x509_extensions {
    uint32 numberOfExtensions;
    CSSM_X509_EXTENSION_PTR extensions;
} CSSM_X509_EXTENSIONS, *CSSM_X509_EXTENSIONS_PTR;

/* X509V3 certificate structure */
typedef struct cssm_x509_tbs_certificate {
    CSSM_DATA version;
    CSSM_DATA serialNumber;
    CSSM_X509_ALGORITHM_IDENTIFIER signature;
    CSSM_X509_NAME issuer;
    CSSM_X509_VALIDITY validity;
    CSSM_X509_NAME subject;
    CSSM_X509_SUBJECT_PUBLIC_KEY_INFO subjectPublicKeyInfo;
    CSSM_DATA issuerUniqueIdentifier;
    CSSM_DATA subjectUniqueIdentifier;
    CSSM_X509_EXTENSIONS extensions;
} CSSM_X509_TBS_CERTIFICATE, *CSSM_X509_TBS_CERTIFICATE_PTR;

/* Signature structure */
typedef struct cssm_x509_signature {
    CSSM_X509_ALGORITHM_IDENTIFIER algorithmIdentifier;
    CSSM_DATA encrypted;
} CSSM_X509_SIGNATURE, *CSSM_X509_SIGNATURE_PTR;

/* Signed certificate structure */
typedef struct cssm_x509_signed_certificate {
    CSSM_X509_TBS_CERTIFICATE certificate;
    CSSM_X509_SIGNATURE signature;
} CSSM_X509_SIGNED_CERTIFICATE, *CSSM_X509_SIGNED_CERTIFICATE_PTR;

typedef struct cssm_x509ext_policyQualifierInfo {
    CSSM_OID policyQualifierId;
    CSSM_DATA value;
} CSSM_X509EXT_POLICYQUALIFIERINFO, *CSSM_X509EXT_POLICYQUALIFIERINFO_PTR;

typedef struct cssm_x509ext_policyQualifiers {
    uint32 numberOfPolicyQualifiers;
    CSSM_X509EXT_POLICYQUALIFIERINFO *policyQualifier;
} CSSM_X509EXT_POLICYQUALIFIERS, *CSSM_X509EXT_POLICYQUALIFIERS_PTR;

typedef struct cssm_x509ext_policyInfo {
    CSSM_OID policyIdentifier;
    CSSM_X509EXT_POLICYQUALIFIERS policyQualifiers;
} CSSM_X509EXT_POLICYINFO, *CSSM_X509EXT_POLICYINFO_PTR;


/* Data Structures for X.509 Certificate Revocations Lists */

/* x509V2 entry in the CRL revokedCertificates sequence */
typedef struct cssm_x509_revoked_cert_entry {
    CSSM_DATA certificateSerialNumber;
    CSSM_X509_TIME revocationDate;
    CSSM_X509_EXTENSIONS extensions;
} CSSM_X509_REVOKED_CERT_ENTRY, *CSSM_X509_REVOKED_CERT_ENTRY_PTR;

typedef struct cssm_x509_revoked_cert_list {
    uint32 numberOfRevokedCertEntries;
    CSSM_X509_REVOKED_CERT_ENTRY_PTR revokedCertEntry;
} CSSM_X509_REVOKED_CERT_LIST, *CSSM_X509_REVOKED_CERT_LIST_PTR;

/* x509v2 Certificate Revocation List (CRL) (unsigned) structure */
typedef struct cssm_x509_tbs_certlist {
    CSSM_DATA version;
    CSSM_X509_ALGORITHM_IDENTIFIER signature;
    CSSM_X509_NAME issuer;
    CSSM_X509_TIME thisUpdate;
    CSSM_X509_TIME nextUpdate;
    CSSM_X509_REVOKED_CERT_LIST_PTR revokedCertificates;
    CSSM_X509_EXTENSIONS extensions;
} CSSM_X509_TBS_CERTLIST, *CSSM_X509_TBS_CERTLIST_PTR;

typedef struct cssm_x509_signed_crl {
    CSSM_X509_TBS_CERTLIST tbsCertList;
    CSSM_X509_SIGNATURE signature;
} CSSM_X509_SIGNED_CRL, *CSSM_X509_SIGNED_CRL_PTR;

#ifdef __cplusplus
}
#endif

#endif /* _X509DEFS_H_ */
