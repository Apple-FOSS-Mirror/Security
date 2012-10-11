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


#include "dictionary.h"
#include <ctype.h>

namespace Security {

CssmData NameValuePair::CloneData (const CssmData &value)
{
	void* clonedData = (void*) new unsigned char [value.length ()];
	memcpy (clonedData, value.data (), value.length ());
	return CssmData (clonedData, value.length ());
}



NameValuePair::NameValuePair (uint32 name, const CssmData &value) : mName (name), mValue (CloneData (value))
{
}



NameValuePair::NameValuePair (const CssmData &data)
{
	// the first four bytes are the name
	unsigned char* finger = (unsigned char*) data.data ();
	mName = 0;
	
	unsigned int i;
	for (i = 0; i < sizeof (uint32); ++i)
	{
		mName = (mName << 8) | *finger++;
	}
	
	// the next four bytes are the length
	uint32 length = 0;
	for (i = 0; i < sizeof (uint32); ++i)
	{
		length = (length << 8) | *finger++;
	}
	
	// what's left is the data
	mValue = CloneData (CssmData (finger, length));
}



NameValuePair::~NameValuePair ()
{
	delete (unsigned char*) mValue.data ();
}



void NameValuePair::Export (CssmData &data) const
{
	// export the data in the format name length data
	uint32 outSize = 2 * sizeof (uint32) + mValue.length ();
	unsigned char* d = new unsigned char [outSize];
	unsigned char* finger = d;
	
	// export the name
	uint32 intBuffer = mName;

	int i;
	for (i = sizeof (uint32) - 1; i >= 0; --i)
	{
		finger[i] = intBuffer & 0xFF;
		intBuffer >>= 8;
	}
	
	// export the length
	finger += sizeof (uint32);
	intBuffer = mValue.length ();
	for (i = sizeof (uint32) - 1; i >= 0; --i)
	{
		finger[i] = intBuffer & 0xFF;
		intBuffer >>= 8;
	}

	// export the data
	finger += sizeof (uint32);
	memcpy (finger, mValue.data (), mValue.length ());
	
	data = CssmData (d, outSize);
}



NameValueDictionary::NameValueDictionary ()
{
}



NameValueDictionary::~NameValueDictionary ()
{
	// to prevent leaks, delete all members of the vector
	int i = mVector.size ();
	while (i > 0)
	{
		delete mVector[--i];
		
		mVector.erase (mVector.begin () + i);
	}
}



NameValueDictionary::NameValueDictionary (const CssmData &data)
{
	// reconstruct a name value dictionary from a series of exported NameValuePair blobs
	unsigned char* finger = (unsigned char*) data.data ();
	unsigned char* target = finger + data.length ();

	do
	{
		// compute the length of data blob
		unsigned int i;
		uint32 length = 0;
		for (i = sizeof (uint32); i < 2 * sizeof (uint32); ++i)
		{
			length = (length << 8) | finger[i];
		}
		
		// add the length of the "header"
		length += 2 * sizeof (uint32);
		Insert (new NameValuePair (CssmData (finger, length)));
		
		// skip to the next data
		finger += length;
	} while (finger < target);
}
	


void NameValueDictionary::Insert (NameValuePair* pair)
{
	mVector.push_back (pair);
}



void NameValueDictionary::RemoveByName (uint32 name)
{
	int which = FindPositionByName (name);
	if (which != -1)
	{
		NameValuePair* nvp = mVector[which];
		mVector.erase (mVector.begin () + which);
		delete nvp;
	}
}



int NameValueDictionary::FindPositionByName (uint32 name) const
{
	int target = CountElements ();
	int i;
	
	for (i = 0; i < target; ++i)
	{
		if (mVector[i]->Name () == name)
		{
			return i;
		}
	}
	
	return -1;
}



const NameValuePair* NameValueDictionary::FindByName (uint32 name) const
{
	int which = FindPositionByName (name);
	return which == -1 ? NULL : mVector[which];
}




int NameValueDictionary::CountElements () const
{
	return mVector.size ();
}



const NameValuePair* NameValueDictionary::GetElement (int which)
{
	return mVector[which];
}



void NameValueDictionary::Export (CssmData &outData)
{
	// get each element in the dictionary, and add it to the data blob
	int i;
	uint32 length = 0;
	unsigned char* data = 0;

	for (i = 0; i < CountElements (); ++i)
	{
		CssmData exportedData;
		const NameValuePair *nvp = GetElement (i);
		nvp->Export (exportedData);
		
		uint32 oldLength = length;
		length += exportedData.length ();
		data = (unsigned char*) realloc (data, length);
		
		memcpy (data + oldLength, exportedData.data (), exportedData.length ());
		
		delete (unsigned char*) exportedData.data ();
	}
	
	outData = CssmData (data, length);
}



void NameValueDictionary::MakeNameValueDictionaryFromDLDbIdentifier (const DLDbIdentifier &identifier, NameValueDictionary &nvd)
{
	// get the subserviceID
	const CssmSubserviceUid &ssuid = identifier.ssuid ();
	const CSSM_SUBSERVICE_UID* baseID = &ssuid;
	nvd.Insert (new NameValuePair (SSUID_KEY, CssmData ((void*) (baseID), sizeof (CSSM_SUBSERVICE_UID))));
	
	// get the name
	const char* dbName = identifier.dbName ();
	nvd.Insert (new NameValuePair (DB_NAME, CssmData ((void*) (dbName), strlen (dbName) + 1)));
	
	// get the net address
	const CSSM_NET_ADDRESS* add = identifier.dbLocation ();
	if (add != NULL)
	{
		nvd.Insert (new NameValuePair (DB_LOCATION, CssmData ((void*) add, sizeof (CSSM_NET_ADDRESS))));
	}
}



DLDbIdentifier NameValueDictionary::MakeDLDbIdentifierFromNameValueDictionary (const NameValueDictionary &nvd)
{
	CSSM_SUBSERVICE_UID* uid = (CSSM_SUBSERVICE_UID*) nvd.FindByName (SSUID_KEY)->Value ().data ();
	char* name = (char*) nvd.FindByName (DB_NAME)->Value ().data ();
	
	const NameValuePair* nvp = nvd.FindByName (DB_LOCATION);
	CSSM_NET_ADDRESS* address = nvp ? (CSSM_NET_ADDRESS*) nvp->Value ().data () : NULL;
	
	return DLDbIdentifier (*uid, name, address);
}

}; // end Security namespace
