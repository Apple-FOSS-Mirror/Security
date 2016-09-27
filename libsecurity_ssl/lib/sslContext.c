/*
 * Copyright (c) 1999-2001,2005-2012 Apple Inc. All Rights Reserved.
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
 */

/*
 * sslContext.c - SSLContext accessors
 */

#include "SecureTransport.h"

#include "SSLRecordInternal.h"
#include "SecureTransportPriv.h"
#include "appleSession.h"
#include "ssl.h"
#include "sslCipherSpecs.h"
#include "sslContext.h"
#include "sslCrypto.h"
#include "sslDebug.h"
#include "sslDigests.h"
#include "sslKeychain.h"
#include "sslMemory.h"
#include "sslUtils.h"

#include <AssertMacros.h>
#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFPreferences.h>
#include <Security/SecCertificate.h>
#include <Security/SecCertificatePriv.h>
#include <Security/SecTrust.h>
#include <Security/SecTrustSettingsPriv.h>
#include <Security/oidsalg.h>
#include "utilities/SecCFRelease.h"
#include <pthread.h>
#include <string.h>

#if TARGET_OS_IPHONE
#include <Security/SecCertificateInternal.h>
#else
#include <Security/oidsalg.h>
#include <Security/oidscert.h>
#include <Security/SecTrustSettingsPriv.h>
#endif


static void sslFreeDnList(
	SSLContext *ctx)
{
    DNListElem      *dn, *nextDN;

    dn = ctx->acceptableDNList;
    while (dn)
    {   
	SSLFreeBuffer(&dn->derDN);
        nextDN = dn->next;
        sslFree(dn);
        dn = nextDN;
    }
    ctx->acceptableDNList = NULL;
}


Boolean sslIsSessionActive(const SSLContext *ctx)
{
	assert(ctx != NULL);
	switch(ctx->state) {
		case SSL_HdskStateUninit:
		case SSL_HdskStateServerUninit:
		case SSL_HdskStateClientUninit:
		case SSL_HdskStateGracefulClose:
		case SSL_HdskStateErrorClose:
			return false;
		default:
			return true;
	}
}

/*
 * Minimum and maximum supported versions
 */
//#define MINIMUM_STREAM_VERSION  SSL_Version_2_0 /* Disabled */
#define MINIMUM_STREAM_VERSION  SSL_Version_3_0
#define MAXIMUM_STREAM_VERSION  TLS_Version_1_2
#define MINIMUM_DATAGRAM_VERSION  DTLS_Version_1_0

/* This should be changed when we start supporting DTLS_Version_1_x */
#define MAXIMUM_DATAGRAM_VERSION  DTLS_Version_1_0

#define SSL_ENABLE_ECDSA_SIGN_AUTH			0
#define SSL_ENABLE_RSA_FIXED_ECDH_AUTH		0
#define SSL_ENABLE_ECDSA_FIXED_ECDH_AUTH	0

#define DEFAULT_DTLS_TIMEOUT    1
#define DEFAULT_DTLS_MTU        1400
#define MIN_ALLOWED_DTLS_MTU    64      /* this ensure than there will be no integer
                                            underflow when calculating max write size */

static CFTypeID kSSLContextTypeID;
int kSplitDefaultValue;

static void _sslContextDestroy(CFTypeRef arg);
static Boolean _sslContextEqual(CFTypeRef a, CFTypeRef b);
static CFHashCode _sslContextHash(CFTypeRef arg);
static CFStringRef _sslContextDescribe(CFTypeRef arg);

static void _SSLContextReadDefault()
{
	/* 0 = disabled, 1 = split every write, 2 = split second and subsequent writes */
    /* Enabled by default, this make cause some interop issues, see <rdar://problem/12307662> and <rdar://problem/12323307> */
    const int defaultSplitDefaultValue = 2;

	CFTypeRef value = (CFTypeRef)CFPreferencesCopyValue(CFSTR("SSLWriteSplit"),
							CFSTR("com.apple.security"),
							kCFPreferencesAnyUser,
							kCFPreferencesAnyHost);
	if (value) {
		if (CFGetTypeID(value) == CFBooleanGetTypeID())
			kSplitDefaultValue = CFBooleanGetValue((CFBooleanRef)value) ? 1 : 0;
		else if (CFGetTypeID(value) == CFNumberGetTypeID()) {
			if (!CFNumberGetValue((CFNumberRef)value, kCFNumberIntType, &kSplitDefaultValue))
				kSplitDefaultValue = defaultSplitDefaultValue;
		}
		if (kSplitDefaultValue < 0 || kSplitDefaultValue > 2) {
			kSplitDefaultValue = defaultSplitDefaultValue;
		}
		CFRelease(value);
	}
	else {
		kSplitDefaultValue = defaultSplitDefaultValue;
	}
}

static void _SSLContextRegisterClass()
{
	static const CFRuntimeClass kSSLContextRegisterClass = {
		0,												/* version */
        "SSLContext",					     			/* class name */
		NULL,											/* init */
		NULL,											/* copy */
		_sslContextDestroy,     	                    /* dealloc */
		_sslContextEqual,							    /* equal */
		_sslContextHash,								/* hash */
		NULL,											/* copyFormattingDesc */
		_sslContextDescribe		                        /* copyDebugDesc */
	};

    kSSLContextTypeID = _CFRuntimeRegisterClass(&kSSLContextRegisterClass);
}

CFTypeID
SSLContextGetTypeID(void)
{
	static pthread_once_t sOnce = PTHREAD_ONCE_INIT;
	pthread_once(&sOnce, _SSLContextRegisterClass);
	return kSSLContextTypeID;
}


OSStatus
SSLNewContext				(Boolean 			isServer,
							 SSLContextRef 		*contextPtr)	/* RETURNED */
{
	if(contextPtr == NULL) {
		return errSecParam;
	}

	*contextPtr = SSLCreateContext(kCFAllocatorDefault, isServer?kSSLServerSide:kSSLClientSide, kSSLStreamType);

	if (*contextPtr == NULL)
		return errSecAllocate;

	return errSecSuccess;
}

SSLContextRef SSLCreateContext(CFAllocatorRef alloc, SSLProtocolSide protocolSide, SSLConnectionType connectionType)
{
    SSLContextRef ctx;
    
    ctx = SSLCreateContextWithRecordFuncs(alloc, protocolSide, connectionType, &SSLRecordLayerInternal);
    
    if(ctx==NULL)
        return NULL;
    
    ctx->recCtx = SSLCreateInternalRecordLayer(connectionType);
    if(ctx->recCtx==NULL) {
    	CFRelease(ctx);
		return NULL;
    }
    
    return ctx;
}

SSLContextRef SSLCreateContextWithRecordFuncs(CFAllocatorRef alloc, SSLProtocolSide protocolSide, SSLConnectionType connectionType, const struct SSLRecordFuncs *recFuncs)
{
	OSStatus	serr = errSecSuccess;
	SSLContext *ctx = (SSLContext*) _CFRuntimeCreateInstance(alloc, SSLContextGetTypeID(), sizeof(SSLContext) - sizeof(CFRuntimeBase), NULL);

	if(ctx == NULL) {
		return NULL;
	}

	/* subsequent errors to errOut: */
    memset(((uint8_t*) ctx) + sizeof(CFRuntimeBase), 0, sizeof(SSLContext) - sizeof(CFRuntimeBase));

    ctx->state = SSL_HdskStateUninit;
    ctx->timeout_duration = DEFAULT_DTLS_TIMEOUT;
    ctx->clientCertState = kSSLClientCertNone;

    ctx->minProtocolVersion = MINIMUM_STREAM_VERSION;
    ctx->maxProtocolVersion = MAXIMUM_STREAM_VERSION;

    ctx->isDTLS = false;
    ctx->dtlsCookie.data = NULL;
    ctx->dtlsCookie.length = 0;
    ctx->hdskMessageSeq = 0;
    ctx->hdskMessageSeqNext = 0;
    ctx->mtu = DEFAULT_DTLS_MTU;

	ctx->negProtocolVersion = SSL_Version_Undetermined;


    ctx->protocolSide = protocolSide;
	/* Default value so we can send and receive hello msgs */
	ctx->sslTslCalls = &Ssl3Callouts;

    ctx->recFuncs = recFuncs;

    /* Initialize the cipher state to NULL_WITH_NULL_NULL */
    ctx->selectedCipher        = TLS_NULL_WITH_NULL_NULL;
    InitCipherSpecParams(ctx);

    /* this gets init'd on first call to SSLHandshake() */
    ctx->validCipherSuites = NULL;
    ctx->numValidCipherSuites = 0;
#if ENABLE_SSLV2
    ctx->numValidNonSSLv2Suites = 0;
#endif

	ctx->peerDomainName = NULL;
	ctx->peerDomainNameLen = 0;

#ifdef USE_CDSA_CRYPTO
	/* attach to CSP, CL, TP */
	serr = attachToAll(ctx);
	if(serr) {
		goto errOut;
	}
#endif /* USE_CDSA_CRYPTO */

	/* Initial cert verify state: verify with default system roots */
	ctx->enableCertVerify = true;

	/* Default for RSA blinding is ENABLED */
	ctx->rsaBlindingEnable = true;

	/* Default for sending one-byte app data record is DISABLED */
	ctx->oneByteRecordEnable = false;

	/* Consult global system preference for default behavior:
	 * 0 = disabled, 1 = split every write, 2 = split second and subsequent writes
	 * (caller can override by setting kSSLSessionOptionSendOneByteRecord)
	 */
	static pthread_once_t sReadDefault = PTHREAD_ONCE_INIT;
	pthread_once(&sReadDefault, _SSLContextReadDefault);
	if (kSplitDefaultValue > 0)
		ctx->oneByteRecordEnable = true;

	/* default for anonymous ciphers is DISABLED */
	ctx->anonCipherEnable = false;

    ctx->breakOnServerAuth = false;
    ctx->breakOnCertRequest = false;
    ctx->breakOnClientAuth = false;
    ctx->signalServerAuth = false;
    ctx->signalCertRequest = false;
    ctx->signalClientAuth = false;

	/*
	 * Initial/default set of ECDH curves
	 */
	ctx->ecdhNumCurves = SSL_ECDSA_NUM_CURVES;
	ctx->ecdhCurves[0] = SSL_Curve_secp256r1;
	ctx->ecdhCurves[1] = SSL_Curve_secp384r1;
	ctx->ecdhCurves[2] = SSL_Curve_secp521r1;

	ctx->ecdhPeerCurve = SSL_Curve_None;		/* until we negotiate one */
	ctx->negAuthType = SSLClientAuthNone;		/* ditto */

    ctx->messageWriteQueue = NULL;

    if(connectionType==kSSLDatagramType) {
        ctx->minProtocolVersion = MINIMUM_DATAGRAM_VERSION;
        ctx->maxProtocolVersion = MAXIMUM_DATAGRAM_VERSION;
        ctx->isDTLS = true;
	}

    ctx->secure_renegotiation = false;

	if (serr != errSecSuccess) {
		CFRelease(ctx);
		ctx = NULL;
    }
	return ctx;
}

