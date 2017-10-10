/*
 * Copyright (c) 2007-2009,2012-2015 Apple Inc. All Rights Reserved.
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
#ifndef	_SECURITYD_CLIENT_H_
#define _SECURITYD_CLIENT_H_

#include <stdint.h>

#include <securityd/SecKeybagSupport.h>

#include <Security/SecTrust.h>
#include <Security/SecTask.h>
#ifndef MINIMIZE_INCLUDES
# include <Security/SecTrustStore.h>
# include <Security/SecCertificatePath.h>
#else
typedef struct __SecTrustStore *SecTrustStoreRef;
# ifndef _SECURITY_SECCERTIFICATE_H_
typedef struct __SecCertificate *SecCertificateRef;
# endif // _SECURITY_SECCERTIFICATE_H_
# ifndef _SECURITY_SECCERTIFICATEPATH_H_
typedef struct SecCertificatePath *SecCertificatePathRef;
# endif // _SECURITY_SECCERTIFICATEPATH_H_
#endif // MINIMIZE_INCLUDES

#if TARGET_HAS_KEYSTORE
#include <libaks.h>
#endif

#include <CoreFoundation/CFArray.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFError.h>

#include <Security/SecureObjectSync/SOSCloudCircle.h>
#include <Security/SecureObjectSync/SOSPeerInfo.h>
#include <Security/SecureObjectSync/SOSRing.h>

#include <xpc/xpc.h>
#include <CoreFoundation/CFXPCBridge.h>

#include <TargetConditionals.h>

#if TARGET_OS_OSX
#define kSecuritydXPCServiceName "com.apple.securityd.xpc"
#define kSecuritydSystemXPCServiceName "com.apple.securityd.system.xpc"
#define kTrustdAgentXPCServiceName "com.apple.trustd.agent"
#define kTrustdXPCServiceName "com.apple.trustd"
#else
#define kSecuritydXPCServiceName "com.apple.securityd"
#define kTrustdAgentXPCServiceName "com.apple.trustd"
#define kTrustdXPCServiceName "com.apple.trustd"
#endif // *** END TARGET_OS_OSX ***

//
// MARK: XPC Information.
//

extern CFStringRef sSecXPCErrorDomain;

extern const char *kSecXPCKeyOperation;
extern const char *kSecXPCKeyResult;
extern const char *kSecXPCKeyEndpoint;
extern const char *kSecXPCKeyError;
extern const char *kSecXPCKeyPeerInfoArray;
extern const char *kSecXPCKeyUserLabel;
extern const char *kSecXPCKeyBackup;
extern const char *kSecXPCKeyKeybag;
extern const char *kSecXPCKeyUserPassword;
extern const char *kSecXPCKeyDSID;
extern const char *kSecXPCKeyViewName;
extern const char *kSecXPCKeyViewActionCode;
extern const char *kSecXPCKeyNewPublicBackupKey;
extern const char *kSecXPCKeyRecoveryPublicKey;
extern const char *kSecXPCKeyIncludeV0;
extern const char *kSecXPCKeyEnabledViewsKey;
extern const char *kSecXPCKeyDisabledViewsKey;
extern const char *kSecXPCKeyEscrowLabel;
extern const char *kSecXPCKeyTriesLabel;
extern const char *kSecXPCKeyFileDescriptor;
extern const char *kSecXPCKeyAccessGroups;
extern const char *kSecXPCKeyClasses;
extern const char *kSecXPCKeyNormalizedIssuer;
extern const char *kSecXPCKeySerialNumber;
extern const char *kSecXPCKeyBackupKeybagIdentifier;
extern const char *kSecXPCKeyBackupKeybagPath;

//
// MARK: Dispatch macros
//

#define SECURITYD_XPC(sdp, wrapper, ...) ((gSecurityd && gSecurityd->sdp) ? gSecurityd->sdp(__VA_ARGS__) : wrapper(sdp ## _id, __VA_ARGS__))
#define TRUSTD_XPC(sdp, wrapper, ...) ((gTrustd && gTrustd->sdp) ? gTrustd->sdp(__VA_ARGS__) : wrapper(sdp ## _id, __VA_ARGS__))

//
// MARK: Object to XPC format conversion.
//


//
// MARK: XPC Interfaces
//

extern const char *kSecXPCKeyOperation;
extern const char *kSecXPCKeyResult;
extern const char *kSecXPCKeyError;
extern const char *kSecXPCKeyPeerInfoArray;
extern const char *kSecXPCKeyPeerInfo;
extern const char *kSecXPCKeyUserLabel;
extern const char *kSecXPCKeyUserPassword;
extern const char *kSecXPCKeyDSID;
extern const char *kSecXPCLimitInMinutes;
extern const char *kSecXPCKeyQuery;
extern const char *kSecXPCKeyAttributesToUpdate;
extern const char *kSecXPCKeyDomain;
extern const char *kSecXPCKeyDigest;
extern const char *kSecXPCKeyCertificate;
extern const char *kSecXPCKeySettings;
extern const char *kSecXPCPublicPeerId; // Public peer id
extern const char *kSecXPCOTRSession; // OTR session bytes
extern const char *kSecXPCData; // Data to process
extern const char *kSecXPCOTRReady; // OTR ready for messages
extern const char *kSecXPCKeyDeviceID;
extern const char *kSecXPCKeyIDSMessage;
extern const char *kSecXPCKeyViewName;
extern const char *kSecXPCKeyViewActionCode;
extern const char *kSecXPCKeySendIDSMessage;
extern const char *kSecXPCKeyHSA2AutoAcceptInfo;
extern const char *kSecXPCKeyEscrowLabel;
extern const char *kSecXPCKeyTriesLabel;
extern const char *kSecXPCKeyString;
extern const char *kSecXPCKeyArray;
extern const char *kSecXPCKeySet;
extern const char *kSecXPCKeySet2;
extern const char *kSecXPCVersion;

extern const char *kSecXPCKeyReason;

//
// MARK: Mach port request IDs
//
enum SecXPCOperation {
    sec_item_add_id = 0,
    sec_item_copy_matching_id = 1,
    sec_item_update_id = 2,
    sec_item_delete_id = 3,
    // trust_store_for_domain -- NOT an ipc
    sec_trust_store_contains_id = 4,
    sec_trust_store_set_trust_settings_id = 5,
    sec_trust_store_remove_certificate_id = 6,
    // remove_all -- NOT an ipc
    sec_delete_all_id = 7,
    sec_trust_evaluate_id = 8,
    // Any new items MUST be added below here
    // This allows updating roots on a device, since SecTrustEvaluate must continue to work
    sec_keychain_backup_id,
    sec_keychain_restore_id,
    sec_keychain_backup_syncable_id,
    sec_keychain_restore_syncable_id,
    sec_item_backup_copy_names_id,
    sec_item_backup_handoff_fd_id,
    sec_item_backup_set_confirmed_manifest_id,
    sec_item_backup_restore_id,
    sec_keychain_sync_update_message_id,
    sec_ota_pki_asset_version_id,
    sec_otr_session_create_remote_id,
    sec_otr_session_process_packet_remote_id,
	kSecXPCOpOTAPKIGetNewAsset,
	kSecXPCOpOTAGetEscrowCertificates,
    kSecXPCOpProcessUnlockNotification,
    kSecXPCOpProcessSyncWithAllPeers,
    kSecXPCOpRollKeys,
    sec_add_shared_web_credential_id,
    sec_copy_shared_web_credential_id,
    sec_get_log_settings_id,
    sec_set_xpc_log_settings_id,
    sec_set_circle_log_settings_id,
    soscc_EnsurePeerRegistration_id,
    kSecXPCOpRequestEnsureFreshParameters,
    kSecXPCOpGetAllTheRings,
    kSecXPCOpApplyToARing,
    kSecXPCOpWithdrawlFromARing,
    kSecXPCOpEnableRing,
    kSecXPCOpRingStatus,
    kSecXPCOpRequestDeviceID,
    kSecXPCOpSetDeviceID,
    kSecXPCOpHandleIDSMessage,
    kSecXPCOpSyncWithKVSPeer,
    kSecXPCOpSyncWithIDSPeer,
    kSecXPCOpSendIDSMessage,
    kSecXPCOpPingTest,
    kSecXPCOpIDSDeviceID,
    kSecXPCOpSyncWithKVSPeerIDOnly,
    // any process using an operation below here is required to have entitlement keychain-cloud-circle
    kSecXPCOpTryUserCredentials,
    kSecXPCOpSetUserCredentials,
    kSecXPCOpSetUserCredentialsAndDSID,
    kSecXPCOpCanAuthenticate,
    kSecXPCOpPurgeUserCredentials,
    kSecXPCOpDeviceInCircle,
    kSecXPCOpRequestToJoin,
    kSecXPCOpRequestToJoinAfterRestore,
    kSecXPCOpResetToOffering,
    kSecXPCOpResetToEmpty,
    kSecXPCOpView,
    kSecXPCOpViewSet,
    kSecXPCOpSecurityProperty,
    kSecXPCOpRemoveThisDeviceFromCircle,
    kSecXPCOpRemovePeersFromCircle,
    kSecXPCOpLoggedOutOfAccount,
    kSecXPCOpBailFromCircle,
    kSecXPCOpAcceptApplicants,
    kSecXPCOpRejectApplicants,
    kSecXPCOpCopyApplicantPeerInfo,
    kSecXPCOpCopyValidPeerPeerInfo,
    kSecXPCOpValidateUserPublic,
    kSecXPCOpCopyNotValidPeerPeerInfo,
    kSecXPCOpCopyPeerPeerInfo,
    kSecXPCOpCopyConcurringPeerPeerInfo,
    kSecXPCOpCopyGenerationPeerInfo,
    kSecXPCOpGetLastDepartureReason,
    kSecXPCOpSetLastDepartureReason,
    kSecXPCOpCopyIncompatibilityInfo,
    kSecXPCOpCopyRetirementPeerInfo,
    kSecXPCOpCopyViewUnawarePeerInfo,
    kSecXPCOpCopyEngineState,
    kSecXPCOpCopyMyPeerInfo,
	kSecXPCOpAccountSetToNew,
    kSecXPCOpSetNewPublicBackupKey,
    kSecXPCOpSetBagForAllSlices,
    kSecXPCOpWaitForInitialSync,
    kSecXPCOpCopyYetToSyncViews,
    kSecXPCOpSetEscrowRecord,
    kSecXPCOpGetEscrowRecord,
    kSecXPCOpCheckPeerAvailability,
    kSecXPCOpCopyAccountData,
    kSecXPCOpDeleteAccountData,
    kSecXPCOpCopyEngineData,
    kSecXPCOpDeleteEngineData,
    kSecXPCOpCopyApplication,
    kSecXPCOpCopyCircleJoiningBlob,
    kSecXPCOpJoinWithCircleJoiningBlob,
    kSecXPCOpKVSKeyCleanup,
    kSecXPCOpPopulateKVS,
    kSecXPCOpAccountHasPublicKey,
    kSecXPCOpAccountIsNew,
    kSecXPCOpClearKVSPeerMessage,
    kSecXPCOpRegisterRecoveryPublicKey,
    kSecXPCOpGetRecoveryPublicKey,
    kSecXPCOpCopyBackupInformation,
    kSecXPCOpCopyInitialSyncBlob,
    /* after this is free for all */
    kSecXPCOpWhoAmI,
    kSecXPCOpTransmogrifyToSyncBubble,
    kSecXPCOpTransmogrifyToSystemKeychain,
    kSecXPCOpWrapToBackupSliceKeyBagForView,
    sec_item_update_token_items_id,
    kSecXPCOpDeleteUserView,
    sec_trust_store_copy_all_id,
    sec_trust_store_copy_usage_constraints_id,
    sec_delete_items_with_access_groups_id,
    kSecXPCOpIsThisDeviceLastBackup,
    sec_keychain_backup_keybag_uuid_id,
    kSecXPCOpPeersHaveViewsEnabled,
    kSecXPCOpProcessSyncWithPeers,
    kSecXPCOpMessageFromPeerIsPending,
    kSecXPCOpSendToPeerIsPending,
    sec_item_copy_parent_certificates_id,
    sec_item_certificate_exists_id,
    kSecXPCOpCKKSEndpoint,
    kSecXPCOpSOSEndpoint,
    kSecXPCOpSecuritydXPCServerEndpoint,
    kSecXPCOpBackupKeybagAdd,
    kSecXPCOpBackupKeybagDelete,
};


