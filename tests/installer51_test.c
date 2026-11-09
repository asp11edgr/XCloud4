/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/update/installer51.h"
#include "../src/update/update_log51.h"
#include "../src/auth/json.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/libkernel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>

static const char *directory;
static const char journal[] = "/data/xcloud4/update-log.json";
static const int32_t registration_error = (int32_t)UINT32_C(0x80990088);
static const int32_t progress_error = (int32_t)UINT32_C(0x80990015);
static uint64_t clock_us;
static int32_t bgft_init_rc, register_rc, start_rc, query_rc, progress_rc, term_rc;
static uint32_t statfs_version = UINT32_C(0x20030518);
static int statfs_error = EPERM;
static unsigned progress_calls, register_calls, app_init_calls, app_term_calls;
static bool accepted_seen, progress_unknown, stop_after_acceptance, reject_rename, diagnosis;

static const char *mapped(const char *path, char out[1024])
{
    if (strncmp(path, "/data/", 6)) return path;
    const int size = snprintf(out, 1024, "%s/%s", directory, path + 6);
    assert(size > 0 && size < 1024);
    return out;
}

int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...)
{
    char out[1024];
    if (!(flags & O_CREAT)) return __real_open(mapped(path, out), flags);
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);
    va_end(args);
    assert(mode == 0666 && (flags & (O_EXCL | O_NOFOLLOW)) == (O_EXCL | O_NOFOLLOW));
    return __real_open(mapped(path, out), flags, mode);
}

int __real_stat(const char *, struct stat *);
int __wrap_stat(const char *path, struct stat *info)
{
    char out[1024];
    return __real_stat(mapped(path, out), info);
}

int __real_mkdir(const char *, mode_t);
int __wrap_mkdir(const char *path, mode_t mode)
{
    char out[1024];
    return __real_mkdir(mapped(path, out), mode);
}

int __real_rename(const char *, const char *);
int __wrap_rename(const char *old, const char *path)
{
    char a[1024], b[1024];
    if (reject_rename) {
        errno = EIO;
        return -1;
    }
    return __real_rename(mapped(old, a), mapped(path, b));
}

int __real_unlink(const char *);
int __wrap_unlink(const char *path)
{
    char out[1024];
    return __real_unlink(mapped(path, out));
}

int _fstatfs(int fd, void *out)
{
    assert(fd >= 0);
    const uint64_t block_size = 4096;
    const int64_t blocks = 100000;
    memcpy(out, &statfs_version, sizeof(statfs_version));
    memcpy((unsigned char *)out + 16, &block_size, sizeof(block_size));
    memcpy((unsigned char *)out + 48, &blocks, sizeof(blocks));
    return 0;
}

int __wrap_fstatvfs(int fd, struct statvfs *out)
{
    assert(fd >= 0 && out);
    /* An SDK implementation can return success without filling its output. */
    return 0;
}

int __wrap_statvfs(const char *path, struct statvfs *out)
{
    assert(!strcmp(path, "/data") && out);
    errno = EPERM;
    return -1;
}

int __wrap_statfs(const char *path, struct statfs *out)
{
    assert(!strcmp(path, "/data") && out);
    errno = statfs_error;
    if (!statfs_error) {
        out->f_bsize = 8192;
        out->f_bavail = 25000;
    }
    /* The PS4's statfs returns the descriptor used for its internal fstatfs. */
    return 7;
}

int32_t sceAppInstUtilInitialize(void)
{
    assert(diagnosis);
    ++app_init_calls;
    return 0;
}

int32_t sceAppInstUtilTerminate(void)
{
    assert(diagnosis);
    ++app_term_calls;
    return 0;
}

int32_t sceBgftServiceInit(OrbisBgftInitParams *params)
{
    assert(params->heap && params->heapSize == 1024 * 1024);
    return bgft_init_rc;
}

int32_t sceBgftServiceTerm(void) { return term_rc; }

int32_t sceBgftServiceIntDownloadRegisterTaskByStorageEx(OrbisBgftDownloadParamEx *params,
    OrbisBgftTaskId *task)
{
    assert(!strcmp(params->params.contentUrl, X4_INSTALL51_PKG_PATH));
    assert(!strcmp(params->params.id, "XCLD00001"));
    assert(params->params.option == ORBIS_BGFT_TASK_OPT_NONE);
    assert(params->params.packageSize == 20000000);
    ++register_calls;
    *task = 37;
    return register_rc;
}

