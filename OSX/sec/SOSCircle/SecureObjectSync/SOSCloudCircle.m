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

#import <Foundation/Foundation.h>
#import <Foundation/NSXPCConnection_Private.h>

#include <stdio.h>
#include <AssertMacros.h>
#include <Security/SecureObjectSync/SOSCloudCircle.h>
#include <Security/SecureObjectSync/SOSCloudCircleInternal.h>
#include <Security/SecureObjectSync/SOSCircle.h>
#include <Security/SecureObjectSync/SOSAccount.h>
#include <Security/SecureObjectSync/SOSFullPeerInfo.h>
#include <Security/SecureObjectSync/SOSPeerInfoCollections.h>
#include <Security/SecureObjectSync/SOSInternal.h>
#include <Security/SecureObjectSync/SOSRing.h>
#include <Security/SecureObjectSync/SOSBackupSliceKeyBag.h>
#include <Security/SecureObjectSync/SOSViews.h>
#include <Security/SecureObjectSync/SOSControlHelper.h>

#include <Security/SecKeyPriv.h>
#include <Security/SecFramework.h>
#include <CoreFoundation/CFXPCBridge.h>

#include <securityd/SecItemServer.h>

#include <utilities/SecDispatchRelease.h>
#include <utilities/SecCFRelease.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecXPCError.h>

#include <corecrypto/ccsha2.h>

#include <utilities/debugging.h>

#include <CoreFoundation/CoreFoundation.h>

#include <xpc/xpc.h>
#define MINIMIZE_INCLUDES MINIMIZE_INCLUDES
#include <ipc/securityd_client.h>
#include <securityd/spi.h>

#include <Security/SecuritydXPC.h>
#include "SOSPeerInfoDER.h"

const char * kSOSCCCircleChangedNotification = "com.apple.security.secureobjectsync.circlechanged";
const char * kSOSCCViewMembershipChangedNotification = "com.apple.security.secureobjectsync.viewschanged";
const char * kSOSCCInitialSyncChangedNotification = "com.apple.security.secureobjectsync.initialsyncchanged";
const char * kSOSCCHoldLockForInitialSync = "com.apple.security.secureobjectsync.holdlock";
const char * kSOSCCPeerAvailable = "com.apple.security.secureobjectsync.peeravailable";
const char * kSOSCCRecoveryKeyChanged = "com.apple.security.secureobjectsync.recoverykeychanged";
const char * kSOSCCCircleOctagonKeysChangedNotification = "com.apple.security.sosoctagonbitschanged";

#define do_if_registered(sdp, ...) if (gSecurityd && gSecurityd->sdp) { return gSecurityd->sdp(__VA_ARGS__); }

static bool xpc_dictionary_entry_is_type(xpc_object_t dictionary, const char *key, xpc_type_t type)
{
    xpc_object_t value = xpc_dictionary_get_value(dictionary, key);
    
    return value && (xpc_get_type(value) == type);
}

SOSCCStatus SOSCCThisDeviceIsInCircle(CFErrorRef *error)
{
    SOSCCStatus retval = SOSGetCachedCircleStatus(error);
    if(retval != kSOSNoCachedValue) {
        secnotice("circleOps", "Retrieved cached circle value %d", retval);
        return retval;
    }
    return SOSCCThisDeviceIsInCircleNonCached(error);
}


SOSCCStatus SOSCCThisDeviceIsInCircleNonCached(CFErrorRef *error)
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
        }
        secnotice("circleOps", "Retrieved non-cached circle value %d", result);

        return result;
    }, CFSTR("SOSCCStatus=%d"))
}

static bool sfsigninanalytics_bool_error_request(enum SecXPCOperation op, CFDataRef parentEvent, CFErrorRef* error)
{
    __block bool result = false;

    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetData(message, kSecXPCKeySignInAnalytics, parentEvent, error);
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    return result;
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
            xString = nil;
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
            xString = nil;
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

static bool SecXPCDictionarySetPeerInfoData(xpc_object_t message, const char *key, SOSPeerInfoRef peerInfo, CFErrorRef *error) {
    bool success = false;
    CFDataRef peerData = SOSPeerInfoCopyEncodedData(peerInfo, kCFAllocatorDefault, error);
    if (peerData) {
        success = SecXPCDictionarySetData(message, key, peerData, error);
    }
    CFReleaseNull(peerData);
    return success;
}

static bool peer_info_to_bool_error_request(enum SecXPCOperation op, SOSPeerInfoRef peerInfo, CFErrorRef* error)
{
    __block bool result = false;

    secdebug("sosops","enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetPeerInfoData(message, kSecXPCKeyPeerInfo, peerInfo, error);
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    return result;
}

static CFBooleanRef cfarray_to_cfboolean_error_request(enum SecXPCOperation op, CFArrayRef views, CFErrorRef* error)
{
    __block bool result = false;

    secdebug("sosops","enter - operation: %d", op);
    bool noError = securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetPList(message, kSecXPCKeyArray, views, error);
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return true;
    });
    return noError ? (result ? kCFBooleanTrue : kCFBooleanFalse) : NULL;
}


static CFSetRef cfset_cfset_to_cfset_error_request(enum SecXPCOperation op, CFSetRef set1, CFSetRef set2, CFErrorRef* error)
{
    __block CFSetRef result = NULL;

    secdebug("sosops","enter - operation: %d", op);
    bool noError = securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetPList(message, kSecXPCKeySet, set1, error) && SecXPCDictionarySetPList(message, kSecXPCKeySet2, set2, error);
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = SecXPCDictionaryCopySet(response, kSecXPCKeyResult, error);
        return result;
    });

    if (!noError) {
        CFReleaseNull(result);
    }
    return result;
}


