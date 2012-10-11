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
 * RSA_DSA_signature.cpp - openssl-based signature classes.  
 */

#include "RSA_DSA_signature.h"
#include "RSA_DSA_utils.h"
#include <stdexcept>
#include <assert.h>
#include <Security/debugging.h>
#include <Security/cssmdata.h>
#include <opensslUtils/opensslUtils.h>
#include <opensslUtils/openRsaSnacc.h>

#define rsaSigDebug(args...) 	debug("rsaSig", ## args)

RSASigner::~RSASigner()
{
	if(mWeMallocdRsaKey) {
		assert(mRsaKey != NULL);
		RSA_free(mRsaKey);
		mRsaKey = NULL;
		mWeMallocdRsaKey = false;
	}
}
	
/* reusable init */
void RSASigner::signerInit(
	const Context 	&context,
	bool			isSigning)
{
	setIsSigning(isSigning);
	keyFromContext(context);
	setInitFlag(true);
}

/* sign */
void RSASigner::sign(
	const void 		*data, 
	size_t 			dataLen,
	void			*sig,	
	size_t			*sigLen)	/* IN/OUT */
{
	if(mRsaKey == NULL) {
		CssmError::throwMe(CSSMERR_CSP_INTERNAL_ERROR);
	}

	/* get encoded digest info */
	CssmAutoData 	encodedInfo(alloc());
	int irtn = generateDigestInfo(data,
		dataLen,
		digestAlg(),
		encodedInfo,
		RSA_size(mRsaKey));
	if(irtn) {
		rsaSigDebug("***digestInfo error\n");
		throwOpensslErr(irtn);
	}

	/* signature := encrypted digest info */
	irtn = RSA_private_encrypt(encodedInfo.length(), 
		(unsigned char *)encodedInfo.data(),
		(unsigned char *)sig, 
		mRsaKey,
		RSA_PKCS1_PADDING);
	if(irtn < 0) {
		throwRsaDsa("RSA_private_encrypt");
	}
	if((unsigned)irtn > *sigLen) {
		rsaSigDebug("RSA_private_encrypt: sig overflow");
		CssmError::throwMe(CSSMERR_CSP_OUTPUT_LENGTH_ERROR);
	}
	*sigLen = (unsigned)irtn;
}

/* verify */
void RSASigner::verify(
	const void 		*data, 
	size_t 			dataLen,
	const void		*sig,			
	size_t			sigLen)
{
	const char *op = NULL;
	bool throwSigVerify = false;
	
	if(mRsaKey == NULL) {
		CssmError::throwMe(CSSMERR_CSP_INTERNAL_ERROR);
	}

	/* get encoded digest info */
	CssmAutoData 	encodedInfo(alloc());
	int irtn = generateDigestInfo(data,
		dataLen,
		digestAlg(),
		encodedInfo,
		RSA_size(mRsaKey));
	if(irtn) {
		rsaSigDebug("***digestInfo error\n");
		CssmError::throwMe(/* FIXME */CSSMERR_CSP_INTERNAL_ERROR);
	}

	/* malloc decrypted signature */
	unsigned char *decryptSig = 
		(unsigned char *)alloc().malloc(RSA_size(mRsaKey));
	unsigned decryptSigLen;
	
	/* signature should be encrypted digest info; decrypt the signature  */
	irtn = RSA_public_decrypt(sigLen, 
		(unsigned char *)sig,
		decryptSig, 
		mRsaKey,
		RSA_PKCS1_PADDING);
	if(irtn < 0) {
		op = "RSA_public_decrypt";
		throwSigVerify = true;
		goto abort;
	}
	decryptSigLen = (unsigned)irtn;
	if(decryptSigLen != encodedInfo.length()) {
		rsaSigDebug("***Decrypted signature length error (exp %ld, got %d)\n",
			encodedInfo.length(), decryptSigLen);
		throwSigVerify = true;
		op = "RSA Sig length check";
		goto abort;
	}
	if(memcmp(decryptSig, encodedInfo.data(), decryptSigLen)) {
		rsaSigDebug("***Signature miscompare\n");
		throwSigVerify = true;
		op = "RSA Sig miscompare";
		goto abort;
	}
	else {
		irtn = 0;
	}
abort:
	if(decryptSig != NULL) {
		alloc().free(decryptSig);
	}	
	if(throwSigVerify) {
		CssmError::throwMe(CSSMERR_CSP_VERIFY_FAILED);
	}
}
		
/* works for both, but only used for signing */
size_t RSASigner::maxSigSize()
{
	if(mRsaKey == NULL) {
		return 0;
	}
	return RSA_size(mRsaKey);
}

/* 
 * obtain key from context, validate, convert to native RSA key
 */
