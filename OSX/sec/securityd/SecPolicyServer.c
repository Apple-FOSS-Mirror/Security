/*
 * Copyright (c) 2008-2017 Apple Inc. All Rights Reserved.
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
 * SecPolicyServer.c - Engine for evaluating certificate paths against trust policies.
 */

#include <securityd/SecPolicyServer.h>
#include <Security/SecPolicyInternal.h>
#include <Security/SecPolicyPriv.h>
#include <Security/SecTask.h>
#include <securityd/asynchttp.h>
#include <securityd/policytree.h>
#include <securityd/nameconstraints.h>
#include <CoreFoundation/CFTimeZone.h>
#include <wctype.h>
#include <libDER/oidsPriv.h>
#include <CoreFoundation/CFNumber.h>
#include <Security/SecCertificateInternal.h>
#include <AssertMacros.h>
#include <utilities/debugging.h>
#include <utilities/SecInternalReleasePriv.h>
#include <security_asn1/SecAsn1Coder.h>
#include <security_asn1/ocspTemplates.h>
#include <security_asn1/oidsalg.h>
#include <security_asn1/oidsocsp.h>
#include <CommonCrypto/CommonDigest.h>
#include <Security/SecFramework.h>
#include <Security/SecPolicyInternal.h>
#include <Security/SecTrustPriv.h>
#include <Security/SecTrustInternal.h>
#include <Security/SecTrustSettingsPriv.h>
#include <Security/SecInternal.h>
#include <Security/SecKeyPriv.h>
#include <Security/SecTask.h>
#include <CFNetwork/CFHTTPMessage.h>
#include <CFNetwork/CFHTTPStream.h>
#include <SystemConfiguration/SCDynamicStoreCopySpecific.h>
#include <asl.h>
#include <securityd/SecTrustServer.h>
#include <securityd/SecTrustLoggingServer.h>
#include <securityd/SecRevocationServer.h>
#include <securityd/SecCertificateServer.h>
#include <securityd/SecCertificateSource.h>
#include <securityd/SecOCSPResponse.h>
#include <utilities/array_size.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecAppleAnchorPriv.h>
#include "OTATrustUtilities.h"
#include "personalization.h"
#include <sys/codesign.h>

#if !TARGET_OS_IPHONE
#include <Security/SecTaskPriv.h>
#endif

/* Set this to 1 to dump the ocsp responses received in DER form in /tmp. */
#ifndef DUMP_OCSPRESPONSES
#define DUMP_OCSPRESPONSES  0
#endif

#if DUMP_OCSPRESPONSES

#include <unistd.h>
#include <fcntl.h>

