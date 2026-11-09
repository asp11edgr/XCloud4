/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_runner.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_pipeline.h"
/* Bank-worker producer plus the real streaming Decode/converter/display
 * owners. display_open/close hand off to the sole display/main owner; open
 * must not return until UI VideoOut is closed and pipeline display is open.
 * Close must stop/join/close the pipeline without freeing it, and may reopen
 * UI VideoOut only after display_detached. Both operations are synchronous.
 *
 * AU1 alone starts Query/Create, with a bounded10s initial wait outside
 * steady measurement. After its first Decode return, AU2..781 are paced60Hz
 * (approximately3s/180-AU warmup) and600 native images are measured (source
 * ordinals181..780). With depth2 AU781 releases image780; its own pending
 * image is deliberately retired by Delete, not Flush. Association is the
 * verified no-B/POC2 FIFO inference, never an offered slot or a native PTS.
 * Decode-call statistics cover input ordinals181..780; output latency and
 * interval statistics cover inferred source pictures181..780. CPU wall runs
 * from the Decode owner's post-AU180 snapshot to measured output600, so it
 * includes source pacing and the small depth-dependent boundary difference.
 * Only three outputs are hashed synchronously, immediately before pipeline
 * publication; hash_observer_us makes that observation cost explicit.
 * gpu_frames/cpu_frames are the pipeline's actual completed conversions over
 * the entire session including warmup, not just the600 measured outputs. A
 * GPU request with zero completed GPU frames is not an accepted realista run.
 *
 * On retained close *retained_pipeline and its callback scratch/platform
 * remain allocated, as do the borrowed ops context and cancel owner. The
 * caller must preserve those owners and stop the bank, never force-free. */
/* T27 changes producer pacing/presentation only; native configuration is
 * the unchanged T26 base. Output window and hashes remain181..780. */
typedef struct { unsigned source_hz,vsync_policy; bool mailbox; } X4T26RealisticOptions;
struct X4PipelinePresentationSummary;
#if X4_PROBE_T27
int x4_t26_run_realistic_options(const X4T26Config *,const X4T26GeneratedSample *,
    const X4T26ClipData *,const char baseline[3][65],const X4T26RunOps *,
    const X4T26RealisticOptions *,struct X4PipelinePresentationSummary *,
    X4T26ModeResult *,struct X4StreamPipeline **retained_pipeline);
#endif
int x4_t26_run_realistic(const X4T26Config *,const X4T26GeneratedSample *,
    const X4T26ClipData *,const char baseline[3][65],const X4T26RunOps *,
    X4T26ModeResult *,struct X4StreamPipeline **retained_pipeline);
#endif
