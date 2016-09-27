// ======================================================================
//	File:		KCAPI_Keychain.h
//
//	Operation classes for core KC APIs:
//		- KCMakeKCRefFromFSRef
//		- KCMakeKCRefFromFSSpec
//		- KCMakeKCRefFromAlias
//		- KCMakeAliasFromKCRef
//		- KCReleaseKeychain
//		- KCUnlockNoUI
//		- KCUnlock
//		- KCLogin
//		- KCChangeLoginPassword
//		- KCLogout
//		- KCUnlockWithInfo
//		- KCLock
//		- KCLockNoUI
//		- KCGetDefaultKeychain
//		- KCSetDefaultKeychain
//		- KCCreateKeychain
//		- KCCreateKeychainNoUI
//		- KCGetStatus
//		- KCChangeSettingsNoUI
//		- KCGetKeychain
//		- KCGetKeychainName
//		- KCChangeSettings
//		- KCCountKeychains
//		- KCGetIndKeychain
//		- KCAddCallback
//		- KCRemoveCallback
//		- KCSetInteractionAllowed
//		- KCIsInteractionAllowed
//
//	Copyright:	Copyright (c) 2000,2003 Apple Computer, Inc. All Rights Reserved.
//
//	Change History (most recent first):
//
//		 <1>	2/25/00	em		Created.
// ======================================================================
#ifndef __KCAPI_KEYCHAIN__
#define __KCAPI_KEYCHAIN__
#include "KCOperation.h"
#include "KCOperationID.h"
#include "KCParamUtility.h"

// ���������������������������������������������������������������������������
// 	� COp_KCMakeKCRefFromFSRef
// ���������������������������������������������������������������������������
class COp_KCMakeKCRefFromFSRef : public KCOperation
{
public:
OPERATION_ID(KCMakeKCRefFromFSRef)
								COp_KCMakeKCRefFromFSRef();
	virtual	OSStatus			Operate();

protected:
	CParamFSRef					mFSRef;
private:
	OSStatus					KCMakeKCRefFromFSRef(
									FSRef *inKeychainFSRef,
									KCRef *outKeychain)
								{	
									*outKeychain = (KCRef)NULL;
									return noErr;
								}	
};
// ���������������������������������������������������������������������������
// 	� COp_KCMakeKCRefFromFSSpec
// ���������������������������������������������������������������������������
class COp_KCMakeKCRefFromFSSpec : public KCOperation
{
public:
OPERATION_ID(KCMakeKCRefFromFSSpec)
								COp_KCMakeKCRefFromFSSpec();						
	virtual	OSStatus			Operate();
protected:
	CParamFSSpec				mKeychainFile;
};
// ���������������������������������������������������������������������������
// 	� COp_KCMakeKCRefFromAlias
// ���������������������������������������������������������������������������
class COp_KCMakeKCRefFromAlias : public KCOperation
{
public:
OPERATION_ID(KCMakeKCRefFromAlias)
								COp_KCMakeKCRefFromAlias();						
	virtual	OSStatus			Operate();
protected:
};
// ���������������������������������������������������������������������������
// 	� COp_KCMakeAliasFromKCRef
// ���������������������������������������������������������������������������
class COp_KCMakeAliasFromKCRef : public KCOperation
{
public:
OPERATION_ID(KCMakeAliasFromKCRef)
								
								COp_KCMakeAliasFromKCRef();
	virtual	OSStatus			Operate();
protected:
};
// ���������������������������������������������������������������������������
// 	� COp_KCReleaseKeychain
// ���������������������������������������������������������������������������
class COp_KCReleaseKeychain : public KCOperation
{
public:
OPERATION_ID(KCReleaseKeychain)
								
								COp_KCReleaseKeychain();
	virtual	OSStatus			Operate();
protected:
};
// ���������������������������������������������������������������������������
// 	� COp_KCUnlockNoUI
// ���������������������������������������������������������������������������
class COp_KCUnlockNoUI : public KCOperation
{
public:
OPERATION_ID(KCUnlockNoUI)
								
								COp_KCUnlockNoUI();
	virtual	OSStatus			Operate();
    virtual StringPtr			GetPassword(){ return (StringPtr)mPassword; }
protected:
    CParamStringPtr				mPassword;
};
// ���������������������������������������������������������������������������
// 	� COp_KCUnlock
// ���������������������������������������������������������������������������
class COp_KCUnlock : public KCOperation
{
public:
OPERATION_ID(KCUnlock)
								
								COp_KCUnlock();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mPassword;
};	
// ���������������������������������������������������������������������������
// 	� COp_KCChangeLoginPassword
// ���������������������������������������������������������������������������
class COp_KCChangeLoginPassword : public KCOperation
{
public:
OPERATION_ID(KCChangeLoginPassword)
								
