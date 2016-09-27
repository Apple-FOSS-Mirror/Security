// ======================================================================
//	File:		KCAPI_Item.cpp
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
//
//	Copyright:	Copyright (c) 2000,2003 Apple Computer, Inc. All Rights Reserved.
//
//	Change History (most recent first):
//
//		 <1>	3/1/00	em		Created.
// ======================================================================


#include "KCAPI_Item.h"
#include "KCParamUtility.h"

#if TARGET_RT_MAC_MACHO
	#include <OSServices/KeychainCore.h>
	#include <OSServices/KeychainCorePriv.h>
	#include <SecurityHI/KeychainHI.h>
#else
	#include <Keychain.h>
#endif



// ���������������������������������������������������������������������������
// 	� COp_KCNewItem
// ���������������������������������������������������������������������������
COp_KCNewItem::COp_KCNewItem()
	:mItemClass("Class"),
	mItemCreator("Creator"),
	mData("Data")
{
    AddParam(mItemClass);
    AddParam(mItemCreator);
    AddParam(mData);
    
    AddResult(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCNewItem::Operate()
{
	KCItemRef	aKCItemRef = NULL;
	mStatus = ::KCNewItem(
					(KCItemClass)mItemClass,
					(OSType)mItemCreator,
					(UInt32)((kcBlob*)mData)->length,
					(const void *)((kcBlob*)mData)->data,
					(KCItemRef *)&aKCItemRef);

	AddItem(aKCItemRef);				
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCSetAttribute
// ���������������������������������������������������������������������������
COp_KCSetAttribute::COp_KCSetAttribute()
	:mAttribute("Attribute")
{
    AddParam(mItemIndex);
    AddParam(mAttribute);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCSetAttribute::Operate()
{
	mStatus = ::KCSetAttribute(
					(KCItemRef)GetItem(),
					(KCAttribute *)mAttribute);
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCGetAttribute
// ���������������������������������������������������������������������������
COp_KCGetAttribute::COp_KCGetAttribute()
	:mAttribute("Attribute"),
	mActualLength("ActualLength")
{
    AddParam(mItemIndex);
    AddParam(mAttribute);

    AddResult(mAttribute);
    AddResult(mActualLength);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCGetAttribute::Operate()
{
	mStatus = ::KCGetAttribute(
					(KCItemRef)GetItem(),
					(KCAttribute *)mAttribute,
					(UInt32 *)mActualLength);
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCSetData
// ���������������������������������������������������������������������������
COp_KCSetData::COp_KCSetData()
	:mData("Data")
{
    AddParam(mItemIndex);
    AddParam(mData);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCSetData::Operate()
{
	mStatus = ::KCSetData(
					(KCItemRef)GetItem(),
					(UInt32)((kcBlob*)mData)->length,
					(const void *)((kcBlob*)mData)->data);
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCGetData
// ���������������������������������������������������������������������������
COp_KCGetData::COp_KCGetData()
	:mData("Data"),
	mActualLength("ActualLength")
{
    AddParam(mItemIndex);
    AddParam(mData);
	
    AddResult(mData);
    AddResult(mActualLength);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCGetData::Operate()
{	
	mStatus = ::KCGetData(
					(KCItemRef)GetItem(),
					(UInt32)((kcBlob*)mData)->length,
					(void *)((kcBlob*)mData)->data,
					(UInt32 *)mActualLength);

	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCGetDataNoUI
// ���������������������������������������������������������������������������
/*
COp_KCGetDataNoUI::COp_KCGetDataNoUI()
	:mData("Data"),
	mActualLength("ActualLength")
{
    AddParam(mItemIndex);
    AddParam(mData);
	
    AddResult(mData);
    AddResult(mActualLength);
}
*/
// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
/*
OSStatus
COp_KCGetDataNoUI::Operate()
{
//#if TARGET_RT_MAC_MACHO
	mStatus = ::KCGetDataNoUI((KCItemRef)GetItem(),
					(UInt32)((kcBlob*)mData)->length,
					(void *)((kcBlob*)mData)->data,
					(UInt32 *)mActualLength);
//#else
//	throw("KCGetDataNoUI is not implemented");
//#endif
	return(mStatus);
}
*/
/*
// ���������������������������������������������������������������������������
// 	� Callback1
// ���������������������������������������������������������������������������
OSStatus
COp_KCGetDataNoUI::Callback1(
		KCItemRef		*outItemRef, 
		UInt32			*outMaxLength, 
		void 			**outData, 
		UInt32 			**outActualLength, 
		void 			*inContext)
{
    COp_KCGetDataNoUI	*thisObject = static_cast<COp_KCGetDataNoUI*>(inContext);
    if(thisObject == NULL) return -1;
    
	*outItemRef = thisObject->GetItem();
	*outMaxLength = thisObject->GetMaxLength();
    *outData = thisObject->GetDataPtr();
    *outActualLength = thisObject->GetActualLengthPtr();
	return(noErr);
}

// ���������������������������������������������������������������������������
// 	� CallBack2
// ���������������������������������������������������������������������������
OSStatus
COp_KCGetDataNoUI::Callback2(
		void 			*inContext)
{
	return noErr;
}
*/

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCAddItem
// ���������������������������������������������������������������������������
COp_KCAddItem::COp_KCAddItem()
{
    AddParam(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCAddItem::Operate()
{
	mStatus = ::KCAddItem((KCItemRef)GetItem());
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCAddItemNoUI
// ���������������������������������������������������������������������������
COp_KCAddItemNoUI::COp_KCAddItemNoUI()
{
    AddParam(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCAddItemNoUI::Operate()
{	
#if TARGET_RT_MAC_MACHO
	KCRef keychainRef=NULL;	//%%% add test for non-default keychain
	mStatus = ::KCAddItemNoUI(keychainRef,GetItem());
#else
	throw("COp_KCAddItemNoUI is not implemented");
#endif
	return(mStatus);
}

/*
// ���������������������������������������������������������������������������
// 	� COp_KCAddItemNoUI
// ���������������������������������������������������������������������������
OSStatus
COp_KCAddItemNoUI::Callback(
	KCItemRef	*outItem,
	void		*inContext)
{
    COp_KCAddItemNoUI	*thisObject = static_cast<COp_KCAddItemNoUI*>(inContext);
    if(thisObject == NULL) return -1;

	*outItem = thisObject->GetItem();
	return noErr;
}
*/

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCDeleteItem
// ���������������������������������������������������������������������������
COp_KCDeleteItem::COp_KCDeleteItem()
{
    AddParam(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCDeleteItem::Operate()
{
	mStatus = ::KCDeleteItem(GetItem());
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCDeleteItemNoUI
// ���������������������������������������������������������������������������
COp_KCDeleteItemNoUI::COp_KCDeleteItemNoUI()
{
    AddParam(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCDeleteItemNoUI::Operate()
{
#if TARGET_RT_MAC_MACHO
//%%%couldn't link?
//	mStatus = ::KCDeleteItemNoUI(
//						(KCDeleteItemNoUIProcPtr)COp_KCDeleteItemNoUI::CallBack,
//						(void*)this);
	throw("KCDeleteItemNoUI is not implemented");
#else
	throw("KCDeleteItemNoUI is not implemented");
#endif
	return(mStatus);
}

// ���������������������������������������������������������������������������
// 	� CallBack
// ���������������������������������������������������������������������������
OSStatus		
COp_KCDeleteItemNoUI::CallBack(
		KCItemRef	*outItem, 
		void		*inContext)
{
    COp_KCAddItemNoUI	*thisObject = static_cast<COp_KCAddItemNoUI*>(inContext);
    if(thisObject == NULL) return -1;

	*outItem = thisObject->GetItem();
	return noErr;
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCUpdateItem
// ���������������������������������������������������������������������������
COp_KCUpdateItem::COp_KCUpdateItem()
{
    AddParam(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCUpdateItem::Operate()
{
	mStatus = ::KCUpdateItem((KCItemRef)GetItem());
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCReleaseItem
// ���������������������������������������������������������������������������
COp_KCReleaseItem::COp_KCReleaseItem()
{
    AddParam(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCReleaseItem::Operate()
{
	KCItemRef	aItem = GetItem();
	mStatus = ::KCReleaseItem(&aItem);
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCCopyItem
// ���������������������������������������������������������������������������
COp_KCCopyItem::COp_KCCopyItem()
{
    AddParam(mKeychainIndex);
    AddParam(mItemIndex);
    AddResult(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCCopyItem::Operate()
{
	KCItemRef	aItem = NULL;
	mStatus = ::KCCopyItem(
						GetItem(),
						GetKeychain(),
						&aItem);
	AddItem(aItem);
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCFindFirstItem
// ���������������������������������������������������������������������������
COp_KCFindFirstItem::COp_KCFindFirstItem()
	:mAttrList("AttributeList")
{
    AddParam(mKeychainIndex);
	AddParam(mAttrList);
	AddResult(mSearchIndex);
	AddResult(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCFindFirstItem::Operate()
{
	KCSearchRef	aSearch = NULL;
	KCItemRef	aItem = NULL;
	mStatus = ::KCFindFirstItem(
					GetKeychain(),
					(const KCAttributeList *)mAttrList,
					&aSearch,
					&aItem);

	AddSearch(aSearch);
	AddItem(aItem);
	return(mStatus);
}


#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCFindNextItem
// ���������������������������������������������������������������������������
COp_KCFindNextItem::COp_KCFindNextItem()
{
	AddParam(mSearchIndex);
	AddResult(mItemIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCFindNextItem::Operate()
{
	KCItemRef	aItem = NULL;
	mStatus = ::KCFindNextItem(
					GetSearch(),
					&aItem);
	AddItem(aItem);
	return(mStatus);
}

#pragma mark -
// ���������������������������������������������������������������������������
// 	� COp_KCReleaseSearch
// ���������������������������������������������������������������������������
COp_KCReleaseSearch::COp_KCReleaseSearch()
{
	AddParam(mSearchIndex);
}

// ���������������������������������������������������������������������������
// 	� Operate
// ���������������������������������������������������������������������������
OSStatus
COp_KCReleaseSearch::Operate()
{
	KCSearchRef	aSearch = GetSearch();
	mStatus = ::KCReleaseSearch(&aSearch);
	return(mStatus);
}
