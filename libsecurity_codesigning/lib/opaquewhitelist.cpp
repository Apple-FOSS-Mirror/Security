/*
 * Copyright (c) 2014 Apple Inc. All Rights Reserved.
 * 
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#include "opaquewhitelist.h"
#include "csutilities.h"
#include "StaticCode.h"
#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecCodePriv.h>
#include <Security/SecCodeSigner.h>
#include <Security/SecStaticCode.h>
#include <security_utilities/cfutilities.h>
#include <security_utilities/cfmunge.h>
#include <CoreFoundation/CFBundlePriv.h>

namespace Security {
namespace CodeSigning {

using namespace SQLite;


static void traceHash(MessageTrace &trace, const char *key, CFDataRef hash);
static void attachOpaque(SecStaticCodeRef code);


//
// Open the database
//
OpaqueWhitelist::OpaqueWhitelist(const char *path, int flags)
	: SQLite::Database(path ? path : opaqueDatabase, flags)
{
	SQLite::Statement createConditions(*this,
		"CREATE TABLE IF NOT EXISTS conditions ("
		" label text,"
	   " weight real not null unique,"
	   " source text,"
	   " identifier text,"
	   " version text,"
	   " conditions text not null);"
	);
	createConditions.execute();

}

OpaqueWhitelist::~OpaqueWhitelist()
{ /* virtual */ }


//
// Check if a code object is whitelisted
//
bool OpaqueWhitelist::contains(SecStaticCodeRef codeRef, OSStatus reason, bool trace)
{
	// make our own copy of the code object, so we can poke at it without disturbing the original
	SecPointer<SecStaticCode> code = new SecStaticCode(SecStaticCode::requiredStatic(codeRef)->diskRep());

	CFCopyRef<CFDataRef> current = code->cdHash();	// current cdhash
	CFDataRef opaque = NULL;	// holds computed opaque cdhash
	bool match = false; 	// holds final result

	if (!current)
		return false;	// unsigned

	// collect auxiliary information for trace
	CFRef<CFDictionaryRef> info;
	std::string team = "";
	CFStringRef cfVersion = NULL, cfShortVersion = NULL, cfExecutable = NULL;
	if (errSecSuccess == SecCodeCopySigningInformation(code->handle(false), kSecCSSigningInformation, &info.aref())) {
		if (CFStringRef cfTeam = CFStringRef(CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier)))
			team = cfString(cfTeam);
		if (CFDictionaryRef infoPlist = CFDictionaryRef(CFDictionaryGetValue(info, kSecCodeInfoPList))) {
			if (CFTypeRef version = CFDictionaryGetValue(infoPlist, kCFBundleVersionKey))
				if (CFGetTypeID(version) == CFStringGetTypeID())
					cfVersion = CFStringRef(version);
			if (CFTypeRef shortVersion = CFDictionaryGetValue(infoPlist, _kCFBundleShortVersionStringKey))
				if (CFGetTypeID(shortVersion) == CFStringGetTypeID())
					cfShortVersion = CFStringRef(shortVersion);
			if (CFTypeRef executable = CFDictionaryGetValue(infoPlist, kCFBundleExecutableKey))
				if (CFGetTypeID(executable) == CFStringGetTypeID())
					cfExecutable = CFStringRef(executable);
		}
	}

	// compute and attach opaque signature
	attachOpaque(code->handle(false));
	opaque = code->cdHash();

	// lookup current cdhash in whitelist
	SQLite::Statement lookup(*this, "SELECT opaque FROM whitelist WHERE current=:current");
	lookup.bind(":current") = current.get();
	while (lookup.nextRow()) {
		CFRef<CFDataRef> expected = lookup[0].data();
		if (CFEqual(opaque, expected)) {
			match = true;	// actual opaque cdhash matches expected
			break;
		}
	}

	if (trace) {
		// send a trace indicating the result
		MessageTrace trace("com.apple.security.assessment.whitelist2", code->identifier().c_str());
		traceHash(trace, "signature2", current);
		traceHash(trace, "signature3", opaque);
		trace.add("result", match ? "pass" : "fail");
		trace.add("reason", "%d", reason);
		if (!team.empty())
			trace.add("teamid", "%s", team.c_str());
		if (cfVersion)
			trace.add("version", "%s", cfString(cfVersion).c_str());
		if (cfShortVersion)
			trace.add("version2", "%s", cfString(cfShortVersion).c_str());
		if (cfExecutable)
			trace.add("execname", "%s", cfString(cfExecutable).c_str());
		trace.send("");
	}

	return match;
}
	

