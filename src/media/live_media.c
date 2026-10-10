/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_media.h"
#include "../video/live_h264.h"
#include "../video/display.h"
#include "../audio/live_audio.h"
#include <orbis/libkernel.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static unsigned be16(const uint8_t *p) { return ((unsigned)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }
/* Fixed trace schema. No RTP timestamp, SSRC, payload, address or pointer.
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
int x4_live_reorder_init(X4LiveReorder *q, uint16_t window, uint16_t depth, uint64_t wait)
{
    memset(q, 0, sizeof(*q));
    if (!window || window > 1024 || (window & (window - 1)) || !depth || depth > window || !wait) return -1;
    q->slots = calloc(window, sizeof(*q->slots)); q->data = malloc((size_t)window * X4_LIVE_RTP_MAX);
    if (!q->slots || !q->data) { x4_live_reorder_free(q); return X4_LIVE_ERR_MEMORY; }
    q->window = window; q->depth = depth; q->max_wait = wait; return 0;
}
void x4_live_reorder_free(X4LiveReorder *q) { free(q->slots); free(q->data); memset(q, 0, sizeof(*q)); }
void x4_live_reorder_reset(X4LiveReorder *q) { if (q->slots) memset(q->slots, 0, q->window * sizeof(*q->slots)); q->started = false; q->buffered = 0; }
int x4_live_reorder_insert(X4LiveReorder *q, const uint8_t *p, size_t size, uint16_t seq, uint64_t now)
{
    if (!q || !q->window || !p || size > X4_LIVE_RTP_MAX || !size) return X4_LIVE_REORDER_LATE;
    if (!q->started) { q->expected = seq; q->started = true; }
    int diff = (int16_t)(seq - q->expected), result = X4_LIVE_REORDER_STORED;
    if (diff < 0) return X4_LIVE_REORDER_LATE;
    if ((unsigned)diff >= q->window) { x4_live_reorder_reset(q); q->expected = seq; q->started = true; result = X4_LIVE_REORDER_JUMP; }
    unsigned at = seq & (q->window - 1);
    if (q->slots[at].used) return X4_LIVE_REORDER_LATE;
    memcpy(q->data + (size_t)at * X4_LIVE_RTP_MAX, p, size);
    q->slots[at] = (X4LiveReorderSlot){.sequence = seq, .size = size, .used = true, .arrival = now};
    ++q->buffered; return result;
}
static const uint8_t *reorder_next_reason(X4LiveReorder *q, uint64_t now, size_t *size, uint32_t *lost, unsigned *reason)
{
    if (reason) *reason = 0;
    *size = 0; *lost = 0;
    if (!q || !q->started || !q->buffered) return NULL;
    unsigned at = q->expected & (q->window - 1);
    if (!q->slots[at].used || q->slots[at].sequence != q->expected) {
        unsigned gap = q->window;
        uint64_t oldest = now;
        for (unsigned i = 0; i < q->window; ++i) if (q->slots[i].used) {
            unsigned d = (uint16_t)(q->slots[i].sequence - q->expected);
            if (d < gap) gap = d;
            if (q->slots[i].arrival < oldest) oldest = q->slots[i].arrival;
        }
        /* A zero clock suppresses time expiry while ingress still awaits
         * insertion. The bounded depth/window policy continues to apply. */
        if (gap >= q->window || (q->buffered < q->depth && (!now || now - oldest < q->max_wait))) return NULL;
        if (reason) *reason = (q->buffered >= q->depth ? 1u : 0u) |
            (now && now - oldest >= q->max_wait ? 2u : 0u);
        q->expected += gap; *lost = gap; at = q->expected & (q->window - 1);
    }
    X4LiveReorderSlot *s = &q->slots[at];
    if (!s->used || s->sequence != q->expected) return NULL;
    *size = s->size; s->used = false; --q->buffered; ++q->expected;
    return q->data + (size_t)at * X4_LIVE_RTP_MAX;
}
const uint8_t *x4_live_reorder_next(X4LiveReorder *q, uint64_t now, size_t *size, uint32_t *lost)
{ return reorder_next_reason(q, now, size, lost, NULL); }
enum { RGB_SLOTS = 3, WORKER_BUDGET_US = 16000, WORKER_DECODE_LIMIT = 4 };
typedef struct { uint32_t *pixels; unsigned width, height; uint64_t generation, completed_us, native_output; } RgbSlot;
struct X4LiveMedia {
    X4Trace *trace;
    uint64_t trace_session;
    uint64_t trace_au_next, trace_au, trace_track_epoch, last_draw_output;
    uint32_t damage_bits, first_damage;
    unsigned trace_budget_reason;
    uint64_t reset_counts[RESET_COUNT], discard_counts[RESET_COUNT], damage_counts[13];
    X4LiveRing ring;
    X4LiveReorder reorder;
    X4LiveTrack track;
    X4LiveVideo video;
    X4LiveVideo video_totals; /* Retired decoder counters; never owns pointers. */
    X4LiveAudio *audio;
    uint8_t *au;
    size_t au_size, fu_start;
    uint8_t sps[4096], pps[4096], au_sps[4096], au_pps[4096];
    size_t sps_size, pps_size, au_sps_size, au_pps_size;
    uint32_t timestamp, discard_timestamp;
    uint8_t fu_type;
    bool fu, au_open, idr, damaged, waiting_keyframe, discard_timestamp_valid;
    atomic_bool started, stop, start_complete, reset_maxima;
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
    atomic_uint_fast64_t packets, dropped, requests;
    uint64_t lost, dropped_frames;
    /* Maps live in the heap context rather than growing the UI stack. */
    uint16_t scale_x[X4_WIDTH];
    uint32_t scale_y[X4_HEIGHT];
    unsigned scale_w, scale_h, scale_out_w, scale_out_h;
    uint64_t tick_calls, tick_us, tick_max_us, tick_packets;
    uint64_t draw_calls, draw_new, draw_repeat, draw_us, draw_max_us, last_draw_frame, last_presented_frame;
    uint64_t present_calls, present_us, present_max_us, report_time;
    X4LiveMediaSnapshot report_previous;
};
static void mailbox_lock(X4LiveMedia *m)
{
    while (atomic_flag_test_and_set_explicit(&m->mailbox_gate, memory_order_acquire)) sceKernelUsleep(50);
}
static void mailbox_unlock(X4LiveMedia *m) { atomic_flag_clear_explicit(&m->mailbox_gate, memory_order_release); }
static void *video_worker(void *context);
static void note_damage(X4LiveMedia *m, uint32_t bit)
{
    if (!m->first_damage) {
        m->first_damage = bit;
        x4_trace_record(m->trace, MT_DAMAGE, 0, m->trace_au, bit);
    }
    m->damage_bits |= bit;
}
static void reset_au(X4LiveMedia *m, bool gap, unsigned reason, unsigned detail)
{
    bool waiting_before = m->waiting_keyframe;
    if (reason < RESET_COUNT) ++m->reset_counts[reason];
    uint16_t flags = (gap ? 1u : 0u) | (m->au_open ? 2u : 0u) | (m->fu ? 4u : 0u) |
        (m->idr ? 8u : 0u) | (m->damaged ? 16u : 0u) | (!m->au_size ? 32u : 0u) |
        (waiting_before ? 64u : 0u);
    if (gap && m->au_open) {
        if (reason < RESET_COUNT) ++m->discard_counts[reason];
        for (unsigned i = 0; i < 13; ++i) if (m->damage_bits & (1u << i)) ++m->damage_counts[i];
    }
    if (gap) {
        if (m->au_open) { m->discard_timestamp = m->timestamp; m->discard_timestamp_valid = true; ++m->dropped_frames; }
        m->waiting_keyframe = true; atomic_store(&m->keyframe, true);
    }
    if (m->waiting_keyframe) flags |= 128u;
    x4_trace_record(m->trace, MT_AU_RESET, flags, m->trace_au,
        ((uint64_t)reason << 48) | ((uint64_t)(detail & 0xffffu) << 32) | m->damage_bits);
    if (!waiting_before && m->waiting_keyframe)
        x4_trace_record(m->trace, MT_WAIT_BEGIN, (uint16_t)reason, m->trace_au, 0);
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
static void depacketize(X4LiveMedia *m, const X4LiveRtp *r)
{
    if (m->au_open && m->timestamp != r->timestamp) reset_au(m, true, RESET_TIMESTAMP, 0);
    if (!m->au_open) {
        m->au_open = true; m->timestamp = r->timestamp; m->trace_au = ++m->trace_au_next;
        x4_trace_record(m->trace, MT_AU_OPEN, m->waiting_keyframe ? 1u : 0u, m->trace_au, r->sequence);
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
    /* Parameter sets are committed only after the complete AU is valid.
     * A damaged STAP-A/FU-A cannot poison a later keyframe's cache. */
    if (m->au_sps_size) { memcpy(m->sps, m->au_sps, m->au_sps_size); m->sps_size = m->au_sps_size; }
    if (m->au_pps_size) { memcpy(m->pps, m->au_pps, m->au_pps_size); m->pps_size = m->au_pps_size; }
    if (m->waiting_keyframe && (!m->idr || !m->sps_size || !m->pps_size)) {
        reset_au(m, true, RESET_WAIT, (!m->idr ? 1u : 0u) | (!m->sps_size ? 2u : 0u) | (!m->pps_size ? 4u : 0u)); return;
    }
    if (m->waiting_keyframe) {
        size_t params = m->sps_size + m->pps_size;
        if (params > X4_LIVE_AU_MAX - m->au_size) { reset_au(m, true, RESET_PARAMS, 0); return; }
        memmove(m->au + params, m->au, m->au_size); memcpy(m->au, m->sps, m->sps_size); memcpy(m->au + m->sps_size, m->pps, m->pps_size); m->au_size += params;
    }
    ++m->au_submitted;
    m->video.trace_au = m->trace_au;
    x4_trace_record(m->trace, MT_SUBMIT_BEGIN, m->waiting_keyframe ? 1u : 0u, m->trace_au, m->au_size);
    int rc = x4_live_video_feed(&m->video, m->au, m->au_size, (uint64_t)m->timestamp * 1000000 / 90000);
    x4_trace_record(m->trace, MT_SUBMIT_END, 0, m->trace_au, (uint32_t)rc);
    if (rc < 0) reset_au(m, true, RESET_FEED, 0);
    else {
        if (m->waiting_keyframe) x4_trace_record(m->trace, MT_WAIT_END, 0, m->trace_au, 0);
        m->waiting_keyframe = false; reset_au(m, false, RESET_SUBMITTED, 0);
    }
}
X4LiveMedia *x4_live_media_create(int *error)
{
    int rc = X4_LIVE_ERR_MEMORY;
    X4LiveMedia *m = calloc(1, sizeof(*m));
    if (!m) { if (error) *error = rc; return NULL; }
    atomic_init(&m->keyframe, true); atomic_init(&m->ingress_gap, false);
    atomic_init(&m->packets, 0); atomic_init(&m->dropped, 0); atomic_init(&m->requests, 0);
    atomic_init(&m->started, false); atomic_init(&m->stop, false); atomic_init(&m->start_complete, false);
    atomic_init(&m->reset_maxima, false); atomic_init(&m->start_result, X4_LIVE_ERR_THREAD);
    atomic_flag_clear(&m->mailbox_gate);
    m->published_slot = m->reader_slot = m->writer_slot = -1;
    m->audio_payload_type = -1;
    m->waiting_keyframe = true; x4_live_track_init(&m->track, -1);
    m->au = malloc(X4_LIVE_AU_MAX); if (!m->au) goto fail;
    rc = x4_live_ring_init(&m->ring, 256); if (rc < 0) goto fail;
    m->ring.arrivals = calloc(m->ring.capacity, sizeof(*m->ring.arrivals));
    if (!m->ring.arrivals) { rc = X4_LIVE_ERR_MEMORY; goto fail; }
    rc = x4_live_reorder_init(&m->reorder, 128, 32, 25000); if (rc < 0) goto fail;
    for (unsigned i = 0; i < RGB_SLOTS; ++i) {
        m->rgb[i].pixels = calloc((size_t)X4_LIVE_WIDTH * X4_LIVE_HEIGHT, sizeof(uint32_t));
        if (!m->rgb[i].pixels) { rc = X4_LIVE_ERR_MEMORY; goto fail; }
    }
    m->trace_session = x4_trace_now_us();
    m->trace = x4_trace_create(m->trace_session, X4_LIVE_COPY_READERS);
    m->video.trace = m->trace;
    printf("XCloud4: numeric trace enabled=%u configured_readers=%u\n", m->trace ? 1u : 0u,
        X4_LIVE_COPY_READERS);
    if (error) *error = 0; return m;
fail:
    if (error) *error = rc; x4_live_media_close(m); return NULL;
}
int x4_live_media_start(X4LiveMedia *m)
{
    if (!m || m->running || atomic_load(&m->started) || atomic_load(&m->stop) || atomic_load(&m->start_complete)) return X4_LIVE_ERR_STATE;
    int rc = scePthreadCreate(&m->thread, NULL, video_worker, m, "x4-video");
    if (rc) return rc < 0 ? rc : -rc;
    m->running = true;
    /* Native initialization was synchronous before this change too. All
     * native video calls now remain on the same owning thread. */
    while (!atomic_load_explicit(&m->start_complete, memory_order_acquire)) sceKernelUsleep(1000);
    rc = atomic_load(&m->start_result); if (rc < 0) return rc;
    m->audio = x4_live_audio_start(&m->audio_error, m->audio_payload_type);
    m->report_time = sceKernelGetProcessTime(); atomic_store_explicit(&m->started, true, memory_order_release); return 0;
}
int x4_live_media_set_payload_type(X4LiveMedia *m, int kind, int pt)
{
    if (!m || m->running || atomic_load(&m->started) || atomic_load(&m->stop) || pt < 0 || pt > 127) return -1;
    if (kind == X4_LIVE_KIND_VIDEO) m->track.forced_type = pt;
    else if (kind == X4_LIVE_KIND_AUDIO) m->audio_payload_type = pt;
    else return -1;
    return 0;
}
void x4_live_media_receive(void *context, int kind, const uint8_t *p, size_t size)
{
    X4LiveMedia *m = context; if (!m || !atomic_load_explicit(&m->started, memory_order_acquire) || atomic_load(&m->stop)) return;
    if (kind == X4_LIVE_KIND_AUDIO) { x4_live_audio_receive(m->audio, p, size); return; }
    if (kind != X4_LIVE_KIND_VIDEO) return;
    X4LiveRtp r;
    unsigned parse_reason = 0;
    int parsed = rtp_parse_reason(p, size, &r, &parse_reason);
    unsigned reason = 0;
    bool pushed = parsed >= 0 && ring_push_reason(&m->ring, p, size, &reason);
    if (parsed < 0 || !pushed) {
        atomic_fetch_add(&m->dropped, 1); atomic_store(&m->ingress_gap, true); atomic_store(&m->keyframe, true);
    }
    else atomic_fetch_add(&m->packets, 1);
    /* Exact push branch, not a racy difference of queue counters. Queue gate
     * has already been released. seq16 alone is not proof of packet loss. */
    if (parsed < 0) x4_trace_record(m->trace, MT_RX_REJECT, X4_TRACE_REASON(X4_TRACE_R_RTP_INVALID), size, parse_reason);
    else {
        x4_trace_record(m->trace, MT_RTP, (r.marker ? X4_TRACE_F_MARKER : 0u) |
            (pushed ? X4_TRACE_F_OBSERVED_METADATA : 0u), r.sequence, size);
        if (!pushed) x4_trace_record(m->trace, MT_QUEUE_REJECT,
            X4_TRACE_REASON(reason == 2 ? X4_TRACE_R_PUSH_CONTENDED : reason == 3 ? X4_TRACE_R_QUEUE_FULL : X4_TRACE_R_SIZE),
            r.sequence, atomic_load_explicit(&m->ring.depth, memory_order_relaxed));
    }
    x4_trace_poll(m->trace);
}
static bool tick_budget(X4LiveMedia *m, uint64_t begin, uint64_t decode_begin)
{
    /* Preserve original short-circuit order and every yielding condition.
     * Only the exact condition observed at a budget check is recorded. */
    if (atomic_load(&m->stop)) { m->trace_budget_reason = X4_TRACE_R_USER_STOP; return true; }
    if (m->video.decode_calls - decode_begin >= WORKER_DECODE_LIMIT) {
        m->trace_budget_reason = X4_TRACE_R_BUDGET_DECODE; return true;
    }
    if (sceKernelGetProcessTime() - begin >= WORKER_BUDGET_US) {
        m->trace_budget_reason = X4_TRACE_R_BUDGET_TIME; return true;
    }
    return false;
}
static void drain_reordered(X4LiveMedia *m, uint64_t begin, uint64_t decode_begin, bool allow_expire)
{
    for (unsigned n = 0; n < 128 && !m->video.error; ++n) {
        if (tick_budget(m, begin, decode_begin)) break;
        size_t size = 0; uint32_t lost = 0;
        unsigned reason = 0;
        uint16_t expected = m->reorder.expected, buffered = m->reorder.buffered;
        const uint8_t *p = reorder_next_reason(&m->reorder, allow_expire ? sceKernelGetProcessTime() : 0, &size, &lost, &reason);
        if (!p) break;
        if (lost) {
            x4_trace_record(m->trace, MT_REORDER_GAP, (uint16_t)(reason | (allow_expire ? 4u : 0u)),
                ((uint64_t)expected << 32) | lost, buffered);
            m->lost += lost; reset_au(m, true, RESET_REORDER, reason);
        }
        X4LiveRtp r; if (x4_live_rtp_parse(p, size, &r) == 0) {
            x4_trace_record(m->trace, MT_REORDER_EMIT, r.marker ? X4_TRACE_F_MARKER : 0u, r.sequence, size);
            depacketize(m, &r);
        }
    }
}
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
static void prepare_worker_view(X4LiveMedia *m, X4LiveMediaSnapshot *out)
{
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
    *out = s;
}
/* Gate held: geometry/generation and their snapshot commit are indivisible. */
static void commit_worker_view(X4LiveMedia *m, X4LiveMediaSnapshot *s)
{
    s->video_publications = s->video_publication_generation = m->publication_generation;
    s->video_publication_superseded = m->publication_superseded;
    if (m->published_slot >= 0) {
        s->width = m->rgb[m->published_slot].width; s->height = m->rgb[m->published_slot].height;
        s->video_ready = true;
    }
    m->worker_view = *s;
}
static void publish_worker_view(X4LiveMedia *m)
{
    X4LiveMediaSnapshot s;
    prepare_worker_view(m, &s);
    mailbox_lock(m);
    commit_worker_view(m, &s);
    mailbox_unlock(m);
}
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
    if (superseded) x4_trace_record(m->trace, MT_SUPERSEDE, 0, superseded, generation);
    if (generation) x4_trace_record(m->trace, MT_PUBLISH, 0, generation, output_id);
}
static void reset_worker_maxima(X4LiveMedia *m)
{
    if (!atomic_exchange(&m->reset_maxima, false)) return;
    m->tick_max_us = m->queue_age_max_us = 0;
    m->video.decode_max_us = m->video.copy_max_us = m->video.convert_max_us = m->video.picture_gap_max_us = 0;
    m->video_totals.decode_max_us = m->video_totals.copy_max_us = m->video_totals.convert_max_us = m->video_totals.picture_gap_max_us = 0;
    m->video.copy_wait_max_us = m->video_totals.copy_wait_max_us = 0;
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
    m->report_previous = s; m->report_time = now;
    m->draw_max_us = m->present_max_us = 0;
    atomic_store(&m->reset_maxima, true);
}
static void video_batch(X4LiveMedia *m)
{
    int slot = reserve_rgb(m);
    if (slot < 0) return;
    if (x4_live_video_set_target(&m->video, m->rgb[slot].pixels, (size_t)X4_LIVE_WIDTH * X4_LIVE_HEIGHT) < 0) {
        m->video.error = X4_LIVE_ERR_DECODER; finish_rgb(m, slot, false); return;
    }
    uint64_t begin = sceKernelGetProcessTime(), decode_begin = m->video.decode_calls;
    m->trace_budget_reason = X4_TRACE_R_NONE;
    uint64_t frame_begin = m->video.frames;
    if (atomic_exchange(&m->ingress_gap, false)) reset_au(m, true, RESET_INGRESS, 0);
    /* Consume available old packets before ingress, without expiring a time
     * gap whose missing packet may still be queued after the previous yield.
     * Depth-based gap abandonment remains bounded by the reorder policy. */
    drain_reordered(m, begin, decode_begin, false);
    if (m->video.error) goto finish;
    uint8_t packet[X4_LIVE_RTP_MAX];
    for (unsigned n = 0; n < 512; ++n) {
        if (tick_budget(m, begin, decode_begin)) break;
        uint64_t arrival = 0;
        unsigned pop_reason = 0;
        size_t size = ring_pop_at(&m->ring, packet, &arrival, &pop_reason);
        if (!size) {
            if (pop_reason == 2) x4_trace_record(m->trace, MT_POP_CONTENDED,
                X4_TRACE_REASON(X4_TRACE_R_POP_CONTENDED), atomic_load_explicit(&m->ring.depth, memory_order_relaxed), 0);
            break;
        }
        ++m->tick_packets;
        uint64_t popped = sceKernelGetProcessTime(), age = arrival && popped >= arrival ? popped - arrival : 0;
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
            x4_trace_record(m->trace, MT_TRACK, locked ? 1u : 0u, ++m->trace_track_epoch, r.sequence);
            reset_au(m, true, RESET_TRACK, locked ? 1u : 0u); x4_live_reorder_reset(&m->reorder); m->sps_size = m->pps_size = 0;
            if (locked) {
                int rc = x4_live_video_stop(&m->video); if (rc < 0) goto finish;
                retire_video_counters(m);
                if (x4_live_video_start(&m->video, m->rgb[slot].pixels, (size_t)X4_LIVE_WIDTH * X4_LIVE_HEIGHT) < 0) goto finish;
                decode_begin = frame_begin = 0;
            }
        }
        uint16_t expected = m->reorder.expected, buffered = m->reorder.buffered;
        int rc = x4_live_reorder_insert(&m->reorder, packet, size, r.sequence, sceKernelGetProcessTime());
        x4_trace_record(m->trace, MT_REORDER_INSERT, (uint16_t)rc,
            ((uint64_t)expected << 32) | r.sequence, buffered);
        if (rc == X4_LIVE_REORDER_LATE) atomic_fetch_add(&m->dropped, 1);
        else if (rc == X4_LIVE_REORDER_JUMP) { ++m->lost; reset_au(m, true, RESET_JUMP, 0); }
        /* Consume continuously so high packet rates do not fill the reorder window. */
        drain_reordered(m, begin, decode_begin,
            atomic_load_explicit(&m->ring.depth, memory_order_relaxed) == 0);
        if (m->video.error) goto finish;
    }
    /* A last hole must expire even when no subsequent RTP arrives this frame. */
    drain_reordered(m, begin, decode_begin,
        atomic_load_explicit(&m->ring.depth, memory_order_relaxed) == 0);
finish:
    x4_live_video_convert_pending(&m->video);
    uint64_t now = sceKernelGetProcessTime(), elapsed = now - begin;
    ++m->tick_calls; m->tick_us += elapsed;
    if (elapsed > m->tick_max_us) m->tick_max_us = elapsed;
    x4_trace_record(m->trace, MT_YIELD, X4_TRACE_REASON(m->trace_budget_reason),
        m->video.decode_calls - decode_begin, elapsed);
    finish_rgb(m, slot, m->video.frames > frame_begin);
}
static void *video_worker(void *context)
{
    X4LiveMedia *m = context;
    x4_trace_record(m->trace, MT_WORKER_START, 0, X4_LIVE_COPY_READERS, WORKER_BUDGET_US);
    x4_trace_record(m->trace, MT_WAIT_BEGIN, 0x8000u, 0, 0);
    int rc = x4_live_video_start(&m->video, m->rgb[0].pixels, (size_t)X4_LIVE_WIDTH * X4_LIVE_HEIGHT);
    publish_worker_view(m);
    atomic_store(&m->start_result, rc);
    atomic_store_explicit(&m->start_complete, true, memory_order_release);
    if (rc >= 0) printf("XCloud4: video owner ready rgb_slots=%u budget_us=%u decode_limit=%u\n",
        RGB_SLOTS, WORKER_BUDGET_US, WORKER_DECODE_LIMIT);
    while (rc >= 0 && !atomic_load(&m->stop)) {
        x4_trace_poll(m->trace);
        reset_worker_maxima(m);
        if (atomic_load_explicit(&m->started, memory_order_acquire) && !m->video.error &&
            (atomic_load_explicit(&m->ring.depth, memory_order_relaxed) || m->reorder.buffered)) video_batch(m);
        else { ++m->worker_idle_yields; sceKernelUsleep(1000); }
        publish_worker_view(m);
        /* A missing sequence with no new ingress needs its timeout checked,
         * without a busy spin while waiting for the next packet. */
        if (!atomic_load_explicit(&m->ring.depth, memory_order_relaxed) && m->reorder.buffered) {
            ++m->worker_idle_yields; sceKernelUsleep(1000);
        }
    }
    m->worker_close_result = x4_live_video_stop(&m->video);
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
    if (m && atomic_load(&m->started)) report_performance(m, sceKernelGetProcessTime());
}
bool x4_live_media_has_new_picture(X4LiveMedia *m)
{
    if (!m) return false;
    mailbox_lock(m);
    bool fresh = m->published_slot >= 0 && m->rgb[m->published_slot].generation > m->last_presented_frame;
    mailbox_unlock(m);
    return fresh;
}
X4Trace *x4_live_media_trace(X4LiveMedia *m) { return m ? m->trace : NULL; }
bool x4_live_media_drawn(X4LiveMedia *m, uint64_t *generation, uint64_t *output, bool *fresh)
{
    if (generation) *generation = 0;
    if (output) *output = 0;
    if (fresh) *fresh = false;
    if (!m) return false;
    mailbox_lock(m);
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
    mailbox_lock(m);
    m->last_presented_frame = m->last_draw_frame;
    mailbox_unlock(m);
    ++m->present_calls; m->present_us += elapsed;
    if (elapsed > m->present_max_us) m->present_max_us = elapsed;
}
void x4_live_media_snapshot(const X4LiveMedia *m, X4LiveMediaSnapshot *s)
{
    memset(s, 0, sizeof(*s)); if (!m) return;
    X4LiveMedia *mutable = (X4LiveMedia *)m;
    mailbox_lock(mutable);
    *s = m->worker_view;
    uint64_t completed = m->published_slot >= 0 ? m->rgb[m->published_slot].completed_us : 0;
    mailbox_unlock(mutable);
    X4LiveAudioSnapshot a; x4_live_audio_snapshot(m->audio, &a);
    s->video_packets = atomic_load(&m->packets); s->video_dropped_packets = atomic_load(&m->dropped);
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
    s->video_queue_depth = atomic_load_explicit(&m->ring.depth, memory_order_relaxed);
    s->video_queue_highwater = atomic_load_explicit(&m->ring.highwater, memory_order_relaxed);
    s->video_queue_full = atomic_load_explicit(&m->ring.full, memory_order_relaxed);
    s->video_queue_push_contended = atomic_load_explicit(&m->ring.push_contended, memory_order_relaxed);
    s->video_queue_pop_contended = atomic_load_explicit(&m->ring.pop_contended, memory_order_relaxed);
    /* Approximate independently sampled age; snapshot adds no queue lock. */
    uint64_t arrival = atomic_load_explicit(&m->ring.oldest_arrival, memory_order_relaxed);
    s->video_queue_oldest_age_us = arrival && now >= arrival ? now - arrival : 0;
}
void x4_live_media_set_muted(X4LiveMedia *m, bool mute) { if (m) x4_live_audio_mute(m->audio, mute); }
bool x4_live_media_take_keyframe_request(X4LiveMedia *m)
{
    if (!m || !atomic_exchange(&m->keyframe, false)) return false;
    atomic_fetch_add(&m->requests, 1);
    x4_trace_record(m->trace, MT_KEY_REQUEST, 0, atomic_load(&m->requests), 0);
    return true;
}
int x4_live_media_close(X4LiveMedia *m)
{
    if (!m) return 0;
    x4_trace_set_active(m->trace, false);
    atomic_store_explicit(&m->started, false, memory_order_release);
    atomic_store(&m->stop, true);
    if (m->running) {
        int rc = scePthreadJoin(m->thread, NULL);
        if (rc) return rc < 0 ? rc : -rc;
        m->running = false;
    }
    int rc = x4_live_audio_stop(m->audio);
    if (rc >= 0) m->audio = NULL;
    /* Join synchronized the latched result. Never retry video APIs from main
     * after their owning thread exited, even if its native teardown failed. */
    if (m->worker_close_result < 0) return m->worker_close_result;
    if (rc < 0) return rc;
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
    char path[96];
    snprintf(path, sizeof(path), "/data/xcloud4-trace-0727-%llu.bin", (unsigned long long)m->trace_session);
    int dump = x4_trace_dump_file(m->trace, path);
    printf("XCloud4: numeric trace dump rc=%d\n", dump);
    bool trace_released = x4_trace_free(m->trace);
    if (!trace_released) printf("XCloud4: numeric trace retained=1\n");
    /* Export/optional trace-storage errors never alter playback close rc. */
    m->trace = NULL;
    m->video.trace = NULL;
    for (unsigned i = 0; i < RGB_SLOTS; ++i) free(m->rgb[i].pixels);
    x4_live_ring_free(&m->ring); x4_live_reorder_free(&m->reorder); free(m->au); free(m); return 0;
}