OSStatus
SSLNewDatagramContext       (Boolean 			isServer,
							 SSLContextRef 		*contextPtr)	/* RETURNED */
{
	if (contextPtr == NULL)
		return errSecParam;
	*contextPtr = SSLCreateContext(kCFAllocatorDefault, isServer?kSSLServerSide:kSSLClientSide, kSSLDatagramType);
	if (*contextPtr == NULL)
		return errSecAllocate;
    return errSecSuccess;
}

/*
 * Dispose of an SSLContext. (private)
 * This function is invoked after our dispatch queue is safely released,
 * or directly from SSLDisposeContext if there is no dispatch queue.
 */
OSStatus
SSLDisposeContext				(SSLContextRef context)
{
    if(context == NULL) {
        return errSecParam;
    }
	CFRelease(context);
	return errSecSuccess;
}

CF_RETURNS_RETAINED CFStringRef _sslContextDescribe(CFTypeRef arg)
{
    SSLContext* ctx = (SSLContext*) arg;

    if (ctx == NULL) {
        return NULL;
    } else {
		CFStringRef result = CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<SSLContext(%p) { ... }>"), ctx);
		return result;
    }
}

Boolean _sslContextEqual(CFTypeRef a, CFTypeRef b)
{
	return a == b;
}

CFHashCode _sslContextHash(CFTypeRef arg)
{
	return (CFHashCode) arg;
}

void _sslContextDestroy(CFTypeRef arg)
{
	SSLContext* ctx = (SSLContext*) arg;

#if USE_SSLCERTIFICATE
	sslDeleteCertificateChain(ctx->localCert, ctx);
    sslDeleteCertificateChain(ctx->encryptCert, ctx);
    sslDeleteCertificateChain(ctx->peerCert, ctx);
    ctx->localCert = ctx->encryptCert = ctx->peerCert = NULL;
#else
	CFReleaseNull(ctx->localCert);
	CFReleaseNull(ctx->encryptCert);
	CFReleaseNull(ctx->peerCert);
	CFReleaseNull(ctx->trustedCerts);
#endif

    /* Free the last handshake message flight */
    SSLResetFlight(ctx);

    if(ctx->peerSecTrust) {
		CFRelease(ctx->peerSecTrust);
		ctx->peerSecTrust = NULL;
	}
    SSLFreeBuffer(&ctx->sessionTicket);
    
	#if APPLE_DH
    SSLFreeBuffer(&ctx->dhParamsEncoded);
#ifdef USE_CDSA_CRYPTO
	sslFreeKey(ctx->cspHand, &ctx->dhPrivate, NULL);
#else
    if (ctx->secDHContext)
        SecDHDestroy(ctx->secDHContext);
#endif /* !USE_CDSA_CRYPTO */
    SSLFreeBuffer(&ctx->dhPeerPublic);
    SSLFreeBuffer(&ctx->dhExchangePublic);
    #endif	/* APPLE_DH */

    SSLFreeBuffer(&ctx->ecdhPeerPublic);
    SSLFreeBuffer(&ctx->ecdhExchangePublic);
#if USE_CDSA_CRYPTO
	if(ctx->ecdhPrivCspHand == ctx->cspHand) {
		sslFreeKey(ctx->ecdhPrivCspHand, &ctx->ecdhPrivate, NULL);
	}
	/* else we got this key from a SecKeyRef, no free needed */
#endif

    /* Only destroy if we were using the internal record layer */
    if(ctx->recFuncs==&SSLRecordLayerInternal)
        SSLDestroyInternalRecordLayer(ctx->recCtx);

	CloseHash(&SSLHashSHA1, &ctx->shaState);
	CloseHash(&SSLHashMD5,  &ctx->md5State);
	CloseHash(&SSLHashSHA256,  &ctx->sha256State);
	CloseHash(&SSLHashSHA384,  &ctx->sha512State);

    SSLFreeBuffer(&ctx->sessionID);
    SSLFreeBuffer(&ctx->peerID);
    SSLFreeBuffer(&ctx->resumableSession);
    SSLFreeBuffer(&ctx->preMasterSecret);
    SSLFreeBuffer(&ctx->fragmentedMessageCache);
    SSLFreeBuffer(&ctx->receivedDataBuffer);

	if(ctx->peerDomainName) {
		sslFree(ctx->peerDomainName);
		ctx->peerDomainName = NULL;
		ctx->peerDomainNameLen = 0;
	}

	sslFree(ctx->validCipherSuites);
	ctx->validCipherSuites = NULL;
	ctx->numValidCipherSuites = 0;

#if USE_CDSA_CRYPTO
	/*
	 * NOTE: currently, all public keys come from the CL via CSSM_CL_CertGetKeyInfo.
	 * We really don't know what CSP the CL used to generate a public key (in fact,
	 * it uses the raw CSP only to get LogicalKeySizeInBits, but we can't know
	 * that). Thus using e.g. signingKeyCsp (or any other CSP) to free
	 * signingPubKey is not tecnically accurate. However, our public keys
	 * are all raw keys, and all Apple CSPs dispose of raw keys in the same
	 * way.
	 */
	sslFreeKey(ctx->cspHand, &ctx->signingPubKey, NULL);
	sslFreeKey(ctx->cspHand, &ctx->encryptPubKey, NULL);
	sslFreeKey(ctx->peerPubKeyCsp, &ctx->peerPubKey, NULL);

	if(ctx->signingPrivKeyRef) {
		CFRelease(ctx->signingPrivKeyRef);
	}
	if(ctx->encryptPrivKeyRef) {
		CFRelease(ctx->encryptPrivKeyRef);
	}
	if(ctx->trustedCerts) {
		CFRelease(ctx->trustedCerts);
	}
	detachFromAll(ctx);
#else
	sslFreePubKey(&ctx->signingPubKey);
	sslFreePubKey(&ctx->encryptPubKey);
	sslFreePubKey(&ctx->peerPubKey);
	sslFreePrivKey(&ctx->signingPrivKeyRef);
	sslFreePrivKey(&ctx->encryptPrivKeyRef);
#endif /* USE_CDSA_CRYPTO */
    CFReleaseSafe(ctx->acceptableCAs);
    CFReleaseSafe(ctx->trustedLeafCerts);
	CFReleaseSafe(ctx->localCertArray);
	CFReleaseSafe(ctx->encryptCertArray);
    CFReleaseSafe(ctx->encryptCertArray);
	if(ctx->clientAuthTypes) {
		sslFree(ctx->clientAuthTypes);
	}
    if(ctx->serverSigAlgs != NULL) {
        sslFree(ctx->serverSigAlgs);
    }
    if(ctx->clientSigAlgs != NULL) {
        sslFree(ctx->clientSigAlgs);
    }
	sslFreeDnList(ctx);

    SSLFreeBuffer(&ctx->ownVerifyData);
    SSLFreeBuffer(&ctx->peerVerifyData);

    SSLFreeBuffer(&ctx->pskIdentity);
    SSLFreeBuffer(&ctx->pskSharedSecret);

    memset(((uint8_t*) ctx) + sizeof(CFRuntimeBase), 0, sizeof(SSLContext) - sizeof(CFRuntimeBase));

	sslCleanupSession();
}

/*
 * Determine the state of an SSL session.
 */
OSStatus
SSLGetSessionState			(SSLContextRef		context,
							 SSLSessionState	*state)		/* RETURNED */
{
	SSLSessionState rtnState = kSSLIdle;

	if(context == NULL) {
		return errSecParam;
	}
	*state = rtnState;
	switch(context->state) {
		case SSL_HdskStateUninit:
		case SSL_HdskStateServerUninit:
		case SSL_HdskStateClientUninit:
			rtnState = kSSLIdle;
			break;
		case SSL_HdskStateGracefulClose:
			rtnState = kSSLClosed;
			break;
		case SSL_HdskStateErrorClose:
		case SSL_HdskStateNoNotifyClose:
			rtnState = kSSLAborted;
			break;
		case SSL_HdskStateServerReady:
		case SSL_HdskStateClientReady:
			rtnState = kSSLConnected;
			break;
		default:
			assert((context->state >= SSL_HdskStateServerHello) &&
			        (context->state <= SSL_HdskStateFinished));
			rtnState = kSSLHandshake;
			break;

	}
	*state = rtnState;
	return errSecSuccess;
}

/*
 * Set options for an SSL session.
 */
OSStatus
SSLSetSessionOption			(SSLContextRef		context,
							 SSLSessionOption	option,
							 Boolean			value)
{
    if(context == NULL) {
	return errSecParam;
    }
    if(sslIsSessionActive(context)) {
	/* can't do this with an active session */
	return errSecBadReq;
    }
    switch(option) {
        case kSSLSessionOptionBreakOnServerAuth:
            context->breakOnServerAuth = value;
            context->enableCertVerify = !value;
            break;
        case kSSLSessionOptionBreakOnCertRequested:
            context->breakOnCertRequest = value;
            break;
        case kSSLSessionOptionBreakOnClientAuth:
            context->breakOnClientAuth = value;
            context->enableCertVerify = !value;
            break;
        case kSSLSessionOptionSendOneByteRecord:
            context->oneByteRecordEnable = value;
            break;
        case kSSLSessionOptionFalseStart:
            context->falseStartEnabled = value;
            break;
        default: 
            return errSecParam;
    }

    return errSecSuccess;
}

