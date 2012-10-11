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
// SSCSPDLSession.cpp - Security Server CSP/DL session.
//
#include "SSCSPDLSession.h"

#include "CSPDLPlugin.h"
#include "SSKey.h"


#ifndef SECURITYSERVER_ACL_EDITS

#include <Security/aclclient.h>
#include <Security/Access.h>
#include <Security/TrustedApplication.h>

//
// ClientSessionKey - Lightweight wrapper for a KeyHandle that is also an CssmClient::AclBearer
//
class ClientSessionKey: public CssmClient::AclBearer
{
public:
	ClientSessionKey(SecurityServer::ClientSession &clientSession, SecurityServer::KeyHandle keyHandle);
	~ClientSessionKey();

	// Acl manipulation
	virtual void getAcl(AutoAclEntryInfoList &aclInfos,
		const char *selectionTag = NULL) const;
	virtual void changeAcl(const CSSM_ACL_EDIT &aclEdit,
		const CSSM_ACCESS_CREDENTIALS *cred = NULL);
	
	// Acl owner manipulation
	virtual void getOwner(AutoAclOwnerPrototype &owner) const;
	virtual void changeOwner(const CSSM_ACL_OWNER_PROTOTYPE &newOwner,
		const CSSM_ACCESS_CREDENTIALS *cred = NULL);


private:
	SecurityServer::ClientSession &mClientSession;
	SecurityServer::KeyHandle mKeyHandle;
};

#endif //!SECURITYSERVER_ACL_EDITS


using namespace SecurityServer;

//
// SSCSPDLSession -- Security Server CSP session
//
SSCSPDLSession::SSCSPDLSession()
{
}


//
// Reference Key management
//
void
SSCSPDLSession::makeReferenceKey(SSCSPSession &session, KeyHandle inKeyHandle,
								 CssmKey &outKey, SSDatabase &inSSDatabase,
								 uint32 inKeyAttr, const CssmData *inKeyLabel)
{
	new SSKey(session, inKeyHandle, outKey, inSSDatabase, inKeyAttr,
			  inKeyLabel);
}

SSKey &
SSCSPDLSession::lookupKey(const CssmKey &inKey)
{
	/* for now we only allow ref keys */
	if(inKey.blobType() != CSSM_KEYBLOB_REFERENCE) {
		CssmError::throwMe(CSSMERR_CSP_INVALID_KEY);
	}
	
	/* fetch key (this is just mapping the value in inKey.KeyData to an SSKey) */
	SSKey &theKey = find<SSKey>(inKey);
	
	#ifdef someday 
	/* 
	 * Make sure caller hasn't changed any crucial header fields.
	 * Some fields were changed by makeReferenceKey, so make a local copy....
	 */
	CSSM_KEYHEADER localHdr = cssmKey.KeyHeader;
	get binKey-like thing from SSKey, maybe SSKey should keep a copy of 
	hdr...but that's' not supersecure....;
	
	localHdr.BlobType = binKey->mKeyHeader.BlobType;
	localHdr.Format = binKey->mKeyHeader.Format;
	if(memcmp(&localHdr, &binKey->mKeyHeader, sizeof(CSSM_KEYHEADER))) {
		CssmError::throwMe(CSSMERR_CSP_INVALID_KEY_REFERENCE);
	}
	#endif
	return theKey;
}

