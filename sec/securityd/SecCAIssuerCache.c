/*
 * Copyright (c) 2011 Apple Inc. All Rights Reserved.
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
 *
 * Created by Michael Brouwer on 6/17/11.
 */

/*
 *  SecCAIssuerCache.c - securityd
 */

#include <securityd/SecCAIssuerCache.h>
#include <utilities/debugging.h>
#include <Security/SecCertificateInternal.h>
#include <Security/SecFramework.h>
#include <Security/SecInternal.h>
#include <sqlite3.h>
#include <AssertMacros.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <dispatch/dispatch.h>
#include <asl.h>
#include "utilities/sqlutils.h"
#include "utilities/iOSforOSX.h"

#include <CoreFoundation/CFUtilities.h>
#include <utilities/SecFileLocations.h>

#define caissuerErrorLog(args...)     asl_log(NULL, NULL, ASL_LEVEL_ERR, ## args)

static const char expireSQL[] = "DELETE FROM issuers WHERE expires<?";
static const char beginTxnSQL[] = "BEGIN EXCLUSIVE TRANSACTION";
static const char endTxnSQL[] = "COMMIT TRANSACTION";
static const char insertIssuerSQL[] = "INSERT OR REPLACE INTO issuers "
    "(uri,expires,certificate) VALUES (?,?,?)";
static const char selectIssuerSQL[] = "SELECT certificate FROM "
    "issuers WHERE uri=?";

#define kSecCAIssuerFileName "caissuercache.sqlite3"

typedef struct __SecCAIssuerCache *SecCAIssuerCacheRef;
struct __SecCAIssuerCache {
    dispatch_queue_t queue;
	sqlite3 *s3h;
	sqlite3_stmt *expire;
	sqlite3_stmt *beginTxn;
	sqlite3_stmt *endTxn;
	sqlite3_stmt *insertIssuer;
	sqlite3_stmt *selectIssuer;
    bool in_transaction;
};

static dispatch_once_t kSecCAIssuerCacheOnce;
static SecCAIssuerCacheRef kSecCAIssuerCache;

/* @@@ Duplicated from SecTrustStore.c */
static int sec_create_path(const char *path)
{
	char pathbuf[PATH_MAX];
	size_t pos, len = strlen(path);
	if (len == 0 || len > PATH_MAX)
		return SQLITE_CANTOPEN;
	memcpy(pathbuf, path, len);
	for (pos = len-1; pos > 0; --pos)
	{
		/* Search backwards for trailing '/'. */
		if (pathbuf[pos] == '/')
		{
			pathbuf[pos] = '\0';
			/* Attempt to create parent directories of the database. */
			if (!mkdir(pathbuf, 0777))
				break;
			else
			{
				int err = errno;
				if (err == EEXIST)
					return 0;
				if (err == ENOTDIR)
					return SQLITE_CANTOPEN;
				if (err == EROFS)
					return SQLITE_READONLY;
				if (err == EACCES)
					return SQLITE_PERM;
				if (err == ENOSPC || err == EDQUOT)
					return SQLITE_FULL;
				if (err == EIO)
					return SQLITE_IOERR;

				/* EFAULT || ELOOP | ENAMETOOLONG || something else */
				return SQLITE_INTERNAL;
			}
		}
	}
	return SQLITE_OK;
}

static int sec_sqlite3_open(const char *db_name, sqlite3 **s3h,
                            bool create_path)
{
	int s3e;
	s3e = sqlite3_open(db_name, s3h);
	if (s3e == SQLITE_CANTOPEN && create_path) {
		/* Make sure the path to db_name exists and is writable, then
         try again. */
		s3e = sec_create_path(db_name);
		if (!s3e)
			s3e = sqlite3_open(db_name, s3h);
	}

	return s3e;
}

