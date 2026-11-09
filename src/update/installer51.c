/* SPDX-License-Identifier: GPL-3.0-only */
#include "installer51.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>
#if defined(__FreeBSD__) && !defined(X4_OPENORBIS)
#include <sys/mount.h>
#else
#include <sys/statfs.h>
#endif

#if defined(X4_OPENORBIS)
extern int _fstatfs(int fd, void *out);
#endif

#if defined(X4_OPENORBIS) && !X4_PUBLIC
#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/libkernel.h>

/* The installed stubs export these direct imports. The native progress ABI
 * has 64-bit byte counters, unlike the installed OpenOrbis declaration. */
extern int32_t sceBgftServiceInit(OrbisBgftInitParams *params);
extern int32_t sceBgftServiceTerm(void);
typedef struct {
    uint32_t bits;
    int32_t error;
    uint64_t total, transferred, total_all, transferred_all;
    uint32_t indexes_and_eta[4];
    int32_t percentages[2];
} X4BgftProgress51;

_Static_assert(sizeof(OrbisBgftTaskProgress) == 48 &&
    offsetof(OrbisBgftTaskProgress, errorResult) == 4 &&
    sizeof(X4BgftProgress51) == 64 && offsetof(X4BgftProgress51, error) == 4 &&
    offsetof(X4BgftProgress51, total) == 8 && offsetof(X4BgftProgress51, total_all) == 24 &&
    offsetof(X4BgftProgress51, indexes_and_eta) == 40 &&
    offsetof(X4BgftProgress51, percentages) == 56, "unexpected BGFT progress ABI");
extern int32_t x4_bgft_progress51(OrbisBgftTaskId task_id, X4BgftProgress51 *progress)
    __asm__("sceBgftServiceDownloadGetProgress");
_Static_assert(sizeof(OrbisBgftInitParams) == 16 && offsetof(OrbisBgftInitParams, heap) == 0 &&
    offsetof(OrbisBgftInitParams, heapSize) == 8, "unexpected BGFT init ABI");
_Static_assert(sizeof(OrbisBgftDownloadParam) == 104 && sizeof(OrbisBgftDownloadParamEx) == 112 &&
    sizeof(OrbisBgftTaskId) == 4 && offsetof(OrbisBgftDownloadParam, contentUrl) == 16 &&
    offsetof(OrbisBgftDownloadParam, contentName) == 32 &&
    offsetof(OrbisBgftDownloadParam, iconPath) == 40 && offsetof(OrbisBgftDownloadParam, option) == 56 &&
    offsetof(OrbisBgftDownloadParam, packageType) == 80 &&
    offsetof(OrbisBgftDownloadParam, packageSize) == 96 &&
    offsetof(OrbisBgftDownloadParamEx, params) == 0 &&
    offsetof(OrbisBgftDownloadParamEx, slot) == 104, "unexpected BGFT registration ABI");
#endif

#if defined(X4_OPENORBIS)
/* PS4's FreeBSD 9.x/12.x statfs layouts share this prefix. Reserve space for
 * the entire kernel structure and reject an unrecognized version. */
typedef struct {
    uint32_t version;
    unsigned char header_tail[12];
    uint64_t block_bytes;
    unsigned char other_counts[24];
    int64_t available_blocks;
    unsigned char structure_tail[4096 - 56];
} X4StatFs51;

_Static_assert(sizeof(X4StatFs51) == 4096 && offsetof(X4StatFs51, available_blocks) == 48,
    "unexpected statfs ABI");
#if !defined(__linux__)
_Static_assert(offsetof(struct statfs, f_version) == 0 &&
    offsetof(struct statfs, f_bsize) == 16 && offsetof(struct statfs, f_bavail) == 48 &&
    sizeof(struct statfs) <= sizeof(X4StatFs51), "unexpected SDK statfs ABI");
#endif

static bool fs_version51(uint32_t version)
{
    return version == UINT32_C(0x20030518) || version == UINT32_C(0x20140518);
}
#endif

