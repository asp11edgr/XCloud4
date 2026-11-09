/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/auth/http_client.h"
#include "../src/core/app_identity.h"
#include "../src/update/update_job51.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <orbis/libkernel.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static const char staged[] = "/data/xcloud4/update/xcloud4-update.pkg";
static const char marker[] = "/data/xcloud4/update/xcloud4-update.appver";
static const char partial[] = "/data/xcloud4/update/xcloud4-update.pkg.part";
static const char ticket[] = "/data/xcloud4/update/xcloud4-update.auth";
static const char tag[] = "v1.3.0-beta";
static const char *sandbox, *fixture_pkg, *fixture_manifest, *fixture_sig, *pkg_name;
static char destination[PATH_MAX];
static enum { MOVE, SAME, CONFLICT, FALLBACK, APPLIED } scenario;
static pthread_t worker;
static bool worker_started;
static unsigned requests, space_calls, short_reads, false_stats, publish_renames;
static ino_t staged_inode;
static unsigned char session;

static const char *path(const char *name, char local[PATH_MAX])
{
    if (strcmp(name, "/data") && strncmp(name, "/data/", 6)) return name;
    const int count = snprintf(local, PATH_MAX, "%s%s", sandbox, name + 5);
    assert(count > 0 && count < PATH_MAX);
    return local;
}

int __real_open(const char *, int, ...);
int __wrap_open(const char *name, int flags, ...)
{
    char local[PATH_MAX];
    if (!(flags & O_CREAT)) return __real_open(path(name, local), flags);
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);
    va_end(args);
    if (!strcmp(name, destination)) assert((flags & (O_EXCL | O_NOFOLLOW)) == (O_EXCL | O_NOFOLLOW));
    return __real_open(path(name, local), flags, mode);
}

int __real_stat(const char *, struct stat *);
int __wrap_stat(const char *name, struct stat *info)
{
    char local[PATH_MAX];
    return __real_stat(path(name, local), info);
}

int __real_lstat(const char *, struct stat *);
int __wrap_lstat(const char *name, struct stat *info)
{
    char local[PATH_MAX];
    return __real_lstat(path(name, local), info);
}

int __real_fstat(int, struct stat *);
int __wrap_fstat(int fd, struct stat *info)
{
    const int rc = __real_fstat(fd, info);
    if (!rc && S_ISREG(info->st_mode) && info->st_size > 1048576) {
        info->st_size = 39168;
        ++false_stats;
    }
    return rc;
}

int __real_mkdir(const char *, mode_t);
int __wrap_mkdir(const char *name, mode_t mode)
{
    char local[PATH_MAX];
    return __real_mkdir(path(name, local), mode);
}

int __real_chmod(const char *, mode_t);
int __wrap_chmod(const char *name, mode_t mode)
{
    char local[PATH_MAX];
    return __real_chmod(path(name, local), mode);
}

int __real_unlink(const char *);
int __wrap_unlink(const char *name)
{
    char local[PATH_MAX];
    return __real_unlink(path(name, local));
}

int __real_rename(const char *, const char *);
int __wrap_rename(const char *from, const char *to)
{
    char source[PATH_MAX], target[PATH_MAX];
    if (!strcmp(from, staged) && !strcmp(to, destination)) {
        struct stat info;
        assert(!__real_stat(path(from, source), &info));
        staged_inode = info.st_ino;
        ++publish_renames;
        if (scenario == FALLBACK) { errno = EXDEV; return -1; }
    }
    return __real_rename(path(from, source), path(to, target));
}

size_t __real_fread(void *, size_t, size_t, FILE *);
size_t __wrap_fread(void *out, size_t size, size_t count, FILE *file)
{
    if (size == 1 && count > 1000) {
        ++short_reads;
        count = 1000;
    }
    return __real_fread(out, size, count, file);
}

static int deny_space(void) { ++space_calls; errno = EPERM; return -1; }
int __wrap_fstatvfs(int fd, struct statvfs *out) { (void)fd; (void)out; return deny_space(); }
int __wrap_statvfs(const char *name, struct statvfs *out) { assert(!strcmp(name, "/data")); (void)out; return deny_space(); }
int __wrap_statfs(const char *name, struct statfs *out) { assert(!strcmp(name, "/data")); (void)out; return deny_space(); }
int _fstatfs(int fd, void *out) { (void)fd; (void)out; return deny_space(); }

int32_t sceKernelUsleep(uint32_t delay)
{
    const struct timespec span = {.tv_sec = delay / 1000000u, .tv_nsec = (delay % 1000000u) * 1000L};
    (void)nanosleep(&span, NULL);
    return 0;
}

int32_t scePthreadAttrInit(OrbisPthreadAttr *attr) { return pthread_attr_init(attr); }
int32_t scePthreadAttrDestroy(OrbisPthreadAttr *attr) { return pthread_attr_destroy(attr); }
int32_t scePthreadAttrSetstacksize(OrbisPthreadAttr *attr, size_t bytes)
{
    assert(bytes == 512u * 1024u);
    return pthread_attr_setstacksize(attr, 2u * 1024u * 1024u);
}
int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr *attr, int mode)
{
    assert(mode == ORBIS_PTHREAD_CREATE_DETACHED);
    return pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);
}
int32_t scePthreadCreate(OrbisPthread *thread, const OrbisPthreadAttr *attr,
    void *(*run)(void *), void *context, const char *name)
{
    assert(!worker_started && !strcmp(name, "x4-update-job"));
    const int rc = pthread_create(thread, attr, run, context);
    if (!rc) { worker = *thread; worker_started = true; }
    return rc;
}

