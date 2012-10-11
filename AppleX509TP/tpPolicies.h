/*
 * Copyright (c) 2000-2001 Apple Computer, Inc. All Rights Reserved.
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
	tpPolicies.h - TP module policy implementation

	Created 10/9/2000 by Doug Mitchell. 
*/

#ifndef	_TP_POLICIES_H_
#define _TP_POLICIES_H_

#include <Security/cssmtype.h>
#include <Security/cssmalloc.h>
#include <Security/cssmapple.h>
#include "TPCertInfo.h"

#ifdef __cplusplus
extern	"C" {
#endif /* __cplusplus */

/* 
 * Private CSSM_APPLE_TP_ACTION_FLAGS value to enable implicit 
 * root certs.
 */
#define	CSSM_TP_USE_INTERNAL_ROOT_CERTS		0x80000000

/*
 * Enumerated certificate policies enforced by this module.
 */
typedef enum {
	kTPDefault,			/* no extension parsing, just sig and expiration */
	kTPx509Basic,		/* basic X.509/RFC2459 */
	kTPiSign,			/* Apple code signing */
	kTP_SSL,			/* SecureTransport/SSL */
	kCrlPolicy,			/* cert chain verification via CRL */
	kTP_SMIME				/* S/MIME */			
} TPPolicy;

/*
 * Perform TP verification on a constructed (ordered) cert group.
 * Returns CSSM_TRUE on success.
 */
CSSM_RETURN tp_policyVerify(
	TPPolicy						policy,
	CssmAllocator					&alloc,
	CSSM_CL_HANDLE					clHand,
	CSSM_CSP_HANDLE					cspHand,
	TPCertGroup 					*certGroup,
	CSSM_BOOL						verifiedToRoot,		// last cert is good root
	CSSM_APPLE_TP_ACTION_FLAGS		actionFlags,
	const CSSM_DATA					*policyFieldData,	// optional
    void 							*policyControl);	// future use

#ifdef __cplusplus
}
#endif
#endif	/* _TP_POLICIES_H_ */
