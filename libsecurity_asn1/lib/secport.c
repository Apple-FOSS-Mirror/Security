/*
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 * 
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 * 
 * The Original Code is the Netscape security libraries.
 * 
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are 
 * Copyright (C) 1994-2000 Netscape Communications Corporation.  All
 * Rights Reserved.
 * 
 * Contributor(s):
 * 
 * Alternatively, the contents of this file may be used under the
 * terms of the GNU General Public License Version 2 or later (the
 * "GPL"), in which case the provisions of the GPL are applicable 
 * instead of those above.  If you wish to allow use of your 
 * version of this file only under the terms of the GPL and not to
 * allow others to use your version of this file under the MPL,
 * indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by
 * the GPL.  If you do not delete the provisions above, a recipient
 * may use your version of this file under either the MPL or the
 * GPL.
 */

/*
 * secport.c - portability interfaces for security libraries
 *
 * This file abstracts out libc functionality that libsec depends on
 * 
 * NOTE - These are not public interfaces
 *
 * $Id: secport.c,v 1.5 2004/10/27 20:36:36 dmitch Exp $
 */

#include "seccomon.h"
#include "prmem.h"
#include "prerror.h"
#include "plarena.h"
#include "secerr.h"
#include "prmon.h"
#include "nsslocks.h"
#include "secport.h"
#include "prvrsion.h"
#include "prenv.h"

#ifdef DEBUG
//#define THREADMARK
#endif /* DEBUG */

#ifdef THREADMARK
#include "prthread.h"
#endif /* THREADMARK */

#if defined(XP_UNIX) || defined(XP_MAC) || defined(XP_OS2) || defined(XP_BEOS)
#include <stdlib.h>
#else
#include "wtypes.h"
#endif

#define SET_ERROR_CODE	/* place holder for code to set PR error code. */

#ifdef THREADMARK
typedef struct threadmark_mark_str {
  struct threadmark_mark_str *next;
  void *mark;
} threadmark_mark;

#endif /* THREADMARK */

/* The value of this magic must change each time PORTArenaPool changes. */
#define ARENAPOOL_MAGIC 0xB8AC9BDF 

/* enable/disable mutex in PORTArenaPool */
#define ARENA_POOL_LOCK		0

typedef struct PORTArenaPool_str {
  PLArenaPool arena;
  PRUint32    magic;
  #if ARENA_POOL_LOCK
  PRLock *    lock;
  #endif
#ifdef THREADMARK
  PRThread *marking_thread;
  threadmark_mark *first_mark;
#endif
} PORTArenaPool;


/* count of allocation failures. */
unsigned long port_allocFailures;

#ifndef __APPLE__
/* locations for registering Unicode conversion functions.  
 * XXX is this the appropriate location?  or should they be
 *     moved to client/server specific locations?
 */
PORTCharConversionFunc ucs4Utf8ConvertFunc;
PORTCharConversionFunc ucs2Utf8ConvertFunc;
PORTCharConversionWSwapFunc  ucs2AsciiConvertFunc;
#endif  /* __APPLE__ */

void *
PORT_Alloc(size_t bytes)
{
    void *rv;

    /* Always allocate a non-zero amount of bytes */
    rv = (void *)PR_Malloc(bytes ? bytes : 1);
    if (!rv) {
	++port_allocFailures;
	PORT_SetError(SEC_ERROR_NO_MEMORY);
    }
    return rv;
}

void *
PORT_Realloc(void *oldptr, size_t bytes)
{
    void *rv;

    rv = (void *)PR_Realloc(oldptr, bytes);
    if (!rv) {
	++port_allocFailures;
	PORT_SetError(SEC_ERROR_NO_MEMORY);
    }
    return rv;
}