static void secdumpdata(CFDataRef data, const char *name) {
    int fd = open(name, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    write(fd, CFDataGetBytePtr(data), CFDataGetLength(data));
    close(fd);
}

#endif


/********************************************************
 ****************** SecPolicy object ********************
 ********************************************************/

static SecCertificateRef SecPVCGetCertificateAtIndex(SecPVCRef pvc, CFIndex ix);
static CFIndex SecPVCGetCertificateCount(SecPVCRef pvc);
static CFAbsoluteTime SecPVCGetVerifyTime(SecPVCRef pvc);

static CFMutableDictionaryRef gSecPolicyLeafCallbacks = NULL;
static CFMutableDictionaryRef gSecPolicyPathCallbacks = NULL;

static CFArrayRef SecPolicyAnchorDigestsForEVPolicy(const DERItem *policyOID)
{
	CFArrayRef result = NULL;
	SecOTAPKIRef otapkiRef = SecOTAPKICopyCurrentOTAPKIRef();
	if (NULL == otapkiRef)
	{
		return result;
	}

	CFDictionaryRef evToPolicyAnchorDigest = SecOTAPKICopyEVPolicyToAnchorMapping(otapkiRef);
	CFRelease(otapkiRef);

    if (NULL == evToPolicyAnchorDigest)
    {
        return result;
    }

    CFArrayRef roots = NULL;
    CFStringRef oid = SecDERItemCopyOIDDecimalRepresentation(kCFAllocatorDefault, policyOID);
    if (oid && evToPolicyAnchorDigest)
	{
        result = (CFArrayRef)CFDictionaryGetValue(evToPolicyAnchorDigest, oid);
		if (roots && CFGetTypeID(result) != CFArrayGetTypeID())
		{
            secerror("EVRoot.plist has non array value");
            result = NULL;
        }
        CFRelease(oid);
    }
    CFReleaseSafe(evToPolicyAnchorDigest);
    return result;
}


bool SecPolicyIsEVPolicy(const DERItem *policyOID) {
    return SecPolicyAnchorDigestsForEVPolicy(policyOID);
}

static bool SecPolicyRootCACertificateIsEV(SecCertificateRef certificate,
    policy_set_t valid_policies) {
    CFDictionaryRef keySizes = NULL;
    CFNumberRef rsaSize = NULL, ecSize = NULL;
    bool isEV = false;
    /* Ensure that this certificate is a valid anchor for one of the
       certificate policy oids specified in the leaf. */
    CFDataRef digest = SecCertificateGetSHA1Digest(certificate);
    policy_set_t ix;
    bool good_ev_anchor = false;
    for (ix = valid_policies; ix; ix = ix->oid_next) {
        CFArrayRef digests = SecPolicyAnchorDigestsForEVPolicy(&ix->oid);
        if (digests && CFArrayContainsValue(digests,
            CFRangeMake(0, CFArrayGetCount(digests)), digest)) {
            secdebug("ev", "found anchor for policy oid");
            good_ev_anchor = true;
            break;
        }
    }
    require_action_quiet(good_ev_anchor, notEV, secnotice("ev", "anchor not in plist"));

    CFAbsoluteTime october2006 = 178761600;
    if (SecCertificateNotValidBefore(certificate) >= october2006) {
        require_action_quiet(SecCertificateVersion(certificate) >= 3, notEV,
                             secnotice("ev", "Anchor issued after October 2006 and is not v3"));
    }
    if (SecCertificateVersion(certificate) >= 3
        && SecCertificateNotValidBefore(certificate) >= october2006) {
        const SecCEBasicConstraints *bc = SecCertificateGetBasicConstraints(certificate);
        require_action_quiet(bc && bc->isCA == true, notEV,
                             secnotice("ev", "Anchor has invalid basic constraints"));
        SecKeyUsage ku = SecCertificateGetKeyUsage(certificate);
        require_action_quiet((ku & (kSecKeyUsageKeyCertSign | kSecKeyUsageCRLSign))
            == (kSecKeyUsageKeyCertSign | kSecKeyUsageCRLSign), notEV,
                             secnotice("ev", "Anchor has invalid key usage %u", ku));
    }

    /* At least RSA 2048 or ECC NIST P-256. */
    require_quiet(rsaSize = CFNumberCreateWithCFIndex(NULL, 2048), notEV);
    require_quiet(ecSize = CFNumberCreateWithCFIndex(NULL, 256), notEV);
    const void *keys[] = { kSecAttrKeyTypeRSA, kSecAttrKeyTypeEC };
    const void *values[] = { rsaSize, ecSize };
    require_quiet(keySizes = CFDictionaryCreate(NULL, keys, values, 2,
                                          &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks), notEV);
    require_action_quiet(SecCertificateIsAtLeastMinKeySize(certificate, keySizes), notEV,
                         secnotice("ev", "Anchor's public key is too weak for EV"));

    isEV = true;

notEV:
    CFReleaseNull(rsaSize);
    CFReleaseNull(ecSize);
    CFReleaseNull(keySizes);
    return isEV;
}

static bool SecPolicySubordinateCACertificateCouldBeEV(SecCertificateRef certificate) {
    CFMutableDictionaryRef keySizes = NULL;
    CFNumberRef rsaSize = NULL, ecSize = NULL;
    bool isEV = false;

    const SecCECertificatePolicies *cp;
    cp = SecCertificateGetCertificatePolicies(certificate);
    require_action_quiet(cp && cp->numPolicies > 0, notEV,
                         secnotice("ev", "SubCA missing certificate policies"));
    CFArrayRef cdp = SecCertificateGetCRLDistributionPoints(certificate);
    require_action_quiet(cdp && CFArrayGetCount(cdp) > 0, notEV,
                         secnotice("ev", "SubCA missing CRLDP"));
    const SecCEBasicConstraints *bc = SecCertificateGetBasicConstraints(certificate);
    require_action_quiet(bc && bc->isCA == true, notEV,
                         secnotice("ev", "SubCA has invalid basic constraints"));
    SecKeyUsage ku = SecCertificateGetKeyUsage(certificate);
    require_action_quiet((ku & (kSecKeyUsageKeyCertSign | kSecKeyUsageCRLSign))
        == (kSecKeyUsageKeyCertSign | kSecKeyUsageCRLSign), notEV,
                         secnotice("ev", "SubCA has invalid key usage %u", ku));

    /* 6.1.5 Key Sizes */
    CFAbsoluteTime jan2011 = 315532800;
    CFAbsoluteTime jan2014 = 410227200;
    require_quiet(ecSize = CFNumberCreateWithCFIndex(NULL, 256), notEV);
    require_quiet(keySizes = CFDictionaryCreateMutable(NULL, 2, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks), notEV);
    CFDictionaryAddValue(keySizes, kSecAttrKeyTypeEC, ecSize);
    if (SecCertificateNotValidBefore(certificate) < jan2011 ||
        SecCertificateNotValidAfter(certificate) < jan2014) {
        /* At least RSA 1024 or ECC NIST P-256. */
        require_quiet(rsaSize = CFNumberCreateWithCFIndex(NULL, 1024), notEV);
        CFDictionaryAddValue(keySizes, kSecAttrKeyTypeRSA, rsaSize);
        require_action_quiet(SecCertificateIsAtLeastMinKeySize(certificate, keySizes), notEV,
                             secnotice("ev", "SubCA's public key is too small for issuance before 2011 or expiration before 2014"));
    } else {
        /* At least RSA 2028 or ECC NIST P-256. */
        require_quiet(rsaSize = CFNumberCreateWithCFIndex(NULL, 2048), notEV);
        CFDictionaryAddValue(keySizes, kSecAttrKeyTypeRSA, rsaSize);
        require_action_quiet(SecCertificateIsAtLeastMinKeySize(certificate, keySizes), notEV,
                             secnotice("ev", "SubCA's public key is too small for issuance after 2010 or expiration after 2013"));
    }

    /* 7.1.3 Algorithm Object Identifiers */
    CFAbsoluteTime jan2016 = 473299200;
    if (SecCertificateNotValidBefore(certificate) > jan2016) {
        /* SHA-2 only */
        require_action_quiet(SecCertificateGetSignatureHashAlgorithm(certificate) > kSecSignatureHashAlgorithmSHA1,
                             notEV, secnotice("ev", "SubCA was issued with SHA-1 after 2015"));
    }

    isEV = true;

notEV:
    CFReleaseNull(rsaSize);
    CFReleaseNull(ecSize);
    CFReleaseNull(keySizes);
    return isEV;
}

/********************************************************
 **************** SecPolicy Callbacks *******************
 ********************************************************/
static void SecPolicyCheckCriticalExtensions(SecPVCRef pvc,
	CFStringRef key) {
}

static void SecPolicyCheckIdLinkage(SecPVCRef pvc,
	CFStringRef key) {
	CFIndex ix, count = SecPVCGetCertificateCount(pvc);
	CFDataRef parentSubjectKeyID = NULL;
	for (ix = count - 1; ix >= 0; --ix) {
		SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
		/* If the previous certificate in the chain had a SubjectKeyID,
		   make sure it matches the current certificates AuthorityKeyID. */
		if (parentSubjectKeyID) {
			/* @@@ According to RFC 2459 neither AuthorityKeyID nor
			   SubjectKeyID can be critical.  Currenty we don't check
			   for this. */
			CFDataRef authorityKeyID = SecCertificateGetAuthorityKeyID(cert);
			if (authorityKeyID) {
				if (!CFEqual(parentSubjectKeyID, authorityKeyID)) {
					/* AuthorityKeyID doesn't match issuers SubjectKeyID. */
					if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
						return;
				}
			}
		}

		parentSubjectKeyID = SecCertificateGetSubjectKeyID(cert);
	}
}

static void SecPolicyCheckKeyUsage(SecPVCRef pvc,
	CFStringRef key) {
    SecCertificateRef leaf = SecPVCGetCertificateAtIndex(pvc, 0);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef xku = CFDictionaryGetValue(policy->_options, key);
    if (!SecPolicyCheckCertKeyUsage(leaf, xku)) {
        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckExtendedKeyUsage(SecPVCRef pvc, CFStringRef key) {
    SecCertificateRef leaf = SecPVCGetCertificateAtIndex(pvc, 0);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef xeku = CFDictionaryGetValue(policy->_options, key);
    if (!SecPolicyCheckCertExtendedKeyUsage(leaf, xeku)){
        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

#if 0
static void SecPolicyCheckBasicContraintsCommon(SecPVCRef pvc,
	CFStringRef key, bool strict) {
	CFIndex ix, count = SecPVCGetCertificateCount(pvc);
	for (ix = 0; ix < count; ++ix) {
		SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
		const SecCEBasicConstraints *bc =
			SecCertificateGetBasicConstraints(cert);
		if (bc) {
			if (strict) {
				if (ix == 0) {
					/* Leaf certificate has basic constraints extension. */
					if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
						return;
				} else if (!bc->critical) {
					/* Basic constraints extension is not marked critical. */
					if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
						return;
				}
			}

			if (ix > 0 || count == 1) {
				if (!bc->isCA) {
					/* Non leaf certificate marked as isCA false. */
					if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
						return;
				}

				if (bc->pathLenConstraintPresent) {
					if (bc->pathLenConstraint < (uint32_t)(ix - 1)) {
#if 0
						/* @@@ If a self signed certificate is issued by
						   another cert that is trusted, then we are supposed
						   to treat the self signed cert itself as the anchor
						   for path length purposes. */
						CFIndex ssix = SecCertificatePathSelfSignedIndex(path);
						if (ssix >= 0 && ix >= ssix) {
							/* It's ok if the pathLenConstraint isn't met for
							   certificates signing a self signed cert in the
							   chain. */
						} else
#endif
						{
							/* Path Length Constraint Exceeded. */
							if (!SecPVCSetResult(pvc, key, ix,
								kCFBooleanFalse))
								return;
						}
					}
				}
			}
		} else if (strict && ix > 0) {
			/* In strict mode all CA certificates *MUST* have a critical
			   basic constraints extension and the leaf certificate
			   *MUST NOT* have a basic constraints extension. */
			/* CA certificate is missing basicConstraints extension. */
			if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
				return;
		}
	}
}
#endif

static void SecPolicyCheckBasicConstraints(SecPVCRef pvc,
	CFStringRef key) {
	//SecPolicyCheckBasicContraintsCommon(pvc, key, false);
}

static void SecPolicyCheckNonEmptySubject(SecPVCRef pvc,
	CFStringRef key) {
	CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef pvcValue = CFDictionaryGetValue(policy->_options, key);
	for (ix = 0; ix < count; ++ix) {
		SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (!SecPolicyCheckCertNonEmptySubject(cert, pvcValue)) {
            if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
                return;
        }
	}
}

static void SecPolicyCheckQualifiedCertStatements(SecPVCRef pvc,
	CFStringRef key) {
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckSSLHostname(SecPVCRef pvc,
	CFStringRef key) {
	/* @@@ Consider what to do if the caller passes in no hostname.  Should
	   we then still fail if the leaf has no dnsNames or IPAddresses at all? */
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
	CFStringRef hostName = (CFStringRef)
		CFDictionaryGetValue(policy->_options, key);
    if (!isString(hostName)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }

	SecCertificateRef leaf = SecPVCGetCertificateAtIndex(pvc, 0);
    bool dnsMatch = SecPolicyCheckCertSSLHostname(leaf, hostName);

	if (!dnsMatch) {
		/* Hostname mismatch or no hostnames found in certificate. */
		SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
	}

}

/* AUDIT[securityd](done):
 policy->_options is a caller provided dictionary, only its cf type has
 been checked.
 */
static void SecPolicyCheckEmail(SecPVCRef pvc, CFStringRef key) {
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
	CFStringRef email = (CFStringRef)CFDictionaryGetValue(policy->_options, key);
    if (!isString(email)) {
        /* We can't return an error here and making the evaluation fail
         won't help much either. */
        return;
    }

	SecCertificateRef leaf = SecPVCGetCertificateAtIndex(pvc, 0);

	if (!SecPolicyCheckCertEmail(leaf, email)) {
		/* Hostname mismatch or no hostnames found in certificate. */
		SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

static void SecPolicyCheckValidIntermediates(SecPVCRef pvc,
	CFStringRef key) {
	CFIndex ix, count = SecPVCGetCertificateCount(pvc);
	CFAbsoluteTime verifyTime = SecPVCGetVerifyTime(pvc);
	for (ix = 1; ix < count - 1; ++ix) {
		SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
		if (!SecCertificateIsValid(cert, verifyTime)) {
			/* Intermediate certificate has expired. */
			if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
				return;
		}
	}
}

static void SecPolicyCheckValidLeaf(SecPVCRef pvc,
	CFStringRef key) {
	CFAbsoluteTime verifyTime = SecPVCGetVerifyTime(pvc);
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
	if (!SecCertificateIsValid(cert, verifyTime)) {
		/* Leaf certificate has expired. */
		if (!SecPVCSetResult(pvc, key, 0, kCFBooleanFalse))
			return;
	}
}

static void SecPolicyCheckValidRoot(SecPVCRef pvc,
	CFStringRef key) {
	CFIndex ix, count = SecPVCGetCertificateCount(pvc);
	CFAbsoluteTime verifyTime = SecPVCGetVerifyTime(pvc);
	ix = count - 1;
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
	if (!SecCertificateIsValid(cert, verifyTime)) {
		/* Root certificate has expired. */
		if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
			return;
	}
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckIssuerCommonName(SecPVCRef pvc,
	CFStringRef key) {
    CFIndex count = SecPVCGetCertificateCount(pvc);
    if (count < 2) {
		/* Can't check intermediates common name if there is no intermediate. */
		SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
        return;
    }

	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 1);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFStringRef commonName =
        (CFStringRef)CFDictionaryGetValue(policy->_options, key);
    if (!isString(commonName)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }
    if (!SecPolicyCheckCertSubjectCommonName(cert, commonName)) {
        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckSubjectCommonName(SecPVCRef pvc,
	CFStringRef key) {
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
	CFStringRef common_name = (CFStringRef)CFDictionaryGetValue(policy->_options,
		key);
    if (!isString(common_name)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }
    if (!SecPolicyCheckCertSubjectCommonName(cert, common_name)) {
		SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckSubjectCommonNamePrefix(SecPVCRef pvc,
	CFStringRef key) {
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFStringRef prefix = (CFStringRef)CFDictionaryGetValue(policy->_options,
		key);
    if (!isString(prefix)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }
    if (!SecPolicyCheckCertSubjectCommonNamePrefix(cert, prefix)) {
        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckSubjectCommonNameTEST(SecPVCRef pvc,
	CFStringRef key) {
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
	CFStringRef common_name = (CFStringRef)CFDictionaryGetValue(policy->_options,
		key);
    if (!isString(common_name)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }
    if (!SecPolicyCheckCertSubjectCommonNameTEST(cert, common_name)) {
        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckNotValidBefore(SecPVCRef pvc,
	CFStringRef key) {
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFDateRef date = (CFDateRef)CFDictionaryGetValue(policy->_options, key);
    if (!isDate(date)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }
    if (!SecPolicyCheckCertNotValidBefore(cert, date)) {
		if (!SecPVCSetResult(pvc, key, 0, kCFBooleanFalse))
			return;
	}
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckChainLength(SecPVCRef pvc,
	CFStringRef key) {
	CFIndex count = SecPVCGetCertificateCount(pvc);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFNumberRef chainLength =
        (CFNumberRef)CFDictionaryGetValue(policy->_options, key);
    CFIndex value;
    if (!chainLength || CFGetTypeID(chainLength) != CFNumberGetTypeID() ||
        !CFNumberGetValue(chainLength, kCFNumberCFIndexType, &value)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }
    if (value != count) {
		/* Chain length doesn't match policy requirement. */
		if (!SecPVCSetResult(pvc, key, 0, kCFBooleanFalse))
			return;
    }
}

static bool isDigestInPolicy(SecPVCRef pvc, CFStringRef key, CFDataRef digest) {
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef value = CFDictionaryGetValue(policy->_options, key);

    bool foundMatch = false;
    if (isData(value))
        foundMatch = CFEqual(digest, value);
    else if (isArray(value))
        foundMatch = CFArrayContainsValue((CFArrayRef) value, CFRangeMake(0, CFArrayGetCount((CFArrayRef) value)), digest);
    else {
        /* @@@ We only support Data and Array but we can't return an error here so.
         we let the evaluation fail (not much help) and assert in debug. */
        assert(false);
    }

    return foundMatch;
}

static void SecPolicyCheckAnchorSHA256(SecPVCRef pvc, CFStringRef key) {
    CFIndex count = SecPVCGetCertificateCount(pvc);
    SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, count - 1);
    CFDataRef anchorSHA256 = NULL;
    anchorSHA256 = SecCertificateCopySHA256Digest(cert);

    if (!isDigestInPolicy(pvc, key, anchorSHA256)) {
        SecPVCSetResult(pvc, kSecPolicyCheckAnchorSHA256, count-1, kCFBooleanFalse);
    }

    CFReleaseNull(anchorSHA256);
    return;
}


/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckAnchorSHA1(SecPVCRef pvc,
	CFStringRef key) {
    CFIndex count = SecPVCGetCertificateCount(pvc);
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, count - 1);
    CFDataRef anchorSHA1 = SecCertificateGetSHA1Digest(cert);

    if (!isDigestInPolicy(pvc, key, anchorSHA1))
        if (!SecPVCSetResult(pvc, kSecPolicyCheckAnchorSHA1, count-1, kCFBooleanFalse))
            return;

    return;
}

/*
   Check the SHA256 of SPKI of the first intermediate CA certificate in the path
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckIntermediateSPKISHA256(SecPVCRef pvc,
                                                 CFStringRef key) {
    SecCertificateRef cert = NULL;
    CFDataRef digest = NULL;

    if (SecPVCGetCertificateCount(pvc) < 2) {
        SecPVCSetResult(pvc, kSecPolicyCheckIntermediateSPKISHA256, 0, kCFBooleanFalse);
        return;
    }

    cert = SecPVCGetCertificateAtIndex(pvc, 1);
    digest = SecCertificateCopySubjectPublicKeyInfoSHA256Digest(cert);

    if (!isDigestInPolicy(pvc, key, digest)) {
        SecPVCSetResult(pvc, kSecPolicyCheckIntermediateSPKISHA256, 1, kCFBooleanFalse);
    }
    CFReleaseNull(digest);
}

/*
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckAnchorApple(SecPVCRef pvc,
                                      CFStringRef key) {
    CFIndex count = SecPVCGetCertificateCount(pvc);
    SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, count - 1);
    SecAppleTrustAnchorFlags flags = 0;


    bool foundMatch = SecIsAppleTrustAnchor(cert, flags);

    if (!foundMatch)
        if (!SecPVCSetResult(pvc, kSecPolicyCheckAnchorApple, 0, kCFBooleanFalse))
            return;

    return;
}


/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckSubjectOrganization(SecPVCRef pvc,
	CFStringRef key) {
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
	CFStringRef org = (CFStringRef)CFDictionaryGetValue(policy->_options,
		key);
    if (!isString(org)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }
    if (!SecPolicyCheckCertSubjectOrganization(cert, org)) {
		/* Leaf Subject Organization mismatch. */
		SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
	}
}

static void SecPolicyCheckSubjectOrganizationalUnit(SecPVCRef pvc,
	CFStringRef key) {
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
	CFStringRef orgUnit = (CFStringRef)CFDictionaryGetValue(policy->_options,
		key);
    if (!isString(orgUnit)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }
    if (!SecPolicyCheckCertSubjectOrganizationalUnit(cert, orgUnit)) {
        /* Leaf Subject Organization mismatch. */
        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
static void SecPolicyCheckEAPTrustedServerNames(SecPVCRef pvc,
	CFStringRef key) {
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
	CFArrayRef trustedServerNames = (CFArrayRef)
		CFDictionaryGetValue(policy->_options, key);
    /* No names specified means we accept any name. */
    if (!trustedServerNames)
        return;
    if (!isArray(trustedServerNames)) {
        /* @@@ We can't return an error here and making the evaluation fail
           won't help much either. */
        return;
    }

	SecCertificateRef leaf = SecPVCGetCertificateAtIndex(pvc, 0);
    if (!SecPolicyCheckCertEAPTrustedServerNames(leaf, trustedServerNames)) {
		/* Hostname mismatch or no hostnames found in certificate. */
		SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
	}
}

static const unsigned char UTN_USERFirst_Hardware_Serial[][16] = {
{ 0xd8, 0xf3, 0x5f, 0x4e, 0xb7, 0x87, 0x2b, 0x2d, 0xab, 0x06, 0x92, 0xe3, 0x15, 0x38, 0x2f, 0xb0 },
{ 0x92, 0x39, 0xd5, 0x34, 0x8f, 0x40, 0xd1, 0x69, 0x5a, 0x74, 0x54, 0x70, 0xe1, 0xf2, 0x3f, 0x43 },
{ 0xb0, 0xb7, 0x13, 0x3e, 0xd0, 0x96, 0xf9, 0xb5, 0x6f, 0xae, 0x91, 0xc8, 0x74, 0xbd, 0x3a, 0xc0 },
{ 0xe9, 0x02, 0x8b, 0x95, 0x78, 0xe4, 0x15, 0xdc, 0x1a, 0x71, 0x0a, 0x2b, 0x88, 0x15, 0x44, 0x47 },
{ 0x39, 0x2a, 0x43, 0x4f, 0x0e, 0x07, 0xdf, 0x1f, 0x8a, 0xa3, 0x05, 0xde, 0x34, 0xe0, 0xc2, 0x29 },
{ 0x3e, 0x75, 0xce, 0xd4, 0x6b, 0x69, 0x30, 0x21, 0x21, 0x88, 0x30, 0xae, 0x86, 0xa8, 0x2a, 0x71 },
{ 0xd7, 0x55, 0x8f, 0xda, 0xf5, 0xf1, 0x10, 0x5b, 0xb2, 0x13, 0x28, 0x2b, 0x70, 0x77, 0x29, 0xa3 },
{ 0x04, 0x7e, 0xcb, 0xe9, 0xfc, 0xa5, 0x5f, 0x7b, 0xd0, 0x9e, 0xae, 0x36, 0xe1, 0x0c, 0xae, 0x1e },
{ 0xf5, 0xc8, 0x6a, 0xf3, 0x61, 0x62, 0xf1, 0x3a, 0x64, 0xf5, 0x4f, 0x6d, 0xc9, 0x58, 0x7c, 0x06 } };

static const unsigned char UTN_USERFirst_Hardware_Normalized_Issuer[] = {
  0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55,
  0x53, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x08, 0x13, 0x02,
  0x55, 0x54, 0x31, 0x17, 0x30, 0x15, 0x06, 0x03, 0x55, 0x04, 0x07, 0x13,
  0x0e, 0x53, 0x41, 0x4c, 0x54, 0x20, 0x4c, 0x41, 0x4b, 0x45, 0x20, 0x43,
  0x49, 0x54, 0x59, 0x31, 0x1e, 0x30, 0x1c, 0x06, 0x03, 0x55, 0x04, 0x0a,
  0x13, 0x15, 0x54, 0x48, 0x45, 0x20, 0x55, 0x53, 0x45, 0x52, 0x54, 0x52,
  0x55, 0x53, 0x54, 0x20, 0x4e, 0x45, 0x54, 0x57, 0x4f, 0x52, 0x4b, 0x31,
  0x21, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x04, 0x0b, 0x13, 0x18, 0x48, 0x54,
  0x54, 0x50, 0x3a, 0x2f, 0x2f, 0x57, 0x57, 0x57, 0x2e, 0x55, 0x53, 0x45,
  0x52, 0x54, 0x52, 0x55, 0x53, 0x54, 0x2e, 0x43, 0x4f, 0x4d, 0x31, 0x1f,
  0x30, 0x1d, 0x06, 0x03, 0x55, 0x04, 0x03, 0x13, 0x16, 0x55, 0x54, 0x4e,
  0x2d, 0x55, 0x53, 0x45, 0x52, 0x46, 0x49, 0x52, 0x53, 0x54, 0x2d, 0x48,
  0x41, 0x52, 0x44, 0x57, 0x41, 0x52, 0x45
};
static const unsigned int UTN_USERFirst_Hardware_Normalized_Issuer_len = 151;


static void SecPolicyCheckBlackListedLeaf(SecPVCRef pvc,
	CFStringRef key) {
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
    CFDataRef issuer = cert ? SecCertificateGetNormalizedIssuerContent(cert) : NULL;

    if (issuer && (CFDataGetLength(issuer) == (CFIndex)UTN_USERFirst_Hardware_Normalized_Issuer_len) &&
        (0 == memcmp(UTN_USERFirst_Hardware_Normalized_Issuer, CFDataGetBytePtr(issuer),
            UTN_USERFirst_Hardware_Normalized_Issuer_len)))
    {
    #if (TARGET_OS_MAC && !(TARGET_OS_EMBEDDED || TARGET_OS_IPHONE))
        CFDataRef serial = SecCertificateCopySerialNumber(cert, NULL);
    #else
        CFDataRef serial = SecCertificateCopySerialNumber(cert);
    #endif

        if (serial) {
            CFIndex serial_length = CFDataGetLength(serial);
            const uint8_t *serial_ptr = CFDataGetBytePtr(serial);

            while ((serial_length > 0) && (*serial_ptr == 0)) {
                serial_ptr++;
                serial_length--;
            }

            if (serial_length == (CFIndex)sizeof(*UTN_USERFirst_Hardware_Serial)) {
                unsigned int i;
                for (i = 0; i < array_size(UTN_USERFirst_Hardware_Serial); i++)
                {
                    if (0 == memcmp(UTN_USERFirst_Hardware_Serial[i],
                        serial_ptr, sizeof(*UTN_USERFirst_Hardware_Serial)))
                    {
                        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
                        pvc->result = kSecTrustResultFatalTrustFailure;
                        CFReleaseSafe(serial);
                        return;
                    }
                }
            }
            CFRelease(serial);
        }
    }

	SecOTAPKIRef otapkiRef = SecOTAPKICopyCurrentOTAPKIRef();
	if (NULL != otapkiRef)
	{
		CFSetRef blackListedKeys = SecOTAPKICopyBlackListSet(otapkiRef);
		CFRelease(otapkiRef);
		if (NULL != blackListedKeys)
		{
			/* Check for blacklisted intermediates keys. */
			CFDataRef dgst = SecCertificateCopyPublicKeySHA1Digest(cert);
			if (dgst)
			{
				/* Check dgst against blacklist. */
				if (CFSetContainsValue(blackListedKeys, dgst))
				{
					SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
                    pvc->result = kSecTrustResultFatalTrustFailure;
				}
				CFRelease(dgst);
			}
			CFRelease(blackListedKeys);
		}
	}
}

static void SecPolicyCheckGrayListedLeaf(SecPVCRef pvc, CFStringRef key)
{
	SecOTAPKIRef otapkiRef = SecOTAPKICopyCurrentOTAPKIRef();
	if (NULL != otapkiRef)
	{
		CFSetRef grayListedKeys = SecOTAPKICopyGrayList(otapkiRef);
		CFRelease(otapkiRef);
		if (NULL != grayListedKeys)
		{
			SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);

			CFDataRef dgst = SecCertificateCopyPublicKeySHA1Digest(cert);
			if (dgst)
			{
				/* Check dgst against gray. */
				if (CFSetContainsValue(grayListedKeys, dgst))
				{
					SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
				}
				CFRelease(dgst);
			}
			CFRelease(grayListedKeys);
		}
	}
}

static void SecPolicyCheckLeafMarkerOid(SecPVCRef pvc, CFStringRef key)
{
	SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef value = CFDictionaryGetValue(policy->_options, key);

    if (!SecPolicyCheckCertLeafMarkerOid(cert, value)) {
        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

static void SecPolicyCheckLeafMarkerOidWithoutValueCheck(SecPVCRef pvc, CFStringRef key)
{
    SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef value = CFDictionaryGetValue(policy->_options, key);

    if (!SecPolicyCheckCertLeafMarkerOidWithoutValueCheck(cert, value)) {
        SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}

/*
 * The value is a dictionary. The dictionary contains keys indicating
 * whether the value is for Prod or QA. The values are the same as
 * in the options dictionary for SecPolicyCheckLeafMarkerOid.
 */
static void SecPolicyCheckLeafMarkersProdAndQA(SecPVCRef pvc, CFStringRef key)
{
    SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFDictionaryRef value = CFDictionaryGetValue(policy->_options, key);
    CFTypeRef prodValue = CFDictionaryGetValue(value, kSecPolicyLeafMarkerProd);

    if (!SecPolicyCheckCertLeafMarkerOid(cert, prodValue)) {
        bool result = false;
        if (!result) {
            SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
        }
    }
}

static void SecPolicyCheckIntermediateMarkerOid(SecPVCRef pvc, CFStringRef key)
{
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef value = CFDictionaryGetValue(policy->_options, key);

    for (ix = 1; ix < count - 1; ix++) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (SecCertificateHasMarkerExtension(cert, value))
            return;
    }
    SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
}

static void SecPolicyCheckIntermediateEKU(SecPVCRef pvc, CFStringRef key)
{
	CFIndex ix, count = SecPVCGetCertificateCount(pvc);
	SecPolicyRef policy = SecPVCGetPolicy(pvc);
	CFTypeRef peku = CFDictionaryGetValue(policy->_options, key);

	for (ix = 1; ix < count - 1; ix++) {
		SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (!SecPolicyCheckCertExtendedKeyUsage(cert, peku)) {
			SecPVCSetResult(pvc, key, ix, kCFBooleanFalse);
		}
	}
}

static void SecPolicyCheckIntermediateOrganization(SecPVCRef pvc, CFStringRef key)
{
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef organization = CFDictionaryGetValue(policy->_options, key);

    for (ix = 1; ix < count - 1; ix++) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (!SecPolicyCheckCertSubjectOrganization(cert, organization)) {
            SecPVCSetResult(pvc, key, ix, kCFBooleanFalse);
        }
    }
}

static void SecPolicyCheckIntermediateCountry(SecPVCRef pvc, CFStringRef key)
{
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef country = CFDictionaryGetValue(policy->_options, key);

    for (ix = 1; ix < count - 1; ix++) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (!SecPolicyCheckCertSubjectCountry(cert, country)) {
            SecPVCSetResult(pvc, key, ix, kCFBooleanFalse);
        }
    }
}

/****************************************************************************
 *********************** New rfc5280 Chain Validation ***********************
 ****************************************************************************/

#define POLICY_MAPPING 1
#define POLICY_SUBTREES 1

/* rfc5280 basic cert processing. */
static void SecPolicyCheckBasicCertificateProcessing(SecPVCRef pvc,
	CFStringRef key) {
    /* Inputs */
    //cert_path_t path;
    CFIndex count = SecPVCGetCertificateCount(pvc);
    SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
    /* 64 bits cast: worst case here is we truncate the number of cert, and the validation may fail */
    assert((unsigned long)count<=UINT32_MAX); /* Debug check. Correct as long as CFIndex is long */
    uint32_t n = (uint32_t)count;

    bool is_anchored = SecPathBuilderIsAnchored(pvc->builder);
    bool is_anchor_trusted = false;
    if (is_anchored) {
        CFArrayRef constraints = SecCertificatePathVCGetUsageConstraintsAtIndex(path, n - 1);
        if (CFArrayGetCount(constraints) == 0) {
            /* Given that the path builder has already indicated the last cert in this chain has
             * trust set on it, empty constraints means trusted. */
            is_anchor_trusted = true;
        } else {
            /* Determine whether constraints say to trust this cert for this PVC. */
            SecTrustSettingsResult tsResult = SecPVCGetTrustSettingsResult(pvc, SecCertificatePathVCGetCertificateAtIndex(path, n - 1),
                                                                           constraints);
            if (tsResult == kSecTrustSettingsResultTrustRoot || tsResult == kSecTrustSettingsResultTrustAsRoot) {
                is_anchor_trusted = true;
            }
        }
    }

    if (is_anchor_trusted) {
        /* If the anchor is trusted we don't process the last cert in the
           chain (root). */
        n--;
    } else {
        /* trust may be restored for a path with an untrusted root that matches the allow list.
           (isAllowlisted is set by revocation check, which is performed prior to path checks) */
        if (!SecCertificatePathVCIsAllowlisted(path)) {
            /* Add a detail for the root not being trusted. */
            if (!SecPVCSetResultForced(pvc, kSecPolicyCheckAnchorTrusted,
                                      n - 1, kCFBooleanFalse, true)) {
                return;
            }
        }
    }

    CFAbsoluteTime verify_time = SecPVCGetVerifyTime(pvc);
    //policy_set_t user_initial_policy_set = NULL;
    //trust_anchor_t anchor;

    /* Initialization */
#if POLICY_SUBTREES
    CFMutableArrayRef permitted_subtrees = NULL;
    CFMutableArrayRef excluded_subtrees = NULL;
    permitted_subtrees = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    excluded_subtrees = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    require_action_quiet(permitted_subtrees != NULL, errOut,
                         SecPVCSetResultForced(pvc, key, 0, kCFBooleanFalse, true));
    require_action_quiet(excluded_subtrees != NULL, errOut,
                         SecPVCSetResultForced(pvc, key, 0, kCFBooleanFalse, true));
#endif

    if (!SecCertificatePathVCVerifyPolicyTree(path, is_anchor_trusted)) {
        if (!SecPVCSetResultForced(pvc, key, 0, kCFBooleanFalse, true)) {
            goto errOut;
        }
    }

#if 0
    /* Path builder ensures we only get cert chains with proper issuer
       chaining with valid signatures along the way. */
    algorithm_id_t working_public_key_algorithm = anchor->public_key_algorithm;
    SecKeyRef working_public_key = anchor->public_key;
    x500_name_t working_issuer_name = anchor->issuer_name;
#endif
    uint32_t i, max_path_length = n;
    SecCertificateRef cert = NULL;
    for (i = 1; i <= n; ++i) {
        /* Process Cert */
        cert = SecPVCGetCertificateAtIndex(pvc, n - i);
        bool is_self_issued = SecCertificatePathVCIsCertificateAtIndexSelfIssued(SecPathBuilderGetPath(pvc->builder), n - i);

        /* (a) Verify the basic certificate information. */
        /* @@@ Ensure that cert was signed with working_public_key_algorithm
           using the working_public_key and the working_public_key_parameters. */
#if 1
        /* Already done by chain builder. */
        if (!SecCertificateIsValid(cert, verify_time)) {
            CFStringRef fail_key = i == n ? kSecPolicyCheckValidLeaf : kSecPolicyCheckValidIntermediates;
            if (!SecPVCSetResult(pvc, fail_key, n - i, kCFBooleanFalse)) {
                goto errOut;
            }
        }
        if (SecCertificateIsWeakKey(cert)) {
            CFStringRef fail_key = i == n ? kSecPolicyCheckWeakLeaf : kSecPolicyCheckWeakIntermediates;
            if (!SecPVCSetResult(pvc, fail_key, n - i, kCFBooleanFalse)) {
                goto errOut;
            }
            pvc->result = kSecTrustResultFatalTrustFailure;
        }
#endif
        /* @@@ cert.issuer == working_issuer_name. */

#if POLICY_SUBTREES
        /* (b) (c) */
        if (!is_self_issued || i == n) {
            bool found = false;
            /* Verify certificate Subject Name and SubjectAltNames are not within any of the excluded_subtrees */
            if(excluded_subtrees && CFArrayGetCount(excluded_subtrees)) {
                if ((errSecSuccess != SecNameContraintsMatchSubtrees(cert, excluded_subtrees, &found, false)) || found) {
                    secnotice("policy", "name in excluded subtrees");
                    if(!SecPVCSetResultForced(pvc, key, n - i, kCFBooleanFalse, true)) { goto errOut; }
                }
            }
            /* Verify certificate Subject Name and SubjectAltNames are within the permitted_subtrees */
            if(permitted_subtrees && CFArrayGetCount(permitted_subtrees)) {
               if ((errSecSuccess != SecNameContraintsMatchSubtrees(cert, permitted_subtrees, &found, true)) || !found) {
                   secnotice("policy", "name not in permitted subtrees");
                   if(!SecPVCSetResultForced(pvc, key, n - i, kCFBooleanFalse, true)) { goto errOut; }
               }
            }
        }
#endif
        /* (d) (e) (f) handled by SecCertificatePathVCVerifyPolicyTree */

        /* If Last Cert in Path */
        if (i == n)
            break;

        /* Prepare for Next Cert */
        /* (a) (b) Done by SecCertificatePathVCVerifyPolicyTree */
        /* (c)(d)(e)(f)  Done by SecPathBuilderGetNext and SecCertificatePathVCVerify */
        //working_issuer_name = SecCertificateGetNormalizedSubjectContent(cert);
        //working_public_key = SecCertificateCopyPublicKey(cert);
        //working_public_key_parameters = SecCertificateCopyPublicKeyParameters(cert);
        //working_public_key_algorithm = SecCertificateCopyPublicKeyAlgorithm(cert);
#if POLICY_SUBTREES
        /* (g) If a name constraints extension is included in the certificate, modify the permitted_subtrees and excluded_subtrees state variables.
         */
        CFArrayRef permitted_subtrees_in_cert = SecCertificateGetPermittedSubtrees(cert);
        if (permitted_subtrees_in_cert) {
            SecNameConstraintsIntersectSubtrees(permitted_subtrees, permitted_subtrees_in_cert);
        }

        // could do something smart here to avoid inserting the exact same constraint
        CFArrayRef excluded_subtrees_in_cert = SecCertificateGetExcludedSubtrees(cert);
        if (excluded_subtrees_in_cert) {
            CFIndex num_trees = CFArrayGetCount(excluded_subtrees_in_cert);
            CFRange range = { 0, num_trees };
            CFArrayAppendArray(excluded_subtrees, excluded_subtrees_in_cert, range);
        }
#endif
        /* (h), (i), (j) done by SecCertificatePathVCVerifyPolicyTree */

        /* (k) */
		const SecCEBasicConstraints *bc =
			SecCertificateGetBasicConstraints(cert);
#if 0 /* Checked in chain builder pre signature verify already. SecPVCParentCertificateChecks */
        if (!bc || !bc->isCA) {
            /* Basic constraints not present or not marked as isCA, illegal. */
            if (!SecPVCSetResult(pvc, kSecPolicyCheckBasicConstraints,
                                 n - i, kCFBooleanFalse)) {
                goto errOut;
            }
        }
#endif
        /* (l) */
        if (!is_self_issued) {
            if (max_path_length > 0) {
                max_path_length--;
            } else {
                /* max_path_len exceeded, illegal. */
                if (!SecPVCSetResult(pvc, kSecPolicyCheckBasicConstraints,
                                     n - i, kCFBooleanFalse)) {
                    goto errOut;
                }
            }
        }
        /* (m) */
        if (bc && bc->pathLenConstraintPresent
            && bc->pathLenConstraint < max_path_length) {
            max_path_length = bc->pathLenConstraint;
        }
#if 0 /* Checked in chain builder pre signature verify already. SecPVCParentCertificateChecks */
        /* (n) If a key usage extension is present, verify that the keyCertSign bit is set. */
        SecKeyUsage keyUsage = SecCertificateGetKeyUsage(cert);
        if (keyUsage && !(keyUsage & kSecKeyUsageKeyCertSign)) {
            if (!SecPVCSetResultForced(pvc, kSecPolicyCheckKeyUsage,
                                       n - i, kCFBooleanFalse, true)) {
                goto errOut;
            }
        }
#endif
        /* (o) Recognize and process any other critical extension present in the certificate. Process any other recognized non-critical extension present in the certificate that is relevant to path processing. */
        if (SecCertificateHasUnknownCriticalExtension(cert)) {
			/* Certificate contains one or more unknown critical extensions. */
			if (!SecPVCSetResult(pvc, kSecPolicyCheckCriticalExtensions,
                                 n - i, kCFBooleanFalse)) {
                goto errOut;
            }
		}
    } /* end loop over certs in path */
    /* Wrap up */
    /* (a) (b) done by SecCertificatePathVCVerifyPolicyTree */
    /* (c) */
    //working_public_key = SecCertificateCopyPublicKey(cert);
    /* (d) */
    /* If the subjectPublicKeyInfo field of the certificate contains an algorithm field with null parameters or parameters are omitted, compare the certificate subjectPublicKey algorithm to the working_public_key_algorithm. If the certificate subjectPublicKey algorithm and the
working_public_key_algorithm are different, set the working_public_key_parameters to null. */
    //working_public_key_parameters = SecCertificateCopyPublicKeyParameters(cert);
    /* (e) */
    //working_public_key_algorithm = SecCertificateCopyPublicKeyAlgorithm(cert);
    /* (f) Recognize and process any other critical extension present in the certificate n. Process any other recognized non-critical extension present in certificate n that is relevant to path processing. */
    if (SecCertificateHasUnknownCriticalExtension(cert)) {
        /* Certificate contains one or more unknown critical extensions. */
        if (!SecPVCSetResult(pvc, kSecPolicyCheckCriticalExtensions,
                             0, kCFBooleanFalse)) {
            goto errOut;
        }
    }
    /* (g) done by SecCertificatePathVCVerifyPolicyTree */

errOut:
    CFReleaseNull(permitted_subtrees);
    CFReleaseNull(excluded_subtrees);
}

static policy_set_t policies_for_cert(SecCertificateRef cert) {
    policy_set_t policies = NULL;
    const SecCECertificatePolicies *cp =
        SecCertificateGetCertificatePolicies(cert);
    size_t policy_ix, policy_count = cp ? cp->numPolicies : 0;
    for (policy_ix = 0; policy_ix < policy_count; ++policy_ix) {
        policy_set_add(&policies, &cp->policies[policy_ix].policyIdentifier);
    }
    return policies;
}

static void SecPolicyCheckEV(SecPVCRef pvc,
	CFStringRef key) {
	CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    policy_set_t valid_policies = NULL;

    /* 6.1.7. Key Usage Purposes */
    if (count) {
        CFAbsoluteTime jul2016 = 489024000;
        SecCertificateRef leaf = SecPVCGetCertificateAtIndex(pvc, 0);
        if (SecCertificateNotValidBefore(leaf) > jul2016 && count < 3) {
            /* Root CAs may not sign subscriber certificates after 30 June 2016. */
            if (SecPVCSetResultForced(pvc, key,
                    0, kCFBooleanFalse, true)) {
                return;
            }
        }
    }

	for (ix = 0; ix < count; ++ix) {
		SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        policy_set_t policies = policies_for_cert(cert);
        if (ix == 0) {
            /* Subscriber */
            /* anyPolicy in the leaf isn't allowed for EV, so only init
               valid_policies if we have real policies. */
            if (!policy_set_contains(policies, &oidAnyPolicy)) {
                valid_policies = policies;
                policies = NULL;
            }
        } else if (ix < count - 1) {
            /* Subordinate CA */
            if (!SecPolicySubordinateCACertificateCouldBeEV(cert)) {
                secnotice("ev", "subordinate certificate is not ev");
                if (SecPVCSetResultForced(pvc, key,
                    ix, kCFBooleanFalse, true)) {
                    policy_set_free(valid_policies);
                    policy_set_free(policies);
                    return;
                }
            }
            policy_set_intersect(&valid_policies, policies);
        } else {
            /* Root CA */
            if (!SecPolicyRootCACertificateIsEV(cert, valid_policies)) {
                secnotice("ev", "anchor certificate is not ev");
                if (SecPVCSetResultForced(pvc, key,
                    ix, kCFBooleanFalse, true)) {
                    policy_set_free(valid_policies);
                    policy_set_free(policies);
                    return;
                }
            }
        }
        policy_set_free(policies);
        if (!valid_policies) {
            secnotice("ev", "valid_policies set is empty: chain not ev");
            /* If we ever get into a state where no policies are valid anymore
               this can't be an ev chain. */
            if (SecPVCSetResultForced(pvc, key,
                ix, kCFBooleanFalse, true)) {
                return;
            }
        }
	}

    policy_set_free(valid_policies);

    /* (a) EV Subscriber Certificates   Each EV Certificate issued by the CA to a
Subscriber MUST contain an OID defined by the CA in the certificate’s
certificatePolicies extension that: (i) indicates which CA policy statement relates
to that certificate, (ii) asserts the CA’s adherence to and compliance with these
Guidelines, and (iii), by pre-agreement with the Application Software Vendor,
marks the certificate as being an EV Certificate.
(b) EV Subordinate CA Certificates
(1) Certificates issued to Subordinate CAs that are not controlled by the issuing
CA MUST contain one or more OIDs defined by the issuing CA that
explicitly identify the EV Policies that are implemented by the Subordinate
CA;
(2) Certificates issued to Subordinate CAs that are controlled by the Root CA
MAY contain the special anyPolicy OID (2.5.29.32.0).
(c) Root CA Certificates  Root CA Certificates SHOULD NOT contain the
certificatePolicies or extendedKeyUsage extensions.
*/
}


/*
 * MARK: Certificate Transparency support
 */

/***

struct {
    Version sct_version;        // 1 byte
    LogID id;                   // 32 bytes
    uint64 timestamp;           // 8 bytes
    CtExtensions extensions;    // 2 bytes len field, + n bytes data
    digitally-signed struct {   // 1 byte hash alg, 1 byte sig alg, n bytes signature
        Version sct_version;
        SignatureType signature_type = certificate_timestamp;
        uint64 timestamp;
        LogEntryType entry_type;
        select(entry_type) {
        case x509_entry: ASN.1Cert;
        case precert_entry: PreCert;
        } signed_entry;
        CtExtensions extensions;
    };
} SignedCertificateTimestamp;

***/

#include <Security/SecureTransportPriv.h>

static const
SecAsn1Oid *oidForSigAlg(SSL_HashAlgorithm hash, SSL_SignatureAlgorithm alg)
{
    switch(alg) {
        case SSL_SignatureAlgorithmRSA:
            switch (hash) {
                case SSL_HashAlgorithmSHA1:
                    return &CSSMOID_SHA1WithRSA;
                case SSL_HashAlgorithmSHA256:
                    return &CSSMOID_SHA256WithRSA;
                case SSL_HashAlgorithmSHA384:
                    return &CSSMOID_SHA384WithRSA;
                default:
                    break;
            }
        case SSL_SignatureAlgorithmECDSA:
            switch (hash) {
                case SSL_HashAlgorithmSHA1:
                    return &CSSMOID_ECDSA_WithSHA1;
                case SSL_HashAlgorithmSHA256:
                    return &CSSMOID_ECDSA_WithSHA256;
                case SSL_HashAlgorithmSHA384:
                    return &CSSMOID_ECDSA_WithSHA384;
                default:
                    break;
            }
        default:
            break;
    }

    return NULL;
}


static size_t SSLDecodeUint16(const uint8_t *p)
{
    return (p[0]<<8 | p[1]);
}

static uint8_t *SSLEncodeUint16(uint8_t *p, size_t len)
{
    p[0] = (len >> 8)&0xff;
    p[1] = (len & 0xff);
    return p+2;
}

static uint8_t *SSLEncodeUint24(uint8_t *p, size_t len)
{
    p[0] = (len >> 16)&0xff;
    p[1] = (len >> 8)&0xff;
    p[2] = (len & 0xff);
    return p+3;
}


static
uint64_t SSLDecodeUint64(const uint8_t *p)
{
    uint64_t u = 0;
    for(int i=0; i<8; i++) {
        u=(u<<8)|p[0];
        p++;
    }
    return u;
}

#include <libDER/DER_CertCrl.h>
#include <libDER/DER_Encode.h>
#include <libDER/asn1Types.h>


static CFDataRef copy_x509_entry_from_chain(SecPVCRef pvc)
{
    SecCertificateRef leafCert = SecPVCGetCertificateAtIndex(pvc, 0);

    CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 3+SecCertificateGetLength(leafCert));

    CFDataSetLength(data, 3+SecCertificateGetLength(leafCert));

    uint8_t *q = CFDataGetMutableBytePtr(data);
    q = SSLEncodeUint24(q, SecCertificateGetLength(leafCert));
    memcpy(q, SecCertificateGetBytePtr(leafCert), SecCertificateGetLength(leafCert));

    return data;
}


static CFDataRef copy_precert_entry_from_chain(SecPVCRef pvc)
{
    SecCertificateRef leafCert = NULL;
    SecCertificateRef issuer = NULL;
    CFDataRef issuerKeyHash = NULL;
    CFDataRef tbs_precert = NULL;
    CFMutableDataRef data= NULL;

    require_quiet(SecPVCGetCertificateCount(pvc)>=2, out); //we need the issuer key for precerts.
    leafCert = SecPVCGetCertificateAtIndex(pvc, 0);
    issuer = SecPVCGetCertificateAtIndex(pvc, 1);

    require(leafCert, out);
    require(issuer, out); // Those two would likely indicate an internal error, since we already checked the chain length above.
    issuerKeyHash = SecCertificateCopySubjectPublicKeyInfoSHA256Digest(issuer);
    tbs_precert = SecCertificateCopyPrecertTBS(leafCert);

    require(issuerKeyHash, out);
    require(tbs_precert, out);
    data = CFDataCreateMutable(kCFAllocatorDefault, CFDataGetLength(issuerKeyHash) + 3 + CFDataGetLength(tbs_precert));
    CFDataSetLength(data, CFDataGetLength(issuerKeyHash) + 3 + CFDataGetLength(tbs_precert));

    uint8_t *q = CFDataGetMutableBytePtr(data);
    memcpy(q, CFDataGetBytePtr(issuerKeyHash), CFDataGetLength(issuerKeyHash)); q += CFDataGetLength(issuerKeyHash); // issuer key hash
    q = SSLEncodeUint24(q, CFDataGetLength(tbs_precert));
    memcpy(q, CFDataGetBytePtr(tbs_precert), CFDataGetLength(tbs_precert));

out:
    CFReleaseSafe(issuerKeyHash);
    CFReleaseSafe(tbs_precert);
    return data;
}

static
CFAbsoluteTime TimestampToCFAbsoluteTime(uint64_t ts)
{
    return (ts / 1000) - kCFAbsoluteTimeIntervalSince1970;
}

static
uint64_t TimestampFromCFAbsoluteTime(CFAbsoluteTime at)
{
    return (uint64_t)(at + kCFAbsoluteTimeIntervalSince1970) * 1000;
}




/*
   If the 'sct' is valid, add it to the validatingLogs dictionary.

   Inputs:
    - validatingLogs: mutable dictionary to which to add the log that validate this SCT.
    - sct: the SCT date
    - entry_type: 0 for x509 cert, 1 for precert.
    - entry: the cert or precert data.
    - vt: verification time timestamp (as used in SCTs: ms since 1970 Epoch)
    - trustedLog: Dictionary contain the Trusted Logs.

   The SCT is valid if:
    - It decodes properly.
    - Its timestamp is less than 'verifyTime'.
    - It is signed by a log in 'trustedLogs'.
    - If entry_type = 0, the log must be currently qualified.
    - If entry_type = 1, the log may be expired.

   If the SCT is valid, it's added to the validatinLogs dictionary using the log dictionary as the key, and the timestamp as value.
   If an entry for the same log already existing in the dictionary, the entry is replaced only if the timestamp of this SCT is earlier.

 */


static CFDictionaryRef getSCTValidatingLog(CFDataRef sct, int entry_type, CFDataRef entry, uint64_t vt, CFArrayRef trustedLogs, CFAbsoluteTime *sct_at)
{
    uint8_t version;
    const uint8_t *logID;
    const uint8_t *timestampData;
    uint64_t timestamp;
    size_t extensionsLen;
    const uint8_t *extensionsData;
    uint8_t hashAlg;
    uint8_t sigAlg;
    size_t signatureLen;
    const uint8_t *signatureData;
    SecKeyRef pubKey = NULL;
    uint8_t *signed_data = NULL;
    const SecAsn1Oid *oid = NULL;
    SecAsn1AlgId algId;
    CFDataRef logIDData = NULL;
    CFDictionaryRef result = 0;

    const uint8_t *p = CFDataGetBytePtr(sct);
    size_t len = CFDataGetLength(sct);

    require(len>=43, out);

    version = p[0]; p++; len--;
    logID = p; p+=32; len-=32;
    timestampData = p; p+=8; len-=8;
    extensionsLen = SSLDecodeUint16(p); p+=2; len-=2;

    require(len>=extensionsLen, out);
    extensionsData = p; p+=extensionsLen; len-=extensionsLen;

    require(len>=4, out);
    hashAlg=p[0]; p++; len--;
    sigAlg=p[0]; p++; len--;
    signatureLen = SSLDecodeUint16(p); p+=2; len-=2;
    require(len==signatureLen, out); /* We do not tolerate any extra data after the signature */
    signatureData = p;

    /* verify version: only v1(0) is supported */
    if(version!=0) {
        secerror("SCT version unsupported: %d\n", version);
        goto out;
    }

    /* verify timestamp not in the future */
    timestamp = SSLDecodeUint64(timestampData);
    if(timestamp > vt) {
        secerror("SCT is in the future: %llu > %llu\n", timestamp, vt);
        goto out;
    }

    uint8_t *q;

    /* signed entry */
    size_t signed_data_len = 12 + CFDataGetLength(entry) + 2 + extensionsLen ;
    signed_data = malloc(signed_data_len);
    require(signed_data, out);
    q = signed_data;
    *q++ = version;
    *q++ = 0; // certificate_timestamp
    memcpy(q, timestampData, 8); q+=8;
    q = SSLEncodeUint16(q, entry_type); // logentry type: 0=cert 1=precert
    memcpy(q, CFDataGetBytePtr(entry), CFDataGetLength(entry)); q += CFDataGetLength(entry);
    q = SSLEncodeUint16(q, extensionsLen);
    memcpy(q, extensionsData, extensionsLen);

    logIDData = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, logID, 32, kCFAllocatorNull);

    CFDictionaryRef logData = CFArrayGetValueMatching(trustedLogs, ^bool(const void *dict) {
        const void *key_data;
        if(!isDictionary(dict)) return false;
        if(!CFDictionaryGetValueIfPresent(dict, CFSTR("key"), &key_data)) return false;
        if(!isData(key_data)) return false;
        CFDataRef valueID = SecSHA256DigestCreateFromData(kCFAllocatorDefault, (CFDataRef)key_data);
        bool result = (bool)(CFDataCompare(logIDData, valueID)==kCFCompareEqualTo);
        CFReleaseSafe(valueID);
        return result;
    });
    require(logData, out);

    if(entry_type==0) {
        // For external SCTs, only keep SCTs from currently valid logs.
        require(!CFDictionaryContainsKey(logData, CFSTR("expiry")), out);
    }

    CFDataRef logKeyData = CFDictionaryGetValue(logData, CFSTR("key"));
    require(logKeyData, out); // This failing would be an internal logic error
    pubKey = SecKeyCreateFromSubjectPublicKeyInfoData(kCFAllocatorDefault, logKeyData);
    require(pubKey, out);

    oid = oidForSigAlg(hashAlg, sigAlg);
    require(oid, out);

    algId.algorithm = *oid;
    algId.parameters.Data = NULL;
    algId.parameters.Length = 0;

    if(SecKeyDigestAndVerify(pubKey, &algId, signed_data, signed_data_len, signatureData, signatureLen)==0) {
        *sct_at = TimestampToCFAbsoluteTime(timestamp);
        result = logData;
    } else {
        secerror("SCT signature failed (log=%@)\n", logData);
    }

out:
    CFReleaseSafe(logIDData);
    CFReleaseSafe(pubKey);
    free(signed_data);
    return result;
}


static void addValidatingLog(CFMutableDictionaryRef validatingLogs, CFDictionaryRef log, CFAbsoluteTime sct_at)
{
    CFDateRef validated_time = CFDictionaryGetValue(validatingLogs, log);

    if(validated_time==NULL || (sct_at < CFDateGetAbsoluteTime(validated_time))) {
        CFDateRef sct_time = CFDateCreate(kCFAllocatorDefault, sct_at);
        CFDictionarySetValue(validatingLogs, log, sct_time);
        CFReleaseSafe(sct_time);
    }
}

static CFArrayRef copy_ocsp_scts(SecPVCRef pvc)
{
    CFMutableArrayRef SCTs = NULL;
    SecCertificateRef leafCert = NULL;
    SecCertificateRef issuer = NULL;
    CFArrayRef ocspResponsesData = NULL;
    SecOCSPRequestRef ocspRequest = NULL;

    ocspResponsesData = SecPathBuilderCopyOCSPResponses(pvc->builder);
    require_quiet(ocspResponsesData, out);

    require_quiet(SecPVCGetCertificateCount(pvc)>=2, out); //we need the issuer key for precerts.
    leafCert = SecPVCGetCertificateAtIndex(pvc, 0);
    issuer = SecPVCGetCertificateAtIndex(pvc, 1);

    require(leafCert, out);
    require(issuer, out); // not quiet: Those two would likely indicate an internal error, since we already checked the chain length above.
    ocspRequest = SecOCSPRequestCreate(leafCert, issuer);

    SCTs = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    require(SCTs, out);

    CFArrayForEach(ocspResponsesData, ^(const void *value) {
        /* TODO: Should the builder already have the appropriate SecOCSPResponseRef ? */
        SecOCSPResponseRef ocspResponse = SecOCSPResponseCreate(value);
        if(ocspResponse && SecOCSPGetResponseStatus(ocspResponse)==kSecOCSPSuccess) {
            SecOCSPSingleResponseRef ocspSingleResponse = SecOCSPResponseCopySingleResponse(ocspResponse, ocspRequest);
            if(ocspSingleResponse) {
                CFArrayRef singleResponseSCTs = SecOCSPSingleResponseCopySCTs(ocspSingleResponse);
                if(singleResponseSCTs) {
                    CFArrayAppendArray(SCTs, singleResponseSCTs, CFRangeMake(0, CFArrayGetCount(singleResponseSCTs)));
                    CFRelease(singleResponseSCTs);
                }
                SecOCSPSingleResponseDestroy(ocspSingleResponse);
            }
        }
        if(ocspResponse) SecOCSPResponseFinalize(ocspResponse);
    });

    if(CFArrayGetCount(SCTs)==0) {
        CFReleaseNull(SCTs);
    }

out:
    CFReleaseSafe(ocspResponsesData);
    if(ocspRequest)
        SecOCSPRequestFinalize(ocspRequest);

    return SCTs;
}

static void SecPolicyCheckCT(SecPVCRef pvc, CFStringRef key)
{
    SecCertificateRef leafCert = SecPVCGetCertificateAtIndex(pvc, 0);
    CFArrayRef embeddedScts = SecCertificateCopySignedCertificateTimestamps(leafCert);
    CFArrayRef builderScts = SecPathBuilderCopySignedCertificateTimestamps(pvc->builder);
    CFArrayRef trustedLogs = SecPathBuilderCopyTrustedLogs(pvc->builder);
    CFArrayRef ocspScts = copy_ocsp_scts(pvc);
    CFDataRef precertEntry = copy_precert_entry_from_chain(pvc);
    CFDataRef x509Entry = copy_x509_entry_from_chain(pvc);

    // This eventually contain list of logs who validated the SCT.
    CFMutableDictionaryRef currentLogsValidatingScts = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef logsValidatingEmbeddedScts = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    uint64_t vt = TimestampFromCFAbsoluteTime(SecPVCGetVerifyTime(pvc));

    __block bool at_least_one_currently_valid_external = 0;
    __block bool at_least_one_currently_valid_embedded = 0;

    require(logsValidatingEmbeddedScts, out);
    require(currentLogsValidatingScts, out);

    if(trustedLogs) { // Don't bother trying to validate SCTs if we don't have any trusted logs.
        if(embeddedScts && precertEntry) { // Don't bother if we could not get the precert.
            CFArrayForEach(embeddedScts, ^(const void *value){
                CFAbsoluteTime sct_at;
                CFDictionaryRef log = getSCTValidatingLog(value, 1, precertEntry, vt, trustedLogs, &sct_at);
                if(log) {
                    addValidatingLog(logsValidatingEmbeddedScts, log, sct_at);
                    if(!CFDictionaryContainsKey(log, CFSTR("expiry"))) {
                        addValidatingLog(currentLogsValidatingScts, log, sct_at);
                        at_least_one_currently_valid_embedded = true;
                    }
                }
            });
        }

        if(builderScts && x509Entry) { // Don't bother if we could not get the cert.
            CFArrayForEach(builderScts, ^(const void *value){
                CFAbsoluteTime sct_at;
                CFDictionaryRef log = getSCTValidatingLog(value, 0, x509Entry, vt, trustedLogs, &sct_at);
                if(log) {
                    addValidatingLog(currentLogsValidatingScts, log, sct_at);
                    at_least_one_currently_valid_external = true;
                }
            });
        }

        if(ocspScts && x509Entry) {
            CFArrayForEach(ocspScts, ^(const void *value){
                CFAbsoluteTime sct_at;
                CFDictionaryRef log = getSCTValidatingLog(value, 0, x509Entry, vt, trustedLogs, &sct_at);
                if(log) {
                    addValidatingLog(currentLogsValidatingScts, log, sct_at);
                    at_least_one_currently_valid_external = true;
                }
            });
        }
    }


    /* We now have 2 sets of logs that validated those SCTS, count them and make a final decision.

     Current Policy:
     is_ct = (A1 AND A2) OR (B1 AND B2).

     A1: embedded SCTs from 2+ to 5+ logs valid at issuance time
     A2: At least one embedded SCT from a currently valid log.

     B1: SCTs from 2 currently valid logs (from any source)
     B2: At least 1 external SCT from a currently valid log.

     */

    SecCertificatePathVCSetIsCT(SecPathBuilderGetPath(pvc->builder), false);

    if(at_least_one_currently_valid_external && CFDictionaryGetCount(currentLogsValidatingScts)>=2) {
        SecCertificatePathVCSetIsCT(SecPathBuilderGetPath(pvc->builder), true);
    } else if(at_least_one_currently_valid_embedded) {
        __block CFAbsoluteTime issuanceTime = SecPVCGetVerifyTime(pvc);
        __block int lifetime; // in Months
        __block unsigned once_or_current_qualified_embedded = 0;

        /* Calculate issuance time base on timestamp of SCTs from current logs */
        CFDictionaryForEach(currentLogsValidatingScts, ^(const void *key, const void *value) {
            CFDictionaryRef log = key;
            if(!CFDictionaryContainsKey(log, CFSTR("expiry"))) {
                // Log is still qualified
                CFDateRef ts = (CFDateRef) value;
                CFAbsoluteTime timestamp = CFDateGetAbsoluteTime(ts);
                if(timestamp < issuanceTime) {
                    issuanceTime = timestamp;
                }
            }
        });

        /* Count Logs */
        CFDictionaryForEach(logsValidatingEmbeddedScts, ^(const void *key, const void *value) {
            CFDictionaryRef log = key;
            CFDateRef ts = value;
            CFDateRef expiry = CFDictionaryGetValue(log, CFSTR("expiry"));
            if(expiry == NULL || CFDateCompare(ts, expiry, NULL) == kCFCompareLessThan) {
                once_or_current_qualified_embedded++;
            }
        });

        SecCFCalendarDoWithZuluCalendar(^(CFCalendarRef zuluCalendar) {
            int _lifetime;
            CFCalendarGetComponentDifference(zuluCalendar,
                                             SecCertificateNotValidBefore(leafCert),
                                             SecCertificateNotValidAfter(leafCert),
                                             0, "M", &_lifetime);
            lifetime = _lifetime;
        });

        unsigned requiredEmbeddedSctsCount;

        if (lifetime < 15) {
            requiredEmbeddedSctsCount = 2;
        } else if (lifetime <= 27) {
            requiredEmbeddedSctsCount = 3;
        } else if (lifetime <= 39) {
            requiredEmbeddedSctsCount = 4;
        } else {
            requiredEmbeddedSctsCount = 5;
        }

        if(once_or_current_qualified_embedded >= requiredEmbeddedSctsCount){
            SecCertificatePathVCSetIsCT(SecPathBuilderGetPath(pvc->builder), true);
        }
    }

out:
    CFReleaseSafe(logsValidatingEmbeddedScts);
    CFReleaseSafe(currentLogsValidatingScts);
    CFReleaseSafe(builderScts);
    CFReleaseSafe(embeddedScts);
    CFReleaseSafe(ocspScts);
    CFReleaseSafe(precertEntry);
    CFReleaseSafe(trustedLogs);
    CFReleaseSafe(x509Entry);
}

static bool checkPolicyOidData(SecPVCRef pvc, CFDataRef oid) {
	CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    DERItem	key_value;
    key_value.data = (DERByte *)CFDataGetBytePtr(oid);
    key_value.length = (DERSize)CFDataGetLength(oid);

    for (ix = 0; ix < count; ix++) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        policy_set_t policies = policies_for_cert(cert);

        if (policy_set_contains(policies, &key_value)) {
            policy_set_free(policies);
            return true;
        }
        policy_set_free(policies);
    }
    return false;
}

static void SecPolicyCheckCertificatePolicyOid(SecPVCRef pvc, CFStringRef key)
{
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef value = CFDictionaryGetValue(policy->_options, key);
    bool result = false;

	if (CFGetTypeID(value) == CFDataGetTypeID())
	{
        result = checkPolicyOidData(pvc, value);
    } else if (CFGetTypeID(value) == CFStringGetTypeID()) {
        CFDataRef dataOid = SecCertificateCreateOidDataFromString(NULL, value);
        if (dataOid) {
            result = checkPolicyOidData(pvc, dataOid);
            CFRelease(dataOid);
        }
    }
    if(!result) {
		SecPVCSetResult(pvc, key, 0, kCFBooleanFalse);
    }
}


static void SecPolicyCheckRevocation(SecPVCRef pvc,
	CFStringRef key) {
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef value = CFDictionaryGetValue(policy->_options, key);
    if (isString(value)) {
        SecPathBuilderSetRevocationMethod(pvc->builder, value);
    }
}

static void SecPolicyCheckRevocationResponseRequired(SecPVCRef pvc,
	CFStringRef key) {
    pvc->require_revocation_response = true;
    secdebug("policy", "revocation response required");
}

static void SecPolicyCheckRevocationOnline(SecPVCRef pvc, CFStringRef key) {
    SecPathBuilderSetCheckRevocationOnline(pvc->builder);
}

static void SecPolicyCheckNoNetworkAccess(SecPVCRef pvc,
    CFStringRef key) {
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFTypeRef value = CFDictionaryGetValue(policy->_options, key);
    if (value == kCFBooleanTrue) {
        SecPathBuilderSetCanAccessNetwork(pvc->builder, false);
    } else {
        SecPathBuilderSetCanAccessNetwork(pvc->builder, true);
    }
}

static void SecPolicyCheckWeakIntermediates(SecPVCRef pvc,
    CFStringRef key) {
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    for (ix = 1; ix < count - 1; ++ix) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (cert && SecCertificateIsWeakKey(cert)) {
            /* Intermediate certificate has a weak key. */
            if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
                return;
            pvc->result = kSecTrustResultFatalTrustFailure;
        }
    }
}

static void SecPolicyCheckWeakLeaf(SecPVCRef pvc,
    CFStringRef key) {
    SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, 0);
    if (cert && SecCertificateIsWeakKey(cert)) {
        /* Leaf certificate has a weak key. */
        if (!SecPVCSetResult(pvc, key, 0, kCFBooleanFalse))
            return;
        pvc->result = kSecTrustResultFatalTrustFailure;
    }
}

static void SecPolicyCheckWeakRoot(SecPVCRef pvc,
    CFStringRef key) {
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    ix = count - 1;
    SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
    if (cert && SecCertificateIsWeakKey(cert)) {
        /* Root certificate has a weak key. */
        if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
            return;
        pvc->result = kSecTrustResultFatalTrustFailure;
    }
}

static void SecPolicyCheckKeySize(SecPVCRef pvc, CFStringRef key) {
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFDictionaryRef keySizes = CFDictionaryGetValue(policy->_options, key);
    for (ix = 0; ix < count; ++ix) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (!SecCertificateIsAtLeastMinKeySize(cert, keySizes)) {
            if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
                return;
        }
    }
}

static void SecPolicyCheckSignatureHashAlgorithms(SecPVCRef pvc,
    CFStringRef key) {
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);
    SecPolicyRef policy = SecPVCGetPolicy(pvc);
    CFSetRef disallowedHashAlgorithms = CFDictionaryGetValue(policy->_options, key);
    for (ix = 0; ix < count; ++ix) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (!SecPolicyCheckCertSignatureHashAlgorithms(cert, disallowedHashAlgorithms)) {
            if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse))
                return;
        }
    }
}

static bool leaf_is_on_weak_hash_whitelist(SecPVCRef pvc) {
    SecCertificateRef leaf = SecPVCGetCertificateAtIndex(pvc, 0);
    require_quiet(leaf, out);

    /* And now a special snowflake from our tests */

    /* subject:/C=AU/ST=NSW/L=St Leonards/O=VODAFONE HUTCHISON AUSTRALIA PTY LIMITED/OU=Technology Shared Services/CN=mybill.vodafone.com.au */
    /* issuer :/C=UK/O=Vodafone Group/CN=Vodafone (Corporate Services 2009) */
    /* Not After : May 26 09:37:50 2017 GMT */
    static const uint8_t vodafone[] = {
        0xde, 0x77, 0x63, 0x97, 0x79, 0x47, 0xee, 0x6e, 0xc1, 0x3a,
        0x7b, 0x3b, 0xad, 0x43, 0x88, 0xa9, 0x66, 0x59, 0xa8, 0x18
    };

    /* subject:/C=US/ST=Kansas/L=Overland Park/O=Sprint/CN=oma.ssprov.sprint.com */
    /* issuer :/C=US/O=Entrust, Inc./OU=www.entrust.net/rpa is incorporated by reference/OU=(c) 2009 Entrust, Inc./CN=Entrust Certification Authority - L1C */
    /* Not After : Aug 16 05:04:29 2017 GMT */
    static const uint8_t sprint[] = {
        0xa3, 0x18, 0x70, 0x4f, 0xf7, 0xbf, 0xfb, 0x2b, 0xe2, 0x64,
        0x3a, 0x2d, 0x2b, 0xb8, 0x10, 0x5f, 0x77, 0xd5, 0x01, 0xab
    };

    CFDataRef leafFingerprint = SecCertificateGetSHA1Digest(leaf);
    require_quiet(leafFingerprint, out);
    const unsigned int len = 20;
    const uint8_t *dp = CFDataGetBytePtr(leafFingerprint);
    if (dp && (!memcmp(vodafone, dp, len) || !memcmp(sprint,dp,len))) {
        return true;
    }

out:
    return false;
}

static bool SecPVCKeyIsConstraintPolicyOption(SecPVCRef pvc, CFStringRef key);

static void SecPolicyCheckSystemTrustedWeakHash(SecPVCRef pvc,
    CFStringRef key) {
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);

    Boolean keyInPolicy = false;
    CFArrayRef policies = pvc->policies;
    CFIndex policyIX, policyCount = CFArrayGetCount(policies);
    for (policyIX = 0; policyIX < policyCount; ++policyIX) {
		SecPolicyRef policy = (SecPolicyRef)CFArrayGetValueAtIndex(policies, policyIX);
        if (policy && CFDictionaryContainsKey(policy->_options, key)) {
            keyInPolicy = true;
        }
    }

    /* We only enforce this check when *both* of the following are true:
     *  1. One of the certs in the path has this usage constraint, and
     *  2. One of the policies in the PVC has this key
     * (As compared to normal policy options which require only one to be true..) */
    require_quiet(SecPVCKeyIsConstraintPolicyOption(pvc, key) &&
                  keyInPolicy, out);

    /* Ignore the anchor if it's trusted */
    if (SecPathBuilderIsAnchored(pvc->builder)) {
        count--;
    }
    for (ix = 0; ix < count; ++ix) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (SecCertificateIsWeakHash(cert)) {
            if (!leaf_is_on_weak_hash_whitelist(pvc)) {
                if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse)) {
                    return;
                }
            }
        }
    }
out:
    return;
}

