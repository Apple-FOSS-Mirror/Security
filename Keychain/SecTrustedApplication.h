/*
 * Copyright (c) 2002 Apple Computer, Inc. All Rights Reserved.
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

/*!
	@header SecTrustedApplication
	The functions provided in SecTrustedApplication implement an object representing an application in a
	SecAccess object.
*/

#ifndef _SECURITY_SECTRUSTEDAPPLICATION_H_
#define _SECURITY_SECTRUSTEDAPPLICATION_H_

#include <Security/SecBase.h>
#include <CoreFoundation/CoreFoundation.h>


#if defined(__cplusplus)
extern "C" {
#endif

/*!
	@function SecTrustedApplicationGetTypeID
	@abstract Returns the type identifier of SecTrustedApplication instances.
	@result The CFTypeID of SecTrustedApplication instances.
*/
CFTypeID SecTrustedApplicationGetTypeID(void);

/*!
	@function SecTrustedApplicationCreateFromPath
    @abstract Creates a trusted application reference based on the trusted application specified by path.
    @param path The path to the application or tool to trust. For application bundles, use the
		path to the bundle directory. Pass NULL to refer to yourself, i.e. the application or tool
		making this call.
    @param app On return, a pointer to the trusted application reference.
    @result A result code.  See "Security Error Codes" (SecBase.h).
*/
OSStatus SecTrustedApplicationCreateFromPath(const char *path, SecTrustedApplicationRef *app);

/*!
	@function SecTrustedApplicationCopyData
	@abstract Retrieves the data of a given trusted application reference
	@param appRef A trusted application reference to retrieve data from
	@param data On return, a pointer to a data reference of the trusted application.
	@result A result code.  See "Security Error Codes" (SecBase.h).
*/
OSStatus SecTrustedApplicationCopyData(SecTrustedApplicationRef appRef, CFDataRef *data);

/*!
	@function SecTrustedApplicationSetData
	@abstract Sets the data of a given trusted application reference
	@param appRef A trusted application reference.
	@param data A reference to the data to set in the trusted application.
	@result A result code.  See "Security Error Codes" (SecBase.h).
*/
OSStatus SecTrustedApplicationSetData(SecTrustedApplicationRef appRef, CFDataRef data);


#if defined(__cplusplus)
}
#endif

#endif /* !_SECURITY_SECTRUSTEDAPPLICATION_H_ */
