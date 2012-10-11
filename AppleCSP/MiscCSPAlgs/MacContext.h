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
 * MacContext.h - AppleCSPContext for HMACSHA1
 */

#ifndef	_MAC_CONTEXT_H_
#define _MAC_CONTEXT_H_

#include <AppleCSP/AppleCSPContext.h>
#include <PBKDF2/HMACSHA1.h>

#define HMAC_MIN_KEY_SIZE		20		/* in bytes */
#define HMAC_MAX_KEY_SIZE		2048


class MacContext : public AppleCSPContext  {
public:
	MacContext(
		AppleCSPSession &session) : 
			AppleCSPContext(session), mHmac(NULL) { }
	~MacContext();
	
	/* called out from CSPFullPluginSession....
	 * both generate and verify: */
	void init(const Context &context, bool isSigning);
	void update(const CssmData &data);
	
	/* generate only */
	void final(CssmData &out);	
	
	/* verify only */
	void final(const CssmData &in);	

	size_t outputSize(bool final, size_t inSize);

private:
	hmacContextRef	mHmac;
};

#ifdef	CRYPTKIT_CSP_ENABLE
#include <CryptKit/HmacSha1Legacy.h>

/* This version is bug-for-bug compatible with a legacy implementation */

class MacLegacyContext : public AppleCSPContext  {
public:
	MacLegacyContext(
		AppleCSPSession &session) : 
			AppleCSPContext(session), mHmac(NULL) { }
	~MacLegacyContext();
	
	/* called out from CSPFullPluginSession....
	 * both generate and verify: */
	void init(const Context &context, bool isSigning);
	void update(const CssmData &data);
	
	/* generate only */
	void final(CssmData &out);	
	
	/* verify only */
	void final(const CssmData &in);	

	size_t outputSize(bool final, size_t inSize);

private:
	hmacLegacyContextRef	mHmac;
};

#endif	/* CRYPTKIT_CSP_ENABLE */

#endif	/* _MAC_CONTEXT_H_ */