static bool escrow_to_bool_error_request(enum SecXPCOperation op, CFStringRef escrow_label, uint64_t tries, CFErrorRef* error)
{
    __block bool result = false;
    
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
      
        bool success = false;
        xpc_object_t xEscrowLabel = _CFXPCCreateXPCObjectFromCFObject(escrow_label);
        if (xEscrowLabel){
            xpc_dictionary_set_value(message, kSecXPCKeyEscrowLabel, xEscrowLabel);
            success = true;
            xEscrowLabel = nil;
        }
        if(tries){
            xpc_dictionary_set_int64(message, kSecXPCKeyTriesLabel, tries);
            success = true;
        }
            
        return success;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    return result;
}

static CF_RETURNS_RETAINED CFArrayRef simple_array_error_request(enum SecXPCOperation op, CFErrorRef* error)
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

static CF_RETURNS_RETAINED CFArrayRef der_array_error_request(enum SecXPCOperation op, CFErrorRef* error)
{
    __block CFArrayRef result = NULL;

    secdebug("sosops","enter - operation: %d", op);
    if (securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, CFErrorRef *error) {
        size_t length = 0;
        const uint8_t* bytes = xpc_dictionary_get_data(response, kSecXPCKeyResult, &length);
        der_decode_plist(kCFAllocatorDefault, 0, (CFPropertyListRef*) &result, error, bytes, bytes + length);

        return result != NULL;
    })) {
        if (!isArray(result)) {
            SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected array, got: %@"), result);
            CFReleaseNull(result);
        }
    }
    return result;
}

static CFDictionaryRef strings_to_dictionary_error_request(enum SecXPCOperation op, CFErrorRef* error)
{
    __block CFDictionaryRef result = NULL;
    
    secdebug("sosops","enter - operation: %d", op);
    
    if (securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, CFErrorRef *error) {
        xpc_object_t temp_result = xpc_dictionary_get_value(response, kSecXPCKeyResult);
        if(temp_result)
            result = _CFXPCCreateCFObjectFromXPCObject(temp_result);
        return result != NULL;
    })){
        
        if (!isDictionary(result)) {
            SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected dictionary, got: %@"), result);
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

static CF_RETURNS_RETAINED SOSPeerInfoRef peer_info_error_request(enum SecXPCOperation op, CFErrorRef* error)
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

    if (isData(data)) {
        result = SOSPeerInfoCreateFromData(kCFAllocatorDefault, error, data);
    } else {
        SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected CFData, got: %@"), data);
    }

    CFReleaseNull(data);
    return result;
}

static CFDataRef data_to_error_request(enum SecXPCOperation op, CFErrorRef *error)
{
    __block CFDataRef result = NULL;

    secdebug("sosops", "enter -- operation: %d", op);
    securityd_send_sync_and_do(op, error, NULL, ^bool(xpc_object_t response, CFErrorRef *error) {
        xpc_object_t temp_result = xpc_dictionary_get_value(response, kSecXPCKeyResult);
        if (response && (NULL != temp_result)) {
            result = _CFXPCCreateCFObjectFromXPCObject(temp_result);
        }
        return result != NULL;
    });
    
    if (!isData(result)) {
        SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected CFData, got: %@"), result);
        return NULL;
    }
  
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

static CF_RETURNS_RETAINED SOSPeerInfoRef data_to_peer_info_error_request(enum SecXPCOperation op, CFDataRef secret, CFErrorRef* error)
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
            xsecretData = nil;
        }
        return success;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        xpc_object_t temp_result = xpc_dictionary_get_value(response, kSecXPCKeyResult);
        if (response && (NULL != temp_result)) {
            data = _CFXPCCreateCFObjectFromXPCObject(temp_result);
        }
        return data != NULL;
    });

    if (isData(data)) {
        result = SOSPeerInfoCreateFromData(kCFAllocatorDefault, error, data);
    } else {
        SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected CFData, got: %@"), data);
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
            xData = nil;
            success = true;
        }
        xpc_dictionary_set_bool(message, kSecXPCKeyIncludeV0, include);
        return success;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        return xpc_dictionary_get_bool(response, kSecXPCKeyResult);
    });
}

static bool recovery_and_bool_to_bool_error_request(enum SecXPCOperation op, CFDataRef data, CFErrorRef* error)
{
    secdebug("sosops", "enter - operation: %d", op);
    return securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t xData = NULL;
        if(data) {
            xData = _CFXPCCreateXPCObjectFromCFObject(data);
        } else {
            uint8_t zero = 0;
            CFDataRef nullData = CFDataCreate(kCFAllocatorDefault, &zero, 1);
            xData = _CFXPCCreateXPCObjectFromCFObject(nullData);
            CFReleaseNull(nullData);
        }
        bool success = false;
        if (xData){
            xpc_dictionary_set_value(message, kSecXPCKeyRecoveryPublicKey, xData);
            xData = nil;
            success = true;
        }
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
            xpc_dictionary_set_value(message, kSecXPCKeyPeerInfoArray, encoded_peers);
        return encoded_peers != NULL;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    return result;
}

