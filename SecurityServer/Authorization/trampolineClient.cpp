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
// trampolineClient - Authorization trampoline client-side implementation
//
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <Security/Authorization.h>
#include <Security/debugging.h>

//
// Where is the trampoline itself?
//
#if !defined(TRAMPOLINE)
# define TRAMPOLINE "/System/Library/CoreServices/AuthorizationTrampoline" /* fallback */
#endif


//
// A few names for clarity's sake
//
enum {
	READ = 0,		// read end of standard UNIX pipe
	WRITE = 1		// write end of standard UNIX pipe
};


//
// Local (static) functions
//
static const char **argVector(const char *trampoline,
	const char *tool, const char *commFd,
	char *const *arguments);


//
// The public client API function.
//
OSStatus AuthorizationExecuteWithPrivileges(AuthorizationRef authorization,
	const char *pathToTool,
	unsigned long flags,
	char *const *arguments,
	FILE **communicationsPipe)
{
	// flags are currently reserved
	if (flags != 0)
		return errAuthorizationInvalidFlags;

	// externalize the authorization
	AuthorizationExternalForm extForm;
	if (OSStatus err = AuthorizationMakeExternalForm(authorization, &extForm))
		return err;

    // create the mailbox file
    FILE *mbox = tmpfile();
    if (!mbox)
        return errAuthorizationInternal;
    if (fwrite(&extForm, sizeof(extForm), 1, mbox) != 1) {
        fclose(mbox);
        return errAuthorizationInternal;
    }
    
    // make text representation of the temp-file descriptor
    char mboxFdText[20];
    snprintf(mboxFdText, sizeof(mboxFdText), "auth %d", fileno(mbox));
    
	// make a notifier pipe
	int notify[2];
	if (pipe(notify)) {
        fclose(mbox);
		return errAuthorizationToolExecuteFailure;
    }

	// make the communications pipe if requested
	int comm[2];
	if (communicationsPipe && socketpair(AF_UNIX, SOCK_STREAM, 0, comm)) {
		close(notify[READ]); close(notify[WRITE]);
        fclose(mbox);
		return errAuthorizationToolExecuteFailure;
	}

	// do the standard forking tango...
	int delay = 1;
	for (int n = 5;; n--, delay *= 2) {
		switch (pid_t pid = fork()) {
		case -1:	// error
			if (errno == EAGAIN) {
				// potentially recoverable resource shortage
				if (n > 0) {
					debug("authexec", "resource shortage (EAGAIN), delaying %d seconds", delay);
					sleep(delay);
					continue;
				}
			}
			debug("authexec", "fork failed (errno=%d)", errno);
			close(notify[READ]); close(notify[WRITE]);
			return errAuthorizationToolExecuteFailure;

		default:	// parent
			// close foreign side of pipes
			close(notify[WRITE]);
			if (communicationsPipe)
				close(comm[WRITE]);
                
            // close mailbox file (child has it open now)
            fclose(mbox);
			
			// get status notification from child
			OSStatus status;
			debug("authexec", "parent waiting for status");
			switch (ssize_t rc = read(notify[READ], &status, sizeof(status))) {
			default:				// weird result of read: post error
				debug("authexec", "unexpected read return value %ld", long(rc));
				status = errAuthorizationToolEnvironmentError;
				// fall through
			case sizeof(status):	// read succeeded: child reported an error
				debug("authexec", "parent received status=%ld", status);
				close(notify[READ]);
				if (communicationsPipe) { close(comm[READ]); close(comm[WRITE]); }
				return status;
			case 0:					// end of file: exec succeeded
				close(notify[READ]);
				if (communicationsPipe)
					*communicationsPipe = fdopen(comm[READ], "r+");
				debug("authexec", "parent resumes (no error)");
				return noErr;
			}

		case 0:		// child
			// close foreign side of pipes
			close(notify[READ]);
			if (communicationsPipe)
				close(comm[READ]);
			
			// fd 1 (stdout) holds the notify write end
			dup2(notify[WRITE], 1);
			close(notify[WRITE]);
			
			// fd 0 (stdin) holds either the comm-link write-end or /dev/null
			if (communicationsPipe) {
				dup2(comm[WRITE], 0);
				close(comm[WRITE]);
			} else {
				close(0);
				open("/dev/null", O_RDWR);
			}
			
			// where is the trampoline?
#if defined(NDEBUG)
			const char *trampoline = TRAMPOLINE;
#else //!NDEBUG
			const char *trampoline = getenv("AUTHORIZATIONTRAMPOLINE");
			if (!trampoline)
				trampoline = TRAMPOLINE;
#endif //NDEBUG

			// okay, execute the trampoline
			debug("authexec", "child exec(%s:%s)",
				trampoline, pathToTool);
			if (const char **argv = argVector(trampoline, pathToTool, mboxFdText, arguments))
				execv(trampoline, (char *const[])argv);
			debug("authexec", "trampoline exec failed (errno=%d)", errno);
			
			// execute failed - tell the parent
			{
				OSStatus error = errAuthorizationToolExecuteFailure;
				write(1, &error, sizeof(error));
				_exit(1);
			}
		}
	}
}


//
// Build an argv vector
//
static const char **argVector(const char *trampoline, const char *pathToTool,
	const char *mboxFdText, char *const *arguments)
{
	int length = 0;
	if (arguments) {
		for (char *const *p = arguments; *p; p++)
			length++;
	}
	if (const char **args = (const char **)malloc(sizeof(const char *) * (length + 4))) {
		args[0] = trampoline;
		args[1] = pathToTool;
		args[2] = mboxFdText;
		if (arguments)
			for (int n = 0; arguments[n]; n++)
				args[n + 3] = arguments[n];
		args[length + 3] = NULL;
		return args;
	}
	return NULL;
}
