/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_media.h"
#include "video_ingress.h"
#include "nack_grace.h"
#include "nack_sequence_binding.h"
#include "../streaming/nack_grace_inbox.h"
#include "../streaming/rtc_receive_diag.h"
#include "../streaming/receiver_nack_state.h"
#include "../video/live_h264.h"
#include "../video/display.h"
#include "../audio/live_audio.h"
/* Legacy ingress harnesses deliberately omit the transport bridge. The new
 * observation harness opts in with explicit doubles; native Lab is enabled. */
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE && !X4_PUBLIC && \
    (!defined(X4_INGRESS_HOST_TEST) || defined(X4_RECOVERY43_MEDIA_HOST_TEST))
#define X4_RECOVERY43_MEDIA 1
#include "../streaming/recovery43_bridge.h"
#else
#define X4_RECOVERY43_MEDIA 0
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
#include "../video/stream_pipeline.h"
#include "../video/stream_cpu.h"
#include "../streaming/keyframe_limiter.h"
/* Private reconnect bridge: AU queue only; no decoder/display ownership. */
extern int x4_stream_pipeline_begin_receive_epoch(X4StreamPipeline *);
#endif
#if X4_VIDEO_PROBE
#include "../video/probe_hash.h"
#include "../video/probe_stack.h"
#endif
#include <orbis/libkernel.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
#ifndef X4_NACK_GRACE_MS
#define X4_NACK_GRACE_MS 0
#endif
enum { GR_CONTROL_REQUEST=1, GR_CONTROL_OUTCOME, GR_OFFER, GR_RELEASE,
    GR_CALLBACK, GR_QUEUE_POP, GR_WORKER_ACCEPT, GR_WORK_COST, GR_QUEUE_AGE,
    GR_COUNTER, GR_HOLD_BEGIN, GR_ABANDON,
    GR_SEQUENCE_ANNOUNCE, GR_SEQUENCE_CLOCK, GR_OBS_OMISSION, GR_OBS_RESET,
    GR_BIND_CANDIDATE, GR_BIND_PROOF, GR_BIND_COMMIT, GR_BIND_REJECT, GR_BIND_MAP,
    GR_DIRECT_BUDGET_DROP };
/* Legacy trace schema excludes SSRC. The private progress sidecar records
 * numeric SSRC/sequence only; neither format contains payloads or credentials.
 * MEDIA events 0x100..0x13f are reserved in live_trace.h. */
enum {
    MT_RTP = X4_TRACE_RX_VALID, MT_RX_REJECT = X4_TRACE_RX_REJECT,
    MT_QUEUE_REJECT = X4_TRACE_QUEUE_DROP, MT_POP = X4_TRACE_QUEUE_POP,
    MT_FOREIGN = X4_TRACE_REORDER_DROP, MT_TRACK = X4_TRACE_SOURCE_EPOCH,
    MT_REORDER_INSERT = X4_TRACE_REORDER_INSERT, MT_REORDER_GAP = X4_TRACE_REORDER_HOLE,
    MT_AU_OPEN = X4_TRACE_AU_BEGIN, MT_AU_RESET = X4_TRACE_AU_RESET,
    MT_MARKER = X4_TRACE_AU_COMPLETE, MT_PUBLISH = X4_TRACE_RGB_PUBLICATION,
    MT_SUPERSEDE = X4_TRACE_RGB_SUPERSEDED, MT_WORKER_START = X4_TRACE_MEDIA_START,
    MT_WORKER_STOP = X4_TRACE_MEDIA_STOP, MT_KEY_REQUEST = X4_TRACE_PLI_REQUEST,
    MT_DAMAGE = 0x120, MT_IDR, MT_SUBMIT_BEGIN, MT_SUBMIT_END, MT_RESET_TOTAL,
    MT_DAMAGE_TOTAL, MT_WAIT_BEGIN, MT_WAIT_END, MT_POP_CONTENDED,
    MT_REORDER_EMIT = X4_TRACE_REORDER_EMIT, MT_YIELD = X4_TRACE_WORKER_YIELD
};
enum { RESET_SUBMITTED, RESET_TIMESTAMP, RESET_MARKER, RESET_WAIT,
    RESET_PARAMS, RESET_FEED, RESET_INGRESS, RESET_TRACK, RESET_JUMP,
    RESET_REORDER, RESET_COUNT };
enum {
    DM_FORBIDDEN = 1u << 0, DM_NAL_TYPE = 1u << 1, DM_FU_ACTIVE = 1u << 2,
    DM_STAP_SHORT = 1u << 3, DM_STAP_LENGTH = 1u << 4, DM_STAP_TRAIL = 1u << 5,
    DM_FU_HEADER = 1u << 6, DM_FU_NESTED = 1u << 7, DM_FU_ORPHAN = 1u << 8,
    DM_FU_MISMATCH = 1u << 9, DM_CAPACITY = 1u << 10,
    DM_DISCARDED = 1u << 11, DM_PACKETIZATION = 1u << 12
};
static int rtp_parse_reason(const uint8_t *p, size_t size, X4LiveRtp *r, unsigned *reason)
{
    if (reason) *reason = 0;
    if (!p || !r || size < 12 || size > X4_LIVE_RTP_MAX || (p[0] >> 6) != 2 || (p[1] >= 200 && p[1] <= 206)) { if (reason) *reason = 1; return -1; }
    size_t at = 12 + (p[0] & 15) * 4u;
    if (at > size) { if (reason) *reason = 2; return -1; }
    if (p[0] & 16) {
        if (size - at < 4) { if (reason) *reason = 3; return -1; }
        size_t ext = (size_t)be16(p + at + 2) * 4u + 4;
        if (ext > size - at) { if (reason) *reason = 4; return -1; }
        at += ext;
    }
    size_t end = size;
    if (p[0] & 32) {
        unsigned pad = p[size - 1];
        if (!pad || pad > size - at) { if (reason) *reason = 5; return -1; }
        end -= pad;
    }
    if (at >= end) { if (reason) *reason = 6; return -1; }
    *r = (X4LiveRtp){.payload = p + at, .size = end - at, .sequence = be16(p + 2),
        .timestamp = be32(p + 4), .ssrc = be32(p + 8), .payload_type = p[1] & 127, .marker = (p[1] & 128) != 0};
    return 0;
}
int x4_live_rtp_parse(const uint8_t *p, size_t size, X4LiveRtp *r) { return rtp_parse_reason(p, size, r, NULL); }
int x4_live_ring_init(X4LiveRing *q, uint32_t capacity)
{
    memset(q, 0, sizeof(*q)); atomic_flag_clear(&q->lock);
    atomic_init(&q->depth, 0); atomic_init(&q->highwater, 0);
    atomic_init(&q->oldest_arrival, 0);
    atomic_init(&q->full, 0); atomic_init(&q->push_contended, 0); atomic_init(&q->pop_contended, 0);
    if (!capacity || capacity > 1024) return -1;
    q->sizes = calloc(capacity, sizeof(*q->sizes)); q->data = malloc((size_t)capacity * X4_LIVE_RTP_MAX);
    if (!q->sizes || !q->data) { x4_live_ring_free(q); return X4_LIVE_ERR_MEMORY; }
    q->capacity = capacity; return 0;
}
void x4_live_ring_free(X4LiveRing *q) { free(q->sizes); free(q->data); free(q->arrivals); q->sizes = NULL; q->data = NULL; q->arrivals = NULL; q->capacity = 0; }
static bool ring_push_reason(X4LiveRing *q, const uint8_t *p, size_t size, unsigned *reason)
{
    if (reason) *reason = 0;
    if (!q || !q->capacity || !p || !size || size > X4_LIVE_RTP_MAX) { if (reason) *reason = 1; return false; }
    if (atomic_flag_test_and_set_explicit(&q->lock, memory_order_acquire)) {
        if (reason) *reason = 2;
        atomic_fetch_add_explicit(&q->push_contended, 1, memory_order_relaxed); return false;
    }
    bool ok = q->count < q->capacity;
    if (ok) {
        unsigned at = (q->head + q->count) % q->capacity;
        memcpy(q->data + (size_t)at * X4_LIVE_RTP_MAX, p, size); q->sizes[at] = size; ++q->count;
        if (q->arrivals) {
            q->arrivals[at] = sceKernelGetProcessTime();
            if (q->count == 1) atomic_store_explicit(&q->oldest_arrival, q->arrivals[at], memory_order_relaxed);
        }
        atomic_store_explicit(&q->depth, q->count, memory_order_relaxed);
        if (q->count > atomic_load_explicit(&q->highwater, memory_order_relaxed))
            atomic_store_explicit(&q->highwater, q->count, memory_order_relaxed);
    }
    else { if (reason) *reason = 3; atomic_fetch_add_explicit(&q->full, 1, memory_order_relaxed); }
    atomic_flag_clear_explicit(&q->lock, memory_order_release); return ok;
}
bool x4_live_ring_push(X4LiveRing *q, const uint8_t *p, size_t size) { return ring_push_reason(q, p, size, NULL); }
static size_t ring_pop_at(X4LiveRing *q, uint8_t *p, uint64_t *arrival, unsigned *reason)
{
    if (reason) *reason = 0;
    if (arrival) *arrival = 0;
    if (!q || !q->capacity) return 0;
    if (atomic_flag_test_and_set_explicit(&q->lock, memory_order_acquire)) {
        if (reason) *reason = 2;
        atomic_fetch_add_explicit(&q->pop_contended, 1, memory_order_relaxed); return 0;
    }
    size_t size = 0;
    if (q->count) {
        size = q->sizes[q->head]; memcpy(p, q->data + (size_t)q->head * X4_LIVE_RTP_MAX, size);
        if (arrival && q->arrivals) *arrival = q->arrivals[q->head];
        q->head = (q->head + 1) % q->capacity; --q->count;
        if (q->arrivals) atomic_store_explicit(&q->oldest_arrival, q->count ? q->arrivals[q->head] : 0, memory_order_relaxed);
    }
    atomic_store_explicit(&q->depth, q->count, memory_order_relaxed);
    atomic_flag_clear_explicit(&q->lock, memory_order_release); return size;
}
size_t x4_live_ring_pop(X4LiveRing *q, uint8_t *p) { return ring_pop_at(q, p, NULL, NULL); }
void x4_live_track_init(X4LiveTrack *t, int pt) { memset(t, 0, sizeof(*t)); t->forced_type = pt; }
int x4_live_track_accept(X4LiveTrack *t, const X4LiveRtp *r)
{
    if ((t->forced_type >= 0 && r->payload_type != t->forced_type) ||
        (t->locked && (r->payload_type != t->payload_type || r->ssrc != t->ssrc))) return X4_LIVE_TRACK_FOREIGN;
    if (!t->locked) { t->locked = true; t->ssrc = r->ssrc; t->payload_type = r->payload_type; return X4_LIVE_TRACK_NEW; }
    return X4_LIVE_TRACK_SAME;
}
enum { RGB_SLOTS = 3, WORKER_BUDGET_US = 16000, WORKER_DECODE_LIMIT = 4 };
typedef struct { uint32_t *pixels; unsigned width, height; uint64_t generation, completed_us, native_output; } RgbSlot;
struct X4LiveMedia {
    X4Trace *trace;
    uint64_t trace_session;
    uint64_t trace_au_next, trace_au, trace_track_epoch, last_draw_output;
    uint32_t damage_bits, first_damage;
    unsigned trace_budget_reason;
    unsigned diagnostic_worker_phase; /* Worker-owned phase restored after mailbox publication. */
    uint64_t reset_counts[RESET_COUNT], discard_counts[RESET_COUNT], damage_counts[13];
    X4VideoIngress ingress; /* Video only; the original ring stays in audio. */
    /* The video reorder borrows these physical-slot tickets. Sequence holes
     * consume no slot; payload and ticket move/retire together. AU tickets
     * remain owned by the existing assembler, without a new decoder queue. */
    X4IngressTicket reorder_tickets[128], au_tickets[128];
    unsigned au_ticket_count;
    uint64_t au_ticket_omissions, reorder_ticket_omissions;
    uint64_t reorder_admission[9], reorder_peak_bytes;
    uint64_t reorder_direct_budget_drops;
    unsigned reorder_peak_packets;
    X4NackGraceInbox grace_inbox;
    X4NackGraceState grace;
    X4NackSequenceBinding grace_binding;
    uint64_t binding_reject_source,binding_reject_omitted;
    uint32_t binding_reject_epoch,binding_reject_flags;
    bool binding_reject_valid;
    uint64_t grace_controls, grace_seen_omissions;
    uint64_t grace_decode_start[64], grace_copy_start[64], grace_convert_start[64];
    uint64_t grace_max_dwell[64];
    uint64_t grace_hold_logged;
    uint64_t grace_force_ranges;
    bool grace_force_gap;
    atomic_uint callback_active, callback_peak;
    atomic_uint_fast64_t callback_calls, callback_overlap, callback_us, callback_max_us;
    atomic_uint_fast64_t reserve_us, reserve_max_us, rx_invalid;
    atomic_uint_fast64_t callback_hist[12];
    uint64_t recovery_begin_us, recovery_episodes, recovery_completed, recovery_us;
    X4LiveReorder reorder;
    X4LiveTrack track;
    X4LiveVideo video;
    X4LiveVideo video_totals; /* Retired decoder counters; never owns pointers. */
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4StreamPipeline *stream;
    X4ProbeThread stream_ingress_thread;
#endif
    X4LiveAudio *audio;
    uint8_t *au;
    size_t au_size, fu_start;
    uint8_t sps[4096], pps[4096], au_sps[4096], au_pps[4096];
    size_t sps_size, pps_size, au_sps_size, au_pps_size;
    uint32_t timestamp, discard_timestamp;
    uint8_t fu_type;
    bool fu, au_open, idr, damaged, waiting_keyframe, discard_timestamp_valid;
    atomic_bool started, stop, start_complete, reset_maxima, video_stopped;
    atomic_int start_result;
    OrbisPthread thread;
    bool running;
    int worker_close_result;
    atomic_flag mailbox_gate;
    RgbSlot rgb[RGB_SLOTS];
    int published_slot, reader_slot, writer_slot;
    uint64_t publication_generation, publication_superseded, worker_idle_yields, au_submitted;
    uint64_t queue_age_us, queue_age_max_us;
    X4LiveMediaSnapshot worker_view;
    int audio_error, audio_payload_type;
    atomic_bool keyframe, ingress_gap;
    /* Session worker publishes after RTC close joined callbacks. Only the
     * video owner resets track/reorder/AU state; the callback never waits. */
    atomic_bool receive_epoch_hold;
    atomic_uint receive_epoch_callbacks;
    atomic_uint_fast64_t receive_epoch_requested, receive_epoch_applied;
    atomic_uint_fast64_t retired_video_source, receive_epoch_rejected;
    atomic_int receive_epoch_result;
    atomic_uint_fast64_t packets, video_bytes, dropped, requests;
    atomic_uint_fast64_t post_halt_video_packets, first_post_halt_video_us;
    uint64_t lost, dropped_frames;
    bool stall_tracking;
    uint64_t stall_since_us, stall_last_dequeue;
    /* Maps live in the heap context rather than growing the UI stack. */
    uint16_t scale_x[X4_WIDTH];
    uint32_t scale_y[X4_HEIGHT];
    unsigned scale_w, scale_h, scale_out_w, scale_out_h;
    uint64_t tick_calls, tick_us, tick_max_us, tick_packets;
    uint64_t draw_calls, draw_new, draw_repeat, draw_us, draw_max_us, last_draw_frame, last_presented_frame;
    uint64_t present_calls, present_us, present_max_us, report_time;
    X4LiveMediaSnapshot report_previous;
#if X4_VIDEO_PROBE
    bool local_probe;
    const X4ProbeCase *probe_case;
    X4ProbeMode probe_mode;
    X4ProbeMetrics *probe_metrics; /* Caller retains through all owner/helper joins. */
    int probe_output_memory_type;
    X4ProbeSource probe_source;
    X4ProbeThread probe_thread;
    X4ProbeHashContext probe_hash; /* Main reserves; sole probe owner uses; joined close releases. */
    X4ProbeStatus probe_owner, probe_view;
    atomic_bool probe_owner_done;
    uint64_t probe_origin_us, probe_last_admission_us;
    bool probe_admission_seen;
    unsigned probe_flush_calls;
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    /* Appended to preserve the offsets of unrelated media/audio state. */
    X4KeyframeLimiter keyframe_limiter;
    X4StreamPipelineSnapshot stream_worker_shadow;
    uint64_t keyframe_retry_us;
    X4StreamWait ingress_wait;
    X4StreamMutex stream_mailbox;
    bool stream_mailbox_initialized;
    unsigned stream_cpu_variant;
    X4StreamCpuReport stream_ingress_cpu;
    uint64_t ingress_wait_started_us, ingress_wait_elapsed_us;
    bool stream_audio_previous;
#endif
#if X4_RECOVERY43_MEDIA
    /* Appended diagnostics only; worker counters and main observations have
     * separate owners. Existing fields and audio/input offsets do not move. */
    uint64_t recovery43_incomplete, recovery43_reported_incomplete, recovery43_reported_discarded;
    uint64_t recovery43_last_presentations, recovery43_last_observed_us;
#endif
    /* Adaptive video has its own startup allocation; retain the legacy
     * embedded tickets and the offsets of all other media owners. */
    X4IngressTicket *adaptive_reorder_tickets;
#if X4_RECOVERY43_MEDIA
    uint64_t adaptive_completed, adaptive_capacity, adaptive_deadline, adaptive_oversized;
    uint64_t adaptive_returns, adaptive_late_returns, adaptive_report_us;
    uint16_t adaptive_hold_end;
    bool adaptive_holding, adaptive_hold_startup;
#endif
    bool grace43c, grace46b, startup_seen;
};
static bool mailbox_lock(X4LiveMedia *m)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(!m->stream_mailbox_initialized)return false;
    int rc=x4_stream_mutex_lock(&m->stream_mailbox);
    if(rc){atomic_store(&m->stop,true);printf("XCloud4: stream mailbox lock rc=%d\n",rc);return false;}
#else
    while (atomic_flag_test_and_set_explicit(&m->mailbox_gate, memory_order_acquire)) sceKernelUsleep(50);
