/*
 * Copyright (c) 2016-2018 Apple Inc. All Rights Reserved.
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
 *
 */

/*
 *  SecPinningDb.m
 */

#include <AssertMacros.h>
#import <Foundation/Foundation.h>
#import <sys/stat.h>
#import <notify.h>

#if !TARGET_OS_BRIDGE
#import <MobileAsset/MAAsset.h>
#import <MobileAsset/MAAssetQuery.h>
#endif

#if TARGET_OS_OSX
#import <MobileAsset/MobileAsset.h>
#include <sys/csr.h>
#endif

#import <Security/SecInternalReleasePriv.h>

#import <securityd/OTATrustUtilities.h>
#import <securityd/SecPinningDb.h>
#import <securityd/SecTrustLoggingServer.h>

#include "utilities/debugging.h"
#include "utilities/sqlutils.h"
#include "utilities/iOSforOSX.h"
#include <utilities/SecCFError.h>
#include <utilities/SecCFRelease.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecDb.h>
#include <utilities/SecFileLocations.h>
#include "utilities/sec_action.h"

#define kSecPinningBasePath         "/Library/Keychains/"
#define kSecPinningDbFileName       "pinningrules.sqlite3"

const uint64_t PinningDbSchemaVersion = 2;
const NSString *PinningDbPolicyNameKey = @"policyName"; /* key for a string value */
const NSString *PinningDbDomainsKey = @"domains"; /* key for an array of dictionaries */
const NSString *PinningDbPoliciesKey = @"rules"; /* key for an array of dictionaries */
const NSString *PinningDbDomainSuffixKey = @"suffix"; /* key for a string */
const NSString *PinningDbLabelRegexKey = @"labelRegex"; /* key for a regex string */

const CFStringRef kSecPinningDbKeyHostname = CFSTR("PinningHostname");
const CFStringRef kSecPinningDbKeyPolicyName = CFSTR("PinningPolicyName");
const CFStringRef kSecPinningDbKeyRules = CFSTR("PinningRules");

@interface SecPinningDb : NSObject
@property (assign) SecDbRef db;
@property dispatch_queue_t queue;
@property NSURL *dbPath;
- (instancetype) init;
- ( NSDictionary * _Nullable ) queryForDomain:(NSString *)domain;
- ( NSDictionary * _Nullable ) queryForPolicyName:(NSString *)policyName;
@end

static inline bool isNSNumber(id nsType) {
    return nsType && [nsType isKindOfClass:[NSNumber class]];
}

static inline bool isNSArray(id nsType) {
    return nsType && [nsType isKindOfClass:[NSArray class]];
}

static inline bool isNSDictionary(id nsType) {
    return nsType && [nsType isKindOfClass:[NSDictionary class]];
}

@implementation SecPinningDb
#define getSchemaVersionSQL CFSTR("PRAGMA user_version")
#define selectVersionSQL CFSTR("SELECT ival FROM admin WHERE key='version'")
#define insertAdminSQL CFSTR("INSERT OR REPLACE INTO admin (key,ival,value) VALUES (?,?,?)")
#define selectDomainSQL CFSTR("SELECT DISTINCT labelRegex,policyName,policies FROM rules WHERE domainSuffix=?")
#define selectPolicyNameSQL CFSTR("SELECT DISTINCT policies FROM rules WHERE policyName=?")
#define insertRuleSQL CFSTR("INSERT OR REPLACE INTO rules (policyName,domainSuffix,labelRegex,policies) VALUES (?,?,?,?) ")
#define removeAllRulesSQL CFSTR("DELETE FROM rules;")

- (NSNumber *)getSchemaVersion:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error {
    __block bool ok = true;
    __block NSNumber *version = nil;
    ok &= SecDbWithSQL(dbconn, getSchemaVersionSQL, error, ^bool(sqlite3_stmt *selectVersion) {
        ok &= SecDbStep(dbconn, selectVersion, error, ^(bool *stop) {
            int ival = sqlite3_column_int(selectVersion, 0);
            version = [NSNumber numberWithInt:ival];
        });
        return ok;
    });
    return version;
}

