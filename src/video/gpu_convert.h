/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "video_pipeline.h"
#include "gpu_quality_ref.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Asynchronous converter. The caller serializes owner functions.
 * This object owns its unchanged 64 KiB legacy arena and optional, lazy
 * quality controls / RGBA32F intermediate. They share the same EOP lifetime. It does
 * NOT own the native source or the scanout. The caller must retain both leases
 * from an accepted start until DONE/LATE_DONE, including after TIMEOUT.
 * Exactly one thread may submit on an object; poll/close never submit.
 */
typedef struct X4GpuConvert X4GpuConvert;
enum { X4_GPU_CONVERT_ARENA_BYTES=65536, X4_GPU_CONVERT_TIMEOUT_US=50000,
    X4_GPU_CONVERT_QUALITY_ARENA_BYTES=262144,
    X4_GPU_CONVERT_QUALITY_INTERMEDIATE_NATIVE_BYTES=33226752,
    X4_GPU_CONVERT_ACCEPTED=0, X4_GPU_CONVERT_CPU=1, X4_GPU_CONVERT_REPEAT=2,
    /* Native returns are zero/negative. This sentinel means no call occurred. */
    X4_GPU_CONVERT_NOT_RUN=INT32_MAX };
typedef enum {
    X4_GPU_CONVERT_IDLE=0, X4_GPU_CONVERT_WAIT=1,
    X4_GPU_CONVERT_DONE=2, X4_GPU_CONVERT_TIMEOUT=3,
    X4_GPU_CONVERT_LATE_DONE=4,
    /* Verification failure is not an EOP timeout. Retain/quarantine both
     * native and scanout leases exactly as for TIMEOUT until matching EOPs. */
    X4_GPU_CONVERT_VERIFY_QUARANTINE=5
} X4GpuConvertPoll;
typedef struct {
    X4VideoNv12Layout layout;
    X4VideoFormat format;
    X4VideoPipelineColor color;
    const uint8_t *source;
    size_t source_bytes;
    uint32_t *destination;
    size_t destination_bytes;
    unsigned destination_pitch;
    /* Immutable settings captured by the streaming session, never reloaded
     * from R3 while an accepted job owns either buffer. Zero is the old ABI. */
    uint32_t quality_word;
} X4GpuConvertFrame;
enum { X4_GPU_RESOLUTION_720=0, X4_GPU_RESOLUTION_1080=1,
    X4_GPU_RESOLUTION_COUNT=2 };
typedef struct {
    /* samples counts completed EOP observations, including invalid marks.
     * Uncalibrated durations are pending, never reported as zero microseconds.
     * The bounded pending ledger is validated when calibration becomes ready;
     * overflow counts raw observations that could not be kept for backfill. */
    uint64_t samples,valid_samples,invalid,pending_samples,pending_overflow;
    uint64_t clock_hz,first_end_ticks,last_end_ticks;
    uint64_t first_observed_us,last_observed_us;
    bool clock_calibrated,unsupported;
} X4GpuTimestampStatus;
typedef struct {
    bool prepared,disabled,pending,quarantined,owner_bound,pause_active;
    int reserve_result,map_result,submit_result,submit_done_result;
    int unmap_result,release_result,allowed_result;
    uint64_t accepted,completed,timeouts,quarantines,late_completed;
    uint64_t cpu_fallbacks,wrong_thread_submits;
    uint64_t submitted_us,last_elapsed_us;
    /* Samples share the existing start-to-observed-EOP population. The clock
     * boundary is immediately after Submit, before SubmitDone; EOP includes
     * CPU observation latency and is not a hardware-only timestamp. */
    uint64_t submit_returned_us,last_prep_us,last_eop_us;
    /* CPU selections caused by allowed==0, not proof that conversion finished.
     * An episode ends on the next nonzero result or close. For an active
     * episode, callers may combine pause_started_us with their own now_us. */
    uint64_t pause_episodes,pause_cpu_frames,pause_current_frames;
    uint64_t pause_started_us,pause_last_us,pause_last_frames,pause_max_us;
    X4GpuTimestampStatus timestamps[X4_GPU_RESOLUTION_COUNT];
    unsigned last_resolution;
    uint64_t last_start_ticks,last_end_ticks,last_real_us;
    bool last_real_valid;
    size_t arena_bytes;
    const char *reason;
    uint32_t quality_requested_word,quality_effective_word,quality_key;
    bool quality_active,quality_prepared,quality_failed,quality_cas;
    unsigned quality_arena_bytes,quality_intermediate_bytes;
    uint64_t pausa_repetidas;
    int quality_reserve_result,quality_map_result,quality_unmap_result,quality_release_result;
    int intermediate_reserve_result,intermediate_map_result,intermediate_unmap_result,intermediate_release_result;
    const char *quality_reason;
    bool verification_prepared,verification_failed,verification_pending;
    size_t verification_native_bytes,verification_control_bytes;
    int verification_reserve_result,verification_map_result;
    int verification_unmap_result,verification_release_result;
    int verification_control_reserve_result,verification_control_map_result;
    int verification_control_unmap_result,verification_control_release_result;
    uint64_t verification_captures,verification_skipped,verification_checks,verification_copy_completed;
    uint64_t verification_errors,verification_pixels,verification_outside;
    uint32_t verification_max_difference;
    uint64_t verification_last_enqueue_us,verification_last_worker_us;
    uint64_t verification_last_copy_real_us;
    bool verification_last_copy_real_valid;
    int verification_last_result;
} X4GpuConvertStatus;

