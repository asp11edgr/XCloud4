/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/update/update_job51.h"
#include "../src/update/installer51.h"
#include "../src/update/update_log51.h"
#include "../src/update/update_manifest.h"
#include "../src/auth/http_client.h"
#include "../src/auth/json.h"
#include "../src/core/app_identity.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <orbis/libkernel.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *sandbox;
static const char part_path[] = "/data/xcloud4/update/xcloud4-update.pkg.part";
static const char target_path[] = "/data/xcloud4/update/xcloud4-update.appver";
static const char unrelated_path[] = "/data/xcloud4/update/unrelated.pkg";
static _Atomic time_t test_now = 1700000000;
static unsigned char json[X4_UPDATE_MANIFEST_MAX], signature[X4_UPDATE_SIGNATURE_MAX], key[128], pkg[4096];
static size_t json_size, signature_size, key_size, pkg_size;
static atomic_uint installs;
static atomic_bool at_midpoint, finish_send;
static atomic_bool fail_start;
static bool fake_applied_identity;
enum Scenario { TRUNCATED, CANCELLED, WRONG_HASH, ACCEPTED, OLD_VERSION };
static _Atomic enum Scenario scenario;

time_t __wrap_time(time_t *out)
{
    const time_t now = atomic_load(&test_now);
    if (out) *out = now;
    return now;
}

static const char *mapped(const char *path, char out[PATH_MAX])
{
    assert(!strncmp(path, "/data/", 6));
    const int n = snprintf(out, PATH_MAX, "%s/%s", sandbox, path + 6);
    assert(n > 0 && n < PATH_MAX);
    return out;
}

int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    char out[PATH_MAX];
    if (!(flags & O_CREAT)) return __real_open(mapped(path, out), flags);
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);
    va_end(args);
    return __real_open(mapped(path, out), flags, mode);
}

int __real_mkdir(const char *, mode_t);
int __wrap_mkdir(const char *path, mode_t mode)
{
    char out[PATH_MAX];
    return __real_mkdir(mapped(path, out), mode);
}

int __real_chmod(const char *, mode_t);
int __wrap_chmod(const char *path, mode_t mode)
{
    char out[PATH_MAX];
    return __real_chmod(mapped(path, out), mode);
}

int __real_rename(const char *, const char *);
int __wrap_rename(const char *before, const char *after)
{
    char from[PATH_MAX], to[PATH_MAX];
    return __real_rename(mapped(before, from), mapped(after, to));
}

int __real_unlink(const char *);
int __wrap_unlink(const char *path)
{
    char out[PATH_MAX];
    return __real_unlink(mapped(path, out));
}

int __real_stat(const char *, struct stat *);
int __wrap_stat(const char *path, struct stat *st)
{
    char out[PATH_MAX];
    return __real_stat(!strncmp(path, "/data/", 6) ? mapped(path, out) : path, st);
}

static bool exists(const char *path)
{
    struct stat st;
    char out[PATH_MAX];
    if (!stat(mapped(path, out), &st)) {
        assert(S_ISREG(st.st_mode));
        return true;
    }
    assert(errno == ENOENT);
    return false;
}

int32_t sceKernelUsleep(uint32_t delay)
{
    const struct timespec pause = {.tv_sec = delay / 1000000u, .tv_nsec = (delay % 1000000u) * 1000L};
    (void)nanosleep(&pause, NULL);
    return 0;
}

int32_t scePthreadAttrInit(OrbisPthreadAttr *attr) { return pthread_attr_init(attr); }
int32_t scePthreadAttrDestroy(OrbisPthreadAttr *attr) { return pthread_attr_destroy(attr); }
int32_t scePthreadAttrSetstacksize(OrbisPthreadAttr *attr, size_t size)
{
    assert(size >= 512u * 1024u);
    return pthread_attr_setstacksize(attr, 2u * 1024u * 1024u);
}
int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr *attr, int state)
{
    assert(state == ORBIS_PTHREAD_CREATE_DETACHED);
    return pthread_attr_setdetachstate(attr, PTHREAD_CREATE_DETACHED);
}
int32_t scePthreadCreate(OrbisPthread *thread, const OrbisPthreadAttr *attr,
    void *(*worker)(void *), void *context, const char *name)
{
    assert(!strcmp(name, "x4-update-job"));
    if (atomic_load(&fail_start)) return EAGAIN;
    return pthread_create(thread, attr, worker, context);
}

