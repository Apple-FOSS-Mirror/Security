/* Copyright (c) 2012-2014 Apple Inc. All Rights Reserved. */

#include "securityd_service.h"
#include "securityd_service_client.h"
#include <libaks.h>

#include <sandbox.h>
#include <vproc.h>
#include <xpc/xpc.h>
#include <xpc/private.h>
#include <dispatch/dispatch.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/codesign.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>
#include <syslog.h>
#include <unistd.h>
#include <pwd.h>
#include <uuid/uuid.h>
#include <bsm/libbsm.h>
#include <copyfile.h>
#include <AssertMacros.h>
#include <Security/Security.h>
#include <Security/SecTask.h>
#include <Security/SecTaskPriv.h>
#include <Security/SecKeychainPriv.h>

#include <IOKit/IOKitLib.h>
#include <Kernel/IOKit/crypto/AppleFDEKeyStoreDefs.h>

#if DEBUG
#define LOG(...)    syslog(LOG_ERR, ##__VA_ARGS__);
#else
#define LOG(...)
#endif

static bool check_signature(xpc_connection_t connection);

static pid_t get_caller_pid(audit_token_t * token)
{
    pid_t pid = 0;
    if (token) {
        audit_token_to_au32(*token, NULL, NULL, NULL, NULL, NULL, &pid, NULL, NULL);
    }
    return pid;
}

// exported from libaks.a
kern_return_t aks_register_for_notifications(mach_port_t server_port, uintptr_t message_id);
kern_return_t aks_stash_escrow(keybag_handle_t handle, bool create, const void * secret, int secret_len, const void * in_data, int in_data_len, void ** out_data, int * out_data_len);

const char * kb_home_path = "Library/Keychains";
const char * kb_user_bag = "user.kb";
const char * kb_stash_bag = "stash.kb";

#define HEXBUF_LEN 2048

typedef struct {
    uid_t uid;
    gid_t gid;
    char * name;
    char * home;
} service_user_record_t;

typedef enum {
    kb_bag_type_user,
    kb_bag_type_stash
} kb_bag_type_t;

static io_connect_t
openiodev(void)
{
    io_registry_entry_t service;
    io_connect_t conn;
    kern_return_t kr;

    service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(kAppleFDEKeyStoreServiceName));
    if (service == IO_OBJECT_NULL)
        return IO_OBJECT_NULL;

    kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
    if (kr != KERN_SUCCESS)
        return IO_OBJECT_NULL;

    kr = IOConnectCallMethod(conn, kAppleFDEKeyStoreUserClientOpen, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    if (kr != KERN_SUCCESS) {
        IOServiceClose(conn);
        return IO_OBJECT_NULL;
    }

    return conn;
}

static void
closeiodev(io_connect_t conn)
{
    kern_return_t kr;
    kr = IOConnectCallMethod(conn, kAppleFDEKeyStoreUserClientClose, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL);
    if (kr != KERN_SUCCESS)
        return;
    IOServiceClose(conn);
}

static dispatch_queue_t
_kb_service_get_dispatch_queue()
{
    static dispatch_once_t onceToken = 0;
    static dispatch_queue_t connection_queue = NULL;

    dispatch_once(&onceToken, ^{
        connection_queue = dispatch_queue_create("kb-service-queue", DISPATCH_QUEUE_SERIAL);
    });

    return connection_queue;
}

static service_user_record_t * get_user_record(uid_t uid)
{
    service_user_record_t * ur = NULL;
    long bufsize = 0;
    if ((bufsize = sysconf(_SC_GETPW_R_SIZE_MAX)) == -1) {
        bufsize = 4096;
    }
    char buf[bufsize];
    struct passwd pwbuf, *pw = NULL;
    if ((getpwuid_r(uid, &pwbuf, buf, bufsize, &pw) == 0) && pw != NULL) {
        ur = calloc(1u, sizeof(service_user_record_t));
        require(ur, done);
        ur->uid = pw->pw_uid;
        ur->gid = pw->pw_gid;
        ur->home = strdup(pw->pw_dir);
        ur->name = strdup(pw->pw_name);
    } else {
        syslog(LOG_ERR, "failed to lookup user record for uid: %d", uid);
    }

done:
    return ur;
}

static void free_user_record(service_user_record_t * ur)
{
    if (ur != NULL) {
        if (ur->home) {
            free(ur->home);
        }
        if (ur->name) {
            free(ur->name);
        }
        free(ur);
    }
}

static const char * get_host_uuid()
{
    static uuid_string_t hostuuid = {};
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        struct timespec timeout = {30, 0};
        uuid_t uuid = {};
        if (gethostuuid(uuid, &timeout) == 0) {
            uuid_unparse(uuid, hostuuid);
        } else {
            syslog(LOG_ERR, "failed to get host uuid");
        }
    });

    return hostuuid;
}

