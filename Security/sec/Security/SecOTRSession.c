/*
 * Copyright (c) 2011-2014 Apple Inc. All Rights Reserved.
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


#include <stdint.h>
#include <sys/types.h>
#include "utilities/comparison.h"

#include "SecOTRSession.h"

#include "SecOTRMath.h"
#include "SecOTRDHKey.h"
#include "SecOTRSessionPriv.h"
#include "SecOTRPackets.h"
#include "SecOTRPacketData.h"
#include "SecOTRIdentityPriv.h"

#include <utilities/SecCFWrappers.h>

#include <CoreFoundation/CFRuntime.h>
#include <CoreFoundation/CFString.h>

#include <Security/SecBasePriv.h>
#include <Security/SecRandom.h>
#include <Security/SecBase64.h>
#include <Security/SecKeyPriv.h>

#include <SecureObjectSync/SOSPeerInfo.h>
#include <SecureObjectSync/SOSCircle.h>
#include <SecureObjectSync/SOSCloudCircle.h>
#include <SecureObjectSync/SOSInternal.h>
#include <SecureObjectSync/SOSUserKeygen.h>

#include <AssertMacros.h>

#include <corecrypto/cchmac.h>
#include <corecrypto/ccsha2.h>
#include <corecrypto/ccsha1.h>

#include <string.h>
#include <stdlib.h>

#include <syslog.h>

#include <utilities/array_size.h>

#include <ipc/securityd_client.h>
#include <Security/SecuritydXPC.h>

CFGiblisFor(SecOTRSession);

static OTRMessageType SecOTRSGetMessageType(CFDataRef message)
{
    OTRMessageType type = kInvalidMessage;

    CFDataRef decodedBytes = SecOTRCopyIncomingBytes(message);

    const uint8_t *bytes = CFDataGetBytePtr(decodedBytes);
    size_t size = CFDataGetLength(decodedBytes);

    if (noErr != ReadHeader(&bytes, &size, &type)) {
        uint8_t firstByte = *CFDataGetBytePtr(decodedBytes);
        switch (firstByte) {
            case kOddCompactDataMessage:
            case kEvenCompactDataMessage:
                type = firstByte;
                break;
                
            default:
                break;
        }
    }

    CFReleaseNull(decodedBytes);

    return type;
}

const char *SecOTRPacketTypeString(CFDataRef message)
{
    if (!message) return "NoMessage";
    switch (SecOTRSGetMessageType(message)) {
        case kDHMessage:                return "DHMessage (0x02)";
        case kDataMessage:              return "DataMessage (0x03)";
        case kDHKeyMessage:             return "DHKeyMessage (0x0A)";
        case kRevealSignatureMessage:   return "RevealSignatureMessage (0x11)";
        case kSignatureMessage:         return "SignatureMessage (0x12)";
        case kEvenCompactDataMessage:   return "kEvenCompactDatamessage (0x20)";
        case kOddCompactDataMessage:    return "kOddCompactDataMessage (0x21)";
        case kInvalidMessage:           return "InvalidMessage (0xFF)";
        default:                        return "UnknownMessage";
    }
}

static const char *SecOTRAuthStateString(SecOTRAuthState authState)
{
    switch (authState) {
        case kIdle:                     return "Idle";
        case kAwaitingDHKey:            return "AwaitingDHKey";
        case kAwaitingRevealSignature:  return "AwaitingRevealSignature";
        case kAwaitingSignature:        return "AwaitingSignature";
        case kDone:                     return "Done";
        default:                        return "InvalidState";
    }
}

static CF_RETURNS_RETAINED CFStringRef SecOTRSessionCopyDescription(CFTypeRef cf) {
    SecOTRSessionRef session = (SecOTRSessionRef)cf;
    return CFStringCreateWithFormat(kCFAllocatorDefault,NULL,CFSTR("<%s %s %s%s%s%s %d:%d %s%s>"),
                                    SecOTRAuthStateString(session->_state),
                                    session->_compactAppleMessages ? "C" :"c",
                                    session->_me ? "F" : "f",
                                    session->_them ? "P" : "p",
                                    session->_receivedDHMessage ? "D" : "d",
                                    session->_receivedDHKeyMessage ? "K" : "k",
                                    session->_keyID,
                                    session->_theirKeyID,
                                    session->_theirPreviousKey ? "P" : "p",
                                    session->_theirKey ? "T" : "t");
}

static void SecOTRSessionDestroy(CFTypeRef cf) {
    SecOTRSessionRef session = (SecOTRSessionRef)cf;

    CFReleaseNull(session->_receivedDHMessage);
    CFReleaseNull(session->_receivedDHKeyMessage);

    CFReleaseNull(session->_me);
    CFReleaseNull(session->_myKey);
    CFReleaseNull(session->_myNextKey);

    CFReleaseNull(session->_them);
    CFReleaseNull(session->_theirKey);
    CFReleaseNull(session->_theirPreviousKey);

    CFReleaseNull(session->_macKeysToExpose);

    dispatch_release(session->_queue);
}

static void SecOTRSessionResetInternal(SecOTRSessionRef session)
{
    session->_state = kIdle;
    
    CFReleaseNull(session->_receivedDHMessage);
    CFReleaseNull(session->_receivedDHKeyMessage);

    session->_keyID = 0;
    CFReleaseNull(session->_myKey);
    CFReleaseNull(session->_myNextKey);
    //session->_myNextKey = SecOTRFullDHKCreate(kCFAllocatorDefault);
    session->_theirKeyID = 0;
    CFReleaseNull(session->_theirKey);
    CFReleaseNull(session->_theirPreviousKey);
    CFReleaseNull(session->_macKeysToExpose);
    session->_macKeysToExpose = CFDataCreateMutable(kCFAllocatorDefault, 0);

    bzero(session->_keyCache, sizeof(session->_keyCache));
}

void SecOTRSessionReset(SecOTRSessionRef session)
{
    dispatch_sync_f(session->_queue, session, (dispatch_function_t) SecOTRSessionResetInternal);
}


SecOTRSessionRef SecOTRSessionCreateFromID(CFAllocatorRef allocator,
                                           SecOTRFullIdentityRef myID,
                                           SecOTRPublicIdentityRef theirID)
{
    SecOTRSessionRef newID = CFTypeAllocate(SecOTRSession, struct _SecOTRSession, allocator);

    newID->_queue = dispatch_queue_create("OTRSession", DISPATCH_QUEUE_SERIAL);

    newID->_me = myID;
    newID->_them = theirID;
    newID->_receivedDHMessage = NULL;
    newID->_receivedDHKeyMessage = NULL;
    newID->_myKey = NULL;
    newID->_myNextKey = NULL;
    newID->_theirKey = NULL;
    newID->_theirPreviousKey = NULL;
    newID->_macKeysToExpose = NULL;
    newID->_textOutput = false;
    newID->_compactAppleMessages = false;

    SecOTRSessionResetInternal(newID);

    CFRetain(newID->_me);
    CFRetain(newID->_them);

    return newID;
}

SecOTRSessionRef SecOTRSessionCreateFromIDAndFlags(CFAllocatorRef allocator,
                                           SecOTRFullIdentityRef myID,
                                           SecOTRPublicIdentityRef theirID,
                                           uint32_t flags)
{
    SecOTRSessionRef newID = SecOTRSessionCreateFromID(allocator, myID, theirID);
    if (flags & kSecOTRSendTextMessages) {
        newID->_textOutput = true;
    }
    if (flags & kSecOTRUseAppleCustomMessageFormat) {
        newID->_compactAppleMessages = true;
    }
    return newID;
}

static uint64_t constant_zero = 0;

static bool hashIsZero(uint8_t hash[CCSHA1_OUTPUT_SIZE])
{
    bool isZero = true;
    for(size_t byte = 0; isZero && byte < CCSHA1_OUTPUT_SIZE; ++byte)
        isZero = (0 == hash[byte]);
    
    return isZero;
}


static bool SOSOTRSCacheEntryIsEmpty(SecOTRCacheElement *element)
{
    return hashIsZero(element->_fullKeyHash) && hashIsZero(element->_publicKeyHash);
}

static void SecOTRSFindKeysForMessage(SecOTRSessionRef session,
                                      SecOTRFullDHKeyRef myKey,
                                      SecOTRPublicDHKeyRef theirKey,
                                      bool sending,
                                      uint8_t** messageKey, uint8_t** macKey, uint64_t **counter)
{
    SecOTRCacheElement* emptyKeys = NULL;
    SecOTRCacheElement* cachedKeys = NULL;
    
    if ((NULL == myKey) || (NULL == theirKey)) {
        if (messageKey)
            *messageKey = NULL;
        if (macKey)
            *macKey = NULL;
        if (counter)
            *counter = &constant_zero;
            
        return;
    }
    
    for(int i = 0; i < kOTRKeyCacheSize; ++i)
    {
        if (0 == constant_memcmp(session->_keyCache[i]._fullKeyHash, SecFDHKGetHash(myKey), CCSHA1_OUTPUT_SIZE)
         && (0 == constant_memcmp(session->_keyCache[i]._publicKeyHash, SecPDHKGetHash(theirKey), CCSHA1_OUTPUT_SIZE))) {
            cachedKeys = &session->_keyCache[i];
            break;
        }

        if (emptyKeys == NULL && SOSOTRSCacheEntryIsEmpty(&(session->_keyCache[i]))) {
            emptyKeys = &session->_keyCache[i];
        }
    }

    if (cachedKeys == NULL) {
        if (emptyKeys == NULL) {
            secerror("SecOTRSession key cache was full. Should never happen, spooky.\n");
            emptyKeys = &session->_keyCache[0];
        }

        // Fill in the entry.
        memcpy(emptyKeys->_fullKeyHash, SecFDHKGetHash(myKey), CCSHA1_OUTPUT_SIZE);
        memcpy(emptyKeys->_publicKeyHash, SecPDHKGetHash(theirKey), CCSHA1_OUTPUT_SIZE);
        
        emptyKeys->_counter = 0;
        emptyKeys->_theirCounter = 0;

        SecOTRDHKGenerateOTRKeys(myKey, theirKey,
                              emptyKeys->_sendEncryptionKey, emptyKeys->_sendMacKey,
                              emptyKeys->_receiveEncryptionKey, emptyKeys->_receiveMacKey);

        cachedKeys = emptyKeys;
    }
    
    if (messageKey)
        *messageKey = sending ? cachedKeys->_sendEncryptionKey : cachedKeys->_receiveEncryptionKey;
    if (macKey)
        *macKey = sending ? cachedKeys->_sendMacKey : cachedKeys->_receiveMacKey;
    if (counter)
        *counter = sending ? &cachedKeys->_counter : &cachedKeys->_theirCounter;
}

SecOTRSessionRef SecOTRSessionCreateFromData(CFAllocatorRef allocator, CFDataRef data)
{
    if (data == NULL)
        return NULL;

    SecOTRSessionRef result = NULL;
    SecOTRSessionRef session = CFTypeAllocate(SecOTRSession, struct _SecOTRSession, allocator);

    const uint8_t *bytes = CFDataGetBytePtr(data);
    size_t size = (size_t)CFDataGetLength(data);

    session->_queue = dispatch_queue_create("OTRSession", DISPATCH_QUEUE_SERIAL);

    session->_me = NULL;
    session->_them = NULL;
    session->_myKey = NULL;
    session->_myNextKey = NULL;
    session->_theirKey = NULL;
    session->_theirPreviousKey = NULL;
    session->_receivedDHMessage = NULL;
    session->_receivedDHKeyMessage = NULL;
    session->_textOutput = false;
    session->_compactAppleMessages = false;

    bzero(session->_keyCache, sizeof(session->_keyCache));

    uint8_t version;
    require_noerr(ReadByte(&bytes, &size, &version), fail);
    require(version <= 4, fail);

    require_noerr(ReadLong(&bytes, &size, &session->_state), fail);
    session->_me = SecOTRFullIdentityCreateFromBytes(kCFAllocatorDefault, &bytes, &size, NULL);
    require(session->_me != NULL, fail);
    session->_them = SecOTRPublicIdentityCreateFromBytes(kCFAllocatorDefault, &bytes, &size, NULL);
    require(session->_them != NULL, fail);
    
    require(size > sizeof(session->_r), fail);
    memcpy(session->_r, bytes, sizeof(session->_r));
    bytes += sizeof(session->_r);
    size -= sizeof(session->_r);

    {
        uint8_t hasMessage = false;
        ReadByte(&bytes, &size, &hasMessage);
        if (hasMessage) {
            session->_receivedDHMessage = CFDataCreateMutableFromOTRDATA(kCFAllocatorDefault, &bytes, &size);
        }
    }

    if (version >= 2) {
        uint8_t hasMessage = false;
        ReadByte(&bytes, &size, &hasMessage);
        if (hasMessage) {
            session->_receivedDHKeyMessage = CFDataCreateMutableFromOTRDATA(kCFAllocatorDefault, &bytes, &size);
        }
    }

    if (version < 3) {
        uint8_t ready;
        require_noerr(ReadByte(&bytes, &size, &ready), fail);
        if (ready && session->_state == kIdle)
            session->_state = kDone;
    }

    require_noerr(ReadLong(&bytes, &size, &session->_keyID), fail);
    if (session->_keyID > 0) {
        session->_myKey = SecOTRFullDHKCreateFromBytes(kCFAllocatorDefault, &bytes, &size);
        require(session->_myKey != NULL, fail);
        session->_myNextKey = SecOTRFullDHKCreateFromBytes(kCFAllocatorDefault, &bytes, &size);
        require(session->_myNextKey != NULL, fail);
    }
    
    require_noerr(ReadLong(&bytes, &size, &session->_theirKeyID), fail);
    if (session->_theirKeyID > 0) {
        if (session->_theirKeyID > 1) {
            session->_theirPreviousKey = SecOTRPublicDHKCreateFromSerialization(kCFAllocatorDefault, &bytes, &size);
            require(session->_theirPreviousKey != NULL, fail);
        }
        session->_theirKey = SecOTRPublicDHKCreateFromSerialization(kCFAllocatorDefault, &bytes, &size);
        require(session->_theirKey != NULL, fail);
    }

    uint64_t *counter;
    SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirKey, false, NULL, NULL, &counter);
    require_noerr(ReadLongLong(&bytes, &size, counter), fail);
    SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirKey, true, NULL, NULL, &counter);
    require_noerr(ReadLongLong(&bytes, &size, counter), fail);
    SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirPreviousKey, false, NULL, NULL, &counter);
    require_noerr(ReadLongLong(&bytes, &size, counter), fail);
    SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirPreviousKey, true, NULL, NULL, &counter);
    require_noerr(ReadLongLong(&bytes, &size, counter), fail);
    SecOTRSFindKeysForMessage(session, session->_myNextKey, session->_theirKey, false, NULL, NULL, &counter);
    require_noerr(ReadLongLong(&bytes, &size, counter), fail);
    SecOTRSFindKeysForMessage(session, session->_myNextKey, session->_theirKey, true, NULL, NULL, &counter);
    require_noerr(ReadLongLong(&bytes, &size, counter), fail);
    SecOTRSFindKeysForMessage(session, session->_myNextKey, session->_theirPreviousKey, false, NULL, NULL, &counter);
    require_noerr(ReadLongLong(&bytes, &size, counter), fail);
    SecOTRSFindKeysForMessage(session, session->_myNextKey, session->_theirPreviousKey, true, NULL, NULL, &counter);
    require_noerr(ReadLongLong(&bytes, &size, counter), fail);
    
    session->_macKeysToExpose = CFDataCreateMutableFromOTRDATA(kCFAllocatorDefault, &bytes, &size);
    require(session->_macKeysToExpose != NULL, fail);
    
    require_noerr(ReadByteAsBool(&bytes, &size, &session->_textOutput), fail);

    if (version >= 4) {
        require_noerr(ReadByteAsBool(&bytes, &size, &session->_compactAppleMessages), fail);
    }

    result = session;
    session = NULL;

fail:
    CFReleaseNull(session);
    return result;
}


OSStatus SecOTRSAppendSerialization(SecOTRSessionRef session, CFMutableDataRef serializeInto)
{
    __block OSStatus result  = errSecParam;

    require(session, abort);
    require(serializeInto, abort);

    CFIndex start = CFDataGetLength(serializeInto);

    dispatch_sync(session->_queue, ^{
        const uint8_t version = 4;

        CFDataAppendBytes(serializeInto, &version, sizeof(version));

        AppendLong(serializeInto, session->_state);

        result = (SecOTRFIAppendSerialization(session->_me, serializeInto, NULL)) ? errSecSuccess : errSecParam;
    
        if (result == errSecSuccess) {
            result = (SecOTRPIAppendSerialization(session->_them, serializeInto, NULL)) ? errSecSuccess : errSecParam;
        }
    
        if (result == errSecSuccess) {
            CFDataAppendBytes(serializeInto, session->_r, sizeof(session->_r));

            if (session->_receivedDHMessage == NULL) {
                AppendByte(serializeInto, 0);
            } else {
                AppendByte(serializeInto, 1);
                AppendCFDataAsDATA(serializeInto, session->_receivedDHMessage);
            }

            if (session->_receivedDHKeyMessage == NULL) {
                AppendByte(serializeInto, 0);
            } else {
                AppendByte(serializeInto, 1);
                AppendCFDataAsDATA(serializeInto, session->_receivedDHKeyMessage);
            }

            AppendLong(serializeInto, session->_keyID);
            if (session->_keyID > 0) {
                SecFDHKAppendSerialization(session->_myKey, serializeInto);
                SecFDHKAppendSerialization(session->_myNextKey, serializeInto);
            }

            AppendLong(serializeInto, session->_theirKeyID);
            if (session->_theirKeyID > 0) {
                if (session->_theirKeyID > 1) {
                    SecPDHKAppendSerialization(session->_theirPreviousKey, serializeInto);
                }
                SecPDHKAppendSerialization(session->_theirKey, serializeInto);
            }
            
            uint64_t *counter;
            SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirKey, false, NULL, NULL, &counter);
            AppendLongLong(serializeInto, *counter);
            SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirKey, true, NULL, NULL, &counter);
            AppendLongLong(serializeInto, *counter);
            SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirPreviousKey, false, NULL, NULL, &counter);
            AppendLongLong(serializeInto, *counter);
            SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirPreviousKey, true, NULL, NULL, &counter);
            AppendLongLong(serializeInto, *counter);
            SecOTRSFindKeysForMessage(session, session->_myNextKey, session->_theirKey, false, NULL, NULL, &counter);
            AppendLongLong(serializeInto, *counter);
            SecOTRSFindKeysForMessage(session, session->_myNextKey, session->_theirKey, true, NULL, NULL, &counter);
            AppendLongLong(serializeInto, *counter);
            SecOTRSFindKeysForMessage(session, session->_myNextKey, session->_theirPreviousKey, false, NULL, NULL, &counter);
            AppendLongLong(serializeInto, *counter);
            SecOTRSFindKeysForMessage(session, session->_myNextKey, session->_theirPreviousKey, true, NULL, NULL, &counter);
            AppendLongLong(serializeInto, *counter);

            AppendCFDataAsDATA(serializeInto, session->_macKeysToExpose);
            
            AppendByte(serializeInto, session->_textOutput ? 1 : 0);
            AppendByte(serializeInto, session->_compactAppleMessages ? 1 : 0);
        }
    });

    if (result != errSecSuccess)
        CFDataSetLength(serializeInto, start);

abort:
    return result;
}


bool SecOTRSGetIsReadyForMessages(SecOTRSessionRef session)
{
    __block bool result;

    dispatch_sync(session->_queue, ^{ result = session->_state == kDone; });

    return result;
}

bool SecOTRSGetIsIdle(SecOTRSessionRef session)
{
    __block bool result;
    
    dispatch_sync(session->_queue, ^{ result = session->_state == kIdle; });
    
    return result;
}

static void SecOTRSExpireCachedKeysForFullKey(SecOTRSessionRef session, SecOTRFullDHKeyRef myKey)
{
    for(int i = 0; i < kOTRKeyCacheSize; ++i)
    {
        if (0 == constant_memcmp(session->_keyCache[i]._fullKeyHash, SecFDHKGetHash(myKey), CCSHA1_OUTPUT_SIZE)) {
            CFDataAppendBytes(session->_macKeysToExpose, session->_keyCache[i]._receiveMacKey, sizeof(session->_keyCache[i]._receiveMacKey));

            bzero(&session->_keyCache[i], sizeof(session->_keyCache[i]));
        }
    }
}

static void SecOTRSExpireCachedKeysForPublicKey(SecOTRSessionRef session, SecOTRPublicDHKeyRef theirKey)
{
    for(int i = 0; i < kOTRKeyCacheSize; ++i)
    {
        if (0 == constant_memcmp(session->_keyCache[i]._publicKeyHash, SecPDHKGetHash(theirKey), CCSHA1_OUTPUT_SIZE)) {
            CFDataAppendBytes(session->_macKeysToExpose, session->_keyCache[i]._receiveMacKey, sizeof(session->_keyCache[i]._receiveMacKey));

            bzero(&session->_keyCache[i], sizeof(session->_keyCache[i]));
        }
    }
}

static void SecOTRSPrecalculateForPair(SecOTRSessionRef session,
                                       SecOTRFullDHKeyRef myKey,
                                       SecOTRPublicDHKeyRef theirKey)
{
    if (myKey == NULL || theirKey == NULL)
        return;

    SecOTRSFindKeysForMessage(session, myKey, theirKey, true, NULL, NULL, NULL);
    SecOTRSFindKeysForMessage(session, myKey, theirKey, false, NULL, NULL, NULL);
}

static void SecOTRSPrecalculateKeysInternal(SecOTRSessionRef session)
{
    SecOTRSPrecalculateForPair(session, session->_myKey, session->_theirKey);
    SecOTRSPrecalculateForPair(session, session->_myNextKey, session->_theirKey);
    SecOTRSPrecalculateForPair(session, session->_myKey, session->_theirPreviousKey);
    SecOTRSPrecalculateForPair(session, session->_myNextKey, session->_theirPreviousKey);
}

void SecOTRSPrecalculateKeys(SecOTRSessionRef session)
{
    dispatch_sync_f(session->_queue, session, (dispatch_function_t) SecOTRSPrecalculateKeysInternal);
}

enum SecOTRSMessageKind SecOTRSGetMessageKind(SecOTRSessionRef session, CFDataRef message)
{
    OTRMessageType type = SecOTRSGetMessageType(message);

    enum SecOTRSMessageKind kind;

    switch (type) {
        case kDataMessage:
        case kEvenCompactDataMessage:
        case kOddCompactDataMessage:
            kind = kOTRDataPacket;
            break;
        case kDHMessage:
        case kDHKeyMessage:
        case kRevealSignatureMessage:
        case kSignatureMessage:
            kind = kOTRNegotiationPacket;
            break;
        case kInvalidMessage:
        default:
            kind = kOTRUnknownPacket;
            break;
    }

    return kind;
}

static OSStatus SecOTRSSignAndProtectRaw_locked(SecOTRSessionRef session,
                                                CFDataRef sourceMessage, CFMutableDataRef destinationMessage,
                                                uint8_t* messageKey, uint8_t* macKey, uint64_t* counter)
{
    CFIndex start = CFDataGetLength(destinationMessage);

    AppendHeader(destinationMessage, kDataMessage);
    AppendByte(destinationMessage, 0); // Flags, all zero

    AppendLong(destinationMessage, session->_keyID);
    AppendLong(destinationMessage, session->_theirKeyID);
    SecFDHKAppendPublicSerialization(session->_myNextKey, destinationMessage);
    AppendLongLong(destinationMessage, ++*counter);

    CFIndex sourceSize = CFDataGetLength(sourceMessage);
    assert(((unsigned long)sourceSize)<=UINT32_MAX); /* this is correct as long as CFIndex is a signed long */
    AppendLong(destinationMessage, (uint32_t)sourceSize);
    uint8_t* encryptedDataPointer = CFDataIncreaseLengthAndGetMutableBytes(destinationMessage, sourceSize);
    AES_CTR_HighHalf_Transform(kOTRMessageKeyBytes, messageKey,
                               *counter,
                               (size_t)sourceSize, CFDataGetBytePtr(sourceMessage),
                               encryptedDataPointer);

    CFIndex macedContentsSize = CFDataGetLength(destinationMessage) - start;
    CFIndex macSize = CCSHA1_OUTPUT_SIZE;
    uint8_t* macDataPointer = CFDataIncreaseLengthAndGetMutableBytes(destinationMessage, macSize);

    cchmac(ccsha1_di(),
           kOTRMessageMacKeyBytes, macKey,
           macedContentsSize, CFDataGetBytePtr(destinationMessage) + start,
           macDataPointer);

    CFDataAppend(destinationMessage, session->_macKeysToExpose);

    return errSecSuccess;
}

