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
   File:      cssmkrapi.h

   Contains:  Application Programmers Interface for Key Recovery Modules

   Copyright: (c) 1999-2000 Apple Computer, Inc., all rights reserved.
*/

#ifndef _CSSMKRAPI_H_
#define _CSSMKRAPI_H_  1

#include <Security/cssmtype.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32 CSSM_KRSP_HANDLE; /* Key Recovery Service Provider Handle */

typedef struct cssm_kr_name {
    uint8 Type; /* namespace type */
    uint8 Length; /* name string length */
    char *Name; /* name string */
} CSSM_KR_NAME;

typedef struct cssm_kr_profile {
    CSSM_KR_NAME UserName; /* name of the user */
    CSSM_CERTGROUP_PTR UserCertificate; /* public key certificate of the user */
    CSSM_CERTGROUP_PTR KRSCertChain; /* cert chain for the KRSP coordinator */
    uint8 LE_KRANum; /* number of KRA cert chains in the following list */
    CSSM_CERTGROUP_PTR LE_KRACertChainList; /* list of Law enforcement KRA certificate chains */
    uint8 ENT_KRANum; /* number of KRA cert chains in the following list */
    CSSM_CERTGROUP_PTR ENT_KRACertChainList; /* list of Enterprise KRA certificate chains */
    uint8 INDIV_KRANum; /* number of KRA cert chains in the following list */
    CSSM_CERTGROUP_PTR INDIV_KRACertChainList; /* list of Individual KRA certificate chains */
    CSSM_DATA_PTR INDIV_AuthenticationInfo; /* authentication information for individual key recovery */
    uint32 KRSPFlags; /* flag values to be interpreted by KRSP */
    CSSM_DATA_PTR KRSPExtensions; /* reserved for extensions specific to KRSPs */
} CSSM_KR_PROFILE, *CSSM_KR_PROFILE_PTR;

typedef struct cssm_kr_wrappedproductinfo {
    CSSM_VERSION StandardVersion;
    CSSM_STRING StandardDescription;
    CSSM_VERSION ProductVersion;
    CSSM_STRING ProductDescription;
    CSSM_STRING ProductVendor;
    uint32 ProductFlags;
} CSSM_KR_WRAPPEDPRODUCT_INFO, *CSSM_KR_WRAPPEDPRODUCT_INFO_PTR;

typedef struct cssm_krsubservice {
    uint32 SubServiceId;
    char *Description; /* Description of this sub service */
    CSSM_KR_WRAPPEDPRODUCT_INFO WrappedProduct;
} CSSM_KRSUBSERVICE, *CSSM_KRSUBSERVICE_PTR;

typedef uint32 CSSM_KR_POLICY_TYPE;
#define CSSM_KR_INDIV_POLICY			(0x00000001)
#define CSSM_KR_ENT_POLICY				(0x00000002)
#define CSSM_KR_LE_MAN_POLICY			(0x00000003)
#define CSSM_KR_LE_USE_POLICY			(0x00000004)

typedef uint32 CSSM_KR_POLICY_FLAGS;

#define CSSM_KR_INDIV					(0x00000001)
#define CSSM_KR_ENT						(0x00000002)
#define CSSM_KR_LE_MAN					(0x00000004)
#define CSSM_KR_LE_USE					(0x00000008)
#define CSSM_KR_LE						(CSSM_KR_LE_MAN | CSSM_KR_LE_USE)
#define CSSM_KR_OPTIMIZE				(0x00000010)
#define CSSM_KR_DROP_WORKFACTOR			(0x00000020)

typedef struct cssm_kr_policy_list_item {
    struct kr_policy_list_item *next;
    CSSM_ALGORITHMS AlgorithmId;
    CSSM_ENCRYPT_MODE Mode;
    uint32 MaxKeyLength;
    uint32 MaxRounds;
    uint8 WorkFactor;
    CSSM_KR_POLICY_FLAGS PolicyFlags;
    CSSM_CONTEXT_TYPE AlgClass;
} CSSM_KR_POLICY_LIST_ITEM, *CSSM_KR_POLICY_LIST_ITEM_PTR;

typedef struct cssm_kr_policy_info {
    CSSM_BOOL krbNotAllowed;
    uint32 numberOfEntries;
    CSSM_KR_POLICY_LIST_ITEM *policyEntry;
} CSSM_KR_POLICY_INFO, *CSSM_KR_POLICY_INFO_PTR;


/* Key Recovery Module Mangement Operations */

CSSM_RETURN CSSMAPI
CSSM_KR_SetEnterpriseRecoveryPolicy (const CSSM_DATA *RecoveryPolicyFileName,
                                     const CSSM_ACCESS_CREDENTIALS *OldPassPhrase,
                                     const CSSM_ACCESS_CREDENTIALS *NewPassPhrase);


/* Key Recovery Context Operations */

CSSM_RETURN CSSMAPI
CSSM_KR_CreateRecoveryRegistrationContext (CSSM_KRSP_HANDLE KRSPHandle,
                                           CSSM_CC_HANDLE *NewContext);

