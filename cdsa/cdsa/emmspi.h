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
   File:      emmspi.h

   Contains:  Service Provider Interface for Elective Module Managers

   Copyright: (c) 1999-2000 Apple Computer, Inc., all rights reserved.
*/

#ifndef _EMMSPI_H_
#define _EMMSPI_H_  1

#include <Security/cssmtype.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cssm_state_funcs {
    CSSM_RETURN (CSSMAPI *cssm_GetAttachFunctions)
        (CSSM_MODULE_HANDLE hAddIn,
         CSSM_SERVICE_MASK AddinType,
         void **SPFunctions,
         CSSM_GUID_PTR Guid,
	 CSSM_BOOL *Serialized);
    CSSM_RETURN (CSSMAPI *cssm_ReleaseAttachFunctions)
        (CSSM_MODULE_HANDLE hAddIn);
    CSSM_RETURN (CSSMAPI *cssm_GetAppMemoryFunctions)
        (CSSM_MODULE_HANDLE hAddIn,
         CSSM_UPCALLS_PTR UpcallTable);
    CSSM_RETURN (CSSMAPI *cssm_IsFuncCallValid)
        (CSSM_MODULE_HANDLE hAddin,
         CSSM_PROC_ADDR SrcAddress,
         CSSM_PROC_ADDR DestAddress,
         CSSM_PRIVILEGE InPriv,
         CSSM_PRIVILEGE *OutPriv,
         CSSM_BITMASK Hints,
         CSSM_BOOL *IsOK);
    CSSM_RETURN (CSSMAPI *cssm_DeregisterManagerServices)
        (const CSSM_GUID *GUID);
    CSSM_RETURN (CSSMAPI *cssm_DeliverModuleManagerEvent)
        (const CSSM_MANAGER_EVENT_NOTIFICATION *EventDescription);
} CSSM_STATE_FUNCS, *CSSM_STATE_FUNCS_PTR;

typedef struct cssm_manager_registration_info {
    /* loading, unloading, dispatch table, and event notification */
    CSSM_RETURN (CSSMAPI *Initialize)
        (uint32 VerMajor,
         uint32 VerMinor);
    CSSM_RETURN (CSSMAPI *Terminate) (void);
    CSSM_RETURN (CSSMAPI *RegisterDispatchTable)
        (CSSM_STATE_FUNCS_PTR CssmStateCallTable);
    CSSM_RETURN (CSSMAPI *DeregisterDispatchTable) (void);
    CSSM_RETURN (CSSMAPI *EventNotifyManager)
        (const CSSM_MANAGER_EVENT_NOTIFICATION *EventDescription);
    CSSM_RETURN (CSSMAPI *RefreshFunctionTable)
        (CSSM_FUNC_NAME_ADDR_PTR FuncNameAddrPtr,
         uint32 NumOfFuncNameAddr);
} CSSM_MANAGER_REGISTRATION_INFO, *CSSM_MANAGER_REGISTRATION_INFO_PTR;

enum {
	CSSM_HINT_NONE =			0,
	CSSM_HINT_ADDRESS_APP = 	1 << 0,
	CSSM_HINT_ADDRESS_SP =		1 << 1
};

CSSM_RETURN CSSMAPI
ModuleManagerAuthenticate (CSSM_KEY_HIERARCHY KeyHierarchy,
                           const CSSM_GUID *CssmGuid,
                           const CSSM_GUID *AppGuid,
                           CSSM_MANAGER_REGISTRATION_INFO_PTR FunctionTable);

#ifdef __cplusplus
}
#endif

#endif /* _EMMSPI_H_ */