- (BOOL)setSchemaVersion:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error {
    bool ok = true;
    NSString *setVersion = [NSString stringWithFormat:@"PRAGMA user_version = %llu", PinningDbSchemaVersion];
    ok &= SecDbExec(dbconn,
                    (__bridge CFStringRef)setVersion,
                    error);
    if (!ok) {
        secerror("SecPinningDb: failed to create admin table: %@", error ? *error : nil);
    }
    return ok;
}

- (NSNumber *)getContentVersion:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error {
    __block bool ok = true;
    __block NSNumber *version = nil;
    ok &= SecDbWithSQL(dbconn, selectVersionSQL, error, ^bool(sqlite3_stmt *selectVersion) {
        ok &= SecDbStep(dbconn, selectVersion, error, ^(bool *stop) {
            uint64_t ival = sqlite3_column_int64(selectVersion, 0);
            version = [NSNumber numberWithUnsignedLongLong:ival];
        });
        return ok;
    });
    return version;
}

- (BOOL)setContentVersion:(NSNumber *)version dbConnection:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error {
    __block BOOL ok = true;
    ok &= SecDbWithSQL(dbconn, insertAdminSQL, error, ^bool(sqlite3_stmt *insertAdmin) {
        const char *versionKey = "version";
        ok &= SecDbBindText(insertAdmin, 1, versionKey, strlen(versionKey), SQLITE_TRANSIENT, error);
        ok &= SecDbBindInt64(insertAdmin, 2, [version unsignedLongLongValue], error);
        ok &= SecDbStep(dbconn, insertAdmin, error, NULL);
        return ok;
    });
    if (!ok) {
        secerror("SecPinningDb: failed to set version %@ from pinning list: %@", version, error ? *error : nil);
    }
    return ok;
}

- (BOOL) shouldUpdateContent:(NSNumber *)new_version {
    __block CFErrorRef error = NULL;
    __block BOOL ok = YES;
    __block BOOL newer = NO;
    ok &= SecDbPerformRead(_db, &error, ^(SecDbConnectionRef dbconn) {
        NSNumber *db_version = [self getContentVersion:dbconn error:&error];
        if (!db_version || [new_version compare:db_version] == NSOrderedDescending) {
            newer = YES;
            secnotice("pinningDb", "Pinning database should update from version %@ to version %@", db_version, new_version);
        }
    });

    if (!ok || error) {
        secerror("SecPinningDb: error reading content version from database %@", error);
    }
    CFReleaseNull(error);
    return newer;
}

- (BOOL) insertRuleWithName:(NSString *)policyName
               domainSuffix:(NSString *)domainSuffix
                 labelRegex:(NSString *)labelRegex
                   policies:(NSArray *)policies
               dbConnection:(SecDbConnectionRef)dbconn
                      error:(CFErrorRef *)error{
    /* @@@ This insertion mechanism assumes that the input is trusted -- namely, that the new rules
     * are allowed to replace existing rules. For third-party inputs, this assumption isn't true. */

    secdebug("pinningDb", "inserting new rule: %@ for %@.%@", policyName, labelRegex, domainSuffix);

    __block bool ok = true;
    ok &= SecDbWithSQL(dbconn, insertRuleSQL, error, ^bool(sqlite3_stmt *insertRule) {
        ok &= SecDbBindText(insertRule, 1, [policyName UTF8String], [policyName length], SQLITE_TRANSIENT, error);
        ok &= SecDbBindText(insertRule, 2, [domainSuffix UTF8String], [domainSuffix length], SQLITE_TRANSIENT, error);
        ok &= SecDbBindText(insertRule, 3, [labelRegex UTF8String], [labelRegex length], SQLITE_TRANSIENT, error);
        NSData *xmlPolicies = [NSPropertyListSerialization dataWithPropertyList:policies
                                                                         format:NSPropertyListXMLFormat_v1_0
                                                                        options:0
                                                                          error:nil];
        if (!xmlPolicies) {
            secerror("SecPinningDb: failed to serialize policies");
            ok = false;
        }
        ok &= SecDbBindBlob(insertRule, 4, [xmlPolicies bytes], [xmlPolicies length], SQLITE_TRANSIENT, error);
        ok &= SecDbStep(dbconn, insertRule, error, NULL);
        return ok;
    });
    if (!ok) {
        secerror("SecPinningDb: failed to insert rule %@ for %@.%@ with error %@", policyName, labelRegex, domainSuffix, error ? *error : nil);
    }
    return ok;
}

