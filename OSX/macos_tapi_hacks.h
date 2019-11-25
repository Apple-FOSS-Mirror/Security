/*
 * Copyright (c) 2017 Apple Inc. All Rights Reserved.
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
#ifndef macos_tapi_hack_h
#define macos_tapi_hack_h

// This file is to work around TAPI's insistence that every exported symbol is in a header file.
// The Security project just simply rejects such ideas, so this is the pressure valve:
//
// One-offs in header files that shouldn't be exported in the real-live macOS Security framework
// can be added here, and TAPI will accept them.
//
// Please don't add anything here.

#ifndef SECURITY_PROJECT_TAPI_HACKS
#error This header is not for inclusion; it's a nasty hack to get the macOS Security framework to build with TAPI.
#endif

#include <sqlite3.h>
#include <xpc/xpc.h>

CFDataRef SecDistinguishedNameCopyNormalizedContent(CFDataRef distinguished_name);
CFDataRef _SecItemCreatePersistentRef(CFTypeRef iclass, sqlite_int64 rowid, CFDictionaryRef attributes);
CFDictionaryRef SecTokenItemValueCopy(CFDataRef db_value, CFErrorRef *error);
CFArrayRef SecTrustCopyProperties_ios(SecTrustRef trust);
CFArrayRef SecItemCopyParentCertificates_ios(CFDataRef normalizedIssuer, CFArrayRef accessGroups, CFErrorRef *error);
bool SecItemCertificateExists(CFDataRef normalizedIssuer, CFDataRef serialNumber, CFArrayRef accessGroups, CFErrorRef *error);
bool _SecItemParsePersistentRef(CFDataRef persistent_ref, CFStringRef *return_class,
                                sqlite_int64 *return_rowid, CFDictionaryRef *return_token_attrs);

// iOS-only SecKey functions
size_t SecKeyGetSize(SecKeyRef key, int whichSize);
CFDataRef SecKeyCopyPublicKeyHash(SecKeyRef key);

// SecItemPriv.h
extern const CFStringRef kSecUseSystemKeychain;

// securityd_client.h

typedef struct SecurityClient {
} SecurityClient;

extern struct securityd *gSecurityd;
extern struct trustd *gTrustd;
extern SecurityClient * SecSecurityClientGet(void);
bool securityd_send_sync_and_do(enum SecXPCOperation op, CFErrorRef *error,
                                bool (^add_to_message)(xpc_object_t message, CFErrorRef* error),
                                bool (^handle_response)(xpc_object_t response, CFErrorRef* error));
XPC_RETURNS_RETAINED xpc_object_t securityd_message_with_reply_sync(xpc_object_t message, CFErrorRef *error);
XPC_RETURNS_RETAINED xpc_object_t securityd_create_message(enum SecXPCOperation op, CFErrorRef *error);
bool securityd_message_no_error(xpc_object_t message, CFErrorRef *error);

@interface SecuritydXPCClient : NSObject
@end

void SecAccessGroupsSetCurrent(CFArrayRef accessGroups);
CFArrayRef SecAccessGroupsGetCurrent(void);

// checkpw.c
int checkpw_internal( const struct passwd* pw, const char* password );

// SecFramework.h
CFDataRef SecDigestCreate(CFAllocatorRef allocator,
                          const SecAsn1Oid *algorithm, const SecAsn1Item *params,
                          const UInt8 *data, CFIndex length);
CFDataRef SecSHA256DigestCreateFromData(CFAllocatorRef allocator, CFDataRef data);
CFStringRef SecFrameworkCopyLocalizedString(CFStringRef key,
                                            CFStringRef tableName);

#endif /* macos_tapi_hack_h */