struct X4Http { int fixture; };
static X4Http link_fixture;
X4Http *x4_http_update_open(int *error) { *error = 0; return &link_fixture; }
void x4_http_close(X4Http *http) { assert(http == &link_fixture); }

static int emit(X4HttpUpdateSink sink, void *context, const void *data, size_t size,
    size_t max, size_t *received)
{
    assert(size && size <= max);
    const int rc = sink(context, data, size);
    if (!rc) *received = size;
    return rc;
}

int x4_http_update_asset(X4Http *http, const char *tag, const char *name, size_t max,
    X4HttpUpdateSink sink, void *context, size_t *received, const _Atomic int *cancel)
{
    assert(http == &link_fixture && !strcmp(tag, "lab-0.7.100"));
    *received = 0;
    if (!strcmp(name, X4_UPDATE51_MANIFEST)) return emit(sink, context, json, json_size, max, received);
    if (!strcmp(name, X4_UPDATE51_SIGNATURE)) return emit(sink, context, signature, signature_size, max, received);
    assert(!strcmp(name, "valid.pkg") && max == pkg_size);
    const size_t mid = pkg_size / 2;
    assert(mid && !sink(context, pkg, mid));
    atomic_store(&at_midpoint, true);
    for (unsigned i = 0; !atomic_load(&finish_send) && !atomic_load(cancel); ++i) {
        assert(i < 5000);
        (void)sceKernelUsleep(1000);
    }
    if (atomic_load(cancel)) return X4_HTTP_CANCELLED;
    const enum Scenario mode = atomic_load(&scenario);
    if (mode == TRUNCATED) return X4_HTTP_PROTOCOL;
    assert(mode == WRONG_HASH || mode == ACCEPTED);
    if (mode == WRONG_HASH) {
        const unsigned char changed = pkg[mid] ^ 1u;
        assert(!sink(context, &changed, 1));
        assert(!sink(context, pkg + mid + 1, pkg_size - mid - 1));
    } else assert(!sink(context, pkg + mid, pkg_size - mid));
    *received = pkg_size;
    return 0;
}

X4UpdateVerifyResult __real_x4_update_manifest_verify(const void *, size_t, const unsigned char *,
    size_t, const unsigned char *, size_t, const char *, const char *, const char *, const char *,
    X4UpdateManifest *);

X4UpdateVerifyResult __wrap_x4_update_manifest_verify(const void *manifest, size_t length,
    const unsigned char *sig, size_t sig_length, const unsigned char *embedded_key, size_t embedded_size,
    const char *edition, const char *title, const char *version, const char *app_ver, X4UpdateManifest *out)
{
    assert(embedded_key && embedded_size);
    if (!strcmp(version, "0.0.0")) {
        assert(!strcmp(app_ver, "00.00"));
        const X4UpdateVerifyResult rc = __real_x4_update_manifest_verify(manifest, length, sig,
            sig_length, key, key_size, edition, title, version, app_ver, out);
        if (!rc && fake_applied_identity) {
            memcpy(out->version, X4_PRODUCT_VERSION, sizeof(X4_PRODUCT_VERSION));
            memcpy(out->app_ver, X4_APP_VERSION, sizeof(X4_APP_VERSION));
        }
        return rc;
    }
    assert(!strcmp(version, X4_PRODUCT_VERSION) && !strcmp(app_ver, X4_APP_VERSION));
    /* Reuse T51's signed synthetic fixture, comparing it to that fixture's
     * installed version. Verification uses the fixture's public key. */
    const bool old = atomic_load(&scenario) == OLD_VERSION;
    return __real_x4_update_manifest_verify(manifest, length, sig, sig_length, key, key_size,
        edition, title, old ? version : "0.7.99-lab", old ? app_ver : "01.67", out);
}

int32_t x4_install51_free_space(const char *path, uint64_t *bytes, int *native_errno,
    X4Install51Space *trace)
{
    assert(!strcmp(path, "/data/xcloud4"));
    *native_errno = 0;
    *bytes = UINT64_C(100000000);
    *trace = (X4Install51Space){0};
    return 0;
}

