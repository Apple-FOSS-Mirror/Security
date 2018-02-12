/*
 * Copyright (c) 2016-2017 Apple Inc. All Rights Reserved.
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
 */

/*
 *  SecRevocationDb.c
 */

#include <securityd/SecRevocationDb.h>
#include <securityd/asynchttp.h>
#include <securityd/OTATrustUtilities.h>
#include <securityd/SecRevocationNetworking.h>
#include <Security/SecCertificateInternal.h>
#include <Security/SecCMS.h>
#include <Security/CMSDecoder.h>
#include <Security/SecFramework.h>
#include <Security/SecInternal.h>
#include <Security/SecPolicyPriv.h>
#include <AssertMacros.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <dispatch/dispatch.h>
#include <asl.h>
#include <copyfile.h>
#include "utilities/debugging.h"
#include "utilities/sec_action.h"
#include "utilities/sqlutils.h"
#include "utilities/SecAppleAnchorPriv.h"
#include "utilities/iOSforOSX.h"
#include <utilities/SecCFError.h>
#include <utilities/SecCFRelease.h>
#include <utilities/SecCFWrappers.h>
#include <utilities/SecDb.h>
#include <utilities/SecFileLocations.h>
#include <sqlite3.h>
#include <zlib.h>
#include <malloc/malloc.h>
#include <xpc/activity.h>
#include <xpc/private.h>
#include <os/transaction_private.h>

#include <CFNetwork/CFHTTPMessage.h>
#include <CoreFoundation/CFURL.h>
#include <CoreFoundation/CFUtilities.h>

static CFStringRef kValidUpdateServer   = CFSTR("valid.apple.com");

static CFStringRef kSecPrefsDomain      = CFSTR("com.apple.security");
static CFStringRef kUpdateServerKey     = CFSTR("ValidUpdateServer");
static CFStringRef kUpdateEnabledKey    = CFSTR("ValidUpdateEnabled");
static CFStringRef kUpdateIntervalKey   = CFSTR("ValidUpdateInterval");

typedef CF_OPTIONS(CFOptionFlags, SecValidInfoFlags) {
    kSecValidInfoComplete               = 1u << 0,
    kSecValidInfoCheckOCSP              = 1u << 1,
    kSecValidInfoKnownOnly              = 1u << 2,
    kSecValidInfoRequireCT              = 1u << 3,
    kSecValidInfoAllowlist              = 1u << 4,
    kSecValidInfoNoCACheck              = 1u << 5
};

/* minimum update interval */
#define kSecMinUpdateInterval           (60.0 * 5)

/* standard update interval */
#define kSecStdUpdateInterval           (60.0 * 60)

/* maximum allowed interval */
#define kSecMaxUpdateInterval           (60.0 * 60 * 24 * 7)

#define kSecRevocationBasePath          "/Library/Keychains/crls"
#define kSecRevocationCurUpdateFile     "update-current"
#define kSecRevocationDbFileName        "valid.sqlite3"
#define kSecRevocationDbReplaceFile     ".valid_replace"

/* database schema version
   v1 = initial version
   v2 = fix for group entry transitions
   v3 = handle optional entries in update dictionaries
   v4 = add db_format and db_source entries

   Note: kSecRevocationDbMinSchemaVersion is the lowest version whose
   results can be used. This allows revocation results to be obtained
   from an existing db before the next update interval occurs, at which
   time we'll update to the current version (kSecRevocationDbSchemaVersion).
*/
#define kSecRevocationDbSchemaVersion       4  /* current version we support */
#define kSecRevocationDbMinSchemaVersion    3  /* minimum version we can use */

/* update file format
*/
CF_ENUM(CFIndex) {
    kSecValidUpdateFormatG1             = 1,   /* initial version */
    kSecValidUpdateFormatG2             = 2,   /* signed content, single plist */
    kSecValidUpdateFormatG3             = 3    /* signed content, multiple plists */
};

#define kSecRevocationDbUpdateFormat        3  /* current version we support */
#define kSecRevocationDbMinUpdateFormat     2  /* minimum version we can use */

bool SecRevocationDbVerifyUpdate(void *update, CFIndex length);
CFIndex SecRevocationDbIngestUpdate(CFDictionaryRef update, CFIndex chunkVersion);
void SecRevocationDbApplyUpdate(CFDictionaryRef update, CFIndex version);
CFAbsoluteTime SecRevocationDbComputeNextUpdateTime(CFIndex updateInterval);
void SecRevocationDbSetSchemaVersion(CFIndex dbversion);
CFIndex SecRevocationDbGetUpdateFormat(void);
void SecRevocationDbSetUpdateFormat(CFIndex dbformat);
void SecRevocationDbSetUpdateSource(CFStringRef source);
CFStringRef SecRevocationDbCopyUpdateSource(void);
void SecRevocationDbSetNextUpdateTime(CFAbsoluteTime nextUpdate);
CFAbsoluteTime SecRevocationDbGetNextUpdateTime(void);
dispatch_queue_t SecRevocationDbGetUpdateQueue(void);
void SecRevocationDbRemoveAllEntries(void);
void SecRevocationDbReleaseAllConnections(void);


static CFDataRef copyInflatedData(CFDataRef data) {
    if (!data) {
        return NULL;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    /* 32 is a magic value which enables automatic header detection
       of gzip or zlib compressed data. */
    if (inflateInit2(&zs, 32+MAX_WBITS) != Z_OK) {
        return NULL;
    }
    zs.next_in = (UInt8 *)(CFDataGetBytePtr(data));
    zs.avail_in = (uInt)CFDataGetLength(data);

    CFMutableDataRef outData = CFDataCreateMutable(NULL, 0);
    if (!outData) {
        return NULL;
    }
    CFIndex buf_sz = malloc_good_size(zs.avail_in ? zs.avail_in : 1024 * 4);
    unsigned char *buf = malloc(buf_sz);
    int rc;
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = (uInt)buf_sz;
        rc = inflate(&zs, 0);
        CFIndex outLen = CFDataGetLength(outData);
        if (outLen < (CFIndex)zs.total_out) {
            CFDataAppendBytes(outData, (const UInt8*)buf, (CFIndex)zs.total_out - outLen);
        }
    } while (rc == Z_OK);

    inflateEnd(&zs);

    if (buf) {
        free(buf);
    }
    if (rc != Z_STREAM_END) {
        CFReleaseSafe(outData);
        return NULL;
    }
    return (CFDataRef)outData;
}

static CFDataRef copyInflatedDataToFile(CFDataRef data, char *fileName) {
    if (!data) {
        return NULL;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    /* 32 is a magic value which enables automatic header detection
       of gzip or zlib compressed data. */
    if (inflateInit2(&zs, 32+MAX_WBITS) != Z_OK) {
        return NULL;
    }
    zs.next_in = (UInt8 *)(CFDataGetBytePtr(data));
    zs.avail_in = (uInt)CFDataGetLength(data);

    (void)remove(fileName); /* We need an empty file to start */
    int fd;
    off_t off;
    fd = open(fileName, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0  || (off = lseek(fd, 0, SEEK_SET)) < 0) {
        secerror("unable to open %s (errno %d)", fileName, errno);
        if (fd >= 0) {
            close(fd);
        }
        return NULL;
    }

    CFIndex buf_sz = malloc_good_size(zs.avail_in ? zs.avail_in : 1024 * 4);
    unsigned char *buf = malloc(buf_sz);
    int rc;
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = (uInt)buf_sz;
        rc = inflate(&zs, 0);
        if (off < (int64_t)zs.total_out) {
            off = write(fd, buf, (size_t)zs.total_out - (size_t)off);
        }
    } while (rc == Z_OK);
    close(fd);

    inflateEnd(&zs);

    if (buf) {
        free(buf);
    }
    if (rc != Z_STREAM_END) {
        (void)remove(fileName);
        return NULL;
    }

    /* Now return an mmapped version of that data */
    CFDataRef outData = NULL;
    if ((rc = readValidFile(fileName, &outData)) != 0) {
        secerror("unable to read and map %s (errno %d)", fileName, rc);
        CFReleaseNull(outData);
    }
    return outData;
}

static CFDataRef copyDeflatedData(CFDataRef data) {
    if (!data) {
        return NULL;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit(&zs, Z_BEST_COMPRESSION) != Z_OK) {
        return NULL;
    }
    zs.next_in = (UInt8 *)(CFDataGetBytePtr(data));
    zs.avail_in = (uInt)CFDataGetLength(data);

    CFMutableDataRef outData = CFDataCreateMutable(NULL, 0);
    if (!outData) {
        return NULL;
    }
    CFIndex buf_sz = malloc_good_size(zs.avail_in ? zs.avail_in : 1024 * 4);
    unsigned char *buf = malloc(buf_sz);
    int rc = Z_BUF_ERROR;
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = (uInt)buf_sz;
        rc = deflate(&zs, Z_FINISH);

        if (rc == Z_OK || rc == Z_STREAM_END) {
            CFIndex buf_used = buf_sz - zs.avail_out;
            CFDataAppendBytes(outData, (const UInt8*)buf, buf_used);
        }
        else if (rc == Z_BUF_ERROR) {
            free(buf);
            buf_sz = malloc_good_size(buf_sz * 2);
            buf = malloc(buf_sz);
            if (buf) {
                rc = Z_OK; /* try again with larger buffer */
            }
        }
    } while (rc == Z_OK && zs.avail_in);

    deflateEnd(&zs);

    if (buf) {
        free(buf);
    }
    if (rc != Z_STREAM_END) {
        CFReleaseSafe(outData);
        return NULL;
    }
    return (CFDataRef)outData;
}

/* Read file opens the file, mmaps it and then closes the file. */
int readValidFile(const char *fileName,
                    CFDataRef  *bytes) {   // mmapped and returned -- must be munmapped!
    int rtn, fd;
    const uint8_t *buf = NULL;
    struct stat	sb;
    size_t size = 0;

    *bytes = NULL;
    fd = open(fileName, O_RDONLY);
    if (fd < 0) { return errno; }
    rtn = fstat(fd, &sb);
    if (rtn) { goto errOut; }
    if (sb.st_size > (off_t) ((UINT32_MAX >> 1)-1)) {
        rtn = EFBIG;
        goto errOut;
    }
    size = (size_t)sb.st_size;

    buf = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (!buf || buf == MAP_FAILED) {
        rtn = errno;
        secerror("unable to map %s (errno %d)", fileName, rtn);
        goto errOut;
    }

    *bytes = CFDataCreateWithBytesNoCopy(NULL, buf, size, kCFAllocatorNull);

errOut:
    close(fd);
    if(rtn) {
        CFReleaseNull(*bytes);
        if (buf) {
            int unmap_err = munmap((void *)buf, size);
            if (unmap_err != 0) {
                secerror("unable to unmap %ld bytes at %p (error %d)", (long)size, buf, rtn);
            }
        }
    }
    return rtn;
}

static void unmapData(CFDataRef CF_CONSUMED data) {
    if (data) {
        int rtn = munmap((void *)CFDataGetBytePtr(data), CFDataGetLength(data));
        if (rtn != 0) {
            secerror("unable to unmap %ld bytes at %p (error %d)", CFDataGetLength(data), CFDataGetBytePtr(data), rtn);
        }

    }
    CFReleaseNull(data);
}

static bool removeFileWithSuffix(const char *basepath, const char *suffix) {
    bool result = false;
    char *path = NULL;
    asprintf(&path, "%s%s", basepath, suffix);
    if (path) {
        if (remove(path) == -1) {
            int error = errno;
            if (error == ENOENT) {
                result = true; // not an error if the file did not exist
            } else {
                secnotice("validupdate", "remove (%s): %s", path, strerror(error));
            }
        } else {
            result = true;
        }
        free(path);
    }
    return result;
}

static bool isDbOwner() {
#if TARGET_OS_EMBEDDED
    if (getuid() == 64) // _securityd
#else
    if (getuid() == 0)
#endif
    {
        return true;
    }
    return false;
}


// MARK: -
// MARK: SecValidUpdate

/* ======================================================================
   SecValidUpdate
   ======================================================================*/

CFAbsoluteTime gUpdateStarted = 0.0;
CFAbsoluteTime gNextUpdate = 0.0;
static CFIndex gUpdateInterval = 0;
static CFIndex gLastVersion = 0;

