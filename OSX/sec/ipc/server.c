/*
 * Copyright (c) 2007-2015 Apple Inc.  All Rights Reserved.
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

#include <Security/SecureObjectSync/SOSPeerInfoDER.h>
#include <Security/SecureObjectSync/SOSCloudCircle.h>
#include <Security/SecureObjectSync/SOSCloudCircleInternal.h>
#include <Security/SecureObjectSync/SOSInternal.h>
#include <Security/SecureObjectSync/SOSPeerInfoCollections.h>
#include <Security/SecBasePriv.h>
#include <Security/SecCertificatePriv.h>
#include <Security/SecEntitlements.h>
#include <Security/SecInternal.h>
#include <Security/SecItemPriv.h> /* For SecItemDeleteAll */
#include <Security/SecPolicyInternal.h>
#include <Security/SecTask.h>
#include <Security/SecuritydXPC.h>
#include <securityd/OTATrustUtilities.h>
#include <securityd/SOSCloudCircleServer.h>
#include <securityd/SecItemBackupServer.h>
#include <securityd/SecItemServer.h>
#include <securityd/SecLogSettingsServer.h>
#include <securityd/SecOTRRemote.h>
#include <securityd/SecTrustServer.h>
#include <securityd/SecTrustStoreServer.h>
#include <securityd/iCloudTrace.h>
#include <securityd/spi.h>
#include <utilities/SecCFError.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecDb.h>
#include <utilities/SecIOFormat.h>
#include <utilities/SecXPCError.h>
#include <utilities/debugging.h>

#include <AssertMacros.h>
#include <CoreFoundation/CFXPCBridge.h>
#include <CoreFoundation/CoreFoundation.h>
#include <asl.h>
#include <bsm/libbsm.h>
#include <ipc/securityd_client.h>
#include <libkern/OSAtomic.h>
#include <mach/mach.h>
#include <mach/message.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <sys/sysctl.h>
#include <syslog.h>
#include <xpc/private.h>
#include <xpc/xpc.h>

#if (TARGET_OS_MAC && !(TARGET_OS_EMBEDDED || TARGET_OS_IPHONE))
#include <Security/SecTaskPriv.h>
#endif

static CFStringRef SecTaskCopyStringForEntitlement(SecTaskRef task,
    CFStringRef entitlement)
{
    CFStringRef value = (CFStringRef)SecTaskCopyValueForEntitlement(task,
        entitlement, NULL);
    if (value && CFGetTypeID(value) != CFStringGetTypeID()) {
        CFRelease(value);
        value = NULL;
    }

    return value;
}

static CFArrayRef SecTaskCopyArrayOfStringsForEntitlement(SecTaskRef task,
    CFStringRef entitlement)
{
    CFArrayRef value = (CFArrayRef)SecTaskCopyValueForEntitlement(task,
        entitlement, NULL);
    if (value) {
        if (CFGetTypeID(value) == CFArrayGetTypeID()) {
            CFIndex ix, count = CFArrayGetCount(value);
            for (ix = 0; ix < count; ++ix) {
                CFStringRef string = (CFStringRef)CFArrayGetValueAtIndex(value, ix);
                if (CFGetTypeID(string) != CFStringGetTypeID()) {
                    CFRelease(value);
                    value = NULL;
                    break;
                }
            }
        } else {
            CFRelease(value);
            value = NULL;
        }
    }

    return value;
}

static CFStringRef SecTaskCopyApplicationIdentifier(SecTaskRef task) {
    return SecTaskCopyStringForEntitlement(task,
        kSecEntitlementApplicationIdentifier);
}

static CFArrayRef SecTaskCopySharedWebCredentialDomains(SecTaskRef task) {
    return SecTaskCopyArrayOfStringsForEntitlement(task,
        kSecEntitlementAssociatedDomains);
}

static CFArrayRef SecTaskCopyAccessGroups(SecTaskRef task) {
    CFMutableArrayRef groups = NULL;
    CFArrayRef keychainAccessGroups = SecTaskCopyArrayOfStringsForEntitlement(task,
        kSecEntitlementKeychainAccessGroups);
    CFArrayRef appleSecurityApplicationGroups = SecTaskCopyArrayOfStringsForEntitlement(task,
        kSecEntitlementAppleSecurityApplicationGroups);
    CFStringRef appID = SecTaskCopyApplicationIdentifier(task);
    CFIndex kagLen = keychainAccessGroups ? CFArrayGetCount(keychainAccessGroups) : 0;
    CFIndex asagLen = appleSecurityApplicationGroups ? CFArrayGetCount(appleSecurityApplicationGroups) : 0;
#if (TARGET_OS_MAC && !(TARGET_OS_EMBEDDED || TARGET_OS_IPHONE))
    if ((appID || asagLen) && !SecTaskEntitlementsValidated(task)) {
        CFReleaseNull(appID);
        asagLen = 0;
    }
#endif
    CFIndex len = kagLen + asagLen + (appID ? 1 : 0);
    if (len) {
        groups = CFArrayCreateMutable(kCFAllocatorDefault, len, &kCFTypeArrayCallBacks);
        if (kagLen)
            CFArrayAppendArray(groups, keychainAccessGroups, CFRangeMake(0, kagLen));
        if (appID)
            CFArrayAppendValue(groups, appID);
        if (asagLen)
            CFArrayAppendArray(groups, appleSecurityApplicationGroups, CFRangeMake(0, asagLen));
#if TARGET_IPHONE_SIMULATOR
    } else {
        secwarning("No keychain access group specified whilst running in simulator, falling back to default set");
        groups = (CFMutableArrayRef)CFRetainSafe(SecAccessGroupsGetCurrent());
#endif
    }

    CFReleaseSafe(appID);
    CFReleaseSafe(keychainAccessGroups);
    CFReleaseSafe(appleSecurityApplicationGroups);
    return groups;
}

static bool SecTaskGetBooleanValueForEntitlement(SecTaskRef task,
    CFStringRef entitlement) {
    CFStringRef canModify = (CFStringRef)SecTaskCopyValueForEntitlement(task,
        entitlement, NULL);
    if (!canModify)
        return false;
    CFTypeID canModifyType = CFGetTypeID(canModify);
    bool ok = (CFBooleanGetTypeID() == canModifyType) && CFBooleanGetValue((CFBooleanRef)canModify);
    CFRelease(canModify);
    return ok;
}

static void with_label_and_password(xpc_object_t message, void (^action)(CFStringRef label, CFDataRef password)) {
    const char *label_utf8 = xpc_dictionary_get_string(message, kSecXPCKeyUserLabel);
    
    if (label_utf8) {   // Anything we would do here requires a user label
        size_t password_length = 0;
        const void *password_data = xpc_dictionary_get_data(message, kSecXPCKeyUserPassword, &password_length);
        
        CFDataRef user_password = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, password_data, password_length, kCFAllocatorNull);
        CFStringRef user_label = CFStringCreateWithCString(kCFAllocatorDefault, label_utf8, kCFStringEncodingUTF8);
        
        action(user_label, user_password);

        CFReleaseNull(user_password);
        CFReleaseNull(user_label);
    }
}

