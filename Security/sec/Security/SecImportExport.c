/*
 * Copyright (c) 2007-2008,2012-2013 Apple Inc. All Rights Reserved.
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

#include <Security/SecBase.h>
#include <Security/SecBasePriv.h>
#include <Security/SecItem.h>
#include <Security/SecRSAKey.h>
#include <Security/SecECKey.h>
#include <Security/SecCertificate.h>
#include <Security/SecIdentityPriv.h>
#include <Security/SecPolicy.h>
#include <Security/SecTrust.h>
#include <Security/SecInternal.h>
#include <libDER/oids.h>

#include <AssertMacros.h>

#include "p12import.h"
#include "SecImportExport.h"

CFStringRef kSecImportExportPassphrase = CFSTR("passphrase");
CFStringRef kSecImportItemLabel = CFSTR("label");
CFStringRef kSecImportItemKeyID = CFSTR("keyid");
CFStringRef kSecImportItemTrust = CFSTR("trust");
CFStringRef kSecImportItemCertChain = CFSTR("chain");
CFStringRef kSecImportItemIdentity = CFSTR("identity");


static void collect_certs(const void *key, const void *value, void *context)
{
    if (!CFDictionaryContainsKey(value, CFSTR("key"))) {
        CFDataRef cert_bytes = CFDictionaryGetValue(value, CFSTR("cert"));
        if (!cert_bytes)
            return;
        SecCertificateRef cert = 
            SecCertificateCreateWithData(kCFAllocatorDefault, cert_bytes);
        if (!cert)
            return;
        CFMutableArrayRef cert_array = (CFMutableArrayRef)context;
        CFArrayAppendValue(cert_array, cert);
        CFRelease(cert);
    }
}

typedef struct {
    CFMutableArrayRef identities;
    CFArrayRef certs;
} build_trust_chains_context;

static void build_trust_chains(const void *key, const void *value, 
    void *context)
{
    CFMutableDictionaryRef identity_dict = CFDictionaryCreateMutable(kCFAllocatorDefault, 
        0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecKeyRef private_key = NULL;
    SecCertificateRef cert = NULL;
    SecIdentityRef identity = NULL;
    SecPolicyRef policy = NULL;
    CFMutableArrayRef cert_chain = NULL, eval_chain = NULL;
    SecTrustRef trust = NULL;
    build_trust_chains_context * a_build_trust_chains_context = (build_trust_chains_context*)context;

    CFDataRef key_bytes = CFDictionaryGetValue(value, CFSTR("key"));
    require(key_bytes, out);
    CFDataRef cert_bytes = CFDictionaryGetValue(value, CFSTR("cert"));
    require(cert_bytes, out);
    CFDataRef algoid_bytes = CFDictionaryGetValue(value, CFSTR("algid"));


    DERItem algorithm = { (DERByte *)CFDataGetBytePtr(algoid_bytes), CFDataGetLength(algoid_bytes) };
    if (DEROidCompare(&oidEcPubKey, &algorithm)) {
        require (private_key = SecKeyCreateECPrivateKey(kCFAllocatorDefault,
                                                         CFDataGetBytePtr(key_bytes), CFDataGetLength(key_bytes),
                                                         kSecKeyEncodingPkcs1), out);
    } else if (DEROidCompare(&oidRsa, &algorithm)) {
        require (private_key = SecKeyCreateRSAPrivateKey(kCFAllocatorDefault,
                                                         CFDataGetBytePtr(key_bytes), CFDataGetLength(key_bytes),
                                                         kSecKeyEncodingPkcs1), out);
    } else {
        goto out;
    }

    require(cert = SecCertificateCreateWithData(kCFAllocatorDefault, cert_bytes), out);
    require(identity = SecIdentityCreate(kCFAllocatorDefault, cert, private_key), out);
    CFDictionarySetValue(identity_dict, kSecImportItemIdentity, identity);
    
    eval_chain = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    require(eval_chain, out);
    CFArrayAppendValue(eval_chain, cert);
    CFRange all_certs = { 0, CFArrayGetCount(a_build_trust_chains_context->certs) };
    CFArrayAppendArray(eval_chain, a_build_trust_chains_context->certs, all_certs);
    require(policy = SecPolicyCreateBasicX509(), out);
    SecTrustResultType result;
    SecTrustCreateWithCertificates(eval_chain, policy, &trust);
    require(trust, out);
    SecTrustEvaluate(trust, &result);
    CFDictionarySetValue(identity_dict, kSecImportItemTrust, trust);
    
    require(cert_chain = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks), out);
    CFIndex cert_chain_length = SecTrustGetCertificateCount(trust);
    int i;
    for (i = 0; i < cert_chain_length; i++)
        CFArrayAppendValue(cert_chain, SecTrustGetCertificateAtIndex(trust, i));
    CFDictionarySetValue(identity_dict, kSecImportItemCertChain, cert_chain);
    
    CFArrayAppendValue(a_build_trust_chains_context->identities, identity_dict);
out:
    CFReleaseSafe(identity_dict);
    CFReleaseSafe(identity);
    CFReleaseSafe(private_key);
    CFReleaseSafe(cert);
    CFReleaseSafe(policy);
    CFReleaseSafe(cert_chain);
    CFReleaseSafe(eval_chain);
    CFReleaseSafe(trust);
}

OSStatus SecPKCS12Import(CFDataRef pkcs12_data, CFDictionaryRef options, CFArrayRef *items)
{
    pkcs12_context context = {};
    SecAsn1CoderCreate(&context.coder);
    if (options)
        context.passphrase = CFDictionaryGetValue(options, kSecImportExportPassphrase);
    context.items = CFDictionaryCreateMutable(kCFAllocatorDefault, 
        0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int status = p12decode(&context, pkcs12_data);
    if (!status) {
        CFMutableArrayRef certs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        CFDictionaryApplyFunction(context.items, collect_certs, certs);

        CFMutableArrayRef identities = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
        build_trust_chains_context a_build_trust_chains_context = { identities, certs };
        CFDictionaryApplyFunction(context.items, build_trust_chains, &a_build_trust_chains_context);
        CFReleaseSafe(certs);
        
        /* ignoring certs that weren't picked up as part of the certchain for found keys */
        
        *items = identities;
    }

    CFReleaseSafe(context.items);
    SecAsn1CoderRelease(context.coder);
    
    switch (status) {
    case p12_noErr: return errSecSuccess;
    case p12_passwordErr: return errSecAuthFailed;
    case p12_decodeErr: return errSecDecode;
    default: return errSecInternal;
    };
    return errSecSuccess;
}

