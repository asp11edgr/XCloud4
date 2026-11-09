/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "installer51.h"
#include <stddef.h>

typedef enum {
    X4_UPDATE51_STEP_VALIDATION,
    X4_UPDATE51_STEP_START,
    X4_UPDATE51_STEP_MANIFEST,
    X4_UPDATE51_STEP_DOWNLOAD,
    X4_UPDATE51_STEP_VERIFY,
    X4_UPDATE51_STEP_INSTALL,
    X4_UPDATE51_STEP_ACCEPTED,
    X4_UPDATE51_STEP_CANCELLED,
    X4_UPDATE51_STEP_LEGACY,
} X4Update51Step;

typedef struct {
    uint32_t run;
    uint64_t utc;
    char build[32];
    bool local;
    int32_t stage, code, space_code, space_errno;
    X4Install51Result installer;
} X4UpdateLog51;

typedef struct {
    X4Update51Step step;
    int32_t system_errno;
    char function[80];
    X4Install51Space space;
    X4Install51FsCall chmod_update;
    char target_app_ver[6];
    char pending_call[32], last_call[32];
    int32_t last_call_rc;
    uint64_t closed_utc;
    bool manual, diagnostic, bgft_failed;
    char manual_path[160];
} X4UpdateLog51Detail;

#if !X4_PUBLIC
typedef enum {
    X4_UPDATE51_RESUME_NONE, X4_UPDATE51_RESUME_MANUAL, X4_UPDATE51_RESUME_ACCEPTED,
} X4Update51Resume;
#endif

/* A zero run appends an entry. A worker checkpoints its own run even if
 * another confirmation was rejected and added to the journal meanwhile.
 * The settings panel keeps the existing summary layout. */
bool x4_update_log51_store_detail(X4UpdateLog51 *row, const X4UpdateLog51Detail *detail);
bool x4_update_log51_store(X4UpdateLog51 *row);
bool x4_update_log51_last(X4UpdateLog51 *row);
#if !X4_PUBLIC
bool x4_update_log51_interrupted(char *call, size_t capacity, bool *local);
bool x4_update_log51_interrupted_target(const char *app_ver, bool *local);
X4Update51Resume x4_update_log51_resume(const char *app_ver, bool *local, uint64_t *closed_utc);
bool x4_update_log51_note_exit(uint64_t utc);
bool x4_update_log51_diagnostic(char *pending, size_t capacity, int32_t *code);
#endif
