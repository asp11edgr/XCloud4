/* SPDX-License-Identifier: GPL-3.0-only */
#include "update_job51.h"
#ifdef X4_OPENORBIS
#include "installer51.h"
#include "manual51.h"
#include "update_io51.h"
#include "update_manifest.h"
#include "update_log51.h"
#include "../auth/http_client.h"
#include "../core/app_identity.h"
#include "../../build/update/pubkey51.h"
#include <errno.h>
#include <fcntl.h>
#include <orbis/libkernel.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define UPDATE_ROOT "/data/xcloud4/update"
#define UPDATE_PART UPDATE_ROOT "/xcloud4-update.pkg.part"
#define UPDATE_PKG UPDATE_ROOT "/xcloud4-update.pkg"
#define UPDATE_TARGET UPDATE_ROOT "/xcloud4-update.appver"
#define UPDATE_RESERVE (32u * 1024u * 1024u)
#define UPDATE_PENDING_SECONDS (24u * 60u * 60u)
#define UPDATE_RESUME_SECONDS 900u
#define UPDATE_STACK_BYTES (512u * 1024u)
#if X4_PUBLIC
#define UPDATE_EDITION "public"
#define UPDATE_TITLE "XCLD00003"
#else
#define UPDATE_EDITION "lab"
#define UPDATE_TITLE "XCLD00001"
#endif

typedef struct {
    char tag[64];
    bool local, recover, resume_choice, boot_published, diagnostic;
    bool part_owned, pkg_owned, target_owned, auth_owned;
    X4Update51Error error;
    X4UpdateLog51 log;
    X4UpdateLog51Detail detail;
} Job;

static Job job;
static X4Update51View view;
static atomic_flag view_lock = ATOMIC_FLAG_INIT;
static _Atomic int cancelled;
#if !X4_PUBLIC
static _Atomic int install_choice;
static atomic_bool leaving;
enum InstallChoice { INSTALL_WAIT, INSTALL_BGFT, INSTALL_MANUAL };
#endif
static char retry_target[6];
static bool retry_local;
static X4Update51ManualView manual_view;
static bool manual_offer;

static void begin_locked(X4Update51Stage initial);
static void launch_worker(void) __attribute__((noinline));

static void lock_view(void)
{
    while (atomic_flag_test_and_set_explicit(&view_lock, memory_order_acquire)) sceKernelUsleep(50);
}

static void unlock_view(void)
{
    atomic_flag_clear_explicit(&view_lock, memory_order_release);
}

X4Update51View x4_update51_view(void)
{
    lock_view();
    const X4Update51View copy = view;
    unlock_view();
    return copy;
}

static void stage(X4Update51Stage value, int code, bool busy)
{
    lock_view();
    view.stage = value;
    view.code = code;
    view.error = job.error;
    view.busy = busy;
    if (!busy && job.recover && !job.boot_published) manual_offer = true;
    unlock_view();
}

static bool checkpoint(X4Update51Stage value, int code, const X4Install51Result *installer)
{
    job.log.stage = value;
    job.log.code = code;
    if (installer) job.log.installer = *installer;
    return x4_update_log51_store_detail(&job.log, &job.detail);
}

/* Keep the original error when closing files or unwinding a failed sink. */
static int fail_at(X4Update51Error error, const char *function, int code, int system_errno)
{
    if (!job.detail.function[0]) {
        job.error = error;
        job.detail.system_errno = system_errno;
        (void)snprintf(job.detail.function, sizeof(job.detail.function), "%s", function);
    }
    return code;
}

static int fail_errno(X4Update51Error error, const char *function)
{
    const int saved = errno ? errno : EIO;
    return fail_at(error, function, -saved, saved);
}

static int is_cancelled(void)
{
    return atomic_load_explicit(&cancelled, memory_order_acquire);
}