typedef struct SecurityClient {
    SecTaskRef task;
    CFArrayRef accessGroups;
    bool allowSystemKeychain;
    bool allowSyncBubbleKeychain;
    bool isNetworkExtension;
    bool canAccessNetworkExtensionAccessGroups;
    uid_t uid;
    CFDataRef musr;
#if TARGET_OS_EMBEDDED && TARGET_HAS_KEYSTORE
    keybag_handle_t keybag;
#endif
#if TARGET_OS_IPHONE
    bool inMultiUser;
    int activeUser;
#endif
} SecurityClient;


extern SecurityClient * SecSecurityClientGet(void);
#if TARGET_OS_IOS
void SecSecuritySetMusrMode(bool mode, uid_t uid, int activeUser);
#endif

struct securityd {
    bool (*sec_item_add)(CFDictionaryRef attributes, SecurityClient *client, CFTypeRef *result, CFErrorRef* error);
    bool (*sec_item_copy_matching)(CFDictionaryRef query, SecurityClient *client, CFTypeRef *result, CFErrorRef* error);
    bool (*sec_item_update)(CFDictionaryRef query, CFDictionaryRef attributesToUpdate, SecurityClient *client, CFErrorRef* error);
    bool (*sec_item_delete)(CFDictionaryRef query, SecurityClient *client, CFErrorRef* error);
    bool (*sec_add_shared_web_credential)(CFDictionaryRef attributes, SecurityClient *client, const audit_token_t *clientAuditToken, CFStringRef appID, CFArrayRef accessGroups, CFTypeRef *result, CFErrorRef *error);
    bool (*sec_copy_shared_web_credential)(CFDictionaryRef query, SecurityClient *client, const audit_token_t *clientAuditToken, CFStringRef appID, CFArrayRef accessGroups, CFTypeRef *result, CFErrorRef *error);
    bool (*sec_item_delete_all)(CFErrorRef* error);
    CFArrayRef (*sec_item_copy_parent_certificates)(CFDataRef normalizedIssuer, CFArrayRef accessGroups, CFErrorRef *error);
    bool (*sec_item_certificate_exists)(CFDataRef normalizedIssuer, CFDataRef serialNumber, CFArrayRef accessGroups, CFErrorRef *error);
    CFDataRef (*sec_keychain_backup)(SecurityClient *client, CFDataRef keybag, CFDataRef passcode, CFErrorRef* error);
    bool (*sec_keychain_restore)(CFDataRef backup, SecurityClient *client, CFDataRef keybag, CFDataRef passcode, CFErrorRef* error);
    CFDictionaryRef (*sec_keychain_backup_syncable)(CFDictionaryRef backup_in, CFDataRef keybag, CFDataRef passcode, CFErrorRef* error);
    bool (*sec_keychain_restore_syncable)(CFDictionaryRef backup, CFDataRef keybag, CFDataRef passcode, CFErrorRef* error);
    CFArrayRef (*sec_item_backup_copy_names)(CFErrorRef *error);
    int (*sec_item_backup_handoff_fd)(CFStringRef backupName, CFErrorRef *error);
    bool (*sec_item_backup_set_confirmed_manifest)(CFStringRef backupName, CFDataRef keybagDigest, CFDataRef manifest, CFErrorRef *error);
    bool (*sec_item_backup_restore)(CFStringRef backupName, CFStringRef peerID, CFDataRef keybag, CFDataRef secret, CFDataRef backup, CFErrorRef *error);
    CFDataRef (*sec_otr_session_create_remote)(CFDataRef publicPeerId, CFErrorRef* error);
    bool (*sec_otr_session_process_packet_remote)(CFDataRef sessionData, CFDataRef inputPacket, CFDataRef* outputSessionData, CFDataRef* outputPacket, bool *readyForMessages, CFErrorRef* error);
    bool (*soscc_TryUserCredentials)(CFStringRef user_label, CFDataRef user_password, CFErrorRef *error);
    bool (*soscc_SetUserCredentials)(CFStringRef user_label, CFDataRef user_password, CFErrorRef *error);
    bool (*soscc_SetUserCredentialsAndDSID)(CFStringRef user_label, CFDataRef user_password, CFStringRef dsid, CFErrorRef *error);
    bool (*soscc_CanAuthenticate)(CFErrorRef *error);
    bool (*soscc_PurgeUserCredentials)(CFErrorRef *error);
    SOSCCStatus (*soscc_ThisDeviceIsInCircle)(CFErrorRef* error);
    bool (*soscc_RequestToJoinCircle)(CFErrorRef* error);
    bool (*soscc_RequestToJoinCircleAfterRestore)(CFErrorRef* error);
    bool (*soscc_RequestEnsureFreshParameters)(CFErrorRef* error);
    CFStringRef (*soscc_GetAllTheRings)(CFErrorRef *error);
    bool (*soscc_ApplyToARing)(CFStringRef ringName, CFErrorRef* error);
    bool (*soscc_WithdrawlFromARing)(CFStringRef ringName, CFErrorRef* error);
    bool (*soscc_EnableRing)(CFStringRef ringName, CFErrorRef* error);
    SOSRingStatus (*soscc_RingStatus)(CFStringRef ringName, CFErrorRef* error);
    CFStringRef (*soscc_CopyDeviceID)(CFErrorRef* error);
    bool (*soscc_SetDeviceID)(CFStringRef IDS, CFErrorRef *error);
    HandleIDSMessageReason (*soscc_HandleIDSMessage)(CFDictionaryRef IDS, CFErrorRef *error);
    bool (*soscc_CheckIDSRegistration)(CFStringRef message, CFErrorRef *error);
    bool (*soscc_PingTest)(CFStringRef message, CFErrorRef *error);
    bool (*soscc_GetIDSIDFromIDS)(CFErrorRef *error);
    bool (*soscc_SetToNew)(CFErrorRef *error);
    bool (*soscc_ResetToOffering)(CFErrorRef* error);
    bool (*soscc_ResetToEmpty)(CFErrorRef* error);
    SOSViewResultCode (*soscc_View)(CFStringRef view, SOSViewActionCode action, CFErrorRef *error);
    bool (*soscc_ViewSet)(CFSetRef enabledViews, CFSetRef disabledViews);
    SOSSecurityPropertyResultCode (*soscc_SecurityProperty)(CFStringRef property, SOSSecurityPropertyActionCode action, CFErrorRef *error);
    bool (*soscc_RegisterSingleRecoverySecret)(CFDataRef backupSlice, bool forV0Only, CFErrorRef *error);
    bool (*soscc_RegisterRecoveryPublicKey)(CFDataRef recovery_key, CFErrorRef *error);
    CFDataRef (*soscc_CopyRecoveryPublicKey)(CFErrorRef *error);
    bool (*soscc_RemoveThisDeviceFromCircle)(CFErrorRef* error);
    bool (*soscc_RemovePeersFromCircle)(CFArrayRef peers, CFErrorRef* error);
    bool (*soscc_LoggedOutOfAccount)(CFErrorRef* error);
    bool (*soscc_BailFromCircle)(uint64_t limit_in_seconds, CFErrorRef* error);
    bool (*soscc_AcceptApplicants)(CFArrayRef applicants, CFErrorRef* error);
    bool (*soscc_RejectApplicants)(CFArrayRef applicants, CFErrorRef* error);
    SOSPeerInfoRef (*soscc_SetNewPublicBackupKey)(CFDataRef pubKey, CFErrorRef *error);
    bool (*soscc_ValidateUserPublic)(CFErrorRef* error);
    CFArrayRef (*soscc_CopyGenerationPeerInfo)(CFErrorRef* error);
    CFArrayRef (*soscc_CopyApplicantPeerInfo)(CFErrorRef* error);
    CFArrayRef (*soscc_CopyValidPeerPeerInfo)(CFErrorRef* error);
    CFArrayRef (*soscc_CopyNotValidPeerPeerInfo)(CFErrorRef* error);
    CFArrayRef (*soscc_CopyRetirementPeerInfo)(CFErrorRef* error);
    CFArrayRef (*soscc_CopyViewUnawarePeerInfo)(CFErrorRef* error);
    CFArrayRef (*soscc_CopyEngineState)(CFErrorRef* error);
    // Not sure why these are below the last entry in the enum order above, but they are:
    CFArrayRef (*soscc_CopyPeerInfo)(CFErrorRef* error);
    CFArrayRef (*soscc_CopyConcurringPeerInfo)(CFErrorRef* error);
    CFStringRef (*soscc_CopyIncompatibilityInfo)(CFErrorRef* error);
    enum DepartureReason (*soscc_GetLastDepartureReason)(CFErrorRef* error);
    bool (*soscc_SetLastDepartureReason)(enum DepartureReason, CFErrorRef* error);
    CFSetRef (*soscc_ProcessSyncWithPeers)(CFSetRef peerIDs, CFSetRef backupPeerIDs, CFErrorRef* error);
    SyncWithAllPeersReason (*soscc_ProcessSyncWithAllPeers)(CFErrorRef* error);
    bool (*soscc_EnsurePeerRegistration)(CFErrorRef* error);
    bool (*sec_roll_keys)(bool force, CFErrorRef* error);
    CFArrayRef (*sec_keychain_sync_update_message)(CFDictionaryRef update, CFErrorRef *error);
    CFPropertyListRef   (*sec_get_log_settings)(CFErrorRef* error);
    bool   (*sec_set_xpc_log_settings)(CFTypeRef type, CFErrorRef* error);
    bool   (*sec_set_circle_log_settings)(CFTypeRef type, CFErrorRef* error);
    SOSPeerInfoRef (*soscc_CopyMyPeerInfo)(CFErrorRef*);
    bool (*soscc_WaitForInitialSync)(CFErrorRef*);
    CFArrayRef (*soscc_CopyYetToSyncViewsList)(CFErrorRef*);
    bool (*soscc_SetEscrowRecords)(CFStringRef escrow_label, uint64_t tries, CFErrorRef *error);
    CFDictionaryRef (*soscc_CopyEscrowRecords)(CFErrorRef *error);
    CFDictionaryRef (*soscc_CopyBackupInformation)(CFErrorRef *error);
    bool (*soscc_PeerAvailability)(CFErrorRef *error);
    bool (*sosbskb_WrapToBackupSliceKeyBagForView)(CFStringRef viewName, CFDataRef input, CFDataRef* output, CFDataRef* bskbEncoded, CFErrorRef* error);
    CFDataRef (*soscc_CopyAccountState)(CFErrorRef *error);
    bool (*soscc_DeleteAccountState)(CFErrorRef *error);
    CFDataRef (*soscc_CopyEngineData)(CFErrorRef *error);
    bool (*soscc_DeleteEngineState)(CFErrorRef *error);
    SOSPeerInfoRef (*soscc_CopyApplicant)(CFErrorRef *error);
    CFDataRef (*soscc_CopyCircleJoiningBlob)(SOSPeerInfoRef applicant, CFErrorRef *error);
    CFDataRef (*soscc_CopyInitialSyncData)(CFErrorRef *error);
    bool (*soscc_JoinWithCircleJoiningBlob)(CFDataRef joiningBlob, PiggyBackProtocolVersion version, CFErrorRef *error);
    bool (*soscc_SOSCCCleanupKVSKeys)(CFErrorRef *error);
    bool (*soscc_SOSCCTestPopulateKVSWithBadKeys)(CFErrorRef *error);
    bool (*soscc_AccountHasPublicKey)(CFErrorRef *error);
    bool (*soscc_AccountIsNew)(CFErrorRef *error);
    bool (*sec_item_update_token_items)(CFStringRef tokenID, CFArrayRef query, SecurityClient *client, CFErrorRef* error);
    bool (*sec_delete_items_with_access_groups)(CFArrayRef bundleIDs, SecurityClient *client, CFErrorRef *error);
    bool (*soscc_IsThisDeviceLastBackup)(CFErrorRef *error);
    bool (*soscc_requestSyncWithPeerOverKVS)(CFStringRef peerID, CFDataRef message, CFErrorRef *error);
    bool (*soscc_requestSyncWithPeerOverIDS)(CFStringRef peerID, CFErrorRef *error);
    CFBooleanRef (*soscc_SOSCCPeersHaveViewsEnabled)(CFArrayRef views, CFErrorRef *error);
    bool (*socc_clearPeerMessageKeyInKVS)(CFStringRef peerID, CFErrorRef *error);
    bool (*soscc_requestSyncWithPeerOverKVSIDOnly)(CFStringRef peerID, CFErrorRef *error);
    bool (*soscc_SOSCCMessageFromPeerIsPending)(SOSPeerInfoRef peer, CFErrorRef* error);
    bool (*soscc_SOSCCSendToPeerIsPending)(SOSPeerInfoRef peer, CFErrorRef* error);
    CFTypeRef (*soscc_status)(void);
    CFTypeRef secd_xpc_server;
};