#if defined(X4_OPENORBIS) && !X4_PUBLIC
enum { BGFT_HEAP_BYTES51 = 1024 * 1024, BGFT_HEAP_ALIGN51 = 16 };
static void *bgft_storage51;
#endif

static int32_t space_result51(X4Install51FsCall *call, X4Install51FsCall observed,
    int32_t result, uint64_t block_bytes, uint64_t blocks, uint64_t *bytes)
{
    observed.attempted = true;
    const int error = observed.system_errno;
    if (!result && !error && !observed.validation_errno) {
        if (!block_bytes) observed.validation_errno = EINVAL;
        else if (blocks > UINT64_MAX / block_bytes) observed.validation_errno = EOVERFLOW;
        else *bytes = block_bytes * blocks;
    }
    *call = observed;
    if (result == -1) return -(error ? error : EIO);
    if (result) return result;
    return -(error ? error : observed.validation_errno);
}

static int32_t query_vfs51(int fd, X4Install51FsCall *call, uint64_t *bytes)
{
    /* The SDK's wrappers can return success without filling this structure. */
    struct statvfs space = {0};
    errno = 0;
    const int rc = fd >= 0 ? fstatvfs(fd, &space) : statvfs("/data", &space);
    const X4Install51FsCall observed = {.code = rc, .system_errno = errno};
    const uint64_t block_bytes = space.f_frsize ? space.f_frsize : space.f_bsize;
    return space_result51(call, observed, rc, block_bytes, space.f_bavail, bytes);
}

static int32_t query_fs51(int fd, X4Install51FsCall *call, uint64_t *bytes)
{
#if defined(X4_OPENORBIS)
    if (fd >= 0) {
        X4StatFs51 space = {0};
        errno = 0;
        const int rc = _fstatfs(fd, &space);
        X4Install51FsCall observed = {.code = rc, .system_errno = errno};
        if (!rc && !observed.system_errno && !fs_version51(space.version))
            observed.validation_errno = ENOTSUP;
        const uint64_t blocks = space.available_blocks > 0 ? (uint64_t)space.available_blocks : 0;
        return space_result51(call, observed, rc, space.block_bytes, blocks, bytes);
    }
#endif
    struct statfs space = {0};
    errno = 0;
#if defined(X4_OPENORBIS)
    const int rc = statfs("/data", &space);
#else
    const int rc = fd >= 0 ? fstatfs(fd, &space) : statfs("/data", &space);
#endif
    X4Install51FsCall observed = {.code = rc, .system_errno = errno};
#if defined(X4_OPENORBIS)
    /* OpenOrbis musl returns the descriptor, keeping errno from fstatfs. */
    const int result = rc < 0 ? rc : 0;
#if !defined(__linux__)
    if (!result && !observed.system_errno && !fs_version51(space.f_version))
        observed.validation_errno = ENOTSUP;
#endif
#else
    const int result = rc;
#endif
    const uint64_t block_bytes = space.f_bsize > 0 ? (uint64_t)space.f_bsize : 0;
    const uint64_t blocks = space.f_bavail > 0 ? (uint64_t)space.f_bavail : 0;
    return space_result51(call, observed, result, block_bytes, blocks, bytes);
}

