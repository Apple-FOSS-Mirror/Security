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

#if OCTAGON

#import "keychain/ckks/CKKSKeychainView.h"
#import "keychain/ckks/CKKSNearFutureScheduler.h"
#import "keychain/ckks/CKKSScanLocalItemsOperation.h"
#import "keychain/ckks/CKKSMirrorEntry.h"
#import "keychain/ckks/CKKSOutgoingQueueEntry.h"
#import "keychain/ckks/CKKSGroupOperation.h"
#import "keychain/ckks/CKKSKey.h"
#import "keychain/ckks/CKKSViewManager.h"
#import "keychain/ckks/CKKSManifest.h"

#include <securityd/SecItemSchema.h>
#include <securityd/SecItemServer.h>
#include <securityd/SecItemDb.h>
#include <Security/SecItemPriv.h>

@interface CKKSScanLocalItemsOperation ()
@property CKOperationGroup* ckoperationGroup;
@end

@implementation CKKSScanLocalItemsOperation

- (instancetype)init {
    return nil;
}
- (instancetype)initWithCKKSKeychainView:(CKKSKeychainView*)ckks ckoperationGroup:(CKOperationGroup*)ckoperationGroup {
    if(self = [super init]) {
        _ckks = ckks;
        _ckoperationGroup = ckoperationGroup;
        _recordsFound = 0;
        _recordsAdded = 0;
    }
    return self;
}

