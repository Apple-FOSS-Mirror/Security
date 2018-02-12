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

@protocol CKKSControlProtocol
- (void)performanceCounters:(void(^)(NSDictionary <NSString *, NSNumber *> *))reply;
- (void)rpcResetLocal:    (NSString*)viewName reply: (void(^)(NSError* result)) reply;
- (void)rpcResetCloudKit: (NSString*)viewName reply: (void(^)(NSError* result)) reply;
- (void)rpcResync:(NSString*)viewName reply: (void(^)(NSError* result)) reply;
- (void)rpcResyncLocal:(NSString*)viewName reply:(void(^)(NSError* result))reply;
- (void)rpcStatus:(NSString*)viewName reply: (void(^)(NSArray<NSDictionary*>* result, NSError* error)) reply;
- (void)rpcFetchAndProcessChanges:(NSString*)viewName reply: (void(^)(NSError* result)) reply;
- (void)rpcFetchAndProcessClassAChanges:(NSString*)viewName reply: (void(^)(NSError* result)) reply;
- (void)rpcPushOutgoingChanges:(NSString*)viewName reply: (void(^)(NSError* result)) reply;
- (void)rpcGetAnalyticsSysdiagnoseWithReply:(void (^)(NSString* sysdiagnose, NSError* error))reply;
- (void)rpcGetAnalyticsJSONWithReply:(void (^)(NSData* json, NSError* error))reply;
- (void)rpcForceUploadAnalyticsWithReply:(void (^)(BOOL success, NSError* error))reply;
@end

NSXPCInterface* CKKSSetupControlProtocol(NSXPCInterface* interface);