static char *
_kb_copy_bag_filename(service_user_record_t * ur, kb_bag_type_t type)
{
    char * bag_file = NULL;
    const char * name = NULL;

    require(ur, done);
    switch(type) {
        case kb_bag_type_user:
            name = kb_user_bag;
            break;
        case kb_bag_type_stash:
            name = kb_stash_bag;
            break;
        default:
            goto done;
    }

    bag_file = calloc(1u, PATH_MAX);
    require(bag_file, done);

    snprintf(bag_file, PATH_MAX, "%s/%s/%s/%s", ur->home, kb_home_path, get_host_uuid(), name);

done:
    return bag_file;
}

static bool
_kb_verify_create_path(service_user_record_t * ur)
{
    bool created = false;
    struct stat st_info = {};
    char new_path[PATH_MAX] = {};
    char kb_path[PATH_MAX] = {};
    snprintf(kb_path, sizeof(kb_path), "%s/%s/%s", ur->home, kb_home_path, get_host_uuid());
    if (lstat(kb_path, &st_info) == 0) {
        if (S_ISDIR(st_info.st_mode)) {
            created = true;
        } else {
            syslog(LOG_ERR, "invalid directory at '%s' moving aside", kb_path);
            snprintf(new_path, sizeof(new_path), "%s-invalid", kb_path);
            unlink(new_path);
            if (rename(kb_path, new_path) != 0) {
                syslog(LOG_ERR, "failed to rename file: %s (%s)", kb_path, strerror(errno));
                goto done;
            }
        }
    }
    if (!created) {
        require_action(mkpath_np(kb_path, 0700) == 0, done, syslog(LOG_ERR, "could not create path: %s (%s)", kb_path, strerror(errno)));
        created = true;
    }

done:
    return created;
}

static void
_set_thread_credentials(service_user_record_t * ur)
{
    int rc = pthread_setugid_np(ur->uid, ur->gid);
    if (rc) { syslog(LOG_ERR, "failed to set thread credential: %i (%s)", errno, strerror(errno)); }

    rc = initgroups(ur->name, ur->gid);
    if (rc) { syslog(LOG_ERR, "failed to initgroups: %i", rc); }
}

static void
_clear_thread_credentials()
{
    int rc = pthread_setugid_np(KAUTH_UID_NONE, KAUTH_GID_NONE);
    if (rc) { syslog(LOG_ERR, "failed to reset thread credential: %i (%s)", errno, strerror(errno)); }
}

static bool
_kb_bag_exists(service_user_record_t * ur, const char * bag_file)
{
    bool exists = false;
    struct stat st_info = {};
    char new_file[PATH_MAX] = {};

    require(ur, done);

    _set_thread_credentials(ur);
    if (lstat(bag_file, &st_info) == 0) {
        if (S_ISREG(st_info.st_mode)) {
            exists = true;
        } else {
            syslog(LOG_ERR, "invalid file at '%s' moving aside", bag_file);
            snprintf(new_file, sizeof(new_file), "%s-invalid", bag_file);
            unlink(new_file);
            if (rename(bag_file, new_file) != 0) {
                syslog(LOG_ERR, "failed to rename file: %s (%s)", bag_file, strerror(errno));
            }
        }
    }

done:
    _clear_thread_credentials();
    return exists;
}

static bool
_kb_save_bag_to_disk(service_user_record_t * ur, const char * bag_file, void * data, size_t length)
{
    bool result = false;
    int fd = -1;

    require(bag_file, done);

    _set_thread_credentials(ur);
    require(_kb_verify_create_path(ur), done);

    fd = open(bag_file, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    require_action(fd != -1, done, syslog(LOG_ERR, "could not create file: %s (%s)", bag_file, strerror(errno)));
    require_action(write(fd, data, length) != -1, done, syslog(LOG_ERR, "failed to write keybag to disk %s", strerror(errno)));

    result = true;

done:
    if (fd != -1) { close(fd); }
    _clear_thread_credentials();
    return result;
}

static bool
_kb_load_bag_from_disk(service_user_record_t * ur, const char * bag_file, uint8_t ** data, size_t * length)
{
    bool result = false;
    int fd = -1;
    uint8_t * buf = NULL;
    size_t buf_size = 0;
    struct stat st_info = {};

    require(bag_file, done);

    _set_thread_credentials(ur);
    require(_kb_verify_create_path(ur), done);
    require_quiet(lstat(bag_file, &st_info) == 0, done);
    require_action(S_ISREG(st_info.st_mode), done, syslog(LOG_ERR, "failed to load, not a file: %s", bag_file));
    buf_size = (size_t)st_info.st_size;

    fd = open(bag_file, O_RDONLY | O_NOFOLLOW);
    require_action(fd != -1, done, syslog(LOG_ERR, "could not open file: %s (%s)", bag_file, strerror(errno)));

    buf = (uint8_t *)calloc(1u, buf_size);
    require(buf != NULL, done);
    require(read(fd, buf, buf_size) == buf_size, done);

    *data = buf;
    *length = buf_size;
    buf = NULL;
    result = true;

done:
    if (fd != -1) { close(fd); }
    if (buf) { free(buf); }
    _clear_thread_credentials();
    return result;
}

static void
_kb_rename_bag_on_disk(service_user_record_t * ur, const char * bag_file)
{
    char new_file[PATH_MAX] = {};
    if (bag_file) {
        _set_thread_credentials(ur);
        snprintf(new_file, sizeof(new_file), "%s-invalid", bag_file);
        unlink(new_file);
        rename(bag_file, new_file);
        _clear_thread_credentials();
    }
}

static void
_kb_delete_bag_on_disk(service_user_record_t * ur, const char * bag_file)
{
    if (bag_file) {
        _set_thread_credentials(ur);
        unlink(bag_file);
        _clear_thread_credentials();
    }
}

static int service_kb_load(service_context_t *context);
static int service_kb_load_uid(uid_t s_uid);

static int
_kb_get_session_handle(service_context_t * context, keybag_handle_t * handle_out)
{
    int rc = KB_BagNotLoaded;
    require_noerr_quiet(aks_get_system(context->s_uid, handle_out), done);

    rc = KB_Success;

done:
    if (rc == KB_BagNotLoaded) {
        if (service_kb_load(context) == KB_Success) {
            if (aks_get_system(context->s_uid, handle_out) == kIOReturnSuccess) {
                rc = KB_Success;
            }
        }
    }
    return rc;
}

static void update_keybag_handle(keybag_handle_t handle)
{
    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        uid_t uid = abs(handle);
        uint8_t * buf = NULL;
        size_t buf_size = 0;
        service_user_record_t * ur = NULL;
        char * bag_file = NULL;

        require_noerr(aks_save_bag(handle, (void**)&buf, (int*)&buf_size), done);
        require(ur = get_user_record(uid), done);
        require(bag_file = _kb_copy_bag_filename(ur, kb_bag_type_user), done);
        require(_kb_save_bag_to_disk(ur, bag_file, buf, buf_size), done);

        syslog(LOG_NOTICE, "successfully updated handle %d", handle);

    done:
        if (buf) free(buf);
        if (ur) free_user_record(ur);
        if (bag_file) free(bag_file);
    });
}

