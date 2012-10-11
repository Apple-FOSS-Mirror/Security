/*
 * Copyright (c) 2000-2002 Apple Computer, Inc. All Rights Reserved.
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


/*
	File:		StorageManager.cpp

	Contains:	Working with multiple keychains

*/

#include "StorageManager.h"
#include "KCEventNotifier.h"

#include <Security/cssmapple.h>
#include <sys/types.h>
#include <pwd.h>
#include <CoreServices/../Frameworks/CarbonCore.framework/Headers/MacErrors.h>
#include <algorithm>
#include <string>
#include <Security/Authorization.h>
#include <Security/AuthorizationTags.h>
#include <Security/AuthSession.h>
#include <Security/debugging.h>
#include <Security/SecCFTypes.h>

#include "KCCursor.h"
#include "Globals.h"
#include "DefaultKeychain.h"

using namespace CssmClient;
using namespace KeychainCore;

StorageManager::StorageManager() :
    mSavedList(),
    mKeychains(),
    mSearchList()
{
	_doReload();
}

// Create KC if it doesn't exist	
Keychain
StorageManager::keychain(const DLDbIdentifier &dLDbIdentifier)
{
	StLock<Mutex> _(mLock);
	return _keychain(dLDbIdentifier);
}

Keychain
StorageManager::_keychain(const DLDbIdentifier &dLDbIdentifier)
{
    KeychainMap::iterator it = mKeychains.find(dLDbIdentifier);
    if (it != mKeychains.end())
		return it->second;

	// The keychain is not in our cache.  Create it.
	Module module(dLDbIdentifier.ssuid().guid());
	DL dl;
	if (dLDbIdentifier.ssuid().subserviceType() & CSSM_SERVICE_CSP)
		dl = SSCSPDL(module);
	else
		dl = DL(module);

	dl->subserviceId(dLDbIdentifier.ssuid().subserviceId());
	dl->version(dLDbIdentifier.ssuid().version());
	Db db(dl, dLDbIdentifier.dbName());
	Keychain keychain(db);

	// Add the keychain to the cache.
	mKeychains.insert(KeychainMap::value_type(dLDbIdentifier, keychain));
	return keychain;
}

// Create KC if it doesn't exist, add it to the search list if it exists and is not already on it.
Keychain
StorageManager::makeKeychain(const DLDbIdentifier &dLDbIdentifier)
{
	Keychain keychain(keychain(dLDbIdentifier));

	{
		StLock<Mutex> _(mLock);
		if (find(mSearchList.begin(), mSearchList.end(), keychain) != mSearchList.end())
		{
			// This keychain is already on our search list.
			return keychain;
		}
	
		// If the keychain doesn't exist don't bother adding it to the search list yet.
		if (!keychain->exists())
			return keychain;
	
		// The keychain exists and is not in our search list add it to the search
		// list and the cache.  Then inform mMultiDLDb.
		mSavedList.revert(true);
		mSavedList.add(dLDbIdentifier);
		mSavedList.save();
	
		// @@@ Will happen again when kSecKeychainListChangedEvent notification is received.
		_doReload();
	}

	// Make sure we are not holding mLock when we post this event.
	KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);

	return keychain;
}

void
StorageManager::created(const Keychain &keychain) // Be notified a Keychain just got created.
{
    DLDbIdentifier dLDbIdentifier = keychain->dLDbIdentifier();

 	{
		StLock<Mutex> _(mLock);

		// If we don't have a default Keychain yet.  Make the newly created keychain the default.
		DefaultKeychain &defaultKeychain = globals().defaultKeychain;
		if (!defaultKeychain.isSet())
			defaultKeychain.dLDbIdentifier(dLDbIdentifier);
	
		// Add the keychain to the search list and the cache.  Then inform mMultiDLDb.
		mSavedList.revert(true);
		mSavedList.add(dLDbIdentifier);
		mSavedList.save();
	
		// @@@ Will happen again when kSecKeychainListChangedEvent notification is received.
		_doReload();
	}

	// Make sure we are not holding mLock when we post this event.
	KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);
}

KCCursor
StorageManager::createCursor(SecItemClass itemClass, const SecKeychainAttributeList *attrList)
{
	StLock<Mutex> _(mLock);
	return KCCursor(mSearchList, itemClass, attrList);
}

KCCursor
StorageManager::createCursor(const SecKeychainAttributeList *attrList)
{
	StLock<Mutex> _(mLock);
	return KCCursor(mSearchList, attrList);
}

void
StorageManager::lockAll()
{
	// Make a snapshot of all known keychains while holding mLock.
	KeychainList keychainList;
	{
		StLock<Mutex> _(mLock);
		for (KeychainMap::iterator ix = mKeychains.begin(); ix != mKeychains.end(); ix++)
			keychainList.push_back(ix->second);
	}

	// Lock each active keychain after having released mLock since locking keychains
	// will send notifications.
	for (KeychainList::iterator ix = keychainList.begin(); ix != keychainList.end(); ++ix)
	{
		Keychain keychain = *ix;
		if (keychain->isActive())
			keychain->lock();
	}
}

