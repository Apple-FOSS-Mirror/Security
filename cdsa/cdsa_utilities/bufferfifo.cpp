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
// bufferfifo - a Sink that queues data in a FIFO of buffers for retrieval
//
#include "bufferfifo.h"
#include <Security/memutils.h>


namespace Security {


//
// On destruction, throw away all queued buffers (that haven't been picked up)
//
BufferFifo::~BufferFifo()
{
    while (!mBuffers.empty()) {
        delete mBuffers.front();
        mBuffers.pop();
    }
}


//
// This is the put function of a Sink. We store the data in at most two buffers:
// First we append to the last (partially filled) one; then we allocate a new one
// (if needed) to hold the rest.
//
void BufferFifo::consume(const void *data, size_t size)
{
    // step 1: fill the rearmost (partially filled) buffer
    if (size > 0 && !mBuffers.empty()) {
        Buffer *current = mBuffers.back();
        size_t length = current->put(data, size);
        data = LowLevelMemoryUtilities::increment(data, length);
        size -= length;
    }
    // step 2: if there's anything left, make a new buffer and fill it
    if (size > 0) {	// not done
        Buffer *current = new Buffer(max(bufferLength, size));
        mBuffers.push(current);
        assert(current->available() >= size);
        current->put(data, size);
    }
}


//
// Pull the first (FI) buffer off the queue and deliver it.
// We retain no memory of it; it belongs to the caller now.
//
Buffer *BufferFifo::pop()
{
    assert(!mBuffers.empty());
    Buffer *top = mBuffers.front();
    mBuffers.pop();
    return top;
}


}	// end namespace Security