static int
service_kb_create(service_context_t * context, const void * secret, int secret_len)
{
    __block int rc = KB_GeneralError;

    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        uint8_t * buf = NULL;
        size_t buf_size = 0;
        keybag_handle_t session_handle = bad_keybag_handle;
        service_user_record_t * ur = get_user_record(context->s_uid);
        char * bag_file = _kb_copy_bag_filename(ur, kb_bag_type_user);

        require(bag_file, done);

        // check for the existance of the bagfile
        require_action(!_kb_bag_exists(ur, bag_file), done, rc = KB_BagExists);

        require_noerr(rc = aks_create_bag(secret, secret_len, kAppleKeyStoreDeviceBag, &session_handle), done);
        require_noerr(rc = aks_save_bag(session_handle, (void**)&buf, (int*)&buf_size), done);
        require_action(_kb_save_bag_to_disk(ur, bag_file, buf, buf_size), done, rc = KB_BagError);
        require_noerr(rc = aks_set_system(session_handle, context->s_uid), done);
        aks_unload_bag(session_handle);
        require_noerr(rc = _kb_get_session_handle(context, &session_handle), done);

        if (secret && rc == KB_Success) {
            aks_unlock_bag(session_handle, secret, secret_len);
        }

    done:
        if (buf) free(buf);
        if (bag_file) { free(bag_file); }
        if (ur) free_user_record(ur);
    });

    return rc;
}

/* Load s_uid's keybag, unless already loaded */
static int
_service_kb_load_uid(uid_t s_uid)
{
    __block int rc = KB_GeneralError;

    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        uint8_t * buf = NULL;
        size_t buf_size = 0;
        keybag_handle_t session_handle = bad_keybag_handle;
        service_user_record_t * ur = NULL;
        char * bag_file = NULL;

        rc = aks_get_system(s_uid, &session_handle);
        if (rc == kIOReturnNotFound) {
            require_action(ur = get_user_record(s_uid), done, rc = KB_GeneralError);
            require_action(bag_file = _kb_copy_bag_filename(ur, kb_bag_type_user), done, rc = KB_GeneralError);
            require_action_quiet(_kb_load_bag_from_disk(ur, bag_file, &buf, &buf_size), done, rc = KB_BagNotFound);
            rc = aks_load_bag(buf, (int)buf_size, &session_handle);
            if (rc == kIOReturnNotPermitted) {
                syslog(LOG_ERR, "error loading keybag for uid (%i)", s_uid);
                _kb_rename_bag_on_disk(ur, bag_file);
                rc = KB_BagNotFound;
            }
            require_noerr(rc, done);
            require_noerr(rc = aks_set_system(session_handle, s_uid), done);
            aks_unload_bag(session_handle);
        }
        require(rc == KB_Success, done);

    done:
        if (buf) free(buf);
        if (ur) free_user_record(ur);
        if (bag_file) free(bag_file);
    });

    return rc;
}

static int
service_kb_load_uid(uid_t s_uid)
{
    return _service_kb_load_uid(s_uid);
}

static int
service_kb_load(service_context_t * context)
{
    return _service_kb_load_uid(context->s_uid);
}