const size_t kCompactMessageMACSize = 16;

static OSStatus SecOTRSSignAndProtectCompact_locked(SecOTRSessionRef session,
                                                    CFDataRef sourceMessage, CFMutableDataRef destinationMessage,
                                                    uint8_t* messageKey, uint8_t* macKey, uint64_t* counter)
{
    CFIndex start = CFDataGetLength(destinationMessage);

    AppendByte(destinationMessage, (session->_theirKeyID & 0x1) ? kOddCompactDataMessage : kEvenCompactDataMessage);

    SecFDHKAppendCompactPublicSerialization(session->_myNextKey, destinationMessage);
    AppendLongLongCompact(destinationMessage, ++*counter);

    CFIndex sourceSize = CFDataGetLength(sourceMessage);
    assert(((unsigned long)sourceSize)<=UINT32_MAX); /* this is correct as long as CFIndex is a signed long */
    uint8_t* encryptedDataPointer = CFDataIncreaseLengthAndGetMutableBytes(destinationMessage, sourceSize);
    AES_CTR_HighHalf_Transform(kOTRMessageKeyBytes, messageKey,
                               *counter,
                               (size_t)sourceSize, CFDataGetBytePtr(sourceMessage),
                               encryptedDataPointer);

    CFIndex macedContentsSize = CFDataGetLength(destinationMessage) - start;
    CFIndex macSize = CCSHA1_OUTPUT_SIZE;
    uint8_t mac[macSize];
    cchmac(ccsha1_di(),
           kOTRMessageMacKeyBytes, macKey,
           macedContentsSize, CFDataGetBytePtr(destinationMessage) + start,
           mac);

    CFDataAppendBytes(destinationMessage, mac, kCompactMessageMACSize);

    return errSecSuccess;
}

