/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "video_capacity.h"
/* Shared by local probes and network playback. This module owns no native
 * objects, threads, transport or allocator; the caller supplies synchronization. */

enum { X4_PIPELINE_OUTPUTS = 4, X4_PIPELINE_SCANOUTS = 3,
    X4_PIPELINE_SCANOUT_CAPACITY = 4,
    X4_PIPELINE_WIDTH = 1920, X4_PIPELINE_HEIGHT = 1080,
    X4_PIPELINE_OK = 0, X4_PIPELINE_WAIT = 1,
    X4_PIPELINE_INVALID = -7601, X4_PIPELINE_STALE = -7602,
    X4_PIPELINE_STATE = -7603, X4_PIPELINE_EXHAUSTED = -7604 };

#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
/* Ported from probe depth 2: storage for five; runtime defaults to four.
 * Only an admitted depth-two owner selects five after native Query/Create. */
enum { X4_PIPELINE_OUTPUT_CAPACITY = 5 };
#else
enum { X4_PIPELINE_OUTPUT_CAPACITY = X4_PIPELINE_OUTPUTS };
#endif

typedef struct { unsigned slot; uint64_t epoch, generation; } X4VideoPipelineLease;
typedef enum { X4_PIPELINE_FREE, X4_PIPELINE_DECODING,
    X4_PIPELINE_READY, X4_PIPELINE_CONVERTING } X4VideoPipelineOutputState;
typedef struct {
    X4VideoPipelineOutputState state;
    uint64_t generation, output_id, decode_end_us, ready_order;
} X4VideoPipelineOutput;
typedef struct {
    X4VideoPipelineOutput slots[X4_PIPELINE_OUTPUT_CAPACITY];
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
    unsigned output_count; /* initialized to four; depth two explicitly selects five */
#endif
    uint64_t epoch, next_generation, next_ready_order;
    uint64_t acquired, published, converted, cancelled, discarded_ready;
    uint64_t discarded_latency;
    uint64_t waits, rejected;
    unsigned occupied, peak_occupied, ready, peak_ready;
} X4VideoPipelineRing;

/* All state functions require ONE caller-owned lock, including readers of
 * the public fields. No function waits, allocates, calls native APIs or logs.
 * Init is only legal before any worker/native reference exists. A new session
 * supplies a different nonzero epoch; no reset may release in-flight buffers.
 * DECODING may outlive its Decode call when the native decoder retains it. */
int x4_video_pipeline_ring_init(X4VideoPipelineRing *, uint64_t epoch);
#if X4_VIDEO_PROBE || X4_STREAM_PROFILE
int x4_video_pipeline_ring_init_outputs(X4VideoPipelineRing *, uint64_t epoch,
    unsigned output_count);
#endif
int x4_video_pipeline_acquire_decode(X4VideoPipelineRing *,
    X4VideoPipelineLease *, uint64_t *discarded_output_id);
/* Find a native-held lease by returned buffer index. This does not publish it. */
int x4_video_pipeline_decoding_lease(X4VideoPipelineRing *, unsigned slot,
    X4VideoPipelineLease *);
int x4_video_pipeline_publish_ready(X4VideoPipelineRing *, X4VideoPipelineLease,
    uint64_t output_id, uint64_t decode_end_us);
typedef struct {
    unsigned count;
    uint64_t output_ids[X4_PIPELINE_OUTPUT_CAPACITY - 1];
} X4VideoPipelineLatencyDrops;
/* Select the newest READY publication (greatest ready_order), then release
 * every older unclaimed READY. Optional drops is cleared on every call and
 * receives at most three output IDs in oldest-to-newest publication order.
 * These drops increment discarded_latency, NOT discarded_ready (ring-full
 * admission), cancelled or converted. Output IDs, slot indices and native
 * decode timestamps do not determine freshness. DECODING/CONVERTING remain
 * owned by native/the converter. Acquire-decode's full-ring policy is unchanged. */
int x4_video_pipeline_take_ready(X4VideoPipelineRing *, X4VideoPipelineLease *,
    X4VideoPipelineLatencyDrops *drops);
