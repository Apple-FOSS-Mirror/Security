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
	File:		sslCert.cpp

	Contains:	certificate request/verify messages

	Written by:	Doug Mitchell

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/
#include "sslContext.h"
#include "sslHandshake.h"
#include "sslMemory.h"
#include "sslAlertMessage.h"
#include "sslDebug.h"
#include "sslUtils.h"
#include "sslDigests.h"
#include "appleCdsa.h"

#include <string.h>
#include <assert.h>

OSStatus
SSLEncodeCertificate(SSLRecord &certificate, SSLContext *ctx)
{   OSStatus        err;
    UInt32          totalLength;
    int             i, j, certCount;
    UInt8           *charPtr;
    SSLCertificate  *cert;
    
    /* 
	 * TBD: for client side, match Match DER-encoded acceptable DN list
	 * (ctx->acceptableDNList) to one of our certs. For now we just send 
	 * what we have since we don't support multiple certs.
	 *
	 * Note this can be called with localCert==0 for client seide in TLS1;
	 * in that case we send an empty cert msg.
	 */
    cert = ctx->localCert;
	assert((ctx->negProtocolVersion == SSL_Version_3_0) ||
		   (ctx->negProtocolVersion == TLS_Version_1_0));
	assert((cert != NULL) || (ctx->negProtocolVersion == TLS_Version_1_0));
    totalLength = 0;
    certCount = 0;
    while (cert)
    {   totalLength += 3 + cert->derCert.length;    /* 3 for encoded length field */
        ++certCount;
        cert = cert->next;
    }
    
    certificate.contentType = SSL_RecordTypeHandshake;
    certificate.protocolVersion = ctx->negProtocolVersion;
    if ((err = SSLAllocBuffer(certificate.contents, totalLength + 7, ctx)) != 0)
        return err;
    
    charPtr = certificate.contents.data;
    *charPtr++ = SSL_HdskCert;
    charPtr = SSLEncodeInt(charPtr, totalLength+3, 3);    /* Handshake message length */
    charPtr = SSLEncodeInt(charPtr, totalLength, 3);      /* Vector length */
    
    /* Root cert is first in the linked list, but has to go last, 
	 * so walk list backwards */
    for (i = 0; i < certCount; ++i)
    {   cert = ctx->localCert;
        for (j = i+1; j < certCount; ++j)
            cert = cert->next;
        charPtr = SSLEncodeInt(charPtr, cert->derCert.length, 3);
        memcpy(charPtr, cert->derCert.data, cert->derCert.length);
        charPtr += cert->derCert.length;
    }
    
    assert(charPtr == certificate.contents.data + certificate.contents.length);
    
    if ((ctx->protocolSide == SSL_ClientSide) && (ctx->localCert)) {
		/* this tells us to send a CertificateVerify msg after the
		 * client key exchange. We skip the cert vfy if we just
		 * sent an empty cert msg (i.e., we were asked for a cert
		 * but we don't have one). */
        ctx->certSent = 1;	
		assert(ctx->clientCertState == kSSLClientCertRequested);
		assert(ctx->certRequested);
		ctx->clientCertState = kSSLClientCertSent;
	}
    return noErr;
}

OSStatus
SSLProcessCertificate(SSLBuffer message, SSLContext *ctx)
{   OSStatus        err;
    UInt32          listLen, certLen;
    UInt8           *p;
    SSLCertificate  *cert;
    
    p = message.data;
    listLen = SSLDecodeInt(p,3);
    p += 3;
    if (listLen + 3 != message.length) {
    	sslErrorLog("SSLProcessCertificate: length decode error 1\n");
        return errSSLProtocol;
    }
    
    while (listLen > 0)
    {   certLen = SSLDecodeInt(p,3);
        p += 3;
        if (listLen < certLen + 3) {
    		sslErrorLog("SSLProcessCertificate: length decode error 2\n");
            return errSSLProtocol;
        }
		cert = (SSLCertificate *)sslMalloc(sizeof(SSLCertificate));
		if(cert == NULL) {
			return memFullErr;
		}
        if ((err = SSLAllocBuffer(cert->derCert, certLen, ctx)) != 0)
        {   sslFree(cert);
            return err;
        }
        memcpy(cert->derCert.data, p, certLen);
        p += certLen;
        cert->next = ctx->peerCert;     /* Insert backwards; root cert 
										 * will be first in linked list */
        ctx->peerCert = cert;
        listLen -= 3+certLen;
    }
    assert(p == message.data + message.length && listLen == 0);
    
    if (ctx->peerCert == 0) {
		/* this *might* be OK... */
		if((ctx->protocolSide == SSL_ServerSide) &&
		   (ctx->clientAuth != kAlwaysAuthenticate)) {
			/*
			 * we tried to authenticate, client doesn't have a cert, and 
			 * app doesn't require it. OK.
			 */
			return noErr;
		}
		else {
			AlertDescription desc;
			if(ctx->negProtocolVersion == SSL_Version_3_0) {
				/* this one's for SSL3 only */
				desc = SSL_AlertBadCert;
			}
			else {
				desc = SSL_AlertCertUnknown;
			}
			SSLFatalSessionAlert(desc, ctx);
			return errSSLXCertChainInvalid;
		}
    }
    if((err = sslVerifyCertChain(ctx, *ctx->peerCert)) != 0) {
		AlertDescription desc;
		switch(err) {
			case errSSLUnknownRootCert:
			case errSSLNoRootCert:
				desc = SSL_AlertUnknownCA;
				break;
			case errSSLCertExpired:
			case errSSLCertNotYetValid:
				desc = SSL_AlertCertExpired;
				break;
			case errSSLXCertChainInvalid:
			default:
				desc = SSL_AlertCertUnknown;
				break;
		}
		SSLFatalSessionAlert(desc, ctx);
        return err;
	}
	
	/* peer's certificate is the last one in the chain */
    cert = ctx->peerCert;
    while (cert->next != 0)
        cert = cert->next;
	/* Convert its public key to CDSA format */
    if ((err = sslPubKeyFromCert(ctx, 
    	cert->derCert, 
    	&ctx->peerPubKey,
    	&ctx->peerPubKeyCsp)) != 0)
        return err;
        
    return noErr;
}

