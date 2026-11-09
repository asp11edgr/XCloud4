/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#ifndef X4_VIDEO_PROBE
#define X4_VIDEO_PROBE 0
#endif
#ifndef X4_PROBE_T26
#define X4_PROBE_T26 0
#endif
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "live_h264.h"
#include "probe_t26_hooks.h"

/* Observation only: none of these callbacks transfers ownership or changes
 * admission, native publication, conversion, display or recovery policy.
 * They run on the decoder owner, except on_flip (the display/main owner).
 * The pointed-to values are valid only for the callback's duration. Native
 * output memory may be read by on_decode/before_close while still owned;
 * after_close must never read/hash a native allocation released by Delete.
 * Callbacks must not call back into the pipeline or retain output pointers. */
typedef void (*X4T26PipelineDecodeObserver)(void *context, uint64_t au_identity,
    int result, const X4ProbeNativeOutput *, const X4StreamVideoInfo *);
typedef void (*X4T26PipelineFlipObserver)(void *context, uint64_t output_identity,
    uint64_t decode_end_us, uint64_t submitted_us, uint64_t observed_us);
typedef void (*X4T26PipelineBeforeClose)(void *context, const X4LiveVideo *,
    const X4StreamVideoInfo *);
typedef void (*X4T26PipelineAfterClose)(void *context, const X4LiveVideo *,
    const X4StreamVideoInfo *, int result);
typedef struct {
    X4T26NativeOptions native;
    X4T26PipelineDecodeObserver on_decode;
    X4T26PipelineFlipObserver on_flip;
    X4T26PipelineBeforeClose before_close;
    X4T26PipelineAfterClose after_close;
    void *context;
} X4T26PipelineHooks;

/* The constructor copies this entire value, including native options. Both
 * context pointers are borrowed until pipeline close/free has completed,
 * including a retained close. It never changes process-wide settings.
 * Only the exact Videodec2 realista cell (api=2, realistic=true, depth=1/2)
 * is accepted. Decode caller/converters stay CPU-0; native decoder settings
 * come exclusively from native.config. Conversion is the production GPU
 * backend, Suave/Media, classic quality word 0, VSYNC, unchanged queues. */
struct X4StreamPipeline;
struct X4StreamPipeline *x4_stream_pipeline_t26_create(uint64_t session,
    const atomic_bool *stop, const X4T26PipelineHooks *);
/* Call only after the display owner acknowledges close. A false result means
 * a failed join can still have a live observer: preserve its entire context
 * and do not inspect/copy its mutable measurement arrays. */
bool x4_stream_pipeline_t26_observers_stopped(const struct X4StreamPipeline *);
/* Cold observation only, before free. Returns false without touching outputs
 * unless all observers are stopped. Counts cover the entire session including
 * warmup and count completed GPU/CPU conversions, not only measured flips. */
bool x4_stream_pipeline_t26_gpu_status(const struct X4StreamPipeline *,
    bool *requested,uint64_t *gpu_frames,uint64_t *cpu_frames,char reason[64]);
#endif