- (BOOL) populateDbFromBundle:(NSArray *)pinningList dbConnection:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error {
    __block BOOL ok = true;
    [pinningList enumerateObjectsUsingBlock:^(id  _Nonnull obj, NSUInteger idx, BOOL * _Nonnull stop) {
        if (idx ==0) { return; } // Skip the first value which is the version
        if (!isNSDictionary(obj)) {
            secerror("SecPinningDb: rule entry in pinning plist is wrong class");
            ok = false;
            return;
        }
        NSDictionary *rule = obj;
        __block NSString *policyName = [rule objectForKey:PinningDbPolicyNameKey];
        NSArray *domains = [rule objectForKey:PinningDbDomainsKey];
        __block NSArray *policies = [rule objectForKey:PinningDbPoliciesKey];

        if (!policyName || !domains || !policies) {
            secerror("SecPinningDb: failed to get required fields from rule entry %lu", (unsigned long)idx);
            ok = false;
            return;
        }

        [domains enumerateObjectsUsingBlock:^(id  _Nonnull obj, NSUInteger idx, BOOL * _Nonnull stop) {
            if (!isNSDictionary(obj)) {
                secerror("SecPinningDb: domain entry %lu for %@ in pinning rule is wrong class", (unsigned long)idx, policyName);
                ok = false;
                return;
            }
            NSDictionary *domain = obj;
            NSString *suffix = [domain objectForKey:PinningDbDomainSuffixKey];
            NSString *labelRegex = [domain objectForKey:PinningDbLabelRegexKey];

            if (!suffix || !labelRegex) {
                secerror("SecPinningDb: failed to get required fields for entry %lu for %@", (unsigned long)idx, policyName);
                ok = false;
                return;
            }
            ok &= [self insertRuleWithName:policyName domainSuffix:suffix labelRegex:labelRegex policies:policies
                              dbConnection:dbconn error:error];
        }];
    }];
    if (!ok) {
        secerror("SecPinningDb: failed to populate DB from pinning list: %@", error ? *error : nil);
    }
    return ok;
}

- (BOOL) removeAllRulesFromDb:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error {
    __block BOOL ok = true;
    ok &= SecDbWithSQL(dbconn, removeAllRulesSQL, error, ^bool(sqlite3_stmt *deleteRules) {
        ok &= SecDbStep(dbconn, deleteRules, error, NULL);
        return ok;
    });
    if (!ok) {
        secerror("SecPinningDb: failed to delete old values: %@", error ? *error :nil);
    }
    return ok;
}


- (BOOL) createOrAlterAdminTable:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error {
    bool ok = true;
    ok &= SecDbExec(dbconn,
                    CFSTR("CREATE TABLE IF NOT EXISTS admin("
                          "key TEXT PRIMARY KEY NOT NULL,"
                          "ival INTEGER NOT NULL,"
                          "value BLOB"
                          ");"),
                    error);
    if (!ok) {
        secerror("SecPinningDb: failed to create admin table: %@", error ? *error : nil);
    }
    return ok;
}

- (BOOL) createOrAlterRulesTable:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error {
    bool ok = true;
    ok &= SecDbExec(dbconn,
                    CFSTR("CREATE TABLE IF NOT EXISTS rules("
                          "policyName TEXT NOT NULL,"
                          "domainSuffix TEXT NOT NULL,"
                          "labelRegex TEXT NOT NULL,"
                          "policies BLOB NOT NULL,"
                          "UNIQUE(policyName, domainSuffix, labelRegex)"
                          ");"),
                    error);
    ok &= SecDbExec(dbconn, CFSTR("CREATE INDEX IF NOT EXISTS idomain ON rules(domainSuffix);"), error);
    ok &= SecDbExec(dbconn, CFSTR("CREATE INDEX IF NOT EXISTS ipolicy ON rules(policyName);"), error);
    if (!ok) {
        secerror("SecPinningDb: failed to create rules table: %@", error ? *error : nil);
    }
    return ok;
}