/*
 * Determine current value for the specified option in an SSL session.
 */
OSStatus
SSLGetSessionOption			(SSLContextRef		context,
							 SSLSessionOption	option,
							 Boolean			*value)
{
    if(context == NULL || value == NULL) {
        return errSecParam;
    }
    switch(option) {
        case kSSLSessionOptionBreakOnServerAuth:
            *value = context->breakOnServerAuth;
            break;
        case kSSLSessionOptionBreakOnCertRequested:
            *value = context->breakOnCertRequest;
            break;
        case kSSLSessionOptionBreakOnClientAuth:
            *value = context->breakOnClientAuth;
            break;
        case kSSLSessionOptionSendOneByteRecord:
            *value = context->oneByteRecordEnable;
            break;
        case kSSLSessionOptionFalseStart:
            *value = context->falseStartEnabled;
            break;
        default:
            return errSecParam;
    }

    return errSecSuccess;
}

OSStatus
SSLSetRecordContext         (SSLContextRef          ctx,
                             SSLRecordContextRef    recCtx)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
    ctx->recCtx = recCtx;
    return errSecSuccess;
}

/* Those two trampolines are used to make the connetion between
   the record layer IO callbacks and the user provided IO callbacks.
   Those are currently necessary because the record layer read/write callbacks
   have different prototypes that the user callbacks advertised in the API.
   They have different prototypes because the record layer callback have to build in kernelland.

   This situation is not desirable. So we should figure out a way to get rid of them.
 */
static int IORead(SSLIOConnectionRef 	connection,
                  void 				*data,
                  size_t 			*dataLength)
{
    OSStatus rc;
    SSLContextRef ctx = connection;


    rc = ctx->ioCtx.read(ctx->ioCtx.ioRef, data, dataLength);

    /* We may need to translate error codes at this layer */
    if(rc==errSSLWouldBlock) {
        rc=errSSLRecordWouldBlock;
    }

    return rc;
}

static int IOWrite(SSLIOConnectionRef 	connection,
                   const void 		*data,
                   size_t 			*dataLength)
{
    OSStatus rc;
    SSLContextRef ctx = connection;

    rc = ctx->ioCtx.write(ctx->ioCtx.ioRef, data, dataLength);

    /* We may need to translate error codes at this layer */
    if(rc==errSSLWouldBlock) {
        rc=errSSLRecordWouldBlock;
    }
    return rc;
}


OSStatus 
SSLSetIOFuncs				(SSLContextRef		ctx, 
							 SSLReadFunc 		readFunc,
							 SSLWriteFunc		writeFunc)
{
	if(ctx == NULL) {
		return errSecParam;
	}
    if(ctx->recFuncs!=&SSLRecordLayerInternal) {
        /* Can Only do this with the internal record layer */
        check(0);
        return errSecBadReq;
    }
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}

    ctx->ioCtx.read=readFunc;
    ctx->ioCtx.write=writeFunc;

    return SSLSetInternalRecordLayerIOFuncs(ctx->recCtx, IORead, IOWrite);
}

OSStatus
SSLSetConnection			(SSLContextRef		ctx,
							 SSLConnectionRef	connection)
{
	if(ctx == NULL) {
		return errSecParam;
	}
    if(ctx->recFuncs!=&SSLRecordLayerInternal) {
        /* Can Only do this with the internal record layer */
        check(0);
        return errSecBadReq;
    }
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}

    /* Need to keep a copy of it this layer for the Get function */
    ctx->ioCtx.ioRef = connection;

    return SSLSetInternalRecordLayerConnection(ctx->recCtx, ctx);
}

OSStatus
SSLGetConnection			(SSLContextRef		ctx,
							 SSLConnectionRef	*connection)
{
	if((ctx == NULL) || (connection == NULL)) {
		return errSecParam;
	}
	*connection = ctx->ioCtx.ioRef;
	return errSecSuccess;
}

OSStatus
SSLSetPeerDomainName		(SSLContextRef		ctx,
							 const char			*peerName,
							 size_t				peerNameLen)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}

	/* free possible existing name */
	if(ctx->peerDomainName) {
		sslFree(ctx->peerDomainName);
	}

	/* copy in */
	ctx->peerDomainName = (char *)sslMalloc(peerNameLen);
	if(ctx->peerDomainName == NULL) {
		return errSecAllocate;
	}
	memmove(ctx->peerDomainName, peerName, peerNameLen);
	ctx->peerDomainNameLen = peerNameLen;
	return errSecSuccess;
}

/*
 * Determine the buffer size needed for SSLGetPeerDomainName().
 */
OSStatus
SSLGetPeerDomainNameLength	(SSLContextRef		ctx,
							 size_t				*peerNameLen)	// RETURNED
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*peerNameLen = ctx->peerDomainNameLen;
	return errSecSuccess;
}

OSStatus
SSLGetPeerDomainName		(SSLContextRef		ctx,
							 char				*peerName,		// returned here
							 size_t				*peerNameLen)	// IN/OUT
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(*peerNameLen < ctx->peerDomainNameLen) {
		return errSSLBufferOverflow;
	}
	memmove(peerName, ctx->peerDomainName, ctx->peerDomainNameLen);
	*peerNameLen = ctx->peerDomainNameLen;
	return errSecSuccess;
}

OSStatus
SSLSetDatagramHelloCookie   (SSLContextRef	ctx,
                             const void         *cookie,
                             size_t             cookieLen)
{
    OSStatus err;

    if(ctx == NULL) {
		return errSecParam;
	}

    if(!ctx->isDTLS) return errSecParam;

	if((ctx == NULL) || (cookieLen>32)) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}

	/* free possible existing cookie */
	if(ctx->dtlsCookie.data) {
        SSLFreeBuffer(&ctx->dtlsCookie);
	}

	/* copy in */
    if((err=SSLAllocBuffer(&ctx->dtlsCookie, cookieLen)))
       return err;

	memmove(ctx->dtlsCookie.data, cookie, cookieLen);
    return errSecSuccess;
}

OSStatus
SSLSetMaxDatagramRecordSize (SSLContextRef		ctx,
                             size_t             maxSize)
{

    if(ctx == NULL) return errSecParam;
    if(!ctx->isDTLS) return errSecParam;
    if(maxSize < MIN_ALLOWED_DTLS_MTU) return errSecParam;

    ctx->mtu = maxSize;

    return errSecSuccess;
}

OSStatus
SSLGetMaxDatagramRecordSize (SSLContextRef		ctx,
                             size_t             *maxSize)
{
    if(ctx == NULL) return errSecParam;
    if(!ctx->isDTLS) return errSecParam;

    *maxSize = ctx->mtu;

    return errSecSuccess;
}

/*

 Keys to to math below:

 A DTLS record looks like this: | header (13 bytes) | fragment |

 For Null cipher, fragment is clear text as follows:
 | Contents | Mac |

 For block cipher, fragment size must be a multiple of the cipher block size, and is the
 encryption of the following plaintext :
 | IV (1 block) | content | MAC | padding (0 to 255 bytes) | Padlen (1 byte) |

 The maximum content length in that case is achieved for 0 padding bytes.

*/

OSStatus
SSLGetDatagramWriteSize		(SSLContextRef ctx,
							 size_t *bufSize)
{
    if(ctx == NULL) return errSecParam;
    if(!ctx->isDTLS) return errSecParam;
    if(bufSize == NULL) return errSecParam;

    size_t max_fragment_size = ctx->mtu-13; /* 13 = dtls record header */

    SSLCipherSpecParams *currCipher = &ctx->selectedCipherSpecParams;

    size_t blockSize = currCipher->blockSize;
    size_t macSize = currCipher->macSize;

    if (blockSize > 0) {
        /* max_fragment_size must be a multiple of blocksize */
        max_fragment_size = max_fragment_size & ~(blockSize-1);
        max_fragment_size -= blockSize; /* 1 block for IV */
        max_fragment_size -= 1; /* 1 byte for pad length */
    }

    /* less the mac size */
    max_fragment_size -= macSize;

    /* Thats just a sanity check */
    assert(max_fragment_size<ctx->mtu);

    *bufSize = max_fragment_size;

    return errSecSuccess;
}

static SSLProtocolVersion SSLProtocolToProtocolVersion(SSLProtocol protocol) {
    switch (protocol) {
        case kSSLProtocol2:             return SSL_Version_2_0;
        case kSSLProtocol3:             return SSL_Version_3_0;
        case kTLSProtocol1:             return TLS_Version_1_0;
        case kTLSProtocol11:            return TLS_Version_1_1;
        case kTLSProtocol12:            return TLS_Version_1_2;
        case kDTLSProtocol1:            return DTLS_Version_1_0;
        default:                        return SSL_Version_Undetermined;
    }
}

/* concert between private SSLProtocolVersion and public SSLProtocol */
static SSLProtocol SSLProtocolVersionToProtocol(SSLProtocolVersion version)
{
	switch(version) {
		case SSL_Version_2_0:           return kSSLProtocol2;
		case SSL_Version_3_0:           return kSSLProtocol3;
		case TLS_Version_1_0:           return kTLSProtocol1;
		case TLS_Version_1_1:           return kTLSProtocol11;
		case TLS_Version_1_2:           return kTLSProtocol12;
		case DTLS_Version_1_0:          return kDTLSProtocol1;
		default:
			sslErrorLog("SSLProtocolVersionToProtocol: bad prot (%04x)\n",
                        version);
            /* DROPTHROUGH */
		case SSL_Version_Undetermined:  return kSSLProtocolUnknown;
	}
}