#endif
    return true;
}
static void mailbox_unlock(X4LiveMedia *m) {
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    int rc=x4_stream_mutex_unlock(&m->stream_mailbox);
    if(rc){atomic_store(&m->stop,true);printf("XCloud4: stream mailbox unlock rc=%d\n",rc);}
#else
    atomic_flag_clear_explicit(&m->mailbox_gate, memory_order_release);
#endif
}
static inline void set_video_error(X4LiveMedia *m, int err)
{
    if (!m) return;
    m->video.error = err;
    if (err) atomic_store_explicit(&m->video_stopped, true, memory_order_release);
}
static void *video_worker(void *context);
#if X4_VIDEO_PROBE
static void *probe_video_worker(void *context);
#endif
static void note_damage(X4LiveMedia *m, uint32_t bit)
{
    if (!m->first_damage) {
        m->first_damage = bit;
        x4_trace_record(m->trace, MT_DAMAGE, 0, m->trace_au, bit);
    }
    m->damage_bits |= bit;
}
static void nack_au_stage(X4LiveMedia *m, unsigned stage)
{
    uint64_t now = x4_trace_now_us();
    for (unsigned i=0;i<m->au_ticket_count;++i) {
        const X4IngressTicket *t=&m->au_tickets[i];
        x4_trace_monitor_critical(m->trace, now, X4_MON_CR_NACK_AU_CONTRIBUTION,
            0xffffu, stage == 4 && atomic_load(&m->stop) ? 1u : 0u,
            t->identity, t->source, m->trace_au, stage, t->extended_sequence);
    }
}
static void reset_au(X4LiveMedia *m, bool gap, unsigned reason, unsigned detail)
{
    if (gap && m->au_ticket_count) nack_au_stage(m, 4);
    m->au_ticket_count = 0;
    bool waiting_before = m->waiting_keyframe;
    if (reason < RESET_COUNT) ++m->reset_counts[reason];
    uint16_t flags = (gap ? 1u : 0u) | (m->au_open ? 2u : 0u) | (m->fu ? 4u : 0u) |
        (m->idr ? 8u : 0u) | (m->damaged ? 16u : 0u) | (!m->au_size ? 32u : 0u) |
        (waiting_before ? 64u : 0u);
    if (gap && m->au_open) {
#if X4_RECOVERY43_MEDIA
        /* Exact assembler gap-reset count, also included in dropped_frames.
         * WAIT-IDR filtering is a deliberate discard, not a new network gap. */
        ++m->recovery43_incomplete;
#endif
        if (reason < RESET_COUNT) ++m->discard_counts[reason];
        for (unsigned i = 0; i < 13; ++i) if (m->damage_bits & (1u << i)) ++m->damage_counts[i];
    }
    if (gap) {
        if (m->au_open) { m->discard_timestamp = m->timestamp; m->discard_timestamp_valid = true; ++m->dropped_frames; }
        m->waiting_keyframe = true;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        /* Filtering an already-waiting P-frame is not a new loss. Ingress
         * already published its omission atomically in the callback. */
        if(reason!=RESET_WAIT && reason!=RESET_INGRESS) {
            x4_keyframe_limiter_observe_gap(&m->keyframe_limiter);
            x4_keyframe_limiter_request(&m->keyframe_limiter,X4_KEYFRAME_ASSEMBLY);
        }
        if(reason!=RESET_WAIT)m->keyframe_retry_us=sceKernelGetProcessTime();
#else
        atomic_store(&m->keyframe, true);
#endif
    }
    if (m->waiting_keyframe) flags |= 128u;
    x4_trace_record(m->trace, MT_AU_RESET, flags, m->trace_au,
        ((uint64_t)reason << 48) | ((uint64_t)(detail & 0xffffu) << 32) | m->damage_bits);
    if (!waiting_before && m->waiting_keyframe) {
        ++m->recovery_episodes; m->recovery_begin_us = sceKernelGetProcessTime();
        x4_trace_record(m->trace, MT_WAIT_BEGIN, (uint16_t)reason, m->trace_au, 0);
    }
    m->au_size = 0; m->fu = m->au_open = m->idr = m->damaged = false;
    m->damage_bits = m->first_damage = 0;
    m->au_sps_size = m->au_pps_size = 0;
}
static bool append(X4LiveMedia *m, const uint8_t *p, size_t size)
{
    if (size > X4_LIVE_AU_MAX - m->au_size) { note_damage(m, DM_CAPACITY); return false; }
    memcpy(m->au + m->au_size, p, size); m->au_size += size; return true;
}
static void cache_nal(X4LiveMedia *m, const uint8_t *p, size_t size)
{
    unsigned type = p[0] & 31;
    if (type == 5) { m->idr = true; x4_trace_record(m->trace, MT_IDR, 0, m->trace_au, 0); }
    if (size > 4092) return;
    uint8_t *to = type == 7 ? m->au_sps : type == 8 ? m->au_pps : NULL;
    if (to) { memcpy(to, "\0\0\0\1", 4); memcpy(to + 4, p, size); if (type == 7) m->au_sps_size = size + 4; else m->au_pps_size = size + 4; }
}
static bool nal(X4LiveMedia *m, const uint8_t *p, size_t size)
{
    if (!size || (p[0] & 128) || !(p[0] & 31) || (p[0] & 31) > 23) {
        note_damage(m, size && (p[0] & 128) ? DM_FORBIDDEN : DM_NAL_TYPE); return false;
    }
    if (!append(m, (const uint8_t *)"\0\0\0\1", 4) || !append(m, p, size)) return false;
    cache_nal(m, p, size); return true;
}
static void depacketize(X4LiveMedia *m, const X4LiveRtp *r, const X4IngressTicket *ticket)
{
    if (m->au_open && m->timestamp != r->timestamp) reset_au(m, true, RESET_TIMESTAMP, 0);
    if (!m->au_open) {
        m->au_open = true; m->timestamp = r->timestamp; m->trace_au = ++m->trace_au_next;
        x4_trace_record(m->trace, MT_AU_OPEN, m->waiting_keyframe ? 1u : 0u, m->trace_au, r->sequence);
    }
    if (ticket && ticket->identity) {
        if (m->au_ticket_count < 128) {
            m->au_tickets[m->au_ticket_count++] = *ticket;
            x4_trace_monitor_critical(m->trace, x4_trace_now_us(), X4_MON_CR_NACK_AU_CONTRIBUTION,
                0xffffu, 0, ticket->identity, ticket->source, m->trace_au, 0, ticket->extended_sequence);
        } else ++m->au_ticket_omissions;
    }
    if (m->discard_timestamp_valid) {
        if (r->timestamp == m->discard_timestamp) { note_damage(m, DM_DISCARDED); m->damaged = true; }
        else m->discard_timestamp_valid = false;
    }
    const uint8_t *p = r->payload; size_t size = r->size; unsigned type = p[0] & 31;
    bool ok = !(p[0] & 128);
    if (!ok) note_damage(m, DM_FORBIDDEN);
    if (ok && type >= 1 && type <= 23) { if (m->fu) { note_damage(m, DM_FU_ACTIVE); ok = false; } else ok = nal(m, p, size); }
    else if (ok && type == 24 && size > 1 && !m->fu) {
        size_t at = 1;
        while (ok && at < size) {
            if (size - at < 2) { note_damage(m, DM_STAP_SHORT); ok = false; break; }
            size_t length = be16(p + at); at += 2;
            if (!length || length > size - at) { note_damage(m, DM_STAP_LENGTH); ok = false; break; }
            ok = nal(m, p + at, length); at += length;
        }
        if (at != size) { note_damage(m, DM_STAP_TRAIL); ok = false; }
    } else if (ok && type == 28 && size > 2) {
        unsigned nt = p[1] & 31; bool start = p[1] & 128, end = p[1] & 64;
        if ((p[1] & 32) || !nt || nt > 23 || (start && end)) { note_damage(m, DM_FU_HEADER); ok = false; }
        else if (start) {
            if (m->fu) { note_damage(m, DM_FU_NESTED); ok = false; }
            else {
                uint8_t header = (p[0] & 0xe0) | nt;
                m->fu_start = m->au_size + 4; m->fu_type = header;
                ok = append(m, (const uint8_t *)"\0\0\0\1", 4) && append(m, &header, 1) && append(m, p + 2, size - 2);
                m->fu = ok;
            }
        } else if (!m->fu || ((p[0] & 0xe0) | nt) != m->fu_type) {
            note_damage(m, !m->fu ? DM_FU_ORPHAN : DM_FU_MISMATCH); ok = false;
        }
        else {
            ok = append(m, p + 2, size - 2);
            if (ok && end) { m->fu = false; cache_nal(m, m->au + m->fu_start, m->au_size - m->fu_start); }
        }
    } else {
        note_damage(m, type == 24 && m->fu ? DM_FU_ACTIVE : DM_PACKETIZATION); ok = false;
    }
    if (!ok) m->damaged = true;
    if (!r->marker) return;
    x4_trace_record(m->trace, MT_MARKER,
        (m->fu ? 1u : 0u) | (m->damaged ? 2u : 0u) | (!m->au_size ? 4u : 0u) | (m->idr ? 8u : 0u),
        m->trace_au, m->au_size);
    if (m->fu || m->damaged || !m->au_size) {
        reset_au(m, true, RESET_MARKER, (m->fu ? 1u : 0u) | (m->damaged ? 2u : 0u) | (!m->au_size ? 4u : 0u)); return;
    }
    /* Structural completion precedes recovery filtering. A complete P-frame
     * discarded while waiting for IDR is not a depacketization failure. */
    x4_trace_record(m->trace, X4_TRACE_AU_STRUCTURAL,
        (m->idr ? 1u : 0u) | (m->waiting_keyframe ? 2u : 0u) |
        (m->sps_size || m->au_sps_size ? 4u : 0u) |
        (m->pps_size || m->au_pps_size ? 8u : 0u), m->trace_au, m->au_size);
    nack_au_stage(m, 1);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    /* Count complete authenticated/reassembled AUs before recovery filtering;
     * receipt is neither decoder acceptance nor a presented image. */
    x4_stream_pipeline_au_received(m->stream, m->idr, sceKernelGetProcessTime());
#endif
    /* Parameter sets are committed only after the complete AU is valid.
     * A damaged STAP-A/FU-A cannot poison a later keyframe's cache. */
    if (m->au_sps_size) { memcpy(m->sps, m->au_sps, m->au_sps_size); m->sps_size = m->au_sps_size; }
    if (m->au_pps_size) { memcpy(m->pps, m->au_pps, m->au_pps_size); m->pps_size = m->au_pps_size; }
    if (m->waiting_keyframe && (!m->idr || !m->sps_size || !m->pps_size)) {
        nack_au_stage(m, 3);
        x4_trace_record(m->trace, X4_TRACE_AU_FILTER_REJECT,
            (!m->idr ? 1u : 0u) | (!m->sps_size ? 2u : 0u) |
            (!m->pps_size ? 4u : 0u), m->trace_au, m->au_size);
        reset_au(m, true, RESET_WAIT, (!m->idr ? 1u : 0u) | (!m->sps_size ? 2u : 0u) | (!m->pps_size ? 4u : 0u)); return;
    }
    if (m->waiting_keyframe) {
        size_t params = m->sps_size + m->pps_size;
        if (params > X4_LIVE_AU_MAX - m->au_size) { reset_au(m, true, RESET_PARAMS, 0); return; }
        memmove(m->au + params, m->au, m->au_size); memcpy(m->au, m->sps, m->sps_size); memcpy(m->au + m->sps_size, m->pps, m->pps_size); m->au_size += params;
    }
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
    x4_trace_record(m->trace, X4_TRACE_AU_VALID,
        (m->idr ? X4_TRACE_F_IDR : 0u) | (m->waiting_keyframe ? 1u : 0u),
        m->trace_au, m->au_size);
    /* Recovery accepted this AU for Decode; feed/output success is separate
     * and is not implied by this pre-Decode event. */
    nack_au_stage(m, 2);
    ++m->au_submitted;
#endif
    m->video.trace_au = m->trace_au;
    x4_trace_record(m->trace, MT_SUBMIT_BEGIN, m->waiting_keyframe ? 1u : 0u, m->trace_au, m->au_size);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    /* This is AU admission to the bounded decoder queue, not evidence of
     * native output. Recovery retains its existing pre-Decode boundary. */
    int rc = x4_stream_pipeline_submit_parameters(m->stream, m->au, m->au_size,
        (uint64_t)m->timestamp * 1000000 / 90000, m->trace_au, m->sps, m->sps_size,m->pps,m->pps_size);
    if (rc < 0) set_video_error(m, rc);
    if(rc==0) {
        ++m->au_submitted;
        x4_trace_record(m->trace,X4_TRACE_AU_VALID,
            (m->idr?X4_TRACE_F_IDR:0u)|(m->waiting_keyframe?1u:0u),m->trace_au,m->au_size);
        nack_au_stage(m,2);
    }
#else
    int rc = x4_live_video_feed(&m->video, m->au, m->au_size, (uint64_t)m->timestamp * 1000000 / 90000);
#endif
    x4_trace_record(m->trace, MT_SUBMIT_END, 0, m->trace_au, (uint32_t)rc);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(rc>0) {
        /* Queue deliberate omission: never pretend it was native acceptance,
         * never promote it to RESET_FEED or a fatal video error. The queue
         * publishes the one recovery intention through the common limiter. */
        nack_au_stage(m,3);++m->dropped_frames;
        bool was_waiting=m->waiting_keyframe;m->waiting_keyframe=true;
        if(!was_waiting){++m->recovery_episodes;m->recovery_begin_us=sceKernelGetProcessTime();
            x4_trace_record(m->trace,MT_WAIT_BEGIN,0x4000u,m->trace_au,(unsigned)rc);}
        reset_au(m,false,RESET_SUBMITTED,(unsigned)rc);return;
    }
#endif
    if (rc < 0) reset_au(m, true, RESET_FEED, 0);
    else {
        if (m->waiting_keyframe) {
            x4_trace_record(m->trace, MT_WAIT_END, 0, m->trace_au, 0);
            if (m->recovery_begin_us) {
                ++m->recovery_completed;
                m->recovery_us += sceKernelGetProcessTime() - m->recovery_begin_us;
                m->recovery_begin_us = 0;
            }
        }
        m->waiting_keyframe = false; reset_au(m, false, RESET_SUBMITTED, 0);
    }
}
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
static void keyframe_granted(void *context,uint64_t now)
{
    X4LiveMedia *m=context;
    uint64_t count=atomic_fetch_add_explicit(&m->requests,1,memory_order_relaxed)+1;
    x4_stream_pipeline_note_keyframe_request(m->stream,now);
    x4_trace_record(m->trace,MT_KEY_REQUEST,0,count,0);
}
static void assembly_keyframe_retry(X4LiveMedia *m,uint64_t now)
{
    if(m->waiting_keyframe && now>=m->keyframe_retry_us &&
        now-m->keyframe_retry_us>=X4_KEYFRAME_INTERVAL_US) {
        m->keyframe_retry_us=now;
        x4_keyframe_limiter_request(&m->keyframe_limiter,X4_KEYFRAME_ASSEMBLY);
    }
}
#endif
X4LiveMedia *x4_live_media_create(int *error)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4StreamRequestConfig request = x4_stream_settings_get();
    return x4_live_media_create_request(error, &request);
}

X4LiveMedia *x4_live_media_create_request(int *error, const X4StreamRequestConfig *request)
{
    if (!request || (unsigned)request->profile > X4_REQUEST_PROFILE_MAX ||
        (unsigned)request->color_preference > X4_COLOR_FORCE_601 || request->cpu_variant>X4_CPU_3 ||
        request->decoder_depth>X4_DEPTH_2) {
        if (error) *error = X4_LIVE_ERR_ARGUMENT; return NULL;
    }
    X4StreamRequestConfig selected = x4_stream_request_capture(request);
#endif
    int rc = X4_LIVE_ERR_MEMORY;
    X4LiveMedia *m = calloc(1, sizeof(*m));
    if (!m) { if (error) *error = rc; return NULL; }
    atomic_init(&m->keyframe, true); atomic_init(&m->ingress_gap, false);
    atomic_init(&m->receive_epoch_hold,false);atomic_init(&m->receive_epoch_callbacks,0);
    atomic_init(&m->receive_epoch_requested,0);atomic_init(&m->receive_epoch_applied,0);
    atomic_init(&m->retired_video_source,0);atomic_init(&m->receive_epoch_rejected,0);
    atomic_init(&m->receive_epoch_result,0);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    x4_keyframe_limiter_init(&m->keyframe_limiter);
    atomic_store_explicit(&m->keyframe,false,memory_order_relaxed);
#endif
    atomic_init(&m->packets, 0); atomic_init(&m->video_bytes, 0); atomic_init(&m->dropped, 0); atomic_init(&m->requests, 0);
    atomic_init(&m->started, false); atomic_init(&m->stop, false); atomic_init(&m->start_complete, false);
    atomic_init(&m->video_stopped, false);
    atomic_init(&m->post_halt_video_packets, 0); atomic_init(&m->first_post_halt_video_us, 0);
    atomic_init(&m->reset_maxima, false); atomic_init(&m->start_result, X4_LIVE_ERR_THREAD);
    atomic_init(&m->callback_active, 0); atomic_init(&m->callback_peak, 0);
    atomic_init(&m->callback_calls, 0); atomic_init(&m->callback_overlap, 0);
    atomic_init(&m->callback_us, 0); atomic_init(&m->callback_max_us, 0);
    atomic_init(&m->reserve_us, 0); atomic_init(&m->reserve_max_us, 0);
    atomic_init(&m->rx_invalid, 0);
    for (unsigned i = 0; i < 12; ++i) atomic_init(&m->callback_hist[i], 0);
    atomic_flag_clear(&m->mailbox_gate);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    m->stream_cpu_variant=selected.cpu_variant;
    m->stream_audio_previous=(selected.interaction_modes&X4_INTERACTION_AUDIO_PREVIOUS)!=0;
    rc=x4_stream_mutex_init(&m->stream_mailbox,"x4-assembly-view");
    if(rc){rc=rc<0?rc:-rc;goto fail;}
    m->stream_mailbox_initialized=true;
    rc=x4_stream_wait_init(&m->ingress_wait,"x4-ingress-work");
    if(rc<0)goto fail;
#endif
    m->published_slot = m->reader_slot = m->writer_slot = -1;
    m->audio_payload_type = -1;
    m->waiting_keyframe = true; x4_live_track_init(&m->track, -1);
    rc = x4_video_ingress_init(&m->ingress, 256);
    if (rc < 0) { rc = X4_LIVE_ERR_MEMORY; goto fail; }
    m->au = malloc(X4_LIVE_AU_MAX); if (!m->au) { rc = X4_LIVE_ERR_MEMORY; goto fail; }
    unsigned reorder_window = X4_LIVE_VIDEO_REORDER_LEGACY_CAPACITY;
    X4IngressTicket *reorder_tickets = m->reorder_tickets;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4StreamNackPolicy nack_policy = x4_stream_request_nack_policy(&selected);
    m->grace46b = nack_policy == X4_NACK_POLICY_T46B;
    m->grace43c = nack_policy == X4_NACK_POLICY_T43C || m->grace46b;
    if (nack_policy != X4_NACK_POLICY_T43) {
        reorder_window = X4_LIVE_VIDEO_REORDER_CAPACITY;
        m->adaptive_reorder_tickets = calloc(reorder_window, sizeof(*reorder_tickets));
        if (!m->adaptive_reorder_tickets) { rc = X4_LIVE_ERR_MEMORY; goto fail; }
        reorder_tickets = m->adaptive_reorder_tickets;
    }
#endif
    rc = x4_live_reorder_init_video(&m->reorder, reorder_window, 32, 25000, reorder_tickets);
    if (rc < 0) goto fail;
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
    for (unsigned i = 0; i < RGB_SLOTS; ++i) {
        m->rgb[i].pixels = calloc((size_t)X4_LIVE_RGB_PIXELS, sizeof(uint32_t));
        if (!m->rgb[i].pixels) { rc = X4_LIVE_ERR_MEMORY; goto fail; }
    }
#endif
    m->trace_session = x4_trace_now_us();
    x4_nack_grace_inbox_init(&m->grace_inbox);
    x4_nack_grace_init(&m->grace, m->trace_session, X4_NACK_GRACE_MS == 45);
    m->trace = x4_trace_create(m->trace_session, X4_LIVE_COPY_READERS);
    m->video.trace = m->trace;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    m->stream = x4_stream_pipeline_create_request(m->trace_session, m->trace, &m->stop, &selected);
    if (!m->stream) { rc = X4_LIVE_ERR_MEMORY; goto fail; }
    x4_stream_pipeline_set_keyframe_limiter(m->stream,&m->keyframe_limiter);
    x4_keyframe_limiter_set_observer(&m->keyframe_limiter,keyframe_granted,m);
    x4_keyframe_limiter_request(&m->keyframe_limiter,X4_KEYFRAME_STARTUP);
#endif
    /* Optional sampler owns no playback buffers. Bind before all producers;
     * queue storage is retained until its successful stop/join at close. */
    x4_trace_monitor_bind_queue(m->trace, &m->ingress);
    X4MonitorConfig config = {.width = 960, .height = 540, .max_fps = 30,
        .bitrate_kbps = 5000, .readers = X4_LIVE_COPY_READERS,
        .budget_us = WORKER_BUDGET_US, .decode_limit = WORKER_DECODE_LIMIT,
        .queue_capacity = 256, .reorder_capacity = reorder_window, .reorder_depth = 32,
        .reorder_wait_us = 25000, .h264_profile = 0x42e01f,
        .max_fs = 3600, .max_mbps = 108000};
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    config.width=selected.width; config.height=selected.height;
    config.max_fps=selected.fps; config.bitrate_kbps=selected.bitrate_kbps;
    config.h264_profile=selected.h264_profile;
    config.max_fs=selected.max_fs;
    config.max_mbps=selected.max_mbps;
    /* This legacy trace field counts CPU copy readers; direct scanout has
     * none. The new report separately identifies its two converters. */
    config.readers=0;
#endif
    x4_trace_monitor_config(m->trace, &config);
    x4_trace_monitor_epoch(m->trace, X4_MON_ACTOR_SESSION, X4_MON_EPOCH_LOADING);
    int monitor_rc = x4_trace_monitor_start(m->trace);
    printf("XCloud4: progress sampler start rc=%d period_us=100000\n", monitor_rc);
    printf("XCloud4: numeric trace enabled=%u configured_readers=%u\n", m->trace ? 1u : 0u,
        X4_LIVE_COPY_READERS);
    if (error) *error = 0; return m;
fail:
    if (error) *error = rc; x4_live_media_close(m); return NULL;
}
#if X4_VIDEO_PROBE
X4LiveMedia *x4_live_media_probe_create(const X4ProbeCase *sample, X4ProbeMode mode,
    X4ProbeMetrics *metrics, int *error)
{ return x4_live_media_probe_create_memory(sample, mode, metrics, 3, error); }
X4LiveMedia *x4_live_media_probe_create_memory(const X4ProbeCase *sample, X4ProbeMode mode,
    X4ProbeMetrics *metrics, int output_memory_type, int *error)
{
    int rc = X4_LIVE_ERR_ARGUMENT;
    if (!sample || !metrics || (output_memory_type != 0 && output_memory_type != 3) ||
        (mode != X4_PROBE_PACED && mode != X4_PROBE_MAX_SPEED) ||
        !sample->visible_width || !sample->visible_height ||
        sample->visible_width > X4_LIVE_RGB_WIDTH || sample->visible_height > X4_LIVE_RGB_HEIGHT) {
        if (error) *error = rc; return NULL;
    }
    X4LiveMedia *m = calloc(1, sizeof(*m));
    if (!m) { if (error) *error = X4_LIVE_ERR_MEMORY; return NULL; }
    m->local_probe = true; m->probe_case = sample; m->probe_mode = mode; m->probe_metrics = metrics;
    m->probe_owner = x4_probe_status_pending();
    m->probe_output_memory_type = output_memory_type;
    m->probe_owner.output_memory_type = output_memory_type;
    m->probe_owner.output_memory_protection = 0x33;
    m->video.probe_status = m->probe_view = m->probe_owner;
    atomic_init(&m->probe_owner_done, false);
    atomic_init(&m->keyframe, false); atomic_init(&m->ingress_gap, false);
    atomic_init(&m->packets, 0); atomic_init(&m->video_bytes, 0); atomic_init(&m->dropped, 0); atomic_init(&m->requests, 0);
    atomic_init(&m->started, false); atomic_init(&m->stop, false); atomic_init(&m->start_complete, false);
    atomic_init(&m->video_stopped, false);
    atomic_init(&m->post_halt_video_packets, 0); atomic_init(&m->first_post_halt_video_us, 0);
    atomic_init(&m->reset_maxima, false); atomic_init(&m->start_result, X4_LIVE_ERR_THREAD);
    atomic_init(&m->callback_active, 0); atomic_init(&m->callback_peak, 0);
    atomic_init(&m->callback_calls, 0); atomic_init(&m->callback_overlap, 0);
    atomic_init(&m->callback_us, 0); atomic_init(&m->callback_max_us, 0);
    atomic_init(&m->reserve_us, 0); atomic_init(&m->reserve_max_us, 0); atomic_init(&m->rx_invalid, 0);
    for (unsigned i = 0; i < 12; ++i) atomic_init(&m->callback_hist[i], 0);
    atomic_flag_clear(&m->mailbox_gate);
    m->published_slot = m->reader_slot = m->writer_slot = -1; m->audio_payload_type = -1;
    rc = X4_LIVE_ERR_MEMORY;
    if (x4_probe_hash_context_init(&m->probe_hash)) goto fail;
    m->au = malloc(X4_LIVE_AU_MAX); if (!m->au) goto fail;
    for (unsigned i = 0; i < RGB_SLOTS; ++i) {
        m->rgb[i].pixels = calloc((size_t)X4_LIVE_RGB_PIXELS, sizeof(uint32_t));
        if (!m->rgb[i].pixels) goto fail;
    }
    m->trace_session = x4_trace_now_us();
    m->trace = x4_trace_create(m->trace_session, X4_LIVE_COPY_READERS);
    if (!m->trace) goto fail; /* Without the production event bridge, no measured pass is claimed. */
    m->video.trace = m->trace; x4_trace_probe_metrics(m->trace, metrics);
    /* Main owns trace cadence activation, before the video owner starts.
     * Only actual media draws call present_trace; startup UI flips do not. */
    x4_trace_set_active(m->trace, true);
    if (error) *error = 0; return m;
fail:
    if (error) *error = rc; (void)x4_live_media_close(m); return NULL;
}
void x4_live_media_probe_request_cancel(X4LiveMedia *m)
{
    if (m && m->local_probe) atomic_store_explicit(&m->stop, true, memory_order_release);
}
int x4_live_media_probe_snapshot(const X4LiveMedia *m, X4ProbeStatus *out)
{
    if (!m || !m->local_probe || !out) return X4_LIVE_ERR_ARGUMENT;
    X4LiveMedia *mutable = (X4LiveMedia *)m;
    mailbox_lock(mutable); *out = m->probe_view; mailbox_unlock(mutable);
    out->cancel_requested = atomic_load_explicit(&m->stop, memory_order_acquire);
    return 0;
}
bool x4_live_media_probe_owner_done(const X4LiveMedia *m)
{
    return m && m->local_probe && atomic_load_explicit(&m->probe_owner_done, memory_order_acquire);
}
#endif
int x4_live_media_start(X4LiveMedia *m)
{
#if X4_VIDEO_PROBE
    if (m && m->local_probe && !m->running && !atomic_load(&m->start_complete) && atomic_load(&m->stop)) {
        /* Main owns start/running. Cancellation before creation leaves no
         * owner to join; report that fact rather than a fictitious timeout. */
        mailbox_lock(m); m->probe_view.finished = true;
        m->probe_view.pipeline_result = X4_PROBE_CANCELLED; mailbox_unlock(m);
        atomic_store_explicit(&m->probe_owner_done, true, memory_order_release);
        return X4_LIVE_ERR_STATE;
    }
#endif
    if (!m || m->running || atomic_load(&m->started) || atomic_load(&m->stop) || atomic_load(&m->start_complete)) return X4_LIVE_ERR_STATE;
#if X4_VIDEO_PROBE
    int rc;
    if (m->local_probe) {
        rc = x4_probe_thread_create(&m->thread, &m->probe_thread, video_worker, m, "x4-video");
        m->running = m->probe_thread.created; /* Includes aborted created threads needing join. */
        if (rc && !m->running) x4_probe_metrics_record_stack(m->probe_metrics, 0, &m->probe_thread.report);
    } else
        rc = scePthreadCreate(&m->thread, NULL, video_worker, m, "x4-video");
#elif X4_STREAM_PROFILE
    int rc = x4_probe_thread_create(&m->thread, &m->stream_ingress_thread,
        video_worker, m, "x4-video");
    m->running = m->stream_ingress_thread.created;
#else
    int rc = scePthreadCreate(&m->thread, NULL, video_worker, m, "x4-video");
#endif
    if (rc) {
#if X4_VIDEO_PROBE
        if (m->local_probe) {
            mailbox_lock(m); m->probe_view.finished = true;
            m->probe_view.pipeline_result = rc < 0 ? rc : -rc; mailbox_unlock(m);
            atomic_store_explicit(&m->probe_owner_done, true, memory_order_release);
        }
#endif
        return rc < 0 ? rc : -rc;
    }
    m->running = true;
#if X4_VIDEO_PROBE
    if (m->local_probe) return 0; /* UI stays responsive during file/native preparation. */
#endif
    /* Native initialization was synchronous before this change too. All
     * native video calls now remain on the same owning thread. */
    while (!atomic_load_explicit(&m->start_complete, memory_order_acquire)) sceKernelUsleep(1000);
    rc = atomic_load(&m->start_result); if (rc < 0) return rc;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    m->audio = x4_live_audio_start_mode(&m->audio_error, m->audio_payload_type, m->stream_audio_previous);
#else
    m->audio = x4_live_audio_start(&m->audio_error, m->audio_payload_type);
#endif
    m->report_time = sceKernelGetProcessTime(); atomic_store_explicit(&m->started, true, memory_order_release);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    (void)x4_stream_wait_notify(&m->ingress_wait);
#endif
    return 0;
}
int x4_live_media_set_payload_type(X4LiveMedia *m, int kind, int pt)
{
#if X4_VIDEO_PROBE
    if (m && m->local_probe) return X4_LIVE_ERR_STATE;
#endif
    if (!m || m->running || atomic_load(&m->started) || atomic_load(&m->stop) || pt < 0 || pt > 127) return -1;
    if (kind == X4_LIVE_KIND_VIDEO) m->track.forced_type = pt;
    else if (kind == X4_LIVE_KIND_AUDIO) m->audio_payload_type = pt;
    else return -1;
    return 0;
}
/* Bounded best observed maxima; a missed simultaneous larger update is
 * possible under an unsupported extreme caller count. No waiting loop. */