static void with_label_and_password_and_dsid(xpc_object_t message, void (^action)(CFStringRef label, CFDataRef password, CFStringRef dsid)) {
    const char *label_utf8 = xpc_dictionary_get_string(message, kSecXPCKeyUserLabel);
    
    if (label_utf8) {   // Anything we would do here requires a user label
        size_t password_length = 0;
        const void *password_data = xpc_dictionary_get_data(message, kSecXPCKeyUserPassword, &password_length);
        const char *xdsid = xpc_dictionary_get_string(message, kSecXPCKeyDSID);
        
        CFDataRef user_password = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, password_data, password_length, kCFAllocatorNull);
        CFStringRef user_label = CFStringCreateWithCString(kCFAllocatorDefault, label_utf8, kCFStringEncodingUTF8);
        CFStringRef dsid = CFStringCreateWithCString(kCFAllocatorDefault, xdsid, kCFStringEncodingUTF8);
        
        action(user_label, user_password, dsid);
        
        CFReleaseNull(dsid);
        CFReleaseNull(user_password);
        CFReleaseNull(user_label);
    }
}

static void with_label_and_number(xpc_object_t message, void (^action)(CFStringRef label, uint64_t number)) {
    const char *label_utf8 = xpc_dictionary_get_string(message, kSecXPCKeyViewName);
    const int64_t number = xpc_dictionary_get_int64(message, kSecXPCKeyViewActionCode);
    secnotice("views", "Action Code Raw is %d", (int) number);
    CFStringRef user_label = CFStringCreateWithCString(kCFAllocatorDefault, label_utf8, kCFStringEncodingUTF8);

    action(user_label, number);
    CFReleaseNull(user_label);
}

static bool SecXPCDictionarySetChainOptional(xpc_object_t message, const char *key, SecCertificatePathRef path, CFErrorRef *error) {
    if (!path)
        return true;
    xpc_object_t xpc_chain = SecCertificatePathCopyXPCArray(path, error);
    if (!xpc_chain)
        return false;

    xpc_dictionary_set_value(message, key, xpc_chain);
    xpc_release(xpc_chain);
    return true;
}

static SecCertificateRef SecXPCDictionaryCopyCertificate(xpc_object_t message, const char *key, CFErrorRef *error) {
    size_t length = 0;
    const void *bytes = xpc_dictionary_get_data(message, key, &length);
    if (bytes) {
        SecCertificateRef certificate = SecCertificateCreateWithBytes(kCFAllocatorDefault, bytes, length);
        if (certificate)
            return certificate;
        SecError(errSecDecode, error, CFSTR("object for key %s failed to create certificate from data"), key);
    } else {
        SecError(errSecParam, error, CFSTR("object for key %s missing"), key);
    }
    return NULL;
}

static bool SecXPCDictionaryCopyCertificates(xpc_object_t message, const char *key, CFArrayRef *certificates, CFErrorRef *error) {
    xpc_object_t xpc_certificates = xpc_dictionary_get_value(message, key);
    if (!xpc_certificates)
        return SecError(errSecAllocate, error, CFSTR("no certs for key %s"), key);
    *certificates = SecCertificateXPCArrayCopyArray(xpc_certificates, error);
    return *certificates;
}

static bool SecXPCDictionaryCopyCertificatesOptional(xpc_object_t message, const char *key, CFArrayRef *certificates, CFErrorRef *error) {
    xpc_object_t xpc_certificates = xpc_dictionary_get_value(message, key);
    if (!xpc_certificates) {
        *certificates = NULL;
        return true;
    }
    *certificates = SecCertificateXPCArrayCopyArray(xpc_certificates, error);
    return *certificates;
}

static bool SecXPCDictionaryCopyPoliciesOptional(xpc_object_t message, const char *key, CFArrayRef *policies, CFErrorRef *error) {
    xpc_object_t xpc_policies = xpc_dictionary_get_value(message, key);
    if (!xpc_policies) {
        if (policies)
            *policies = NULL;
        return true;
    }
    *policies = SecPolicyXPCArrayCopyArray(xpc_policies, error);
    return *policies != NULL;
}

static SecTrustStoreRef SecXPCDictionaryGetTrustStore(xpc_object_t message, const char *key, CFErrorRef *error) {
    SecTrustStoreRef ts = NULL;
    CFStringRef domain = SecXPCDictionaryCopyString(message, key, error);
    if (domain) {
        ts = SecTrustStoreForDomainName(domain, error);
        CFRelease(domain);
    }
    return ts;
}

static bool SecXPCDictionaryGetDouble(xpc_object_t message, const char *key, double *pvalue, CFErrorRef *error) {
    *pvalue = xpc_dictionary_get_double(message, key);
    if (*pvalue == NAN) {
        return SecError(errSecParam, error, CFSTR("object for key %s bad double"), key);
    }
    return true;
}

static CFDataRef CFDataCreateWithXPCArrayAtIndex(xpc_object_t xpc_data_array, size_t index, CFErrorRef *error) {
    CFDataRef data = NULL;
    size_t length = 0;
    const uint8_t *bytes = xpc_array_get_data(xpc_data_array, index, &length);
    if (bytes) {
        data = CFDataCreate(kCFAllocatorDefault, bytes, length);
    }
    if (!data)
        SecError(errSecParam, error, CFSTR("data_array[%zu] failed to decode"), index);

    return data;
}

static CFArrayRef CFDataXPCArrayCopyArray(xpc_object_t xpc_data_array, CFErrorRef *error) {
    CFMutableArrayRef data_array = NULL;
    require_action_quiet(xpc_get_type(xpc_data_array) == XPC_TYPE_ARRAY, exit,
                         SecError(errSecParam, error, CFSTR("data_array xpc value is not an array")));
    size_t count = xpc_array_get_count(xpc_data_array);
    require_action_quiet(data_array = CFArrayCreateMutable(kCFAllocatorDefault, count, &kCFTypeArrayCallBacks), exit,
                         SecError(errSecAllocate, error, CFSTR("failed to create CFArray of capacity %zu"), count));

    size_t ix;
    for (ix = 0; ix < count; ++ix) {
        CFDataRef data = CFDataCreateWithXPCArrayAtIndex(xpc_data_array, ix, error);
        if (!data) {
            CFRelease(data_array);
            return NULL;
        }
        CFArraySetValueAtIndex(data_array, ix, data);
        CFRelease(data);
    }

exit:
    return data_array;
}

