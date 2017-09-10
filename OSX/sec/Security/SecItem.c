/*
 * Copyright (c) 2006-2014 Apple Inc. All Rights Reserved.
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
 * SecItem.c - CoreFoundation-based constants and functions for
    access to Security items (certificates, keys, identities, and
    passwords.)
 */

#include <Security/SecBasePriv.h>
#include <Security/SecItem.h>
#include <Security/SecItemPriv.h>
#include <Security/SecItemInternal.h>
#include <Security/SecItemShim.h>
#include <Security/SecAccessControl.h>
#include <Security/SecAccessControlPriv.h>
#include <Security/SecKey.h>
#include <Security/SecKeyPriv.h>
#include <Security/SecCertificateInternal.h>
#include <Security/SecIdentity.h>
#include <Security/SecIdentityPriv.h>
#include <Security/SecRandom.h>
#include <Security/SecBasePriv.h>
#include <Security/SecCTKKeyPriv.h>
#include <Security/SecTask.h>
#include <Security/SecPolicyInternal.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <Security/SecBase.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFDate.h>
#include <CoreFoundation/CFDictionary.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFURL.h>
#include <CommonCrypto/CommonDigest.h>
#include <libkern/OSByteOrder.h>
#include <corecrypto/ccder.h>
#include <utilities/array_size.h>
#include <utilities/debugging.h>
#include <utilities/SecCFError.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecIOFormat.h>
#include <utilities/SecXPCError.h>
#include <utilities/der_plist.h>
#include <utilities/der_plist_internal.h>
#include <assert.h>
#include <dlfcn.h>
#include <libaks_acl_cf_keys.h>
#include <os/activity.h>
#include <Security/SecureObjectSync/SOSTransportMessageIDS.h>
#include <pthread.h>

#include <Security/SecInternal.h>
#include "SOSInternal.h"
#include <TargetConditionals.h>
#include <ipc/securityd_client.h>
#include <Security/SecuritydXPC.h>
#include <AssertMacros.h>
#include <asl.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <libDER/asn1Types.h>

#include <utilities/SecDb.h>
#include <IOKit/IOReturn.h>

#include <coreauthd_spi.h>
#include <LocalAuthentication/LAPrivateDefines.h>
#include <LocalAuthentication/LACFSupport.h>

#include <ctkclient.h>

/* Return an OSStatus for a sqlite3 error code. */
static OSStatus osstatus_for_s3e(int s3e)
{
	if (s3e > 0 && s3e <= SQLITE_DONE) switch (s3e)
	{
        case SQLITE_OK:
            return 0;
        case SQLITE_ERROR:
            return errSecNotAvailable; /* errSecDuplicateItem; */
        case SQLITE_FULL: /* Happens if we run out of uniqueids */
            return errSecNotAvailable; /* TODO: Replace with a better error code. */
        case SQLITE_PERM:
        case SQLITE_READONLY:
            return errSecNotAvailable;
        case SQLITE_CANTOPEN:
            return errSecNotAvailable;
        case SQLITE_EMPTY:
            return errSecNotAvailable;
        case SQLITE_CONSTRAINT:
            return errSecDuplicateItem;
        case SQLITE_ABORT:
            return -1;
        case SQLITE_MISMATCH:
            return errSecNoSuchAttr;
        case SQLITE_AUTH:
            return errSecNotAvailable;
        case SQLITE_NOMEM:
            return -2; /* TODO: Replace with a real error code. */
        case SQLITE_INTERNAL:
        default:
            return errSecNotAvailable; /* TODO: Replace with a real error code. */
	}
    return s3e;
}

static OSStatus osstatus_for_kern_return(CFIndex kernResult)
{
	switch (kernResult)
	{
        case KERN_SUCCESS:
            return errSecSuccess;
        case kIOReturnNotReadable:
        case kIOReturnNotWritable:
            return errSecAuthFailed;
        case kIOReturnNotPermitted:
        case kIOReturnNotPrivileged:
        case kIOReturnLockedRead:
        case kIOReturnLockedWrite:
            return errSecInteractionNotAllowed;
        case kIOReturnError:
            return errSecDecode;
        case kIOReturnBadArgument:
            return errSecParam;
        default:
            return errSecNotAvailable; /* TODO: Replace with a real error code. */
	}
}

static OSStatus osstatus_for_xpc_error(CFIndex xpcError) {
    switch (xpcError)
	{
        case kSecXPCErrorSuccess:
            return errSecSuccess;
        case kSecXPCErrorUnexpectedType:
        case kSecXPCErrorUnexpectedNull:
            return errSecParam;
        case kSecXPCErrorConnectionFailed:
            return errSecNotAvailable;
        case kSecXPCErrorUnknown:
        default:
            return errSecInternal;
    }
}

static OSStatus osstatus_for_der_error(CFIndex derError) {
    switch (derError)
	{
        case kSecDERErrorUnknownEncoding:
        case kSecDERErrorUnsupportedDERType:
        case kSecDERErrorUnsupportedNumberType:
            return errSecDecode;
        case kSecDERErrorUnsupportedCFObject:
            return errSecParam;
        case kSecDERErrorAllocationFailure:
            return errSecAllocate;
        default:
            return errSecInternal;
    }
}

static OSStatus osstatus_for_ids_error(CFIndex idsError) {
    switch (idsError)
    {
        case kSecIDSErrorNoDeviceID:
            return errSecDeviceIDNeeded;
        case kSecIDSErrorNotRegistered:
            return errSecIDSNotRegistered;
        case kSecIDSErrorFailedToSend:
            return errSecFailedToSendIDSMessage;
        case kSecIDSErrorCouldNotFindMatchingAuthToken:
            return errSecDeviceIDNoMatch;
        case kSecIDSErrorNoPeersAvailable:
            return errSecPeersNotAvailable;
        default:
            return errSecInternal;
    }
}

static OSStatus osstatus_for_localauthentication_error(CFIndex laError) {
    // Wrap LA error in Sec error.
    switch (laError) {
        case kLAErrorUserCancel:
            return errSecUserCanceled;
        case kLAErrorParameter:
            return errSecParam;
        case kLAErrorNotInteractive:
            return errSecInteractionNotAllowed;
        default:
            return errSecAuthFailed;
    }
}

static OSStatus osstatus_for_ctk_error(CFIndex ctkError) {
    switch (ctkError) {
        case kTKErrorCodeBadParameter:
            return errSecParam;
        case kTKErrorCodeNotImplemented:
            return errSecUnimplemented;
        case kTKErrorCodeCanceledByUser:
            return errSecUserCanceled;
        case kTKErrorCodeCorruptedData:
            return errSecDecode;
        default:
            return errSecInternal;
    }
}


// Convert from securityd error codes to OSStatus for legacy API.
OSStatus SecErrorGetOSStatus(CFErrorRef error) {
    OSStatus status;
    if (error == NULL) {
        status = errSecSuccess;
    } else {
        CFStringRef domain = CFErrorGetDomain(error);
        if (domain == NULL) {
            secerror("No error domain for error: %@", error);
            status = errSecInternal;
        } else if (CFEqual(kSecErrorDomain, domain)) {
            status = (OSStatus)CFErrorGetCode(error);
        } else if (CFEqual(kSecDbErrorDomain, domain)) {
            status = osstatus_for_s3e((int)CFErrorGetCode(error));
        } else if (CFEqual(kSecErrnoDomain, domain)) {
            status = (OSStatus)CFErrorGetCode(error);
        } else if (CFEqual(kSecKernDomain, domain)) {
            status = osstatus_for_kern_return(CFErrorGetCode(error));
        } else if (CFEqual(sSecXPCErrorDomain, domain)) {
            status = osstatus_for_xpc_error(CFErrorGetCode(error));
        } else if (CFEqual(sSecDERErrorDomain, domain)) {
            status = osstatus_for_der_error(CFErrorGetCode(error));
        } else if (CFEqual(kSecIDSErrorDomain, domain)) {
            status = osstatus_for_ids_error(CFErrorGetCode(error));
        } else if (CFEqual(CFSTR(kLAErrorDomain), domain)) {
            status = osstatus_for_localauthentication_error(CFErrorGetCode(error));
        } else if (CFEqual(CFSTR(kTKErrorDomain), domain)) {
            status = osstatus_for_ctk_error(CFErrorGetCode(error));
        } else if (CFEqual(kSOSErrorDomain, domain)) {
            status = errSecInternal;
        } else {
            secnotice("securityd", "unknown error domain: %@ for error: %@", domain, error);
            status = errSecInternal;
        }
    }
    return status;
}

static void
lastErrorReleaseError(void *value)
{
    if (value)
        CFRelease(value);
}

static bool
getLastErrorKey(pthread_key_t *kv)
{
    static pthread_key_t key;
    static bool haveKey = false;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        if (pthread_key_create(&key, lastErrorReleaseError) == 0)
            haveKey = true;
    });
    *kv = key;
    return haveKey;
}

static void
SetLastError(CFErrorRef newError)
{
    pthread_key_t key;
    if (!getLastErrorKey(&key))
        return;
    CFErrorRef oldError = pthread_getspecific(key);
    if (oldError)
        CFRelease(oldError);
    if (newError)
        CFRetain(newError);
    pthread_setspecific(key, newError);
}

CFErrorRef
SecCopyLastError(OSStatus status)
{
    pthread_key_t key;
    CFErrorRef error;

    if (!getLastErrorKey(&key))
        return NULL;

    error = pthread_getspecific(key);
    if (error) {
        if (status && status != SecErrorGetOSStatus(error)) {
            error = NULL;
        } else {
            CFRetain(error);
        }
    }
    return error;
}

// Wrapper to provide a CFErrorRef for legacy API.
OSStatus SecOSStatusWith(bool (^perform)(CFErrorRef *error)) {
    CFErrorRef error = NULL;
    OSStatus status;
    if (perform(&error)) {
        assert(error == NULL);
        SetLastError(NULL);
        status = errSecSuccess;
    } else {
        assert(error);
        SetLastError(error);
        status = SecErrorGetOSStatus(error);
        if (status != errSecItemNotFound)           // Occurs in normal operation, so exclude
            secinfo("OSStatus", "error:[%" PRIdOSStatus "] %@", status, error);
        CFReleaseNull(error);
    }
    return status;
}

/* Drop assorted kSecAttrCanXxxx attributes from the query, because these attributes are generated
   by SecKey implementation and may differ between OS versions, see <rdar://problem/27095761>.
 */

