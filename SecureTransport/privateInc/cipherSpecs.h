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
	File:		cipherSpecs.h

	Contains:	SSLCipherSpec declarations

	Written by:	Doug Mitchell, based on Netscape RSARef 3.0

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#ifndef	_CIPHER_SPECS_H_
#define _CIPHER_SPECS_H_

#ifndef _SSLCTX_H_
#include "sslctx.h"
#endif

#ifndef _CRYPTTYPE_H_
#include "cryptType.h"
#endif

/* 
 * Build ctx->validCipherSpecs as a copy of all known CipherSpecs. 
 */
extern SSLErr sslBuildCipherSpecArray(SSLContext *ctx);

/*
 * Given a valid ctx->selectedCipher and ctx->validCipherSpecs, set
 * ctx->selectedCipherSpec as appropriate. 
 */
SSLErr  FindCipherSpec(SSLContext *ctx);

extern const SSLCipherSpec SSL_NULL_WITH_NULL_NULL_CipherSpec;


#endif	/* _CIPHER_SPECS_H_ */
