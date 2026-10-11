/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "rtc_receive_diag.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct X4Pipeline X4Pipeline;
typedef struct {
    uint64_t time, identity, source, a, b, c;
    unsigned kind, event, stage, flags;
} X4PipelineEvent;
typedef struct {
    uint64_t pending, admitted, accepted, stale_source, duplicate, held;
    uint64_t au_submitted, feed_calls, dropped_frames, recovery_episodes;
    uint64_t queue_depth, inbox_admitted, inbox_omitted, inbox_stale, event_omitted;
    uint32_t epoch, expected, buffered;
    bool locked, waiting_keyframe;
} X4PipelineSnapshot;
uint64_t x4_pipeline_clock(void); /* Explicit test clock, supplied by C++ owner. */
X4Pipeline *x4_pipeline_create(bool grace, uint32_t transport_epoch);
void x4_pipeline_receive(X4Pipeline *, const void *, size_t, const X4NackPacket *);
void x4_pipeline_batch(X4Pipeline *);
/* Explicit functional control fixture, through the actual bounded inbox. */
bool x4_pipeline_control(const X4NackGraceControl *);
unsigned x4_pipeline_reorder_code(bool late);
X4PipelineSnapshot x4_pipeline_snapshot(const X4Pipeline *);
size_t x4_pipeline_event_count(void);
bool x4_pipeline_event(size_t, X4PipelineEvent *);
size_t x4_pipeline_feed_bytes(void *, size_t);
void x4_pipeline_close(X4Pipeline *);
#ifdef __cplusplus
}
#endif