// Notification we receive when the acl on a key has changed.  We should write it back to disk if it's persistant.
void
SSCSPDLSession::didChangeKeyAcl(SecurityServer::ClientSession &clientSession,
	KeyHandle keyHandle, CSSM_ACL_AUTHORIZATION_TAG tag)
{
#ifndef SECURITYSERVER_ACL_EDITS
	{
		// The user checked to don't ask again checkbox in the rogue app alert.  Let's edit the ACL for this key and add the calling application (ourself) to it.
		secdebug("keyacl", "SSCSPDLSession::didChangeKeyAcl(keyHandle: %lu tag: %lu)", keyHandle, tag);
		ClientSessionKey csKey(clientSession, keyHandle);             // the underlying key
		KeychainCore::SecPointer<KeychainCore::Access> access = new KeychainCore::Access(csKey);	// extract access rights
		KeychainCore::SecPointer<KeychainCore::TrustedApplication> thisApp = new KeychainCore::TrustedApplication;
		access->addApplicationToRight(tag, thisApp.get());	// add this app
		access->setAccess(csKey, true);	// commit
	}
#endif // !SECURITYSERVER_ACL_EDITS

	SSKey *theKey = NULL;

	{
		// Lookup the SSKey for the KeyHandle
		StLock<Mutex> _(mKeyMapLock);
		KeyMap::const_iterator it;
		KeyMap::const_iterator end = mKeyMap.end();
		for (it = mKeyMap.begin(); it != end; ++it)
		{
			SSKey *aKey = dynamic_cast<SSKey *>(it->second);
			if (aKey->optionalKeyHandle() == keyHandle)
			{
				// Write the key to disk if it's persistant.
				theKey = aKey;
				break;
			}
		}
	}

	if (theKey)
	{
		theKey->didChangeAcl();
	}
	else
	{
		// @@@ Should we really throw here or just continue without updating the ACL?  In reality this should never happen, so let's at least log it and throw.
		secdebug("keyacl", "SSCSPDLSession::didChangeKeyAcl() keyHandle: %lu not found in map", keyHandle);
		CssmError::throwMe(CSSMERR_CSP_INVALID_KEY_REFERENCE);
	}
}

void
SSCSPDLSession::didChangeKeyAclCallback(void *context, SecurityServer::ClientSession &clientSession,
	SecurityServer::KeyHandle key, CSSM_ACL_AUTHORIZATION_TAG tag)
{
	reinterpret_cast<SSCSPDLSession *>(context)->didChangeKeyAcl(clientSession, key, tag);
}

#ifndef SECURITYSERVER_ACL_EDITS
//
// ClientSessionKey - Lightweight wrapper for a KeyHandle that is also an CssmClient::AclBearer
//
ClientSessionKey::ClientSessionKey(ClientSession &clientSession, SecurityServer::KeyHandle keyHandle) :
	mClientSession(clientSession),
	mKeyHandle(keyHandle)
{
}

ClientSessionKey::~ClientSessionKey()
{
}

void
ClientSessionKey::getAcl(AutoAclEntryInfoList &aclInfos,
	const char *selectionTag) const
{
	secdebug("keyacl", "ClientSessionKey::getAcl() keyHandle: %lu", mKeyHandle);
	aclInfos.allocator(mClientSession.returnAllocator);
	mClientSession.getKeyAcl(mKeyHandle, selectionTag,
		*static_cast<uint32 *>(aclInfos),
		*reinterpret_cast<AclEntryInfo **>(static_cast<CSSM_ACL_ENTRY_INFO_PTR *>(aclInfos)));
}

void
ClientSessionKey::changeAcl(const CSSM_ACL_EDIT &aclEdit,
	const CSSM_ACCESS_CREDENTIALS *cred)
{
	secdebug("keyacl", "ClientSessionKey::changeAcl() keyHandle: %lu", mKeyHandle);
	mClientSession.changeKeyAcl(mKeyHandle, AccessCredentials::overlay(*cred), AclEdit::overlay(aclEdit));
}

void
ClientSessionKey::getOwner(AutoAclOwnerPrototype &owner) const
{
	secdebug("keyacl", "ClientSessionKey::getOwner() keyHandle: %lu", mKeyHandle);
	owner.allocator(mClientSession.returnAllocator);
	mClientSession.getKeyOwner(mKeyHandle,
		*reinterpret_cast<AclOwnerPrototype *>(static_cast<CSSM_ACL_OWNER_PROTOTYPE *>(owner)));
}

void
ClientSessionKey::changeOwner(const CSSM_ACL_OWNER_PROTOTYPE &newOwner,
	const CSSM_ACCESS_CREDENTIALS *cred)
{
	secdebug("keyacl", "ClientSessionKey::changeOwner() keyHandle: %lu", mKeyHandle);
	mClientSession.changeKeyOwner(mKeyHandle, AccessCredentials::overlay(*cred), AclOwnerPrototype::overlay(newOwner));
}

#endif // !SECURITYSERVER_ACL_EDITS
