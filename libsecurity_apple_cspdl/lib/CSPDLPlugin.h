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
// CSPDLCSPDL.h - File Based CSP/DL plug-in module.
//
#ifndef _H_CSPDLPLUGIN
#define _H_CSPDLPLUGIN

#include "SSCSPDLSession.h"
#include "CSPDLDatabase.h"
#include "SSFactory.h"
#include <security_cdsa_client/cspclient.h>
#include <security_cdsa_plugin/cssmplugin.h>

class SSCSPSession;

class CSPDLPlugin : public CssmPlugin
{
	NOCOPY(CSPDLPlugin)
public:
    CSPDLPlugin();
    ~CSPDLPlugin();

    PluginSession *makeSession(CSSM_MODULE_HANDLE handle,
                               const CSSM_VERSION &version,
                               uint32 subserviceId,
                               CSSM_SERVICE_TYPE subserviceType,
                               CSSM_ATTACH_FLAGS attachFlags,
                               const CSSM_UPCALLS &upcalls);
private:
	friend class SSCSPSession;
	friend class SSCSPDLSession;
	SSCSPDLSession mSSCSPDLSession;
    CSPDLDatabaseManager mDatabaseManager;
    SSFactory mSSFactory;
	CssmClient::CSP mRawCsp;		// raw (nonsecure) CSP connection
};


#endif //_H_CSPDLPLUGIN