- (void) main {
    // Take a strong reference.
    CKKSKeychainView* ckks = self.ckks;
    if(!ckks) {
        ckkserror("ckksscan", ckks, "no CKKS object");
        return;
    }

    [ckks dispatchSyncWithAccountKeys: ^bool{
        if(self.cancelled) {
            ckksnotice("ckksscan", ckks, "CKKSScanLocalItemsOperation cancelled, quitting");
            return false;
        }
        ckks.lastScanLocalItemsOperation = self;

        NSMutableArray* itemsForManifest = [NSMutableArray array];

        // First, query for all synchronizable items
        __block CFErrorRef cferror = NULL;
        __block NSError* error = nil;
        __block bool newEntries = false;
        
        // Must query per-class, so:
        const SecDbSchema *newSchema = current_schema();
        for (const SecDbClass *const *class = newSchema->classes; *class != NULL; class++) {
            cferror = NULL;

            if(!((*class)->itemclass)) {
                // Don't try to scan non-item 'classes'
                continue;
            }

            NSDictionary* queryAttributes = @{(__bridge NSString*) kSecClass: (__bridge NSString*) (*class)->name,
                                              (__bridge NSString*) kSecReturnRef: @(YES),
                                              (__bridge NSString*) kSecAttrSynchronizable: @(YES),
                                              (__bridge NSString*) kSecAttrTombstone: @(NO),
                                              // This works ~as long as~ item views are chosen by view hint only. It's a significant perf win, though.
                                              // <rdar://problem/32269541> SpinTracer: CKKSScanLocalItemsOperation expensive on M8 machines
                                              (__bridge NSString*) kSecAttrSyncViewHint: ckks.zoneName,
                                              };
            ckksinfo("ckksscan", ckks, "Scanning all synchronizable items for: %@", queryAttributes);

            Query *q = query_create_with_limit( (__bridge CFDictionaryRef) queryAttributes, NULL, kSecMatchUnlimited, &cferror);
            bool ok = false;

            if(cferror) {
                ckkserror("ckksscan", ckks, "couldn't create query: %@", cferror);
                SecTranslateError(&error, cferror);
                self.error = error;
                continue;
            }

            ok = kc_with_dbt(true, &cferror, ^(SecDbConnectionRef dbt) {
                return SecDbItemQuery(q, NULL, dbt, &cferror, ^(SecDbItemRef item, bool *stop) {
                    ckksnotice("ckksscan", ckks, "scanning item: %@", item);

                    SecDbItemRef itemToSave = NULL;

                    // First check: is this a tombstone? If so, skip with prejudice.
                    if(SecDbItemIsTombstone(item)) {
                        ckksinfo("ckksscan", ckks, "Skipping tombstone %@", item);
                        return;
                    }

                    // Second check: is this item even for this view? If not, skip.
                    NSString* viewForItem = [[CKKSViewManager manager] viewNameForItem:item];
                    if(![viewForItem isEqualToString: ckks.zoneName]) {
                        ckksinfo("ckksscan", ckks, "Scanned item is for view %@, skipping", viewForItem);
                        return;
                    }

                    // Third check: is this item one of our keys for a view? If not, skip.
                    if([CKKSKey isItemKeyForKeychainView: item] != nil) {
                        ckksinfo("ckksscan", ckks, "Scanned item is a CKKS internal key, skipping");
                        return;
                    }

                    // Fourth check: does this item have a UUID? If not, ONBOARD!
                    NSString* uuid = (__bridge_transfer NSString*) CFRetain(SecDbItemGetValue(item, &v10itemuuid, &cferror));
                    if(!uuid || [uuid isEqual: [NSNull null]]) {
                        ckksnotice("ckksscan", ckks, "making new UUID for item %@", item);

                        uuid = [[NSUUID UUID] UUIDString];
                        NSDictionary* updates = @{(id) kSecAttrUUID: uuid};

                        SecDbItemRef new_item = SecDbItemCopyWithUpdates(item, (__bridge CFDictionaryRef) updates, &cferror);
                        if(SecErrorGetOSStatus(cferror) != errSecSuccess) {
                            ckkserror("ckksscan", ckks, "couldn't update item with new UUID: %@", cferror);
                            SecTranslateError(&error, cferror);
                            self.error = error;
                            CFReleaseNull(new_item);
                            return;
                        }

                        if (new_item) {
                            bool ok = kc_transaction_type(dbt, kSecDbExclusiveRemoteCKKSTransactionType, &cferror, ^{
                                return SecDbItemUpdate(item, new_item, dbt, kCFBooleanFalse, q->q_uuid_from_primary_key, &cferror);
                            });

                            if(!ok || SecErrorGetOSStatus(cferror) != errSecSuccess) {
                                ckkserror("ckksscan", ckks, "couldn't update item with new UUID: %@", cferror);
                                SecTranslateError(&error, cferror);
                                self.error = error;
                                CFReleaseNull(new_item);
                                return;
                            }
                        }
                        itemToSave = CFRetainSafe(new_item);
                        CFReleaseNull(new_item);

                    } else {
                        // Is there a known sync item with this UUID?
                        CKKSMirrorEntry* ckme = [CKKSMirrorEntry tryFromDatabase: uuid zoneID:ckks.zoneID error: &error];
                        if(ckme != nil) {
                            if ([CKKSManifest shouldSyncManifests]) {
                                [itemsForManifest addObject:ckme.item];
                            }
                            ckksinfo("ckksscan", ckks, "Existing mirror entry with UUID %@", uuid);
                            return;
                        }

                        // We don't care about the oqe state here, just that one exists
                        CKKSOutgoingQueueEntry* oqe = [CKKSOutgoingQueueEntry tryFromDatabase: uuid zoneID:ckks.zoneID error: &error];
                        if(oqe != nil) {
                            ckksinfo("ckksscan", ckks, "Existing outgoing queue entry with UUID %@", uuid);
                            // If its state is 'new', mark down that we've seen new entries that need processing
                            newEntries |= !![oqe.state isEqualToString: SecCKKSStateNew];
                            return;
                        }

                        itemToSave = CFRetainSafe(item);
                    }

                    // Hurray, we can help!
                    self.recordsFound += 1;

                    CKKSOutgoingQueueEntry* oqe = [CKKSOutgoingQueueEntry withItem: itemToSave action: SecCKKSActionAdd ckks:ckks error: &error];

                    if(error) {
                        ckkserror("ckksscan", ckks, "Need to upload %@, but can't create outgoing entry: %@", item, error);
                        self.error = error;
                        CFReleaseNull(itemToSave);
                        return;
                    }

                    ckksnotice("ckksscan", ckks, "Syncing new item: %@", oqe);
                    CFReleaseNull(itemToSave);

                    [oqe saveToDatabase: &error];
                    if(error) {
                        ckkserror("ckksscan", ckks, "Need to upload %@, but can't save to database: %@", oqe, error);
                        self.error = error;
                        return;
                    }
                    newEntries = true;
                    if ([CKKSManifest shouldSyncManifests]) {
                        [itemsForManifest addObject:oqe.item];
                    }

                    self.recordsAdded += 1;
                });
            });

            if(cferror || !ok) {
                ckkserror("ckksscan", ckks, "error processing or finding items: %@", cferror);
                SecTranslateError(&error, cferror);
                self.error = error;
                query_destroy(q, NULL);
                continue;
            }

            ok = query_notify_and_destroy(q, ok, &cferror);

            if(cferror || !ok) {
                ckkserror("ckksscan", ckks, "couldn't delete query: %@", cferror);
                SecTranslateError(&error, cferror);
                self.error = error;
                continue;
            }
        }
        
        if ([CKKSManifest shouldSyncManifests]) {
            // TODO: this manifest needs to incorporate peer manifests
            CKKSEgoManifest* manifest = [CKKSEgoManifest newManifestForZone:ckks.zoneName withItems:itemsForManifest peerManifestIDs:@[] currentItems:@{} error:&error];
            if (!manifest || error) {
                ckkserror("ckksscan", ckks, "could not create manifest: %@", error);
                self.error = error;
                return false;
            }

            [manifest saveToDatabase:&error];
            if (error) {
                ckkserror("ckksscan", ckks, "could not save manifest to database: %@", error);
                self.error = error;
                return false;
            }

            ckks.egoManifest = manifest;
        }

        if(newEntries) {
            // Schedule a "view changed" notification
            [ckks.notifyViewChangedScheduler trigger];

            // notify CKKS that it should process these new entries
            [ckks processOutgoingQueue:self.ckoperationGroup];
        }

        return true;
    }];
}

@end;

#endif