static void SecPolicyCheckSystemTrustedWeakKey(SecPVCRef pvc,
                                                CFStringRef key) {
    CFIndex ix, count = SecPVCGetCertificateCount(pvc);

    Boolean keyInPolicy = false;
    CFArrayRef policies = pvc->policies;
    CFIndex policyIX, policyCount = CFArrayGetCount(policies);
    for (policyIX = 0; policyIX < policyCount; ++policyIX) {
        SecPolicyRef policy = (SecPolicyRef)CFArrayGetValueAtIndex(policies, policyIX);
        if (policy && CFDictionaryContainsKey(policy->_options, key)) {
            keyInPolicy = true;
        }
    }

    /* We only enforce this check when *both* of the following are true:
     *  1. One of the certs in the path has this usage constraint, and
     *  2. One of the policies in the PVC has this key
     * (As compared to normal policy options which require only one to be true..) */
    require_quiet(SecPVCKeyIsConstraintPolicyOption(pvc, key) &&
                  keyInPolicy, out);

    /* Ignore the anchor if it's trusted */
    if (SecPathBuilderIsAnchored(pvc->builder)) {
        count--;
    }
    for (ix = 0; ix < count; ++ix) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
        if (!SecCertificateIsStrongKey(cert)) {
            if (!SecPVCSetResult(pvc, key, ix, kCFBooleanFalse)) {
                return;
            }
        }

    } /* Cert loop */
