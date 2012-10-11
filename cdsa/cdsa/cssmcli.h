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
   File:      cssmcli.h

   Contains:  Service Provider Interface for Certificate Library Modules

   Copyright: (c) 1999-2000 Apple Computer, Inc., all rights reserved.
*/

#ifndef _CSSMCLI_H_
#define _CSSMCLI_H_  1

#include <Security/cssmtype.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cssm_spi_cl_funcs {
    CSSM_RETURN (CSSMCLI *CertCreateTemplate)
        (CSSM_CL_HANDLE CLHandle,
         uint32 NumberOfFields,
         const CSSM_FIELD *CertFields,
         CSSM_DATA_PTR CertTemplate);
    CSSM_RETURN (CSSMCLI *CertGetAllTemplateFields)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *CertTemplate,
         uint32 *NumberOfFields,
         CSSM_FIELD_PTR *CertFields);
    CSSM_RETURN (CSSMCLI *CertSign)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DATA *CertTemplate,
         const CSSM_FIELD *SignScope,
         uint32 ScopeSize,
         CSSM_DATA_PTR SignedCert);
    CSSM_RETURN (CSSMCLI *CertVerify)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DATA *CertToBeVerified,
         const CSSM_DATA *SignerCert,
         const CSSM_FIELD *VerifyScope,
         uint32 ScopeSize);
    CSSM_RETURN (CSSMCLI *CertVerifyWithKey)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DATA *CertToBeVerified);
    CSSM_RETURN (CSSMCLI *CertGetFirstFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Cert,
         const CSSM_OID *CertField,
         CSSM_HANDLE_PTR ResultsHandle,
         uint32 *NumberOfMatchedFields,
         CSSM_DATA_PTR *Value);
    CSSM_RETURN (CSSMCLI *CertGetNextFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE ResultsHandle,
         CSSM_DATA_PTR *Value);
    CSSM_RETURN (CSSMCLI *CertAbortQuery)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE ResultsHandle);
    CSSM_RETURN (CSSMCLI *CertGetKeyInfo)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Cert,
         CSSM_KEY_PTR *Key);
    CSSM_RETURN (CSSMCLI *CertGetAllFields)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Cert,
         uint32 *NumberOfFields,
         CSSM_FIELD_PTR *CertFields);
	CSSM_RETURN (CSSMCLI *FreeFields)
		(CSSM_CL_HANDLE CLHandle,
		 uint32 NumberOfFields,
		 CSSM_FIELD_PTR *FieldArray);
    CSSM_RETURN (CSSMCLI *FreeFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_OID *CertOrCrlOid,
         CSSM_DATA_PTR Value);
    CSSM_RETURN (CSSMCLI *CertCache)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Cert,
         CSSM_HANDLE_PTR CertHandle);
    CSSM_RETURN (CSSMCLI *CertGetFirstCachedFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE CertHandle,
         const CSSM_OID *CertField,
         CSSM_HANDLE_PTR ResultsHandle,
         uint32 *NumberOfMatchedFields,
         CSSM_DATA_PTR *Value);
    CSSM_RETURN (CSSMCLI *CertGetNextCachedFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE ResultsHandle,
         CSSM_DATA_PTR *Value);
    CSSM_RETURN (CSSMCLI *CertAbortCache)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE CertHandle);
    CSSM_RETURN (CSSMCLI *CertGroupToSignedBundle)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_CERTGROUP *CertGroupToBundle,
         const CSSM_CERT_BUNDLE_HEADER *BundleInfo,
         CSSM_DATA_PTR SignedBundle);
    CSSM_RETURN (CSSMCLI *CertGroupFromVerifiedBundle)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_CERT_BUNDLE *CertBundle,
         const CSSM_DATA *SignerCert,
         CSSM_CERTGROUP_PTR *CertGroup);
    CSSM_RETURN (CSSMCLI *CertDescribeFormat)
        (CSSM_CL_HANDLE CLHandle,
         uint32 *NumberOfFields,
         CSSM_OID_PTR *OidList);
    CSSM_RETURN (CSSMCLI *CrlCreateTemplate)
        (CSSM_CL_HANDLE CLHandle,
         uint32 NumberOfFields,
         const CSSM_FIELD *CrlTemplate,
         CSSM_DATA_PTR NewCrl);
    CSSM_RETURN (CSSMCLI *CrlSetFields)
        (CSSM_CL_HANDLE CLHandle,
         uint32 NumberOfFields,
         const CSSM_FIELD *CrlTemplate,
         const CSSM_DATA *OldCrl,
         CSSM_DATA_PTR ModifiedCrl);
    CSSM_RETURN (CSSMCLI *CrlAddCert)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DATA *Cert,
         uint32 NumberOfFields,
         const CSSM_FIELD *CrlEntryFields,
         const CSSM_DATA *OldCrl,
         CSSM_DATA_PTR NewCrl);
    CSSM_RETURN (CSSMCLI *CrlRemoveCert)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Cert,
         const CSSM_DATA *OldCrl,
         CSSM_DATA_PTR NewCrl);
    CSSM_RETURN (CSSMCLI *CrlSign)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DATA *UnsignedCrl,
         const CSSM_FIELD *SignScope,
         uint32 ScopeSize,
         CSSM_DATA_PTR SignedCrl);
    CSSM_RETURN (CSSMCLI *CrlVerify)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DATA *CrlToBeVerified,
         const CSSM_DATA *SignerCert,
         const CSSM_FIELD *VerifyScope,
         uint32 ScopeSize);
    CSSM_RETURN (CSSMCLI *CrlVerifyWithKey)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DATA *CrlToBeVerified);
    CSSM_RETURN (CSSMCLI *IsCertInCrl)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Cert,
         const CSSM_DATA *Crl,
         CSSM_BOOL *CertFound);
    CSSM_RETURN (CSSMCLI *CrlGetFirstFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Crl,
         const CSSM_OID *CrlField,
         CSSM_HANDLE_PTR ResultsHandle,
         uint32 *NumberOfMatchedFields,
         CSSM_DATA_PTR *Value);
    CSSM_RETURN (CSSMCLI *CrlGetNextFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE ResultsHandle,
         CSSM_DATA_PTR *Value);
    CSSM_RETURN (CSSMCLI *CrlAbortQuery)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE ResultsHandle);
    CSSM_RETURN (CSSMCLI *CrlGetAllFields)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Crl,
         uint32 *NumberOfCrlFields,
         CSSM_FIELD_PTR *CrlFields);
    CSSM_RETURN (CSSMCLI *CrlCache)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Crl,
         CSSM_HANDLE_PTR CrlHandle);
    CSSM_RETURN (CSSMCLI *IsCertInCachedCrl)
        (CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *Cert,
         CSSM_HANDLE CrlHandle,
         CSSM_BOOL *CertFound,
         CSSM_DATA_PTR CrlRecordIndex);
    CSSM_RETURN (CSSMCLI *CrlGetFirstCachedFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE CrlHandle,
         const CSSM_DATA *CrlRecordIndex,
         const CSSM_OID *CrlField,
         CSSM_HANDLE_PTR ResultsHandle,
         uint32 *NumberOfMatchedFields,
         CSSM_DATA_PTR *Value);
    CSSM_RETURN (CSSMCLI *CrlGetNextCachedFieldValue)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE ResultsHandle,
         CSSM_DATA_PTR *Value);
    CSSM_RETURN (CSSMCLI *CrlGetAllCachedRecordFields)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE CrlHandle,
         const CSSM_DATA *CrlRecordIndex,
         uint32 *NumberOfFields,
         CSSM_FIELD_PTR *CrlFields);
    CSSM_RETURN (CSSMCLI *CrlAbortCache)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_HANDLE CrlHandle);
    CSSM_RETURN (CSSMCLI *CrlDescribeFormat)
        (CSSM_CL_HANDLE CLHandle,
         uint32 *NumberOfFields,
         CSSM_OID_PTR *OidList);
    CSSM_RETURN (CSSMCLI *PassThrough)
        (CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         uint32 PassThroughId,
         const void *InputParams,
         void **OutputParams);
} CSSM_SPI_CL_FUNCS, *CSSM_SPI_CL_FUNCS_PTR;

#ifdef __cplusplus
}
#endif

#endif /* _CSSMCLI_H_ */