static bool SecXPCDictionaryCopyCFDataArrayOptional(xpc_object_t message, const char *key, CFArrayRef *data_array, CFErrorRef *error) {
    xpc_object_t xpc_data_array = xpc_dictionary_get_value(message, key);
    if (!xpc_data_array) {
        if (data_array)
            *data_array = NULL;
        return true;
    }
    *data_array = CFDataXPCArrayCopyArray(xpc_data_array, error);
    return *data_array != NULL;
}

static CFSetRef SecXPCSetCreateFromXPCDictionaryElement(xpc_object_t event, const char *key) {
    CFErrorRef error = NULL;
    xpc_object_t object = xpc_dictionary_get_value(event, key);
    CFSetRef retval = NULL;
    if(object) retval = CreateCFSetRefFromXPCObject(object, &error);
    CFReleaseNull(error);
    return retval;
}


static void securityd_xpc_dictionary_handler(const xpc_connection_t connection, xpc_object_t event) {
    xpc_type_t type = xpc_get_type(event);
    __block CFErrorRef error = NULL;
    xpc_object_t xpcError = NULL;
    xpc_object_t replyMessage = NULL;
    SecTaskRef clientTask = NULL;
    CFDataRef  clientAuditToken = NULL;
    CFArrayRef accessGroups = NULL;
    CFArrayRef domains = NULL;

    secdebug("serverxpc", "entering");
    if (type == XPC_TYPE_DICTIONARY) {
        // TODO: Find out what we're dispatching.
        replyMessage = xpc_dictionary_create_reply(event);

        uint64_t operation = xpc_dictionary_get_uint64(event, kSecXPCKeyOperation);

        bool hasEntitlement;
        audit_token_t auditToken = {};
        xpc_connection_get_audit_token(connection, &auditToken);

        clientTask = SecTaskCreateWithAuditToken(kCFAllocatorDefault, auditToken);
        clientAuditToken = CFDataCreate(kCFAllocatorDefault, (const UInt8*)&auditToken, sizeof(auditToken));
        accessGroups = SecTaskCopyAccessGroups(clientTask);
        if (operation == sec_add_shared_web_credential_id || operation == sec_copy_shared_web_credential_id) {
            domains = SecTaskCopySharedWebCredentialDomains(clientTask);
        }

        // TODO: change back to secdebug
        secinfo("serverxpc", "XPC [%@] operation: %@ (%" PRIu64 ")", clientTask, SOSCCGetOperationDescription((enum SecXPCOperation)operation), operation);

        if (true) {
            // Ensure that we remain dirty for a minimum of two seconds to avoid jetsam loops.
            // Refer to rdar://problem/18615626&18616300 for more details.
            int64_t minimumDirtyInterval = (int64_t) (2 * NSEC_PER_SEC);
            xpc_transaction_begin();
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, minimumDirtyInterval), dispatch_get_main_queue(), ^{
                xpc_transaction_end();
            });
        }

        // operations before kSecXPCOpTryUserCredentials don't need this entitlement.
        hasEntitlement = (operation < kSecXPCOpTryUserCredentials) ||
        (clientTask && SecTaskGetBooleanValueForEntitlement(clientTask, kSecEntitlementKeychainCloudCircle));

        // Per <rdar://problem/13315020> Disable the entitlement check for "keychain-cloud-circle"
        //  we disable entitlement enforcement. However, we still log so we know who needs the entitlement

        if (!hasEntitlement) {
            CFErrorRef entitlementError = NULL;
            SecError(errSecMissingEntitlement, &entitlementError, CFSTR("%@: %@ lacks entitlement %@"), SOSCCGetOperationDescription((enum SecXPCOperation)operation), clientTask, kSecEntitlementKeychainCloudCircle);
            secnotice("serverxpc", "MissingEntitlement: %@", entitlementError);
            CFReleaseSafe(entitlementError);
        }

        if (true) {
            switch (operation)
            {
            case sec_item_add_id:
            {
                CFDictionaryRef query = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyQuery, &error);
                if (query) {
                    CFTypeRef result = NULL;
                    if (_SecItemAdd(query, accessGroups, &result, &error) && result) {
                        SecXPCDictionarySetPList(replyMessage, kSecXPCKeyResult, result, &error);
                        CFRelease(result);
                    }
                    CFRelease(query);
                }
                break;
            }
            case sec_item_copy_matching_id:
            {
                CFDictionaryRef query = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyQuery, &error);
                if (query) {
                    CFTypeRef result = NULL;
                    if (_SecItemCopyMatching(query, accessGroups, &result, &error) && result) {
                        SecXPCDictionarySetPList(replyMessage, kSecXPCKeyResult, result, &error);
                        CFRelease(result);
                    }
                    CFRelease(query);
                }
                break;
            }
            case sec_item_update_id:
            {
                CFDictionaryRef query = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyQuery, &error);
                if (query) {
                    CFDictionaryRef attributesToUpdate = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyAttributesToUpdate, &error);
                    if (attributesToUpdate) {
                        bool result = _SecItemUpdate(query, attributesToUpdate, accessGroups, &error);
                        xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);
                        CFRelease(attributesToUpdate);
                    }
                    CFRelease(query);
                }
                break;
            }
            case sec_item_delete_id:
            {
                CFDictionaryRef query = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyQuery, &error);
                if (query) {
                    bool result = _SecItemDelete(query, accessGroups, &error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);
                    CFRelease(query);
                }
                break;
            }
            case sec_trust_store_contains_id:
            {
                SecTrustStoreRef ts = SecXPCDictionaryGetTrustStore(event, kSecXPCKeyDomain, &error);
                if (ts) {
                    CFDataRef digest = SecXPCDictionaryCopyData(event, kSecXPCKeyDigest, &error);
                    if (digest) {
                        bool contains;
                        if (SecTrustStoreContainsCertificateWithDigest(ts, digest, &contains, &error))
                            xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, contains);
                        CFRelease(digest);
                    }
                }
                break;
            }
            case sec_trust_store_set_trust_settings_id:
            {
                if (SecTaskGetBooleanValueForEntitlement(clientTask, kSecEntitlementModifyAnchorCertificates)) {
                    SecTrustStoreRef ts = SecXPCDictionaryGetTrustStore(event, kSecXPCKeyDomain, &error);
                    if (ts) {
                        SecCertificateRef certificate = SecXPCDictionaryCopyCertificate(event, kSecXPCKeyCertificate, &error);
                        if (certificate) {
                            CFTypeRef trustSettingsDictOrArray = NULL;
                            if (SecXPCDictionaryCopyPListOptional(event, kSecXPCKeySettings, &trustSettingsDictOrArray, &error)) {
                                bool result = _SecTrustStoreSetTrustSettings(ts, certificate, trustSettingsDictOrArray, &error);
                                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);
                                CFReleaseSafe(trustSettingsDictOrArray);
                            }
                            CFRelease(certificate);
                        }
                    }
                } else {
                    SecError(errSecMissingEntitlement, &error, CFSTR("%@: %@ lacks entitlement %@"), SOSCCGetOperationDescription((enum SecXPCOperation)operation), clientTask, kSecEntitlementModifyAnchorCertificates);
                }
                break;
            }
            case sec_trust_store_remove_certificate_id:
            {
                if (SecTaskGetBooleanValueForEntitlement(clientTask, kSecEntitlementModifyAnchorCertificates)) {
                    SecTrustStoreRef ts = SecXPCDictionaryGetTrustStore(event, kSecXPCKeyDomain, &error);
                    if (ts) {
                        CFDataRef digest = SecXPCDictionaryCopyData(event, kSecXPCKeyDigest, &error);
                        if (digest) {
                            bool result = SecTrustStoreRemoveCertificateWithDigest(ts, digest, &error);
                            xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);
                            CFRelease(digest);
                        }
                    }
                } else {
                    SecError(errSecMissingEntitlement, &error, CFSTR("%@: %@ lacks entitlement %@"), SOSCCGetOperationDescription((enum SecXPCOperation)operation), clientTask, kSecEntitlementModifyAnchorCertificates);
                }
                break;
            }
            case sec_delete_all_id:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, _SecItemDeleteAll(&error));
                break;
            case sec_trust_evaluate_id:
            {
                CFArrayRef certificates = NULL, anchors = NULL, policies = NULL, responses = NULL, scts = NULL, trustedLogs = NULL;
                bool anchorsOnly = xpc_dictionary_get_bool(event, kSecTrustAnchorsOnlyKey);
                double verifyTime;
                if (SecXPCDictionaryCopyCertificates(event, kSecTrustCertificatesKey, &certificates, &error) &&
                    SecXPCDictionaryCopyCertificatesOptional(event, kSecTrustAnchorsKey, &anchors, &error) &&
                    SecXPCDictionaryCopyPoliciesOptional(event, kSecTrustPoliciesKey, &policies, &error) &&
                    SecXPCDictionaryCopyCFDataArrayOptional(event, kSecTrustResponsesKey, &responses, &error) &&
                    SecXPCDictionaryCopyCFDataArrayOptional(event, kSecTrustSCTsKey, &scts, &error) &&
                    SecXPCDictionaryCopyArrayOptional(event, kSecTrustTrustedLogsKey, &trustedLogs, &error) &&
                    SecXPCDictionaryGetDouble(event, kSecTrustVerifyDateKey, &verifyTime, &error)) {
                    // If we have no error yet, capture connection and reply in block and properly retain them.
                    xpc_retain(connection);
                    CFRetainSafe(clientTask);
                    CFRetainSafe(clientAuditToken);

                    // Clear replyMessage so we don't send a synchronous reply.
                    xpc_object_t asyncReply = replyMessage;
                    replyMessage = NULL;

                    SecTrustServerEvaluateBlock(clientAuditToken,
                                                certificates, anchors, anchorsOnly, policies, responses, scts, trustedLogs, verifyTime, accessGroups,
                                                ^(SecTrustResultType tr, CFArrayRef details, CFDictionaryRef info, SecCertificatePathRef chain, CFErrorRef replyError) {
                        // Send back reply now
                        if (replyError) {
                            CFRetain(replyError);
                        } else {
                            xpc_dictionary_set_int64(asyncReply, kSecTrustResultKey, tr);
                            SecXPCDictionarySetPListOptional(asyncReply, kSecTrustDetailsKey, details, &replyError) &&
                            SecXPCDictionarySetPListOptional(asyncReply, kSecTrustInfoKey, info, &replyError) &&
                            SecXPCDictionarySetChainOptional(asyncReply, kSecTrustChainKey, chain, &replyError);
                        }
                        if (replyError) {
                            secdebug("ipc", "%@ %@ %@", clientTask, SOSCCGetOperationDescription((enum SecXPCOperation)operation), replyError);
                            xpc_object_t xpcReplyError = SecCreateXPCObjectWithCFError(replyError);
                            if (xpcReplyError) {
                                xpc_dictionary_set_value(asyncReply, kSecXPCKeyError, xpcReplyError);
                                xpc_release(xpcReplyError);
                            }
                            CFRelease(replyError);
                        } else {
                            secdebug("ipc", "%@ %@ reponding %@", clientTask, SOSCCGetOperationDescription((enum SecXPCOperation)operation), asyncReply);
                        }

                        xpc_connection_send_message(connection, asyncReply);
                        xpc_release(asyncReply);
                        xpc_release(connection);
                        CFReleaseSafe(clientTask);
                        CFReleaseSafe(clientAuditToken);
                    });
                }
                CFReleaseSafe(policies);
                CFReleaseSafe(anchors);
                CFReleaseSafe(certificates);
                CFReleaseSafe(responses);
                CFReleaseSafe(scts);
                CFReleaseSafe(trustedLogs);
                break;
            }
            case sec_keychain_backup_id:
            {
                CFDataRef keybag = NULL, passcode = NULL;
                if (SecXPCDictionaryCopyDataOptional(event, kSecXPCKeyKeybag, &keybag, &error)) {
                    if (SecXPCDictionaryCopyDataOptional(event, kSecXPCKeyUserPassword, &passcode, &error)) {
                        CFDataRef backup = _SecServerKeychainBackup(keybag, passcode, &error);
                        if (backup) {
                            SecXPCDictionarySetData(replyMessage, kSecXPCKeyResult, backup, &error);
                            CFRelease(backup);
                        }
                        CFReleaseSafe(passcode);
                    }
                    CFReleaseSafe(keybag);
                }
                break;
            }
            case sec_keychain_restore_id:
            {
                CFDataRef backup = SecXPCDictionaryCopyData(event, kSecXPCKeyBackup, &error);
                if (backup) {
                    CFDataRef keybag = SecXPCDictionaryCopyData(event, kSecXPCKeyKeybag, &error);
                    if (keybag) {
                        CFDataRef passcode = NULL;
                        if (SecXPCDictionaryCopyDataOptional(event, kSecXPCKeyUserPassword, &passcode, &error)) {
                            bool result = _SecServerKeychainRestore(backup, keybag, passcode, &error);
                            xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);
                            CFReleaseSafe(passcode);
                        }
                        CFRelease(keybag);
                    }
                    CFRelease(backup);
                }
                break;
            }
            case sec_keychain_sync_update_message_id:
            {
                CFDictionaryRef updates = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyQuery, &error);
                if (updates) {
                    CFArrayRef result = _SecServerKeychainSyncUpdateMessage(updates, &error);
                    SecXPCDictionarySetPList(replyMessage, kSecXPCKeyResult, result, &error);
                    CFReleaseNull(result);
                }
                CFReleaseNull(updates);
                break;
            }
            case sec_keychain_backup_syncable_id:
            {
                CFDictionaryRef oldbackup = NULL;
                if (SecXPCDictionaryCopyDictionaryOptional(event, kSecXPCKeyBackup, &oldbackup, &error)) {
                    CFDataRef keybag = SecXPCDictionaryCopyData(event, kSecXPCKeyKeybag, &error);
                    if (keybag) {
                        CFDataRef passcode = NULL;
                        if (SecXPCDictionaryCopyDataOptional(event, kSecXPCKeyUserPassword, &passcode, &error)) {
                            CFDictionaryRef newbackup = _SecServerBackupSyncable(oldbackup, keybag, passcode, &error);
                            if (newbackup) {
                                SecXPCDictionarySetPList(replyMessage, kSecXPCKeyResult, newbackup, &error);
                                CFRelease(newbackup);
                            }
                            CFReleaseSafe(passcode);
                        }
                        CFRelease(keybag);
                    }
                    CFReleaseSafe(oldbackup);
                }
                break;
            }
            case sec_keychain_restore_syncable_id:
            {
                CFDictionaryRef backup = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyBackup, &error);
                if (backup) {
                    CFDataRef keybag = SecXPCDictionaryCopyData(event, kSecXPCKeyKeybag, &error);
                    if (keybag) {
                        CFDataRef passcode = NULL;
                        if (SecXPCDictionaryCopyDataOptional(event, kSecXPCKeyUserPassword, &passcode, &error)) {
                            bool result = _SecServerRestoreSyncable(backup, keybag, passcode, &error);
                            xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);
                            CFReleaseSafe(passcode);
                        }
                        CFRelease(keybag);
                    }
                    CFRelease(backup);
                }
                break;
            }
            case sec_item_backup_copy_names_id:
            {
                CFArrayRef names = SecServerItemBackupCopyNames(&error);
                SecXPCDictionarySetPListOptional(replyMessage, kSecXPCKeyResult, names, &error);
                CFReleaseSafe(names);
                break;
            }
            case sec_item_backup_handoff_fd_id:
            {
                CFStringRef backupName = SecXPCDictionaryCopyString(event, kSecXPCKeyBackup, &error);
                int fd = -1;
                if (backupName) {
                    fd = SecServerItemBackupHandoffFD(backupName, &error);
                    CFRelease(backupName);
                }
                SecXPCDictionarySetFileDescriptor(replyMessage, kSecXPCKeyResult, fd, &error);
                if (fd != -1)
                    close(fd);
                break;
            }
            case sec_item_backup_set_confirmed_manifest_id:
            {
                CFDataRef keybagDigest = NULL;
                if (SecXPCDictionaryCopyDataOptional(event, kSecXPCKeyKeybag, &keybagDigest, &error)) {
                    CFDataRef manifest = NULL;
                    if (SecXPCDictionaryCopyDataOptional(event, kSecXPCData, &manifest, &error)) {
                        CFStringRef backupName = SecXPCDictionaryCopyString(event, kSecXPCKeyBackup, &error);
                        if (backupName) {
                            bool result = SecServerItemBackupSetConfirmedManifest(backupName, keybagDigest, manifest, &error);
                            CFRelease(backupName);
                            xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);
                        }
                        CFReleaseSafe(manifest);
                    }
                    CFRelease(keybagDigest);
                }
                break;
            }
            case sec_item_backup_restore_id:
            {
                bool result = false;
                CFStringRef backupName = SecXPCDictionaryCopyString(event, kSecXPCKeyBackup, &error);
                if (backupName) {
                    CFStringRef peerID = NULL;
                    if (SecXPCDictionaryCopyStringOptional(event, kSecXPCKeyDigest, &peerID, &error)) {
                        CFDataRef keybag = SecXPCDictionaryCopyData(event, kSecXPCKeyKeybag, &error);
                        if (keybag) {
                            CFDataRef secret = SecXPCDictionaryCopyData(event, kSecXPCKeyUserPassword, &error);
                            if (secret) {
                                CFDataRef backup = SecXPCDictionaryCopyData(event, kSecXPCData, &error);
                                if (backup) {
                                    result = SecServerItemBackupRestore(backupName, peerID, keybag, secret, backup, &error);
                                    CFRelease(backup);
                                }
                                CFRelease(secret);
                            }
                            CFRelease(keybag);
                        }
                        CFReleaseSafe(peerID);
                    }
                    CFRelease(backupName);
                }
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);
                break;
            }
            case sec_ota_pki_asset_version_id:
            {
                xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult,
                                         SecOTAPKIGetCurrentAssetVersion(&error));
                break;
            }
            case sec_add_shared_web_credential_id:
            {
                CFDictionaryRef query = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyQuery, &error);
                if (query) {
                    CFTypeRef result = NULL;
                    CFStringRef appID = (clientTask) ? SecTaskCopyApplicationIdentifier(clientTask) : NULL;
                    if (_SecAddSharedWebCredential(query, &auditToken, appID, domains, &result, &error) && result) {
                        SecXPCDictionarySetPList(replyMessage, kSecXPCKeyResult, result, &error);
                        CFRelease(result);
                    }
                    CFReleaseSafe(appID);
                    CFRelease(query);
                }
                break;
            }
            case sec_copy_shared_web_credential_id:
            {
                CFDictionaryRef query = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyQuery, &error);
                if (query) {
                    CFTypeRef result = NULL;
                    CFStringRef appID = (clientTask) ? SecTaskCopyApplicationIdentifier(clientTask) : NULL;
                    if (_SecCopySharedWebCredential(query, &auditToken, appID, domains, &result, &error) && result) {
                        SecXPCDictionarySetPList(replyMessage, kSecXPCKeyResult, result, &error);
                        CFRelease(result);
                    }
                    CFReleaseSafe(appID);
                    CFRelease(query);
                }
                break;
            }
            case sec_get_log_settings_id:
            {
                CFPropertyListRef currentList = SecCopyLogSettings_Server(&error);
                if (currentList) {
                    SecXPCDictionarySetPList(replyMessage, kSecXPCKeyResult, currentList, &error);
                }
                CFReleaseSafe(currentList);
                break;
            }
            case sec_set_xpc_log_settings_id:
            {
                CFPropertyListRef newSettings = SecXPCDictionaryCopyPList(event, kSecXPCKeyQuery, &error);
                if (newSettings) {
                    SecSetXPCLogSettings_Server(newSettings, &error);
                }
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, true);
                CFReleaseNull(newSettings);
                break;
            }
            case sec_set_circle_log_settings_id:
            {
                CFPropertyListRef newSettings = SecXPCDictionaryCopyPList(event, kSecXPCKeyQuery, &error);
                if (newSettings) {
                    SecSetCircleLogSettings_Server(newSettings, &error);
                }
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, true);
                CFReleaseNull(newSettings);
                break;
            }
            case sec_otr_session_create_remote_id:
            {
                CFDataRef publicPeerId = NULL;
                if (SecXPCDictionaryCopyDataOptional(event, kSecXPCPublicPeerId, &publicPeerId, &error)) {
                    CFDataRef otrSession = _SecOTRSessionCreateRemote(publicPeerId, &error);
                    if (otrSession) {
                        SecXPCDictionarySetData(replyMessage, kSecXPCKeyResult, otrSession, &error);
                        CFRelease(otrSession);
                    }
                    CFReleaseSafe(publicPeerId);
                }
                break;
            }
            case sec_otr_session_process_packet_remote_id:
            {
                CFDataRef sessionData = NULL, inputPacket = NULL, outputSessionData = NULL, outputPacket = NULL;
                bool readyForMessages = false;
                if (SecXPCDictionaryCopyDataOptional(event, kSecXPCOTRSession, &sessionData, &error)) {
                    if (SecXPCDictionaryCopyDataOptional(event, kSecXPCData, &inputPacket, &error)) {
                        bool result = _SecOTRSessionProcessPacketRemote(sessionData, inputPacket, &outputSessionData, &outputPacket, &readyForMessages, &error);
                        if (result) {
                            SecXPCDictionarySetData(replyMessage, kSecXPCOTRSession, outputSessionData, &error);
                            SecXPCDictionarySetData(replyMessage, kSecXPCData, outputPacket, &error);
                            xpc_dictionary_set_bool(replyMessage, kSecXPCOTRReady, readyForMessages);
                            CFRelease(outputSessionData);
                            CFRelease(outputPacket);
                        }
                        xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, result);

                        CFReleaseSafe(inputPacket);
                    }
                    CFReleaseSafe(sessionData);
                }
                break;
            }
            case kSecXPCOpTryUserCredentials:
                with_label_and_password(event, ^(CFStringRef label, CFDataRef password) {
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                            SOSCCTryUserCredentials_Server(label, password, &error));
                });
                break;
            case kSecXPCOpSetUserCredentials:
                with_label_and_password(event, ^(CFStringRef label, CFDataRef password) {
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                            SOSCCSetUserCredentials_Server(label, password, &error));
                });
                break;
            case kSecXPCOpSetUserCredentialsAndDSID:
                    with_label_and_password_and_dsid(event, ^(CFStringRef label, CFDataRef password, CFStringRef dsid) {
                        xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                                SOSCCSetUserCredentialsAndDSID_Server(label, password, dsid, &error));
                    });
                    break;
            case kSecXPCOpView:
                with_label_and_number(event, ^(CFStringRef view, uint64_t actionCode) {
                    xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult,
                                            SOSCCView_Server(view, (SOSViewActionCode)actionCode, &error));
                });
                break;
            case kSecXPCOpViewSet:
                {
                    CFSetRef enabledViews = SecXPCSetCreateFromXPCDictionaryElement(event, kSecXPCKeyEnabledViewsKey);
                    CFSetRef disabledViews = SecXPCSetCreateFromXPCDictionaryElement(event, kSecXPCKeyDisabledViewsKey);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCViewSet_Server(enabledViews, disabledViews));
                    CFReleaseNull(enabledViews);
                    CFReleaseNull(disabledViews);
                }
                break;
            case kSecXPCOpSecurityProperty:
                with_label_and_number(event, ^(CFStringRef property, uint64_t actionCode) {
                    xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult,
                                             SOSCCSecurityProperty_Server(property, (SOSSecurityPropertyActionCode)actionCode, &error));
                });
                break;
            case kSecXPCOpCanAuthenticate:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCCanAuthenticate_Server(&error));
                break;
            case kSecXPCOpPurgeUserCredentials:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCPurgeUserCredentials_Server(&error));
                break;
            case kSecXPCOpDeviceInCircle:
                xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult,
                                         SOSCCThisDeviceIsInCircle_Server(&error));
                break;
            case kSecXPCOpRequestToJoin:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCRequestToJoinCircle_Server(&error));
                break;
            case kSecXPCOpRequestToJoinAfterRestore:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCRequestToJoinCircleAfterRestore_Server(&error));
                break;
            case kSecXPCOpRequestEnsureFreshParameters:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCRequestEnsureFreshParameters_Server(&error));
                break;
            case kSecXPCOpGetAllTheRings:
                {
                   CFStringRef ringDescriptions = SOSCCGetAllTheRings_Server(&error);
                    xpc_object_t xpc_dictionary = _CFXPCCreateXPCObjectFromCFObject(ringDescriptions);
                    xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_dictionary);
                    xpc_release(xpc_dictionary);
                }
                break;
            case kSecXPCOpApplyToARing:
                {
                    CFStringRef ringName = SecXPCDictionaryCopyString(event, kSecXPCKeyString, &error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCApplyToARing_Server(ringName, &error));
                    CFReleaseNull(ringName);
                }
                break;
            case kSecXPCOpWithdrawlFromARing:
                {
                    CFStringRef ringName = SecXPCDictionaryCopyString(event, kSecXPCKeyString, &error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCWithdrawlFromARing_Server(ringName, &error));
                    CFReleaseNull(ringName);
                }
                break;
            case kSecXPCOpRingStatus:
                {
                    CFStringRef ringName = SecXPCDictionaryCopyString(event, kSecXPCKeyString, &error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCRingStatus_Server(ringName, &error));
                    CFReleaseNull(ringName);
                }
                break;
            case kSecXPCOpEnableRing:
                {
                    CFStringRef ringName = SecXPCDictionaryCopyString(event, kSecXPCKeyString, &error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCEnableRing_Server(ringName, &error));
                    CFReleaseNull(ringName);
                }
                break;
            case kSecXPCOpRequestDeviceID:
                {
                    CFStringRef deviceID = SOSCCRequestDeviceID_Server(&error);
                    if (deviceID) {
                        SecXPCDictionarySetString(replyMessage, kSecXPCKeyResult, deviceID, &error);
                    }
                }
                break;
            case kSecXPCOpSetDeviceID:
                {
                    CFStringRef IDS = SecXPCDictionaryCopyString(event, kSecXPCKeyDeviceID, &error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCSetDeviceID_Server(IDS, &error));
                    CFReleaseNull(IDS);
                }
                break;
            case kSecXPCOpHandleIDSMessage:
                {
                    CFDictionaryRef IDS = SecXPCDictionaryCopyDictionary(event, kSecXPCKeyIDSMessage, &error);
                    xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult, SOSCCHandleIDSMessage_Server(IDS, &error));
                    CFReleaseNull(IDS);
                }
                break;

            case kSecXPCOpSendIDSMessage:
                {
                    CFStringRef message = SecXPCDictionaryCopyString(event, kSecXPCKeySendIDSMessage, &error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCIDSServiceRegistrationTest_Server(message, &error));
                    CFReleaseNull(message);
                }
                break;
            case kSecXPCOpPingTest:
                {
                    CFStringRef message = SecXPCDictionaryCopyString(event, kSecXPCKeySendIDSMessage, &error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCIDSPingTest_Server(message, &error));
                    CFReleaseNull(message);
                }
                break;
            case kSecXPCOpIDSDeviceID:
                {
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCIDSDeviceIDIsAvailableTest_Server(&error));
                }
                break;
			case kSecXPCOpAccountSetToNew:
				xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, SOSCCAccountSetToNew_Server(&error));
				break;
            case kSecXPCOpResetToOffering:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCResetToOffering_Server(&error));
                break;
            case kSecXPCOpResetToEmpty:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCResetToEmpty_Server(&error));
                break;
            case kSecXPCOpRemoveThisDeviceFromCircle:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCRemoveThisDeviceFromCircle_Server(&error));
                    break;
            case kSecXPCOpLoggedOutOfAccount:
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                            SOSCCLoggedOutOfAccount_Server(&error));
                    break;
            case kSecXPCOpBailFromCircle:
                {
                uint64_t limit_in_seconds = xpc_dictionary_get_uint64(event, kSecXPCLimitInMinutes);
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCBailFromCircle_Server(limit_in_seconds, &error));
                }
                break;
            case kSecXPCOpAcceptApplicants:
                {
                    xpc_object_t xapplicants = xpc_dictionary_get_value(event, kSecXPCKeyPeerInfos);
                    CFArrayRef applicants = CreateArrayOfPeerInfoWithXPCObject(xapplicants, &error); //(CFArrayRef)(_CFXPCCreateCFObjectFromXPCObject(xapplicants));
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                            (applicants && SOSCCAcceptApplicants_Server(applicants, &error)));
                    CFReleaseSafe(applicants);
                }
                break;
            case kSecXPCOpRejectApplicants:
                {
                    xpc_object_t xapplicants = xpc_dictionary_get_value(event, kSecXPCKeyPeerInfos);
                    CFArrayRef applicants = CreateArrayOfPeerInfoWithXPCObject(xapplicants, &error); //(CFArrayRef)(_CFXPCCreateCFObjectFromXPCObject(xapplicants));
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                            (applicants && SOSCCRejectApplicants_Server(applicants, &error)));
                    CFReleaseSafe(applicants);
                }
                break;
            case kSecXPCOpSetNewPublicBackupKey:
                {
                    CFDataRef publicBackupKey = SecXPCDictionaryCopyData(event, kSecXPCKeyNewPublicBackupKey, &error);
                    SOSPeerInfoRef peerInfo = SOSCCSetNewPublicBackupKey_Server(publicBackupKey, &error);
                    CFDataRef peerInfoData = peerInfo ? SOSPeerInfoCopyEncodedData(peerInfo, kCFAllocatorDefault, &error) : NULL;
                    CFReleaseNull(peerInfo);
                    if (peerInfoData) {
                        xpc_object_t xpc_object = _CFXPCCreateXPCObjectFromCFObject(peerInfoData);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_object);
                        xpc_release(xpc_object);
                    }
                    CFReleaseNull(peerInfoData);
                    CFReleaseSafe(publicBackupKey);

                }
                break;
            case kSecXPCOpSetBagForAllSlices:
                {
                    CFDataRef backupSlice = SecXPCDictionaryCopyData(event, kSecXPCKeyKeybag, &error);
                    bool includeV0 = xpc_dictionary_get_bool(event, kSecXPCKeyIncludeV0);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, backupSlice && SOSCCRegisterSingleRecoverySecret_Server(backupSlice, includeV0, &error));
                    CFReleaseSafe(backupSlice);
                }
                break;
            case kSecXPCOpCopyApplicantPeerInfo:
                {
                    CFArrayRef array = SOSCCCopyApplicantPeerInfo_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = CreateXPCObjectWithArrayOfPeerInfo(array, &error);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;
            case kSecXPCOpCopyValidPeerPeerInfo:
                {
                    CFArrayRef array = SOSCCCopyValidPeerPeerInfo_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = CreateXPCObjectWithArrayOfPeerInfo(array, &error);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;
            case kSecXPCOpValidateUserPublic:
                {
                    bool trusted = SOSCCValidateUserPublic_Server(&error);
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult, trusted);
                }
                break;
            case kSecXPCOpCopyNotValidPeerPeerInfo:
                {
                    CFArrayRef array = SOSCCCopyNotValidPeerPeerInfo_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = CreateXPCObjectWithArrayOfPeerInfo(array, &error);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;
            case kSecXPCOpCopyGenerationPeerInfo:
                {
                    CFArrayRef array = SOSCCCopyGenerationPeerInfo_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = _CFXPCCreateXPCObjectFromCFObject(array);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;
            case kSecXPCOpCopyRetirementPeerInfo:
                {
                    CFArrayRef array = SOSCCCopyRetirementPeerInfo_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = CreateXPCObjectWithArrayOfPeerInfo(array, &error);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;
            case kSecXPCOpCopyEngineState:
                {
                    CFArrayRef array = SOSCCCopyEngineState_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = _CFXPCCreateXPCObjectFromCFObject(array);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;
            case kSecXPCOpCopyPeerPeerInfo:
                {
                    CFArrayRef array = SOSCCCopyPeerPeerInfo_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = CreateXPCObjectWithArrayOfPeerInfo(array, &error);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;
            case kSecXPCOpCopyConcurringPeerPeerInfo:
                {
                    CFArrayRef array = SOSCCCopyConcurringPeerPeerInfo_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = CreateXPCObjectWithArrayOfPeerInfo(array, &error);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;
            case kSecXPCOpCopyMyPeerInfo:
                {
                    SOSPeerInfoRef peerInfo = SOSCCCopyMyPeerInfo_Server(&error);
                    CFDataRef peerInfoData = peerInfo ? SOSPeerInfoCopyEncodedData(peerInfo, kCFAllocatorDefault, &error) : NULL;
                    CFReleaseNull(peerInfo);
                    if (peerInfoData) {
                        xpc_object_t xpc_object = _CFXPCCreateXPCObjectFromCFObject(peerInfoData);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_object);
                        xpc_release(xpc_object);
                    }
                    CFReleaseNull(peerInfoData);
                }
                break;
            case kSecXPCOpGetLastDepartureReason:
                xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult,
                                         SOSCCGetLastDepartureReason_Server(&error));
                    break;
			case kSecXPCOpSetLastDepartureReason:
			{
				int32_t reason = (int32_t) xpc_dictionary_get_int64(event, kSecXPCKeyReason);
				xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult,
										 SOSCCSetLastDepartureReason_Server(reason, &error));
				break;
			}
            case kSecXPCOpProcessSyncWithAllPeers:
                xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult,
                                         SOSCCProcessSyncWithAllPeers_Server(&error));
                    break;
            case soscc_EnsurePeerRegistration_id:
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                             SOSCCProcessEnsurePeerRegistration_Server(&error));
                    break;
            case kSecXPCOpCopyIncompatibilityInfo: {
                CFStringRef iis = SOSCCCopyIncompatibilityInfo_Server(&error);
                SecXPCDictionarySetString(replyMessage, kSecXPCKeyResult, iis, &error);
                CFReleaseSafe(iis);
                break;
            }
			case kSecXPCOpOTAGetEscrowCertificates:
				{
                    uint32_t escrowRootType = (uint32_t)xpc_dictionary_get_uint64(event, "escrowType");
					CFArrayRef array = SecOTAPKICopyCurrentEscrowCertificates(escrowRootType, &error);
					if (array) {
						xpc_object_t xpc_array = _CFXPCCreateXPCObjectFromCFObject(array);
						xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
	                    xpc_release(xpc_array);
					}
					CFReleaseNull(array);
				}
				break;
			case kSecXPCOpOTAPKIGetNewAsset:
				 xpc_dictionary_set_int64(replyMessage, kSecXPCKeyResult,
	                                         SecOTAPKISignalNewAsset(&error));
				break;
            case kSecXPCOpRollKeys:
                {
                    bool force = xpc_dictionary_get_bool(event, "force");
                    xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                                 _SecServerRollKeys(force, &error));
                }
                break;
			case kSecXPCOpSetHSA2AutoAcceptInfo:
				{
					CFDataRef cfbytes = NULL;
					const uint8_t *bytes = NULL;
					size_t len = 0;

					bytes = xpc_dictionary_get_data(event,
							kSecXPCKeyHSA2AutoAcceptInfo, &len);
					if (!bytes) {
						SOSCreateError(kSOSErrorBadKey,
								CFSTR("missing autoaccept info"), NULL, &error);
						break;
					}

					cfbytes = CFDataCreate(NULL, bytes, len);
					if (!cfbytes) {
						SOSCreateError(kSOSErrorAllocationFailure,
								CFSTR("could not allocate autoaccept info"),
								NULL, &error);
						break;
					}

					xpc_dictionary_set_bool(replyMessage,
							kSecXPCKeyResult,
							SOSCCSetHSA2AutoAcceptInfo_Server(cfbytes, &error));
					CFRelease(cfbytes);
				}
				break;
            case kSecXPCOpWaitForInitialSync:
                xpc_dictionary_set_bool(replyMessage, kSecXPCKeyResult,
                                        SOSCCWaitForInitialSync_Server(&error));
               break;

            case kSecXPCOpCopyYetToSyncViews:
                {
                    CFArrayRef array = SOSCCCopyYetToSyncViewsList_Server(&error);
                    if (array) {
                        xpc_object_t xpc_array = _CFXPCCreateXPCObjectFromCFObject(array);
                        xpc_dictionary_set_value(replyMessage, kSecXPCKeyResult, xpc_array);
                        xpc_release(xpc_array);
                    }
                    CFReleaseNull(array);
                }
                break;

			default:
				break;
            }


        }

        if (error)
        {
            if(SecErrorGetOSStatus(error) == errSecItemNotFound || isSOSErrorCoded(error, kSOSErrorPublicKeyAbsent))
                secdebug("ipc", "%@ %@ %@", clientTask, SOSCCGetOperationDescription((enum SecXPCOperation)operation), error);
            else if (SecErrorGetOSStatus(error) == errSecAuthNeeded)
                secwarning("Authentication is needed %@ %@ %@", clientTask, SOSCCGetOperationDescription((enum SecXPCOperation)operation), error);
            else
                secerror("%@ %@ %@", clientTask, SOSCCGetOperationDescription((enum SecXPCOperation)operation), error);

            xpcError = SecCreateXPCObjectWithCFError(error);
            if (replyMessage) {
                xpc_dictionary_set_value(replyMessage, kSecXPCKeyError, xpcError);
            }
        } else if (replyMessage) {
            secdebug("ipc", "%@ %@ responding %@", clientTask, SOSCCGetOperationDescription((enum SecXPCOperation)operation), replyMessage);
        }
    } else {
        SecCFCreateErrorWithFormat(kSecXPCErrorUnexpectedType, sSecXPCErrorDomain, NULL, &error, 0, CFSTR("Messages expect to be xpc dictionary, got: %@"), event);
        secerror("%@: returning error: %@", clientTask, error);
        xpcError = SecCreateXPCObjectWithCFError(error);
        replyMessage = xpc_create_reply_with_format(event, "{%string: %value}", kSecXPCKeyError, xpcError);
    }

    if (replyMessage) {
        xpc_connection_send_message(connection, replyMessage);
        xpc_release(replyMessage);
    }
    if (xpcError)
        xpc_release(xpcError);
    CFReleaseSafe(error);
    CFReleaseSafe(accessGroups);
    CFReleaseSafe(domains);
    CFReleaseSafe(clientTask);
    CFReleaseSafe(clientAuditToken);
}

