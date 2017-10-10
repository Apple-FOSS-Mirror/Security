//
//  CKDKVSStore.m
//  Security
//
//  Created by Mitch Adler on 5/15/16.
//
//

#import "CKDKVSStore.h"
#import "CKDKVSProxy.h"

#include "SOSCloudKeychainConstants.h"
#include <utilities/debugging.h>

#import <Foundation/NSUbiquitousKeyValueStore.h>
#import <Foundation/NSUbiquitousKeyValueStore_Private.h>
#import "SyncedDefaults/SYDConstants.h"
#include <os/activity.h>

struct CKDKVSCounters {
    uint64_t synchronize;
    uint64_t synchronizeWithCompletionHandler;
    uint64_t incomingMessages;
    uint64_t outgoingMessages;
    uint64_t totalWaittimeSynchronize;
    uint64_t longestWaittimeSynchronize;
    uint64_t synchronizeFailures;
};



@interface CKDKVSStore ()
@property (readwrite, weak) UbiqitousKVSProxy* proxy;
@property (readwrite) NSUbiquitousKeyValueStore* cloudStore;
@property (assign,readwrite) struct CKDKVSCounters *perfCounters;
@property dispatch_queue_t perfQueue;
@end

@implementation CKDKVSStore

+ (instancetype)kvsInterface {
    return [[CKDKVSStore alloc] init];
}

- (instancetype)init {
    self = [super init];

    self->_cloudStore = [NSUbiquitousKeyValueStore defaultStore];
    self->_proxy = nil;

    if (!self.cloudStore) {
        secerror("NO NSUbiquitousKeyValueStore defaultStore!!!");
        return nil;
    }
    self.perfQueue = dispatch_queue_create("CKDKVSStorePerfQueue", NULL);
    self.perfCounters = calloc(1, sizeof(struct CKDKVSCounters));
    

    return self;
}

- (void)dealloc {
    if (_perfCounters)
        free(_perfCounters);
}

- (void) connectToProxy: (UbiqitousKVSProxy*) proxy {
    _proxy = proxy;

    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(kvsStoreChangedAsync:)
                                                 name:NSUbiquitousKeyValueStoreDidChangeExternallyNotification
                                               object:nil];

}

- (void)setObject:(id)obj forKey:(NSString*)key {
    secdebug("kvsdebug", "%@ key %@ set to: %@ from: %@", self, key, obj, [self.cloudStore objectForKey:key]);
    [self.cloudStore setObject:obj forKey:key];
}

- (NSDictionary<NSString *, id>*) copyAsDictionary {
    return [self.cloudStore dictionaryRepresentation];
}

- (void)addEntriesFromDictionary:(NSDictionary<NSString*, NSObject*> *)otherDictionary {
    [otherDictionary enumerateKeysAndObjectsUsingBlock:^(NSString * _Nonnull key, NSObject * _Nonnull obj, BOOL * _Nonnull stop) {
        [self setObject:obj forKey:key];
    }];
}

- (id)objectForKey:(NSString*)key {
    return [self.cloudStore objectForKey:key];
}

- (void)removeObjectForKey:(NSString*)key {
    return [self.cloudStore removeObjectForKey:key];
}

- (void)removeAllObjects {
    [[[[self.cloudStore dictionaryRepresentation] allKeys] copy] enumerateObjectsUsingBlock:^(NSString * _Nonnull obj, NSUInteger idx, BOOL * _Nonnull stop) {
        [self.cloudStore removeObjectForKey:obj];
    }];
}

- (void)pushWrites {
    [[self cloudStore] synchronize];
    dispatch_async(self.perfQueue, ^{
        self.perfCounters->synchronize++;
    });
}

// Runs on the same thread that posted the notification, and that thread _may_ be the
// kdkvsproxy_queue (see 30470419). Avoid deadlock by bouncing through global queue.
- (void)kvsStoreChangedAsync:(NSNotification *)notification
{
    secnotice("CloudKeychainProxy", "%@ KVS Remote changed notification: %@", self, notification);
    dispatch_async(self.perfQueue, ^{
        self.perfCounters->incomingMessages++;
    });
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        [self kvsStoreChanged:notification];
    });
}