static int sec_sqlite3_reset(sqlite3_stmt *stmt, int s3e) {
    int s3e2;
    if (s3e == SQLITE_ROW || s3e == SQLITE_DONE)
        s3e = SQLITE_OK;
    s3e2 = sqlite3_reset(stmt);
    if (s3e2 && !s3e)
        s3e = s3e2;
    s3e2 = sqlite3_clear_bindings(stmt);
    if (s3e2 && !s3e)
        s3e = s3e2;
    return s3e;
}

static int SecCAIssuerCacheEnsureTxn(SecCAIssuerCacheRef this) {
    int s3e, s3e2;

    if (this->in_transaction)
        return SQLITE_OK;

    s3e = sqlite3_step(this->beginTxn);
    if (s3e == SQLITE_DONE) {
        this->in_transaction = true;
        s3e = SQLITE_OK;
    } else {
        secdebug("caissuercache", "sqlite3_step returned [%d]: %s", s3e,
                 sqlite3_errmsg(this->s3h));
    }
    s3e2 = sqlite3_reset(this->beginTxn);
    if (s3e2 && !s3e)
        s3e = s3e2;

    return s3e;
}

static int SecCAIssuerCacheCommitTxn(SecCAIssuerCacheRef this) {
    int s3e, s3e2;

    if (!this->in_transaction)
        return SQLITE_OK;

    s3e = sqlite3_step(this->endTxn);
    if (s3e == SQLITE_DONE) {
        this->in_transaction = false;
        s3e = SQLITE_OK;
    } else {
        secdebug("caissuercache", "sqlite3_step returned [%d]: %s", s3e,
                 sqlite3_errmsg(this->s3h));
    }
    s3e2 = sqlite3_reset(this->endTxn);
    if (s3e2 && !s3e)
        s3e = s3e2;

    return s3e;
}

static SecCAIssuerCacheRef SecCAIssuerCacheCreate(const char *db_name) {
	SecCAIssuerCacheRef this;
	int s3e;
    bool create = true;

    require(this = (SecCAIssuerCacheRef)calloc(sizeof(struct __SecCAIssuerCache), 1), errOut);
    require_action_quiet((this->queue = dispatch_queue_create("caissuercache", 0)), errOut, s3e = errSecAllocate);
    require_noerr(s3e = sec_sqlite3_open(db_name, &this->s3h, create), errOut);
    this->in_transaction = false;

	s3e = sqlite3_prepare_v2(this->s3h, beginTxnSQL, sizeof(beginTxnSQL),
                             &this->beginTxn, NULL);
	require_noerr(s3e, errOut);
	s3e = sqlite3_prepare_v2(this->s3h, endTxnSQL, sizeof(endTxnSQL),
                             &this->endTxn, NULL);
	require_noerr(s3e, errOut);

	s3e = sqlite3_prepare_v2(this->s3h, expireSQL, sizeof(expireSQL),
                             &this->expire, NULL);
	if (create && s3e == SQLITE_ERROR) {
        s3e = SecCAIssuerCacheEnsureTxn(this);
		require_noerr(s3e, errOut);

		/* sqlite3_prepare returns SQLITE_ERROR if the table we are
         compiling this statement for doesn't exist. */
		char *errmsg = NULL;
		s3e = sqlite3_exec(this->s3h,
                           "CREATE TABLE issuers("
                           "uri BLOB PRIMARY KEY,"
                           "expires DOUBLE NOT NULL,"
                           "certificate BLOB NOT NULL"
                           ");"
                           "CREATE INDEX iexpires ON issuers(expires);"
                           , NULL, NULL, &errmsg);
		if (errmsg) {
			caissuerErrorLog("caissuer db CREATE TABLES: %s", errmsg);
			sqlite3_free(errmsg);
		}
		require_noerr(s3e, errOut);
        s3e = sqlite3_prepare_v2(this->s3h, expireSQL, sizeof(expireSQL),
                                 &this->expire, NULL);
	}
	require_noerr(s3e, errOut);
	s3e = sqlite3_prepare_v2(this->s3h, insertIssuerSQL, sizeof(insertIssuerSQL),
                             &this->insertIssuer, NULL);
	require_noerr(s3e, errOut);
	s3e = sqlite3_prepare_v2(this->s3h, selectIssuerSQL, sizeof(selectIssuerSQL),
                             &this->selectIssuer, NULL);
	require_noerr(s3e, errOut);

	return this;

errOut:
	if (this) {
        if (this->queue)
            dispatch_release(this->queue);
        if (this->s3h)
            sqlite3_close(this->s3h);
		free(this);
	}

	return NULL;
}