OSStatus
SSLSetProtocolVersionMin  (SSLContextRef      ctx,
                           SSLProtocol        minVersion)
{
    if(ctx == NULL) return errSecParam;

    SSLProtocolVersion version = SSLProtocolToProtocolVersion(minVersion);
    if (ctx->isDTLS) {
        if (version > MINIMUM_DATAGRAM_VERSION ||
            version < MAXIMUM_DATAGRAM_VERSION)
            return errSSLIllegalParam;
        if (version < ctx->maxProtocolVersion)
            ctx->maxProtocolVersion = version;
    } else {
        if (version < MINIMUM_STREAM_VERSION || version > MAXIMUM_STREAM_VERSION)
            return errSSLIllegalParam;
        if (version > ctx->maxProtocolVersion)
            ctx->maxProtocolVersion = version;
    }
    ctx->minProtocolVersion = version;

    return errSecSuccess;
}

OSStatus
SSLGetProtocolVersionMin  (SSLContextRef      ctx,
                           SSLProtocol        *minVersion)
{
    if(ctx == NULL) return errSecParam;

    *minVersion = SSLProtocolVersionToProtocol(ctx->minProtocolVersion);
    return errSecSuccess;
}

OSStatus
SSLSetProtocolVersionMax  (SSLContextRef      ctx,
                           SSLProtocol        maxVersion)
{
    if(ctx == NULL) return errSecParam;

    SSLProtocolVersion version = SSLProtocolToProtocolVersion(maxVersion);
    if (ctx->isDTLS) {
        if (version > MINIMUM_DATAGRAM_VERSION ||
            version < MAXIMUM_DATAGRAM_VERSION)
            return errSSLIllegalParam;
        if (version > ctx->minProtocolVersion)
            ctx->minProtocolVersion = version;
    } else {
        if (version < MINIMUM_STREAM_VERSION || version > MAXIMUM_STREAM_VERSION)
            return errSSLIllegalParam;
        if (version < ctx->minProtocolVersion)
            ctx->minProtocolVersion = version;
    }
    ctx->maxProtocolVersion = version;

    return errSecSuccess;
}

OSStatus
SSLGetProtocolVersionMax  (SSLContextRef      ctx,
                           SSLProtocol        *maxVersion)
{
    if(ctx == NULL) return errSecParam;

    *maxVersion = SSLProtocolVersionToProtocol(ctx->maxProtocolVersion);
    return errSecSuccess;
}

#define max(x,y) ((x)<(y)?(y):(x))

OSStatus
SSLSetProtocolVersionEnabled(SSLContextRef     ctx,
							 SSLProtocol		protocol,
							 Boolean			enable)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx) || ctx->isDTLS) {
		/* Can't do this with an active session, nor with a DTLS session */
		return errSecBadReq;
	}
    if (protocol == kSSLProtocolAll) {
        if (enable) {
            ctx->minProtocolVersion = MINIMUM_STREAM_VERSION;
            ctx->maxProtocolVersion = MAXIMUM_STREAM_VERSION;
        } else {
            ctx->minProtocolVersion = SSL_Version_Undetermined;
            ctx->maxProtocolVersion = SSL_Version_Undetermined;
        }
	} else {
		SSLProtocolVersion version = SSLProtocolToProtocolVersion(protocol);
        if (enable) {
			if (version < MINIMUM_STREAM_VERSION || version > MAXIMUM_STREAM_VERSION) {
				return errSecParam;
			}
            if (version > ctx->maxProtocolVersion) {
                ctx->maxProtocolVersion = version;
                if (ctx->minProtocolVersion == SSL_Version_Undetermined)
                    ctx->minProtocolVersion = version;
            }
            if (version < ctx->minProtocolVersion) {
                ctx->minProtocolVersion = version;
            }
        } else {
			if (version < SSL_Version_2_0 || version > MAXIMUM_STREAM_VERSION) {
				return errSecParam;
			}
			/* Disabling a protocol version now resets the minimum acceptable
			 * version to the next higher version. This means it's no longer
			 * possible to enable a discontiguous set of protocol versions.
			 */
			SSLProtocolVersion nextVersion;
			switch (version) {
				case SSL_Version_2_0:
					nextVersion = SSL_Version_3_0;
					break;
				case SSL_Version_3_0:
					nextVersion = TLS_Version_1_0;
					break;
				case TLS_Version_1_0:
					nextVersion = TLS_Version_1_1;
					break;
				case TLS_Version_1_1:
					nextVersion = TLS_Version_1_2;
					break;
				case TLS_Version_1_2:
				default:
					nextVersion = SSL_Version_Undetermined;
					break;
			}
			ctx->minProtocolVersion = max(ctx->minProtocolVersion, nextVersion);
			if (ctx->minProtocolVersion > ctx->maxProtocolVersion) {
				ctx->minProtocolVersion = SSL_Version_Undetermined;
				ctx->maxProtocolVersion = SSL_Version_Undetermined;
			}
        }
    }

	return errSecSuccess;
}

OSStatus
SSLGetProtocolVersionEnabled(SSLContextRef 		ctx,
							 SSLProtocol		protocol,
							 Boolean			*enable)		/* RETURNED */
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(ctx->isDTLS) {
		/* Can't do this with a DTLS session */
		return errSecBadReq;
	}
	switch(protocol) {
		case kSSLProtocol2:
		case kSSLProtocol3:
		case kTLSProtocol1:
        case kTLSProtocol11:
        case kTLSProtocol12:
        {
            SSLProtocolVersion version = SSLProtocolToProtocolVersion(protocol);
			*enable = (ctx->minProtocolVersion <= version
                       && ctx->maxProtocolVersion >= version);
			break;
        }
		case kSSLProtocolAll:
            *enable = (ctx->minProtocolVersion <= MINIMUM_STREAM_VERSION
                       && ctx->maxProtocolVersion >= MAXIMUM_STREAM_VERSION);
			break;
		default:
			return errSecParam;
	}
	return errSecSuccess;
}

/* deprecated */
OSStatus
SSLSetProtocolVersion		(SSLContextRef 		ctx,
							 SSLProtocol		version)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx) || ctx->isDTLS) {
		/* Can't do this with an active session, nor with a DTLS session */
		return errSecBadReq;
	}

	switch(version) {
		case kSSLProtocol3:
			/* this tells us to do our best, up to 3.0 */
            ctx->minProtocolVersion = MINIMUM_STREAM_VERSION;
            ctx->maxProtocolVersion = SSL_Version_3_0;
			break;
		case kSSLProtocol3Only:
            ctx->minProtocolVersion = SSL_Version_3_0;
            ctx->maxProtocolVersion = SSL_Version_3_0;
			break;
		case kTLSProtocol1:
			/* this tells us to do our best, up to TLS, but allows 3.0 */
            ctx->minProtocolVersion = MINIMUM_STREAM_VERSION;
            ctx->maxProtocolVersion = TLS_Version_1_0;
            break;
        case kTLSProtocol1Only:
            ctx->minProtocolVersion = TLS_Version_1_0;
            ctx->maxProtocolVersion = TLS_Version_1_0;
			break;
        case kTLSProtocol11:
			/* This tells us to do our best, up to TLS 1.1, currently also
               allows 3.0 or TLS 1.0 */
            ctx->minProtocolVersion = MINIMUM_STREAM_VERSION;
            ctx->maxProtocolVersion = TLS_Version_1_1;
			break;
        case kTLSProtocol12:
        case kSSLProtocolAll:
		case kSSLProtocolUnknown:
			/* This tells us to do our best, up to TLS 1.2, currently also
               allows 3.0 or TLS 1.0 or TLS 1.1 */
            ctx->minProtocolVersion = MINIMUM_STREAM_VERSION;
            ctx->maxProtocolVersion = MAXIMUM_STREAM_VERSION;
			break;
		default:
			return errSecParam;
	}

    return errSecSuccess;
}

/* deprecated */
OSStatus
SSLGetProtocolVersion		(SSLContextRef		ctx,
							 SSLProtocol		*protocol)		/* RETURNED */
{
	if(ctx == NULL) {
		return errSecParam;
	}
	/* translate array of booleans to public value; not all combinations
	 * are legal (i.e., meaningful) for this call */
    if (ctx->maxProtocolVersion == MAXIMUM_STREAM_VERSION) {
        if(ctx->minProtocolVersion == MINIMUM_STREAM_VERSION) {
            /* traditional 'all enabled' */
            *protocol = kSSLProtocolAll;
            return errSecSuccess;
		}
	} else if (ctx->maxProtocolVersion == TLS_Version_1_1) {
        if(ctx->minProtocolVersion == MINIMUM_STREAM_VERSION) {
            /* traditional 'all enabled' */
            *protocol = kTLSProtocol11;
            return errSecSuccess;
        }
	} else if (ctx->maxProtocolVersion == TLS_Version_1_0) {
        if(ctx->minProtocolVersion == MINIMUM_STREAM_VERSION) {
            /* TLS1.1 and below enabled */
            *protocol = kTLSProtocol1;
            return errSecSuccess;
        } else if(ctx->minProtocolVersion == TLS_Version_1_0) {
        	*protocol = kTLSProtocol1Only;
		}
	} else if(ctx->maxProtocolVersion == SSL_Version_3_0) {
        if(ctx->minProtocolVersion == MINIMUM_STREAM_VERSION) {
            /* Could also return kSSLProtocol3Only since
               MINIMUM_STREAM_VERSION == SSL_Version_3_0. */
            *protocol = kSSLProtocol3;
			return errSecSuccess;
		}
	}

    return errSecParam;
}

OSStatus
SSLGetNegotiatedProtocolVersion		(SSLContextRef		ctx,
									 SSLProtocol		*protocol) /* RETURNED */
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*protocol = SSLProtocolVersionToProtocol(ctx->negProtocolVersion);
	return errSecSuccess;
}

OSStatus
SSLSetEnableCertVerify		(SSLContextRef		ctx,
							 Boolean			enableVerify)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	sslCertDebug("SSLSetEnableCertVerify %s",
		enableVerify ? "true" : "false");
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
	ctx->enableCertVerify = enableVerify;
	return errSecSuccess;
}

