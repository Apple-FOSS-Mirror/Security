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
	File:		cipherSpecs.c

	Contains:	SSLCipherSpec declarations

	Written by:	Doug Mitchell, based on Netscape SSLRef 3.0

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#include "sslctx.h"
#include "cryptType.h"
#include "symCipher.h"
#include "cipherSpecs.h"
#include "sslDebug.h"
#include "sslalloc.h"
#include "sslDebug.h"
#include "sslutil.h"
#include <string.h>
#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacErrors.h>

/* FIXME - domestic suites do not work in server side in level 3 */

#define ENABLE_3DES		1		/* normally enabled, our first preference */
#define ENABLE_RC4		1		/* normally enabled, the most common one */
#define ENABLE_DES		1		/* normally enabled */
#define ENABLE_RC2		1		/* normally enabled */

#define ENABLE_RSA_DES_SHA_NONEXPORT		ENABLE_DES	
#define ENABLE_RSA_DES_MD5_NONEXPORT		ENABLE_DES
#define ENABLE_RSA_DES_SHA_EXPORT			ENABLE_DES
#define ENABLE_RSA_RC4_MD5_EXPORT			ENABLE_RC4	/* the most common one */
#define ENABLE_RSA_RC4_MD5_NONEXPORT		ENABLE_RC4 
#define ENABLE_RSA_RC4_SHA_NONEXPORT		ENABLE_RC4
#define ENABLE_RSA_RC2_MD5_EXPORT			ENABLE_RC2
#define ENABLE_RSA_RC2_MD5_NONEXPORT		ENABLE_RC2
#define ENABLE_RSA_3DES_SHA					ENABLE_3DES 
#define ENABLE_RSA_3DES_MD5					ENABLE_3DES	

extern SSLSymmetricCipher SSLCipherNull;		/* in nullciph.c */

/*
 * The symmetric ciphers currently supported (in addition to the
 * NULL cipher in nullciph.c).
 */
#if	ENABLE_DES
static const SSLSymmetricCipher SSLCipherDES_CBC = {
    8,      /* Key size in bytes */
    8,      /* Secret key size = 64 bits */
    8,      /* IV size */
    8,      /* Block size */
    CSSM_ALGID_DES,
    CSSM_ALGID_DES,
    /* Note we don't want CSSM_ALGMODE_CBCPadIV8; our clients do that
     * for us */
    CSSM_ALGMODE_CBC_IV8,
	CSSM_PADDING_NONE,
    CDSASymmInit,
    CDSASymmEncrypt,
    CDSASymmDecrypt,
    CDSASymmFinish
};

static const SSLSymmetricCipher SSLCipherDES40_CBC = {
    8,      /* Key size in bytes */
    5,      /* Secret key size = 40 bits */
    8,      /* IV size */
    8,      /* Block size */
    CSSM_ALGID_DES,
    CSSM_ALGID_DES,
    CSSM_ALGMODE_CBC_IV8,
	CSSM_PADDING_NONE,
    CDSASymmInit,
    CDSASymmEncrypt,
    CDSASymmDecrypt,
    CDSASymmFinish
};
#endif	/* ENABLE_DES */

#if	ENABLE_3DES
static const SSLSymmetricCipher SSLCipher3DES_CBC = {
    24,     /* Key size in bytes */
    24,     /* Secret key size = 192 bits */
    8,      /* IV size */
    8,      /* Block size */
    CSSM_ALGID_3DES_3KEY,			// key gen 
    CSSM_ALGID_3DES_3KEY_EDE,		// encryption
    /* Note we don't want CSSM_ALGMODE_CBCPadIV8; our clients do that
     * for us */
    CSSM_ALGMODE_CBC_IV8,
	CSSM_PADDING_NONE,
    CDSASymmInit,
    CDSASymmEncrypt,
    CDSASymmDecrypt,
    CDSASymmFinish
};
#endif	/* ENABLE_3DES */

#if		ENABLE_RC4
static const SSLSymmetricCipher SSLCipherRC4_40 = {
    16,         /* Key size in bytes */
    5,          /* Secret key size = 40 bits */
    0,          /* IV size */
    0,          /* Block size */
    CSSM_ALGID_RC4,
    CSSM_ALGID_RC4,
    CSSM_ALGMODE_NONE,
	CSSM_PADDING_NONE,
    CDSASymmInit,
    CDSASymmEncrypt,
    CDSASymmDecrypt,
    CDSASymmFinish
};

