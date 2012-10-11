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
   File:      cssmtpi.h

   Contains:  Service Provider Interface for Trust Policy Modules

   Copyright: (c) 1999-2000 Apple Computer, Inc., all rights reserved.
*/

#ifndef _CSSMTPI_H_
#define _CSSMTPI_H_  1

#include <Security/cssmtype.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cssm_spi_tp_funcs {
    CSSM_RETURN (CSSMTPI *SubmitCredRequest)
        (CSSM_TP_HANDLE TPHandle,
         const CSSM_TP_AUTHORITY_ID *PreferredAuthority,
         CSSM_TP_AUTHORITY_REQUEST_TYPE RequestType,
         const CSSM_TP_REQUEST_SET *RequestInput,
         const CSSM_TP_CALLERAUTH_CONTEXT *CallerAuthContext,
         sint32 *EstimatedTime,
         CSSM_DATA_PTR ReferenceIdentifier);
    CSSM_RETURN (CSSMTPI *RetrieveCredResult)
        (CSSM_TP_HANDLE TPHandle,
         const CSSM_DATA *ReferenceIdentifier,
         const CSSM_TP_CALLERAUTH_CONTEXT *CallerAuthCredentials,
         sint32 *EstimatedTime,
         CSSM_BOOL *ConfirmationRequired,
         CSSM_TP_RESULT_SET_PTR *RetrieveOutput);
    CSSM_RETURN (CSSMTPI *ConfirmCredResult)
        (CSSM_TP_HANDLE TPHandle,
         const CSSM_DATA *ReferenceIdentifier,
         const CSSM_TP_CALLERAUTH_CONTEXT *CallerAuthCredentials,
         const CSSM_TP_CONFIRM_RESPONSE *Responses,
         const CSSM_TP_AUTHORITY_ID *PreferredAuthority);
    CSSM_RETURN (CSSMTPI *ReceiveConfirmation)
        (CSSM_TP_HANDLE TPHandle,
         const CSSM_DATA *ReferenceIdentifier,
         CSSM_TP_CONFIRM_RESPONSE_PTR *Responses,
         sint32 *ElapsedTime);
    CSSM_RETURN (CSSMTPI *CertReclaimKey)
        (CSSM_TP_HANDLE TPHandle,
         const CSSM_CERTGROUP *CertGroup,
         uint32 CertIndex,
         CSSM_LONG_HANDLE KeyCacheHandle,
         CSSM_CSP_HANDLE CSPHandle,
         const CSSM_RESOURCE_CONTROL_CONTEXT *CredAndAclEntry);
    CSSM_RETURN (CSSMTPI *CertReclaimAbort)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_LONG_HANDLE KeyCacheHandle);
    CSSM_RETURN (CSSMTPI *FormRequest)
        (CSSM_TP_HANDLE TPHandle,
         const CSSM_TP_AUTHORITY_ID *PreferredAuthority,
         CSSM_TP_FORM_TYPE FormType,
         CSSM_DATA_PTR BlankForm);
    CSSM_RETURN (CSSMTPI *FormSubmit)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_TP_FORM_TYPE FormType,
         const CSSM_DATA *Form,
         const CSSM_TP_AUTHORITY_ID *ClearanceAuthority,
         const CSSM_TP_AUTHORITY_ID *RepresentedAuthority,
         CSSM_ACCESS_CREDENTIALS_PTR Credentials);
    CSSM_RETURN (CSSMTPI *CertGroupVerify)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CSP_HANDLE CSPHandle,
         const CSSM_CERTGROUP *CertGroupToBeVerified,
         const CSSM_TP_VERIFY_CONTEXT *VerifyContext,
         CSSM_TP_VERIFY_CONTEXT_RESULT_PTR VerifyContextResult);
    CSSM_RETURN (CSSMTPI *CertCreateTemplate)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         uint32 NumberOfFields,
         const CSSM_FIELD *CertFields,
         CSSM_DATA_PTR CertTemplate);
    CSSM_RETURN (CSSMTPI *CertGetAllTemplateFields)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         const CSSM_DATA *CertTemplate,
         uint32 *NumberOfFields,
         CSSM_FIELD_PTR *CertFields);
    CSSM_RETURN (CSSMTPI *CertSign)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DATA *CertTemplateToBeSigned,
         const CSSM_CERTGROUP *SignerCertGroup,
         const CSSM_TP_VERIFY_CONTEXT *SignerVerifyContext,
         CSSM_TP_VERIFY_CONTEXT_RESULT_PTR SignerVerifyResult,
         CSSM_DATA_PTR SignedCert);
    CSSM_RETURN (CSSMTPI *CrlVerify)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CSP_HANDLE CSPHandle,
         const CSSM_ENCODED_CRL *CrlToBeVerified,
         const CSSM_CERTGROUP *SignerCertGroup,
         const CSSM_TP_VERIFY_CONTEXT *VerifyContext,
         CSSM_TP_VERIFY_CONTEXT_RESULT_PTR RevokerVerifyResult);
    CSSM_RETURN (CSSMTPI *CrlCreateTemplate)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         uint32 NumberOfFields,
         const CSSM_FIELD *CrlFields,
         CSSM_DATA_PTR NewCrlTemplate);
    CSSM_RETURN (CSSMTPI *CertRevoke)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CSP_HANDLE CSPHandle,
         const CSSM_DATA *OldCrlTemplate,
         const CSSM_CERTGROUP *CertGroupToBeRevoked,
         const CSSM_CERTGROUP *RevokerCertGroup,
         const CSSM_TP_VERIFY_CONTEXT *RevokerVerifyContext,
         CSSM_TP_VERIFY_CONTEXT_RESULT_PTR RevokerVerifyResult,
         CSSM_TP_CERTCHANGE_REASON Reason,
         CSSM_DATA_PTR NewCrlTemplate);
    CSSM_RETURN (CSSMTPI *CertRemoveFromCrlTemplate)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CSP_HANDLE CSPHandle,
         const CSSM_DATA *OldCrlTemplate,
         const CSSM_CERTGROUP *CertGroupToBeRemoved,
         const CSSM_CERTGROUP *RevokerCertGroup,
         const CSSM_TP_VERIFY_CONTEXT *RevokerVerifyContext,
         CSSM_TP_VERIFY_CONTEXT_RESULT_PTR RevokerVerifyResult,
         CSSM_DATA_PTR NewCrlTemplate);
    CSSM_RETURN (CSSMTPI *CrlSign)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_ENCODED_CRL *CrlToBeSigned,
         const CSSM_CERTGROUP *SignerCertGroup,
         const CSSM_TP_VERIFY_CONTEXT *SignerVerifyContext,
         CSSM_TP_VERIFY_CONTEXT_RESULT_PTR SignerVerifyResult,
         CSSM_DATA_PTR SignedCrl);
    CSSM_RETURN (CSSMTPI *ApplyCrlToDb)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CSP_HANDLE CSPHandle,
         const CSSM_ENCODED_CRL *CrlToBeApplied,
         const CSSM_CERTGROUP *SignerCertGroup,
         const CSSM_TP_VERIFY_CONTEXT *ApplyCrlVerifyContext,
         CSSM_TP_VERIFY_CONTEXT_RESULT_PTR ApplyCrlVerifyResult);
    CSSM_RETURN (CSSMTPI *CertGroupConstruct)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CSP_HANDLE CSPHandle,
         const CSSM_DL_DB_LIST *DBList,
         const void *ConstructParams,
         const CSSM_CERTGROUP *CertGroupFrag,
         CSSM_CERTGROUP_PTR *CertGroup);
    CSSM_RETURN (CSSMTPI *CertGroupPrune)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         const CSSM_DL_DB_LIST *DBList,
         const CSSM_CERTGROUP *OrderedCertGroup,
         CSSM_CERTGROUP_PTR *PrunedCertGroup);
    CSSM_RETURN (CSSMTPI *CertGroupToTupleGroup)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         const CSSM_CERTGROUP *CertGroup,
         CSSM_TUPLEGROUP_PTR *TupleGroup);
    CSSM_RETURN (CSSMTPI *TupleGroupToCertGroup)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         const CSSM_TUPLEGROUP *TupleGroup,
         CSSM_CERTGROUP_PTR *CertTemplates);
    CSSM_RETURN (CSSMTPI *PassThrough)
        (CSSM_TP_HANDLE TPHandle,
         CSSM_CL_HANDLE CLHandle,
         CSSM_CC_HANDLE CCHandle,
         const CSSM_DL_DB_LIST *DBList,
         uint32 PassThroughId,
         const void *InputParams,
         void **OutputParams);
} CSSM_SPI_TP_FUNCS, *CSSM_SPI_TP_FUNCS_PTR;

#ifdef __cplusplus
}
#endif

#endif /* _CSSMTPI_H_ */