static CFDictionaryRef
AttributeCreateFilteredOutSecAttrs(CFDictionaryRef attributes)
{
    CFMutableDictionaryRef filtered = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, attributes);
    if (filtered == NULL)
        return NULL;
    CFDictionaryRemoveValue(filtered, kSecAttrCanSign);
    CFDictionaryRemoveValue(filtered, kSecAttrCanVerify);
    CFDictionaryRemoveValue(filtered, kSecAttrCanEncrypt);
    CFDictionaryRemoveValue(filtered, kSecAttrCanDecrypt);
    CFDictionaryRemoveValue(filtered, kSecAttrCanDerive);
    CFDictionaryRemoveValue(filtered, kSecAttrCanWrap);
    CFDictionaryRemoveValue(filtered, kSecAttrCanUnwrap);
    CFDictionaryRemoveValue(filtered, kSecAttrCanSignRecover);
    CFDictionaryRemoveValue(filtered, kSecAttrCanVerifyRecover);

    return filtered;
}


/* IPC uses CFPropertyList to un/marshall input/output data and can handle:
   CFData, CFString, CFArray, CFDictionary, CFDate, CFBoolean, and CFNumber

   Currently in need of conversion below:
   @@@ kSecValueRef allows SecKeychainItemRef and SecIdentityRef
   @@@ kSecUseItemList allows a query against a list of itemrefs, this isn't
       currently implemented at all, but when it is needs to short circuit to
	   local evaluation, different from the sql query abilities
*/

static CFDictionaryRef
SecItemCopyAttributeDictionary(CFTypeRef ref, bool forQuery) {
	CFDictionaryRef refDictionary = NULL;
	CFTypeID typeID = CFGetTypeID(ref);
	if (typeID == SecKeyGetTypeID()) {
		refDictionary = SecKeyCopyAttributeDictionary((SecKeyRef)ref);
        if (refDictionary && forQuery) {
            CFDictionaryRef filtered = AttributeCreateFilteredOutSecAttrs(refDictionary);
            CFAssignRetained(refDictionary, filtered);
        }
	} else if (typeID == SecCertificateGetTypeID()) {
		refDictionary = SecCertificateCopyAttributeDictionary((SecCertificateRef)ref);
	} else if (typeID == SecIdentityGetTypeID()) {
        assert(false);
        SecIdentityRef identity = (SecIdentityRef)ref;
        SecCertificateRef cert = NULL;
        SecKeyRef key = NULL;
        if (!SecIdentityCopyCertificate(identity, &cert) &&
            !SecIdentityCopyPrivateKey(identity, &key)) 
        {
            CFDataRef data = SecCertificateCopyData(cert);
            CFDictionaryRef key_dict = SecKeyCopyAttributeDictionary(key);
            
            if (key_dict && data) {
                refDictionary = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, key_dict);
                CFDictionarySetValue((CFMutableDictionaryRef)refDictionary, kSecAttrIdentityCertificateData, data);
            }
            CFReleaseNull(key_dict);
            CFReleaseNull(data);
        }
        CFReleaseNull(cert);
        CFReleaseNull(key);
	}
	return refDictionary;
}

#ifdef SECITEM_SHIM_OSX
extern CFTypeRef SecItemCreateFromAttributeDictionary_osx(CFDictionaryRef refAttributes);
#endif

static CFTypeRef
SecItemCreateFromAttributeDictionary(CFDictionaryRef refAttributes) {
	CFTypeRef ref = NULL;
	CFStringRef class = CFDictionaryGetValue(refAttributes, kSecClass);
    if (CFEqual(class, kSecClassKey)) {
        ref = SecKeyCreateFromAttributeDictionary(refAttributes);
    } else if (CFEqual(class, kSecClassCertificate)) {
		ref = SecCertificateCreateFromAttributeDictionary(refAttributes);
	} else if (CFEqual(class, kSecClassIdentity)) {
		CFDataRef data = CFDictionaryGetValue(refAttributes, kSecAttrIdentityCertificateData);
		SecCertificateRef cert = SecCertificateCreateWithData(kCFAllocatorDefault, data);
		SecKeyRef key = SecKeyCreateFromAttributeDictionary(refAttributes);
		if (key && cert)
			ref = SecIdentityCreate(kCFAllocatorDefault, cert, key);
		CFReleaseSafe(cert);
		CFReleaseSafe(key);
#ifdef SECITEM_SHIM_OSX
	} else {
        ref = SecItemCreateFromAttributeDictionary_osx(refAttributes);
#endif
	}
	return ref;
}

OSStatus
SecItemCopyDisplayNames(CFArrayRef items, CFArrayRef *displayNames)
{
    // @@@ TBI
    return -1 /* errSecUnimplemented */;
}

typedef OSStatus (*secitem_operation)(CFDictionaryRef attributes, CFTypeRef *result);

static bool explode_identity(CFDictionaryRef attributes, secitem_operation operation, 
    OSStatus *return_status, CFTypeRef *return_result)
{
    bool handled = false;
	CFTypeRef value = CFDictionaryGetValue(attributes, kSecValueRef);
    if (value) {
        CFTypeID typeID = CFGetTypeID(value);
        if (typeID == SecIdentityGetTypeID()) {
            handled = true;
            OSStatus status = errSecSuccess;
            SecIdentityRef identity = (SecIdentityRef)value;
            SecCertificateRef cert = NULL;
            SecKeyRef key = NULL;
            if (!SecIdentityCopyCertificate(identity, &cert) &&
                !SecIdentityCopyPrivateKey(identity, &key)) 
            {
                CFMutableDictionaryRef partial_query =
                    CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, attributes);
                CFDictionarySetValue(partial_query, kSecValueRef, cert);
                CFTypeRef result = NULL;
                bool duplicate_cert = false;
                /* an identity is first and foremost a key, but it can have multiple
                   certs associated with it: so we identify it by the cert */
                status = operation(partial_query, return_result ? &result : NULL);
                if ((operation == (secitem_operation)SecItemAdd) &&
                    (status == errSecDuplicateItem)) {
                        duplicate_cert = true;
                        status = errSecSuccess;
                }

                if (!status || status == errSecItemNotFound) {
					bool skip_key_operation = false;
	
					/* if the key is still in use, skip deleting it */
					if (operation == (secitem_operation)SecItemDelete) {
						// find certs with cert.pkhh == keys.klbl
						CFDictionaryRef key_dict = NULL, query_dict = NULL;
						CFDataRef pkhh = NULL;
						
						key_dict = SecKeyCopyAttributeDictionary(key);
						if (key_dict)
							pkhh = (CFDataRef)CFDictionaryGetValue(key_dict, kSecAttrApplicationLabel);
						const void *keys[] = { kSecClass, kSecAttrPublicKeyHash };
						const void *vals[] = { kSecClassCertificate, pkhh };
						if (pkhh)
							query_dict = CFDictionaryCreate(NULL, keys, 
					            vals, (array_size(keys)),
								NULL, NULL);
						if (query_dict)
							if (errSecSuccess == SecItemCopyMatching(query_dict, NULL))
								skip_key_operation = true;
						CFReleaseSafe(query_dict);
						CFReleaseSafe(key_dict);
					}
					
					if (!skip_key_operation) {
	                    /* now perform the operation for the key */
	                    CFDictionarySetValue(partial_query, kSecValueRef, key);
	                    CFDictionarySetValue(partial_query, kSecReturnPersistentRef, kCFBooleanFalse);

	                    status = operation(partial_query, NULL);
	                    if ((operation == (secitem_operation)SecItemAdd) &&
	                        (status == errSecDuplicateItem) &&
	                        !duplicate_cert)
	                            status = errSecSuccess;
					}
					
                    /* add and copy matching for an identityref have a persistent ref result */
                    if (result) {
                        if (!status) {
                            /* result is a persistent ref to a cert */
                            sqlite_int64 rowid;
                            if (_SecItemParsePersistentRef(result, NULL, &rowid)) {
                                *return_result = _SecItemMakePersistentRef(kSecClassIdentity, rowid);
                            }
                        }
                        CFRelease(result);
                    }
                }
                CFReleaseNull(partial_query);
            }
            else
                status = errSecInvalidItemRef;
            
            CFReleaseNull(cert);
            CFReleaseNull(key);
            *return_status = status;
        }
    } else {
		value = CFDictionaryGetValue(attributes, kSecClass);
		if (value && CFEqual(kSecClassIdentity, value) && 
			(operation == (secitem_operation)SecItemDelete)) {
			CFMutableDictionaryRef dict = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, attributes);
			CFDictionaryRemoveValue(dict, kSecClass);
			CFDictionarySetValue(dict, kSecClass, kSecClassCertificate);
			OSStatus status = SecItemDelete(dict);
			if (!status) {
				CFDictionarySetValue(dict, kSecClass, kSecClassKey);
				status = SecItemDelete(dict);
			}
			CFRelease(dict);
			*return_status = status;
            handled = true;
		}
	}
    return handled;
}

static bool
SecErrorPropagateLastError(OSStatus status, CFErrorRef *error)
{
    if (status) {
        CFErrorRef lastError = SecCopyLastError(status);
        if (lastError)
            CFErrorPropagate(lastError, error);
        else
            SecError(status, error, CFSTR("SecError: error not captured, OSStatus was: %d"), (int)status);
        return false;
    }
    return true;
}

static bool
handleUpdateIdentity(CFDictionaryRef query,
                     CFDictionaryRef update,
                     bool *result,
                     CFErrorRef *error)
{
    CFMutableDictionaryRef updatedQuery = NULL;
    SecCertificateRef cert = NULL;
    SecKeyRef key = NULL;
    bool handled = false;

    *result = false;

    CFTypeRef value = CFDictionaryGetValue(query, kSecValueRef);
    if (value) {
        CFTypeID typeID = CFGetTypeID(value);
        if (typeID == SecIdentityGetTypeID()) {
            SecIdentityRef identity = (SecIdentityRef)value;
            OSStatus status;

            handled = true;

            status = SecIdentityCopyCertificate(identity, &cert);
            require_noerr_action_quiet(status, errOut, SecErrorPropagateLastError(status, error));

            status = SecIdentityCopyPrivateKey(identity, &key);
            require_noerr_action_quiet(status, errOut, SecErrorPropagateLastError(status, error));

            updatedQuery = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, query);
            require_action_quiet(updatedQuery, errOut, *result = false);

            CFDictionarySetValue(updatedQuery, kSecValueRef, cert);
            require_quiet(SecItemUpdateWithError(updatedQuery, update, error), errOut);

            CFDictionarySetValue(updatedQuery, kSecValueRef, key);
            require_quiet(SecItemUpdateWithError(updatedQuery, update, error), errOut);

        }
    } else {
        value = CFDictionaryGetValue(query, kSecClass);
        if (value && CFEqual(kSecClassIdentity, value)) {
            handled = true;

            updatedQuery = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, query);
            require_action_quiet(updatedQuery, errOut, *result = false);

            CFDictionarySetValue(updatedQuery, kSecClass, kSecClassCertificate);
            require_quiet(SecItemUpdateWithError(updatedQuery, update, error), errOut);

            CFDictionarySetValue(updatedQuery, kSecClass, kSecClassKey);
            require_quiet(SecItemUpdateWithError(updatedQuery, update, error), errOut);

            CFReleaseNull(updatedQuery);
        }
    }
    *result = true;
