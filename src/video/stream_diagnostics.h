/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { X4_STREAM_METRIC_BINS = 2048, X4_STREAM_METRIC_QUANTUM_US = 100,
    X4_STREAM_BITRATE_SECONDS = 3600, X4_STREAM_PAUSE_RECORDS = 256,
    X4_STREAM_KEYFRAME_REQUESTS = 128, X4_STREAM_IDR_LATENCY_BINS = 6001 };
enum { X4_STREAM_PAUSE_RTP_GAP = 1, X4_STREAM_PAUSE_WAIT_IDR = 2,
    X4_STREAM_PAUSE_DECODER = 4, X4_STREAM_PAUSE_PRESENTATION = 8 };
typedef enum {
    X4_STREAM_DECODE, X4_STREAM_CONVERSION, X4_STREAM_STRIPE0, X4_STREAM_STRIPE1,
    X4_STREAM_DECODE_TO_FLIP, X4_STREAM_CONVERT_TO_SUBMIT, X4_STREAM_FLIP_CALL,
    X4_STREAM_SUBMIT_TO_OBSERVATION, X4_STREAM_PRESENT_INTERVAL,
    X4_STREAM_DECODE_IDR, X4_STREAM_DECODE_NON_IDR,
    X4_STREAM_FLIP_END_TO_CONVERSION, X4_STREAM_CONTROLLER_READ_INTERVAL,
    X4_STREAM_DECODE_WITH_OUTPUT, X4_STREAM_DECODE_WITHOUT_OUTPUT,
    X4_STREAM_NATIVE_OUTPUT_INTERVAL,
    X4_STREAM_DECODER_SLOT_WAIT, X4_STREAM_DECODER_RING_GATE_WAIT,
    X4_STREAM_STAGE_COUNT
} X4StreamStage;

enum { X4_PRESENT_HSYNC=0, X4_PRESENT_VSYNC_COLA=1, X4_PRESENT_VSYNC_MAILBOX=2,
    X4_PRESENT_MODE_COUNT=3, X4_PRESENT_VSYNC_SYNC=3, X4_PRESENT_METRIC_MODE_COUNT=4 };
typedef struct {
    uint64_t samples,sum_us,maximum_us,p50_us,p95_us,p99_us;
    unsigned finite_mask; /* bit0=p50, bit1=p95, bit2=p99; overflow stays explicit */
} X4PipelineTimingSummary;
typedef struct {
    uint64_t frames,first_observed_us,last_observed_us;
    X4PipelineTimingSummary decode_to_flip,submit_to_flip,flip_to_conversion;
} X4PipelinePresentationModeSummary;
typedef struct X4PipelinePresentationSummary {
    /* Preserve the historical T27 probe's three-mode ABI and file format.
     * Streaming diagnostics independently export all four metric modes. */
    X4PipelinePresentationModeSummary mode[X4_PRESENT_MODE_COUNT];
    X4PipelineTimingSummary decoder_slot_wait,decoder_ring_gate_wait;
    uint64_t decode_ownership_samples,held_native_by_count[6],occupied_ring_by_count[6];
    uint64_t ring_full_drops,mailbox_replaced,mailbox_discarded_old;
    uint64_t mailbox_late_opportunities,mailbox_late_flips;
    uint64_t clock_anchors,clock_invalid_anchors,clock_residual_samples,clock_residual_sum_us,clock_residual_max_us;
    uint64_t clock_period_us,clock_margin_us,clock_warmup_submissions;
    unsigned scanouts;
    uint64_t scanout_bytes;
} X4PipelinePresentationSummary;
typedef struct {
    uint64_t ordinal,at_us,complete_second,au_count;
    bool previous_vsync,vsync;
} X4StreamVsyncTransition;
typedef struct X4StreamDiagnostics X4StreamDiagnostics;
/* One recording owner per stage; all recording owners must be joined before
 * export/free. Cumulative 100-us bins retain the whole session, not a rolling
 * tail. Last bin is overflow: its percentile has no finite upper bound. */