static void callback_max(atomic_uint_fast64_t *counter, uint64_t value)
{
    uint_fast64_t old = atomic_load_explicit(counter, memory_order_relaxed);
    for (unsigned n = 0; n < 16 && old < value; ++n)
        if (atomic_compare_exchange_strong_explicit(counter, &old, value,
            memory_order_relaxed, memory_order_relaxed)) break;
}
static void receive_current_epoch(void *context, int kind, const uint8_t *p, size_t size, bool proven, uint64_t epoch)
{
    X4LiveMedia *m = context; if (!m || !atomic_load_explicit(&m->started, memory_order_acquire) || atomic_load(&m->stop)) return;
#if X4_VIDEO_PROBE
    if (m->local_probe) return; /* Source is a complete local AU, never a Track callback. */
#endif
    if (kind == X4_LIVE_KIND_AUDIO) {
        X4LiveRtp audio_r;
        uint64_t arrival = x4_trace_now_us();
        bool valid = x4_live_rtp_parse(p, size, &audio_r) >= 0;
        x4_trace_monitor_rx(m->trace, X4_MON_RX_AUDIO, valid,
            valid ? audio_r.ssrc : 0, valid ? audio_r.sequence : 0, size, arrival);
        if(proven)x4_live_audio_receive_for_epoch(m->audio,p,size,epoch);
        else x4_live_audio_receive(m->audio,p,size);
        return;
    }
    if (kind != X4_LIVE_KIND_VIDEO) return;
    if (atomic_load_explicit(&m->video_stopped, memory_order_acquire) ||
        !atomic_load_explicit(&m->ingress.accepting, memory_order_acquire)) {
        uint64_t post_halt_now = sceKernelGetProcessTime();
        atomic_fetch_add_explicit(&m->post_halt_video_packets, 1, memory_order_relaxed);
        uint64_t empty = 0;
        (void)atomic_compare_exchange_strong_explicit(&m->first_post_halt_video_us, &empty, post_halt_now,
            memory_order_release, memory_order_relaxed);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        x4_stream_pipeline_note_post_halt_video(m->stream, post_halt_now);
#endif
        return;
    }
    uint64_t callback_begin = sceKernelGetProcessTime();
    unsigned active = atomic_fetch_add_explicit(&m->callback_active, 1, memory_order_relaxed) + 1;
    unsigned peak = atomic_load_explicit(&m->callback_peak, memory_order_relaxed);
    for (unsigned n = 0; n < 16 && peak < active; ++n)
        if (atomic_compare_exchange_strong_explicit(&m->callback_peak, &peak, active,
            memory_order_relaxed, memory_order_relaxed)) break;
    if (active > 1) atomic_fetch_add_explicit(&m->callback_overlap, 1, memory_order_relaxed);
    X4LiveRtp r;
    unsigned parse_reason = 0;
    int parsed = rtp_parse_reason(p, size, &r, &parse_reason);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if (parsed >= 0) x4_stream_pipeline_received(m->stream, size, callback_begin);
#endif
    x4_trace_monitor_rx(m->trace, X4_MON_RX_VIDEO, parsed >= 0,
        parsed >= 0 ? r.ssrc : 0, parsed >= 0 ? r.sequence : 0, size, callback_begin);
    X4IngressResult result = { .reason = X4_INGRESS_INVALID };
    X4NackPacket delivered = x4_rtc_nack_delivery_ticket();
    X4IngressTicket ticket = {.identity=delivered.identity,.source=delivered.source,
        .extended_sequence=delivered.extended_sequence,.rtp_timestamp=delivered.rtp_timestamp,
        .authenticated_us=delivered.authenticated_us,.sequence=delivered.sequence};
    uint64_t source = parsed >= 0 ? (1ull<<63) | ((uint64_t)r.payload_type<<32) | r.ssrc : 0;
    if (parsed < 0 || ticket.source != source || (uint16_t)ticket.extended_sequence != r.sequence ||
        ticket.rtp_timestamp != r.timestamp) ticket = (X4IngressTicket){0};
    bool pushed = parsed >= 0 && x4_video_ingress_push_ticket(&m->ingress, p, size, callback_begin, &result, &ticket);
    if(ticket.identity) x4_trace_monitor_critical(m->trace,callback_begin,X4_MON_CR_NACK_GRACE,
        GR_CALLBACK,result.reason,ticket.identity,ticket.source,
        ticket.extended_sequence,ticket.authenticated_us,callback_begin);
    if (parsed < 0) atomic_fetch_add_explicit(&m->rx_invalid, 1, memory_order_relaxed);
    if (parsed < 0 || !pushed) {
        atomic_fetch_add(&m->dropped, 1);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        x4_stream_pipeline_note_callback_reject(m->stream,parsed>=0 && result.reason==X4_INGRESS_FULL);
        if(!atomic_exchange_explicit(&m->ingress_gap,true,memory_order_acq_rel)) {
            x4_keyframe_limiter_observe_gap(&m->keyframe_limiter);
            x4_keyframe_limiter_request(&m->keyframe_limiter,X4_KEYFRAME_ASSEMBLY);
        }
#else
        atomic_store(&m->ingress_gap, true); atomic_store(&m->keyframe, true);
#endif
    }
    else {
        atomic_fetch_add(&m->packets, 1);
        atomic_fetch_add(&m->video_bytes, size);
    }
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    /* Publication precedes the retained wake token. This callback never waits
     * for the assembler, Decode or any consumer-owned mutex. */
    (void)x4_stream_wait_notify(&m->ingress_wait);
#endif
    /* Exact reservation branch, not a racy difference of queue counters.
     * No video consumer lock exists. seq16 alone is not proof of loss. */
    if (parsed < 0) x4_trace_record(m->trace, MT_RX_REJECT, X4_TRACE_REASON(X4_TRACE_R_RTP_INVALID), size, parse_reason);
    else {
        x4_trace_record(m->trace, MT_RTP, (r.marker ? X4_TRACE_F_MARKER : 0u) |
            (pushed ? X4_TRACE_F_OBSERVED_METADATA : 0u), r.sequence, size);
        if (!pushed) x4_trace_record(m->trace, MT_QUEUE_REJECT,
            X4_TRACE_REASON(result.reason == X4_INGRESS_CONTENDED ? X4_TRACE_R_PUSH_CONTENDED :
                result.reason == X4_INGRESS_FULL ? X4_TRACE_R_QUEUE_FULL :
                result.reason == X4_INGRESS_STOPPED ? X4_TRACE_R_USER_STOP : X4_TRACE_R_SIZE),
            r.sequence, ((uint64_t)result.owner_sample << 32) | x4_video_ingress_depth(&m->ingress));
    }
    x4_trace_poll(m->trace);
    uint64_t elapsed = sceKernelGetProcessTime() - callback_begin;
    atomic_fetch_add_explicit(&m->callback_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->callback_us, elapsed, memory_order_relaxed);
    callback_max(&m->callback_max_us, elapsed);
    atomic_fetch_add_explicit(&m->reserve_us, result.reserve_us, memory_order_relaxed);
    callback_max(&m->reserve_max_us, result.reserve_us);
    unsigned bucket = 0;
    while (bucket < 11 && elapsed > (2ull << bucket)) ++bucket;
    atomic_fetch_add_explicit(&m->callback_hist[bucket], 1, memory_order_relaxed);
    /* One bounded numeric record; no payload/SSRC/PTS or per-packet print.
     * Durations saturate at UINT32_MAX. Reservation time is monotonic
     * elapsed, including scheduling; no intentional sleep is added to the
     * callback. The final diagnostic append is outside elapsed. */
    uint64_t cb32 = elapsed > UINT32_MAX ? UINT32_MAX : elapsed;
    uint64_t reserve32 = result.reserve_us > UINT32_MAX ? UINT32_MAX : result.reserve_us;
    x4_trace_record(m->trace, X4_TRACE_RX_CALLBACK,
        (uint16_t)(result.retries | (pushed ? 64u : 0u) | (parsed < 0 ? 128u : 0u)),
        ((uint64_t)active << 32) | (parsed >= 0 ? r.sequence : 0u), (cb32 << 32) | reserve32);
    atomic_fetch_sub_explicit(&m->callback_active, 1, memory_order_relaxed);
}
/* No lock/allocation/sleep in this callback. seq_cst pairs entry with the
 * epoch gate: an old entrant remains counted, or observes hold and returns. */
static void receive_epoch_entry(void *context,int kind,const uint8_t *p,size_t size,bool proven,uint64_t epoch)
{
    X4LiveMedia *m=context;if(!m)return;
#if X4_VIDEO_PROBE
    if(m->local_probe)return;
#endif
    atomic_fetch_add_explicit(&m->receive_epoch_callbacks,1,memory_order_seq_cst);
    bool reject=atomic_load_explicit(&m->receive_epoch_hold,memory_order_seq_cst);
    if(proven&&epoch!=atomic_load_explicit(&m->receive_epoch_applied,memory_order_acquire))reject=true;
    if(!reject&&!proven&&kind==X4_LIVE_KIND_VIDEO){
        X4LiveRtp r;
        uint64_t retired=atomic_load_explicit(&m->retired_video_source,memory_order_acquire);
        reject=retired&&x4_live_rtp_parse(p,size,&r)>=0&&(uint32_t)retired==r.ssrc;
    }
    if(reject)atomic_fetch_add_explicit(&m->receive_epoch_rejected,1,memory_order_relaxed);
    else receive_current_epoch(context,kind,p,size,proven,epoch);
    atomic_fetch_sub_explicit(&m->receive_epoch_callbacks,1,memory_order_seq_cst);
}

void x4_live_media_receive(void *context,int kind,const uint8_t *p,size_t size)
{receive_epoch_entry(context,kind,p,size,false,0);}
uint64_t x4_live_media_current_epoch(const X4LiveMedia *m)
{return m?atomic_load_explicit(&m->receive_epoch_applied,memory_order_acquire):0;}
void x4_live_media_receive_for_epoch(void *context,int kind,const uint8_t *p,size_t size,uint64_t epoch)
{receive_epoch_entry(context,kind,p,size,true,epoch);}