int x4_video_pipeline_release_conversion(X4VideoPipelineRing *, X4VideoPipelineLease);
/* Call only after native confirms that this particular lease was NOT accepted,
 * a completed (possibly damaged) output has returned its exact validated slot,
 * or native decoder destruction has released all references to it. */
int x4_video_pipeline_abort_decode(X4VideoPipelineRing *, X4VideoPipelineLease);
unsigned x4_video_pipeline_cancel_ready(X4VideoPipelineRing *);
bool x4_video_pipeline_ring_idle(const X4VideoPipelineRing *);

typedef enum { X4_SCANOUT_FREE, X4_SCANOUT_WRITING, X4_SCANOUT_READY,
    X4_SCANOUT_PENDING, X4_SCANOUT_SCREEN, X4_SCANOUT_QUARANTINED
} X4VideoPipelineScanoutState;
typedef struct {
    X4VideoPipelineScanoutState state;
    uint64_t lease_generation, output_id, decode_end_us, ready_order;
    uint64_t converted_us, flip_arg, submitted_us, observed_us;
    uint64_t target_vblank_us;
    unsigned presentation_mode; /* 0=HSYNC, 1=VSYNC Cola, 2=VSYNC Mailbox */
} X4VideoPipelineScanout;
typedef struct {
    X4VideoPipelineScanout slots[X4_PIPELINE_SCANOUT_CAPACITY];
    uint64_t epoch, next_generation, next_ready_order, last_submitted_arg;
    uint64_t submitted, observed, rejected;
    uint64_t last_submitted_output_id, mailbox_replaced, mailbox_discarded_old;
    uint64_t ready_since_us;
    unsigned peak_occupied, peak_in_flight;
    unsigned scanout_count;
    bool mailbox_capable, mailbox_admission;
    /* Occupies the existing bool padding, preserving display/scanout ABI. */
    bool synced;
    int pending, screen;
} X4VideoPipelineDisplay;
/* Display-owner-only history stored separately from the embedded ring so
 * decoder/GPU fields keep the exact same offsets in the production owner. */
typedef struct {
    uint8_t synced_history[30];
    unsigned synced_history_count, synced_history_next;
    uint64_t synced_last_observed_us, synced_recovery_drops;
    uint64_t synced_last_dropped_output;
} X4VideoPipelineSyncHistory;
typedef struct {
    unsigned free, writing, ready, pending, screen, occupied, in_flight;
    unsigned quarantined; /* excluded from the one displayable frame in flight */
} X4VideoPipelineDisplayOccupancy;
int x4_video_pipeline_display_init(X4VideoPipelineDisplay *, uint64_t epoch);
/* Cold initialization only. Legacy init is exactly three/Cola. Mailbox can
 * be exercised with three or four slots; production selects four so SCREEN,
 * PENDING, READY and WRITING can coexist without reusing any owner. */
int x4_video_pipeline_display_init_mode(X4VideoPipelineDisplay *,uint64_t epoch,
    unsigned scanouts,bool mailbox_capable);
/* Cold-only initialization. Uses the existing four scanouts, oldest READY
 * selection and bounded latency recovery after about 500 ms of backlog. */
int x4_video_pipeline_display_init_synced(X4VideoPipelineDisplay *,uint64_t epoch);
void x4_video_pipeline_sync_history_init(X4VideoPipelineSyncHistory *);
/* Call only after an exact observe_flip succeeds, before submitting another
 * frame. UI-only refreshes are excluded by the owner. Never touches a native
 * pending, writing or currently displayed slot. */
int x4_video_pipeline_synced_observed(X4VideoPipelineDisplay *,X4VideoPipelineSyncHistory *,uint64_t observed_us);
/* Admission changes only. Already-owned work drains under its original
 * safety contract; this never releases or rewrites an owned slot. */
int x4_video_pipeline_display_mailbox_admission(X4VideoPipelineDisplay *,bool enabled);
/* Pure snapshot under the same owner lock. SCREEN counts as occupied but is
 * not in flight. Cola permits one WRITING/READY/PENDING total; Mailbox permits
 * one of each, and never two native pending flips. */