OSStatus
SSLGetEnableCertVerify		(SSLContextRef		ctx,
							Boolean				*enableVerify)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*enableVerify = ctx->enableCertVerify;
	return errSecSuccess;
}

OSStatus
SSLSetAllowsExpiredCerts(SSLContextRef		ctx,
						 Boolean			allowExpired)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	sslCertDebug("SSLSetAllowsExpiredCerts %s",
		allowExpired ? "true" : "false");
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
	ctx->allowExpiredCerts = allowExpired;
	return errSecSuccess;
}

OSStatus
SSLGetAllowsExpiredCerts	(SSLContextRef		ctx,
							 Boolean			*allowExpired)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*allowExpired = ctx->allowExpiredCerts;
	return errSecSuccess;
}

OSStatus
SSLSetAllowsExpiredRoots(SSLContextRef		ctx,
						 Boolean			allowExpired)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	sslCertDebug("SSLSetAllowsExpiredRoots %s",
		allowExpired ? "true" : "false");
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
	ctx->allowExpiredRoots = allowExpired;
	return errSecSuccess;
}

OSStatus
SSLGetAllowsExpiredRoots	(SSLContextRef		ctx,
							 Boolean			*allowExpired)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*allowExpired = ctx->allowExpiredRoots;
	return errSecSuccess;
}

OSStatus SSLSetAllowsAnyRoot(
	SSLContextRef	ctx,
	Boolean			anyRoot)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	sslCertDebug("SSLSetAllowsAnyRoot %s",	anyRoot ? "true" : "false");
	ctx->allowAnyRoot = anyRoot;
	return errSecSuccess;
}

OSStatus
SSLGetAllowsAnyRoot(
	SSLContextRef	ctx,
	Boolean			*anyRoot)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*anyRoot = ctx->allowAnyRoot;
	return errSecSuccess;
}

#if !TARGET_OS_IPHONE
/* obtain the system roots sets for this app, policy SSL */
static OSStatus sslDefaultSystemRoots(
	SSLContextRef ctx,
	CFArrayRef *systemRoots)				// created and RETURNED

{
	return SecTrustSettingsCopyQualifiedCerts(&CSSMOID_APPLE_TP_SSL,
		ctx->peerDomainName,
		(uint32_t)ctx->peerDomainNameLen,
		(ctx->protocolSide == kSSLServerSide) ?
			/* server verifies, client encrypts */
			CSSM_KEYUSE_VERIFY : CSSM_KEYUSE_ENCRYPT,
		systemRoots);
}
#endif /* OS X only */

OSStatus
SSLSetTrustedRoots			(SSLContextRef 		ctx,
							 CFArrayRef 		trustedRoots,
							 Boolean 			replaceExisting)
{
#ifdef USE_CDSA_CRYPTO
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}

	if(replaceExisting) {
		/* trivial case - retain the new, throw out the old.  */
		if (trustedRoots)
            CFRetain(trustedRoots);
        CFReleaseSafe(ctx->trustedCerts);
		ctx->trustedCerts = trustedRoots;
		return errSecSuccess;
	}

	/* adding new trusted roots - to either our existing set, or the system set */
	CFArrayRef existingRoots = NULL;
	OSStatus ortn;
	if(ctx->trustedCerts != NULL) {
		/* we'll release these as we exit */
		existingRoots = ctx->trustedCerts;
	}
	else {
		/* get system set for this app, policy SSL */
		ortn = sslDefaultSystemRoots(ctx, &existingRoots);
		if(ortn) {
            CFReleaseSafe(existingRoots);
			return ortn;
		}
	}

	/* Create a new root array with caller's roots first */
	CFMutableArrayRef newRoots = CFArrayCreateMutableCopy(NULL, 0, trustedRoots);
	CFRange existRange = { 0, CFArrayGetCount(existingRoots) };
	CFArrayAppendArray(newRoots, existingRoots, existRange);
	CFRelease(existingRoots);
	ctx->trustedCerts = newRoots;
	return errSecSuccess;

#else
	if (sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
	sslCertDebug("SSLSetTrustedRoot  numCerts %d  replaceExist %s",
		(int)CFArrayGetCount(trustedRoots), replaceExisting ? "true" : "false");

    if (replaceExisting) {
        ctx->trustedCertsOnly = true;
        CFReleaseNull(ctx->trustedCerts);
    }

    if (ctx->trustedCerts) {
        CFIndex count = CFArrayGetCount(trustedRoots);
        CFRange range = { 0, count };
        CFArrayAppendArray(ctx->trustedCerts, trustedRoots, range);
    } else {
        require(ctx->trustedCerts =
            CFArrayCreateMutableCopy(kCFAllocatorDefault, 0, trustedRoots),
            errOut);
    }

    return errSecSuccess;

errOut:
    return errSecAllocate;
#endif /* !USE_CDSA_CRYPTO */
}

OSStatus
SSLCopyTrustedRoots			(SSLContextRef 		ctx,
							 CFArrayRef 		*trustedRoots)	/* RETURNED */
{
	if(ctx == NULL || trustedRoots == NULL) {
		return errSecParam;
	}
	if(ctx->trustedCerts != NULL) {
		*trustedRoots = ctx->trustedCerts;
		CFRetain(ctx->trustedCerts);
		return errSecSuccess;
	}
#if (TARGET_OS_MAC && !(TARGET_OS_EMBEDDED || TARGET_OS_IPHONE))
	/* use default system roots */
    return sslDefaultSystemRoots(ctx, trustedRoots);
#else
    *trustedRoots = NULL;
    return errSecSuccess;
#endif
}

OSStatus
SSLSetTrustedLeafCertificates	(SSLContextRef 		ctx,
								 CFArrayRef 		trustedCerts)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}

	if(ctx->trustedLeafCerts) {
		CFRelease(ctx->trustedLeafCerts);
	}
	ctx->trustedLeafCerts = trustedCerts;
	CFRetain(trustedCerts);
	return errSecSuccess;
}

OSStatus
SSLCopyTrustedLeafCertificates	(SSLContextRef 		ctx,
								 CFArrayRef 		*trustedCerts)	/* RETURNED */
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(ctx->trustedLeafCerts != NULL) {
		*trustedCerts = ctx->trustedLeafCerts;
		CFRetain(ctx->trustedCerts);
		return errSecSuccess;
	}
	*trustedCerts = NULL;
	return errSecSuccess;
}

OSStatus
SSLSetClientSideAuthenticate 	(SSLContext			*ctx,
								 SSLAuthenticate	auth)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
	ctx->clientAuth = auth;
	switch(auth) {
		case kNeverAuthenticate:
			ctx->tryClientAuth = false;
			break;
		case kAlwaysAuthenticate:
		case kTryAuthenticate:
			ctx->tryClientAuth = true;
			break;
	}
	return errSecSuccess;
}

OSStatus
SSLGetClientSideAuthenticate 	(SSLContext			*ctx,
								 SSLAuthenticate	*auth)	/* RETURNED */
{
	if(ctx == NULL || auth == NULL) {
		return errSecParam;
	}
	*auth = ctx->clientAuth;
	return errSecSuccess;
}

OSStatus
SSLGetClientCertificateState	(SSLContextRef				ctx,
								 SSLClientCertificateState	*clientState)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*clientState = ctx->clientCertState;
	return errSecSuccess;
}

OSStatus
SSLSetCertificate			(SSLContextRef		ctx,
							 CFArrayRef			certRefs)
{
	/*
	 * -- free localCerts if we have any
	 * -- Get raw cert data, convert to ctx->localCert
	 * -- get pub, priv keys from certRef[0]
	 * -- validate cert chain
	 */
	if(ctx == NULL) {
		return errSecParam;
	}

	/* can't do this with an active session */
	if(sslIsSessionActive(ctx) &&
	   /* kSSLClientCertRequested implies client side */
	   (ctx->clientCertState != kSSLClientCertRequested))
	{
			return errSecBadReq;
	}
    CFReleaseNull(ctx->localCertArray);
	/* changing the client cert invalidates negotiated auth type */
	ctx->negAuthType = SSLClientAuthNone;
	if(certRefs == NULL) {
		return errSecSuccess; // we have cleared the cert, as requested
	}
	OSStatus ortn = parseIncomingCerts(ctx,
		certRefs,
		&ctx->localCert,
		&ctx->signingPubKey,
		&ctx->signingPrivKeyRef,
		&ctx->ourSignerAlg);
	if(ortn == errSecSuccess) {
		ctx->localCertArray = certRefs;
		CFRetain(certRefs);
		/* client cert was changed, must update auth type */
		ortn = SSLUpdateNegotiatedClientAuthType(ctx);
	}
	return ortn;
}

OSStatus
SSLSetEncryptionCertificate	(SSLContextRef		ctx,
							 CFArrayRef			certRefs)
{
	/*
	 * -- free encryptCert if we have any
	 * -- Get raw cert data, convert to ctx->encryptCert
	 * -- get pub, priv keys from certRef[0]
	 * -- validate cert chain
	 */
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
    CFReleaseNull(ctx->encryptCertArray);
	OSStatus ortn = parseIncomingCerts(ctx,
		certRefs,
		&ctx->encryptCert,
		&ctx->encryptPubKey,
		&ctx->encryptPrivKeyRef,
		NULL);			/* Signer alg */
	if(ortn == errSecSuccess) {
		ctx->encryptCertArray = certRefs;
		CFRetain(certRefs);
	}
	return ortn;
}

OSStatus SSLGetCertificate(SSLContextRef		ctx,
						   CFArrayRef			*certRefs)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*certRefs = ctx->localCertArray;
	return errSecSuccess;
}

OSStatus SSLGetEncryptionCertificate(SSLContextRef		ctx,
								     CFArrayRef			*certRefs)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*certRefs = ctx->encryptCertArray;
	return errSecSuccess;
}