void RSASigner::keyFromContext(
	const Context 	&context)
{
	if(initFlag() && (mRsaKey != NULL)) {
		/* reusing context, OK */
		return;
	}
	
	CSSM_KEYCLASS 	keyClass;
	CSSM_KEYUSE		keyUse;
	if(isSigning()) {
		/* signing with private key */
		keyClass = CSSM_KEYCLASS_PRIVATE_KEY; 
		keyUse   = CSSM_KEYUSE_SIGN;
	}
	else {
		/* verifying with public key */
		keyClass = CSSM_KEYCLASS_PUBLIC_KEY;
		keyUse   = CSSM_KEYUSE_VERIFY;
	}
	if(mRsaKey == NULL) {
		mRsaKey = contextToRsaKey(context,
			mSession,
			keyClass,
			keyUse,
			mWeMallocdRsaKey);
	}
}

DSASigner::~DSASigner()
{
	if(mWeMallocdDsaKey) {
		assert(mDsaKey != NULL);
		DSA_free(mDsaKey);
		mDsaKey = NULL;
		mWeMallocdDsaKey = false;
	}
}
	
/* reusable init */
void DSASigner::signerInit(
	const Context 	&context,
	bool			isSigning)
{
	setIsSigning(isSigning);
	keyFromContext(context);
	setInitFlag(true);
}

/* sign */
void DSASigner::sign(
	const void 		*data, 
	size_t 			dataLen,
	void			*sig,	
	size_t			*sigLen)	/* IN/OUT */
{
	if(mDsaKey == NULL) {
		CssmError::throwMe(CSSMERR_CSP_INTERNAL_ERROR);
	}
	if(mDsaKey->priv_key == NULL) {
		CssmError::throwMe(CSSMERR_CSP_INVALID_KEY_CLASS);
	}
	
	/* get signature in internal format */
	DSA_SIG *dsaSig = DSA_do_sign((unsigned char *)data, dataLen, mDsaKey);
	if(dsaSig == NULL) {
		throwRsaDsa("DSA_do_sign");
	}
	
	/* DER encode the signature */
	CssmAutoData 	encodedSig(alloc());
	int irtn = DSASigEncode(dsaSig, encodedSig);
	if(irtn) {
		throwRsaDsa("DSASigEncode");
	}
	if(encodedSig.length() > *sigLen) {
		throwRsaDsa("DSA sign overflow");
	}
	memmove(sig, encodedSig.data(), encodedSig.length());
	*sigLen = encodedSig.length();
	DSA_SIG_free(dsaSig);
}

/* verify */
void DSASigner::verify(
	const void 		*data, 
	size_t 			dataLen,
	const void		*sig,			
	size_t			sigLen)
{
	bool 			throwSigVerify = false;
	DSA_SIG 		*dsaSig = NULL;
	CSSM_RETURN		crtn = CSSM_OK;
	int					irtn;
	
	if(mDsaKey == NULL) {
		CssmError::throwMe(CSSMERR_CSP_INTERNAL_ERROR);
	}
	if(mDsaKey->pub_key == NULL) {
		CssmError::throwMe(CSSMERR_CSP_INVALID_KEY_CLASS);
	}

	/* incoming sig is DER encoded....decode into internal format */
	dsaSig = DSA_SIG_new();
	crtn = DSASigDecode(dsaSig, sig, sigLen);
	if(crtn) {
		goto abort;
	}

	irtn = DSA_do_verify((unsigned char *)data, dataLen, dsaSig, mDsaKey);
	if(!irtn) {
		throwSigVerify = true;
	}

abort:
	if(dsaSig != NULL) {
		DSA_SIG_free(dsaSig);
	}	
	if(throwSigVerify) {
		CssmError::throwMe(CSSMERR_CSP_VERIFY_FAILED);
	}
	else if(crtn) {
		CssmError::throwMe(crtn);
	}
}
		
/* 
 * Works for both, but only used for signing.
 * DSA sig is a sequence of two 160-bit integers. 
 */
size_t DSASigner::maxSigSize()
{
	if(mDsaKey == NULL) {
		return 0;
	}
	size_t outSize;
	size_t sizeOfOneInt;
	
	sizeOfOneInt = (160 / 8) +		// the raw contents
					1 +				// possible leading zero
					2;				// tag + length (assume DER, not BER)
	outSize = (2 * sizeOfOneInt) + 5;
	return outSize;
}

/* 
 * obtain key from context, validate, convert to native DSA key
 */
void DSASigner::keyFromContext(
	const Context 	&context)
{
	if(initFlag() && (mDsaKey != NULL)) {
		/* reusing context, OK */
		return;
	}
	
	CSSM_KEYCLASS 	keyClass;
	CSSM_KEYUSE		keyUse;
	if(isSigning()) {
		/* signing with private key */
		keyClass = CSSM_KEYCLASS_PRIVATE_KEY;
		keyUse   = CSSM_KEYUSE_SIGN;
	}
	else {
		/* verifying with public key */
		keyClass = CSSM_KEYCLASS_PUBLIC_KEY;
		keyUse   = CSSM_KEYUSE_VERIFY;
	}
	if(mDsaKey == NULL) {
		mDsaKey = contextToDsaKey(context,
			mSession,
			keyClass,
			keyUse,
			mWeMallocdDsaKey);
	}
}