#if !TARGET_OS_BRIDGE
- (BOOL) installDbFromURL:(NSURL *)localURL {
    if (!localURL) {
        secerror("SecPinningDb: missing url for downloaded asset");
        return NO;
    }
    NSURL *fileLoc = [NSURL URLWithString:@"CertificatePinning.plist"
                     relativeToURL:localURL];
    __block NSArray *pinningList = [NSArray arrayWithContentsOfURL:fileLoc];
    if (!pinningList) {
        secerror("SecPinningDb: unable to create pinning list from asset file: %@", fileLoc);
        return NO;
    }

    NSNumber *plist_version = [pinningList objectAtIndex:0];
    if (![self shouldUpdateContent:plist_version]) {
        /* We got a new plist but we already have that version installed. */
        return YES;
    }

    /* Update Content */
    __block CFErrorRef error = NULL;
    __block BOOL ok = YES;
    dispatch_sync(self->_queue, ^{
        ok &= SecDbPerformWrite(self->_db, &error, ^(SecDbConnectionRef dbconn) {
            ok &= [self updateDb:dbconn error:&error pinningList:pinningList updateSchema:NO updateContent:YES];
        });
    });

    if (!ok || error) {
        secerror("SecPinningDb: error installing updated pinning list version %@: %@", [pinningList objectAtIndex:0], error);
#if ENABLE_TRUSTD_ANALYTICS
        [[TrustdHealthAnalytics logger] logHardError:(__bridge NSError *)error
                                       withEventName:TrustdHealthAnalyticsEventDatabaseEvent
                                      withAttributes:@{TrustdHealthAnalyticsAttributeAffectedDatabase : @(TAPinningDb),
                                                       TrustdHealthAnalyticsAttributeDatabaseOperation : @(TAOperationWrite) }];
#endif // ENABLE_TRUSTD_ANALYTICS
        CFReleaseNull(error);
    }

    return ok;
}
#endif /* !TARGET_OS_BRIDGE */

- (NSArray *) copySystemPinningList {
    NSArray *pinningList = nil;
    NSURL *pinningListURL = nil;
    /* Get the pinning list shipped with the OS */
    SecOTAPKIRef otapkiref = SecOTAPKICopyCurrentOTAPKIRef();
    if (otapkiref) {
        pinningListURL = CFBridgingRelease(SecOTAPKICopyPinningList(otapkiref));
        CFReleaseNull(otapkiref);
        if (!pinningListURL) {
            secerror("SecPinningDb: failed to get pinning plist URL");
        }
        NSError *error = nil;
        pinningList = [NSArray arrayWithContentsOfURL:pinningListURL error:&error];
        if (!pinningList) {
            secerror("SecPinningDb: failed to read pinning plist from bundle: %@", error);
        }
    }

    return pinningList;
}

- (BOOL) updateDb:(SecDbConnectionRef)dbconn error:(CFErrorRef *)error pinningList:(NSArray *)pinningList
     updateSchema:(BOOL)updateSchema updateContent:(BOOL)updateContent
{
    if (!SecOTAPKIIsSystemTrustd()) { return false; }
    secdebug("pinningDb", "updating or creating database");

    __block bool ok = true;
    ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, error, ^(bool *commit) {
        if (updateSchema) {
            /* update the tables */
            ok &= [self createOrAlterAdminTable:dbconn error:error];
            ok &= [self createOrAlterRulesTable:dbconn error:error];
            ok &= [self setSchemaVersion:dbconn error:error];
        }

        if (updateContent) {
            /* remove the old data */
            /* @@@ This behavior assumes that we have all the rules we want to populate
             * elsewhere on disk and that the DB doesn't contain the sole copy of that data. */
            ok &= [self removeAllRulesFromDb:dbconn error:error];

            /* read the new data */
            NSNumber *version = [pinningList objectAtIndex:0];

            /* populate the tables */
            ok &= [self populateDbFromBundle:pinningList dbConnection:dbconn error:error];
            ok &= [self setContentVersion:version dbConnection:dbconn error:error];
        }

        *commit = ok;
    });

    return ok;
}

