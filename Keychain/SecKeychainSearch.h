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

/*!
	@header SecKeychainSearch
	The functions provided in SecKeychainSearch implement a query of one or more keychains to search for a particular SecKeychainItem.
*/

#ifndef _SECURITY_SECKEYCHAINSEARCH_H_
#define _SECURITY_SECKEYCHAINSEARCH_H_

#include <Security/SecKeychainItem.h>


#if defined(__cplusplus)
extern "C" {
#endif

/*!
	@function SecKeychainSearchGetTypeID
	@abstract Returns the type identifier of SecKeychainSearch instances.
	@result The CFTypeID of SecKeychainSearch instances.
*/
CFTypeID SecKeychainSearchGetTypeID(void);

/*!
	@function SecKeychainSearchCreateFromAttributes
	@abstract Creates a search reference matching a list of zero or more specified attributes in the specified keychain.
    @param keychainOrArray An reference to an array of keychains to search, a single keychain or NULL to search the user's default keychain search list.
	@param itemClass The keychain item class.
	@param attrList A pointer to a list of zero or more keychain attribute records to match.  Pass NULL to match any keychain attribute.
	@param searchRef On return, a pointer to the current search reference. You are responsible for calling the CFRelease function to release this reference when finished with it.
    @result A result code.  See "Security Error Codes" (SecBase.h).
*/
OSStatus SecKeychainSearchCreateFromAttributes(CFTypeRef keychainOrArray, SecItemClass itemClass, const SecKeychainAttributeList *attrList, SecKeychainSearchRef *searchRef);

/*!
	@function SecKeychainSearchCopyNext
	@abstract Finds the next keychain item matching the given search criteria.
	@param searchRef A reference to the current search criteria.  The search reference is created in the SecKeychainSearchCreateFromAttributes function and must be released by calling the CFRelease function when you are done with it.
	@param itemRef On return, a pointer to a keychain item reference of the next matching keychain item, if any.  	
	@result A result code.  When there are no more items that match the parameters specified to SecPolicySearchCreate, errSecItemNotFound is returned. See "Security Error Codes" (SecBase.h).
*/
OSStatus SecKeychainSearchCopyNext(SecKeychainSearchRef searchRef, SecKeychainItemRef *itemRef);

#if defined(__cplusplus)
}
#endif

#endif /* !_SECURITY_SECKEYCHAINSEARCH_H_ */
