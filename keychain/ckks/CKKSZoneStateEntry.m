/*
 * Copyright (c) 2016 Apple Inc. All Rights Reserved.
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

#import "CKKSKeychainView.h"

#include <utilities/SecDb.h>
#include <securityd/SecDbItem.h>
#include <securityd/SecItemSchema.h>

#if OCTAGON

#import <CloudKit/CloudKit.h>
#import "CKKSZoneStateEntry.h"
#import "keychain/ckks/CKKSRateLimiter.h"


@implementation CKKSZoneStateEntry

- (instancetype) initWithCKZone: (NSString*) ckzone zoneCreated: (bool) ckzonecreated zoneSubscribed: (bool) ckzonesubscribed changeToken: (NSData*) changetoken lastFetch: (NSDate*) lastFetch encodedRateLimiter: (NSData*) encodedRateLimiter {
    if(self = [super init]) {
        _ckzone = ckzone;
        _ckzonecreated = ckzonecreated;
        _ckzonesubscribed = ckzonesubscribed;
        _encodedChangeToken = changetoken;
        _lastFetchTime = lastFetch;

        self.encodedRateLimiter = encodedRateLimiter;
    }
    return self;
}

- (BOOL)isEqual: (id) object {
    if(![object isKindOfClass:[CKKSZoneStateEntry class]]) {
        return NO;
    }

    CKKSZoneStateEntry* obj = (CKKSZoneStateEntry*) object;

    return ([self.ckzone isEqualToString: obj.ckzone] &&
            self.ckzonecreated == obj.ckzonecreated &&
            self.ckzonesubscribed == obj.ckzonesubscribed &&
            ((self.encodedChangeToken == nil && obj.encodedChangeToken == nil) || [self.encodedChangeToken isEqual: obj.encodedChangeToken]) &&
            ((self.lastFetchTime == nil && obj.lastFetchTime == nil) || [self.lastFetchTime isEqualToDate: obj.lastFetchTime]) &&
            ((self.rateLimiter == nil && obj.rateLimiter == nil) || [self.rateLimiter isEqual: obj.rateLimiter])
           ) ? YES : NO;
}

+ (instancetype) state: (NSString*) ckzone {
    NSError* error = nil;
    CKKSZoneStateEntry* ret = [CKKSZoneStateEntry tryFromDatabase:ckzone error:&error];

    if(error) {
        secerror("CKKS: error fetching CKState(%@): %@", ckzone, error);
    }

    if(!ret) {
        ret = [[CKKSZoneStateEntry alloc] initWithCKZone: ckzone zoneCreated: false zoneSubscribed: false changeToken: nil lastFetch:nil encodedRateLimiter: nil];
    }
    return ret;
}

- (CKServerChangeToken*) getChangeToken {
    if(self.encodedChangeToken) {
        NSKeyedUnarchiver* unarchiver = [[NSKeyedUnarchiver alloc] initForReadingWithData:self.encodedChangeToken];
        unarchiver.requiresSecureCoding = YES;
        return [unarchiver decodeObjectOfClass:[CKServerChangeToken class] forKey:NSKeyedArchiveRootObjectKey];
    } else {
        return nil;
    }
}

- (void) setChangeToken: (CKServerChangeToken*) token {
    self.encodedChangeToken = token ? [NSKeyedArchiver archivedDataWithRootObject:token] : nil;
}

- (NSData*)encodedRateLimiter {
    if(self.rateLimiter == nil) {
        return nil;
    }
    return [NSKeyedArchiver archivedDataWithRootObject: self.rateLimiter];
}

- (void)setEncodedRateLimiter:(NSData *)encodedRateLimiter {
    if(encodedRateLimiter == nil) {
        self.rateLimiter = nil;
        return;
    }

    NSKeyedUnarchiver* unarchiver = [[NSKeyedUnarchiver alloc] initForReadingWithData:encodedRateLimiter];
    unarchiver.requiresSecureCoding = YES;
    self.rateLimiter = [unarchiver decodeObjectOfClass: [CKKSRateLimiter class] forKey:NSKeyedArchiveRootObjectKey];
}

#pragma mark - Database Operations

+ (instancetype) fromDatabase: (NSString*) ckzone error: (NSError * __autoreleasing *) error {
    return [self fromDatabaseWhere: @{@"ckzone": CKKSNilToNSNull(ckzone)} error: error];
}

+ (instancetype) tryFromDatabase: (NSString*) ckzone error: (NSError * __autoreleasing *) error {
    return [self tryFromDatabaseWhere: @{@"ckzone": CKKSNilToNSNull(ckzone)} error: error];
}

#pragma mark - CKKSSQLDatabaseObject methods

+ (NSString*) sqlTable {
    return @"ckstate";
}

+ (NSArray<NSString*>*) sqlColumns {
    return @[@"ckzone", @"ckzonecreated", @"ckzonesubscribed", @"changetoken", @"lastfetch", @"ratelimiter"];
}

- (NSDictionary<NSString*,NSString*>*) whereClauseToFindSelf {
    return @{@"ckzone": self.ckzone};
}

- (NSDictionary<NSString*,NSString*>*) sqlValues {
    NSISO8601DateFormatter* dateFormat = [[NSISO8601DateFormatter alloc] init];

    return @{@"ckzone": self.ckzone,
         @"ckzonecreated": [NSNumber numberWithBool:self.ckzonecreated],
         @"ckzonesubscribed": [NSNumber numberWithBool:self.ckzonesubscribed],
         @"changetoken": CKKSNilToNSNull([self.encodedChangeToken base64EncodedStringWithOptions:0]),
         @"lastfetch": CKKSNilToNSNull(self.lastFetchTime ? [dateFormat stringFromDate: self.lastFetchTime] : nil),
         @"ratelimiter": CKKSNilToNSNull([self.encodedRateLimiter base64EncodedStringWithOptions:0])
             };
}

+ (instancetype) fromDatabaseRow: (NSDictionary*) row {
    NSISO8601DateFormatter* dateFormat = [[NSISO8601DateFormatter alloc] init];

    return [[CKKSZoneStateEntry alloc] initWithCKZone: row[@"ckzone"]
                                          zoneCreated: [row[@"ckzonecreated"] boolValue]
                                       zoneSubscribed: [row[@"ckzonesubscribed"] boolValue]
                                          changeToken: ![row[@"changetoken"] isEqual: [NSNull null]] ?
                                                   [[NSData alloc] initWithBase64EncodedString: row[@"changetoken"] options:0] :
                                                   nil
                                            lastFetch: [row[@"lastfetch"] isEqual: [NSNull null]] ? nil : [dateFormat dateFromString: row[@"lastfetch"]]
                                   encodedRateLimiter: [row[@"ratelimiter"] isEqual: [NSNull null]] ? nil : [[NSData alloc] initWithBase64EncodedString: row[@"ratelimiter"] options:0]
            ];
}

@end

#endif
