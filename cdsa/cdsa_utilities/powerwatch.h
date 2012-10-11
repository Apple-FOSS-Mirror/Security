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
// powerwatch - hook into system notifications of power events
//
#ifndef _H_POWERWATCH
#define _H_POWERWATCH

#include <Security/machserver.h>
#include <IOKit/pwr_mgt/IOPMLib.h>


namespace Security {
namespace MachPlusPlus {


//
// PowerWatcher embodies the ability to respond to power events.
// By itself, it is inert - nobody will call its virtual methods.
// Use one of it subclasses, which take care of "hooking" into an
// event delivery mechanism.
//
class PowerWatcher {
public:
    PowerWatcher();
    virtual ~PowerWatcher();
    
protected:
    virtual void systemWillSleep();
    virtual void systemIsWaking();
    virtual void systemWillPowerDown();
    
protected:
    io_connect_t mKernelPort;
    IONotificationPortRef mPortRef;
    io_object_t mHandle;
    
    static void ioCallback(void *refCon, io_service_t service,
        natural_t messageType, void *argument);
};


//
// Hook into a "raw" MachServer object for event delivery
//
class PortPowerWatcher : public PowerWatcher, public MachServer::NoReplyHandler {
public:
    PortPowerWatcher();
    ~PortPowerWatcher();
    
    boolean_t handle(mach_msg_header_t *in);    
};


//
// Someone should add a RunLoopPowerWatcher class here, I suppose.
// Well, if you need one: Tag, You're It!
//


} // end namespace MachPlusPlus

} // end namespace Security

#endif //_H_POWERWATCH
