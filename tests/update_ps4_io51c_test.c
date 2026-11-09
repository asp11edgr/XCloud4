/* SPDX-License-Identifier: GPL-3.0-only */
#include "update_job51.h"
#include "installer51.h"
#include "update_log51.h"
#include "update_manifest.h"
#include "http_client.h"
#include "app_identity.h"

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
#include <time.h>
#include <unistd.h>

#if X4_TEST_SPACE51D
#include "json.h"
#include <stdlib.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>

typedef enum { SPACE_UNKNOWN, SPACE_ENOSPC, SPACE_NO_PROGRESS, SPACE_LOW } SpaceCase;
typedef enum { QUERY_FSTATVFS, QUERY_FSTATFS, QUERY_STATVFS, QUERY_STATFS } SpaceQuery;
static SpaceCase space_case;
static SpaceQuery queries[4];
static size_t query_count;
static unsigned part_opens, dir_chmods, update_mkdir_calls;
static mode_t part_create_mode, update_create_mode;
static int space_directory_fd = -1;
#endif

#if X4_TEST_INSTALL51E
#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>

static const char journal_path[] = "/data/xcloud4/update-log.json";
static int journal_fd = -1;
static unsigned journal_writes, journal_closes;
static bool wrote_journal_fd;
#if X4_TEST_INSTALL51F
static int manual_fd = -1;
static unsigned manual_short_writes;
#endif

ssize_t __real_write(int fd, const void *bytes, size_t count);
int __real_close(int fd);

ssize_t __wrap_write(int fd, const void *bytes, size_t count)
{
    const ssize_t rc = __real_write(fd, bytes, count);
    if (fd == journal_fd && rc > 0) {
        wrote_journal_fd = true;
        ++journal_writes;
    }
    return rc;
}

int __wrap_close(int fd)
{
#if X4_TEST_INSTALL51F
    if (fd == manual_fd) manual_fd = -1;
#endif
    if (fd >= 0 && fd == journal_fd) {
        if (wrote_journal_fd) ++journal_closes;
        journal_fd = -1;
    }
    return __real_close(fd);
}
#endif

enum { PS4_FALSE_SIZE = 39168, PS4_MAX_COUNT = 1000 };
static const char part_path[] = "/data/xcloud4/update/xcloud4-update.pkg.part";
static const char *sandbox, *source_pkg;
static char source_leaf[128];
static uint64_t package_size;
#if X4_TEST_INSTALL51F
static const char *remote_fixture_dir;
static const char remote_tag[] = "lab-0.7.100";
static unsigned remote_requests;
static bool allow_remote;
static unsigned char remote_handle;
static _Thread_local FILE *read_pkg_file;
static _Thread_local bool read_pkg_match;
#endif
static unsigned char fixture_key[128];
static size_t fixture_key_size;
static pthread_t worker;
static bool only_size;
static atomic_int part_fd = -1;
static atomic_uint false_stats, short_reads, short_writes, tail_bytes, nonblock_opens;
static atomic_uint verified_manifests, install_calls, measured_ends, rewinds;

int __real_open(const char *, int, ...);
int __real_stat(const char *, struct stat *);
int __real_fstat(int, struct stat *);
off_t __real_lseek(int, off_t, int);
size_t __real_fread(void *, size_t, size_t, FILE *);
size_t __real_fwrite(const void *, size_t, size_t, FILE *);
int __real_fclose(FILE *);
int __real_fgetc(FILE *);
int __real_mkdir(const char *, mode_t);
int __real_rename(const char *, const char *);
int __real_unlink(const char *);
int __real_chmod(const char *, mode_t);

static const char *mapped(const char *path, char out[PATH_MAX])
{
    if (strncmp(path, "/data/", 6)) return path;
    const int n = snprintf(out, PATH_MAX, "%s/%s", sandbox, path + 6);
    assert(n > 0 && n < PATH_MAX);
    return out;
}

