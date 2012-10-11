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
// Manage the Tower of Babel of CSSM dates and times.
//
#ifndef _H_CSSMDATES
#define _H_CSSMDATES

#include <Security/utilities.h>

#ifdef _CPP_CSSMDATES
#pragma export on
#endif

namespace Security
{

class CssmDate : public PodWrapper<CssmDate, CSSM_DATE>
{
};


class CssmStringDate
{
public:
	CssmStringDate(CSSM_TIMESTRING str);
private:
	CSSM_TIMESTRING timeString;
};

} // end namespace Security

#ifdef _CPP_CSSMDATES
#pragma export off
#endif

#endif //_H_CSSMDATES
