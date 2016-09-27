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


#include <stdio.h>
#include "SOSAccountPriv.h"
#include "SOSPeerInfoCollections.h"
#include "SOSTransport.h"

//
// MARK: User Credential management
//

void SOSAccountSetPreviousPublic(SOSAccountRef account) {
    CFReleaseNull(account->previous_public);
    account->previous_public = account->user_public;
    CFRetain(account->previous_public);
}

static void SOSAccountRemoveInvalidApplications(SOSAccountRef account, SOSCircleRef circle)
{
    CFMutableSetRef peersToRemove = CFSetCreateMutableForSOSPeerInfosByID(kCFAllocatorDefault);
    SOSCircleForEachApplicant(circle, ^(SOSPeerInfoRef peer) {
        if (!SOSPeerInfoApplicationVerify(peer, account->user_public, NULL))
            CFSetAddValue(peersToRemove, peer);
    });

    CFSetForEach(peersToRemove, ^(const void *value) {
        SOSPeerInfoRef peer = (SOSPeerInfoRef) value;

        SOSCircleWithdrawRequest(circle, peer, NULL);
    });
}

static void SOSAccountGenerationSignatureUpdateWith(SOSAccountRef account, SecKeyRef privKey) {
    if (account->trusted_circle && account->my_identity &&
        SOSCircleHasPeer(account->trusted_circle, SOSFullPeerInfoGetPeerInfo(account->my_identity), NULL) &&
        !SOSCircleVerify(account->trusted_circle, account->user_public, NULL)) {
        SOSAccountModifyCircle(account, NULL, ^(SOSCircleRef circle) {
            SOSAccountRemoveInvalidApplications(account, circle); // We might be updating our signatures so remove, but don't reject applicants

            SOSFullPeerInfoRef cloud_fpi = SOSCircleCopyiCloudFullPeerInfoRef(circle, NULL);
            require_quiet(cloud_fpi != NULL, gen_sign);
            require_quiet(SOSFullPeerInfoUpgradeSignatures(cloud_fpi, privKey, NULL), gen_sign);
            if(!SOSCircleUpdatePeerInfo(circle, SOSFullPeerInfoGetPeerInfo(cloud_fpi))) {
            }
        gen_sign: // finally generation sign this.
            SOSCircleGenerationUpdate(circle, privKey, account->my_identity, NULL);
            account->departure_code = kSOSNeverLeftCircle;
            return (bool) true;
        });
    }
}

bool SOSAccountGenerationSignatureUpdate(SOSAccountRef account, CFErrorRef *error) {
    bool result = false;
    SecKeyRef priv_key = SOSAccountGetPrivateCredential(account, error);
    require_quiet(priv_key, bail);

    SOSAccountGenerationSignatureUpdateWith(account, priv_key);
    
    result = true;
bail:
    return result;
}

/* this one is meant to be local - not published over KVS. */
static bool SOSAccountPeerSignatureUpdate(SOSAccountRef account, SecKeyRef privKey, CFErrorRef *error) {
    return account->my_identity && SOSFullPeerInfoUpgradeSignatures(account->my_identity, privKey, error);
}


void SOSAccountPurgePrivateCredential(SOSAccountRef account)
{
    CFReleaseNull(account->_user_private);
    CFReleaseNull(account->_password_tmp);
    if (account->user_private_timer) {
        dispatch_source_cancel(account->user_private_timer);
        dispatch_release(account->user_private_timer);
        account->user_private_timer = NULL;
        xpc_transaction_end();
    }
    if (account->lock_notification_token) {
        notify_cancel(account->lock_notification_token);
        account->lock_notification_token = 0;
    }
}


