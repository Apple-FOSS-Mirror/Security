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


/*
 *  AuthorizationEngine.cpp
 *  Authorization
 *
 *  Created by Michael Brouwer on Thu Oct 12 2000.
 *  Copyright (c) 2000 Apple Computer Inc. All rights reserved.
 *
 */
#include "AuthorizationEngine.h"
#include <Security/AuthorizationWalkers.h>
#include "AuthorizationPriv.h"
#include "AuthorizationDB.h"


#include "authority.h"

#include <Security/AuthorizationTags.h>
#include <Security/logging.h>
#include <Security/cfutilities.h>
#include <Security/debugging.h>
#include "session.h"
#include "server.h"

#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFPropertyList.h>

#include <errno.h>
#include <fcntl.h>
#include <float.h>

namespace Authorization {

//
// Errors to be thrown
//
Error::Error(int err) : error(err)
{
}

const char *Error::what() const throw()
{ return "Authorization error"; }

CSSM_RETURN Error::cssmError() const throw()
{ return error; }	// @@@ eventually...

OSStatus Error::osStatus() const throw()
{ return error; }

void Error::throwMe(int err) { throw Error(err); }

//
// Engine class
//
Engine::Engine(const char *configFile) : mAuthdb(configFile)
{
}

Engine::~Engine()
{
}


/*!
	@function AuthorizationEngine::authorize

	@@@.

	@param inRights (input) List of rights being requested for authorization.
	@param environment (optional/input) Environment containing information to be used during evaluation.
	@param flags (input) Optional flags @@@ see AuthorizationCreate for a description.
	@param inCredentials (input) Credentials already held by the caller.
	@param outCredentials (output/optional) Credentials obtained, used or refreshed during this call to authorize the requested rights.
	@param outRights (output/optional) Subset of inRights which were actually authorized.

	@results Returns errAuthorizationSuccess if all rights requested are authorized, or if the kAuthorizationFlagPartialRights flag was specified.  Might return other status values like errAuthorizationDenied, errAuthorizationCanceled or errAuthorizationInteractionNotAllowed 
*/
OSStatus
Engine::authorize(const AuthItemSet &inRights, const AuthItemSet &environment,
	AuthorizationFlags flags, const CredentialSet *inCredentials, CredentialSet *outCredentials,
	AuthItemSet &outRights, AuthorizationToken &auth)
{
	CredentialSet credentials;
	OSStatus status = errAuthorizationSuccess;

	// Get current time of day.
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

	// Update rules from database if needed
	mAuthdb.sync(now);

	// Check if a credential was passed into the environment and we were asked to extend the rights
	if (flags & kAuthorizationFlagExtendRights)
	{
		string username, password;
		bool shared = false;
		for (AuthItemSet::iterator item = environment.begin(); item != environment.end(); item ++)
		{
			if (!strcmp((*item)->name(), kAuthorizationEnvironmentUsername))
				username = (*item)->stringValue();
			else if (!strcmp((*item)->name(), kAuthorizationEnvironmentPassword))
				password = (*item)->stringValue();
			else if (!strcmp((*item)->name(), kAuthorizationEnvironmentShared))
				shared = true;
		}

		if (username.length())
		{
			// Let's create a credential from the passed in username and password.
			Credential newCredential(username, password, shared);
			// If it's valid insert it into the credentials list.  Normally this is
			// only done if it actually authorizes a requested right, but for this
			// special case (environment) we do it even when no rights are being requested.
			if (newCredential->isValid())
				credentials.insert(newCredential);
		}
	}
	
	// generate hints for every authorization
    AuthItemSet environmentToClient = environment;

	AuthItemSet::const_iterator end = inRights.end();
	for (AuthItemSet::const_iterator it = inRights.begin(); it != end; ++it)
	{
		// Get the rule for each right we are trying to obtain.
		const Rule &toplevelRule = mAuthdb.getRule(*it);
		OSStatus result = toplevelRule->evaluate(*it, toplevelRule, environmentToClient, flags, now, inCredentials, credentials, auth);
		secdebug("autheval", "evaluate rule %s for right %s returned %ld.", toplevelRule->name().c_str(), (*it)->name(), result);

		{
			CodeSigning::OSXCode *processCode = Server::connection().process.clientCode();
			string processName = processCode ? processCode->canonicalPath() : "unknown";
			CodeSigning::OSXCode *authCreatorCode = auth.creatorCode();
			string authCreatorName = authCreatorCode ? authCreatorCode->canonicalPath() : "unknown";
			
			if (result == errAuthorizationSuccess)
				Syslog::info("Succeeded authorizing right %s by process %s for authorization created by %s.", (*it)->name(), processName.c_str(), authCreatorName.c_str());
			else if (result == errAuthorizationDenied)
				Syslog::notice("Failed to authorize right %s by process %s for authorization created by %s.", (*it)->name(), processName.c_str(), authCreatorName.c_str());
		}
		
		if (result == errAuthorizationSuccess)
			outRights.insert(*it);
		else if (result == errAuthorizationDenied || result == errAuthorizationInteractionNotAllowed)
		{
			// add creator pid to authorization token
			if (!(flags & kAuthorizationFlagPartialRights))
			{
				status = result;
				break;
			}
		}
        else if (result == errAuthorizationCanceled)
        {
            status = result;
            break;
        }
		else
		{
			Syslog::error("Engine::authorize: Rule::evaluate returned %ld returning errAuthorizationInternal", result);
			status = errAuthorizationInternal;
			break;
		}
	}

	if (outCredentials)
		outCredentials->swap(credentials);

	return status;
}

OSStatus
Engine::verifyModification(string inRightName, bool remove,
	const CredentialSet *inCredentials, CredentialSet *outCredentials, AuthorizationToken &auth)
{
	// Validate right

	// meta rights are constructed as follows:
	// we don't allow setting of wildcard rights, so you can only be more specific
	// note that you should never restrict things with a wildcard right without disallowing
	// changes to the entire domain.  ie. 
	//		system.privilege.   		-> never
	//		config.add.system.privilege.	-> never
	//		config.modify.system.privilege.	-> never
	//		config.delete.system.privilege.	-> never
	// For now we don't allow any configuration of configuration rules
	//		config.config. -> never
	
	string rightnameToCheck;
	
	// @@@ verify right name is is not NULL or zero length
	if (inRightName.length() == 0)
		return errAuthorizationDenied;
		
	// @@@ make sure it isn't a wildcard right by checking trailing "."
	if ( *(inRightName.rbegin()) == '.')
		return errAuthorizationDenied;
		
	// @@@ make sure it isn't a configure right by checking it doesn't start with config.
	if (inRightName.find(kConfigRight, 0) != string::npos)
	{
		// special handling of meta right change:
		// config.add. config.modify. config.remove. config.{}.
		// check for config.<right> (which always starts with config.config.)
		rightnameToCheck = string(kConfigRight) + inRightName;
	}
	else
	{
		// regular check of rights
		bool existingRule = mAuthdb.existRule(inRightName);
		if (!remove)
		{
			if (existingRule)
				rightnameToCheck = string(kAuthorizationConfigRightModify) + inRightName;
			else
				rightnameToCheck = string(kAuthorizationConfigRightAdd) + inRightName;
		}
		else
		{
			if (existingRule)
				rightnameToCheck = string(kAuthorizationConfigRightRemove) + inRightName;
			else
			{
				secdebug("engine", "rule %s doesn't exist.", inRightName.c_str());
				return errAuthorizationSuccess; // doesn't exist, done
			}
		}
	}

	
	AuthItemSet rights, environment, outRights;
	rights.insert(AuthItemRef(rightnameToCheck.c_str()));
	secdebug("engine", "authorizing %s for db modification.", rightnameToCheck.c_str());
	return authorize(rights, environment, kAuthorizationFlagDefaults | kAuthorizationFlagInteractionAllowed | kAuthorizationFlagExtendRights, inCredentials, outCredentials, outRights, auth);
}

OSStatus
Engine::getRule(string &inRightName, CFDictionaryRef *outRuleDefinition)
{
	// Get current time of day.
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

	// Update rules from database if needed
	mAuthdb.sync(now);

	CFDictionaryRef definition = mAuthdb.getRuleDefinition(inRightName);
	if (definition)
	{
		if (outRuleDefinition)
			*outRuleDefinition = definition;
		else
			CFRelease(definition);
		
		return errAuthorizationSuccess;
	}
	
	return errAuthorizationDenied;
}

OSStatus 
Engine::setRule(const char *inRightName, CFDictionaryRef inRuleDefinition, const CredentialSet *inCredentials, CredentialSet *outCredentials, AuthorizationToken &auth)
{
	// Get current time of day.
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

	// Update rules from database if needed
	mAuthdb.sync(now);

	// Validate rule by constructing it from the passed dictionary
	if (!mAuthdb.validateRule(inRightName, inRuleDefinition))
		return errAuthorizationDenied; // @@@ separate error for this?

	OSStatus result = verifyModification(inRightName, false /*setting, not removing*/, inCredentials, outCredentials, auth);
	if (result != errAuthorizationSuccess)
		return result;
			
	// set the rule for the right and save the database
	mAuthdb.setRule(inRightName, inRuleDefinition);

	return errAuthorizationSuccess;
}

OSStatus 
Engine::removeRule(const char *inRightName, const CredentialSet *inCredentials, CredentialSet *outCredentials, AuthorizationToken &auth)
{
	// Get current time of day.
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

	// Update rules from database if needed
	mAuthdb.sync(now);

	OSStatus result = verifyModification(inRightName, true /*removing*/, inCredentials, outCredentials, auth);
	if (result != errAuthorizationSuccess)
		return result;
	
	// set the rule for the right and save the database
	mAuthdb.removeRule(inRightName);

	return errAuthorizationSuccess;
}

}	// end namespace Authorization