/* NULL means CPU heap allocation failed. An arena-preparation failure returns
 * a disabled object with native returns in status; close must still retire it.
 */
X4GpuConvert *x4_gpu_convert_open(void);
bool x4_gpu_convert_admit(const X4VideoNv12Layout *,X4VideoFormat,
    const X4VideoPipelineColor *,const char **reason);
int x4_gpu_convert_start(X4GpuConvert *,const X4GpuConvertFrame *,uint64_t now_us);
/* allowed==0 returns CPU, or REPEAT for applicable active quality with the
 * repeat policy, without disabling or waiting. Every start retries once. Negative is submits_error and
 * permanent; the actual native value remains in status.allowed_result. */
/* TIMEOUT disables future starts but leaves pending true and all memory live.
 * It is emitted once. A later matching EOP returns LATE_DONE exactly once.
 */
X4GpuConvertPoll x4_gpu_convert_poll(X4GpuConvert *,uint64_t now_us,uint64_t *elapsed_us);
/* Owner only. Flushes/reads the label without consuming completion, changing
 * status or releasing either lease. It must not be used as cancellation. */
bool x4_gpu_convert_label_ready(X4GpuConvert *);
bool x4_gpu_convert_pending(const X4GpuConvert *);
bool x4_gpu_convert_disabled(const X4GpuConvert *);
const char *x4_gpu_convert_reason(const X4GpuConvert *);
void x4_gpu_convert_status(const X4GpuConvert *,X4GpuConvertStatus *);
/* No waits and no force-free. False preserves *state, its arena and any
 * in-flight source/destination references. A timeout is not GPU cancellation.
 */
bool x4_gpu_convert_close(X4GpuConvert **state,uint64_t now_us);
/* Prepare off the image path before starting the one verification worker.
 * Owner calls request/collect/cancel; only the worker calls take/complete.
 * Request never waits and skips a busy snapshot. It applies only to the next
 * accepted start. Old starts without a request keep their original DCB bytes.
 * The original converter DCB and separate copy DCB must BOTH reach EOP before
 * DONE can retire the source and scanout. A worker owns only copied pixels. */
int x4_gpu_convert_verification_prepare(X4GpuConvert *);
bool x4_gpu_convert_verification_request(X4GpuConvert *,unsigned first_row,unsigned rows);
bool x4_gpu_convert_verification_take(X4GpuConvert *,X4GpuVerifySnapshot *);
int x4_gpu_convert_verification_complete(X4GpuConvert *,const X4GpuVerifyResult *);
bool x4_gpu_convert_verification_collect(X4GpuConvert *,X4GpuVerifyResult *);
/* AFTER the worker has joined. False preserves the object and WB storage if
 * a GPU command or a worker can still access it. No forced cancellation. */
bool x4_gpu_convert_verification_cancel(X4GpuConvert *);