static void SOSAccountSetTrustedUserPublicKey(SOSAccountRef account, bool public_was_trusted, SecKeyRef privKey)
{
    if (!privKey) return;
    SecKeyRef publicKey = SecKeyCreatePublicFromPrivate(privKey);
    
    if (account->user_public && account->user_public_trusted && CFEqual(publicKey, account->user_public)) return;
    
    if(public_was_trusted && account->user_public) {
        CFReleaseNull(account->previous_public);
        account->previous_public = account->user_public;
        CFRetain(account->previous_public);
    }
    
    CFReleaseNull(account->user_public);
    account->user_public = publicKey;
    account->user_public_trusted = true;
    
    if(!account->previous_public) {
        account->previous_public = account->user_public;
        CFRetain(account->previous_public);
    }
    
	secnotice("keygen", "trusting new public key: %@", account->user_public);
}

void SOSAccountSetUnTrustedUserPublicKey(SOSAccountRef account, SecKeyRef publicKey) {
    if(account->user_public_trusted && account->user_public) {
        secnotice("keygen", "Moving : %@ to previous_public", account->user_public);
        CFRetainAssign(account->previous_public, account->user_public);
    }
    
    CFReleaseNull(account->user_public);
    account->user_public = publicKey;
    account->user_public_trusted = false;
    
    if(!account->previous_public) {
        CFRetainAssign(account->previous_public, account->user_public);
    }
    
    secnotice("keygen", "not trusting new public key: %@", account->user_public);
}


static void SOSAccountSetPrivateCredential(SOSAccountRef account, SecKeyRef private, CFDataRef password) {
    if (!private)
        return SOSAccountPurgePrivateCredential(account);
    
    CFRetain(private);
    CFReleaseSafe(account->_user_private);
    account->_user_private = private;
    CFReleaseSafe(account->_password_tmp);
    account->_password_tmp = CFDataCreateCopy(kCFAllocatorDefault, password);
    
    bool resume_timer = false;
    if (!account->user_private_timer) {
        xpc_transaction_begin();
        resume_timer = true;
        account->user_private_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, account->queue);
        dispatch_source_set_event_handler(account->user_private_timer, ^{
            SOSAccountPurgePrivateCredential(account);
        });
        
        notify_register_dispatch(kUserKeybagStateChangeNotification, &account->lock_notification_token, account->queue, ^(int token) {
            bool locked = false;
            CFErrorRef lockCheckError = NULL;
            
            if (!SecAKSGetIsLocked(&locked, &lockCheckError)) {
                secerror("Checking for locked after change failed: %@", lockCheckError);
            }
            
            if (locked) {
                SOSAccountPurgePrivateCredential(account);
            }
        });
    }
    
    // (Re)set the timer's fire time to now + 120 seconds with a 5 second fuzz factor.
    dispatch_time_t purgeTime = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(10 * 60 * NSEC_PER_SEC));
    dispatch_source_set_timer(account->user_private_timer, purgeTime, DISPATCH_TIME_FOREVER, (int64_t)(5 * NSEC_PER_SEC));
    if (resume_timer)
        dispatch_resume(account->user_private_timer);
}

SecKeyRef SOSAccountGetPrivateCredential(SOSAccountRef account, CFErrorRef* error)
{
    if (account->_user_private == NULL) {
        SOSCreateError(kSOSErrorPrivateKeyAbsent, CFSTR("Private Key not available - failed to prompt user recently"), NULL, error);
    }
    return account->_user_private;
}

CFDataRef SOSAccountGetCachedPassword(SOSAccountRef account, CFErrorRef* error)
{
    if (account->_password_tmp == NULL) {
        secnotice("keygen", "Password cache expired");
    }
    return account->_password_tmp;
}


bool SOSAccountHasPublicKey(SOSAccountRef account, CFErrorRef* error)
{
    if (account->user_public == NULL || account->user_public_trusted == false) {
        SOSCreateError(kSOSErrorPublicKeyAbsent, CFSTR("Public Key not available - failed to register before call"), NULL, error);
        return false;
    }
    
    return true;
}

static void sosAccountSetTrustedCredentials(SOSAccountRef account, CFDataRef user_password, SecKeyRef user_private, bool public_was_trusted) {
    (void) SOSAccountPeerSignatureUpdate(account, user_private, NULL);
    SOSAccountSetTrustedUserPublicKey(account, public_was_trusted, user_private);
    SOSAccountSetPrivateCredential(account, user_private, user_password);
}

