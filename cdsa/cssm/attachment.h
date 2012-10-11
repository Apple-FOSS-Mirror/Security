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
// attachment - CSSM module attachment objects
//
#ifndef _H_ATTACHMENT
#define _H_ATTACHMENT

#include "cssmint.h"
#include "module.h"
#include <Security/handleobject.h>
#include <Security/cssmalloc.h>

#ifdef _CPP_ATTACHMENT
# pragma export on
#endif


//
// This type represents a single Attachment of any kind.
// Attachments are formed by the CSSM_ModuleAttach call and represent a "session"
// between a caller client and a particular plugin module. Any number of attachments
// can exist for a particular caller and a particular module.
// Attachment is an abstract class. You must subclass it to implement a concrete
// type of plugin. For the standard ones, see the end of this header.
// Key recovery does not use Attachments.
// EMMs are not Attachments, but the plugins they manage are.
// And yes, an Attachment *is* a lock. The API transition layer functions need
// to lock Attachments from the "outside", so there's no point in being coy about it.
// Locking the Attachment is equivalent to locking all its members except for
// instance-constant ones.
//
class Attachment : public HandleObject,						// has a handle
				   public CssmMemoryFunctionsAllocator,		// is an allocator
				   public CountingMutex {					// is a counting lock
	NOCOPY(Attachment)
public:
	Attachment(Module *theModule,
			const CSSM_VERSION &version,
			uint32 subserviceId,
			CSSM_SERVICE_TYPE subserviceType,
			const CSSM_API_MEMORY_FUNCS &memoryOps,
			CSSM_ATTACH_FLAGS attachFlags,
			CSSM_KEY_HIERARCHY keyHierarchy);
	virtual ~Attachment();

	Module &module;

	// feature inquiries. These features are constant for the life of the Attachment
	const Guid &myGuid() const { return module.myGuid(); }
	CSSM_SERVICE_TYPE subserviceType() const { return mSubserviceType; }
	uint32 subserviceId() const	{ return mSubserviceId; }
	const CSSM_VERSION &pluginVersion() const { return mVersion; }
	CSSM_ATTACH_FLAGS attachFlags() const { return mAttachFlags; }
	
	bool isThreadSafe() const	{ return module.isThreadSafe(); }

	// service a symbol table inquiry against our attachment
	virtual void resolveSymbols(CSSM_FUNC_NAME_ADDR *FunctionTable,
								uint32 NumFunctions) = 0;

	// terminate the live attachment and prepare to die
	void detach(bool isLocked);

	template <class Sub> friend Sub &enterAttachment(CSSM_HANDLE);

	// need to redefine lock/trylock to implement HandleObject protocol
	void lock() { CountingMutex::lock(); }
	bool tryLock() { return CountingMutex::tryLock(); }

private:
	bool mIsActive;						// successfully attached to plugin

	uint32 mSubserviceId;				// subservice ID in use
	CSSM_SERVICE_TYPE mSubserviceType;	// service type
	
	// how we were created
	CSSM_VERSION mVersion;				// plugin version requested
	CSSM_ATTACH_FLAGS mAttachFlags;		// attach flags requested
	CSSM_KEY_HIERARCHY mKeyHierarchy;	// key hierarchy verified

protected:
	CSSM_MODULE_FUNCS *spiFunctionTable; // entry table as returned by plugin on attach

private:
	CSSM_UPCALLS upcalls;				// upcall functions for our module

	// upcall implementors - these are handed to the plugin in `upcalls'
	static void *upcallMalloc(CSSM_HANDLE handle, uint32 size);
	static void upcallFree(CSSM_HANDLE handle, void *mem);
	static void *upcallRealloc(CSSM_HANDLE handle, void *mem,uint32 size);
	static void *upcallCalloc(CSSM_HANDLE handle, uint32 num, uint32 size);
	static CSSM_RETURN upcallCcToHandle(CSSM_CC_HANDLE handle, CSSM_MODULE_HANDLE *modHandle);
	static CSSM_RETURN upcallGetModuleInfo(CSSM_MODULE_HANDLE Module,
										CSSM_GUID_PTR Guid,
										CSSM_VERSION_PTR Version,
										uint32 *SubServiceId,
										CSSM_SERVICE_TYPE *SubServiceType,
										CSSM_ATTACH_FLAGS *AttachFlags,
										CSSM_KEY_HIERARCHY *KeyHierarchy,
										CSSM_API_MEMORY_FUNCS_PTR AttachedMemFuncs,
										CSSM_FUNC_NAME_ADDR_PTR FunctionTable,
										uint32 NumFunctions);
};

// this should be a member template, but egcs barfs on its invocation
// @@@ pass module code in to get better "invalid handle" diag?
// @@@ or use template specializations here?
template <class AttachmentSubclass>
inline AttachmentSubclass &enterAttachment(CSSM_HANDLE h)
{
	AttachmentSubclass &attachment = findHandleAndLock<AttachmentSubclass>(h);
	attachment.finishEnter();
	return attachment;
}


//
// For the standard attachment types, function dispatch to the plugin
// is done based on the CSSM_SPI_xxx_FUNCS structures describing the
// types and ordering of entry points. The StandardAttachment class
// implements this by holding a copy of these tables for the use of
// the transition layer.
// You are free to subclass from Attachment directly if that makes better
// sense to your kind of plugin.
//
template <CSSM_SERVICE_TYPE type, class FunctionTable>
class StandardAttachment : public Attachment {
public:
	typedef map<const char *, unsigned int> NameMap;

	StandardAttachment(Module *theModule,
					const NameMap &names,
					const CSSM_VERSION &version,
					uint32 subserviceId,
					CSSM_SERVICE_TYPE subserviceType,
					const CSSM_API_MEMORY_FUNCS &memoryOps,
					CSSM_ATTACH_FLAGS attachFlags,
					CSSM_KEY_HIERARCHY keyHierarchy)
		: Attachment(theModule, version, subserviceId, subserviceType,
					memoryOps, attachFlags, keyHierarchy),
		nameMap(names)
	{
			try {
				if (spiFunctionTable->NumberOfServiceFuncs <
					sizeof(FunctionTable) / sizeof(CSSM_PROC_ADDR))
					CssmError::throwMe(CSSMERR_CSSM_INVALID_ADDIN_FUNCTION_TABLE);
				// tolerate a table that's TOO large - perhaps it's a newer version
				// @@@ with the new spec, we could just store the pointer
				memcpy(&downcalls, spiFunctionTable->ServiceFuncs, sizeof(downcalls));
			} catch (...) {
				// we are attached to the plugin, so tell it the show is off
				detach(false);
				throw;
			}
	}

	void resolveSymbols(CSSM_FUNC_NAME_ADDR *inFunctionTable,
								uint32 NumFunctions)
	{
		for (unsigned n = 0; n < NumFunctions; n++) {
			NameMap::const_iterator it = nameMap.find(inFunctionTable[n].Name);
			inFunctionTable[n].Address =
				(it == nameMap.end()) ? NULL : downcallNumber(it->second);
		}
	}

	FunctionTable downcalls;

	CSSM_PROC_ADDR downcallNumber(uint32 index) const
	{
		assert(index < sizeof(downcalls) / sizeof(CSSM_PROC_ADDR));
		return reinterpret_cast<const CSSM_PROC_ADDR *>(&downcalls)[index];
	}
	
private:
	const NameMap &nameMap;
};

#ifdef _CPP_ATTACHMENT
# pragma export off
#endif


#endif //_H_ATTACHMENT