								COp_KCChangeLoginPassword();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mOldPassword;
	CParamStringPtr				mNewPassword;
};	
// ���������������������������������������������������������������������������
// 	� COp_KCLogin
// ���������������������������������������������������������������������������
class COp_KCLogin : public KCOperation
{
public:
OPERATION_ID(KCLogin)
								
								COp_KCLogin();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mName;
	CParamStringPtr				mPassword;
};	
// ���������������������������������������������������������������������������
// 	� COp_KCLogout
// ���������������������������������������������������������������������������
class COp_KCLogout : public KCOperation
{
public:
OPERATION_ID(KCLogout)
								
								COp_KCLogout();
	virtual	OSStatus			Operate();
protected:
};	
// ���������������������������������������������������������������������������
// 	� COp_KCUnlockWithInfo
// ���������������������������������������������������������������������������
class COp_KCUnlockWithInfo : public KCOperation
{
public:
OPERATION_ID(KCUnlockWithInfo)
								COp_KCUnlockWithInfo();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mPassword;
	CParamStringPtr				mMessage;
};										
// ���������������������������������������������������������������������������
// 	� COp_KCLock
// ���������������������������������������������������������������������������
class COp_KCLock : public KCOperation
{
public:
OPERATION_ID(KCLock)
								COp_KCLock();
	virtual	OSStatus			Operate();
};
// ���������������������������������������������������������������������������
// 	� COp_KCLockNoUI
// ���������������������������������������������������������������������������
/*
class COp_KCLockNoUI : public KCOperation
{
public:
OPERATION_ID(KCLockNoUI)
								COp_KCLockNoUI();
	virtual	OSStatus			Operate();
protected:
};
*/										
// ���������������������������������������������������������������������������
// 	� COp_KCGetDefaultKeychain
// ���������������������������������������������������������������������������
class COp_KCGetDefaultKeychain : public KCOperation
{
public:
OPERATION_ID(KCGetDefaultKeychain)
								COp_KCGetDefaultKeychain();
	virtual	OSStatus			Operate();
};
// ���������������������������������������������������������������������������
// 	� COp_KCSetDefaultKeychain
// ���������������������������������������������������������������������������
class COp_KCSetDefaultKeychain : public KCOperation
{
public:
OPERATION_ID(KCSetDefaultKeychain)
								COp_KCSetDefaultKeychain();
	virtual	OSStatus			Operate();
};
// ���������������������������������������������������������������������������
// 	� COp_KCCreateKeychain
// ���������������������������������������������������������������������������
class COp_KCCreateKeychain : public KCOperation
{
public:
OPERATION_ID(KCCreateKeychain)
								COp_KCCreateKeychain();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mPassword;
};
// ���������������������������������������������������������������������������
// 	� COp_KCCreateKeychainNoUI
// ���������������������������������������������������������������������������
class COp_KCCreateKeychainNoUI : public KCOperation
{
public:
OPERATION_ID(KCCreateKeychainNoUI)
								COp_KCCreateKeychainNoUI();
	virtual	OSStatus			Operate();
    
    virtual StringPtr			GetPassword(){ return (StringPtr)mPassword; }
    virtual	KCRef *				GetKeychainInCallback(){ return &mKeychainInCallback; }
protected:
    CParamStringPtr				mPassword;
	KCRef						mKeychainInCallback;
    static OSStatus				Callback(
                                    KCRef			*outKeychain, 
                                    StringPtr		*outPassword, 
                                    void			*inContext);
};
// ���������������������������������������������������������������������������
// 	� COp_KCGetStatus
// ���������������������������������������������������������������������������
class COp_KCGetStatus : public KCOperation
{
public:
OPERATION_ID(KCGetStatus)
								COp_KCGetStatus();
	virtual	OSStatus			Operate();
protected:
	CParamUInt32				mKeychainStatus;
};
// ���������������������������������������������������������������������������
// 	� COp_KCChangeSettingsNoUI
// ���������������������������������������������������������������������������
class COp_KCChangeSettingsNoUI : public KCOperation
{
public:
OPERATION_ID(KCChangeSettingsNoUI)
									COp_KCChangeSettingsNoUI();
	virtual	OSStatus				Operate();
	
#if TARGET_RT_MAC_MACHO
	virtual KCChangeSettingsInfo	
                *GetChangeSettingsInfoPtr(){ return &mChangeSettingsInfo; }
#endif

protected:
#if TARGET_RT_MAC_MACHO
    static OSStatus				Callback(
                                    KCChangeSettingsInfo	*outSettings, 
                                    void					*inContext);
#endif

