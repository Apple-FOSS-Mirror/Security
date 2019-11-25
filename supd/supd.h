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

#import <Foundation/Foundation.h>
#import "supdProtocol.h"

@interface SFAnalyticsClient: NSObject
@property (nonatomic) NSString* storePath;
@property (nonatomic) NSString* name;
@property (atomic) BOOL requireDeviceAnalytics;
@property (atomic) BOOL requireiCloudAnalytics;
@end

@interface SFAnalyticsTopic : NSObject <NSURLSessionDelegate>
@property NSString* splunkTopicName;
@property NSURL* splunkBagURL;
@property NSString *internalTopicName;

@property NSArray<SFAnalyticsClient *> *topicClients;

// --------------------------------
// Things below are for unit testing
- (instancetype)initWithDictionary:(NSDictionary *)dictionary name:(NSString *)topicName samplingRates:(NSDictionary *)rates;
- (BOOL)haveEligibleClients;
+ (NSString*)databasePathForCKKS;
+ (NSString*)databasePathForSOS;
+ (NSString*)databasePathForPCS;
+ (NSString*)databasePathForTLS;
@end

@interface SFAnalyticsReporter : NSObject
- (BOOL)saveReport:(NSData *)reportData fileName:(NSString *)fileName;
@end

@interface supd : NSObject <supdProtocol>
+ (instancetype)instance;
+ (void)removeInstance;
+ (void)instantiate;
- (instancetype)initWithReporter:(SFAnalyticsReporter *)reporter;

// --------------------------------
// Things below are for unit testing
@property (readonly) dispatch_queue_t queue;
@property (readonly) NSArray<SFAnalyticsTopic*>* analyticsTopics;
@property (readonly) SFAnalyticsReporter *reporter;
- (void)sendNotificationForOncePerReportSamplers;
@end

// --------------------------------
// Things below are for unit testing
extern BOOL deviceAnalyticsOverride;
extern BOOL deviceAnalyticsEnabled;
extern BOOL iCloudAnalyticsOverride;
extern BOOL iCloudAnalyticsEnabled;