static int
service_kb_unload(service_context_t *context)
{
    __block int rc = KB_GeneralError;

    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        keybag_handle_t session_handle = bad_keybag_handle;

        rc = aks_get_system(context->s_uid, &session_handle);
        if (rc == kIOReturnNotFound) {
            // No session bag, nothing to do
            rc = KB_Success;
            return;
        } else if (rc != kIOReturnSuccess) {
            syslog(LOG_ERR, "error locating session keybag for uid (%i) in session (%i)", context->s_uid, context->s_id);
            rc = KB_BagError;
            return;
        }

        rc = aks_unload_bag(session_handle);
        if (rc != kAKSReturnSuccess) {
            syslog(LOG_ERR, "error unloading keybag for uid (%i) in session (%i)", context->s_uid, context->s_id);
            rc = KB_BagError;
        } else {
            syslog(LOG_ERR, "successfully unloaded keybag (%ld) for uid (%i) in session (%i)", (long)session_handle, context->s_uid, context->s_id);
        }
    });

    return rc;
}

static int
service_kb_save(service_context_t * context)
{
    __block int rc = KB_GeneralError;
    keybag_handle_t session_handle;
    require_noerr(rc = _kb_get_session_handle(context, &session_handle), done);

    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        uint8_t * buf = NULL;
        size_t buf_size = 0;
        service_user_record_t * ur = NULL;
        char * bag_file = NULL;

        require_noerr(rc = aks_save_bag(session_handle, (void**)&buf, (int*)&buf_size), done);
        require_action(ur = get_user_record(context->s_uid), done, rc = KB_GeneralError);
        require_action(bag_file = _kb_copy_bag_filename(ur, kb_bag_type_user), done, rc = KB_GeneralError);
        require_action(_kb_save_bag_to_disk(ur, bag_file, buf, buf_size), done, rc = KB_BagError);

        rc = KB_Success;

    done:
        if (buf) free(buf);
        if (ur) free_user_record(ur);
        if (bag_file) free(bag_file);
        return;
    });

done:
    return rc;
}

static int
service_kb_unlock(service_context_t * context, const void * secret, int secret_len)
{
    int rc = KB_GeneralError;
    keybag_handle_t session_handle;
    require_noerr(rc = _kb_get_session_handle(context, &session_handle), done);

    rc = aks_unlock_bag(session_handle, secret, secret_len);

done:
    syslog(LOG_NOTICE, "aks_unlock_bag result: (%ld)", (long)rc);
    return rc;
}

static int
service_kb_lock(service_context_t * context)
{
    // this call has been disabled
    return -1;
}

static int
service_kb_change_secret(service_context_t * context, const void * secret, int secret_len, const void * new_secret, int new_secret_len)
{
    __block int rc = KB_GeneralError;
    keybag_handle_t session_handle;
    require_noerr(rc = _kb_get_session_handle(context, &session_handle), done);

    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        uint8_t * buf = NULL;
        size_t buf_size = 0;
        service_user_record_t * ur = NULL;
        char * bag_file = NULL;

        require_noerr(rc = aks_change_secret(session_handle, secret, secret_len, new_secret, new_secret_len, NULL, NULL), done);
        require_noerr(rc = aks_save_bag(session_handle, (void**)&buf, (int*)&buf_size), done);
        require_action(ur = get_user_record(context->s_uid), done, rc = KB_GeneralError);
        require_action(bag_file = _kb_copy_bag_filename(ur, kb_bag_type_user), done, rc = KB_GeneralError);
        require_action(_kb_save_bag_to_disk(ur, bag_file, buf, buf_size), done, rc = KB_BagError);

        rc = KB_Success;

    done:
        if (buf) free(buf);
        if (ur) free_user_record(ur);
        if (bag_file) free(bag_file);
        return;
    });

done:
    return rc;
}

static int
service_kb_reset(service_context_t * context, const void * secret, int secret_len)
{
    __block int rc = KB_GeneralError;
    service_user_record_t * ur = NULL;
    char * bag_file = NULL;

    require_action(ur = get_user_record(context->s_uid), done, rc = KB_GeneralError);
    require_action(bag_file = _kb_copy_bag_filename(ur, kb_bag_type_user), done, rc = KB_GeneralError);

    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        uint8_t * buf = NULL;
        size_t buf_size = 0;
        keybag_handle_t session_handle = bad_keybag_handle;

        syslog(LOG_ERR, "resetting keybag for uid (%i) in session (%i)", context->s_uid, context->s_id);
        _kb_rename_bag_on_disk(ur, bag_file);

        require_noerr(rc = aks_create_bag(secret, secret_len, kAppleKeyStoreDeviceBag, &session_handle), done);
        require_noerr(rc = aks_save_bag(session_handle, (void**)&buf, (int*)&buf_size), done);
        require_action(_kb_save_bag_to_disk(ur, bag_file, buf, buf_size), done, rc = KB_BagError);
        require_noerr(rc = aks_set_system(session_handle, context->s_uid), done);
        aks_unload_bag(session_handle);
        require_noerr(rc = _kb_get_session_handle(context, &session_handle), done);

        if (secret && rc == KB_Success) {
            aks_unlock_bag(session_handle, secret, secret_len);
        }

    done:
        if (buf) free(buf);
        return;
    });