OSStatus SecOTRSSignAndProtectMessage(SecOTRSessionRef session,
                                      CFDataRef sourceMessage,
                                      CFMutableDataRef protectedMessage)
{
    __block OSStatus result = errSecParam;

    require(session, abort);
    require(sourceMessage, abort);
    require(protectedMessage, abort);

    dispatch_sync(session->_queue, ^{
        if (session->_myKey == NULL ||
            session->_theirKey == NULL) {
            return;
        }

        uint8_t *messageKey;
        uint8_t *macKey;
        uint64_t *counter;

        SecOTRSFindKeysForMessage(session, session->_myKey, session->_theirKey,
                                  true,
                                  &messageKey, &macKey, &counter);


        CFMutableDataRef destinationMessage = session->_textOutput ? CFDataCreateMutable(kCFAllocatorDefault, 0) : CFRetainSafe(protectedMessage);

        result = session->_compactAppleMessages ? SecOTRSSignAndProtectCompact_locked(session, sourceMessage, destinationMessage, messageKey, macKey, counter)
        : SecOTRSSignAndProtectRaw_locked(session, sourceMessage, destinationMessage, messageKey, macKey, counter);
        
        if (result == errSecSuccess) {
            if (session->_textOutput) {
                SecOTRPrepareOutgoingBytes(destinationMessage, protectedMessage);
            }

            CFDataSetLength(session->_macKeysToExpose, 0);
        }

        CFReleaseSafe(destinationMessage);

        result = errSecSuccess;
    });

abort:
    return result;
}

