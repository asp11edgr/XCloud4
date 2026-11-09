/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_cache.h"
#include "probe_t26_result.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
struct X4StreamPipeline;
struct X4PipelinePresentationSummary;
typedef struct {
    void *context;
    uint64_t (*now_us)(void *);
    void (*sleep_us)(void *,unsigned);
    bool (*budget_expired)(void *);
    X4T26BeforeNative before;
    const atomic_bool *cancel;
    /* Optional realista handoff to the sole display/main owner. Close stops
     * and joins the pipeline, but does not free it; the driver frees only
     * after observing clean native and display ownership. */
    int (*display_open)(void *,struct X4StreamPipeline *);
    int (*display_close)(void *,struct X4StreamPipeline *);
#if X4_PROBE_T27
    unsigned presentation_source_hz,presentation_vsync_policy;
    bool presentation_mailbox;
    struct X4PipelinePresentationSummary *presentation_summary;
#endif
} X4T26RunOps;
/* Synchronous sole decoder owner. All compressed data is already verified
 * and cached. No disk I/O, display, sorting or pixel hashing in the measured
 * window. Hash slots are application-owned until hashed after the window.
 * A failed native close retains the backend and every borrowed owner. */
int x4_t26_run_raw(const X4T26Config *,unsigned mode,const X4T26GeneratedSample *,
    const X4T26ClipData *,const char baseline[3][65],const X4T26RunOps *,
    X4T26ModeResult *,X4T26Backend **retained_backend);
#endif