done:
    if (ur) free_user_record(ur);
    if (bag_file) free(bag_file);
    return rc;
}

static int
service_kb_is_locked(service_context_t * context, xpc_object_t reply)
{
    int rc = KB_GeneralError;
    keybag_state_t state;
    keybag_handle_t session_handle;
    require_noerr(rc = _kb_get_session_handle(context, &session_handle), done);

    require_noerr(rc = aks_get_lock_state(session_handle, &state), done);

    xpc_dictionary_set_bool(reply, SERVICE_XPC_LOCKED, state & keybag_state_locked);
    xpc_dictionary_set_bool(reply, SERVICE_XPC_NO_PIN, state & keybag_state_no_pin);

done:
    return rc;
}

static int
service_kb_stash_create(service_context_t * context, const void * key, unsigned key_size)
{
    int rc = KB_GeneralError;
    char * bag_file = NULL;
    keybag_handle_t session_handle;
    service_user_record_t * ur = NULL;
    void * stashbag = NULL;
    int stashbag_size = 0;
    __block bool saved = false;

    require(key, done);
    require_noerr(rc = _kb_get_session_handle(context, &session_handle), done);
    require_action(ur = get_user_record(context->s_uid), done, rc = KB_GeneralError);
    require_noerr(rc = aks_stash_escrow(session_handle, true, key, key_size, NULL, 0, (void**)&stashbag, &stashbag_size), done);
    require_action(bag_file = _kb_copy_bag_filename(ur, kb_bag_type_stash), done, rc = KB_GeneralError);

    // sync writing the bag to disk
    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        saved = _kb_save_bag_to_disk(ur, bag_file, stashbag, stashbag_size);
    });
    require_action(saved, done, rc = KB_BagError);
    rc = KB_Success;

done:
    if (stashbag) { free(stashbag); }
    if (bag_file) { free(bag_file); }
    if (ur) free_user_record(ur);
    return rc;
}

static int
service_kb_stash_load(service_context_t * context, const void * key, unsigned key_size, bool nondestructive)
{
    __block int rc = KB_GeneralError;
    char * bag_file = NULL;
    keybag_handle_t session_handle;
    service_user_record_t * ur = NULL;
    __block uint8_t * stashbag = NULL;
    __block size_t stashbag_size = 0;

    require(key, done);
    require_noerr(rc = _kb_get_session_handle(context, &session_handle), done);
    require_action(ur = get_user_record(context->s_uid), done, rc = KB_GeneralError);
    require_action(bag_file = _kb_copy_bag_filename(ur, kb_bag_type_stash), done, rc = KB_GeneralError);

    // sync loading the bag from disk
    dispatch_sync(_kb_service_get_dispatch_queue(), ^{
        if (!_kb_load_bag_from_disk(ur, bag_file, &stashbag, &stashbag_size)) {
            rc = KB_BagError;
        }
    });
    require_noerr(rc, done);

    require_noerr(rc = aks_stash_escrow(session_handle, false, key, key_size, stashbag, (int)stashbag_size, NULL, NULL), done);
    rc = KB_Success;

done:
    if (stashbag) { free(stashbag); }
    if ((bag_file) && (!nondestructive)) {
        _kb_delete_bag_on_disk(ur, bag_file);
        free(bag_file);
    }
    if (ur) free_user_record(ur);
    return rc;
}

//
// Get the keychain master key from the AppleFDEKeyStore.
// Note that this is a one-time call - the master key is
// removed from the keystore after it is returned.
// Requires the entitlement: com.apple.private.securityd.keychain
//
OSStatus service_stash_get_key(service_context_t * context, xpc_object_t event, xpc_object_t reply)
{
    getStashKey_InStruct_t inStruct;
    getStashKey_OutStruct_t outStruct;
    size_t outSize = sizeof(outStruct);
    kern_return_t kr = KERN_INVALID_ARGUMENT;

    io_connect_t conn = openiodev();
    require(conn, done);
    inStruct.type = kAppleFDEKeyStoreStash_master;

    kr = IOConnectCallMethod(conn, kAppleFDEKeyStore_getStashKey,
                             NULL, 0,
                             &inStruct, sizeof(inStruct),
                             NULL, NULL,
                             &outStruct, &outSize);

    if (kr == KERN_SUCCESS) {
        xpc_dictionary_set_data(reply, SERVICE_XPC_KEY, outStruct.outBuf.key.key, outStruct.outBuf.key.keysize);
        service_kb_stash_load(context, outStruct.outBuf.key.key, outStruct.outBuf.key.keysize, false);
    }

done:
    if (conn)
        closeiodev(conn);

    return kr;
}

