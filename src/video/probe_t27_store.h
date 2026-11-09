/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t27_state.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
#include <stdio.h>
#if defined(X4_T27_STORE_TESTING) && defined(X4_OPENORBIS)
#error Host-configurable store roots must never enter a PS4 build
#endif

#define X4_T27_STORE_ROOT "/data/xcloud4-probe/t27"
enum { X4_T27_STORE_PATH_MAX = 480, X4_T27_STORE_PROGRESS_MAX = 2048,
    X4_T27_STORE_CELL_MAX = 96 * 1024, X4_T27_STORE_SUMMARY_MAX = 2 * 1024 * 1024 };
_Static_assert(X4_T27_STORE_CELL_MAX>=X4_T26_RESULT_TEXT_MAX+16*1024,
    "T27 cell reserves native JSON plus bounded presentation wrapper");
_Static_assert(X4_T27_STORE_SUMMARY_MAX>=X4_T27_CELL_COUNT*X4_T27_STORE_CELL_MAX+4096,
    "T27 summary holds all eleven bounded cells plus envelope");
typedef struct {
    char root[320], session[48], build[65], manifest[65];
    uint64_t nonce;
    FILE *cell_file, *summary_file;
    unsigned active_cell;
    char active_cell_path[X4_T27_STORE_PATH_MAX];
    char active_summary_path[X4_T27_STORE_PATH_MAX];
    int last_rc, last_errno;
} X4T27Store;

/* Owner-thread-only filesystem access, never called inside timed decode.
 * open restores a valid, matching progress envelope without applying core
 * resume policy; the caller reconciles durable cell files with core state.
 * Only missing progress creates an O_EXCL lock and fresh progress. Existing
 * corrupt/oversized/mismatched progress fails closed and preserves all files.
 * No history/partial/temp/lock file is deleted or overwritten. Only the
 * well-known progress.json is intentionally replaced by atomic rename.
 */
int x4_t27_store_open(X4T27Store *, const char *build, const char *manifest,
    uint64_t now_us, X4T27State *state, bool *resumed);
#if defined(X4_T27_STORE_TESTING)
int x4_t27_store_open_at(X4T27Store *, const char *root, const char *build,
    const char *manifest, uint64_t now_us, X4T27State *state, bool *resumed);
enum { X4_T27_STORE_FAULT_FLUSH = 1, X4_T27_STORE_FAULT_SYNC,
    X4_T27_STORE_FAULT_CLOSE, X4_T27_STORE_FAULT_RENAME };
/* One-shot error at the selected durability boundary; real files are kept. */
void x4_t27_store_test_fail(unsigned phase, int error_number);
#endif
int x4_t27_store_write_progress(X4T27Store *, const X4T27State *);

/* begin writes the envelope through `"result":`; the caller writes one JSON
 * object. finish appends complete:true, validates the complete envelope, then
 * fflush -> native sceKernelFsync/host fsync -> fclose. Only return0 confirms
 * that whole sequence. A failed/abandoned file is preserved and never reused.
 */
FILE *x4_t27_store_cell_begin(X4T27Store *, unsigned cell);
int x4_t27_store_cell_finish(X4T27Store *, FILE *);
/* 0 valid complete envelope, 1 missing, 2 invalid/partial; negative I/O/capacity
 * failure. size is set only for a complete valid envelope; result remains a
 * member for the caller to inspect with the real JSON span reader. */
int x4_t27_store_cell_read(X4T27Store *, unsigned cell, char *buffer,
    size_t capacity, size_t *size);

/* The caller owns the complete summary JSON. No prefix/suffix is added;
 * finish bounds its size, validates JSON, then flushes/syncs/closes it. */
FILE *x4_t27_store_summary_begin(X4T27Store *);
int x4_t27_store_summary_finish(X4T27Store *, FILE *);
int x4_t27_store_progress_path(const X4T27Store *, char *, size_t);
int x4_t27_store_cell_path(const X4T27Store *, unsigned cell, char *, size_t);
int x4_t27_store_summary_path(const X4T27Store *, char *, size_t);
#endif
