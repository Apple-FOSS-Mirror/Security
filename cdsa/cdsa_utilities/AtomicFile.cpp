/*
 * Copyright (c) 2000-2001, 2003 Apple Computer, Inc. All Rights Reserved.
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


#include <Security/AtomicFile.h>

#include <Security/devrandom.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <syslog.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>


#define kAtomicFileMaxBlockSize INT_MAX


//
//  AtomicFile.cpp - Description t.b.d.
//
AtomicFile::AtomicFile(const std::string &inPath) :
	mPath(inPath)
{
	pathSplit(inPath, mDir, mFile);
}

AtomicFile::~AtomicFile()
{
}

// Aquire the write lock and remove the file.
void
AtomicFile::performDelete()
{
	AtomicLockedFile lock(*this);
	if (::unlink(mPath.c_str()) != 0)
	{
		int error = errno;
		secdebug("atomicfile", "unlink %s: %s", mPath.c_str(), strerror(error));
        if (error == ENOENT)
			CssmError::throwMe(CSSMERR_DL_DATASTORE_DOESNOT_EXIST);
		else
			UnixError::throwMe(error);
	}
}

// Aquire the write lock and rename the file (and bump the version and stuff).
void
AtomicFile::rename(const std::string &inNewPath)
{
	const char *path = mPath.c_str();
	const char *newPath = inNewPath.c_str();

	// @@@ lock the destination file too.
	AtomicLockedFile lock(*this);
	if (::rename(path, newPath) != 0)
	{
		int error = errno;
		secdebug("atomicfile", "rename(%s, %s): %s", path, newPath, strerror(error));
		UnixError::throwMe(error);
	}
}

// Lock the file for writing and return a newly created AtomicTempFile.
RefPointer<AtomicTempFile>
AtomicFile::create(mode_t mode)
{
	const char *path = mPath.c_str();

	// First make sure the directory to this file exists and is writable
	mkpath(mDir);

	RefPointer<AtomicLockedFile> lock(new AtomicLockedFile(*this));
	int fileRef = ropen(path, O_WRONLY|O_CREAT|O_EXCL, mode);
    if (fileRef == -1)
    {
        int error = errno;
		secdebug("atomicfile", "open %s: %s", path, strerror(error));

        // Do the obvious error code translations here.
		// @@@ Consider moving these up a level.
        if (error == EACCES)
			CssmError::throwMe(CSSM_ERRCODE_OS_ACCESS_DENIED);
        else if (error == EEXIST)
			CssmError::throwMe(CSSMERR_DL_DATASTORE_ALREADY_EXISTS);
		else
			UnixError::throwMe(error);
    }
	rclose(fileRef);

	try
	{
		// Now that we have created the lock and the new db file create a tempfile
		// object.
		RefPointer<AtomicTempFile> temp(new AtomicTempFile(*this, lock, mode));
		secdebug("atomicfile", "%p created %s", this, path);
		return temp;
	}
	catch (...)
	{
		// Creating the temp file failed so remove the db file we just created too.
		if (::unlink(path) == -1)
		{
			secdebug("atomicfile", "unlink %s: %s", path, strerror(errno));
		}
		throw;
	}
}

// Lock the database file for writing and return a newly created AtomicTempFile.
RefPointer<AtomicTempFile>
AtomicFile::write()
{
	RefPointer<AtomicLockedFile> lock(new AtomicLockedFile(*this));
	return new AtomicTempFile(*this, lock);
}

// Return a bufferedFile containing current version of the file for reading.
RefPointer<AtomicBufferedFile>
AtomicFile::read()
{
	return new AtomicBufferedFile(mPath);
}

mode_t
AtomicFile::mode() const
{
	const char *path = mPath.c_str();
	struct stat st;
	if (::stat(path, &st) == -1)
	{
		int error = errno;
		secdebug("atomicfile", "stat %s: %s", path, strerror(error));
		UnixError::throwMe(error);
	}
	return st.st_mode;
}

// Split full into a dir and file component.
void
AtomicFile::pathSplit(const std::string &inFull, std::string &outDir, std::string &outFile)
{
	std::string::size_type slash, len = inFull.size();
	slash = inFull.rfind('/');
	if (slash == std::string::npos)
	{
		outDir = "";
		outFile = inFull;
	}
	else if (slash + 1 == len)
	{
		outDir = inFull;
		outFile = "";
	}
	else
	{
		outDir = inFull.substr(0, slash + 1);
		outFile = inFull.substr(slash + 1, len);
	}
}

//
// Make sure the directory up to inDir exists inDir *must* end in a slash.
//
void
AtomicFile::mkpath(const std::string &inDir, mode_t mode)
{
	for (std::string::size_type pos = 0; (pos = inDir.find('/', pos + 1)) != std::string::npos;)
	{
		std::string path = inDir.substr(0, pos);
		const char *cpath = path.c_str();
		struct stat sb;
		if (::stat(cpath, &sb))
		{
			if (errno != ENOENT || ::mkdir(cpath, mode))
				UnixError::throwMe(errno);
		}
		else if (!S_ISDIR(sb.st_mode))
			CssmError::throwMe(CSSM_ERRCODE_OS_ACCESS_DENIED);  // @@@ Should be is a directory
	}
}

int
AtomicFile::ropen(const char *const name, int flags, mode_t mode)
{
	int fd, tries_left = 4 /* kNoResRetry */;
	do
	{
		fd = ::open(name, flags, mode);
	} while (fd < 0 && (errno == EINTR || errno == ENFILE && --tries_left >= 0));

	return fd;
}

