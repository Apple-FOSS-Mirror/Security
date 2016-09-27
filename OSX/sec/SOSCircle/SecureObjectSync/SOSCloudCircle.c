/*
 * Copyright (c) 2012-2014 Apple Inc. All Rights Reserved.
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
//  SOSCloudCircle.m
//

#include <stdio.h>
#include <AssertMacros.h>
#include <Security/SecureObjectSync/SOSCloudCircle.h>
#include <Security/SecureObjectSync/SOSCloudCircleInternal.h>
#include <Security/SecureObjectSync/SOSCircle.h>
#include <Security/SecureObjectSync/SOSAccount.h>
#include <Security/SecureObjectSync/SOSAccountPriv.h>
#include <Security/SecureObjectSync/SOSFullPeerInfo.h>
#include <Security/SecureObjectSync/SOSPeerInfoCollections.h>
#include <Security/SecureObjectSync/SOSInternal.h>
#include <Security/SecureObjectSync/SOSRing.h>
#include <Security/SecureObjectSync/SOSBackupSliceKeyBag.h>

#include <Security/SecKeyPriv.h>
#include <Security/SecFramework.h>
#include <CoreFoundation/CFXPCBridge.h>

#include <securityd/SecItemServer.h>

#include <utilities/SecDispatchRelease.h>
#include <utilities/SecCFRelease.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecXPCError.h>

#include <utilities/debugging.h>

#include <CoreFoundation/CoreFoundation.h>

#include <xpc/xpc.h>
#define MINIMIZE_INCLUDES MINIMIZE_INCLUDES
#include <ipc/securityd_client.h>
#include <securityd/spi.h>

#include "SOSRegressionUtilities.h"
#include <Security/SecuritydXPC.h>
#include <SOSPeerInfoDER.h>
#include <SOSCloudKeychainClient.h>

const char * kSOSCCCircleChangedNotification = "com.apple.security.secureobjectsync.circlechanged";
const char * kSOSCCViewMembershipChangedNotification = "com.apple.security.secureobjectsync.viewschanged";
const char * kSOSCCInitialSyncChangedNotification = "com.apple.security.secureobjectsync.initialsyncchanged";

#define do_if_registered(sdp, ...) if (gSecurityd && gSecurityd->sdp) { return gSecurityd->sdp(__VA_ARGS__); }

static bool xpc_dictionary_entry_is_type(xpc_object_t dictionary, const char *key, xpc_type_t type)
{
    xpc_object_t value = xpc_dictionary_get_value(dictionary, key);
    
    return value && (xpc_get_type(value) == type);
}

SOSCCStatus SOSCCThisDeviceIsInCircle(CFErrorRef *error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(SOSCCStatus, ^{
        SOSCCStatus result = kSOSCCError;
        
        do_if_registered(soscc_ThisDeviceIsInCircle, error);
        
        xpc_object_t message = securityd_create_message(kSecXPCOpDeviceInCircle, error);
        if (message) {
            xpc_object_t response = securityd_message_with_reply_sync(message, error);
            
            if (response && xpc_dictionary_entry_is_type(response, kSecXPCKeyResult, XPC_TYPE_INT64)) {
                result = (SOSCCStatus) xpc_dictionary_get_int64(response, kSecXPCKeyResult);
            } else {
                result = kSOSCCError;
            }
            
            if (result < 0) {
                if (response && securityd_message_no_error(response, error))
                {
                    char *desc = xpc_copy_description(response);
                    SecCFCreateErrorWithFormat(0, sSecXPCErrorDomain, NULL, error, NULL, CFSTR("Remote error occurred/no info: %s"), desc);
                    free((void *)desc);
                }
            }
            if(response)
                xpc_release(response);
            if(message)
                xpc_release(message);
        }
        
        
        return result;
    }, CFSTR("SOSCCStatus=%d"))
}

static bool cfstring_to_error_request(enum SecXPCOperation op, CFStringRef string, CFErrorRef* error)
{
    __block bool result = false;
   
    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t xString = _CFXPCCreateXPCObjectFromCFObject(string);
        bool success = false;
        if (xString){
            xpc_dictionary_set_value(message, kSecXPCKeyString, xString);
            success = true;
            xpc_release(xString);
        }
        return success;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    return result;
}

static SOSRingStatus cfstring_to_uint64_request(enum SecXPCOperation op, CFStringRef string, CFErrorRef* error)
{
    __block bool result = false;
    
    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t xString = _CFXPCCreateXPCObjectFromCFObject(string);
        bool success = false;
        if (xString){
            xpc_dictionary_set_value(message, kSecXPCKeyString, xString);
            success = true;
            xpc_release(xString);
        }
        return success;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_int64(response, kSecXPCKeyResult);
        return result;
    });
    return result;
}

static CFStringRef simple_cfstring_error_request(enum SecXPCOperation op, CFErrorRef* error)
{
    __block CFStringRef result = NULL;
    
    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        const char *c_string = xpc_dictionary_get_string(response, kSecXPCKeyResult);

        if (c_string) {
            result = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*)c_string, strlen(c_string), kCFStringEncodingUTF8, false);
        }
        
        return c_string != NULL;
    });
    return result;
}

static bool simple_bool_error_request(enum SecXPCOperation op, CFErrorRef* error)
{
    __block bool result = false;
    
    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    return result;
}

static CFArrayRef simple_array_error_request(enum SecXPCOperation op, CFErrorRef* error)
{
    __block CFArrayRef result = NULL;
    
    secdebug("sosops","enter - operation: %d", op);
    if (securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, CFErrorRef *error) {
        xpc_object_t temp_result = xpc_dictionary_get_value(response, kSecXPCKeyResult);
        result = _CFXPCCreateCFObjectFromXPCObject(temp_result);
        return result != NULL;
    })) {
        if (!isArray(result)) {
            SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected array, got: %@"), result);
            CFReleaseNull(result);
        }
    }
    return result;
}

static int simple_int_error_request(enum SecXPCOperation op, CFErrorRef* error)
{
    __block int result = 0;
    
    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        int64_t temp_result =  xpc_dictionary_get_int64(response, kSecXPCKeyResult);
        if ((temp_result >= INT32_MIN) && (temp_result <= INT32_MAX)) {
            result = (int)temp_result;
        }
        return result;
    });
    return result;
}

static SOSPeerInfoRef peer_info_error_request(enum SecXPCOperation op, CFErrorRef* error)
{
    SOSPeerInfoRef result = NULL;
    __block CFDataRef data = NULL;

    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, CFErrorRef *error) {
        xpc_object_t temp_result = xpc_dictionary_get_value(response, kSecXPCKeyResult);
        if (response && (NULL != temp_result)) {
            data = _CFXPCCreateCFObjectFromXPCObject(temp_result);
        }
        return data != NULL;
    });

    if (!isData(data)) {
        SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected CFData, got: %@"), result);
    }

    if (data) {
        result = SOSPeerInfoCreateFromData(kCFAllocatorDefault, error, data);
    }
    CFReleaseNull(data);
    return result;
}

static CFArrayRef array_of_info_error_request(enum SecXPCOperation op, CFErrorRef* error)
{
    __block CFArrayRef result = NULL;
    
    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, CFErrorRef *error) {
        xpc_object_t encoded_array = xpc_dictionary_get_value(response, kSecXPCKeyResult);
        if (response && (NULL != encoded_array)) {
            result = CreateArrayOfPeerInfoWithXPCObject(encoded_array,  error);
        }
        return result != NULL;
    });

    if (!isArray(result)) {
        SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected array, got: %@"), result);
        CFReleaseNull(result);
    }
    return result;
}

static SOSPeerInfoRef data_to_peer_info_error_request(enum SecXPCOperation op, CFDataRef secret, CFErrorRef* error)
{
    __block SOSPeerInfoRef result = false;
    __block CFDataRef data = NULL;

    secdebug("sosops", "enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t xsecretData = _CFXPCCreateXPCObjectFromCFObject(secret);
        bool success = false;
        if (xsecretData){
            xpc_dictionary_set_value(message, kSecXPCKeyNewPublicBackupKey, xsecretData);
            success = true;
            xpc_release(xsecretData);
        }
        return success;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        xpc_object_t temp_result = xpc_dictionary_get_value(response, kSecXPCKeyResult);
        if (response && (NULL != temp_result)) {
            data = _CFXPCCreateCFObjectFromXPCObject(temp_result);
        }
        return result != NULL;
    });

    if (!isData(data)) {
        SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected CFData, got: %@"), result);
    }

    if (data) {
        result = SOSPeerInfoCreateFromData(kCFAllocatorDefault, error, data);
    }
    CFReleaseNull(data);
    return result;
}

static bool keybag_and_bool_to_bool_error_request(enum SecXPCOperation op, CFDataRef data, bool include, CFErrorRef* error)
{
    secdebug("sosops", "enter - operation: %d", op);
    return securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t xData = _CFXPCCreateXPCObjectFromCFObject(data);
        bool success = false;
        if (xData){
            xpc_dictionary_set_value(message, kSecXPCKeyKeybag, xData);
            xpc_release(xData);
            success = true;
        }
        xpc_dictionary_set_bool(message, kSecXPCKeyIncludeV0, include);
        return success;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        return xpc_dictionary_get_bool(response, kSecXPCKeyResult);
    });
}


static bool info_array_to_bool_error_request(enum SecXPCOperation op, CFArrayRef peer_infos, CFErrorRef* error)
{
    __block bool result = false;
    
    secdebug("sosops", "enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t encoded_peers = CreateXPCObjectWithArrayOfPeerInfo(peer_infos, error);
        if (encoded_peers)
            xpc_dictionary_set_value(message, kSecXPCKeyPeerInfos, encoded_peers);
        return encoded_peers != NULL;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    return result;
}

static bool uint64_t_to_bool_error_request(enum SecXPCOperation op,
                                           uint64_t number,
                                           CFErrorRef* error)
{
    __block bool result = false;
    
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        xpc_dictionary_set_uint64(message, kSecXPCLimitInMinutes, number);
        return true;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    
    return result;
}

static bool set_hsa2_autoaccept_error_request(enum SecXPCOperation op, CFDataRef pubKey, CFErrorRef *error)
{
	__block bool result = false;

	sec_trace_enter_api(NULL);
	securityd_send_sync_and_do(op, error, ^(xpc_object_t message,
			CFErrorRef *error) {
		xpc_object_t xpubkey = _CFXPCCreateXPCObjectFromCFObject(pubKey);
        bool success = false;
		if (xpubkey) {
			xpc_dictionary_set_value(message,
					kSecXPCKeyHSA2AutoAcceptInfo, xpubkey);
            success = true;
            xpc_release(xpubkey);
		}

		return success;
	}, ^(xpc_object_t response, __unused CFErrorRef *error) {
		result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
		return (bool)true;
	});

	return result;
}

bool SOSCCRequestToJoinCircle(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RequestToJoinCircle, error);
        
        return simple_bool_error_request(kSecXPCOpRequestToJoin, error);
    }, NULL)
}

bool SOSCCRequestToJoinCircleAfterRestore(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RequestToJoinCircleAfterRestore, error);

        return simple_bool_error_request(kSecXPCOpRequestToJoinAfterRestore, error);
    }, NULL)
}


bool SOSCCWaitForInitialSync(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_WaitForInitialSync, error);

        return simple_bool_error_request(kSecXPCOpWaitForInitialSync, error);
    }, NULL)
}

CFArrayRef SOSCCCopyYetToSyncViewsList(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyYetToSyncViewsList, error);

        return simple_array_error_request(kSecXPCOpCopyYetToSyncViews, error);
    }, NULL)
}

bool SOSCCRequestEnsureFreshParameters(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RequestEnsureFreshParameters, error);
        
        return simple_bool_error_request(kSecXPCOpRequestEnsureFreshParameters, error);
    }, NULL)
}

CFStringRef SOSCCGetAllTheRings(CFErrorRef *error){
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFStringRef, ^{
        do_if_registered(soscc_GetAllTheRings, error);
        
        
        return simple_cfstring_error_request(kSecXPCOpGetAllTheRings, error);
    }, NULL)
}
bool SOSCCApplyToARing(CFStringRef ringName, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_ApplyToARing, ringName, error);
        
        return cfstring_to_error_request(kSecXPCOpApplyToARing, ringName, error);
    }, NULL)
}

bool SOSCCWithdrawlFromARing(CFStringRef ringName, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_WithdrawlFromARing, ringName, error);
        
        return cfstring_to_error_request(kSecXPCOpWithdrawlFromARing, ringName, error);
    }, NULL)
}

SOSRingStatus SOSCCRingStatus(CFStringRef ringName, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(SOSRingStatus, ^{
        do_if_registered(soscc_RingStatus, ringName, error);
        
        return cfstring_to_uint64_request(kSecXPCOpRingStatus, ringName, error);
    }, CFSTR("SOSCCStatus=%d"))
}

bool SOSCCEnableRing(CFStringRef ringName, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_EnableRing, ringName, error);
        
        return cfstring_to_error_request(kSecXPCOpEnableRing, ringName, error);
    }, NULL)
}

bool SOSCCAccountSetToNew(CFErrorRef *error)
{
	secwarning("SOSCCAccountSetToNew called");
	sec_trace_enter_api(NULL);
	sec_trace_return_bool_api(^{
		do_if_registered(soscc_SetToNew, error);
		return simple_bool_error_request(kSecXPCOpAccountSetToNew, error);
	}, NULL)
}

bool SOSCCResetToOffering(CFErrorRef* error)
{
    secwarning("SOSCCResetToOffering called");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_ResetToOffering, error);
        
        return simple_bool_error_request(kSecXPCOpResetToOffering, error);
    }, NULL)
}

bool SOSCCResetToEmpty(CFErrorRef* error)
{
    secwarning("SOSCCResetToEmpty called");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_ResetToEmpty, error);
        
        return simple_bool_error_request(kSecXPCOpResetToEmpty, error);
    }, NULL)
}

bool SOSCCRemoveThisDeviceFromCircle(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RemoveThisDeviceFromCircle, error);
        
        return simple_bool_error_request(kSecXPCOpRemoveThisDeviceFromCircle, error);
    }, NULL)
}

bool SOSCCLoggedOutOfAccount(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_LoggedOutOfAccount, error);
        
        return simple_bool_error_request(kSecXPCOpLoggedOutOfAccount, error);
    }, NULL)
}

bool SOSCCBailFromCircle_BestEffort(uint64_t limit_in_seconds, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_BailFromCircle, limit_in_seconds, error);
        
        return uint64_t_to_bool_error_request(kSecXPCOpBailFromCircle, limit_in_seconds, error);
    }, NULL)
}

bool SOSCCSignedOut(bool immediate, CFErrorRef* error)
{
    uint64_t limit = strtoul(optarg, NULL, 10);
    
    if(immediate)
        return SOSCCRemoveThisDeviceFromCircle(error);
    else
        return SOSCCBailFromCircle_BestEffort(limit, error);
    
}

CFArrayRef SOSCCCopyPeerPeerInfo(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyPeerInfo, error);
        
        return array_of_info_error_request(kSecXPCOpCopyPeerPeerInfo, error);
    }, CFSTR("return=%@"));
}

bool SOSCCSetAutoAcceptInfo(CFDataRef autoaccept, CFErrorRef *error)
{
	sec_trace_return_bool_api(^{
		do_if_registered(soscc_SetHSA2AutoAcceptInfo, autoaccept, error);

		return set_hsa2_autoaccept_error_request(kSecXPCOpSetHSA2AutoAcceptInfo, autoaccept, error);
	}, NULL)
}

CFArrayRef SOSCCCopyConcurringPeerPeerInfo(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyConcurringPeerInfo, error);
        
        return array_of_info_error_request(kSecXPCOpCopyConcurringPeerPeerInfo, error);
    }, CFSTR("return=%@"));
}

CFArrayRef SOSCCCopyGenerationPeerInfo(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyGenerationPeerInfo, error);
        
        return simple_array_error_request(kSecXPCOpCopyGenerationPeerInfo, error);
    }, CFSTR("return=%@"));
}

CFArrayRef SOSCCCopyApplicantPeerInfo(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyApplicantPeerInfo, error);
        
        return array_of_info_error_request(kSecXPCOpCopyApplicantPeerInfo, error);
    }, CFSTR("return=%@"));
}

bool SOSCCValidateUserPublic(CFErrorRef* error){
    sec_trace_enter_api(NULL);
    sec_trace_return_api(bool, ^{
        do_if_registered(soscc_ValidateUserPublic, error);
        
        return simple_bool_error_request(kSecXPCOpValidateUserPublic, error);
    }, NULL);
}

CFArrayRef SOSCCCopyValidPeerPeerInfo(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyValidPeerPeerInfo, error);
        
        return array_of_info_error_request(kSecXPCOpCopyValidPeerPeerInfo, error);
    }, CFSTR("return=%@"));
}

CFArrayRef SOSCCCopyNotValidPeerPeerInfo(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyNotValidPeerPeerInfo, error);
        
        return array_of_info_error_request(kSecXPCOpCopyNotValidPeerPeerInfo, error);
    }, CFSTR("return=%@"));
}

CFArrayRef SOSCCCopyRetirementPeerInfo(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyRetirementPeerInfo, error);
        
        return array_of_info_error_request(kSecXPCOpCopyRetirementPeerInfo, error);
    }, CFSTR("return=%@"));
}

SOSPeerInfoRef SOSCCCopyMyPeerInfo(CFErrorRef *error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(SOSPeerInfoRef, ^{
        do_if_registered(soscc_CopyMyPeerInfo, error);

        return peer_info_error_request(kSecXPCOpCopyMyPeerInfo, error);
    }, CFSTR("return=%@"));
}

CFArrayRef SOSCCCopyEngineState(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyEngineState, error);

        return simple_array_error_request(kSecXPCOpCopyEngineState, error);
    }, CFSTR("return=%@"));
}

bool SOSCCAcceptApplicants(CFArrayRef applicants, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_AcceptApplicants, applicants, error);

        return info_array_to_bool_error_request(kSecXPCOpAcceptApplicants, applicants, error);
    }, NULL)
}

bool SOSCCRejectApplicants(CFArrayRef applicants, CFErrorRef *error)
{
    sec_trace_enter_api(CFSTR("applicants=%@"), applicants);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RejectApplicants, applicants, error);
        
        return info_array_to_bool_error_request(kSecXPCOpRejectApplicants, applicants, error);
    }, NULL)
}

static SOSPeerInfoRef SOSSetNewPublicBackupKey(CFDataRef pubKey, CFErrorRef *error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(SOSPeerInfoRef, ^{
        do_if_registered(soscc_SetNewPublicBackupKey, pubKey, error);

        return data_to_peer_info_error_request(kSecXPCOpSetNewPublicBackupKey, pubKey, error);
    }, CFSTR("return=%@"));
}

SOSPeerInfoRef SOSCCCopyMyPeerWithNewDeviceRecoverySecret(CFDataRef secret, CFErrorRef *error){
    CFDataRef publicKeyData = SOSCopyDeviceBackupPublicKey(secret, error);

    return SOSSetNewPublicBackupKey(publicKeyData, error);
}

bool SOSCCRegisterSingleRecoverySecret(CFDataRef aks_bag, bool includeV0Backups, CFErrorRef *error){
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RegisterSingleRecoverySecret, aks_bag, includeV0Backups, error);
        return keybag_and_bool_to_bool_error_request(kSecXPCOpSetBagForAllSlices, aks_bag, includeV0Backups, error);
    }, NULL);
}


static bool label_and_password_to_bool_error_request(enum SecXPCOperation op,
                                                     CFStringRef user_label, CFDataRef user_password,
                                                     CFErrorRef* error)
{
    __block bool result = false;

    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        CFStringPerformWithCString(user_label, ^(const char *utf8Str) {
            xpc_dictionary_set_string(message, kSecXPCKeyUserLabel, utf8Str);
        });
        xpc_dictionary_set_data(message, kSecXPCKeyUserPassword, CFDataGetBytePtr(user_password), CFDataGetLength(user_password));
        return true;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });

    return result;
}

static bool label_and_password_and_dsid_to_bool_error_request(enum SecXPCOperation op,
                                                     CFStringRef user_label, CFDataRef user_password,
                                                     CFStringRef dsid, CFErrorRef* error)
{
    __block bool result = false;
    
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        CFStringPerformWithCString(user_label, ^(const char *utf8Str) {
            xpc_dictionary_set_string(message, kSecXPCKeyUserLabel, utf8Str);
        });
        CFStringPerformWithCString(dsid, ^(const char *utr8StrDSID) {
            xpc_dictionary_set_string(message, kSecXPCKeyDSID, utr8StrDSID);
        });
        xpc_dictionary_set_data(message, kSecXPCKeyUserPassword, CFDataGetBytePtr(user_password), CFDataGetLength(user_password));
        return true;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    
    return result;
}

static bool deviceid_to_bool_error_request(enum SecXPCOperation op,
                                                     CFStringRef IDS,
                                                     CFErrorRef* error)
{
    __block bool result = false;
    
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        CFStringPerformWithCString(IDS, ^(const char *utf8Str) {
            xpc_dictionary_set_string(message, kSecXPCKeyDeviceID, utf8Str);
        });
        return true;
    }, ^bool(xpc_object_t response, CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        if(result == false){
            xpc_object_t xpc_error = xpc_dictionary_get_value(response, kSecXPCKeyError);
            if (xpc_error && error) {
                *error = SecCreateCFErrorWithXPCObject(xpc_error);
            }
        }
        return result;
    });
    
    return result;
}

static int idsDict_to_bool_error_request(enum SecXPCOperation op,
                                           CFDictionaryRef IDS,
                                           CFErrorRef* error)
{
    __block int result = false;
    
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        SecXPCDictionarySetPListOptional(message, kSecXPCKeyIDSMessage, IDS, error);
        return true;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        int64_t temp_result =  xpc_dictionary_get_int64(response, kSecXPCKeyResult);
        if ((temp_result >= INT32_MIN) && (temp_result <= INT32_MAX)) {
            result = (int)temp_result;
        }
        return result;
    });
    
    return result;
}

static bool idscommand_to_bool_error_request(enum SecXPCOperation op,
                                           CFStringRef idsMessage,
                                           CFErrorRef* error)
{
    __block bool result = false;
    
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        CFStringPerformWithCString(idsMessage, ^(const char *utf8Str) {
            xpc_dictionary_set_string(message, kSecXPCKeySendIDSMessage, utf8Str);
        });
        return true;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    
    return result;
}

bool SOSCCRegisterUserCredentials(CFStringRef user_label, CFDataRef user_password, CFErrorRef* error)
{
    secnotice("sosops", "SOSCCRegisterUserCredentials - calling SOSCCSetUserCredentials!! %@\n", user_label);
    return SOSCCSetUserCredentials(user_label, user_password, error);
}

bool SOSCCSetUserCredentials(CFStringRef user_label, CFDataRef user_password, CFErrorRef* error)
{
    secnotice("sosops", "SOSCCSetUserCredentials!! %@\n", user_label);
	sec_trace_enter_api(CFSTR("user_label=%@"), user_label);
    sec_trace_return_bool_api(^{
		do_if_registered(soscc_SetUserCredentials, user_label, user_password, error);

    	return label_and_password_to_bool_error_request(kSecXPCOpSetUserCredentials, user_label, user_password, error);
    }, NULL)
}

bool SOSCCSetUserCredentialsAndDSID(CFStringRef user_label, CFDataRef user_password, CFStringRef dsid, CFErrorRef *error)
{
    secnotice("sosops", "SOSCCSetUserCredentialsAndDSID!! %@\n", user_label);
    sec_trace_enter_api(CFSTR("user_label=%@"), user_label);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_SetUserCredentialsAndDSID, user_label, user_password, dsid, error);
        if(dsid == NULL){
            return label_and_password_and_dsid_to_bool_error_request(kSecXPCOpSetUserCredentialsAndDSID, user_label, user_password, CFSTR(""), error);
        }
        else{
            return label_and_password_and_dsid_to_bool_error_request(kSecXPCOpSetUserCredentialsAndDSID, user_label, user_password, dsid, error);
        }
    }, NULL)
    
}
bool SOSCCSetDeviceID(CFStringRef IDS, CFErrorRef* error)
{
    secnotice("sosops", "SOSCCSetDeviceID!! %@\n", IDS);
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        CFErrorRef localError = NULL;
        do_if_registered(soscc_SetDeviceID, IDS, &localError);
        bool result = deviceid_to_bool_error_request(kSecXPCOpSetDeviceID, IDS, &localError);
        if(localError){
            *error = CFRetainSafe(localError);
        }
        return result;
    }, NULL)
}

bool SOSCCIDSServiceRegistrationTest(CFStringRef message, CFErrorRef *error)
{
    secnotice("sosops", "SOSCCSendIDSTestMessage!! %@\n", message);
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_CheckIDSRegistration, message, error);
        return idscommand_to_bool_error_request(kSecXPCOpSendIDSMessage, message, error);
    }, NULL)
}

bool SOSCCIDSPingTest(CFStringRef message, CFErrorRef *error)
{
    secnotice("sosops", "SOSCCSendIDSTestMessage!! %@\n", message);
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_PingTest, message, error);
        return idscommand_to_bool_error_request(kSecXPCOpPingTest, message, error);
    }, NULL)
}

bool SOSCCIDSDeviceIDIsAvailableTest(CFErrorRef *error)
{
    secnotice("sosops", "SOSCCIDSDeviceIDIsAvailableTest!!\n");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_GetIDSIDFromIDS, error);
        return simple_bool_error_request(kSecXPCOpIDSDeviceID, error);
    }, NULL)
}

HandleIDSMessageReason SOSCCHandleIDSMessage(CFDictionaryRef IDS, CFErrorRef* error)
{
    secnotice("sosops", "SOSCCHandleIDSMessage!! %@\n", IDS);
    sec_trace_enter_api(NULL);
    sec_trace_return_api(HandleIDSMessageReason, ^{
        do_if_registered(soscc_HandleIDSMessage, IDS, error);
        return (HandleIDSMessageReason) idsDict_to_bool_error_request(kSecXPCOpHandleIDSMessage, IDS, error);
    }, NULL)
}

bool SOSCCTryUserCredentials(CFStringRef user_label, CFDataRef user_password, CFErrorRef* error)
{
	sec_trace_enter_api(CFSTR("user_label=%@"), user_label);
    sec_trace_return_bool_api(^{
	    do_if_registered(soscc_TryUserCredentials, user_label, user_password, error);

    	return label_and_password_to_bool_error_request(kSecXPCOpTryUserCredentials, user_label, user_password, error);
    }, NULL)
}


bool SOSCCCanAuthenticate(CFErrorRef* error) {
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
	    do_if_registered(soscc_CanAuthenticate, error);

	    return simple_bool_error_request(kSecXPCOpCanAuthenticate, error);
    }, NULL)
}

bool SOSCCPurgeUserCredentials(CFErrorRef* error) {
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
	    do_if_registered(soscc_PurgeUserCredentials, error);

	    return simple_bool_error_request(kSecXPCOpPurgeUserCredentials, error);
    }, NULL)
}

enum DepartureReason SOSCCGetLastDepartureReason(CFErrorRef *error) {
    sec_trace_enter_api(NULL);
    sec_trace_return_api(enum DepartureReason, ^{
	    do_if_registered(soscc_GetLastDepartureReason, error);
        
	    return (enum DepartureReason) simple_int_error_request(kSecXPCOpGetLastDepartureReason, error);
    }, NULL)
}

bool SOSCCSetLastDepartureReason(enum DepartureReason reason, CFErrorRef *error) {
	sec_trace_enter_api(NULL);
	sec_trace_return_api(bool, ^{
		do_if_registered(soscc_SetLastDepartureReason, reason, error);
		return securityd_send_sync_and_do(kSecXPCOpSetLastDepartureReason, error,
			^bool(xpc_object_t message, CFErrorRef *error) {
				xpc_dictionary_set_int64(message, kSecXPCKeyReason, reason);
				return true;
			},
			^bool(xpc_object_t response, __unused CFErrorRef *error) {
				return xpc_dictionary_get_bool(response, kSecXPCKeyResult);
			}
		);
	}, NULL)
}

CFStringRef SOSCCCopyIncompatibilityInfo(CFErrorRef* error) {
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFStringRef, ^{
	    do_if_registered(soscc_CopyIncompatibilityInfo, error);
        
	    return simple_cfstring_error_request(kSecXPCOpCopyIncompatibilityInfo, error);
    }, NULL)
}

CFStringRef SOSCCRequestDeviceID(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFStringRef, ^{
        do_if_registered(soscc_RequestDeviceID, error);
        CFStringRef deviceID = simple_cfstring_error_request(kSecXPCOpRequestDeviceID, error);
        return deviceID;
    }, NULL)
}

bool SOSCCProcessEnsurePeerRegistration(CFErrorRef* error){
    secnotice("updates", "enter SOSCCProcessEnsurePeerRegistration");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_EnsurePeerRegistration, error);
        
        return simple_bool_error_request(soscc_EnsurePeerRegistration_id, error);
    }, NULL)
}


SyncWithAllPeersReason SOSCCProcessSyncWithAllPeers(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(SyncWithAllPeersReason, ^{
	    do_if_registered(soscc_ProcessSyncWithAllPeers, error);
        
	    return (SyncWithAllPeersReason) simple_int_error_request(kSecXPCOpProcessSyncWithAllPeers, error);
    }, NULL)
}

CFStringRef SOSCCGetStatusDescription(SOSCCStatus status)
{
    switch (status) {
        case kSOSCCInCircle:
            return CFSTR("InCircle");
        case kSOSCCNotInCircle:
            return CFSTR("NotInCircle");
        case kSOSCCRequestPending:
            return CFSTR("RequestPending");
        case kSOSCCCircleAbsent:
            return CFSTR("CircleAbsent");
        case kSOSCCError:
            return CFSTR("InternalError");
        default:
            return CFSTR("Unknown Status (%d)");
    };
}

#if 0
    kSOSCCGeneralViewError    = -1,
    kSOSCCViewMember          = 0,
    kSOSCCViewNotMember       = 1,
    kSOSCCViewNotQualified    = 2,
    kSOSCCNoSuchView          = 3,
#endif

static int64_t name_action_to_code_request(enum SecXPCOperation op, uint16_t error_result,
                                           CFStringRef name, uint64_t action, CFErrorRef *error) {
    __block int64_t result = error_result;

    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        CFStringPerformWithCString(name, ^(const char *utf8Str) {
            xpc_dictionary_set_string(message, kSecXPCKeyViewName, utf8Str);
        });
        xpc_dictionary_set_int64(message, kSecXPCKeyViewActionCode, action);
        return true;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        if (response && xpc_dictionary_entry_is_type(response, kSecXPCKeyResult, XPC_TYPE_INT64)) {
            result = xpc_dictionary_get_int64(response, kSecXPCKeyResult);
        }
        return result != error_result;
    });

    return result;
}

SOSViewResultCode SOSCCView(CFStringRef view, SOSViewActionCode actionCode, CFErrorRef *error) {
	sec_trace_enter_api(NULL);
    sec_trace_return_api(SOSViewResultCode, ^{
        do_if_registered(soscc_View, view, actionCode, error);

        return (SOSViewResultCode) name_action_to_code_request(kSecXPCOpView, kSOSCCGeneralViewError, view, actionCode, error);
    }, CFSTR("SOSViewResultCode=%d"))
}


bool SOSCCViewSet(CFSetRef enabledViews, CFSetRef disabledViews) {
    CFErrorRef *error = NULL;
    __block bool result = false;

    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_ViewSet, enabledViews, disabledViews);
        return securityd_send_sync_and_do(kSecXPCOpViewSet, error, ^bool(xpc_object_t message, CFErrorRef *error) {
            xpc_object_t enabledSetXpc = CreateXPCObjectWithCFSetRef(enabledViews, error);
            xpc_object_t disabledSetXpc = CreateXPCObjectWithCFSetRef(disabledViews, error);
            if (enabledSetXpc) xpc_dictionary_set_value(message, kSecXPCKeyEnabledViewsKey, enabledSetXpc);
            if (disabledSetXpc) xpc_dictionary_set_value(message, kSecXPCKeyDisabledViewsKey, disabledSetXpc);
            return (enabledSetXpc != NULL) || (disabledSetXpc != NULL) ;
        }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
            result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
            return result;
        });
    }, NULL)
}

SOSSecurityPropertyResultCode SOSCCSecurityProperty(CFStringRef property, SOSSecurityPropertyActionCode actionCode, CFErrorRef *error) {
    sec_trace_enter_api(NULL);
    sec_trace_return_api(SOSSecurityPropertyResultCode, ^{
        SOSSecurityPropertyResultCode result = kSOSCCGeneralSecurityPropertyError;
        do_if_registered(soscc_SecurityProperty, property, actionCode, error);
        xpc_object_t message = securityd_create_message(kSecXPCOpSecurityProperty, error);
        if (message) {
            int64_t bigac = actionCode;
            xpc_dictionary_set_string(message, kSecXPCKeyViewName, CFStringGetCStringPtr(property, kCFStringEncodingUTF8));
            xpc_dictionary_set_int64(message, kSecXPCKeyViewActionCode, bigac);
            
            xpc_object_t response = securityd_message_with_reply_sync(message, error);
            
            if (response && xpc_dictionary_entry_is_type(response, kSecXPCKeyResult, XPC_TYPE_INT64)) {
                result = (SOSSecurityPropertyResultCode) xpc_dictionary_get_int64(response, kSecXPCKeyResult);
            }
            
            if (result == kSOSCCGeneralSecurityPropertyError) {
                if (response && securityd_message_no_error(response, error)) {
                    char *desc = xpc_copy_description(response);
                    SecCFCreateErrorWithFormat(0, sSecXPCErrorDomain, NULL, error, NULL, CFSTR("Remote error occurred/no info: %s"), desc);
                    free((void *)desc);
                }
            }
            if(response)
                xpc_release(response);
            if(message)
                xpc_release(message);
        }
        
        return result;
    }, CFSTR("SOSSecurityPropertyResultCode=%d"))
}


static bool sosIsViewSetSyncing(size_t n, CFStringRef *views) {
    __block bool retval = true;
    
    SOSCCStatus cstatus = SOSCCThisDeviceIsInCircle(NULL);
    if(cstatus == kSOSCCInCircle) {
        for(size_t i = 0; i < n; i++) {
            SOSViewResultCode vstatus = SOSCCView(views[i], kSOSCCViewQuery, NULL);
            if(vstatus != kSOSCCViewMember) retval = false;
        }
    } else {
        retval = false;
    }
    return retval;
}

bool SOSCCIsIcloudKeychainSyncing(void) {
    CFStringRef views[] = { kSOSViewKeychainV0 };
    return sosIsViewSetSyncing(1, views);
}

bool SOSCCIsSafariSyncing(void) {
    CFStringRef views[] = { kSOSViewAutofillPasswords, kSOSViewSafariCreditCards };
    return sosIsViewSetSyncing(2, views);
}

bool SOSCCIsAppleTVSyncing(void) {
    CFStringRef views[] = { kSOSViewAppleTV };
    return sosIsViewSetSyncing(1, views);
}

bool SOSCCIsHomeKitSyncing(void) {
    CFStringRef views[] = { kSOSViewHomeKit };
    return sosIsViewSetSyncing(1, views);
}

bool SOSCCIsWiFiSyncing(void) {
    CFStringRef views[] = { kSOSViewWiFi };
    return sosIsViewSetSyncing(1, views);
}