errOut:
    CFReleaseNull(updatedQuery);
    CFReleaseNull(cert);
    CFReleaseNull(key);
    return handled;
}

static void infer_cert_label(SecCFDictionaryCOW *attributes)
{
    if (!CFDictionaryContainsKey(attributes->dictionary, kSecAttrLabel)) {
        CFTypeRef value_ref = CFDictionaryGetValue(attributes->dictionary, kSecValueRef);
        if (value_ref && CFGetTypeID(value_ref) == SecCertificateGetTypeID()) {
            SecCertificateRef certificate = (SecCertificateRef)value_ref;
            CFStringRef label = SecCertificateCopySubjectSummary(certificate);
            if (label) {
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(attributes), kSecAttrLabel, label);
                CFReleaseNull(label);
            }
        }
    }
}

/* A persistent ref is just the class and the rowid of the record. */
CF_RETURNS_RETAINED CFDataRef _SecItemMakePersistentRef(CFTypeRef class, sqlite_int64 rowid)
{
    uint8_t bytes[sizeof(sqlite_int64) + 4];
    if (rowid < 0)
        return NULL;
    if (CFStringGetCString(class, (char *)bytes, 4 + 1 /*null-term*/, 
        kCFStringEncodingUTF8))
    {
        OSWriteBigInt64(bytes + 4, 0, rowid);
        return CFDataCreate(NULL, bytes, sizeof(bytes));
    }
    return NULL;
}

/* AUDIT[securityd](done):
   persistent_ref (ok) is a caller provided, non NULL CFTypeRef.
 */
bool _SecItemParsePersistentRef(CFDataRef persistent_ref, CFStringRef *return_class, sqlite_int64 *return_rowid)
{
	bool valid_ref = false;
    if (CFGetTypeID(persistent_ref) == CFDataGetTypeID() &&
        CFDataGetLength(persistent_ref) == (CFIndex)(sizeof(sqlite_int64) + 4)) {
        const uint8_t *bytes = CFDataGetBytePtr(persistent_ref);
        sqlite_int64 rowid = OSReadBigInt64(bytes + 4, 0);
		
        CFStringRef class = CFStringCreateWithBytes(kCFAllocatorDefault, 
            bytes, CFStringGetLength(kSecClassGenericPassword), 
            kCFStringEncodingUTF8, true);
        const void *valid_classes[] = { kSecClassGenericPassword,
            kSecClassInternetPassword,
            kSecClassAppleSharePassword,
            kSecClassCertificate,
            kSecClassKey,
            kSecClassIdentity };
        
        unsigned i;
        for (i=0; i< array_size(valid_classes); i++) {
            if (CFEqual(valid_classes[i], class)) {
                if (return_class)
                    *return_class = valid_classes[i];
                if (return_rowid)
                    *return_rowid = rowid;
                valid_ref = true;
                break;
            }
        }
        CFRelease(class);
    }
    return valid_ref;
}

static bool cf_bool_value(CFTypeRef cf_bool)
{
	return (cf_bool && CFEqual(kCFBooleanTrue, cf_bool));
}

CFMutableDictionaryRef SecCFDictionaryCOWGetMutable(SecCFDictionaryCOW *cow_dictionary) {
    if (cow_dictionary->mutable_dictionary == NULL) {
        cow_dictionary->mutable_dictionary = CFDictionaryCreateMutableForCFTypes(NULL);
        if (cow_dictionary->dictionary != NULL) {
            CFDictionaryForEach(cow_dictionary->dictionary, ^(const void *key, const void *value) {
                CFDictionarySetValue(cow_dictionary->mutable_dictionary, key, value);
            });
        }
        cow_dictionary->dictionary = cow_dictionary->mutable_dictionary;
    }

    return cow_dictionary->mutable_dictionary;
}

// Keys for dictionary of kSecvalueData of token-based items.
static const CFStringRef kSecTokenValueObjectIDKey = CFSTR("oid");
static const CFStringRef kSecTokenValueAccessControlKey = CFSTR("ac");
static const CFStringRef kSecTokenValueDataKey = CFSTR("data");

// Creates kSecValueData field stored in the DB for token-based items.  Data field consists of objectID, real
// access_control and optionally of the data value.
static CFDataRef SecTokenItemValueCreate(CFDataRef oid, CFDataRef access_control, CFDataRef object_value, CFErrorRef *error) {
    CFMutableDictionaryRef value = NULL;
    value = CFDictionaryCreateMutableForCFTypesWith(NULL,
                                                    kSecTokenValueObjectIDKey, oid,
                                                    kSecTokenValueAccessControlKey, access_control,
                                                    NULL);
    if (object_value != NULL) {
        CFDictionarySetValue(value, kSecTokenValueDataKey, object_value);
    }

    CFDataRef value_data = CFPropertyListCreateDERData(NULL, value, error);
    CFRelease(value);
    return value_data;
}

static CFDictionaryRef SecTokenItemValueCopy(CFDataRef db_value, CFErrorRef *error) {
    CFPropertyListRef plist = NULL;
    const uint8_t *der = CFDataGetBytePtr(db_value);
    const uint8_t *der_end = der + CFDataGetLength(db_value);
    require_quiet(der = der_decode_plist(0, kCFPropertyListImmutable, &plist, error, der, der_end), out);
    require_action_quiet(der == der_end, out, SecError(errSecDecode, error, CFSTR("trailing garbage at end of token data field")));
    require_action_quiet(CFDictionaryGetValue(plist, kSecTokenValueObjectIDKey) != NULL, out,
                         SecError(errSecInternal, error, CFSTR("token based item data does not have OID")));

out:
    return plist;
}

CFDataRef _SecTokenItemCopyValueData(CFDataRef db_value, CFErrorRef *error) {
    CFDataRef valueData = NULL;
    CFDictionaryRef itemDict = NULL;
    require_quiet(itemDict = SecTokenItemValueCopy(db_value, error), out);
    CFRetainAssign(valueData, CFDictionaryGetValue(itemDict, kSecTokenValueDataKey));
    require_action_quiet(valueData, out, SecError(errSecInternal, error, CFSTR("token item does not contain value data")));

out:
    CFReleaseSafe(itemDict);
    return valueData;
}

TKTokenRef SecTokenCreate(CFStringRef token_id, CFDictionaryRef auth_params, CFErrorRef *error) {
    CFMutableDictionaryRef token_attrs = NULL;
    TKTokenRef token = NULL;
    token_attrs = (auth_params != NULL) ?
        CFDictionaryCreateMutableCopy(NULL, 0, auth_params) :
        CFDictionaryCreateMutableForCFTypes(NULL);
    CFDictionarySetValue(token_attrs, kSecAttrTokenID, token_id);

    CFDictionaryRemoveValue(token_attrs, kSecUseAuthenticationContext);
    token = TKTokenCreate(token_attrs, error);

    CFReleaseSafe(token_attrs);
    return token;
}

static bool SecTokenItemCreateFromAttributes(CFDictionaryRef attributes, CFDictionaryRef auth_params,
                                             TKTokenRef token, CFDataRef object_id, CFTypeRef *ref, CFErrorRef *error) {
    bool ok = false;
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutableCopy(NULL, 0, attributes);
    CFTypeRef token_id = CFDictionaryGetValue(attributes, kSecAttrTokenID);
    if (token_id != NULL && object_id != NULL) {
        if (CFRetainSafe(token) == NULL) {
            require_quiet(token = SecTokenCreate(token_id, auth_params, error), out);
        }

        if (auth_params != NULL) {
            CFDictionaryForEach(auth_params, ^(const void *key, const void *value) {
                CFDictionarySetValue(attrs, key, value);
            });
        }
        CFDictionarySetValue(attrs, kSecUseToken, token);
        CFDictionarySetValue(attrs, kSecUseTokenObjectID, object_id);
        CFRelease(token);
    }
    *ref = SecItemCreateFromAttributeDictionary(attrs);
    ok = true;

out:
    CFReleaseSafe(attrs);
    return ok;
}


/* Turn the returned single value or dictionary that contains all the attributes to create a
 ref into the exact result the client asked for */