int
AtomicFile::rclose(int fd)
{
	int result;
	do
	{
		result = ::close(fd);
	} while(result && errno == EINTR);

	return result;
}

//
// AtomicBufferedFile - This represents an instance of a file opened for reading.
// The file is read into memory and closed after this is done.
// The memory is released when this object is destroyed.
//
AtomicBufferedFile::AtomicBufferedFile(const std::string &inPath) :
	mPath(inPath),
	mFileRef(-1),
	mBuffer(NULL),
	mLength(0)
{
}

AtomicBufferedFile::~AtomicBufferedFile()
{
	if (mFileRef >= 0)
	{
		AtomicFile::rclose(mFileRef);
		secdebug("atomicfile", "%p closed %s", this, mPath.c_str());
	}

	if (mBuffer)
	{
		secdebug("atomicfile", "%p free %s buffer %p", this, mPath.c_str(), mBuffer);
		free(mBuffer);
	}
}

//
// Open the file and return the length in bytes.
//
off_t
AtomicBufferedFile::open()
{
	const char *path = mPath.c_str();
	if (mFileRef >= 0)
	{
		secdebug("atomicfile", "open %s: already open, closing and reopening", path);
		close();
	}

	mFileRef = AtomicFile::ropen(path, O_RDONLY, 0);
    if (mFileRef == -1)
    {
        int error = errno;
		secdebug("atomicfile", "open %s: %s", path, strerror(error));

        // Do the obvious error code translations here.
		// @@@ Consider moving these up a level.
        if (error == ENOENT)
			CssmError::throwMe(CSSMERR_DL_DATASTORE_DOESNOT_EXIST);
		else if (error == EACCES)
			CssmError::throwMe(CSSM_ERRCODE_OS_ACCESS_DENIED);
		else
			UnixError::throwMe(error);
    }

	mLength = ::lseek(mFileRef, 0, SEEK_END);
	if (mLength == -1)
	{
		int error = errno;
		secdebug("atomicfile", "lseek(%s, END): %s", path, strerror(error));
		AtomicFile::rclose(mFileRef);
		UnixError::throwMe(error);
	}

	secdebug("atomicfile", "%p opened %s: %qd bytes", this, path, mLength);

	return mLength;
}

