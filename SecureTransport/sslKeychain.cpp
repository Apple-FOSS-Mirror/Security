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
	File:		sslKeychain.c

	Contains:	Apple Keychain routines

	Written by:	Doug Mitchell

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#include "ssl.h"
#include "sslContext.h"
#include "sslMemory.h"
#include "appleCdsa.h"
#include "sslDebug.h"
#include "sslKeychain.h"
#include "sslUtils.h"
#include <string.h>
#include <assert.h>
#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacErrors.h>
#include <Security/Security.h>

/*
 * Given an array of certs (as SecIdentityRefs, specified by caller
 * in SSLSetCertificate or SSLSetEncryptionCertificate) and a 
 * destination SSLCertificate:
 *
 * -- free destCerts if we have any
 * -- Get raw cert data, convert to array of SSLCertificates in *destCert 
 * -- validate cert chain
 * -- get pub, priv keys from certRef[0], store in *pubKey, *privKey
 */
 
/* Convert a SecCertificateRef to an SSLCertificate * */
static OSStatus secCertToSslCert(
	SSLContext			*ctx,
	SecCertificateRef 	certRef,
	SSLCertificate		**sslCert)
{
	CSSM_DATA		certData;		// struct is transient, referent owned by 
									//   Sec layer
	OSStatus		ortn;
	SSLCertificate	*thisSslCert = NULL;
	
	ortn = SecCertificateGetData(certRef, &certData);
	if(ortn) {
		sslErrorLog("SecCertificateGetData() returned %d\n", (int)ortn);
		return ortn;
	}
	
	thisSslCert = (SSLCertificate *)sslMalloc(sizeof(SSLCertificate));
	if(thisSslCert == NULL) {
		return memFullErr;
	}
	if(SSLAllocBuffer(thisSslCert->derCert, certData.Length, 
			ctx)) {
		return memFullErr;
	}
	memcpy(thisSslCert->derCert.data, certData.Data, certData.Length);
	thisSslCert->derCert.length = certData.Length;
	*sslCert = thisSslCert;
	return noErr;
}