static bool package_fd(int fd)
{
    struct stat st;
    return fd >= 0 && !__real_fstat(fd, &st) && S_ISREG(st.st_mode) &&
        st.st_size >= 0 && (uint64_t)st.st_size == package_size;
}

int __wrap_open(const char *path, int flags, ...)
{
    char out[PATH_MAX];
    int fd;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        const int mode = va_arg(args, int);
        va_end(args);
        fd = __real_open(mapped(path, out), flags, mode);
#if X4_TEST_INSTALL51F
        if (fd >= 0 && !strncmp(path, "/data/pkg/", sizeof("/data/pkg/") - 1)) {
            assert((flags & (O_EXCL | O_NOFOLLOW)) == (O_EXCL | O_NOFOLLOW) && mode == 0666);
            manual_fd = fd;
        }
#endif
#if X4_TEST_SPACE51D
        if (!strcmp(path, part_path)) part_create_mode = (mode_t)mode;
#endif
    } else {
        fd = __real_open(mapped(path, out), flags);
    }
    if (fd >= 0 && !strncmp(path, "/data/", 6) && (flags & O_NONBLOCK))
        atomic_fetch_add(&nonblock_opens, 1);
    if (fd >= 0 && !strcmp(path, part_path) && (flags & O_ACCMODE) == O_WRONLY) {
        atomic_store(&part_fd, fd);
#if X4_TEST_SPACE51D
        ++part_opens;
#endif
    }
#if X4_TEST_SPACE51D
    if (fd >= 0 && (flags & O_DIRECTORY) &&
        !strcmp(path, X4_TEST_BASE51C ? "/data/xcloud4/update" : "/data/xcloud4"))
        space_directory_fd = fd;
#endif
#if X4_TEST_INSTALL51E
    if (fd >= 0 && (flags & O_ACCMODE) == O_WRONLY &&
        !strncmp(path, journal_path, sizeof(journal_path) - 1)) {
        assert(journal_fd < 0);
        journal_fd = fd;
        wrote_journal_fd = false;
    }
#endif
    return fd;
}

int __wrap_stat(const char *path, struct stat *st)
{
    char out[PATH_MAX];
    return __real_stat(mapped(path, out), st);
}

int __wrap_fstat(int fd, struct stat *st)
{
    const int rc = __real_fstat(fd, st);
    if (!rc && S_ISREG(st->st_mode) && st->st_size >= 0 && (uint64_t)st->st_size == package_size) {
        st->st_size = PS4_FALSE_SIZE;
        atomic_fetch_add(&false_stats, 1);
    }
    return rc;
}

off_t __wrap_lseek(int fd, off_t offset, int whence)
{
    if (!offset && package_fd(fd)) {
        if (whence == SEEK_END) atomic_fetch_add(&measured_ends, 1);
        if (whence == SEEK_SET) atomic_fetch_add(&rewinds, 1);
    }
    return __real_lseek(fd, offset, whence);
}

size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file)
{
    if (!only_size && size == 1 && count > PS4_MAX_COUNT) {
#if X4_TEST_INSTALL51F
        if (read_pkg_file != file) {
            read_pkg_file = file;
            read_pkg_match = package_fd(fileno(file));
        }
        const bool is_package = read_pkg_match;
#else
        const bool is_package = package_fd(fileno(file));
#endif
        if (is_package) {
            atomic_fetch_add(&short_reads, 1);
            count = PS4_MAX_COUNT;
        }
    }
    return __real_fread(data, size, count, file);
}

size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
#if X4_TEST_SPACE51D
    if (count && fileno(file) == atomic_load(&part_fd) &&
        (space_case == SPACE_ENOSPC || space_case == SPACE_NO_PROGRESS)) {
        errno = space_case == SPACE_ENOSPC ? ENOSPC : 0;
        return 0;
    }