//
// Read the file starting at inOffset for inLength bytes into the buffer and return
// a pointer to it.  On return outLength contain the actual number of bytes read, it
// will only ever be less than inLength if EOF was reached, and it will never be more
// than inLength.
//
const uint8 *
AtomicBufferedFile::read(off_t inOffset, off_t inLength, off_t &outLength)
{
	if (mFileRef < 0)
	{
		secdebug("atomicfile", "read %s: file yet not opened, opening", mPath.c_str());
		open();
	}

	off_t bytesLeft = inLength;
	uint8 *ptr;
	if (mBuffer)
	{
		secdebug("atomicfile", "%p free %s buffer %p", this, mPath.c_str(), mBuffer);
		free(mBuffer);
	}

	mBuffer = ptr = reinterpret_cast<uint8 *>(malloc(bytesLeft));
	secdebug("atomicfile", "%p allocated %s buffer %p size %qd", this, mPath.c_str(), mBuffer, bytesLeft);
	off_t pos = inOffset;
	while (bytesLeft)
	{
		size_t toRead = bytesLeft > kAtomicFileMaxBlockSize ? kAtomicFileMaxBlockSize : size_t(bytesLeft);
		ssize_t bytesRead = ::pread(mFileRef, ptr, toRead, pos);
		if (bytesRead == -1)
		{
			int error = errno;
			if (error == EINTR)
			{
				// We got interrupted by a signal, so try again.
				secdebug("atomicfile", "pread %s: interrupted, retrying", mPath.c_str());
				continue;
			}

			secdebug("atomicfile", "pread %s: %s", mPath.c_str(), strerror(error));
			free(mBuffer);
			mBuffer = NULL;
			UnixError::throwMe(error);
		}

		// Read returning 0 means EOF was reached so we're done.
		if (bytesRead == 0)
			break;

		secdebug("atomicfile", "%p read %s: %d bytes to %p", this, mPath.c_str(), bytesRead, ptr);

		bytesLeft -= bytesRead;
		ptr += bytesRead;
		pos += bytesRead;
	}

	// Compute length
	outLength = ptr - mBuffer;

	return mBuffer;
}

void
AtomicBufferedFile::close()
{
	if (mFileRef < 0)
	{
		secdebug("atomicfile", "close %s: already closed", mPath.c_str());
	}
	else
	{
		int result = AtomicFile::rclose(mFileRef);
		mFileRef = -1;
		if (result == -1)
		{
			int error = errno;
			secdebug("atomicfile", "close %s: %s", mPath.c_str(), strerror(errno));
			UnixError::throwMe(error);
		}

		secdebug("atomicfile", "%p closed %s", this, mPath.c_str());
	}
}


//
// AtomicTempFile - A temporary file to write changes to.
//
AtomicTempFile::AtomicTempFile(AtomicFile &inFile, const RefPointer<AtomicLockedFile> &inLockedFile, mode_t mode) :
	mFile(inFile),
	mLockedFile(inLockedFile),
	mCreating(true)
{
	create(mode);
}

AtomicTempFile::AtomicTempFile(AtomicFile &inFile, const RefPointer<AtomicLockedFile> &inLockedFile) :
	mFile(inFile),
	mLockedFile(inLockedFile),
	mCreating(false)
{
	create(mFile.mode());
}

AtomicTempFile::~AtomicTempFile()
{
	// rollback if we didn't commit yet.
	if (mFileRef >= 0)
		rollback();
}

//
// Open the file and return the length in bytes.
//
void
AtomicTempFile::create(mode_t mode)
{
	mPath = mFile.dir() + "," + mFile.file();
	const char *path = mPath.c_str();

	mFileRef = AtomicFile::ropen(path, O_WRONLY|O_CREAT|O_TRUNC, mode);
    if (mFileRef == -1)
    {
        int error = errno;
		secdebug("atomicfile", "open %s: %s", path, strerror(error));

        // Do the obvious error code translations here.
		// @@@ Consider moving these up a level.
        if (error == EACCES)
			CssmError::throwMe(CSSM_ERRCODE_OS_ACCESS_DENIED);
		else
			UnixError::throwMe(error);
    }

	secdebug("atomicfile", "%p created %s", this, path);
}

void
AtomicTempFile::write(AtomicFile::OffsetType inOffsetType, off_t inOffset, const uint32 inData)
{
    uint32 aData = htonl(inData);
    write(inOffsetType, inOffset, reinterpret_cast<uint8 *>(&aData), sizeof(aData));
}

void
AtomicTempFile::write(AtomicFile::OffsetType inOffsetType, off_t inOffset,
				  const uint32 *inData, uint32 inCount)
{
#ifdef HOST_LONG_IS_NETWORK_LONG
    // Optimize this for the case where hl == nl
    const uint32 *aBuffer = inData;
#else
    auto_array<uint32> aBuffer(inCount);
    for (uint32 i = 0; i < inCount; i++)
        aBuffer.get()[i] = htonl(inData[i]);
#endif

    write(inOffsetType, inOffset, reinterpret_cast<const uint8 *>(aBuffer.get()),
    	  inCount * sizeof(*inData));
}

