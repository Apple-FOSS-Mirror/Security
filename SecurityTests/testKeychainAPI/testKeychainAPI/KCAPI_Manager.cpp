// ======================================================================
//	File:		KCAPI_Manager.cpp
//
//	Operation classes for KC manager APIs:
//		- KCGetKeychainManagerVersion
//		- KeychainManagerAvailable
//
//
//	Copyright:	Copyright (c) 2000,2003 Apple Computer, Inc. All Rights Reserved.
//
//	Change History (most recent first):
//
//		 <1>	2/22/00	em		Created.
// ======================================================================

#include "KCAPI_Manager.h"
#include "KCParamUtility.h"

#if TARGET_RT_MAC_MACHO
	#include <OSServices/KeychainCore.h>
	#include <OSServices/KeychainCorePriv.h>
	#include <SecurityHI/KeychainHI.h>
#else
	#include <Keychain.h>
#endif

// ���������������������������������������������������������������������������
// 	� COp_KCGetKeychainManagerVersion
// ���������������������������������������������������������������������������
COp_KCGetKeychainManagerVersion::COp_KCGetKeychainManagerVersion()
	:mVersion("Version")
{
	AddResult(mVersion);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCGetKeychainManagerVersion::Operate()
{
	mStatus = ::KCGetKeychainManagerVersion((UInt32*)mVersion);
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KeychainManagerAvailable
// ���������������������������������������������������������������������������
COp_KeychainManagerAvailable::COp_KeychainManagerAvailable()
	:mAvailable("Available")
{
	AddResult(mAvailable);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KeychainManagerAvailable::Operate()
{
	mAvailable = ::KeychainManagerAvailable();
	return(noErr);
}