- (SecDbRef) createAtPath {
    bool readWrite = SecOTAPKIIsSystemTrustd();
#if TARGET_OS_OSX
    mode_t mode = 0644; // Root trustd can rw. All other trustds need to read.
#else
    mode_t mode = 0600; // Only one trustd.
#endif

    CFStringRef path = CFStringCreateWithCString(NULL, [_dbPath fileSystemRepresentation], kCFStringEncodingUTF8);
    SecDbRef result = SecDbCreateWithOptions(path, mode, readWrite, readWrite, false,
         ^bool (SecDbRef db, SecDbConnectionRef dbconn, bool didCreate, bool *callMeAgainForNextConnection, CFErrorRef *error) {
             if (!SecOTAPKIIsSystemTrustd()) {
                 /* Non-owner process can't update the db, but it should get a db connection.
                  * @@@ Revisit if new schema version is needed by reader processes. */
                 return true;
             }

             __block BOOL ok = true;
             dispatch_sync(self->_queue, ^{
                 bool updateSchema = false;
                 bool updateContent = false;

                 /* Get the pinning plist */
                 NSArray *pinningList = [self copySystemPinningList];
                 if (!pinningList) {
                     secerror("SecPinningDb: failed to find pinning plist in bundle");
                     ok = false;
                     return;
                 }

                 /* Check latest data and schema versions against existing table. */
                 if (!isNSNumber([pinningList objectAtIndex:0])) {
                     secerror("SecPinningDb: pinning plist in wrong format");
                     return; // Don't change status. We can continue to use old DB.
                 }
                 NSNumber *plist_version = [pinningList objectAtIndex:0];
                 NSNumber *db_version = [self getContentVersion:dbconn error:error];
                 secnotice("pinningDb", "Opening db with version %@", db_version);
                 if (!db_version || [plist_version compare:db_version] == NSOrderedDescending) {
                     secnotice("pinningDb", "Updating pinning database content from version %@ to version %@",
                               db_version ? db_version : 0, plist_version);
                     updateContent = true;
                 }
                 NSNumber *schema_version = [self getSchemaVersion:dbconn error:error];
                 NSNumber *current_version = [NSNumber numberWithUnsignedLongLong:PinningDbSchemaVersion];
                 if (!schema_version || ![schema_version isEqualToNumber:current_version]) {
                     secnotice("pinningDb", "Updating pinning database schema from version %@ to version %@",
                               schema_version, current_version);
                     updateSchema = true;
                 }

                 if (updateContent || updateSchema) {
                     ok &= [self updateDb:dbconn error:error pinningList:pinningList updateSchema:updateSchema updateContent:updateContent];
                     /* Since we updated the DB to match the list that shipped with the system,
                      * reset the OTAPKI Asset version to the system asset version */
                     (void)SecOTAPKIResetCurrentAssetVersion(NULL);
                 }
                 if (!ok) {
                     secerror("SecPinningDb: %s failed: %@", didCreate ? "Create" : "Open", error ? *error : NULL);
#if ENABLE_TRUSTD_ANALYTICS
                     [[TrustdHealthAnalytics logger] logHardError:(error ? (__bridge NSError *)*error : nil)
                                                    withEventName:TrustdHealthAnalyticsEventDatabaseEvent
                                                   withAttributes:@{TrustdHealthAnalyticsAttributeAffectedDatabase : @(TAPinningDb),
                                                                    TrustdHealthAnalyticsAttributeDatabaseOperation : didCreate ? @(TAOperationCreate) : @(TAOperationOpen)}];
#endif // ENABLE_TRUSTD_ANALYTICS
                 }
             });
             return ok;
         });

    CFReleaseNull(path);
    return result;
}

static void verify_create_path(const char *path)
{
    int ret = mkpath_np(path, 0755);
    if (!(ret == 0 || ret ==  EEXIST)) {
        secerror("could not create path: %s (%s)", path, strerror(ret));
    }
}

