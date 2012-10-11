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
	File:		sslPriv.h

	Contains:	Misc. private SSL typedefs

	Written by:	Doug Mitchell, based on Netscape SSLRef 3.0

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#ifndef	_SSL_PRIV_H_
#define _SSL_PRIV_H_	1

#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacTypes.h>
#include "sslBuildFlags.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Apple CSP doesn't support D-H yet */
#define APPLE_DH		0

/* 
 * For ease of porting, we'll keep this around for internal use.
 * It's used extensively; eventually we'll convert over to
 * CFData, as in the public API.
 */
typedef struct
{   UInt32  length;
    UInt8   *data;
} SSLBuffer;

/*
 * We can make this more Mac-like as well...
 */
typedef struct
{   UInt32  high;
    UInt32  low;
}   sslUint64;

/*
 * Not exposed in public API
 */
typedef enum
{   SSL_ServerSide = 1,
    SSL_ClientSide = 2
} SSLProtocolSide;

typedef enum
{   
	/* These values never appear in the actual protocol */
	SSL_Version_Undetermined = 0,
    SSL_Version_3_0_With_2_0_Hello = 100,
    SSL_Version_3_0_Only = 101,
	TLS_Version_1_0_Only = 202,
	/* actual protocol values */
    SSL_Version_2_0 = 0x0002,
    SSL_Version_3_0 = 0x0300,
	TLS_Version_1_0 = 0x0301		/* TLS 1.0 == SSL 3.1 */
} SSLProtocolVersion;

/*
 * Clients see an opaque SSLContextRef; internal code uses the 
 * following typedef.
 */
typedef struct SSLContext SSLContext;

/*
 * Some hard-coded constants. 
 */

/* The size of of client- and server-generated random numbers in hello messages. */
#define SSL_CLIENT_SRVR_RAND_SIZE		32

/* The size of the pre-master and master secrets. */
#define SSL_RSA_PREMASTER_SECRET_SIZE	48
#define SSL_MASTER_SECRET_SIZE			48

#ifdef __cplusplus
}
#endif

#endif	/* _SSL_PRIV_H */