OSStatus
SSLSetPeerID				(SSLContext 		*ctx,
							 const void 		*peerID,
							 size_t				peerIDLen)
{
	OSStatus serr;

	/* copy peerId to context->peerId */
	if((ctx == NULL) ||
	   (peerID == NULL) ||
	   (peerIDLen == 0)) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx) &&
        /* kSSLClientCertRequested implies client side */
        (ctx->clientCertState != kSSLClientCertRequested))
    {
		return errSecBadReq;
	}
	SSLFreeBuffer(&ctx->peerID);
	serr = SSLAllocBuffer(&ctx->peerID, peerIDLen);
	if(serr) {
		return serr;
	}
	memmove(ctx->peerID.data, peerID, peerIDLen);
	return errSecSuccess;
}

OSStatus
SSLGetPeerID				(SSLContextRef 		ctx,
							 const void 		**peerID,
							 size_t				*peerIDLen)
{
	*peerID = ctx->peerID.data;			// may be NULL
	*peerIDLen = ctx->peerID.length;
	return errSecSuccess;
}

OSStatus
SSLGetNegotiatedCipher		(SSLContextRef 		ctx,
							 SSLCipherSuite 	*cipherSuite)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(!sslIsSessionActive(ctx)) {
		return errSecBadReq;
	}
	*cipherSuite = (SSLCipherSuite)ctx->selectedCipher;
	return errSecSuccess;
}

/*
 * Add an acceptable distinguished name (client authentication only).
 */
OSStatus
SSLAddDistinguishedName(
	SSLContextRef ctx,
	const void *derDN,
	size_t derDNLen)
{
    DNListElem      *dn;
    OSStatus        err;

	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		return errSecBadReq;
	}

	dn = (DNListElem *)sslMalloc(sizeof(DNListElem));
	if(dn == NULL) {
		return errSecAllocate;
	}
    if ((err = SSLAllocBuffer(&dn->derDN, derDNLen)))
        return err;
    memcpy(dn->derDN.data, derDN, derDNLen);
    dn->next = ctx->acceptableDNList;
    ctx->acceptableDNList = dn;
    return errSecSuccess;
}

/* single-cert version of SSLSetCertificateAuthorities() */
static OSStatus
sslAddCA(SSLContextRef		ctx,
		 SecCertificateRef	cert)
{
	OSStatus ortn = errSecParam;

    /* Get subject from certificate. */
#if TARGET_OS_IPHONE
    CFDataRef subjectName = NULL;
    subjectName = SecCertificateCopySubjectSequence(cert);
    require(subjectName, errOut);
#else
    CSSM_DATA_PTR subjectName = NULL;
    ortn = SecCertificateCopyFirstFieldValue(cert, &CSSMOID_X509V1SubjectNameStd, &subjectName);
    require_noerr(ortn, errOut);
#endif

	/* add to acceptableCAs as cert, creating array if necessary */
	if(ctx->acceptableCAs == NULL) {
		require(ctx->acceptableCAs = CFArrayCreateMutable(NULL, 0,
            &kCFTypeArrayCallBacks), errOut);
		if(ctx->acceptableCAs == NULL) {
			return errSecAllocate;
		}
	}
	CFArrayAppendValue(ctx->acceptableCAs, cert);

	/* then add this cert's subject name to acceptableDNList */
#if TARGET_OS_IPHONE
	ortn = SSLAddDistinguishedName(ctx,
                                   CFDataGetBytePtr(subjectName),
                                   CFDataGetLength(subjectName));
#else
    ortn = SSLAddDistinguishedName(ctx, subjectName->Data, subjectName->Length);
#endif

errOut:
#if TARGET_OS_IPHONE
    CFReleaseSafe(subjectName);
#endif
	return ortn;
}

/*
 * Add a SecCertificateRef, or a CFArray of them, to a server's list
 * of acceptable Certificate Authorities (CAs) to present to the client
 * when client authentication is performed.
 */
OSStatus
SSLSetCertificateAuthorities(SSLContextRef		ctx,
							 CFTypeRef			certificateOrArray,
							 Boolean 			replaceExisting)
{
	CFTypeID itemType;
	OSStatus ortn = errSecSuccess;

	if((ctx == NULL) || sslIsSessionActive(ctx) ||
	   (ctx->protocolSide != kSSLServerSide)) {
		return errSecParam;
	}
	if(replaceExisting) {
		sslFreeDnList(ctx);
		if(ctx->acceptableCAs) {
			CFRelease(ctx->acceptableCAs);
			ctx->acceptableCAs = NULL;
		}
	}
	/* else appending */

	itemType = CFGetTypeID(certificateOrArray);
	if(itemType == SecCertificateGetTypeID()) {
		/* one cert */
		ortn = sslAddCA(ctx, (SecCertificateRef)certificateOrArray);
	}
	else if(itemType == CFArrayGetTypeID()) {
		CFArrayRef cfa = (CFArrayRef)certificateOrArray;
		CFIndex numCerts = CFArrayGetCount(cfa);
		CFIndex dex;

		/* array of certs */
		for(dex=0; dex<numCerts; dex++) {
			SecCertificateRef cert = (SecCertificateRef)CFArrayGetValueAtIndex(cfa, dex);
			if(CFGetTypeID(cert) != SecCertificateGetTypeID()) {
				return errSecParam;
			}
			ortn = sslAddCA(ctx, cert);
			if(ortn) {
				break;
			}
		}
	}
	else {
		ortn = errSecParam;
	}
	return ortn;
}


/*
 * Obtain the certificates specified in SSLSetCertificateAuthorities(),
 * if any. Returns a NULL array if SSLSetCertificateAuthorities() has not
 * been called.
 * Caller must CFRelease the returned array.
 */
OSStatus
SSLCopyCertificateAuthorities(SSLContextRef		ctx,
							  CFArrayRef		*certificates)	/* RETURNED */
{
	if((ctx == NULL) || (certificates == NULL)) {
		return errSecParam;
	}
	if(ctx->acceptableCAs == NULL) {
		*certificates = NULL;
		return errSecSuccess;
	}
	*certificates = ctx->acceptableCAs;
	CFRetain(ctx->acceptableCAs);
	return errSecSuccess;
}


/*
 * Obtain the list of acceptable distinguished names as provided by
 * a server (if the SSLCotextRef is configured as a client), or as
 * specified by SSLSetCertificateAuthorities() (if the SSLContextRef
 * is configured as a server).
  */
OSStatus
SSLCopyDistinguishedNames	(SSLContextRef		ctx,
							 CFArrayRef			*names)
{
	CFMutableArrayRef outArray = NULL;
	DNListElem *dn;

	if((ctx == NULL) || (names == NULL)) {
		return errSecParam;
	}
	if(ctx->acceptableDNList == NULL) {
		*names = NULL;
		return errSecSuccess;
	}
	outArray = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	dn = ctx->acceptableDNList;
	while (dn) {
		CFDataRef cfDn = CFDataCreate(NULL, dn->derDN.data, dn->derDN.length);
		CFArrayAppendValue(outArray, cfDn);
		CFRelease(cfDn);
		dn = dn->next;
	}
	*names = outArray;
	return errSecSuccess;
}


/*
 * Request peer certificates. Valid anytime, subsequent to
 * a handshake attempt.
 * Common code for SSLGetPeerCertificates() and SSLCopyPeerCertificates().
 * TODO: the 'legacy' argument is not used anymore.
 */
static OSStatus
sslCopyPeerCertificates		(SSLContextRef 		ctx,
							 CFArrayRef			*certs,
							 Boolean			legacy)
{
	if(ctx == NULL) {
		return errSecParam;
	}

#ifdef USE_SSLCERTIFICATE
	uint32 				numCerts;
	CFMutableArrayRef	ca;
	CFIndex				i;
	SecCertificateRef	cfd;
	OSStatus			ortn;
	CSSM_DATA			certData;
	SSLCertificate		*scert;

	*certs = NULL;

	/*
	 * Copy peerCert, a chain of SSLCertificates, to a CFArray of
	 * CFDataRefs, each of which is one DER-encoded cert.
	 */
	numCerts = SSLGetCertificateChainLength(ctx->peerCert);
	if(numCerts == 0) {
		return errSecSuccess;
	}
	ca = CFArrayCreateMutable(kCFAllocatorDefault,
		(CFIndex)numCerts, &kCFTypeArrayCallBacks);
	if(ca == NULL) {
		return errSecAllocate;
	}

	/*
	 * Caller gets leaf cert first, the opposite of the way we store them.
	 */
	scert = ctx->peerCert;
	for(i=0; (unsigned)i<numCerts; i++) {
		assert(scert != NULL);		/* else SSLGetCertificateChainLength
									 * broken */
		SSLBUF_TO_CSSM(&scert->derCert, &certData);
		ortn = SecCertificateCreateFromData(&certData,
			CSSM_CERT_X_509v3,
			CSSM_CERT_ENCODING_DER,
			&cfd);
		if(ortn) {
			CFRelease(ca);
			return ortn;
		}
		/* insert at head of array */
		CFArrayInsertValueAtIndex(ca, 0, cfd);
		if(!legacy) {
			/* skip for legacy SSLGetPeerCertificates() */
			CFRelease(cfd);
		}
		scert = scert->next;
	}
	*certs = ca;

#else
	if (!ctx->peerCert) {
		*certs = NULL;
		return errSecBadReq;
	}

    CFArrayRef ca = CFArrayCreateCopy(kCFAllocatorDefault, ctx->peerCert);
    *certs = ca;
    if (ca == NULL) {
        return errSecAllocate;
    }

	if (legacy) {
		CFIndex ix, count = CFArrayGetCount(ca);
		for (ix = 0; ix < count; ++ix) {
			CFRetain(CFArrayGetValueAtIndex(ca, ix));
		}
	}
#endif

	return errSecSuccess;
}

OSStatus
SSLCopyPeerCertificates		(SSLContextRef 		ctx,
							 CFArrayRef			*certs)
{
	return sslCopyPeerCertificates(ctx, certs, false);
}

#if !TARGET_OS_IPHONE
// Permanently removing from iOS, keep for OSX (deprecated), removed from headers.
// <rdar://problem/14215831> Mailsmith Crashes While Getting New Mail Under Mavericks Developer Preview
OSStatus
SSLGetPeerCertificates (SSLContextRef ctx,
                        CFArrayRef *certs);
