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
	File:		SecureTransport.h

	Contains:	Public API for Apple SSL 3.0 Implementation

	Written by:	Doug Mitchell

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#ifndef _SECURE_TRANSPORT_H_
#define _SECURE_TRANSPORT_H_

/*
 * Initial X port: no keychain storage of certs; no server mode, no
 * client-side authentication. 
 */
#define ST_KEYCHAIN_ENABLE			0
#define ST_SERVER_MODE_ENABLE		0
#define ST_CLIENT_AUTHENTICATION	0

/*
 * This file describes the public API for an implementation of the 
 * Secure Socket Layer, V. 3.0. This implementation is based on Netscape's
 * SSLRef 3.0, modified for Apple use. (Appropriate copyrights and
 * acknowledgements are found elsewhere, and in all files containing 
 * Netscape code.)
 *
 * As in SSLRef 3.0, there no transport layer dependencies in this library;
 * it can be used with sockets, Open Transport, etc. Applications using
 * this library provide callback functions which do the actual I/O
 * on underlying network connections. Applications are also responsible
 * for setting up raw network connections; the application passes in
 * an opaque reference to the underlying (connected) entity at the 
 * start of an SSL session.
 *
 * Some terminology:
 *
 * A "client" is the initiator of an SSL Session. The canonical example
 * of a client is a web browser, when it's talking to an https URL. 
 *
 * A "server" is an entity which accepts requests for SSL sessions made
 * by clients. E.g., a secure web server. 
 
 * An "SSL Session", or "session", is bounded by calls to SSLHandshake()
 * and SSLClose(). An "Active session" is in some state between these
 * two calls, inclusive.
 * 
 * An SSL Session Context, or SSLContextRef, is an opaque reference in this
 * library to the state associated with one session.  
 */ 
 
#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacTypes.h>
//#include <CoreServices/CoreServices.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFArray.h>
#include <Security/CipherSuite.h>

#if		ST_KEYCHAIN_ENABLE
#include <Keychain.h>
#endif	/* ST_KEYCHAIN_ENABLE */