void
AtomicTempFile::write(AtomicFile::OffsetType inOffsetType, off_t inOffset, const uint8 *inData, size_t inLength)
{
	off_t pos;
	if (inOffsetType == AtomicFile::FromEnd)
	{
		pos = ::lseek(mFileRef, 0, SEEK_END);
		if (pos == -1)
		{
			int error = errno;
			secdebug("atomicfile", "lseek(%s, %qd): %s", mPath.c_str(), inOffset, strerror(error));
			UnixError::throwMe(error);
		}
	}
	else if (inOffsetType == AtomicFile::FromStart)
		pos = inOffset;
	else
		CssmError::throwMe(CSSM_ERRCODE_INTERNAL_ERROR);

	off_t bytesLeft = inLength;
	const uint8 *ptr = inData;
	while (bytesLeft)
	{
		size_t toWrite = bytesLeft > kAtomicFileMaxBlockSize ? kAtomicFileMaxBlockSize : size_t(bytesLeft);
		ssize_t bytesWritten = ::pwrite(mFileRef, ptr, toWrite, pos);
		if (bytesWritten == -1)
		{
			int error = errno;
			if (error == EINTR)
			{
				// We got interrupted by a signal, so try again.
				secdebug("atomicfile", "write %s: interrupted, retrying", mPath.c_str());
				continue;
			}

			secdebug("atomicfile", "write %s: %s", mPath.c_str(), strerror(error));
			UnixError::throwMe(error);
		}

		// Write returning 0 is bad mmkay.
		if (bytesWritten == 0)
		{
			secdebug("atomicfile", "write %s: 0 bytes written", mPath.c_str());
			CssmError::throwMe(CSSMERR_DL_INTERNAL_ERROR);
		}

		secdebug("atomicfile", "%p wrote %s %d bytes from %p", this, mPath.c_str(), bytesWritten, ptr);

		bytesLeft -= bytesWritten;
		ptr += bytesWritten;
		pos += bytesWritten;
	}
}

void
AtomicTempFile::fsync()
{
	if (mFileRef < 0)
	{
		secdebug("atomicfile", "fsync %s: already closed", mPath.c_str());
	}
	else
	{
		int result;
		do
		{
			result = ::fsync(mFileRef);
		} while (result && errno == EINTR);

		if (result == -1)
		{
			int error = errno;
			secdebug("atomicfile", "fsync %s: %s", mPath.c_str(), strerror(errno));
			UnixError::throwMe(error);
		}

		secdebug("atomicfile", "%p fsynced %s", this, mPath.c_str());
	}
}

void
AtomicTempFile::close()
{
	if (mFileRef < 0)
	{
		secdebug("atomicfile", "close %s: already closed", mPath.c_str());
	}
	else
	{
		int result = AtomicFile::rclose(mFileRef);
		mFileRef = -1;
		if (result == -1)
		{
			int error = errno;
			secdebug("atomicfile", "close %s: %s", mPath.c_str(), strerror(errno));
			UnixError::throwMe(error);
		}

		secdebug("atomicfile", "%p closed %s", this, mPath.c_str());
	}
}

// Commit the current create or write and close the write file.  Note that a throw during the commit does an automatic rollback.
void
AtomicTempFile::commit()
{
	try
	{
		fsync();
		close();
		const char *oldPath = mPath.c_str();
		const char *newPath = mFile.path().c_str();
		if (::rename(oldPath, newPath) == -1)
		{
			int error = errno;
			secdebug("atomicfile", "rename (%s, %s): %s", oldPath, newPath, strerror(errno));
			UnixError::throwMe(error);
		}

		// Unlock the lockfile
		mLockedFile = NULL;

		secdebug("atomicfile", "%p commited %s", this, oldPath);
	}
	catch (...)
	{
		rollback();
		throw;
	}
}

// Rollback the current create or write (happens automatically if commit() isn't called before the destructor is.
void
AtomicTempFile::rollback() throw()
{
	if (mFileRef >= 0)
	{
		AtomicFile::rclose(mFileRef);
		mFileRef = -1;
	}

	// @@@ Log errors if this fails.
	const char *path = mPath.c_str();
	if (::unlink(path) == -1)
	{
		secdebug("atomicfile", "unlink %s: %s", path, strerror(errno));
		// rollback can't throw
	}

	// @@@ Think about this.  Depending on how we do locking we might not need this.
	if (mCreating)
	{
		const char *path = mFile.path().c_str();
		if (::unlink(path) == -1)
		{
			secdebug("atomicfile", "unlink %s: %s", path, strerror(errno));
			// rollback can't throw
		}
	}
}


