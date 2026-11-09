/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "../media/live_trace.h"
#include "video_capacity.h"
#include "../streaming/stream_profile.h"
#ifndef X4_VIDEO_PROBE
#define X4_VIDEO_PROBE 0
#endif
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
#include <stdatomic.h>
#include "probe_stack.h"
#endif
#if X4_VIDEO_PROBE
#include "probe_case.h"
#include "probe_hash.h"
#include "probe_stack.h"
#include "probe_reference.h"
#endif
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_hooks.h"
#endif
enum { X4_LIVE_WIDTH = 1280, X4_LIVE_HEIGHT = 720, X4_LIVE_AU_MAX = 2 * 1024 * 1024,
    X4_LIVE_COPY_READERS = 4 };
/* RGB storage only; decoder limits and live admission remain 1280x720. */
enum { X4_LIVE_RGB_WIDTH = X4_VIDEO_CAPACITY_1080 ? 1920 : X4_LIVE_WIDTH,
    X4_LIVE_RGB_HEIGHT = X4_VIDEO_CAPACITY_1080 ? 1080 : X4_LIVE_HEIGHT,
    X4_LIVE_RGB_PIXELS = X4_LIVE_RGB_WIDTH * X4_LIVE_RGB_HEIGHT };

#if X4_STREAM_PROFILE
#include "stream_sps.h"
enum { X4_STREAM_ATTEMPT_SPS, X4_STREAM_ATTEMPT_GEOMETRY, X4_STREAM_ATTEMPT_LEGACY };
enum {
    X4_STREAM_POLICY_EXACT, X4_STREAM_POLICY_FLOOR,
    X4_STREAM_POLICY_LEVEL_LIMITED, X4_STREAM_POLICY_UNKNOWN_LEVEL,
    X4_STREAM_POLICY_LEVEL_CONFLICT, X4_STREAM_POLICY_REF_CONFLICT,
    X4_STREAM_POLICY_INELIGIBLE_GEOMETRY, X4_STREAM_POLICY_NATIVE,
    X4_STREAM_POLICY_QUERY_BUDGET
};
typedef enum {
    X4_STREAM_FRAME_OK, X4_STREAM_FRAME_BAD_FRAME,
    X4_STREAM_FRAME_RECOVERABLE_RESTART, X4_STREAM_FRAME_RECONFIGURE,
    X4_STREAM_FRAME_FATAL
} X4StreamFrameClass;
typedef struct {
    unsigned profile, level, width, height, dpb, depth;
    unsigned kind, dpb_requested, dpb_minimum, dpb_level_cap, reason, failure_reason;
    bool fallback, level_known, skipped;
    char failure_stage[48]; /* Fixed project label, captured before cleanup. */
    int compute_query, allocate_queue, query, create, cleanup;
    uint64_t compute_bytes, cpu_bytes, gpu_bytes, shared_bytes, frame_bytes, planned_bytes;
} X4StreamDecoderAttempt;
typedef struct {
    X4StreamSps sps;
    X4StreamSpsDiagnostics sps_diagnostics;
    X4StreamDecoderAttempt attempts[6];
    unsigned attempt_count, active_attempt;
    bool fallback_used, depth_fallback_used, input_lifetime_assumption;
    unsigned requested_depth, active_depth, input_count, output_count;
    unsigned held_output_mask, live_input_count;
    uint64_t native_budget_bytes, budget_auxiliary_bytes;
    uint64_t geometry_warnings;
    uint64_t planned_native_bytes, queried_compute_bytes, queried_cpu_bytes;
    uint64_t queried_gpu_bytes, queried_shared_bytes, queried_frame_bytes;
    int compute_query_result, allocate_queue_result, query_result, create_result;
    int delete_decoder_result, release_queue_result, close_result;
    bool retained, creation_uncertain;
    unsigned cpu_variant; int decoder_cpu_priority; uint64_t decoder_cpu_affinity;
} X4StreamVideoInfo;
#endif
typedef struct {
    void *state;
    uint32_t *pixels;
    unsigned width, height;
    uint64_t frames;
    /* Video-owner cumulative timings in sceKernelGetProcessTime microseconds. */
    uint64_t decode_calls, decoded_frames, no_picture_calls, decode_us, decode_max_us;
    uint64_t convert_us, convert_max_us;
    /* Selected-copy wall time includes dispatch/completion, but excludes the
     * first-copy serial reference/compare. Reader times may overlap. */
    uint64_t copy_calls, copy_us, copy_max_us, copy_bytes;
    /* helper_us is the SUM of overlapping elapsed spans across three helpers;
     * it is neither CPU time nor an additional contribution to copy wall. */
    uint64_t copy_parallel_calls, copy_serial_calls, copy_owner_us, copy_helper_us;
    uint64_t copy_wait_us, copy_wait_max_us, forced_preserve_calls;
    uint64_t copy_check_attempts, copy_check_pass, copy_check_mismatch, copy_check_not_checked;
    uint64_t copy_check_bytes, copy_check_us;
    uint64_t last_picture_time_us, picture_gap_max_us;
    int error;
    char stage[48];
    /* Session diagnostic ordinals survive decoder restart. Native output has
     * no returned PTS: output IDs never identify an input AU. */
    X4Trace *trace;
    uint64_t trace_au, trace_decode_next, trace_output_next, trace_copy_next;
    uint64_t trace_decoder_epoch, trace_converted_output;
#if X4_STREAM_PROFILE
    X4StreamVideoInfo stream_info;
#endif
#if X4_VIDEO_PROBE
    X4ProbeReferenceScore probe_raw_scores[3][2];
    unsigned probe_raw_score_counts[3];
    X4ProbeStatus probe_status;
    X4ProbeStackReport probe_stacks[3];
#endif
} X4LiveVideo;
/* All calls run on one video-owner thread. Three optional persistent CPU-only
 * copy helpers finish their disjoint spans before conversion, Decode or native
 * teardown. A partial pool is stopped/joined before serial fallback. The caller
 * retains RGB storage until teardown succeeds. Any failed helper join latches
 * an error and retains the entire state, native and staging storage. */