OSStatus 
parseIncomingCerts(
	SSLContext		*ctx,
	CFArrayRef		certs,
	SSLCertificate	**destCert,		/* &ctx->{localCert,encryptCert} */
	CSSM_KEY_PTR	*pubKey,		/* &ctx->signingPubKey, etc. */
	CSSM_KEY_PTR	*privKey,		/* &ctx->signingPrivKey, etc. */
	CSSM_CSP_HANDLE	*cspHand)		/* &ctx->signingKeyCsp, etc. */
{
	CFIndex				numCerts;
	CFIndex				cert;
	SSLCertificate		*certChain = NULL;
	SSLCertificate		*thisSslCert;
	SecKeychainRef		kcRef;
	OSStatus			ortn;
	SecIdentityRef 		identity;
	SecCertificateRef	certRef;
	SecKeyRef			keyRef;
	CSSM_DATA			certData;
	CSSM_CL_HANDLE		clHand;		// carefully derive from a SecCertificateRef
	CSSM_RETURN			crtn;
	
	assert(ctx != NULL);
	assert(destCert != NULL);		/* though its referent may be NULL */
	assert(pubKey != NULL);
	assert(privKey != NULL);
	assert(cspHand != NULL);
	
	sslDeleteCertificateChain(*destCert, ctx);
	*destCert = NULL;
	*pubKey   = NULL;
	*privKey  = NULL;
	*cspHand  = 0;
	
	if(certs == NULL) {
		sslErrorLog("parseIncomingCerts: NULL incoming cert array\n");
		return errSSLBadCert;
	}
	numCerts = CFArrayGetCount(certs);
	if(numCerts == 0) {
		sslErrorLog("parseIncomingCerts: empty incoming cert array\n");
		return errSSLBadCert;
	}
	
	/* 
	 * Certs[0] is an SecIdentityRef from which we extract subject cert,
	 * privKey, pubKey, and cspHand.
	 *
	 * 1. ensure the first element is a SecIdentityRef.
	 */
	identity = (SecIdentityRef)CFArrayGetValueAtIndex(certs, 0);
	if(identity == NULL) {
		sslErrorLog("parseIncomingCerts: bad cert array (1)\n");
		return paramErr;
	}	
	if(CFGetTypeID(identity) != SecIdentityGetTypeID()) {
		sslErrorLog("parseIncomingCerts: bad cert array (2)\n");
		return paramErr;
	}
	
	/* 
	 * 2. Extract cert, keys, CSP handle and convert to local format. 
	 */
	ortn = SecIdentityCopyCertificate(identity, &certRef);
	if(ortn) {
		sslErrorLog("parseIncomingCerts: bad cert array (3)\n");
		return ortn;
	}
	ortn = secCertToSslCert(ctx, certRef, &thisSslCert);
	if(ortn) {
		sslErrorLog("parseIncomingCerts: bad cert array (4)\n");
		return ortn;
	}
	/* enqueue onto head of cert chain */
	thisSslCert->next = certChain;
	certChain = thisSslCert;

	/* fetch private key from identity */
	ortn = SecIdentityCopyPrivateKey(identity, &keyRef);
	if(ortn) {
		sslErrorLog("parseIncomingCerts: SecIdentityCopyPrivateKey err %d\n",
			(int)ortn);
		return ortn;
	}
	ortn = SecKeyGetCSSMKey(keyRef, (const CSSM_KEY **)privKey);
	if(ortn) {
		sslErrorLog("parseIncomingCerts: SecKeyGetCSSMKey err %d\n",
			(int)ortn);
		return ortn;
	}
	/* FIXME = release keyRef? */
	
	/* obtain public key from cert */
	ortn = SecCertificateGetCLHandle(certRef, &clHand);
	if(ortn) {
		sslErrorLog("parseIncomingCerts: SecCertificateGetCLHandle err %d\n",
			(int)ortn);
		return ortn;
	}
	certData.Data = thisSslCert->derCert.data;
	certData.Length = thisSslCert->derCert.length;
	crtn = CSSM_CL_CertGetKeyInfo(clHand, &certData, pubKey);
	if(crtn) {
		sslErrorLog("parseIncomingCerts: CSSM_CL_CertGetKeyInfo err\n");
		return (OSStatus)crtn;
	}
	
	/* obtain keychain from key, CSP handle from keychain */
	ortn = SecKeychainItemCopyKeychain((SecKeychainItemRef)keyRef, &kcRef);
	if(ortn) {
		sslErrorLog("parseIncomingCerts: SecKeychainItemCopyKeychain err %d\n",
			(int)ortn);
		return ortn;
	}
	ortn = SecKeychainGetCSPHandle(kcRef, cspHand);
	if(ortn) {
		sslErrorLog("parseIncomingCerts: SecKeychainGetCSPHandle err %d\n",
			(int)ortn);
		return ortn;
	}
	
	/* OK, that's the subject cert. Fetch optional remaining certs. */
	/* 
	 * Convert: CFArray of SecCertificateRefs --> chain of SSLCertificates. 
	 * Incoming certs have root last; SSLCertificate chain has root
	 * first.
	 */
	for(cert=1; cert<numCerts; cert++) {
		certRef = (SecCertificateRef)CFArrayGetValueAtIndex(certs, cert);
		if(certRef == NULL) {
			sslErrorLog("parseIncomingCerts: bad cert array (5)\n");
			return paramErr;
		}	
		if(CFGetTypeID(certRef) != SecCertificateGetTypeID()) {
			sslErrorLog("parseIncomingCerts: bad cert array (6)\n");
			return paramErr;
		}
		
		/* Extract cert, convert to local format. 
		*/
		ortn = secCertToSslCert(ctx, certRef, &thisSslCert);
		if(ortn) {
			sslErrorLog("parseIncomingCerts: bad cert array (7)\n");
			return ortn;
		}
		/* enqueue onto head of cert chain */
		thisSslCert->next = certChain;
		certChain = thisSslCert;
	}
	
	/* validate the whole mess, skipping host name verify */
	ortn = sslVerifyCertChain(ctx, *certChain, false);
	if(ortn) {
		goto errOut;
	}
		
	/* SUCCESS */ 
	*destCert = certChain;
	return noErr;
	
errOut:
	/* free certChain, everything in it, other vars, return ortn */
	sslDeleteCertificateChain(certChain, ctx);
	/* FIXME - anything else? */
	return ortn;
}