int x4_video_pipeline_display_occupancy(const X4VideoPipelineDisplay *,
    X4VideoPipelineDisplayOccupancy *);
/* Cola: WAIT while any conversion, READY or PENDING owns a slot. Mailbox:
 * acquire one FREE writer alongside at most one READY and one PENDING.
 * Always acquire BEFORE taking latest native READY; SCREEN is never writable. */
int x4_video_pipeline_acquire_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease *);
int x4_video_pipeline_finish_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease,
    uint64_t output_id, uint64_t decode_end_us, uint64_t converted_us);
int x4_video_pipeline_abort_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease);
/* A CPU timeout does not revoke GPU ownership. A quarantined slot cannot be
 * acquired, flipped, cancelled or closed. Retire it ONLY after its exact EOP. */
int x4_video_pipeline_quarantine_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease);
int x4_video_pipeline_retire_quarantine(X4VideoPipelineDisplay *, X4VideoPipelineLease);
/* Cancel only unsubmitted READY. Never releases WRITING, PENDING or SCREEN. */
unsigned x4_video_pipeline_cancel_ready_scanout(X4VideoPipelineDisplay *);
/* Cancel only this unsubmitted READY lease. Other READY/WRITING/native-owned
 * slots remain owned. Used when a verification result invalidates one image. */
int x4_video_pipeline_cancel_ready_lease(X4VideoPipelineDisplay *,X4VideoPipelineLease);
/* Call only AFTER VideoOutClose succeeds and all converters have joined.
 * Retires display references while preserving generations and statistics. */
int x4_video_pipeline_display_closed(X4VideoPipelineDisplay *);
/* Returns oldest READY, only while no flip is pending. Caller must submit it
 * to VideoOut before mark_submitted; on failure it remains READY for teardown. */
int x4_video_pipeline_ready_scanout(X4VideoPipelineDisplay *, X4VideoPipelineLease *);
int x4_video_pipeline_mark_submitted(X4VideoPipelineDisplay *, X4VideoPipelineLease,
    uint64_t flip_arg, uint64_t submitted_us);
/* An exact pending flip-arg observation transfers SCREEN ownership and frees
 * only the previous SCREEN. Unrelated/stale observations never free buffers. */
int x4_video_pipeline_observe_flip(X4VideoPipelineDisplay *, uint64_t flip_arg,
    uint64_t observed_us, X4VideoPipelineLease *presented);

/* No untyped Vblank ABI is invented. The caller supplies only validated
 * VSYNC FlipStatus.ptime anchors in process-time microseconds. This estimates
 * refresh phase; it is not a physical scanout timestamp or a hard RT bound. */
typedef struct {
    uint64_t anchor_us,period_us,margin_us,target_us;
    uint64_t anchors,invalid_anchors,residual_samples,residual_sum_us,residual_max_us;
    uint64_t missed_opportunities,warmup_submissions;
    bool valid;
} X4VideoPipelineMailboxClock;
void x4_video_pipeline_mailbox_clock_init(X4VideoPipelineMailboxClock *);
bool x4_video_pipeline_mailbox_anchor(X4VideoPipelineMailboxClock *,
    uint64_t ptime,uint64_t submitted_us,uint64_t observed_us,bool vsync);
/* WAIT preserves READY for replacement. Without a valid anchor one VSYNC
 * warm-up submission is admitted immediately. Late opportunities advance to
 * a later refresh; they never submit HSYNC or rewrite PENDING/SCREEN. */
int x4_video_pipeline_mailbox_due(X4VideoPipelineMailboxClock *,uint64_t now_us,
    uint64_t ready_since_us,uint64_t *target_us);

typedef struct { unsigned first_row, end_row; } X4VideoPipelineStripe;
int x4_video_pipeline_stripe(unsigned height, unsigned workers, unsigned index,
    X4VideoPipelineStripe *);