void *
PORT_ZAlloc(size_t bytes)
{
    void *rv;

    /* Always allocate a non-zero amount of bytes */
    rv = (void *)PR_Calloc(1, bytes ? bytes : 1);
    if (!rv) {
	++port_allocFailures;
	PORT_SetError(SEC_ERROR_NO_MEMORY);
    }
    return rv;
}

void
PORT_Free(void *ptr)
{
    if (ptr) {
	PR_Free(ptr);
    }
}

void
PORT_ZFree(void *ptr, size_t len)
{
    if (ptr) {
	memset(ptr, 0, len);
	PR_Free(ptr);
    }
}

char *
PORT_Strdup(const char *str)
{
    size_t len = PORT_Strlen(str)+1;
    char *newstr;

    newstr = (char *)PORT_Alloc(len);
    if (newstr) {
        PORT_Memcpy(newstr, str, len);
    }
    return newstr;
}

void
PORT_SetError(int value)
{	
    PR_SetError(value, 0);
    return;
}

int
PORT_GetError(void)
{
    return(PR_GetError());
}

/********************* Arena code follows *****************************/

PLArenaPool *
PORT_NewArena(unsigned long chunksize)
{
    PORTArenaPool *pool;
    
    /* 64 bits cast: Safe. We only use chunksize 1024. */
    PORT_Assert(chunksize<=PR_UINT32_MAX);

    pool = PORT_ZNew(PORTArenaPool);
    if (!pool) {
	return NULL;
    }
    pool->magic = ARENAPOOL_MAGIC;
	#if ARENA_POOL_LOCK
    pool->lock = PZ_NewLock(nssILockArena);
    if (!pool->lock) {
		++port_allocFailures;
		PORT_Free(pool);
		return NULL;
    }
	#endif
    PL_InitArenaPool(&pool->arena, "security", (PRUint32) chunksize, (PRUint32)sizeof(double));
    return(&pool->arena);
}

void *
PORT_ArenaAlloc(PLArenaPool *arena, size_t size)
{
    void *p;

    PORTArenaPool *pool = (PORTArenaPool *)arena;

    PORT_Assert(size<=PR_UINT32_MAX);

    /* Is it one of ours?  Assume so and check the magic */
    if (ARENAPOOL_MAGIC == pool->magic ) {
		#if ARENA_POOL_LOCK
		PZ_Lock(pool->lock);
		#ifdef THREADMARK
			/* Most likely one of ours.  Is there a thread id? */
		if (pool->marking_thread  &&
			pool->marking_thread != PR_GetCurrentThread() ) {
			/* Another thread holds a mark in this arena */
			PZ_Unlock(pool->lock);
			PORT_SetError(SEC_ERROR_NO_MEMORY);
			PORT_Assert(0);
			return NULL;
		} /* tid != null */
		#endif /* THREADMARK */
		#endif /* ARENA_POOL_LOCK */
		PL_ARENA_ALLOCATE(p, arena, (PRUint32)size);
		#if ARENA_POOL_LOCK
		PZ_Unlock(pool->lock);
		#endif
    } else {
		PL_ARENA_ALLOCATE(p, arena, (PRUint32)size);
    }

    if (!p) {
	++port_allocFailures;
	PORT_SetError(SEC_ERROR_NO_MEMORY);
    }

    return(p);
}

void *
PORT_ArenaZAlloc(PLArenaPool *arena, size_t size)
{
    void *p = PORT_ArenaAlloc(arena, size);

    if (p) {
	PORT_Memset(p, 0, size);
    }

    return(p);
}