#endif
#if X4_TEST_INSTALL51F
    if (size == 1 && count > PS4_MAX_COUNT && fileno(file) == manual_fd) {
        ++manual_short_writes;
        count = PS4_MAX_COUNT;
    }
#endif
    if (!only_size && size == 1 && count > PS4_MAX_COUNT && fileno(file) == atomic_load(&part_fd)) {
        atomic_fetch_add(&short_writes, 1);
        count = PS4_MAX_COUNT;
    }
    return __real_fwrite(data, size, count, file);
}

int __wrap_fclose(FILE *file)
{
#if X4_TEST_INSTALL51F
    if (file == read_pkg_file) read_pkg_file = NULL;
    if (fileno(file) == manual_fd) manual_fd = -1;
#endif
#if X4_TEST_INSTALL51E
    if (fileno(file) == journal_fd) journal_fd = -1;
#endif
    if (fileno(file) == atomic_load(&part_fd)) atomic_store(&part_fd, -1);
    return __real_fclose(file);
}

int __wrap_fgetc(FILE *file)
{
    const int c = __real_fgetc(file);
    if (c != EOF && package_fd(fileno(file))) atomic_fetch_add(&tail_bytes, 1);
    return c;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    char out[PATH_MAX];
#if X4_TEST_SPACE51D
    if (!strcmp(path, "/data/xcloud4/update")) {
        ++update_mkdir_calls;
        update_create_mode = mode;
    }
#endif
    return __real_mkdir(mapped(path, out), mode);
}

int __wrap_chmod(const char *path, mode_t mode)
{
    char out[PATH_MAX];
#if X4_TEST_SPACE51D
    assert(!strcmp(path, "/data/xcloud4/update") && mode == 0777);
    ++dir_chmods;
    if (space_case == SPACE_ENOSPC) {
        errno = EPERM;
        return -1;
    }
#endif
    return __real_chmod(mapped(path, out), mode);
}

int __wrap_rename(const char *from, const char *to)
{
    char first[PATH_MAX], second[PATH_MAX];
    return __real_rename(mapped(from, first), mapped(to, second));
}

int __wrap_unlink(const char *path)
{
    char out[PATH_MAX];
    return __real_unlink(mapped(path, out));
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
#if X4_TEST_INSTALL51E || X4_TEST_INSTALL_API51E
    assert(size >= 512u * 1024u);
#else
    assert(size == 256u * 1024u);
#endif
    return pthread_attr_setstacksize(attr, 2u * 1024u * 1024u);
}
int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr *attr, int state)
{
    assert(state == ORBIS_PTHREAD_CREATE_DETACHED);
    return pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);
}
int32_t scePthreadCreate(OrbisPthread *thread, const OrbisPthreadAttr *attr,
    void *(*run)(void *), void *context, const char *name)
{
    assert(!strcmp(name, "x4-update-job"));
    const int rc = pthread_create(thread, attr, run, context);
    if (!rc) worker = *thread;
    return rc;
}