/* Update Format:
    1. The length of the signed data, as a 4-byte integer in network byte order.
    2. The signed data, which consists of:
        a. A 4-byte integer in network byte order, the count of plists to follow; and then for each plist:
            i. A 4-byte integer, the length of each plist
            ii. A plist, in binary form
        b. There may be other data after the plists in the signed data, described by a future version of this specification.
    3. The length of the following CMS blob, as a 4-byte integer in network byte order.
    4. A detached CMS signature of the signed data described above.
    5. There may be additional data after the CMS blob, described by a future version of this specification.

   Note: the difference between g2 and g3 format is the addition of the 4-byte count in (2a).
*/
static bool SecValidUpdateProcessData(CFIndex format, CFDataRef updateData) {
    if (!updateData || format < 2) {
        return false;
    }
    bool result = false;
    CFIndex version = 0;
    CFIndex interval = 0;
    const UInt8* p = CFDataGetBytePtr(updateData);
    size_t bytesRemaining = (p) ? (size_t)CFDataGetLength(updateData) : 0;
    /* make sure there is enough data to contain length and count */
    if (bytesRemaining < ((CFIndex)sizeof(uint32_t) * 2)) {
        secinfo("validupdate", "Skipping property list creation (length %ld is too short)", (long)bytesRemaining);
        return result;
    }
    /* get length of signed data */
    uint32_t dataLength = OSSwapInt32(*((uint32_t *)p));
    bytesRemaining -= sizeof(uint32_t);
    p += sizeof(uint32_t);

    /* get plist count (G3 format and later) */
    uint32_t plistCount = 1;
    uint32_t plistTotal = 1;
    if (format > kSecValidUpdateFormatG2) {
        plistCount = OSSwapInt32(*((uint32_t *)p));
        plistTotal = plistCount;
        bytesRemaining -= sizeof(uint32_t);
        p += sizeof(uint32_t);
    }
    if (dataLength > bytesRemaining) {
        secinfo("validupdate", "Skipping property list creation (dataLength=%ld, bytesRemaining=%ld)",
                (long)dataLength, (long)bytesRemaining);
        return result;
    }

    /* process each chunked plist */
    uint32_t plistProcessed = 0;
    while (plistCount > 0 && bytesRemaining > 0) {
        CFPropertyListRef propertyList = NULL;
        uint32_t plistLength = dataLength;
        if (format > kSecValidUpdateFormatG2) {
            plistLength = OSSwapInt32(*((uint32_t *)p));
            bytesRemaining -= sizeof(uint32_t);
            p += sizeof(uint32_t);
        }
        --plistCount;
        ++plistProcessed;

        /* We're about to use a lot of memory for the plist -- go active so we don't get jetsammed */
        os_transaction_t transaction;
        transaction = os_transaction_create("com.apple.trustd.valid");

        if (plistLength <= bytesRemaining) {
            CFDataRef data = CFDataCreateWithBytesNoCopy(NULL, p, plistLength, kCFAllocatorNull);
            propertyList = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
            CFReleaseNull(data);
        }
        if (isDictionary(propertyList)) {
            secdebug("validupdate", "Ingesting plist chunk %u of %u, length: %u",
                    plistProcessed, plistTotal, plistLength);
            CFIndex curVersion = SecRevocationDbIngestUpdate((CFDictionaryRef)propertyList, version);
            if (plistProcessed == 1) {
                version = curVersion;
                // get server-provided interval
                CFTypeRef value = (CFNumberRef)CFDictionaryGetValue((CFDictionaryRef)propertyList,
                                                                    CFSTR("check-again"));
                if (isNumber(value)) {
                    CFNumberGetValue((CFNumberRef)value, kCFNumberCFIndexType, &interval);
                }
            }
            if (curVersion < 0) {
                plistCount = 0; // we already had this version; skip remaining plists
                result = true;
            }
        } else {
            secinfo("validupdate", "Failed to deserialize update chunk %u of %u",
                    plistProcessed, plistTotal);
            if (plistProcessed == 1) {
                gNextUpdate = SecRevocationDbComputeNextUpdateTime(0);
            }
        }
        /* All finished with this property list */
        CFReleaseSafe(propertyList);
        os_release(transaction);

        bytesRemaining -= plistLength;
        p += plistLength;
    }

    if (version > 0) {
        secdebug("validupdate", "Update received: v%lu", (unsigned long)version);
        gLastVersion = version;
        gNextUpdate = SecRevocationDbComputeNextUpdateTime(interval);
        secdebug("validupdate", "Next update time: %f", gNextUpdate);
        result = true;
    }

    // remember next update time in case of restart
    SecRevocationDbSetNextUpdateTime(gNextUpdate);

    return result;
}

void SecValidUpdateVerifyAndIngest(CFDataRef updateData) {
    if (!updateData) {
        secnotice("validupdate", "invalid update data");
        return;
    }
    /* Verify CMS signature on signed data */
    if (SecRevocationDbVerifyUpdate((void *)CFDataGetBytePtr(updateData), CFDataGetLength(updateData))) {
        bool result = SecValidUpdateProcessData(kSecValidUpdateFormatG3, updateData);
        if (!result) {
            // Try g2 update format as a fallback if we failed to read g3
            result = SecValidUpdateProcessData(kSecValidUpdateFormatG2, updateData);
        }
        if (!result) {
            secerror("failed to process valid update");
        }
    } else {
        secerror("failed to verify valid update");
    }
}

static bool SecValidUpdateFromCompressed(CFDataRef CF_CONSUMED data) {
    if (!data) { return false; }

    /* We're about to use a lot of memory for the uncompressed update -- go active */
    os_transaction_t transaction;
    transaction = os_transaction_create("com.apple.trustd.valid");

    /* Expand the update */
    __block CFDataRef inflatedData = NULL;
    WithPathInRevocationInfoDirectory(CFSTR(kSecRevocationCurUpdateFile), ^(const char *curUpdatePath) {
        inflatedData = copyInflatedDataToFile(data, (char *)curUpdatePath);
        secdebug("validupdate", "data expanded: %ld bytes", (long)CFDataGetLength(inflatedData));
    });
    unmapData(data);
    os_release(transaction);

    if (inflatedData) {
        SecValidUpdateVerifyAndIngest(inflatedData);
        unmapData(inflatedData);
    }

    /* All done with the temporary file */
    WithPathInRevocationInfoDirectory(CFSTR(kSecRevocationCurUpdateFile), ^(const char *curUpdatePath) {
        (void)removeFileWithSuffix(curUpdatePath, "");
    });

    return true;
}

static bool SecValidDatabaseFromCompressed(CFDataRef CF_CONSUMED data) {
    if (!data) { return false; }

    secdebug("validupdate", "read %ld bytes from file", (long)CFDataGetLength(data));

    /* We're about to use a lot of memory for the uncompressed update -- go active */
    os_transaction_t transaction;
    transaction = os_transaction_create("com.apple.trustd.valid");

    /* Expand the database */
    __block CFDataRef inflatedData = NULL;
    WithPathInRevocationInfoDirectory(CFSTR(kSecRevocationDbFileName), ^(const char *dbPath) {
        inflatedData = copyInflatedDataToFile(data, (char *)dbPath);
        secdebug("validupdate", "data expanded: %ld bytes", (long)CFDataGetLength(inflatedData));
    });
    unmapData(data);
    os_release(transaction);

    if (inflatedData) {
        unmapData(inflatedData);
    }
    return true;
}

static bool SecValidUpdateSatisfiedLocally(CFStringRef server, CFIndex version, bool safeToReplace) {
    __block bool result = false;
    CFDataRef data = NULL;
    SecOTAPKIRef otapkiRef = NULL;
    int rtn = 0;
    static int sNumLocalUpdates = 0;

    // if we've replaced the database with a local asset twice in a row,
    // something is wrong with it. Get this update from the server.
    if (sNumLocalUpdates > 1) {
        secdebug("validupdate", "%d consecutive db resets, ignoring local asset", sNumLocalUpdates);
        goto updateExit;
    }

    // if a non-production server is specified, we will not be able to use a
    // local production asset since its update sequence will be different.
    if (kCFCompareEqualTo != CFStringCompare(server, kValidUpdateServer,
        kCFCompareCaseInsensitive)) {
        secdebug("validupdate", "non-production server specified, ignoring local asset");
        goto updateExit;
    }

    // check static database asset(s)
    otapkiRef = SecOTAPKICopyCurrentOTAPKIRef();
    if (!otapkiRef) {
        goto updateExit;
    }
    CFIndex assetVersion = SecOTAPKIGetValidSnapshotVersion(otapkiRef);
    CFIndex assetFormat = SecOTAPKIGetValidSnapshotFormat(otapkiRef);
    // version <= 0 means the database is invalid or empty.
    // version > 0 means we have some version, but we need to see if a
    // newer version is available as a local asset.
    if (assetVersion <= version || assetFormat < kSecValidUpdateFormatG3) {
        // asset is not newer than ours, or its version is unknown
        goto updateExit;
    }

    // replace database only if safe to do so (i.e. called at startup)
    if (!safeToReplace) {
        // write semaphore file that we will pick up when we next launch
        char *semPathBuf = NULL;
        asprintf(&semPathBuf, "%s/%s", kSecRevocationBasePath, kSecRevocationDbReplaceFile);
        if (semPathBuf) {
            struct stat sb;
            int fd = open(semPathBuf, O_WRONLY | O_CREAT, DEFFILEMODE);
            if (fd == -1 || fstat(fd, &sb)) {
                secnotice("validupdate", "unable to write %s", semPathBuf);
            }
            if (fd >= 0) {
                close(fd);
            }
            free(semPathBuf);
        }
        // exit as gracefully as possible so we can replace the database
        secnotice("validupdate", "process exiting to replace db file");
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3ull*NSEC_PER_SEC), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
            xpc_transaction_exit_clean();
        });
        goto updateExit;
    }

    // try to copy uncompressed database asset, if available
    const char *validDbPathBuf = SecOTAPKIGetValidDatabaseSnapshot(otapkiRef);
    if (validDbPathBuf) {
        WithPathInRevocationInfoDirectory(CFSTR(kSecRevocationDbFileName), ^(const char *path) {
            secdebug("validupdate", "will copy data from \"%s\"", validDbPathBuf);
            copyfile_state_t state = copyfile_state_alloc();
            int retval = copyfile(validDbPathBuf, path, state, COPYFILE_DATA);
            copyfile_state_free(state);
            if (retval < 0) {
                secnotice("validupdate", "copyfile error %d", retval);
            } else {
                result = true;
            }
        });
    }
    if (result) {
        goto updateExit;
    }

    // see if compressed database asset is available
    if (validDbPathBuf) {
        char *validDbCmpPathBuf = NULL;
        asprintf(&validDbCmpPathBuf, "%s%s", validDbPathBuf, ".gz");
        if (validDbCmpPathBuf) {
            secdebug("validupdate", "will read data from \"%s\"", validDbCmpPathBuf);
            if ((rtn = readValidFile(validDbCmpPathBuf, &data)) != 0) {
                unmapData(data);
                data = NULL;
                secnotice("validupdate", "readValidFile error %d", rtn);
            }
            free(validDbCmpPathBuf);
        }
    }
    result = SecValidDatabaseFromCompressed(data);
    if (result) {
        goto updateExit;
    }

    // unable to use database asset; try update asset
    const char *validUpdatePathBuf = SecOTAPKIGetValidUpdateSnapshot(otapkiRef);
    if (validUpdatePathBuf) {
        secdebug("validupdate", "will read data from \"%s\"", validUpdatePathBuf);
        if ((rtn = readValidFile(validUpdatePathBuf, &data)) != 0) {
            unmapData(data);
            data = NULL;
            secnotice("validupdate", "readValidFile error %d", rtn);
        }
    }
    result = SecValidUpdateFromCompressed(data);

updateExit:
    CFReleaseNull(otapkiRef);
    if (result) {
        sNumLocalUpdates++;
        SecRevocationDbSetUpdateSource(server);
        gLastVersion = SecRevocationDbGetVersion();
        gUpdateStarted = 0;
        secdebug("validupdate", "local update to g%ld/v%ld complete at %f",
                 (long)SecRevocationDbGetUpdateFormat(), (long)gLastVersion,
                 (double)CFAbsoluteTimeGetCurrent());
    } else {
        sNumLocalUpdates = 0; // reset counter
    }
    return result;
}

static bool SecValidUpdateSchedule(bool updateEnabled, CFStringRef server, CFIndex version) {
    /* Check if we have a later version available locally */
    if (SecValidUpdateSatisfiedLocally(server, version, false)) {
        return true;
    }

    /* If update not permitted return */
    if (!updateEnabled) {
        return false;
    }

#if !TARGET_OS_BRIDGE
    /* Schedule as a maintenance task */
    return SecValidUpdateRequest(SecRevocationDbGetUpdateQueue(), server, version);
#else
    return false;
#endif
}