/* XXX - need to zeroize!! - jsw */
void
PORT_FreeArena(PLArenaPool *arena, PRBool zero)
{
    PORTArenaPool *pool = (PORTArenaPool *)arena;
	#if ARENA_POOL_LOCK
    PRLock *       lock = (PRLock *)0;
	#endif
    size_t         len  = sizeof *arena;
    extern const PRVersionDescription * libVersionPoint(void);
	#ifndef	__APPLE__
    static const PRVersionDescription * pvd;
	#endif
    static PRBool  doFreeArenaPool = PR_FALSE;

    if (ARENAPOOL_MAGIC == pool->magic ) {
		len  = sizeof *pool;
		#if ARENA_POOL_LOCK
		lock = pool->lock;
		PZ_Lock(lock);
		#endif
    }
	#ifndef __APPLE__
	/* dmitch - not needed */
    if (!pvd) {
		/* Each of NSPR's DLLs has a function libVersionPoint().
		** We could do a lot of extra work to be sure we're calling the
		** one in the DLL that holds PR_FreeArenaPool, but instead we
		** rely on the fact that ALL NSPR DLLs in the same directory
		** must be from the same release, and we call which ever one we get. 
		*/
		/* no need for thread protection here */
		pvd = libVersionPoint();
		if ((pvd->vMajor > 4) || 
			(pvd->vMajor == 4 && pvd->vMinor > 1) ||
			(pvd->vMajor == 4 && pvd->vMinor == 1 && pvd->vPatch >= 1)) {
			const char *ev = PR_GetEnv("NSS_DISABLE_ARENA_FREE_LIST");
			if (!ev) doFreeArenaPool = PR_TRUE;
		}
    }
	#endif
    if (doFreeArenaPool) {
		PL_FreeArenaPool(arena);
    } else {
		PL_FinishArenaPool(arena);
    }
	#if ARENA_POOL_LOCK
    if (lock) {
		PZ_Unlock(lock);
		PZ_DestroyLock(lock);
    }
	#endif
    PORT_ZFree(arena, len);
}

void *
PORT_ArenaGrow(PLArenaPool *arena, void *ptr, size_t oldsize, size_t newsize)
{
    PORTArenaPool *pool = (PORTArenaPool *)arena;
    PORT_Assert(newsize >= oldsize);
    PORT_Assert(oldsize <= PR_UINT32_MAX);
    PORT_Assert(newsize <= PR_UINT32_MAX);
    
    if (ARENAPOOL_MAGIC == pool->magic ) {
		#if ARENA_POOL_LOCK
		PZ_Lock(pool->lock);
		#endif
		/* Do we do a THREADMARK check here? */
		PL_ARENA_GROW(ptr, arena, (PRUint32)oldsize, (PRUint32)( newsize - oldsize ) );
		#if ARENA_POOL_LOCK
		PZ_Unlock(pool->lock);
		#endif
    } else {
		PL_ARENA_GROW(ptr, arena, (PRUint32)oldsize, (PRUint32)( newsize - oldsize ) );
    }
    
    return(ptr);
}

void *
PORT_ArenaMark(PLArenaPool *arena)
{
#if ARENA_MARK_ENABLE
    void * result;

    PORTArenaPool *pool = (PORTArenaPool *)arena;
    if (ARENAPOOL_MAGIC == pool->magic ) {
	PZ_Lock(pool->lock);
#ifdef THREADMARK
	{
	  threadmark_mark *tm, **pw;
	  PRThread * currentThread = PR_GetCurrentThread();

	    if (! pool->marking_thread ) {
		/* First mark */
		pool->marking_thread = currentThread;
	    } else if (currentThread != pool->marking_thread ) {
		PZ_Unlock(pool->lock);
		PORT_SetError(SEC_ERROR_NO_MEMORY);
		PORT_Assert(0);
		return NULL;
	    }

	    result = PL_ARENA_MARK(arena);
	    PL_ARENA_ALLOCATE(tm, arena, sizeof(threadmark_mark));
	    if (!tm) {
		PZ_Unlock(pool->lock);
		PORT_SetError(SEC_ERROR_NO_MEMORY);
		return NULL;
	    }

	    tm->mark = result;
	    tm->next = (threadmark_mark *)NULL;

	    pw = &pool->first_mark;
	    while( *pw ) {
		 pw = &(*pw)->next;
	    }

	    *pw = tm;
	}
#else /* THREADMARK */
	result = PL_ARENA_MARK(arena);
#endif /* THREADMARK */
	PZ_Unlock(pool->lock);
    } else {
	/* a "pure" NSPR arena */
	result = PL_ARENA_MARK(arena);
    }
    return result;
#else
	/* Some code in libsecurity_smime really checks for a nonzero 
	 * return here, so... */
	return (void *)-1;
#endif
}