static bool SecItemResultCopyPrepared(CFTypeRef raw_result, TKTokenRef token,
                                      CFDictionaryRef query, CFDictionaryRef auth_params,
                                      CFTypeRef *result, CFErrorRef *error) {
    bool ok = false;
    CFDataRef ac_data = NULL;
    CFDataRef value = NULL;
    CFTypeRef persistent_ref = NULL;
    CFStringRef token_id = NULL;
    CFStringRef cert_token_id = NULL;
    CFDataRef object_id = NULL;
    CFMutableDictionaryRef attrs = NULL;
    CFDataRef cert_data = NULL;
    CFDataRef cert_object_id = NULL;
    TKTokenRef cert_token = NULL;

    bool wants_ref = cf_bool_value(CFDictionaryGetValue(query, kSecReturnRef));
    bool wants_data = cf_bool_value(CFDictionaryGetValue(query, kSecReturnData));
    bool wants_attributes = cf_bool_value(CFDictionaryGetValue(query, kSecReturnAttributes));
    bool wants_persistent_ref = cf_bool_value(CFDictionaryGetValue(query, kSecReturnPersistentRef));

    // Get token value if not provided by the caller.
    bool token_item = false;
    bool cert_token_item = false;
    if (token == NULL) {
        if (CFGetTypeID(raw_result) == CFDictionaryGetTypeID()) {
            token_id = CFDictionaryGetValue(raw_result, kSecAttrTokenID);
            token_item = (token_id != NULL);

            cert_token_id = CFDictionaryGetValue(raw_result, kSecAttrIdentityCertificateTokenID);
            cert_token_item = (cert_token_id != NULL);
        }
    } else {
        token_item = true;
        cert_token_item = true;
        CFRetain(token);
        CFRetainAssign(cert_token, token);
    }

    // Decode and prepare data value, if it is requested at the output, or if we want attributes from token.
    if (wants_data || wants_ref || (token_item && wants_attributes)) {
        if (CFGetTypeID(raw_result) == CFDictionaryGetTypeID())
            value = CFRetainSafe(CFDictionaryGetValue(raw_result, kSecValueData));
        else
            value = CFRetainSafe(raw_result);
        if (token_item && value != NULL) {
            // Parse token-based item's data field.
            CFDataRef object_value = NULL;
            CFDictionaryRef parsed_value = NULL;
            require_quiet(parsed_value = SecTokenItemValueCopy(value, error), out);
            object_id = CFRetainSafe(CFDictionaryGetValue(parsed_value, kSecTokenValueObjectIDKey));
            ac_data = CFRetainSafe(CFDictionaryGetValue(parsed_value, kSecTokenValueAccessControlKey));
            object_value = CFRetainSafe(CFDictionaryGetValue(parsed_value, kSecTokenValueDataKey));
            CFRelease(parsed_value);
            if ((wants_data || wants_ref) && object_value == NULL) {
                // Retrieve value directly from the token.
                if (token == NULL) {
                    require_quiet(token = SecTokenCreate(token_id, auth_params, error), out);
                }
                require_quiet(object_value = TKTokenCopyObjectData(token, object_id, error), out);
                if (CFEqual(object_value, kCFNull))
                    CFReleaseNull(object_value);
            }
            CFAssignRetained(value, object_value);
        }

        // If only thing requested is data, return them directly.
        if (!(wants_attributes || wants_persistent_ref || wants_ref)) {
            *result = CFRetainSafe(value);
            ok = true;
            goto out;
        }
    }

    // Extract persistent_ref, if caller wants it.
    if (wants_persistent_ref) {
        if (CFGetTypeID(raw_result) == CFDictionaryGetTypeID())
            persistent_ref = CFRetainSafe(CFDictionaryGetValue(raw_result, kSecValuePersistentRef));
        else
            persistent_ref = CFRetainSafe(raw_result);

        // If only thing requested is persistentref, extract it from dictionary if needed and return it.
        if (!(wants_attributes || wants_data || wants_ref)) {
            *result = CFRetainSafe(persistent_ref);
            ok = true;
            goto out;
        }
    }

    if (wants_ref || wants_attributes || (wants_data && wants_persistent_ref)) {
        // For these cases we need output dictionary.
        if (CFGetTypeID(raw_result) == CFDictionaryGetTypeID())
            *result = CFDictionaryCreateMutableCopy(NULL, 0, raw_result);
        else
            *result = CFDictionaryCreateForCFTypes(NULL, NULL);
        CFMutableDictionaryRef output = (CFMutableDictionaryRef)*result;

        if ((wants_data || wants_ref) && value != NULL)
            CFDictionarySetValue(output, kSecValueData, value);
        else
            CFDictionaryRemoveValue(output, kSecValueData);

        if (wants_persistent_ref && persistent_ref != NULL)
            CFDictionarySetValue(output, kSecValuePersistentRef, persistent_ref);
        else
            CFDictionaryRemoveValue(output, kSecValuePersistentRef);

        if ((wants_ref || wants_attributes) && cert_token_item &&
            CFEqualSafe(CFDictionaryGetValue(output, kSecClass), kSecClassIdentity)) {
            // Decode also certdata field of the identity.
            CFDataRef data = CFDictionaryGetValue(output, kSecAttrIdentityCertificateData);
            if (data != NULL) {
                CFDictionaryRef parsed_value;
                require_quiet(parsed_value = SecTokenItemValueCopy(data, error), out);
                cert_data = CFRetainSafe(CFDictionaryGetValue(parsed_value, kSecTokenValueDataKey));
                cert_object_id = CFRetainSafe(CFDictionaryGetValue(parsed_value, kSecTokenValueObjectIDKey));
                CFRelease(parsed_value);
                if (cert_data == NULL) {
                    // Retrieve value directly from the token.
                    if (cert_token == NULL) {
                        require_quiet(cert_token = SecTokenCreate(cert_token_id, auth_params, error), out);
                    }
                    require_quiet(cert_data = TKTokenCopyObjectData(cert_token, cert_object_id, error), out);
                    if (CFEqual(cert_data, kCFNull))
                        CFReleaseNull(cert_data);
                }
                if (cert_data != NULL) {
                    CFDictionarySetValue(output, kSecAttrIdentityCertificateData, cert_data);
                } else {
                    CFDictionaryRemoveValue(output, kSecAttrIdentityCertificateData);
                }
            }
        }

        if (wants_ref) {
            CFTypeRef ref;
            require_quiet(SecTokenItemCreateFromAttributes(output, auth_params, token, object_id, &ref, error), out);
            if (!(wants_attributes || wants_data || wants_persistent_ref)) {
                CFAssignRetained(*result, ref);
            } else if (ref != NULL) {
                CFDictionarySetValue(output, kSecValueRef, ref);
                CFRelease(ref);
                if (!wants_data) {
                    // We could have stored data value previously to make ref creation succeed.
                    // They are not needed any more and in case that caller did not want the data, avoid returning them.
                    CFDictionaryRemoveValue(output, kSecValueData);
                }
            }
        }

        if (wants_attributes) {
            // Convert serialized form of access control to object form.
            if (!token_item) {
                CFRetainAssign(ac_data, CFDictionaryGetValue(output, kSecAttrAccessControl));
            }

            if (ac_data != NULL) {
                SecAccessControlRef ac;
                require_quiet(ac = SecAccessControlCreateFromData(kCFAllocatorDefault, ac_data, error), out);
                CFDictionarySetValue(output, kSecAttrAccessControl, ac);
                CFRelease(ac);
            }
        }
    } else {
        *result = NULL;
    }

    ok = true;

out:
    CFReleaseSafe(cert_object_id);
    CFReleaseSafe(cert_data);
    CFReleaseSafe(ac_data);
    CFReleaseSafe(value);
    CFReleaseSafe(persistent_ref);
    CFReleaseSafe(object_id);
    CFReleaseSafe(attrs);
    CFReleaseSafe(token);
    CFReleaseSafe(cert_token);
    return ok;
}

static bool SecItemResultProcess(CFDictionaryRef query, CFDictionaryRef auth_params, TKTokenRef token,
                                 CFTypeRef raw_result, CFTypeRef *result, CFErrorRef *error) {
    bool ok = false;
    require_action_quiet(raw_result != NULL, out, ok = true);
    require_action_quiet(result != NULL, out, ok = true);

    if (CFGetTypeID(raw_result) == CFArrayGetTypeID()) {
        CFIndex i, count = CFArrayGetCount(raw_result);
        *result = CFArrayCreateMutableForCFTypes(NULL);
        for (i = 0; i < count; i++) {
            CFTypeRef ref;
            require_quiet(SecItemResultCopyPrepared(CFArrayGetValueAtIndex(raw_result, i),
                                                    token, query, auth_params, &ref, error), out);
            if (ref != NULL) {
                CFArrayAppendValue((CFMutableArrayRef)*result, ref);
                CFRelease(ref);
            }
        }
    } else {
        require_quiet(SecItemResultCopyPrepared(raw_result, token, query, auth_params, result, error), out);
    }

    ok = true;

out:
    return ok;
}

CFDataRef SecItemAttributesCopyPreparedAuthContext(CFTypeRef la_context, CFErrorRef *error) {
    void *la_lib = NULL;
    CFDataRef acm_context = NULL;
    require_action_quiet(la_lib = dlopen("/System/Library/Frameworks/LocalAuthentication.framework/LocalAuthentication", RTLD_LAZY), out,
                         SecError(errSecInternal, error, CFSTR("failed to open LocalAuthentication.framework")));
    LAFunctionCopyExternalizedContext fnCopyExternalizedContext = NULL;
    require_action_quiet(fnCopyExternalizedContext = dlsym(la_lib, "LACopyExternalizedContext"), out,
                         SecError(errSecInternal, error, CFSTR("failed to obtain LACopyExternalizedContext")));
    require_action_quiet(acm_context = fnCopyExternalizedContext(la_context), out,
                         SecError(errSecInternal, error, CFSTR("failed to get ACM handle from LAContext")));
out:
    if (la_lib != NULL) {
        dlclose(la_lib);
    }
    return acm_context;
}

static bool SecItemAttributesPrepare(SecCFDictionaryCOW *attrs, bool forQuery, CFErrorRef *error) {
    bool ok = false;
    CFDataRef ac_data = NULL, acm_context = NULL;
    void *la_lib = NULL;

    SecAccessControlRef access_control = (SecAccessControlRef)CFDictionaryGetValue(attrs->dictionary, kSecAttrAccessControl);
    if (access_control != NULL) {
        require_quiet(ac_data = SecAccessControlCopyData(access_control), out);
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(attrs), kSecAttrAccessControl, ac_data);
    }

    const CFTypeRef la_context = CFDictionaryGetValue(attrs->dictionary, kSecUseAuthenticationContext);
    if (la_context) {
        require_action_quiet(!CFDictionaryContainsKey(attrs->dictionary, kSecUseCredentialReference), out,
                             SecError(errSecParam, error, CFSTR("kSecUseAuthenticationContext cannot be used together with kSecUseCredentialReference")));
        require_quiet(acm_context = SecItemAttributesCopyPreparedAuthContext(la_context, error), out);
        CFDictionaryRemoveValue(SecCFDictionaryCOWGetMutable(attrs), kSecUseAuthenticationContext);
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(attrs), kSecUseCredentialReference, acm_context);
    }

    // If a ref was specified we get its attribute dictionary and parse it.
    CFTypeRef value = CFDictionaryGetValue(attrs->dictionary, kSecValueRef);
    if (value) {
        CFDictionaryRef ref_attributes;
        require_action_quiet(ref_attributes = SecItemCopyAttributeDictionary(value, forQuery), out,
                             SecError(errSecValueRefUnsupported, error, CFSTR("unsupported kSecValueRef in query")));

        /* Replace any attributes we already got from the ref with the ones
         from the attributes dictionary the caller passed us.  This allows
         a caller to add an item using attributes from the ref and still
         override some of them in the dictionary directly.  */
        CFMutableDictionaryRef new_query = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, ref_attributes);
        CFRelease(ref_attributes);
        CFDictionaryForEach(attrs->dictionary, ^(const void *key, const void *value) {
            if (!CFEqual(key, kSecValueRef))
                CFDictionarySetValue(new_query, key, value);
        });
        CFAssignRetained(attrs->mutable_dictionary, new_query);
        attrs->dictionary = attrs->mutable_dictionary;
    }

    CFTypeRef policy = CFDictionaryGetValue(attrs->dictionary, kSecMatchPolicy);
    if (policy) {
        require_action_quiet(CFGetTypeID(policy) == SecPolicyGetTypeID(), out,
                             SecError(errSecParam, error, CFSTR("unsupported kSecMatchPolicy in query")));

        CFTypeRef values[] = { policy };
        CFArrayRef policiesArray = CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks);
        xpc_object_t policiesArrayXPC = SecPolicyArrayCopyXPCArray(policiesArray, error);
        CFReleaseSafe(policiesArray);
        require_action_quiet(policiesArrayXPC, out,
                             SecError(errSecInternal, error, CFSTR("Failed to copy XPC policy")));

        CFTypeRef objectReadyForXPC = _CFXPCCreateCFObjectFromXPCObject(policiesArrayXPC);
        xpc_release(policiesArrayXPC);
        require_action_quiet(objectReadyForXPC, out,
                             SecError(errSecInternal, error, CFSTR("Failed to create CFObject from XPC policy")));
        
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(attrs), kSecMatchPolicy, objectReadyForXPC);
        CFRelease(objectReadyForXPC);
    }
    value = CFDictionaryGetValue(attrs->dictionary, kSecAttrIssuer);
    if (value) {
        /* convert DN to canonical issuer, if value is DN (top level sequence) */
        const DERItem name = { (unsigned char *)CFDataGetBytePtr(value), CFDataGetLength(value) };
        DERDecodedInfo content;
        if (!DERDecodeItem(&name, &content) &&
            (content.tag == ASN1_CONSTR_SEQUENCE))
        {
            CFDataRef canonical_issuer = createNormalizedX501Name(kCFAllocatorDefault, &content.content);
            if (canonical_issuer) {
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(attrs), kSecAttrIssuer, canonical_issuer);
                CFRelease(canonical_issuer);
            }
        }
    }

    ok = true;