extern struct securityd *gSecurityd;

struct trustd {
    SecTrustStoreRef (*sec_trust_store_for_domain)(CFStringRef domainName, CFErrorRef* error);
    bool (*sec_trust_store_contains)(SecTrustStoreRef ts, CFDataRef digest, bool *contains, CFErrorRef* error);
    bool (*sec_trust_store_set_trust_settings)(SecTrustStoreRef ts, SecCertificateRef certificate, CFTypeRef trustSettingsDictOrArray, CFErrorRef* error);
    bool (*sec_trust_store_remove_certificate)(SecTrustStoreRef ts, CFDataRef digest, CFErrorRef* error);
    bool (*sec_truststore_remove_all)(SecTrustStoreRef ts, CFErrorRef* error);
    SecTrustResultType (*sec_trust_evaluate)(CFArrayRef certificates, CFArrayRef anchors, bool anchorsOnly, bool keychainsAllowed, CFArrayRef policies, CFArrayRef responses, CFArrayRef SCTs, CFArrayRef trustedLogs, CFAbsoluteTime verifyTime, __unused CFArrayRef accessGroups, CFArrayRef exceptions, CFArrayRef *details, CFDictionaryRef *info, SecCertificatePathRef *chain, CFErrorRef *error);
    int (*sec_ota_pki_asset_version)(CFErrorRef* error);
    CFArrayRef (*ota_CopyEscrowCertificates)(uint32_t escrowRootType, CFErrorRef* error);
    int (*sec_ota_pki_get_new_asset)(CFErrorRef* error);
    bool (*sec_trust_store_copy_all)(SecTrustStoreRef ts, CFArrayRef *trustStoreContents, CFErrorRef *error);
    bool (*sec_trust_store_copy_usage_constraints)(SecTrustStoreRef ts, CFDataRef digest, CFArrayRef *usageConstraints, CFErrorRef *error);
};