int x4_live_video_start(X4LiveVideo *, uint32_t *pixels, size_t pixel_capacity);
/* Select an unpublished destination before a batch. No reader may own it. */
int x4_live_video_set_target(X4LiveVideo *, uint32_t *pixels, size_t pixel_capacity);
int x4_live_video_feed(X4LiveVideo *, const uint8_t *, size_t, uint64_t pts);
/* Publish the last validated native picture of this batch. A feed converts
 * first if its output reservation would overwrite that pending picture. */
int x4_live_video_convert_pending(X4LiveVideo *);
int x4_live_video_stop(X4LiveVideo *);
#if X4_VIDEO_PROBE
int x4_live_video_probe_start(X4LiveVideo *, uint32_t *, size_t,
    const X4ProbeCase *, X4ProbeHashContext *, const atomic_bool *cancel);
int x4_live_video_probe_start_memory(X4LiveVideo *, uint32_t *, size_t,
    const X4ProbeCase *, X4ProbeHashContext *, const atomic_bool *cancel,
    int output_memory_type, X4ProbeMetrics *metrics);
/* Native Flush through the same validated output lease/conversion pipeline.
 * 1 indicates a native picture; 0 drained; +3 cancellation without a native
 * call; negative controlled error. */
int x4_live_video_probe_flush(X4LiveVideo *);
#endif

#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
typedef struct {
    const uint8_t *pixels; size_t owned_bytes, returned_bytes;
    X4VideoNv12Layout layout;
    unsigned slot, native_width, native_height, pitch;
    bool accepted, valid, layout_verified;
#if X4_STREAM_PROFILE
    /* A completed_output identifies one validated native output slot, even
     * when the picture is damaged. An uncertain lease requires Delete==0
     * before any output/input reservation may be reused. */
    X4StreamFrameClass frame_class;
    bool ownership_uncertain, completed_output;
    int control_result; /* SPS/control result, never a native Decode return. */
#endif
    int native_result;
#if X4_STREAM_PROFILE
    bool offered_retained;
    unsigned input_slot; uint64_t native_call;
#endif
    uint64_t output_id, decode_us, decode_end_us;
#if X4_VIDEO_PROBE && X4_PROBE_T26
    uint32_t t26_native_valid,t26_native_error,t26_picture_count;
    bool t26_native_called;
    const void *t26_native_pointer;
    size_t t26_native_bytes;
#endif
} X4ProbeNativeOutput;
#endif
#if X4_VIDEO_PROBE
/* Sole native decoder owner. Offered/returned slots are deliberately distinct.
 * No copy helpers, NV12 staging, RGB publication or automatic slot reuse. */
int x4_live_video_probe_raw_start(X4LiveVideo *, uint32_t *, size_t,
    const X4ProbeCase *, X4ProbeHashContext *, const atomic_bool *);
int x4_live_video_probe_raw_decode(X4LiveVideo *, unsigned offered_slot,
    const uint8_t *, size_t, uint64_t pts, bool flush, X4ProbeNativeOutput *);
#endif

#if X4_STREAM_PROFILE
enum { X4_STREAM_VIDEO_CANCELLED=3 };
int x4_live_video_stream_raw_start(X4LiveVideo *, const uint8_t *sps,
    size_t sps_size, const atomic_bool *cancel);
int x4_live_video_stream_raw_start_cpu(X4LiveVideo *, const uint8_t *sps,
    size_t sps_size, const atomic_bool *cancel, unsigned cpu_variant);
/* Requested depth is 1 or 2 after resolving the menu Auto policy. Native
 * Query/Create rejection of depth 2 safely retries the same config at depth 1. */
int x4_live_video_stream_raw_start_cpu_depth(X4LiveVideo *, const uint8_t *sps,
    size_t sps_size, const atomic_bool *cancel, unsigned cpu_variant, unsigned depth);
/* Sole decode-owner snapshot, no synchronization added. A set bit means the
 * ring lease must stay DECODE until an exact returned pointer or Delete==0. */
unsigned x4_live_video_stream_output_held_mask(const X4LiveVideo *);
/* Zero includes classified nonfatal control/frame outcomes. Inspect
 * frame_class, completed_output and ownership_uncertain before touching a
 * lease. +3 is cancellation before Decode; negative is a fatal local/native
 * resource-contract failure. Unknown native errors request bounded restart,
 * never silently authorize reuse. No output PTS is exposed by this ABI. */
int x4_live_video_stream_raw_decode(X4LiveVideo *, unsigned offered_slot,
    const uint8_t *, size_t, uint64_t pts, bool flush, X4ProbeNativeOutput *);
void x4_live_video_stream_info(const X4LiveVideo *, X4StreamVideoInfo *);
#endif
#if X4_VIDEO_PROBE && X4_PROBE_T26
/* Exact one-attempt local benchmark. No SPS/geometry/depth fallback. The
 * caller closes even a failed start and retains the context on close error. */
int x4_live_video_t26_raw_start(X4LiveVideo *, const uint8_t *sps, size_t,
    const atomic_bool *, const X4T26NativeOptions *);
unsigned x4_live_video_t26_allocations(const X4LiveVideo *,
    X4T26Allocation out[X4_T26_ALLOCATION_MAX]);
#endif