out:
    return;
}

static void SecPolicyCheckPinningRequired(SecPVCRef pvc, CFStringRef key) {
    /* Pinning is disabled on the system, skip. */
    if (SecIsInternalRelease()) {
        if (CFPreferencesGetAppBooleanValue(CFSTR("AppleServerAuthenticationNoPinning"),
                                            CFSTR("com.apple.security"), NULL)) {
            return;
        }
    }

    CFArrayRef policies = pvc->policies;
    CFIndex policyIX, policyCount = CFArrayGetCount(policies);
    for (policyIX = 0; policyIX < policyCount; ++policyIX) {
        SecPolicyRef policy = (SecPolicyRef)CFArrayGetValueAtIndex(policies, policyIX);
        CFStringRef policyName = SecPolicyGetName(policy);
        if (CFEqualSafe(policyName, CFSTR("sslServer"))) {
            /* policy required pinning, but we didn't use a pinning policy */
            if (!SecPVCSetResult(pvc, key, 0, kCFBooleanFalse)) {
                return;
            }
        }
    }
}

void SecPolicyServerInitialize(void) {
	gSecPolicyLeafCallbacks = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks, NULL);
	gSecPolicyPathCallbacks = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks, NULL);

	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckBasicCertificateProcessing,
		SecPolicyCheckBasicCertificateProcessing);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckCriticalExtensions,
		SecPolicyCheckCriticalExtensions);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckIdLinkage,
		SecPolicyCheckIdLinkage);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckKeyUsage,
		SecPolicyCheckKeyUsage);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckExtendedKeyUsage,
		SecPolicyCheckExtendedKeyUsage);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckBasicConstraints,
		SecPolicyCheckBasicConstraints);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckNonEmptySubject,
		SecPolicyCheckNonEmptySubject);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckQualifiedCertStatements,
		SecPolicyCheckQualifiedCertStatements);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckSSLHostname,
		SecPolicyCheckSSLHostname);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckEmail,
		SecPolicyCheckEmail);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckValidIntermediates,
		SecPolicyCheckValidIntermediates);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckValidLeaf,
		SecPolicyCheckValidLeaf);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckValidRoot,
		SecPolicyCheckValidRoot);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckIssuerCommonName,
		SecPolicyCheckIssuerCommonName);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckSubjectCommonNamePrefix,
		SecPolicyCheckSubjectCommonNamePrefix);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckSubjectCommonName,
		SecPolicyCheckSubjectCommonName);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckNotValidBefore,
		SecPolicyCheckNotValidBefore);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckChainLength,
		SecPolicyCheckChainLength);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckAnchorSHA1,
		SecPolicyCheckAnchorSHA1);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckAnchorSHA256,
        SecPolicyCheckAnchorSHA256);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckAnchorApple,
		SecPolicyCheckAnchorApple);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckSubjectOrganization,
		SecPolicyCheckSubjectOrganization);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckSubjectOrganizationalUnit,
		SecPolicyCheckSubjectOrganizationalUnit);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckEAPTrustedServerNames,
		SecPolicyCheckEAPTrustedServerNames);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckSubjectCommonNameTEST,
		SecPolicyCheckSubjectCommonNameTEST);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckRevocation,
		SecPolicyCheckRevocation);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckRevocationResponseRequired,
		SecPolicyCheckRevocationResponseRequired);
    CFDictionaryAddValue(gSecPolicyLeafCallbacks,
        kSecPolicyCheckRevocationOnline,
        SecPolicyCheckRevocationOnline);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckNoNetworkAccess,
		SecPolicyCheckNoNetworkAccess);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckBlackListedLeaf,
		SecPolicyCheckBlackListedLeaf);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckGrayListedLeaf,
		SecPolicyCheckGrayListedLeaf);
	CFDictionaryAddValue(gSecPolicyLeafCallbacks,
		kSecPolicyCheckLeafMarkerOid,
		SecPolicyCheckLeafMarkerOid);
    CFDictionaryAddValue(gSecPolicyLeafCallbacks,
        kSecPolicyCheckLeafMarkerOidWithoutValueCheck,
        SecPolicyCheckLeafMarkerOidWithoutValueCheck);
    CFDictionaryAddValue(gSecPolicyLeafCallbacks,
        kSecPolicyCheckLeafMarkersProdAndQA,
        SecPolicyCheckLeafMarkersProdAndQA);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckIntermediateSPKISHA256,
		SecPolicyCheckIntermediateSPKISHA256);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckIntermediateEKU,
		SecPolicyCheckIntermediateEKU);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckIntermediateMarkerOid,
		SecPolicyCheckIntermediateMarkerOid);
	CFDictionaryAddValue(gSecPolicyPathCallbacks,
		kSecPolicyCheckCertificatePolicy,
		SecPolicyCheckCertificatePolicyOid);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckWeakIntermediates,
        SecPolicyCheckWeakIntermediates);
    CFDictionaryAddValue(gSecPolicyLeafCallbacks,
        kSecPolicyCheckWeakLeaf,
        SecPolicyCheckWeakLeaf);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckWeakRoot,
        SecPolicyCheckWeakRoot);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckKeySize,
        SecPolicyCheckKeySize);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckSignatureHashAlgorithms,
        SecPolicyCheckSignatureHashAlgorithms);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckSystemTrustedWeakHash,
        SecPolicyCheckSystemTrustedWeakHash);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckSystemTrustedWeakKey,
        SecPolicyCheckSystemTrustedWeakKey);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckIntermediateOrganization,
        SecPolicyCheckIntermediateOrganization);
    CFDictionaryAddValue(gSecPolicyPathCallbacks,
        kSecPolicyCheckIntermediateCountry,
        SecPolicyCheckIntermediateCountry);
    CFDictionaryAddValue(gSecPolicyLeafCallbacks,
        kSecPolicyCheckPinningRequired,
        SecPolicyCheckPinningRequired);
}

