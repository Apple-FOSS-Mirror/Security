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
	File:		sslHandshakeHello.c

	Contains:	Support for client hello and server hello messages. 

	Written by:	Doug Mitchell

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#include "sslContext.h"
#include "sslHandshake.h"
#include "sslMemory.h"
#include "sslSession.h"
#include "sslUtils.h"
#include "sslDebug.h"
#include "appleCdsa.h"
#include "sslDigests.h"
#include "cipherSpecs.h"

#include <string.h>

static OSStatus SSLEncodeRandom(unsigned char *p, SSLContext *ctx);

/* IE treats null session id as valid; two consecutive sessions with NULL ID
 * are considered a match. Workaround: when resumable sessions are disabled, 
 * send a random session ID. */
#define SSL_IE_NULL_RESUME_BUG		1
#if		SSL_IE_NULL_RESUME_BUG
#define SSL_NULL_ID_LEN				32	/* length of bogus session ID */
#endif

OSStatus
SSLEncodeServerHello(SSLRecord &serverHello, SSLContext *ctx)
{   OSStatus        err;
    UInt8           *charPtr;
    int             sessionIDLen;
    
    sessionIDLen = 0;
    if (ctx->sessionID.data != 0)
        sessionIDLen = (UInt8)ctx->sessionID.length;
	#if 	SSL_IE_NULL_RESUME_BUG
	if(sessionIDLen == 0) {
		sessionIDLen = SSL_NULL_ID_LEN;
	}	
	#endif	/* SSL_IE_NULL_RESUME_BUG */
		
	/* this was set to a known quantity in SSLProcessClientHello */
	assert(ctx->negProtocolVersion != SSL_Version_Undetermined);
	/* should not be here in this case */
	assert(ctx->negProtocolVersion != SSL_Version_2_0);
	sslLogNegotiateDebug("===SSL3 server: sending version %d_%d",
		ctx->negProtocolVersion >> 8, ctx->negProtocolVersion & 0xff);
	sslLogNegotiateDebug("...sessionIDLen = %d", sessionIDLen);
    serverHello.protocolVersion = ctx->negProtocolVersion;
    serverHello.contentType = SSL_RecordTypeHandshake;
    if ((err = SSLAllocBuffer(serverHello.contents, 42 + sessionIDLen, ctx)) != 0)
        return err;
    
    charPtr = serverHello.contents.data;
    *charPtr++ = SSL_HdskServerHello;
    charPtr = SSLEncodeInt(charPtr, 38 + sessionIDLen, 3);
    charPtr = SSLEncodeInt(charPtr, serverHello.protocolVersion, 2);
    if ((err = SSLEncodeRandom(charPtr, ctx)) != 0)
        return err;
    memcpy(ctx->serverRandom, charPtr, SSL_CLIENT_SRVR_RAND_SIZE);
    charPtr += SSL_CLIENT_SRVR_RAND_SIZE;
	*(charPtr++) = (UInt8)sessionIDLen;
	#if 	SSL_IE_NULL_RESUME_BUG
	if(ctx->sessionID.data != NULL) {
		/* normal path for enabled resumable session */
		memcpy(charPtr, ctx->sessionID.data, sessionIDLen);
	}
	else {
		/* IE workaround */
		SSLBuffer rb;
		rb.data = charPtr;
		rb.length = SSL_NULL_ID_LEN;
		sslRand(ctx, &rb);
	}
	#else	
    if (sessionIDLen > 0)
        memcpy(charPtr, ctx->sessionID.data, sessionIDLen);
	#endif	/* SSL_IE_NULL_RESUME_BUG */
	charPtr += sessionIDLen;
    charPtr = SSLEncodeInt(charPtr, ctx->selectedCipher, 2);
    *(charPtr++) = 0;      /* Null compression */

    sslLogNegotiateDebug("ssl3: server specifying cipherSuite 0x%lx", 
		(UInt32)ctx->selectedCipher);
	
    assert(charPtr == serverHello.contents.data + serverHello.contents.length);
    
    return noErr;
}