void SecRevocationDbInitialize() {
    if (!isDbOwner()) { return; }
    __block bool initializeDb = false;

    /* create base path if it doesn't exist */
    (void)mkpath_np(kSecRevocationBasePath, 0755);

    /* check semaphore file */
    WithPathInRevocationInfoDirectory(CFSTR(kSecRevocationDbReplaceFile), ^(const char *path) {
        struct stat sb;
        if (stat(path, &sb) == 0) {
            initializeDb = true; /* file was found, so we will replace the database */
            if (remove(path) == -1) {
                int error = errno;
                secnotice("validupdate", "remove (%s): %s", path, strerror(error));
            }
        }
    });

    /* check database */
    WithPathInRevocationInfoDirectory(CFSTR(kSecRevocationDbFileName), ^(const char *path) {
        if (initializeDb) {
            /* remove old database file(s) */
            (void)removeFileWithSuffix(path, "");
            (void)removeFileWithSuffix(path, "-journal");
            (void)removeFileWithSuffix(path, "-shm");
            (void)removeFileWithSuffix(path, "-wal");
        }
        else {
            struct stat sb;
            if (stat(path, &sb) == -1) {
                initializeDb = true; /* file not found, so we will create the database */
            }
        }
    });

    if (!initializeDb) {
        return; /* database exists and doesn't need replacing */
    }

    /* initialize database from local asset */
    CFTypeRef value = (CFStringRef)CFPreferencesCopyValue(kUpdateServerKey, kSecPrefsDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
    CFStringRef server = (isString(value)) ? (CFStringRef)value : (CFStringRef)kValidUpdateServer;
    CFIndex version = 0;
    secnotice("validupdate", "initializing database");
    if (!SecValidUpdateSatisfiedLocally(server, version, true)) {
#if !TARGET_OS_BRIDGE
        /* Schedule full update as a maintenance task */
        (void)SecValidUpdateRequest(SecRevocationDbGetUpdateQueue(), server, version);
#endif
    }
    CFReleaseSafe(value);
}


// MARK: -
// MARK: SecValidInfoRef

/* ======================================================================
   SecValidInfoRef
   ======================================================================
 */

static SecValidInfoRef SecValidInfoCreate(SecValidInfoFormat format,
                                          CFOptionFlags flags,
                                          bool isOnList,
                                          CFDataRef certHash,
                                          CFDataRef issuerHash,
                                          CFDataRef anchorHash) {
    SecValidInfoRef validInfo;
    validInfo = (SecValidInfoRef)calloc(1, sizeof(struct __SecValidInfo));
    if (!validInfo) { return NULL; }

    CFRetainSafe(certHash);
    CFRetainSafe(issuerHash);
    validInfo->format = format;
    validInfo->certHash = certHash;
    validInfo->issuerHash = issuerHash;
    validInfo->anchorHash = anchorHash;
    validInfo->isOnList = isOnList;
    validInfo->valid = (flags & kSecValidInfoAllowlist);
    validInfo->complete = (flags & kSecValidInfoComplete);
    validInfo->checkOCSP = (flags & kSecValidInfoCheckOCSP);
    validInfo->knownOnly = (flags & kSecValidInfoKnownOnly);
    validInfo->requireCT = (flags & kSecValidInfoRequireCT);
    validInfo->noCACheck = (flags & kSecValidInfoNoCACheck);

    return validInfo;
}

void SecValidInfoRelease(SecValidInfoRef validInfo) {
    if (validInfo) {
        CFReleaseSafe(validInfo->certHash);
        CFReleaseSafe(validInfo->issuerHash);
        CFReleaseSafe(validInfo->anchorHash);
        free(validInfo);
    }
}

void SecValidInfoSetAnchor(SecValidInfoRef validInfo, SecCertificateRef anchor) {
    if (!validInfo) {
        return;
    }
    CFDataRef anchorHash = NULL;
    if (anchor) {
        anchorHash = SecCertificateCopySHA256Digest(anchor);

        /* clear no-ca flag for anchors where we want OCSP checked [32523118] */
        if (SecIsAppleTrustAnchor(anchor, 0)) {
            validInfo->noCACheck = false;
        }
    }
    CFReleaseNull(validInfo->anchorHash);
    validInfo->anchorHash = anchorHash;
}


// MARK: -
// MARK: SecRevocationDb

/* ======================================================================
   SecRevocationDb
   ======================================================================
*/

/* SecRevocationDbCheckNextUpdate returns true if we dispatched an
   update request, otherwise false.
*/
static bool _SecRevocationDbCheckNextUpdate(void) {
    // are we the db owner instance?
    if (!isDbOwner()) {
        return false;
    }
    CFTypeRef value = NULL;

    // is it time to check?
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    CFAbsoluteTime minNextUpdate = now + gUpdateInterval;
    gUpdateStarted = now;

    if (0 == gNextUpdate) {
        // first time we're called, check if we have a saved nextUpdate value
        gNextUpdate = SecRevocationDbGetNextUpdateTime();
        minNextUpdate = now;
        if (gNextUpdate < minNextUpdate) {
            gNextUpdate = minNextUpdate;
        }
        // allow pref to override update interval, if it exists
        CFIndex interval = -1;
        value = (CFNumberRef)CFPreferencesCopyValue(kUpdateIntervalKey, kSecPrefsDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
        if (isNumber(value)) {
            if (CFNumberGetValue((CFNumberRef)value, kCFNumberCFIndexType, &interval)) {
                if (interval < kSecMinUpdateInterval) {
                    interval = kSecMinUpdateInterval;
                } else if (interval > kSecMaxUpdateInterval) {
                    interval = kSecMaxUpdateInterval;
                }
            }
        }
        CFReleaseNull(value);
        gUpdateInterval = kSecStdUpdateInterval;
        if (interval > 0) {
            gUpdateInterval = interval;
        }
        // pin next update time to the preferred update interval
        if (gNextUpdate > (gUpdateStarted + gUpdateInterval)) {
            gNextUpdate = gUpdateStarted + gUpdateInterval;
        }
        secdebug("validupdate", "next update at %f (in %f seconds)",
                 (double)gUpdateStarted, (double)gNextUpdate-gUpdateStarted);
    }
    if (gNextUpdate > now) {
        gUpdateStarted = 0;
        return false;
    }
    secnotice("validupdate", "starting update");

    // set minimum next update time here in case we can't get an update
    gNextUpdate = minNextUpdate;

    // determine which server to query
    CFStringRef server;
    value = (CFStringRef)CFPreferencesCopyValue(kUpdateServerKey, kSecPrefsDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
    if (isString(value)) {
        server = (CFStringRef) CFRetain(value);
    } else {
        server = (CFStringRef) CFRetain(kValidUpdateServer);
    }
    CFReleaseNull(value);

    // determine version of our current database
    CFIndex version = SecRevocationDbGetVersion();
    secdebug("validupdate", "got version %ld from db", (long)version);
    if (version <= 0) {
        if (gLastVersion > 0) {
            secdebug("validupdate", "error getting version; using last good version: %ld", (long)gLastVersion);
        }
        version = gLastVersion;
    }

    // determine source of our current database
    // (if this ever changes, we will need to reload the db)
    CFStringRef db_source = SecRevocationDbCopyUpdateSource();
    if (!db_source) {
        db_source = (CFStringRef) CFRetain(kValidUpdateServer);
    }

    // determine whether we need to recreate the database
    CFIndex db_version = SecRevocationDbGetSchemaVersion();
    CFIndex db_format = SecRevocationDbGetUpdateFormat();
    if (db_version < kSecRevocationDbSchemaVersion ||
        db_format < kSecRevocationDbUpdateFormat ||
        kCFCompareEqualTo != CFStringCompare(server, db_source, kCFCompareCaseInsensitive)) {
        /* we need to fully rebuild the db contents. */
        SecRevocationDbRemoveAllEntries();
        version = gLastVersion = 0;
    }

    // determine whether update fetching is enabled
#if (__MAC_OS_X_VERSION_MIN_REQUIRED >= 101300 || __IPHONE_OS_VERSION_MIN_REQUIRED >= 110000)
    bool updateEnabled = true; // macOS 10.13 or iOS 11.0
#else
    bool updateEnabled = false;
#endif
    value = (CFBooleanRef)CFPreferencesCopyValue(kUpdateEnabledKey, kSecPrefsDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
    if (isBoolean(value)) {
        updateEnabled = CFBooleanGetValue((CFBooleanRef)value);
    }
    CFReleaseNull(value);

    // Schedule maintenance work
    bool result = SecValidUpdateSchedule(updateEnabled, server, version);
    CFReleaseNull(server);
    CFReleaseNull(db_source);
    return result;
}

void SecRevocationDbCheckNextUpdate(void) {
    static dispatch_once_t once;
    static sec_action_t action;

    dispatch_once(&once, ^{
        dispatch_queue_t update_queue = SecRevocationDbGetUpdateQueue();
        action = sec_action_create_with_queue(update_queue, "update_check", kSecMinUpdateInterval);
        sec_action_set_handler(action, ^{
            (void)_SecRevocationDbCheckNextUpdate();
        });
    });
    sec_action_perform(action);
}

/*  This function verifies an update, in this format:
    1) unsigned 32-bit network-byte-order length of binary plist
    2) binary plist data
    3) unsigned 32-bit network-byte-order length of CMS message
    4) CMS message (containing certificates and signature over binary plist)

    The length argument is the total size of the packed update data.
*/
bool SecRevocationDbVerifyUpdate(void *update, CFIndex length) {
    if (!update || length <= (CFIndex)sizeof(uint32_t)) {
        return false;
    }
    uint32_t plistLength = OSSwapInt32(*((uint32_t *)update));
    if ((plistLength + (CFIndex)(sizeof(uint32_t)*2)) > (uint64_t) length) {
        secdebug("validupdate", "ERROR: reported plist length (%lu)+%lu exceeds total length (%lu)\n",
                (unsigned long)plistLength, (unsigned long)sizeof(uint32_t)*2, (unsigned long)length);
        return false;
    }
    uint8_t *plistData = (uint8_t *)update + sizeof(uint32_t);
    uint8_t *sigData = (uint8_t *)plistData + plistLength;
    uint32_t sigLength = OSSwapInt32(*((uint32_t *)sigData));
    sigData += sizeof(uint32_t);
    if ((plistLength + sigLength + (CFIndex)(sizeof(uint32_t) * 2)) != (uint64_t) length) {
        secdebug("validupdate", "ERROR: reported lengths do not add up to total length\n");
        return false;
    }

    OSStatus status = 0;
    CMSSignerStatus signerStatus;
    CMSDecoderRef cms = NULL;
    SecPolicyRef policy = NULL;
    SecTrustRef trust = NULL;
    CFDataRef content = NULL;

    if ((content = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault,
        (const UInt8 *)plistData, (CFIndex)plistLength, kCFAllocatorNull)) == NULL) {
        secdebug("validupdate", "CFDataCreateWithBytesNoCopy failed (%ld bytes)\n", (long)plistLength);
        return false;
    }

    if ((status = CMSDecoderCreate(&cms)) != errSecSuccess) {
        secdebug("validupdate", "CMSDecoderCreate failed with error %d\n", (int)status);
        goto verifyExit;
    }
    if ((status = CMSDecoderUpdateMessage(cms, sigData, sigLength)) != errSecSuccess) {
        secdebug("validupdate", "CMSDecoderUpdateMessage failed with error %d\n", (int)status);
        goto verifyExit;
    }
    if ((status = CMSDecoderSetDetachedContent(cms, content)) != errSecSuccess) {
        secdebug("validupdate", "CMSDecoderSetDetachedContent failed with error %d\n", (int)status);
        goto verifyExit;
    }
    if ((status = CMSDecoderFinalizeMessage(cms)) != errSecSuccess) {
        secdebug("validupdate", "CMSDecoderFinalizeMessage failed with error %d\n", (int)status);
        goto verifyExit;
    }

    policy = SecPolicyCreateApplePinned(CFSTR("ValidUpdate"), // kSecPolicyNameAppleValidUpdate
                CFSTR("1.2.840.113635.100.6.2.10"), // System Integration 2 Intermediate Certificate
                CFSTR("1.2.840.113635.100.6.51"));  // Valid update signing OID

    // Check that the first signer actually signed this message.
    if ((status = CMSDecoderCopySignerStatus(cms, 0, policy,
            false, &signerStatus, &trust, NULL)) != errSecSuccess) {
        secdebug("validupdate", "CMSDecoderCopySignerStatus failed with error %d\n", (int)status);
        goto verifyExit;
    }
    // Make sure the signature verifies against the detached content
    if (signerStatus != kCMSSignerValid) {
        secdebug("validupdate", "ERROR: signature did not verify (signer status %d)\n", (int)signerStatus);
        status = errSecInvalidSignature;
        goto verifyExit;
    }
    // Make sure the signing certificate is valid for the specified policy
    SecTrustResultType trustResult = kSecTrustResultInvalid;
    status = SecTrustEvaluate(trust, &trustResult);
    if (status != errSecSuccess) {
        secdebug("validupdate", "SecTrustEvaluate failed with error %d (trust=%p)\n", (int)status, (void *)trust);
    } else if (!(trustResult == kSecTrustResultUnspecified || trustResult == kSecTrustResultProceed)) {
        secdebug("validupdate", "SecTrustEvaluate failed with trust result %d\n", (int)trustResult);
        status = errSecVerificationFailure;
        goto verifyExit;
    }

verifyExit:
    CFReleaseSafe(content);
    CFReleaseSafe(trust);
    CFReleaseSafe(policy);
    CFReleaseSafe(cms);

    return (status == errSecSuccess);
}

CFAbsoluteTime SecRevocationDbComputeNextUpdateTime(CFIndex updateInterval) {
    CFIndex interval = updateInterval;
    // try to use interval preference if it exists
    CFTypeRef value = (CFNumberRef)CFPreferencesCopyValue(kUpdateIntervalKey, kSecPrefsDomain, kCFPreferencesAnyUser, kCFPreferencesCurrentHost);
    if (isNumber(value)) {
        CFNumberGetValue((CFNumberRef)value, kCFNumberCFIndexType, &interval);
    }
    CFReleaseNull(value);

    if (interval <= 0) {
        interval = kSecStdUpdateInterval;
    }

    // sanity check
    if (interval < kSecMinUpdateInterval) {
        interval = kSecMinUpdateInterval;
    } else if (interval > kSecMaxUpdateInterval) {
        interval = kSecMaxUpdateInterval;
    }

    // compute randomization factor, between 0 and 50% of the interval
    CFIndex fuzz = arc4random() % (long)(interval/2.0);
    CFAbsoluteTime nextUpdate =  CFAbsoluteTimeGetCurrent() + interval + fuzz;
    secdebug("validupdate", "next update in %ld seconds", (long)(interval + fuzz));
    return nextUpdate;
}

void SecRevocationDbComputeAndSetNextUpdateTime(void) {
    gNextUpdate = SecRevocationDbComputeNextUpdateTime(0);
    SecRevocationDbSetNextUpdateTime(gNextUpdate);
    gUpdateStarted = 0; /* no update is currently in progress */
}

CFIndex SecRevocationDbIngestUpdate(CFDictionaryRef update, CFIndex chunkVersion) {
    CFIndex version = 0;
    if (!update) {
        return version;
    }
    CFTypeRef value = (CFNumberRef)CFDictionaryGetValue(update, CFSTR("version"));
    if (isNumber(value)) {
        if (!CFNumberGetValue((CFNumberRef)value, kCFNumberCFIndexType, &version)) {
            version = 0;
        }
    }
    if (version == 0) {
        // only the first chunk will have a version, so the second and
        // subsequent chunks will need to pass it in chunkVersion.
        version = chunkVersion;
    }
    CFIndex curVersion = SecRevocationDbGetVersion();
    if (version > curVersion || chunkVersion > 0) {
        SecRevocationDbApplyUpdate(update, version);
    } else {
        secdebug("validupdate", "we have v%ld, skipping update to v%ld",
                 (long)curVersion, (long)version);
        version = -1; // invalid, so we know to skip subsequent chunks
    }
    return version;
}


/* Database schema */

/* admin table holds these key-value (or key-ival) pairs:
 'version' (integer)    // version of database content
 'check_again' (double) // CFAbsoluteTime of next check (optional; this value is currently stored in prefs)
 'db_version' (integer) // version of database schema
 'db_hash' (blob)       // SHA-256 database hash
 --> entries in admin table are unique by text key

 issuers table holds map of issuing CA hashes to group identifiers:
 groupid (integer)     // associated group identifier in group ID table
 issuer_hash (blob)    // SHA-256 hash of issuer certificate (primary key)
 --> entries in issuers table are unique by issuer_hash;
 multiple issuer entries may have the same groupid!

 groups table holds records with these attributes:
 groupid (integer)     // ordinal ID associated with this group entry
 flags (integer)       // a bitmask of the following values:
   kSecValidInfoComplete   (0x00000001) set if we have all revocation info for this issuer group
   kSecValidInfoCheckOCSP  (0x00000002) set if must check ocsp for certs from this issuer group
   kSecValidInfoKnownOnly  (0x00000004) set if any CA from this issuer group must be in database
   kSecValidInfoRequireCT  (0x00000008) set if all certs from this issuer group must have SCTs
   kSecValidInfoAllowlist  (0x00000010) set if this entry describes valid certs (i.e. is allowed)
   kSecValidInfoNoCACheck  (0x00000020) set if this entry does not require an OCSP check to accept
 format (integer)      // an integer describing format of entries:
   kSecValidInfoFormatUnknown (0) unknown format
   kSecValidInfoFormatSerial  (1) serial number, not greater than 20 bytes in length
   kSecValidInfoFormatSHA256  (2) SHA-256 hash, 32 bytes in length
   kSecValidInfoFormatNto1    (3) filter data blob of arbitrary length
 data (blob)           // Bloom filter data if format is 'nto1', otherwise NULL
 --> entries in groups table are unique by groupid

 serials table holds serial number blobs with these attributes:
 rowid (integer)       // ordinal ID associated with this serial number entry
 groupid (integer)     // identifier for issuer group in the groups table
 serial (blob)         // serial number
 --> entries in serials table are unique by serial and groupid

 hashes table holds SHA-256 hashes of certificates with these attributes:
 rowid (integer)       // ordinal ID associated with this sha256 hash entry
 groupid (integer)     // identifier for issuer group in the groups table
 sha256 (blob)         // SHA-256 hash of subject certificate
 --> entries in hashes table are unique by sha256 and groupid
 */
#define createTablesSQL   CFSTR("CREATE TABLE admin(" \
                                "key TEXT PRIMARY KEY NOT NULL," \
                                "ival INTEGER NOT NULL," \
                                "value BLOB" \
                                ");" \
                                "CREATE TABLE issuers(" \
                                "groupid INTEGER NOT NULL," \
                                "issuer_hash BLOB PRIMARY KEY NOT NULL" \
                                ");" \
                                "CREATE INDEX issuer_idx ON issuers(issuer_hash);" \
                                "CREATE TABLE groups(" \
                                "groupid INTEGER PRIMARY KEY AUTOINCREMENT," \
                                "flags INTEGER," \
                                "format INTEGER," \
                                "data BLOB" \
                                ");" \
                                "CREATE TABLE serials(" \
                                "rowid INTEGER PRIMARY KEY AUTOINCREMENT," \
                                "groupid INTEGER NOT NULL," \
                                "serial BLOB NOT NULL," \
                                "UNIQUE(groupid,serial)" \
                                ");" \
                                "CREATE TABLE hashes(" \
                                "rowid INTEGER PRIMARY KEY AUTOINCREMENT," \
                                "groupid INTEGER NOT NULL," \
                                "sha256 BLOB NOT NULL," \
                                "UNIQUE(groupid,sha256)" \
                                ");" \
                                "CREATE TRIGGER group_del BEFORE DELETE ON groups FOR EACH ROW " \
                                "BEGIN " \
                                "DELETE FROM serials WHERE groupid=OLD.groupid; " \
                                "DELETE FROM hashes WHERE groupid=OLD.groupid; " \
                                "DELETE FROM issuers WHERE groupid=OLD.groupid; " \
                                "END;")

#define selectGroupIdSQL  CFSTR("SELECT DISTINCT groupid " \
"FROM issuers WHERE issuer_hash=?")
#define selectVersionSQL CFSTR("SELECT ival FROM admin " \
"WHERE key='version'")
#define selectDbVersionSQL CFSTR("SELECT ival FROM admin " \
"WHERE key='db_version'")
#define selectDbFormatSQL CFSTR("SELECT ival FROM admin " \
"WHERE key='db_format'")
#define selectDbHashSQL CFSTR("SELECT value FROM admin " \
"WHERE key='db_hash'")
#define selectDbSourceSQL CFSTR("SELECT value FROM admin " \
"WHERE key='db_source'")
#define selectNextUpdateSQL CFSTR("SELECT value FROM admin " \
"WHERE key='check_again'")
#define selectGroupRecordSQL CFSTR("SELECT flags,format,data FROM " \
"groups WHERE groupid=?")
#define selectSerialRecordSQL CFSTR("SELECT rowid FROM serials " \
"WHERE groupid=? AND serial=?")
#define selectHashRecordSQL CFSTR("SELECT rowid FROM hashes " \
"WHERE groupid=? AND sha256=?")
#define insertAdminRecordSQL CFSTR("INSERT OR REPLACE INTO admin " \
"(key,ival,value) VALUES (?,?,?)")
#define insertIssuerRecordSQL CFSTR("INSERT OR REPLACE INTO issuers " \
"(groupid,issuer_hash) VALUES (?,?)")
#define insertGroupRecordSQL CFSTR("INSERT OR REPLACE INTO groups " \
"(groupid,flags,format,data) VALUES (?,?,?,?)")
#define insertSerialRecordSQL CFSTR("INSERT OR REPLACE INTO serials " \
"(groupid,serial) VALUES (?,?)")
#define insertSha256RecordSQL CFSTR("INSERT OR REPLACE INTO hashes " \
"(groupid,sha256) VALUES (?,?)")
#define deleteGroupRecordSQL CFSTR("DELETE FROM groups WHERE groupid=?")