extern struct trustd *gTrustd;

CFArrayRef SecAccessGroupsGetCurrent(void);

// TODO Rename me
CFStringRef SOSCCGetOperationDescription(enum SecXPCOperation op);
XPC_RETURNS_RETAINED xpc_object_t securityd_message_with_reply_sync(xpc_object_t message, CFErrorRef *error);
XPC_RETURNS_RETAINED xpc_object_t securityd_create_message(enum SecXPCOperation op, CFErrorRef *error);
bool securityd_message_no_error(xpc_object_t message, CFErrorRef *error);


bool securityd_send_sync_and_do(enum SecXPCOperation op, CFErrorRef *error,
                                bool (^add_to_message)(xpc_object_t message, CFErrorRef* error),
                                bool (^handle_response)(xpc_object_t response, CFErrorRef* error));

// For testing only, never call this in a threaded program!
void SecServerSetMachServiceName(const char *name);

XPC_RETURNS_RETAINED xpc_endpoint_t _SecSecuritydCopyEndpoint(enum SecXPCOperation op, CFErrorRef *error);

#if __OBJC__
#import <Foundation/Foundation.h>
#import <Foundation/NSXPCConnection.h>
typedef void (^SecBoolNSErrorCallback) (bool, NSError*);

@protocol SecuritydXPCCallbackProtocol
- (void)callCallback: (bool) result error:(NSError*) error;
@end