static bool info_array_data_to_bool_error_request(enum SecXPCOperation op, CFArrayRef peer_infos, CFDataRef parentEvent, CFErrorRef* error)
{
    __block bool result = false;

    secdebug("sosops", "enter - operation: %d", op);
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        SecXPCDictionarySetData(message, kSecXPCKeySignInAnalytics, parentEvent, error);
        xpc_object_t encoded_peers = CreateXPCObjectWithArrayOfPeerInfo(peer_infos, error);
        if (encoded_peers)
            xpc_dictionary_set_value(message, kSecXPCKeyPeerInfoArray, encoded_peers);
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

static bool cfstring_and_cfdata_to_cfdata_cfdata_error_request(enum SecXPCOperation op, CFStringRef viewName, CFDataRef input, CFDataRef* data, CFDataRef* data2, CFErrorRef* error) {
    secdebug("sosops", "enter - operation: %d", op);
    __block bool result = false;
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t xviewname = _CFXPCCreateXPCObjectFromCFObject(viewName);
        xpc_object_t xinput = _CFXPCCreateXPCObjectFromCFObject(input);
        bool success = false;
        if (xviewname && xinput){
            xpc_dictionary_set_value(message, kSecXPCKeyViewName, xviewname);
            xpc_dictionary_set_value(message, kSecXPCData, xinput);
            success = true;
            xviewname = nil;
            xinput = nil;
        }
        return success;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);

        xpc_object_t temp_result = xpc_dictionary_get_value(response, kSecXPCData);
        if ((NULL != temp_result) && data) {
            *data = _CFXPCCreateCFObjectFromXPCObject(temp_result);
        }
        temp_result = xpc_dictionary_get_value(response, kSecXPCKeyKeybag);
        if ((NULL != temp_result) && data2) {
            *data2 = _CFXPCCreateCFObjectFromXPCObject(temp_result);
        }

        return result;
    });

    if (data &&!isData(*data)) {
        SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected CFData, got: %@"), *data);
    }
    if (data2 &&!isData(*data2)) {
        SOSErrorCreate(kSOSErrorUnexpectedType, error, NULL, CFSTR("Expected CFData, got: %@"), *data2);
    }

    return result;
}

static bool cfdata_and_int_error_request_returns_bool(enum SecXPCOperation op, CFDataRef thedata,
                                                      PiggyBackProtocolVersion version, CFErrorRef *error) {
    __block bool result = false;
    
    sec_trace_enter_api(NULL);
    securityd_send_sync_and_do(op, error, ^(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t xdata = _CFXPCCreateXPCObjectFromCFObject(thedata);
        bool success = false;
        if (xdata) {
            xpc_dictionary_set_value(message, kSecXPCData, xdata);
            xpc_dictionary_set_uint64(message, kSecXPCVersion, version);
            success = true;
            xdata = nil;
        }
        
        return success;
    }, ^(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return (bool)true;
    });
    
    return result;
}

static CFDataRef cfdata_error_request_returns_cfdata(enum SecXPCOperation op, CFDataRef thedata, CFErrorRef *error) {
    __block CFDataRef result = NULL;
    
    sec_trace_enter_api(NULL);
    securityd_send_sync_and_do(op, error, ^(xpc_object_t message, CFErrorRef *error) {
        xpc_object_t xdata = _CFXPCCreateXPCObjectFromCFObject(thedata);
        bool success = false;
        if (xdata) {
            xpc_dictionary_set_value(message, kSecXPCData, xdata);
            success = true;
            xdata = nil;
        }
        return success;
    }, ^(xpc_object_t response, __unused CFErrorRef *error) {
        xpc_object_t temp_result = xpc_dictionary_get_value(response, kSecXPCKeyResult);
        if (response && (NULL != temp_result)) {
            CFTypeRef object = _CFXPCCreateCFObjectFromXPCObject(temp_result);
            result = copyIfData(object, error);
            CFReleaseNull(object);
        }
        return (bool) (result != NULL);
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

bool SOSCCRequestToJoinCircleWithAnalytics(CFDataRef parentEvent, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RequestToJoinCircleWithAnalytics, parentEvent, error);

        return sfsigninanalytics_bool_error_request(kSecXPCOpRequestToJoinWithAnalytics, parentEvent, error);
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

bool SOSCCRequestToJoinCircleAfterRestoreWithAnalytics(CFDataRef parentEvent, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RequestToJoinCircleAfterRestoreWithAnalytics, parentEvent, error);

        return sfsigninanalytics_bool_error_request(kSecXPCOpRequestToJoinAfterRestoreWithAnalytics, parentEvent, error);
    }, NULL)
}

bool SOSCCAccountHasPublicKey(CFErrorRef *error)
{
    // murfxx
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_AccountHasPublicKey, error);
        
        return simple_bool_error_request(kSecXPCOpAccountHasPublicKey, error);
    }, NULL)
    
}

bool SOSCCAccountIsNew(CFErrorRef *error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_AccountIsNew, error);
        
        return simple_bool_error_request(kSecXPCOpAccountIsNew, error);
    }, NULL)
}

bool SOSCCWaitForInitialSyncWithAnalytics(CFDataRef parentEvent, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_WaitForInitialSyncWithAnalytics, parentEvent, error);

        return sfsigninanalytics_bool_error_request(kSecXPCOpWaitForInitialSyncWithAnalytics, parentEvent, error);
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

bool SOSCCResetToEmptyWithAnalytics(CFDataRef parentEvent, CFErrorRef* error)
{
    secwarning("SOSCCResetToEmptyWithAnalytics called");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_ResetToEmptyWithAnalytics, parentEvent, error);

        return sfsigninanalytics_bool_error_request(kSecXPCOpResetToEmptyWithAnalytics, parentEvent, error);
    }, NULL)
}
bool SOSCCRemovePeersFromCircle(CFArrayRef peers, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RemovePeersFromCircle, peers, error);

        return info_array_to_bool_error_request(kSecXPCOpRemovePeersFromCircle, peers, error);
    }, NULL)
}

bool SOSCCRemovePeersFromCircleWithAnalytics(CFArrayRef peers, CFDataRef parentEvent, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RemovePeersFromCircleWithAnalytics, peers, parentEvent, error);

        return info_array_data_to_bool_error_request(kSecXPCOpRemovePeersFromCircleWithAnalytics, peers, parentEvent, error);
    }, NULL)
}