static void SecOTRAcceptNewRemoteKey(SecOTRSessionRef session, SecOTRPublicDHKeyRef newKey)
{
    if (session->_theirPreviousKey) {
        SecOTRSExpireCachedKeysForPublicKey(session, session->_theirPreviousKey);
    }

    CFReleaseNull(session->_theirPreviousKey);
    session->_theirPreviousKey = session->_theirKey;
    session->_theirKey = CFRetainSafe(newKey);

    session->_theirKeyID += 1;
}

static void SecOTRGenerateNewProposedKey(SecOTRSessionRef session)
{
    SecOTRSExpireCachedKeysForFullKey(session, session->_myKey);

    // Swap the keys so we know the current key.
    {
        SecOTRFullDHKeyRef oldKey = session->_myKey;
        session->_myKey = session->_myNextKey;
        session->_myNextKey = oldKey;
    }

    // Derive a new next key by regenerating over the old key.
    SecFDHKNewKey(session->_myNextKey);

    session->_keyID += 1;
}

static OSStatus SecOTRVerifyAndExposeRaw_locked(SecOTRSessionRef session,
                                                CFDataRef decodedBytes,
                                                CFMutableDataRef exposedMessageContents)
{
    OSStatus result = errSecDecode;
    
    SecOTRPublicDHKeyRef newKey = NULL;
    const uint8_t* bytes;
    size_t  size;

    bytes = CFDataGetBytePtr(decodedBytes);
    size = CFDataGetLength(decodedBytes);

    const uint8_t* macDataStart = bytes;

    uint32_t theirID;
    uint32_t myID;

    require_noerr_quiet(result = ReadAndVerifyHeader(&bytes, &size, kDataMessage), fail);
    require_action_quiet(size > 0, fail, result = errSecDecode);

    require_noerr_quiet(result = ReadAndVerifyByte(&bytes, &size, 0), fail); // Flags, always zero

    require_noerr_quiet(result = ReadLong(&bytes, &size, &theirID), fail);

    require_action_quiet(theirID == session->_theirKeyID || (theirID == (session->_theirKeyID - 1) && session->_theirPreviousKey != NULL),
                         fail,
                         result = ((theirID + 1) < session->_theirKeyID) ? errSecOTRTooOld : errSecOTRIDTooNew);

    require_noerr_quiet(result = ReadLong(&bytes, &size, &myID), fail);

    require_action_quiet(myID == session->_keyID || (myID == session->_keyID + 1 && session->_myNextKey != NULL),
                         fail,
                         result = (myID < session->_keyID) ? errSecOTRTooOld : errSecOTRIDTooNew);


    // Choose appripriate keys for message:
    {
        uint8_t *messageKey;
        uint8_t *macKey;
        uint64_t *theirCounter;

        SecOTRFullDHKeyRef myKeyForMessage = (myID == session->_keyID) ? session->_myKey : session->_myNextKey;
        SecOTRPublicDHKeyRef theirKeyForMessage = (theirID == session->_theirKeyID) ? session->_theirKey : session->_theirPreviousKey;

        SecOTRSFindKeysForMessage(session, myKeyForMessage, theirKeyForMessage, false,
                                  &messageKey, &macKey, &theirCounter);

        size_t nextKeyMPISize;
        const uint8_t* nextKeyMPIBytes;
        require_noerr_quiet(result = SizeAndSkipMPI(&bytes, &size, &nextKeyMPIBytes, &nextKeyMPISize), fail);

        uint64_t counter;
        require_noerr_quiet(result = ReadLongLong(&bytes, &size, &counter), fail);
        require_action_quiet(counter > *theirCounter, fail, result = errSecOTRTooOld);

        size_t messageSize;
        const uint8_t* messageStart;
        require_noerr_quiet(result = SizeAndSkipDATA(&bytes, &size, &messageStart, &messageSize), fail);

        size_t macDataSize = (bytes - macDataStart) ? (size_t)(bytes - macDataStart) : 0;
        uint8_t mac[CCSHA1_OUTPUT_SIZE];
        require_action_quiet(sizeof(mac) <= size, fail, result = errSecDecode);

        cchmac(ccsha1_di(),
               kOTRMessageMacKeyBytes, macKey,
               macDataSize, macDataStart,
               mac);

        require_noerr_action_quiet(constant_memcmp(mac, bytes, sizeof(mac)), fail, result = errSecAuthFailed);

        uint8_t* dataSpace = CFDataIncreaseLengthAndGetMutableBytes(exposedMessageContents, (CFIndex)messageSize);

        AES_CTR_HighHalf_Transform(kOTRMessageKeyBytes, messageKey,
                                   counter,
                                   messageSize, messageStart,
                                   dataSpace);

        // Everything is good, accept the meta data.
        *theirCounter = counter;

        newKey = SecOTRPublicDHKCreateFromBytes(kCFAllocatorDefault, &nextKeyMPIBytes, &nextKeyMPISize);
    }

    bool acceptTheirNewKey = newKey != NULL && theirID == session->_theirKeyID;

    if (acceptTheirNewKey) {
        SecOTRAcceptNewRemoteKey(session, newKey);
    }

    if (myID == (session->_keyID + 1)) {
        SecOTRGenerateNewProposedKey(session);
    }

    SecOTRSPrecalculateKeysInternal(session);

fail:
    CFReleaseNull(newKey);
    return result;
}