/* Restricted to the layouts independently established by the probe: visible
 * 1280x720 in 720 rows, or 1920x1080 in 1088 rows, uncropped NV12. Bounds cover
 * full storage planes, not just visible bytes. Returns before writes on error.
 * Destination is A8B8G8R8, 1920x1080, with pitch in uint32_t pixels. */
int x4_video_pipeline_convert_stripe(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes, unsigned destination_pitch,
    unsigned workers, unsigned index);
/* Diagnostic-only 1:1 conversion for initial reference comparison. */
int x4_video_pipeline_convert_reference(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes);

#ifndef X4_CONVERT_SMOOTH_DEFAULT
#define X4_CONVERT_SMOOTH_DEFAULT 1
#endif
#ifndef X4_SHARPNESS_PUBLIC_DEFAULT
#define X4_SHARPNESS_PUBLIC_DEFAULT 2
#endif

typedef enum {
    X4_CONVERT_ORIGINAL = 0,
    X4_CONVERT_SMOOTH = 1
} X4VideoConvertMode;

typedef enum {
    X4_SHARPNESS_OFF = 0,
    X4_SHARPNESS_LOW = 1,
    X4_SHARPNESS_MED = 2
} X4VideoSharpness;

/* Streaming color is selected from the SPS VUI. Matrix 1 is BT.709; 2
 * (unspecified), 5 and 6 retain BT.601 compatibility. Unknown matrix values
 * also use BT.601; the owner must record that assumption. Both limited and
 * full range are implemented. The legacy entry points above keep their
 * original restricted geometry and exact Q8 BT.601 arithmetic. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

typedef struct {
    unsigned matrix_coefficients;
    bool full_range;
    unsigned char convert_mode;
    unsigned char sharpness;
} X4VideoPipelineColor;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
enum { X4_PIPELINE_CONVERT_SCALAR = 0, X4_PIPELINE_CONVERT_SSE2 = 1 };
/* Validates progressive 8-bit NV12 storage up to coded 1920x1088. Explicit
 * visible offsets encode cropping; crop coordinates may be odd, in which
 * case chroma is sampled from the absolute source coordinate. Fast conversion
 * requires coded/storage 1280x720 or 1920x1088 (visible 1080), with even crop
 * coordinates. Other safe coded/storage geometry uses scalar scaling even
 * when its visible rectangle happens to be exactly 720p or 1080p.
 * The returned path is the compiled implementation, not a timing claim. */
int x4_video_pipeline_color_path(const X4VideoNv12Layout *, size_t source_bytes,
    unsigned *path);
int x4_video_pipeline_convert_stripe_color(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes, unsigned destination_pitch,
    unsigned workers, unsigned index, const X4VideoPipelineColor *);
int x4_video_pipeline_convert_reference_color(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes, const X4VideoPipelineColor *);
/* Lab verification: copy up to 16 visible WC scanout rows to cached memory.
 * Source must be 16-byte aligned, its row pitch a multiple of four pixels.
 * This is a readback helper only; it does not select a conversion backend. */
int x4_video_pipeline_gpu_copy_rows(const uint32_t *source, size_t source_bytes,
    unsigned source_pitch, uint32_t *destination, size_t destination_bytes,
    unsigned first_row, unsigned rows);
/* GPU verification only: an admitted 1080p row slice into a compact CPU
 * scratch surface. Reuses the unchanged CPU row converters and color math. */
int x4_video_pipeline_gpu_reference_rows(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes, unsigned first_row, unsigned rows,
    const X4VideoPipelineColor *);
/* Lab streaming's two converters reserve scratch slots 0/1. This main-owner
 * reference reserves production slot 2; the probe's three-converter program
 * is mutually exclusive. Destination addresses a full 1920x1080 image, but
 * only [first_row,first_row+rows) is written (at most 16 rows). */
int x4_video_pipeline_gpu_reference_720_rows(const X4VideoNv12Layout *,
    const uint8_t *source, size_t source_bytes, uint32_t *destination,
    size_t destination_bytes, unsigned first_row, unsigned rows,
    const X4VideoPipelineColor *);
