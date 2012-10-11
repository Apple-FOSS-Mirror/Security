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

#include <Security/SecTrust.h>
#include <Security/Trust.h>

#include "SecBridge.h"


static inline Trust *Required(SecTrustRef trustRef)
{
    return gTypes().trust.required(trustRef);
}


//
// CF boilerplate
//
CFTypeID SecTrustGetTypeID(void)
{
	BEGIN_SECAPI

	return gTypes().trust.typeId;

	END_SECAPI1(_kCFRuntimeNotATypeID)
}


//
// Sec* API bridge functions
//
OSStatus SecTrustCreateWithCertificates(
	CFArrayRef certificates,
	CFTypeRef policies,
	SecTrustRef *trustRef)
{
    BEGIN_SECAPI
	Required(trustRef);	// preflight
    RefPointer<Trust> trust(new Trust(certificates, policies));
	*trustRef = gTypes().trust.handle(*trust);
    END_SECAPI
}


OSStatus SecTrustSetParameters(
    SecTrustRef trustRef,
    CSSM_TP_ACTION action,
    CFDataRef actionData)
{
    BEGIN_SECAPI
    Trust *trust = gTypes().trust.required(trustRef);
    trust->action(action);
    trust->actionData(actionData);
    END_SECAPI
}


OSStatus SecTrustSetAnchorCertificates(SecTrustRef trust, CFArrayRef anchorCertificates)
{
    BEGIN_SECAPI
    Required(trust)->anchors(anchorCertificates);
    END_SECAPI
}


OSStatus SecTrustSetKeychains(SecTrustRef trust, CFTypeRef keychainOrArray)
{
    BEGIN_SECAPI
	StorageManager::KeychainList keychains;
	globals().storageManager.optionalSearchList(keychainOrArray, keychains);
    Required(trust)->searchLibs() = keychains;
    END_SECAPI
}


OSStatus SecTrustSetVerifyDate(SecTrustRef trust, CFDateRef verifyDate)
{
    BEGIN_SECAPI
    Required(trust)->time(verifyDate);
    END_SECAPI
}


OSStatus SecTrustEvaluate(SecTrustRef trustRef, SecTrustResultType *resultP)
{
    BEGIN_SECAPI
    Trust *trust = Required(trustRef);
    trust->evaluate();
    if (resultP)
        *resultP = trust->result();
    END_SECAPI
}


//
// Construct the "official" result evidence and return it
//
OSStatus SecTrustGetResult(
    SecTrustRef trustRef,
    SecTrustResultType *result,
	CFArrayRef *certChain, CSSM_TP_APPLE_EVIDENCE_INFO **statusChain)
{
    BEGIN_SECAPI
    Trust *trust = Required(trustRef);
    if (result)
        *result = trust->result();
    if (certChain && statusChain)
        trust->buildEvidence(*certChain, TPEvidenceInfo::overlayVar(*statusChain));
    END_SECAPI
}


//
// Retrieve CSSM-level information for those who want to dig down
//
OSStatus SecTrustGetCssmResult(SecTrustRef trust, CSSM_TP_VERIFY_CONTEXT_RESULT_PTR *result)
{
    BEGIN_SECAPI
    Required(result) = Required(trust)->cssmResult();
    END_SECAPI
}

OSStatus SecTrustGetTPHandle(SecTrustRef trust, CSSM_TP_HANDLE *handle)
{
    BEGIN_SECAPI
    Required(handle) = Required(trust)->getTPHandle();
    END_SECAPI
}


//
// Get the user's default anchor certificate set
//
OSStatus SecTrustCopyAnchorCertificates(CFArrayRef* anchorCertificates)
{
    BEGIN_SECAPI
	Required(anchorCertificates) = Trust::gStore().copyRootCertificates();
    END_SECAPI
}

OSStatus SecTrustGetCSSMAnchorCertificates(const CSSM_DATA **cssmAnchors,
	uint32 *cssmAnchorCount)
{
	BEGIN_SECAPI
	CertGroup certs;
	Trust::gStore().getCssmRootCertificates(certs);
	Required(cssmAnchors) = certs.blobCerts();
	Required(cssmAnchorCount) = certs.count();
	END_SECAPI
}


//
// Get and set user trust settings
//
OSStatus SecTrustGetUserTrust(SecCertificateRef certificate,
    SecPolicyRef policy, SecTrustUserSetting *trustSetting)
{
	BEGIN_SECAPI
	Required(trustSetting) = Trust::gStore().find(
		gTypes().certificate.required(certificate),
		gTypes().policy.required(policy));
	END_SECAPI
}

OSStatus SecTrustSetUserTrust(SecCertificateRef certificate,
    SecPolicyRef policy, SecTrustUserSetting trustSetting)
{
	BEGIN_SECAPI
	switch (trustSetting) {
    case kSecTrustResultProceed:
    case kSecTrustResultConfirm:
    case kSecTrustResultDeny:
    case kSecTrustResultUnspecified:
		break;
	default:
		MacOSError::throwMe(errSecInvalidTrustSetting);
	}
	Trust::gStore().assign(
		gTypes().certificate.required(certificate),
		gTypes().policy.required(policy),
		trustSetting);
	END_SECAPI
}