// MARK: -
// MARK: SecPVCRef
/********************************************************
 ****************** SecPVCRef Functions *****************
 ********************************************************/

void SecPVCInit(SecPVCRef pvc, SecPathBuilderRef builder, CFArrayRef policies) {
    secdebug("alloc", "%p", pvc);
    // Weird logging policies crashes.
    //secdebug("policy", "%@", policies);

    // Zero the pvc struct so only non-zero fields need to be explicitly set
    memset(pvc, 0, sizeof(struct OpaqueSecPVC));
    pvc->builder = builder;
    pvc->policies = policies;
    if (policies)
        CFRetain(policies);
    pvc->result = kSecTrustResultUnspecified;

    CFMutableDictionaryRef certDetail = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                                  &kCFTypeDictionaryKeyCallBacks,
                                                                  &kCFTypeDictionaryValueCallBacks);
    pvc->leafDetails = CFArrayCreate(kCFAllocatorDefault, (const void **)&certDetail,
                                         1, &kCFTypeArrayCallBacks);
    CFRelease(certDetail);
}

void SecPVCDelete(SecPVCRef pvc) {
    secdebug("alloc", "%p", pvc);
    CFReleaseNull(pvc->policies);
    CFReleaseNull(pvc->details);
    CFReleaseNull(pvc->leafDetails);
}