void
PORT_ArenaRelease(PLArenaPool *arena, void *mark)
{
#if ARENA_MARK_ENABLE
   PORTArenaPool *pool = (PORTArenaPool *)arena;
    if (ARENAPOOL_MAGIC == pool->magic ) {
	PZ_Lock(pool->lock);
#ifdef THREADMARK
	{
	    threadmark_mark **pw, *tm;

	    if (PR_GetCurrentThread() != pool->marking_thread ) {
		PZ_Unlock(pool->lock);
		PORT_SetError(SEC_ERROR_NO_MEMORY);
		PORT_Assert(0);
		return /* no error indication available */ ;
	    }

	    pw = &pool->first_mark;
	    while( *pw && (mark != (*pw)->mark) ) {
		pw = &(*pw)->next;
	    }

	    if (! *pw ) {
		/* bad mark */
		PZ_Unlock(pool->lock);
		PORT_SetError(SEC_ERROR_NO_MEMORY);
		PORT_Assert(0);
		return /* no error indication available */ ;
	    }

	    tm = *pw;
	    *pw = (threadmark_mark *)NULL;

	    PL_ARENA_RELEASE(arena, mark);

	    if (! pool->first_mark ) {
		pool->marking_thread = (PRThread *)NULL;
	    }
	}
#else /* THREADMARK */
	PL_ARENA_RELEASE(arena, mark);
#endif /* THREADMARK */
	PZ_Unlock(pool->lock);
    } else {
	PL_ARENA_RELEASE(arena, mark);
    }
#endif	/* ARENA_MARK_ENABLE */
}

void
PORT_ArenaUnmark(PLArenaPool *arena, void *mark)
{
#if ARENA_MARK_ENABLE
#ifdef THREADMARK
    PORTArenaPool *pool = (PORTArenaPool *)arena;
    if (ARENAPOOL_MAGIC == pool->magic ) {
	threadmark_mark **pw, *tm;

	PZ_Lock(pool->lock);

	if (PR_GetCurrentThread() != pool->marking_thread ) {
	    PZ_Unlock(pool->lock);
	    PORT_SetError(SEC_ERROR_NO_MEMORY);
	    PORT_Assert(0);
	    return /* no error indication available */ ;
	}

	pw = &pool->first_mark;
	while( ((threadmark_mark *)NULL != *pw) && (mark != (*pw)->mark) ) {
	    pw = &(*pw)->next;
	}

	if ((threadmark_mark *)NULL == *pw ) {
	    /* bad mark */
	    PZ_Unlock(pool->lock);
	    PORT_SetError(SEC_ERROR_NO_MEMORY);
	    PORT_Assert(0);
	    return /* no error indication available */ ;
	}

	tm = *pw;
	*pw = (threadmark_mark *)NULL;

	if (! pool->first_mark ) {
	    pool->marking_thread = (PRThread *)NULL;
	}

	PZ_Unlock(pool->lock);
    }
#endif	/* THREADMARK */
#endif	/* ARENA_MARK_ENABLE */
}

char *
PORT_ArenaStrdup(PLArenaPool *arena, const char *str) {
    size_t len = PORT_Strlen(str)+1;
    char *newstr;

    newstr = (char*)PORT_ArenaAlloc(arena,len);
    if (newstr) {
        PORT_Memcpy(newstr,str,len);
    }
    return newstr;
}

/********************** end of arena functions ***********************/

#ifndef __APPLE__

