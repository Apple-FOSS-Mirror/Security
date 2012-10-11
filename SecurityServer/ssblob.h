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
// ssblob - objects to represent persistent blobs used by SecurityServer
//
#ifndef _H_SSBLOB
#define _H_SSBLOB

#include <Security/SecurityServerClient.h>
#include <Security/cssm.h>
#include <Security/utilities.h>
#include <Security/cssmalloc.h>
#include <Security/cssmacl.h>
#include <Security/memutils.h>
#include <Security/endian.h>


namespace Security {
namespace SecurityServer {

using LowLevelMemoryUtilities::increment;


//
// A generic blob.
// Note that Blob and its subclasses are meant to be Byte Order Corrected.
// Make sure all non-byte fields are Endian<> qualified.
//
class Blob {
public:
	typedef Endian<uint32> uint32e;
	typedef Endian<sint32> sint32e;

protected:
    template <class T>
    T *at(off_t offset)		{ return LowLevelMemoryUtilities::increment<T>(this, offset); }
    void *at(off_t offset)	{ return LowLevelMemoryUtilities::increment(this, offset); }
};


//
// The common features of our blobs
//
class CommonBlob : public Blob {
public:
    // initial fixed fields for versioning
    uint32e magic;		// magic number
    uint32e blobVersion; // version code
	uint32 version() const { return blobVersion; }
    
    static const uint32 magicNumber = 0xfade0711;

    static const uint32 version_MacOS_10_0 = 0x00000100;	// MacOS 10.0.x
    static const uint32 version_MacOS_10_1 = 0x00000101;	// MacOS 10.1.x and on
    static const uint32 currentVersion = version_MacOS_10_0;
    
public:
    void initialize(uint32 version = currentVersion);
	bool isValid() const;
    void validate(CSSM_RETURN failureCode) const;
	
	void *data()		{ return at(0); }
};


//
// A Database blob
//
class DbBlob : public CommonBlob {
public:    
    struct Signature {
        uint8 bytes[16];
        
        bool operator < (const Signature &sig) const
        { return memcmp(bytes, sig.bytes, sizeof(bytes)) < 0; }
        bool operator == (const Signature &sig) const
        { return memcmp(bytes, sig.bytes, sizeof(bytes)) == 0; }
    };
    
    struct PrivateBlob : public Blob {
	    typedef uint8 EncryptionKey[24];
		typedef uint8 SigningKey[20];

        EncryptionKey encryptionKey;	// master encryption key
        SigningKey signingKey;		// master signing key

        // private ACL blob follows, to the end
        void *privateAclBlob()	{ return at(sizeof(PrivateBlob)); }
    };

public:    
    // position separators between variable-length fields (see below)
    uint32e startCryptoBlob;	// end of public ACL; start of crypto blob
    uint32e totalLength;		// end of crypto blob; end of entire blob

    Signature randomSignature;	// randomizing database signature
    uint32e sequence;			// database sequence number
    DBParameters params;		// database settable parameters

    uint8 salt[20];				// derivation salt
    uint8 iv[8];				// encryption iv

    uint8 blobSignature[20];	// HMAC/SHA1 of entire blob except itself
    
    // variable length fields:
    void *publicAclBlob()	{ return at(sizeof(DbBlob)); }
    size_t publicAclBlobLength() const
    { return startCryptoBlob - sizeof(DbBlob); }
    
    void *cryptoBlob()		{ return at(startCryptoBlob); }
    size_t cryptoBlobLength() const { return totalLength - startCryptoBlob; }
    
    uint32 length() const	{ return totalLength; }

    DbBlob *copy(CssmAllocator &alloc = CssmAllocator::standard()) const
    {
        DbBlob *blob = alloc.malloc<DbBlob>(length());
        memcpy(blob, this, length());
        return blob;
    }
};


//
// A key blob
//
class KeyBlob : public CommonBlob {
public:
    uint32e startCryptoBlob;	// end of public ACL; start of crypto blob
    uint32e totalLength;		// end of crypto blob; end of entire blob

    uint8 iv[8];				// encryption iv

    CssmKey::Header header;		// key header as-is
    struct WrappedFields {
        Endian<CSSM_KEYBLOB_TYPE> blobType;
        Endian<CSSM_KEYBLOB_FORMAT> blobFormat;
        Endian<CSSM_ALGORITHMS> wrapAlgorithm;
        Endian<CSSM_ENCRYPT_MODE> wrapMode;
    } wrappedHeader;

    uint8 blobSignature[20];	// HMAC/SHA1 of entire blob except itself
    
    // variable length fields:
    void *publicAclBlob()	{ return at(sizeof(KeyBlob)); }
    size_t publicAclBlobLength() const
    { return startCryptoBlob - sizeof(KeyBlob); }
    
    void *cryptoBlob()		{ return at(startCryptoBlob); }
    size_t cryptoBlobLength() const { return totalLength - startCryptoBlob; }
    
    uint32 length() const	{ return totalLength; }

    // these bits are managed internally by the SecurityServer (and not passed to the CSPs)
    static const uint32 managedAttributes =
        CSSM_KEYATTR_ALWAYS_SENSITIVE |
        CSSM_KEYATTR_NEVER_EXTRACTABLE |
        CSSM_KEYATTR_PERMANENT |
		CSSM_KEYATTR_EXTRACTABLE;
	static const uint32 forcedAttributes =
		CSSM_KEYATTR_EXTRACTABLE;

public:
    KeyBlob *copy(CssmAllocator &alloc) const
    {
        KeyBlob *blob = alloc.malloc<KeyBlob>(length());
        memcpy(blob, this, length());
        return blob;
    }
};


//
// An auto-unlock record (database identity plus raw unlock key)
//
class UnlockBlob : public CommonBlob {
public:
	typedef uint8 MasterKey[24];
	MasterKey masterKey;		// raw bits (triple-DES) - make your own CssmKey
	DbBlob::Signature signature; // signature is index
};


} // end namespace SecurityServer
} // end namespace Security


#endif //_H_SSBLOB
