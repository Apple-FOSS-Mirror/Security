/*
 * Copyright (c) 2000-2002 Apple Computer, Inc. All Rights Reserved.
 * 
 * The contents of this file constitute Original Code as defined in and are
 * subject to the Apple Public Source License Version 1.2 (the 'License').
 * You may not use this file except in compliance with the License. Please obtain
 * a copy of the License at http://www.apple.com/publicsource and read it before
 * using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS
 * OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES, INCLUDING WITHOUT
 * LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT. Please see the License for the
 * specific language governing rights and limitations under the License.
 */


/*
 *  AuthorizationTags.h -- Right tags for implementing access control in
 *  applications and daemons
 */

#ifndef _SECURITY_AUTHORIZATIONTAGS_H_
#define _SECURITY_AUTHORIZATIONTAGS_H_


/*!
	@header AuthorizationTags
	Draft version 2 01/23/2001

	This header defines some of the supported rights tags to be used in the Authorization API.
*/


/*!
	@define kAuthorizationEnvironmentUsername
	The name of the AuthorizationItem that should be passed into the environment when specifying a username.  The value and valueLength should contain the username itself.
*/
#define kAuthorizationEnvironmentUsername  "username"

/*!
	@define kAuthorizationEnvironmentPassword
	The name of the AuthorizationItem that should be passed into the environment when specifying a password for a given username.  The value and valueLength should contain the actual password data.
*/
#define kAuthorizationEnvironmentPassword  "password"

/*!
	@define kAuthorizationEnvironmentShared
	The name of the AuthorizationItem that should be passed into the environment when specifying a username and password.  Adding this entry to the environment will cause the username/password to be added to the shared credential pool of the calling applications session.  This means that further calls by other applications in this session will automatically have this credential availible to them.  The value is ignored.
*/
#define kAuthorizationEnvironmentShared  "shared"

/*!
	@define kAuthorizationRightExecute
	The name of the AuthorizationItem that should be passed into the rights when preauthorizing for a call to AuthorizationExecuteWithPrivileges().
	
	You need to aquire this right to be able to perform a AuthorizationExecuteWithPrivileges() operation.  In addtion to this right you should obtain whatever rights the tool you are executing with privileges need to perform it's operation on your behalf.  Currently no options are supported but you should pass in the full path of the tool you wish to execute in the value and valueLength fields.  In the future we will limit the right to only execute the requested path, and we will display this information to the user.
*/
#define kAuthorizationRightExecute "system.privilege.admin"

#endif /* !_SECURITY_AUTHORIZATIONTAGS_H_ */