- (void) kvsStoreChanged:(NSNotification *)notification {
    /*
     Posted when the value of one or more keys in the local key-value store
     changed due to incoming data pushed from iCloud. This notification is
     sent only upon a change received from iCloud; it is not sent when your
     app sets a value.

     The user info dictionary can contain the reason for the notification as
     well as a list of which values changed, as follows:

     The value of the NSUbiquitousKeyValueStoreChangeReasonKey key, when
     present, indicates why the key-value store changed. Its value is one of
     the constants in "Change Reason Values."

     The value of the NSUbiquitousKeyValueStoreChangedKeysKey, when present,
     is an array of strings, each the name of a key whose value changed. The
     notification object is the NSUbiquitousKeyValueStore object whose contents
     changed.

     NSUbiquitousKeyValueStoreInitialSyncChange is only posted if there is any
     local value that has been overwritten by a distant value. If there is no
     conflict between the local and the distant values when doing the initial
     sync (e.g. if the cloud has no data stored or the client has not stored
     any data yet), you'll never see that notification.

     NSUbiquitousKeyValueStoreInitialSyncChange implies an initial round trip
     with server but initial round trip with server does not imply
     NSUbiquitousKeyValueStoreInitialSyncChange.
     */
    os_activity_initiate("cloudChanged", OS_ACTIVITY_FLAG_DEFAULT, ^{
        secdebug(XPROXYSCOPE, "%@ KVS Remote changed notification: %@", self, notification);

        UbiqitousKVSProxy* proxy = self.proxy;
        if(!proxy) {
            // Weak reference went away.
            return;
        }

        NSDictionary *userInfo = [notification userInfo];
        NSNumber *reason = [userInfo objectForKey:NSUbiquitousKeyValueStoreChangeReasonKey];
        NSArray *keysChangedArray = [userInfo objectForKey:NSUbiquitousKeyValueStoreChangedKeysKey];
        NSSet *keysChanged = keysChangedArray ? [NSSet setWithArray: keysChangedArray] : nil;

        if (reason) switch ([reason integerValue]) {
            case NSUbiquitousKeyValueStoreInitialSyncChange:
                [proxy storeKeysChanged: keysChanged initial: YES];
                break;

            case NSUbiquitousKeyValueStoreServerChange:
                [proxy storeKeysChanged: keysChanged initial: NO];
                break;

            case NSUbiquitousKeyValueStoreQuotaViolationChange:
                seccritical("Received NSUbiquitousKeyValueStoreQuotaViolationChange");
                break;

            case NSUbiquitousKeyValueStoreAccountChange:
                [proxy storeAccountChanged];
                break;

            default:
                secinfo("kvsstore", "ignoring unknown notification: %@", reason);
                break;
        }
    });
}

- (BOOL) pullUpdates:(NSError **)failure
{
    __block NSError *tempFailure = nil;
    
    dispatch_semaphore_t freshSemaphore = dispatch_semaphore_create(0);
    
    secnoticeq("fresh", "%s CALLING OUT TO syncdefaultsd SWCH: %@", kWAIT2MINID, self);
    
    dispatch_async(self.perfQueue, ^{
        self.perfCounters->synchronizeWithCompletionHandler++;
    });
    
    [[self cloudStore] synchronizeWithCompletionHandler:^(NSError *error) {
        if (error) {
            dispatch_async(self.perfQueue, ^{
                self.perfCounters->synchronizeFailures++;
            });
            tempFailure = error;
            secnotice("fresh", "%s RETURNING FROM syncdefaultsd SWCH: %@: %@", kWAIT2MINID, self, error);
        } else {
            dispatch_async(self.perfQueue, ^{
                self.perfCounters->synchronize++;
            });
            secnotice("fresh", "%s RETURNING FROM syncdefaultsd SWCH: %@", kWAIT2MINID, self);
            [[self cloudStore] synchronize]; // Per olivier in <rdar://problem/13412631>, sync before getting values
            secnotice("fresh", "%s RETURNING FROM syncdefaultsd SYNC: %@", kWAIT2MINID, self);
        }
        dispatch_semaphore_signal(freshSemaphore);
    }];
    dispatch_semaphore_wait(freshSemaphore, DISPATCH_TIME_FOREVER);
    
    if (failure && (*failure == NULL)) {
        *failure = tempFailure;
    }
    
    return tempFailure == nil;
}

- (void)perfCounters:(void(^)(NSDictionary *counters))callback
{
    dispatch_async(self.perfQueue, ^{
        callback(@{
            @"CKDKVS-synchronize" : @(self.perfCounters->synchronize),
            @"CKDKVS-synchronizeWithCompletionHandler" : @(self.perfCounters->synchronizeWithCompletionHandler),
            @"CKDKVS-incomingMessages" : @(self.perfCounters->incomingMessages),
            @"CKDKVS-outgoingMessages" : @(self.perfCounters->outgoingMessages),
            @"CKDKVS-totalWaittimeSynchronize" : @(self.perfCounters->totalWaittimeSynchronize),
            @"CKDKVS-longestWaittimeSynchronize" : @(self.perfCounters->longestWaittimeSynchronize),
            @"CKDKVS-synchronizeFailures" : @(self.perfCounters->synchronizeFailures),
        });
 });
}

- (void)addOneToOutGoing
{
    dispatch_async(self.perfQueue, ^{
        self.perfCounters->outgoingMessages++;
    });
}


@end