//
// Obtain special validation conditions for a static code, based on database configuration.
//
CFDictionaryRef OpaqueWhitelist::validationConditionsFor(SecStaticCodeRef code)
{
	// figure out which team key to use
	std::string team = "UNKNOWN";
	CFStringRef cfId = NULL;
	CFStringRef cfVersion = NULL;
	CFRef<CFDictionaryRef> info;	// holds lifetimes for the above
	if (errSecSuccess == SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info.aref())) {
		if (CFStringRef cfTeam = CFStringRef(CFDictionaryGetValue(info, kSecCodeInfoTeamIdentifier)))
			team = cfString(cfTeam);
		cfId = CFStringRef(CFDictionaryGetValue(info, kSecCodeInfoIdentifier));
		if (CFDictionaryRef infoPlist = CFDictionaryRef(CFDictionaryGetValue(info, kSecCodeInfoPList)))
			if (CFTypeRef version = CFDictionaryGetValue(infoPlist, _kCFBundleShortVersionStringKey))
				if (CFGetTypeID(version) == CFStringGetTypeID())
					cfVersion = CFStringRef(version);
	}
	if (cfId == NULL)	// unsigned; punt
		return NULL;
	
	// find the highest weight matching condition. We perform no merging and the heaviest rule wins
	SQLite::Statement matches(*this,
		"SELECT conditions FROM conditions"
		" WHERE (source = :source or source IS NULL)"
		" AND (identifier = :identifier or identifier is NULL)"
		" AND ((:version IS NULL AND version IS NULL) OR (version = :version OR version IS NULL))"
		" ORDER BY weight DESC"
		" LIMIT 1"
	);
	matches.bind(":source") = team;
	matches.bind(":identifier") = cfString(cfId);
	if (cfVersion)
		matches.bind(":version") = cfString(cfVersion);
	if (matches.nextRow()) {
		CFTemp<CFDictionaryRef> conditions((const char*)matches[0]);
		return conditions.yield();
	}
	// no matches
	return NULL;
}


//
// Convert a SHA1 hash to hex and add to a trace
//
static void traceHash(MessageTrace &trace, const char *key, CFDataRef hash)
{
	if (CFDataGetLength(hash) != sizeof(SHA1::Digest)) {
		trace.add(key, "(unknown format)");
	} else {
		const UInt8 *bytes = CFDataGetBytePtr(hash);
		char s[2 * SHA1::digestLength + 1];
		for (unsigned n = 0; n < SHA1::digestLength; n++)
			sprintf(&s[2*n], "%2.2x", bytes[n]);
		trace.add(key, s);
	}
}


//
// Add a code object to the whitelist
//
void OpaqueWhitelist::add(SecStaticCodeRef codeRef)
{
	// make our own copy of the code object
	SecPointer<SecStaticCode> code = new SecStaticCode(SecStaticCode::requiredStatic(codeRef)->diskRep());

	CFCopyRef<CFDataRef> current = code->cdHash();
	attachOpaque(code->handle(false));	// compute and attach an opaque signature
	CFDataRef opaque = code->cdHash();

	SQLite::Statement insert(*this, "INSERT OR REPLACE INTO whitelist (current,opaque) VALUES (:current, :opaque)");
	insert.bind(":current") = current.get();
	insert.bind(":opaque") = opaque;
	insert.execute();
}


//
// Generate and attach an ad-hoc opaque signature
//
static void attachOpaque(SecStaticCodeRef code)
{
	CFTemp<CFDictionaryRef> rules("{"	// same resource rules as used for collection
		"rules={"
			"'^.*' = #T"
			"'^Info\\.plist$' = {omit=#T,weight=10}"
		"},rules2={"
			"'^(Frameworks|SharedFrameworks|Plugins|Plug-ins|XPCServices|Helpers|MacOS)/' = {nested=#T, weight=0}" 
			"'^.*' = #T"
			"'^Info\\.plist$' = {omit=#T,weight=10}"
			"'^[^/]+$' = {top=#T, weight=0}"
		"}"
	"}");

	CFRef<CFDataRef> signature = CFDataCreateMutable(NULL, 0);
	CFTemp<CFDictionaryRef> arguments("{%O=%O, %O=#N, %O=%O}",
		kSecCodeSignerDetached, signature.get(),
		kSecCodeSignerIdentity, /* kCFNull, */
		kSecCodeSignerResourceRules, rules.get());
	CFRef<SecCodeSignerRef> signer;
	SecCSFlags flags = kSecCSSignOpaque | kSecCSSignNoV1 | kSecCSSignBundleRoot;
	MacOSError::check(SecCodeSignerCreate(arguments, flags, &signer.aref()));
	MacOSError::check(SecCodeSignerAddSignature(signer, code, kSecCSDefaultFlags));
	MacOSError::check(SecCodeSetDetachedSignature(code, signature, kSecCSDefaultFlags));
}


} // end namespace CodeSigning
} // end namespace Security