static void grace_record(X4LiveMedia *m,unsigned kind,unsigned flags,uint64_t identity,
    uint64_t source,uint64_t a,uint64_t b,uint64_t c)
{
    x4_trace_monitor_critical(m->trace,x4_trace_now_us(),X4_MON_CR_NACK_GRACE,
        kind,flags,identity,source,a,b,c);
}
static uint64_t grace_source(const X4LiveMedia *m)
{ return (UINT64_C(1)<<63)|((uint64_t)m->track.payload_type<<32)|m->track.ssrc; }
static uint32_t grace_span(const X4LiveMedia *m)
{
    return x4_live_reorder_span(&m->reorder);
}
static void grace_effects(X4LiveMedia *m,X4NackGraceEffects effects)
{
    uint64_t changed=effects.completed|effects.deadline|effects.capacity|effects.passed|effects.invalidated|effects.distance;
    for(unsigned i=0;i<64;++i) if(changed&(UINT64_C(1)<<i)) {
        X4NackGraceRange *r=&m->grace.ranges[i];
        unsigned offset=(uint16_t)(m->reorder.expected-(uint16_t)r->first_extended);
        if(x4_nack_grace_release_forces_gap(r->release_reason)&&offset<64&&
            ((r->granted_mask&~r->accepted_mask)&(UINT64_C(1)<<offset))) {
            m->grace_force_gap=true; m->grace_force_ranges|=UINT64_C(1)<<i;
        }
        uint64_t source=(UINT64_C(1)<<63)|((uint64_t)m->track.payload_type<<32)|r->ssrc;
        grace_record(m,GR_RELEASE,r->release_reason,r->identity,source,
            r->deadline_us,r->accepted_mask,m->reorder.buffered);
        grace_record(m,GR_WORK_COST,0,r->identity,source,
            m->video.decode_us+m->video_totals.decode_us-m->grace_decode_start[i],
            m->video.copy_us+m->video_totals.copy_us-m->grace_copy_start[i],
            m->video.convert_us+m->video_totals.convert_us-m->grace_convert_start[i]);
        uint64_t now=x4_trace_now_us(),oldest=x4_video_ingress_oldest(&m->ingress);
        grace_record(m,GR_QUEUE_AGE,0,r->identity,source,
            oldest&&now>=oldest?now-oldest:0,x4_video_ingress_depth(&m->ingress),m->grace_max_dwell[i]);
    }
}
static void grace_binding_reject(X4LiveMedia *m,unsigned flags,uint32_t sequence_epoch,
    uint64_t identity,uint64_t source,uint64_t a,uint64_t b,uint64_t c)
{
    if(m->binding_reject_valid && m->binding_reject_source==source &&
        m->binding_reject_epoch==sequence_epoch && m->binding_reject_flags==flags) {
        ++m->binding_reject_omitted; return;
    }
    m->binding_reject_valid=true; m->binding_reject_source=source;
    m->binding_reject_epoch=sequence_epoch; m->binding_reject_flags=flags;
    grace_record(m,GR_BIND_REJECT,flags,identity,source,a,b,c);
}
static void grace_binding_result(X4LiveMedia *m,int result,const X4NackSequenceTag *tag,uint64_t source)
{
    uint64_t identity=((uint64_t)tag->transport_epoch<<32)|tag->sequence_epoch;
    if(result==X4_BINDING_CANDIDATE || result==X4_BINDING_PROOF || result==X4_BINDING_COMMITTED) {
        /* An announced or media-proven transition removes old holds. It does
         * not force a gap, flush media, restart recovery or refund attempts. */
        grace_effects(m,x4_nack_grace_unbind_source(&m->grace));
        m->grace_force_gap=false; m->grace_force_ranges=0;
    }
    if(result==X4_BINDING_COMMITTED) {
        grace_effects(m,x4_nack_grace_bind_source(&m->grace,(uint32_t)source,tag->sequence_epoch));
        grace_record(m,GR_BIND_COMMIT,tag->reason,identity,source,tag->anchor_extended,
            tag->anchor_extended+m->grace_binding.canonical_offset,m->grace_binding.canonical_offset);
    } else if(result==X4_BINDING_CANDIDATE) {
        grace_record(m,GR_BIND_CANDIDATE,tag->reason,identity,source,tag->anchor_extended,
            tag->previous_high_extended,tag->previous_epoch);
    } else if(result==X4_BINDING_PROOF) {
        grace_record(m,GR_BIND_PROOF,tag->reason,identity,source,tag->anchor_extended,
            tag->anchor_rtp_timestamp,m->grace_binding.proof_authenticated_us);
    } else if(result!=X4_BINDING_NONE && result!=X4_BINDING_DUPLICATE && result!=X4_BINDING_UNPROVEN) {
        grace_binding_reject(m,(unsigned)result|(tag->reason<<8),tag->sequence_epoch,identity,source,
            tag->anchor_extended,tag->previous_high_extended,m->grace_binding.committed.sequence_epoch);
    }
}
static bool grace_ticket_map(X4LiveMedia *m,const X4IngressTicket *ticket,
    const X4LiveRtp *rtp,uint64_t *canonical)
{
    if(ticket->source!=grace_source(m) || ticket->rtp_timestamp!=rtp->timestamp ||
        rtp->payload_type!=102 || rtp->ssrc!=m->track.ssrc) return false;
    return x4_nack_binding_map(&m->grace_binding,&ticket->sequence,ticket->source,
        ticket->extended_sequence,rtp->sequence,rtp->timestamp,ticket->authenticated_us,
        x4_trace_now_us(),canonical);
}
static void grace_controls(X4LiveMedia *m)
{
    uint64_t omitted=atomic_load_explicit(&m->grace_inbox.omitted,memory_order_relaxed);
    if(omitted!=m->grace_seen_omissions) {
        m->grace_seen_omissions=omitted;
        /* Unknown control history must not extend a hold. Keep NACK enabled,
         * but irreversibly disable grace for this session. */
        grace_effects(m,x4_nack_grace_close_reason(&m->grace,X4_GRACE_RELEASE_EXPLICIT));
        grace_record(m,GR_COUNTER,0,0,0,101,omitted,0);
    }
    if(!m->track.locked) return; /* First accepted packet owns source binding. */
    X4NackGraceControl control;
    for(unsigned n=0;n<X4_GRACE_INBOX_CAP&&x4_nack_grace_inbox_peek(&m->grace_inbox,&control);++n) {
        uint64_t now=x4_trace_now_us();
        uint32_t transport=(uint32_t)(control.identity>>32);
        uint32_t owner=atomic_load_explicit(&m->grace_inbox.epoch,memory_order_relaxed);
        unsigned ordinal=(uint32_t)control.identity;
        unsigned highest=0;
        for(uint64_t bits=control.original_mask;bits>1;bits>>=1) ++highest;
        bool request_shape=ordinal && ordinal<=64 && control.sequence_epoch && control.original_mask &&
            control.detected_us<=now && control.detected_us<=UINT64_MAX-X4_NACK_GRACE_US &&
            control.deadline_us==control.detected_us+X4_NACK_GRACE_US &&
            control.first_extended<=UINT64_MAX-highest;
        bool request_seen=ordinal && ordinal<=64 &&
            (m->grace.seen_request_ids&(UINT64_C(1)<<(ordinal-1)));
        /* Waiting preserves the existing FIFO cell, never stops media. It
         * ends at the original absolute deadline even if no anchor appears.
         * A later proven anchor may bind future requests, not renew this one. */
        if(control.kind==X4_NACK_GRACE_REQUEST && transport==owner && !m->grace.closed &&
            request_shape && !request_seen &&
            x4_nack_binding_waiting(&m->grace_binding,control.source,transport,control.sequence_epoch) &&
            control.detected_us<=UINT64_MAX-X4_NACK_GRACE_US &&
            now<control.detected_us+X4_NACK_GRACE_US) break;
        if(!x4_nack_grace_inbox_pop(&m->grace_inbox,&control)) break;
        ++m->grace_controls;
        if(!owner || transport!=owner || control.source!=grace_source(m)) {
            grace_binding_reject(m,X4_BINDING_STALE,control.sequence_epoch,control.identity,control.source,
                control.first_extended,control.sequence_epoch,owner);
            continue;
        }
        if(control.kind==X4_NACK_GRACE_SEQUENCE) {
            X4NackSequenceTag tag={.transport_epoch=transport,.sequence_epoch=control.sequence_epoch,
                .anchor_extended=control.first_extended,.previous_high_extended=control.original_mask,
                .previous_epoch=(uint32_t)control.selected_mask,.anchor_us=control.detected_us,
                .anchor_rtp_timestamp=(uint32_t)control.attempt_start_us,.reason=control.reason};
            int result=(control.selected_mask>UINT32_MAX || control.attempt_start_us>UINT32_MAX ||
                (uint32_t)control.identity!=control.sequence_epoch || control.deadline_us ||
                control.local_return_us || control.local_result) ? X4_BINDING_INVALID :
                x4_nack_binding_announce(&m->grace_binding,&tag,control.source,now);
            if(result==X4_BINDING_INVALID && tag.sequence_epoch>=m->grace_binding.committed.sequence_epoch) {
                /* Unknown/contradictory newer provenance cannot keep old
                 * protection alive. It also cannot establish a new binding. */
                grace_effects(m,x4_nack_grace_unbind_source(&m->grace));
                m->grace_force_gap=false; m->grace_force_ranges=0;
            }
            grace_binding_result(m,result,&tag,control.source);
        } else if(control.kind==X4_NACK_GRACE_REQUEST) {
            grace_record(m,GR_CONTROL_REQUEST,0,control.identity,control.source,
                control.first_extended,control.original_mask,control.detected_us);
            uint64_t canonical=control.first_extended;
            bool mapped=x4_nack_binding_offer_map(&m->grace_binding,control.source,transport,
                control.sequence_epoch,control.first_extended,control.original_mask,&canonical);
            X4NackGraceRequest request={m->trace_session,control.identity,
                canonical,control.original_mask,control.detected_us,
                (uint32_t)control.source,control.sequence_epoch};
            X4NackGraceEffects effects;
            int offer;
            if(!request_shape) {
                effects=(X4NackGraceEffects){0};
                ++m->grace.counters.offers; ++m->grace.counters.invalid_offers;
                offer=X4_GRACE_OFFER_INVALID;
            } else if(mapped) {
                grace_record(m,GR_BIND_MAP,0,control.identity,control.source,
                    control.first_extended,canonical,control.sequence_epoch);
                offer=x4_nack_grace_offer(&m->grace,&request,now,&effects);
                /* A coherent current-owner attempt can map while grace was
                 * safely unbound by a contradictory transition. Consumption
                 * still burns its ID: a later proven rebase cannot revive it. */
                if(offer==X4_GRACE_OFFER_STALE_SOURCE)
                    m->grace.seen_request_ids|=UINT64_C(1)<<(ordinal-1);
            } else {
                /* Preserve offer accounting without allowing an unproven
                 * tag to initialize a source or reinterpret an old mask. */
                effects=(X4NackGraceEffects){0};
                ++m->grace.counters.offers;
                if(request_seen) {
                    ++m->grace.counters.duplicate_offers; offer=X4_GRACE_OFFER_DUPLICATE;
                } else {
                    /* Burn the consumed opaque attempt ID, without storing
                     * its unproven raw mask as a canonical tombstone. A later
                     * copy cannot receive a renewed grace deadline. */
                    m->grace.seen_request_ids|=UINT64_C(1)<<(ordinal-1);
                    ++m->grace.counters.stale_source_offers; offer=X4_GRACE_OFFER_STALE_SOURCE;
                }
                grace_binding_reject(m,X4_BINDING_UNPROVEN,control.sequence_epoch,
                    control.identity,control.source,control.first_extended,control.deadline_us,now);
            }
            grace_effects(m,effects);
            X4NackGraceRange *r=ordinal&&ordinal<=64?&m->grace.ranges[ordinal-1]:NULL;
            grace_record(m,GR_OFFER,0,control.identity,control.source,
                control.deadline_us,r?r->granted_mask:0,((uint64_t)m->reorder.buffered<<32)|(unsigned)offer);
            if(offer==X4_GRACE_OFFER_ADMITTED) {
                unsigned i=ordinal-1;
                m->grace_decode_start[i]=m->video.decode_us+m->video_totals.decode_us;
                m->grace_copy_start[i]=m->video.copy_us+m->video_totals.copy_us;
                m->grace_convert_start[i]=m->video.convert_us+m->video_totals.convert_us;
                /* Controls may reach the worker after a requested packet was
                 * already STORED. Callback/AUTH alone never clear pending. */
                for(unsigned slot=0;slot<m->reorder.window;++slot) if(m->reorder.slots[slot].used) {
                    X4LiveRtp rtp;
                    const X4LiveReorderSlot *stored=&m->reorder.slots[slot];
                    uint64_t position;
                    if(x4_live_rtp_parse(m->reorder.data+(size_t)slot*X4_LIVE_RTP_MAX,stored->size,&rtp)>=0 &&
                        grace_ticket_map(m,&m->reorder_tickets[slot],&rtp,&position))
                        grace_effects(m,x4_nack_grace_accepted_extended(&m->grace,rtp.ssrc,
                            m->reorder_tickets[slot].sequence.sequence_epoch,position,x4_trace_now_us()));
                }
            }
        } else if(control.kind==X4_NACK_GRACE_OUTCOME) {
            /* Receipt retains the local return clock, including retired old
             * epochs. Recording it is not permission to cancel worker grace. */
            grace_record(m,GR_CONTROL_OUTCOME,(((uint32_t)control.local_result+2u)&255u)|(control.reason<<8),
                control.identity,control.source,control.attempt_start_us,control.local_return_us,control.selected_mask);
            unsigned ordinal=(uint32_t)control.identity;
            const X4NackGraceRange *range=ordinal && ordinal<=64 ? &m->grace.ranges[ordinal-1] : NULL;
            if(!m->grace_binding.bound || m->grace_binding.candidate_valid ||
                control.sequence_epoch!=m->grace_binding.committed.sequence_epoch ||
                !range || range->identity!=control.identity || range->ssrc!=(uint32_t)control.source ||
                range->sequence_epoch!=control.sequence_epoch) {
                grace_binding_reject(m,X4_BINDING_STALE,control.sequence_epoch,control.identity,control.source,
                    control.first_extended,control.sequence_epoch,m->grace_binding.committed.sequence_epoch);
                continue;
            }
            if((control.reason==X4_NACK_GRACE_ATTEMPT && control.local_result!=1) ||
                control.reason==X4_NACK_GRACE_CANCEL_RETIRED || control.reason==X4_NACK_GRACE_CANCEL_CLOSING)
                grace_effects(m,x4_nack_grace_cancel(&m->grace,control.identity,X4_GRACE_RELEASE_TRANSPORT));
            /* AUTH-complete cancels a send watch, not worker grace. */
        }
    }
}
static X4NackGraceDecision grace_query(X4LiveMedia *m,uint32_t prospective_offset,bool capacity_exhausted)
{
    grace_controls(m);
    if(!m->grace.active_count) return (X4NackGraceDecision){0};
    uint32_t span=grace_span(m);
    if(prospective_offset>span) span=prospective_offset;
    X4NackGraceDecision decision=x4_nack_grace_query_video(&m->grace,m->reorder.expected,
        m->reorder.buffered,span,x4_live_reorder_advance_bound(&m->reorder),
        capacity_exhausted,x4_trace_now_us());
    grace_effects(m,decision.effects);
    if(decision.reason==X4_GRACE_HOLD) {
        unsigned ordinal=(uint32_t)decision.identity;
        uint64_t bit=UINT64_C(1)<<(ordinal-1);
        if(!(m->grace_hold_logged&bit)) {
            m->grace_hold_logged|=bit;
            grace_record(m,GR_HOLD_BEGIN,0,decision.identity,grace_source(m),
                ((uint64_t)m->reorder.expected<<32)|m->reorder.buffered,decision.deadline_us,span);
        }
    }
    return decision;
}
static void grace_abandon_before_insert(X4LiveMedia *m)
{
    if(!m->grace_force_gap) return;
    uint16_t expected=m->reorder.expected,buffered=m->reorder.buffered;
    uint32_t span=grace_span(m);
    uint32_t limit=x4_nack_grace_advance_limit(&m->grace,expected,x4_live_reorder_advance_bound(&m->reorder));
    uint32_t lost=x4_live_reorder_abandon_gap_limited(&m->reorder,limit);
    if(lost) {
        bool protected_boundary=!x4_live_reorder_has(&m->reorder,m->reorder.expected);
        unsigned reason=8u|(protected_boundary?16u:0u);
        for(unsigned i=0;i<64;++i) if(m->grace_force_ranges&(UINT64_C(1)<<i))
            grace_record(m,GR_ABANDON,m->grace.ranges[i].release_reason|(protected_boundary?0x100u:0u),
                m->grace.ranges[i].identity,grace_source(m),
                ((uint64_t)expected<<32)|lost,buffered,span);
        x4_trace_record(m->trace,MT_REORDER_GAP,reason,
            ((uint64_t)expected<<32)|lost,buffered);
        m->lost+=lost; reset_au(m,true,RESET_REORDER,reason);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        x4_stream_pipeline_rtp_gap(m->stream, sceKernelGetProcessTime());
#endif
        m->grace_force_gap=false; m->grace_force_ranges=0;
    }
}
static bool tick_budget(X4LiveMedia *m, uint64_t begin, uint64_t decode_begin)
{
    /* Preserve original short-circuit order and every yielding condition.
     * Only the exact condition observed at a budget check is recorded. */
    if (atomic_load(&m->stop)) { m->trace_budget_reason = X4_TRACE_R_USER_STOP; return true; }
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    /* Decode now has its own owner. Bound this assembler to four submitted
     * AUs and the same 16 ms checks; an in-progress queue admission is not
     * interrupted, just as the old in-progress native call was not. */
    uint64_t work_count = m->au_submitted;
#else
    uint64_t work_count = m->video.decode_calls;
#endif
    if (work_count - decode_begin >= WORKER_DECODE_LIMIT) {
        m->trace_budget_reason = X4_TRACE_R_BUDGET_DECODE; return true;
    }
    if (sceKernelGetProcessTime() - begin >= WORKER_BUDGET_US) {
        m->trace_budget_reason = X4_TRACE_R_BUDGET_TIME; return true;
    }
    return false;
}
/* Ordinary resident emission and the full-capacity expected-packet rescue
 * share the exact existing assembler. A DIRECT packet is borrowed from the
 * worker's already allocated ingress-pop stack until this call completes.
 * It was accepted/emitted, never reported as occupying a 129th reorder slot. */
static void consume_reordered(X4LiveMedia *m,const uint8_t *p,size_t size,
    const X4IngressTicket *emitted,bool direct)
{
    X4LiveRtp r;
    if(x4_live_rtp_parse(p,size,&r)<0) return;
    x4_trace_record(m->trace,MT_REORDER_EMIT,
        (r.marker?X4_TRACE_F_MARKER:0u)|(direct?0x4000u:0u),r.sequence,size);
    X4IngressTicket ticket=emitted?*emitted:(X4IngressTicket){0};
    if((ticket.identity||ticket.source||ticket.sequence.sequence_epoch) &&
        ((uint16_t)ticket.extended_sequence!=r.sequence ||
         (uint32_t)ticket.source!=r.ssrc || ticket.rtp_timestamp!=r.timestamp)) {
        ++m->reorder_ticket_omissions; ticket=(X4IngressTicket){0};
    }
    depacketize(m,&r,&ticket);
}
/* Worker-only adaptive hold. The earliest resident packet fixes the
 * clock origin independently of NACK reservation/transport/feedback. */
static void adaptive_gap_abandon(X4LiveMedia *m, uint64_t now, bool capacity)
{
    uint64_t deadline;
    unsigned current_ms = 0;
#if defined(X4_OPENORBIS)
    if (m->grace46b) current_ms = x4_rtc_diag_video_grace46b_ms();
    else if (m->grace43c) current_ms = x4_rtc_diag_video_grace43c_ms();
#endif
    bool startup;
    uint32_t gap = x4_live_reorder_video_gap_policy(&m->reorder, current_ms, &deadline, &startup);
    uint16_t expected = m->reorder.expected;
#if X4_RECOVERY43_MEDIA
    if (m->adaptive_holding && (uint16_t)(expected - m->adaptive_hold_end) < 32768u) {
        ++m->adaptive_completed;
#if defined(X4_OPENORBIS)
        if (m->adaptive_hold_startup) x4_rtc_diag_startup_release(X4_STARTUP_RELEASE_REPAIRED);
#endif
        m->adaptive_holding = false;
    }
    if (gap && !m->adaptive_holding) {
        m->adaptive_holding = true;
        m->adaptive_hold_end = (uint16_t)(expected + gap);
        m->adaptive_hold_startup = startup;
    }
#endif
    if (!gap || (!capacity && gap <= m->reorder.window && (!now || now < deadline))) return;
    uint16_t buffered = m->reorder.buffered;
    uint32_t lost = x4_live_reorder_abandon_gap_limited(&m->reorder,
        x4_live_reorder_advance_bound(&m->reorder));
    if (!lost) return;
#if X4_RECOVERY43_MEDIA
    if (capacity) ++m->adaptive_capacity;
    else if (gap > m->reorder.window) ++m->adaptive_oversized;
    else ++m->adaptive_deadline;
#if defined(X4_OPENORBIS)
    if (startup) x4_rtc_diag_startup_release(capacity || gap > m->reorder.window ?
        X4_STARTUP_RELEASE_CAPACITY : X4_STARTUP_RELEASE_EXPIRED);
#endif
    m->adaptive_holding = false;
#endif
    unsigned reason = capacity || gap > m->reorder.window ? 8u : 2u;
    x4_trace_record(m->trace, MT_REORDER_GAP, reason,
        ((uint64_t)expected << 32) | lost, buffered);
    m->lost += lost;
    reset_au(m, true, RESET_REORDER, reason);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    x4_stream_pipeline_rtp_gap(m->stream, sceKernelGetProcessTime());
#endif
}

static void drain_adaptive(X4LiveMedia *m, uint64_t begin, uint64_t decode_begin, bool allow_expire)
{
    for (unsigned n = 0; n < m->reorder.window && m->reorder.buffered && !m->video.error; ++n) {
        if (atomic_load_explicit(&m->receive_epoch_hold, memory_order_seq_cst) ||
            tick_budget(m, begin, decode_begin)) break;
        const bool idle=allow_expire&&!x4_video_ingress_depth(&m->ingress);
        adaptive_gap_abandon(m,idle?sceKernelGetProcessTime():0,false);
        size_t size = 0;
        uint32_t lost = 0;
        X4IngressTicket ticket = {0};
        const uint8_t *p = x4_live_reorder_next_video_ticket_limited(&m->reorder,
            0, &size, &lost, NULL, false, x4_live_reorder_advance_bound(&m->reorder), &ticket);
        if (!p) break;
        consume_reordered(m, p, size, &ticket, false);
    }
}