	CParamBoolean				mLockOnSleep;
	CParamBoolean				mUseKCGetDataSound;
	CParamBoolean				mUseKCGetDataAlert;
	CParamBoolean				mUseLockInterval;
	CParamUInt32				mLockInterval;
	CParamStringPtr				mNewPassword;
	CParamStringPtr				mOldPassword;

#if TARGET_RT_MAC_MACHO
	KCChangeSettingsInfo		mChangeSettingsInfo;
#endif
};
// ���������������������������������������������������������������������������
// 	� COp_KCGetKeychain
// ���������������������������������������������������������������������������
class COp_KCGetKeychain : public KCItemOperation
{
public:
OPERATION_ID(KCGetKeychain)
								COp_KCGetKeychain();
	virtual	OSStatus			Operate();
protected:
};
// ���������������������������������������������������������������������������
// 	� COp_KCGetKeychainName
// ���������������������������������������������������������������������������
class COp_KCGetKeychainName : public KCOperation
{
public:
OPERATION_ID(KCGetKeychainName)
								COp_KCGetKeychainName();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mKeychainName;
};
// ���������������������������������������������������������������������������
// 	� COp_KCChangeSettings
// ���������������������������������������������������������������������������
class COp_KCChangeSettings : public KCOperation
{
public:
OPERATION_ID(KCChangeSettings)
								COp_KCChangeSettings();
	virtual	OSStatus			Operate();
protected:
};
// ���������������������������������������������������������������������������
// 	� COp_KCCountKeychains
// ���������������������������������������������������������������������������
class COp_KCCountKeychains : public KCOperation
{
public:
OPERATION_ID(KCCountKeychains)
								COp_KCCountKeychains();
	virtual	OSStatus			Operate();

protected:
	CParamUInt16				mCount;
};

// ���������������������������������������������������������������������������
// 	� COp_KCGetIndKeychain
// ���������������������������������������������������������������������������
class COp_KCGetIndKeychain : public KCOperation
{
public:
OPERATION_ID(KCGetIndKeychain)
								COp_KCGetIndKeychain();
	virtual	OSStatus			Operate();

protected:
	CParamUInt16				mIndex;
};

// ���������������������������������������������������������������������������
// 	� COp_KCAddCallback
// ���������������������������������������������������������������������������
class COp_KCAddCallback : public KCOperation
{
public:
OPERATION_ID(KCAddCallback)
								COp_KCAddCallback();
	virtual	OSStatus			Operate();

protected:
	CParamUInt16				mEvent;
	static UInt32				sCounter[11];
	static KCCallbackUPP		sCallbacks[11];

#define KCADDCALLBACK_DEF(N) \
	static OSStatus				Callback ## N( \
									KCEvent			inKeychainEvent, \
									KCCallbackInfo	*inInfo, \
									void			*inContext)

	KCADDCALLBACK_DEF(0);
	KCADDCALLBACK_DEF(1);
	KCADDCALLBACK_DEF(2);
	KCADDCALLBACK_DEF(3);
	KCADDCALLBACK_DEF(4);
	KCADDCALLBACK_DEF(5);
	KCADDCALLBACK_DEF(6);
	KCADDCALLBACK_DEF(7);
	KCADDCALLBACK_DEF(8);
	KCADDCALLBACK_DEF(9);
	KCADDCALLBACK_DEF(10);
#undef KCADDCALLBACK_DEF


	friend class COp_KCRemoveCallback;
};

// ���������������������������������������������������������������������������
// 	� COp_KCRemoveCallback
// ���������������������������������������������������������������������������
class COp_KCRemoveCallback : public KCOperation
{
public:
OPERATION_ID(KCRemoveCallback)
								COp_KCRemoveCallback();
	virtual	OSStatus			Operate();
protected:
	CParamUInt16				mEvent;
	CParamUInt32				mIdleCount;
	CParamUInt32				mLockCount;
	CParamUInt32				mUnlockCount;
	CParamUInt32				mAddCount;
	CParamUInt32				mDeleteCount;
	CParamUInt32				mUpdateCount;
	CParamUInt32				mChangeIdentityCount;
	CParamUInt32				mFindCount;
	CParamUInt32				mSystemCount;
	CParamUInt32				mDefaultChangedCount;
	CParamUInt32				mDataAccessCount;
};

// ���������������������������������������������������������������������������
// 	� COp_KCSetInteractionAllowed
// ���������������������������������������������������������������������������
class COp_KCSetInteractionAllowed : public KCOperation
{
public:
OPERATION_ID(KCSetInteractionAllowed)
								COp_KCSetInteractionAllowed();
	virtual	OSStatus			Operate();
protected:
	CParamBoolean				mAllow;
};
// ���������������������������������������������������������������������������
// 	� COp_KCIsInteractionAllowed
// ���������������������������������������������������������������������������
class COp_KCIsInteractionAllowed : public KCOperation
{
public:
OPERATION_ID(KCIsInteractionAllowed)
								COp_KCIsInteractionAllowed();
	virtual	OSStatus			Operate();
protected:
	CParamBoolean				mAllow;
};
#endif	// __KCAPI_KEYCHAIN__