out:
    if (la_lib != NULL) {
        dlclose(la_lib);
    }
    CFReleaseSafe(ac_data);
    CFReleaseSafe(acm_context);
    return ok;
}

static bool SecItemAuthMaxAttemptsReached(CFArrayRef ac_pairs, CFErrorRef *error)
{
    CFMutableStringRef log_string = CFStringCreateMutable(kCFAllocatorDefault, 0);
    CFArrayRef ac_pair;
    CFArrayForEachC(ac_pairs, ac_pair) {
        CFStringRef acl_hex_string = CFDataCopyHexString(CFArrayGetValueAtIndex(ac_pair, 0));
        CFStringRef str = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("operation: %@ acl:%@\n"), CFArrayGetValueAtIndex(ac_pair, 1), acl_hex_string);
        CFStringAppend(log_string, str);
        CFRelease(acl_hex_string);
        CFRelease(str);
    }

    CFStringRef reason = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("Reached maximum count of authentication attempts\n %@"), log_string);
    SecError(errSecAuthFailed, error, reason);
    __security_simulatecrash(reason, __sec_exception_code_AuthLoop);

    CFRelease(reason);
    CFRelease(log_string);
    return false;
}

bool SecItemAuthDo(SecCFDictionaryCOW *auth_params, CFErrorRef *error, SecItemAuthResult (^perform)(CFDictionaryRef auth_params, CFArrayRef *ac_pairs, CFErrorRef *error)) {
    bool ok = false;
    CFArrayRef ac_pairs = NULL;
    SecCFDictionaryCOW auth_options = { NULL };
    // We need to create shared LAContext for Mail to reduce popups with Auth UI.
    // This app-hack will be removed by:<rdar://problem/28305552>
    // Similar workaround is for Safari, will be removed by fixing <rdar://problem/29683072>
    static CFTypeRef sharedLAContext = NULL;
    static CFDataRef sharedACMContext = NULL;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        CFBundleRef bundle = CFBundleGetMainBundle();
        CFStringRef bundleName = (bundle != NULL) ? CFBundleGetIdentifier(bundle) : NULL;
        if (CFEqualSafe(bundleName, CFSTR("com.apple.mail")) ||
            CFEqualSafe(bundleName, CFSTR("com.apple.WebKit.Networking"))) {
            sharedLAContext = LACreateNewContextWithACMContext(NULL, error);
            sharedACMContext = (sharedLAContext != NULL) ? LACopyACMContext(sharedLAContext, error) : NULL;
        }
    });
    if (sharedLAContext && sharedACMContext &&
        (auth_params->dictionary == NULL || (CFDictionaryGetValue(auth_params->dictionary, kSecUseAuthenticationContext) == NULL &&
                                             CFDictionaryGetValue(auth_params->dictionary, kSecUseCredentialReference) == NULL))) {
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecUseAuthenticationContext, sharedLAContext);
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecUseCredentialReference, sharedACMContext);
    }

    for (uint32_t i = 0;; ++i) {
        // If the operation succeeded or failed with other than auth-needed error, just leave.
        SecItemAuthResult auth_result = perform(auth_params->dictionary, &ac_pairs, error);
        require_quiet(auth_result != kSecItemAuthResultError, out);
        require_action_quiet(auth_result == kSecItemAuthResultNeedAuth, out, ok = true);

        // If auth_params were not created up to now, do create them because we will definitely need them.
        SecCFDictionaryCOWGetMutable(auth_params);

        // Retrieve or create authentication handle and/or ACM context.
        CFTypeRef auth_handle = CFDictionaryGetValue(auth_params->dictionary, kSecUseAuthenticationContext);
        if (auth_handle == NULL) {
            CFDataRef acm_context = CFDictionaryGetValue(auth_params->dictionary, kSecUseCredentialReference);
            require_quiet(auth_handle = LACreateNewContextWithACMContext(acm_context, error), out);
            CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecUseAuthenticationContext, auth_handle);
            CFRelease(auth_handle);
            if (acm_context == NULL) {
                require_quiet(acm_context = LACopyACMContext(auth_handle, error), out);
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecUseCredentialReference, acm_context);
                CFRelease(acm_context);
            }
        }

        // Throttle max authentication attempts.  This is mainly protection against exceptional states, not ordinary
        // user retry limit.
        require_action(i < 20, out, SecItemAuthMaxAttemptsReached(ac_pairs, error));

        // Prepare auth options dictionary.
        if (auth_options.dictionary == NULL) {
            CFStringRef operation_prompt = CFDictionaryGetValue(auth_params->dictionary, kSecUseOperationPrompt);
            if (operation_prompt != NULL) {
                CFNumberRef key = CFNumberCreateWithCFIndex(NULL, kLAOptionAuthenticationReason);
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(&auth_options), key, operation_prompt);
                CFRelease(key);
            }

            CFStringRef caller_name = CFDictionaryGetValue(auth_params->dictionary, kSecUseCallerName);
            if (caller_name != NULL) {
                CFNumberRef key = CFNumberCreateWithCFIndex(NULL, kLAOptionCallerName);
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(&auth_options), key, caller_name);
                CFRelease(key);
            }

            CFTypeRef auth_ui = CFDictionaryGetValue(auth_params->dictionary, kSecUseAuthenticationUI);
            if (CFEqualSafe(auth_ui, kSecUseAuthenticationUIFail)) {
                CFNumberRef key = CFNumberCreateWithCFIndex(NULL, kLAOptionNotInteractive);
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(&auth_options), key, kCFBooleanTrue);
                CFRelease(key);
            }
        }

        // Go through all access_control-operation pairs and evaluate them.
        CFArrayRef ac_pair;
        CFArrayForEachC(ac_pairs, ac_pair) {
            CFDataRef updated_acl = NULL;
            require_quiet(LAEvaluateAndUpdateACL(auth_handle,
                                                 CFArrayGetValueAtIndex(ac_pair, 0), CFArrayGetValueAtIndex(ac_pair, 1),
                                                 auth_options.dictionary, &updated_acl, error), out);

            if (updated_acl || CFEqual(CFArrayGetValueAtIndex(ac_pair, 1), CFSTR(""))) {
                // we assume that only one ACL can be modified during ItemAdd or ItemUpdate
                SecAccessControlRef ac = NULL;
                require(ac = SecAccessControlCreateFromData(kCFAllocatorDefault,
                                                            updated_acl ? updated_acl : CFArrayGetValueAtIndex(ac_pair, 0), error), out);
                SecAccessControlSetBound(ac, true);
                CFAssignRetained(updated_acl, SecAccessControlCopyData(ac));
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecAttrAccessControl, updated_acl);
                CFRelease(updated_acl);
                CFRelease(ac);
            }
        }
    }

    ok = true;

out:
    CFReleaseSafe(auth_options.mutable_dictionary);
    CFReleaseSafe(ac_pairs);
    return ok;
}

void SecItemAuthCopyParams(SecCFDictionaryCOW *auth_params, SecCFDictionaryCOW *query) {
    // Store operation prompt.
    CFStringRef operation_prompt = CFDictionaryGetValue(query->dictionary, kSecUseOperationPrompt);
    if (operation_prompt != NULL) {
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecUseOperationPrompt, operation_prompt);
        CFDictionaryRemoveValue(SecCFDictionaryCOWGetMutable(query), kSecUseOperationPrompt);
    }

    // Store caller name.
    CFStringRef caller_name = CFDictionaryGetValue(query->dictionary, kSecUseCallerName);
    if (caller_name != NULL) {
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecUseCallerName, caller_name);
        CFDictionaryRemoveValue(SecCFDictionaryCOWGetMutable(query), kSecUseCallerName);
    }

    // Find out whether we are allowed to pop up a UI.
    CFTypeRef auth_ui = CFDictionaryGetValue(query->dictionary, kSecUseAuthenticationUI) ?:
    (CFEqualSafe(CFDictionaryGetValue(query->dictionary, kSecUseNoAuthenticationUI), kCFBooleanTrue) ?
     kSecUseAuthenticationUIFail : kSecUseAuthenticationUIAllow);
    if (!CFEqual(auth_ui, kSecUseAuthenticationUISkip) || CFDictionaryGetValue(query->dictionary, kSecUseNoAuthenticationUI)) {
        if (CFDictionaryContainsKey(query->dictionary, kSecUseNoAuthenticationUI))
            CFDictionaryRemoveValue(SecCFDictionaryCOWGetMutable(query), kSecUseNoAuthenticationUI);
        if (!CFEqualSafe(auth_ui, kSecUseAuthenticationUISkip) && CFDictionaryContainsKey(query->dictionary, kSecUseAuthenticationUI))
            CFDictionaryRemoveValue(SecCFDictionaryCOWGetMutable(query), kSecUseAuthenticationUI);
    }

    if (!CFEqual(auth_ui, kSecUseAuthenticationUIAllow)) {
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecUseAuthenticationUI, auth_ui);
    }

    CFDataRef acm_context = CFDictionaryGetValue(query->dictionary, kSecUseCredentialReference);
    if (acm_context != NULL) {
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(auth_params), kSecUseCredentialReference, acm_context);
    }
}

