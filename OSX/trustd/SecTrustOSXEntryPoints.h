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

/*
 * SecTrustOSXEntryPoints - Interface for unified SecTrust into OS X Security
 * Framework.
 */

#ifndef _SECURITY_SECTRUST_OSX_ENTRY_POINTS_H_
#define _SECURITY_SECTRUST_OSX_ENTRY_POINTS_H_

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

__BEGIN_DECLS

void SecTrustLegacySourcesEventRunloopCreate(void);

OSStatus SecTrustLegacyCRLStatus(SecCertificateRef cert, CFArrayRef chain, CFURLRef currCRLDP);

typedef struct async_ocspd_s {
    void (*completed)(struct async_ocspd_s *ocspd);
    void *info;
    OSStatus response;
    dispatch_queue_t queue;
} async_ocspd_t;

bool SecTrustLegacyCRLFetch(async_ocspd_t *ocspd,
                            CFURLRef currCRLDP, CFAbsoluteTime verifyTime,
                            SecCertificateRef cert, CFArrayRef chain);
__END_DECLS

#endif /* _SECURITY_SECTRUST_OSX_ENTRY_POINTS_H_ */
