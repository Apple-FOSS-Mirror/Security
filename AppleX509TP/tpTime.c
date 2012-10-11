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
 * tpTime.c - cert related time functions
 *
 * Written 10/10/2000 by Doug Mitchell.
 */
 
#include "tpTime.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

/*
 * Given a string containing either a UTC-style or "generalized time"
 * time string, convert to a struct tm (in GMT/UTC). Returns nonzero on
 * error. 
 */
int timeStringToTm(
	const char			*str,
	unsigned			len,
	struct tm			*tmp)
{
	char 		szTemp[5];
	bool 		isUtc = false;			// 2-digit year
	bool		isCssmTime = false;		// no trailing 'Z'
	unsigned 	x;
	unsigned 	i;
	char 		*cp;

	if((str == NULL) || (len == 0) || (tmp == NULL)) {
    	return 1;
  	}
  	
  	/* tolerate NULL terminated or not */
  	if(str[len - 1] == '\0') {
  		len--;
  	}
  	switch(len) {
  		case UTC_TIME_STRLEN:			// 2-digit year, not Y2K compliant
  			isUtc = true;
  			break;
		case CSSM_TIME_STRLEN:
			isCssmTime = true;
			break;
  		case GENERALIZED_TIME_STRLEN:	// 4-digit year
  			break;
  		default:						// unknown format 
  			return 1;
  	}
  	
  	cp = (char *)str;
  	
	/* check that all characters except last are digits */
	for(i=0; i<(len - 1); i++) {
		if ( !(isdigit(cp[i])) ) {
		  	return 1;
		}
	}

  	/* check last character is a 'Z' or digit as appropriate */
	if(isCssmTime) {
		if(!isdigit(cp[len - 1])) {
			return 1;
		}
	}
	else {
		if(cp[len - 1] != 'Z' )	{
			return 1;
		}
	}
	
  	/* YEAR */
	szTemp[0] = *cp++;
	szTemp[1] = *cp++;
	if(!isUtc) {
		/* two more digits */
		szTemp[2] = *cp++;
		szTemp[3] = *cp++;
		szTemp[4] = '\0';
	}
	else { 
		szTemp[2] = '\0';
	}
	x = atoi( szTemp );
	if(isUtc) {
		/* 
		 * 2-digit year. 
		 *   0  <= year <  50 : assume century 21
		 *   50 <= year <  70 : illegal per PKIX
		 *   70 <  year <= 99 : assume century 20
		 */
		if(x < 50) {
			x += 2000;
		}
		else if(x < 70) {
			return 1;
		}
		else {
			/* century 20 */
			x += 1900;			
		}
	}
  	/* by definition - tm_year is year - 1900 */
  	tmp->tm_year = x - 1900;

  	/* MONTH */
	szTemp[0] = *cp++;
	szTemp[1] = *cp++;
	szTemp[2] = '\0';
	x = atoi( szTemp );
	/* in the string, months are from 1 to 12 */
	if((x > 12) || (x <= 0)) {
    	return 1;
	}
	/* in a tm, 0 to 11 */
  	tmp->tm_mon = x - 1;

 	/* DAY */
	szTemp[0] = *cp++;
	szTemp[1] = *cp++;
	szTemp[2] = '\0';
	x = atoi( szTemp );
	/* 1..31 in both formats */
	if((x > 31) || (x <= 0)) {
		return 1;
	}
  	tmp->tm_mday = x;

	/* HOUR */
	szTemp[0] = *cp++;
	szTemp[1] = *cp++;
	szTemp[2] = '\0';
	x = atoi( szTemp );
	if((x > 23) || (x < 0)) {
		return 1;
	}
	tmp->tm_hour = x;

  	/* MINUTE */
	szTemp[0] = *cp++;
	szTemp[1] = *cp++;
	szTemp[2] = '\0';
	x = atoi( szTemp );
	if((x > 59) || (x < 0)) {
		return 1;
	}
  	tmp->tm_min = x;

  	/* SECOND */
	szTemp[0] = *cp++;
	szTemp[1] = *cp++;
	szTemp[2] = '\0';
  	x = atoi( szTemp );
	if((x > 59) || (x < 0)) {
		return 1;
	}
  	tmp->tm_sec = x;
	return 0;
}

/* 
 * Return current GMT time as a struct tm.
 * Caller must hold tpTimeLock.
 */
void nowTime(
	struct tm *now)
{
	time_t nowTime = time(NULL);
	*now = *gmtime(&nowTime);
}

/*
 * Compare two times. Assumes they're both in GMT. Returns:
 * -1 if t1 <  t2
 *  0 if t1 == t2
 *  1 if t1 >  t2
 */
int compareTimes(
	const struct tm 	*t1,
	const struct tm 	*t2)
{
	if(t1->tm_year > t2->tm_year) {
		return 1;
	}
	else if(t1->tm_year < t2->tm_year) {
		return -1;
	}
	/* year equal */
	else if(t1->tm_mon > t2->tm_mon) {
		return 1;
	}
	else if(t1->tm_mon < t2->tm_mon) {
		return -1;
	}
	/* month equal */
	else if(t1->tm_mday > t2->tm_mday) {
		return 1;
	}
	else if(t1->tm_mday < t2->tm_mday) {
		return -1;
	}
	/* day of month equal */
	else if(t1->tm_hour > t2->tm_hour) {
		return 1;
	}
	else if(t1->tm_hour < t2->tm_hour) {
		return -1;
	}
	/* hour equal */
	else if(t1->tm_min > t2->tm_min) {
		return 1;
	}
	else if(t1->tm_min < t2->tm_min) {
		return -1;
	}
	/* minute equal */
	else if(t1->tm_sec > t2->tm_sec) {
		return 1;
	}
	else if(t1->tm_sec < t2->tm_sec) {
		return -1;
	}
	/* equal */
	return 0;
}

/*
 * Create a time string, in either UTC (2-digit) or or Generalized (4-digit)
 * year format. Caller mallocs the output string whose length is at least
 * (UTC_TIME_STRLEN+1) or (GENERALIZED_TIME_STRLEN+1) respectively.
 * Caller must hold tpTimeLock.
 */
void timeAtNowPlus(unsigned secFromNow, 
	TpTimeSpec timeSpec,
	char *outStr)
{
	struct tm utc;
	time_t baseTime;
	
	baseTime = time(NULL);
	baseTime += (time_t)secFromNow;
	utc = *gmtime(&baseTime);
	
	if(timeSpec == TIME_UTC) {
		/* UTC - 2 year digits - code which parses this assumes that
		 * (2-digit) years between 0 and 49 are in century 21 */
		if(utc.tm_year >= 100) {
			utc.tm_year -= 100;
		}
		sprintf(outStr, "%02d%02d%02d%02d%02d%02dZ",
			utc.tm_year /* + 1900 */, utc.tm_mon + 1,
			utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
	}
	else {
		sprintf(outStr, "%04d%02d%02d%02d%02d%02dZ",
			/* note year is relative to 1900, hopefully it'll have 
			 * four valid digits! */
			utc.tm_year + 1900, utc.tm_mon + 1,
			utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
	}
}
