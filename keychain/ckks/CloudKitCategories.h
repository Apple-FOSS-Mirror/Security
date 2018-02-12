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

#if OCTAGON

#import <Foundation/Foundation.h>
#import <CloudKit/CloudKit.h>
#import <CloudKit/CloudKit_Private.h>

@interface CKOperationGroup (CKKS)
+(instancetype) CKKSGroupWithName:(NSString*)name;
@end

@interface NSError (CKKS)

// More useful constructor
+ (instancetype)errorWithDomain:(NSErrorDomain)domain code:(NSInteger)code description:(NSString*)description;
+ (instancetype)errorWithDomain:(NSErrorDomain)domain code:(NSInteger)code description:(NSString*)description underlying:(NSError*)underlying;

// Returns true if this is a CloudKit error where
// 1) An atomic write failed
// 2) Every single suberror is either CKErrorServerRecordChanged or CKErrorUnknownItem
-(bool) ckksIsCKErrorRecordChangedError;
@end

// Ensure we don't print addresses
@interface CKAccountInfo (CKKS)
-(NSString*)description;
@end

#endif // OCTAGON