//
// Stash the keychain master key in the AppleFDEKeyStore and
// flag it as the keychain master key to be added to the
// reboot NVRAM blob.
// This requires two calls to the AKS: the first to store the
// key and get its uuid.  The second uses the uuid to flag the
// key for blob inclusion.
//
OSStatus service_stash_set_key(service_context_t * context, xpc_object_t event, xpc_object_t reply)
{
    kern_return_t kr = KERN_INVALID_ARGUMENT;
    io_connect_t conn = IO_OBJECT_NULL;
    size_t keydata_len = 0;
    size_t len;

    keybag_state_t state;
    keybag_handle_t session_handle;
    require_noerr(_kb_get_session_handle(context, &session_handle), done);
    require_noerr(aks_get_lock_state(session_handle, &state), done);
    require_action(!(state & keybag_lock_locked), done, kr = CSSMERR_CSP_OS_ACCESS_DENIED; LOG("stash failed keybag locked"));

    conn = openiodev();
    require(conn, done);

    // Store the key in the keystore and get its uuid
    setKeyGetUUID_InStruct_t inStruct1;
    uuid_OutStruct_t outStruct1;


    const uint8_t *keydata = xpc_dictionary_get_data(event, SERVICE_XPC_KEY, &keydata_len);
    require(keydata, done);

    memcpy(&inStruct1.inKey.key.key, keydata, keydata_len);
    inStruct1.inKey.key.keysize = (cryptosize_t) keydata_len;
    len = sizeof(outStruct1);
    kr = IOConnectCallMethod(conn, kAppleFDEKeyStore_setKeyGetUUID,
                             NULL, 0,
                             &inStruct1, sizeof(inStruct1),
                             NULL, NULL,
                             &outStruct1, &len);
    require(kr == KERN_SUCCESS, done);

    // Now using the uuid stash it as the master key
    setStashKey_InStruct_t inStruct2;
    memcpy(&inStruct2.uuid, &outStruct1.uuid, sizeof(outStruct1.uuid));
    inStruct2.type  = kAppleFDEKeyStoreStash_master;

    kr = IOConnectCallMethod(conn, kAppleFDEKeyStore_setStashKey,
                             NULL, 0,
                             &inStruct2, sizeof(inStruct2),
                             NULL, NULL,
                             NULL, NULL);

    if  (kr == KERN_SUCCESS) {
        service_kb_stash_create(context, keydata, (unsigned)keydata_len);
    }
done:
    if (conn)
        closeiodev(conn);

    return kr;
}

//
// Load the master stash key
//
OSStatus service_stash_load_key(service_context_t * context, xpc_object_t event, xpc_object_t reply)
{
    kern_return_t kr = KERN_SUCCESS;
    size_t keydata_len = 0;

    const uint8_t *keydata = xpc_dictionary_get_data(event, SERVICE_XPC_KEY, &keydata_len);
    require(keydata, done);

    kr = service_kb_stash_load(context, keydata, (cryptosize_t) keydata_len, true);
done:

    return kr;
}

//
// Signal the AppleFDEKeyStore to take the tagged FDE key
// and keychain master key, stash them in an encrypted
// blob structure and write the blob to NVRAM.  The random
// encryption key is written to the SMC.
//
#if DEBUG
OSStatus service_stash_blob(xpc_object_t event, xpc_object_t reply)
{
    kern_return_t kr = KERN_INVALID_ARGUMENT;

    io_connect_t conn = openiodev();
    require(conn, done);

    kr = IOConnectCallMethod(conn, kAppleFDEKeyStore_commitStash,
                             NULL, 0,
                             NULL, 0,
                             NULL, NULL,
                             NULL, NULL);
done:
    if (conn)
        closeiodev(conn);

    return kr;
}
#endif

bool peer_has_entitlement(xpc_connection_t peer, const char * entitlement)
{
    bool entitled = false;

    xpc_object_t value = xpc_connection_copy_entitlement_value(peer, entitlement);
    if (value && (xpc_get_type(value) == XPC_TYPE_BOOL)) {
        entitled = xpc_bool_get_value(value);
    }

    if (value) xpc_release(value);
    return entitled;
}

static char * sel_to_char(uint64_t sel)
{
    switch (sel) {
        case SERVICE_STASH_SET_KEY:
            return "set_key";
        case SERVICE_STASH_GET_KEY:
            return "get_key";
        case SERVICE_STASH_BLOB:
            return "stash_blob";
        case SERVICE_KB_LOAD:
            return "kb_load";
        case SERVICE_KB_SAVE:
            return "kb_save";
        case SERVICE_KB_UNLOCK:
            return "kb_unlock";
        case SERVICE_KB_LOCK:
            return "kb_lock";
        case SERVICE_KB_CHANGE_SECRET:
            return "kb_change_secret";
        case SERVICE_KB_CREATE:
            return "kb_create";
        case SERVICE_KB_IS_LOCKED:
            return "kb_is_locked";
        case SERVICE_KB_RESET:
            return "kb_reset";
        case SERVICE_KB_UNLOAD:
            return "kb_unload";
        case SERVICE_KB_LOAD_UID:
            return "kb_load_uid";
        default:
            return "unknown";
    }
}

static char * err_to_char(int err)
{
    switch (err) {
        case KB_Success:
            return "success";
        case KB_GeneralError:
            return "general error";
        case KB_BagNotFound:
            return "bag not found";
        case KB_BagError:
            return "bag error";
        case KB_BagNotLoaded:
            return "bag not loaded";
        case KB_BagExists:
            return "bag exists";
        case KB_InvalidSession:
            return "invalid session";
        default:
            return "";
    }
}

