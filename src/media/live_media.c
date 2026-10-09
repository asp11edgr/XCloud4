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
int x4_live_rtp_parse(const uint8_t *p, size_t size, X4LiveRtp *r)
{
    if (!p || !r || size < 12 || size > X4_LIVE_RTP_MAX || (p[0] >> 6) != 2 || (p[1] >= 200 && p[1] <= 206)) return -1;
    size_t at = 12 + (p[0] & 15) * 4u;
    if (at > size) return -1;
    if (p[0] & 16) {
        if (size - at < 4) return -1;
        size_t ext = (size_t)be16(p + at + 2) * 4u + 4;
        if (ext > size - at) return -1;
        at += ext;
    }
    size_t end = size;
    if (p[0] & 32) {
        unsigned pad = p[size - 1];
        if (!pad || pad > size - at) return -1;
        end -= pad;
    }
    if (at >= end) return -1;
    *r = (X4LiveRtp){.payload = p + at, .size = end - at, .sequence = be16(p + 2),
        .timestamp = be32(p + 4), .ssrc = be32(p + 8), .payload_type = p[1] & 127, .marker = (p[1] & 128) != 0};
    return 0;
}
int x4_live_ring_init(X4LiveRing *q, uint32_t capacity)
{
    memset(q, 0, sizeof(*q)); atomic_flag_clear(&q->lock);
    if (!capacity || capacity > 1024) return -1;
    q->sizes = calloc(capacity, sizeof(*q->sizes)); q->data = malloc((size_t)capacity * X4_LIVE_RTP_MAX);
    if (!q->sizes || !q->data) { x4_live_ring_free(q); return X4_LIVE_ERR_MEMORY; }
    q->capacity = capacity; return 0;
}
void x4_live_ring_free(X4LiveRing *q) { free(q->sizes); free(q->data); q->sizes = NULL; q->data = NULL; q->capacity = 0; }
bool x4_live_ring_push(X4LiveRing *q, const uint8_t *p, size_t size)
{
    if (!q || !q->capacity || !p || !size || size > X4_LIVE_RTP_MAX || atomic_flag_test_and_set_explicit(&q->lock, memory_order_acquire)) return false;
    bool ok = q->count < q->capacity;
    if (ok) {
        unsigned at = (q->head + q->count) % q->capacity;
        memcpy(q->data + (size_t)at * X4_LIVE_RTP_MAX, p, size); q->sizes[at] = size; ++q->count;
    }
    atomic_flag_clear_explicit(&q->lock, memory_order_release); return ok;
}
size_t x4_live_ring_pop(X4LiveRing *q, uint8_t *p)
{
    if (!q || !q->capacity || atomic_flag_test_and_set_explicit(&q->lock, memory_order_acquire)) return 0;
    size_t size = 0;
    if (q->count) { size = q->sizes[q->head]; memcpy(p, q->data + (size_t)q->head * X4_LIVE_RTP_MAX, size); q->head = (q->head + 1) % q->capacity; --q->count; }
    atomic_flag_clear_explicit(&q->lock, memory_order_release); return size;
}
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
const uint8_t *x4_live_reorder_next(X4LiveReorder *q, uint64_t now, size_t *size, uint32_t *lost)
{
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
        if (gap >= q->window || (q->buffered < q->depth && now - oldest < q->max_wait)) return NULL;
        q->expected += gap; *lost = gap; at = q->expected & (q->window - 1);
    }
    X4LiveReorderSlot *s = &q->slots[at];
    if (!s->used || s->sequence != q->expected) return NULL;
    *size = s->size; s->used = false; --q->buffered; ++q->expected;
    return q->data + (size_t)at * X4_LIVE_RTP_MAX;
}
struct X4LiveMedia {
    X4LiveRing ring;
    X4LiveReorder reorder;
    X4LiveTrack track;
    X4LiveVideo video;
    X4LiveAudio *audio;
    uint8_t *au;
    size_t au_size, fu_start;
    uint8_t sps[4096], pps[4096], au_sps[4096], au_pps[4096];
    size_t sps_size, pps_size, au_sps_size, au_pps_size;
    uint32_t timestamp, discard_timestamp;
    uint8_t fu_type;
    bool fu, au_open, idr, damaged, waiting_keyframe, discard_timestamp_valid;
    bool started;
    int audio_error, audio_payload_type;
    atomic_bool keyframe, ingress_gap;
    atomic_uint_fast64_t packets, dropped, requests;
    uint64_t lost, dropped_frames;
};
static void reset_au(X4LiveMedia *m, bool gap)
{
    if (gap) {
        if (m->au_open) { m->discard_timestamp = m->timestamp; m->discard_timestamp_valid = true; ++m->dropped_frames; }
        m->waiting_keyframe = true; atomic_store(&m->keyframe, true);
    }
    m->au_size = 0; m->fu = m->au_open = m->idr = m->damaged = false;
    m->au_sps_size = m->au_pps_size = 0;
}
static bool append(X4LiveMedia *m, const uint8_t *p, size_t size)
{
    if (size > X4_LIVE_AU_MAX - m->au_size) return false;
    memcpy(m->au + m->au_size, p, size); m->au_size += size; return true;
}
static void cache_nal(X4LiveMedia *m, const uint8_t *p, size_t size)
{
    unsigned type = p[0] & 31;
    if (type == 5) m->idr = true;
    if (size > 4092) return;
    uint8_t *to = type == 7 ? m->au_sps : type == 8 ? m->au_pps : NULL;
    if (to) { memcpy(to, "\0\0\0\1", 4); memcpy(to + 4, p, size); if (type == 7) m->au_sps_size = size + 4; else m->au_pps_size = size + 4; }
}
static bool nal(X4LiveMedia *m, const uint8_t *p, size_t size)
{
    if (!size || (p[0] & 128) || !(p[0] & 31) || (p[0] & 31) > 23) return false;
    if (!append(m, (const uint8_t *)"\0\0\0\1", 4) || !append(m, p, size)) return false;
    cache_nal(m, p, size); return true;
}
static void depacketize(X4LiveMedia *m, const X4LiveRtp *r)
{
    if (m->au_open && m->timestamp != r->timestamp) reset_au(m, true);
    if (!m->au_open) { m->au_open = true; m->timestamp = r->timestamp; }
    if (m->discard_timestamp_valid) {
        if (r->timestamp == m->discard_timestamp) m->damaged = true;
        else m->discard_timestamp_valid = false;
    }
    const uint8_t *p = r->payload; size_t size = r->size; unsigned type = p[0] & 31;
    bool ok = !(p[0] & 128);
    if (ok && type >= 1 && type <= 23) { if (m->fu) ok = false; else ok = nal(m, p, size); }
    else if (ok && type == 24 && size > 1 && !m->fu) {
        size_t at = 1;
        while (ok && at < size) {
            if (size - at < 2) { ok = false; break; }
            size_t length = be16(p + at); at += 2;
            if (!length || length > size - at) { ok = false; break; }
            ok = nal(m, p + at, length); at += length;
        }
        if (at != size) ok = false;
    } else if (ok && type == 28 && size > 2) {
        unsigned nt = p[1] & 31; bool start = p[1] & 128, end = p[1] & 64;
        if ((p[1] & 32) || !nt || nt > 23 || (start && end)) ok = false;
        else if (start) {
            if (m->fu) ok = false;
            else {
                uint8_t header = (p[0] & 0xe0) | nt;
                m->fu_start = m->au_size + 4; m->fu_type = header;
                ok = append(m, (const uint8_t *)"\0\0\0\1", 4) && append(m, &header, 1) && append(m, p + 2, size - 2);
                m->fu = ok;
            }
        } else if (!m->fu || ((p[0] & 0xe0) | nt) != m->fu_type) ok = false;
        else {
            ok = append(m, p + 2, size - 2);
            if (ok && end) { m->fu = false; cache_nal(m, m->au + m->fu_start, m->au_size - m->fu_start); }
        }
    } else ok = false;
    if (!ok) m->damaged = true;
    if (!r->marker) return;
    if (m->fu || m->damaged || !m->au_size) {
        reset_au(m, true); return;
    }
    /* Parameter sets are committed only after the complete AU is valid.
     * A damaged STAP-A/FU-A cannot poison a later keyframe's cache. */
    if (m->au_sps_size) { memcpy(m->sps, m->au_sps, m->au_sps_size); m->sps_size = m->au_sps_size; }
    if (m->au_pps_size) { memcpy(m->pps, m->au_pps, m->au_pps_size); m->pps_size = m->au_pps_size; }
    if (m->waiting_keyframe && (!m->idr || !m->sps_size || !m->pps_size)) {
        reset_au(m, true); return;
    }
    if (m->waiting_keyframe) {
        size_t params = m->sps_size + m->pps_size;
        if (params > X4_LIVE_AU_MAX - m->au_size) { reset_au(m, true); return; }
        memmove(m->au + params, m->au, m->au_size); memcpy(m->au, m->sps, m->sps_size); memcpy(m->au + m->sps_size, m->pps, m->pps_size); m->au_size += params;
    }
    int rc = x4_live_video_feed(&m->video, m->au, m->au_size, (uint64_t)m->timestamp * 1000000 / 90000);
    if (rc < 0) reset_au(m, true);
    else { m->waiting_keyframe = false; reset_au(m, false); }
}
X4LiveMedia *x4_live_media_create(int *error)
{
    int rc = X4_LIVE_ERR_MEMORY;
    X4LiveMedia *m = calloc(1, sizeof(*m));
    if (!m) { if (error) *error = rc; return NULL; }
    atomic_init(&m->keyframe, true); atomic_init(&m->ingress_gap, false);
    atomic_init(&m->packets, 0); atomic_init(&m->dropped, 0); atomic_init(&m->requests, 0);
    m->audio_payload_type = -1;
    m->waiting_keyframe = true; x4_live_track_init(&m->track, -1);
    m->au = malloc(X4_LIVE_AU_MAX); if (!m->au) goto fail;
    rc = x4_live_ring_init(&m->ring, 256); if (rc < 0) goto fail;
    rc = x4_live_reorder_init(&m->reorder, 128, 32, 25000); if (rc < 0) goto fail;
    if (error) *error = 0; return m;
fail:
    if (error) *error = rc; x4_live_media_close(m); return NULL;
}
int x4_live_media_start(X4LiveMedia *m)
{
    if (!m || m->started) return X4_LIVE_ERR_STATE;
    int rc = x4_live_video_start(&m->video); if (rc < 0) return rc;
    m->audio = x4_live_audio_start(&m->audio_error, m->audio_payload_type); m->started = true; return 0;
}
int x4_live_media_set_payload_type(X4LiveMedia *m, int kind, int pt)
{
    if (!m || m->started || pt < 0 || pt > 127) return -1;
    if (kind == X4_LIVE_KIND_VIDEO) m->track.forced_type = pt;
    else if (kind == X4_LIVE_KIND_AUDIO) m->audio_payload_type = pt;
    else return -1;
    return 0;
}
void x4_live_media_receive(void *context, int kind, const uint8_t *p, size_t size)
{
    X4LiveMedia *m = context; if (!m || !m->started) return;
    if (kind == X4_LIVE_KIND_AUDIO) { x4_live_audio_receive(m->audio, p, size); return; }
    if (kind != X4_LIVE_KIND_VIDEO) return;
    X4LiveRtp r;
    if (x4_live_rtp_parse(p, size, &r) < 0 || !x4_live_ring_push(&m->ring, p, size)) {
        atomic_fetch_add(&m->dropped, 1); atomic_store(&m->ingress_gap, true); atomic_store(&m->keyframe, true);
    }
    else atomic_fetch_add(&m->packets, 1);
}
void x4_live_media_tick(X4LiveMedia *m)
{
    if (!m || !m->started || m->video.error) return;
    if (atomic_exchange(&m->ingress_gap, false)) reset_au(m, true);
    uint8_t packet[X4_LIVE_RTP_MAX];
    for (unsigned n = 0; n < 512; ++n) {
        size_t size = x4_live_ring_pop(&m->ring, packet); if (!size) break;
        X4LiveRtp r; if (x4_live_rtp_parse(packet, size, &r) < 0) continue;
        bool locked = m->track.locked;
        int track = x4_live_track_accept(&m->track, &r);
        if (track < 0) { atomic_fetch_add(&m->dropped, 1); continue; }
        if (track == X4_LIVE_TRACK_NEW) {
            reset_au(m, true); x4_live_reorder_reset(&m->reorder); m->sps_size = m->pps_size = 0;
            if (locked) { int rc = x4_live_video_stop(&m->video); if (rc < 0) return; if (x4_live_video_start(&m->video) < 0) return; }
        }
        int rc = x4_live_reorder_insert(&m->reorder, packet, size, r.sequence, sceKernelGetProcessTime());
        if (rc == X4_LIVE_REORDER_LATE) atomic_fetch_add(&m->dropped, 1);
        else if (rc == X4_LIVE_REORDER_JUMP) { ++m->lost; reset_au(m, true); }
        /* Consume continuously so high packet rates do not fill the reorder window. */
        for (unsigned j = 0; j < 128; ++j) {
            size_t next_size = 0; uint32_t lost = 0;
            const uint8_t *next = x4_live_reorder_next(&m->reorder, sceKernelGetProcessTime(), &next_size, &lost);
            if (!next) break;
            if (lost) { m->lost += lost; reset_au(m, true); }
            if (x4_live_rtp_parse(next, next_size, &r) == 0) depacketize(m, &r);
            if (m->video.error) return;
        }
    }
    /* A last hole must expire even when no subsequent RTP arrives this frame. */
    for (unsigned n = 0; n < 128; ++n) {
        size_t size = 0; uint32_t lost = 0;
        const uint8_t *p = x4_live_reorder_next(&m->reorder, sceKernelGetProcessTime(), &size, &lost);
        if (!p) break;
        if (lost) { m->lost += lost; reset_au(m, true); }
        X4LiveRtp r; if (x4_live_rtp_parse(p, size, &r) == 0) depacketize(m, &r);
        if (m->video.error) return;
    }
}
int x4_live_media_draw(const X4LiveMedia *m, uint32_t *p)
{
    if (!m || !p || !m->video.frames || !m->video.width || !m->video.height || !m->video.pixels) return 0;
    unsigned w = m->video.width, h = m->video.height, out_w = X4_WIDTH, out_h = X4_HEIGHT;
    if ((uint64_t)w * out_h > (uint64_t)h * out_w) out_h = (unsigned)((uint64_t)out_w * h / w);
    else out_w = (unsigned)((uint64_t)out_h * w / h);
    unsigned ox = (X4_WIDTH - out_w) / 2, oy = (X4_HEIGHT - out_h) / 2;
    memset(p, 0, X4_WIDTH * X4_HEIGHT * sizeof(*p));
    for (unsigned y = 0; y < out_h; ++y) {
        const uint32_t *row = m->video.pixels + (size_t)y * h / out_h * w;
        for (unsigned x = 0; x < out_w; ++x) p[(size_t)(y + oy) * X4_WIDTH + ox + x] = row[(size_t)x * w / out_w];
    }
    return 1;
}
void x4_live_media_snapshot(const X4LiveMedia *m, X4LiveMediaSnapshot *s)
{
    memset(s, 0, sizeof(*s)); if (!m) return;
    X4LiveAudioSnapshot a; x4_live_audio_snapshot(m->audio, &a);
    s->video_packets = atomic_load(&m->packets); s->video_frames = m->video.frames;
    s->video_dropped_packets = atomic_load(&m->dropped); s->video_lost_packets = m->lost; s->video_dropped_frames = m->dropped_frames;
    s->audio_packets = a.packets; s->audio_frames = a.frames; s->audio_dropped_packets = a.dropped;
    s->audio_lost_packets = a.lost; s->audio_dropped_frames = a.bad_frames; s->audio_underflows = a.underflows;
    s->dropped = s->video_dropped_packets + s->video_lost_packets + s->video_dropped_frames + a.dropped + a.lost + a.bad_frames;
    s->width = m->video.width; s->height = m->video.height; s->video_error = m->video.error; s->audio_error = m->audio_error ? m->audio_error : a.error;
    s->started = m->started; s->video_ready = m->video.frames != 0; s->audio_ready = a.frames != 0; s->audio_playing = a.playing;
    s->waiting_keyframe = m->waiting_keyframe; s->keyframe_requests = atomic_load(&m->requests);
}
void x4_live_media_set_muted(X4LiveMedia *m, bool mute) { if (m) x4_live_audio_mute(m->audio, mute); }
bool x4_live_media_take_keyframe_request(X4LiveMedia *m)
{
    if (!m || !atomic_exchange(&m->keyframe, false)) return false;
    atomic_fetch_add(&m->requests, 1);
    return true;
}
int x4_live_media_close(X4LiveMedia *m)
{
    if (!m) return 0;
    int rc = x4_live_audio_stop(m->audio); if (rc < 0) return rc; m->audio = NULL;
    rc = x4_live_video_stop(&m->video); if (rc < 0) return rc;
    x4_live_ring_free(&m->ring); x4_live_reorder_free(&m->reorder); free(m->au); free(m); return 0;
}