X4StreamDiagnostics *x4_stream_diagnostics_create(uint64_t started_us);
void x4_stream_diagnostics_free(X4StreamDiagnostics *);
void x4_stream_diagnostics_record(X4StreamDiagnostics *, X4StreamStage, uint64_t us);
/* Concurrent Track callbacks: fixed storage and atomics only. Bytes are valid
 * video RTP callback bytes (including RTP header), not IP/UDP wire bytes. */
void x4_stream_diagnostics_received(X4StreamDiagnostics *, size_t bytes, uint64_t now_us);
/* Complete AU assembly owner, before the recovery filter. This is not a
 * count of RTP packets, submitted AUs or native outputs. */
void x4_stream_diagnostics_au_received(X4StreamDiagnostics *, bool idr, uint64_t now_us);
/* Set once before recording starts. Only legacy Auto T27 uses cadence.
 * The assembly owner closes complete AU-second buckets and publishes effective
 * state; the main owner never reads its non-atomic bucket array. */
void x4_stream_diagnostics_vsync_init(X4StreamDiagnostics *,unsigned policy);
bool x4_stream_diagnostics_vsync_effective(const X4StreamDiagnostics *);
/* Main owner drains bounded transition notifications for Lab klog. */
bool x4_stream_diagnostics_vsync_take_transition(X4StreamDiagnostics *,X4StreamVsyncTransition *);
/* Main-owned timing samples; caller filters a probe's output-ID window. */
void x4_stream_diagnostics_presentation(X4StreamDiagnostics *,unsigned mode,
    uint64_t decode_to_flip_us,uint64_t submit_to_flip_us,uint64_t observed_us);
void x4_stream_diagnostics_flip_to_conversion(X4StreamDiagnostics *,unsigned mode,uint64_t us);
/* Presentation owner only. Ready is the actual READY publication time. A
 * validated native ptime measures the flip, otherwise observation time is an
 * explicitly counted fallback. No UI-only redraw enters these measurements.
 * Repeats are estimates from consecutive valid VSYNC ptime gaps; no untyped
 * GetVblankStatus ABI, physical scanout or HSYNC vblank count is assumed. */
void x4_stream_diagnostics_pacing_observe(X4StreamDiagnostics *,unsigned mode,
    uint64_t ready_us,uint64_t flip_ptime_us,uint64_t observed_us,uint64_t period_us);
/* Call at an existing FlipStatus sample. Depths are separate, not assumed
 * additive. Negative/oversized driver observations stay explicit. */
void x4_stream_diagnostics_pacing_queue(X4StreamDiagnostics *,unsigned ready,
    int driver_pending,int gpu_pending);
/* Cumulative snapshots from their proper recording owners. */
void x4_stream_diagnostics_pacing_drops(X4StreamDiagnostics *,uint64_t recovery,
    uint64_t mailbox,uint64_t ring_full,uint64_t ring_latest,uint64_t cola_vsync_wait);
/* Lab only, one cumulative record each ten seconds and one final record.
 * The histogram has 1 ms bins through 30 ms plus explicit overflow. The
 * percentile accumulator remains the existing 100 us / 204.7 ms model.
 * Complete-AU total is mirrored atomically by the assembly owner. */
void x4_stream_diagnostics_pacing_log(X4StreamDiagnostics *,uint64_t now_us,bool final);
/* Decode-owner samples; never adds a lock or changes admission. */
void x4_stream_diagnostics_decoder_ownership(X4StreamDiagnostics *,uint64_t slot_wait_us,
    uint64_t ring_gate_us,unsigned held_native,unsigned ring_occupied,uint64_t ring_full_drops);
/* After all recording owners joined; no native reads or allocation. */
bool x4_stream_diagnostics_presentation_summary(const X4StreamDiagnostics *,X4PipelinePresentationSummary *);
/* One auth-grant observer producer; paired to the next complete IDR observed
 * by the assembly owner, never to a Decode output or a presumed remote send. */
