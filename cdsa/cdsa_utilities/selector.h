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
// selector - I/O stream multiplexing
//
#ifndef _H_SELECTOR
#define _H_SELECTOR

#include <Security/utilities.h>
#include <Security/fdsel.h>
#include "timeflow.h"
#include <sys/types.h>
#include <cstdio>
#include <cstdarg>
#include <map>
#include <Security/debugging.h>


namespace Security {
namespace UnixPlusPlus {


//
// A Selector is an I/O dispatch facility that can supervise any number of "file descriptors",
// each of which can perform I/O. Obviously this is geared towards the UNIX facility.
//
class Selector {
    class Client; friend class Client;
public:
    Selector();
    virtual ~Selector();
    
    //@@@ preliminary interface
    void operator () ();		// run just once (now)
    void operator () (Time::Absolute stopTime);
    void operator () (Time::Interval duration)
    { (*this)(Time::now() + duration); }
    
    typedef unsigned int Type;
    static const Type none = 0x00;
    static const Type input = 0x01;
    static const Type output = 0x02;
    static const Type critical = 0x04;
    static const Type all = input | output | critical;
    
public:
    class Client {
        typedef Selector::Type Type;
        friend class Selector;
    public:
        Client() : mSelector(NULL) { }
        virtual void notify(int fd, Type type) = 0;
        virtual ~Client() { }
        
        bool isActive() const		{ return mSelector != NULL; }

        static const Type input = Selector::input;
        static const Type output = Selector::output;
        static const Type critical = Selector::critical;
        
    protected:
        void events(Type type)	{ mSelector->set(mFd, type); mEvents = type; }
        Type events() const		{ return mEvents; }

        void enable(Type type)	{ events(events() | type); }
        void disable(Type type)	{ events(events() & ~type); }

        template <class Sel> Sel &selectorAs()
        { assert(mSelector); return safer_cast<Sel &>(*mSelector); }
        
    private:
        int mFd;
        Selector *mSelector;
        Type mEvents;
    };
    
    void add(int fd, Client &client, Type type = all);
    void remove(int fd);
    bool isEmpty() const				{ return clientMap.empty(); }
    
private:
    void set(int fd, Type type);		// (re)set mask for one client
    
    void singleStep(Time::Interval maxWait);
    
private:
    unsigned int fdSetSize;				// number of fd_masks allocated in FDSets
    int fdMin, fdMax;					// highest/lowest fds in use
    FDSet inSet, outSet, errSet;		// current in/out/error select masks
    
private:
    typedef map<int, Client *> ClientMap;
    ClientMap clientMap;
};


}	// end namespace UnixPlusPlus
}	// end namespace Security


#endif //_H_SELECTOR
