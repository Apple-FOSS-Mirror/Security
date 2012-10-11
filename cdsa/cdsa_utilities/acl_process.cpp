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
// acl_process - Process-attribute ACL subject type.
//
#ifdef __MWERKS__
#define _CPP_ACL_PROCESS
#endif

#include <Security/acl_process.h>
#include <algorithm>

#include <cstdio>	// testing


//
// Validate a credential set against this subject
//
bool ProcessAclSubject::validate(const AclValidationContext &context,
    const TypedList &sample) const
{
    if (sample.length() != 1)	// no-argument sample
		CssmError::throwMe(CSSM_ERRCODE_INVALID_SAMPLE_VALUE);
    
    // reality check (internal structure was validated when created)
    assert(select.uses(CSSM_ACL_MATCH_BITS));
    
    // access the environment
    Environment *env = context.environment<Environment>();
    if (env == NULL) {
        static Environment localEnvironment;
        env = &localEnvironment;
    }
    
    // match uid
    if (select.uses(CSSM_ACL_MATCH_UID)) {
        uid_t uid = env->getuid();
        if (!(uid == select.uid || (select.uses(CSSM_ACL_MATCH_HONOR_ROOT) && uid == 0)))
            return false;
    }
    
    // match gid
    if (select.uses(CSSM_ACL_MATCH_GID) && select.gid != env->getgid())
        return false;
        
    return true;
}


//
// Make a copy of this subject in CSSM_LIST form
//
CssmList ProcessAclSubject::toList(CssmAllocator &alloc) const
{
    // all associated data is public (no secrets)
    //@@@ ownership of selector data is murky; revisit after leak-plugging pass
    CssmData sData(memcpy(alloc.alloc<CSSM_ACL_PROCESS_SUBJECT_SELECTOR>(),
        &select, sizeof(select)), sizeof(select));
	return TypedList(alloc, CSSM_ACL_SUBJECT_TYPE_PROCESS,
        new(alloc) ListElement(sData));
}


//
// Create a ProcessAclSubject
//
ProcessAclSubject *ProcessAclSubject::Maker::make(const TypedList &list) const
{
    // crack input apart
	ListElement *selectorData;
	crack(list, 1, &selectorData, CSSM_LIST_ELEMENT_DATUM);
    AclProcessSubjectSelector selector;
    selectorData->extract(selector);
    
    // validate input
    if (selector.version != CSSM_ACL_PROCESS_SELECTOR_CURRENT_VERSION)
        CssmError::throwMe(CSSM_ERRCODE_INVALID_ACL_SUBJECT_VALUE);
    if (!selector.uses(CSSM_ACL_MATCH_BITS))
        CssmError::throwMe(CSSM_ERRCODE_INVALID_ACL_SUBJECT_VALUE);
        
    // okay
	return new ProcessAclSubject(selector);
}

ProcessAclSubject *ProcessAclSubject::Maker::make(Version, Reader &pub, Reader &priv) const
{
    AclProcessSubjectSelector selector; pub(selector);
	return new ProcessAclSubject(selector);
}


//
// Export the subject to a memory blob
//
void ProcessAclSubject::exportBlob(Writer::Counter &pub, Writer::Counter &priv)
{
    pub(select);
}

void ProcessAclSubject::exportBlob(Writer &pub, Writer &priv)
{
    pub(select);
}


//
// Implement the default methods of a ProcessEnvironment
//
uid_t ProcessAclSubject::Environment::getuid() const
{
    return ::getuid();
}

gid_t ProcessAclSubject::Environment::getgid() const
{
    return ::getgid();
}


#ifdef DEBUGDUMP

void ProcessAclSubject::debugDump() const
{
	Debug::dump("Process ");
	if (select.uses(CSSM_ACL_MATCH_UID)) {
		Debug::dump("uid=%d", int(select.uid));
		if (select.uses(CSSM_ACL_MATCH_HONOR_ROOT))
			Debug::dump("+root");
	}
	if (select.uses(CSSM_ACL_MATCH_GID))
		Debug::dump("gid=%d", int(select.gid));
}

#endif //DEBUGDUMP
