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


/*  *********************************************************************
    File: sslHandshake.h - SSL Handshake Layer
    ****************************************************************** */

#ifndef _SSLHANDSHAKE_H_
#define _SSLHANDSHAKE_H_

#include "cryptType.h"
#include "sslRecord.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{   SSL_HdskHelloRequest = 0,
    SSL_HdskClientHello = 1,
    SSL_HdskServerHello = 2,
    SSL_HdskCert = 11,
    SSL_HdskServerKeyExchange = 12,
    SSL_HdskCertRequest = 13,
    SSL_HdskServerHelloDone = 14,
    SSL_HdskCertVerify = 15,
    SSL_HdskClientKeyExchange = 16,
    SSL_HdskFinished = 20,
    SSL_HdskNoCertAlert = 100
} SSLHandshakeType;

typedef enum
{   SSL_read,
    SSL_write
} CipherSide;

typedef enum
{   
	SSL_HdskStateUninit = 0,			/* only valid within SSLContextAlloc */
	SSL_HdskStateServerUninit,			/* no handshake yet */
	SSL_HdskStateClientUninit,			/* no handshake yet */
	SSL_HdskStateGracefulClose,
    SSL_HdskStateErrorClose,
	SSL_HdskStateNoNotifyClose,			/* server disconnected with no
										 *   notify msg */
    /* remainder must be consecutive */
    SSL_HdskStateServerHello,           /* must get server hello; client hello sent */
    SSL_HdskStateServerHelloUnknownVersion, 
										/* Could get SSL 2 or SSL 3 server hello back */
    SSL_HdskStateKeyExchange,           /* must get key exchange; cipher spec 
										 *   requires it */
    SSL_HdskStateCert,               	/* may get certificate or certificate 
										 *   request (if no cert request received yet) */
    SSL_HdskStateHelloDone,             /* must get server hello done; after key 
										 *   exchange or fixed DH parameters */
    SSL_HdskStateClientCert,         	/* must get certificate or no cert alert 
										 *   from client */
    SSL_HdskStateClientKeyExchange,     /* must get client key exchange */
    SSL_HdskStateClientCertVerify,      /* must get certificate verify from client */
    SSL_HdskStateChangeCipherSpec,      /* time to change the cipher spec */
    SSL_HdskStateFinished,              /* must get a finished message in the 
										 *   new cipher spec */
    SSL2_HdskStateClientMasterKey,
    SSL2_HdskStateClientFinished,
    SSL2_HdskStateServerHello,
    SSL2_HdskStateServerVerify,
    SSL2_HdskStateServerFinished,
    SSL2_HdskStateServerReady,          /* ready for I/O; server side */
    SSL2_HdskStateClientReady           /* ready for I/O; client side */
} SSLHandshakeState;
    
typedef struct
{   SSLHandshakeType    type;
    SSLBuffer           contents;
} SSLHandshakeMsg;

#define SSL_Finished_Sender_Server  0x53525652
#define SSL_Finished_Sender_Client  0x434C4E54

/** sslHandshake.c **/
typedef OSStatus (*EncodeMessageFunc)(SSLRecord &rec, SSLContext *ctx);
OSStatus SSLProcessHandshakeRecord(SSLRecord rec, SSLContext *ctx);
OSStatus SSLPrepareAndQueueMessage(EncodeMessageFunc msgFunc, SSLContext *ctx);
OSStatus SSLAdvanceHandshake(SSLHandshakeType processed, SSLContext *ctx);
OSStatus SSL3ReceiveSSL2ClientHello(SSLRecord rec, SSLContext *ctx);

/** sslChangeCipher.c **/
OSStatus SSLEncodeChangeCipherSpec(SSLRecord &rec, SSLContext *ctx);
OSStatus SSLProcessChangeCipherSpec(SSLRecord rec, SSLContext *ctx);
OSStatus SSLDisposeCipherSuite(CipherContext *cipher, SSLContext *ctx);

/** sslCert.c **/
OSStatus SSLEncodeCertificate(SSLRecord &certificate, SSLContext *ctx);
OSStatus SSLProcessCertificate(SSLBuffer message, SSLContext *ctx);
OSStatus SSLEncodeCertificateRequest(SSLRecord &request, SSLContext *ctx);
OSStatus SSLProcessCertificateRequest(SSLBuffer message, SSLContext *ctx);
OSStatus SSLEncodeCertificateVerify(SSLRecord &verify, SSLContext *ctx);
OSStatus SSLProcessCertificateVerify(SSLBuffer message, SSLContext *ctx);

/** sslHandshakeHello.c **/
OSStatus SSLEncodeServerHello(SSLRecord &serverHello, SSLContext *ctx);
OSStatus SSLProcessServerHello(SSLBuffer message, SSLContext *ctx);
OSStatus SSLEncodeClientHello(SSLRecord &clientHello, SSLContext *ctx);
OSStatus SSLProcessClientHello(SSLBuffer message, SSLContext *ctx);
OSStatus SSLInitMessageHashes(SSLContext *ctx);
OSStatus SSLEncodeRSAPremasterSecret(SSLContext *ctx);
OSStatus SSLEncodeDHPremasterSecret(SSLContext *ctx);
OSStatus SSLInitPendingCiphers(SSLContext *ctx);

/** sslKeyExchange.c **/
OSStatus SSLEncodeServerKeyExchange(SSLRecord &keyExch, SSLContext *ctx);
OSStatus SSLProcessServerKeyExchange(SSLBuffer message, SSLContext *ctx);
OSStatus SSLEncodeKeyExchange(SSLRecord &keyExchange, SSLContext *ctx);
OSStatus SSLProcessKeyExchange(SSLBuffer keyExchange, SSLContext *ctx);

/** sslHandshakeFinish.c **/
OSStatus SSLEncodeFinishedMessage(SSLRecord &finished, SSLContext *ctx);
OSStatus SSLProcessFinished(SSLBuffer message, SSLContext *ctx);
OSStatus SSLEncodeServerHelloDone(SSLRecord &helloDone, SSLContext *ctx);
OSStatus SSLProcessServerHelloDone(SSLBuffer message, SSLContext *ctx);
OSStatus SSLCalculateFinishedMessage(SSLBuffer finished, SSLBuffer shaMsgState, SSLBuffer md5MsgState, UInt32 senderID, SSLContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* _SSLHANDSHAKE_H_ */
