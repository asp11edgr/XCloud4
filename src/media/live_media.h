/* SPDX-License-Identifier: GPL-3.0-only */
/* Live WebRTC media reception: RTP H264 (RFC 6184, packetization-mode 1)
 * and Opus. Independent of the local H264/PCM demo. */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "live_trace.h"

enum { X4_LIVE_KIND_VIDEO = 0, X4_LIVE_KIND_AUDIO = 1 };
/* Largest RTP packet copied from the transport; larger packets are dropped. */
enum { X4_LIVE_RTP_MAX = 2048 };

enum {
    X4_LIVE_ERR_ARGUMENT = -20, X4_LIVE_ERR_MEMORY = -21, X4_LIVE_ERR_STATE = -22,
    X4_LIVE_ERR_NO_OPUS = -23, X4_LIVE_ERR_OPUS = -24, X4_LIVE_ERR_THREAD = -25,
    X4_LIVE_ERR_DECODER = -26
};

typedef struct X4LiveMedia X4LiveMedia;

typedef struct {
    uint64_t video_packets, audio_packets; /* valid RTP copied into the queues */
    uint64_t video_frames;                 /* decoded, validated, converted pictures */
    uint64_t audio_frames;                 /* Opus packets decoded into PCM */
    uint64_t dropped;                      /* sum of every drop/loss counter below */
    uint64_t video_dropped_packets, audio_dropped_packets; /* invalid, queue full, late, duplicate, foreign */
    uint64_t video_lost_packets, audio_lost_packets;       /* sequence gaps */
    uint64_t video_dropped_frames;         /* damaged, undecodable or rejected access units */
    uint64_t audio_dropped_frames;         /* Opus errors and latency trims */
    uint64_t audio_underflows;             /* silence blocks while playing (not real frames) */
    uint64_t keyframe_requests;
    /* Video-owner diagnostics; durations are cumulative microseconds.
     * Worker maxima reset when it acknowledges the last report boundary. */
    uint64_t video_decode_calls, video_decoded_frames, video_no_picture_calls;
    uint64_t video_decode_us, video_decode_max_us, video_convert_us, video_convert_max_us;
    uint64_t video_copy_calls, video_copy_us, video_copy_max_us, video_copy_bytes;
    uint64_t video_copy_parallel_calls, video_copy_serial_calls, video_copy_owner_us, video_copy_helper_us;
    uint64_t video_copy_wait_us, video_copy_wait_max_us, video_forced_preserve_calls;
    uint64_t video_copy_check_attempts, video_copy_check_pass, video_copy_check_mismatch, video_copy_check_not_checked;
    uint64_t video_copy_check_bytes, video_copy_check_us;
    uint64_t video_tick_calls, video_tick_us, video_tick_max_us, video_tick_packets;
    uint64_t video_draw_calls, video_draw_new, video_draw_repeat, video_draw_us, video_draw_max_us;
    uint64_t video_present_calls, video_present_us, video_present_max_us;
    uint64_t video_picture_age_us, video_picture_gap_max_us;
    uint64_t video_au_submitted, video_publications, video_publication_superseded;
    uint64_t video_publication_generation, video_worker_idle_yields;
    /* Sum/max ingress dwell of popped packets, and current oldest sample. */
    uint64_t video_queue_age_us, video_queue_age_max_us, video_queue_oldest_age_us;
    uint64_t video_queue_full, video_queue_push_contended, video_queue_pop_contended;
    uint32_t video_queue_depth, video_queue_highwater;
    uint32_t width, height;                /* last decoded picture, 0 until real video */
    int video_error, audio_error;          /* last failure, 0 when none */
    bool started, video_ready, audio_ready, audio_playing, waiting_keyframe;
} X4LiveMediaSnapshot;

/* Ownership contract:
 * - create/start/tick/draw/snapshot/set_muted/close run on the main thread.
 * - one worker owns video initialization, RTP ordering/AUs, Decode,
 *   copy/conversion and native teardown. A triple RGB mailbox transfers only
 *   completed pictures; draw claims a slot without holding the gate while
 *   reading pixels. Native output buffers never cross to the main thread.
 *   Three optional helpers copy disjoint NV12 spans only; the video owner
 *   waits for all completions before conversion or decoder-output reuse.
 * - receive may run on any transport thread; it copies at most
 *   X4_LIVE_RTP_MAX bytes into a bounded queue and never retains `rtp`.
 * - close may only be called after the transport callbacks have stopped
 *   (x4_rtc_close returned) and any auth worker using the context was
 *   joined. No callback may run after close. */
X4LiveMedia *x4_live_media_create(int *error);
/* Optional, before start: fix the negotiated RTP payload type (0..127) of a
 * kind. Without it the first valid packet of each kind locks the track. */
int x4_live_media_set_payload_type(X4LiveMedia *media, int kind, int payload_type);
/* Initializes the native H264 decoder and the audio worker, once. Returns
 * negative only when video cannot start; audio failures are reported in the
 * snapshot while video continues. */
int x4_live_media_start(X4LiveMedia *media);
/* Signature matches X4RtcMediaCallback: kind 0 video, 1 audio. */
void x4_live_media_receive(void *context, int kind, const uint8_t *rtp, size_t size);
/* Nonblocking main-thread maintenance/reporting; does not Decode or copy.
 * The worker yields between packets after 16 ms or four Decode calls and
 * converts its last validated output. Native calls cannot be interrupted. */