static void drain_reordered(X4LiveMedia *m, uint64_t begin, uint64_t decode_begin, bool allow_expire)
{
    if (m->adaptive_reorder_tickets) {
        drain_adaptive(m,begin,decode_begin,allow_expire);
        return;
    }
    for (unsigned n = 0; n < 128 && !m->video.error; ++n) {
        if (atomic_load_explicit(&m->receive_epoch_hold,memory_order_seq_cst)||tick_budget(m, begin, decode_begin)) break;
        size_t size = 0; uint32_t lost = 0;
        unsigned reason = 0;
        uint16_t expected = m->reorder.expected, buffered = m->reorder.buffered;
        X4NackGraceDecision grace=grace_query(m,0,false);
        bool hole=m->reorder.started&&!x4_live_reorder_has(&m->reorder,m->reorder.expected);
        if(hole&&grace.reason==X4_GRACE_HOLD) break;
        uint64_t forced=m->grace_force_ranges;
        uint32_t bound=x4_live_reorder_advance_bound(&m->reorder);
        uint32_t limit=x4_nack_grace_advance_limit(&m->grace,expected,bound);
        uint32_t advance_span=(forced||limit<bound)?grace_span(m):0;
        X4IngressTicket ticket={0};
        const uint8_t *p = x4_live_reorder_next_ticket_limited(&m->reorder, allow_expire ? sceKernelGetProcessTime() : 0,
            &size, &lost, &reason,m->grace_force_gap&&hole,limit,&ticket);
        if(lost||p||!hole) { m->grace_force_gap=false; m->grace_force_ranges=0; }
        x4_trace_record(m->trace, X4_TRACE_REORDER_STATE, 0,
            ((uint64_t)m->reorder.expected << 32) | m->reorder.buffered, m->reorder.started);
        if (lost) {
            if(reason&8u) for(unsigned i=0;i<64;++i) if(forced&(UINT64_C(1)<<i))
                grace_record(m,GR_ABANDON,m->grace.ranges[i].release_reason|((reason&16u)?0x100u:0u),
                    m->grace.ranges[i].identity,grace_source(m),
                    ((uint64_t)expected<<32)|lost,buffered,advance_span);
            if((reason&16u)&&!(reason&8u))
                grace_record(m,GR_ABANDON,0x100u,0,grace_source(m),
                    ((uint64_t)expected<<32)|lost,buffered,advance_span);
            x4_trace_record(m->trace, MT_REORDER_GAP, (uint16_t)(reason | (allow_expire ? 4u : 0u)),
                ((uint64_t)expected << 32) | lost, buffered);
            m->lost += lost; reset_au(m, true, RESET_REORDER, reason);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
            x4_stream_pipeline_rtp_gap(m->stream, sceKernelGetProcessTime());
#endif
        }
        /* A protected pending position may stop an advance before any
         * buffered packet. Its skipped prefix is still real loss/recovery. */
        if (!p) break;
        consume_reordered(m,p,size,&ticket,false);
    }
}
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
static void retire_video_counters(X4LiveMedia *m)
{
#define ADD(field) m->video_totals.field += m->video.field
    ADD(frames); ADD(decode_calls); ADD(decoded_frames); ADD(no_picture_calls); ADD(decode_us);
    ADD(convert_us); ADD(copy_calls); ADD(copy_us); ADD(copy_bytes);
    ADD(copy_parallel_calls); ADD(copy_serial_calls); ADD(copy_owner_us); ADD(copy_helper_us);
    ADD(copy_wait_us); ADD(forced_preserve_calls); ADD(copy_check_attempts); ADD(copy_check_pass);
    ADD(copy_check_mismatch); ADD(copy_check_not_checked); ADD(copy_check_bytes); ADD(copy_check_us);
#undef ADD
#define MAXIMUM(field) if (m->video.field > m->video_totals.field) m->video_totals.field = m->video.field
    MAXIMUM(decode_max_us); MAXIMUM(convert_max_us); MAXIMUM(copy_max_us); MAXIMUM(picture_gap_max_us);
    MAXIMUM(copy_wait_max_us);
#undef MAXIMUM
}
#endif
#if X4_RECOVERY43_MEDIA
static void adaptive_reorder_report(X4LiveMedia *m, bool final)
{
    if (!m->adaptive_reorder_tickets) return;
    uint64_t now = sceKernelGetProcessTime();
    if (!final && !m->adaptive_report_us) { m->adaptive_report_us = now; return; }
    if (!final && now - m->adaptive_report_us < UINT64_C(10000000)) return;
    m->adaptive_report_us = now;
    printf("XCloud4: reorden43b final=%u plazas=%u pico=%u capacidad=%llu plazo=%llu "
           "completados=%llu fuera_de_rango=%llu retornos_observados=%llu retornos_tardios=%llu\n",
        final ? 1u : 0u, m->reorder.window, m->reorder_peak_packets,
        (unsigned long long)m->adaptive_capacity, (unsigned long long)m->adaptive_deadline,
        (unsigned long long)m->adaptive_completed, (unsigned long long)m->adaptive_oversized,
        (unsigned long long)m->adaptive_returns, (unsigned long long)m->adaptive_late_returns);
}
#endif
static void prepare_worker_view(X4LiveMedia *m, X4LiveMediaSnapshot *out)
{
#if X4_RECOVERY43_MEDIA
    adaptive_reorder_report(m, false);
    /* Deltas of existing worker-owned totals; no packet-path registry lookup.
     * PLI is recorded by its actual transport caller, never overwritten here. */
    x4_recovery43_note_media(atomic_load_explicit(&m->grace_inbox.epoch,memory_order_acquire),
        m->recovery43_incomplete-m->recovery43_reported_incomplete,
        m->dropped_frames-m->recovery43_reported_discarded,0,0);
    m->recovery43_reported_incomplete=m->recovery43_incomplete;
    m->recovery43_reported_discarded=m->dropped_frames;
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4StreamPipelineSnapshot stream=m->stream_worker_shadow;
    if(x4_stream_pipeline_try_snapshot(m->stream,&stream))m->stream_worker_shadow=stream;
    /* Worker-local numerical shadow only: the native X4LiveVideo belongs to
     * the dedicated decoder and is never read by the assembly owner. */
    m->video.frames = stream.conversions;
    m->video.decode_calls = stream.decode_calls;
    m->video.decoded_frames = stream.outputs;
    m->video.no_picture_calls = stream.decode_calls >= stream.outputs ? stream.decode_calls - stream.outputs : 0;
    m->video.decode_us = stream.decode_us; m->video.decode_max_us = stream.decode_max_us;
    m->video.convert_us = stream.convert_us; m->video.convert_max_us = stream.convert_max_us;
    if (stream.error) set_video_error(m, stream.error);
#endif
    X4LiveMediaSnapshot s = {0};
    const X4LiveVideo *v = &m->video, *t = &m->video_totals;
    s.video_frames = t->frames + v->frames;
    s.video_lost_packets = m->lost; s.video_dropped_frames = m->dropped_frames;
    s.video_error = v->error; s.waiting_keyframe = m->waiting_keyframe;
    s.video_decode_calls = t->decode_calls + v->decode_calls;
    s.video_decoded_frames = t->decoded_frames + v->decoded_frames;
    s.video_no_picture_calls = t->no_picture_calls + v->no_picture_calls;
    s.video_decode_us = t->decode_us + v->decode_us;
    s.video_decode_max_us = v->decode_max_us > t->decode_max_us ? v->decode_max_us : t->decode_max_us;
    s.video_convert_us = t->convert_us + v->convert_us;
    s.video_convert_max_us = v->convert_max_us > t->convert_max_us ? v->convert_max_us : t->convert_max_us;
    s.video_copy_calls = t->copy_calls + v->copy_calls; s.video_copy_us = t->copy_us + v->copy_us;
    s.video_copy_max_us = v->copy_max_us > t->copy_max_us ? v->copy_max_us : t->copy_max_us;
    s.video_copy_bytes = t->copy_bytes + v->copy_bytes;
    s.video_copy_parallel_calls = t->copy_parallel_calls + v->copy_parallel_calls;
    s.video_copy_serial_calls = t->copy_serial_calls + v->copy_serial_calls;
    s.video_copy_owner_us = t->copy_owner_us + v->copy_owner_us;
    s.video_copy_helper_us = t->copy_helper_us + v->copy_helper_us;
    s.video_copy_wait_us = t->copy_wait_us + v->copy_wait_us;
    s.video_copy_wait_max_us = v->copy_wait_max_us > t->copy_wait_max_us ? v->copy_wait_max_us : t->copy_wait_max_us;
    s.video_forced_preserve_calls = t->forced_preserve_calls + v->forced_preserve_calls;
    s.video_copy_check_attempts = t->copy_check_attempts + v->copy_check_attempts;
    s.video_copy_check_pass = t->copy_check_pass + v->copy_check_pass;
    s.video_copy_check_mismatch = t->copy_check_mismatch + v->copy_check_mismatch;
    s.video_copy_check_not_checked = t->copy_check_not_checked + v->copy_check_not_checked;
    s.video_copy_check_bytes = t->copy_check_bytes + v->copy_check_bytes;
    s.video_copy_check_us = t->copy_check_us + v->copy_check_us;
    s.video_tick_calls = m->tick_calls; s.video_tick_us = m->tick_us;
    s.video_tick_max_us = m->tick_max_us; s.video_tick_packets = m->tick_packets;
    s.video_picture_gap_max_us = v->picture_gap_max_us > t->picture_gap_max_us ? v->picture_gap_max_us : t->picture_gap_max_us;
    s.video_au_submitted = m->au_submitted; s.video_worker_idle_yields = m->worker_idle_yields;
    s.video_queue_age_us = m->queue_age_us; s.video_queue_age_max_us = m->queue_age_max_us;
    s.video_ingress_resets = m->reset_counts[RESET_INGRESS];
    s.video_recovery_episodes = m->recovery_episodes;
    s.video_recovery_completed = m->recovery_completed; s.video_recovery_us = m->recovery_us;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    s.width = stream.width; s.height = stream.height;
    /* Native readiness lets main hand off VideoOut before conversion starts. */
    s.video_ready = stream.outputs != 0;
    s.video_publications = s.video_publication_generation = stream.conversions;
    s.video_publication_superseded = stream.discarded_latency + stream.discarded_ring_full;
#endif
    *out = s;
}
/* Gate held: geometry/generation and their snapshot commit are indivisible. */
static void commit_worker_view(X4LiveMedia *m, X4LiveMediaSnapshot *s)
{
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
    s->video_publications = s->video_publication_generation = m->publication_generation;
    s->video_publication_superseded = m->publication_superseded;
    if (m->published_slot >= 0) {
        s->width = m->rgb[m->published_slot].width; s->height = m->rgb[m->published_slot].height;
        s->video_ready = true;
    }
#endif
    m->worker_view = *s;
#if X4_VIDEO_PROBE
    if (m->local_probe) {
        X4ProbeStatus p = m->video.probe_status;
        p.input_aus = m->probe_owner.input_aus; p.rgb_publications = m->publication_generation;
        p.rgb_conversions = m->video.frames;
        p.last_input_pts_us = m->probe_owner.last_input_pts_us;
        p.late_inputs = m->probe_owner.late_inputs; p.max_input_late_us = m->probe_owner.max_input_late_us;
        p.source_result = m->probe_owner.source_result; p.ready = m->probe_owner.ready;
        p.pipeline_result = m->probe_owner.pipeline_result;
        p.source_eof = m->probe_owner.source_eof; p.draining = m->probe_owner.draining;
        p.finished = m->probe_owner.finished;
        p.cancel_requested = atomic_load_explicit(&m->stop, memory_order_relaxed);
        if (m->probe_owner.close_result != X4_PROBE_RC_NOT_RUN) p.close_result = m->probe_owner.close_result;
        m->probe_view = p;
    }
#endif
}
static void publish_worker_view(X4LiveMedia *m)
{
    X4LiveMediaSnapshot s;
    prepare_worker_view(m, &s);
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, X4_MON_PHASE_MAILBOX, 0);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    /* A UI reader may delay this numerical publication, never RTP assembly. */
    if(x4_stream_mutex_trylock(&m->stream_mailbox)) {
        x4_trace_monitor_state(m->trace,X4_MON_ACTOR_WORKER,m->diagnostic_worker_phase,0);return;
    }
#else
    mailbox_lock(m);
#endif
    commit_worker_view(m, &s);
    mailbox_unlock(m);
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, m->diagnostic_worker_phase, 0);
}
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
static int reserve_rgb(X4LiveMedia *m)
{
    int slot = -1;
    mailbox_lock(m);
    for (int i = 0; i < RGB_SLOTS; ++i)
        if (i != m->published_slot && i != m->reader_slot) { slot = i; break; }
    m->writer_slot = slot;
    mailbox_unlock(m);
    return slot;
}
static void finish_rgb(X4LiveMedia *m, int slot, bool converted)
{
    X4LiveMediaSnapshot s;
    prepare_worker_view(m, &s);
    uint64_t superseded = 0, generation = 0, output_id = 0;
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, X4_MON_PHASE_MAILBOX, 0);
#if X4_VIDEO_PROBE
    uint64_t publication_begin = m->local_probe ? sceKernelGetProcessTime() : 0;
#endif
    mailbox_lock(m);
    if (converted && !m->video.error && m->video.width && m->video.height) {
        /* A ready slot being drawn or already drawn is not discarded. */
        if (m->published_slot >= 0 && m->published_slot != m->reader_slot &&
            m->rgb[m->published_slot].generation > m->last_draw_frame) {
            ++m->publication_superseded; superseded = m->rgb[m->published_slot].generation;
        }
        RgbSlot *rgb = &m->rgb[slot];
        rgb->width = m->video.width; rgb->height = m->video.height;
        rgb->generation = ++m->publication_generation;
        rgb->completed_us = m->video.last_picture_time_us;
        rgb->native_output = m->video.trace_converted_output;
        generation = rgb->generation; output_id = rgb->native_output;
        m->published_slot = slot;
    }
    m->writer_slot = -1;
    commit_worker_view(m, &s);
    mailbox_unlock(m);
#if X4_VIDEO_PROBE
    if (m->local_probe && generation) {
        uint64_t completed = sceKernelGetProcessTime();
        x4_probe_metrics_record_duration(m->probe_metrics, X4_PROBE_STAGE_PUBLICATION,
            completed, completed - publication_begin);
    }
#endif
    if (superseded) x4_trace_record(m->trace, MT_SUPERSEDE, 0, superseded, generation);
    if (generation) x4_trace_record(m->trace, MT_PUBLISH, 0, generation, output_id);
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, X4_MON_PHASE_ASSEMBLE, 0);
}
#endif
static void reset_worker_maxima(X4LiveMedia *m)
{
    if (!atomic_exchange(&m->reset_maxima, false)) return;
    m->tick_max_us = m->queue_age_max_us = 0;
    m->video.decode_max_us = m->video.copy_max_us = m->video.convert_max_us = m->video.picture_gap_max_us = 0;
    m->video_totals.decode_max_us = m->video_totals.copy_max_us = m->video_totals.convert_max_us = m->video_totals.picture_gap_max_us = 0;
    m->video.copy_wait_max_us = m->video_totals.copy_wait_max_us = 0;
}
static void report_ingress(const X4LiveMediaSnapshot *s, const X4LiveMediaSnapshot *p, bool final)
{
#ifdef X4_INGRESS_BASELINE
    const char *mode = "baseline";
    uint64_t consumer_drops = s->video_lock_owner[2] - p->video_lock_owner[2];
#else
    const char *mode = "mpsc";
    uint64_t consumer_drops = 0;
#endif
    printf("XCloud4: video ingress final=%u mode=%s capacity=256 callback=%llu overlap=%llu active=%u peak=%u "
        "callback_us=%llu callback_max_us=%llu reserve_us=%llu reserve_max_us=%llu retries=%llu "
        "consumer_lock_drop=%llu contended=%llu full=%llu other=%llu invalid_rtp=%llu "
        "ingress_resets=%llu recovery_episodes=%llu recovery_completed=%llu recovery_us=%llu\n",
        final ? 1u : 0u, mode,
        (unsigned long long)(s->video_callback_calls - p->video_callback_calls),
        (unsigned long long)(s->video_callback_overlap - p->video_callback_overlap),
        s->video_callback_active, s->video_callback_peak,
        (unsigned long long)(s->video_callback_us - p->video_callback_us),
        (unsigned long long)s->video_callback_max_us,
        (unsigned long long)(s->video_reserve_us - p->video_reserve_us),
        (unsigned long long)s->video_reserve_max_us,
        (unsigned long long)(s->video_reserve_retries - p->video_reserve_retries),
        (unsigned long long)consumer_drops,
        (unsigned long long)(s->video_queue_push_contended - p->video_queue_push_contended),
        (unsigned long long)(s->video_queue_full - p->video_queue_full),
        (unsigned long long)(s->video_queue_other - p->video_queue_other),
        (unsigned long long)(s->video_rx_invalid - p->video_rx_invalid),
        (unsigned long long)(s->video_ingress_resets - p->video_ingress_resets),
        (unsigned long long)(s->video_recovery_episodes - p->video_recovery_episodes),
        (unsigned long long)(s->video_recovery_completed - p->video_recovery_completed),
        (unsigned long long)(s->video_recovery_us - p->video_recovery_us));
    printf("XCloud4: video gate samples final=%u unknown=%llu producer=%llu consumer=%llu\n",
        final ? 1u : 0u,
        (unsigned long long)(s->video_lock_owner[0] - p->video_lock_owner[0]),
        (unsigned long long)(s->video_lock_owner[1] - p->video_lock_owner[1]),
        (unsigned long long)(s->video_lock_owner[2] - p->video_lock_owner[2]));
    printf("XCloud4: video callback histogram final=%u le2=%llu le4=%llu le8=%llu le16=%llu le32=%llu "
        "le64=%llu le128=%llu le256=%llu le512=%llu le1024=%llu le2048=%llu gt2048=%llu\n",
        final ? 1u : 0u,
        (unsigned long long)(s->video_callback_hist[0] - p->video_callback_hist[0]),
        (unsigned long long)(s->video_callback_hist[1] - p->video_callback_hist[1]),
        (unsigned long long)(s->video_callback_hist[2] - p->video_callback_hist[2]),
        (unsigned long long)(s->video_callback_hist[3] - p->video_callback_hist[3]),
        (unsigned long long)(s->video_callback_hist[4] - p->video_callback_hist[4]),
        (unsigned long long)(s->video_callback_hist[5] - p->video_callback_hist[5]),
        (unsigned long long)(s->video_callback_hist[6] - p->video_callback_hist[6]),
        (unsigned long long)(s->video_callback_hist[7] - p->video_callback_hist[7]),
        (unsigned long long)(s->video_callback_hist[8] - p->video_callback_hist[8]),
        (unsigned long long)(s->video_callback_hist[9] - p->video_callback_hist[9]),
        (unsigned long long)(s->video_callback_hist[10] - p->video_callback_hist[10]),
        (unsigned long long)(s->video_callback_hist[11] - p->video_callback_hist[11]));
}
static void report_performance(X4LiveMedia *m, uint64_t now)
{
    if (now - m->report_time < 5000000) return;
    X4LiveMediaSnapshot s; x4_live_media_snapshot(m, &s);
    const X4LiveMediaSnapshot *p = &m->report_previous;
    printf("XCloud4: video perf interval_us=%llu tick=%llu packets=%llu tick_us=%llu tick_max_us=%llu "
        "decode=%llu pictures=%llu no_picture=%llu decode_us=%llu decode_max_us=%llu "
        "copy=%llu copy_us=%llu copy_max_us=%llu copy_bytes=%llu "
        "convert=%llu convert_us=%llu convert_max_us=%llu picture_age_us=%llu picture_gap_max_us=%llu\n",
        (unsigned long long)(now - m->report_time),
        (unsigned long long)(s.video_tick_calls - p->video_tick_calls),
        (unsigned long long)(s.video_tick_packets - p->video_tick_packets),
        (unsigned long long)(s.video_tick_us - p->video_tick_us), (unsigned long long)s.video_tick_max_us,
        (unsigned long long)(s.video_decode_calls - p->video_decode_calls),
        (unsigned long long)(s.video_decoded_frames - p->video_decoded_frames),
        (unsigned long long)(s.video_no_picture_calls - p->video_no_picture_calls),
        (unsigned long long)(s.video_decode_us - p->video_decode_us), (unsigned long long)s.video_decode_max_us,
        (unsigned long long)(s.video_copy_calls - p->video_copy_calls),
        (unsigned long long)(s.video_copy_us - p->video_copy_us), (unsigned long long)s.video_copy_max_us,
        (unsigned long long)(s.video_copy_bytes - p->video_copy_bytes),
        (unsigned long long)(s.video_frames - p->video_frames),
        (unsigned long long)(s.video_convert_us - p->video_convert_us), (unsigned long long)s.video_convert_max_us,
        (unsigned long long)s.video_picture_age_us, (unsigned long long)s.video_picture_gap_max_us);
    printf("XCloud4: video render draw=%llu new=%llu repeat=%llu draw_us=%llu draw_max_us=%llu "
        "present=%llu present_us=%llu present_max_us=%llu queue_depth=%u highwater=%u full=%llu "
        "push_contended=%llu pop_contended=%llu dropped=%llu lost=%llu damaged=%llu pli=%llu waiting_keyframe=%u\n",
        (unsigned long long)(s.video_draw_calls - p->video_draw_calls),
        (unsigned long long)(s.video_draw_new - p->video_draw_new),
        (unsigned long long)(s.video_draw_repeat - p->video_draw_repeat),
        (unsigned long long)(s.video_draw_us - p->video_draw_us), (unsigned long long)s.video_draw_max_us,
        (unsigned long long)(s.video_present_calls - p->video_present_calls),
        (unsigned long long)(s.video_present_us - p->video_present_us), (unsigned long long)s.video_present_max_us,
        s.video_queue_depth, s.video_queue_highwater,
        (unsigned long long)(s.video_queue_full - p->video_queue_full),
        (unsigned long long)(s.video_queue_push_contended - p->video_queue_push_contended),
        (unsigned long long)(s.video_queue_pop_contended - p->video_queue_pop_contended),
        (unsigned long long)(s.video_dropped_packets - p->video_dropped_packets),
        (unsigned long long)(s.video_lost_packets - p->video_lost_packets),
        (unsigned long long)(s.video_dropped_frames - p->video_dropped_frames),
        (unsigned long long)(s.keyframe_requests - p->keyframe_requests), s.waiting_keyframe ? 1u : 0u);
    printf("XCloud4: video worker au=%llu publications=%llu superseded=%llu generation=%llu idle=%llu "
        "queue_age_us=%llu queue_age_max_us=%llu queue_oldest_age_us=%llu budget_us=%u decode_limit=%u\n",
        (unsigned long long)(s.video_au_submitted - p->video_au_submitted),
        (unsigned long long)(s.video_publications - p->video_publications),
        (unsigned long long)(s.video_publication_superseded - p->video_publication_superseded),
        (unsigned long long)s.video_publication_generation,
        (unsigned long long)(s.video_worker_idle_yields - p->video_worker_idle_yields),
        (unsigned long long)(s.video_queue_age_us - p->video_queue_age_us),
        (unsigned long long)s.video_queue_age_max_us, (unsigned long long)s.video_queue_oldest_age_us,
        WORKER_BUDGET_US, WORKER_DECODE_LIMIT);
    printf("XCloud4: video copy parallel=%llu serial=%llu owner_us=%llu helper_us=%llu wait_us=%llu wait_max_us=%llu "
        "forced_preserve=%llu check_attempts=%llu check_pass=%llu check_mismatch=%llu check_not_checked=%llu "
        "check_bytes=%llu check_us=%llu check_window=%u\n",
        (unsigned long long)(s.video_copy_parallel_calls - p->video_copy_parallel_calls),
        (unsigned long long)(s.video_copy_serial_calls - p->video_copy_serial_calls),
        (unsigned long long)(s.video_copy_owner_us - p->video_copy_owner_us),
        (unsigned long long)(s.video_copy_helper_us - p->video_copy_helper_us),
        (unsigned long long)(s.video_copy_wait_us - p->video_copy_wait_us), (unsigned long long)s.video_copy_wait_max_us,
        (unsigned long long)(s.video_forced_preserve_calls - p->video_forced_preserve_calls),
        (unsigned long long)(s.video_copy_check_attempts - p->video_copy_check_attempts),
        (unsigned long long)(s.video_copy_check_pass - p->video_copy_check_pass),
        (unsigned long long)(s.video_copy_check_mismatch - p->video_copy_check_mismatch),
        (unsigned long long)(s.video_copy_check_not_checked - p->video_copy_check_not_checked),
        (unsigned long long)(s.video_copy_check_bytes - p->video_copy_check_bytes),
        (unsigned long long)(s.video_copy_check_us - p->video_copy_check_us),
        s.video_copy_check_attempts != p->video_copy_check_attempts ? 1u : 0u);
    report_ingress(&s, p, false);
    m->report_previous = s; m->report_time = now;
    m->draw_max_us = m->present_max_us = 0;
    atomic_store(&m->reset_maxima, true);
}
static void video_batch(X4LiveMedia *m)
{
    m->diagnostic_worker_phase = X4_MON_PHASE_WAIT_SURFACE;
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, X4_MON_PHASE_WAIT_SURFACE, 0);
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
    int slot = reserve_rgb(m);
    if (slot < 0) return;
#endif
    m->diagnostic_worker_phase = X4_MON_PHASE_ASSEMBLE;
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, X4_MON_PHASE_ASSEMBLE, 0);
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
    if (x4_live_video_set_target(&m->video, m->rgb[slot].pixels, (size_t)X4_LIVE_RGB_PIXELS) < 0) {
        set_video_error(m, X4_LIVE_ERR_DECODER); finish_rgb(m, slot, false); return;
    }
    uint64_t begin = sceKernelGetProcessTime(), decode_begin = m->video.decode_calls;
    uint64_t frame_begin = m->video.frames;
#else
    uint64_t begin = sceKernelGetProcessTime(), decode_begin = m->au_submitted;
