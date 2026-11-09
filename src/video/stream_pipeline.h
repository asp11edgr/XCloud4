/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "../media/live_trace.h"
#include "probe_stack.h"
#include "../streaming/stream_profile.h"
#include "../streaming/stream_startup.h"
#include "stream_wait.h"
#include "stream_diagnostics.h"
#include "stream_cpu.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_pipeline.h"
#endif

typedef struct X4StreamPipeline X4StreamPipeline;
typedef struct X4KeyframeLimiter X4KeyframeLimiter;
typedef void (*X4StreamOverlay)(uint32_t *pixels, void *context);
enum { X4_STREAM_FRAME_ERROR_LIMIT = 10,
    X4_STREAM_FRAME_FAILURE_LIMIT = -7430, X4_STREAM_RECONFIGURATION_TIMEOUT = -7431 };
typedef struct {
    uint64_t accepted_aus, decode_calls, outputs, conversions, presentations;
    uint64_t decode_us, decode_max_us, convert_us, convert_max_us;
    uint64_t latest_picture_us, discarded_latency, discarded_ring_full;
    unsigned width, height, native_width, native_height, pitch;
    unsigned requested_depth,active_depth;
    unsigned rejected_width, rejected_height, consecutive_frame_errors;
    unsigned received_matrix, effective_matrix;
    bool full_range;
    uint64_t color_notice_started_us;
    uint64_t frame_errors, keyframe_requests, reconfigurations;
    int error;
    bool native_ready, display_open, retained, waiting_idr, reconfiguring, display_detached;
} X4StreamPipelineSnapshot;

/* Create before Track callbacks. stop belongs to the live-media context and
 * outlives close. Receive is atomic-only; submit has exactly one AU owner and
 * never waits for Decode, queue space, or a queue lock. Positive submit
 * results are deliberate drops, NOT admission and NOT fatal video errors. */
X4StreamPipeline *x4_stream_pipeline_create(uint64_t session, X4Trace *, const atomic_bool *stop);
X4StreamPipeline *x4_stream_pipeline_create_request(uint64_t session, X4Trace *, const atomic_bool *stop,
    const X4StreamRequestConfig *request);
/* Borrowed until the RTC and auth owners are stopped, before pipeline free. */
X4StreamStartup *x4_stream_pipeline_startup(X4StreamPipeline *);
void x4_stream_pipeline_set_keyframe_limiter(X4StreamPipeline *,X4KeyframeLimiter *);
int x4_stream_pipeline_start(X4StreamPipeline *);
/* Immediate admission only: zero copied/accepted; positive is a deliberate
 * policy drop/stop and never successful admission; negative is fatal. No
 * waiting on native Decode, queue space or an occupied metadata gate. */
int x4_stream_pipeline_submit(X4StreamPipeline *, const uint8_t *au, size_t bytes,
    uint64_t pts_us, uint64_t au_identity, const uint8_t *sps, size_t sps_bytes);
int x4_stream_pipeline_submit_parameters(X4StreamPipeline *, const uint8_t *au, size_t bytes,
    uint64_t pts_us, uint64_t au_identity, const uint8_t *sps, size_t sps_bytes,
    const uint8_t *pps, size_t pps_bytes);
void x4_stream_pipeline_received(X4StreamPipeline *, size_t bytes, uint64_t now_us);
void x4_stream_pipeline_note_post_halt_video(X4StreamPipeline *, uint64_t now_us);
void x4_stream_pipeline_au_received(X4StreamPipeline *, bool is_idr, uint64_t now_us);
void x4_stream_pipeline_rtp_gap(X4StreamPipeline *, uint64_t now_us);
void x4_stream_pipeline_note_keyframe_request(X4StreamPipeline *, uint64_t now_us);
/* Compatibility/test mailbox only. Production posts into the common limiter;
 * the sole auth grant observer calls note_keyframe_request before local I/O.
 * Neither mailbox consumption nor a grant proves network delivery. */
bool x4_stream_pipeline_take_keyframe_request(X4StreamPipeline *);
void x4_stream_pipeline_request_stop(X4StreamPipeline *);
void x4_stream_pipeline_snapshot(X4StreamPipeline *, X4StreamPipelineSnapshot *);
/* Assembly owner: single try per gate, failure leaves the cached view intact. */
bool x4_stream_pipeline_try_snapshot(X4StreamPipeline *,X4StreamPipelineSnapshot *);
/* Callback atomics only. FULL is exported separately as a design failure. */
void x4_stream_pipeline_note_callback_reject(X4StreamPipeline *,bool queue_full);
void x4_stream_pipeline_note_ingress_stall_recovery(X4StreamPipeline *);
uint64_t x4_stream_pipeline_ingress_stall_recoveries(const X4StreamPipeline *);
/* Main owner only. UI VideoOut must already be closed. Overlay is invoked
 * after conversion, before SubmitFlip, on a buffer not SCREEN or PENDING. */
