/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All Rights Reserved.
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


//
// cssmmds - MDS interface for CSSM & friends  
//
#include "cssmmds.h"
#include <ctype.h>
#include <security_cdsa_client/dlquery.h>


//
// Construct a MdsComponent.
// This will perform an MDS lookup in the Common table
//
MdsComponent::MdsComponent(const Guid &guid) : mMyGuid(guid)
{
	using namespace MDSClient;
	Table<Common> common(mds());	// MDS "CDSA Common" table
	mCommon = common.fetch(Attribute("ModuleID") == mMyGuid, CSSMERR_CSSM_MDS_ERROR);
}

MdsComponent::~MdsComponent()
{
}