@protocol SecuritydXPCProtocol
- (void) SecItemAddAndNotifyOnSync:(NSDictionary*) attributes
                      syncCallback:(id<SecuritydXPCCallbackProtocol>) callback
                          complete:(void (^) (NSDictionary* opDictResult, NSArray* opArrayResult, NSError* operror)) complete;

// For the given item (specified exactly by its hash (currently SHA1)), attempt to set the CloudKit 'current' pointer
// to point to the given item.
// This can fail if:
//    1. your knowledge of the old current item is out of date
//    2. either the new item or old item has changed (checked by hash)
//    3. If this device can't talk with CloudKit for any reason
- (void)secItemSetCurrentItemAcrossAllDevices:(NSData*)newItemPersistentRef
                           newCurrentItemHash:(NSData*)newItemSHA1
                                  accessGroup:(NSString*)accessGroup
                                   identifier:(NSString*)identifier
                                     viewHint:(NSString*)viewHint
                      oldCurrentItemReference:(NSData*)oldCurrentItemPersistentRef
                           oldCurrentItemHash:(NSData*)oldItemSHA1
                                     complete:(void (^) (NSError* operror)) complete;

// For the given access group and identifier, check the current local idea of the 'current' item
-(void)secItemFetchCurrentItemAcrossAllDevices:(NSString*)accessGroup
                                    identifier:(NSString*)identifier
                                      viewHint:(NSString*)viewHint
                               fetchCloudValue:(bool)fetchCloudValue
                                      complete:(void (^) (NSData* persistentref, NSError* operror)) complete;