void SecPVCSetPath(SecPVCRef pvc, SecCertificatePathVCRef path) {
    secdebug("policy", "%@", path);
    pvc->policyIX = 0;
    pvc->result = kSecTrustResultUnspecified;
    CFReleaseNull(pvc->details);
}

void SecPVCComputeDetails(SecPVCRef pvc, SecCertificatePathVCRef path) {
    pvc->policyIX = 0;

    /* Since we don't run the LeafChecks again, we need to preserve the
     * result the leaf had. */
    CFIndex ix, pathLength = SecCertificatePathVCGetCount(path);
    CFMutableArrayRef details = CFArrayCreateMutableCopy(kCFAllocatorDefault,
                                                         pathLength, pvc->leafDetails);
    for (ix = 1; ix < pathLength; ++ix) {
        CFMutableDictionaryRef certDetail = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                                      &kCFTypeDictionaryKeyCallBacks,
                                                                      &kCFTypeDictionaryValueCallBacks);
        CFArrayAppendValue(details, certDetail);
        CFRelease(certDetail);
    }
    CFRetainAssign(pvc->details, details);
    pvc->result = pvc->leafResult;
    CFReleaseSafe(details);
}

SecPolicyRef SecPVCGetPolicy(SecPVCRef pvc) {
	return (SecPolicyRef)CFArrayGetValueAtIndex(pvc->policies, pvc->policyIX);
}

static CFIndex SecPVCGetCertificateCount(SecPVCRef pvc) {
	return SecPathBuilderGetCertificateCount(pvc->builder);
}

static SecCertificateRef SecPVCGetCertificateAtIndex(SecPVCRef pvc, CFIndex ix) {
	return SecPathBuilderGetCertificateAtIndex(pvc->builder, ix);
}

static CFAbsoluteTime SecPVCGetVerifyTime(SecPVCRef pvc) {
    return SecPathBuilderGetVerifyTime(pvc->builder);
}

static bool SecPVCIsExceptedError(SecPVCRef pvc, CFIndex ix, CFStringRef key, CFTypeRef value) {
    CFArrayRef exceptions = SecPathBuilderGetExceptions(pvc->builder);
    if (!exceptions) { return false; }
    CFIndex exceptionsCount = CFArrayGetCount(exceptions);

    /* There are two types of exceptions:
     *  1. Those that are built from SecTrustCopyExceptions, which are particular to the
     *  certs in the chain -- as indicated by the SHA1 digest in the exception dictionary.
     *  2. On macOS, those built from SecTrustSetOptions, which are generic excepted errors.
     */
#if TARGET_OS_OSX
    CFDictionaryRef options = CFArrayGetValueAtIndex(exceptions, 0);
    /* Type 2 */
    if (exceptionsCount == 1 && (ix > 0 || !CFDictionaryContainsKey(options, kSecCertificateDetailSHA1Digest))) {
        /* SHA1Digest not allowed */
        if (CFDictionaryContainsKey(options, kSecCertificateDetailSHA1Digest)) { return false; }
        /* Key excepted */
        if (CFDictionaryContainsKey(options, key)) {
            /* Special case -- AnchorTrusted only for self-signed certs */
            if (CFEqual(kSecPolicyCheckAnchorTrusted, key)) {
                Boolean isSelfSigned = false;
                SecCertificateRef cert = SecPathBuilderGetCertificateAtIndex(pvc->builder, ix);
                if (!cert || (errSecSuccess != SecCertificateIsSelfSigned(cert, &isSelfSigned)) || !isSelfSigned) {
                    return false;
                }
            }
            return true;
        }
    }
#endif

    /* Type 1 */
    if (ix >= exceptionsCount) { return false; }
    CFDictionaryRef exception = CFArrayGetValueAtIndex(exceptions, ix);

    /* Compare the cert hash */
    if (!CFDictionaryContainsKey(exception, kSecCertificateDetailSHA1Digest)) { return false; }
    SecCertificateRef cert = SecPathBuilderGetCertificateAtIndex(pvc->builder, ix);
    if (!CFEqual(SecCertificateGetSHA1Digest(cert), CFDictionaryGetValue(exception, kSecCertificateDetailSHA1Digest))) {
        return false;
    }

    /* Key Excepted */
    CFTypeRef exceptionValue = CFDictionaryGetValue(exception, key);
    if (exceptionValue && CFEqual(value, exceptionValue)) {
        /* Only change result if PVC is already ok */
        if (SecPVCIsOkResult(pvc)) {
            // Chains that pass due to exceptions get Proceed result.
            pvc->result = kSecTrustResultProceed;
        }
        return true;
    }

    return false;
}

static int32_t detailKeyToCssmErr(CFStringRef key) {
    int32_t result = 0;

    if (CFEqual(key, kSecPolicyCheckSSLHostname)) {
        result = -2147408896; // CSSMERR_APPLETP_HOSTNAME_MISMATCH
    }
    else if (CFEqual(key, kSecPolicyCheckEmail)) {
        result = -2147408872; // CSSMERR_APPLETP_SMIME_EMAIL_ADDRS_NOT_FOUND
    }
    else if (CFEqual(key, kSecPolicyCheckValidLeaf) ||
             CFEqual(key, kSecPolicyCheckValidIntermediates) ||
             CFEqual(key, kSecPolicyCheckValidRoot)) {
        result = -2147409654; // CSSMERR_TP_CERT_EXPIRED
    }

    return result;
}

static bool SecPVCMeetsConstraint(SecPVCRef pvc, SecCertificateRef certificate, CFDictionaryRef constraint);

static bool SecPVCIsAllowedError(SecPVCRef pvc, CFIndex ix, CFStringRef key) {
    bool result = false;
    SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
    CFArrayRef constraints = SecCertificatePathVCGetUsageConstraintsAtIndex(path, ix);
    SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
    CFIndex constraintIX, constraintCount = CFArrayGetCount(constraints);

    for (constraintIX = 0; constraintIX < constraintCount; constraintIX++) {
        CFDictionaryRef constraint = (CFDictionaryRef)CFArrayGetValueAtIndex(constraints, constraintIX);
        CFNumberRef allowedErrorNumber = NULL;
        if (!isDictionary(constraint)) {
            continue;
        }
        allowedErrorNumber = (CFNumberRef)CFDictionaryGetValue(constraint, kSecTrustSettingsAllowedError);
        int32_t allowedErrorValue = 0;
        if (!isNumber(allowedErrorNumber) || !CFNumberGetValue(allowedErrorNumber, kCFNumberSInt32Type, &allowedErrorValue)) {
            continue;
        }

        if (SecPVCMeetsConstraint(pvc, cert, constraint)) {
            if (allowedErrorValue == detailKeyToCssmErr(key)) {
                result = true;
                break;
            }
        }
    }
    return result;
}