OSStatus
SSLGetPeerCertificates (SSLContextRef ctx,
                        CFArrayRef *certs)
{
    return sslCopyPeerCertificates(ctx, certs, true);
}
#endif

/*
 * Specify Diffie-Hellman parameters. Optional; if we are configured to allow
 * for D-H ciphers and a D-H cipher is negotiated, and this function has not
 * been called, a set of process-wide parameters will be calculated. However
 * that can take a long time (30 seconds).
 */
OSStatus SSLSetDiffieHellmanParams(
	SSLContextRef	ctx,
	const void 		*dhParams,
	size_t			dhParamsLen)
{
#if APPLE_DH
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		return errSecBadReq;
	}
	SSLFreeBuffer(&ctx->dhParamsEncoded);
#if !USE_CDSA_CRYPTO
    if (ctx->secDHContext)
        SecDHDestroy(ctx->secDHContext);
#endif

	OSStatus ortn;
	ortn = SSLCopyBufferFromData(dhParams, dhParamsLen,
		&ctx->dhParamsEncoded);

    return ortn;

#endif /* APPLE_DH */
}

/*
 * Return parameter block specified in SSLSetDiffieHellmanParams.
 * Returned data is not copied and belongs to the SSLContextRef.
 */
OSStatus SSLGetDiffieHellmanParams(
	SSLContextRef	ctx,
	const void 		**dhParams,
	size_t			*dhParamsLen)
{
#if APPLE_DH
	if(ctx == NULL) {
		return errSecParam;
	}
	*dhParams = ctx->dhParamsEncoded.data;
	*dhParamsLen = ctx->dhParamsEncoded.length;
	return errSecSuccess;
#else
    return errSecUnimplemented;
#endif /* APPLE_DH */
}

OSStatus SSLSetRsaBlinding(
	SSLContextRef	ctx,
	Boolean			blinding)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	ctx->rsaBlindingEnable = blinding;
	return errSecSuccess;
}

OSStatus SSLGetRsaBlinding(
	SSLContextRef	ctx,
	Boolean			*blinding)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*blinding = ctx->rsaBlindingEnable;
	return errSecSuccess;
}

OSStatus
SSLCopyPeerTrust(
    SSLContextRef 		ctx,
    SecTrustRef        *trust)	/* RETURNED */
{
	OSStatus status = errSecSuccess;
	if (ctx == NULL || trust == NULL)
		return errSecParam;

	/* Create a SecTrustRef if this was a resumed session and we
	   didn't have one yet. */
	if (!ctx->peerSecTrust && ctx->peerCert) {
		status = sslCreateSecTrust(ctx, ctx->peerCert, true,
			&ctx->peerSecTrust);
    }

	*trust = ctx->peerSecTrust;
    if (ctx->peerSecTrust)
        CFRetain(ctx->peerSecTrust);

	return status;
}

OSStatus SSLGetPeerSecTrust(
	SSLContextRef	ctx,
	SecTrustRef		*trust)	/* RETURNED */
{
    OSStatus status = errSecSuccess;
	if (ctx == NULL || trust == NULL)
		return errSecParam;

	/* Create a SecTrustRef if this was a resumed session and we
	   didn't have one yet. */
	if (!ctx->peerSecTrust && ctx->peerCert) {
		status = sslCreateSecTrust(ctx, ctx->peerCert, true,
			&ctx->peerSecTrust);
    }

	*trust = ctx->peerSecTrust;
	return status;
}

OSStatus SSLInternalMasterSecret(
   SSLContextRef ctx,
   void *secret,        // mallocd by caller, SSL_MASTER_SECRET_SIZE
   size_t *secretSize)  // in/out
{
	if((ctx == NULL) || (secret == NULL) || (secretSize == NULL)) {
		return errSecParam;
	}
	if(*secretSize < SSL_MASTER_SECRET_SIZE) {
		return errSecParam;
	}
	memmove(secret, ctx->masterSecret, SSL_MASTER_SECRET_SIZE);
	*secretSize = SSL_MASTER_SECRET_SIZE;
	return errSecSuccess;
}

OSStatus SSLInternalServerRandom(
   SSLContextRef ctx,
   void *randBuf, 			// mallocd by caller, SSL_CLIENT_SRVR_RAND_SIZE
   size_t *randSize)	// in/out
{
	if((ctx == NULL) || (randBuf == NULL) || (randSize == NULL)) {
		return errSecParam;
	}
	if(*randSize < SSL_CLIENT_SRVR_RAND_SIZE) {
		return errSecParam;
	}
	memmove(randBuf, ctx->serverRandom, SSL_CLIENT_SRVR_RAND_SIZE);
	*randSize = SSL_CLIENT_SRVR_RAND_SIZE;
	return errSecSuccess;
}

OSStatus SSLInternalClientRandom(
   SSLContextRef ctx,
   void *randBuf,  		// mallocd by caller, SSL_CLIENT_SRVR_RAND_SIZE
   size_t *randSize)	// in/out
{
	if((ctx == NULL) || (randBuf == NULL) || (randSize == NULL)) {
		return errSecParam;
	}
	if(*randSize < SSL_CLIENT_SRVR_RAND_SIZE) {
		return errSecParam;
	}
	memmove(randBuf, ctx->clientRandom, SSL_CLIENT_SRVR_RAND_SIZE);
	*randSize = SSL_CLIENT_SRVR_RAND_SIZE;
	return errSecSuccess;
}

/* This is used by EAP 802.1x */
OSStatus SSLGetCipherSizes(
	SSLContextRef ctx,
	size_t *digestSize,
	size_t *symmetricKeySize,
	size_t *ivSize)
{
	const SSLCipherSpecParams *currCipher;
	
	if((ctx == NULL) || (digestSize == NULL) || 
	   (symmetricKeySize == NULL) || (ivSize == NULL)) {
		return errSecParam;
	}
	currCipher = &ctx->selectedCipherSpecParams;
	*digestSize = currCipher->macSize;
	*symmetricKeySize = currCipher->keySize;
	*ivSize = currCipher->ivSize;
	return errSecSuccess;
}

OSStatus
SSLGetResumableSessionInfo(
	SSLContextRef	ctx,
	Boolean			*sessionWasResumed,		// RETURNED
	void			*sessionID,				// RETURNED, mallocd by caller
	size_t			*sessionIDLength)		// IN/OUT
{
	if((ctx == NULL) || (sessionWasResumed == NULL) ||
	   (sessionID == NULL) || (sessionIDLength == NULL) ||
	   (*sessionIDLength < MAX_SESSION_ID_LENGTH)) {
		return errSecParam;
	}
	if(ctx->sessionMatch) {
		*sessionWasResumed = true;
		if(ctx->sessionID.length > *sessionIDLength) {
			/* really should never happen - means ID > 32 */
			return errSecParam;
		}
		if(ctx->sessionID.length) {
			/*
 			 * Note PAC-based session resumption can result in sessionMatch
			 * with no sessionID
			 */
			memmove(sessionID, ctx->sessionID.data, ctx->sessionID.length);
		}
		*sessionIDLength = ctx->sessionID.length;
	}
	else {
		*sessionWasResumed = false;
		*sessionIDLength = 0;
	}
	return errSecSuccess;
}

/*
 * Get/set enable of anonymous ciphers. Default is enabled.
 */
OSStatus
SSLSetAllowAnonymousCiphers(
	SSLContextRef	ctx,
	Boolean			enable)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		return errSecBadReq;
	}
	if(ctx->validCipherSuites != NULL) {
		/* SSLSetEnabledCiphers() has already been called */
		return errSecBadReq;
	}
	ctx->anonCipherEnable = enable;
	return errSecSuccess;
}

OSStatus
SSLGetAllowAnonymousCiphers(
	SSLContextRef	ctx,
	Boolean			*enable)
{
	if((ctx == NULL) || (enable == NULL)) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		return errSecBadReq;
	}
	*enable = ctx->anonCipherEnable;
	return errSecSuccess;
}

/*
 * Override the default session cache timeout for a cache entry created for
 * the current session.
 */
OSStatus
SSLSetSessionCacheTimeout(
	SSLContextRef ctx,
	uint32_t timeoutInSeconds)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	ctx->sessionCacheTimeout = timeoutInSeconds;
	return errSecSuccess;
}

/*
 * Register a callback for obtaining the master_secret when performing
 * PAC-based session resumption.
 */
OSStatus
SSLInternalSetMasterSecretFunction(
	SSLContextRef ctx,
	SSLInternalMasterSecretFunction mFunc,
	const void *arg)		/* opaque to SecureTransport; app-specific */
{
	if(ctx == NULL) {
		return errSecParam;
	}
	ctx->masterSecretCallback = mFunc;
	ctx->masterSecretArg = arg;
	return errSecSuccess;
}

/*
 * Provide an opaque SessionTicket for use in PAC-based session
 * resumption. Client side only. The provided ticket is sent in
 * the ClientHello message as a SessionTicket extension.
 *
 * We won't reject this on the server side, but server-side support
 * for PAC-based session resumption is currently enabled for
 * Development builds only. To fully support this for server side,
 * besides the rudimentary support that's here for Development builds,
 * we'd need a getter for the session ticket, so the app code can
 * access the SessionTicket when its SSLInternalMasterSecretFunction
 * callback is called.
 */
OSStatus SSLInternalSetSessionTicket(
   SSLContextRef ctx,
   const void *ticket,
   size_t ticketLength)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
	if(ticketLength > 0xffff) {
		/* extension data encoded with a 2-byte length! */
		return errSecParam;
	}
	SSLFreeBuffer(&ctx->sessionTicket);
	return SSLCopyBufferFromData(ticket, ticketLength, &ctx->sessionTicket);
}

/*
 * ECDSA curve accessors.
 */

/*
 * Obtain the SSL_ECDSA_NamedCurve negotiated during a handshake.
 * Returns errSecParam if no ECDH-related ciphersuite was negotiated.
 */