int32_t x4_install51_pkg(const char *path, const char *title, uint64_t size,
    X4Install51Result *result, X4Install51Observe observe, X4Install51StopMonitor stop,
    X4Install51Before before, void *context)
{
    assert(before);
    assert(atomic_load(&scenario) == ACCEPTED && size == pkg_size);
    assert(!strcmp(path, X4_INSTALL51_PKG_PATH) && !strcmp(title, "XCLD00001"));
    assert(exists(path) && exists(target_path) && !exists(part_path));
    assert(!stop(context));
    atomic_fetch_add(&installs, 1);
    *result = (X4Install51Result){.accepted = true, .task_id = 55};
    observe(result, context);
    return 0;
}

int32_t x4_install51_diagnose_appinst(X4Install51Result *result, X4Install51Observe observe,
    X4Install51Before before, void *context)
{
    (void)result;
    (void)observe;
    (void)before;
    (void)context;
    assert(!"normal updates do not run the AppInstUtil diagnosis");
    return -EIO;
}

static uint32_t last_run(void)
{
    X4UpdateLog51 row;
    return x4_update_log51_last(&row) ? row.run : 0;
}

static void assert_record(uint32_t id, X4Update51Stage stage, X4Update51Step step, int system_errno,
    const char *function)
{
    char path[PATH_MAX], body[32768];
    FILE *f = fopen(mapped("/data/xcloud4/update-log.json", path), "rb");
    assert(f);
    const size_t n = fread(body, 1, sizeof(body), f);
    assert(n && n < sizeof(body) && !ferror(f) && !fclose(f));
    X4JsonSpan root, array, row, field;
    assert(!x4_json_parse(body, n, &root) && x4_json_member(root, "runs", &array) == 1);
    size_t cursor = 0;
    while (x4_json_item(array, &cursor, &row) == 1) {
        uint32_t run, value;
        assert(x4_json_member(row, "run", &field) == 1 && !x4_json_uint32(field, &run));
        if (run != id) continue;
        assert(x4_json_member(row, "stage", &field) == 1 && !x4_json_uint32(field, &value) && value == stage);
        assert(x4_json_member(row, "step", &field) == 1 && !x4_json_uint32(field, &value) && value == step);
        assert(x4_json_member(row, "errno", &field) == 1 && !x4_json_uint32(field, &value) &&
            value == (uint32_t)system_errno);
        char text[80];
        assert(x4_json_member(row, "function", &field) == 1 && !x4_json_token(field, text, sizeof(text)) &&
            !strcmp(text, function));
        assert(x4_json_member(row, "app_ver", &field) == 1 && !x4_json_token(field, text, sizeof(text)) &&
            !strcmp(text, X4_APP_VERSION));
        assert(x4_json_member(row, "stage_name", &field) == 1 && !x4_json_token(field, text, sizeof(text)) && text[0]);
        return;
    }
    assert(!"missing expected run in journal");
}

static size_t read_fixture(const char *directory, const char *name, unsigned char *bytes, size_t cap)
{
    char path[PATH_MAX];
    const int n = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(n > 0 && n < PATH_MAX);
    FILE *f = fopen(path, "rb");
    assert(f);
    const size_t size = fread(bytes, 1, cap, f);
    assert(size > 0 && size < cap && !ferror(f) && !fclose(f));
    return size;
}

static void run_case(enum Scenario test, X4Update51Stage stage, int code, X4Update51Error error)
{
    const uint32_t initial_run = last_run();
    const unsigned installs_before = atomic_load(&installs);
    atomic_store(&at_midpoint, false);
    atomic_store(&finish_send, false);
    atomic_store(&scenario, test);
    assert(x4_update51_start("lab-0.7.100", false));
    for (unsigned i = 0; !atomic_load(&at_midpoint); ++i) {
        assert(i < 5000);
        assert(x4_update51_view().busy);
        (void)sceKernelUsleep(1000);
    }
    const X4Update51View mid = x4_update51_view();
    assert(mid.busy && mid.stage == X4_UPDATE51_DOWNLOADING);
    assert(mid.received == pkg_size / 2 && mid.total == pkg_size && exists(part_path));
    assert(!x4_update51_pending() && !x4_update51_cancel_pending() && exists(part_path));
    X4Update51View rejected;
    assert(!x4_update51_start_checked("lab-0.7.101", false, true, &rejected));
    assert(rejected.stage == X4_UPDATE51_FAILED && rejected.error == X4_UPDATE51_BUSY &&
        rejected.code == -EBUSY && x4_update51_view().busy);
    if (test == CANCELLED) x4_update51_cancel();
    else atomic_store(&finish_send, true);
    X4Update51View end;
    bool confirmed = false;
    for (unsigned i = 0; ; ++i) {
        assert(i < 5000);
        end = x4_update51_view();
        if (!end.busy) break;
        if (end.stage == X4_UPDATE51_CHOOSE && !confirmed) {
            x4_update51_select_install(false);
            x4_update51_confirm_install();
            confirmed = true;
        }
        (void)sceKernelUsleep(1000);
    }
    assert(end.stage == stage && end.code == code && end.error == error);
    assert(!exists(part_path));
    assert(exists(X4_INSTALL51_PKG_PATH) == (test == ACCEPTED));
    assert(exists(target_path) == (test == ACCEPTED));
    assert(atomic_load(&installs) == installs_before + (test == ACCEPTED));
    assert(last_run() == initial_run + 2);
    assert_record(initial_run + 2, X4_UPDATE51_FAILED, X4_UPDATE51_STEP_VALIDATION, EBUSY,
        "x4_update51_start_checked.busy");
    const X4Update51Step step = test == ACCEPTED ? X4_UPDATE51_STEP_ACCEPTED :
        test == CANCELLED ? X4_UPDATE51_STEP_CANCELLED :
        test == WRONG_HASH ? X4_UPDATE51_STEP_VERIFY : X4_UPDATE51_STEP_DOWNLOAD;
    const char *function = test == WRONG_HASH ? "x4_update_pkg_verify.sha256" :
        test == TRUNCATED ? "x4_http_update_asset" : "";
    assert_record(initial_run + 1, stage, step, 0, function);
}