// For each item in the keychainClass, return a persistant reference and the digest of the value
// The digest is not stable, and can change any time, the only promise is that if the digest
// value didn't change, the item didn't change. If digest change, the value MIGHT have changed,/
// but it could also just have stayed the same.
// The this interface bypass SEP/AKS and for that reason is a higher performance then SecItemCopyMatching().
- (void) secItemDigest:(NSString *)keychainClass
           accessGroup:(NSString *)accessGroup
              complete:(void (^)(NSArray<NSDictionary *> *digest, NSError* error))complete;
@end

// Call this to receive a proxy object conforming to SecuritydXPCProtocol that you can call methods on.
// It's probably a remote object for securityd/secd, but it might be in-process if you've configured it that way.
id<SecuritydXPCProtocol> SecuritydXPCProxyObject(void (^rpcErrorHandler)(NSError *));

// Set up a local securityxpcserver: after this call, all securitydxpc calls will be handled in-process instead of actually transferring to securityd
id<SecuritydXPCProtocol> SecCreateLocalSecuritydXPCServer(void);

// Make a SecBoolNSErrorCallback block into an Objective-C object (for proxying across NSXPC)
@interface SecuritydXPCCallback : NSObject <SecuritydXPCCallbackProtocol> {
    SecBoolNSErrorCallback _callback;
}
@property SecBoolNSErrorCallback callback;
- (instancetype)initWithCallback: (SecBoolNSErrorCallback) callback;
@end

@interface SecuritydXPCClient : NSObject {
    NSXPCConnection* _connection;
}
@property NSXPCConnection* connection;

+(void)configureSecuritydXPCProtocol: (NSXPCInterface*) interface;
@end

#endif // OBJC

#endif /* _SECURITYD_CLIENT_H_ */