void
StorageManager::_doReload()
{
	KeychainList newList;
	newList.reserve(mSavedList.size());
	for (CssmClient::DLDbList::iterator ix = mSavedList.begin(); ix != mSavedList.end(); ++ix)
	{
		Keychain keychain(_keychain(*ix));
		newList.push_back(keychain);
	}
	mSearchList.swap(newList);
}

void
StorageManager::reload(bool force)
{
	StLock<Mutex> _(mLock);
    _reload(force);
}

void
StorageManager::_reload(bool force)
{
    // Reinitialize list from CFPrefs if changed.  When force is true force a prefs revert now.
    if (mSavedList.revert(force))
        _doReload();
}

size_t
StorageManager::size()
{
	StLock<Mutex> _(mLock);
    _reload();
    return mSearchList.size();
}

Keychain
StorageManager::at(unsigned int ix)
{
	StLock<Mutex> _(mLock);
    _reload();
    if (ix >= mSearchList.size())
        MacOSError::throwMe(errSecInvalidKeychain);

    return mSearchList.at(ix);
}

Keychain
StorageManager::operator[](unsigned int ix)
{
    return at(ix);
}	

void StorageManager::remove(const KeychainList &kcsToRemove, bool deleteDb)
{
	bool unsetDefault = false;
	{
		StLock<Mutex> _(mLock);
		mSavedList.revert(true);
		DLDbIdentifier defaultId = globals().defaultKeychain.dLDbIdentifier();
		for (KeychainList::const_iterator ix = kcsToRemove.begin(); ix != kcsToRemove.end(); ++ix)
		{
			// Find the keychain object for the given ref
			Keychain keychainToRemove = *ix;
			DLDbIdentifier dLDbIdentifier = keychainToRemove->dLDbIdentifier();
	
			// Remove it from the saved list
			mSavedList.remove(dLDbIdentifier);
			if (dLDbIdentifier == defaultId)
				unsetDefault=true;

			if (deleteDb)
			{
				keychainToRemove->database()->deleteDb();
				// Now remove it from the map
				KeychainMap::iterator it = mKeychains.find(dLDbIdentifier);
				if (it == mKeychains.end())
					continue;
				mKeychains.erase(it);
			}
		}
		mSavedList.save();
		_doReload();
	}

	// Make sure we are not holding mLock when we post this event.
	KCEventNotifier::PostKeychainEvent(kSecKeychainListChangedEvent);

	if (unsetDefault)
	{
		// Make sure we are not holding mLock when we call this since it posts an event.
		globals().defaultKeychain.unset();
	}
}

void
StorageManager::getSearchList(KeychainList &keychainList)
{
	// Make a copy of the searchList
	StLock<Mutex> _(mLock);
	StorageManager::KeychainList searchList(mSearchList);

	// Return the copy of the list.
	keychainList.swap(searchList);
}

void
StorageManager::setSearchList(const KeychainList &keychainList)
{
	// Make a copy of the passed in searchList
	StorageManager::KeychainList keychains(keychainList);

	// Set the current searchlist to be what was passed in, the old list will be freed
	// upon exit of this stackframe.
	StLock<Mutex> _(mLock);
	mSearchList.swap(keychains);
}

void
StorageManager::optionalSearchList(CFTypeRef keychainOrArray, KeychainList &keychainList)
{
	if (!keychainOrArray)
		getSearchList(keychainList);
	else
	{
		CFTypeID typeID = CFGetTypeID(keychainOrArray);
		if (typeID == CFArrayGetTypeID())
			convertToKeychainList(CFArrayRef(keychainOrArray), keychainList);
		else if (typeID == gTypes().keychain.typeId)
			keychainList.push_back(gTypes().keychain.required(SecKeychainRef(keychainOrArray)));
		else
			MacOSError::throwMe(paramErr);
	}
}

// static methods.
void
StorageManager::convertToKeychainList(CFArrayRef keychainArray, KeychainList &keychainList)
{
	assert(keychainArray);
	CFIndex count = CFArrayGetCount(keychainArray);
	KeychainList keychains(count);
	CFClass<KeychainImpl, SecKeychainRef, errSecInvalidKeychain> &kcClass = gTypes().keychain;
	for (CFIndex ix = 0; ix < count; ++ix)
	{
		keychains[ix] = kcClass.required(SecKeychainRef(CFArrayGetValueAtIndex(keychainArray, ix)));
	}

	keychainList.swap(keychains);
}

CFArrayRef
StorageManager::convertFromKeychainList(const KeychainList &keychainList)
{
	CFRef<CFMutableArrayRef> keychainArray(CFArrayCreateMutable(NULL, keychainList.size(), &kCFTypeArrayCallBacks));

	CFClass<KeychainImpl, SecKeychainRef, errSecInvalidKeychain> &kcClass = gTypes().keychain;
	for (KeychainList::const_iterator ix = keychainList.begin(); ix != keychainList.end(); ++ix)
	{
		SecKeychainRef keychainRef = kcClass.handle(**ix);
		CFArrayAppendValue(keychainArray, keychainRef);
		CFRelease(keychainRef);
	}

	// Counter the CFRelease that CFRef<> is about to do when keychainArray goes out of scope.
	CFRetain(keychainArray);
	return keychainArray;
}