#endif
    m->trace_budget_reason = X4_TRACE_R_NONE;
    if (atomic_exchange(&m->ingress_gap, false)) reset_au(m, true, RESET_INGRESS, 0);
    /* Consume available old packets before ingress, without expiring a time
     * gap whose missing packet may still be queued after the previous yield.
     * Depth-based gap abandonment remains bounded by the reorder policy. */
    drain_reordered(m, begin, decode_begin, false);
    if (m->video.error) goto finish;
    uint8_t packet[X4_LIVE_RTP_MAX];
    for (unsigned n = 0; n < 512; ++n) {
        if (atomic_load_explicit(&m->receive_epoch_hold,memory_order_seq_cst)||tick_budget(m, begin, decode_begin)) break;
        uint64_t arrival = 0;
        uint64_t pop_before = atomic_load_explicit(&m->ingress.pop_contended, memory_order_relaxed);
        X4IngressTicket ticket = {0};
        size_t size = x4_video_ingress_pop_ticket(&m->ingress, packet, &arrival, &ticket);
        if (!size) {
            if (atomic_load_explicit(&m->ingress.pop_contended, memory_order_relaxed) != pop_before)
                x4_trace_record(m->trace, MT_POP_CONTENDED, X4_TRACE_REASON(X4_TRACE_R_POP_CONTENDED),
                    x4_video_ingress_depth(&m->ingress), 0);
            /* A reserved, unpublished head preserves FIFO. Yield instead
             * of spinning while a producer finishes its bounded copy. */
#ifndef X4_INGRESS_BASELINE
            if (x4_video_ingress_depth(&m->ingress)) {
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
                int waited=x4_stream_wait_block(&m->ingress_wait,1000);
                if(waited<0){set_video_error(m, waited);atomic_store(&m->stop,true);}
#else
                sceKernelUsleep(50);
#endif
            }
#endif
            break;
        }
        ++m->tick_packets;
        uint64_t popped = sceKernelGetProcessTime(), age = arrival && popped >= arrival ? popped - arrival : 0;
        if(ticket.identity) {
            grace_record(m,GR_QUEUE_POP,0,ticket.identity,ticket.source,
                ticket.extended_sequence,arrival,ticket.authenticated_us);
            unsigned ordinal=(uint32_t)ticket.identity;
            if(ordinal&&ordinal<=64&&age>m->grace_max_dwell[ordinal-1]) m->grace_max_dwell[ordinal-1]=age;
        }
        m->queue_age_us += age; if (age > m->queue_age_max_us) m->queue_age_max_us = age;
        X4LiveRtp r; if (x4_live_rtp_parse(packet, size, &r) < 0) continue;
        x4_trace_record(m->trace, MT_POP, r.marker ? X4_TRACE_F_MARKER : 0u,
            ((uint64_t)r.sequence << 32) | size, age);
        bool locked = m->track.locked;
        int track = x4_live_track_accept(&m->track, &r);
        if (track < 0) {
            x4_trace_record(m->trace, MT_FOREIGN, X4_TRACE_REASON(X4_TRACE_R_FOREIGN), r.sequence, 0);
            atomic_fetch_add(&m->dropped, 1); continue;
        }
        if (track == X4_LIVE_TRACK_NEW) {
            memset(m->reorder_tickets, 0, sizeof(m->reorder_tickets));
#if X4_RECOVERY43_MEDIA
            m->adaptive_holding = false;
#endif
            x4_trace_record(m->trace, MT_TRACK, locked ? 1u : 0u, ++m->trace_track_epoch, r.sequence);
            reset_au(m, true, RESET_TRACK, locked ? 1u : 0u); x4_live_reorder_reset(&m->reorder); m->sps_size = m->pps_size = 0;
            /* Session/Track owner establish the expected source and transport.
             * Neither SEQUENCE nor REQUEST is allowed to initialize them. */
            x4_nack_binding_init(&m->grace_binding,
                atomic_load_explicit(&m->grace_inbox.epoch,memory_order_relaxed),grace_source(m));
            if (locked) {
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
                /* A new source needs a new streaming session and native
                 * ownership graph. Do not replace a decoder behind readers. */
                set_video_error(m, X4_LIVE_ERR_STATE);
                x4_stream_pipeline_request_stop(m->stream); goto finish;
#else
                int rc = x4_live_video_stop(&m->video); if (rc < 0) goto finish;
                retire_video_counters(m);
                if (x4_live_video_start(&m->video, m->rgb[slot].pixels, (size_t)X4_LIVE_RGB_PIXELS) < 0) goto finish;
                decode_begin = frame_begin = 0;
#endif
            }
        }
        const bool adaptive = m->adaptive_reorder_tickets != NULL;
        if (!adaptive) {
            grace_controls(m);
            if(m->reorder.started&&m->grace.active_count)
                (void)grace_query(m,0,false);
        }
        /* Deadline fallback precedes accepting a delayed queued
         * return, even if the work budget has just elapsed. This bounded
         * advance performs no Decode/emit and cannot prolong grace to the
         * callback-to-worker delay. Physical pressure is decided only by an
         * actual failed admission; an expected packet can unlock a full
         * sparse store using its existing ingress-pop buffer. */
        const uint64_t noticed_us=arrival&&arrival<=popped?arrival:popped;
        if (adaptive) adaptive_gap_abandon(m,noticed_us,false);
        else grace_abandon_before_insert(m);
        uint16_t expected = m->reorder.expected, buffered = m->reorder.buffered;
        /* A duplicate means the same complete wire sequence, not a collision
         * between low seven bits. All decisions are worker-owned. */
        unsigned nack_reject = 4u;
        if (ticket.identity && m->reorder.window && size && size <= X4_LIVE_RTP_MAX) {
            if (m->reorder.started && (int16_t)(r.sequence - expected) < 0)
                nack_reject = 1u;
            else if (x4_live_reorder_has(&m->reorder,r.sequence))
                nack_reject = 2u;
        }
        uint64_t stored_us=sceKernelGetProcessTime();
        int rc;
        if (adaptive) {
#if defined(X4_OPENORBIS)
            unsigned grace_ms = m->grace43c ?
                (x4_rtc_diag_startup_active(noticed_us) ?
                    X4_NACK_STARTUP_GRACE_MS : m->grace46b ?
                    x4_rtc_diag_video_grace46b_ms() : x4_rtc_diag_video_grace43c_ms()) :
                x4_rtc_diag_video_grace_ms();
#else
            unsigned grace_ms = 60;
#endif
            rc = x4_live_reorder_insert_video_ticket(&m->reorder, packet, size,
                r.sequence, noticed_us, &ticket, grace_ms);
#if X4_RECOVERY43_MEDIA
            if (rc == X4_LIVE_REORDER_JUMP) m->adaptive_holding = false;
            if (ticket.identity) {
                ++m->adaptive_returns;
                if (rc == X4_LIVE_REORDER_LATE) ++m->adaptive_late_returns;
            }
#endif
        } else {
            rc = x4_live_reorder_insert_ticket(&m->reorder, packet, size, r.sequence, stored_us, &ticket);
        }
#if X4_RECOVERY43_MEDIA
        /* Observe the real admission result, not the repair cache's TTL.
         * The immutable packet epoch prevents attribution to a newer peer. */
        if(ticket.sequence.transport_epoch &&
            (rc==X4_LIVE_REORDER_LATE || rc==X4_LIVE_REORDER_DUPLICATE))
            x4_recovery43_delivery(ticket.sequence.transport_epoch,r.sequence,r.timestamp,
                rc==X4_LIVE_REORDER_LATE?X4_RECOVERY43_DELIVERY_LATE:X4_RECOVERY43_DELIVERY_DUPLICATE);
#endif
        if(rc>=0&&(unsigned)rc<sizeof(m->reorder_admission)/sizeof(m->reorder_admission[0]))
            ++m->reorder_admission[rc];
        if(m->reorder.buffered>m->reorder_peak_packets) m->reorder_peak_packets=m->reorder.buffered;
        if(m->reorder.resident_bytes>m->reorder_peak_bytes) m->reorder_peak_bytes=m->reorder.resident_bytes;
        bool accepted=rc==X4_LIVE_REORDER_STORED||rc==X4_LIVE_REORDER_JUMP||rc==X4_LIVE_REORDER_DIRECT;
        if(rc==X4_LIVE_REORDER_FULL) {
            /* The attempted new packet was not stored. Preserve every
             * resident payload/ticket, end the obstructing hold explicitly,
             * and apply the existing AU recovery before draining. */
            if (adaptive) adaptive_gap_abandon(m, stored_us, true);
            else {
                (void)grace_query(m,0,true);
                grace_abandon_before_insert(m);
            }
        }
        if(!adaptive && rc!=X4_LIVE_REORDER_STORED && ticket.sequence.sequence_epoch &&
            ticket.extended_sequence==ticket.sequence.anchor_extended &&
            r.timestamp==ticket.sequence.anchor_rtp_timestamp)
            grace_binding_reject(m,X4_BINDING_UNPROVEN|(ticket.sequence.reason<<8),ticket.sequence.sequence_epoch,
                ((uint64_t)ticket.sequence.transport_epoch<<32)|ticket.sequence.sequence_epoch,
                ticket.source,ticket.sequence.anchor_extended,rc,m->grace_binding.committed.sequence_epoch);
        if(!adaptive && rc==X4_LIVE_REORDER_JUMP) {
            for(unsigned i=0;i<64;++i) if(m->grace.ranges[i].active)
                grace_effects(m,x4_nack_grace_cancel(&m->grace,m->grace.ranges[i].identity,X4_GRACE_RELEASE_DISTANCE));
            m->grace_force_gap=false; m->grace_force_ranges=0;
        }
        /* Insert/reset owns physical-slot ticket retirement. Clearing this
         * array here would erase the newly retained JUMP packet's ticket. */
        if (ticket.identity) x4_trace_monitor_critical(m->trace, x4_trace_now_us(),
            X4_MON_CR_NACK_REORDER_RESULT, 0xffffu,
            !accepted ? (rc==X4_LIVE_REORDER_LATE||rc==X4_LIVE_REORDER_DUPLICATE?nack_reject:4u+(unsigned)rc) : 0,
            ticket.identity, ticket.source,
            ticket.extended_sequence, rc, ticket.identity & UINT32_MAX);
        if(ticket.identity) {
            unsigned ordinal=(uint32_t)ticket.identity;
            uint64_t deadline=ordinal&&ordinal<=64?m->grace.ranges[ordinal-1].deadline_us:0;
            grace_record(m,GR_WORKER_ACCEPT,rc,ticket.identity,ticket.source,
                ticket.extended_sequence,((uint64_t)expected<<32)|buffered,deadline);
        }
        if(!adaptive && rc==X4_LIVE_REORDER_STORED) {
            uint64_t canonical=0;
            int binding=x4_nack_binding_stored(&m->grace_binding,&ticket.sequence,ticket.source,
                ticket.extended_sequence,r.sequence,r.timestamp,ticket.authenticated_us,
                x4_trace_now_us(),&canonical);
            grace_binding_result(m,binding,&ticket.sequence,ticket.source);
            /* The exact anchor may now confirm the already announced epoch.
             * Drain controls before emit so queued returns can reconcile only
             * against retained slots from the same proven canonical domain. */
            if(binding==X4_BINDING_COMMITTED || binding==X4_BINDING_PROOF)
                grace_controls(m);
            if(grace_ticket_map(m,&ticket,&r,&canonical))
                grace_effects(m,x4_nack_grace_accepted_extended(&m->grace,r.ssrc,
                    ticket.sequence.sequence_epoch,canonical,x4_trace_now_us()));
        }
        if(rc==X4_LIVE_REORDER_DIRECT) {
            /* Existing canonical provenance can accept a timely direct
             * return. DIRECT is not a STORED anchor and cannot prove/rebase
             * a new epoch. Its emission immediately transfers the caller's
             * exact ticket to the ordinary AU assembler. */
            if(tick_budget(m,begin,decode_begin)) {
                /* Resident packets encounter the same fresh budget check
                 * before emission. DIRECT already advanced once, but its
                 * borrowed input cannot survive a yield or invoke Decode
                 * after rejection. Account this real local discard, never
                 * STORED/grace accepted/AU. Keep all128 residents intact. */
                ++m->reorder_direct_budget_drops;
#if X4_RECOVERY43_MEDIA
                if (adaptive) m->adaptive_holding = false;
#endif
                atomic_fetch_add(&m->dropped,1); ++m->lost;
                grace_record(m,GR_DIRECT_BUDGET_DROP,m->trace_budget_reason,
                    ticket.identity,ticket.source,ticket.extended_sequence,
                    ((uint64_t)r.sequence<<32)|m->reorder.expected,m->reorder.buffered);
                x4_trace_record(m->trace,MT_REORDER_GAP,32u,
                    ((uint64_t)r.sequence<<32)|1u,m->reorder.buffered);
                reset_au(m,true,RESET_REORDER,32u);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
                x4_stream_pipeline_rtp_gap(m->stream, sceKernelGetProcessTime());
#endif
                if (!adaptive) (void)grace_query(m,0,false); /* Passed, never grace acceptance. */
            } else {
                uint64_t canonical=0;
                if(!adaptive && grace_ticket_map(m,&ticket,&r,&canonical))
                    grace_effects(m,x4_nack_grace_accepted_extended(&m->grace,r.ssrc,
                        ticket.sequence.sequence_epoch,canonical,x4_trace_now_us()));
                consume_reordered(m,packet,size,&ticket,true);
            }
        }
        if (rc == X4_LIVE_REORDER_JUMP)
            x4_trace_record(m->trace, X4_TRACE_REORDER_WINDOW_JUMP,(uint16_t)m->reorder.last_insert_reason,
                ((uint64_t)expected << 32) | r.sequence, buffered);
        x4_trace_record(m->trace, X4_TRACE_REORDER_STATE, 0,
            ((uint64_t)m->reorder.expected << 32) | m->reorder.buffered, m->reorder.started);
        x4_trace_record(m->trace, MT_REORDER_INSERT, (uint16_t)rc,
            ((uint64_t)expected << 32) | r.sequence, buffered);
        if (!accepted) atomic_fetch_add(&m->dropped, 1);
        else if (rc == X4_LIVE_REORDER_JUMP) { ++m->lost; reset_au(m, true, RESET_JUMP,m->reorder.last_insert_reason); }
        /* Consume continuously so high packet rates do not fill the reorder window. */
        drain_reordered(m, begin, decode_begin,
            x4_video_ingress_depth(&m->ingress) == 0);
        if (m->video.error) goto finish;
    }
    /* A last hole must expire even when no subsequent RTP arrives this frame. */
    drain_reordered(m, begin, decode_begin,
        x4_video_ingress_depth(&m->ingress) == 0);
finish:
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
    x4_live_video_convert_pending(&m->video);
#endif
    uint64_t now = sceKernelGetProcessTime(), elapsed = now - begin;
    ++m->tick_calls; m->tick_us += elapsed;
    if (elapsed > m->tick_max_us) m->tick_max_us = elapsed;
    x4_trace_record(m->trace, MT_YIELD, X4_TRACE_REASON(m->trace_budget_reason),
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        m->au_submitted - decode_begin, elapsed);
#else
        m->video.decode_calls - decode_begin, elapsed);
    finish_rgb(m, slot, m->video.frames > frame_begin);
#endif
}
#if X4_VIDEO_PROBE
static bool probe_source_matches(const X4ProbeCase *sample, const X4ProbeSourceInfo *source)
{
    return source->visible_width == sample->visible_width && source->visible_height == sample->visible_height &&
        source->coded_width == sample->coded_width && source->coded_height == sample->coded_height &&
        source->fps_num == sample->fps_num && source->fps_den == sample->fps_den &&
        source->records == sample->au_count && source->duration_us == sample->duration_us &&
        !memcmp(source->source_sha256, sample->clip_sha256, sizeof(source->source_sha256)) &&
        !memcmp(source->index_sha256, sample->index_sha256, sizeof(source->index_sha256));
}
static void probe_backlog(X4LiveMedia *m, uint64_t now, uint64_t head_late_us)
{
    /* Nominal paced due-AU estimate, not an allocated queue or physical packet
     * backlog. Every indexed AU is still read; maximum-speed has no time pace. */
    uint64_t due = 0;
    if (m->probe_mode == X4_PROBE_PACED && now >= m->probe_origin_us) {
        uint64_t elapsed = now - m->probe_origin_us;
        if (elapsed > m->probe_case->duration_us) elapsed = m->probe_case->duration_us;
        due = elapsed * m->probe_case->fps_num / (UINT64_C(1000000) * m->probe_case->fps_den) + 1;
        if (due > m->probe_case->au_count) due = m->probe_case->au_count;
    }
    uint64_t depth = due > m->probe_owner.input_aus ? due - m->probe_owner.input_aus : 0;
    x4_probe_metrics_backlog(m->probe_metrics, depth, head_late_us);
}
static bool probe_video_batch(X4LiveMedia *m)
{
    int slot = reserve_rgb(m);
    if (slot < 0) { x4_probe_metrics_note(m->probe_metrics, X4_PROBE_COUNT_BACKPRESSURE, 1); return false; }
    if (x4_live_video_set_target(&m->video, m->rgb[slot].pixels, (size_t)X4_LIVE_RGB_PIXELS) < 0) {
        m->video.error = m->probe_owner.pipeline_result = X4_LIVE_ERR_DECODER;
        finish_rgb(m, slot, false); return false;
    }
    uint64_t begin = sceKernelGetProcessTime(), frame_begin = m->video.frames;
    unsigned native_calls = 0; bool worked = false;
    while (!atomic_load_explicit(&m->stop, memory_order_acquire) && !m->video.error &&
        !m->probe_owner.finished && native_calls < WORKER_DECODE_LIMIT) {
        uint64_t now = sceKernelGetProcessTime();
        if (now < begin || now - begin >= WORKER_BUDGET_US) break;
        if (m->probe_owner.draining) {
            uint64_t limit = (uint64_t)m->probe_case->chosen_dpb_frames + 5; /* depth1 + bounded tail4 */
            if (limit > 64) limit = 64;
            if (m->probe_flush_calls >= limit) {
                m->video.error = m->probe_owner.pipeline_result = X4_LIVE_ERR_STATE;
                break;
            }
            ++m->probe_flush_calls; ++native_calls; worked = true;
            m->video.trace_au = 0; /* Flush has no new input AU/output PTS association. */
            int rc = x4_live_video_probe_flush(&m->video);
            if (rc == X4_PROBE_CANCELLED) {
                m->probe_owner.pipeline_result = rc; m->probe_owner.finished = true; break;
            }
            if (rc < 0) { m->video.error = m->probe_owner.pipeline_result = rc; break; }
            if (rc > 1) { m->video.error = m->probe_owner.pipeline_result = X4_LIVE_ERR_STATE; break; }
            if (!rc) { m->probe_owner.draining = false; m->probe_owner.finished = true; }
        } else {
            X4ProbeAU au;
            int rc = x4_probe_source_next(&m->probe_source, now, m->au, X4_LIVE_AU_MAX, &au);
            m->probe_owner.source_result = rc;
            if (rc == X4_PROBE_WAIT) { probe_backlog(m, now, 0); break; }
            if (rc == X4_PROBE_EOF) {
                m->probe_owner.source_eof = true; m->probe_owner.draining = true;
                x4_probe_metrics_note(m->probe_metrics, X4_PROBE_COUNT_EOF, 1); continue;
            }
            if (rc == X4_PROBE_CANCELLED) {
                m->probe_owner.pipeline_result = rc; m->probe_owner.finished = true; break;
            }
            if (rc < 0) {
                m->video.error = m->probe_owner.pipeline_result = rc; break;
            }
            if (atomic_load_explicit(&m->stop, memory_order_acquire)) break;
            /* Admission lateness includes the local read, and is independent
             * of any native-output PTS that the adapted ABI does not expose. */
            uint64_t admitted = sceKernelGetProcessTime();
            uint64_t late = 0;
            if (m->probe_admission_seen) {
                if (admitted < m->probe_last_admission_us) {
                    m->video.error = m->probe_owner.pipeline_result = X4_PROBE_CLOCK;
                    x4_probe_metrics_note(m->probe_metrics, X4_PROBE_COUNT_CLOCK_REJECT, 1); break;
                }
                x4_probe_metrics_record_duration(m->probe_metrics, X4_PROBE_STAGE_INPUT_INTERVAL,
                    admitted, admitted - m->probe_last_admission_us);
            }
            m->probe_last_admission_us = admitted; m->probe_admission_seen = true;
            if (m->probe_mode == X4_PROBE_PACED && au.pts_us <= UINT64_MAX - m->probe_origin_us) {
                uint64_t due = m->probe_origin_us + au.pts_us;
                if (admitted > due) late = admitted - due;
                x4_probe_metrics_record_duration(m->probe_metrics, X4_PROBE_STAGE_INPUT_LATENESS, admitted, late);
            }
            ++m->probe_owner.input_aus; m->probe_owner.last_input_pts_us = au.pts_us;
            if (late) {
                ++m->probe_owner.late_inputs;
                if (late > m->probe_owner.max_input_late_us) m->probe_owner.max_input_late_us = late;
                x4_probe_metrics_note(m->probe_metrics, X4_PROBE_COUNT_INPUT_LATE, 1);
            }
            probe_backlog(m, admitted, late);
            ++m->au_submitted; ++native_calls; worked = true;
            x4_probe_metrics_note_at(m->probe_metrics, admitted, X4_PROBE_COUNT_INPUT_AU, 1);
            m->video.trace_au = ++m->trace_au_next;
            int feed = x4_live_video_feed(&m->video, m->au, (size_t)au.bytes, au.pts_us);
            if (feed == X4_PROBE_CANCELLED) {
                m->probe_owner.pipeline_result = feed; m->probe_owner.finished = true; break;
            }
            if (feed < 0) { m->video.error = m->probe_owner.pipeline_result = feed; break; }
            if (feed) { m->video.error = m->probe_owner.pipeline_result = X4_LIVE_ERR_STATE; break; }
        }
    }
    if (!m->video.error && !atomic_load_explicit(&m->stop, memory_order_acquire)) {
        int rc = x4_live_video_convert_pending(&m->video);
        if (rc == X4_PROBE_CANCELLED) { m->probe_owner.pipeline_result = rc; m->probe_owner.finished = true; }
        if (rc < 0) m->video.error = m->probe_owner.pipeline_result = rc;
        else if (rc && rc != X4_PROBE_CANCELLED) m->video.error = m->probe_owner.pipeline_result = X4_LIVE_ERR_STATE;
    }
    uint64_t elapsed = sceKernelGetProcessTime() - begin;
    ++m->tick_calls; m->tick_us += elapsed;
    if (elapsed > m->tick_max_us) m->tick_max_us = elapsed;
    uint64_t converted = m->video.frames - frame_begin;
    if (converted > 1) x4_probe_metrics_note(m->probe_metrics,
        X4_PROBE_COUNT_RGB_PREPUBLICATION_SUPERSEDE, converted - 1);
    finish_rgb(m, slot, converted > 0);
    return worked;
}
static void *probe_video_worker(void *context)
{
    X4LiveMedia *m = context;
    x4_trace_record(m->trace, MT_WORKER_START, 0, X4_LIVE_COPY_READERS, WORKER_BUDGET_US);
    m->probe_owner.source_result = x4_probe_source_open_fully_verified(&m->probe_source,
        m->probe_case->clip_path, m->probe_case->index_path, m->probe_case->clip_sha256,
        m->probe_case->index_sha256, x4_probe_hash_file, &m->probe_hash, &m->stop);
    int rc = m->probe_owner.source_result;
    if (!rc && !probe_source_matches(m->probe_case, &m->probe_source.info))
        rc = m->probe_owner.source_result = X4_PROBE_INDEX;
    publish_worker_view(m);
    if (!rc && !atomic_load_explicit(&m->stop, memory_order_acquire)) {
        rc = x4_live_video_probe_start_memory(&m->video, m->rgb[0].pixels, (size_t)X4_LIVE_RGB_PIXELS,
            m->probe_case, &m->probe_hash, &m->stop, m->probe_output_memory_type, m->probe_metrics);
        if (!rc) {
            m->probe_origin_us = sceKernelGetProcessTime();
            rc = x4_probe_source_begin(&m->probe_source, m->probe_origin_us, m->probe_mode);
            m->probe_owner.source_result = rc;
        }
    }
    m->probe_owner.ready = rc == 0 && !atomic_load_explicit(&m->stop, memory_order_acquire);
    m->probe_owner.pipeline_result = rc;
    atomic_store(&m->start_result, rc);
    atomic_store_explicit(&m->start_complete, true, memory_order_release);
    if (m->probe_owner.ready) atomic_store_explicit(&m->started, true, memory_order_release);
    publish_worker_view(m);
    while (m->probe_owner.ready && !m->probe_owner.finished && !m->video.error &&
        !atomic_load_explicit(&m->stop, memory_order_acquire)) {
        x4_trace_poll(m->trace); reset_worker_maxima(m);
        bool worked = probe_video_batch(m); publish_worker_view(m);
        if (!worked && !m->video.error && !m->probe_owner.finished) {
            ++m->worker_idle_yields; sceKernelUsleep(1000);
        }
    }
    bool cancelled = atomic_load_explicit(&m->stop, memory_order_acquire) && !m->video.error &&
        m->probe_owner.pipeline_result >= 0 &&
        !(m->probe_owner.finished && m->probe_owner.pipeline_result == 0);
    if (cancelled) {
        m->probe_owner.pipeline_result = X4_PROBE_CANCELLED;
        x4_probe_metrics_note(m->probe_metrics, X4_PROBE_COUNT_CANCEL, 1);
    } else if (m->probe_owner.pipeline_result < 0 || m->video.error)
        x4_probe_metrics_note(m->probe_metrics, X4_PROBE_COUNT_ERRORS, 1);
    m->worker_close_result = x4_live_video_stop(&m->video);
    if (m->video.probe_status.reference_close_result != X4_PROBE_RC_NOT_RUN &&
        m->video.probe_status.reference_close_result < 0)
        x4_probe_metrics_note(m->probe_metrics, X4_PROBE_COUNT_ERRORS, 1);
    for (unsigned i = 0; i < 3; ++i)
        if (m->video.probe_stacks[i].name[0])
            x4_probe_metrics_record_stack(m->probe_metrics, i + 1, &m->video.probe_stacks[i]);
    int source_close = x4_probe_source_close(&m->probe_source);
    if (!m->worker_close_result && source_close < 0) m->worker_close_result = source_close;
    m->probe_owner.close_result = m->worker_close_result; m->probe_owner.finished = true;
    m->probe_owner.draining = false;
    x4_trace_record(m->trace, MT_WORKER_STOP, 0, 0, (uint32_t)m->worker_close_result);
    publish_worker_view(m);
    /* release publishes all final status/teardown results. Failed teardown
     * still forbids close from freeing this context or the borrowed metrics. */
    atomic_store_explicit(&m->probe_owner_done, true, memory_order_release);
    return NULL;
}
#endif
/* Deadline cadence only; no data admission or NACK/grace policy is changed. */
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
static unsigned ingress_wait_timeout(const X4LiveMedia *m)
{return (X4_NACK_GRACE_MS||m->reorder.buffered||m->grace.active_count)?1000u:20000u;}
#endif
/* Video owner only. Peer close plus the callback gate establish producer
 * quiescence. This is also the sole ingress consumer; native leases survive. */