static void SecCAIssuerCacheInit(void) {
    WithPathInKeychainDirectory(CFSTR(kSecCAIssuerFileName), ^(const char *utf8String) {
        kSecCAIssuerCache = SecCAIssuerCacheCreate(utf8String);
    });

    if (kSecCAIssuerCache)
        atexit(SecCAIssuerCacheGC);
}

/* Instance implemenation. */

static void _SecCAIssuerCacheAddCertificate(SecCAIssuerCacheRef this,
                                            SecCertificateRef certificate,
                                            CFURLRef uri, CFAbsoluteTime expires) {
    int s3e;

    secdebug("caissuercache", "adding certificate from %@", uri);
    require_noerr(s3e = SecCAIssuerCacheEnsureTxn(this), errOut);

    /* issuer.uri */
    CFDataRef uriData;
    require_action(uriData = CFURLCreateData(kCFAllocatorDefault, uri,
        kCFStringEncodingUTF8, false), errOut, s3e = SQLITE_NOMEM);
    s3e = sqlite3_bind_blob_wrapper(this->insertIssuer, 1,
        CFDataGetBytePtr(uriData), CFDataGetLength(uriData), SQLITE_TRANSIENT);
    CFRelease(uriData);

    /* issuer.expires */
    if (!s3e) s3e = sqlite3_bind_double(this->insertIssuer, 2, expires);

    /* issuer.certificate */
    if (!s3e) s3e = sqlite3_bind_blob_wrapper(this->insertIssuer, 3,
        SecCertificateGetBytePtr(certificate),
        SecCertificateGetLength(certificate), SQLITE_TRANSIENT);

    /* Execute the insert statement. */
    if (!s3e) s3e = sqlite3_step(this->insertIssuer);
    require_noerr(s3e = sec_sqlite3_reset(this->insertIssuer, s3e), errOut);

errOut:
    if (s3e) {
        caissuerErrorLog("caissuer cache add failed: %s", sqlite3_errmsg(this->s3h));
        /* TODO: Blow away the cache and create a new db. */
    }
}

static SecCertificateRef _SecCAIssuerCacheCopyMatching(SecCAIssuerCacheRef this,
                                                       CFURLRef uri) {
    SecCertificateRef certificate = NULL;
    int s3e = SQLITE_OK;

    CFDataRef uriData = NULL;
    require(uriData = CFURLCreateData(kCFAllocatorDefault, uri,
                                      kCFStringEncodingUTF8, false), errOut);
    s3e = sqlite3_bind_blob_wrapper(this->selectIssuer, 1, CFDataGetBytePtr(uriData),
                            CFDataGetLength(uriData), SQLITE_TRANSIENT);
    CFRelease(uriData);

    if (!s3e) s3e = sqlite3_step(this->selectIssuer);
    if (s3e == SQLITE_ROW) {
        /* Found an entry! */
        secdebug("caissuercache", "found cached response for %@", uri);

        const void *respData = sqlite3_column_blob(this->selectIssuer, 0);
        int respLen = sqlite3_column_bytes(this->selectIssuer, 0);
        certificate = SecCertificateCreateWithBytes(NULL, respData, respLen);
    }

    require_noerr(s3e = sec_sqlite3_reset(this->selectIssuer, s3e), errOut);

errOut:
    if (s3e) {
        if (s3e != SQLITE_DONE) {
            caissuerErrorLog("caissuer cache lookup failed: %s", sqlite3_errmsg(this->s3h));
            /* TODO: Blow away the cache and create a new db. */
        }

        if (certificate) {
            CFRelease(certificate);
            certificate = NULL;
        }
    }

    secdebug("caissuercache", "returning %s for %@", (certificate ? "cached response" : "NULL"), uri);

    return certificate;
}