void service_peer_event_handler(xpc_connection_t connection, xpc_object_t event)
{
    xpc_type_t type = xpc_get_type(event);
    uid_t uid;

    if (type == XPC_TYPE_ERROR) {
		if (event == XPC_ERROR_CONNECTION_INVALID) {
        }
    } else {
        assert(type == XPC_TYPE_DICTIONARY);

        int rc = KB_GeneralError;
        uint64_t request = 0;
        const uint8_t * secret = NULL, * new_secret = NULL;
        size_t secret_len = 0, new_secret_len = 0, data_len = 0;
        service_context_t * context = NULL;
        bool free_context = false;
        const void * data;
        const char *entitlement;

        xpc_object_t reply = xpc_dictionary_create_reply(event);

        request = xpc_dictionary_get_uint64(event, SERVICE_XPC_REQUEST);


        // For SERVICE_KB_{UNLOAD,LOAD} only, allow non-securityd, non-root but
        // entitled callers.
        if (request == SERVICE_KB_UNLOAD || request == SERVICE_KB_LOAD_UID) {
            switch (request) {
                case SERVICE_KB_UNLOAD:
                    entitlement = "com.apple.private.securityd.keybag-unload";
                    break;
                case SERVICE_KB_LOAD_UID:
                    entitlement = "com.apple.private.securityd.keybag-load";
                    break;
            }
            if (!peer_has_entitlement(connection, entitlement) && !peer_has_entitlement(connection, "com.apple.keystore.device")) {
                xpc_connection_cancel(connection);
                return;
            }
        } else {
            if (xpc_connection_get_euid(connection) != 0) {
                xpc_connection_cancel(connection);
                return;
            }
            if (!check_signature(connection)) {
                xpc_connection_cancel(connection);
                return;
            }
        }

        data = xpc_dictionary_get_data(event, SERVICE_XPC_CONTEXT, &data_len);
        require_action(data || request == SERVICE_KB_UNLOAD || request == SERVICE_KB_LOAD_UID, done, rc = KB_GeneralError);
        if (data) {
            require(data_len == sizeof(service_context_t), done);
            context = (service_context_t*)data;
        } else {
            audit_token_t audit_token = { 0 };
            xpc_connection_get_audit_token(connection, &audit_token);
            context = calloc(1, sizeof(service_context_t));
            context->s_id = xpc_connection_get_asid(connection);
            context->s_uid = xpc_connection_get_euid(connection);
            context->procToken = audit_token;
            free_context = true;
        }

        require_action(context->s_id != AU_DEFAUDITSID, done, rc = KB_InvalidSession);
        require_action(context->s_uid != AU_DEFAUDITID, done, rc = KB_InvalidSession); // we only want to work in actual user sessions.

        switch (request) {
            case SERVICE_KB_CREATE:
                //                if (kb_service_has_entitlement(peer, "com.apple.keystore.device")) {
                secret = xpc_dictionary_get_data(event, SERVICE_XPC_SECRET, &secret_len);
                rc = service_kb_create(context, secret, (int)secret_len);
                //                }
                break;
            case SERVICE_KB_LOAD:
                rc = service_kb_load(context);
                break;
            case SERVICE_KB_UNLOAD:
                rc = service_kb_unload(context);
                break;
            case SERVICE_KB_SAVE:
                rc = service_kb_save(context);
                break;
            case SERVICE_KB_UNLOCK:
                secret = xpc_dictionary_get_data(event, SERVICE_XPC_SECRET, &secret_len);
                rc = service_kb_unlock(context, secret, (int)secret_len);
                break;
            case SERVICE_KB_LOCK:
                rc = service_kb_lock(context);
                break;
            case SERVICE_KB_CHANGE_SECRET:
                secret = xpc_dictionary_get_data(event, SERVICE_XPC_SECRET, &secret_len);
                new_secret = xpc_dictionary_get_data(event, SERVICE_XPC_SECRET_NEW, &new_secret_len);
                rc = service_kb_change_secret(context, secret, (int)secret_len, new_secret, (int)new_secret_len);
                break;
            case SERVICE_KB_RESET:
                secret = xpc_dictionary_get_data(event, SERVICE_XPC_SECRET, &secret_len);
                rc = service_kb_reset(context, secret, (int)secret_len);
                break;
            case SERVICE_KB_IS_LOCKED:
                rc = service_kb_is_locked(context, reply);
                break;
            case SERVICE_STASH_GET_KEY:
                rc = service_stash_get_key(context, event, reply);
                break;
            case SERVICE_STASH_SET_KEY:
                rc = service_stash_set_key(context, event, reply);
                break;
            case SERVICE_STASH_LOAD_KEY:
                rc = service_stash_load_key(context, event, reply);
                break;
            case SERVICE_KB_LOAD_UID:
                uid = (uid_t)xpc_dictionary_get_uint64(event, SERVICE_XPC_UID);
                rc = service_kb_load_uid(uid);
                break;
#if DEBUG
            case SERVICE_STASH_BLOB:
                rc = service_stash_blob(event, reply);
                break;
#endif
            default:
                LOG("unknown service type");
                break;
        }

    done:
#if DEBUG
        LOG("selector: %s (%llu), error: %s (%x), sid: %d, suid: %d, pid: %d", sel_to_char(request), request, err_to_char(rc), rc, context ? context->s_id : 0, context ? context->s_uid : 0, context ? get_caller_pid(&context->procToken) : 0);
#else
        if (rc != 0) {
            syslog(LOG_NOTICE, "selector: %s (%llu), error: %s (%x), sid: %d, suid: %d, pid: %d", sel_to_char(request), request, err_to_char(rc), rc, context ? context->s_id : 0, context ? context->s_uid : 0, context ? get_caller_pid(&context->procToken) : 0);
        }
#endif
        xpc_dictionary_set_int64(reply, SERVICE_XPC_RC, rc);
        xpc_connection_send_message(connection, reply);
        xpc_release(reply);
        if (free_context) {
            free(context);
        }
    }
}