void x4_live_media_tick(X4LiveMedia *media);
/* Main-thread query: a completed publication has not yet been presented.
 * UI changes can force draw even when this returns false. */
bool x4_live_media_has_new_picture(X4LiveMedia *media);
/* Borrowed until close succeeds; numeric diagnostics only. Main-only drawn
 * observation does not advance presentation state or read a newer slot. */
X4Trace *x4_live_media_trace(X4LiveMedia *media);
bool x4_live_media_drawn(X4LiveMedia *media, uint64_t *generation,
    uint64_t *native_output_id, bool *fresh_vs_presented);
/* Draws the last real picture scaled with aspect ratio into a 1920x1080
 * framebuffer with black bars. Returns 1 when drawn, 0 when no real picture
 * exists yet (the framebuffer is untouched). */
int x4_live_media_draw(X4LiveMedia *media, uint32_t *pixels);
/* After a successful display call containing a live draw, record its VSYNC
 * wait and confirm that drawn generation as presented. UI-only flips must
 * not call this function. Failed flips never advance presented generation. */
void x4_live_media_note_present(X4LiveMedia *media, uint64_t elapsed_us);
void x4_live_media_snapshot(const X4LiveMedia *media, X4LiveMediaSnapshot *snapshot);
void x4_live_media_set_muted(X4LiveMedia *media, bool muted);
/* Thread-safe: returns true once per pending keyframe request so the
 * transport owner can call x4_rtc_request_keyframe. */
bool x4_live_media_take_keyframe_request(X4LiveMedia *media);
/* Stops/joins the owner before freeing storage. Returns 0 when released.
 * A native video teardown failure is latched: future close calls retain the
 * context and return that error; main never retries native video teardown. */
int x4_live_media_close(X4LiveMedia *media);

/* ---- Helpers shared by src/media, src/audio live modules (not UI API). ---- */

typedef struct {
    const uint8_t *payload;
    size_t size;
    uint32_t timestamp, ssrc;
    uint16_t sequence;
    uint8_t payload_type;
    bool marker;
} X4LiveRtp;
/* Validates RTP v2 framing (CSRC, extension, padding) and rejects RTCP
 * packet types 200..206. Returns 0 on success, negative otherwise. */
int x4_live_rtp_parse(const uint8_t *packet, size_t size, X4LiveRtp *rtp);

/* Bounded multi-producer/single-consumer packet queue guarded by an
 * atomic_flag held only for one short copy. */
typedef struct {
    atomic_flag lock;
    uint32_t head, count, capacity;
    uint16_t *sizes;
    uint8_t *data;
    /* Optional video-only ingress timestamps; audio leaves this NULL. */
    uint64_t *arrivals;
    atomic_uint_fast64_t oldest_arrival;
    atomic_uint depth, highwater;
    atomic_uint_fast64_t full, push_contended, pop_contended;
} X4LiveRing;
int x4_live_ring_init(X4LiveRing *ring, uint32_t capacity);
void x4_live_ring_free(X4LiveRing *ring);
/* Returns false when the queue is full or contended; the caller counts it. */
bool x4_live_ring_push(X4LiveRing *ring, const uint8_t *packet, size_t size);
/* Copies the oldest packet into out[X4_LIVE_RTP_MAX]; returns 0 when empty. */
size_t x4_live_ring_pop(X4LiveRing *ring, uint8_t *out);

typedef struct {
    bool locked;
    int forced_type;  /* -1 for automatic */
    uint8_t payload_type;
    uint32_t ssrc;
} X4LiveTrack;
/* A track locks both payload type and SSRC. Other sources are rejected;
 * a new session must explicitly initialize a new track. */
enum { X4_LIVE_TRACK_FOREIGN = -1, X4_LIVE_TRACK_SAME = 0, X4_LIVE_TRACK_NEW = 1 };
void x4_live_track_init(X4LiveTrack *track, int forced_type);
int x4_live_track_accept(X4LiveTrack *track, const X4LiveRtp *rtp);

typedef struct {
    uint16_t sequence, size;
    bool used;
    uint64_t arrival;
} X4LiveReorderSlot;
typedef struct {
    uint16_t window, depth, expected, buffered;
    bool started;
    uint64_t max_wait;
    X4LiveReorderSlot *slots;
    uint8_t *data;
} X4LiveReorder;
enum { X4_LIVE_REORDER_STORED = 0, X4_LIVE_REORDER_LATE = 1, X4_LIVE_REORDER_JUMP = 2 };
/* window is a power of two <= 1024; depth is how many packets past a hole
 * are tolerated; max_wait is in microseconds of sceKernelGetProcessTime. */
int x4_live_reorder_init(X4LiveReorder *reorder, uint16_t window, uint16_t depth, uint64_t max_wait);
void x4_live_reorder_free(X4LiveReorder *reorder);
void x4_live_reorder_reset(X4LiveReorder *reorder);
/* LATE: duplicate or already passed. JUMP: the buffered packets were
 * discarded and the stream restarts at this packet (treat as a gap). */
int x4_live_reorder_insert(X4LiveReorder *reorder, const uint8_t *packet, size_t size,
                           uint16_t sequence, uint64_t now);
/* Returns the next in-order packet, or NULL. When a hole is abandoned, the
 * number of skipped sequence numbers is added to *lost. The pointer stays
 * valid until the next insert/reset. now=0 suppresses only time expiry;
 * the depth/window bounds still apply while queued ingress is inserted. */
const uint8_t *x4_live_reorder_next(X4LiveReorder *reorder, uint64_t now, size_t *size, uint32_t *lost);