OSStatus SSLGetNegotiatedCurve(
   SSLContextRef ctx,
   SSL_ECDSA_NamedCurve *namedCurve)    /* RETURNED */
{
	if((ctx == NULL) || (namedCurve == NULL)) {
		return errSecParam;
	}
	if(ctx->ecdhPeerCurve == SSL_Curve_None) {
		return errSecParam;
	}
	*namedCurve = ctx->ecdhPeerCurve;
	return errSecSuccess;
}

/*
 * Obtain the number of currently enabled SSL_ECDSA_NamedCurves.
 */
OSStatus SSLGetNumberOfECDSACurves(
   SSLContextRef ctx,
   unsigned *numCurves)	/* RETURNED */
{
	if((ctx == NULL) || (numCurves == NULL)) {
		return errSecParam;
	}
	*numCurves = ctx->ecdhNumCurves;
	return errSecSuccess;
}

/*
 * Obtain the ordered list of currently enabled SSL_ECDSA_NamedCurves.
 */
OSStatus SSLGetECDSACurves(
   SSLContextRef ctx,
   SSL_ECDSA_NamedCurve *namedCurves,		/* RETURNED */
   unsigned *numCurves)						/* IN/OUT */
{
	if((ctx == NULL) || (namedCurves == NULL) || (numCurves == NULL)) {
		return errSecParam;
	}
	if(*numCurves < ctx->ecdhNumCurves) {
		return errSecParam;
	}
	memmove(namedCurves, ctx->ecdhCurves,
		(ctx->ecdhNumCurves * sizeof(SSL_ECDSA_NamedCurve)));
	*numCurves = ctx->ecdhNumCurves;
	return errSecSuccess;
}

/*
 * Specify ordered list of allowable named curves.
 */
OSStatus SSLSetECDSACurves(
   SSLContextRef ctx,
   const SSL_ECDSA_NamedCurve *namedCurves,
   unsigned numCurves)
{
	if((ctx == NULL) || (namedCurves == NULL) || (numCurves == 0)) {
		return errSecParam;
	}
	if(numCurves > SSL_ECDSA_NUM_CURVES) {
		return errSecParam;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return errSecBadReq;
	}
	memmove(ctx->ecdhCurves, namedCurves, (numCurves * sizeof(SSL_ECDSA_NamedCurve)));
	ctx->ecdhNumCurves = numCurves;
	return errSecSuccess;
}

/*
 * Obtain the number of client authentication mechanisms specified by
 * the server in its Certificate Request message.
 * Returns errSecParam if server hasn't sent a Certificate Request message
 * (i.e., client certificate state is kSSLClientCertNone).
 */
OSStatus SSLGetNumberOfClientAuthTypes(
	SSLContextRef ctx,
	unsigned *numTypes)
{
	if((ctx == NULL) || (ctx->clientCertState == kSSLClientCertNone)) {
		return errSecParam;
	}
	*numTypes = ctx->numAuthTypes;
	return errSecSuccess;
}

/*
 * Obtain the client authentication mechanisms specified by
 * the server in its Certificate Request message.
 * Caller allocates returned array and specifies its size (in
 * SSLClientAuthenticationTypes) in *numType on entry; *numTypes
 * is the actual size of the returned array on successful return.
 */
OSStatus SSLGetClientAuthTypes(
   SSLContextRef ctx,
   SSLClientAuthenticationType *authTypes,		/* RETURNED */
   unsigned *numTypes)							/* IN/OUT */
{
	if((ctx == NULL) || (ctx->clientCertState == kSSLClientCertNone)) {
		return errSecParam;
	}
	memmove(authTypes, ctx->clientAuthTypes,
		ctx->numAuthTypes * sizeof(SSLClientAuthenticationType));
	*numTypes = ctx->numAuthTypes;
	return errSecSuccess;
}

/*
 * Obtain the SSLClientAuthenticationType actually performed.
 * Only valid if client certificate state is kSSLClientCertSent
 * or kSSLClientCertRejected; returns errSecParam otherwise.
 */
OSStatus SSLGetNegotiatedClientAuthType(
   SSLContextRef ctx,
   SSLClientAuthenticationType *authType)		/* RETURNED */
{
	if(ctx == NULL) {
		return errSecParam;
	}
	*authType = ctx->negAuthType;
	return errSecSuccess;
}

/*
 * Update the negotiated client authentication type.
 * This function may be called at any time; however, note that
 * the negotiated authentication type will be SSLClientAuthNone
 * until both of the following have taken place (in either order):
 *   - a CertificateRequest message from the server has been processed
 *   - a client certificate has been specified
 * As such, this function (only) needs to be called from (both)
 * SSLProcessCertificateRequest and SSLSetCertificate.
 */
OSStatus SSLUpdateNegotiatedClientAuthType(
	SSLContextRef ctx)
{
	if(ctx == NULL) {
		return errSecParam;
	}
	/*
	 * See if we have a signing cert that matches one of the
	 * allowed auth types. The x509Requested flag indicates "we
	 * have a cert that we think the server will accept".
	 */
	ctx->x509Requested = 0;
	ctx->negAuthType = SSLClientAuthNone;
	if(ctx->signingPrivKeyRef != NULL) {
        CFIndex ourKeyAlg = sslPubKeyGetAlgorithmID(ctx->signingPubKey);
		unsigned i;
		for(i=0; i<ctx->numAuthTypes; i++) {
			switch(ctx->clientAuthTypes[i]) {
				case SSLClientAuth_RSASign:
					if(ourKeyAlg == kSecRSAAlgorithmID) {
						ctx->x509Requested = 1;
						ctx->negAuthType = SSLClientAuth_RSASign;
					}
					break;
			#if SSL_ENABLE_ECDSA_SIGN_AUTH
				case SSLClientAuth_ECDSASign:
			#endif
			#if SSL_ENABLE_ECDSA_FIXED_ECDH_AUTH
				case SSLClientAuth_ECDSAFixedECDH:
			#endif
					if((ourKeyAlg == kSecECDSAAlgorithmID) &&
					   (ctx->ourSignerAlg == kSecECDSAAlgorithmID)) {
						ctx->x509Requested = 1;
						ctx->negAuthType = ctx->clientAuthTypes[i];
					}
					break;
			#if SSL_ENABLE_RSA_FIXED_ECDH_AUTH
				case SSLClientAuth_RSAFixedECDH:
					/* Odd case, we differ from our signer */
					if((ourKeyAlg == kSecECDSAAlgorithmID) &&
					   (ctx->ourSignerAlg == kSecRSAAlgorithmID)) {
						ctx->x509Requested = 1;
						ctx->negAuthType = SSLClientAuth_RSAFixedECDH;
					}
					break;
			#endif
				default:
					/* None others supported */
					break;
			}
			if(ctx->x509Requested) {
				sslLogNegotiateDebug("===CHOOSING authType %d", (int)ctx->negAuthType);
				break;
			}
		}	/* parsing authTypes */
	}	/* we have a signing key */

	return errSecSuccess;
}

OSStatus SSLGetNumberOfSignatureAlgorithms(
    SSLContextRef ctx,
    unsigned *numSigAlgs)
{
	if((ctx == NULL) || (ctx->clientCertState == kSSLClientCertNone)) {
		return errSecParam;
	}
	*numSigAlgs = ctx->numServerSigAlgs;
	return errSecSuccess;
}

OSStatus SSLGetSignatureAlgorithms(
    SSLContextRef ctx,
    SSLSignatureAndHashAlgorithm *sigAlgs,		/* RETURNED */
    unsigned *numSigAlgs)							/* IN/OUT */
{
	if((ctx == NULL) || (ctx->clientCertState == kSSLClientCertNone)) {
		return errSecParam;
	}
	memmove(sigAlgs, ctx->serverSigAlgs,
            ctx->numServerSigAlgs * sizeof(SSLSignatureAndHashAlgorithm));
	*numSigAlgs = ctx->numServerSigAlgs;
	return errSecSuccess;
}

/* PSK SPIs */
OSStatus SSLSetPSKSharedSecret(SSLContextRef ctx,
                               const void *secret,
                               size_t secretLen)
{
    if(ctx == NULL) return errSecParam;

    if(ctx->pskSharedSecret.data)
        SSLFreeBuffer(&ctx->pskSharedSecret);

    if(SSLCopyBufferFromData(secret, secretLen, &ctx->pskSharedSecret))
        return errSecAllocate;

    return errSecSuccess;
}

OSStatus SSLSetPSKIdentity(SSLContextRef ctx,
                           const void *pskIdentity,
                           size_t pskIdentityLen)
{
    if((ctx == NULL) || (pskIdentity == NULL) || (pskIdentityLen == 0)) return errSecParam;

    if(ctx->pskIdentity.data)
        SSLFreeBuffer(&ctx->pskIdentity);

    if(SSLCopyBufferFromData(pskIdentity, pskIdentityLen, &ctx->pskIdentity))
        return errSecAllocate;

    return errSecSuccess;

}

OSStatus SSLGetPSKIdentity(SSLContextRef ctx,
                           const void **pskIdentity,
                           size_t *pskIdentityLen)
{
    if((ctx == NULL) || (pskIdentity == NULL) || (pskIdentityLen == NULL)) return errSecParam;

    *pskIdentity=ctx->pskIdentity.data;
    *pskIdentityLen=ctx->pskIdentity.length;
    return errSecSuccess;
}


#ifdef USE_SSLCERTIFICATE

size_t
SSLGetCertificateChainLength(const SSLCertificate *c)
{
	size_t rtn = 0;

    while (c)
    {
	rtn++;
        c = c->next;
    }
    return rtn;
}

OSStatus sslDeleteCertificateChain(
                                   SSLCertificate		*certs,
                                   SSLContext 			*ctx)
{
	SSLCertificate		*cert;
	SSLCertificate		*nextCert;

	assert(ctx != NULL);
	cert=certs;
	while(cert != NULL) {
		nextCert = cert->next;
		SSLFreeBuffer(&cert->derCert);
		sslFree(cert);
		cert = nextCert;
	}
	return errSecSuccess;
}
#endif /* USE_SSLCERTIFICATE */