OSStatus
SSLProcessServerHello(SSLBuffer message, SSLContext *ctx)
{   OSStatus            err;
    SSLProtocolVersion  protocolVersion, negVersion;
    unsigned int        sessionIDLen;
    UInt8               *p;
    
    assert(ctx->protocolSide == SSL_ClientSide);
    
    if (message.length < 38 || message.length > 70) {
    	sslErrorLog("SSLProcessServerHello: msg len error\n");
        return errSSLProtocol;
    }
    p = message.data;
    
    protocolVersion = (SSLProtocolVersion)SSLDecodeInt(p, 2);
    p += 2;
	/* FIXME this should probably send appropriate alerts */
	err = sslVerifyProtVersion(ctx, protocolVersion, &negVersion);
	if(err) {
		return err;
	}
    ctx->negProtocolVersion = negVersion;
	switch(negVersion) {
		case SSL_Version_3_0:
			ctx->sslTslCalls = &Ssl3Callouts;
			break;
		case TLS_Version_1_0:
 			ctx->sslTslCalls = &Tls1Callouts;
			break;
		default:
			return errSSLNegotiation;
	}
    sslLogNegotiateDebug("===SSL3 client: negVersion is %d_%d",
		(negVersion >> 8) & 0xff, negVersion & 0xff);
    
    memcpy(ctx->serverRandom, p, 32);
    p += 32;
    
    sessionIDLen = *p++;
    if (message.length != 38 + sessionIDLen) {
    	sslErrorLog("SSLProcessServerHello: msg len error 2\n");
        return errSSLProtocol;
    }
    if (sessionIDLen > 0 && ctx->peerID.data != 0)
    {   /* Don't die on error; just treat it as an uncached session */
        err = SSLAllocBuffer(ctx->sessionID, sessionIDLen, ctx);
        if (err == 0)
            memcpy(ctx->sessionID.data, p, sessionIDLen);
    }
    p += sessionIDLen;
    
    ctx->selectedCipher = (UInt16)SSLDecodeInt(p,2);
    sslLogNegotiateDebug("===ssl3: server requests cipherKind %d", 
    	(unsigned)ctx->selectedCipher);
    p += 2;
    if ((err = FindCipherSpec(ctx)) != 0) {
        return err;
    }
    
    if (*p++ != 0)      /* Compression */
        return unimpErr;
    
    assert(p == message.data + message.length);
    return noErr;
}

OSStatus
SSLEncodeClientHello(SSLRecord &clientHello, SSLContext *ctx)
{   
	unsigned		length, i;
    OSStatus        err;
    unsigned char   *p;
    SSLBuffer       sessionIdentifier;
    UInt16          sessionIDLen;
    
    assert(ctx->protocolSide == SSL_ClientSide);
	
    sessionIDLen = 0;
    if (ctx->resumableSession.data != 0)
    {   if ((err = SSLRetrieveSessionID(ctx->resumableSession,
				&sessionIdentifier, ctx)) != 0)
        {   return err;
        }
        sessionIDLen = sessionIdentifier.length;
    }
    
    length = 39 + 2*(ctx->numValidCipherSpecs) + sessionIDLen;
    
	err = sslGetMaxProtVersion(ctx, &clientHello.protocolVersion);
	if(err) {
		/* we don't have a protocol enabled */
		return err;
	}
    clientHello.contentType = SSL_RecordTypeHandshake;
    if ((err = SSLAllocBuffer(clientHello.contents, length + 4, ctx)) != 0)
        return err;
    
    p = clientHello.contents.data;
    *p++ = SSL_HdskClientHello;
    p = SSLEncodeInt(p, length, 3);
    p = SSLEncodeInt(p, clientHello.protocolVersion, 2);
	sslLogNegotiateDebug("===SSL3 client: proclaiming max protocol "
		"%d_%d capable ONLY",
		clientHello.protocolVersion >> 8, clientHello.protocolVersion & 0xff);
   if ((err = SSLEncodeRandom(p, ctx)) != 0)
    {   SSLFreeBuffer(clientHello.contents, ctx);
        return err;
    }
    memcpy(ctx->clientRandom, p, SSL_CLIENT_SRVR_RAND_SIZE);
    p += 32;
    *p++ = sessionIDLen;    				/* 1 byte vector length */
    if (sessionIDLen > 0)
    {   memcpy(p, sessionIdentifier.data, sessionIDLen);
        if ((err = SSLFreeBuffer(sessionIdentifier, ctx)) != 0)
            return err;
    }
    p += sessionIDLen;
    p = SSLEncodeInt(p, 2*(ctx->numValidCipherSpecs), 2);  
											/* 2 byte long vector length */
    for (i = 0; i<ctx->numValidCipherSpecs; ++i)
        p = SSLEncodeInt(p, ctx->validCipherSpecs[i].cipherSpec, 2);
    *p++ = 1;                               /* 1 byte long vector */
    *p++ = 0;                               /* null compression */
    
    assert(p == clientHello.contents.data + clientHello.contents.length);
    
    if ((err = SSLInitMessageHashes(ctx)) != 0)
        return err;
    
    return noErr;
}