static void put_file(const char *path, const char *content, size_t length, int flags)
{
    const int fd = __wrap_open(path, O_WRONLY | flags, 0600);
    assert(fd >= 0 && write(fd, content, length) == (ssize_t)length);
    assert(!close(fd));
}

static void set_marker_age(time_t age)
{
    char path[PATH_MAX];
    const time_t modified = atomic_load(&test_now) - age;
    const struct timespec times[] = {{.tv_sec = modified}, {.tv_sec = modified}};
    assert(!utimensat(AT_FDCWD, mapped(target_path, path), times, 0));
}

static void simulate_newer_pending(time_t age)
{
    /* The already-signed T51 package covers the real verifier and installer
       handoff. Model the retained marker of a newer build for the TTL tests. */
    const char marker[] = "XCLD00001 99.99";
    assert(strcmp(X4_APP_VERSION, "99.99") < 0);
    put_file(target_path, marker, sizeof(marker), O_TRUNC);
    set_marker_age(age);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    sandbox = argv[1];
    json_size = read_fixture(argv[2], "valid.json", json, sizeof(json));
    signature_size = read_fixture(argv[2], "valid.sig", signature, sizeof(signature));
    key_size = read_fixture(argv[2], "public.der", key, sizeof(key));
    pkg_size = read_fixture(argv[2], "valid.pkg", pkg, sizeof(pkg));
    X4Update51View rejected;
    assert(!x4_update51_start_checked("invalid/tag", false, true, &rejected));
    assert(rejected.error == X4_UPDATE51_BAD_MANIFEST && last_run() == 1);
    assert_record(1, X4_UPDATE51_FAILED, X4_UPDATE51_STEP_VALIDATION, EINVAL,
        "x4_update51_start_checked.tag");
    assert(!x4_update51_start_checked("lab-0.7.100", false, false, &rejected));
    assert(rejected.error == X4_UPDATE51_BUSY && last_run() == 2);
    assert_record(2, X4_UPDATE51_FAILED, X4_UPDATE51_STEP_VALIDATION, EBUSY, "main.update_guard");
    atomic_store(&fail_start, true);
    assert(x4_update51_start("lab-0.7.100", false));
    atomic_store(&fail_start, false);
    assert(x4_update51_view().error == X4_UPDATE51_START_ERROR && last_run() == 3);
    assert_record(3, X4_UPDATE51_FAILED, X4_UPDATE51_STEP_START, 0, "scePthreadCreate");
    atomic_store(&scenario, OLD_VERSION);
    assert(x4_update51_start("lab-0.7.100", false));
    for (unsigned i = 0; x4_update51_view().busy; ++i) {
        assert(i < 5000);
        (void)sceKernelUsleep(1000);
    }
    assert(x4_update51_view().error == X4_UPDATE51_OLD_VERSION && last_run() == 4);
    assert_record(4, X4_UPDATE51_FAILED, X4_UPDATE51_STEP_MANIFEST, 0, "x4_update_manifest_verify");
    run_case(TRUNCATED, X4_UPDATE51_FAILED, X4_HTTP_PROTOCOL, X4_UPDATE51_NETWORK);
    run_case(CANCELLED, X4_UPDATE51_CANCELLED, X4_HTTP_CANCELLED, X4_UPDATE51_GENERAL);
    run_case(WRONG_HASH, X4_UPDATE51_FAILED, X4_UPDATE_VERIFY_HASH, X4_UPDATE51_BAD_HASH);
    run_case(ACCEPTED, X4_UPDATE51_ACCEPTED, 0, X4_UPDATE51_GENERAL);
    simulate_newer_pending(24 * 60 * 60 - 1);
    assert(!x4_update51_start("lab-0.7.101", false));
    x4_update51_boot_cleanup();
    assert(x4_update51_pending() && exists(X4_INSTALL51_PKG_PATH) && exists(target_path));
    put_file(unrelated_path, "untouched", 9, O_CREAT | O_EXCL);
    put_file(part_path, "untouched", 9, O_CREAT | O_EXCL);
    assert(x4_update51_cancel_pending());
    assert(!x4_update51_pending() && !x4_update51_cancel_pending());
    assert(x4_update51_view().stage == X4_UPDATE51_IDLE);
    assert(!exists(X4_INSTALL51_PKG_PATH) && !exists(target_path));
    assert(exists(unrelated_path) && exists(part_path));
    assert(!__wrap_unlink(part_path));

    run_case(ACCEPTED, X4_UPDATE51_ACCEPTED, 0, X4_UPDATE51_GENERAL);
    simulate_newer_pending(-1); /* A clock rollback cannot expire a marker. */
    assert(x4_update51_pending());
    set_marker_age(24 * 60 * 60);
    assert(!x4_update51_pending());
    assert(x4_update51_view().stage == X4_UPDATE51_IDLE);
    assert(!exists(X4_INSTALL51_PKG_PATH) && !exists(target_path) && exists(unrelated_path));

    run_case(ACCEPTED, X4_UPDATE51_ACCEPTED, 0, X4_UPDATE51_GENERAL);
    const char foreign_marker[] = "XCLD00003 01.42";
    put_file(target_path, foreign_marker, sizeof(foreign_marker), O_TRUNC);
    set_marker_age(24 * 60 * 60 - 1);
    x4_update51_boot_cleanup();
    assert(!x4_update51_pending() && !x4_update51_cancel_pending());
    assert(exists(X4_INSTALL51_PKG_PATH) && exists(target_path));
    set_marker_age(24 * 60 * 60);
    x4_update51_boot_cleanup();
    assert(!exists(X4_INSTALL51_PKG_PATH) && !exists(target_path));

    run_case(ACCEPTED, X4_UPDATE51_ACCEPTED, 0, X4_UPDATE51_GENERAL);
    const char local_marker[] = "XCLD00001 " X4_APP_VERSION;
    put_file(target_path, local_marker, sizeof(local_marker), O_TRUNC);
    x4_update51_boot_cleanup();
    assert(!exists(X4_INSTALL51_PKG_PATH) && !exists(target_path) && !exists(part_path));
    assert(!x4_update51_take_manual_offer());
    run_case(ACCEPTED, X4_UPDATE51_ACCEPTED, 0, X4_UPDATE51_GENERAL);
    put_file(target_path, local_marker, sizeof(local_marker), O_TRUNC);
    fake_applied_identity = true;
    x4_update51_boot_cleanup();
    assert(x4_update51_view().stage == X4_UPDATE51_UPDATED);
    assert(x4_update51_take_manual_offer() && !x4_update51_take_manual_offer());
    assert(!exists(X4_INSTALL51_PKG_PATH) && !exists(target_path));
    x4_update51_boot_cleanup();
    assert(!x4_update51_take_manual_offer());
    assert(exists(unrelated_path));
    puts("UPDATE51C_JOURNAL_PASS invalid=1 blocked=1 start_error=1 old_version=1 every_confirmation=1"
         " earlier_checkpoint=1 clean_followup=1 truncation_cleanup=1 cancel_cleanup=1 sha_failure_cleanup=1"
         " verified_install=1 pending_86399s=1 expires_86400s=1 pending_cancel=1 rollback_kept=1"
         " busy_safe=1 foreign_safe=1 foreign_expiry=1 finished_boot=1 updated_once=1 unrelated_kept=1");
    return 0;
}