static const SSLSymmetricCipher SSLCipherRC4_128 = {
    16,         /* Key size in bytes */
    16,         /* Secret key size = 128 bits */
    0,          /* IV size */
    0,          /* Block size */
    CSSM_ALGID_RC4,
    CSSM_ALGID_RC4,
    CSSM_ALGMODE_NONE,
	CSSM_PADDING_NONE,
    CDSASymmInit,
    CDSASymmEncrypt,
    CDSASymmDecrypt,
    CDSASymmFinish
};
#endif	/* ENABLE_RC4 */

#if		ENABLE_RC2
static const SSLSymmetricCipher SSLCipherRC2_40 = {
    16,         /* Key size in bytes */
    5,          /* Secret key size = 40 bits */
    8,          /* IV size */
    8,          /* Block size */
    CSSM_ALGID_RC2,
    CSSM_ALGID_RC2,
    CSSM_ALGMODE_CBC_IV8,
	CSSM_PADDING_NONE,
    CDSASymmInit,
    CDSASymmEncrypt,
    CDSASymmDecrypt,
    CDSASymmFinish
};

static const SSLSymmetricCipher SSLCipherRC2_128 = {
    16,         /* Key size in bytes */
    16,          /* Secret key size = 40 bits */
    8,          /* IV size */
    8,          /* Block size */
    CSSM_ALGID_RC2,
    CSSM_ALGID_RC2,
    CSSM_ALGMODE_CBC_IV8,
	CSSM_PADDING_NONE,
    CDSASymmInit,
    CDSASymmEncrypt,
    CDSASymmDecrypt,
    CDSASymmFinish
};

#endif	/* ENABLE_RC2*/


/* Even if we don't support NULL_WITH_NULL_NULL for transport, 
 * we need a reference for startup */
const SSLCipherSpec SSL_NULL_WITH_NULL_NULL_CipherSpec =
{   SSL_NULL_WITH_NULL_NULL,
    Exportable,
    SSL_NULL_auth,
    &HashHmacNull,
    &SSLCipherNull
};

/*
 * List of all CipherSpecs we implement. Depending on a context's 
 * exportable flag, not all of these might be available for use. 
 *
 * FIXME - I'm not sure the distinction between e.g. SSL_RSA and SSL_RSA_EXPORT
 * makes any sense here. See comments for the definition of 
 * KeyExchangeMethod in cryptType.h.
 */
/* Order by preference, domestic first */
static const SSLCipherSpec KnownCipherSpecs[] =
{
	/*** domestic only ***/
	#if	ENABLE_RSA_3DES_SHA
	    {   
	    	SSL_RSA_WITH_3DES_EDE_CBC_SHA, 
	    	NotExportable, 
	    	SSL_RSA, 
	    	&HashHmacSHA1, 
	    	&SSLCipher3DES_CBC 
	    },
	#endif
	#if	ENABLE_RSA_3DES_MD5
	    {   
	    	SSL_RSA_WITH_3DES_EDE_CBC_MD5, 
	    	NotExportable, 
	    	SSL_RSA, 
	    	&HashHmacMD5, 
	    	&SSLCipher3DES_CBC 
	    },
	#endif
    #if	ENABLE_RSA_RC4_SHA_NONEXPORT
	    {   
	    	SSL_RSA_WITH_RC4_128_SHA, 
	    	NotExportable, 
	    	SSL_RSA, 
	    	&HashHmacSHA1, 
	    	&SSLCipherRC4_128 
	    },
    #endif
    #if	ENABLE_RSA_RC4_MD5_NONEXPORT
	    {   
	    	SSL_RSA_WITH_RC4_128_MD5, 
	    	NotExportable, 
	    	SSL_RSA, 
	    	&HashHmacMD5, 
	    	&SSLCipherRC4_128 
	    },
    #endif
	#if	ENABLE_RSA_DES_SHA_NONEXPORT
	    {   
	    	SSL_RSA_WITH_DES_CBC_SHA, 
	    	NotExportable, 
	    	SSL_RSA, 
	    	&HashHmacSHA1, 
	    	&SSLCipherDES_CBC 
	    },
    #endif
	#if	ENABLE_RSA_DES_MD5_NONEXPORT
	    {   
	    	SSL_RSA_WITH_DES_CBC_MD5, 
	    	NotExportable, 
	    	SSL_RSA, 
	    	&HashHmacMD5, 
	    	&SSLCipherDES_CBC 
	    },
    #endif
	/*** exportable ***/
	#if	ENABLE_RSA_RC4_MD5_EXPORT
		{   
			SSL_RSA_EXPORT_WITH_RC4_40_MD5, 
			Exportable, 
			SSL_RSA_EXPORT, 
			&HashHmacMD5, 
			&SSLCipherRC4_40 
		},
	#endif
    #if APPLE_DH
	    /* Apple CSP doesn't support D-H yet */
	    {   
	    	SSL_DH_anon_WITH_RC4_128_MD5, 
	    	NotExportable, 
	    	SSL_DH_anon, 
	    	&HashHmacMD5, 
	    	&SSLCipherRC4_128 
	    },
    #endif
	#if ENABLE_RSA_DES_SHA_EXPORT
	    {   
	    	SSL_RSA_EXPORT_WITH_DES40_CBC_SHA, 
	    	Exportable, 
	    	SSL_RSA_EXPORT, 
	    	&HashHmacSHA1, 
	    	&SSLCipherDES40_CBC 
	    },
	#endif 
	
    #if	ENABLE_RSA_RC2_MD5_EXPORT
	    {   
	    	SSL_RSA_EXPORT_WITH_RC2_CBC_40_MD5, 
	    	Exportable, 
	    	SSL_RSA_EXPORT, 
	    	&HashHmacMD5, 
	    	&SSLCipherRC2_40 
	    },
    #endif
    #if	ENABLE_RSA_RC2_MD5_NONEXPORT
	    {   
	    	SSL_RSA_WITH_RC2_CBC_MD5, 
	    	NotExportable, 
	    	SSL_RSA, 
	    	&HashHmacMD5, 
	    	&SSLCipherRC2_128 
	    },
    #endif
	    {   
	    	SSL_RSA_WITH_NULL_MD5, 
	    	Exportable, 
	    	SSL_RSA, 
	    	&HashHmacMD5, 
	    	&SSLCipherNull 
	    }
};