bool SOSCCRemoveThisDeviceFromCircleWithAnalytics(CFDataRef parentEvent, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RemoveThisDeviceFromCircleWithAnalytics, parentEvent, error);

        return sfsigninanalytics_bool_error_request(kSecXPCOpRemoveThisDeviceFromCircleWithAnalytics, parentEvent, error);
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

CFArrayRef SOSCCCopyViewUnawarePeerInfo(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyViewUnawarePeerInfo, error);

        return array_of_info_error_request(kSecXPCOpCopyViewUnawarePeerInfo, error);
    }, CFSTR("return=%@"));
}

CFDataRef SOSCCCopyAccountState(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFDataRef, ^{
        do_if_registered(soscc_CopyAccountState, error);
        
        return data_to_error_request(kSecXPCOpCopyAccountData, error);
    }, CFSTR("return=%@"));
}

bool SOSCCDeleteAccountState(CFErrorRef *error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(bool, ^{
        do_if_registered(soscc_DeleteAccountState, error);
        return simple_bool_error_request(kSecXPCOpDeleteAccountData, error);
    }, NULL);
}
CFDataRef SOSCCCopyEngineData(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFDataRef, ^{
        do_if_registered(soscc_CopyEngineData, error);
        
        return data_to_error_request(kSecXPCOpCopyEngineData, error);
    }, CFSTR("return=%@"));
}

bool SOSCCDeleteEngineState(CFErrorRef *error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(bool, ^{
        do_if_registered(soscc_DeleteEngineState, error);
        return simple_bool_error_request(kSecXPCOpDeleteEngineData, error);
    }, NULL);
}

SOSPeerInfoRef SOSCCCopyMyPeerInfo(CFErrorRef *error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(SOSPeerInfoRef, ^{
        do_if_registered(soscc_CopyMyPeerInfo, error);

        return peer_info_error_request(kSecXPCOpCopyMyPeerInfo, error);
    }, CFSTR("return=%@"));
}

static CFArrayRef SOSCCCopyEngineState(CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFArrayRef, ^{
        do_if_registered(soscc_CopyEngineState, error);

        return der_array_error_request(kSecXPCOpCopyEngineState, error);
    }, CFSTR("return=%@"));
}

CFStringRef kSOSCCEngineStatePeerIDKey = CFSTR("PeerID");
CFStringRef kSOSCCEngineStateManifestCountKey = CFSTR("ManifestCount");
CFStringRef kSOSCCEngineStateSyncSetKey = CFSTR("SyncSet");
CFStringRef kSOSCCEngineStateCoderKey = CFSTR("CoderDump");
CFStringRef kSOSCCEngineStateManifestHashKey = CFSTR("ManifestHash");

void SOSCCForEachEngineStateAsStringFromArray(CFArrayRef states, void (^block)(CFStringRef oneStateString)) {

    CFArrayForEach(states, ^(const void *value) {
        CFDictionaryRef dict = asDictionary(value, NULL);
        if (dict) {
            CFMutableStringRef description = CFStringCreateMutable(kCFAllocatorDefault, 0);

            CFStringRef id = asString(CFDictionaryGetValue(dict, kSOSCCEngineStatePeerIDKey), NULL);

            if (id) {
                CFStringAppendFormat(description, NULL, CFSTR("remote %@ "), id);
            } else {
                CFStringAppendFormat(description, NULL, CFSTR("local "));
            }

            CFSetRef viewSet = asSet(CFDictionaryGetValue(dict, kSOSCCEngineStateSyncSetKey), NULL);
            if (viewSet) {
                CFStringSetPerformWithDescription(viewSet, ^(CFStringRef setDescription) {
                    CFStringAppend(description, setDescription);
                });
            } else {
                CFStringAppendFormat(description, NULL, CFSTR("<Missing view set!>"));
            }

            CFStringAppendFormat(description, NULL, CFSTR(" [%@]"),
                                 CFDictionaryGetValue(dict, kSOSCCEngineStateManifestCountKey));

            CFDataRef mainfestHash = asData(CFDictionaryGetValue(dict, kSOSCCEngineStateManifestHashKey), NULL);

            if (mainfestHash) {
                CFDataPerformWithHexString(mainfestHash, ^(CFStringRef dataString) {
                    CFStringAppendFormat(description, NULL, CFSTR(" %@"), dataString);
                });
            }

            CFStringRef coderDescription = asString(CFDictionaryGetValue(dict, kSOSCCEngineStateCoderKey), NULL);
            if (coderDescription) {
                CFStringAppendFormat(description, NULL, CFSTR(" %@"), coderDescription);
            }

            block(description);
            
            CFReleaseNull(description);
        }
    });
}

bool SOSCCForEachEngineStateAsString(CFErrorRef* error, void (^block)(CFStringRef oneStateString)) {
    CFArrayRef states = SOSCCCopyEngineState(error);
    if (states == NULL)
        return false;

    SOSCCForEachEngineStateAsStringFromArray(states, block);

    return true;
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

static CF_RETURNS_RETAINED SOSPeerInfoRef SOSSetNewPublicBackupKey(CFDataRef pubKey, CFErrorRef *error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(SOSPeerInfoRef, ^{
        do_if_registered(soscc_SetNewPublicBackupKey, pubKey, error);

        return data_to_peer_info_error_request(kSecXPCOpSetNewPublicBackupKey, pubKey, error);
    }, CFSTR("return=%@"));
}

SOSPeerInfoRef SOSCCCopyMyPeerWithNewDeviceRecoverySecret(CFDataRef secret, CFErrorRef *error){
    secnotice("devRecovery", "Enter SOSCCCopyMyPeerWithNewDeviceRecoverySecret()");
    CFDataRef publicKeyData = SOSCopyDeviceBackupPublicKey(secret, error);
    secnotice("devRecovery", "SOSCopyDeviceBackupPublicKey (%@)", publicKeyData);
    SOSPeerInfoRef copiedPeer = publicKeyData ? SOSSetNewPublicBackupKey(publicKeyData, error) : NULL;
    secnotice("devRecovery", "SOSSetNewPublicBackupKey (%@)", copiedPeer);
    CFReleaseNull(publicKeyData);
    return copiedPeer;
}

bool SOSCCRegisterSingleRecoverySecret(CFDataRef aks_bag, bool forV0Only, CFErrorRef *error){
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_RegisterSingleRecoverySecret, aks_bag, forV0Only, error);
        return keybag_and_bool_to_bool_error_request(kSecXPCOpSetBagForAllSlices, aks_bag, forV0Only, error);
    }, NULL);
}


