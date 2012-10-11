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
	File:		sslContext.h

	Contains:	Private SSL typedefs: SSLContext and its components

	Written by:	Doug Mitchell

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#ifndef _SSLCONTEXT_H_
#define _SSLCONTEXT_H_ 1

#include <Security/SecureTransport.h>
#include "sslBuildFlags.h"
#include <Security/cssmtype.h>

#include "sslPriv.h"
#include "tls_ssl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{   SSLReadFunc         read;
    SSLWriteFunc        write;
    SSLConnectionRef   	ioRef;
} IOContext;

/*
 * An element in a certificate chain. 
 */
typedef struct SSLCertificate
{   
	struct SSLCertificate   *next;
    SSLBuffer               derCert;
} SSLCertificate;

#include "cryptType.h"

/*
 * An SSLContext contains four of these - one for each of {read,write} and for
 * {current, pending}.
 */
struct CipherContext
{   
	 
	const HashHmacReference   	*macRef;			/* HMAC (TLS) or digest (SSL) */
    const SSLSymmetricCipher  	*symCipher;
	
	/* this is a context which is reused once per record */
    HashHmacContext				macCtx;
	
    /* 
     * symKey is obtained from the CSP at cspHand. Normally this 
     * cspHand is the same as ctx->cspHand; some day they might differ.
     * Code which deals with this struct doesn't ever have to 
     * attach or detach from cspHand - that's taken care of at the
     * SSLContext level.
     */
    CSSM_KEY_PTR				symKey;	
    CSSM_CSP_HANDLE				cspHand;
    CSSM_CC_HANDLE				ccHand;

	/* needed in CDSASymmInit */
	uint8						encrypting;
	
    sslUint64          			sequenceNum;
    uint8              			ready;

	/* in SSL2 mode, the macSecret is the same size as the
	 * cipher key - which is 24 bytes in the 3DES case. */
	uint8						macSecret[MAX_SYMKEY_SIZE];
};
/* typedef in cryptType.h */

#include "sslHandshake.h"

typedef struct WaitingRecord
{   struct WaitingRecord    *next;
    SSLBuffer				data;
    uint32                  sent;
} WaitingRecord;

typedef struct DNListElem
{   struct DNListElem   *next;
    SSLBuffer		    derDN;
} DNListElem;
 
struct SSLContext
{   
    IOContext           ioCtx;
    
	/* 
	 * Prior to successful protocol negotiation, negProtocolVersion
	 * is SSL_Version_Undetermined. Subsequent to successful
	 * negotiation, negProtocolVersion contains the actual over-the-wire
	 * protocol value. 
	 *
	 * The Boolean versionEnable flags are set by 
	 * SSLSetProtocolVersionEnabled or SSLSetProtocolVersion and
	 * remain invariant once negotiation has started. If there
	 * were a large number of these and/or we were adding new
	 * protocol versions on a regular basis, we'd probably want
	 * to implement these as a word of flags. For now, in the 
	 * real world, this is the most straightfoprward implementation. 
	 */
    SSLProtocolVersion  negProtocolVersion;	/* negotiated */
    SSLProtocolVersion  clientReqProtocol;	/* requested by client in hello msg */
	Boolean				versionSsl2Enable;
	Boolean				versionSsl3Enable;
	Boolean				versionTls1Enable;
    SSLProtocolSide     protocolSide;		
	
    const struct _SslTlsCallouts *sslTslCalls; /* selects between SSLv3 and TLSv1 */
	
    /* crypto state in CDSA-centric terms */
    
    CSSM_KEY_PTR		signingPrivKey;	/* our private signing key */
    CSSM_KEY_PTR		signingPubKey;	/* our public signing key */
    CSSM_CSP_HANDLE		signingKeyCsp;	/* associated DL/CSP */
	
    CSSM_KEY_PTR		encryptPrivKey;	/* our private encrypt key, for 
    									 * server-initiated key exchange */
    CSSM_KEY_PTR		encryptPubKey;	/* public version of above */
    CSSM_CSP_HANDLE		encryptKeyCsp;
	