#define deleteAllEntriesSQL CFSTR("DELETE from hashes; " \
"DELETE from serials; DELETE from issuers; DELETE from groups; " \
"DELETE from admin; DELETE from sqlite_sequence")
#define deleteTablesSQL CFSTR("DROP TABLE hashes; " \
"DROP TABLE serials; DROP TABLE issuers; DROP TABLE groups; " \
"DROP TABLE admin; DELETE from sqlite_sequence")

/* Database management */

static SecDbRef SecRevocationDbCreate(CFStringRef path) {
    /* only the db owner should open a read-write connection. */
    bool readWrite = isDbOwner();
    mode_t mode = 0644;

    SecDbRef result = SecDbCreateWithOptions(path, mode, readWrite, false, false, ^bool (SecDbRef db, SecDbConnectionRef dbconn, bool didCreate, bool *callMeAgainForNextConnection, CFErrorRef *error) {
        __block bool ok = true;
        CFErrorRef localError = NULL;
        if (ok && !SecDbWithSQL(dbconn, selectGroupIdSQL, &localError, NULL) && CFErrorGetCode(localError) == SQLITE_ERROR) {
            /* SecDbWithSQL returns SQLITE_ERROR if the table we are preparing the above statement for doesn't exist. */

            /* Create all database tables, indexes, and triggers. */
            ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, error, ^(bool *commit) {
                ok = SecDbExec(dbconn, createTablesSQL, error);
                *commit = ok;
            });
        }
        CFReleaseSafe(localError);
        if (!ok)
            secerror("%s failed: %@", didCreate ? "Create" : "Open", error ? *error : NULL);
        return ok;
    });

    return result;
}

typedef struct __SecRevocationDb *SecRevocationDbRef;
struct __SecRevocationDb {
    SecDbRef db;
    dispatch_queue_t update_queue;
    bool updateInProgress;
    bool unsupportedVersion;
};

static dispatch_once_t kSecRevocationDbOnce;
static SecRevocationDbRef kSecRevocationDb = NULL;

static SecRevocationDbRef SecRevocationDbInit(CFStringRef db_name) {
    SecRevocationDbRef rdb;
    dispatch_queue_attr_t attr;

    require(rdb = (SecRevocationDbRef)malloc(sizeof(struct __SecRevocationDb)), errOut);
    rdb->db = NULL;
    rdb->update_queue = NULL;
    rdb->updateInProgress = false;
    rdb->unsupportedVersion = false;

    require(rdb->db = SecRevocationDbCreate(db_name), errOut);
    attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_BACKGROUND, 0);
    require(rdb->update_queue = dispatch_queue_create(NULL, attr), errOut);

    return rdb;

errOut:
    secdebug("validupdate", "Failed to create db at \"%@\"", db_name);
    if (rdb) {
        if (rdb->update_queue) {
            dispatch_release(rdb->update_queue);
        }
        CFReleaseSafe(rdb->db);
        free(rdb);
    }
    return NULL;
}

static CFStringRef SecRevocationDbCopyPath(void) {
    CFURLRef revDbURL = NULL;
    CFStringRef revInfoRelPath = NULL;
    if ((revInfoRelPath = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s"), kSecRevocationDbFileName)) != NULL) {
        revDbURL = SecCopyURLForFileInRevocationInfoDirectory(revInfoRelPath);
    }
    CFReleaseSafe(revInfoRelPath);

    CFStringRef revDbPath = NULL;
    if (revDbURL) {
        revDbPath = CFURLCopyFileSystemPath(revDbURL, kCFURLPOSIXPathStyle);
        CFRelease(revDbURL);
    }
    return revDbPath;
}

static void SecRevocationDbWith(void(^dbJob)(SecRevocationDbRef db)) {
    dispatch_once(&kSecRevocationDbOnce, ^{
        CFStringRef dbPath = SecRevocationDbCopyPath();
        if (dbPath) {
            kSecRevocationDb = SecRevocationDbInit(dbPath);
            CFRelease(dbPath);
        }
    });
    // Do pre job run work here (cancel idle timers etc.)
    if (kSecRevocationDb->updateInProgress) {
        return; // this would block since SecDb has an exclusive transaction lock
    }
    dbJob(kSecRevocationDb);
    // Do post job run work here (gc timer, etc.)
}

static int64_t _SecRevocationDbGetVersion(SecRevocationDbRef rdb, CFErrorRef *error) {
    /* look up version entry in admin table; returns -1 on error */
    __block int64_t version = -1;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        if (ok) ok &= SecDbWithSQL(dbconn, selectVersionSQL, &localError, ^bool(sqlite3_stmt *selectVersion) {
            ok = SecDbStep(dbconn, selectVersion, &localError, NULL);
            version = sqlite3_column_int64(selectVersion, 0);
            return ok;
        });
    });
    (void) CFErrorPropagate(localError, error);
    return version;
}