- (NSURL *)pinningDbPath {
    /* Make sure the /Library/Keychains directory is there */
#if TARGET_OS_IPHONE
    NSURL *directory = CFBridgingRelease(SecCopyURLForFileInKeychainDirectory(nil));
#else
    NSURL *directory = [NSURL fileURLWithFileSystemRepresentation:"/Library/Keychains/" isDirectory:YES relativeToURL:nil];
#endif
    verify_create_path([directory fileSystemRepresentation]);

    /* Get the full path of the pinning DB */
    return [directory URLByAppendingPathComponent:@"pinningrules.sqlite3"];
}

- (void) initializedDb {
    dispatch_sync(_queue, ^{
        if (!self->_db) {
            self->_dbPath = [self pinningDbPath];
            self->_db = [self createAtPath];
        }
    });
}

- (instancetype) init {
    if (self = [super init]) {
        _queue = dispatch_queue_create("Pinning DB Queue", DISPATCH_QUEUE_SERIAL_WITH_AUTORELEASE_POOL);
        [self initializedDb];
    }
    return self;
}

- (void) dealloc {
    CFReleaseNull(_db);
}

- (BOOL) isPinningDisabled:(NSString * _Nullable)policy {
    static dispatch_once_t once;
    static sec_action_t action;

    BOOL pinningDisabled = NO;
    if (SecIsInternalRelease()) {
        NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.apple.security"];
        pinningDisabled = [defaults boolForKey:@"AppleServerAuthenticationNoPinning"];
        if (!pinningDisabled && policy) {
            NSMutableString *policySpecificKey = [NSMutableString stringWithString:@"AppleServerAuthenticationNoPinning"];
            [policySpecificKey appendString:policy];
            pinningDisabled = [defaults boolForKey:policySpecificKey];
            secinfo("pinningQA", "%@ disable pinning = %d", policy, pinningDisabled);
        }
    }


    dispatch_once(&once, ^{
        /* Only log system-wide pinning status once every five minutes */
        action = sec_action_create("pinning logging charles", 5*60.0);
        sec_action_set_handler(action, ^{
            if (!SecIsInternalRelease()) {
                secnotice("pinningQA", "could not disable pinning: not an internal release");
            } else {
                NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:@"com.apple.security"];
                secnotice("pinningQA", "generic pinning disable = %d", [defaults boolForKey:@"AppleServerAuthenticationNoPinning"]);
            }
        });
    });
    sec_action_perform(action);

    return pinningDisabled;
}