static SecItemAuthResult SecItemCreatePairsFromError(CFErrorRef *error, CFArrayRef *ac_pairs)
{
    if (error && *error && CFErrorGetCode(*error) == errSecAuthNeeded && CFEqualSafe(CFErrorGetDomain(*error), kSecErrorDomain)) {
        // Extract ACLs to be verified from the error.
        CFDictionaryRef user_info = CFErrorCopyUserInfo(*error);
        CFNumberRef key = CFNumberCreateWithCFIndex(NULL, errSecAuthNeeded);
        CFRetainAssign(*ac_pairs, CFDictionaryGetValue(user_info, key));
        if (*ac_pairs == NULL)
            CFAssignRetained(*ac_pairs, CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks));

        CFRelease(key);
        CFRelease(user_info);
        CFReleaseNull(*error);
        return kSecItemAuthResultNeedAuth;
    }
    return kSecItemAuthResultError;
}

// Wrapper to handle automatic authentication and token/secd case switching.
static bool SecItemAuthDoQuery(SecCFDictionaryCOW *query, SecCFDictionaryCOW *attributes, const void *secItemOperation, CFErrorRef *error,
                                   bool (^perform)(TKTokenRef token, CFDictionaryRef query, CFDictionaryRef attributes, CFDictionaryRef auth_params, CFErrorRef *error)) {
    bool ok = false;
    SecCFDictionaryCOW auth_params = { NULL };
    SecAccessControlRef access_control = NULL;
    __block TKTokenRef token = NULL;

    if (secItemOperation == SecItemAdd || secItemOperation == SecItemUpdate) {
        CFDictionaryRef dict = attributes ? attributes->dictionary : query->dictionary;
        access_control = (SecAccessControlRef)CFDictionaryGetValue(dict, kSecAttrAccessControl);
        if (access_control && SecAccessControlGetConstraints(access_control) &&
            CFEqualSafe(CFDictionaryGetValue(dict, kSecAttrSynchronizable), kCFBooleanTrue))
            require_quiet(SecError(errSecParam, error, CFSTR("item with kSecAttrAccessControl is not synchronizable")), out);
    }

    // Perform initial surgery on query/attributes (resolve LAContext to serialized ACM handle, resolve
    // SecAccessControlRef to serialized forms, expand kSecValueRef etc.)
    bool forQuery =
        secItemOperation == SecItemCopyMatching ||
        secItemOperation == SecItemUpdate ||
        secItemOperation == SecItemDelete;

    require_quiet(SecItemAttributesPrepare(query, forQuery, error), out);
    if (attributes != NULL)
        require_quiet(SecItemAttributesPrepare(attributes, false, error), out);

    // Populate auth_params dictionary according to initial query contents.
    SecItemAuthCopyParams(&auth_params, query);

    if (secItemOperation != SecItemCopyMatching) {
        // UISkip is allowed only for CopyMatching.
        require_action_quiet(!CFEqualSafe(CFDictionaryGetValue(query->dictionary, kSecUseAuthenticationUI), kSecUseAuthenticationUISkip), out,
                             SecError(errSecParam, error,
                                      CFSTR("kSecUseAuthenticationUISkip is allowed only for SecItemCopyMatching")));
    }

    ok = SecItemAuthDo(&auth_params, error, ^SecItemAuthResult(CFDictionaryRef auth_params, CFArrayRef *ac_pairs, CFErrorRef *error) {
        SecItemAuthResult result = kSecItemAuthResultError;

        // Propagate actual credential reference to the query.
        if (auth_params != NULL) {
            CFDataRef acm_context = CFDictionaryGetValue(auth_params, kSecUseCredentialReference);
            if (acm_context != NULL) {
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(query), kSecUseCredentialReference, acm_context);
            }

            CFDataRef acl_data_ref = CFDictionaryGetValue(auth_params, kSecAttrAccessControl);
            if (acl_data_ref != NULL) {
                CFDictionarySetValue(SecCFDictionaryCOWGetMutable(attributes ?: query), kSecAttrAccessControl, acl_data_ref);
            }
        }

        // Prepare connection to target token if it is present.
        CFStringRef token_id = CFDictionaryGetValue(query->dictionary, kSecAttrTokenID);
        if (secItemOperation != SecItemCopyMatching && token_id != NULL) {
            require_quiet(CFAssignRetained(token, SecTokenCreate(token_id, auth_params, error)), out);
        }

        CFDictionaryRef attrs = (attributes != NULL) ? attributes->dictionary : NULL;
        if(!perform(token, query->dictionary, attrs, auth_params, error)) {
            require_quiet((result = SecItemCreatePairsFromError(error, ac_pairs)) == kSecItemAuthResultOK, out);
        }

        result = kSecItemAuthResultOK;

    out:
        return result;
    });
    require_quiet(ok, out);

    ok = true;

out:
    CFReleaseSafe(token);
    CFReleaseSafe(auth_params.mutable_dictionary);
    return ok;
}

static bool cftype_to_bool_cftype_error_request(enum SecXPCOperation op, CFTypeRef attributes, CFTypeRef *result, CFErrorRef *error)
{
    return securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetPList(message, kSecXPCKeyQuery, attributes, error);
    }, ^bool(xpc_object_t response, CFErrorRef *error) {
        if (result) {
            return SecXPCDictionaryCopyPListOptional(response, kSecXPCKeyResult, result, error);
        }
        return true;
    });
}

static CFArrayRef dict_to_array_error_request(enum SecXPCOperation op, CFDictionaryRef attributes, CFErrorRef *error)
{
    CFArrayRef result = NULL;
    bool success = cftype_to_bool_cftype_error_request(op, attributes, (CFTypeRef*)&result, error);
    if(success && !isArray(result)){
        SecError(errSecUnimplemented, error, CFSTR("Unexpected nonarray returned: %@"), result);
        CFReleaseNull(result);
    }
    return result;
}

bool cftype_client_to_bool_cftype_error_request(enum SecXPCOperation op, CFTypeRef attributes, __unused SecurityClient *client, CFTypeRef *result, CFErrorRef *error) {
    return cftype_to_bool_cftype_error_request(op, attributes, result, error);
}

static bool dict_to_error_request(enum SecXPCOperation op, CFDictionaryRef query, CFErrorRef *error)
{
    return securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetPList(message, kSecXPCKeyQuery, query, error);
    }, NULL);
}

static bool cfstring_array_to_error_request(enum SecXPCOperation op, CFStringRef string, CFArrayRef attributes, __unused SecurityClient *client, CFErrorRef *error)
{
    return securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        if (string) {
            if (!SecXPCDictionarySetString(message, kSecXPCKeyString, string, error))
                return false;
        }
        
        if (attributes) {
            if (!SecXPCDictionarySetPList(message, kSecXPCKeyQuery, attributes, error))
                return false;
        }
        
        return true;
    }, NULL);
}

static bool dict_client_to_error_request(enum SecXPCOperation op, CFDictionaryRef query, __unused SecurityClient *client, CFErrorRef *error)
{
    return dict_to_error_request(op, query, error);
}

static bool SecTokenCreateAccessControlError(CFStringRef operation, CFDataRef access_control, CFErrorRef *error) {
    CFArrayRef ac_pair = CFArrayCreateForCFTypes(NULL, access_control, operation, NULL);
    const void *ac_pairs[] = { CFArrayCreateForCFTypes(NULL, ac_pair, NULL) };
    const void *keys[] = { CFNumberCreateWithCFIndex(NULL, errSecAuthNeeded) };
    CFAssignRetained(*error, CFErrorCreateWithUserInfoKeysAndValues(NULL, kSecErrorDomain, errSecAuthNeeded,
                                                                    keys, ac_pairs, 1));
    CFRelease(keys[0]);
    CFRelease(ac_pairs[0]);
    CFRelease(ac_pair);
    return false;
}

static bool SecTokenProcessError(CFStringRef operation, TKTokenRef token, CFTypeRef object_or_attrs, CFErrorRef *error) {
    if (CFEqualSafe(CFErrorGetDomain(*error), CFSTR(kTKErrorDomain)) &&
        CFErrorGetCode(*error) == kTKErrorCodeAuthenticationFailed) {
        // Replace error with the one which is augmented with access control and operation which failed,
        // which will cause SecItemDoWithAuth to throw UI.
        // Create array containing tuple (array) with error and requested operation.
        CFDataRef access_control = (CFGetTypeID(object_or_attrs) == CFDataGetTypeID()) ?
            TKTokenCopyObjectAccessControl(token, object_or_attrs, error) :
            TKTokenCopyObjectCreationAccessControl(token, object_or_attrs, error);
        if (access_control != NULL) {
            SecTokenCreateAccessControlError(operation, access_control, error);
            CFRelease(access_control);
        }
    }
    return false;
}

static CFTypeRef SecTokenCopyUpdatedObjectID(TKTokenRef token, CFDataRef object_id, CFMutableDictionaryRef attributes, CFErrorRef *error) {
    CFDataRef access_control = NULL, db_value = NULL, new_object_id = NULL;
    SecAccessControlRef ac = NULL;

    // Make sure that ACL is bound - if not, generate an error which will trigger binding.
    CFDataRef ac_data = CFDictionaryGetValue(attributes, kSecAttrAccessControl);
    if (ac_data != NULL) {
        require_quiet(ac = SecAccessControlCreateFromData(NULL, ac_data, error), out);
        require_action_quiet(SecAccessControlIsBound(ac), out,
                             SecTokenCreateAccessControlError(CFSTR(""), ac_data, error));
    }

    // Create or update the object on the token.
    require_action_quiet(new_object_id = TKTokenCreateOrUpdateObject(token, object_id, attributes, error), out,
                         SecTokenProcessError(kAKSKeyOpEncrypt, token, object_id ?: (CFTypeRef)attributes, error));

    // Prepare kSecValueData field for the item to be stored into the keychain DB.
    require_quiet(access_control = TKTokenCopyObjectAccessControl(token, new_object_id, error), out);
    require_quiet(db_value = SecTokenItemValueCreate(new_object_id, access_control,
                                                     CFDictionaryGetValue(attributes, kSecValueData), error), out);
    CFDictionarySetValue(attributes, kSecValueData, db_value);

    // kSecAttrAccessControl is handled directly by the token and stored inside data field.
    CFDictionaryRemoveValue(attributes, kSecAttrAccessControl);

out:
    CFReleaseSafe(ac);
    CFReleaseSafe(access_control);
    CFReleaseSafe(db_value);
    return new_object_id;
}