/****************** unicode conversion functions ***********************/
/*
 * NOTE: These conversion functions all assume that the multibyte
 * characters are going to be in NETWORK BYTE ORDER, not host byte
 * order.  This is because the only time we deal with UCS-2 and UCS-4
 * are when the data was received from or is going to be sent out
 * over the wire (in, e.g. certificates).
 */

void
PORT_SetUCS4_UTF8ConversionFunction(PORTCharConversionFunc convFunc)
{ 
    ucs4Utf8ConvertFunc = convFunc;
}

void
PORT_SetUCS2_ASCIIConversionFunction(PORTCharConversionWSwapFunc convFunc)
{ 
    ucs2AsciiConvertFunc = convFunc;
}

void
PORT_SetUCS2_UTF8ConversionFunction(PORTCharConversionFunc convFunc)
{ 
    ucs2Utf8ConvertFunc = convFunc;
}

//#ifndef	__APPLE__
/* dmitch - not needed */
PRBool 
PORT_UCS4_UTF8Conversion(PRBool toUnicode, unsigned char *inBuf,
			 unsigned int inBufLen, unsigned char *outBuf,
			 unsigned int maxOutBufLen, unsigned int *outBufLen)
{
    if(!ucs4Utf8ConvertFunc) {
      return sec_port_ucs4_utf8_conversion_function(toUnicode,
        inBuf, inBufLen, outBuf, maxOutBufLen, outBufLen);
    }

    return (*ucs4Utf8ConvertFunc)(toUnicode, inBuf, inBufLen, outBuf, 
				  maxOutBufLen, outBufLen);
}

PRBool 
PORT_UCS2_UTF8Conversion(PRBool toUnicode, unsigned char *inBuf,
			 unsigned int inBufLen, unsigned char *outBuf,
			 unsigned int maxOutBufLen, unsigned int *outBufLen)
{
    if(!ucs2Utf8ConvertFunc) {
      return sec_port_ucs2_utf8_conversion_function(toUnicode,
        inBuf, inBufLen, outBuf, maxOutBufLen, outBufLen);
    }

    return (*ucs2Utf8ConvertFunc)(toUnicode, inBuf, inBufLen, outBuf, 
				  maxOutBufLen, outBufLen);
}
//#endif	/* __APPLE__ */

PRBool 
PORT_UCS2_ASCIIConversion(PRBool toUnicode, unsigned char *inBuf,
			  unsigned int inBufLen, unsigned char *outBuf,
			  unsigned int maxOutBufLen, unsigned int *outBufLen,
			  PRBool swapBytes)
{
    if(!ucs2AsciiConvertFunc) {
	return PR_FALSE;
    }

    return (*ucs2AsciiConvertFunc)(toUnicode, inBuf, inBufLen, outBuf, 
				  maxOutBufLen, outBufLen, swapBytes);
}


/* Portable putenv.  Creates/replaces an environment variable of the form
 *  envVarName=envValue
 */
int
NSS_PutEnv(const char * envVarName, const char * envValue)
{
#if  defined(XP_MAC) || defined(_WIN32_WCE)
    return SECFailure;
#else
    SECStatus result = SECSuccess;
    char *    encoded;
    int       putEnvFailed;
#ifdef _WIN32
    PRBool      setOK;

    setOK = SetEnvironmentVariable(envVarName, envValue);
    if (!setOK) {
        SET_ERROR_CODE
        return SECFailure;
    }
#endif

    encoded = (char *)PORT_ZAlloc(strlen(envVarName) + 2 + strlen(envValue));
    strcpy(encoded, envVarName);
    strcat(encoded, "=");
    strcat(encoded, envValue);

    putEnvFailed = putenv(encoded); /* adopt. */
    if (putEnvFailed) {
        SET_ERROR_CODE
        result = SECFailure;
        PORT_Free(encoded);
    }
    return result;
#endif
}
#endif  /* __APPLE__ */

