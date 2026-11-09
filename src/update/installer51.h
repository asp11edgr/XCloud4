/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define X4_INSTALL51_PKG_PATH "/data/xcloud4/update/xcloud4-update.pkg"

enum {
    X4_INSTALL51_APP_INIT = 1u << 0,
    X4_INSTALL51_TITLE = 1u << 1,
    X4_INSTALL51_BGFT_INIT = 1u << 2,
    X4_INSTALL51_REGISTER = 1u << 3,
    X4_INSTALL51_START = 1u << 4,
    X4_INSTALL51_PROGRESS = 1u << 5,
    X4_INSTALL51_FALLBACK = 1u << 6,
    X4_INSTALL51_BGFT_TERM = 1u << 7,
    X4_INSTALL51_APP_TERM = 1u << 8,
};

typedef struct {
    int32_t app_init_code, title_code, bgft_init_code;
    int32_t register_code, start_code, progress_query_code, progress_error, fallback_code;
    int32_t bgft_term_code, app_term_code, task_id;
    unsigned calls;
    bool accepted, transfer_complete, progress_timeout;
} X4Install51Result;

typedef struct {
    int32_t code, system_errno, validation_errno;
    bool attempted;
} X4Install51FsCall;

typedef enum {
    X4_INSTALL51_FS_OPEN,
    X4_INSTALL51_FS_FSTATVFS,
    X4_INSTALL51_FS_FSTATFS,
    X4_INSTALL51_FS_CLOSE,
    X4_INSTALL51_FS_STATVFS,
    X4_INSTALL51_FS_STATFS,
    X4_INSTALL51_FS_COUNT
} X4Install51FsMethod;

typedef struct {
    X4Install51FsCall calls[X4_INSTALL51_FS_COUNT];
    bool unknown;
} X4Install51Space;

/* Return the first valid result, or the last query error if space is unknown.
 * The caller can still stage the package when the space is unknown. */
int32_t x4_install51_free_space(const char *path, uint64_t *bytes, int *native_errno,
    X4Install51Space *trace);

#if !X4_PUBLIC
typedef void (*X4Install51Observe)(const X4Install51Result *result, void *context);
typedef bool (*X4Install51StopMonitor)(void *context);
typedef bool (*X4Install51Before)(const char *call, void *context);

/* Runs on the sole updater worker after signature, version, size, hash and
 * package identity checks. title_id is the verified title from the package.
 * Each BGFT call requires a successful before callback and its result goes
 * to observe before the next call. Keep an accepted task's staged file. */
int32_t x4_install51_pkg(const char *verified_path, const char *title_id, uint64_t pkg_bytes,
    X4Install51Result *result, X4Install51Observe observe,
    X4Install51StopMonitor stop_monitor, X4Install51Before before, void *context);

/* Isolated diagnostic for the Lab. It never opens or installs a package. */
int32_t x4_install51_diagnose_appinst(X4Install51Result *result, X4Install51Observe observe,
    X4Install51Before before, void *context);
#endif
