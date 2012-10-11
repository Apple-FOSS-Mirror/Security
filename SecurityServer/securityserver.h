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
// securityserver.h - master header file for the SecurityServer.
//
#ifndef _H_SECURITYSERVER
#define _H_SECURITYSERVER

#include "ssblob.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Security/cssm.h>
#include <Security/utilities.h>
#include <Security/context.h>
#include <Security/SecurityServerClient.h>
#include <Security/mach++.h>
#include <Security/unix++.h>
#include <Security/debugging.h>
#include <Security/logging.h>

namespace Security {

using namespace SecurityServer;
using namespace UnixPlusPlus;


//
// Logging and verbosity levels
//
extern uint32 debugMode;

} // end namespace Security

#endif //_H_SECURITYSERVER