static bool SecTokenItemAdd(TKTokenRef token, CFDictionaryRef attributes, CFDictionaryRef auth_params,
                            CFTypeRef *result, CFErrorRef *error) {
    bool ok = false;
    CFTypeRef object_id = NULL, ref = NULL;
    CFDictionaryRef ref_attrs = NULL;
    CFTypeRef db_result = NULL;

    CFMutableDictionaryRef attrs = CFDictionaryCreateMutableCopy(NULL, 0, attributes);
    require_quiet(object_id = SecTokenCopyUpdatedObjectID(token, NULL, attrs, error), out);

    // Augment attributes from default attributes of the related ref (SecKeyRef, SecCertificateRef).  This is best done
    // by creating ref and getting back its attributes.
    require_quiet(SecTokenItemCreateFromAttributes(attrs, auth_params, token, object_id, &ref, error), out);
    if (ref != NULL) {
        if ((ref_attrs = SecItemCopyAttributeDictionary(ref, false)) != NULL) {
            CFDictionaryForEach(ref_attrs, ^(const void *key, const void *value) {
                if (!CFEqual(key, kSecValueData)) {
                    CFDictionaryAddValue(attrs, key, value);
                }
            });
        }
    }

    // Make sure that both attributes and data are returned.
    CFDictionarySetValue(attrs, kSecReturnAttributes, kCFBooleanTrue);
    CFDictionarySetValue(attrs, kSecReturnData, kCFBooleanTrue);

    if (!CFEqualSafe(CFDictionaryGetValue(attrs, kSecAttrIsPermanent), kCFBooleanFalse)) {
        // IsPermanent is not present or is true, so add item to the db.
        require_quiet(SECURITYD_XPC(sec_item_add, cftype_client_to_bool_cftype_error_request, attrs,
                                    SecSecurityClientGet(), &db_result, error), out);
    } else {
        // Process directly result of token call.
        db_result = CFRetain(attrs);
    }
    require_quiet(SecItemResultProcess(attributes, auth_params, token, db_result, result, error), out);

    ok = true;

out:
    CFReleaseSafe(db_result);
    CFReleaseSafe(attrs);
    CFReleaseSafe(ref_attrs);
    CFReleaseSafe(object_id);
    CFReleaseSafe(ref);
    return ok;
}

OSStatus SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result) {
    __block SecCFDictionaryCOW attrs = { attributes };
    OSStatus status;

    os_activity_t trace_activity = os_activity_start("SecItemAdd_ios", OS_ACTIVITY_FLAG_DEFAULT);
    
    require_quiet(!explode_identity(attrs.dictionary, (secitem_operation)SecItemAdd, &status, result), errOut);
	infer_cert_label(&attrs);

    status = SecOSStatusWith(^bool(CFErrorRef *error) {
        return SecItemAuthDoQuery(&attrs, NULL, SecItemAdd, error, ^bool(TKTokenRef token, CFDictionaryRef attributes, CFDictionaryRef unused, CFDictionaryRef auth_params, CFErrorRef *error) {
            if (token == NULL) {
                CFTypeRef raw_result = NULL;
                if (!SECURITYD_XPC(sec_item_add, cftype_client_to_bool_cftype_error_request, attributes, SecSecurityClientGet(), &raw_result, error))
                    return false;

                bool ok = SecItemResultProcess(attributes, auth_params, token, raw_result, result, error);
                CFReleaseSafe(raw_result);
                return ok;
            } else {
                // Send request to an appropriate token instead of secd.
                return SecTokenItemAdd(token, attributes, auth_params, result, error);
            }
        });
    });

errOut:
    CFReleaseSafe(attrs.mutable_dictionary);

    os_activity_end(trace_activity);
	return status;
}


OSStatus SecItemCopyMatching(CFDictionaryRef inQuery, CFTypeRef *result) {
    OSStatus status;
    __block SecCFDictionaryCOW query = { inQuery };

    os_activity_t trace_activity = os_activity_start("SecItemCopyMatching_ios", OS_ACTIVITY_FLAG_DEFAULT);
    
    require_quiet(!explode_identity(query.dictionary, (secitem_operation)SecItemCopyMatching, &status, result), errOut);

    bool wants_data = cf_bool_value(CFDictionaryGetValue(query.dictionary, kSecReturnData));
    bool wants_attributes = cf_bool_value(CFDictionaryGetValue(query.dictionary, kSecReturnAttributes));
    if ((wants_data && !wants_attributes) || (!wants_data && wants_attributes)) {
        // When either attributes or data are requested, we need to query both, because for token based items,
        // both are needed in order to generate proper data and/or attributes results.
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(&query), kSecReturnAttributes, kCFBooleanTrue);
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(&query), kSecReturnData, kCFBooleanTrue);
    }

    status = SecOSStatusWith(^bool(CFErrorRef *error) {
        return SecItemAuthDoQuery(&query, NULL, SecItemCopyMatching, error, ^bool(TKTokenRef token, CFDictionaryRef query, CFDictionaryRef attributes, CFDictionaryRef auth_params, CFErrorRef *error) {
            CFTypeRef raw_result = NULL;
            if (!SECURITYD_XPC(sec_item_copy_matching, cftype_client_to_bool_cftype_error_request, query, SecSecurityClientGet(), &raw_result, error))
                return false;

            // We intentionally pass NULL as token argument, because we want to be able to decide about token on which the item lives
            // on per-record basis, not wholesale.  Logic inside SecItemResultCopyPrepared will open proper token according
            // to currently processed item.
            bool ok = SecItemResultProcess(inQuery, auth_params, NULL, raw_result, result, error);
            CFReleaseSafe(raw_result);
            return ok;
        });
    });

errOut:

    CFReleaseSafe(query.mutable_dictionary);
    os_activity_end(trace_activity);
    return status;
}

// Invokes token-object handler for each item matching specified query.
static bool SecTokenItemForEachMatching(CFDictionaryRef query, CFErrorRef *error,
                                        bool (^perform)(CFDictionaryRef item_value, CFDictionaryRef item_query,
                                                        CFErrorRef *error)) {
    bool ok = false;
    CFMutableDictionaryRef list_query = NULL;
    CFTypeRef items = NULL;
    CFArrayRef ref_array = NULL;
    CFDictionaryRef item_query = NULL, item_data = NULL;

    // Query all items with data and persistent_ref, so that we can extract objectIDs and also identify originating
    // items in the keychain.
    list_query = CFDictionaryCreateMutableCopy(NULL, 0, query);
    if (CFDictionaryGetValue(list_query, kSecMatchLimit) == NULL) {
        CFDictionarySetValue(list_query, kSecMatchLimit, kSecMatchLimitAll);
    }
    CFDictionarySetValue(list_query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(list_query, kSecReturnPersistentRef, kCFBooleanTrue);
    require_quiet(SECURITYD_XPC(sec_item_copy_matching, cftype_client_to_bool_cftype_error_request, list_query,
                                SecSecurityClientGet(), &items, error), out);
    if (CFGetTypeID(items) != CFArrayGetTypeID()) {
        // Wrap single returned item into the array.
        CFArrayRef item_array = CFArrayCreateForCFTypes(NULL, items, NULL);
        CFAssignRetained(items, item_array);
    }

    CFTypeRef item;
    CFArrayForEachC(items, item) {
        CFDataRef data = CFDictionaryGetValue(item, kSecValueData);
        require_action_quiet(data != NULL, out, SecError(errSecInternal, error, CFSTR("value not present for token item")));

        CFAssignRetained(item_data, SecTokenItemValueCopy(data, error));
        require_quiet(item_data, out);

        CFAssignRetained(item_query,
                         CFDictionaryCreateForCFTypes(NULL,
                                                      kSecValuePersistentRef, CFDictionaryGetValue(item, kSecValuePersistentRef),
                                                      NULL));
        require_quiet(perform(item_data, item_query, error), out);
    }

    ok = true;

out:
    CFReleaseSafe(list_query);
    CFReleaseSafe(items);
    CFReleaseSafe(item_data);
    CFReleaseSafe(ref_array);
    CFReleaseSafe(item_query);
    return ok;
}

static bool SecItemRawUpdate(CFDictionaryRef query, CFDictionaryRef attributesToUpdate, CFErrorRef *error) {
    bool ok = false;
    if (gSecurityd) {
        // Ensure the dictionary passed to securityd has proper kCFTypeDictionaryKeyCallBacks.
        CFMutableDictionaryRef tmp = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, NULL);
        CFDictionaryForEach(attributesToUpdate, ^(const void *key, const void *value) { CFDictionaryAddValue(tmp, key, value); });
        ok = gSecurityd->sec_item_update(query, tmp, SecSecurityClientGet(), error);
        CFRelease(tmp);
    } else {
        xpc_object_t message = securityd_create_message(sec_item_update_id, error);
        if (message) {
            if (SecXPCDictionarySetPList(message, kSecXPCKeyQuery, query, error) &&
                SecXPCDictionarySetPList(message, kSecXPCKeyAttributesToUpdate, attributesToUpdate, error)) {
                xpc_object_t reply = securityd_message_with_reply_sync(message, error);
                if (reply) {
                    ok = securityd_message_no_error(reply, error);
                    xpc_release(reply);
                }
            }
            xpc_release(message);
        }
    }
    return ok;
}

static bool SecTokenItemUpdate(TKTokenRef token, CFDictionaryRef query, CFDictionaryRef attributesToUpdate, CFErrorRef *error) {
    return SecTokenItemForEachMatching(query, error, ^bool(CFDictionaryRef object_data, CFDictionaryRef item_query, CFErrorRef *error) {
        bool ok = false;
        CFDataRef object_id = NULL;
        CFMutableDictionaryRef db_value = NULL;

        // Update attributes on the token.
        CFMutableDictionaryRef attributes = CFDictionaryCreateMutableCopy(NULL, 0, attributesToUpdate);
        require_quiet(object_id = SecTokenCopyUpdatedObjectID(token, CFDictionaryGetValue(object_data, kSecTokenValueObjectIDKey),
                                                              attributes, error), out);

        // Update attributes in the database.
        require_quiet(SecItemRawUpdate(item_query, attributes, error), out);

        ok = true;

    out:
        CFReleaseSafe(object_id);
        CFReleaseSafe(attributes);
        CFReleaseSafe(db_value);
        return ok;
    });
}