//
// An advisory write lock for inFile.
//
AtomicLockedFile::AtomicLockedFile(AtomicFile &inFile) :
	mDir(inFile.dir()),
	mPath(inFile.dir() + "lck~" + inFile.file())
{
	lock();
}

AtomicLockedFile::~AtomicLockedFile()
{
	unlock();
}

std::string
AtomicLockedFile::unique(mode_t mode)
{
	static const int randomPart = 16;
	DevRandomGenerator randomGen;
	std::string::size_type dirSize = mDir.size();
	std::string fullname(dirSize + randomPart + 2, '\0');
	fullname.replace(0, dirSize, mDir);
	fullname[dirSize] = '~'; /* UNIQ_PREFIX */
	char buf[randomPart];
	struct stat filebuf;
	int result, fd = -1;

	for (int retries = 0; retries < 10; ++retries)
	{
		/* Make a random filename. */
		randomGen.random(buf, randomPart);
		for (int ix = 0; ix < randomPart; ++ix)
		{
			char ch = buf[ix] & 0x3f;
			fullname[ix + dirSize + 1] = ch +
				( ch < 26            ? 'A'
				: ch < 26 + 26       ? 'a' - 26
				: ch < 26 + 26 + 10  ? '0' - 26 - 26
				: ch == 26 + 26 + 10 ? '-' - 26 - 26 - 10
				:                      '_' - 26 - 26 - 11);
		}

		result = lstat(fullname.c_str(), &filebuf);
		if (result && errno == ENAMETOOLONG)
		{
			do
				fullname.erase(fullname.end() - 1);
			while((result = lstat(fullname.c_str(), &filebuf)) && errno == ENAMETOOLONG && fullname.size() > dirSize + 8);
		}       /* either it stopped being a problem or we ran out of filename */

		if (result && errno == ENOENT)
		{
			fd = AtomicFile::ropen(fullname.c_str(), O_WRONLY|O_CREAT|O_EXCL, mode);
			if (fd >= 0 || errno != EEXIST)
				break;
		}
	}

	if (fd < 0)
	{
		int error = errno;
		::syslog(LOG_ERR, "Couldn't create temp file %s: %s", fullname.c_str(), strerror(error));
		secdebug("atomicfile", "Couldn't create temp file %s: %s", fullname.c_str(), strerror(error));
		UnixError::throwMe(error);
	}

	/* @@@ Check for EINTR. */
	write(fd, "0", 1); /* pid 0, `works' across networks */

	AtomicFile::rclose(fd);

	return fullname;
}

/* Return 0 on success and 1 on failure if st is set to the result of stat(old) and -1 on failure if the stat(old) failed. */
int
AtomicLockedFile::rlink(const char *const old, const char *const newn, struct stat &sto)
{
	int result = ::link(old,newn);
	if (result)
	{
		int serrno = errno;
		if (::lstat(old, &sto) == 0)
		{
			struct stat stn;
			if (::lstat(newn, &stn) == 0
				&& sto.st_dev == stn.st_dev
				&& sto.st_ino == stn.st_ino
				&& sto.st_uid == stn.st_uid
				&& sto.st_gid == stn.st_gid
				&& !S_ISLNK(sto.st_mode))
			{
				/* Link failed but files are the same so the link really went ok. */
				return 0;
			}
			else
				result = 1;
		}
		errno = serrno; /* Restore errno from link() */
	}

	return result;
}

/* NFS-resistant rename()
 * rename with fallback for systems that don't support it
 * Note that this does not preserve the contents of the file. */