static bool sosAccountValidatePasswordOrFail(SOSAccountRef account, CFDataRef user_password, CFErrorRef *error) {
    bool public_was_trusted = account->user_public_trusted;
    account->user_public_trusted = false;
    SecKeyRef user_private = NULL;
    
    if (account->user_public && account->user_key_parameters) {
        // We have an untrusted public key – see if our generation makes the same key:
        // if so we trust it and we have the private key.
        // if not we still don't trust it.
        require_quiet(user_private = SOSUserKeygen(user_password, account->user_key_parameters, error), errOut);
        SecKeyRef public_candidate = SecKeyCreatePublicFromPrivate(user_private);
        
        if (!CFEqualSafe(account->user_public, public_candidate)) { // We don't trust the account->user_public
            secnotice("keygen", "Public keys don't match:  expected: %@, calculated: %@", account->user_public, public_candidate);
            debugDumpUserParameters(CFSTR("params"), account->user_key_parameters);
        } else {                                                    // We trust the account->user_public
            sosAccountSetTrustedCredentials(account, user_password, user_private, public_was_trusted);
        }
        CFReleaseNull(user_private);
        CFReleaseSafe(public_candidate);
    }
errOut:
    return account->user_public_trusted;
}

bool SOSAccountAssertUserCredentials(SOSAccountRef account, CFStringRef user_account __unused, CFDataRef user_password, CFErrorRef *error)
{
    bool public_was_trusted = account->user_public_trusted;
    account->user_public_trusted = false;
    SecKeyRef user_private = NULL;
    
    if (!sosAccountValidatePasswordOrFail(account, user_password, error)) {
        // We may or may not have parameters here.
        // In any case we tried using them and they didn't match
        // So forget all that and start again, assume we're the first to push anything useful.

        if (CFDataGetLength(user_password) > 20) {
            secwarning("Long password (>20 byte utf8) being used to derive account key – this may be a PET by mistake!!");
        }
        
        CFReleaseNull(account->user_key_parameters);
        account->user_key_parameters = SOSUserKeyCreateGenerateParameters(error);
        require_quiet(user_private = SOSUserKeygen(user_password, account->user_key_parameters, error), errOut);
        
        sosAccountSetTrustedCredentials(account, user_password, user_private, public_was_trusted);

        CFErrorRef publishError = NULL;
        if (!SOSAccountPublishCloudParameters(account, &publishError))
            secerror("Failed to publish new cloud parameters: %@", publishError);
        
        CFReleaseSafe(publishError);
    }
    
errOut:
    SOSUpdateKeyInterest();

    CFReleaseSafe(user_private);
    
    return account->user_public_trusted;
}


bool SOSAccountTryUserCredentials(SOSAccountRef account, CFStringRef user_account __unused, CFDataRef user_password, CFErrorRef *error)
{
    bool success = false;
    
    if (!SOSAccountHasPublicKey(account, error))
        return false;
    
    if (account->user_key_parameters) {
        SecKeyRef new_key = SOSUserKeygen(user_password, account->user_key_parameters, error);
        if (new_key) {
            SecKeyRef new_public_key = SecKeyCreatePublicFromPrivate(new_key);
            
            if (CFEqualSafe(new_public_key, account->user_public)) {
                SOSAccountSetPrivateCredential(account, new_key, user_password);
                success = true;
            } else {
                SOSCreateError(kSOSErrorWrongPassword, CFSTR("Password passed in incorrect: ▇█████▇▇██"), NULL, error);
            }
            CFReleaseSafe(new_public_key);
            CFReleaseSafe(new_key);
        }
    } else {
        SOSCreateError(kSOSErrorProcessingFailure, CFSTR("Have public key but no parameters??"), NULL, error);
    }
    
    return success;
}

bool SOSAccountRetryUserCredentials(SOSAccountRef account) {
    CFDataRef cached_password = SOSAccountGetCachedPassword(account, NULL);
    return (cached_password != NULL) && sosAccountValidatePasswordOrFail(account, cached_password, NULL);
}