    CSSM_KEY_PTR		peerPubKey;
    CSSM_CSP_HANDLE		peerPubKeyCsp;	/* may not be needed, we figure this
    									 * one out by trial&error, right? */
    									 
  	/* 
  	 * Various cert chains.
  	 * For all three, the root is the first in the chain. 
  	 */
    SSLCertificate      *localCert;
    SSLCertificate		*encryptCert;
    SSLCertificate      *peerCert;
    
	/* peer certs as SecTrustRef */
	SecTrustRef			peerSecTrust;
	
    /* 
     * trusted root certs; specific to this implementation, we'll store
     * them conveniently...these will be used as AnchorCerts in a TP
     * call. 
     */
    uint32				numTrustedCerts;
    CSSM_DATA_PTR		trustedCerts;
    
    /* for symmetric cipher and RNG */
    CSSM_CSP_HANDLE		cspHand;
    
    /* session-wide handles for Apple TP, CL */
    CSSM_TP_HANDLE		tpHand;
    CSSM_CL_HANDLE		clHand;
    
	#if		APPLE_DH
	SSLBuffer			dhParamsPrime;
	SSLBuffer			dhParamsGenerator;
	SSLBuffer			dhParamsEncoded;	/* prime + generator */
    SSLBuffer			dhPeerPublic;
    SSLBuffer 			dhExchangePublic;
	CSSM_KEY_PTR		dhPrivate;
	#endif	/* APPLE_DH */
        
	Boolean				allowExpiredCerts;
	Boolean				allowExpiredRoots;
	Boolean				enableCertVerify;
	
    SSLBuffer		    sessionID;
	
    SSLBuffer			peerID;
    SSLBuffer			resumableSession;
    
	char				*peerDomainName;
	UInt32				peerDomainNameLen;
	
    CipherContext       readCipher;
    CipherContext       writeCipher;
    CipherContext       readPending;
    CipherContext       writePending;
    
    uint16              selectedCipher;			/* currently selected */
    const SSLCipherSpec *selectedCipherSpec;	/* ditto */
    SSLCipherSpec		*validCipherSpecs;		/* context's valid specs */ 
    unsigned			numValidCipherSpecs;	/* size of validCipherSpecs */
    SSLHandshakeState   state;
    
	/* server-side only */
    SSLAuthenticate		clientAuth;				/* kNeverAuthenticate, etc. */
    Boolean				tryClientAuth;

	/* client and server */
	SSLClientCertificateState	clientCertState;
	
    DNListElem          *acceptableDNList;

    int                 certRequested;
    int                 certSent;
    int                 certReceived;
    int                 x509Requested;
    
    uint8               clientRandom[SSL_CLIENT_SRVR_RAND_SIZE];
    uint8               serverRandom[SSL_CLIENT_SRVR_RAND_SIZE];
    SSLBuffer   		preMasterSecret;
    uint8               masterSecret[SSL_MASTER_SECRET_SIZE];
    
	/* running digests of all handshake messages */
    SSLBuffer   		shaState, md5State;
    
    SSLBuffer		    fragmentedMessageCache;
    
    unsigned            ssl2ChallengeLength;
    unsigned			ssl2ConnectionIDLength;
    unsigned            sessionMatch;
    
	/* Record layer fields */
    SSLBuffer    		partialReadBuffer;
    uint32              amountRead;
    
	/* Transport layer fields */
    WaitingRecord       *recordWriteQueue;
    SSLBuffer			receivedDataBuffer;
    uint32              receivedDataPos;
    
    Boolean				allowAnyRoot;		// don't require known roots
	Boolean				sentFatalAlert;		// this session terminated by fatal alert
	Boolean				rsaBlindingEnable;
};

#ifdef __cplusplus
}
#endif

#endif /* _SSLCONTEXT_H_ */