static void _SecCAIssuerCacheGC(void *context) {
    SecCAIssuerCacheRef this = context;
    int s3e;

    require_noerr(s3e = SecCAIssuerCacheEnsureTxn(this), errOut);
    secdebug("caissuercache", "expiring stale responses");
    s3e = sqlite3_bind_double(this->expire, 1, CFAbsoluteTimeGetCurrent());
    if (!s3e) s3e = sqlite3_step(this->expire);
    require_noerr(s3e = sec_sqlite3_reset(this->expire, s3e), errOut);
    require_noerr(s3e = SecCAIssuerCacheCommitTxn(this), errOut);

errOut:
    if (s3e) {
        caissuerErrorLog("caissuer cache expire failed: %s", sqlite3_errmsg(this->s3h));
        /* TODO: Blow away the cache and create a new db. */
    }
}

static void _SecCAIssuerCacheFlush(void *context) {
    SecCAIssuerCacheRef this = context;
    int s3e;

    secdebug("caissuercache", "flushing pending changes");
    s3e = SecCAIssuerCacheCommitTxn(this);

    if (s3e) {
        caissuerErrorLog("caissuer cache flush failed: %s", sqlite3_errmsg(this->s3h));
        /* TODO: Blow away the cache and create a new db. */
    }
}

/* Public API */

void SecCAIssuerCacheAddCertificate(SecCertificateRef certificate,
                                    CFURLRef uri, CFAbsoluteTime expires) {
    dispatch_once(&kSecCAIssuerCacheOnce, ^{
        SecCAIssuerCacheInit();
    });
    if (!kSecCAIssuerCache)
        return;

    dispatch_sync(kSecCAIssuerCache->queue, ^{
        _SecCAIssuerCacheAddCertificate(kSecCAIssuerCache, certificate, uri, expires);
    });
}

SecCertificateRef SecCAIssuerCacheCopyMatching(CFURLRef uri) {
    dispatch_once(&kSecCAIssuerCacheOnce, ^{
        SecCAIssuerCacheInit();
    });
    __block SecCertificateRef cert = NULL;
    if (kSecCAIssuerCache)
        dispatch_sync(kSecCAIssuerCache->queue, ^{
            cert = _SecCAIssuerCacheCopyMatching(kSecCAIssuerCache, uri);
        });
    return cert;
}

/* This should be called on a normal non emergency exit. This function
 effectively does a SecCAIssuerCacheFlush.
 Currently this is called from our atexit handeler.
 This function expires any records that are stale and commits.

 Idea for future cache management policies:
 Expire old cache entires from database if:
 - The time to do so has arrived based on the nextExpire date in the
 policy table.
 - If the size of the database exceeds the limit set in the maxSize field
 in the policy table, vacuum the db.  If the database is still too
 big, expire records on a LRU basis.
 */
void SecCAIssuerCacheGC(void) {
    if (kSecCAIssuerCache)
        dispatch_sync(kSecCAIssuerCache->queue, ^{
            _SecCAIssuerCacheGC(kSecCAIssuerCache);
        });
}

/* Call this periodically or perhaps when we are exiting due to low memory. */
void SecCAIssuerCacheFlush(void) {
    if (kSecCAIssuerCache)
        dispatch_sync(kSecCAIssuerCache->queue, ^{
            _SecCAIssuerCacheFlush(kSecCAIssuerCache);
        });
}