X4Http *x4_http_update_open(int *error)
{
    assert(scenario != APPLIED);
    (void)error;
    return (X4Http *)(void *)&session;
}
void x4_http_close(X4Http *http) { assert(http == (X4Http *)(void *)&session); }

int x4_http_update_asset(X4Http *http, const char *release, const char *name, size_t max,
    X4HttpUpdateSink sink, void *context, size_t *received, const _Atomic int *cancel)
{
    assert(http == (X4Http *)(void *)&session && !strcmp(release, tag) && !atomic_load(cancel));
    const char *const names[] = {X4_UPDATE51_MANIFEST, X4_UPDATE51_SIGNATURE, pkg_name};
    const char *const files[] = {fixture_manifest, fixture_sig, fixture_pkg};
    assert(requests < 3 && !strcmp(name, names[requests]));
    FILE *input = fopen(files[requests++], "rb");
    assert(input);
    unsigned char buffer[8192];
    *received = 0;
    size_t count;
    int rc = 0;
    while ((count = __real_fread(buffer, 1, sizeof(buffer), input))) {
        assert(count <= max - *received);
        rc = sink(context, buffer, count);
        if (rc) break;
        *received += count;
    }
    assert(!ferror(input) && !fclose(input));
    return rc;
}

static bool exists(const char *name)
{
    struct stat info;
    return __wrap_stat(name, &info) == 0;
}

static void compare_target(bool different)
{
    char target[PATH_MAX];
    FILE *left = fopen(path(destination, target), "rb"), *right = fopen(fixture_pkg, "rb");
    assert(left && right);
    unsigned char a[8192], b[8192];
    size_t n, offset = 0;
    while ((n = __real_fread(a, 1, sizeof(a), left))) {
        assert(__real_fread(b, 1, sizeof(b), right) == n);
        if (different && offset <= 200 && 200 - offset < n) b[200 - offset] ^= 1;
        assert(!memcmp(a, b, n));
        offset += n;
    }
    assert(fgetc(right) == EOF && !ferror(left) && !ferror(right));
    assert(!fclose(left) && !fclose(right));
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    sandbox = argv[1];
    fixture_pkg = argv[2]; fixture_manifest = argv[3]; fixture_sig = argv[4];
    const char *leaf = strrchr(fixture_pkg, '/');
    assert(leaf);
    pkg_name = leaf + 1;
    const int bytes = snprintf(destination, sizeof(destination), "/data/pkg/%s", pkg_name);
    assert(bytes > 0 && (size_t)bytes < sizeof(destination));
    const char *const names[] = {"move", "same", "conflict", "fallback", "applied"};
    while (scenario < APPLIED && strcmp(argv[5], names[scenario])) ++scenario;
    assert(!strcmp(argv[5], names[scenario]));
    if (scenario == APPLIED) {
        assert(exists(marker) && exists(ticket) && !exists(staged));
        x4_update51_boot_cleanup();
        X4Update51ManualView applied = {0};
        assert(x4_update51_view().stage == X4_UPDATE51_UPDATED && x4_update51_applied_view(&applied));
        assert(!strcmp(applied.version, X4_PRODUCT_VERSION) && !strcmp(applied.pkg_name, pkg_name));
        assert(x4_update51_take_manual_offer() && !x4_update51_take_manual_offer());
        assert(!exists(marker) && !exists(ticket) && exists(destination));
        x4_update51_boot_cleanup();
        assert(!x4_update51_take_manual_offer() && !worker_started && requests == 0);
        printf("T51G_APPLIED_PASS version=%s app_ver=%s signed_name=1 one_shot=1\n", X4_PRODUCT_VERSION, X4_APP_VERSION);
        return 0;
    }
    struct stat before, after;
    const bool existing = scenario == SAME || scenario == CONFLICT;
    if (existing) assert(!__wrap_stat(destination, &before));
    else assert(!exists(destination));
    assert(x4_update51_start_checked(tag, false, true, NULL) && worker_started);
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec += 120;
    assert(!pthread_timedjoin_np(worker, NULL, &deadline));
    X4Update51ManualView manual = {0};
    assert(x4_update51_view().stage == X4_UPDATE51_MANUAL && x4_update51_manual_view(&manual));
    assert(!strcmp(manual.pkg_name, pkg_name));
    assert(manual.code == (scenario == CONFLICT ? -EEXIST : 0));
    assert(requests == 3 && space_calls == 4 && false_stats > 0 && short_reads > 1);
    assert(!exists(partial) && exists(marker) && exists(ticket));
    assert(exists(staged) == (scenario == CONFLICT));
    assert(!__wrap_stat(destination, &after));
    assert(publish_renames == (existing ? 0u : 1u));
    if (existing) assert(before.st_dev == after.st_dev && before.st_ino == after.st_ino);
    if (scenario == MOVE) assert(staged_inode == after.st_ino);
    if (scenario == FALLBACK) assert(staged_inode != after.st_ino);
    compare_target(scenario == CONFLICT);
    printf("T51G_MANUAL_PASS case=%s requests=3 eperm=4 short_io=1 bytes_verified=1\n", names[scenario]);
    return 0;
}