static void _SecRevocationDbSetVersion(SecRevocationDbRef rdb, CFIndex version){
    secdebug("validupdate", "setting version to %ld", (long)version);

    __block CFErrorRef localError = NULL;
    __block bool ok = true;
    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            if (ok) ok = SecDbWithSQL(dbconn, insertAdminRecordSQL, &localError, ^bool(sqlite3_stmt *insertVersion) {
                if (ok) {
                    const char *versionKey = "version";
                    ok = SecDbBindText(insertVersion, 1, versionKey, strlen(versionKey),
                                       SQLITE_TRANSIENT, &localError);
                }
                if (ok) {
                    ok = SecDbBindInt64(insertVersion, 2,
                                        (sqlite3_int64)version, &localError);
                }
                if (ok) {
                    ok = SecDbStep(dbconn, insertVersion, &localError, NULL);
                }
                return ok;
            });
        });
    });
    if (!ok) {
        secerror("_SecRevocationDbSetVersion failed: %@", localError);
    }
    CFReleaseSafe(localError);
}

static int64_t _SecRevocationDbGetSchemaVersion(SecRevocationDbRef rdb, CFErrorRef *error) {
    /* look up db_version entry in admin table; returns -1 on error */
    __block int64_t db_version = -1;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        if (ok) ok &= SecDbWithSQL(dbconn, selectDbVersionSQL, &localError, ^bool(sqlite3_stmt *selectDbVersion) {
            ok = SecDbStep(dbconn, selectDbVersion, &localError, NULL);
            db_version = sqlite3_column_int64(selectDbVersion, 0);
            return ok;
        });
    });
    (void) CFErrorPropagate(localError, error);
    return db_version;
}

static void _SecRevocationDbSetSchemaVersion(SecRevocationDbRef rdb, CFIndex dbversion) {
    secdebug("validupdate", "setting db_version to %ld", (long)dbversion);

    __block CFErrorRef localError = NULL;
    __block bool ok = true;
    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            if (ok) ok = SecDbWithSQL(dbconn, insertAdminRecordSQL, &localError, ^bool(sqlite3_stmt *insertDbVersion) {
                if (ok) {
                    const char *dbVersionKey = "db_version";
                    ok = SecDbBindText(insertDbVersion, 1, dbVersionKey, strlen(dbVersionKey),
                                       SQLITE_TRANSIENT, &localError);
                }
                if (ok) {
                    ok = SecDbBindInt64(insertDbVersion, 2,
                                        (sqlite3_int64)dbversion, &localError);
                }
                if (ok) {
                    ok = SecDbStep(dbconn, insertDbVersion, &localError, NULL);
                }
                return ok;
            });
        });
    });
    if (!ok) {
        secerror("_SecRevocationDbSetSchemaVersion failed: %@", localError);
    } else {
        rdb->unsupportedVersion = false;
    }
    CFReleaseSafe(localError);
}

static int64_t _SecRevocationDbGetUpdateFormat(SecRevocationDbRef rdb, CFErrorRef *error) {
    /* look up db_format entry in admin table; returns -1 on error */
    __block int64_t db_format = -1;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        if (ok) ok &= SecDbWithSQL(dbconn, selectDbFormatSQL, &localError, ^bool(sqlite3_stmt *selectDbFormat) {
            ok = SecDbStep(dbconn, selectDbFormat, &localError, NULL);
            db_format = sqlite3_column_int64(selectDbFormat, 0);
            return ok;
        });
    });
    (void) CFErrorPropagate(localError, error);
    return db_format;
}

static void _SecRevocationDbSetUpdateFormat(SecRevocationDbRef rdb, CFIndex dbformat) {
    secdebug("validupdate", "setting db_format to %ld", (long)dbformat);

    __block CFErrorRef localError = NULL;
    __block bool ok = true;
    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            if (ok) ok = SecDbWithSQL(dbconn, insertAdminRecordSQL, &localError, ^bool(sqlite3_stmt *insertDbFormat) {
                if (ok) {
                    const char *dbFormatKey = "db_format";
                    ok = SecDbBindText(insertDbFormat, 1, dbFormatKey, strlen(dbFormatKey),
                                       SQLITE_TRANSIENT, &localError);
                }
                if (ok) {
                    ok = SecDbBindInt64(insertDbFormat, 2,
                                        (sqlite3_int64)dbformat, &localError);
                }
                if (ok) {
                    ok = SecDbStep(dbconn, insertDbFormat, &localError, NULL);
                }
                return ok;
            });
        });
    });
    if (!ok) {
        secerror("_SecRevocationDbSetUpdateFormat failed: %@", localError);
    } else {
        rdb->unsupportedVersion = false;
    }
    CFReleaseSafe(localError);
}

static CFStringRef _SecRevocationDbCopyUpdateSource(SecRevocationDbRef rdb, CFErrorRef *error) {
    /* look up db_source entry in admin table; returns NULL on error */
    __block CFStringRef updateSource = NULL;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        if (ok) ok &= SecDbWithSQL(dbconn, selectDbSourceSQL, &localError, ^bool(sqlite3_stmt *selectDbSource) {
            ok = SecDbStep(dbconn, selectDbSource, &localError, NULL);
            const UInt8 *p = (const UInt8 *)sqlite3_column_blob(selectDbSource, 0);
            if (p != NULL) {
                CFIndex length = (CFIndex)sqlite3_column_bytes(selectDbSource, 0);
                if (length > 0) {
                    updateSource = CFStringCreateWithBytes(kCFAllocatorDefault, p, length, kCFStringEncodingUTF8, false);
                }
            }
            return ok;
        });
    });

    (void) CFErrorPropagate(localError, error);
    return updateSource;
}

static void _SecRevocationDbSetUpdateSource(SecRevocationDbRef rdb, CFStringRef updateSource) {
    if (!updateSource) {
        secerror("_SecRevocationDbSetUpdateSource failed: %d", errSecParam);
        return;
    }
    __block char buffer[256];
    __block const char *updateSourceCStr = CFStringGetCStringPtr(updateSource, kCFStringEncodingUTF8);
    if (!updateSourceCStr) {
        if (CFStringGetCString(updateSource, buffer, 256, kCFStringEncodingUTF8)) {
            updateSourceCStr = buffer;
        }
    }
    if (!updateSourceCStr) {
        secerror("_SecRevocationDbSetUpdateSource failed: unable to get UTF-8 encoding");
        return;
    }
    secdebug("validupdate", "setting update source to \"%s\"", updateSourceCStr);

    __block CFErrorRef localError = NULL;
    __block bool ok = true;
    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            if (ok) ok = SecDbWithSQL(dbconn, insertAdminRecordSQL, &localError, ^bool(sqlite3_stmt *insertRecord) {
                if (ok) {
                    const char *dbSourceKey = "db_source";
                    ok = SecDbBindText(insertRecord, 1, dbSourceKey, strlen(dbSourceKey),
                                       SQLITE_TRANSIENT, &localError);
                }
                if (ok) {
                    ok = SecDbBindInt64(insertRecord, 2,
                                        (sqlite3_int64)0, &localError);
                }
                if (ok) {
                    ok = SecDbBindBlob(insertRecord, 3,
                                       updateSourceCStr, strlen(updateSourceCStr),
                                       SQLITE_TRANSIENT, &localError);
                }
                if (ok) {
                    ok = SecDbStep(dbconn, insertRecord, &localError, NULL);
                }
                return ok;
            });
        });
    });
    if (!ok) {
        secerror("_SecRevocationDbSetUpdateSource failed: %@", localError);
    }
    CFReleaseSafe(localError);
}

static CFAbsoluteTime _SecRevocationDbGetNextUpdateTime(SecRevocationDbRef rdb, CFErrorRef *error) {
    /* look up check_again entry in admin table; returns 0 on error */
    __block CFAbsoluteTime nextUpdate = 0;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        if (ok) ok &= SecDbWithSQL(dbconn, selectNextUpdateSQL, &localError, ^bool(sqlite3_stmt *selectNextUpdate) {
            ok = SecDbStep(dbconn, selectNextUpdate, &localError, NULL);
            CFAbsoluteTime *p = (CFAbsoluteTime *)sqlite3_column_blob(selectNextUpdate, 0);
            if (p != NULL) {
                if (sizeof(CFAbsoluteTime) == sqlite3_column_bytes(selectNextUpdate, 0)) {
                    nextUpdate = *p;
                }
            }
            return ok;
        });
    });

    (void) CFErrorPropagate(localError, error);
    return nextUpdate;
}

static void _SecRevocationDbSetNextUpdateTime(SecRevocationDbRef rdb, CFAbsoluteTime nextUpdate){
    secdebug("validupdate", "setting next update to %f", (double)nextUpdate);

    __block CFErrorRef localError = NULL;
    __block bool ok = true;
    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            if (ok) ok = SecDbWithSQL(dbconn, insertAdminRecordSQL, &localError, ^bool(sqlite3_stmt *insertRecord) {
                if (ok) {
                    const char *nextUpdateKey = "check_again";
                    ok = SecDbBindText(insertRecord, 1, nextUpdateKey, strlen(nextUpdateKey),
                                       SQLITE_TRANSIENT, &localError);
                }
                if (ok) {
                    ok = SecDbBindInt64(insertRecord, 2,
                                        (sqlite3_int64)0, &localError);
                }
                if (ok) {
                    ok = SecDbBindBlob(insertRecord, 3,
                                       &nextUpdate, sizeof(CFAbsoluteTime),
                                       SQLITE_TRANSIENT, &localError);
                }
                if (ok) {
                    ok = SecDbStep(dbconn, insertRecord, &localError, NULL);
                }
                return ok;
            });
        });
    });
    if (!ok) {
        secerror("_SecRevocationDbSetNextUpdate failed: %@", localError);
    }
    CFReleaseSafe(localError);
}

static bool _SecRevocationDbRemoveAllEntries(SecRevocationDbRef rdb) {
    /* clear out the contents of the database and start fresh */
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            //ok &= SecDbWithSQL(dbconn, deleteAllEntriesSQL, &localError, ^bool(sqlite3_stmt *deleteAll) {
            //    ok = SecDbStep(dbconn, deleteAll, &localError, NULL);
            //    return ok;
            //});
            /* drop all tables and recreate them, in case of schema changes */
            ok &= SecDbExec(dbconn, deleteTablesSQL, &localError);
            ok &= SecDbExec(dbconn, createTablesSQL, &localError);
            secdebug("validupdate", "resetting database, result: %d", (ok) ? 1 : 0);
            *commit = ok;
        });
        /* compact the db (must be done outside transaction scope) */
        SecDbExec(dbconn, CFSTR("VACUUM"), &localError);
    });
    /* one more thing: update the schema version and format to current */
    _SecRevocationDbSetSchemaVersion(rdb, kSecRevocationDbSchemaVersion);
    _SecRevocationDbSetUpdateFormat(rdb, kSecRevocationDbUpdateFormat);

    CFReleaseSafe(localError);
    return ok;
}

static bool _SecRevocationDbUpdateIssuers(SecRevocationDbRef rdb, int64_t groupId, CFArrayRef issuers, CFErrorRef *error) {
    /* insert or replace issuer records in issuers table */
    if (!issuers || groupId < 0) {
        return false; /* must have something to insert, and a group to associate with it */
    }
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            if (isArray(issuers)) {
                CFIndex issuerIX, issuerCount = CFArrayGetCount(issuers);
                for (issuerIX=0; issuerIX<issuerCount && ok; issuerIX++) {
                    CFDataRef hash = (CFDataRef)CFArrayGetValueAtIndex(issuers, issuerIX);
                    if (!hash) { continue; }
                    if (ok) ok = SecDbWithSQL(dbconn, insertIssuerRecordSQL, &localError, ^bool(sqlite3_stmt *insertIssuer) {
                        if (ok) {
                            ok = SecDbBindInt64(insertIssuer, 1,
                                                groupId, &localError);
                        }
                        if (ok) {
                            ok = SecDbBindBlob(insertIssuer, 2,
                                               CFDataGetBytePtr(hash),
                                               CFDataGetLength(hash),
                                               SQLITE_TRANSIENT, &localError);
                        }
                        /* Execute the insert statement for this issuer record. */
                        if (ok) {
                            ok = SecDbStep(dbconn, insertIssuer, &localError, NULL);
                        }
                        return ok;
                    });
                }
            }
        });
    });

    (void) CFErrorPropagate(localError, error);
    return ok;
}