int32_t sceBgftServiceDownloadStartTask(OrbisBgftTaskId task)
{
    assert(task == 37);
    return start_rc;
}

int32_t mock_progress51(OrbisBgftTaskId task, void *out)
    __asm__("sceBgftServiceDownloadGetProgress");
int32_t mock_progress51(OrbisBgftTaskId task, void *out)
{
    assert(task == 37 && accepted_seen);
    ++progress_calls;
    const uint64_t total = progress_unknown ? 0 : UINT64_C(0x100000020);
    const uint64_t transferred = progress_calls > 1 ? total : 0;
    memcpy((unsigned char *)out + 4, &progress_rc, sizeof(progress_rc));
    memcpy((unsigned char *)out + 8, &total, sizeof(total));
    memcpy((unsigned char *)out + 16, &transferred, sizeof(transferred));
    memcpy((unsigned char *)out + 24, &total, sizeof(total));
    memcpy((unsigned char *)out + 32, &transferred, sizeof(transferred));
    return query_rc;
}

uint64_t sceKernelGetProcessTime(void) { return clock_us; }
int32_t sceKernelUsleep(uint32_t delay) { clock_us += delay; return 0; }

static void observe(const X4Install51Result *result, void *context)
{
    assert(!context);
    if (result->accepted) accepted_seen = true;
}

static bool stop(void *context)
{
    assert(!context);
    return stop_after_acceptance && accepted_seen;
}

static bool before(const char *call, void *context)
{
    assert(call && call[0] && !context);
    return true;
}

static X4Install51Result run(int32_t expected)
{
    X4Install51Result result;
    clock_us = 0;
    progress_calls = 0;
    accepted_seen = false;
    assert(x4_install51_pkg(X4_INSTALL51_PKG_PATH, "XCLD00001", 20000000,
        &result, observe, stop, before, NULL) == expected);
    assert(accepted_seen == result.accepted);
    return result;
}