static bool verified_dir(const char **failure, mode_t *mode)
{
    if (failure) *failure = "open.update_dir";
    const int fd = open(UPDATE_ROOT, O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
    if (fd < 0) return false;
    struct stat st;
    if (failure) *failure = "fstat.update_dir";
    const int stat_rc = fstat(fd, &st);
    const int saved = stat_rc ? errno : S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
    const int close_rc = close(fd);
    if (saved) { errno = saved; return false; }
    if (failure) *failure = "close.update_dir";
    if (close_rc) return false;
    if (mode) *mode = st.st_mode & 0777;
    return true;
}

enum StagedPackage {
    STAGED_NONE, STAGED_UNREADABLE, STAGED_STALE, STAGED_OWN, STAGED_OTHER, STAGED_APPLIED,
};

static enum StagedPackage staged_package(const char **failure, char app_ver[6])
{
    uint64_t size = 0;
    FILE *file = x4_update51_open_regular(UPDATE_TARGET, &size, failure);
    if (!file) return errno == ENOENT ? STAGED_NONE : STAGED_UNREADABLE;
    char target[sizeof(UPDATE_TITLE " " X4_APP_VERSION)] = {0};
    const bool exact = size == sizeof(target);
    const int rc = exact ? x4_update51_read_all(file, target, sizeof(target)) : 0;
    const int close_rc = fclose(file);
    if (rc || close_rc) {
        if (failure) *failure = rc ? "fread.marker" : "fclose.marker";
        if (rc) errno = -rc;
        return STAGED_UNREADABLE;
    }
    if (!exact || target[sizeof(target) - 1] != '\0' ||
        (strncmp(target, "XCLD00001 ", 10) && strncmp(target, "XCLD00003 ", 10))) return STAGED_STALE;
    const char *version = target + sizeof(UPDATE_TITLE);
    if (version[2] != '.' || strspn(version, "0123456789") != 2u ||
        strspn(version + 3, "0123456789") != 2u) return STAGED_STALE;
    if (strncmp(target, UPDATE_TITLE, sizeof(UPDATE_TITLE) - 1)) return STAGED_OTHER;
    if (app_ver) memcpy(app_ver, version, 6);
    if (strcmp(X4_APP_VERSION, version) >= 0) return STAGED_APPLIED;

    /* The persisted marker's write time survives reboots and works with
     * markers from T51. An unavailable or rolled-back clock cannot expire it. */
    struct stat st;
    if (failure) *failure = "stat.marker";
    if (stat(UPDATE_TARGET, &st)) return STAGED_UNREADABLE;
    if (!S_ISREG(st.st_mode)) { errno = EINVAL; return STAGED_UNREADABLE; }
    const time_t now = time(NULL);
    if (st.st_mtime > 0 && now >= st.st_mtime &&
        (uint64_t)now - (uint64_t)st.st_mtime >= UPDATE_PENDING_SECONDS) {
        /* A completed move has no staged PKG. Keep its signed notice until
         * the user installs and opens the corresponding edition. */
        if (!stat(UPDATE_PKG, &st) || errno != ENOENT ||
            stat(X4_UPDATE51_AUTH_PATH, &st) || !S_ISREG(st.st_mode)) return STAGED_STALE;
    }
    return STAGED_OWN;
}

static bool discard_staged_package(void)
{
    if (unlink(UPDATE_PKG) && errno != ENOENT) return false;
    if (unlink(X4_UPDATE51_AUTH_PATH) && errno != ENOENT) return false;
    return !unlink(UPDATE_TARGET) || errno == ENOENT;
}

/* Call under view_lock. The updater worker owns the same paths while busy. */
static bool staged_was_applied(const char *target, X4Update51ManualView *notice)
{
    X4UpdateSigned51 ticket;
    X4UpdateManifest manifest;
    if (x4_update51_auth_read(&ticket) ||
        x4_update_manifest_verify(ticket.json, ticket.json_size, ticket.signature, ticket.signature_size,
            x4_update_release_pubkey_der, sizeof(x4_update_release_pubkey_der),
            UPDATE_EDITION, UPDATE_TITLE, "0.0.0", "00.00", &manifest) ||
        strcmp(manifest.app_ver, target) ||
        !x4_update_manifest_is_applied(&manifest, X4_PRODUCT_VERSION, X4_APP_VERSION)) return false;
    *notice = (X4Update51ManualView){.version = X4_PRODUCT_VERSION};
    memcpy(notice->pkg_name, manifest.pkg_name, sizeof(notice->pkg_name));
    return true;
}

static enum StagedPackage refresh_pending(bool boot, bool *resume_choice)
{
    if (view.busy) return STAGED_UNREADABLE;
    if (!verified_dir(NULL, NULL)) {
        retry_target[0] = '\0';
        return STAGED_UNREADABLE;
    }
    char target[6] = {0};
    enum StagedPackage state = staged_package(NULL, target);
    X4Update51ManualView notice;
    const bool applied = boot && state == STAGED_APPLIED && staged_was_applied(target, &notice);
    if (state == STAGED_STALE || state == STAGED_APPLIED || (boot && state == STAGED_NONE)) {
        if (!discard_staged_package()) return STAGED_UNREADABLE;
        state = STAGED_NONE;
    }
    if (state == STAGED_NONE && (view.stage == X4_UPDATE51_ACCEPTED || view.stage == X4_UPDATE51_MANUAL)) {
        view = (X4Update51View){0};
        manual_offer = false;
    }
    if (boot || state != STAGED_OWN || strcmp(retry_target, target)) retry_target[0] = '\0';
    if (boot && state == STAGED_OWN) {
#if X4_PUBLIC
        (void)resume_choice;
        memcpy(retry_target, target, sizeof(retry_target));
        retry_local = false;
#else
        uint64_t closed_utc;
        const X4Update51Resume mode = x4_update_log51_resume(target, &retry_local, &closed_utc);
        const time_t now = time(NULL);
        *resume_choice = mode == X4_UPDATE51_RESUME_ACCEPTED && closed_utc &&
            now > 0 && (uint64_t)now >= closed_utc &&
            (uint64_t)now - closed_utc < UPDATE_RESUME_SECONDS;
        if (mode != X4_UPDATE51_RESUME_NONE) memcpy(retry_target, target, sizeof(retry_target));
#endif
    }
    if (applied) {
        view = (X4Update51View){.stage = X4_UPDATE51_UPDATED};
        manual_view = notice;
        manual_offer = true;
    }
    return state;
}

void x4_update51_boot_cleanup(void)
{
    lock_view();
    bool resume_choice = false;
    const enum StagedPackage pending = refresh_pending(true, &resume_choice);
    if (pending == STAGED_NONE) (void)unlink(UPDATE_PART);
    const bool recover = pending == STAGED_OWN && retry_target[0];
    if (recover) {
        job = (Job){.local = retry_local, .recover = true, .resume_choice = resume_choice};
        memcpy(job.detail.target_app_ver, retry_target, sizeof(retry_target));
        begin_locked(X4_UPDATE51_VERIFYING);
    }
    unlock_view();
    if (recover) launch_worker();
}

bool x4_update51_pending(void)
{
    lock_view();
    const bool pending = refresh_pending(false, NULL) == STAGED_OWN;
    unlock_view();
    return pending;
}

bool x4_update51_retry_available(bool *local)
{
    lock_view();
    const bool available = !view.busy && retry_target[0];
    if (local) *local = available && retry_local;
    unlock_view();
    return available;
}

bool x4_update51_cancel_pending(void)
{
    lock_view();
    const bool removed = refresh_pending(false, NULL) == STAGED_OWN && discard_staged_package();
    if (removed) {
        retry_target[0] = '\0';
        view = (X4Update51View){0};
        manual_offer = false;
    }
    unlock_view();
    return removed;
}

#if !X4_PUBLIC
void x4_update51_select_install(bool manual)
{
    lock_view();
    if (view.stage == X4_UPDATE51_CHOOSE && !atomic_load_explicit(&install_choice, memory_order_relaxed))
        view.manual_selected = manual;
    unlock_view();
}

void x4_update51_confirm_install(void)
{
    lock_view();
    if (view.stage == X4_UPDATE51_CHOOSE && !atomic_load_explicit(&install_choice, memory_order_relaxed))
        atomic_store_explicit(&install_choice, view.manual_selected ? INSTALL_MANUAL : INSTALL_BGFT,
            memory_order_release);
    unlock_view();
}
#endif

bool x4_update51_manual_view(X4Update51ManualView *out)
{
    lock_view();
    const bool available = out && view.stage == X4_UPDATE51_MANUAL;
    if (available) *out = manual_view;
    unlock_view();
    return available;
}

bool x4_update51_applied_view(X4Update51ManualView *out)
{
    lock_view();
    const bool available = out && view.stage == X4_UPDATE51_UPDATED;
    if (available) *out = manual_view;
    unlock_view();
    return available;
}

bool x4_update51_take_manual_offer(void)
{
    lock_view();
    const bool offered = manual_offer;
    manual_offer = false;
    unlock_view();
    return offered;
}

#if !X4_PUBLIC
static int read_local_asset(const char *name, size_t maximum, X4HttpUpdateSink sink,
    void *context, size_t *received)
{
    char path[160];
    const int n = snprintf(path, sizeof(path), "/data/pkg/%s", name);
    if (n < 0 || (size_t)n >= sizeof(path))
        return fail_at(X4_UPDATE51_READ, "read_local_asset", -EINVAL, 0);
    uint64_t size = 0;
    const char *failed = NULL;
    FILE *file = x4_update51_open_regular(path, &size, &failed);
    if (!file) return fail_errno(X4_UPDATE51_READ, failed ? failed : "read_local_asset");
    int rc = size && size <= maximum ? 0 : fail_at(X4_UPDATE51_READ, "read_local_asset", -EFBIG, 0);
    unsigned char block[4096];
    size_t count = 0;
    while (!rc && count < size) {
        if (is_cancelled()) { rc = -ECANCELED; break; }
        const size_t wanted = size - count < sizeof(block) ? (size_t)(size - count) : sizeof(block);
        rc = x4_update51_read_all(file, block, wanted);
        if (rc) { (void)fail_at(X4_UPDATE51_READ, "fread", rc, -rc); break; }
        rc = sink(context, block, wanted);
        if (!rc) count += wanted;
    }
    if (!rc) {
        rc = x4_update51_expect_eof(file);
        if (rc) (void)fail_at(X4_UPDATE51_READ, "fgetc", rc, -rc);
    }
    if (fclose(file) && !rc) rc = fail_errno(X4_UPDATE51_READ, "fclose");
    if (!rc) *received = count;
    return rc;
}
#endif

static int fetch_asset(X4Http *http, const char *name, size_t maximum,
    X4HttpUpdateSink sink, void *context, size_t *received)
{
#if !X4_PUBLIC
    if (job.local) {
        const int rc = read_local_asset(name, maximum, sink, context, received);
        return rc && rc != -ECANCELED ?
            fail_at(X4_UPDATE51_READ, "read_local_asset", rc, -rc) : rc;
    }
#endif
    const int rc = x4_http_update_asset(http, job.tag, name, maximum, sink, context, received, &cancelled);
    return rc && rc != X4_HTTP_CANCELLED ?
        fail_at(X4_UPDATE51_NETWORK, "x4_http_update_asset", rc, 0) : rc;
}

typedef struct {
    unsigned char *data;
    size_t used, capacity;
} Buffer;

static int buffer_write(void *context, const void *bytes, size_t size)
{
    Buffer *buffer = context;
    if (size > buffer->capacity - buffer->used) return -EFBIG;
    memcpy(buffer->data + buffer->used, bytes, size);
    buffer->used += size;
    return 0;
}

static int fetch_buffer(X4Http *http, const char *name, unsigned char *data, size_t maximum, size_t *size)
{
    Buffer buffer = {data, 0, maximum};
    const int rc = fetch_asset(http, name, maximum, buffer_write, &buffer, size);
    if (rc) return rc;
    return buffer.used == *size && buffer.used ? 0 :
        fail_at(X4_UPDATE51_READ, "fetch_buffer", -EIO, EIO);
}

static int file_write(void *context, const void *bytes, size_t size)
{
    if (is_cancelled()) return -ECANCELED;
    const int rc = x4_update51_write_all(context, bytes, size);
    if (rc) return fail_at(rc == -ENOSPC ? X4_UPDATE51_NO_SPACE : X4_UPDATE51_READ, "fwrite", rc, -rc);
    lock_view();
    view.received += (uint32_t)size;
    unlock_view();
    return 0;
}

static int download(X4Http *http, const X4UpdateManifest *manifest)
{
    job.detail.step = X4_UPDATE51_STEP_DOWNLOAD;
    if (mkdir("/data/xcloud4", 0777) && errno != EEXIST) return fail_errno(X4_UPDATE51_READ, "mkdir.data");
    if (mkdir(UPDATE_ROOT, 0777) && errno != EEXIST) return fail_errno(X4_UPDATE51_READ, "mkdir.update");
    const char *function = NULL;
    mode_t mode;
    if (!verified_dir(&function, &mode)) return fail_errno(X4_UPDATE51_READ, function);
    if (mode != 0777) {
        errno = 0;
        const int rc = chmod(UPDATE_ROOT, 0777);
        job.detail.chmod_update = (X4Install51FsCall){.attempted = true, .code = rc,
            .system_errno = rc ? errno : 0};
    }
    char target[6] = {0};
    const enum StagedPackage pending = staged_package(&function, target);
    if (pending == STAGED_UNREADABLE) return fail_errno(X4_UPDATE51_READ, function);
    lock_view();
    const bool retry = pending == STAGED_OWN && !strcmp(retry_target, target);
    unlock_view();
    if ((pending == STAGED_OWN && !retry) || pending == STAGED_OTHER)
        return fail_at(X4_UPDATE51_BUSY, "staged_package", -EBUSY, EBUSY);
    if (pending == STAGED_STALE || pending == STAGED_APPLIED || retry) {
        if (!discard_staged_package()) return fail_errno(X4_UPDATE51_READ, "unlink.staged");
        lock_view();
        retry_target[0] = '\0';
        unlock_view();
    }
    uint64_t free_bytes = 0;
    int native_errno = 0;
    const int space_rc = x4_install51_free_space("/data/xcloud4", &free_bytes, &native_errno, &job.detail.space);
    job.log.space_code = space_rc;
    job.log.space_errno = native_errno;
    if (!space_rc && free_bytes < (uint64_t)manifest->pkg_size + UPDATE_RESERVE)
        return fail_at(X4_UPDATE51_NO_SPACE, "x4_install51_free_space", -ENOSPC, ENOSPC);
    const int fd = open(UPDATE_PART, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
    if (fd < 0) return fail_errno(errno == ENOSPC ? X4_UPDATE51_NO_SPACE : X4_UPDATE51_READ, "open.part");
    job.part_owned = true;
    FILE *file = fdopen(fd, "wb");
    if (!file) { const int rc = fail_errno(X4_UPDATE51_READ, "fdopen.part"); (void)close(fd); return rc; }
    lock_view();
    view.total = manifest->pkg_size;
    view.stage = X4_UPDATE51_DOWNLOADING;
    unlock_view();
    checkpoint(X4_UPDATE51_DOWNLOADING, 0, NULL);
    size_t size = 0;
    int rc = fetch_asset(http, manifest->pkg_name, manifest->pkg_size, file_write, file, &size);
    if (fclose(file) && !rc)
        rc = fail_errno(errno == ENOSPC ? X4_UPDATE51_NO_SPACE : X4_UPDATE51_READ, "fclose.part");
    if (!rc && size != manifest->pkg_size)
        rc = fail_at(X4_UPDATE51_READ, "download.size", -EIO, EIO);
    return rc;
}

static int verify_package(const char *path, const X4UpdateManifest *manifest, char title[10])
{
    job.detail.step = X4_UPDATE51_STEP_VERIFY;
    uint64_t size = 0;
    const char *function = NULL;
    FILE *file = x4_update51_open_regular(path, &size, &function);
    if (!file) return fail_errno(X4_UPDATE51_READ, function ? function : "verify_package");
    errno = 0;
    int rc = size == manifest->pkg_size ? x4_update_pkg_verify_title(file, manifest, UPDATE_TITLE, title)
                                      : X4_UPDATE_VERIFY_PACKAGE;
    if (rc == X4_UPDATE_VERIFY_HASH)
        fail_at(X4_UPDATE51_BAD_HASH, "x4_update_pkg_verify.sha256", rc, 0);
    else if (rc == X4_UPDATE_VERIFY_PACKAGE)
        fail_at(X4_UPDATE51_BAD_PACKAGE, "x4_update_pkg_verify.identity", rc, 0);
    else if (rc)
        fail_at(X4_UPDATE51_READ, "x4_update_pkg_verify", rc, errno ? errno : EIO);
    if (fclose(file) && !rc) rc = fail_errno(X4_UPDATE51_READ, "fclose.verify");
    return rc;
}

#if !X4_PUBLIC
static bool install_before(const char *call, void *context)
{
    (void)context;
    (void)snprintf(job.detail.pending_call, sizeof(job.detail.pending_call), "%s", call);
    errno = 0;
    if (checkpoint((X4Update51Stage)job.log.stage, 0, NULL)) return true;
    const int error = errno ? errno : EIO;
    job.detail.pending_call[0] = '\0';
    if (!job.log.installer.accepted)
        (void)fail_at(X4_UPDATE51_INSTALL_ERROR, "update-log.pending_call", -EIO, error);
    return false;
}

static int32_t native_call_code(const char *call, const X4Install51Result *result)
{
    const struct { const char *name; int32_t code; } codes[] = {
        {"AppInstUtilInit", result->app_init_code}, {"AppInstUtilTerm", result->app_term_code},
        {"BGFTInit", result->bgft_init_code}, {"Register", result->register_code},
        {"Start", result->start_code}, {"Progress", result->progress_query_code},
        {"BGFTTerm", result->bgft_term_code},
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
        if (!strcmp(call, codes[i].name)) return codes[i].code;
    return 0;
}

static void install_observe(const X4Install51Result *result, void *context)
{
    (void)context;
    if (job.detail.pending_call[0]) {
        job.detail.last_call_rc = native_call_code(job.detail.pending_call, result);
        memcpy(job.detail.last_call, job.detail.pending_call, sizeof(job.detail.last_call));
        job.detail.pending_call[0] = '\0';
    }
    const X4Update51Stage current = result->accepted ? X4_UPDATE51_ACCEPTED : X4_UPDATE51_INSTALLING;
    job.detail.step = result->accepted ? X4_UPDATE51_STEP_ACCEPTED : X4_UPDATE51_STEP_INSTALL;
    checkpoint(current, 0, result);
    stage(current, 0, true);
}

static bool stop_monitor(void *context)
{
    (void)context;
    return atomic_load_explicit(&leaving, memory_order_acquire);
}
#endif

static int verify_signed(const X4UpdateSigned51 *ticket, X4UpdateManifest *manifest)
{
    const int rc = x4_update_manifest_verify(ticket->json, ticket->json_size, ticket->signature,
        ticket->signature_size, x4_update_release_pubkey_der, sizeof(x4_update_release_pubkey_der),
        UPDATE_EDITION, UPDATE_TITLE, X4_PRODUCT_VERSION, X4_APP_VERSION, manifest);
    if (!rc) return 0;
    const X4Update51Error error = rc == X4_UPDATE_VERIFY_SIGNATURE ? X4_UPDATE51_BAD_SIGNATURE :
        rc == X4_UPDATE_VERIFY_VERSION ? X4_UPDATE51_OLD_VERSION : X4_UPDATE51_BAD_MANIFEST;
    return fail_at(error, "x4_update_manifest_verify", rc, 0);
}

static int obtain_package(X4Http *http, X4UpdateManifest *manifest, char title[10])
{
    X4UpdateSigned51 ticket;
    int rc = fetch_buffer(http, X4_UPDATE51_MANIFEST, ticket.json, sizeof(ticket.json), &ticket.json_size);
    if (!rc) rc = fetch_buffer(http, X4_UPDATE51_SIGNATURE, ticket.signature, sizeof(ticket.signature),
        &ticket.signature_size);
    if (!rc) rc = verify_signed(&ticket, manifest);
    if (!rc) rc = download(http, manifest);
    if (!rc && is_cancelled()) rc = -ECANCELED;
    if (!rc) {
        stage(X4_UPDATE51_VERIFYING, 0, true);
        rc = verify_package(UPDATE_PART, manifest, title);
    }
    if (!rc) {
        if (rename(UPDATE_PART, UPDATE_PKG)) rc = fail_errno(X4_UPDATE51_READ, "rename.pkg");
        else { job.part_owned = false; job.pkg_owned = true; }
    }
    if (!rc) {
        rc = x4_update51_auth_save(&ticket, &job.auth_owned);
        if (rc) fail_at(rc == -ENOSPC ? X4_UPDATE51_NO_SPACE : X4_UPDATE51_READ, "auth.save", rc, -rc);
    }
    if (!rc) {
        const int fd = open(UPDATE_TARGET, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) rc = fail_errno(errno == ENOSPC ? X4_UPDATE51_NO_SPACE : X4_UPDATE51_READ, "open.marker");
        else {
            char target[sizeof(UPDATE_TITLE " " X4_APP_VERSION)] = UPDATE_TITLE " ";
            memcpy(target + sizeof(UPDATE_TITLE), manifest->app_ver, sizeof(manifest->app_ver));
            job.target_owned = true;
            FILE *file = fdopen(fd, "wb");
            if (!file) {
                rc = fail_errno(X4_UPDATE51_READ, "fdopen.marker");
                (void)close(fd);
            } else {
                rc = x4_update51_write_all(file, target, sizeof(target));
                if (rc) fail_at(rc == -ENOSPC ? X4_UPDATE51_NO_SPACE : X4_UPDATE51_READ,
                    "fwrite.marker", rc, -rc);
                if (fclose(file) && !rc)
                    rc = fail_errno(errno == ENOSPC ? X4_UPDATE51_NO_SPACE : X4_UPDATE51_READ, "fclose.marker");
            }
        }
    }
    if (!rc) memcpy(job.detail.target_app_ver, manifest->app_ver, sizeof(job.detail.target_app_ver));
    return rc;
}

static int recover_package(X4UpdateManifest *manifest, char title[10])
{
    const char *function = NULL;
    if (!verified_dir(&function, NULL)) return fail_errno(X4_UPDATE51_READ, function);
    X4UpdateSigned51 ticket;
    int rc = x4_update51_auth_read(&ticket);
    if (rc) return fail_at(X4_UPDATE51_READ, "auth.read", rc, -rc);
    rc = verify_signed(&ticket, manifest);
    if (rc) return rc;
    if (strcmp(manifest->app_ver, job.detail.target_app_ver))
        return fail_at(X4_UPDATE51_BAD_MANIFEST, "auth.target", -EINVAL, EINVAL);
    struct stat st;
    if (stat(UPDATE_PKG, &st) && errno == ENOENT) {
        /* A move may have finished before the last journal checkpoint.
         * Manual publication verifies the existing destination again. */
        job.resume_choice = false;
        memcpy(title, UPDATE_TITLE, sizeof(UPDATE_TITLE));
        return 0;
    }
    return verify_package(UPDATE_PKG, manifest, title);
}

#if !X4_PUBLIC
static enum InstallChoice choose_install(void)
{
    lock_view();
    view.stage = X4_UPDATE51_CHOOSE;
    view.manual_selected = true;
    view.resuming = job.resume_choice;
    if (job.resume_choice) {
        manual_offer = true;
        job.boot_published = true;
    }
    unlock_view();
    checkpoint(X4_UPDATE51_CHOOSE, 0, NULL);
    int choice = INSTALL_WAIT;
    while (!is_cancelled() && !(choice = atomic_load_explicit(&install_choice, memory_order_acquire)))
        sceKernelUsleep(10000);
    lock_view();
    if (is_cancelled()) choice = INSTALL_WAIT;
    if (choice != INSTALL_WAIT) view.stage = X4_UPDATE51_INSTALLING;
    unlock_view();
    return (enum InstallChoice)choice;
}
#endif

static int publish_manual(const X4UpdateManifest *manifest, const char *title,
    const X4Install51Result *result)
{
    job.detail.manual = true;
    (void)snprintf(job.detail.manual_path, sizeof(job.detail.manual_path),
        X4_UPDATE51_MANUAL_DIR "/%s", manifest->pkg_name);
    job.detail.step = X4_UPDATE51_STEP_INSTALL;
    stage(X4_UPDATE51_INSTALLING, 0, true);
    checkpoint(X4_UPDATE51_INSTALLING, 0, result);
    const int rc = x4_update51_manual_publish(UPDATE_PKG, manifest, title, job.detail.manual_path);
    if (rc) fail_at(rc == -ENOSPC ? X4_UPDATE51_NO_SPACE : X4_UPDATE51_READ, "manual.publish", rc, -rc);
    lock_view();
    manual_view = (X4Update51ManualView){.code = rc};
    memcpy(manual_view.version, manifest->version, sizeof(manual_view.version));
    memcpy(manual_view.pkg_name, manifest->pkg_name, sizeof(manual_view.pkg_name));
    memcpy(retry_target, manifest->app_ver, sizeof(retry_target));
    retry_local = job.local;
    unlock_view();
    return rc;
}

#if !X4_PUBLIC
static const char *install_failure(const X4Install51Result *result)
{
    if (result->progress_error) return "sceBgftServiceDownloadGetProgress";
    if (result->start_code) return "sceBgftServiceDownloadStartTask";
    if (result->register_code) return "sceBgftServiceIntDownloadRegisterTaskByStorageEx";
    if (result->bgft_init_code) return "sceBgftServiceInit";
    return "x4_install51_pkg";
}
#endif

static void *update_worker(void *unused)
{
    (void)unused;
    X4Install51Result result = {0};
#if !X4_PUBLIC
    if (job.diagnostic) {
        job.detail.step = X4_UPDATE51_STEP_INSTALL;
        const int rc = x4_install51_diagnose_appinst(&result, install_observe, install_before, NULL);
        if (rc) fail_at(X4_UPDATE51_INSTALL_ERROR, "x4_install51_diagnose_appinst", rc, 0);
        const X4Update51Stage final = rc ? X4_UPDATE51_FAILED : X4_UPDATE51_IDLE;
        checkpoint(final, rc, &result);
        stage(final, rc, false);
        return NULL;
    }
#endif
    X4UpdateManifest manifest;
    char title[10];
    int rc;
    if (job.recover) rc = recover_package(&manifest, title);
    else {
        job.detail.step = X4_UPDATE51_STEP_MANIFEST;
        checkpoint(X4_UPDATE51_FETCHING, 0, NULL);
        int open_error = 0;
        X4Http *http = job.local ? NULL : x4_http_update_open(&open_error);
        rc = job.local || http ? obtain_package(http, &manifest, title) :
            fail_at(X4_UPDATE51_NETWORK, "x4_http_update_open", open_error ? open_error : -EIO, 0);
        if (http) x4_http_close(http);
    }
    if (!rc && is_cancelled()) rc = -ECANCELED;
    if (!rc) {
#if !X4_PUBLIC
        enum InstallChoice choice = job.recover && !job.resume_choice ?
            INSTALL_MANUAL : choose_install();
        if (choice == INSTALL_BGFT) rc = verify_package(UPDATE_PKG, &manifest, title);
        if (rc) choice = INSTALL_WAIT;
        if (choice == INSTALL_BGFT) {
            job.detail.step = X4_UPDATE51_STEP_INSTALL;
            checkpoint(X4_UPDATE51_INSTALLING, 0, NULL);
            rc = x4_install51_pkg(UPDATE_PKG, title, manifest.pkg_size, &result,
                install_observe, stop_monitor, install_before, NULL);
            if (rc) {
                job.detail.step = X4_UPDATE51_STEP_INSTALL;
                fail_at(X4_UPDATE51_INSTALL_ERROR, install_failure(&result), rc, 0);
                job.detail.bgft_failed = true;
                choice = INSTALL_MANUAL;
            }
        }
        if (choice == INSTALL_MANUAL)
#endif
        {
            rc = publish_manual(&manifest, title, &result);
            checkpoint(X4_UPDATE51_MANUAL, rc, &result);
            stage(X4_UPDATE51_MANUAL, rc, false);
            return NULL;
        }
#if !X4_PUBLIC
        if (choice == INSTALL_WAIT && !rc) rc = -ECANCELED;
#endif
    }
    if (verified_dir(NULL, NULL)) {
        if (job.part_owned) (void)unlink(UPDATE_PART);
#if !X4_PUBLIC
        if (job.resume_choice && rc == -ECANCELED) {
            (void)discard_staged_package();
            lock_view();
            retry_target[0] = '\0';
            unlock_view();
        } else if (rc || !result.accepted)
#endif
        {
            if (job.pkg_owned) (void)unlink(UPDATE_PKG);
            if (job.auth_owned) (void)unlink(X4_UPDATE51_AUTH_PATH);
            if (job.target_owned) (void)unlink(UPDATE_TARGET);
        }
    }
#if !X4_PUBLIC
    const bool accepted = result.accepted;
#else
    const bool accepted = false;
#endif
    const X4Update51Stage final = !rc && accepted ? X4_UPDATE51_ACCEPTED :
        !accepted && (rc == -ECANCELED || rc == X4_HTTP_CANCELLED) ?
            X4_UPDATE51_CANCELLED : X4_UPDATE51_FAILED;
    if (final == X4_UPDATE51_CANCELLED) job.detail.step = X4_UPDATE51_STEP_CANCELLED;
    if (final == X4_UPDATE51_ACCEPTED) job.detail.step = X4_UPDATE51_STEP_ACCEPTED;
    if (final == X4_UPDATE51_FAILED && !job.detail.function[0])
        rc = fail_at(X4_UPDATE51_INSTALL_ERROR, "update_worker", rc ? rc : -EIO, 0);
    checkpoint(final, rc, &result);
    stage(final, rc, false);
    return NULL;
}

static bool reject_attempt(bool local, X4Update51View *rejected, X4Update51Error error,
    const char *function, int code)
{
    if (rejected) *rejected = (X4Update51View){.stage = X4_UPDATE51_FAILED, .error = error, .code = code};
    X4UpdateLog51 row = {.local = local, .stage = X4_UPDATE51_FAILED, .code = code};
    X4UpdateLog51Detail detail = {.step = X4_UPDATE51_STEP_VALIDATION, .system_errno = -code};
    (void)snprintf(detail.function, sizeof(detail.function), "%s", function);
    (void)x4_update_log51_store_detail(&row, &detail);
    return false;
}

bool x4_update51_start_checked(const char *tag, bool local, bool allowed, X4Update51View *rejected)
{
    if (rejected) *rejected = (X4Update51View){0};
    if (!allowed)
        return reject_attempt(local, rejected, X4_UPDATE51_BUSY, "main.update_guard", -EBUSY);
#if X4_PUBLIC
    if (local)
        return reject_attempt(local, rejected, X4_UPDATE51_START_ERROR, "x4_update51_start_checked.edition", -EINVAL);
#endif
    const size_t length = tag ? strnlen(tag, sizeof(job.tag)) : 0;
    if (length >= sizeof(job.tag) || (!local &&
            (!length || strspn(tag, "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-") != length ||
            !strcmp(tag, ".") || !strcmp(tag, ".."))))
        return reject_attempt(local, rejected, X4_UPDATE51_BAD_MANIFEST, "x4_update51_start_checked.tag", -EINVAL);
    lock_view();
#if !X4_PUBLIC
    if (view.stage == X4_UPDATE51_ACCEPTED) (void)refresh_pending(false, NULL);
    if (view.busy || view.stage == X4_UPDATE51_ACCEPTED) {
#else
    if (view.busy) {
#endif
        unlock_view();
        return reject_attempt(local, rejected, X4_UPDATE51_BUSY, "x4_update51_start_checked.busy", -EBUSY);
    }
    job = (Job){.local = local};
    if (length) memcpy(job.tag, tag, length + 1);
    begin_locked(X4_UPDATE51_FETCHING);
    unlock_view();
    launch_worker();
    return true;
}

static void begin_locked(X4Update51Stage initial)
{
    job.log.local = job.local;
    job.detail.diagnostic = job.diagnostic;
    job.detail.step = job.recover ? X4_UPDATE51_STEP_VERIFY : X4_UPDATE51_STEP_VALIDATION;
    view = (X4Update51View){.stage = initial, .busy = true};
    manual_view = (X4Update51ManualView){0};
    manual_offer = false;
    atomic_store_explicit(&cancelled, 0, memory_order_relaxed);
#if !X4_PUBLIC
    atomic_store_explicit(&install_choice, INSTALL_WAIT, memory_order_relaxed);
    atomic_store_explicit(&leaving, false, memory_order_relaxed);
#endif
    checkpoint(initial, 0, NULL);
}

static void launch_worker(void)
{
    OrbisPthreadAttr attr;
    OrbisPthread thread;
    const char *function = "scePthreadAttrInit";
    int rc = scePthreadAttrInit(&attr);
    if (!rc) {
        function = "scePthreadAttrSetstacksize";
        rc = scePthreadAttrSetstacksize(&attr, UPDATE_STACK_BYTES);
        if (!rc) {
            function = "scePthreadAttrSetdetachstate";
            rc = scePthreadAttrSetdetachstate(&attr, ORBIS_PTHREAD_CREATE_DETACHED);
        }
        if (!rc) {
            function = "scePthreadCreate";
            rc = scePthreadCreate(&thread, &attr, update_worker, NULL, "x4-update-job");
        }
        (void)scePthreadAttrDestroy(&attr);
    }
    if (rc) {
        job.detail.step = X4_UPDATE51_STEP_START;
        fail_at(X4_UPDATE51_START_ERROR, function, rc, 0);
        checkpoint(X4_UPDATE51_FAILED, rc, NULL);
        stage(X4_UPDATE51_FAILED, rc, false);
    }
}

#if !X4_PUBLIC
bool x4_update51_diagnose_appinst(void)
{
    lock_view();
    if (view.busy || view.stage == X4_UPDATE51_ACCEPTED || manual_offer) {
        unlock_view();
        return false;
    }
    job = (Job){.diagnostic = true};
    begin_locked(X4_UPDATE51_INSTALLING);
    unlock_view();
    launch_worker();
    return true;
}
#endif

bool x4_update51_start(const char *tag, bool local)
{
    return x4_update51_start_checked(tag, local, true, NULL);
}

void x4_update51_cancel(void)
{
    lock_view();
    if (view.stage == X4_UPDATE51_FETCHING || view.stage == X4_UPDATE51_DOWNLOADING ||
            view.stage == X4_UPDATE51_VERIFYING
#if !X4_PUBLIC
            || view.stage == X4_UPDATE51_CHOOSE
#endif
            )
        atomic_store_explicit(&cancelled, 1, memory_order_release);
    unlock_view();
}

void x4_update51_exit(void)
{
    x4_update51_cancel();
#if !X4_PUBLIC
    if (x4_update51_view().stage == X4_UPDATE51_ACCEPTED) {
        const time_t now = time(NULL);
        if (now > 0) (void)x4_update_log51_note_exit((uint64_t)now);
    }
    atomic_store_explicit(&leaving, true, memory_order_release);
#endif
}
#endif