int x4_stream_pipeline_display_open(X4StreamPipeline *);
int x4_stream_pipeline_present(X4StreamPipeline *, X4StreamOverlay, void *context);
/* Main-only punctual frozen-picture UI update:0 applied, WAIT retry, <0 error.
 * Reads SCREEN, writes only an acquired FREE slot. Not a new media image. */
int x4_stream_pipeline_redraw_notice(X4StreamPipeline *,X4StreamOverlay,void *context);
enum { X4_STREAM_MAIN_CONVERTING=1, X4_STREAM_MAIN_FLIP_PENDING=2 };
/* Main owner only: these fields are written by the same owner, so this
 * observation adds no lock, and is not a cross-thread status snapshot. */
unsigned x4_stream_pipeline_main_state(const X4StreamPipeline *);
/* Main-owned diagnostic state only; no rendering or ring policy changes. */
void x4_stream_pipeline_note_menu_open(X4StreamPipeline *,bool open);
void x4_stream_pipeline_note_controller_read(X4StreamPipeline *,uint64_t sampled_us);
typedef struct {
    bool available;
    uint64_t observed_us,packets,frames,dropped,lost,bad_frames,underflows;
    int startup_error,error,stop_result;
} X4StreamAudioObservation;
/* Main-owned copy of existing audio counters immediately before audio stop.
 * It does not change audio scheduling or claim counts after that snapshot. */
void x4_stream_pipeline_record_audio(X4StreamPipeline *,const X4StreamAudioObservation *);
/* Main owner: wake hint from publication/conversion/cancel. Flip and pad
 * observation use 1 ms with work in flight, otherwise 5 ms. */
int x4_stream_pipeline_wait_present(X4StreamPipeline *,unsigned timeout_us);
int x4_stream_pipeline_wake(X4StreamPipeline *);
enum { X4_STREAM_WAIT_MAIN=0, X4_STREAM_WAIT_INGRESS=1,
    X4_STREAM_WAIT_EXTERNAL_ACTORS=2 };
/* Owner-written after the corresponding actor stops, before export. */
void x4_stream_pipeline_record_wait_stats(X4StreamPipeline *,unsigned actor,
    const X4StreamWaitStats *,uint64_t elapsed_us);
void x4_stream_pipeline_record_ingress_cpu(X4StreamPipeline *,const X4StreamCpuReport *);
/* Main-only video shutdown after a fatal video error; AU storage and callback
 * diagnostics stay allocated until full media close. display_detached is the
 * only permission to reopen UI VideoOut, including retained-decoder cases. */
int x4_stream_pipeline_halt_video(X4StreamPipeline *);
/* Main only after ingress owner joined and Track callbacks unbound. Joins all
 * CPU readers before releasing native outputs. A negative return retains the
 * entire context; only display_detached permits independent UI display use. */
int x4_stream_pipeline_close(X4StreamPipeline *);
/* Export after full close and all CPU producers joined, including retained
 * native teardown. No pixel/native memory is read. Does not free context. */
int x4_stream_pipeline_export(X4StreamPipeline *, const X4ProbeStackReport *ingress_thread);
/* Free only after close and all producers join. Failure retains the context
 * and its borrowed owners; the caller must not free their parent storage. */
int x4_stream_pipeline_free(X4StreamPipeline *);
#if X4_VIDEO_PROBE && X4_PROBE_T26
/* Probe controls are cold-only, before start and before opening VideoOut.
 * Legacy t26_create remains Always/Cola. No decoder configuration is changed. */
int x4_stream_pipeline_t26_set_present_policy(X4StreamPipeline *,unsigned mode,bool mailbox);
int x4_stream_pipeline_t26_set_measure_window(X4StreamPipeline *,
    uint64_t first_output_id,uint64_t last_output_id);
/* Cold summary only after all observers have joined, before free. */
bool x4_stream_pipeline_t26_presentation_summary(const X4StreamPipeline *,
    X4PipelinePresentationSummary *);
#endif