bool SOSCCRegisterRecoveryPublicKey(CFDataRef recovery_key, CFErrorRef *error){
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        bool retval = false;
        do_if_registered(soscc_RegisterRecoveryPublicKey, recovery_key, error);
        // NULL recovery_key is handled in recovery_and_bool_to_bool_error_request now.
        retval = recovery_and_bool_to_bool_error_request(kSecXPCOpRegisterRecoveryPublicKey, recovery_key, error);
        return retval;
    }, NULL);
}

CFDataRef SOSCCCopyRecoveryPublicKey(CFErrorRef *error){
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFDataRef, ^{
        do_if_registered(soscc_CopyRecoveryPublicKey, error);
        return data_to_error_request(kSecXPCOpGetRecoveryPublicKey, error);
    }, CFSTR("return=%@"));
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
                                                     CFStringRef dsid, CFDataRef parentEvent, CFErrorRef* error)
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
        if(parentEvent){
            SecXPCDictionarySetData(message, kSecXPCKeySignInAnalytics, parentEvent, error);
        }
        return true;
    }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
        result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
        return result;
    });
    
    return result;
}

bool SOSCCRegisterUserCredentials(CFStringRef user_label, CFDataRef user_password, CFErrorRef* error)
{
    secnotice("circleOps", "SOSCCRegisterUserCredentials - calling SOSCCSetUserCredentials for %@\n", user_label);
    return SOSCCSetUserCredentials(user_label, user_password, error);
}

bool SOSCCSetUserCredentials(CFStringRef user_label, CFDataRef user_password, CFErrorRef* error)
{
    secnotice("circleOps", "SOSCCSetUserCredentials for %@\n", user_label);
	sec_trace_enter_api(CFSTR("user_label=%@"), user_label);
    sec_trace_return_bool_api(^{
		do_if_registered(soscc_SetUserCredentials, user_label, user_password, error);

    	return label_and_password_to_bool_error_request(kSecXPCOpSetUserCredentials, user_label, user_password, error);
    }, NULL)
}

bool SOSCCSetUserCredentialsAndDSID(CFStringRef user_label, CFDataRef user_password, CFStringRef dsid, CFErrorRef *error)
{
    secnotice("circleOps", "SOSCCSetUserCredentialsAndDSID for %@\n", user_label);
    sec_trace_enter_api(CFSTR("user_label=%@"), user_label);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_SetUserCredentialsAndDSID, user_label, user_password, dsid, error);

        bool result = false;
        __block CFStringRef account_dsid = dsid;

        require_action_quiet(user_label, out, SOSErrorCreate(kSOSErrorParam, error, NULL, CFSTR("user_label is nil")));
        require_action_quiet(user_password, out, SOSErrorCreate(kSOSErrorParam, error, NULL, CFSTR("user_password is nil")));

        if(account_dsid == NULL){
            account_dsid = CFSTR("");
        }
        return label_and_password_and_dsid_to_bool_error_request(kSecXPCOpSetUserCredentialsAndDSID, user_label, user_password, account_dsid, nil, error);
    out:
        return result;

    }, NULL)
}

bool SOSCCSetUserCredentialsAndDSIDWithAnalytics(CFStringRef user_label, CFDataRef user_password, CFStringRef dsid, CFDataRef parentEvent, CFErrorRef *error)
{
    secnotice("circleOps", "SOSCCSetUserCredentialsAndDSIDWithAnalytics for %@\n", user_label);
    sec_trace_enter_api(CFSTR("user_label=%@"), user_label);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_SetUserCredentialsAndDSIDWithAnalytics, user_label, user_password, dsid, parentEvent, error);

        bool result = false;
        __block CFStringRef account_dsid = dsid;

        require_action_quiet(user_label, out, SOSErrorCreate(kSOSErrorParam, error, NULL, CFSTR("user_label is nil")));
        require_action_quiet(user_password, out, SOSErrorCreate(kSOSErrorParam, error, NULL, CFSTR("user_password is nil")));

        if(account_dsid == NULL){
            account_dsid = CFSTR("");
        }
        return label_and_password_and_dsid_to_bool_error_request(kSecXPCOpSetUserCredentialsAndDSIDWithAnalytics, user_label, user_password, account_dsid, parentEvent, error);
    out:
        return result;

    }, NULL)
}

static bool SOSCCTryUserCredentialsAndDSID_internal(CFStringRef user_label, CFDataRef user_password, CFStringRef dsid, CFErrorRef *error) {
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_TryUserCredentials, user_label, user_password, dsid, error);
        
        bool result = false;
        __block CFStringRef account_dsid = dsid;
        
        require_action_quiet(user_label, out, SOSErrorCreate(kSOSErrorParam, error, NULL, CFSTR("user_label is nil")));
        require_action_quiet(user_password, out, SOSErrorCreate(kSOSErrorParam, error, NULL, CFSTR("user_password is nil")));
        
        if(account_dsid == NULL){
            account_dsid = CFSTR("");
        }
        
        return label_and_password_and_dsid_to_bool_error_request(kSecXPCOpTryUserCredentials, user_label, user_password, account_dsid, nil, error);
    out:
        return result;
        
    }, NULL)

}