int
AtomicLockedFile::myrename(const char *const old, const char *const newn)
{
	struct stat stbuf;
	int fd = -1;
	int ret;

	/* Try a real hardlink */
	ret = rlink(old, newn, stbuf);
	if (ret > 0)
	{
		if (stbuf.st_nlink < 2 && (errno == EXDEV || errno == ENOTSUP))
		{
			/* Hard link failed so just create a new file with O_EXCL instead.  */
			fd = AtomicFile::ropen(newn, O_WRONLY|O_CREAT|O_EXCL, stbuf.st_mode);
			if (fd >= 0)
				ret = 0;
		}
	}

	/* We want the errno from the link or the ropen, not that of the unlink. */
	int serrno = errno;

	/* Unlink the temp file. */
	::unlink(old);
	if (fd > 0)
		AtomicFile::rclose(fd);

	errno = serrno;
	return ret;
}

int
AtomicLockedFile::xcreat(const char *const name, mode_t mode, time_t &tim)
{
	std::string uniqueName = unique(mode);
	const char *uniquePath = uniqueName.c_str();
	struct stat stbuf;       /* return the filesystem time to the caller */
	stat(uniquePath, &stbuf);
	tim = stbuf.st_mtime;
	return myrename(uniquePath, name);
}

void
AtomicLockedFile::lock(mode_t mode)
{
	const char *path = mPath.c_str();
	bool triedforce = false;
	struct stat stbuf;
	time_t t, locktimeout = 1024; /* DEFlocktimeout, 17 minutes. */
	bool doSyslog = false;
	bool failed = false;
	int retries = 0;

	while (!failed)
	{
		/* Don't syslog first time through. */
		if (doSyslog)
			::syslog(LOG_NOTICE, "Locking %s", path);
		else
			doSyslog = true;

		secdebug("atomicfile", "Locking %s", path);          /* in order to cater for clock skew: get */
		if (!xcreat(path, mode, t))    /* time t from the filesystem */
		{
			/* lock acquired, hurray! */
			break;
		}
		switch(errno)
		{
		case EEXIST:               /* check if it's time for a lock override */
			if (!lstat(path, &stbuf) && stbuf.st_size <= 16 /* MAX_locksize */ && locktimeout
				&& !lstat(path, &stbuf) && locktimeout < t - stbuf.st_mtime)
				/* stat() till unlink() should be atomic, but can't guarantee that. */
			{
				if (triedforce)
				{
					/* Already tried, force lock override, not trying again */
					failed = true;
					break;
				}
				else if (S_ISDIR(stbuf.st_mode) || ::unlink(path))
				{
					triedforce=true;
					::syslog(LOG_ERR, "Forced unlock denied on %s", path);
					secdebug("atomicfile", "Forced unlock denied on %s", path);
				}
				else
				{
					::syslog(LOG_ERR, "Forcing lock on %s", path);
					secdebug("atomicfile", "Forcing lock on %s", path);
					sleep(16 /* DEFsuspend */);
					break;
				}
			}
			else
				triedforce = false;              /* legitimate iteration, clear flag */

			/* Reset retry counter. */
			retries = 0;
			sleep(8 /* DEFlocksleep */);
			break;

		case ENOSPC:               /* no space left, treat it as a transient */
#ifdef EDQUOT                                                 /* NFS failure */
		case EDQUOT:                  /* maybe it was a short term shortage? */
#endif
		case ENOENT:
		case ENOTDIR:
		case EIO:
		/*case EACCES:*/
			if(++retries < (7 + 1))  /* nfsTRY number of times+1 to ignore spurious NFS errors */
				sleep(8 /* DEFlocksleep */);
			else
				failed = true;
			break;

#ifdef ENAMETOOLONG
		case ENAMETOOLONG:     /* Filename is too long, shorten and retry */
			if (mPath.size() > mDir.size() + 8)
			{
				secdebug("atomicfile", "Truncating %s and retrying lock", path);
				mPath.erase(mPath.end() - 1);
				path = mPath.c_str();
				/* Reset retry counter. */
				retries = 0;
				break;
			}
		/* DROPTHROUGH */
#endif
		default:
			failed = true;
			break;
		}
	}

	if (failed)
	{
		int error = errno;
		::syslog(LOG_ERR, "Lock failure on %s: %s", path, strerror(error));
		secdebug("atomicfile", "Lock failure on %s: %s", path, strerror(error));
		UnixError::throwMe(error);
	}
}

void
AtomicLockedFile::unlock() throw()
{
	const char *path = mPath.c_str();
	if (::unlink(path) == -1)
	{
		secdebug("atomicfile", "unlink %s: %s", path, strerror(errno));
		// unlock can't throw
	}
}


#undef kAtomicFileMaxBlockSize