static void securityd_xpc_init(const char *service_name)
{
    secdebug("serverxpc", "start");
    xpc_connection_t listener = xpc_connection_create_mach_service(service_name, NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    if (!listener) {
        seccritical("security failed to register xpc listener for %s, exiting", service_name);
        abort();
    }

    xpc_connection_set_event_handler(listener, ^(xpc_object_t connection) {
        if (xpc_get_type(connection) == XPC_TYPE_CONNECTION) {
            xpc_connection_set_event_handler(connection, ^(xpc_object_t event) {
                if (xpc_get_type(event) == XPC_TYPE_DICTIONARY) {
                    xpc_retain(connection);
                    xpc_retain(event);
                    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                        securityd_xpc_dictionary_handler(connection, event);
                        xpc_release(event);
                        xpc_release(connection);
                    });
                }
            });
            xpc_connection_resume(connection);
        }
    });
    xpc_connection_resume(listener);
}



int main(int argc, char *argv[])
{
    char *wait4debugger = getenv("WAIT4DEBUGGER");
    if (wait4debugger && !strcasecmp("YES", wait4debugger)) {
		seccritical("SIGSTOPing self, awaiting debugger");
		kill(getpid(), SIGSTOP);
		asl_log(NULL, NULL, ASL_LEVEL_CRIT,
                "Again, for good luck (or bad debuggers)");
		kill(getpid(), SIGSTOP);
	}

    const char *serviceName = kSecuritydXPCServiceName;
#if TRUSTD_SERVER
    serviceName = kTrustdXPCServiceName;
    if (argc > 1 && (!strcmp(argv[1], "--agent"))) {
        serviceName = kTrustdAgentXPCServiceName;
    }
#endif
    securityd_init_server();
    securityd_xpc_init(serviceName);

    dispatch_main();

    return 0;
}

/* vi:set ts=4 sw=4 et: */