bool SOSCCTryUserCredentialsAndDSID(CFStringRef user_label, CFDataRef user_password, CFStringRef dsid, CFErrorRef *error)
{
    secnotice("sosops", "SOSCCTryUserCredentialsAndDSID!! %@\n", user_label);
    require_action_quiet(user_label, out, SOSErrorCreate(kSOSErrorParam, error, NULL, CFSTR("user_label is nil")));
    require_action_quiet(user_password, out, SOSErrorCreate(kSOSErrorParam, error, NULL, CFSTR("user_password is nil")));
    CFStringRef account_dsid = (dsid != NULL) ? dsid: CFSTR("");
    return SOSCCTryUserCredentialsAndDSID_internal(user_label, user_password, account_dsid, error);
out:
    return false;
}

bool SOSCCTryUserCredentials(CFStringRef user_label, CFDataRef user_password, CFErrorRef* error) {
    return SOSCCTryUserCredentialsAndDSID_internal(user_label, user_password, NULL, error);
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

bool SOSCCProcessEnsurePeerRegistration(CFErrorRef* error){
    secnotice("updates", "enter SOSCCProcessEnsurePeerRegistration");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_EnsurePeerRegistration, error);
        
        return simple_bool_error_request(soscc_EnsurePeerRegistration_id, error);
    }, NULL)
}


CFSetRef /* CFString */ SOSCCProcessSyncWithPeers(CFSetRef peers, CFSetRef backupPeers, CFErrorRef* error)
{
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFSetRef, ^{
        do_if_registered(soscc_ProcessSyncWithPeers, peers, backupPeers, error);

        return cfset_cfset_to_cfset_error_request(kSecXPCOpProcessSyncWithPeers, peers, backupPeers, error);
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
            return CFSTR("Unknown Status");
    };
}

#if 0
    kSOSCCGeneralViewError    = -1,
    kSOSCCViewMember          = 0,
    kSOSCCViewNotMember       = 1,
    kSOSCCViewNotQualified    = 2,
    kSOSCCNoSuchView          = 3,
#endif


CFStringRef SOSCCGetViewResultDescription(SOSViewResultCode vrc)
{
    switch (vrc) {
        case kSOSCCGeneralViewError:
            return CFSTR("GeneralViewError");
        case kSOSCCViewMember:
            return CFSTR("ViewMember");
        case kSOSCCViewNotMember:
            return CFSTR("ViewNotMember");
        case kSOSCCViewNotQualified:
            return CFSTR("ViewNotQualified");
        case kSOSCCNoSuchView:
            return CFSTR("ViewUndefined");
        default:
            return CFSTR("Unknown View Status");
    };
}


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
    if(actionCode == kSOSCCViewQuery) {
        uint64_t circleStat = SOSGetCachedCircleBitmask();
        if(circleStat & CC_STATISVALID) {
            SOSViewResultCode retval = kSOSCCViewNotMember;
            CFSetRef enabledViews = SOSCreateCachedViewStatus();
            if(enabledViews) {
                if(CFSetContainsValue(enabledViews, view)) {
                    retval = kSOSCCViewMember;
                } else {
                    retval = kSOSCCViewNotMember;
                }
                CFReleaseNull(enabledViews);
            }
            return retval;
        }
    }
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

bool SOSCCViewSetWithAnalytics(CFSetRef enabledViews, CFSetRef disabledViews, CFDataRef parentEvent) {
    CFErrorRef *error = NULL;
    __block bool result = false;

    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_ViewSetWithAnalytics, enabledViews, disabledViews, parentEvent);
        return securityd_send_sync_and_do(kSecXPCOpViewSetWithAnalytics, error, ^bool(xpc_object_t message, CFErrorRef *error) {
            xpc_object_t enabledSetXpc = CreateXPCObjectWithCFSetRef(enabledViews, error);
            xpc_object_t disabledSetXpc = CreateXPCObjectWithCFSetRef(disabledViews, error);
            if (enabledSetXpc) xpc_dictionary_set_value(message, kSecXPCKeyEnabledViewsKey, enabledSetXpc);
            if (disabledSetXpc) xpc_dictionary_set_value(message, kSecXPCKeyDisabledViewsKey, disabledSetXpc);
            if(parentEvent) SecXPCDictionarySetData(message, kSecXPCKeySignInAnalytics, parentEvent, error);
            return (enabledSetXpc != NULL) || (disabledSetXpc != NULL) ;
        }, ^bool(xpc_object_t response, __unused CFErrorRef *error) {
            result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
            return result;
        });
    }, NULL)
}

static CFStringRef copyViewNames(size_t n, CFStringRef *views) {
    CFMutableStringRef retval = CFStringCreateMutable(kCFAllocatorDefault, 0);
    CFStringAppend(retval, CFSTR("|"));
    for(size_t i = 0; i < n; i++) {
        CFStringAppend(retval, views[i]);
        CFStringAppend(retval, CFSTR("|"));
    }
    return retval;
}