X4Http *x4_http_update_open(int *error)
{
    (void)error;
#if X4_TEST_INSTALL51F
    assert(allow_remote);
    return (X4Http *)(void *)&remote_handle;
#else
    assert(!"local update must not open HTTP");
    return NULL;
#endif
}
void x4_http_close(X4Http *http)
{
#if X4_TEST_INSTALL51F
    assert(http == (X4Http *)(void *)&remote_handle);
#else
    assert(!http);
#endif
}
int x4_http_update_asset(X4Http *http, const char *tag, const char *name, size_t maximum,
    X4HttpUpdateSink sink, void *context, size_t *received, const _Atomic int *cancel)
{
#if X4_TEST_INSTALL51F
    assert(allow_remote && http == (X4Http *)(void *)&remote_handle && !strcmp(tag, remote_tag));
    const char *const names[] = {X4_UPDATE51_MANIFEST, X4_UPDATE51_SIGNATURE, source_leaf};
    const char *const fixture[] = {"real_pkg.json", "real_pkg.sig"};
    assert(remote_requests < sizeof(names) / sizeof(names[0]) && !strcmp(name, names[remote_requests]));
    const char *path = source_pkg;
    char path_buffer[PATH_MAX];
    if (remote_requests < sizeof(fixture) / sizeof(fixture[0])) {
        const int n = snprintf(path_buffer, sizeof(path_buffer), "%s/%s", remote_fixture_dir,
            fixture[remote_requests]);
        assert(n > 0 && (size_t)n < sizeof(path_buffer));
        path = path_buffer;
    }
    ++remote_requests;
    FILE *file = fopen(path, "rb");
    assert(file && received && sink);
    *received = 0;
    unsigned char bytes[16384];
    size_t count;
    int rc = 0;
    while ((count = __real_fread(bytes, 1, sizeof(bytes), file)) != 0) {
        assert(!atomic_load(cancel) && count <= maximum - *received);
        rc = sink(context, bytes, count);
        if (rc) break;
        *received += count;
    }
    assert(!ferror(file) && !__real_fclose(file));
    return rc;
#else
    (void)http; (void)tag; (void)name; (void)maximum;
    (void)sink; (void)context; (void)received; (void)cancel;
    assert(!"local update must not request an asset");
    return -EIO;
#endif
}

X4UpdateVerifyResult __real_x4_update_manifest_verify(const void *, size_t, const unsigned char *,
    size_t, const unsigned char *, size_t, const char *, const char *, const char *, const char *,
    X4UpdateManifest *);
X4UpdateVerifyResult __wrap_x4_update_manifest_verify(const void *json, size_t length,
    const unsigned char *signature, size_t signature_size, const unsigned char *embedded, size_t embedded_size,
    const char *edition, const char *title, const char *version, const char *app_ver, X4UpdateManifest *out)
{
    assert(embedded && embedded_size && !strcmp(version, X4_PRODUCT_VERSION) && !strcmp(app_ver, X4_APP_VERSION));
    /* The real T51 trial package and its existing test signature are reused.
     * Supply the older installed identity from that fixture to both builds. */
    const X4UpdateVerifyResult rc = __real_x4_update_manifest_verify(json, length, signature, signature_size,
        fixture_key, fixture_key_size, edition, title, "0.7.99-lab", "01.67", out);
    if (!rc) {
        assert(out->pkg_size == package_size && !strcmp(out->pkg_name, source_leaf));
        atomic_fetch_add(&verified_manifests, 1);
    }
    return rc;
}

#if !X4_TEST_SPACE51D
int32_t x4_install51_free_space(const char *path, uint64_t *bytes, int *native_errno
#if X4_TEST_SPACE_API51D
    , X4Install51Space *trace
#endif
    )
{
#if X4_TEST_SPACE_API51D
    assert(!strcmp(path, "/data/xcloud4") && trace);
    *trace = (X4Install51Space){0};
#else
    assert(!strcmp(path, "/data/xcloud4/update"));
#endif
    *native_errno = 0;
    *bytes = UINT64_C(100000000);
    return 0;
}
#endif

static void verify_fixture_bytes(const char *path, uint64_t changed_byte)
{
    char actual[PATH_MAX];
    FILE *installed = fopen(mapped(path, actual), "rb");
    FILE *original = fopen(source_pkg, "rb");
    assert(installed && original);
    unsigned char a[8192], b[8192];
    uint64_t remaining = package_size;
    while (remaining) {
        const size_t count = remaining < sizeof(a) ? (size_t)remaining : sizeof(a);
        assert(__real_fread(a, 1, count, installed) == count);
        assert(__real_fread(b, 1, count, original) == count);
        const uint64_t offset = package_size - remaining;
        if (changed_byte >= offset && changed_byte - offset < count)
            b[changed_byte - offset] ^= 1;
        assert(!memcmp(a, b, count));
        remaining -= count;
    }
    assert(__real_fgetc(installed) == EOF && !ferror(installed));
    assert(!__real_fclose(installed) && !__real_fclose(original));
}