- ( NSDictionary * _Nullable ) queryForDomain:(NSString *)domain {
    if (!_queue) { (void)[self init]; }
    if (!_db) { [self initializedDb]; }

    /* Check for general no-pinning setting */
    if ([self isPinningDisabled:nil]) {
        return nil;
    }

    /* parse the domain into suffix and 1st label */
    NSRange firstDot = [domain rangeOfString:@"."];
    if (firstDot.location == NSNotFound) { return nil; } // Probably not a legitimate domain name
    __block NSString *firstLabel = [domain substringToIndex:firstDot.location];
    __block NSString *suffix = [domain substringFromIndex:(firstDot.location + 1)];

    /* Perform SELECT */
    __block bool ok = true;
    __block CFErrorRef error = NULL;
    __block NSMutableArray *resultRules = [NSMutableArray array];
    __block NSString *resultName = nil;
    ok &= SecDbPerformRead(_db, &error, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbWithSQL(dbconn, selectDomainSQL, &error, ^bool(sqlite3_stmt *selectDomain) {
            ok &= SecDbBindText(selectDomain, 1, [suffix UTF8String], [suffix length], SQLITE_TRANSIENT, &error);
            ok &= SecDbStep(dbconn, selectDomain, &error, ^(bool *stop) {
                /* Match the labelRegex */
                const uint8_t *regex = sqlite3_column_text(selectDomain, 0);
                if (!regex) { return; }
                NSString *regexStr = [NSString stringWithUTF8String:(const char *)regex];
                if (!regexStr) { return; }
                NSRegularExpression *regularExpression = [NSRegularExpression regularExpressionWithPattern:regexStr
                                                                                                  options:NSRegularExpressionCaseInsensitive
                                                                                                    error:nil];
                if (!regularExpression) { return; }
                NSUInteger numMatches = [regularExpression numberOfMatchesInString:firstLabel
                                                                           options:0
                                                                             range:NSMakeRange(0, [firstLabel length])];
                if (numMatches == 0) {
                    return;
                }
                secdebug("SecPinningDb", "found matching rule for %@.%@", firstLabel, suffix);

                /* Check the policyName for no-pinning settings */
                const uint8_t *policyName = sqlite3_column_text(selectDomain, 1);
                NSString *policyNameStr = [NSString stringWithUTF8String:(const char *)policyName];
                if ([self isPinningDisabled:policyNameStr]) {
                    return;
                }

                /* Deserialize the policies and return.
                 * @@@ Assumes there is only one rule with matching suffix/label pairs. */
                NSData *xmlPolicies = [NSData dataWithBytes:sqlite3_column_blob(selectDomain, 2) length:sqlite3_column_bytes(selectDomain, 2)];
                if (!xmlPolicies) { return; }
                id policies = [NSPropertyListSerialization propertyListWithData:xmlPolicies options:0 format:nil error:nil];
                if (!isNSArray(policies)) {
                    return;
                }
                [resultRules addObjectsFromArray:(NSArray *)policies];
                resultName = policyNameStr;
            });
            return ok;
        });
    });

    if (!ok || error) {
        secerror("SecPinningDb: error querying DB for hostname: %@", error);
#if ENABLE_TRUSTD_ANALYTICS
        [[TrustdHealthAnalytics logger] logHardError:(__bridge NSError *)error
                                       withEventName:TrustdHealthAnalyticsEventDatabaseEvent
                                      withAttributes:@{TrustdHealthAnalyticsAttributeAffectedDatabase : @(TAPinningDb),
                                                       TrustdHealthAnalyticsAttributeDatabaseOperation : @(TAOperationRead)}];
#endif // ENABLE_TRUSTD_ANALYTICS
        CFReleaseNull(error);
    }

    if ([resultRules count] > 0) {
        NSDictionary *results = @{(__bridge NSString*)kSecPinningDbKeyRules:resultRules,
                                  (__bridge NSString*)kSecPinningDbKeyPolicyName:resultName};
        return results;
    }
    return nil;
}

- (NSDictionary * _Nullable) queryForPolicyName:(NSString *)policyName {
    if (!_queue) { (void)[self init]; }
    if (!_db) { [self initializedDb]; }

    /* Skip the "sslServer" policyName, which is not a pinning policy */
    if ([policyName isEqualToString:@"sslServer"]) {
        return nil;
    }

    /* Check for general no-pinning setting */
    if ([self isPinningDisabled:nil] || [self isPinningDisabled:policyName]) {
        return nil;
    }

    secinfo("SecPinningDb", "Fetching rules for policy named %@", policyName);

    /* Perform SELECT */
    __block bool ok = true;
    __block CFErrorRef error = NULL;
    __block NSMutableArray *resultRules = [NSMutableArray array];
    ok &= SecDbPerformRead(_db, &error, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbWithSQL(dbconn, selectPolicyNameSQL, &error, ^bool(sqlite3_stmt *selectPolicyName) {
            ok &= SecDbBindText(selectPolicyName, 1, [policyName UTF8String], [policyName length], SQLITE_TRANSIENT, &error);
            ok &= SecDbStep(dbconn, selectPolicyName, &error, ^(bool *stop) {
                secdebug("SecPinningDb", "found matching rule for %@ policy", policyName);

                /* Deserialize the policies and return */
                NSData *xmlPolicies = [NSData dataWithBytes:sqlite3_column_blob(selectPolicyName, 0) length:sqlite3_column_bytes(selectPolicyName, 0)];
                if (!xmlPolicies) { return; }
                id policies = [NSPropertyListSerialization propertyListWithData:xmlPolicies options:0 format:nil error:nil];
                if (!isNSArray(policies)) {
                    return;
                }
                [resultRules addObjectsFromArray:(NSArray *)policies];
            });
            return ok;
        });
    });

    if (!ok || error) {
        secerror("SecPinningDb: error querying DB for policyName: %@", error);
#if ENABLE_TRUSTD_ANALYTICS
        [[TrustdHealthAnalytics logger] logHardError:(__bridge NSError *)error
                                       withEventName:TrustdHealthAnalyticsEventDatabaseEvent
                                      withAttributes:@{TrustdHealthAnalyticsAttributeAffectedDatabase : @(TAPinningDb),
                                                       TrustdHealthAnalyticsAttributeDatabaseOperation : @(TAOperationRead)}];
#endif // ENABLE_TRUSTD_ANALYTICS
        CFReleaseNull(error);
    }

    if ([resultRules count] > 0) {
        NSDictionary *results = @{(__bridge NSString*)kSecPinningDbKeyRules:resultRules,
                                  (__bridge NSString*)kSecPinningDbKeyPolicyName:policyName};
        return results;
    }
    return nil;
}

