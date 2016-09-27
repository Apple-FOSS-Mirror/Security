// ======================================================================
//	File:		Radar.cpp
//
//	Repository of test cases which are entered into Radar.  Use them to
//	reproduce and regress Radar bugs.
//
//	Copyright:	Copyright (c) 2000,2003 Apple Computer, Inc. All Rights Reserved.
//
//	Change History (most recent first):
//
//		 <1>	4/12/00	em		Created.
// ======================================================================

#include "testKeychainAPI.h"
#include "Radar.h"

// ���������������������������������������������������������������������������
// 	� Radar_2456779
// ���������������������������������������������������������������������������
//
//	Wrong error number when creating duplicate keychain
//
void	Radar_2456779(CTestApp *inClient)
{
	inClient->DoRunTestScript("0001");
}

// ���������������������������������������������������������������������������
// 	� Radar_2458217
// ���������������������������������������������������������������������������
//
//	GetData() causes bus error
//
void	Radar_2458217(CTestApp *inClient)
{
	inClient->DoRunTestScript("0002");
	inClient->DoRunTestScript("0003");
	inClient->DoRunTestScript("0004");
}

// ���������������������������������������������������������������������������
// 	� Radar_2458257
// ���������������������������������������������������������������������������
//
//	GetKeychainManagerVersion() returns a wrong version
//
void	Radar_2458257(CTestApp *inClient)
{
	inClient->DoRunTestScript("0000");
}

// ���������������������������������������������������������������������������
// 	� Radar_2458503
// ���������������������������������������������������������������������������
//
//	KCAddItem() fails to detect duplicate items
//
void	Radar_2458503(CTestApp *inClient)
{
	inClient->DoRunTestScript("0008");
}

// ���������������������������������������������������������������������������
// 	� Radar_2458613
// ���������������������������������������������������������������������������
//
//	FindFirstItem returns an item from an empty keychain
//
void	Radar_2458613(CTestApp *inClient)
{
	inClient->DoRunTestScript("0009");
}

// ���������������������������������������������������������������������������
// 	� Radar_2459096
// ���������������������������������������������������������������������������
//
//	InvalidItemRef error when deleting an item not previously added to keychain
//
void	Radar_2459096(CTestApp *inClient)
{
	inClient->DoRunTestScript("0012");
}

// ���������������������������������������������������������������������������
// 	� Radar_2462081
// ���������������������������������������������������������������������������
//
//	AddAppleSharePassword returns DL_INVALID_FIELD_NAME
//
void	Radar_2462081(CTestApp *inClient)
{
	inClient->DoRunTestScript("0013");
}

// ���������������������������������������������������������������������������
// 	� Radar_2462265
// ���������������������������������������������������������������������������
//
//	GetDataUI does not set ActualLength
//
void	Radar_2462265(CTestApp *inClient)
{
	inClient->DoRunTestScript("0027");
}

// ���������������������������������������������������������������������������
// 	� Radar_2462300
// ���������������������������������������������������������������������������
//
//	No dialog for KCChangeSettings
//
void	Radar_2462300(CTestApp *inClient)
{
	inClient->DoRunTestScript("0025");
}