static OSStatus SecOTRVerifyAndExposeRawCompact_locked(SecOTRSessionRef session,
                                                CFDataRef decodedBytes,
                                                CFMutableDataRef exposedMessageContents)
{
    SecOTRPublicDHKeyRef theirProposal = NULL;
    OSStatus result = errSecDecode;
    const uint8_t* bytes;
    size_t  size;

    bytes = CFDataGetBytePtr(decodedBytes);
    size = CFDataGetLength(decodedBytes);

    const uint8_t* macDataStart = bytes;

    uint8_t type_byte;
    require_noerr_quiet(result = ReadByte(&bytes, &size, &type_byte), fail);
    require_action_quiet(type_byte == kOddCompactDataMessage || type_byte == kEvenCompactDataMessage, fail, result = errSecDecode);

    bool useEvenKey = (type_byte == kEvenCompactDataMessage);

    bool useCurrentKey = useEvenKey ^ (session->_keyID & 1);
    SecOTRFullDHKeyRef myKeyForMessage =  useCurrentKey ? session->_myKey : session->_myNextKey;

    theirProposal = SecOTRPublicDHKCreateFromCompactSerialization(kCFAllocatorDefault, &bytes, &size);

    require_action_quiet(theirProposal, fail, result = errSecDecode);

    bool proposalIsNew = !CFEqualSafe(theirProposal, session->_theirKey);
    SecOTRPublicDHKeyRef theirKeyForMessage = proposalIsNew ? session->_theirKey : session->_theirPreviousKey;

    uint8_t *messageKey;
    uint8_t *macKey;
    uint64_t *theirCounter;

    SecOTRSFindKeysForMessage(session, myKeyForMessage, theirKeyForMessage, false, &messageKey, &macKey, &theirCounter);

    uint64_t counter;
    require_noerr_quiet(result = ReadLongLongCompact(&bytes, &size, &counter), fail);
    require_action_quiet(counter > *theirCounter, fail, result = errSecOTRTooOld);

    uint8_t mac[CCSHA1_OUTPUT_SIZE];
    require_action_quiet(sizeof(mac) < size, fail, result = errSecDecode); // require space for the mac and some bytes

    size_t messageSize = size - kCompactMessageMACSize; // It's all message except for the MAC
    const uint8_t* messageStart = bytes;

    bytes += messageSize;

    size_t macDataSize = (size_t)(bytes - macDataStart);

    cchmac(ccsha1_di(),
           kOTRMessageMacKeyBytes, macKey,
           macDataSize, macDataStart,
           mac);

    require_noerr_action_quiet(constant_memcmp(mac, bytes, kCompactMessageMACSize), fail, result = errSecAuthFailed);

    uint8_t* dataSpace = CFDataIncreaseLengthAndGetMutableBytes(exposedMessageContents, (CFIndex)messageSize);

    AES_CTR_HighHalf_Transform(kOTRMessageKeyBytes, messageKey,
                               counter,
                               messageSize, messageStart,
                               dataSpace);

    // Everything is good, accept the meta data.
    *theirCounter = counter;

    if (proposalIsNew) {
        SecOTRAcceptNewRemoteKey(session, theirProposal);
    }
    
    if (!useCurrentKey) {
        SecOTRGenerateNewProposedKey(session);
    }

    SecOTRSPrecalculateKeysInternal(session);

fail:
    CFReleaseNull(theirProposal);
    return result;
}


