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
 * tpTime.h - cert related time functions
 *
 * Written 10/10/2000 by Doug Mitchell.
 */
 
#ifndef	_TP_TIME_H_
#define _TP_TIME_H_

#include <time.h>

#ifdef	__cplusplus
extern "C" {
#endif

/* lengths of time strings without trailing NULL */
#define UTC_TIME_STRLEN				13
#define CSSM_TIME_STRLEN			14		/* no trailing 'Z' */
#define GENERALIZED_TIME_STRLEN		15		

/*
 * Given a string containing either a UTC-style or "generalized time"
 * time string, convert to a struct tm (in GMT/UTC). Returns nonzero on
 * error. 
 */
extern int timeStringToTm(
	const char			*str,
	unsigned			len,
	struct tm			*tmp);

/* 
 * Return current GMT time as a struct tm.
 * Caller must hold tpTimeLock.
 */
extern void nowTime(
	struct tm		 	*now);

/*
 * Compare two times. Assumes they're both in GMT. Returns:
 * -1 if t1 <  t2
 *  0 if t1 == t2
 *  1 if t1 >  t2
 */
extern int compareTimes(
	const struct tm 	*t1,
	const struct tm 	*t2);
	
/*
 * Create a time string, in either UTC (2-digit) or or Generalized (4-digit)
 * year format. Caller mallocs the output string whose length is at least
 * (UTC_TIME_STRLEN+1) or (GENERALIZED_TIME_STRLEN+1) respectively.
 * Caller must hold tpTimeLock.
 */
typedef enum {
	TIME_UTC,
	TIME_GEN
} TpTimeSpec;

void timeAtNowPlus(unsigned secFromNow,
	TpTimeSpec timeSpec,
	char *outStr);

#ifdef	__cplusplus
}
#endif

#endif	/* _TP_TIME_H_*/