OSStatus
SSLEncodeCertificateRequest(SSLRecord &request, SSLContext *ctx)
{   
	OSStatus    err;
    UInt32      dnListLen, msgLen;
    UInt8       *charPtr;
    DNListElem  *dn;
    
	assert(ctx->protocolSide == SSL_ServerSide);
	dnListLen = 0;
    dn = ctx->acceptableDNList;
    while (dn)
    {   dnListLen += 2 + dn->derDN.length;
        dn = dn->next;
    }
    msgLen = 1 + 1 + 2 + dnListLen;
    
    request.contentType = SSL_RecordTypeHandshake;
	assert((ctx->negProtocolVersion == SSL_Version_3_0) ||
		   (ctx->negProtocolVersion == TLS_Version_1_0));
    request.protocolVersion = ctx->negProtocolVersion;
    if ((err = SSLAllocBuffer(request.contents, msgLen + 4, ctx)) != 0)
        return err;
    
    charPtr = request.contents.data;
    *charPtr++ = SSL_HdskCertRequest;
    charPtr = SSLEncodeInt(charPtr, msgLen, 3);
    
    *charPtr++ = 1;        /* one cert type */
    *charPtr++ = 1;        /* RSA-sign type */
    charPtr = SSLEncodeInt(charPtr, dnListLen, 2);
    dn = ctx->acceptableDNList;
    while (dn)
    {   charPtr = SSLEncodeInt(charPtr, dn->derDN.length, 2);
        memcpy(charPtr, dn->derDN.data, dn->derDN.length);
        charPtr += dn->derDN.length;
        dn = dn->next;
    }
    
    assert(charPtr == request.contents.data + request.contents.length);
    return noErr;
}

OSStatus
SSLProcessCertificateRequest(SSLBuffer message, SSLContext *ctx)
{   
    unsigned        i;
    unsigned	    typeCount;
    UInt8           *charPtr;
    
	/* 
	 * Cert request only happens in during client authentication, which
	 * we don't do. We will however take this handshake msg and do 
	 * nothing with the enclosed DNList. We'll send a client cert
	 * if we have one but we don't do any DNList compare.
	 */
    if (message.length < 3) {
    	sslErrorLog("SSLProcessCertificateRequest: length decode error 1\n");
        return errSSLProtocol;
    }
    charPtr = message.data;
    typeCount = *charPtr++;
    if (typeCount < 1 || message.length < 3 + typeCount) {
    	sslErrorLog("SSLProcessCertificateRequest: length decode error 2\n");
        return errSSLProtocol;
    }
    for (i = 0; i < typeCount; i++)
    {   if (*charPtr++ == 1)
            ctx->x509Requested = 1;
    }
    
	#if		0	
	/* FIXME - currently untested  */
    unsigned	dnListLen;
	unsigned	dnLen;
    SSLBuffer	dnBuf;
    DNListElem  *dn;
	OSStatus	err;	
	
    dnListLen = SSLDecodeInt(charPtr, 2);
    charPtr += 2;
    if (message.length != 3 + typeCount + dnListLen) {
    	sslErrorLog("SSLProcessCertificateRequest: length decode error 3\n");
        return errSSLProtocol;
	}    
    while (dnListLen > 0)
    {   if (dnListLen < 2) {
    		sslErrorLog("SSLProcessCertificateRequest: dnListLen error 1\n");
            return errSSLProtocol;
        }
        dnLen = SSLDecodeInt(charPtr, 2);
        charPtr += 2;
        if (dnListLen < 2 + dnLen) {
     		sslErrorLog("SSLProcessCertificateRequest: dnListLen error 2\n");
           	return errSSLProtocol;
    	}
        if ((err = SSLAllocBuffer(dnBuf, sizeof(DNListElem), ctx)) != 0)
            return err;
        dn = (DNListElem*)dnBuf.data;
        if ((err = SSLAllocBuffer(dn->derDN, dnLen, ctx)) != 0)
        {   SSLFreeBuffer(dnBuf, ctx);
            return err;
        }
        memcpy(dn->derDN.data, charPtr, dnLen);
        charPtr += dnLen;
        dn->next = ctx->acceptableDNList;
        ctx->acceptableDNList = dn;
        dnListLen -= 2 + dnLen;
    }
    
    assert(charPtr == message.data + message.length);
	#endif	/* untested client-side authentication */
	
    return noErr;
}