OSStatus SecOTRSVerifyAndExposeMessage(SecOTRSessionRef session,
                                       CFDataRef incomingMessage,
                                       CFMutableDataRef exposedMessageContents)
{
    __block OSStatus result = errSecParam;


    require(session, abort);
    require(incomingMessage, abort);
    require(exposedMessageContents, abort);

    dispatch_sync(session->_queue, ^{
        CFDataRef decodedBytes = SecOTRCopyIncomingBytes(incomingMessage);

        OTRMessageType messageType = SecOTRSGetMessageType(decodedBytes);

        switch (messageType) {
            case kDataMessage:
                result = SecOTRVerifyAndExposeRaw_locked(session, decodedBytes, exposedMessageContents);
                break;

            case kOddCompactDataMessage:
            case kEvenCompactDataMessage:
                result = SecOTRVerifyAndExposeRawCompact_locked(session, decodedBytes, exposedMessageContents);
                break;

            default:
                result = errSecUnsupportedFormat;
                break;
        }

        CFReleaseSafe(decodedBytes);
    });

abort:
    return result;
}


OSStatus SecOTRSEndSession(SecOTRSessionRef session,
                           CFMutableDataRef messageToSend)
{
    return errSecUnimplemented;
}

static CFDataRef data_to_data_error_request(enum SecXPCOperation op, CFDataRef publicPeerId, CFErrorRef *error) {
    __block CFDataRef result = NULL;
    securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetDataOptional(message, kSecXPCPublicPeerId, publicPeerId, error);
    }, ^bool(xpc_object_t response, CFErrorRef *error) {
        return (result = SecXPCDictionaryCopyData(response, kSecXPCKeyResult, error));
    });
    return result;
}

