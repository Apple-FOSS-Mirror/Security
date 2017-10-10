/*
 * Copyright (c) 2006,2010,2012,2014-2017 Apple Inc. All Rights Reserved.
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
 *
 * verify_cert.c
 */

#include <Security/SecTrust.h>
#include <Security/SecKeychain.h>
#include <Security/SecPolicy.h>
#include <Security/SecPolicySearch.h>
#include <Security/cssmapple.h>
#include <Security/oidsalg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include "trusted_cert_utils.h"
#include "verify_cert.h"
#include <utilities/SecCFRelease.h>

/*
 * Read file as a cert, add to a CFArray, creating the array if necessary
 */
static int addCertFile(
	const char *fileName,
	CFMutableArrayRef *array)
{
	SecCertificateRef certRef;

	if(readCertFile(fileName, &certRef)) {
		return -1;
	}
	if(*array == NULL) {
		*array = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	}
	CFArrayAppendValue(*array, certRef);
	CFRelease(certRef);
	return 0;
}

int
verify_cert(int argc, char * const *argv)
{
	extern char			*optarg;
	extern int			optind;
	OSStatus			ortn;
	int					arg;
	CFMutableArrayRef	certs = NULL;
	CFMutableArrayRef	roots = NULL;
	CFMutableArrayRef	keychains = NULL;
	CFMutableArrayRef	policies = NULL;
	const CSSM_OID		*policy = &CSSMOID_APPLE_X509_BASIC;
	SecKeychainRef		kcRef = NULL;
	int					ourRtn = 0;
	bool				quiet = false;
	bool				client = false;
	SecPolicyRef		policyRef = NULL;
	SecPolicyRef		revPolicyRef = NULL;
	SecTrustRef			trustRef = NULL;
	SecPolicySearchRef	searchRef = NULL;
	const char			*emailAddrs = NULL;
	const char			*sslHost = NULL;
	const char			*name = NULL;
	CSSM_APPLE_TP_SSL_OPTIONS	sslOpts;
	CSSM_APPLE_TP_SMIME_OPTIONS	smimeOpts;
	CSSM_APPLE_TP_ACTION_FLAGS actionFlags = 0;
	bool				forceActionFlags = false;
	CSSM_APPLE_TP_ACTION_DATA	actionData;
	CSSM_DATA			optionData;
	CFDataRef			cfActionData = NULL;
	SecTrustResultType	resultType;
	OSStatus			ocrtn;
	struct tm			time;
	CFGregorianDate		gregorianDate;
	CFDateRef			dateRef = NULL;
	CFOptionFlags		revOptions = 0;

	if(argc < 2) {
		return 2; /* @@@ Return 2 triggers usage message. */
	}
	/* permit network cert fetch unless explicitly turned off with '-L' */
	actionFlags |= CSSM_TP_ACTION_FETCH_CERT_FROM_NET;
	optind = 1;
	while ((arg = getopt(argc, argv, "Cc:r:p:k:e:s:d:LlNnqR:")) != -1) {
		switch (arg) {
			case 'C':
				client = true;
				break;
			case 'c':
				/* this can be specified multiple times */
				if(addCertFile(optarg, &certs)) {
					ourRtn = 1;
					goto errOut;
				}
				break;
			case 'r':
				/* this can be specified multiple times */
				if(addCertFile(optarg, &roots)) {
					ourRtn = 1;
					goto errOut;
				}
				break;
			case 'p':
				policy = policyStringToOid(optarg);
				if(policy == NULL) {
					ourRtn = 2;
					goto errOut;
				}
				break;
			case 'k':
				ortn = SecKeychainOpen(optarg, &kcRef);
				if(ortn) {
					cssmPerror("SecKeychainOpen", ortn);
					ourRtn = 1;
					goto errOut;
				}
				/* this can be specified multiple times */
				if(keychains == NULL) {
					keychains = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
				}
				CFArrayAppendValue(keychains, kcRef);
				CFRelease(kcRef);
				break;
			case 'L':
				actionFlags &= ~CSSM_TP_ACTION_FETCH_CERT_FROM_NET;
				forceActionFlags = true;
				break;
			case 'l':
				actionFlags |= CSSM_TP_ACTION_LEAF_IS_CA;
				break;
			case 'n': {
				/* Legacy macOS used 'n' as the "no keychain search list" flag.
				   iOS interprets it as the name option, with one argument.
				*/
				char *o = argv[optind];
				if (o && o[0] != '-') {
					name = optarg;
					++optind;
					break;
				}
			}	/* intentional fall-through to "no keychains" case, if no arg */
			case 'N':
				/* No keychains, signalled by empty keychain array */
				if(keychains != NULL) {
					fprintf(stderr, "-k and -%c are mutually exclusive\n", arg);
					ourRtn = 2;
					goto errOut;
				}
				keychains = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
				break;
			case 'e':
				emailAddrs = optarg;
				break;
			case 's':
				sslHost = optarg;
				break;
			case 'q':
				quiet = true;
				break;
			case 'd':
				memset(&time, 0, sizeof(struct tm));
				if (strptime(optarg, "%Y-%m-%d-%H:%M:%S", &time) == NULL) {
					if (strptime(optarg, "%Y-%m-%d", &time) == NULL) {
						fprintf(stderr, "Date processing error\n");
						ourRtn = 2;
						goto errOut;
					}
				}
				gregorianDate.second = time.tm_sec;
				gregorianDate.minute = time.tm_min;
				gregorianDate.hour = time.tm_hour;
				gregorianDate.day = time.tm_mday;
				gregorianDate.month = time.tm_mon + 1;
				gregorianDate.year = time.tm_year + 1900;

				if (dateRef == NULL) {
					dateRef = CFDateCreate(NULL, CFGregorianDateGetAbsoluteTime(gregorianDate, NULL));
				}
				break;
			case 'R':
				revOptions |= revCheckOptionStringToFlags(optarg);
				break;
			default:
				ourRtn = 2;
				goto errOut;
		}
	}
	if(optind != argc) {
		ourRtn = 2;
		goto errOut;
	}

	if(certs == NULL) {
		if(roots == NULL) {
			fprintf(stderr, "***No certs specified.\n");
			ourRtn = 2;
			goto errOut;
		}
		if(CFArrayGetCount(roots) != 1) {
			fprintf(stderr, "***Multiple roots and no certs not allowed.\n");
			ourRtn = 2;
			goto errOut;
		}

		/* no certs and one root: verify the root */
		certs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
		CFArrayAppendValue(certs, CFArrayGetValueAtIndex(roots, 0));
		actionFlags |= CSSM_TP_ACTION_LEAF_IS_CA;
	}

	/* cook up a SecPolicyRef */
	ortn = SecPolicySearchCreate(CSSM_CERT_X_509v3,
		policy,
		NULL,				// policy opts
		&searchRef);
	if(ortn) {
		cssmPerror("SecPolicySearchCreate", ortn);
		ourRtn = 1;
		goto errOut;
	}
	ortn = SecPolicySearchCopyNext(searchRef, &policyRef);
	if(ortn) {
		cssmPerror("SecPolicySearchCopyNext", ortn);
		ourRtn = 1;
		goto errOut;
	}

	/* per-policy options */
	if(compareOids(policy, &CSSMOID_APPLE_TP_SSL) || compareOids(policy, &CSSMOID_APPLE_TP_APPLEID_SHARING)) {
		const char *nameStr = (name) ? name : ((sslHost) ? sslHost : NULL);
		if(nameStr) {
			memset(&sslOpts, 0, sizeof(sslOpts));
			sslOpts.Version = CSSM_APPLE_TP_SSL_OPTS_VERSION;
			sslOpts.ServerName = nameStr;
			sslOpts.ServerNameLen = (uint32) strlen(nameStr);
			sslOpts.Flags = (client) ? CSSM_APPLE_TP_SSL_CLIENT : 0;
			optionData.Data = (uint8 *)&sslOpts;
			optionData.Length = sizeof(sslOpts);
			ortn = SecPolicySetValue(policyRef, &optionData);
			if(ortn) {
				cssmPerror("SecPolicySetValue", ortn);
				ourRtn = 1;
				goto errOut;
			}
		}
	}
	if(compareOids(policy, &CSSMOID_APPLE_TP_SMIME)) {
		const char *nameStr = (name) ? name : ((emailAddrs) ? emailAddrs : NULL);
		if(nameStr) {
			memset(&smimeOpts, 0, sizeof(smimeOpts));
			smimeOpts.Version = CSSM_APPLE_TP_SMIME_OPTS_VERSION;
			smimeOpts.SenderEmail = nameStr;
			smimeOpts.SenderEmailLen = (uint32) strlen(nameStr);
			optionData.Data = (uint8 *)&smimeOpts;
			optionData.Length = sizeof(smimeOpts);
			ortn = SecPolicySetValue(policyRef, &optionData);
			if(ortn) {
				cssmPerror("SecPolicySetValue", ortn);
				ourRtn = 1;
				goto errOut;
			}
		}
	}

	/* create policies array */
	policies = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFArrayAppendValue(policies, policyRef);
	/* add optional SecPolicyRef for revocation, if specified */
	if(revOptions != 0) {
		revPolicyRef = SecPolicyCreateRevocation(revOptions);
		CFArrayAppendValue(policies, revPolicyRef);
	}

	/* create trust reference from certs and policies */
	ortn = SecTrustCreateWithCertificates(certs, policies, &trustRef);
	if(ortn) {
		cssmPerror("SecTrustCreateWithCertificates", ortn);
		ourRtn = 1;
		goto errOut;
	}

	/* roots (anchors) are optional */
	if(roots != NULL) {
		ortn = SecTrustSetAnchorCertificates(trustRef, roots);
		if(ortn) {
			cssmPerror("SecTrustSetAnchorCertificates", ortn);
			ourRtn = 1;
			goto errOut;
		}
	}
	if(actionFlags || forceActionFlags) {
		memset(&actionData, 0, sizeof(actionData));
		actionData.Version = CSSM_APPLE_TP_ACTION_VERSION;
		actionData.ActionFlags = actionFlags;
		cfActionData = CFDataCreate(NULL, (UInt8 *)&actionData, sizeof(actionData));
		ortn = SecTrustSetParameters(trustRef, CSSM_TP_ACTION_DEFAULT, cfActionData);
		if(ortn) {
			cssmPerror("SecTrustSetParameters", ortn);
			ourRtn = 1;
			goto errOut;
		}
	}
	if(keychains) {
		ortn = SecTrustSetKeychains(trustRef, keychains);
		if(ortn) {
			cssmPerror("SecTrustSetKeychains", ortn);
			ourRtn = 1;
			goto errOut;
		}
	}
	if(dateRef != NULL) {
		ortn = SecTrustSetVerifyDate(trustRef, dateRef);
		if(ortn) {
			cssmPerror("SecTrustSetVerifyDate", ortn);
			ourRtn = 1;
			goto errOut;
		}
	}

	/* GO */
	ortn = SecTrustEvaluate(trustRef, &resultType);
	if(ortn) {
		/* should never fail - error on this doesn't mean the cert verified badly */
		cssmPerror("SecTrustEvaluate", ortn);
		ourRtn = 1;
		goto errOut;
	}
	switch(resultType) {
		case kSecTrustResultUnspecified:
			/* cert chain valid, no special UserTrust assignments */
		case kSecTrustResultProceed:
			/* cert chain valid AND user explicitly trusts this */
			break;
		case kSecTrustResultDeny:
			if(!quiet) {
				fprintf(stderr, "SecTrustEvaluate result: kSecTrustResultDeny\n");
			}
			ourRtn = 1;
			break;
		default:
			ourRtn = 1;
			if(!quiet) {
				/* See what the TP had to say about this */
				ortn = SecTrustGetCssmResultCode(trustRef, &ocrtn);
				if(ortn) {
					cssmPerror("SecTrustGetCssmResultCode", ortn);
				}
				else  {
					cssmPerror("Cert Verify Result", ocrtn);
				}
			}
			break;
	}

	if((ourRtn == 0) & !quiet) {
		printf("...certificate verification successful.\n");
	}
errOut:
    CFReleaseNull(dateRef);
	/* cleanup */
	CFRELEASE(certs);
	CFRELEASE(roots);
	CFRELEASE(keychains);
	CFRELEASE(policies);
	CFRELEASE(revPolicyRef);
	CFRELEASE(policyRef);
	CFRELEASE(trustRef);
	CFRELEASE(searchRef);
	CFRELEASE(cfActionData);
	return ourRtn;
}