CSSM_RETURN CSSMAPI
CSSM_KR_CreateRecoveryEnablementContext (CSSM_KRSP_HANDLE KRSPHandle,
                                         const CSSM_KR_PROFILE *LocalProfile,
                                         const CSSM_KR_PROFILE *RemoteProfile,
                                         CSSM_CC_HANDLE *NewContext);

CSSM_RETURN CSSMAPI
CSSM_KR_CreateRecoveryRequestContext (CSSM_KRSP_HANDLE KRSPHandle,
                                      const CSSM_KR_PROFILE *LocalProfile,
                                      CSSM_CC_HANDLE *NewContext);

CSSM_RETURN CSSMAPI
CSSM_KR_GetPolicyInfo (CSSM_CC_HANDLE CCHandle,
                       CSSM_KR_POLICY_FLAGS *EncryptionProhibited,
                       uint32 *WorkFactor);


/* Key Recovery Registration Operations */

CSSM_RETURN CSSMAPI
CSSM_KR_RegistrationRequest (CSSM_CC_HANDLE RecoveryRegistrationContext,
                             const CSSM_DATA *KRInData,
                             const CSSM_ACCESS_CREDENTIALS *AccessCredentials,
                             CSSM_KR_POLICY_FLAGS KRFlags,
                             sint32 *EstimatedTime,
                             CSSM_HANDLE_PTR ReferenceHandle);

CSSM_RETURN CSSMAPI
CSSM_KR_RegistrationRetrieve (CSSM_KRSP_HANDLE KRSPHandle,
                              CSSM_HANDLE ReferenceHandle,
                              const CSSM_ACCESS_CREDENTIALS *AccessCredentials,
                              sint32 *EstimatedTime,
                              CSSM_KR_PROFILE_PTR KRProfile);


/* Key Recovery Enablement Operations */

CSSM_RETURN CSSMAPI
CSSM_KR_GenerateRecoveryFields (CSSM_CC_HANDLE KeyRecoveryContext,
                                CSSM_CC_HANDLE CCHandle,
                                const CSSM_DATA *KRSPOptions,
                                CSSM_KR_POLICY_FLAGS KRFlags,
                                CSSM_DATA_PTR KRFields,
                                CSSM_CC_HANDLE *NewCCHandle);

CSSM_RETURN CSSMAPI
CSSM_KR_ProcessRecoveryFields (CSSM_CC_HANDLE KeyRecoveryContext,
                               CSSM_CC_HANDLE CryptoContext,
                               const CSSM_DATA *KRSPOptions,
                               CSSM_KR_POLICY_FLAGS KRFlags,
                               const CSSM_DATA *KRFields,
                               CSSM_CC_HANDLE *NewCryptoContext);


/* Key Recovery Request Operations */

CSSM_RETURN CSSMAPI
CSSM_KR_RecoveryRequest (CSSM_CC_HANDLE RecoveryRequestContext,
                         const CSSM_DATA *KRInData,
                         const CSSM_ACCESS_CREDENTIALS *AccessCredentials,
                         sint32 *EstimatedTime,
                         CSSM_HANDLE_PTR ReferenceHandle);

CSSM_RETURN CSSMAPI
CSSM_KR_RecoveryRetrieve (CSSM_KRSP_HANDLE KRSPHandle,
                          CSSM_HANDLE ReferenceHandle,
                          const CSSM_ACCESS_CREDENTIALS *AccessCredentials,
                          sint32 *EstimatedTime,
                          CSSM_HANDLE_PTR CacheHandle,
                          uint32 *NumberOfRecoveredKeys);

CSSM_RETURN CSSMAPI
CSSM_KR_GetRecoveredObject (CSSM_KRSP_HANDLE KRSPHandle,
                            CSSM_HANDLE CacheHandle,
                            uint32 IndexInResults,
                            CSSM_CSP_HANDLE CSPHandle,
                            const CSSM_RESOURCE_CONTROL_CONTEXT *CredAndAclEntry,
                            uint32 Flags,
                            CSSM_KEY_PTR RecoveredKey,
                            CSSM_DATA_PTR OtherInfo);

CSSM_RETURN CSSMAPI
CSSM_KR_RecoveryRequestAbort (CSSM_KRSP_HANDLE KRSPHandle,
                              CSSM_HANDLE CacheHandle);

CSSM_RETURN CSSMAPI
CSSM_KR_QueryPolicyInfo (CSSM_KRSP_HANDLE KRSPHandle,
                         CSSM_ALGORITHMS AlgorithmID,
                         CSSM_ENCRYPT_MODE Mode,
                         CSSM_CONTEXT_TYPE Class,
                         CSSM_KR_POLICY_INFO_PTR *PolicyInfoData);


/* Extensibility Functions */

CSSM_RETURN CSSMAPI
CSSM_KR_PassThrough (CSSM_KRSP_HANDLE KRSPHandle,
                     CSSM_CC_HANDLE KeyRecoveryContext,
                     CSSM_CC_HANDLE CryptoContext,
                     uint32 PassThroughId,
                     const void *InputParams,
                     void **OutputParams);

#ifdef __cplusplus
}
#endif

#endif /* _CSSMKRAPI_H_ */