int32_t x4_install51_free_space(const char *path, uint64_t *bytes, int *native_errno,
    X4Install51Space *trace)
{
    if (native_errno) *native_errno = 0;
    if (bytes) *bytes = 0;
    if (trace) *trace = (X4Install51Space){0};
    if (!path || !bytes || !trace) return -EINVAL;
    X4Install51FsCall *calls = trace->calls;
    errno = 0;
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
    calls[X4_INSTALL51_FS_OPEN] = (X4Install51FsCall){
        .code = fd < 0 ? fd : 0, .system_errno = fd < 0 ? errno : 0, .attempted = true
    };
    if (fd >= 0) {
        int32_t rc = query_vfs51(fd, &calls[X4_INSTALL51_FS_FSTATVFS], bytes);
        if (rc) rc = query_fs51(fd, &calls[X4_INSTALL51_FS_FSTATFS], bytes);
        errno = 0;
        const int close_rc = close(fd);
        calls[X4_INSTALL51_FS_CLOSE] = (X4Install51FsCall){
            .code = close_rc, .system_errno = close_rc ? errno : 0, .attempted = true
        };
        if (!rc) return 0;
    }
    int32_t rc = query_vfs51(-1, &calls[X4_INSTALL51_FS_STATVFS], bytes);
    if (rc) rc = query_fs51(-1, &calls[X4_INSTALL51_FS_STATFS], bytes);
    trace->unknown = rc != 0;
    if (rc && native_errno) *native_errno = calls[X4_INSTALL51_FS_STATFS].system_errno;
    return rc;
}

#if defined(X4_OPENORBIS) && !X4_PUBLIC
typedef struct {
    X4Install51Observe observe;
    X4Install51StopMonitor stop_monitor;
    X4Install51Before before;
    void *context;
} Install51Callbacks;

static bool install_before51(const Install51Callbacks *callbacks, const char *call)
{
    return callbacks->before(call, callbacks->context);
}

static void install_observe51(X4Install51Result *result, unsigned call,
    const Install51Callbacks *callbacks)
{
    result->calls |= call;
    callbacks->observe(result, callbacks->context);
}

static int32_t monitor_bgft51(X4Install51Result *result, const Install51Callbacks *callbacks)
{
    const uint64_t start = sceKernelGetProcessTime();
    uint64_t next = start;
    while (!callbacks->stop_monitor || !callbacks->stop_monitor(callbacks->context)) {
        const uint64_t now = sceKernelGetProcessTime();
        if (now - start >= UINT64_C(600000000)) {
            result->progress_timeout = true;
            install_observe51(result, 0, callbacks);
            break;
        }
        if (now < next) {
            const uint64_t wait = next - now;
            (void)sceKernelUsleep((uint32_t)(wait < 100000 ? wait : 100000));
            continue;
        }
        X4BgftProgress51 progress = {0};
        if (!install_before51(callbacks, "Progress")) break;
        result->progress_query_code = x4_bgft_progress51(result->task_id, &progress);
        if (!result->progress_query_code) {
            result->progress_error = progress.error;
            const uint64_t total = progress.total_all ? progress.total_all : progress.total;
            const uint64_t done = progress.total_all ? progress.transferred_all : progress.transferred;
            result->transfer_complete = !progress.error && total && done >= total;
        }
        install_observe51(result, X4_INSTALL51_PROGRESS, callbacks);
        if (result->progress_error) return result->progress_error;
        if (result->transfer_complete) break;
        next = now + 1000000;
    }
    return 0;
}

static void finish_bgft51(X4Install51Result *result, const Install51Callbacks *callbacks)
{
    if (!install_before51(callbacks, "BGFTTerm")) return;
    result->bgft_term_code = sceBgftServiceTerm();
    install_observe51(result, X4_INSTALL51_BGFT_TERM, callbacks);
    if (!result->bgft_term_code) {
        free(bgft_storage51);
        bgft_storage51 = NULL;
    }
}