OSStatus
SSLProcessClientHello(SSLBuffer message, SSLContext *ctx)
{   OSStatus            err;
    SSLProtocolVersion  negVersion;
    UInt16              cipherListLen, cipherCount, desiredSpec, cipherSpec;
    UInt8               sessionIDLen, compressionCount;
    UInt8               *charPtr;
    unsigned            i;
    
    if (message.length < 41) {
    	sslErrorLog("SSLProcessClientHello: msg len error 1\n");
        return errSSLProtocol;
    }
    charPtr = message.data;
    ctx->clientReqProtocol = (SSLProtocolVersion)SSLDecodeInt(charPtr, 2);
    charPtr += 2;
	err = sslVerifyProtVersion(ctx, ctx->clientReqProtocol, &negVersion);
	if(err) {
		return err;
	}
	switch(negVersion) {
		case SSL_Version_3_0:
			ctx->sslTslCalls = &Ssl3Callouts;
			break;
		case TLS_Version_1_0:
 			ctx->sslTslCalls = &Tls1Callouts;
			break;
		default:
			return errSSLNegotiation;
	}
	ctx->negProtocolVersion = negVersion;
    sslLogNegotiateDebug("===SSL3 server: negVersion is %d_%d",
		negVersion >> 8, negVersion & 0xff);
    
    memcpy(ctx->clientRandom, charPtr, SSL_CLIENT_SRVR_RAND_SIZE);
    charPtr += 32;
    sessionIDLen = *(charPtr++);
    if (message.length < (unsigned)(41 + sessionIDLen)) {
    	sslErrorLog("SSLProcessClientHello: msg len error 2\n");
        return errSSLProtocol;
    }
	/* FIXME peerID is never set on server side.... */
    if (sessionIDLen > 0 && ctx->peerID.data != 0) 
    {   /* Don't die on error; just treat it as an uncacheable session */
        err = SSLAllocBuffer(ctx->sessionID, sessionIDLen, ctx);
        if (err == 0)
            memcpy(ctx->sessionID.data, charPtr, sessionIDLen);
    }
    charPtr += sessionIDLen;
    
    cipherListLen = (UInt16)SSLDecodeInt(charPtr, 2);  
								/* Count of cipherSpecs, must be even & >= 2 */
    charPtr += 2;
    if ((cipherListLen & 1) || 
	    (cipherListLen < 2) || 
		(message.length < (unsigned)(39 + sessionIDLen + cipherListLen))) {
    	sslErrorLog("SSLProcessClientHello: msg len error 3\n");
        return errSSLProtocol;
    }
    cipherCount = cipherListLen/2;
    cipherSpec = 0xFFFF;        /* No match marker */
    while (cipherSpec == 0xFFFF && cipherCount--)
    {   desiredSpec = (UInt16)SSLDecodeInt(charPtr, 2);
        charPtr += 2;
        for (i = 0; i <ctx->numValidCipherSpecs; i++)
        {   if (ctx->validCipherSpecs[i].cipherSpec == desiredSpec)
            {   cipherSpec = desiredSpec;
                break;
            }
        }
    }
    
    if (cipherSpec == 0xFFFF)
        return errSSLNegotiation;
    charPtr += 2 * cipherCount;    /* Advance past unchecked cipherCounts */
    ctx->selectedCipher = cipherSpec;
    if ((err = FindCipherSpec(ctx)) != 0) {
        return err;
    }
    sslLogNegotiateDebug("ssl3 server: selecting cipherKind 0x%x", (unsigned)ctx->selectedCipher);
    
    compressionCount = *(charPtr++);
    if ((compressionCount < 1) || 
	    (message.length < 
		    (unsigned)(38 + sessionIDLen + cipherListLen + compressionCount))) {
    	sslErrorLog("SSLProcessClientHello: msg len error 4\n");
        return errSSLProtocol;
    }
    /* Ignore list; we're doing null */
    
    if ((err = SSLInitMessageHashes(ctx)) != 0)
        return err;
    
    return noErr;
}

static OSStatus
SSLEncodeRandom(unsigned char *p, SSLContext *ctx)
{   SSLBuffer   randomData;
    OSStatus    err;
    UInt32      time;
    
    if ((err = sslTime(&time)) != 0)
        return err;
    SSLEncodeInt(p, time, 4);
    randomData.data = p+4;
    randomData.length = 28;
   	if((err = sslRand(ctx, &randomData)) != 0)
        return err;
    return noErr;
}

OSStatus
SSLInitMessageHashes(SSLContext *ctx)
{   OSStatus          err;

    if ((err = CloseHash(SSLHashSHA1, ctx->shaState, ctx)) != 0)
        return err;
    if ((err = CloseHash(SSLHashMD5,  ctx->md5State, ctx)) != 0)
        return err;
    if ((err = ReadyHash(SSLHashSHA1, ctx->shaState, ctx)) != 0)
        return err;
    if ((err = ReadyHash(SSLHashMD5,  ctx->md5State, ctx)) != 0)
        return err;
    return noErr;
}