OSStatus
SSLEncodeCertificateVerify(SSLRecord &certVerify, SSLContext *ctx)
{   OSStatus        err;
    UInt8           hashData[36];
    SSLBuffer       hashDataBuf, shaMsgState, md5MsgState;
    UInt32          len;
    UInt32		    outputLen;
    
    certVerify.contents.data = 0;
    hashDataBuf.data = hashData;
    hashDataBuf.length = 36;
    
    if ((err = CloneHashState(SSLHashSHA1, ctx->shaState, shaMsgState, ctx)) != 0)
        goto fail;
    if ((err = CloneHashState(SSLHashMD5, ctx->md5State, md5MsgState, ctx)) != 0)
        goto fail;
	assert(ctx->sslTslCalls != NULL);
    if ((err = ctx->sslTslCalls->computeCertVfyMac(ctx,	hashDataBuf, 
			shaMsgState, md5MsgState)) != 0)
        goto fail;
    
	assert(ctx->signingPrivKey != NULL);
	len = sslKeyLengthInBytes(ctx->signingPrivKey);
    
    certVerify.contentType = SSL_RecordTypeHandshake;
	assert((ctx->negProtocolVersion == SSL_Version_3_0) ||
		   (ctx->negProtocolVersion == TLS_Version_1_0));
    certVerify.protocolVersion = ctx->negProtocolVersion;
    if ((err = SSLAllocBuffer(certVerify.contents, len + 6, ctx)) != 0)
        goto fail;
    
    certVerify.contents.data[0] = SSL_HdskCertVerify;
    SSLEncodeInt(certVerify.contents.data+1, len+2, 3);
    SSLEncodeInt(certVerify.contents.data+4, len, 2);

	err = sslRawSign(ctx,
		ctx->signingPrivKey,
		ctx->signingKeyCsp,
		hashData,						// data to sign 
		36,								// MD5 size + SHA1 size
		certVerify.contents.data+6,		// signature destination
		len,							// we mallocd len+6
		&outputLen);
	if(err) {
		goto fail;
	}
    
    assert(outputLen == len);
    
    err = noErr;
    
fail:
    SSLFreeBuffer(shaMsgState, ctx);
    SSLFreeBuffer(md5MsgState, ctx);

    return err;
}

OSStatus
SSLProcessCertificateVerify(SSLBuffer message, SSLContext *ctx)
{   OSStatus        err;
    UInt8           hashData[36];
    UInt16          signatureLen;
    SSLBuffer       hashDataBuf, shaMsgState, md5MsgState;
    unsigned int    publicModulusLen;
    
    shaMsgState.data = 0;
    md5MsgState.data = 0;
    
    if (message.length < 2) {
    	sslErrorLog("SSLProcessCertificateVerify: msg len error\n");
        return errSSLProtocol;     
    }
    
    signatureLen = (UInt16)SSLDecodeInt(message.data, 2);
    if (message.length != (unsigned)(2 + signatureLen)) {
    	sslErrorLog("SSLProcessCertificateVerify: sig len error 1\n");
        return errSSLProtocol;
    }
    
	assert(ctx->peerPubKey != NULL);
	publicModulusLen = sslKeyLengthInBytes(ctx->peerPubKey);
    
    if (signatureLen != publicModulusLen) {
    	sslErrorLog("SSLProcessCertificateVerify: sig len error 2\n");
        return errSSLProtocol;
    }
    hashDataBuf.data = hashData;
    hashDataBuf.length = 36;
    
    if ((err = CloneHashState(SSLHashSHA1, ctx->shaState, shaMsgState, ctx)) != 0)
        goto fail;
    if ((err = CloneHashState(SSLHashMD5, ctx->md5State, md5MsgState, ctx)) != 0)
        goto fail;
	assert(ctx->sslTslCalls != NULL);
    if ((err = ctx->sslTslCalls->computeCertVfyMac(ctx, hashDataBuf, 
			shaMsgState, md5MsgState)) != 0)
        goto fail;
    
	/* 
	 * The CSP does the decrypt & compare for us in one shot
	 */
	err = sslRawVerify(ctx,
		ctx->peerPubKey,
		ctx->peerPubKeyCsp,		// FIXME - maybe we just use cspHand?
		hashData,				// data to verify
		36,
		message.data + 2, 		// signature
		signatureLen);
	if(err) {
		SSLFatalSessionAlert(SSL_AlertDecryptError, ctx);
		goto fail;
	}
    err = noErr;
    
fail:
    SSLFreeBuffer(shaMsgState, ctx);
    SSLFreeBuffer(md5MsgState, ctx);

    return err;
}