#pragma mark ���� Login Functions ����

void StorageManager::login(ConstStringPtr name, ConstStringPtr password)
{
    if ( name == NULL || password == NULL )
        MacOSError::throwMe(paramErr);

	login(name[0], name + 1, password[0], password + 1);
}

void StorageManager::login(UInt32 nameLength, const void *name, UInt32 passwordLength, const void *password)
{
    // @@@ set up the login session on behalf of loginwindow
    // @@@ (this code should migrate into loginwindow)
#if 0
    debug("KClogin", "setting up login session");
    if (OSStatus ssnErr = SessionCreate(sessionKeepCurrentBootstrap,
            sessionHasGraphicAccess | sessionHasTTY))
       debug("KClogin", "session setup failed status=%ld", ssnErr);
#endif

    if (name == NULL || (passwordLength != 0 && password == NULL))
        MacOSError::throwMe(paramErr);

	// Make sure name is zero terminated
	string theName(reinterpret_cast<const char *>(name), nameLength);
	Keychain keychain = make(theName.c_str());
	try
	{
		keychain->unlock(CssmData(const_cast<void *>(password), passwordLength));
        debug("KClogin", "keychain unlock successful");
	}
	catch(const CssmError &e)
	{
		if (e.osStatus() != CSSMERR_DL_DATASTORE_DOESNOT_EXIST)
			throw;
        debug("KClogin", "creating login keychain");
		keychain->create(passwordLength, password);
		// Login Keychain does not lock on sleep nor lock after timeout by default.
		keychain->setSettings(INT_MAX, false);
	}
#if 0
	// @@@ Create a authorization credential for the current user.
    debug("KClogin", "creating login authorization");
	const AuthorizationItem envList[] =
	{
		{ kAuthorizationEnvironmentUsername, nameLength, const_cast<void *>(name), 0 },
		{ kAuthorizationEnvironmentPassword, passwordLength, const_cast<void *>(password), 0 },
		{ kAuthorizationEnvironmentShared, 0, NULL, 0 }
	};
	const AuthorizationEnvironment environment =
	{
		sizeof(envList) / sizeof(*envList),
		const_cast<AuthorizationItem *>(envList)
	};
	if (OSStatus authErr = AuthorizationCreate(NULL, &environment,
            kAuthorizationFlagExtendRights | kAuthorizationFlagPreAuthorize, NULL))
        debug("KClogin", "failed to create login auth, status=%ld", authErr);
#endif
}

void StorageManager::logout()
{
    // nothing left to do here
}

void StorageManager::changeLoginPassword(ConstStringPtr oldPassword, ConstStringPtr newPassword)
{
	globals().defaultKeychain.keychain()->changePassphrase(oldPassword, newPassword);
}


void StorageManager::changeLoginPassword(UInt32 oldPasswordLength, const void *oldPassword,  UInt32 newPasswordLength, const void *newPassword)
{
	globals().defaultKeychain.keychain()->changePassphrase(oldPasswordLength, oldPassword,  newPasswordLength, newPassword);
}

#pragma mark ���� File Related ����

Keychain StorageManager::make(const char *pathName)
{
	string fullPathName;
    if ( pathName[0] == '/' )
		fullPathName = pathName;
	else
    {
		// Get Home directory from environment.
		const char *homeDir = getenv("HOME");
		if (homeDir == NULL)
		{
			// If $HOME is unset get the current users home directory from the passwd file.
			struct passwd *pw = getpwuid(getuid());
			if (!pw)
				MacOSError::throwMe(paramErr);

			homeDir = pw->pw_dir;
		}

		fullPathName = homeDir;
		fullPathName += "/Library/Keychains/";
		fullPathName += pathName;
	}

    const CSSM_NET_ADDRESS *DbLocation = NULL;	// NULL for keychains
    const CSSM_VERSION *version = NULL;
    uint32 subserviceId = 0;
    CSSM_SERVICE_TYPE subserviceType = CSSM_SERVICE_DL | CSSM_SERVICE_CSP;
    const CssmSubserviceUid ssuid(gGuidAppleCSPDL, version, 
                                   subserviceId, subserviceType);
	DLDbIdentifier dLDbIdentifier(ssuid, fullPathName.c_str(), DbLocation);
	return makeKeychain(dLDbIdentifier);
}

KeychainSchema
StorageManager::keychainSchemaFor(const CssmClient::Db &db)
{
	// @@@ Locking
	KeychainSchema schema(db);
	pair<KeychainSchemaSet::iterator, bool> result = mKeychainSchemaSet.insert(db);
	if (result.second)
		return schema;
	return *result.first;
}

