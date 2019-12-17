/*
 * Copyright (c) 2013-2014 Apple Inc. All Rights Reserved.
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

#ifndef _SECURITY_SECTASKPRIV_H_
#define _SECURITY_SECTASKPRIV_H_

#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecTask.h>

__BEGIN_DECLS

#if SEC_OS_OSX
/*!
    @function SecTaskValidateForRequirement
    @abstract Validate a SecTask instance for a specified requirement.
    @param task The SecTask instance to validate.
    @param requirement A requirement string to be validated.
    @result An error code of type OSStatus. Returns errSecSuccess if the
    task satisfies the requirement.
*/

OSStatus SecTaskValidateForRequirement(SecTaskRef _Nonnull task, CFStringRef _Nonnull requirement);

/*!
  @function SecTaskGetCodeSignStatus
  @abstract Get code signing flags
  @param task A previously created SecTask object
*/

uint32_t
SecTaskGetCodeSignStatus(SecTaskRef _Nonnull task);
#endif /* SEC_OS_OSX */

/*!
 @function SecTaskEntitlementsValidated
 @abstract Check whether entitlements can be trusted or not.  If this returns
 false the tasks entitlements must not be used for anything security sensetive.
 @param task A previously created SecTask object
 */
Boolean SecTaskEntitlementsValidated(SecTaskRef _Nonnull task);

/*!
 @function SecTaskCopyTeamIdentifier
 @abstract Return the value of the team identifier.
 @param task A previously created SecTask object
 @param error On a NULL return, this will contain a CFError describing
 the problem.  This argument may be NULL if the caller is not interested in
 detailed errors. The caller must CFRelease the returned value
 */
__nullable
CFStringRef SecTaskCopyTeamIdentifier(SecTaskRef _Nonnull task, CFErrorRef _Nullable * _Nullable error);

__END_DECLS

#endif /* !_SECURITY_SECTASKPRIV_H_ */