static bool _SecRevocationDbUpdatePerIssuerData(SecRevocationDbRef rdb, int64_t groupId, CFDictionaryRef dict, CFErrorRef *error) {
    /* update/delete records in serials or hashes table. */
    if (!dict || groupId < 0) {
        return false; /* must have something to insert, and a group to associate with it */
    }
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            CFArrayRef deleteArray = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("delete"));
            /* process deletions */
            if (isArray(deleteArray)) {
               //%%% delete old data here (rdar://31439625)
            }
            /* process additions */
            CFArrayRef addArray = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("add"));
            if (isArray(addArray)) {
                CFIndex identifierIX, identifierCount = CFArrayGetCount(addArray);
                for (identifierIX=0; identifierIX<identifierCount; identifierIX++) {
                    CFDataRef identifierData = (CFDataRef)CFArrayGetValueAtIndex(addArray, identifierIX);
                    if (!identifierData) { continue; }
                    CFIndex length = CFDataGetLength(identifierData);
                    /* we can figure out the format without an extra read to get the format column.
                       len <= 20 is a serial number. len==32 is a sha256 hash. otherwise: xor. */
                    CFStringRef sql = NULL;
                    if (length <= 20) {
                        sql = insertSerialRecordSQL;
                    } else if (length == 32) {
                        sql = insertSha256RecordSQL;
                    }
                    if (!sql) { continue; }

                    if (ok) ok = SecDbWithSQL(dbconn, sql, &localError, ^bool(sqlite3_stmt *insertIdentifier) {
                        /* rowid,(groupid,serial|sha256) */
                        /* rowid is autoincremented and we never set it directly */
                        if (ok) {
                            ok = SecDbBindInt64(insertIdentifier, 1,
                                                groupId, &localError);
                        }
                        if (ok) {
                            ok = SecDbBindBlob(insertIdentifier, 2,
                                               CFDataGetBytePtr(identifierData),
                                               CFDataGetLength(identifierData),
                                               SQLITE_TRANSIENT, &localError);
                        }
                        /* Execute the insert statement for the identifier record. */
                        if (ok) {
                            ok = SecDbStep(dbconn, insertIdentifier, &localError, NULL);
                        }
                        return ok;
                    });
                }
            }
        });
    });

    (void) CFErrorPropagate(localError, error);
    return ok;
}

static SecValidInfoFormat _SecRevocationDbGetGroupFormat(SecRevocationDbRef rdb,
    int64_t groupId, SecValidInfoFlags *flags, CFDataRef *data, CFErrorRef *error) {
    /* return group record fields for a given groupId.
       on success, returns a non-zero format type, and other field values in optional output parameters.
       caller is responsible for releasing data and error parameters, if provided.
    */
    __block bool ok = true;
    __block SecValidInfoFormat format = 0;
    __block CFErrorRef localError = NULL;

    /* Select the group record to determine flags and format. */
    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbWithSQL(dbconn, selectGroupRecordSQL, &localError, ^bool(sqlite3_stmt *selectGroup) {
            ok = SecDbBindInt64(selectGroup, 1, groupId, &localError);
            ok &= SecDbStep(dbconn, selectGroup, &localError, ^(bool *stop) {
                if (flags) {
                    *flags = (SecValidInfoFlags)sqlite3_column_int(selectGroup, 0);
                }
                format = (SecValidInfoFormat)sqlite3_column_int(selectGroup, 1);
                if (data) {
                    //TODO: stream this from sqlite through the inflation so we return an inflated copy, then remove inflate from others
                    uint8_t *p = (uint8_t *)sqlite3_column_blob(selectGroup, 2);
                    if (p != NULL && format == kSecValidInfoFormatNto1) {
                        CFIndex length = (CFIndex)sqlite3_column_bytes(selectGroup, 2);
                        *data = CFDataCreate(kCFAllocatorDefault, p, length);
                    }
                }
            });
            return ok;
        });
    });
    if (!ok) {
        secdebug("validupdate", "GetGroupFormat for groupId %lu failed", (unsigned long)groupId);
        format = kSecValidInfoFormatUnknown;
    }
    (void) CFErrorPropagate(localError, error);
    if (!(format > kSecValidInfoFormatUnknown)) {
        secdebug("validupdate", "GetGroupFormat: got format %d for groupId %lld", format, (long long)groupId);
    }
    return format;
}

static bool _SecRevocationDbUpdateFlags(CFDictionaryRef dict, CFStringRef key, SecValidInfoFlags mask, SecValidInfoFlags *flags) {
    /* If a boolean value exists in the given dictionary for the given key,
       set or clear the corresponding bit(s) defined by the mask argument.
       Function returns true if the flags value was changed, false otherwise.
    */
    bool result = false;
    CFTypeRef value = (CFBooleanRef)CFDictionaryGetValue(dict, key);
    if (isBoolean(value) && flags) {
        SecValidInfoFlags oldFlags = *flags;
        if (CFBooleanGetValue((CFBooleanRef)value)) {
            *flags |= mask;
        } else {
            *flags &= ~(mask);
        }
        result = (*flags != oldFlags);
    }
    return result;
}

static bool _SecRevocationDbUpdateFilter(CFDictionaryRef dict, CFDataRef oldData, CFDataRef * __nonnull CF_RETURNS_RETAINED xmlData) {
    /* If xor and/or params values exist in the given dictionary, create a new
       property list containing the updated values, and return as a flattened
       data blob in the xmlData output parameter (note: caller must release.)
       Function returns true if there is new xmlData to save, false otherwise.
    */
    bool result = false;
    bool xorProvided = false;
    bool paramsProvided = false;
    bool missingData = false;

    if (!dict || !xmlData) {
        return result; /* no-op if no dictionary is provided, or no way to update the data */
    }
    *xmlData = NULL;
    CFDataRef xorCurrent = NULL;
    CFDataRef xorUpdate = (CFDataRef)CFDictionaryGetValue(dict, CFSTR("xor"));
    if (isData(xorUpdate)) {
        xorProvided = true;
    }
    CFArrayRef paramsCurrent = NULL;
    CFArrayRef paramsUpdate = (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("params"));
    if (isArray(paramsUpdate)) {
        paramsProvided = true;
    }
    if (!(xorProvided || paramsProvided)) {
        return result; /* nothing to update, so we can bail out here. */
    }

    CFPropertyListRef nto1Current = NULL;
    CFMutableDictionaryRef nto1Update = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                                                  &kCFTypeDictionaryKeyCallBacks,
                                                                  &kCFTypeDictionaryValueCallBacks);
    if (!nto1Update) {
        return result;
    }

    /* turn old data into property list */
    CFDataRef data = (CFDataRef)CFRetainSafe(oldData);
    CFDataRef inflatedData = copyInflatedData(data);
    if (inflatedData) {
        CFReleaseSafe(data);
        data = inflatedData;
    }
    if (data) {
        nto1Current = CFPropertyListCreateWithData(kCFAllocatorDefault, data, 0, NULL, NULL);
        CFReleaseSafe(data);
    }
    if (nto1Current) {
        xorCurrent = (CFDataRef)CFDictionaryGetValue((CFDictionaryRef)nto1Current, CFSTR("xor"));
        paramsCurrent = (CFArrayRef)CFDictionaryGetValue((CFDictionaryRef)nto1Current, CFSTR("params"));
    }

    /* set current or updated xor data in new property list */
    if (xorProvided) {
        CFDataRef xorNew = NULL;
        if (xorCurrent) {
            CFIndex xorUpdateLen = CFDataGetLength(xorUpdate);
            CFMutableDataRef xor = CFDataCreateMutableCopy(NULL, 0, xorCurrent);
            if (xor && xorUpdateLen > 0) {
                /* truncate or zero-extend data to match update size */
                CFDataSetLength(xor, xorUpdateLen);
                /* exclusive-or update bytes over the existing data */
                UInt8 *xorP = (UInt8 *)CFDataGetMutableBytePtr(xor);
                UInt8 *updP = (UInt8 *)CFDataGetBytePtr(xorUpdate);
                if (xorP && updP) {
                    for (int idx = 0; idx < xorUpdateLen; idx++) {
                        xorP[idx] = xorP[idx] ^ updP[idx];
                    }
                }
            }
            xorNew = (CFDataRef)xor;
        } else {
            xorNew = (CFDataRef)CFRetainSafe(xorUpdate);
        }
        if (xorNew) {
            CFDictionaryAddValue(nto1Update, CFSTR("xor"), xorNew);
            CFReleaseSafe(xorNew);
        } else {
            secdebug("validupdate", "Failed to get updated filter data");
            missingData = true;
        }
    } else if (xorCurrent) {
        /* not provided, so use existing xor value */
        CFDictionaryAddValue(nto1Update, CFSTR("xor"), xorCurrent);
    } else {
        secdebug("validupdate", "Failed to get current filter data");
        missingData = true;
    }

    /* set current or updated params in new property list */
    if (paramsProvided) {
        CFDictionaryAddValue(nto1Update, CFSTR("params"), paramsUpdate);
    } else if (paramsCurrent) {
        /* not provided, so use existing params value */
        CFDictionaryAddValue(nto1Update, CFSTR("params"), paramsCurrent);
    } else {
        /* missing params: neither provided nor existing */
        secdebug("validupdate", "Failed to get current filter params");
        missingData = true;
    }

    CFReleaseSafe(nto1Current);
    if (!missingData) {
        *xmlData = CFPropertyListCreateData(kCFAllocatorDefault, nto1Update,
                                            kCFPropertyListXMLFormat_v1_0,
                                            0, NULL);
        result = (*xmlData != NULL);
    }
    CFReleaseSafe(nto1Update);

    /* compress the xmlData blob, if possible */
    if (result) {
        CFDataRef deflatedData = copyDeflatedData(*xmlData);
        if (deflatedData) {
            if (CFDataGetLength(deflatedData) < CFDataGetLength(*xmlData)) {
                CFRelease(*xmlData);
                *xmlData = deflatedData;
            } else {
                CFRelease(deflatedData);
            }
        }
    }
    return result;
}