static void receive_epoch_service(X4LiveMedia *m)
{
    uint64_t requested=atomic_load_explicit(&m->receive_epoch_requested,memory_order_acquire);
    if(requested==atomic_load_explicit(&m->receive_epoch_applied,memory_order_relaxed))return;
    int rc=0;
    if(!x4_video_ingress_quiesce_reset(&m->ingress,false))rc=X4_LIVE_ERR_STATE;
    if(!rc){
        if(m->track.locked)atomic_store_explicit(&m->retired_video_source,
            (UINT64_C(1)<<63)|m->track.ssrc,memory_order_release);
        reset_au(m,true,RESET_INGRESS,0);
        m->discard_timestamp_valid=false;m->sps_size=m->pps_size=0;
        x4_live_track_init(&m->track,-1);x4_live_reorder_reset(&m->reorder);
#if X4_RECOVERY43_MEDIA
        m->adaptive_holding = false;
#endif
        memset(m->reorder_tickets,0,sizeof(m->reorder_tickets));
        grace_effects(m,x4_nack_grace_unbind_source(&m->grace));
        /* Request ordinals are local to the new peer. Preserve cumulative
         * counters, not old ordinal identities, ranges or sequence bindings. */
        X4NackGraceCounters grace_counters=m->grace.counters;
        x4_nack_grace_init(&m->grace,m->trace_session,X4_NACK_GRACE_MS==45);
        m->grace.counters=grace_counters;
        memset(m->grace_decode_start,0,sizeof(m->grace_decode_start));
        memset(m->grace_copy_start,0,sizeof(m->grace_copy_start));
        memset(m->grace_convert_start,0,sizeof(m->grace_convert_start));
        m->grace_force_ranges=0;
        x4_nack_binding_init(&m->grace_binding,0,0);m->binding_reject_valid=false;
        X4NackGraceControl discarded;
        while(x4_nack_grace_inbox_pop(&m->grace_inbox,&discarded)){}
        m->grace_force_gap=false;m->grace_hold_logged=0;m->stall_tracking=false;
        atomic_store_explicit(&m->ingress_gap,false,memory_order_relaxed);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        rc=x4_stream_pipeline_begin_receive_epoch(m->stream);
        x4_keyframe_limiter_request(&m->keyframe_limiter,X4_KEYFRAME_ASSEMBLY);
#endif
        if(!rc)atomic_store_explicit(&m->ingress.accepting,true,memory_order_release);
    }
    atomic_store_explicit(&m->receive_epoch_result,rc,memory_order_relaxed);
    atomic_store_explicit(&m->receive_epoch_applied,requested,memory_order_release);
}

int x4_live_media_begin_epoch(X4LiveMedia *m)
{
    if(!m||!atomic_load_explicit(&m->started,memory_order_acquire)||atomic_load(&m->stop))
        return X4_LIVE_ERR_STATE;
#if X4_VIDEO_PROBE
    if(m->local_probe)return X4_LIVE_ERR_STATE;
#endif
    if(atomic_exchange_explicit(&m->receive_epoch_hold,true,memory_order_seq_cst))return X4_LIVE_ERR_STATE;
    uint64_t began=sceKernelGetProcessTime();
    while(atomic_load_explicit(&m->receive_epoch_callbacks,memory_order_seq_cst)){
        if(atomic_load(&m->stop)||sceKernelGetProcessTime()-began>=500000)return X4_LIVE_ERR_STATE;
        sceKernelUsleep(500);
    }
    uint64_t requested=atomic_fetch_add_explicit(&m->receive_epoch_requested,1,memory_order_acq_rel)+1;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    (void)x4_stream_wait_notify(&m->ingress_wait);
#endif
    while(atomic_load_explicit(&m->receive_epoch_applied,memory_order_acquire)!=requested){
        if(atomic_load(&m->stop)||sceKernelGetProcessTime()-began>=500000)return X4_LIVE_ERR_STATE;
        sceKernelUsleep(500);
    }
    int rc=atomic_load_explicit(&m->receive_epoch_result,memory_order_relaxed);
    if(!rc)rc=x4_live_audio_begin_epoch(m->audio);
    if(!rc)atomic_store_explicit(&m->receive_epoch_hold,false,memory_order_seq_cst);
    printf("XCloud4: media receive epoch=%llu rc=%d old_packets_rejected=%llu\n",
        (unsigned long long)requested,rc,(unsigned long long)atomic_load(&m->receive_epoch_rejected));
    return rc;
}

static void *video_worker(void *context)
{
    X4LiveMedia *m = context;
#if X4_VIDEO_PROBE
    if (m->local_probe) return probe_video_worker(context);
#endif
    m->diagnostic_worker_phase = X4_MON_PHASE_STARTUP;
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, X4_MON_PHASE_STARTUP, 0);
    x4_trace_record(m->trace, MT_WORKER_START, 0, X4_LIVE_COPY_READERS, WORKER_BUDGET_US);
    x4_trace_record(m->trace, MT_WAIT_BEGIN, 0x8000u, 0, 0);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    m->ingress_wait_started_us=sceKernelGetProcessTime();
    int cpu_rc=x4_stream_cpu_apply_current_thread(m->stream_cpu_variant,
        X4_CPU_ROLE_ASSEMBLY,0,&m->stream_ingress_cpu);
    x4_stream_pipeline_record_ingress_cpu(m->stream,&m->stream_ingress_cpu);
    if(cpu_rc)printf("XCloud4: assembly CPU settings rc=%d; variante parcialmente aplicada, continua\n",cpu_rc);
    int rc = x4_stream_pipeline_start(m->stream);
    if(rc>0)rc=-rc;
    if(rc<0)set_video_error(m, rc);
#else
    int rc = x4_live_video_start(&m->video, m->rgb[0].pixels, (size_t)X4_LIVE_RGB_PIXELS);
#endif
    publish_worker_view(m);
    atomic_store(&m->start_result, rc);
    atomic_store_explicit(&m->start_complete, true, memory_order_release);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if (rc >= 0) printf("XCloud4: assembler ready AU_arena_bytes=8388608 AU_entries=32 budget_us=%u AU_limit=%u direct_scanout=1 converters=2\n",
        WORKER_BUDGET_US, WORKER_DECODE_LIMIT);
#else
    if (rc >= 0) printf("XCloud4: video owner ready rgb_slots=%u budget_us=%u decode_limit=%u\n",
        RGB_SLOTS, WORKER_BUDGET_US, WORKER_DECODE_LIMIT);
#endif
    while (rc >= 0 && !atomic_load(&m->stop)) {
        receive_epoch_service(m);
        x4_trace_poll(m->trace);
        reset_worker_maxima(m);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        assembly_keyframe_retry(m,sceKernelGetProcessTime());
        if (atomic_load_explicit(&m->started, memory_order_acquire) && !m->video.error) {
            uint32_t q_depth = x4_video_ingress_depth(&m->ingress);
            uint64_t cur_deq = atomic_load_explicit(&m->ingress.dequeue, memory_order_relaxed);
            uint64_t now_w_us = sceKernelGetProcessTime();
            if (q_depth >= m->ingress.capacity) {
                if (!m->stall_tracking || cur_deq != m->stall_last_dequeue) {
                    m->stall_tracking = true;
                    m->stall_since_us = now_w_us;
                    m->stall_last_dequeue = cur_deq;
                } else if (now_w_us - m->stall_since_us >= 500000) {
                    if (x4_video_ingress_quiesce_reset(&m->ingress, true)) {
                        printf("XCloud4: ingress stall detected (>500ms full without progress, cur_deq=%llu); recovering\n",
                            (unsigned long long)cur_deq);
                        x4_stream_pipeline_note_ingress_stall_recovery(m->stream);
                        reset_au(m, true, RESET_INGRESS, 0);
                        x4_live_reorder_reset(&m->reorder);
#if X4_RECOVERY43_MEDIA
                        m->adaptive_holding = false;
#endif
                        m->sps_size = m->pps_size = 0;
                        x4_keyframe_limiter_request(&m->keyframe_limiter, X4_KEYFRAME_ASSEMBLY);
                        m->stall_tracking = false;
                    } else {
                        printf("XCloud4: ingress stall recovery deferred (quiesce wait timed out)\n");
                    }
                }
            } else {
                m->stall_tracking = false;
            }
        } else {
            m->stall_tracking = false;
        }
#endif
        /* Independently evaluate absolute grace expiry on the existing
         * worker cadence, including empty ingress and no new reception. */
        if(m->track.locked) (void)grace_query(m,0,false);
        if (atomic_load_explicit(&m->started, memory_order_acquire) && !m->video.error &&
            (x4_video_ingress_depth(&m->ingress) || m->reorder.buffered)) video_batch(m);
        else {
            m->diagnostic_worker_phase = X4_MON_PHASE_WAIT_DATA;
            x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, X4_MON_PHASE_WAIT_DATA, 0);
            ++m->worker_idle_yields;
#if !X4_STREAM_PROFILE || X4_VIDEO_PROBE
            sceKernelUsleep(1000);
#endif
        }
        publish_worker_view(m);
        /* A missing sequence with no new ingress needs its timeout checked,
         * without a busy spin while waiting for the next packet. */
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        /* No ingress: block until publication or an existing protocol timer.
         * Keep the old 1 ms deadline cadence while reorder/grace is pending;
         * the fully idle path needs only a 20 ms cancellation/retry bound. */
        if(!x4_video_ingress_depth(&m->ingress)||m->video.error||!atomic_load(&m->started)) {
            /* Grace controls arrive through an independently owned inbox.
             * With grace enabled retain its 1 ms service bound even before
             * an offer becomes active; the native wait still blocks. */
            int wait_rc=x4_stream_wait_block(&m->ingress_wait,ingress_wait_timeout(m));
            if(wait_rc<0){rc=wait_rc;set_video_error(m, wait_rc);atomic_store(&m->stop,true);}
        }
#else
        if (!x4_video_ingress_depth(&m->ingress) && m->reorder.buffered) {
            ++m->worker_idle_yields; sceKernelUsleep(1000);
        }
#endif
    }
    m->diagnostic_worker_phase = X4_MON_PHASE_STOPPING;
    grace_controls(m);
#if X4_RECOVERY43_MEDIA
    adaptive_reorder_report(m, true);
#endif
    grace_effects(m,x4_nack_grace_close_reason(&m->grace,X4_GRACE_RELEASE_CLOSE));
    const uint64_t grace_counters[]={atomic_load(&m->grace_inbox.admitted),
        atomic_load(&m->grace_inbox.omitted),atomic_load(&m->grace_inbox.stale),
        m->grace_controls,m->grace.counters.offers,m->grace.counters.admitted,
        m->grace.counters.accepted_positions,m->grace.counters.deadline_ranges,
        m->grace.counters.capacity_ranges,m->grace.counters.completed_ranges,
        m->grace.counters.stale_source_offers,m->grace.counters.expired_offers,
        m->grace.counters.overlap_positions,m->grace.counters.active_cap_offers,
        X4_NACK_GRACE_MS,X4_RUMBLE};
    for(unsigned i=0;i<sizeof(grace_counters)/sizeof(grace_counters[0]);++i)
        grace_record(m,GR_COUNTER,0,0,0,100+i,grace_counters[i],0);
    const X4NackBindingCounters *bc=&m->grace_binding.counters;
    const uint64_t binding_counters[]={bc->announcements,bc->candidates,bc->commits,bc->duplicates,
        bc->stale,bc->invalid,bc->proofs,bc->unproven,bc->mapping_checks,
        bc->unmapped_provenance,bc->mapped_offers};
    for(unsigned i=0;i<sizeof(binding_counters)/sizeof(binding_counters[0]);++i)
        grace_record(m,GR_COUNTER,0,0,0,116+i,binding_counters[i],0);
    grace_record(m,GR_COUNTER,0,0,0,127,m->binding_reject_omitted,0);
    grace_record(m,GR_COUNTER,0,0,0,128,
        atomic_load_explicit(&m->grace_inbox.enqueue,memory_order_acquire)-
        atomic_load_explicit(&m->grace_inbox.dequeue,memory_order_relaxed),0);
    /* New IDs append to the existing numeric summary without shifting its
     * earlier counters. These facts are storage/admission, not decoded video. */
    grace_record(m,GR_COUNTER,0,0,0,129,m->grace.counters.distance_ranges,0);
    grace_record(m,GR_COUNTER,0,0,0,130,m->reorder_peak_packets,m->reorder.window);
    grace_record(m,GR_COUNTER,0,0,0,131,m->reorder_peak_bytes,(uint64_t)m->reorder.window * X4_LIVE_RTP_MAX);
    grace_record(m,GR_COUNTER,0,0,0,132,m->reorder.max_forward,m->reorder.discontinuity_rebases);
    grace_record(m,GR_COUNTER,0,0,0,133,m->reorder.direct_packets,0);
    for(unsigned i=0;i<sizeof(m->reorder_admission)/sizeof(m->reorder_admission[0]);++i)
        grace_record(m,GR_COUNTER,0,0,0,134+i,m->reorder_admission[i],0);
    grace_record(m,GR_COUNTER,0,0,0,143,m->reorder_direct_budget_drops,0);
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_WORKER, X4_MON_PHASE_STOPPING, 0);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    x4_stream_pipeline_request_stop(m->stream);
    m->ingress_wait_elapsed_us=sceKernelGetProcessTime()-m->ingress_wait_started_us;
    m->worker_close_result = 0; /* Main joins converters and decoder after us. */
#else
    m->worker_close_result = x4_live_video_stop(&m->video);
#endif
    for (unsigned i = 0; i < RESET_COUNT; ++i) {
        x4_trace_record(m->trace, MT_RESET_TOTAL, 0, i, m->reset_counts[i]);
        x4_trace_record(m->trace, MT_RESET_TOTAL, 1, i, m->discard_counts[i]);
    }
    for (unsigned i = 0; i < 13; ++i)
        x4_trace_record(m->trace, MT_DAMAGE_TOTAL, 0, i, m->damage_counts[i]);
    x4_trace_record(m->trace, MT_WORKER_STOP, 0, 0, (uint32_t)m->worker_close_result);
    publish_worker_view(m);
    return NULL;
}
void x4_live_media_tick(X4LiveMedia *m)
{
#if X4_VIDEO_PROBE
    if (m && m->local_probe) return; /* Probe recording uses the shared event bridge, no RTP report. */
#endif
    if (m && atomic_load(&m->started)) report_performance(m, sceKernelGetProcessTime());
}
bool x4_live_media_has_new_picture(X4LiveMedia *m)
{
    if (!m) return false;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4StreamPipelineSnapshot s = {0}; x4_stream_pipeline_snapshot(m->stream, &s);
    return s.outputs > s.presentations;
#endif
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAILBOX, 0);
    mailbox_lock(m);
    bool fresh = m->published_slot >= 0 && m->rgb[m->published_slot].generation > m->last_presented_frame;
    mailbox_unlock(m);
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAIN_STATUS, 0);
    return fresh;
}
X4Trace *x4_live_media_trace(X4LiveMedia *m) { return m ? m->trace : NULL; }
bool x4_live_media_stream_enabled(const X4LiveMedia *m)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return m && m->stream;
#else
    (void)m; return false;
#endif
}
int x4_live_media_stream_display_open(X4LiveMedia *m)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return m ? x4_stream_pipeline_display_open(m->stream) : X4_LIVE_ERR_ARGUMENT;
#else
    (void)m; return X4_LIVE_ERR_STATE;
#endif
}
int x4_live_media_stream_present(X4LiveMedia *m,
    void (*overlay)(uint32_t *, void *), void *context)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(!m)return X4_LIVE_ERR_ARGUMENT;
    int rc=x4_stream_pipeline_present(m->stream,overlay,context);
#if X4_RECOVERY43_MEDIA || defined(X4_OPENORBIS)
    X4StreamPipelineSnapshot observed;
    if ((!m->startup_seen || X4_RECOVERY43_MEDIA) &&
        x4_stream_pipeline_try_snapshot(m->stream, &observed) && observed.presentations) {
#if defined(X4_OPENORBIS)
        if (!m->startup_seen) {
            m->startup_seen = true;
            x4_rtc_diag_video_presented(sceKernelGetProcessTime());
        }
#endif
#if X4_RECOVERY43_MEDIA
        if (observed.presentations == m->recovery43_last_presentations) return rc;
        uint64_t now=sceKernelGetProcessTime();
        /* Main observation time, not VideoOut hardware ptime. A missed
         * try-snapshot can coarsen this interval; it never delays a flip. */
        if(m->recovery43_last_observed_us && now>m->recovery43_last_observed_us &&
            now-m->recovery43_last_observed_us>100000)
            x4_recovery43_note_media(atomic_load_explicit(&m->grace_inbox.epoch,memory_order_acquire),0,0,0,1);
        m->recovery43_last_presentations=observed.presentations;
        m->recovery43_last_observed_us=now;
#endif
    }
#endif
    return rc;
#else
    (void)m; (void)overlay; (void)context; return X4_LIVE_ERR_STATE;
#endif
}
int x4_live_media_stream_redraw_notice(X4LiveMedia *m,void (*overlay)(uint32_t *,void *),void *context)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return m?x4_stream_pipeline_redraw_notice(m->stream,overlay,context):X4_LIVE_ERR_ARGUMENT;
#else
    (void)m;(void)overlay;(void)context;return X4_LIVE_ERR_STATE;
#endif
}
void x4_live_media_stream_note_menu_open(X4LiveMedia *m,bool open)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(m)x4_stream_pipeline_note_menu_open(m->stream,open);
#else
    (void)m;(void)open;