static int32_t install_bgft51(const char *path, const char *title_id, uint64_t bytes,
    X4Install51Result *result, const Install51Callbacks *callbacks)
{
    void *storage = malloc(BGFT_HEAP_BYTES51 + BGFT_HEAP_ALIGN51 - 1);
    if (!storage) return -ENOMEM;
    const uintptr_t address = (uintptr_t)storage + BGFT_HEAP_ALIGN51 - 1;
    void *heap = (void *)(address & ~(uintptr_t)(BGFT_HEAP_ALIGN51 - 1));
    memset(heap, 0, BGFT_HEAP_BYTES51);
    OrbisBgftInitParams init = {.heap = heap, .heapSize = BGFT_HEAP_BYTES51};
    if (!install_before51(callbacks, "BGFTInit")) {
        free(storage);
        return -EIO;
    }
    result->bgft_init_code = sceBgftServiceInit(&init);
    if (!result->bgft_init_code) bgft_storage51 = storage;
    install_observe51(result, X4_INSTALL51_BGFT_INIT, callbacks);
    if (result->bgft_init_code) {
        free(storage);
        return result->bgft_init_code;
    }

    OrbisBgftDownloadParamEx package;
    memset(&package, 0, sizeof(package));
    package.params.entitlementType = 5;
    package.params.id = title_id;
    package.params.contentUrl = path;
    package.params.contentName = "XCloud4 Lab";
    package.params.iconPath = "";
    package.params.playgoScenarioId = "0";
    package.params.option = ORBIS_BGFT_TASK_OPT_NONE;
    package.params.packageSize = (uint32_t)bytes;
    OrbisBgftTaskId id = -1;
    if (!install_before51(callbacks, "Register")) {
        finish_bgft51(result, callbacks);
        return -EIO;
    }
    result->register_code = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&package, &id);
    result->task_id = id;
    install_observe51(result, X4_INSTALL51_REGISTER, callbacks);
    int32_t rc = result->register_code ? result->register_code : -EIO;
    if (!result->register_code && id >= 0 && install_before51(callbacks, "Start")) {
        result->start_code = sceBgftServiceDownloadStartTask(id);
        result->accepted = result->start_code == 0;
        install_observe51(result, X4_INSTALL51_START, callbacks);
        rc = result->accepted ? monitor_bgft51(result, callbacks) : result->start_code;
    }
    finish_bgft51(result, callbacks);
    return rc;
}
#endif

#if !X4_PUBLIC
int32_t x4_install51_pkg(const char *verified_path, const char *title_id, uint64_t pkg_bytes,
    X4Install51Result *result, X4Install51Observe observe,
    X4Install51StopMonitor stop_monitor, X4Install51Before before, void *context)
{
    if (!result) return -EINVAL;
    *result = (X4Install51Result){.task_id = -1};
    if (!verified_path || strcmp(verified_path, X4_INSTALL51_PKG_PATH) ||
        !title_id || (strcmp(title_id, "XCLD00001") && strcmp(title_id, "XCLD00003")) ||
        !pkg_bytes || pkg_bytes > UINT64_C(67108864) || !observe || !before) return -EINVAL;
#if defined(X4_OPENORBIS)
    if (bgft_storage51) return -EBUSY;
    const Install51Callbacks callbacks = {
        .observe = observe, .stop_monitor = stop_monitor, .before = before, .context = context,
    };
    return install_bgft51(verified_path, title_id, pkg_bytes, result, &callbacks);
#else
    (void)stop_monitor;
    (void)context;
    return -ENOTSUP;
#endif
}

int32_t x4_install51_diagnose_appinst(X4Install51Result *result, X4Install51Observe observe,
    X4Install51Before before, void *context)
{
    if (!result) return -EINVAL;
    *result = (X4Install51Result){.task_id = -1};
    if (!observe || !before) return -EINVAL;
#if defined(X4_OPENORBIS)
    static bool initialized;
    if (initialized || bgft_storage51) return -EBUSY;
    const Install51Callbacks callbacks = {.observe = observe, .before = before, .context = context};
    if (!install_before51(&callbacks, "AppInstUtilInit")) return -EIO;
    result->app_init_code = sceAppInstUtilInitialize();
    if (!result->app_init_code) initialized = true;
    install_observe51(result, X4_INSTALL51_APP_INIT, &callbacks);
    if (result->app_init_code) return result->app_init_code;

    if (!install_before51(&callbacks, "AppInstUtilTerm")) return -EIO;
    result->app_term_code = sceAppInstUtilTerminate();
    install_observe51(result, X4_INSTALL51_APP_TERM, &callbacks);
    if (!result->app_term_code) initialized = false;
    return result->app_term_code;
#else
    (void)context;
    return -ENOTSUP;
#endif
}
#endif
