/*
 * Copyright (c) 2017 Apple Inc.  All Rights Reserved.
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

#include <AssertMacros.h>
#include <sandbox.h>
#include <dirhelper_priv.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <notify.h>
#include <xpc/private.h>
#include <xpc/xpc.h>

#include <Security/SecuritydXPC.h>
#include <Security/SecTrustStore.h>
#include <Security/SecCertificateInternal.h>
#include <Security/SecEntitlements.h>
#include <Security/SecTrustInternal.h>
#include <Security/SecItem.h>
#include <Security/SecItemPriv.h>

#include <ipc/securityd_client.h>
#include <ipc/server_entitlement_helpers.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecDb.h>
#include <utilities/SecFileLocations.h>
#include <utilities/debugging.h>
#include <utilities/SecXPCError.h>
#include <securityd/SecTrustStoreServer.h>
#include <securityd/SecPinningDb.h>
#include <securityd/SecPolicyServer.h>
#include <securityd/SecRevocationDb.h>
#include <securityd/SecTrustServer.h>
#include <securityd/spi.h>

#if TARGET_OS_OSX
#include <Security/SecTaskPriv.h>
#include <login/SessionAgentStatusCom.h>
#include <trustd/macOS/SecTrustOSXEntryPoints.h>
#endif

#include "OTATrustUtilities.h"

static struct trustd trustd_spi = {
    .sec_trust_store_for_domain             = SecTrustStoreForDomainName,
    .sec_trust_store_contains               = SecTrustStoreContainsCertificateWithDigest,
    .sec_trust_store_set_trust_settings     = _SecTrustStoreSetTrustSettings,
    .sec_trust_store_remove_certificate     = SecTrustStoreRemoveCertificateWithDigest,
    .sec_truststore_remove_all              = _SecTrustStoreRemoveAll,
    .sec_trust_evaluate                     = SecTrustServerEvaluate,
    .sec_ota_pki_asset_version              = SecOTAPKIGetCurrentAssetVersion,
    .ota_CopyEscrowCertificates             = SecOTAPKICopyCurrentEscrowCertificates,
    .sec_ota_pki_get_new_asset              = SecOTAPKISignalNewAsset,
    .sec_trust_store_copy_all               = _SecTrustStoreCopyAll,
    .sec_trust_store_copy_usage_constraints = _SecTrustStoreCopyUsageConstraints,
};

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

// Returns error if entitlement isn't present.
static bool
EntitlementPresentAndTrue(uint64_t op, SecTaskRef clientTask, CFStringRef entitlement, CFErrorRef *error)
{
    if (!SecTaskGetBooleanValueForEntitlement(clientTask, entitlement)) {
        SecError(errSecMissingEntitlement, error, CFSTR("%@: %@ lacks entitlement %@"), SOSCCGetOperationDescription((enum SecXPCOperation)op), clientTask, entitlement);
        return false;
    }
    return true;
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

static bool SecXPCTrustStoreContains(xpc_object_t event, xpc_object_t reply, CFErrorRef *error) {
    bool result = false;
    SecTrustStoreRef ts = SecXPCDictionaryGetTrustStore(event, kSecXPCKeyDomain, error);
    if (ts) {
        CFDataRef digest = SecXPCDictionaryCopyData(event, kSecXPCKeyDigest, error);
        if (digest) {
            bool contains;
            if (SecTrustStoreContainsCertificateWithDigest(ts, digest, &contains, error)) {
                xpc_dictionary_set_bool(reply, kSecXPCKeyResult, contains);
                result = true;
            }
            CFReleaseNull(digest);
        }
    }
    return result;
}

static bool SecXPCTrustStoreSetTrustSettings(xpc_object_t event, xpc_object_t reply, CFErrorRef *error) {
    bool noError = false;
    SecTrustStoreRef ts = SecXPCDictionaryGetTrustStore(event, kSecXPCKeyDomain, error);
    if (ts) {
        SecCertificateRef certificate = SecXPCDictionaryCopyCertificate(event, kSecXPCKeyCertificate, error);
        if (certificate) {
            CFTypeRef trustSettingsDictOrArray = NULL;
            if (SecXPCDictionaryCopyPListOptional(event, kSecXPCKeySettings, &trustSettingsDictOrArray, error)) {
                bool result = _SecTrustStoreSetTrustSettings(ts, certificate, trustSettingsDictOrArray, error);
                xpc_dictionary_set_bool(reply, kSecXPCKeyResult, result);
                noError = true;
                CFReleaseSafe(trustSettingsDictOrArray);
            }
            CFReleaseNull(certificate);
        }
    }
    return noError;
}

static bool SecXPCTrustStoreRemoveCertificate(xpc_object_t event, xpc_object_t reply, CFErrorRef *error) {
    bool noError = false;
    SecTrustStoreRef ts = SecXPCDictionaryGetTrustStore(event, kSecXPCKeyDomain, error);
    if (ts) {
        CFDataRef digest = SecXPCDictionaryCopyData(event, kSecXPCKeyDigest, error);
        if (digest) {
            bool result = SecTrustStoreRemoveCertificateWithDigest(ts, digest, error);
            xpc_dictionary_set_bool(reply, kSecXPCKeyResult, result);
            noError = true;
            CFReleaseNull(digest);
        }
    }
    return noError;
}

static bool SecXPCTrustStoreCopyAll(xpc_object_t event, xpc_object_t reply, CFErrorRef *error) {
    bool result = false;
    SecTrustStoreRef ts = SecXPCDictionaryGetTrustStore(event, kSecXPCKeyDomain, error);
    if (ts) {
        CFArrayRef trustStoreContents = NULL;
        if(_SecTrustStoreCopyAll(ts, &trustStoreContents, error) && trustStoreContents) {
            SecXPCDictionarySetPList(reply, kSecXPCKeyResult, trustStoreContents, error);
            CFReleaseNull(trustStoreContents);
            result = true;
        }
    }
    return result;
}

static bool SecXPCTrustStoreCopyUsageConstraints(xpc_object_t event, xpc_object_t reply, CFErrorRef *error) {
    bool result = false;
    SecTrustStoreRef ts = SecXPCDictionaryGetTrustStore(event, kSecXPCKeyDomain, error);
    if (ts) {
        CFDataRef digest = SecXPCDictionaryCopyData(event, kSecXPCKeyDigest, error);
        if (digest) {
            CFArrayRef usageConstraints = NULL;
            if(_SecTrustStoreCopyUsageConstraints(ts, digest, &usageConstraints, error) && usageConstraints) {
                SecXPCDictionarySetPList(reply, kSecXPCKeyResult, usageConstraints, error);
                CFReleaseNull(usageConstraints);
                result = true;
            }
            CFReleaseNull(digest);
        }
    }
    return result;
}

static bool SecXPC_OTAPKI_GetAssetVersion(xpc_object_t __unused event, xpc_object_t reply, CFErrorRef *error) {
    xpc_dictionary_set_int64(reply, kSecXPCKeyResult, SecOTAPKIGetCurrentAssetVersion(error));
    return true;
}

static bool SecXPC_OTAPKI_GetEscrowCertificates(xpc_object_t event, xpc_object_t reply, CFErrorRef *error) {
    bool result = false;
    uint32_t escrowRootType = (uint32_t)xpc_dictionary_get_uint64(event, "escrowType");
    CFArrayRef array = SecOTAPKICopyCurrentEscrowCertificates(escrowRootType, error);
    if (array) {
        xpc_object_t xpc_array = _CFXPCCreateXPCObjectFromCFObject(array);
        xpc_dictionary_set_value(reply, kSecXPCKeyResult, xpc_array);
        xpc_release(xpc_array);
        result = true;
    }
    CFReleaseNull(array);
    return result;
}

static bool SecXPC_OTAPKI_GetNewAsset(xpc_object_t __unused event, xpc_object_t reply, CFErrorRef *error) {
    xpc_dictionary_set_int64(reply, kSecXPCKeyResult, SecOTAPKISignalNewAsset(error));
    return true;
}

typedef bool(*SecXPCOperationHandler)(xpc_object_t event, xpc_object_t reply, CFErrorRef *error);

typedef struct {
    CFStringRef entitlement;
    SecXPCOperationHandler handler;
} SecXPCServerOperation;

struct trustd_operations {
    SecXPCServerOperation trust_store_contains;
    SecXPCServerOperation trust_store_set_trust_settings;
    SecXPCServerOperation trust_store_remove_certificate;
    SecXPCServerOperation trust_store_copy_all;
    SecXPCServerOperation trust_store_copy_usage_constraints;
    SecXPCServerOperation ota_pki_asset_version;
    SecXPCServerOperation ota_pki_get_escrow_certs;
    SecXPCServerOperation ota_pki_get_new_asset;
};

static struct trustd_operations trustd_ops = {
    .trust_store_contains = { NULL, SecXPCTrustStoreContains },
    .trust_store_set_trust_settings = { kSecEntitlementModifyAnchorCertificates, SecXPCTrustStoreSetTrustSettings },
    .trust_store_remove_certificate = { kSecEntitlementModifyAnchorCertificates, SecXPCTrustStoreRemoveCertificate },
    .trust_store_copy_all = { kSecEntitlementModifyAnchorCertificates, SecXPCTrustStoreCopyAll },
    .trust_store_copy_usage_constraints = { kSecEntitlementModifyAnchorCertificates, SecXPCTrustStoreCopyUsageConstraints },
    .ota_pki_asset_version = { NULL, SecXPC_OTAPKI_GetAssetVersion },
    .ota_pki_get_escrow_certs = { NULL, SecXPC_OTAPKI_GetEscrowCertificates },
    .ota_pki_get_new_asset = { NULL, SecXPC_OTAPKI_GetNewAsset },
};

static void trustd_xpc_dictionary_handler(const xpc_connection_t connection, xpc_object_t event) {
    xpc_type_t type = xpc_get_type(event);
    __block CFErrorRef error = NULL;
    xpc_object_t xpcError = NULL;
    xpc_object_t replyMessage = NULL;
    CFDataRef  clientAuditToken = NULL;
    CFArrayRef domains = NULL;
    SecurityClient client = {
        .task = NULL,
        .accessGroups = NULL,
        .musr = NULL,
        .uid = xpc_connection_get_euid(connection),
        .allowSystemKeychain = true,
        .allowSyncBubbleKeychain = false,
        .isNetworkExtension = false,
        .canAccessNetworkExtensionAccessGroups = false,
#if TARGET_OS_IPHONE
        .inMultiUser = false,
#endif
    };

    secdebug("serverxpc", "entering");
    if (type == XPC_TYPE_DICTIONARY) {
        // TODO: Find out what we're dispatching.
        replyMessage = xpc_dictionary_create_reply(event);


        uint64_t operation = xpc_dictionary_get_uint64(event, kSecXPCKeyOperation);

        audit_token_t auditToken = {};
        xpc_connection_get_audit_token(connection, &auditToken);

        client.task = SecTaskCreateWithAuditToken(kCFAllocatorDefault, auditToken);
        clientAuditToken = CFDataCreate(kCFAllocatorDefault, (const UInt8*)&auditToken, sizeof(auditToken));
        client.accessGroups = SecTaskCopyAccessGroups(client.task);

        secinfo("serverxpc", "XPC [%@] operation: %@ (%" PRIu64 ")", client.task, SOSCCGetOperationDescription((enum SecXPCOperation)operation), operation);

        if (operation == sec_trust_evaluate_id) {
            CFArrayRef certificates = NULL, anchors = NULL, policies = NULL, responses = NULL, scts = NULL, trustedLogs = NULL, exceptions = NULL;
            bool anchorsOnly = xpc_dictionary_get_bool(event, kSecTrustAnchorsOnlyKey);
            bool keychainsAllowed = xpc_dictionary_get_bool(event, kSecTrustKeychainsAllowedKey);
            double verifyTime;
            if (SecXPCDictionaryCopyCertificates(event, kSecTrustCertificatesKey, &certificates, &error) &&
                SecXPCDictionaryCopyCertificatesOptional(event, kSecTrustAnchorsKey, &anchors, &error) &&
                SecXPCDictionaryCopyPoliciesOptional(event, kSecTrustPoliciesKey, &policies, &error) &&
                SecXPCDictionaryCopyCFDataArrayOptional(event, kSecTrustResponsesKey, &responses, &error) &&
                SecXPCDictionaryCopyCFDataArrayOptional(event, kSecTrustSCTsKey, &scts, &error) &&
                SecXPCDictionaryCopyArrayOptional(event, kSecTrustTrustedLogsKey, &trustedLogs, &error) &&
                SecXPCDictionaryGetDouble(event, kSecTrustVerifyDateKey, &verifyTime, &error) &&
                SecXPCDictionaryCopyArrayOptional(event, kSecTrustExceptionsKey, &exceptions, &error)) {
                // If we have no error yet, capture connection and reply in block and properly retain them.
                xpc_retain(connection);
                CFRetainSafe(client.task);
                CFRetainSafe(clientAuditToken);

                // Clear replyMessage so we don't send a synchronous reply.
                xpc_object_t asyncReply = replyMessage;
                replyMessage = NULL;

                SecTrustServerEvaluateBlock(clientAuditToken, certificates, anchors, anchorsOnly, keychainsAllowed, policies,
                                            responses, scts, trustedLogs, verifyTime, client.accessGroups, exceptions,
                                            ^(SecTrustResultType tr, CFArrayRef details, CFDictionaryRef info, SecCertificatePathRef chain,
                                              CFErrorRef replyError) {
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
                        secdebug("ipc", "%@ %@ %@", client.task, SOSCCGetOperationDescription((enum SecXPCOperation)operation), replyError);
                        xpc_object_t xpcReplyError = SecCreateXPCObjectWithCFError(replyError);
                        if (xpcReplyError) {
                            xpc_dictionary_set_value(asyncReply, kSecXPCKeyError, xpcReplyError);
                            xpc_release(xpcReplyError);
                        }
                        CFReleaseNull(replyError);
                    } else {
                        secdebug("ipc", "%@ %@ responding %@", client.task, SOSCCGetOperationDescription((enum SecXPCOperation)operation), asyncReply);
                    }

                    xpc_connection_send_message(connection, asyncReply);
                    xpc_release(asyncReply);
                    xpc_release(connection);
                    CFReleaseSafe(client.task);
                    CFReleaseSafe(clientAuditToken);
                });
            }
            CFReleaseSafe(policies);
            CFReleaseSafe(anchors);
            CFReleaseSafe(certificates);
            CFReleaseSafe(responses);
            CFReleaseSafe(scts);
            CFReleaseSafe(trustedLogs);
            CFReleaseSafe(exceptions);
        } else {
            SecXPCServerOperation *server_op = NULL;
            switch (operation) {
                case sec_trust_store_contains_id:
                    server_op = &trustd_ops.trust_store_contains;
                    break;
                case sec_trust_store_set_trust_settings_id:
                    server_op = &trustd_ops.trust_store_set_trust_settings;
                    break;
                case sec_trust_store_remove_certificate_id:
                    server_op = &trustd_ops.trust_store_remove_certificate;
                    break;
                case sec_trust_store_copy_all_id:
                    server_op = &trustd_ops.trust_store_copy_all;
                    break;
                case sec_trust_store_copy_usage_constraints_id:
                    server_op = &trustd_ops.trust_store_copy_usage_constraints;
                    break;
                case sec_ota_pki_asset_version_id:
                    server_op = &trustd_ops.ota_pki_asset_version;
                    break;
                case kSecXPCOpOTAGetEscrowCertificates:
                    server_op = &trustd_ops.ota_pki_get_escrow_certs;
                    break;
                case kSecXPCOpOTAPKIGetNewAsset:
                    server_op = &trustd_ops.ota_pki_get_new_asset;
                    break;
                default:
                    break;
            }
            if (server_op && server_op->handler) {
                bool entitled = true;
                if (server_op->entitlement) {
                    entitled = EntitlementPresentAndTrue(operation, client.task, server_op->entitlement, &error);
                }
                if (entitled) {
                    (void)server_op->handler(event, replyMessage, &error);
                }
            }
        }

        if (error)
        {
            if(SecErrorGetOSStatus(error) == errSecItemNotFound)
                secdebug("ipc", "%@ %@ %@", client.task, SOSCCGetOperationDescription((enum SecXPCOperation)operation), error);
            else if (SecErrorGetOSStatus(error) == errSecAuthNeeded)
                secwarning("Authentication is needed %@ %@ %@", client.task, SOSCCGetOperationDescription((enum SecXPCOperation)operation), error);
            else
                secerror("%@ %@ %@", client.task, SOSCCGetOperationDescription((enum SecXPCOperation)operation), error);

            xpcError = SecCreateXPCObjectWithCFError(error);
            if (replyMessage) {
                xpc_dictionary_set_value(replyMessage, kSecXPCKeyError, xpcError);
            }
        } else if (replyMessage) {
            secdebug("ipc", "%@ %@ responding %@", client.task, SOSCCGetOperationDescription((enum SecXPCOperation)operation), replyMessage);
        }
    } else {
        SecCFCreateErrorWithFormat(kSecXPCErrorUnexpectedType, sSecXPCErrorDomain, NULL, &error, 0, CFSTR("Messages expect to be xpc dictionary, got: %@"), event);
        secerror("%@: returning error: %@", client.task, error);
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
    CFReleaseSafe(client.accessGroups);
    CFReleaseSafe(client.musr);
    CFReleaseSafe(client.task);
    CFReleaseSafe(domains);
    CFReleaseSafe(clientAuditToken);
}

static void trustd_xpc_init(const char *service_name)
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
                        trustd_xpc_dictionary_handler(connection, event);
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

static void trustd_delete_old_files(void) {

#if TARGET_OS_EMBEDDED
    if (getuid() != 64) // _securityd
#else
    if (getuid() != 0)
#endif
    { return; }
    /* If we get past this line, then we can attempt to delete old revocation files;
       otherwise we won't have sufficient privilege. */

    /* We try to clean up after ourselves, but don't care if we succeed. */
    WithPathInRevocationInfoDirectory(CFSTR("update-current"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInRevocationInfoDirectory(CFSTR("update-full"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInRevocationInfoDirectory(CFSTR("update-full.gz"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
}

#if TARGET_OS_OSX
static void trustd_delete_old_caches(void) {
    /* We try to clean up after ourselves, but don't care if we succeed. */
    WithPathInKeychainDirectory(CFSTR("ocspcache.sqlite3"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInKeychainDirectory(CFSTR("ocspcache.sqlite3-wal"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInKeychainDirectory(CFSTR("ocspcache.sqlite3-shm"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInKeychainDirectory(CFSTR("ocspcache.sqlite3-journal"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInKeychainDirectory(CFSTR("caissuercache.sqlite3"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInKeychainDirectory(CFSTR("caissuercache.sqlite3-wal"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInKeychainDirectory(CFSTR("caissuercache.sqlite3-shm"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
    WithPathInKeychainDirectory(CFSTR("caissuercache.sqlite3-journal"), ^(const char *utf8String) {
        (void)remove(utf8String);
    });
}

static void trustd_sandbox(void) {
    char buf[PATH_MAX] = "";

    if (!_set_user_dir_suffix("com.apple.trustd") ||
        confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof(buf)) == 0 ||
        (mkdir(buf, 0700) && errno != EEXIST)) {
        secerror("failed to initialize temporary directory (%d): %s", errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    char *tempdir = realpath(buf, NULL);
    if (tempdir == NULL) {
        secerror("failed to resolve temporary directory (%d): %s", errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf)) == 0 ||
        (mkdir(buf, 0700) && errno != EEXIST)) {
        secerror("failed to initialize cache directory (%d): %s", errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    char *cachedir = realpath(buf, NULL);
    if (cachedir == NULL) {
        secerror("failed to resolve cache directory (%d): %s", errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    const char *parameters[] = {
        "_TMPDIR", tempdir,
        "_DARWIN_CACHE_DIR", cachedir,
        NULL
    };

    char *sberror = NULL;
    if (sandbox_init_with_parameters("com.apple.trustd", SANDBOX_NAMED, parameters, &sberror) != 0) {
        secerror("Failed to enter trustd sandbox: %{public}s", sberror);
        exit(EXIT_FAILURE);
    }

    free(tempdir);
    free(cachedir);
}
#else
static void trustd_sandbox(void) {
    char buf[PATH_MAX] = "";
    _set_user_dir_suffix("com.apple.trustd");
    confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof(buf));
}
#endif

int main(int argc, char *argv[])
{
    char *wait4debugger = getenv("WAIT4DEBUGGER");
    if (wait4debugger && !strcasecmp("YES", wait4debugger)) {
        seccritical("SIGSTOPing self, awaiting debugger");
        kill(getpid(), SIGSTOP);
        seccritical("Again, for good luck (or bad debuggers)");
        kill(getpid(), SIGSTOP);
    }

    /* <rdar://problem/15792007> Users with network home folders are unable to use/save password for Mail/Cal/Contacts/websites
     Our process doesn't realize DB connections get invalidated when network home directory users logout
     and their home gets unmounted. Exit our process and start fresh when user logs back in.
     */
#if TARGET_OS_OSX
    int sessionstatechanged_tok;
    notify_register_dispatch(kSA_SessionStateChangedNotification, &sessionstatechanged_tok, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^(int token __unused) {
        // we could be a process running as root.
        // However, since root never logs out this isn't an issue.
        if (SASSessionStateForUser(getuid()) == kSA_state_loggingout_pointofnoreturn) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3ull*NSEC_PER_SEC), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
                xpc_transaction_exit_clean();
            });
        }
    });
#endif

#if TARGET_OS_OSX
    /* Before we enter the sandbox, we need to delete the old caches kept in ~/Library/Keychains
     * After we enter the sandbox, we won't be able to access them. */
    trustd_delete_old_caches();
#endif
    /* Also clean up old files in /Library/Keychains/crls */
    trustd_delete_old_files();

    trustd_sandbox();

    const char *serviceName = kTrustdXPCServiceName;
    if (argc > 1 && (!strcmp(argv[1], "--agent"))) {
        serviceName = kTrustdAgentXPCServiceName;
    }

    /* set up SQLite before some other component has a chance to create a database connection */
    _SecDbServerSetup();

    /* set up revocation database if it doesn't already exist, or needs to be replaced */
    SecRevocationDbInitialize();

    gTrustd = &trustd_spi;
    SecPolicyServerInitialize();
    SecPinningDbInitialize();
#if TARGET_OS_OSX
    SecTrustLegacySourcesListenForKeychainEvents();
#endif
    trustd_xpc_init(serviceName);

    dispatch_main();
}