static void check_ring(void)
{
    X4UpdateLog51 row, last;
    assert(!x4_update_log51_last(&last));
    for (uint32_t i = 1; i <= 24; ++i) {
        row = (X4UpdateLog51){.local = true, .stage = 3,
            .installer = {.task_id = -1, .register_code = INT32_MIN}};
        assert(x4_update_log51_store(&row) && row.run == i && row.utc && row.build[0]);
        row.stage = 5;
        row.code = registration_error;
        assert(x4_update_log51_store(&row));
    }
    assert(x4_update_log51_last(&last) && last.run == 24 && last.stage == 5 && last.local &&
        last.code == registration_error && last.installer.register_code == INT32_MIN);
    reject_rename = true;
    row = (X4UpdateLog51){.stage = 2};
    assert(!x4_update_log51_store(&row) && row.run == 0);
    reject_rename = false;
    assert(x4_update_log51_last(&last) && last.run == 24);

    char path[1024], body[16384];
    FILE *file = fopen(mapped(journal, path), "rb");
    assert(file);
    size_t bytes = fread(body, 1, sizeof(body) - 1, file);
    assert(bytes > 0 && !fclose(file));
    body[bytes] = '\0';
    assert(!strstr(body, directory) && !strstr(body, "Authorization"));
    X4JsonSpan doc, array, item, field;
    assert(!x4_json_parse(body, bytes, &doc) && x4_json_member(doc, "runs", &array) == 1);
    size_t cursor = 0;
    for (uint32_t i = 5; i <= 24; ++i) {
        uint32_t id;
        assert(x4_json_item(array, &cursor, &item) == 1 && x4_json_member(item, "run", &field) == 1 &&
            !x4_json_uint32(field, &id) && id == i);
    }
    assert(x4_json_item(array, &cursor, &item) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && argv[1][0] == '/');
    directory = argv[1];
    check_ring();

    uint64_t bytes = 0;
    int fs_errno = 0;
    X4Install51Space space;
    assert(!x4_install51_free_space("/data/xcloud4", &bytes, &fs_errno, &space) &&
        bytes == 409600000 && !fs_errno && !space.unknown);
    assert(space.calls[X4_INSTALL51_FS_FSTATVFS].attempted &&
        !space.calls[X4_INSTALL51_FS_FSTATVFS].code &&
        space.calls[X4_INSTALL51_FS_FSTATVFS].validation_errno == EINVAL);
    assert(space.calls[X4_INSTALL51_FS_FSTATFS].attempted && !space.calls[X4_INSTALL51_FS_STATVFS].attempted);
    statfs_version = 1;
    assert(x4_install51_free_space("/data/xcloud4", &bytes, &fs_errno, &space) == -EPERM &&
        !bytes && fs_errno == EPERM && space.unknown);
    assert(space.calls[X4_INSTALL51_FS_FSTATFS].validation_errno == ENOTSUP &&
        space.calls[X4_INSTALL51_FS_STATVFS].system_errno == EPERM &&
        space.calls[X4_INSTALL51_FS_STATFS].code == 7 &&
        space.calls[X4_INSTALL51_FS_STATFS].system_errno == EPERM);
    statfs_error = 0;
    assert(!x4_install51_free_space("/data/xcloud4", &bytes, &fs_errno, &space) &&
        bytes == 204800000 && !fs_errno && !space.unknown);
    assert(space.calls[X4_INSTALL51_FS_STATFS].code == 7 &&
        !space.calls[X4_INSTALL51_FS_STATFS].system_errno &&
        !space.calls[X4_INSTALL51_FS_STATFS].validation_errno);

    X4Install51Result result;
    assert(x4_install51_pkg("/data/pkg/other.pkg", "XCLD00001", 1,
        &result, observe, stop, before, NULL) == -EINVAL);
    assert(!register_calls);
    assert(x4_install51_pkg(X4_INSTALL51_PKG_PATH, "XCLD99999", 1,
        &result, observe, stop, before, NULL) == -EINVAL);
    assert(!register_calls);

    bgft_init_rc = -EIO;
    result = run(-EIO);
    assert(!result.accepted && !(result.calls & X4_INSTALL51_REGISTER));
    bgft_init_rc = 0;
    register_rc = registration_error;
    result = run(registration_error);
    assert(!result.accepted && result.register_code == registration_error &&
        result.calls == (X4_INSTALL51_BGFT_INIT | X4_INSTALL51_REGISTER | X4_INSTALL51_BGFT_TERM));
    register_rc = 0;
    start_rc = -EINVAL;
    result = run(-EINVAL);
    assert(!result.accepted && result.start_code == -EINVAL && !progress_calls);
    start_rc = 0;
    result = run(0);
    assert(result.accepted && result.transfer_complete && progress_calls == 2);
    progress_rc = progress_error;
    result = run(progress_error);
    assert(result.accepted && result.progress_error == progress_error && progress_calls == 1);
    query_rc = -EIO;
    result = run(0);
    assert(result.accepted && result.progress_query_code == -EIO && !result.progress_error &&
        result.progress_timeout && progress_calls == 600);
    query_rc = progress_rc = 0;
    progress_unknown = true;
    result = run(0);
    assert(result.accepted && result.progress_timeout && !result.transfer_complete && progress_calls == 600);
    assert(!app_init_calls && !app_term_calls);
    diagnosis = true;
    assert(!x4_install51_diagnose_appinst(&result, observe, before, NULL));
    assert(result.calls == (X4_INSTALL51_APP_INIT | X4_INSTALL51_APP_TERM));
    assert(app_init_calls == 1 && app_term_calls == 1);
    diagnosis = false;
    stop_after_acceptance = true;
    term_rc = -EIO;
    result = run(0);
    assert(result.accepted && !result.progress_timeout && !progress_calls && result.bgft_term_code == -EIO);

    puts("T51F_INSTALLER_PASS visible=1 register_error=preserved appinst_diagnosis=separate accepted_before_poll=1 "
         "query_failure=accepted unknown=accepted timeout=accepted explicit_error=failed exit=accepted "
         "journal_last20=1 journal_failed_rename=1");
}