static const int CipherSpecCount = sizeof(KnownCipherSpecs) / sizeof(SSLCipherSpec);

/*
 * Build ctx->validCipherSpecs as a copy of KnownCipherSpecs, assuming that
 * validCipherSpecs is currently not valid (i.e., SSLSetEnabledCiphers() has
 * not been called).
 */
SSLErr sslBuildCipherSpecArray(SSLContext *ctx)
{
	unsigned 		size;
	
	CASSERT(ctx != NULL);
	CASSERT(ctx->validCipherSpecs == NULL);
	
	ctx->numValidCipherSpecs = CipherSpecCount;
	size = CipherSpecCount * sizeof(SSLCipherSpec);
	ctx->validCipherSpecs = sslMalloc(size);
	if(ctx->validCipherSpecs == NULL) {
		ctx->numValidCipherSpecs = 0;
		return SSLMemoryErr;
	}
	memmove(ctx->validCipherSpecs, KnownCipherSpecs, size);
	return SSLNoErr;
}

/*
 * Convert an array of SSLCipherSpecs (which is either KnownCipherSpecs or
 * ctx->validCipherSpecs) to an array of SSLCipherSuites.
 */
static OSStatus
cipherSpecsToCipherSuites(
	UInt32				numCipherSpecs,	/* size of cipherSpecs */
	const SSLCipherSpec	*cipherSpecs,
	SSLCipherSuite		*ciphers,		/* RETURNED */
	UInt32				*numCiphers)	/* IN/OUT */
{
	unsigned dex;
	
	if(*numCiphers < numCipherSpecs) {
		return errSSLBufferOverflow;
	}
	for(dex=0; dex<numCipherSpecs; dex++) {
		ciphers[dex] = cipherSpecs[dex].cipherSpec;
	}
	*numCiphers = numCipherSpecs;
	return noErr;
}

/***
 *** Publically exported functions declared in SecureTransport.h
 ***/
 
/*
 * Determine number and values of all of the SSLCipherSuites we support.
 * Caller allocates output buffer for SSLGetSupportedCiphers() and passes in
 * its size in *numCiphers. If supplied buffer is too small, errSSLBufferOverflow
 * will be returned. 
 */
OSStatus
SSLGetNumberSupportedCiphers (SSLContextRef	ctx,
							  UInt32		*numCiphers)
{
	if((ctx == NULL) || (numCiphers == NULL)) {
		return paramErr;
	}
	*numCiphers = CipherSpecCount;
	return noErr;
}
			
OSStatus
SSLGetSupportedCiphers		 (SSLContextRef		ctx,
							  SSLCipherSuite	*ciphers,		/* RETURNED */
							  UInt32			*numCiphers)	/* IN/OUT */
{
	if((ctx == NULL) || (ciphers == NULL) || (numCiphers == NULL)) {
		return paramErr;
	}
	return cipherSpecsToCipherSuites(CipherSpecCount,
		KnownCipherSpecs,
		ciphers,
		numCiphers);
}