static void verify_staged_fixture(const char *path)
{
    assert(!strcmp(path, X4_INSTALL51_PKG_PATH));
#if X4_TEST_SPACE51D
    struct stat info;
    assert(!__wrap_stat(path, &info) && (info.st_mode & 0777) == 0644);
    assert(!__wrap_stat("/data/xcloud4/update", &info) && (info.st_mode & 0777) == 0777);
    assert(space_case == SPACE_UNKNOWN && !X4_TEST_BASE51C);
#endif
    verify_fixture_bytes(path, UINT64_MAX);
}

#if !X4_TEST_INSTALL51E
#if X4_TEST_SPACE51D
#define x4_install51_pkg __wrap_x4_install51_pkg
#endif

int32_t x4_install51_pkg(const char *path, const char *title, uint64_t bytes,
    X4Install51Result *result, X4Install51Observe observe, X4Install51StopMonitor stop,
#if X4_TEST_INSTALL_API51E
    X4Install51Before before,
#endif
    void *context)
{
#if X4_TEST_INSTALL_API51E
    assert(before);
#endif
    assert(!strcmp(title, "XCLD00001") && bytes == package_size && !stop(context));
    verify_staged_fixture(path);
    atomic_fetch_add(&install_calls, 1);
    *result = (X4Install51Result){.accepted = true, .task_id = 51};
    observe(result, context);
    return 0;
}

#if X4_TEST_SPACE51D
#undef x4_install51_pkg
#endif
#endif

#if X4_TEST_BASE51B
bool x4_update_log51_store(X4UpdateLog51 *row)
{
    assert(row && row->local);
    row->run = 1;
    return true;
}
#endif