static int64_t _SecRevocationDbUpdateGroup(SecRevocationDbRef rdb, int64_t groupId, CFDictionaryRef dict, CFErrorRef *error) {
    /* insert group record for a given groupId.
       if the specified groupId is < 0, a new group entry is created.
       returns the groupId on success, or -1 on failure.
     */
    if (!dict) {
        return groupId; /* no-op if no dictionary is provided */
    }

    __block int64_t result = -1;
    __block bool ok = true;
    __block bool isFormatChange = false;
    __block CFErrorRef localError = NULL;

    __block SecValidInfoFlags flags = 0;
    __block SecValidInfoFormat format = kSecValidInfoFormatUnknown;
    __block SecValidInfoFormat formatUpdate = kSecValidInfoFormatUnknown;
    __block CFDataRef data = NULL;

    if (groupId >= 0) {
        /* fetch the flags and data for an existing group record, in case some are being changed. */
        format = _SecRevocationDbGetGroupFormat(rdb, groupId, &flags, &data, NULL);
        if (format == kSecValidInfoFormatUnknown) {
            secdebug("validupdate", "existing group %lld has unknown format %d, flags=%lu",
                     (long long)groupId, format, flags);
            //%%% clean up by deleting all issuers with this groupId, then the group record,
            // or just force a full update? note: we can get here if we fail to bind the
            // format value in the prepared SQL statement below.
            return -1;
        }
    }
    CFTypeRef value = (CFStringRef)CFDictionaryGetValue(dict, CFSTR("format"));
    if (isString(value)) {
        if (CFStringCompare((CFStringRef)value, CFSTR("serial"), 0) == kCFCompareEqualTo) {
            formatUpdate = kSecValidInfoFormatSerial;
        } else if (CFStringCompare((CFStringRef)value, CFSTR("sha256"), 0) == kCFCompareEqualTo) {
            formatUpdate = kSecValidInfoFormatSHA256;
        } else if (CFStringCompare((CFStringRef)value, CFSTR("nto1"), 0) == kCFCompareEqualTo) {
            formatUpdate = kSecValidInfoFormatNto1;
        }
    }
    /* if format value is explicitly supplied, then this is effectively a new group entry. */
    isFormatChange = (formatUpdate > kSecValidInfoFormatUnknown &&
                      formatUpdate != format &&
                      groupId >= 0);

    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            if (isFormatChange) {
                secdebug("validupdate", "group %lld format change from %d to %d",
                         (long long)groupId, format, formatUpdate);
                /* format of an existing group is changing; delete the group first.
                   this should ensure that all entries referencing the old groupid are deleted.
                */
                ok &= SecDbWithSQL(dbconn, deleteGroupRecordSQL, &localError, ^bool(sqlite3_stmt *deleteResponse) {
                    ok = SecDbBindInt64(deleteResponse, 1, groupId, &localError);
                    /* Execute the delete statement. */
                    if (ok) {
                        ok = SecDbStep(dbconn, deleteResponse, &localError, NULL);
                    }
                    return ok;
                });
            }
            ok &= SecDbWithSQL(dbconn, insertGroupRecordSQL, &localError, ^bool(sqlite3_stmt *insertGroup) {
                /* (groupid,flags,format,data) */
                /* groups.groupid */
                if (ok && (!isFormatChange) && (groupId >= 0)) {
                    /* bind to existing groupId row if known, otherwise will insert and autoincrement */
                    ok = SecDbBindInt64(insertGroup, 1, groupId, &localError);
                    if (!ok) {
                        secdebug("validupdate", "failed to set groupId %lld", (long long)groupId);
                    }
                }
                /* groups.flags */
                if (ok) {
                    (void)_SecRevocationDbUpdateFlags(dict, CFSTR("complete"), kSecValidInfoComplete, &flags);
                    (void)_SecRevocationDbUpdateFlags(dict, CFSTR("check-ocsp"), kSecValidInfoCheckOCSP, &flags);
                    (void)_SecRevocationDbUpdateFlags(dict, CFSTR("known-intermediates-only"), kSecValidInfoKnownOnly, &flags);
                    (void)_SecRevocationDbUpdateFlags(dict, CFSTR("require-ct"), kSecValidInfoRequireCT, &flags);
                    (void)_SecRevocationDbUpdateFlags(dict, CFSTR("valid"), kSecValidInfoAllowlist, &flags);
                    (void)_SecRevocationDbUpdateFlags(dict, CFSTR("no-ca"), kSecValidInfoNoCACheck, &flags);

                    ok = SecDbBindInt(insertGroup, 2, (int)flags, &localError);
                    if (!ok) {
                        secdebug("validupdate", "failed to set flags (%lu) for groupId %lld", flags, (long long)groupId);
                    }
                }
                /* groups.format */
                if (ok) {
                    SecValidInfoFormat formatValue = format;
                    if (formatUpdate > kSecValidInfoFormatUnknown) {
                        formatValue = formatUpdate;
                    }
                    ok = SecDbBindInt(insertGroup, 3, (int)formatValue, &localError);
                    if (!ok) {
                        secdebug("validupdate", "failed to set format (%d) for groupId %lld", formatValue, (long long)groupId);
                    }
                }
                /* groups.data */
                CFDataRef xmlData = NULL;
                if (ok) {
                    bool hasFilter = ((formatUpdate == kSecValidInfoFormatNto1) ||
                                      (formatUpdate == kSecValidInfoFormatUnknown &&
                                       format == kSecValidInfoFormatNto1));
                    if (hasFilter) {
                        CFDataRef dataValue = data; /* use existing data */
                        if (_SecRevocationDbUpdateFilter(dict, data, &xmlData)) {
                            dataValue = xmlData; /* use updated data */
                        }
                        if (dataValue) {
                            ok = SecDbBindBlob(insertGroup, 4,
                                               CFDataGetBytePtr(dataValue),
                                               CFDataGetLength(dataValue),
                                               SQLITE_TRANSIENT, &localError);
                        }
                        if (!ok) {
                            secdebug("validupdate", "failed to set data for groupId %lld",
                                     (long long)groupId);
                        }
                    }
                    /* else there is no data, so NULL is implicitly bound to column 4 */
                }

                /* Execute the insert statement for the group record. */
                if (ok) {
                    ok = SecDbStep(dbconn, insertGroup, &localError, NULL);
                    if (!ok) {
                        secdebug("validupdate", "failed to execute insertGroup statement for groupId %lld",
                                 (long long)groupId);
                    }
                    result = (int64_t)sqlite3_last_insert_rowid(SecDbHandle(dbconn));
                }
                if (!ok) {
                    secdebug("validupdate", "failed to insert group %lld", (long long)result);
                }
                /* Clean up temporary allocation made in this block. */
                CFReleaseSafe(xmlData);
                CFReleaseSafe(data);
                return ok;
            });
        });
    });

    (void) CFErrorPropagate(localError, error);
    return result;
}

static int64_t _SecRevocationDbGroupIdForIssuerHash(SecRevocationDbRef rdb, CFDataRef hash, CFErrorRef *error) {
    /* look up issuer hash in issuers table to get groupid, if it exists */
    __block int64_t groupId = -1;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    if (!hash) {
        secdebug("validupdate", "failed to get hash (%@)", hash);
    }
    require(hash, errOut);

    /* This is the starting point for any lookup; find a group id for the given issuer hash.
       Before we do that, need to verify the current db_version. We cannot use results from a
       database created with a schema version older than the minimum supported version.
       However, we may be able to use results from a newer version. At the next database
       update interval, if the existing schema is old, we'll be removing and recreating
       the database contents with the current schema version.
    */
    int64_t db_version = _SecRevocationDbGetSchemaVersion(rdb, NULL);
    if (db_version < kSecRevocationDbMinSchemaVersion) {
        if (!rdb->unsupportedVersion) {
            secdebug("validupdate", "unsupported db_version: %lld", (long long)db_version);
            rdb->unsupportedVersion = true; /* only warn once for a given unsupported version */
        }
    }
    require_quiet(db_version >= kSecRevocationDbMinSchemaVersion, errOut);

    /* Look up provided issuer_hash in the issuers table.
    */
    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbWithSQL(dbconn, selectGroupIdSQL, &localError, ^bool(sqlite3_stmt *selectGroupId) {
            ok = SecDbBindBlob(selectGroupId, 1, CFDataGetBytePtr(hash), CFDataGetLength(hash), SQLITE_TRANSIENT, &localError);
            ok &= SecDbStep(dbconn, selectGroupId, &localError, ^(bool *stopGroupId) {
                groupId = sqlite3_column_int64(selectGroupId, 0);
            });
            return ok;
        });
    });

errOut:
    (void) CFErrorPropagate(localError, error);
    return groupId;
}

static bool _SecRevocationDbApplyGroupDelete(SecRevocationDbRef rdb, CFDataRef issuerHash, CFErrorRef *error) {
    /* delete group associated with the given issuer;
       schema trigger will delete associated issuers, serials, and hashes. */
    __block int64_t groupId = -1;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;

    groupId = _SecRevocationDbGroupIdForIssuerHash(rdb, issuerHash, &localError);
    require(!(groupId < 0), errOut);

    ok &= SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbTransaction(dbconn, kSecDbExclusiveTransactionType, &localError, ^(bool *commit) {
            ok = SecDbWithSQL(dbconn, deleteGroupRecordSQL, &localError, ^bool(sqlite3_stmt *deleteResponse) {
                ok = SecDbBindInt64(deleteResponse, 1, groupId, &localError);
                /* Execute the delete statement. */
                if (ok) {
                    ok = SecDbStep(dbconn, deleteResponse, &localError, NULL);
                }
                return ok;
            });
        });
    });

errOut:
    (void) CFErrorPropagate(localError, error);
    return (groupId < 0) ? false : true;
}

static bool _SecRevocationDbApplyGroupUpdate(SecRevocationDbRef rdb, CFDictionaryRef dict, CFErrorRef *error) {
    /* process one issuer group's update dictionary */
    int64_t groupId = -1;
    CFErrorRef localError = NULL;

    CFArrayRef issuers = (dict) ? (CFArrayRef)CFDictionaryGetValue(dict, CFSTR("issuer-hash")) : NULL;
    if (isArray(issuers)) {
        CFIndex issuerIX, issuerCount = CFArrayGetCount(issuers);
        /* while we have issuers and haven't found a matching group id */
        for (issuerIX=0; issuerIX<issuerCount && groupId < 0; issuerIX++) {
            CFDataRef hash = (CFDataRef)CFArrayGetValueAtIndex(issuers, issuerIX);
            if (!hash) { continue; }
            groupId = _SecRevocationDbGroupIdForIssuerHash(rdb, hash, &localError);
        }
    }
    /* create or update the group entry */
    groupId = _SecRevocationDbUpdateGroup(rdb, groupId, dict, &localError);
    if (groupId < 0) {
        secdebug("validupdate", "failed to get groupId");
    } else {
        /* create or update issuer entries, now that we know the group id */
        _SecRevocationDbUpdateIssuers(rdb, groupId, issuers, &localError);
        /* create or update entries in serials or hashes tables */
        _SecRevocationDbUpdatePerIssuerData(rdb, groupId, dict, &localError);
    }

    (void) CFErrorPropagate(localError, error);
    return (groupId > 0) ? true : false;
}

static void _SecRevocationDbApplyUpdate(SecRevocationDbRef rdb, CFDictionaryRef update, CFIndex version) {
    /* process entire update dictionary */
    if (!rdb || !update) {
        secerror("_SecRevocationDbApplyUpdate failed: invalid args");
        return;
    }

    __block CFDictionaryRef localUpdate = (CFDictionaryRef)CFRetainSafe(update);
    __block CFErrorRef localError = NULL;

    CFTypeRef value = NULL;
    CFIndex deleteCount = 0;
    CFIndex updateCount = 0;

    rdb->updateInProgress = true;

    /* check whether this is a full update */
    value = (CFBooleanRef)CFDictionaryGetValue(update, CFSTR("full"));
    if (isBoolean(value) && CFBooleanGetValue((CFBooleanRef)value)) {
        /* clear the database before processing a full update */
        SecRevocationDbRemoveAllEntries();
    }

    /* process 'delete' list */
    value = (CFArrayRef)CFDictionaryGetValue(localUpdate, CFSTR("delete"));
    if (isArray(value)) {
        deleteCount = CFArrayGetCount((CFArrayRef)value);
        secdebug("validupdate", "processing %ld deletes", (long)deleteCount);
        for (CFIndex deleteIX=0; deleteIX<deleteCount; deleteIX++) {
            CFDataRef issuerHash = (CFDataRef)CFArrayGetValueAtIndex((CFArrayRef)value, deleteIX);
            if (isData(issuerHash)) {
                (void)_SecRevocationDbApplyGroupDelete(rdb, issuerHash, &localError);
                CFReleaseNull(localError);
            }
        }
    }

    /* process 'update' list */
    value = (CFArrayRef)CFDictionaryGetValue(localUpdate, CFSTR("update"));
    if (isArray(value)) {
        updateCount = CFArrayGetCount((CFArrayRef)value);
        secdebug("validupdate", "processing %ld updates", (long)updateCount);
        for (CFIndex updateIX=0; updateIX<updateCount; updateIX++) {
            CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex((CFArrayRef)value, updateIX);
            if (isDictionary(dict)) {
                (void)_SecRevocationDbApplyGroupUpdate(rdb, dict, &localError);
                CFReleaseNull(localError);
            }
        }
    }
    CFRelease(localUpdate);

    /* set version */
    _SecRevocationDbSetVersion(rdb, version);

    /* set db_version if not already set */
    int64_t db_version = _SecRevocationDbGetSchemaVersion(rdb, NULL);
    if (db_version <= 0) {
        _SecRevocationDbSetSchemaVersion(rdb, kSecRevocationDbSchemaVersion);
    }

    /* set db_format if not already set */
    int64_t db_format = _SecRevocationDbGetUpdateFormat(rdb, NULL);
    if (db_format <= 0) {
        _SecRevocationDbSetUpdateFormat(rdb, kSecRevocationDbUpdateFormat);
    }

    /* compact the db (must be done outside transaction scope) */
    (void)SecDbPerformWrite(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        SecDbExec(dbconn, CFSTR("VACUUM"), &localError);
        CFReleaseNull(localError);
    });

    rdb->updateInProgress = false;
}

static bool _SecRevocationDbSerialInGroup(SecRevocationDbRef rdb,
                                          CFDataRef serial,
                                          int64_t groupId,
                                          CFErrorRef *error) {
    __block bool result = false;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;
    require(rdb && serial, errOut);
    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbWithSQL(dbconn, selectSerialRecordSQL, &localError, ^bool(sqlite3_stmt *selectSerial) {
            ok &= SecDbBindInt64(selectSerial, 1, groupId, &localError);
            ok &= SecDbBindBlob(selectSerial, 2, CFDataGetBytePtr(serial),
                                CFDataGetLength(serial), SQLITE_TRANSIENT, &localError);
            ok &= SecDbStep(dbconn, selectSerial, &localError, ^(bool *stop) {
                int64_t foundRowId = (int64_t)sqlite3_column_int64(selectSerial, 0);
                result = (foundRowId > 0);
            });
            return ok;
        });
    });

errOut:
    (void) CFErrorPropagate(localError, error);
    return result;
}