/*
 * Specify a (typically) restricted set of SSLCipherSuites to be enabled by
 * the current SSLContext. Can only be called when no session is active. Default
 * set of enabled SSLCipherSuites is the same as the complete set of supported 
 * SSLCipherSuites as obtained by SSLGetSupportedCiphers().
 */
OSStatus 
SSLSetEnabledCiphers		(SSLContextRef			ctx,
							 const SSLCipherSuite	*ciphers,	
							 UInt32					numCiphers)
{
	unsigned 		size;
	unsigned 		callerDex;
	unsigned		tableDex;
	
	if((ctx == NULL) || (ciphers == NULL) || (numCiphers == 0)) {
		return paramErr;
	}
	if(sslIsSessionActive(ctx)) {
		/* can't do this with an active session */
		return badReqErr;
	}
	size = numCiphers * sizeof(SSLCipherSpec);
	ctx->validCipherSpecs = sslMalloc(size);
	if(ctx->validCipherSpecs == NULL) {
		ctx->numValidCipherSpecs = 0;
		return SSLMemoryErr;
	}

	/* 
	 * Run thru caller's specs, finding a matching SSLCipherSpec for each one.
	 * If caller specifies one we don't know about, abort. 
	 */
	for(callerDex=0; callerDex<numCiphers; callerDex++) {
		/* find matching CipherSpec in our known table */
		int foundOne = 0;
		for(tableDex=0; tableDex<CipherSpecCount; tableDex++) {
			if(ciphers[callerDex] == KnownCipherSpecs[tableDex].cipherSpec) {
				ctx->validCipherSpecs[callerDex] = KnownCipherSpecs[tableDex];
				foundOne = 1;
				break;
			}
		}
		if(!foundOne) {
			/* caller specified one we don't implement */
			sslFree(ctx->validCipherSpecs);
			ctx->validCipherSpecs = NULL;
			return errSSLBadCipherSuite;
		}
	}
	
	/* success */
	ctx->numValidCipherSpecs = numCiphers;
	return noErr;
}
							 
/*
 * Determine number and values of all of the SSLCipherSuites currently enabled.
 * Caller allocates output buffer for SSLGetEnabledCiphers() and passes in
 * its size in *numCiphers. If supplied buffer is too small, errSSLBufferOverflow
 * will be returned. 
 */
OSStatus
SSLGetNumberEnabledCiphers 	(SSLContextRef			ctx,
							 UInt32					*numCiphers)
{
	if((ctx == NULL) || (numCiphers == NULL)) {
		return paramErr;
	}
	if(ctx->validCipherSpecs == NULL) {
		/* hasn't been set; use default */
		*numCiphers = CipherSpecCount;
	}
	else {
		/* caller set via SSLSetEnabledCiphers */
		*numCiphers = ctx->numValidCipherSpecs;
	}
	return noErr;
}
			
OSStatus
SSLGetEnabledCiphers		(SSLContextRef			ctx,
							 SSLCipherSuite			*ciphers,		/* RETURNED */
							 UInt32					*numCiphers)	/* IN/OUT */
{
	if((ctx == NULL) || (ciphers == NULL) || (numCiphers == NULL)) {
		return paramErr;
	}
	if(ctx->validCipherSpecs == NULL) {
		/* hasn't been set; use default */
		return cipherSpecsToCipherSuites(CipherSpecCount,
			KnownCipherSpecs,
			ciphers,
			numCiphers);
	}
	else {
		/* use the ones specified in SSLSetEnabledCiphers() */
		return cipherSpecsToCipherSuites(ctx->numValidCipherSpecs,
			ctx->validCipherSpecs,
			ciphers,
			numCiphers);
	}
}

/***
 *** End of publically exported functions declared in SecureTransport.h
 ***/

/*
 * Given a valid ctx->selectedCipher and ctx->validCipherSpecs, set
 * ctx->selectedCipherSpec as appropriate. 
 */
SSLErr
FindCipherSpec(SSLContext *ctx)
{   

	unsigned i;
    
    CASSERT(ctx != NULL);
    CASSERT(ctx->validCipherSpecs != NULL);
    
    ctx->selectedCipherSpec = NULL;
    for (i=0; i<ctx->numValidCipherSpecs; i++)
    {   if (ctx->validCipherSpecs[i].cipherSpec == ctx->selectedCipher) {
        	ctx->selectedCipherSpec = &ctx->validCipherSpecs[i];
            break;
        }
    }    
    if (ctx->selectedCipherSpec == NULL)         /* Not found */
        return SSLNegotiationErr;
    return SSLNoErr;
}