bool check_signature(xpc_connection_t connection)
{
#if !(DEBUG || RC_BUILDIT_YES)
    audit_token_t token;

    xpc_connection_get_audit_token(connection, &token);

    SecTaskRef task = SecTaskCreateWithAuditToken(NULL, token);
    if (task == NULL) {
        syslog(LOG_NOTICE, "failed getting SecTaskRef of the client");
        return false;
    }

    uint32_t flags = SecTaskGetCodeSignStatus(task);
    /* check if valid and platform binary, but not platform path */
    if ((flags & (CS_VALID | CS_PLATFORM_BINARY | CS_PLATFORM_PATH)) != (CS_VALID | CS_PLATFORM_BINARY)) {
        syslog(LOG_NOTICE, "client is not a platform binary: %0x08x", flags);
        CFRelease(task);
        return false;
    }

    CFStringRef signingIdentity = SecTaskCopySigningIdentifier(task, NULL);
    CFRelease(task);
    if (signingIdentity == NULL) {
        syslog(LOG_NOTICE, "client have no code sign identity");
        return false;
    }

    bool res = CFEqual(signingIdentity, CFSTR("com.apple.securityd"));
    CFRelease(signingIdentity);

    if (!res)
        syslog(LOG_NOTICE, "client is not not securityd");

    return res;
#else
    return true;
#endif
}

static void register_for_notifications()
{
    __block kern_return_t kr;
    static mach_port_t mp = MACH_PORT_NULL;

    static dispatch_once_t onceToken = 0;
    dispatch_once(&onceToken, ^{
        kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &mp);
        if (kr == KERN_SUCCESS) {
            dispatch_source_t mach_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, mp, 0, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
            dispatch_source_set_event_handler(mach_src, ^{
                mach_msg_return_t mr;
                uint8_t buf[sizeof(aks_notification_msg_t) + MAX_TRAILER_SIZE] = {};
                aks_notification_msg_t * msg = (aks_notification_msg_t*)buf;
                mr = mach_msg((mach_msg_header_t*)&buf, MACH_RCV_MSG, 0, sizeof(buf), mp, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
                if (mr == MACH_MSG_SUCCESS && msg->hdr.msgh_id == AKS_NOTIFICATION_MSGID) {
                    // ignored for now
                } else if (mr == MACH_MSG_SUCCESS && msg->hdr.msgh_id == AKS_NOTIFICATION_WRITE_SYSTEM_KEYBAG) {
                    syslog(LOG_NOTICE, "request to update handle %d", msg->handle);
                    update_keybag_handle(msg->handle);
                } else {
                    syslog(LOG_ERR, "mach_msg error: %x", mr);
                }
            });
            dispatch_resume(mach_src);
        } else {
            syslog(LOG_NOTICE, "failed to create notification port");
        }

    });

    kr = aks_register_for_notifications(mp, AKS_NOTIFICATION_WRITE_SYSTEM_KEYBAG);
    if (kr == KERN_SUCCESS) {
        syslog(LOG_NOTICE, "registered for notifications");
    } else {
        syslog(LOG_NOTICE, "failed to register for notifications %d", kr);
    }
}

int main(int argc, const char * argv[])
{
    char * errorbuf;
    if (sandbox_init(SECURITYD_SERVICE_NAME, SANDBOX_NAMED, &errorbuf) != 0) {
        syslog(LOG_ERR, "sandbox_init failed %s", errorbuf);
        sandbox_free_error(errorbuf);
#ifndef DEBUG
        abort();
#endif
    }

    register_for_notifications();

    xpc_connection_t listener = xpc_connection_create_mach_service(SECURITYD_SERVICE_NAME, NULL, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
        // It is safe to cast 'peer' to xpc_connection_t assuming
        // we have a correct configuration in our launchd.plist.
        xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
            vproc_transaction_t transaction = vproc_transaction_begin(NULL);
            service_peer_event_handler(peer, event);
            vproc_transaction_end(NULL, transaction);
        });
        xpc_connection_resume(peer);
    });
    xpc_connection_resume(listener);

    dispatch_main();
}

