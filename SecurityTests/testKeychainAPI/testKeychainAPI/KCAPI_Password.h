// ======================================================================
//	File:		KCAPI_Password.h
//
//	Operation classes for APIs to store and retriev passwords
//		- KCAddAppleSharePassword
//		- KCFindAppleSharePassword
//		- KCAddInternetPassword
//		- KCAddInternetPasswordWithPath
//		- KCFindInternetPassword
//		- KCFindInternetPasswordWithPath
//		- KCAddGenericPassword
//		- KCFindGenericPassword
//
//
//	Copyright:	Copyright (c) 2000,2003 Apple Computer, Inc. All Rights Reserved.
//
//	Change History (most recent first):
//
//		 <1>	3/1/00	em		Created.
// ======================================================================

#ifndef __KCAPI_PASSWORD__
#define __KCAPI_PASSWORD__
#include "KCOperation.h"
#include "KCOperationID.h"

// ���������������������������������������������������������������������������
// 	� COp_KCAddAppleSharePassword
// ���������������������������������������������������������������������������
class COp_KCAddAppleSharePassword : public KCItemOperation
{
public:
OPERATION_ID(KCAddAppleSharePassword)
								COp_KCAddAppleSharePassword();
	virtual	OSStatus			Operate();
protected:
	CParamAFPServerSignature	mServerSignature; 
	CParamStringPtr				mServerAddress;
	CParamStringPtr				mServerName; 
	CParamStringPtr				mVolumeName; 
	CParamStringPtr				mAccountName; 
	CParamkcBlob				mPassword; 
};

// ���������������������������������������������������������������������������
// 	� COp_KCFindAppleSharePassword
// ���������������������������������������������������������������������������
class COp_KCFindAppleSharePassword : public KCItemOperation
{
public:
OPERATION_ID(KCFindAppleSharePassword)
								COp_KCFindAppleSharePassword();
	virtual	OSStatus			Operate();
	
protected:
	CParamAFPServerSignature	mServerSignature; 
	CParamStringPtr				mServerAddress;
	CParamStringPtr				mServerName; 
	CParamStringPtr				mVolumeName; 
	CParamStringPtr				mAccountName; 
	CParamkcBlob				mPassword; 
	CParamUInt32				mActualLength; 
};

// ���������������������������������������������������������������������������
// 	� COp_KCAddInternetPassword
// ���������������������������������������������������������������������������
class COp_KCAddInternetPassword : public KCItemOperation
{
public:
OPERATION_ID(KCAddInternetPassword)
								COp_KCAddInternetPassword();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mServerName;
	CParamStringPtr				mSecurityDomain;
	CParamStringPtr				mAccountName;
	CParamUInt16				mPort;
	CParamOSType				mProtocol;
	CParamOSType				mAuthType;
	CParamkcBlob				mPassword;

};

// ���������������������������������������������������������������������������
// 	� COp_KCAddInternetPasswordWithPath
// ���������������������������������������������������������������������������
class COp_KCAddInternetPasswordWithPath : public KCItemOperation
{
public:
OPERATION_ID(KCAddInternetPasswordWithPath)
								COp_KCAddInternetPasswordWithPath();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mServerName;
	CParamStringPtr				mSecurityDomain; 
	CParamStringPtr				mAccountName; 
	CParamStringPtr				mPath; 
	CParamUInt16				mPort; 
	CParamOSType				mProtocol; 
	CParamOSType				mAuthType; 
	CParamkcBlob				mPassword; 
};

// ���������������������������������������������������������������������������
// 	� COp_KCFindInternetPassword
// ���������������������������������������������������������������������������
class COp_KCFindInternetPassword : public KCItemOperation
{
public:
OPERATION_ID(KCFindInternetPassword)
								COp_KCFindInternetPassword();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mServerName;
	CParamStringPtr				mSecurityDomain; 
	CParamStringPtr				mAccountName; 
	CParamUInt16				mPort; 
	CParamOSType				mProtocol; 
	CParamOSType				mAuthType; 
	CParamkcBlob				mPassword; 
	CParamUInt32				mActualLength; 
};

// ���������������������������������������������������������������������������
// 	� COp_KCFindInternetPasswordWithPath
// ���������������������������������������������������������������������������
class COp_KCFindInternetPasswordWithPath : public KCItemOperation
{
public:
OPERATION_ID(KCFindInternetPasswordWithPath)
								COp_KCFindInternetPasswordWithPath();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mServerName;
	CParamStringPtr				mSecurityDomain; 
	CParamStringPtr				mAccountName; 
	CParamStringPtr				mPath; 
	CParamUInt16				mPort; 
	CParamOSType				mProtocol; 
	CParamOSType				mAuthType; 
	CParamkcBlob				mPassword; 
	CParamUInt32				mActualLength; 
};

// ���������������������������������������������������������������������������
// 	� COp_KCAddGenericPassword
// ���������������������������������������������������������������������������
class COp_KCAddGenericPassword : public KCItemOperation
{
public:
OPERATION_ID(KCAddGenericPassword)
								COp_KCAddGenericPassword();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mServiceName;
	CParamStringPtr				mAccountName; 
	CParamkcBlob				mPassword; 
};

// ���������������������������������������������������������������������������
// 	� COp_KCFindGenericPassword
// ���������������������������������������������������������������������������
class COp_KCFindGenericPassword : public KCItemOperation
{
public:
OPERATION_ID(KCFindGenericPassword)
								COp_KCFindGenericPassword();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mServiceName;
	CParamStringPtr				mAccountName; 
	CParamkcBlob				mPassword; 
	CParamUInt32				mActualLength; 
};
#endif	// __KCAPI_PASSWORD__