#ifdef __cplusplus
extern "C" {
#endif

/***********************
 *** Common typedefs ***
 ***********************/
 
/* Opaque reference to an SSL session context */
struct                      SSLContext;
typedef struct SSLContext   *SSLContextRef;

/* Opaque reference to an I/O conection (socket, Endpoint, etc.) */
typedef const void *		SSLConnectionRef;

/* SSL Protocol version */
typedef enum {
	kSSLProtocolUnknown,		/* no protocol negotiated/specified */
	kSSLProtocol2,				/* SSL 2.0 only */
	kSSLProtocol3,				/* SSL 3.0 preferred, 2.0 OK if peer requires */
	kSSLProtocol3Only			/* use SSL 3.0 only, fail if peer tries to
								 * negotiate 2.0 */
} SSLProtocol;

/* State of an SSLSession */
typedef enum {
	kSSLIdle,					/* no I/O performed yet */
	kSSLHandshake,				/* SSL handshake in progress */
	kSSLConnected,				/* Handshake complete, ready for normal I/O */
	kSSLClosed,					/* connection closed normally */
	kSSLAborted					/* connection aborted */
} SSLSessionState;

/* 
 * R/W functions. The application using this library provides
 * these functions via SSLSetIOFuncs().
 *
 * Data's memory is allocated by caller; on entry to these two functions
 * the *length argument indicates both the size of the available data and the
 * requested byte count. Number of bytes actually transferred is returned in 
 * *length.
 *
 * The application may configure the underlying connection to operate
 * in a non-blocking manner; in such a case, a read operation may
 * well return SSLWouldBlockErr, indicating "I transferred less data than
 * you requested (maybe even zero bytes), nothing is wrong, except 
 * requested I/O hasn't completed". This will be returned back up to 
 * the application as a return from SSLRead(), SSLWrite(), SSLHandshake(),
 * etc. 
 */
typedef OSStatus 
(*SSLReadFunc) 				(SSLConnectionRef 	connection,
							 void 				*data, 			/* owned by 
							 									 * caller, data
							 									 * RETURNED */
							 UInt32 			*dataLength);	/* IN/OUT */ 
typedef OSStatus 
(*SSLWriteFunc) 			(SSLConnectionRef 	connection,
							 const void 		*data, 
							 UInt32 			*dataLength);	/* IN/OUT */ 


/*************************************************
 *** OSStatus values unique to SecureTransport ***
 *************************************************/
 
enum {
	errSSLProtocol				= -9800,	/* SSL protocol error */
	errSSLNegotiation			= -9801,	/* Cipher Suite negotiation failure */
	errSSLFatalAlert			= -9802,	/* Fatal alert */
	errSSLWouldBlock			= -9803,	/* I/O would block (not fatal) */
    errSSLSessionNotFound 		= -9804,	/* attempt to restore an unknown
    										 *    session */
    errSSLClosedGraceful 		= -9805,	/* connection closed gracefully */
    errSSLClosedAbort 			= -9806,	/* connection closed via error */
    errSSLXCertChainInvalid 	= -9807,	/* Invalid certificate chain */
    errSSLBadCert				= -9808,	/* bad certificate format */
	errSSLCrypto				= -9809,	/* underlying cryptographic error */
	errSSLInternal				= -9810,	/* Internal error */
	errSSLModuleAttach			= -9811,	/* module attach failure */
    errSSLUnknownRootCert		= -9812,	/* valid cert chain, untrusted root */
    errSSLNoRootCert			= -9813,	/* cert chain not verified by root */
	errSSLCertExpired			= -9814,	/* chain had an expired cert */
	errSSLCertNotYetValid		= -9815,	/* chain had a cert not yet valid */
	errSSLClosedNoNotify		= -9816,	/* server closed session with no 
											 *     notification */
	errSSLBufferOverflow		= -9817,	/* insufficient buffer provided */
	errSSLBadCipherSuite		= -9818,	/* bad SSLCipherSuite */
	errSSLLast					= -9849		/* end of range, to be deleted */
};


/******************
 *** Public API ***
 ******************/

/* 
 * Create a new session context.
 */
OSStatus
SSLNewContext				(Boolean 			isServer,
							 SSLContextRef 		*contextPtr);	/* RETURNED */

/*
 * Dispose of an SSLContextRef.
 */
OSStatus
SSLDisposeContext			(SSLContextRef		context);

/*
 * Determine the state of an SSL session.
 */
OSStatus 
SSLGetSessionState			(SSLContextRef		context,
							 SSLSessionState	*state);	/* RETURNED */
							 
							 
/********************************************************************
 *** Session context configuration, common to client and servers. ***
 ********************************************************************/
 
/* 
 * Specify functions which do the network I/O. Must be called prior
 * to SSLHandshake(); subsequently can not be called while a session is
 * active. 
 */
OSStatus 
SSLSetIOFuncs				(SSLContextRef		context, 
							 SSLReadFunc 		read,
							 SSLWriteFunc		write);
							 
/* 
 * Get/set SSL protocol version; optional. Default for client is is 
 * kSSLProtocolUnknown (which works with whatever the server prefers);
 * default for server side is kSSLProtocol3 (which prefers SSL3 but
 * works with SSL2-only clients). 
 *
 * SSLSetProtocolVersion can not be called when a session is active. 
 */
OSStatus 
SSLSetProtocolVersion		(SSLContextRef 		context,
							 SSLProtocol		version);

OSStatus 
SSLGetProtocolVersion		(SSLContextRef		context,
							 SSLProtocol		*protocol);		/* RETURNED */

#if		(ST_SERVER_MODE_ENABLE || ST_CLIENT_AUTHENTICATION)

/*
 * Specify this connection's certificate(s). This is mandatory for
 * server connections, optional for clients. Specifying a certificate
 * for a client enables SSL client-side authentication. The end-entity
 * cert is in certRef[0]. Specifying a root cert is optional; if it's
 * not specified, the root cert which verifies the cert chain specified
 * here must have been specified in SSLSetTrustedRootCertKC().
 *
 * The certRefs argument is a CFArray containing KCItemRefs. 
 *
 * Can only be called when no session is active. 
 *
 * SecureTransport assumes the following:
 *   
 *	-- The certRef references remains valid for the lifetime of the 
 *     session.
 *  -- The specified certRef[0] is capable of signing. 
 *  -- In order for a server connection to work with SSL3 protocol,
 *     the private key associated with certRef[0] must ALSO be
 *	   capable of decryption. This is a workaround for a known 
 * 	   Netscape bug.  
 */
OSStatus
SSLSetCertificate			(SSLContextRef		context,
							 CFArrayRef			certRefs);

#endif	/* (ST_SERVER_MODE_ENABLE || ST_CLIENT_AUTHENTICATION) */

#if		ST_KEYCHAIN_ENABLE

/*
 * Specify a Keychain containing trusted root certificates.
 * Optional; the Keychain's root certs either are appended to or 
 * replace the existing SSLContextRef's root certs, which are initialized
 * to a system-wide set of trusted roots at SSLContextAlloc().
 *
 * Can not be called while a session is active.
 */
OSStatus
SSLSetTrustedRootCertKC		(SSLContextRef		context,
							 KCRef				keyChainRef,
							 Boolean			deleteExisting);
							 
/*
 * Specify a Keychain (and access credentials for the keychain)
 * to which newly encountered root certs are attempted to be
 * added. This may or may not result in user interaction, depending
 * on the configuration of the keychain and of the specified
 * accesssCreds.
 *
 * Can not be called while a session is active and can only be
 * called a maximum of one time per SSLContextRef.
 */
OSStatus 
SSLSetNewRootKC				(SSLContextRef		context,
							 KCRef				keyChainRef,
							 void				*accessCreds);
	
#endif	/* ST_KEYCHAIN_ENABLE */

/*
 * Specify I/O connection - a socket, endpoint, etc., which is
 * managed by caller. On the client side, it's assumed that communication
 * has been established with the desired server on this connection.
 * On the server side, it's assumed that an incoming client request
 * has been established. 
 *
 * Must be called prior to SSLHandshake(); subsequently can only be
 * called when no session is active.
 */
OSStatus
SSLSetConnection			(SSLContextRef		context,
							 SSLConnectionRef	connection);

/*
 * Obtain the actual negotiated protocol version of the active
 * session, which may be different that the value specified in 
 * SSLSetProtocolVersion(). Returns kSSLProtocolUnknown if no 
 * SSL session is in progress.
 */
OSStatus 
SSLGetNegotiatedProtocolVersion		(SSLContextRef		context,
									 SSLProtocol		*protocol); /* RETURNED */

/*
 * Determine number and values of all of the SSLCipherSuites we support.
 * Caller allocates output buffer for SSLGetSupportedCiphers() and passes in
 * its size in *numCiphers. If supplied buffer is too small, errSSLBufferOverflow
 * will be returned. 
 */
OSStatus
SSLGetNumberSupportedCiphers (SSLContextRef			context,
							  UInt32				*numCiphers);
			
OSStatus
SSLGetSupportedCiphers		 (SSLContextRef			context,
							  SSLCipherSuite		*ciphers,		/* RETURNED */
							  UInt32				*numCiphers);	/* IN/OUT */

/*
 * Specify a (typlically) restricted set of SSLCipherSuites to be enabled by
 * the current SSLContext. Can only be called when no session is active. Default
 * set of enabled SSLCipherSuites is the same as the complete set of supported 
 * SSLCipherSuites as obtained by SSLGetSupportedCiphers().
 */
OSStatus 
SSLSetEnabledCiphers		(SSLContextRef			context,
							 const SSLCipherSuite	*ciphers,	
							 UInt32					numCiphers);
							 
/*
 * Determine number and values of all of the SSLCipherSuites currently enabled.
 * Caller allocates output buffer for SSLGetEnabledCiphers() and passes in
 * its size in *numCiphers. If supplied buffer is too small, errSSLBufferOverflow
 * will be returned. 
 */
OSStatus
SSLGetNumberEnabledCiphers 	(SSLContextRef			context,
							 UInt32					*numCiphers);
			
OSStatus
SSLGetEnabledCiphers		(SSLContextRef			context,
							 SSLCipherSuite			*ciphers,		/* RETURNED */
							 UInt32					*numCiphers);	/* IN/OUT */


/*
 * Specify the option of ignoring certificates' "expired" times. 
 * This is a common failure in the real SSL world. Default for 
 * this flag is false, meaning expired certs result in a
 * errSSLCertExpired error.
 */ 
OSStatus 
SSLSetAllowExpiredCerts		(SSLContextRef		context,
							 Boolean			allowExpired);
							 
/* 
 * Obtain the current value of an SSLContext's "allowExpiredCerts" flag. 
 */
OSStatus
SSLGetAllowExpiredCerts		(SSLContextRef		context,
							 Boolean			*allowExpired); /* RETURNED */

/*
 * Specify option of allowing for an unknown root cert, i.e., one which
 * this software can not verify as one of a list of known good root certs. 
 * Default for this flag is false, in which case one of the following two
 * errors may occur:
 *    -- The peer returns a cert chain with a root cert, and the chain 
 *       verifies to that root, but the root is not one of our trusted
 *       roots. This results in errSSLUnknownRootCert on handshake. 
 *    -- The peer returns a cert chain which does not contain a root cert,
 *       and we can't verify the chain to one of our trusted roots. This 
 *       results in errSSLNoRootCert on handshake.
 *
 * Both of these error conditions are ignored when the AllowAnyRoot flag is true,
 * allowing connection to a totally untrusted peer. 
 */
OSStatus 
SSLSetAllowAnyRoot			(SSLContextRef		context,
							 Boolean			anyRoot);

/* 
 * Obtain the current value of an SSLContext's "allow any root" flag. 
 */
OSStatus
SSLGetAllowAnyRoot			(SSLContextRef		context,
							 Boolean			*anyRoot); /* RETURNED */

/*
 * Request peer certificates. Valid anytime, subsequent to
 * a handshake attempt.
 *
 * The certs argument is a CFArray containing CFDataRefs, each
 * of which is one DER-encoded cert. The entire array is mallocd
 * by the SecureTransport library. The cert at the end of the 
 * returned array is the subject (end entity) cert; the root cert
 * (or the closest cert to it) is in index 0 of the returned array. 
 */	
OSStatus 
SSLGetPeerCertificates		(SSLContextRef 		context, 
							 CFArrayRef			*certs);	/* RETURNED */
							 								 
/*
 * Specify some data, opaque to this library, which is sufficient
 * to uniquely identify the peer of the current session. An example
 * would be IP address and port, stored in some caller-private manner.
 * To be optionally called prior to SSLHandshake for the current 
 * session. This is mandatory if this session is to be resumable. 
 */
OSStatus 
SSLSetPeerID				(SSLContextRef 		context, 
							 CFDataRef 			peerID);

/*
 * Obtain the SSLCipherSuite (e.g., SSL_RSA_WITH_DES_CBC_SHA) negotiated
 * for this session. Only valid when a session is active.
 */
OSStatus 
SSLGetNegotiatedCipher		(SSLContextRef 		context,
							 SSLCipherSuite 	*cipherSuite);


/********************************************************
 *** Session context configuration, server side only. ***
 ********************************************************/
				 
#if		ST_SERVER_MODE_ENABLE
/*
 * Specify this connection's encryption certificate(s). This is
 * used in one of the following cases:
 *
 *	-- The end-entity certificate specified in SSLSetCertificate() is 
 *	   not capable of encryption. (THIS REQUIREMENT IS OBSOLETE due
 *	   due a workaround for a Netscape bug.)
 *
 *  -- The end-entity certificate specified in SSLSetCertificate() 
 * 	   contains a key which is too large (i.e., too strong) for legal 
 *	   encryption in this session. In this case a weaker cert is 
 *     specified here and is used for server-initiated key exchange. 
 *
 *  -- Servers which establsh an SSL level 2 connection require
 *     encryption certs. (SSL2 does not perform signing and verification,
 *     only asymmetric encryption and decryption.)
 *
 *  The encryptionCertRef argument is a CFArray containing 
 *  KCItemRefs. 
 *
 * The following assumptions are made:
 *
 *	-- The encryptionCertRef references remains valid for the lifetime of the 
 *     connection.
 *  -- The specified encryptionCertRef[0] is capable of encryption. 
 *
 * Can only be called when no session is active. 
 *
 * Notes:
 * ------
 *
 * -- SSL servers which enforce the SSL3 spec to the letter will
 *    not accept encryption certs with key sizes larger than 512
 *    bits for exportable ciphers. Apps which wish to use encryption 
 *    certs with key sizes larger than 512 bits should disable the 
 *    use of exportable ciphers via the SSLSetExportEnable() call. 
 */
OSStatus
SSLSetEncryptionCertificate	(SSLContextRef		context,
							 CFArrayRef			certRefs);

/*
 * Specify requirements for client-side authentication.
 * Optional; Default is kNeverAuthenticate, unless SSLSetTrustedRootCertKC
 * has been called, in which case the default is kTryAuthenticate.
 *
 * Can only be called when no session is active.  
 */
typedef enum {
	kNeverAuthenticate,			/* skip client authentication */
	kAlwaysAuthenticate,		/* require it */
	kTryAuthenticate			/* try to authenticate, but not an error
								 * if client doesn't have a cert */
} SSLAuthenticate;

OSStatus
SSLSetClientSideAuthenticate 	(SSLContextRef		context,
								 SSLAuthenticate	auth);
		
#endif	/* ST_SERVER_MODE_ENABLE */

/*******************************
 ******** I/O Functions ********
 *******************************/
 
/*
 * Note: depending on the configuration of the underlying I/O 
 * connection, all SSL I/O functions can return SSLWouldBlockErr,
 * indicating "not complete, nothing is wrong, except required
 * I/O hasn't completed". Caller may need to repeat I/Os as necessary
 * if the underlying connection has been configured to behave in 
 * a non-blocking manner.
 */
  
/*
 * Perform the SSL handshake. On successful return, session is 
 * ready for normal secure application I/O via SSLWrite and SSLRead.
 *
 * Interesting error returns:
 *
 *	errSSLUnknownRootCert: Peer had a valid cert chain, but the root of 
 *		the chain is unknown. 
 *
 * 	errSSLNoRootCert: Peer had a cert chain which was not verifiable
 *		to a root cert. Handshake was aborted; peer's cert chain
 *		available via SSLGetPeerCertificates().
 *
 * 	errSSLCertExpired: Peer's cert chain had one or more expired certs.
 *
 *  errSSLXCertChainInvalid: Peer had an invalid cert chain (i.e.,
 *		signature verification within the chain failed, or no certs
 *		were found). 
 *
 *  In all of the above errors, the handshake was aborted; peer's 
 *  cert chain available via SSLGetPeerCertificates().
 *
 * A return value of errSSLWouldBlock indicates that SSLHandshake has to be called
 * again (and again and again until something else is returned).
 */ 	 
OSStatus 
SSLHandshake				(SSLContextRef		context);

/*
 * Normal application-level read/write. On both of these, a errSSLWouldBlock
 * return and a partially completed transfer - or even zero bytes transferred -
 * are NOT mutually exclusive. 
 */
OSStatus 
SSLWrite					(SSLContextRef		context,
							 const void *		data,
							 UInt32				dataLength,
							 UInt32 			*processed);	/* RETURNED */ 

/*
 * data is mallocd by caller; available size specified in
 * dataLength; actual number of bytes read returned in
 * *processed.
 */
OSStatus 
SSLRead						(SSLContextRef		context,
							 void *				data,			/* RETURNED */
							 UInt32				dataLength,
							 UInt32 			*processed);	/* RETURNED */ 
							 
/*
 * Terminate current SSL session. 
 */
OSStatus 
SSLClose					(SSLContextRef		context);

#ifdef __cplusplus
}
#endif

#endif /* _SECURE_TRANSPORT_H_ */
