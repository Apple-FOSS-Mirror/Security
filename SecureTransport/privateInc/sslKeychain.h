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
	File:		sslKeychain.h

	Contains:	Apple Keychain routines

	Written by:	Doug Mitchell, based on Netscape SSLRef 3.0

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#ifndef	_SSL_KEYCHAIN_H_
#define _SSL_KEYCHAIN_H_


#ifndef	_SSLCTX_H_
#include "sslctx.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if	(ST_SERVER_MODE_ENABLE || ST_CLIENT_AUTHENTICATION)
/*
 * Given an array of certs (as KCItemRefs) and a destination
 * SSLCertificate:
 *
 * -- free destCerts if we have any
 * -- Get raw cert data, convert to array of SSLCertificates in *destCert 
 * -- get pub, priv keys from certRef[0], store in *pubKey, *privKey
 * -- validate cert chain
 *
 */
OSStatus 
parseIncomingCerts(
	SSLContext		*ctx,
	CFArrayRef		certs,
	SSLCertificate	**destCert,		/* &ctx->{localCert,encryptCert} */
	CSSM_KEY_PTR	*pubKey,		/* &ctx->signingPubKey, etc. */
	CSSM_KEY_PTR	*privKey,		/* &ctx->signingPrivKey, etc. */
	CSSM_CSP_HANDLE	*cspHand		/* &ctx->signingKeyCsp, etc. */
	#if				ST_KC_KEYS_NEED_REF
	,
	SecKeychainRef	*privKeyRef);	/* &ctx->signingKeyRef, etc. */
	#else
	);
	#endif			ST_KC_KEYS_NEED_REF
	
#endif	/* (ST_SERVER_MODE_ENABLE || ST_CLIENT_AUTHENTICATION) */

/*
 * Add Apple built-in root certs to ctx->trustedCerts.
 */
OSStatus 
addBuiltInCerts	(
	SSLContextRef	ctx);

#if		ST_KEYCHAIN_ENABLE && ST_MANAGES_TRUSTED_ROOTS

/*
 * Given an open Keychain:
 * -- Get raw cert data, add to array of CSSM_DATAs in 
 *    ctx->trustedCerts 
 * -- verify that each of these is a valid (self-verifying)
 *    root cert
 * -- add each subject name to acceptableDNList
 */
OSStatus
parseTrustedKeychain(
	SSLContextRef		ctx,
	SecKeychainRef		keyChainRef);

/*
 * Given a newly encountered root cert (obtained from a peer's cert chain),
 * add it to newRootCertKc if the user so allows, and if so, add it to 
 * trustedCerts.
 */
SSLErr
sslAddNewRoot(
	SSLContext			*ctx, 
	const CSSM_DATA_PTR	rootCert);

#endif	/* ST_KEYCHAIN_ENABLE && ST_MANAGES_TRUSTED_ROOTS */

#ifdef __cplusplus
}
#endif

#endif	/* _SSL_KEYCHAIN_H_ */