static void copy_local(const char *from, const char *name)
{
    char path[PATH_MAX];
    const int n = snprintf(path, sizeof(path), "%s/pkg/%s", sandbox, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *in = fopen(from, "rb");
    FILE *out = fopen(path, "wb");
    assert(in && out);
    unsigned char data[8192];
    size_t count;
    while ((count = __real_fread(data, 1, sizeof(data), in)) != 0)
        assert(__real_fwrite(data, 1, count, out) == count);
    assert(!ferror(in) && !__real_fclose(in) && !__real_fclose(out));
}

static void configure_fixture(const char *directory, const char *fixtures, const char *pkg)
{
    sandbox = directory;
    source_pkg = pkg;
    const char *leaf = strrchr(source_pkg, '/');
    leaf = leaf ? leaf + 1 : source_pkg;
    assert(strlen(leaf) < sizeof(source_leaf));
    strcpy(source_leaf, leaf);
    struct stat st;
    assert(!__real_stat(source_pkg, &st) && S_ISREG(st.st_mode) && st.st_size > PS4_FALSE_SIZE);
    package_size = (uint64_t)st.st_size;
#if X4_TEST_INSTALL51F
    remote_fixture_dir = fixtures;
#endif

    char path[PATH_MAX];
    const int n = snprintf(path, sizeof(path), "%s/public.der", fixtures);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *public_key = fopen(path, "rb");
    assert(public_key);
    fixture_key_size = __real_fread(fixture_key, 1, sizeof(fixture_key), public_key);
    assert(fixture_key_size > 0 && fixture_key_size < sizeof(fixture_key) && !__real_fclose(public_key));
}

#if !X4_TEST_INSTALL51F
static void prepare_fixture(const char *directory, const char *fixtures, const char *pkg)
{
    configure_fixture(directory, fixtures, pkg);
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/pkg", sandbox);
    assert(n > 0 && (size_t)n < sizeof(path) && !__real_mkdir(path, 0700));
    n = snprintf(path, sizeof(path), "%s/real_pkg.json", fixtures);
    assert(n > 0 && (size_t)n < sizeof(path));
    copy_local(path, X4_UPDATE51_MANIFEST);
    n = snprintf(path, sizeof(path), "%s/real_pkg.sig", fixtures);
    assert(n > 0 && (size_t)n < sizeof(path));
    copy_local(path, X4_UPDATE51_SIGNATURE);
    copy_local(source_pkg, source_leaf);
}
#endif

static X4Update51View wait_for_update(void)
{
#if X4_TEST_INSTALL51F
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    /* This is a deadlock guard. Virtual disk writeback can stall individual
     * journal operations; the test asserts their ordering, not throughput. */
    deadline.tv_sec += 120;
    assert(!pthread_timedjoin_np(worker, NULL, &deadline));
    assert(!x4_update51_view().busy);
#else
    for (unsigned i = 0; x4_update51_view().busy; ++i) {
        assert(i < 30000);
        (void)sceKernelUsleep(1000);
    }
    assert(!pthread_join(worker, NULL));
#endif
    struct stat st;
    assert(__wrap_stat(part_path, &st) == -1 && errno == ENOENT);
    return x4_update51_view();
}

#if !X4_TEST_INSTALL51F
static X4Update51View run_update(void)
{
#if X4_TEST_INSTALL51E
    assert(x4_update51_start_checked(NULL, true, true, NULL));
#else
    assert(x4_update51_start(NULL, true));
#endif
    return wait_for_update();
}
#endif

#if X4_TEST_SPACE51D
#include "update_space51d_cases.h"
#else
int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
    only_size = argc == 5 && !strcmp(argv[4], "--only-size");
    assert(argc == 4 || (X4_TEST_BASE51B && only_size));
    prepare_fixture(argv[1], argv[2], argv[3]);
    const X4Update51View view = run_update();
    assert(atomic_load(&verified_manifests) == 1 && atomic_load(&false_stats) >= 1);
    if (X4_TEST_BASE51B) {
        assert(view.stage == X4_UPDATE51_FAILED && view.code == -EIO);
        assert(!atomic_load(&install_calls) && atomic_load(&nonblock_opens));
        assert(!atomic_load(&measured_ends));
        if (only_size) {
            assert(!atomic_load(&short_reads) && atomic_load(&tail_bytes) == 1);
        } else {
            assert(atomic_load(&short_reads) == 1 && !atomic_load(&tail_bytes));
        }
        printf("T51C_PS4_IO_BASELINE_PASS false_size=%u code=%d short_reads=%u tail_bytes=%u size_only=%u\n",
            PS4_FALSE_SIZE, view.code, atomic_load(&short_reads), atomic_load(&tail_bytes), only_size ? 1u : 0u);
    } else {
        assert(view.stage == X4_UPDATE51_ACCEPTED && view.code == 0);
        assert(atomic_load(&install_calls) == 1 && !atomic_load(&nonblock_opens));
        assert(atomic_load(&false_stats) >= 2 && atomic_load(&measured_ends) >= 3 && atomic_load(&rewinds) >= 2);
        assert(atomic_load(&short_reads) > 1 && atomic_load(&short_writes) > 1 && !atomic_load(&tail_bytes));
#if !X4_TEST_BASE51B
        X4UpdateLog51 record;
        assert(x4_update_log51_last(&record) && record.local && record.code == 0);
        assert(!strcmp(record.build, X4_PRODUCT_VERSION));
#endif
        printf("T51C_PS4_IO_PASS package_bytes=%llu false_size=%u max_count=%u "
               "short_reads=%u short_writes=%u signed_manifest=1 sha_header=1 copied_exact=1 journal=1\n",
            (unsigned long long)package_size, PS4_FALSE_SIZE, PS4_MAX_COUNT,
            atomic_load(&short_reads), atomic_load(&short_writes));
    }
    return 0;
}
#endif
