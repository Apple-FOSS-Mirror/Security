/*
 * Copyright (c) 2018 Apple Inc. All Rights Reserved.
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
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#include <notify.h>
#include <Security/Security.h>
#include <Security/SecTrustSettingsPriv.h>
#include <Security/SecPolicyPriv.h>
#include <utilities/SecFileLocations.h>
#include <utilities/SecCFWrappers.h>
#import "OTATrustUtilities.h"
#include "SecTrustStoreServer.h"

typedef bool(*exceptionsArrayValueChecker)(id _Nonnull obj);

static bool checkDomainsValuesCompliance(id _Nonnull obj) {
    if (![obj isKindOfClass:[NSString class]]) {
        return false;
    }
    if (SecDNSIsTLD((__bridge CFStringRef)obj)) {
        return false;
    }
    return true;
}

static bool checkCAsValuesCompliance(id _Nonnull obj) {
    if (![obj isKindOfClass:[NSDictionary class]]) {
        return false;
    }
    if (2 != [(NSDictionary*)obj count]) {
        return false;
    }
    if (nil == ((NSDictionary*)obj)[(__bridge NSString*)kSecCTExceptionsHashAlgorithmKey] ||
        nil == ((NSDictionary*)obj)[(__bridge NSString*)kSecCTExceptionsSPKIHashKey]) {
        return false;
    }
    if (![((NSDictionary*)obj)[(__bridge NSString*)kSecCTExceptionsHashAlgorithmKey] isKindOfClass:[NSString class]] ||
        ![((NSDictionary*)obj)[(__bridge NSString*)kSecCTExceptionsSPKIHashKey] isKindOfClass:[NSData class]]) {
        return false;
    }
    if (![((NSDictionary*)obj)[(__bridge NSString*)kSecCTExceptionsHashAlgorithmKey] isEqualToString:@"sha256"]) {
        return false;
    }
    return true;
}

static bool checkExceptionsValues(NSString *key, id value, exceptionsArrayValueChecker checker, CFErrorRef *error) {
    if (![value isKindOfClass:[NSArray class]]) {
        return SecError(errSecParam, error, CFSTR("value for %@ is not an array in exceptions dictionary"), key);
    }

    __block bool result = true;
    [(NSArray*)value enumerateObjectsUsingBlock:^(id  _Nonnull obj, NSUInteger idx, BOOL * _Nonnull stop) {
        if (!checker(obj)) {
            result = SecError(errSecParam, error, CFSTR("value %lu for %@ is not the expected type"), (unsigned long)idx, key);
            *stop = true;
        }
    }];
    return result;
}

static bool checkInputExceptionsAndSetAppExceptions(NSDictionary *inExceptions, NSMutableDictionary *appExceptions, CFErrorRef *error) {
    __block bool result = true;
    [inExceptions enumerateKeysAndObjectsUsingBlock:^(NSString *_Nonnull key, id  _Nonnull obj, BOOL * _Nonnull stop) {
        if ([key isEqualToString:(__bridge NSString*)kSecCTExceptionsDomainsKey]) {
            if (!checkExceptionsValues(key, obj, checkDomainsValuesCompliance, error)) {
                *stop = YES;
                result = false;
                return;
            }
        } else if ([key isEqualToString:(__bridge NSString*)kSecCTExceptionsCAsKey]) {
            if (!checkExceptionsValues(key, obj, checkCAsValuesCompliance, error)) {
                *stop = YES;
                result = false;
                return;
            }
        } else {
            result = SecError(errSecParam, error, CFSTR("unknown key (%@) in exceptions dictionary"), key);
            *stop = YES;
            result = false;
            return;
        }
        if ([(NSArray*)obj count] == 0) {
            [appExceptions removeObjectForKey:key];
        } else {
            appExceptions[key] = obj;
        }
    }];
    return result;
}

static _Atomic bool gHasCTExceptions = false;
#define kSecCTExceptionsChanged "com.apple.trustd.ct.exceptions-changed"

bool _SecTrustStoreSetCTExceptions(CFStringRef appID, CFDictionaryRef exceptions, CFErrorRef *error)  {
    if (!SecOTAPKIIsSystemTrustd()) {
        return SecError(errSecWrPerm, error, CFSTR("Unable to write CT exceptions from user agent"));
    }

    if (!appID) {
        return SecError(errSecParam, error, CFSTR("application-identifier required to set exceptions"));
    }

    @autoreleasepool {
#if TARGET_OS_IPHONE
        NSURL *keychainsDirectory = CFBridgingRelease(SecCopyURLForFileInKeychainDirectory(nil));
#else
        NSURL *keychainsDirectory = [NSURL fileURLWithFileSystemRepresentation:"/Library/Keychains/" isDirectory:YES relativeToURL:nil];
#endif
        NSURL *ctExceptionsFile = [keychainsDirectory URLByAppendingPathComponent:@"CTExceptions.plist"];
        NSMutableDictionary *allExceptions = [NSMutableDictionary dictionaryWithContentsOfURL:ctExceptionsFile];
        NSMutableDictionary *appExceptions = NULL;
        if (allExceptions && allExceptions[(__bridge NSString*)appID]) {
            appExceptions = [allExceptions[(__bridge NSString*)appID] mutableCopy];
        } else {
            appExceptions = [NSMutableDictionary dictionary];
            if (!allExceptions) {
                allExceptions =  [NSMutableDictionary dictionary];
            }
        }

        if (exceptions && (CFDictionaryGetCount(exceptions) > 0)) {
            NSDictionary *inExceptions = (__bridge NSDictionary*)exceptions;
            if (!checkInputExceptionsAndSetAppExceptions(inExceptions, appExceptions, error)) {
                return false;
            }
        }

        if (!exceptions || [appExceptions count] == 0) {
            [allExceptions removeObjectForKey:(__bridge NSString*)appID];
        } else {
            allExceptions[(__bridge NSString*)appID] = appExceptions;
        }

        NSError *nserror = nil;
        if (![allExceptions writeToURL:ctExceptionsFile error:&nserror] && error) {
            *error = CFRetainSafe((__bridge CFErrorRef)nserror);
            return false;
        }
        atomic_store(&gHasCTExceptions, [allExceptions count] != 0);
        notify_post(kSecCTExceptionsChanged);
        return true;
    }
}

CFDictionaryRef _SecTrustStoreCopyCTExceptions(CFStringRef appID, CFErrorRef *error) {
    @autoreleasepool {
        static int notify_token = 0;
        int check = 0;
        if (!SecOTAPKIIsSystemTrustd()) {
            /* Check whether we got a notification. If we didn't, and there are no exceptions set, return NULL.
             * Otherwise, we need to read from disk */
            uint32_t check_status = notify_check(notify_token, &check);
            if (check_status == NOTIFY_STATUS_OK && check == 0 && !atomic_load(&gHasCTExceptions)) {
                return NULL;
            }
        } else {
            if (!atomic_load(&gHasCTExceptions)) {
                return NULL;
            }
        }

