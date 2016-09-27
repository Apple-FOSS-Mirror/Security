// ======================================================================
//	File:		KCAPI_Item.h
//
//	Operation classes for APIs to manage KC items
//		- KCNewItem
//		- KCSetAttribute
//		- KCGetAttribute
//		- KCSetData
//		- KCGetData
//		- KCGetDataNoUI
//		- KCAddItem
//		- KCAddItemNoUI
//		- KCDeleteItem
//		- KCDeleteItemNoUI
//		- KCUpdateItem
//		- KCReleaseItem
//		- KCCopyItem
//
// Operation classes for APIs for searching and enumertating KC items
//		- KCFindFirstItem
//		- KCFindNextItem
//		- KCReleaseSearch
//
//	Copyright:	Copyright (c) 2000,2003 Apple Computer, Inc. All Rights Reserved.
//
//	Change History (most recent first):
//
//		 <1>	3/1/00	em		Created.
// ======================================================================

#ifndef __KCAPI_ITEM__
#define __KCAPI_ITEM__

#include "KCOperation.h"
#include "KCOperationID.h"


// ���������������������������������������������������������������������������
// 	� COp_KCNewItem
// ���������������������������������������������������������������������������
class COp_KCNewItem : public KCItemOperation
{
public:
OPERATION_ID(KCNewItem)

								COp_KCNewItem();
	virtual	OSStatus			Operate();

protected:
	CParamKCItemClass			mItemClass;
	CParamOSType				mItemCreator;
	CParamkcBlob				mData;

};

// ���������������������������������������������������������������������������
// 	� COp_KCSetAttribute
// ���������������������������������������������������������������������������
class COp_KCSetAttribute : public KCItemOperation
{
public:
OPERATION_ID(KCSetAttribute)

								COp_KCSetAttribute();
	virtual	OSStatus			Operate();

protected:
	CParamKCAttribute			mAttribute;
};


// ���������������������������������������������������������������������������
// 	� COp_KCGetAttribute
// ���������������������������������������������������������������������������
class COp_KCGetAttribute : public KCItemOperation
{
public:
OPERATION_ID(KCGetAttribute)

								COp_KCGetAttribute();
	virtual	OSStatus			Operate();

protected:
	CParamKCAttribute			mAttribute;
	CParamUInt32				mActualLength;
};


// ���������������������������������������������������������������������������
// 	� COp_KCSetData
// ���������������������������������������������������������������������������
class COp_KCSetData : public KCItemOperation
{
public:
OPERATION_ID(KCSetData)

								COp_KCSetData();
	virtual	OSStatus			Operate();

protected:
	CParamkcBlob				mData;
};

// ���������������������������������������������������������������������������
// 	� COp_KCGetData
// ���������������������������������������������������������������������������
class COp_KCGetData : public KCItemOperation
{
public:
OPERATION_ID(KCGetData)

								COp_KCGetData();
	virtual	OSStatus			Operate();

protected:
	CParamkcBlob				mData;
	CParamUInt32				mActualLength;
};

// ���������������������������������������������������������������������������
// 	� COp_KCGetDataNoUI
// ���������������������������������������������������������������������������
/*
class COp_KCGetDataNoUI : public KCItemOperation
{
public:
OPERATION_ID(KCGetDataNoUI)

								COp_KCGetDataNoUI();
	virtual	OSStatus			Operate();

protected:
	CParamkcBlob				mData;
	CParamUInt32				mActualLength;

*/
/*    static OSStatus				Callback1(
									KCItemRef		*outItemRef, 
									UInt32			*outMaxLength, 
									void 			**outData, 
									UInt32 			**outActualLength, 
									void 			*inContext);
									
    static OSStatus				Callback2(
									void 			*inContext);
*/
//};

// ���������������������������������������������������������������������������
// 	� COp_KCAddItem
// ���������������������������������������������������������������������������
class COp_KCAddItem : public KCItemOperation
{
public:
OPERATION_ID(KCAddItem)

								COp_KCAddItem();
	virtual	OSStatus			Operate();

protected:
};

// ���������������������������������������������������������������������������
// 	� COp_KCAddItemNoUI
// ���������������������������������������������������������������������������
class COp_KCAddItemNoUI : public KCItemOperation
{
public:
OPERATION_ID(KCAddItemNoUI)

								COp_KCAddItemNoUI();
	virtual	OSStatus			Operate();

protected:
    static OSStatus				Callback(
                                    KCItemRef		*outItem, 
                                    void			*inContext);
};

// ���������������������������������������������������������������������������
// 	� COp_KCDeleteItem
// ���������������������������������������������������������������������������
class COp_KCDeleteItem : public KCItemOperation
{
public:
OPERATION_ID(KCDeleteItem)

								COp_KCDeleteItem();
	virtual	OSStatus			Operate();

protected:
};

// ���������������������������������������������������������������������������
// 	� COp_KCDeleteItemNoUI
// ���������������������������������������������������������������������������
class COp_KCDeleteItemNoUI : public KCItemOperation
{
public:
OPERATION_ID(KCDeleteItemNoUI)

								COp_KCDeleteItemNoUI();
	virtual	OSStatus			Operate();

protected:
	static OSStatus				CallBack(
									KCItemRef	*outItem, 
									void		*inContext);
};

// ���������������������������������������������������������������������������
// 	� COp_KCUpdateItem
// ���������������������������������������������������������������������������
class COp_KCUpdateItem : public KCItemOperation
{
public:
OPERATION_ID(KCUpdateItem)

								COp_KCUpdateItem();
	virtual	OSStatus			Operate();

protected:
};

// ���������������������������������������������������������������������������
// 	� COp_KCReleaseItem
// ���������������������������������������������������������������������������
class COp_KCReleaseItem : public KCItemOperation
{
public:
OPERATION_ID(KCReleaseItem)

								COp_KCReleaseItem();
	virtual	OSStatus			Operate();

protected:
};

// ���������������������������������������������������������������������������
// 	� COp_KCCopyItem
// ���������������������������������������������������������������������������
class COp_KCCopyItem : public KCItemOperation
{
public:
OPERATION_ID(KCCopyItem)

								COp_KCCopyItem();
	virtual	OSStatus			Operate();

protected:
};

// ���������������������������������������������������������������������������
// 	� COp_KCFindFirstItem
// ���������������������������������������������������������������������������
class COp_KCFindFirstItem : public KCSearchOperation
{
public:
OPERATION_ID(KCFindFirstItem)

								COp_KCFindFirstItem();
	virtual	OSStatus			Operate();

protected:
	CParamKCAttributeList		mAttrList;
};

// ���������������������������������������������������������������������������
// 	� COp_KCFindNextItem
// ���������������������������������������������������������������������������
class COp_KCFindNextItem : public KCSearchOperation
{
public:
OPERATION_ID(KCFindNextItem)

								COp_KCFindNextItem();
	virtual	OSStatus			Operate();

protected:
};

// ���������������������������������������������������������������������������
// 	� COp_KCReleaseSearch
// ���������������������������������������������������������������������������
class COp_KCReleaseSearch : public KCSearchOperation
{
public:
OPERATION_ID(KCReleaseSearch)

								COp_KCReleaseSearch();
	virtual	OSStatus			Operate();

protected:
};

#endif	// __KCAPI_ITEM__


