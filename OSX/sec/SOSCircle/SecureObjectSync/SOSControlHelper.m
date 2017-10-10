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
#import <Foundation/NSXPCConnection.h>
#import <Security/SecureObjectSync/SOSTypes.h>
#import <Security/SecureObjectSync/SOSControlHelper.h>
#import <objc/runtime.h>

void
_SOSControlSetupInterface(NSXPCInterface *interface)
{
    static NSMutableSet *errClasses;

    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        errClasses = [NSMutableSet set];

        char *classes[] = {
            "NSURL",
            "NSURLError",
            "NSError"
        };

        for (unsigned n = 0; n < sizeof(classes)/sizeof(classes[0]); n++) {
            Class cls = objc_getClass(classes[n]);
            if (cls)
                [errClasses addObject:cls];
        }
    });

    [interface setClasses:errClasses forSelector:@selector(userPublicKey:) argumentIndex:2 ofReply:YES];

    [interface setClasses:errClasses forSelector:@selector(stashedCredentialPublicKey:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(assertStashedAccountCredential:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(stashAccountCredential:complete:) argumentIndex:1 ofReply:YES];

    [interface setClasses:errClasses forSelector:@selector(myPeerInfo:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(circleJoiningBlob:complete:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(joinCircleWithBlob:version:complete:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(initialSyncCredentials:complete:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(importInitialSyncCredentials:complete:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(triggerSync:complete:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(getWatchdogParameters:) argumentIndex:1 ofReply:YES];
    [interface setClasses:errClasses forSelector:@selector(setWatchdogParmeters:complete:) argumentIndex:0 ofReply:YES];
}