void x4_stream_diagnostics_keyframe_request(X4StreamDiagnostics *, uint64_t now_us);
/* Event association within the observed presentation interval. Flags are
 * evidence, not proof of a unique causal explanation; unknown/mixed retained. */
void x4_stream_diagnostics_evidence(X4StreamDiagnostics *, unsigned flags);
void x4_stream_diagnostics_present(X4StreamDiagnostics *, uint64_t now_us);
/* Real export body used by the runtime and host tests; writes no identities,
 * credentials, AU/pixel contents or network destinations. */
int x4_stream_diagnostics_write(FILE *, const X4StreamDiagnostics *, uint64_t ended_us);
uint64_t x4_stream_diagnostics_presentations(const X4StreamDiagnostics *);
uint64_t x4_stream_diagnostics_first_present(const X4StreamDiagnostics *);
size_t x4_stream_diagnostics_storage_bytes(void);
bool x4_stream_diagnostics_stage_percentile(const X4StreamDiagnostics *, X4StreamStage,
    unsigned p, uint64_t *out_us, uint64_t *out_max_us);
#include "../ui/ui_perf.h"
/* Main-owned Lab conversion telemetry, exported only after that owner stops.
 * submit-to-EOP CPU observation is not a hardware-only shader timestamp. */
typedef struct {
    uint64_t samples,valid_samples,invalid,pending_samples,pending_overflow;
    uint64_t clock_hz,first_end_ticks,last_end_ticks,first_observed_us,last_observed_us;
    uint64_t latest_start_ticks,latest_end_ticks;
    bool clock_calibrated,unsupported;
    X4UiPerf real;
} X4StreamGpuResolutionDiagnostics;
enum { X4_STREAM_GPU_QUALITY_COMBINATIONS=64 };
typedef struct { uint64_t frames;X4UiPerf real; } X4StreamGpuQualityDiagnostics;
typedef struct {
    unsigned requested,effective;
    bool admitted,prepared,disabled,pending,quarantined,pause_active;
    const char *reason;
    uint64_t gpu_frames,cpu_frames,timeouts,quarantines,late_completed;
    uint64_t checks,checked_pixels,pixels_outside,max_channel_difference,verification_errors;
    uint64_t pause_episodes,pause_cpu_frames,pause_cpu_selections,pause_max_us,pause_current_us;
    uint64_t spin_ready,spin_timeout;
    unsigned arena_bytes,verification_bytes;
    int reserve_result,map_result,submit_result,submit_done_result,allowed_result;
    X4UiPerf time,prep,eop,verify,spin,verification_enqueue,verification_copy_real;
    uint64_t verification_captures,verification_skipped,verification_copy_valid;
    uint64_t verification_native_bytes,verification_control_bytes;
    int verification_priority_result,verification_last_result;
    int verification_reserve_result,verification_map_result,verification_control_reserve_result,verification_control_map_result;
    bool verification_prepared,verification_failed;
    /* 0=720p source, 1=1080p source; output scanout stays 1920x1080. */
    X4StreamGpuResolutionDiagnostics resolution[2];
    unsigned quality_requested_word,quality_effective_word,last_quality_key;
    const char *quality_reason;
    uint64_t pausa_repetidas,dither_frames,deband_soft_frames,deband_strong_frames;
    uint64_t cas_low_frames,cas_medium_frames;
    unsigned quality_arena_bytes,quality_intermediate_bytes;
    int quality_reserve_result,quality_map_result,quality_unmap_result,quality_release_result;
    int intermediate_reserve_result,intermediate_map_result,intermediate_unmap_result,intermediate_release_result;
    X4StreamGpuQualityDiagnostics by_quality[X4_STREAM_GPU_QUALITY_COMBINATIONS];
} X4StreamGpuDiagnostics;
int x4_stream_diagnostics_write_gpu(FILE *,const X4StreamGpuDiagnostics *);
