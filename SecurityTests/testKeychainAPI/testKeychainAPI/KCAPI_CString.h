// ======================================================================
//	File:		KCAPI_CString.h
//
//	Operation classes for KC Manager APIs that use "C" strings
//		- kcunlock
//		- kccreatekeychain
//		- kcgetkeychainname
//		- kcaddapplesharepassword
//		- kcfindapplesharepassword
//		- kcaddinternetpassword
//		- kcaddinternetpasswordwithpath
//		- kcfindinternetpassword
//		- kcfindinternetpasswordwithpath
//		- kcaddgenericpassword
//		- kcfindgenericpassword
//
//
//	Copyright:	Copyright (c) 2000,2003 Apple Computer, Inc. All Rights Reserved.
//
//	Change History (most recent first):
//
//		 <1>	3/1/00	em		Created.
// ======================================================================
#ifndef __KCAPI_CSTRING__
#define __KCAPI_CSTRING__
#include "KCOperation.h"
#include "KCOperationID.h"
// ���������������������������������������������������������������������������
// 	� COp_kcunlock
// ���������������������������������������������������������������������������
class COp_kcunlock : public KCOperation
{
public:
OPERATION_ID(kcunlock)
								COp_kcunlock();
	virtual	OSStatus			Operate();
protected:
    CParamStringPtr				mPassword;
};

// ���������������������������������������������������������������������������
// 	� COp_kccreatekeychain
// ���������������������������������������������������������������������������
class COp_kccreatekeychain : public KCOperation
{
public:
OPERATION_ID(kccreatekeychain)
								COp_kccreatekeychain();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mPassword;
};

// ���������������������������������������������������������������������������
// 	� COp_kcgetkeychainname
// ���������������������������������������������������������������������������
class COp_kcgetkeychainname : public KCOperation
{
public:
OPERATION_ID(kcgetkeychainname)
								COp_kcgetkeychainname();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mKeychainName;
};
// ���������������������������������������������������������������������������
// 	� COp_kcaddapplesharepassword
// ���������������������������������������������������������������������������
class COp_kcaddapplesharepassword : public KCItemOperation
{
public:
OPERATION_ID(kcaddapplesharepassword)
								COp_kcaddapplesharepassword();
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
// 	� COp_kcfindapplesharepassword
// ���������������������������������������������������������������������������
class COp_kcfindapplesharepassword : public KCItemOperation
{
public:
OPERATION_ID(kcfindapplesharepassword)
								COp_kcfindapplesharepassword();
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
// 	� COp_kcaddinternetpassword
// ���������������������������������������������������������������������������
class COp_kcaddinternetpassword : public KCItemOperation
{
public:
OPERATION_ID(kcaddinternetpassword)
								COp_kcaddinternetpassword();
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
// 	� COp_kcaddinternetpasswordwithpath
// ���������������������������������������������������������������������������
class COp_kcaddinternetpasswordwithpath : public KCItemOperation
{
public:
OPERATION_ID(kcaddinternetpasswordwithpath)
								COp_kcaddinternetpasswordwithpath();
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
// 	� COp_kcfindinternetpassword
// ���������������������������������������������������������������������������
class COp_kcfindinternetpassword : public KCItemOperation
{
public:
OPERATION_ID(kcfindinternetpassword)
								COp_kcfindinternetpassword();
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
// 	� COp_kcfindinternetpasswordwithpath
// ���������������������������������������������������������������������������
class COp_kcfindinternetpasswordwithpath : public KCItemOperation
{
public:
OPERATION_ID(kcfindinternetpasswordwithpath)
								COp_kcfindinternetpasswordwithpath();
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
// 	� COp_kcaddgenericpassword
// ���������������������������������������������������������������������������
class COp_kcaddgenericpassword : public KCItemOperation
{
public:
OPERATION_ID(kcaddgenericpassword)
								COp_kcaddgenericpassword();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mServiceName;
	CParamStringPtr				mAccountName; 
	CParamkcBlob				mPassword; 
};

// ���������������������������������������������������������������������������
// 	� COp_kcfindgenericpassword
// ���������������������������������������������������������������������������
class COp_kcfindgenericpassword : public KCItemOperation
{
public:
OPERATION_ID(kcfindgenericpassword)
								COp_kcfindgenericpassword();
	virtual	OSStatus			Operate();
protected:
	CParamStringPtr				mServiceName;
	CParamStringPtr				mAccountName; 
	CParamkcBlob				mPassword; 
	CParamUInt32				mActualLength; 
};
#endif	// __KCAPI_CSTRING__
