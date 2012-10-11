/*
 * Copyright (c) 2002 Apple Computer, Inc. All Rights Reserved.
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
// TrustedApplication.cpp
//
#include <Security/TrustedApplication.h>
#include <Security/ACL.h>
#include <Security/osxsigning.h>
#include <Security/osxsigner.h>
#include <Security/trackingallocator.h>
#include <sys/syslimits.h>
#include <memory>

using namespace KeychainCore;
using namespace CodeSigning;


//
// Create a TrustedApplication from a code-signing ACL subject.
// Throws ACL::ParseError if the subject is unexpected.
//
TrustedApplication::TrustedApplication(const TypedList &subject)
	: mSignature(CssmAllocator::standard()),
	  mData(CssmAllocator::standard())
{
	if (subject.type() != CSSM_ACL_SUBJECT_TYPE_CODE_SIGNATURE)
		throw ACL::ParseError();
	if (subject[1] != CSSM_ACL_CODE_SIGNATURE_OSX)
		throw ACL::ParseError();
	mSignature = subject[2].data();
	mData = subject[3].data();
}


TrustedApplication::TrustedApplication(const CssmData &signature, const CssmData &data) :
	mSignature(CssmAllocator::standard(), signature),
	mData(CssmAllocator::standard(), data)
{
}

TrustedApplication::TrustedApplication(const char *path)
	: mSignature(CssmAllocator::standard()),
	  mData(CssmAllocator::standard())
{
	OSXSigner signer;
	RefPointer<OSXCode> object(OSXCode::at(path));
	auto_ptr<OSXSigner::OSXSignature> signature(signer.sign(*object));
	mSignature = *signature;
	string basePath = object->canonicalPath();
	mData = CssmData(const_cast<char *>(basePath.c_str()), basePath.length() + 1);
}

TrustedApplication::TrustedApplication()
	: mSignature(CssmAllocator::standard()),
	  mData(CssmAllocator::standard())
{
	OSXSigner signer;
	RefPointer<OSXCode> object(OSXCode::main());
	auto_ptr<OSXSigner::OSXSignature> signature(signer.sign(*object));
	mSignature = *signature;
	string path = object->canonicalPath();
	mData.copy(path.c_str(), path.length() + 1);	// including trailing null
}

TrustedApplication::~TrustedApplication() throw()
{
}

const CssmData &
TrustedApplication::signature() const
{
	return mSignature;
}

const char *
TrustedApplication::path() const
{
	if (mData)
		return mData.get().interpretedAs<const char>();
	else
		return NULL;
}

bool
TrustedApplication::sameSignature(const char *path)
{
	// return true if object at given path has same signature
    CssmAutoData otherSignature(CssmAllocator::standard());
    calcSignature(path, otherSignature);
	return (mSignature.get() == otherSignature);
}

void
TrustedApplication::calcSignature(const char *path, CssmOwnedData &signature)
{
	// generate a signature for the given object
    RefPointer<CodeSigning::OSXCode> objToVerify(CodeSigning::OSXCode::at(path));
	CodeSigning::OSXSigner signer;
    auto_ptr<CodeSigning::OSXSigner::OSXSignature> osxSignature(signer.sign(*objToVerify));
    signature.copy(osxSignature->data(), osxSignature->length());
}


//
// Produce a TypedList representing a code-signing ACL subject
// for this application.
// Memory is allocated from the allocator given, and belongs to
// the caller.
//
TypedList TrustedApplication::makeSubject(CssmAllocator &allocator)
{
	return TypedList(allocator,
		CSSM_ACL_SUBJECT_TYPE_CODE_SIGNATURE,
		new(allocator) ListElement(CSSM_ACL_CODE_SIGNATURE_OSX),
		new(allocator) ListElement(allocator, mSignature.get()),
		new(allocator) ListElement(allocator, mData.get()));
}


//
// On a completely different note...
// Read a simple text file from disk and cache the lines in a set.
// This is used during re-prebinding to cut down on the number of
// equivalency records being generated.
// This feature is otherwise completely unconnected to anything else here.
//
PathDatabase::PathDatabase(const char *path)
{
    if (FILE *f = fopen(path, "r")) {
        mQualifyAll = false;
        char path[PATH_MAX+1];
        while (fgets(path, sizeof(path), f)) {
            path[strlen(path)-1] = '\0';	// strip NL
            mPaths.insert(path);
        }
		fclose(f);
        secdebug("equivdb", "read %ld paths from %s", mPaths.size(), path);
    } else {
        mQualifyAll = true;
        secdebug("equivdb", "cannot open %s, will qualify all application paths", path);
    }
}


bool PathDatabase::lookup(const string &path)
{
	string::size_type lastSlash = path.rfind('/');
	string::size_type bundleCore = path.find("/Contents/MacOS/");
	if (lastSlash != string::npos && bundleCore != string::npos)
		if (bundleCore + 15 == lastSlash)
			path = path.substr(0, bundleCore);
	return mPaths.find(path) != mPaths.end();
}
