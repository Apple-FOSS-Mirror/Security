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
// debugsupport - support interface for making and managing debugger objects.
//
// This header is not needed for logging debug messages.
//
#ifndef _H_DEBUGSUPPORT
#define _H_DEBUGSUPPORT

//
// Generate stub-code support if NDEBUG (but not CLEAN_NDEBUG) is set, to support
// client code that may have been generated with debug enabled. You don't actually
// get *real* debug logging, of course, just cheap dummy stubs to keep the linker happy.
//
#if defined(NDEBUG) && !defined(CLEAN_NDEBUG)
# undef NDEBUG
# define NDEBUG_STUBS
#endif

#include <Security/debugging.h>
#include <Security/threading.h>
#include <cstdarg>
#include <set>

namespace Security {
namespace Debug {


#if !defined(NDEBUG)


//
// Debug scope names - short strings with value semantics.
// We don't use STL strings because of overhead.
//
class Name {
public:
	static const int maxLength = 12;

	Name(const char *s)
	{ strncpy(mName, s, maxLength-1); mName[maxLength-1] = '\0'; }
	
	Name(const char *start, const char *end)
	{
		int length = end - start; if (length >= maxLength) length = maxLength - 1;
		memcpy(mName, start, length); memset(mName + length, 0, maxLength - length);
	}
	
	operator const char *() const	{ return mName; }
	
	bool operator < (const Name &other) const
	{ return memcmp(mName, other.mName, maxLength) < 0; }
	
	bool operator == (const Name &other) const
	{ return memcmp(mName, other.mName, maxLength) == 0; }

private:
	char mName[maxLength];		// null terminated for easy printing
};


//
// A debugging Target. This is an object that receives debugging requests.
// You can have many, but one default one is always provided.
//
class Target {
public:
	Target();
	virtual ~Target();
	
	// get default (singleton) Target
	static Target &get();
	
	void setFromEnvironment();
	
public:
	class Sink {
	public:
		virtual ~Sink();
		virtual void put(const char *buffer, unsigned int length) = 0;
		virtual void dump(const char *format, va_list args);
		virtual void configure(const char *argument);
	};
	
	void to(Sink *sink);
	void to(const char *filename);
	void to(int syslogPriority);
	void to(FILE *openFile);
	
	void configure();						// from DEBUGOPTIONS
	void configure(const char *options);	// from explicit string
	
public:
	void message(const char *scope, const char *format, va_list args);
	bool debugging(const char *scope);
	void dump(const char *format, va_list args);
	bool dump(const char *scope);
	
protected:
	class Selector {
	public:
		Selector();
		void operator = (const char *config);
		
		bool operator () (const char *name) const;

	private:
		bool useSet;				// use contents of enableSet
		bool negate;				// negate meaning of enableSet
		set<Name> enableSet;		// set of names
	};
	
protected:
	static const size_t messageConstructionSize = 512;	// size of construction buffer

	Selector logSelector;			// selector for logging
	Selector dumpSelector;			// selector for dumping
	
	// output option state (from last configure call)
	bool showScope;					// include scope in output lines
	bool showThread;				// include #Threadid in output lines
	bool showPid;					// include [Pid] in output lines
	size_t dumpLimit;				// max. # of bytes dumped by dumpData & friends

	// current output support
	Sink *sink;
	
	// the default Target
	static Target *singleton;
};


//
// Standard Target::Sinks
//
class FileSink : public Target::Sink {
public:
	FileSink(FILE *f) : file(f), addDate(false), lockIO(true), lock(false) { }
	void put(const char *, unsigned int);
	void dump(const char *format, va_list args);
	void configure(const char *);
	
private:
	FILE *file;
	bool addDate;
	bool lockIO;
	Mutex lock;
};

class SyslogSink : public Target::Sink {
public:
	SyslogSink(int pri) : priority(pri), dumpBase(dumpBuffer), dumpPtr(dumpBuffer) { }
	void put(const char *, unsigned int);
	void dump(const char *format, va_list args);
	void configure(const char *);
	
private:
	int priority;
	
	static const size_t dumpBufferSize = 1024;
	char dumpBuffer[dumpBufferSize];
	char *dumpBase, *dumpPtr;
};


#else // NDEBUG

//
// Note that we don't scaffold up the entire Target hierarchy for NDEBUG.
// If you directly manipulate debug Targets, Names, or Sinks, you need to
// conditionalize the code based on NDEBUG.
//

#endif // NDEBUG


} // end namespace Debug

} // end namespace Security

#ifdef _CPP_DEBUGGING
#pragma export off
#endif

#endif //_H_DEBUGSUPPORT