static bool sosIsViewSetSyncing(size_t n, CFStringRef *views) {
    __block bool retval = true;
    CFErrorRef error = NULL;
    
    if(n == 0 || views == NULL) return false;
    CFStringRef viewString = copyViewNames(n, views);
    
    SOSCCStatus cstatus = SOSCCThisDeviceIsInCircle(&error);
    if(cstatus != kSOSCCInCircle) {
        secnotice("viewCheck", "Checking view / circle status for %@:  SOSCCStatus: (%@)  Error: (%@)", viewString, SOSCCGetStatusDescription(cstatus), error);
        retval = false;
    }

    if(retval == true) {
        //murfxx
        // use cached values if valid
        uint64_t circleStat = SOSGetCachedCircleBitmask();
        if(circleStat & CC_STATISVALID) {
            CFSetRef enabledViews = SOSCreateCachedViewStatus();
            if(enabledViews) {
                for(size_t i = 0; i < n; i++) {
                    if(!CFSetContainsValue(enabledViews, views[i])) {
                        retval = false;
                    }
                }
                CFReleaseNull(enabledViews);
                CFReleaseNull(viewString);
                return retval;
            }
        }
        
        // make the individual calls otherwise.
        for(size_t i = 0; i < n; i++) {
            SOSViewResultCode vstatus = SOSCCView(views[i], kSOSCCViewQuery, &error);
            if(vstatus != kSOSCCViewMember) {
                secnotice("viewCheck", "Checking view / circle status for %@:  SOSCCStatus: (%@) SOSViewResultCode(%@) Error: (%@)", views[i],
                          SOSCCGetStatusDescription(cstatus), SOSCCGetViewResultDescription(vstatus), error);
                retval = false;
            }
        }
    }

    if(retval == true) {
        secnotice("viewCheck", "Checking view / circle status for %@:  ENABLED", viewString);
    }
    CFReleaseNull(error);
    CFReleaseNull(viewString);
    return retval;
}

bool SOSCCIsIcloudKeychainSyncing(void) {
    CFStringRef views[] = { kSOSViewWiFi, kSOSViewAutofillPasswords, kSOSViewSafariCreditCards, kSOSViewOtherSyncable };
    return sosIsViewSetSyncing(sizeof(views)/sizeof(views[0]), views);
}

bool SOSCCIsSafariSyncing(void) {
    CFStringRef views[] = { kSOSViewAutofillPasswords, kSOSViewSafariCreditCards };
    return sosIsViewSetSyncing(sizeof(views)/sizeof(views[0]), views);
}

bool SOSCCIsAppleTVSyncing(void) {
    CFStringRef views[] = { kSOSViewAppleTV };
    return sosIsViewSetSyncing(sizeof(views)/sizeof(views[0]), views);
}

bool SOSCCIsHomeKitSyncing(void) {
    CFStringRef views[] = { kSOSViewHomeKit };
    return sosIsViewSetSyncing(sizeof(views)/sizeof(views[0]), views);
}

bool SOSCCIsWiFiSyncing(void) {
    CFStringRef views[] = { kSOSViewWiFi };
    return sosIsViewSetSyncing(sizeof(views)/sizeof(views[0]), views);
}

bool SOSCCIsContinuityUnlockSyncing(void) {
    CFStringRef views[] = { kSOSViewContinuityUnlock };
    return sosIsViewSetSyncing(sizeof(views)/sizeof(views[0]), views);
}


bool SOSCCSetEscrowRecord(CFStringRef escrow_label, uint64_t tries, CFErrorRef *error ){
    secnotice("escrow", "enter SOSCCSetEscrowRecord");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_SetEscrowRecords, escrow_label, tries, error);
        
        return escrow_to_bool_error_request(kSecXPCOpSetEscrowRecord, escrow_label, tries, error);
    }, NULL)
}

CFDictionaryRef SOSCCCopyEscrowRecord(CFErrorRef *error){
    secnotice("escrow", "enter SOSCCCopyEscrowRecord");
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFDictionaryRef, ^{
        do_if_registered(soscc_CopyEscrowRecords, error);
        
        return strings_to_dictionary_error_request(kSecXPCOpGetEscrowRecord, error);
    }, CFSTR("return=%@"))

}

CFDictionaryRef SOSCCCopyBackupInformation(CFErrorRef *error) {
    secnotice("escrow", "enter SOSCCCopyBackupInformation");
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFDictionaryRef, ^{
        do_if_registered(soscc_CopyBackupInformation, error);
        return strings_to_dictionary_error_request(kSecXPCOpCopyBackupInformation, error);
    }, CFSTR("return=%@"))
}

bool SOSWrapToBackupSliceKeyBagForView(CFStringRef viewName, CFDataRef input, CFDataRef* output, CFDataRef* bskbEncoded, CFErrorRef* error) {
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(sosbskb_WrapToBackupSliceKeyBagForView, viewName, input, output, bskbEncoded, error);

        return cfstring_and_cfdata_to_cfdata_cfdata_error_request(kSecXPCOpWrapToBackupSliceKeyBagForView, viewName, input, output, bskbEncoded, error);
    }, NULL)
}


SOSPeerInfoRef SOSCCCopyApplication(CFErrorRef *error) {
    secnotice("hsa2PB", "enter SOSCCCopyApplication applicant");
    sec_trace_enter_api(NULL);
    
    sec_trace_return_api(SOSPeerInfoRef, ^{
        do_if_registered(soscc_CopyApplicant, error);
        return peer_info_error_request(kSecXPCOpCopyApplication, error);
    }, CFSTR("return=%@"));
}

bool SOSCCCleanupKVSKeys(CFErrorRef *error) {
    secnotice("cleanup-keys", "enter SOSCCCleanupKVSKeys");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_SOSCCCleanupKVSKeys, error);
        
        return simple_bool_error_request(kSecXPCOpKVSKeyCleanup, error);
    }, NULL)
    
    return false;
}

bool SOSCCTestPopulateKVSWithBadKeys(CFErrorRef *error) {
    secnotice("cleanup-keys", "enter SOSCCPopulateKVSWithBadKeys");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_SOSCCTestPopulateKVSWithBadKeys, error);
        
        return simple_bool_error_request(kSecXPCOpPopulateKVS, error);
    }, NULL)
    
    return false;
}

