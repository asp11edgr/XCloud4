/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_backend.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_metrics.h"
#include <stdio.h>

enum { X4_T26_RESULT_TEXT_MAX=65536, X4_T26_RESULT_ID_MAX=64,
    X4_T26_RESULT_HASHES=3 };
enum { X4_T26_RESULT_PASS, X4_T26_RESULT_QUERY_REJECTED,
    X4_T26_RESULT_CREATE_REJECTED, X4_T26_RESULT_NATIVE_ERROR,
    X4_T26_RESULT_LOCAL_ERROR, X4_T26_RESULT_CANCELLED,
    X4_T26_RESULT_CRASH, X4_T26_RESULT_BUDGET, X4_T26_RESULT_NOT_RUN };
typedef struct {
    bool observed,compared,matches_base;
    /* Measured-image ordinal, exactly1/300/600, not a native PTS. */
    uint64_t measured_ordinal;
    char sha256[65];
} X4T26HashResult;
typedef struct {
    bool present,completed,accepted,inference_valid;
    unsigned mode,status,invalid_reasons;
    int native_result,local_result;
    uint64_t elapsed_us,measured_wall_us,output_span_us;
    uint64_t measured_calls,measured_outputs,native_calls,decode_calls;
    uint64_t no_output_calls,error_outputs,flush_calls,drained_outputs;
    uint64_t call_delay_hist[4],measured_flush_outputs;
    uint32_t held_before_close,held_after_close;
    X4T26Stats decode,au_latency,output_interval,decode_to_flip,submit_to_observe;
    X4T26CpuDelta cpu;
    X4T26HashResult hashes[3];
    uint64_t hash_observer_us;
    bool gpu_requested;
    uint64_t gpu_frames,cpu_frames;
    char gpu_reason[64];
    /* Fixed snapshots only; no native pointers, sample arrays or resources. */
    X4T26BackendInfo backend;
} X4T26ModeResult;
typedef struct {
    char build_id[X4_T26_RESULT_ID_MAX+1],manifest_sha256[65];
    unsigned cell,group,clip,factor,status;
    bool final;
    int error_code;
    char reason[128];
    X4T26Config config;
    X4T26ModeResult modes[2];
} X4T26CellResult;
_Static_assert(sizeof(X4T26CellResult)<X4_T26_RESULT_TEXT_MAX,"T26 result fixed bound");
typedef struct {
    unsigned cell,group,clip,factor,status,mode_mask;
    int error_code;char reason[128];
    X4T26Config config;X4T26Outcome outcome;
    uint64_t elapsed_us;
    double native_fps[2];
    bool native_fps_available[2];
    /* Base-factor clean600-image hashes, equal across the two modes. These
     * are recovered only from an accepted, fully validated cell envelope. */
    bool base_hashes_valid;
    char hashes[2][3][65];
} X4T26RecoveredResult;

/* Every field is validated before any write. No I/O inside measurement.
 * encode returns exact bytes excluding NUL; write writes only this object,
 * so the owner can place it in an append-only outer store. */
int x4_t26_result_encode(const X4T26CellResult *,char *,size_t capacity);
int x4_t26_result_write(FILE *,const X4T26CellResult *);
/* Strict complete JSON/schema/identity/numeric/hash reader. Unknown or
 * duplicate fields and truncated objects fail without changing *out.
 * Failed/rejected final cells recover as completed but cannot win best. */
int x4_t26_result_read(const char *,size_t,const char *expected_build,
    const char *expected_manifest,X4T26RecoveredResult *out);
/* Geometry was already validated by the common backend. Hash visible Y
 * followed by visible interleaved UV, row by row; never hashes pitch padding.
 * Uses the project's real MbedTLS SHA256 implementation. Outside measurement. */
int x4_t26_result_hash_nv12(const X4T26Output *,char sha256_hex[65]);
#endif