OSStatus SecItemUpdate(CFDictionaryRef inQuery, CFDictionaryRef inAttributesToUpdate) {
    return SecOSStatusWith(^bool(CFErrorRef *error) {
        return SecItemUpdateWithError(inQuery, inAttributesToUpdate, error);
    });
}

bool
SecItemUpdateWithError(CFDictionaryRef inQuery,
                       CFDictionaryRef inAttributesToUpdate,
                       CFErrorRef *error)
{
    __block SecCFDictionaryCOW query = { inQuery };
    __block SecCFDictionaryCOW attributesToUpdate = { inAttributesToUpdate };
    bool result = false;

    if (handleUpdateIdentity(inQuery, inAttributesToUpdate, &result, error))
        goto errOut;

    result = SecItemAuthDoQuery(&query, &attributesToUpdate, SecItemUpdate, error, ^bool(TKTokenRef token, CFDictionaryRef query, CFDictionaryRef attributes, CFDictionaryRef auth_params, CFErrorRef *error) {
        if (token == NULL) {
                return SecItemRawUpdate(query, attributes, error);
        } else {
            return SecTokenItemUpdate(token, query, attributes, error);
        }
    });

errOut:
    CFReleaseSafe(query.mutable_dictionary);
    CFReleaseSafe(attributesToUpdate.mutable_dictionary);
    return result;
}

static OSStatus explode_persistent_identity_ref(SecCFDictionaryCOW *query)
{
    OSStatus status = errSecSuccess;
    CFTypeRef persist = CFDictionaryGetValue(query->dictionary, kSecValuePersistentRef);
    CFStringRef class;
    if (persist && _SecItemParsePersistentRef(persist, &class, NULL)
        && CFEqual(class, kSecClassIdentity)) {
        const void *keys[] = { kSecReturnRef, kSecValuePersistentRef };
        const void *vals[] = { kCFBooleanTrue, persist };
        CFDictionaryRef persistent_query = CFDictionaryCreate(NULL, keys, 
            vals, (array_size(keys)), NULL, NULL);
        CFTypeRef item_query = NULL;
        status = SecItemCopyMatching(persistent_query, &item_query);
        CFReleaseNull(persistent_query);
        if (status)
            return status;

        CFDictionaryRemoveValue(SecCFDictionaryCOWGetMutable(query), kSecValuePersistentRef);
        CFDictionarySetValue(SecCFDictionaryCOWGetMutable(query), kSecValueRef, item_query);
        CFRelease(item_query);
    }

    return status;
}

OSStatus SecItemDelete(CFDictionaryRef inQuery) {
    OSStatus status;
    __block SecCFDictionaryCOW query = { inQuery };

    os_activity_t trace_activity = os_activity_start("SecItemDelete_ios", OS_ACTIVITY_FLAG_DEFAULT);
        
    require_noerr_quiet(status = explode_persistent_identity_ref(&query), errOut);
    require_quiet(!explode_identity(query.dictionary, (secitem_operation)SecItemDelete, &status, NULL), errOut);

    status = SecOSStatusWith(^bool(CFErrorRef *error) {
        return SecItemAuthDoQuery(&query, NULL, SecItemDelete, error, ^bool(TKTokenRef token, CFDictionaryRef query, CFDictionaryRef attributes, CFDictionaryRef auth_params, CFErrorRef *error) {
            if (token == NULL) {
                return SECURITYD_XPC(sec_item_delete, dict_client_to_error_request, query, SecSecurityClientGet(), error);
            } else {
                return SecTokenItemForEachMatching(query, error, ^bool(CFDictionaryRef object_data, CFDictionaryRef item_query, CFErrorRef *error) {
                    bool ok = false;

                    // Delete item from the token.
                    CFDataRef object_id = CFDictionaryGetValue(object_data, kSecTokenValueObjectIDKey);
                    require_action_quiet(TKTokenDeleteObject(token, object_id, error), out,
                                         SecTokenProcessError(kAKSKeyOpDelete, token, object_id, error));

                    // Delete the item from the keychain.
                    require_quiet(SECURITYD_XPC(sec_item_delete, dict_client_to_error_request, item_query,
                                                SecSecurityClientGet(), error), out);
                    ok = true;

                out:
                    return ok;
                });
            }
        });
    });

errOut:
    CFReleaseSafe(query.mutable_dictionary);
    
    os_activity_end(trace_activity);
    
	return status;
}

OSStatus
SecItemDeleteAll(void)
{
    return SecOSStatusWith(^bool (CFErrorRef *error) {
        bool ok = true;
        if (gSecurityd) {
#ifndef SECITEM_SHIM_OSX
            SecTrustStoreRef ts = SecTrustStoreForDomain(kSecTrustStoreDomainUser);
            if (!gSecurityd->sec_truststore_remove_all(ts, error))
                ok &= SecError(errSecInternal, error, CFSTR("sec_truststore_remove_all is NULL"));
#endif // *** END SECITEM_SHIM_OSX ***
            if (!gSecurityd->sec_item_delete_all(error))
                ok &= SecError(errSecInternal, error, CFSTR("sec_item_delete_all is NULL"));
        } else {
            ok &= securityd_send_sync_and_do(sec_delete_all_id, error, NULL, NULL);
        }
        return ok;
    });
}

static bool
agrps_client_to_error_request(enum SecXPCOperation op, CFArrayRef agrps, __unused SecurityClient *client, CFErrorRef *error)
{
    return securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetPList(message, kSecXPCKeyAccessGroups, agrps, error);
    }, NULL);
}

bool SecItemDeleteAllWithAccessGroups(CFArrayRef accessGroups, CFErrorRef *error) {
#if 0
    os_activity_t trace_activity = os_activity_start("SecItemDeleteAllWithAccessGroups", OS_ACTIVITY_FLAG_DEFAULT);

    bool ok = SECURITYD_XPC(sec_delete_items_with_access_groups, agrps_client_to_error_request, accessGroups,
                            SecSecurityClientGet(), error);

    os_activity_end(trace_activity);
    return ok;
#else
    os_activity_t trace_activity = os_activity_start("SecItemDeleteAllWithAccessGroups", OS_ACTIVITY_FLAG_DEFAULT);
    os_activity_end(trace_activity);
    return true;
#endif
}

OSStatus
#if SECITEM_SHIM_OSX
SecItemUpdateTokenItems_ios(CFTypeRef tokenID, CFArrayRef tokenItemsAttributes)
#else
SecItemUpdateTokenItems(CFTypeRef tokenID, CFArrayRef tokenItemsAttributes)
#endif
{
    OSStatus status;

    os_activity_t trace_activity = os_activity_start("SecItemDelete_ios", OS_ACTIVITY_FLAG_DEFAULT);

    status = SecOSStatusWith(^bool(CFErrorRef *error) {
        CFArrayRef tmpArrayRef = tokenItemsAttributes;
        if (tokenItemsAttributes) {
            CFMutableArrayRef tokenItems = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
            for (CFIndex i = 0; i < CFArrayGetCount(tokenItemsAttributes); ++i) {
                CFDictionaryRef itemAttributes = CFArrayGetValueAtIndex(tokenItemsAttributes, i);
                CFTypeRef accessControl = CFDictionaryGetValue(itemAttributes, kSecAttrAccessControl);
                CFTypeRef tokenOID = CFDictionaryGetValue(itemAttributes, kSecAttrTokenOID);
                CFTypeRef valueData = CFDictionaryGetValue(itemAttributes, kSecValueData);
                if (tokenOID != NULL && accessControl != NULL && CFDataGetTypeID() == CFGetTypeID(accessControl)) {
                    CFDataRef data = SecTokenItemValueCreate(tokenOID, accessControl, valueData, error);
                    if (!data) {
                        CFRelease(tokenItems);
                        return false;
                    }
                    
                    CFMutableDictionaryRef attributes = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, itemAttributes);
                    CFDictionarySetValue(attributes, kSecValueData, data);
                    CFDictionarySetValue(attributes, kSecAttrTokenID, tokenID);
                    CFDictionaryRemoveValue(attributes, kSecAttrAccessControl);
                    CFDictionaryRemoveValue(attributes, kSecAttrTokenOID);
                    CFArrayAppendValue(tokenItems, attributes);
                    CFRelease(attributes);
                    CFRelease(data);
                }
                else
                    CFArrayAppendValue(tokenItems, itemAttributes);
            }
            
            tmpArrayRef = tokenItems;
        }

        return SECURITYD_XPC(sec_item_update_token_items, cfstring_array_to_error_request, tokenID, tmpArrayRef, SecSecurityClientGet(), error);
    });

    os_activity_end(trace_activity);

    return status;
}

CFArrayRef _SecKeychainSyncUpdateMessage(CFDictionaryRef updates, CFErrorRef *error) {
    __block CFArrayRef result;
    os_activity_initiate("_SecKeychainSyncUpdateMessage", OS_ACTIVITY_FLAG_DEFAULT, ^{
        result = SECURITYD_XPC(sec_keychain_sync_update_message, dict_to_array_error_request, updates, error);
    });
    return result;
}

#ifndef SECITEM_SHIM_OSX
OSStatus SecTaskValidateForRequirement(SecTaskRef task, CFStringRef requirement);

OSStatus SecTaskValidateForRequirement(SecTaskRef task, CFStringRef requirement)
{
    return -1; /* this is only on OS X currently */
}

#else

extern OSStatus SecTaskValidateForRequirement(SecTaskRef task, CFStringRef requirement);

#endif

#define do_if_registered(sdp, ...) if (gSecurityd && gSecurityd->sdp) { return gSecurityd->sdp(__VA_ARGS__); }

bool _SecKeychainRollKeys(bool force, CFErrorRef *error)
{
    do_if_registered(sec_roll_keys, force, error);

    __block bool result = false;

    secdebug("secitem","enter - %s", __FUNCTION__);
    securityd_send_sync_and_do(kSecXPCOpRollKeys, error,
        ^bool(xpc_object_t message, CFErrorRef *error) {
            xpc_dictionary_set_bool(message, "force", force);
            return true;
        },
        ^bool(xpc_object_t response, __unused CFErrorRef *error) {
            result = xpc_dictionary_get_bool(response, kSecXPCKeyResult);
            return result;
        });
    return result;
}