CFDataRef SOSCCCopyCircleJoiningBlob(SOSPeerInfoRef applicant, CFErrorRef *error) {
    secnotice("hsa2PB", "enter SOSCCCopyCircleJoiningBlob approver");
    sec_trace_enter_api(NULL);

    sec_trace_return_api(CFDataRef, ^{
        CFDataRef result = NULL;
        do_if_registered(soscc_CopyCircleJoiningBlob, applicant, error);
        CFDataRef piData = SOSPeerInfoCopyEncodedData(applicant, kCFAllocatorDefault, error);
        result = cfdata_error_request_returns_cfdata(kSecXPCOpCopyCircleJoiningBlob, piData, error);
        CFReleaseNull(piData);
        return result;
    }, CFSTR("return=%@"));
}

CFDataRef SOSCCCopyInitialSyncData(CFErrorRef *error) {
    secnotice("circleJoin", "enter SOSCCCopyInitialSyncData approver");
    sec_trace_enter_api(NULL);
    
    sec_trace_return_api(CFDataRef, ^{
        do_if_registered(soscc_CopyInitialSyncData, error);
        return data_to_error_request(kSecXPCOpCopyInitialSyncBlob, error);
    }, CFSTR("return=%@"));
}

bool SOSCCJoinWithCircleJoiningBlob(CFDataRef joiningBlob, PiggyBackProtocolVersion version, CFErrorRef *error) {
    secnotice("hsa2PB", "enter SOSCCJoinWithCircleJoiningBlob applicant");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_JoinWithCircleJoiningBlob, joiningBlob, version, error);
        
        return cfdata_and_int_error_request_returns_bool(kSecXPCOpJoinWithCircleJoiningBlob, joiningBlob, version, error);
    }, NULL)

    return false;
}

bool SOSCCIsThisDeviceLastBackup(CFErrorRef *error) {
    secnotice("peer", "enter SOSCCIsThisDeviceLastBackup");
    sec_trace_enter_api(NULL);
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_IsThisDeviceLastBackup, error);
        
        return simple_bool_error_request(kSecXPCOpIsThisDeviceLastBackup, error);
    }, NULL)
}

CFBooleanRef SOSCCPeersHaveViewsEnabled(CFArrayRef viewNames, CFErrorRef *error) {
    secnotice("view-enabled", "enter SOSCCPeersHaveViewsEnabled");
    sec_trace_enter_api(NULL);
    sec_trace_return_api(CFBooleanRef, ^{
        do_if_registered(soscc_SOSCCPeersHaveViewsEnabled, viewNames, error);

        return cfarray_to_cfboolean_error_request(kSecXPCOpPeersHaveViewsEnabled, viewNames, error);
    }, CFSTR("return=%@"))
}

bool SOSCCMessageFromPeerIsPending(SOSPeerInfoRef peer, CFErrorRef *error) {
    secnotice("pending-check", "enter SOSCCMessageFromPeerIsPending");

    sec_trace_return_bool_api(^{
        do_if_registered(soscc_SOSCCMessageFromPeerIsPending, peer, error);

        return peer_info_to_bool_error_request(kSecXPCOpMessageFromPeerIsPending, peer, error);
    }, NULL)

}

bool SOSCCSendToPeerIsPending(SOSPeerInfoRef peer, CFErrorRef *error) {
    sec_trace_return_bool_api(^{
        do_if_registered(soscc_SOSCCSendToPeerIsPending, peer, error);

        return peer_info_to_bool_error_request(kSecXPCOpSendToPeerIsPending, peer, error);
    }, NULL)
}

/*
 * SecSOSStatus interfaces
 */

@interface SecSOSStatus : NSObject {
    NSXPCConnection* _connection;
}
@property NSXPCConnection *connection;
@end

@implementation SecSOSStatus
@synthesize connection = _connection;

- (instancetype) init
{
    if ((self = [super init]) == NULL)
        return NULL;

    NSXPCInterface *interface = [NSXPCInterface interfaceWithProtocol:@protocol(SOSControlProtocol)];
    _SOSControlSetupInterface(interface);

    self.connection = [[NSXPCConnection alloc] initWithMachServiceName:@(kSecuritydSOSServiceName) options:0];
    if (self.connection == NULL)
        return NULL;

    self.connection.remoteObjectInterface = interface;

    [self.connection resume];

    return self;
}

@end

static id<SOSControlProtocol>
SOSCCGetStatusObject(CFErrorRef *error)
{
    if (gSecurityd && gSecurityd->soscc_status)
        return (__bridge id<SOSControlProtocol>)gSecurityd->soscc_status();

    static SecSOSStatus *control;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        control = [[SecSOSStatus alloc] init];
    });
    return control.connection.remoteObjectProxy;
}

void
SOSCCAccountGetPublicKey(void (^reply)(BOOL trusted, NSData *data, NSError *error))
{
    CFErrorRef error = NULL;
    id<SOSControlProtocol> status = SOSCCGetStatusObject(&error);
    if (status == NULL) {
        reply(false, NULL, (__bridge NSError *)error);
        CFReleaseNull(error);
        return;
    }

    [status userPublicKey:reply];
}

void
SOSCCAccountGetAccountPrivateCredential(void (^complete)(NSData *data, NSError *error))
{
    CFErrorRef error = NULL;
    id<SOSControlProtocol> status = SOSCCGetStatusObject(&error);
    if (status == NULL) {
        complete(NULL, (__bridge NSError *)error);
        CFReleaseNull(error);
        return;
    }

    [status validatedStashedAccountCredential:complete];
}

void
SOSCCAccountGetKeyCircleGeneration(void (^reply)(NSData *data, NSError *error))
{
    SOSCCAccountGetPublicKey(^(BOOL __unused trusted, NSData *data, NSError *error){
        if (data == NULL) {
            reply(data, error);
        } else {
            NSMutableData *digest = [NSMutableData dataWithLength:CCSHA256_OUTPUT_SIZE];
            ccdigest(ccsha256_di(), [data length], [data bytes], [digest mutableBytes]);
            reply(digest, error);
        }
    });
}