@end

/* C interfaces */
static SecPinningDb *pinningDb = nil;
void SecPinningDbInitialize(void) {
    /* Create the pinning object once per launch */
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        @autoreleasepool {
            pinningDb = [[SecPinningDb alloc] init];
            __block CFErrorRef error = NULL;
            BOOL ok = SecDbPerformRead([pinningDb db], &error, ^(SecDbConnectionRef dbconn) {
                NSNumber *contentVersion = [pinningDb getContentVersion:dbconn error:&error];
                NSNumber *schemaVersion = [pinningDb getSchemaVersion:dbconn error:&error];
                secinfo("pinningDb", "Database Schema: %@ Content: %@", schemaVersion, contentVersion);
            });
            if (!ok || error) {
                secerror("SecPinningDb: unable to initialize db: %@", error);
#if ENABLE_TRUSTD_ANALYTICS
                [[TrustdHealthAnalytics logger] logHardError:(__bridge NSError *)error
                                               withEventName:TrustdHealthAnalyticsEventDatabaseEvent
                                              withAttributes:@{TrustdHealthAnalyticsAttributeAffectedDatabase : @(TAPinningDb),
                                                               TrustdHealthAnalyticsAttributeDatabaseOperation : @(TAOperationRead)}];
#endif // ENABLE_TRUSTD_ANALYTICS
            }
            CFReleaseNull(error);
        }
    });
}

CFDictionaryRef _Nullable SecPinningDbCopyMatching(CFDictionaryRef query) {
    @autoreleasepool {
        SecPinningDbInitialize();

        NSDictionary *nsQuery = (__bridge NSDictionary*)query;
        NSString *hostname = [nsQuery objectForKey:(__bridge NSString*)kSecPinningDbKeyHostname];

        NSDictionary *results = [pinningDb queryForDomain:hostname];
        if (results) { return CFBridgingRetain(results); }
        NSString *policyName = [nsQuery objectForKey:(__bridge NSString*)kSecPinningDbKeyPolicyName];
        results = [pinningDb queryForPolicyName:policyName];
        if (!results) { return nil; }
        return CFBridgingRetain(results);
    }
}

#if !TARGET_OS_BRIDGE
bool SecPinningDbUpdateFromURL(CFURLRef url) {
    SecPinningDbInitialize();

    return [pinningDb installDbFromURL:(__bridge NSURL*)url];
}
#endif

CFNumberRef SecPinningDbCopyContentVersion(void) {
    @autoreleasepool {
        __block CFErrorRef error = NULL;
        __block NSNumber *contentVersion = nil;
        BOOL ok = SecDbPerformRead([pinningDb db], &error, ^(SecDbConnectionRef dbconn) {
            contentVersion = [pinningDb getContentVersion:dbconn error:&error];
        });
        if (!ok || error) {
            secerror("SecPinningDb: unable to get content version: %@", error);
        }
        CFReleaseNull(error);
        if (!contentVersion) {
            contentVersion = [NSNumber numberWithInteger:0];
        }
        return CFBridgingRetain(contentVersion);
    }
}