#endif
}
unsigned x4_live_media_stream_main_state(const X4LiveMedia *m)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return m?x4_stream_pipeline_main_state(m->stream):0;
#else
    (void)m;return 0;
#endif
}
void x4_live_media_stream_note_controller_read(X4LiveMedia *m,uint64_t sampled_us)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    if(m)x4_stream_pipeline_note_controller_read(m->stream,sampled_us);
#else
    (void)m;(void)sampled_us;
#endif
}
int x4_live_media_stream_wait(X4LiveMedia *m,unsigned timeout_us)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return m?x4_stream_pipeline_wait_present(m->stream,timeout_us):X4_LIVE_ERR_ARGUMENT;
#else
    (void)m;(void)timeout_us;return X4_LIVE_ERR_STATE;
#endif
}
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
int x4_live_media_stream_halt_video(X4LiveMedia *m)
{
    if (!m) return X4_LIVE_ERR_ARGUMENT;
    if (!m->video.error) set_video_error(m, X4_LIVE_ERR_DECODER);
    (void)x4_video_ingress_quiesce_reset(&m->ingress, false);
    return x4_stream_pipeline_halt_video(m->stream);
}
#endif
X4StreamStartup *x4_live_media_stream_startup(X4LiveMedia *m)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return m?x4_stream_pipeline_startup(m->stream):NULL;
#else
    (void)m;return NULL;
#endif
}
uint64_t x4_live_media_post_halt_video_packets(const X4LiveMedia *m)
{
    return m?atomic_load_explicit(&m->post_halt_video_packets,memory_order_relaxed):0;
}
uint64_t x4_live_media_first_post_halt_video_us(const X4LiveMedia *m)
{
    return m?atomic_load_explicit(&m->first_post_halt_video_us,memory_order_relaxed):0;
}
X4KeyframeLimiter *x4_live_media_keyframe_limiter(X4LiveMedia *m)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return m?&m->keyframe_limiter:NULL;
#else
    (void)m;return NULL;
#endif
}
bool x4_live_media_waiting_initial_resolution(const X4LiveMedia *m)
{
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return m&&x4_stream_startup_waiting(x4_stream_pipeline_startup(m->stream));
#else
    (void)m;return false;
#endif
}
X4NackGraceInbox *x4_live_media_grace_inbox(X4LiveMedia *m)
{
#if X4_VIDEO_PROBE
    if (m && m->local_probe) return NULL;
#endif
    return m ? &m->grace_inbox : NULL;
}
bool x4_live_media_drawn(X4LiveMedia *m, uint64_t *generation, uint64_t *output, bool *fresh)
{
    if (generation) *generation = 0;
    if (output) *output = 0;
    if (fresh) *fresh = false;
    if (!m) return false;
    if(!mailbox_lock(m))return false;
    uint64_t g = m->last_draw_frame;
    if (generation) *generation = g;
    if (output) *output = m->last_draw_output;
    if (fresh) *fresh = g > m->last_presented_frame;
    mailbox_unlock(m);
    return g != 0;
}
int x4_live_media_draw(X4LiveMedia *m, uint32_t *p)
{
    if (!m || !p) return 0;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    return 0; /* No legacy RGB allocation or draw pass in these profiles. */
#endif
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAILBOX, 0);
    mailbox_lock(m);
    int slot = m->published_slot;
    if (slot < 0 || m->reader_slot >= 0) { mailbox_unlock(m); return 0; }
    m->reader_slot = slot;
    const uint32_t *pixels = m->rgb[slot].pixels;
    unsigned w = m->rgb[slot].width, h = m->rgb[slot].height;
    uint64_t generation = m->rgb[slot].generation;
    uint64_t native_output = m->rgb[slot].native_output;
    bool fresh = generation > m->last_presented_frame;
    mailbox_unlock(m);
    x4_trace_monitor_state(m->trace, X4_MON_ACTOR_MAIN, X4_MON_PHASE_DRAW, 0);
    x4_trace_record(m->trace, X4_TRACE_RGB_CONSUME, fresh ? X4_TRACE_F_NEW : X4_TRACE_F_REPEAT,
        generation, native_output);
    x4_trace_record(m->trace, X4_TRACE_DRAW_BEGIN, fresh ? X4_TRACE_F_NEW : X4_TRACE_F_REPEAT,
        generation, native_output);
    /* Reader ownership survives publication of another slot. The gate is
     * never held during scaling or framebuffer writes. */
    unsigned out_w = X4_WIDTH, out_h = X4_HEIGHT;
    if ((uint64_t)w * out_h > (uint64_t)h * out_w) out_h = (unsigned)((uint64_t)out_w * h / w);
    else out_w = (unsigned)((uint64_t)out_h * w / h);
    unsigned ox = (X4_WIDTH - out_w) / 2, oy = (X4_HEIGHT - out_h) / 2;
    uint64_t begin = sceKernelGetProcessTime();
    if (m->scale_w != w || m->scale_h != h || m->scale_out_w != out_w || m->scale_out_h != out_h) {
        for (unsigned x = 0; x < out_w; ++x) m->scale_x[x] = (uint16_t)((size_t)x * w / out_w);
        for (unsigned y = 0; y < out_h; ++y) m->scale_y[y] = (uint32_t)((size_t)y * h / out_h * w);
        m->scale_w = w; m->scale_h = h; m->scale_out_w = out_w; m->scale_out_h = out_h;
    }
    if (oy) memset(p, 0, (size_t)oy * X4_WIDTH * sizeof(*p));
    unsigned bottom = oy + out_h;
    if (bottom < X4_HEIGHT) memset(p + (size_t)bottom * X4_WIDTH, 0,
        (size_t)(X4_HEIGHT - bottom) * X4_WIDTH * sizeof(*p));
    for (unsigned y = 0; y < out_h; ++y) {
        const uint32_t *row = pixels + m->scale_y[y];
        uint32_t *out = p + (size_t)(y + oy) * X4_WIDTH;
        if (ox) memset(out, 0, (size_t)ox * sizeof(*p));
        for (unsigned x = 0; x < out_w; ++x) out[ox + x] = row[m->scale_x[x]];
        if (ox + out_w < X4_WIDTH) memset(out + ox + out_w, 0, (size_t)(X4_WIDTH - ox - out_w) * sizeof(*p));
    }
    ++m->draw_calls;
    mailbox_lock(m);
    if (m->last_draw_frame != generation) { ++m->draw_new; m->last_draw_frame = generation; }
    else ++m->draw_repeat;
    m->last_draw_output = native_output;
    m->reader_slot = -1;
    mailbox_unlock(m);
    uint64_t elapsed = sceKernelGetProcessTime() - begin;
    m->draw_us += elapsed; if (elapsed > m->draw_max_us) m->draw_max_us = elapsed;
    x4_trace_record(m->trace, X4_TRACE_DRAW_END, fresh ? X4_TRACE_F_NEW : X4_TRACE_F_REPEAT,
        generation, native_output);
    return 1;
}
void x4_live_media_note_present(X4LiveMedia *m, uint64_t elapsed)
{
    if (!m || !atomic_load(&m->started)) return;
    if(!mailbox_lock(m))return;
    m->last_presented_frame = m->last_draw_frame;
    mailbox_unlock(m);
    ++m->present_calls; m->present_us += elapsed;
    if (elapsed > m->present_max_us) m->present_max_us = elapsed;
}
void x4_live_media_snapshot(const X4LiveMedia *m, X4LiveMediaSnapshot *s)
{
    memset(s, 0, sizeof(*s)); if (!m) return;
    X4LiveMedia *mutable = (X4LiveMedia *)m;
    if(!mailbox_lock(mutable))return;
    *s = m->worker_view;
    uint64_t completed = m->published_slot >= 0 ? m->rgb[m->published_slot].completed_us : 0;
    mailbox_unlock(mutable);
    X4LiveAudioSnapshot a = {0};
#if X4_VIDEO_PROBE
    if (!m->local_probe)
#endif
        x4_live_audio_snapshot(m->audio, &a);
    s->video_packets = atomic_load(&m->packets); s->video_dropped_packets = atomic_load(&m->dropped);
    s->video_bytes = atomic_load(&m->video_bytes); s->audio_bytes = a.bytes;
    s->audio_packets = a.packets; s->audio_frames = a.frames; s->audio_dropped_packets = a.dropped;
    s->audio_lost_packets = a.lost; s->audio_dropped_frames = a.bad_frames; s->audio_underflows = a.underflows;
    s->dropped = s->video_dropped_packets + s->video_lost_packets + s->video_dropped_frames + a.dropped + a.lost + a.bad_frames;
    s->audio_error = m->audio_error ? m->audio_error : a.error;
    s->started = atomic_load(&m->started); s->audio_ready = a.frames != 0; s->audio_playing = a.playing;
    s->keyframe_requests = atomic_load(&m->requests);
    s->video_draw_calls = m->draw_calls; s->video_draw_new = m->draw_new; s->video_draw_repeat = m->draw_repeat;
    s->video_draw_us = m->draw_us; s->video_draw_max_us = m->draw_max_us;
    s->video_present_calls = m->present_calls; s->video_present_us = m->present_us; s->video_present_max_us = m->present_max_us;
    uint64_t now = sceKernelGetProcessTime();
    s->video_picture_age_us = completed && now >= completed ? now - completed : 0;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4StreamPipelineSnapshot stream = {0}; x4_stream_pipeline_snapshot(m->stream, &stream);
    s->width = stream.width; s->height = stream.height; s->video_ready = stream.outputs != 0;
    s->video_frames = stream.conversions; s->video_present_calls = stream.presentations;
    s->video_decode_calls = stream.decode_calls; s->video_decoded_frames = stream.outputs;
    s->video_decode_us = stream.decode_us; s->video_decode_max_us = stream.decode_max_us;
    s->video_convert_us = stream.convert_us; s->video_convert_max_us = stream.convert_max_us;
    s->video_publications = s->video_publication_generation = stream.conversions;
    s->video_publication_superseded = stream.discarded_latency + stream.discarded_ring_full;
    s->video_picture_age_us = stream.latest_picture_us && now >= stream.latest_picture_us ? now - stream.latest_picture_us : 0;
    s->video_rejected_width = stream.rejected_width; s->video_rejected_height = stream.rejected_height;
    s->video_frame_errors = stream.frame_errors; s->video_consecutive_frame_errors = stream.consecutive_frame_errors;
    s->video_stream_keyframe_requests = stream.keyframe_requests; s->video_reconfigurations = stream.reconfigurations;
    s->video_waiting_idr = stream.waiting_idr; s->video_reconfiguring = stream.reconfiguring;
    s->video_display_detached = stream.display_detached;
    s->video_matrix_received = stream.received_matrix;
    s->video_matrix_selected = stream.effective_matrix;
    s->video_full_range = stream.full_range;
    s->video_color_generation = stream.color_notice_started_us;
    s->video_color_ready = stream.native_ready;
    if (stream.error) s->video_error = stream.error;
#endif
#if X4_VIDEO_PROBE
    if (m->local_probe) return; /* No ingress/reorder atomics exist in the local context. */
#endif
    s->video_queue_depth = x4_video_ingress_depth(&m->ingress);
    s->video_queue_highwater = atomic_load_explicit(&m->ingress.highwater, memory_order_relaxed);
    s->video_queue_full = atomic_load_explicit(&m->ingress.full, memory_order_relaxed);
    s->video_queue_push_contended = atomic_load_explicit(&m->ingress.contended, memory_order_relaxed);
    s->video_queue_pop_contended = atomic_load_explicit(&m->ingress.pop_contended, memory_order_relaxed);
    s->video_queue_other = atomic_load_explicit(&m->ingress.invalid, memory_order_relaxed) +
        atomic_load_explicit(&m->ingress.stopped, memory_order_relaxed);
    s->video_reserve_retries = atomic_load_explicit(&m->ingress.retries, memory_order_relaxed);
    s->video_callback_active = atomic_load_explicit(&m->callback_active, memory_order_relaxed);
    s->video_callback_peak = atomic_load_explicit(&m->callback_peak, memory_order_relaxed);
    s->video_callback_calls = atomic_load_explicit(&m->callback_calls, memory_order_relaxed);
    s->video_callback_overlap = atomic_load_explicit(&m->callback_overlap, memory_order_relaxed);
    s->video_callback_us = atomic_load_explicit(&m->callback_us, memory_order_relaxed);
    s->video_callback_max_us = atomic_load_explicit(&m->callback_max_us, memory_order_relaxed);
    s->video_reserve_us = atomic_load_explicit(&m->reserve_us, memory_order_relaxed);
    s->video_reserve_max_us = atomic_load_explicit(&m->reserve_max_us, memory_order_relaxed);
    s->video_rx_invalid = atomic_load_explicit(&m->rx_invalid, memory_order_relaxed);
    for (unsigned i = 0; i < 12; ++i)
        s->video_callback_hist[i] = atomic_load_explicit(&m->callback_hist[i], memory_order_relaxed);
    for (unsigned i = 0; i < 3; ++i)
        s->video_lock_owner[i] = atomic_load_explicit(&m->ingress.contended_owner[i], memory_order_relaxed);
    /* Approximate atomic metadata sample; snapshot adds no queue lock. */
    uint64_t arrival = x4_video_ingress_oldest(&m->ingress);
    s->video_queue_oldest_age_us = arrival && now >= arrival ? now - arrival : 0;
}
void x4_live_media_set_muted(X4LiveMedia *m, bool mute)
{
#if X4_VIDEO_PROBE
    if (m && m->local_probe) return;
#endif
    if (m) x4_live_audio_mute(m->audio, mute);
}
bool x4_live_media_take_keyframe_request(X4LiveMedia *m)
{
#if X4_VIDEO_PROBE
    if (m && m->local_probe) return false;
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    /* Auth is the sole limiter grant owner, including all startup/recovery
     * producers. Do not consume a second legacy dispatch flag here. */
    (void)m;return false;
#else
    if (!m || !atomic_exchange(&m->keyframe, false)) return false;
    atomic_fetch_add(&m->requests, 1);
    x4_trace_record(m->trace, MT_KEY_REQUEST, 0, atomic_load(&m->requests), 0);
    return true;
#endif
}
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
/* Export-only storage, owned by the closing main thread. RTC has already
 * joined and audio is sampled after its worker joins, before destruction. No live
 * playback path reads these values or makes a decision from them. */
static X4LiveAudioSnapshot interaction_audio_final;
void x4_live_interaction_metrics_write(FILE *f)
{
    fputs("{\"input\":",f);x4_rtc_input_metrics_write(f);
    fputs(",\"main_and_rumble\":",f);x4_main_interaction_metrics_write(f);
    fputs(",\"audio\":",f);x4_live_audio_metrics_write(f,&interaction_audio_final);
    fputc('}',f);
}
#endif
int x4_live_media_close(X4LiveMedia *m)
{
    if (!m) return 0;
    x4_trace_set_active(m->trace, false);
    atomic_store_explicit(&m->started, false, memory_order_release);
    atomic_store(&m->stop, true);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    x4_stream_pipeline_request_stop(m->stream);
    (void)x4_stream_wait_close(&m->ingress_wait);
#endif
#if X4_VIDEO_PROBE
    if (!m->local_probe)
#endif
        x4_video_ingress_stop(&m->ingress);
    if (m->running) {
        int rc = scePthreadJoin(m->thread, NULL);
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
        if (!rc) x4_probe_thread_joined(&m->stream_ingress_thread, rc);
#endif
#if X4_VIDEO_PROBE
        if (m->local_probe && !rc) {
            x4_probe_thread_joined(&m->probe_thread, rc);
            x4_probe_metrics_record_stack(m->probe_metrics, 0, &m->probe_thread.report);
        }
#endif
        if (rc) return rc < 0 ? rc : -rc;
        m->running = false;
    }
#if X4_VIDEO_PROBE
    if (m->local_probe) {
        /* No retry of native/source APIs on main. Metrics remain caller-owned;
         * unsafe teardown keeps this context, every buffer and trace retained. */
        if (m->worker_close_result < 0) return m->worker_close_result;
        if (m->probe_source.state || m->video.state) return X4_LIVE_ERR_STATE;
        int monitor_stop = x4_trace_monitor_stop(m->trace);
        if (monitor_stop < 0) return monitor_stop;
        x4_trace_end_session(m->trace);
        if (!x4_trace_free(m->trace)) return X4_LIVE_ERR_STATE;
        m->trace = NULL; m->video.trace = NULL;
        x4_probe_hash_context_close(&m->probe_hash);
        for (unsigned i = 0; i < RGB_SLOTS; ++i) free(m->rgb[i].pixels);
        free(m->au); free(m); return 0;
    }
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4LiveAudioSnapshot audio_snapshot={0};
    X4StreamAudioObservation audio_observation={.available=m->audio!=NULL,
        .observed_us=sceKernelGetProcessTime(),.startup_error=m->audio_error};
    x4_live_audio_snapshot(m->audio,&audio_snapshot);
    audio_observation.packets=audio_snapshot.packets;audio_observation.frames=audio_snapshot.frames;
    audio_observation.dropped=audio_snapshot.dropped;audio_observation.lost=audio_snapshot.lost;
    audio_observation.bad_frames=audio_snapshot.bad_frames;audio_observation.underflows=audio_snapshot.underflows;
    audio_observation.error=audio_snapshot.error;
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    int rc = x4_live_audio_stop_report(m->audio,&audio_snapshot);
    if(!audio_observation.available)audio_snapshot.previous=m->stream_audio_previous;
    interaction_audio_final=audio_snapshot;
#else
    int rc = x4_live_audio_stop(m->audio);
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    audio_observation.stop_result=rc;
    x4_stream_pipeline_record_audio(m->stream,&audio_observation);
#endif
    if (rc >= 0) m->audio = NULL;
    /* Join synchronized the latched result. Never retry video APIs from main
     * after their owning thread exited, even if its native teardown failed. */
    if (m->worker_close_result < 0) return m->worker_close_result;
    if (rc < 0) return rc;
    if (atomic_load_explicit(&m->callback_active, memory_order_relaxed)) return X4_LIVE_ERR_STATE;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    X4StreamWaitStats ingress_stats;
    x4_stream_wait_snapshot(&m->ingress_wait,&ingress_stats);
    x4_stream_pipeline_record_wait_stats(m->stream,X4_STREAM_WAIT_INGRESS,
        &ingress_stats,m->ingress_wait_elapsed_us);
    int stream_close = x4_stream_pipeline_close(m->stream);
#if !X4_PUBLIC
    int stream_export = x4_stream_pipeline_export(m->stream, &m->stream_ingress_thread.report);
    printf("XCloud4: stream diagnostic export rc=%d\n", stream_export);
#endif
    if (stream_close < 0) return stream_close;
#endif
    int monitor_stop = x4_trace_monitor_stop(m->trace);
    if (monitor_stop < 0) return monitor_stop;
    X4LiveMediaSnapshot final_ingress, zero = {0};
    x4_live_media_snapshot(m, &final_ingress);
    report_ingress(&final_ingress, &zero, true);
    if (m->au_ticket_count) nack_au_stage(m,4); /* Partial AU abandoned by explicit shutdown. */
    x4_trace_monitor_critical(m->trace, x4_trace_now_us(), X4_MON_CR_NACK_COUNTER,
        0xffffu, 0, 0, 0, 107, m->au_ticket_omissions, 0);
    x4_trace_monitor_critical(m->trace, x4_trace_now_us(), X4_MON_CR_NACK_COUNTER,
        0xffffu, 0, 0, 0, 108, m->reorder_ticket_omissions, 0);
    if (!x4_video_ingress_free(&m->ingress)) return X4_LIVE_ERR_STATE;
    /* Closed-session totals are independent of trace-window admission/caps. */
    for (unsigned i = 0; i < RESET_COUNT; ++i)
        printf("XCloud4: trace reset reason=%u calls=%llu discarded_open_au=%llu\n", i,
            (unsigned long long)m->reset_counts[i], (unsigned long long)m->discard_counts[i]);
    for (unsigned i = 0; i < 13; ++i)
        printf("XCloud4: trace damage bit=%u discarded_open_au=%llu\n", i,
            (unsigned long long)m->damage_counts[i]);
    /* Existing transport/auth close precondition, plus successful joins and
     * native teardown above, makes all trace producers quiescent. */
    x4_trace_end_session(m->trace);
#if !X4_PUBLIC
    char path[96];
#ifdef X4_INGRESS_BASELINE
    const unsigned ingress_variant = 0;
#else
    const unsigned ingress_variant = 1;
#endif
    snprintf(path, sizeof(path), "/data/xcloud4-trace-0731-%u-%llu.bin", ingress_variant,
        (unsigned long long)m->trace_session);
    int dump = x4_trace_dump_file(m->trace, path);
    int progress_dump = x4_trace_monitor_dump(m->trace, path);
    printf("XCloud4: progress trace dump rc=%d\n", progress_dump);
    printf("XCloud4: numeric trace dump rc=%d\n", dump);
#endif
    bool trace_released = x4_trace_free(m->trace);
    if (!trace_released) printf("XCloud4: numeric trace retained=1\n");
    /* Export/optional trace-storage errors never alter playback close rc. */
    m->trace = NULL;
    m->video.trace = NULL;
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
    int stream_free=x4_stream_pipeline_free(m->stream);
    if(stream_free<0)return stream_free;
    m->stream = NULL;
    int wait_close=x4_stream_wait_destroy(&m->ingress_wait);
    if(wait_close<0)return wait_close;
    if(m->stream_mailbox_initialized){
        int mutex_close=x4_stream_mutex_destroy(&m->stream_mailbox);
        if(mutex_close)return mutex_close<0?mutex_close:-mutex_close;
        m->stream_mailbox_initialized=false;
    }
#endif
    for (unsigned i = 0; i < RGB_SLOTS; ++i) free(m->rgb[i].pixels);
    x4_live_reorder_free(&m->reorder); free(m->adaptive_reorder_tickets); free(m->au); free(m); return 0;
}