static bool data_data_to_data_data_bool_error_request(enum SecXPCOperation op, CFDataRef sessionData, CFDataRef inputPacket, CFDataRef* outputSessionData, CFDataRef* outputPacket, bool *readyForMessages, CFErrorRef *error) {
    __block CFDataRef tempOutputSessionData = NULL;
    __block CFDataRef tempOutputPacket = NULL;
    __block bool tempReadyForMessages = false;
    
    bool result = securityd_send_sync_and_do(op, error, ^bool(xpc_object_t message, CFErrorRef *error) {
        return SecXPCDictionarySetDataOptional(message, kSecXPCOTRSession, sessionData, error)
        && SecXPCDictionarySetDataOptional(message, kSecXPCData, inputPacket, error);
    }, ^bool(xpc_object_t response, CFErrorRef *error) {
        if (xpc_dictionary_get_bool(response, kSecXPCKeyResult)) {
            tempOutputSessionData = SecXPCDictionaryCopyData(response, kSecXPCOTRSession, error);
            tempOutputPacket = SecXPCDictionaryCopyData(response, kSecXPCData, error);
            tempReadyForMessages = xpc_dictionary_get_bool(response, kSecXPCOTRReady);
            return true;
        } else {
            return false;
        }
        
    });
    
    *outputSessionData = tempOutputSessionData;
    *outputPacket = tempOutputPacket;
    *readyForMessages = tempReadyForMessages;
    
    return result;
}


CFDataRef SecOTRSessionCreateRemote(CFDataRef publicPeerId, CFErrorRef *error) {

    CFDataRef otrSession = SECURITYD_XPC(sec_otr_session_create_remote, data_to_data_error_request, publicPeerId, error);
    return otrSession;

}

bool SecOTRSessionProcessPacketRemote(CFDataRef sessionData, CFDataRef inputPacket, CFDataRef* outputSessionData, CFDataRef* outputPacket, bool *readyForMessages, CFErrorRef *error) {
    
    return SECURITYD_XPC(sec_otr_session_process_packet_remote, data_data_to_data_data_bool_error_request, sessionData, inputPacket, outputSessionData, outputPacket, readyForMessages, error);
}