static bool SecPVCKeyIsConstraintPolicyOption(SecPVCRef pvc, CFStringRef key) {
    CFIndex certIX, certCount = SecPVCGetCertificateCount(pvc);
    for (certIX = 0; certIX < certCount; certIX++) {
        SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
        CFArrayRef constraints = SecCertificatePathVCGetUsageConstraintsAtIndex(path, certIX);
        CFIndex constraintIX, constraintCount = CFArrayGetCount(constraints);
        for (constraintIX = 0; constraintIX < constraintCount; constraintIX++) {
            CFDictionaryRef constraint = (CFDictionaryRef)CFArrayGetValueAtIndex(constraints, constraintIX);
            if (!isDictionary(constraint)) {
                continue;
            }

            CFDictionaryRef policyOptions = NULL;
            policyOptions = (CFDictionaryRef)CFDictionaryGetValue(constraint, kSecTrustSettingsPolicyOptions);
            if (policyOptions && isDictionary(policyOptions) &&
                CFDictionaryContainsKey(policyOptions, key)) {
                return true;
            }
        }
    }
    return false;
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
bool SecPVCSetResultForced(SecPVCRef pvc,
	CFStringRef key, CFIndex ix, CFTypeRef result, bool force) {

    secnotice("policy", "cert[%d]: %@ =(%s)[%s]> %@", (int) ix, key,
        (pvc->callbacks == gSecPolicyLeafCallbacks ? "leaf"
            : (pvc->callbacks == gSecPolicyPathCallbacks ? "path"
                : "custom")),
        (force ? "force" : ""), result);

    /* If this is not something the current policy cares about ignore
       this error and return true so our caller continues evaluation. */
    if (!force) {
        /* Either the policy or the usage constraints have to have this key */
        SecPolicyRef policy = SecPVCGetPolicy(pvc);
        if (!(SecPVCKeyIsConstraintPolicyOption(pvc, key) ||
            (policy && CFDictionaryContainsKey(policy->_options, key)))) {
            return true;
        }
    }

	/* Check to see if the SecTrustSettings for the certificate in question
	   tell us to ignore this error. */
	if (SecPVCIsAllowedError(pvc, ix, key)) {
        secinfo("policy", "cert[%d]: skipped allowed error %@", (int) ix, key);
		return true;
	}

    /* Check to see if exceptions tells us to ignore this error. */
    if (SecPVCIsExceptedError(pvc, ix, key, result)) {
        secinfo("policy", "cert[%d]: skipped exception error %@", (int) ix, key);
        return true;
    }

	/* Check SecPVCIsOkResult to avoid resetting deny or fatal to recoverable */
	if (SecPVCIsOkResult(pvc)) {
		pvc->result = kSecTrustResultRecoverableTrustFailure;
	}
	if (!pvc->details)
		return false;

	CFMutableDictionaryRef detail =
		(CFMutableDictionaryRef)CFArrayGetValueAtIndex(pvc->details, ix);

	/* Perhaps detail should have an array of results per key?  As it stands
       in the case of multiple policy failures the last failure stands.  */
	CFDictionarySetValue(detail, key, result);

	return true;
}

bool SecPVCSetResult(SecPVCRef pvc,
	CFStringRef key, CFIndex ix, CFTypeRef result) {
    return SecPVCSetResultForced(pvc, key, ix, result, false);
}

/* AUDIT[securityd](done):
   key(ok) is a caller provided.
   value(ok, unused) is a caller provided.
 */
static void SecPVCValidateKey(const void *key, const void *value,
	void *context) {
	SecPVCRef pvc = (SecPVCRef)context;

	/* If our caller doesn't want full details and we failed earlier there is
	   no point in doing additional checks. */
	if (!SecPVCIsOkResult(pvc) && !pvc->details)
		return;

	SecPolicyCheckFunction fcn = (SecPolicyCheckFunction)
		CFDictionaryGetValue(pvc->callbacks, key);

	if (!fcn) {
        if (pvc->callbacks == gSecPolicyLeafCallbacks) {
            if (!CFDictionaryContainsKey(gSecPolicyPathCallbacks, key)) {
                pvc->result = kSecTrustResultOtherError;
            }
        } else if (pvc->callbacks == gSecPolicyPathCallbacks) {
            if (!CFDictionaryContainsKey(gSecPolicyLeafCallbacks, key)) {
                pvc->result = kSecTrustResultOtherError;
            }
        } else {
            /* Non standard validation phase. This may be a new key from the
             * pinning DB which is not implemented in this OS version. Log
             * a warning. */
            secwarning("policy: unknown policy key %@, skipping", key);
        }
		return;
	}

	fcn(pvc, (CFStringRef)key);
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
SecTrustResultType SecPVCLeafChecks(SecPVCRef pvc) {
    /* We need to compute details for the leaf. */
    CFRetainAssign(pvc->details, pvc->leafDetails);

    CFArrayRef policies = pvc->policies;
	CFIndex ix, count = CFArrayGetCount(policies);
	for (ix = 0; ix < count; ++ix) {
		SecPolicyRef policy = (SecPolicyRef)CFArrayGetValueAtIndex(policies, ix);
        pvc->policyIX = ix;
        /* Validate all keys for all policies. */
        pvc->callbacks = gSecPolicyLeafCallbacks;
        CFDictionaryApplyFunction(policy->_options, SecPVCValidateKey, pvc);
	}

    pvc->leafResult = pvc->result;
    CFRetainAssign(pvc->leafDetails, pvc->details);

    return pvc->result;
}

bool SecPVCIsOkResult(SecPVCRef pvc) {
    if (pvc->result == kSecTrustResultRecoverableTrustFailure ||
        pvc->result == kSecTrustResultDeny ||
        pvc->result == kSecTrustResultFatalTrustFailure ||
        pvc->result == kSecTrustResultOtherError) {
        return false;
    }
    return true;
}

bool SecPVCParentCertificateChecks(SecPVCRef pvc, CFIndex ix) {
    /* Check stuff common to intermediate and anchors. */
    CFAbsoluteTime verifyTime = SecPVCGetVerifyTime(pvc);
    SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
    CFIndex anchor_ix = SecPVCGetCertificateCount(pvc) - 1;
    bool is_anchor = (ix == anchor_ix && SecPathBuilderIsAnchored(pvc->builder));

    if (!SecCertificateIsValid(cert, verifyTime)) {
        /* Certificate has expired. */
        if (!SecPVCSetResult(pvc, is_anchor ? kSecPolicyCheckValidRoot
            : kSecPolicyCheckValidIntermediates, ix, kCFBooleanFalse)) {
            goto errOut;
        }
    }

    if (SecCertificateIsWeakKey(cert)) {
        /* Certificate uses weak key. */
        if (!SecPVCSetResult(pvc, is_anchor ? kSecPolicyCheckWeakRoot
            : kSecPolicyCheckWeakIntermediates, ix, kCFBooleanFalse)) {
            goto errOut;
        }
    }

    if (is_anchor) {
        /* Perform anchor specific checks. */
        /* Don't think we have any of these. */
    } else {
        /* Perform intermediate specific checks. */

        /* (k) Basic constraints only relevant for v3 and later. */
        if (SecCertificateVersion(cert) >= 3) {
            const SecCEBasicConstraints *bc =
                SecCertificateGetBasicConstraints(cert);
            if (!bc || !bc->isCA) {
                /* Basic constraints not present or not marked as isCA, illegal. */
                if (!SecPVCSetResultForced(pvc, kSecPolicyCheckBasicConstraints,
                            ix, kCFBooleanFalse, true)) {
                    goto errOut;
                }
            }
        }
        /* For a v1 or v2 certificate in an intermediate slot (not a leaf and
           not an anchor), we additionally require that the certificate chain
           does not end in a v3 or later anchor. [rdar://32204517] */
        else if (ix > 0 && ix < anchor_ix) {
            SecCertificateRef anchor = SecPVCGetCertificateAtIndex(pvc, anchor_ix);
            if (SecCertificateVersion(anchor) >= 3) {
                if (!SecPVCSetResultForced(pvc, kSecPolicyCheckBasicConstraints,
                            ix, kCFBooleanFalse, true)) {
                    goto errOut;
                }
            }
        }
        /* (l) max_path_length is checked elsewhere. */

        /* (n) If a key usage extension is present, verify that the keyCertSign bit is set. */
        SecKeyUsage keyUsage = SecCertificateGetKeyUsage(cert);
        if (keyUsage && !(keyUsage & kSecKeyUsageKeyCertSign)) {
            if (!SecPVCSetResultForced(pvc, kSecPolicyCheckKeyUsage,
                ix, kCFBooleanFalse, true)) {
                goto errOut;
            }
        }
    }

errOut:
    return SecPVCIsOkResult(pvc);
}

static bool SecPVCBlackListedKeyChecks(SecPVCRef pvc, CFIndex ix) {
    /* Check stuff common to intermediate and anchors. */

	SecOTAPKIRef otapkiRef = SecOTAPKICopyCurrentOTAPKIRef();
	if (NULL != otapkiRef)
	{
		CFSetRef blackListedKeys = SecOTAPKICopyBlackListSet(otapkiRef);
		CFRelease(otapkiRef);
		if (NULL != blackListedKeys)
		{
			SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
			CFIndex count = SecPVCGetCertificateCount(pvc);
			bool is_last = (ix == count - 1);
			bool is_anchor = (is_last && SecPathBuilderIsAnchored(pvc->builder));
			if (!is_anchor) {
				/* Check for blacklisted intermediate issuer keys. */
				CFDataRef dgst = SecCertificateCopyPublicKeySHA1Digest(cert);
				if (dgst) {
					/* Check dgst against blacklist. */
					if (CFSetContainsValue(blackListedKeys, dgst)) {
						/* Check allow list for this blacklisted issuer key,
						   which is the authority key of the issued cert at ix-1.
						*/
						SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
						bool allowed = path && SecCertificatePathVCIsAllowlisted(path);
						if (!allowed) {
							SecPVCSetResultForced(pvc, kSecPolicyCheckBlackListedKey,
							                      ix, kCFBooleanFalse, true);
                            pvc->result = kSecTrustResultFatalTrustFailure;
						}
					}
					CFRelease(dgst);
				}
			}
			CFRelease(blackListedKeys);
			return SecPVCIsOkResult(pvc);
		}
	}
	// Assume OK
	return true;
}

static bool SecPVCGrayListedKeyChecks(SecPVCRef pvc, CFIndex ix)
{
	/* Check stuff common to intermediate and anchors. */
	SecOTAPKIRef otapkiRef = SecOTAPKICopyCurrentOTAPKIRef();
	if (NULL != otapkiRef)
	{
		CFSetRef grayListKeys = SecOTAPKICopyGrayList(otapkiRef);
		CFRelease(otapkiRef);
		if (NULL != grayListKeys)
		{
			SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, ix);
			CFIndex count = SecPVCGetCertificateCount(pvc);
			bool is_last = (ix == count - 1);
			bool is_anchor = (is_last && SecPathBuilderIsAnchored(pvc->builder));
			if (!is_anchor) {
				/* Check for gray listed intermediate issuer keys. */
				CFDataRef dgst = SecCertificateCopyPublicKeySHA1Digest(cert);
				if (dgst) {
					/* Check dgst against gray list. */
					if (CFSetContainsValue(grayListKeys, dgst)) {
						/* Check allow list for this graylisted issuer key,
						   which is the authority key of the issued cert at ix-1.
						*/
						SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
						bool allowed = path && SecCertificatePathVCIsAllowlisted(path);
						if (!allowed) {
							SecPVCSetResultForced(pvc, kSecPolicyCheckGrayListedKey,
							                      ix, kCFBooleanFalse, true);
						}
					}
					CFRelease(dgst);
				}
			}
			CFRelease(grayListKeys);
			return SecPVCIsOkResult(pvc);
		}
	}
	// Assume ok
	return true;
}

static bool SecPVCContainsPolicy(SecPVCRef pvc, CFStringRef searchOid, CFStringRef searchName, CFIndex *policyIX) {
    if (!isString(searchName) && !isString(searchOid)) {
        return false;
    }
	CFArrayRef policies = pvc->policies;
	CFIndex ix, count = CFArrayGetCount(policies);
	for (ix = 0; ix < count; ++ix) {
		SecPolicyRef policy = (SecPolicyRef)CFArrayGetValueAtIndex(policies, ix);
		CFStringRef policyName = SecPolicyGetName(policy);
        CFStringRef policyOid = SecPolicyGetOidString(policy);
        /* Prefer a match of both name and OID */
        if (searchOid && searchName && policyOid && policyName) {
            if (CFEqual(searchOid, policyOid) &&
                CFEqual(searchName, policyName)) {
                if (policyIX) { *policyIX = ix; }
                return true;
            }
        }
        /* Next best is just OID. */
        if (!searchName && searchOid && policyOid) {
            if (CFEqual(searchOid, policyOid)) {
                if (policyIX) { *policyIX = ix; }
                return true;
            }
        }
        if (!searchOid && searchName && policyName) {
            if (CFEqual(searchName, policyName)) {
                if (policyIX) { *policyIX = ix; }
                return true;
            }
        }
	}
	return false;
}

static bool SecPVCContainsString(SecPVCRef pvc, CFIndex policyIX, CFStringRef stringValue) {
    if (!isString(stringValue)) {
        return false;
    }
    bool result = false;

    CFStringRef tmpStringValue = NULL;
    if (CFStringGetCharacterAtIndex(stringValue, CFStringGetLength(stringValue) -1) == (UniChar)0x0000) {
        tmpStringValue = CFStringCreateTruncatedCopy(stringValue, CFStringGetLength(stringValue) - 1);
    } else {
        tmpStringValue = CFStringCreateCopy(NULL, stringValue);
    }
    if (policyIX >= 0 && policyIX < CFArrayGetCount(pvc->policies)) {
        SecPolicyRef policy = (SecPolicyRef)CFArrayGetValueAtIndex(pvc->policies, policyIX);
        /* Have to look for all the possible locations of name string */
        CFStringRef policyString = NULL;
        policyString = CFDictionaryGetValue(policy->_options, kSecPolicyCheckSSLHostname);
        if (!policyString) {
            policyString = CFDictionaryGetValue(policy->_options, kSecPolicyCheckEmail);
        }
        if (policyString && (CFStringCompare(tmpStringValue, policyString, kCFCompareCaseInsensitive) == kCFCompareEqualTo)) {
            result = true;
            goto out;
        }

        CFArrayRef policyStrings = NULL;
        policyStrings = CFDictionaryGetValue(policy->_options, kSecPolicyCheckEAPTrustedServerNames);
        if (policyStrings && CFArrayContainsValue(policyStrings,
                                                  CFRangeMake(0, CFArrayGetCount(policyStrings)),
                                                  tmpStringValue)) {
            result = true;
            goto out;
        }
    }

out:
    CFReleaseNull(tmpStringValue);
    return result;
}


static uint32_t ts_key_usage_for_kuNumber(CFNumberRef keyUsageNumber) {
    uint32_t ourTSKeyUsage = 0;
    uint32_t keyUsage = 0;
    if (keyUsageNumber &&
        CFNumberGetValue(keyUsageNumber, kCFNumberSInt32Type, &keyUsage)) {
        if (keyUsage & kSecKeyUsageDigitalSignature) {
            ourTSKeyUsage |= kSecTrustSettingsKeyUseSignature;
        }
        if (keyUsage & kSecKeyUsageDataEncipherment) {
            ourTSKeyUsage |= kSecTrustSettingsKeyUseEnDecryptData;
        }
        if (keyUsage & kSecKeyUsageKeyEncipherment) {
            ourTSKeyUsage |= kSecTrustSettingsKeyUseEnDecryptKey;
        }
        if (keyUsage & kSecKeyUsageKeyAgreement) {
            ourTSKeyUsage |= kSecTrustSettingsKeyUseKeyExchange;
        }
        if (keyUsage == kSecKeyUsageAll) {
            ourTSKeyUsage = kSecTrustSettingsKeyUseAny;
        }
    }
    return ourTSKeyUsage;
}

static uint32_t ts_key_usage_for_policy(SecPolicyRef policy) {
    uint32_t ourTSKeyUsage = 0;
    CFTypeRef policyKeyUsageType = NULL;

    policyKeyUsageType = (CFTypeRef)CFDictionaryGetValue(policy->_options, kSecPolicyCheckKeyUsage);
    if (isArray(policyKeyUsageType)) {
        CFIndex ix, count = CFArrayGetCount(policyKeyUsageType);
        for (ix = 0; ix < count; ix++) {
            CFNumberRef policyKeyUsageNumber = NULL;
            policyKeyUsageNumber = (CFNumberRef)CFArrayGetValueAtIndex(policyKeyUsageType, ix);
            ourTSKeyUsage |= ts_key_usage_for_kuNumber(policyKeyUsageNumber);
        }
    } else if (isNumber(policyKeyUsageType)) {
        ourTSKeyUsage |= ts_key_usage_for_kuNumber(policyKeyUsageType);
    }

    return ourTSKeyUsage;
}

static bool SecPVCContainsTrustSettingsKeyUsage(SecPVCRef pvc,
    SecCertificateRef certificate, CFIndex policyIX, CFNumberRef keyUsageNumber) {
    int64_t keyUsageValue = 0;
    uint32_t ourKeyUsage = 0;

    if (!isNumber(keyUsageNumber) || !CFNumberGetValue(keyUsageNumber, kCFNumberSInt64Type, &keyUsageValue)) {
        return false;
    }

    if (keyUsageValue == kSecTrustSettingsKeyUseAny) {
        return true;
    }

    /* We're using the key for revocation if we have the OCSPSigner policy.
     * @@@ If we support CRLs, we'd need to check for that policy here too.
     */
    if (SecPVCContainsPolicy(pvc, kSecPolicyAppleOCSPSigner, NULL, NULL)) {
        ourKeyUsage |= kSecTrustSettingsKeyUseSignRevocation;
    }

    /* We're using the key for verifying a cert if it's a root/intermediate
     * in the chain. If the cert isn't in the path yet, we're about to add it,
     * so it's a root/intermediate. If there is no path, this is the leaf.
     */
    CFIndex pathIndex = -1;
    SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
    if (path) {
        pathIndex = SecCertificatePathVCGetIndexOfCertificate(path, certificate);
    } else {
        pathIndex = 0;
    }
    if (pathIndex != 0) {
        ourKeyUsage |= kSecTrustSettingsKeyUseSignCert;
    }

    /* The rest of the key usages may be specified by the policy(ies). */
    if (policyIX >= 0 && policyIX < CFArrayGetCount(pvc->policies)) {
        SecPolicyRef policy = (SecPolicyRef)CFArrayGetValueAtIndex(pvc->policies, policyIX);
        ourKeyUsage |= ts_key_usage_for_policy(policy);
    } else {
        /* Get key usage from ALL policies */
        CFIndex ix, count = CFArrayGetCount(pvc->policies);
        for (ix = 0; ix < count; ix++) {
            SecPolicyRef policy = (SecPolicyRef)CFArrayGetValueAtIndex(pvc->policies, ix);
            ourKeyUsage |= ts_key_usage_for_policy(policy);
        }
    }

    if (ourKeyUsage == (uint32_t)(keyUsageValue & 0x00ffffffff)) {
        return true;
    }

    return false;
}

#if TARGET_OS_OSX
#include <Security/SecTrustedApplicationPriv.h>
#include <Security/SecTask.h>
#include <Security/SecTaskPriv.h>
#include <bsm/libbsm.h>
#include <libproc.h>

extern OSStatus SecTaskValidateForRequirement(SecTaskRef task, CFStringRef requirement);

static bool SecPVCCallerIsApplication(CFDataRef clientAuditToken, CFTypeRef appRef) {
    bool result = false;
    audit_token_t auditToken = {};
    SecTaskRef task = NULL;
    SecRequirementRef requirement = NULL;
    CFStringRef stringRequirement = NULL;

    require(appRef && clientAuditToken, out);
    require(CFGetTypeID(appRef) == SecTrustedApplicationGetTypeID(), out);
    require_noerr(SecTrustedApplicationCopyRequirement((SecTrustedApplicationRef)appRef, &requirement), out);
    require(requirement, out);
    require_noerr(SecRequirementsCopyString(requirement, kSecCSDefaultFlags, &stringRequirement), out);
    require(stringRequirement, out);

    require(sizeof(auditToken) == CFDataGetLength(clientAuditToken), out);
    CFDataGetBytes(clientAuditToken, CFRangeMake(0, sizeof(auditToken)), (uint8_t *)&auditToken);
    require(task = SecTaskCreateWithAuditToken(NULL, auditToken), out);

    if(errSecSuccess == SecTaskValidateForRequirement(task, stringRequirement)) {
        result = true;
    }

out:
    CFReleaseNull(task);
    CFReleaseNull(requirement);
    CFReleaseNull(stringRequirement);
    return result;
}
#endif

static bool SecPVCContainsTrustSettingsPolicyOption(SecPVCRef pvc, CFDictionaryRef options) {
    if (!isDictionary(options)) {
        return false;
    }

    /* Push */
    CFDictionaryRef currentCallbacks = pvc->callbacks;

    /* We need to run the leaf and path checks using these options. */
    pvc->callbacks = gSecPolicyLeafCallbacks;
    CFDictionaryApplyFunction(options, SecPVCValidateKey, pvc);

    pvc->callbacks = gSecPolicyPathCallbacks;
    CFDictionaryApplyFunction(options, SecPVCValidateKey, pvc);

    /* Pop */
    pvc->callbacks = currentCallbacks;

    /* Our work here is done; no need to claim a match */
    return false;
}

static bool SecPVCMeetsConstraint(SecPVCRef pvc, SecCertificateRef certificate, CFDictionaryRef constraint) {
    CFStringRef policyOid = NULL, policyString = NULL, policyName = NULL;
    CFNumberRef keyUsageNumber = NULL;
    CFTypeRef trustedApplicationData = NULL;
    CFDictionaryRef policyOptions = NULL;

    bool policyMatch = false, policyStringMatch = false, applicationMatch = false ,
         keyUsageMatch = false, policyOptionMatch = false;
    bool result = false;

#if TARGET_OS_MAC && !TARGET_OS_IPHONE
    /* OS X returns a SecPolicyRef in the constraints. Convert to the oid string. */
    SecPolicyRef policy = NULL;
    policy = (SecPolicyRef)CFDictionaryGetValue(constraint, kSecTrustSettingsPolicy);
    policyOid = (policy) ? policy->_oid : NULL;
#else
    policyOid = (CFStringRef)CFDictionaryGetValue(constraint, kSecTrustSettingsPolicy);
#endif
    policyName = (CFStringRef)CFDictionaryGetValue(constraint, kSecTrustSettingsPolicyName);
    policyString = (CFStringRef)CFDictionaryGetValue(constraint, kSecTrustSettingsPolicyString);
    keyUsageNumber = (CFNumberRef)CFDictionaryGetValue(constraint, kSecTrustSettingsKeyUsage);
    policyOptions = (CFDictionaryRef)CFDictionaryGetValue(constraint, kSecTrustSettingsPolicyOptions);

    CFIndex policyIX = -1;
    policyMatch = SecPVCContainsPolicy(pvc, policyOid, policyName, &policyIX);
    policyStringMatch = SecPVCContainsString(pvc, policyIX, policyString);
    keyUsageMatch = SecPVCContainsTrustSettingsKeyUsage(pvc, certificate, policyIX, keyUsageNumber);
    policyOptionMatch = SecPVCContainsTrustSettingsPolicyOption(pvc, policyOptions);

#if TARGET_OS_MAC && !TARGET_OS_IPHONE
    trustedApplicationData =  CFDictionaryGetValue(constraint, kSecTrustSettingsApplication);
    CFDataRef clientAuditToken = SecPathBuilderCopyClientAuditToken(pvc->builder);
    applicationMatch = SecPVCCallerIsApplication(clientAuditToken, trustedApplicationData);
    CFReleaseNull(clientAuditToken);
#else
    if(CFDictionaryContainsKey(constraint, kSecTrustSettingsApplication)) {
        secerror("kSecTrustSettingsApplication is not yet supported on this platform");
    }
#endif

    /* If we either didn't find the parameter in the dictionary or we got a match
     * against that parameter, for all possible parameters in the dictionary, then
     * this trust setting result applies to the output. */
    if (((!policyOid && !policyName) || policyMatch) &&
        (!policyString || policyStringMatch) &&
        (!trustedApplicationData || applicationMatch) &&
        (!keyUsageNumber || keyUsageMatch) &&
        (!policyOptions || policyOptionMatch)) {
        result = true;
    }

    return result;
}

SecTrustSettingsResult SecPVCGetTrustSettingsResult(SecPVCRef pvc, SecCertificateRef certificate, CFArrayRef constraints) {
    SecTrustSettingsResult result = kSecTrustSettingsResultInvalid;
    CFIndex constraintIX, constraintCount = CFArrayGetCount(constraints);
    for (constraintIX = 0; constraintIX < constraintCount; constraintIX++) {
        CFDictionaryRef constraint = (CFDictionaryRef)CFArrayGetValueAtIndex(constraints, constraintIX);
        if (!isDictionary(constraint)) {
            continue;
        }

        CFNumberRef resultNumber = NULL;
        resultNumber = (CFNumberRef)CFDictionaryGetValue(constraint, kSecTrustSettingsResult);
        uint32_t resultValue = kSecTrustSettingsResultInvalid;
        if (!isNumber(resultNumber) || !CFNumberGetValue(resultNumber, kCFNumberSInt32Type, &resultValue)) {
            /* no SecTrustSettingsResult entry defaults to TrustRoot*/
            resultValue = kSecTrustSettingsResultTrustRoot;
        }

        if (SecPVCMeetsConstraint(pvc, certificate, constraint)) {
            result = resultValue;
            break;
        }
    }
    return result;
}

static void SecPVCCheckUsageConstraints(SecPVCRef pvc) {
    CFIndex certIX, certCount = SecPVCGetCertificateCount(pvc);
    for (certIX = 0; certIX < certCount; certIX++) {
        SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
        CFArrayRef constraints = SecCertificatePathVCGetUsageConstraintsAtIndex(path, certIX);
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, certIX);
        SecTrustSettingsResult result = SecPVCGetTrustSettingsResult(pvc, cert, constraints);

        /* Set the pvc trust result based on the usage constraints and anchor source. */
        if (result == kSecTrustSettingsResultDeny) {
            SecPVCSetResultForced(pvc, kSecPolicyCheckUsageConstraints, certIX, kCFBooleanFalse, true);
            pvc->result = kSecTrustResultDeny;
        } else if ((result == kSecTrustSettingsResultTrustRoot || result == kSecTrustSettingsResultTrustAsRoot ||
                    result == kSecTrustSettingsResultInvalid) && SecPVCIsOkResult(pvc)) {
            /* If we already think the PVC is ok and this cert is from one of the user/
             * admin anchor sources, trustRoot, trustAsRoot, and Invalid (no constraints),
             * all mean we should use the special "Proceed" trust result. */
#if TARGET_OS_IPHONE
            if (SecPathBuilderIsAnchorSource(pvc->builder, kSecUserAnchorSource) &&
                SecCertificateSourceContains(kSecUserAnchorSource, cert)) {
#else
            if (SecPathBuilderIsAnchorSource(pvc->builder, kSecLegacyAnchorSource) &&
                SecCertificateSourceContains(kSecLegacyAnchorSource, cert)) {
#endif
                pvc->result = kSecTrustResultProceed;
            }
        }
    }
}

#define kSecPolicySHA256Size 32
static const UInt8 kTestDateConstraintsRoot[kSecPolicySHA256Size] = {
    0x51,0xA0,0xF3,0x1F,0xC0,0x1D,0xEC,0x87,0x32,0xB6,0xFD,0x13,0x6A,0x43,0x4D,0x6C,
    0x87,0xCD,0x62,0xE0,0x38,0xB4,0xFB,0xD6,0x40,0xB0,0xFD,0x62,0x4D,0x1F,0xCF,0x6D
};
static const UInt8 kWS_CA1_G2[kSecPolicySHA256Size] = {
    0xD4,0x87,0xA5,0x6F,0x83,0xB0,0x74,0x82,0xE8,0x5E,0x96,0x33,0x94,0xC1,0xEC,0xC2,
    0xC9,0xE5,0x1D,0x09,0x03,0xEE,0x94,0x6B,0x02,0xC3,0x01,0x58,0x1E,0xD9,0x9E,0x16
};
static const UInt8 kWS_CA1_NEW[kSecPolicySHA256Size] = {
    0x4B,0x22,0xD5,0xA6,0xAE,0xC9,0x9F,0x3C,0xDB,0x79,0xAA,0x5E,0xC0,0x68,0x38,0x47,
    0x9C,0xD5,0xEC,0xBA,0x71,0x64,0xF7,0xF2,0x2D,0xC1,0xD6,0x5F,0x63,0xD8,0x57,0x08
};
static const UInt8 kWS_CA2_NEW[kSecPolicySHA256Size] = {
    0xD6,0xF0,0x34,0xBD,0x94,0xAA,0x23,0x3F,0x02,0x97,0xEC,0xA4,0x24,0x5B,0x28,0x39,
    0x73,0xE4,0x47,0xAA,0x59,0x0F,0x31,0x0C,0x77,0xF4,0x8F,0xDF,0x83,0x11,0x22,0x54
};
static const UInt8 kWS_ECC[kSecPolicySHA256Size] = {
    0x8B,0x45,0xDA,0x1C,0x06,0xF7,0x91,0xEB,0x0C,0xAB,0xF2,0x6B,0xE5,0x88,0xF5,0xFB,
    0x23,0x16,0x5C,0x2E,0x61,0x4B,0xF8,0x85,0x56,0x2D,0x0D,0xCE,0x50,0xB2,0x9B,0x02
};
static const UInt8 kSC_SFSCA[kSecPolicySHA256Size] = {
    0xC7,0x66,0xA9,0xBE,0xF2,0xD4,0x07,0x1C,0x86,0x3A,0x31,0xAA,0x49,0x20,0xE8,0x13,
    0xB2,0xD1,0x98,0x60,0x8C,0xB7,0xB7,0xCF,0xE2,0x11,0x43,0xB8,0x36,0xDF,0x09,0xEA
};
static const UInt8 kSC_SHA2[kSecPolicySHA256Size] = {
    0xE1,0x78,0x90,0xEE,0x09,0xA3,0xFB,0xF4,0xF4,0x8B,0x9C,0x41,0x4A,0x17,0xD6,0x37,
    0xB7,0xA5,0x06,0x47,0xE9,0xBC,0x75,0x23,0x22,0x72,0x7F,0xCC,0x17,0x42,0xA9,0x11
};
static const UInt8 kSC_G2[kSecPolicySHA256Size] = {
    0xC7,0xBA,0x65,0x67,0xDE,0x93,0xA7,0x98,0xAE,0x1F,0xAA,0x79,0x1E,0x71,0x2D,0x37,
    0x8F,0xAE,0x1F,0x93,0xC4,0x39,0x7F,0xEA,0x44,0x1B,0xB7,0xCB,0xE6,0xFD,0x59,0x95
};

static void SecPVCCheckIssuerDateConstraints(SecPVCRef pvc) {
    static CFSetRef sConstrainedRoots = NULL;
    static dispatch_once_t _t;
    dispatch_once(&_t, ^{
        const UInt8 *v_hashes[] = {
            kWS_CA1_G2, kWS_CA1_NEW, kWS_CA2_NEW, kWS_ECC,
            kSC_SFSCA, kSC_SHA2, kSC_G2, kTestDateConstraintsRoot
        };
        CFMutableSetRef set = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
        CFIndex ix, count = sizeof(v_hashes)/sizeof(*v_hashes);
        for (ix=0; ix<count; ix++) {
            CFDataRef hash = CFDataCreateWithBytesNoCopy(NULL, v_hashes[ix],
                kSecPolicySHA256Size, kCFAllocatorNull);
            if (hash) {
                CFSetAddValue(set, hash);
                CFRelease(hash);
            }
        }
        sConstrainedRoots = set;
    });

    bool shouldDeny = false;
    CFIndex certIX, certCount = SecPVCGetCertificateCount(pvc);
    for (certIX = certCount - 1; certIX >= 0 && !shouldDeny; certIX--) {
        SecCertificateRef cert = SecPVCGetCertificateAtIndex(pvc, certIX);
        CFDataRef sha256 = SecCertificateCopySHA256Digest(cert);
        if (sha256 && CFSetContainsValue(sConstrainedRoots, sha256)) {
            /* matched a constrained root; check notBefore dates on all its children. */
            CFIndex childIX = certIX;
            while (--childIX >= 0) {
                SecCertificateRef child = SecPVCGetCertificateAtIndex(pvc, childIX);
                /* 1 Dec 2016 00:00:00 GMT */
                if (child && (CFAbsoluteTime)502243200.0 <= SecCertificateNotValidBefore(child)) {
                    SecPVCSetResultForced(pvc, kSecPolicyCheckBlackListedKey, certIX, kCFBooleanFalse, true);
                    pvc->result = kSecTrustResultFatalTrustFailure;
                    shouldDeny = true;
                    break;
                }
            }
        }
        CFReleaseNull(sha256);
    }
}

/* AUDIT[securityd](done):
   policy->_options is a caller provided dictionary, only its cf type has
   been checked.
 */
void SecPVCPathChecks(SecPVCRef pvc) {
    secdebug("policy", "begin path: %@", SecPathBuilderGetPath(pvc->builder));
    SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
    /* This needs to be initialized before we call any function that might call
       SecPVCSetResultForced(). */
    pvc->policyIX = 0;
    SecPolicyCheckIdLinkage(pvc, kSecPolicyCheckIdLinkage);
    if (SecPVCIsOkResult(pvc) || pvc->details) {
        /* @@@ This theoretically only needs to be done once per path, but since
           this function affects the pvc result, we'll run it every time. */
        SecPolicyCheckBasicCertificateProcessing(pvc,
            kSecPolicyCheckBasicCertificateProcessing);
    }

    CFArrayRef policies = pvc->policies;
    CFIndex count = CFArrayGetCount(policies);
    for (; pvc->policyIX < count; ++pvc->policyIX) {
        /* Validate all keys for all policies. */
        pvc->callbacks = gSecPolicyPathCallbacks;
        SecPolicyRef policy = SecPVCGetPolicy(pvc);
        CFDictionaryApplyFunction(policy->_options, SecPVCValidateKey, pvc);
        if (!SecPVCIsOkResult(pvc) && !pvc->details)
            return;
    }

    // Reset
    pvc->policyIX = 0;

    /* Check whether the TrustSettings say to deny a cert in the path. */
    SecPVCCheckUsageConstraints(pvc);

    /* Check for Blocklisted certs */
    SecPVCCheckIssuerDateConstraints(pvc);
    CFIndex ix;
    count = SecCertificatePathVCGetCount(path);
    for (ix = 1; ix < count; ix++) {
        SecPVCGrayListedKeyChecks(pvc, ix);
        SecPVCBlackListedKeyChecks(pvc, ix);
    }

    /* Path-based check tests. */
    if (!SecCertificatePathVCIsPathValidated(path)) {
        bool ev_check_ok = false;
        if (SecCertificatePathVCIsOptionallyEV(path)) {
            SecTrustResultType pre_ev_check_result = pvc->result;
            SecPolicyCheckEV(pvc, kSecPolicyCheckExtendedValidation);
            ev_check_ok = SecPVCIsOkResult(pvc);
            /* If ev checking failed, we still want to accept this chain
             as a non EV one, if it was valid as such. */
            pvc->result = pre_ev_check_result;
        }

        /* Check for CT */
        /* This call will set the value of pvc->is_ct, but won't change the result (pvc->result) */
        SecPolicyCheckCT(pvc, kSecPolicyCheckCertificateTransparency);

        /* Certs are only EV if they are also CT verified */
        if (ev_check_ok && SecCertificatePathVCIsCT(path)) {
            SecCertificatePathVCSetIsEV(path, true);
        }
    }

//errOut:
    secdebug("policy", "end %strusted path: %@",
        (SecPVCIsOkResult(pvc) ? "" : "not "), SecPathBuilderGetPath(pvc->builder));

    SecCertificatePathVCSetPathValidated(SecPathBuilderGetPath(pvc->builder));
    return;
}

void SecPVCPathCheckRevocationRequired(SecPVCRef pvc) {
    SecCertificatePathVCRef path = SecPathBuilderGetPath(pvc->builder);
    CFIndex ix, certCount = SecCertificatePathVCGetCount(path);
    for (ix = 0; ix < certCount; ix++) {
        /* If we require revocation (for that cert per the SecCertificateVCRef or
         * per the pvc) */
        if (SecCertificatePathVCIsRevocationRequiredForCertificateAtIndex(path, ix) ||
            ((ix == 0) && pvc->require_revocation_response)) {
            /* Do we have a valid revocation response? */
            SecRVCRef rvc = SecCertificatePathVCGetRVCAtIndex(path, ix);
            if (SecRVCGetEarliestNextUpdate(rvc) == NULL_TIME) {
                SecPVCSetResultForced(pvc, kSecPolicyCheckRevocationResponseRequired,
                                      ix, kCFBooleanFalse, true);
            }
        }
    }
}
