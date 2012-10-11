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


//
// miscAlgFactory.h - miscellaneous algorithm factory
// Written by Doug Mitchell 3/28/2001
//
#ifndef _MISC_ALG_FACTORY_H_
#define _MISC_ALG_FACTORY_H_

#include <security_cdsa_plugin/CSPsession.h>
#include "AppleCSP.h"

class AppleCSPSession;

class MiscAlgFactory : public AppleCSPAlgorithmFactory {
public:
	
    MiscAlgFactory(
		Allocator *normAlloc = NULL, 
		Allocator *privAlloc = NULL)
		{ }
	~MiscAlgFactory() { }
	
    bool setup(
		AppleCSPSession &session,
		CSPFullPluginSession::CSPContext * &cspCtx, 
		const Context &context);

};

#endif //_MISC_ALG_FACTORY_H_
