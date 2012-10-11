/*
 * Copyright (c) 2003 Apple Computer, Inc. All Rights Reserved.
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
// codesigdb - code-hash equivalence database
//
#ifndef _H_CODESIGDB
#define _H_CODESIGDB

#include <Security/db++.h>
#include <Security/osxsigner.h>


class Process;
class CodeSignatures;


//
// A CodeSignaturse object represents a database of code-signature equivalencies
// as (previously) expressed by a user and/or the system.
// You'll usually only need one of these.
//
class CodeSignatures {	
public:
	//
	// Identity is an abstract class modeling a code-identity in the database.
	// It can represent either an existing or latent code-hash link.
	// Subclass must provide path and hash source functions.
	//
	class Identity {
		friend class CodeSignatures;
	public:
		Identity();
		virtual ~Identity();
		
		operator bool () const				{ return mState == valid; }
		std::string path()					{ return getPath(); }
		std::string name() 					{ return canonicalName(path()); }
		std::string trustedName() const		{ return mName; }

		static std::string canonicalName(const std::string &path);
		
		IFDUMP(void debugDump(const char *how = NULL) const);
		
		virtual std::string getPath() const = 0;
		virtual const CssmData getHash(CodeSigning::OSXSigner &signer) const = 0;
	
	private:
		enum { untried, valid, invalid } mState;
		std::string mName;		// link db value (canonical name linked to)
	};
	
public:
	CodeSignatures(const char *path);
	~CodeSignatures();
	
	void open(const char *path);
	
public:
	bool find(Identity &id, uid_t user);
	
	void makeLink(Identity &id, const std::string &ident, bool forUser = false, uid_t user = 0);
	void makeApplication(const std::string &name, const std::string &path);

	void addLink(const CssmData &oldHash, const CssmData &newHash,
		const char *name, bool forSystem);
	void removeLink(const CssmData &hash, const char *name, bool forSystem);
	
	IFDUMP(void debugDump(const char *how = NULL) const);
	
public:
	bool verify(Process &process,
		const CodeSigning::Signature *trustedSignature, const CssmData *comment);
	
private:
	UnixPlusPlus::UnixDb mDb;
	CodeSigning::OSXSigner mSigner;

	// lock hierarchy: mUILock first, then mDatabaseLock, no back-off
	Mutex mDatabaseLock;			// controls mDb access
	Mutex mUILock;					// serializes user interaction
};



#endif //_H_CODESIGDB