static bool _SecRevocationDbCertHashInGroup(SecRevocationDbRef rdb,
                                            CFDataRef certHash,
                                            int64_t groupId,
                                            CFErrorRef *error) {
    __block bool result = false;
    __block bool ok = true;
    __block CFErrorRef localError = NULL;
    require(rdb && certHash, errOut);
    ok &= SecDbPerformRead(rdb->db, &localError, ^(SecDbConnectionRef dbconn) {
        ok &= SecDbWithSQL(dbconn, selectHashRecordSQL, &localError, ^bool(sqlite3_stmt *selectHash) {
            ok &= SecDbBindInt64(selectHash, 1, groupId, &localError);
            ok = SecDbBindBlob(selectHash, 2, CFDataGetBytePtr(certHash),
                               CFDataGetLength(certHash), SQLITE_TRANSIENT, &localError);
            ok &= SecDbStep(dbconn, selectHash, &localError, ^(bool *stop) {
                int64_t foundRowId = (int64_t)sqlite3_column_int64(selectHash, 0);
                result = (foundRowId > 0);
            });
            return ok;
        });
    });

errOut:
    (void) CFErrorPropagate(localError, error);
    return result;
}

static bool _SecRevocationDbSerialInFilter(SecRevocationDbRef rdb,
                                           CFDataRef serialData,
                                           CFDataRef xmlData) {
    /* N-To-1 filter implementation.
       The 'xmlData' parameter is a flattened XML dictionary,
       containing 'xor' and 'params' keys. First order of
       business is to reconstitute the blob into components.
    */
    bool result = false;
    CFRetainSafe(xmlData);
    CFDataRef propListData = xmlData;
    /* Expand data blob if needed */
    CFDataRef inflatedData = copyInflatedData(propListData);
    if (inflatedData) {
        CFReleaseSafe(propListData);
        propListData = inflatedData;
    }
    CFDataRef xor = NULL;
    CFArrayRef params = NULL;
    CFPropertyListRef nto1 = CFPropertyListCreateWithData(kCFAllocatorDefault, propListData, 0, NULL, NULL);
    if (nto1) {
        xor = (CFDataRef)CFDictionaryGetValue((CFDictionaryRef)nto1, CFSTR("xor"));
        params = (CFArrayRef)CFDictionaryGetValue((CFDictionaryRef)nto1, CFSTR("params"));
    }
    uint8_t *hash = (xor) ? (uint8_t*)CFDataGetBytePtr(xor) : NULL;
    CFIndex hashLen = (hash) ? CFDataGetLength(xor) : 0;
    uint8_t *serial = (serialData) ? (uint8_t*)CFDataGetBytePtr(serialData) : NULL;
    CFIndex serialLen = (serial) ? CFDataGetLength(serialData) : 0;

    require(hash && serial && params, errOut);

    const uint32_t FNV_OFFSET_BASIS = 2166136261;
    const uint32_t FNV_PRIME = 16777619;
    bool notInHash = false;
    CFIndex ix, count = CFArrayGetCount(params);
    for (ix = 0; ix < count; ix++) {
        int32_t param;
        CFNumberRef cfnum = (CFNumberRef)CFArrayGetValueAtIndex(params, ix);
        if (!isNumber(cfnum) ||
            !CFNumberGetValue(cfnum, kCFNumberSInt32Type, &param)) {
            secinfo("validupdate", "error processing filter params at index %ld", (long)ix);
            continue;
        }
        /* process one param */
        uint32_t hval = FNV_OFFSET_BASIS ^ param;
        CFIndex i = serialLen;
        while (i > 0) {
            hval = ((hval ^ (serial[--i])) * FNV_PRIME) & 0xFFFFFFFF;
        }
        hval = hval % (hashLen * 8);
        if ((hash[hval/8] & (1 << (hval % 8))) == 0) {
            notInHash = true; /* definitely not in hash */
            break;
        }
    }
    if (!notInHash) {
        /* probabilistically might be in hash if we get here. */
        result = true;
    }

errOut:
    CFReleaseSafe(nto1);
    CFReleaseSafe(propListData);
    return result;
}

static SecValidInfoRef _SecRevocationDbValidInfoForCertificate(SecRevocationDbRef rdb,
                                                               SecCertificateRef certificate,
                                                               CFDataRef issuerHash,
                                                               CFErrorRef *error) {
    __block CFErrorRef localError = NULL;
    __block SecValidInfoFlags flags = 0;
    __block SecValidInfoFormat format = kSecValidInfoFormatUnknown;
    __block CFDataRef data = NULL;

    bool matched = false;
    bool isOnList = false;
    int64_t groupId = 0;
    CFDataRef serial = NULL;
    CFDataRef certHash = NULL;
    SecValidInfoRef result = NULL;

    require((serial = SecCertificateCopySerialNumberData(certificate, NULL)) != NULL, errOut);
    require((certHash = SecCertificateCopySHA256Digest(certificate)) != NULL, errOut);
    require((groupId = _SecRevocationDbGroupIdForIssuerHash(rdb, issuerHash, &localError)) > 0, errOut);

    /* Look up the group record to determine flags and format. */
    format = _SecRevocationDbGetGroupFormat(rdb, groupId, &flags, &data, &localError);

    if (format == kSecValidInfoFormatUnknown) {
        /* No group record found for this issuer. */
    }
    else if (format == kSecValidInfoFormatSerial) {
        /* Look up certificate's serial number in the serials table. */
        matched = _SecRevocationDbSerialInGroup(rdb, serial, groupId, &localError);
    }
    else if (format == kSecValidInfoFormatSHA256) {
        /* Look up certificate's SHA-256 hash in the hashes table. */
        matched = _SecRevocationDbCertHashInGroup(rdb, certHash, groupId, &localError);
    }
    else if (format == kSecValidInfoFormatNto1) {
        /* Perform a Bloom filter match against the serial. If matched is false,
           then the cert is definitely not in the list. But if matched is true,
           we don't know for certain, so we would need to check OCSP. */
        matched = _SecRevocationDbSerialInFilter(rdb, serial, data);
    }

    if (matched) {
        /* Found a specific match for this certificate. */
        secdebug("validupdate", "Valid db matched certificate: %@, format=%d, flags=%lu",
                 certHash, format, flags);
        isOnList = true;
    }
    else if ((flags & kSecValidInfoComplete) && (flags & kSecValidInfoAllowlist)) {
        /* Not matching against a complete allowlist is equivalent to revocation. */
        secdebug("validupdate", "Valid db did NOT match certificate on allowlist: %@, format=%d, flags=%lu",
                 certHash, format, flags);
        matched = true;
    }
    else if ((!(flags & kSecValidInfoComplete)) && (format > kSecValidInfoFormatUnknown)) {
        /* Not matching against an incomplete list implies we need to check OCSP. */
        secdebug("validupdate", "Valid db did not find certificate on incomplete list: %@, format=%d, flags=%lu",
                 certHash, format, flags);
        matched = true;
    }

    if (matched) {
        /* Return SecValidInfo for a matched certificate. */
        result = SecValidInfoCreate(format, flags, isOnList, certHash, issuerHash, NULL);
    }

    if (result && SecIsAppleTrustAnchor(certificate, 0)) {
        /* Prevent a catch-22. */
        secdebug("validupdate", "Valid db match for Apple trust anchor: %@, format=%d, flags=%lu",
                 certHash, format, flags);
        SecValidInfoRelease(result);
        result = NULL;
    }

errOut:
    (void) CFErrorPropagate(localError, error);
    CFReleaseSafe(data);
    CFReleaseSafe(certHash);
    CFReleaseSafe(serial);
    return result;
}

static SecValidInfoRef _SecRevocationDbCopyMatching(SecRevocationDbRef db,
                                                    SecCertificateRef certificate,
                                                    SecCertificateRef issuer) {
    SecValidInfoRef result = NULL;
    CFErrorRef error = NULL;
    CFDataRef issuerHash = NULL;

    require(certificate && issuer, errOut);
    require(issuerHash = SecCertificateCopySHA256Digest(issuer), errOut);

    result = _SecRevocationDbValidInfoForCertificate(db, certificate, issuerHash, &error);

errOut:
    CFReleaseSafe(issuerHash);
    CFReleaseSafe(error);
    return result;
}

static dispatch_queue_t _SecRevocationDbGetUpdateQueue(SecRevocationDbRef rdb) {
    return (rdb) ? rdb->update_queue : NULL;
}


/* Given a valid update dictionary, insert/replace or delete records
   in the revocation database. (This function is expected to be called only
   by the database maintainer, normally the system instance of trustd.)
*/
void SecRevocationDbApplyUpdate(CFDictionaryRef update, CFIndex version) {
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        _SecRevocationDbApplyUpdate(db, update, version);
    });
}

/* Set the schema version for the revocation database.
   (This function is expected to be called only by the database maintainer,
   normally the system instance of trustd.)
*/
void SecRevocationDbSetSchemaVersion(CFIndex db_version) {
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        _SecRevocationDbSetSchemaVersion(db, db_version);
    });
}

/* Set the update format for the revocation database.
   (This function is expected to be called only by the database maintainer,
   normally the system instance of trustd.)
*/
void SecRevocationDbSetUpdateFormat(CFIndex db_format) {
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        _SecRevocationDbSetUpdateFormat(db, db_format);
    });
}

/* Set the update source for the revocation database.
   (This function is expected to be called only by the database
   maintainer, normally the system instance of trustd. If the
   caller does not have write access, this is a no-op.)
*/
void SecRevocationDbSetUpdateSource(CFStringRef updateSource) {
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        _SecRevocationDbSetUpdateSource(db, updateSource);
    });
}

/* Return the update source as a retained CFStringRef.
   If the value cannot be obtained, NULL is returned.
*/
CFStringRef SecRevocationDbCopyUpdateSource(void) {
    __block CFStringRef result = NULL;
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        result = _SecRevocationDbCopyUpdateSource(db, NULL);
    });
    return result;
}

/* Set the next update value for the revocation database.
   (This function is expected to be called only by the database
   maintainer, normally the system instance of trustd. If the
   caller does not have write access, this is a no-op.)
*/
void SecRevocationDbSetNextUpdateTime(CFAbsoluteTime nextUpdate) {
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        _SecRevocationDbSetNextUpdateTime(db, nextUpdate);
    });
}

/* Return the next update value as a CFAbsoluteTime.
   If the value cannot be obtained, -1 is returned.
*/
CFAbsoluteTime SecRevocationDbGetNextUpdateTime(void) {
    __block CFAbsoluteTime result = -1;
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        result = _SecRevocationDbGetNextUpdateTime(db, NULL);
    });
    return result;
}

/* Return the serial background queue for database updates.
   If the queue cannot be obtained, NULL is returned.
*/
dispatch_queue_t SecRevocationDbGetUpdateQueue(void) {
    __block dispatch_queue_t result = NULL;
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        result = _SecRevocationDbGetUpdateQueue(db);
    });
    return result;
}

/* Remove all entries in the revocation database and reset its version to 0.
   (This function is expected to be called only by the database maintainer,
   normally the system instance of trustd.)
*/
void SecRevocationDbRemoveAllEntries(void) {
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        _SecRevocationDbRemoveAllEntries(db);
    });
}

/* Release all connections to the revocation database.
*/
void SecRevocationDbReleaseAllConnections(void) {
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        SecDbReleaseAllConnections((db) ? db->db : NULL);
    });
}

/* === SecRevocationDb API === */

/* Given a certificate and its issuer, returns a SecValidInfoRef if the
   valid database contains matching info; otherwise returns NULL.
   Caller must release the returned SecValidInfoRef by calling
   SecValidInfoRelease when finished.
*/
SecValidInfoRef SecRevocationDbCopyMatching(SecCertificateRef certificate,
                                            SecCertificateRef issuer) {
    __block SecValidInfoRef result = NULL;
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        result = _SecRevocationDbCopyMatching(db, certificate, issuer);
    });
    return result;
}

/* Return the current version of the revocation database.
   A version of 0 indicates an empty database which must be populated.
   If the version cannot be obtained, -1 is returned.
*/
CFIndex SecRevocationDbGetVersion(void) {
    __block CFIndex result = -1;
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        result = (CFIndex)_SecRevocationDbGetVersion(db, NULL);
    });
    return result;
}

/* Return the current schema version of the revocation database.
   A version of 0 indicates an empty database which must be populated.
   If the schema version cannot be obtained, -1 is returned.
*/
CFIndex SecRevocationDbGetSchemaVersion(void) {
    __block CFIndex result = -1;
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        result = (CFIndex)_SecRevocationDbGetSchemaVersion(db, NULL);
    });
    return result;
}

/* Return the current update format of the revocation database.
   A version of 0 indicates the format was unknown.
   If the update format cannot be obtained, -1 is returned.
*/
CFIndex SecRevocationDbGetUpdateFormat(void) {
    __block CFIndex result = -1;
    SecRevocationDbWith(^(SecRevocationDbRef db) {
        result = (CFIndex)_SecRevocationDbGetUpdateFormat(db, NULL);
    });
    return result;
}