#if TARGET_OS_IPHONE
        NSURL *keychainsDirectory = CFBridgingRelease(SecCopyURLForFileInKeychainDirectory(nil));
#else
        NSURL *keychainsDirectory = [NSURL fileURLWithFileSystemRepresentation:"/Library/Keychains/" isDirectory:YES relativeToURL:nil];
#endif
        NSURL *ctExceptionsFile = [keychainsDirectory URLByAppendingPathComponent:@"CTExceptions.plist"];
        NSDictionary <NSString*,NSDictionary*> *allExceptions = [NSDictionary dictionaryWithContentsOfURL:ctExceptionsFile];

        /* Set us up for not reading the disk when there are never exceptions */
        static dispatch_once_t onceToken;
        dispatch_once(&onceToken, ^{
            atomic_init(&gHasCTExceptions, allExceptions && [allExceptions count] == 0);
            if (!SecOTAPKIIsSystemTrustd()) {
                uint32_t status = notify_register_check(kSecCTExceptionsChanged, &notify_token);
                if (status == NOTIFY_STATUS_OK) {
                    status = notify_check(notify_token, NULL);
                }
                if (status != NOTIFY_STATUS_OK) {
                    secerror("failed to establish notification for CT exceptions: %ud", status);
                    notify_cancel(notify_token);
                    notify_token = 0;
                }
            }
        });

        if (!allExceptions || [allExceptions count] == 0) {
            atomic_store(&gHasCTExceptions, false);
            return NULL;
        }

        /* If the caller specified an appID, return only the exceptions for that appID */
        if (appID) {
            return CFBridgingRetain(allExceptions[(__bridge NSString*)appID]);
        }

        /* Otherwise, combine all the exceptions into one array */
        NSMutableArray *domainExceptions = [NSMutableArray array];
        NSMutableArray *caExceptions = [NSMutableArray array];
        [allExceptions enumerateKeysAndObjectsUsingBlock:^(NSString * _Nonnull __unused key, NSDictionary * _Nonnull appExceptions,
                                                           BOOL * _Nonnull __unused stop) {
            if (appExceptions[(__bridge NSString*)kSecCTExceptionsDomainsKey] &&
                checkExceptionsValues((__bridge NSString*)kSecCTExceptionsDomainsKey, appExceptions[(__bridge NSString*)kSecCTExceptionsDomainsKey],
                                      checkDomainsValuesCompliance, error)) {
                [domainExceptions addObjectsFromArray:appExceptions[(__bridge NSString*)kSecCTExceptionsDomainsKey]];
            }
            if (appExceptions[(__bridge NSString*)kSecCTExceptionsCAsKey] &&
                checkExceptionsValues((__bridge NSString*)kSecCTExceptionsCAsKey, appExceptions[(__bridge NSString*)kSecCTExceptionsCAsKey],
                                      checkCAsValuesCompliance, error)) {
                [caExceptions addObjectsFromArray:appExceptions[(__bridge NSString*)kSecCTExceptionsCAsKey]];
            }
        }];
        NSMutableDictionary *exceptions = [NSMutableDictionary dictionaryWithCapacity:2];
        if ([domainExceptions count] > 0) {
            exceptions[(__bridge NSString*)kSecCTExceptionsDomainsKey] = domainExceptions;
        }
        if ([caExceptions count] > 0) {
            exceptions[(__bridge NSString*)kSecCTExceptionsCAsKey] = caExceptions;
        }
        if ([exceptions count] > 0) {
            atomic_store(&gHasCTExceptions, true);
            return CFBridgingRetain(exceptions);
        }
        return NULL;
    }
}
