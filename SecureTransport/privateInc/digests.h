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
	File:		digests.h

	Contains:	HashReference declarations

	Written by:	Doug Mitchell, based on Netscape RSARef 3.0

	Copyright: (c) 1999 by Apple Computer, Inc., all rights reserved.

*/

#ifndef	_DIGESTS_H_
#define _DIGESTS_H_	1

#ifdef __cplusplus
extern "C" {
#endif

extern HashReference SSLHashNull;
extern HashReference SSLHashMD5;
extern HashReference SSLHashSHA1;

extern void SSLInitMACPads(void);
extern SSLErr CloneHashState(
	const HashReference *ref, 
	SSLBuffer state, 
	SSLBuffer *newState, 
	SSLContext *ctx);
extern SSLErr ReadyHash(
	const HashReference *ref, 
	SSLBuffer *state, 
	SSLContext *ctx);


#ifdef __cplusplus
}
#endif

#endif	/* _DIGESTS_H_ */
