/* SPDX-License-Identifier: GPL-3.0-only */
/* Worker-owned RTP reorder. The legacy audio policy remains sequence-addressed;
 * video uses sparse physical slots independent of its bounded sequence distance.
 * There are no clock calls, transport operations or per-packet allocations. */
#include "live_media.h"
#include "../streaming/receiver_nack_state.h"
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(X4LiveReorderSlot) == 16 && offsetof(X4LiveReorderSlot, arrival) == 8,
    "Video grace must preserve the shared audio reorder slot layout");

int x4_live_reorder_init(X4LiveReorder *q, uint16_t window, uint16_t depth,
    uint64_t wait)
{
    if (!q) return -1;
    memset(q, 0, sizeof(*q));
    if (!window || window > 1024 || (window & (window - 1)) ||
        !depth || depth > window || !wait) return -1;
    q->slots = calloc(window, sizeof(*q->slots));
    q->data = malloc((size_t)window * X4_LIVE_RTP_MAX);
    if (!q->slots || !q->data) {
        x4_live_reorder_free(q);
        return X4_LIVE_ERR_MEMORY;
    }
    q->window = window;
    q->depth = depth;
    q->max_wait = wait;
    return 0;
}

int x4_live_reorder_init_video(X4LiveReorder *q, uint16_t window,
    uint16_t depth, uint64_t wait, X4IngressTicket *tickets)
{
    if (!q) return -1;
    if ((window != X4_LIVE_VIDEO_REORDER_LEGACY_CAPACITY &&
        window != X4_LIVE_VIDEO_REORDER_CAPACITY) || !tickets) {
        memset(q, 0, sizeof(*q));
        return -1;
    }
    int result = x4_live_reorder_init(q, window, depth, wait);
    if (result < 0) return result;
    q->sparse = true;
    q->max_forward = X4_LIVE_VIDEO_REORDER_FORWARD_LIMIT;
    q->tickets = tickets;
    memset(tickets, 0, (size_t)window * sizeof(*tickets));
    return 0;
}

void x4_live_reorder_reset(X4LiveReorder *q)
{
    if (!q) return;
    if (q->slots) memset(q->slots, 0, (size_t)q->window * sizeof(*q->slots));
    if (q->tickets) memset(q->tickets, 0, (size_t)q->window * sizeof(*q->tickets));
    q->started = false;
    q->buffered = 0;
    q->resident_bytes = 0;
    /* Session-global discontinuity budget and direct acceptance count survive
     * an AU/track reset; only a fresh init replenishes the rebase budget. */
}

void x4_live_reorder_free(X4LiveReorder *q)
{
    if (!q) return;
    x4_live_reorder_reset(q);
    free(q->slots);
    free(q->data);
    memset(q, 0, sizeof(*q));
}

uint32_t x4_live_reorder_advance_bound(const X4LiveReorder *q)
{ return q ? (q->sparse ? q->max_forward : q->window) : 0; }

int x4_live_reorder_find(const X4LiveReorder *q, uint16_t seq)
{
    if (!q || !q->window || !q->slots) return -1;
    if (!q->sparse) {
        unsigned at = seq & (q->window - 1u);
        return q->slots[at].used && q->slots[at].sequence == seq ? (int)at : -1;
    }
    for (unsigned i = 0; i < q->window; ++i)
        if (q->slots[i].used && q->slots[i].sequence == seq) return (int)i;
    return -1;
}

bool x4_live_reorder_has(const X4LiveReorder *q, uint16_t seq)
{ return x4_live_reorder_find(q, seq) >= 0; }

uint32_t x4_live_reorder_span(const X4LiveReorder *q)
{
    uint32_t furthest = 0;
    if (!q || !q->slots || !q->started) return 0;
    for (unsigned i = 0; i < q->window; ++i) if (q->slots[i].used) {
        uint32_t offset = (uint16_t)(q->slots[i].sequence - q->expected);
        if (offset > furthest) furthest = offset;
    }
    return furthest;
}

static int insert_legacy(X4LiveReorder *q, const uint8_t *p, size_t size,
    uint16_t seq, uint64_t now)
{
    if (!q->started) { q->expected = seq; q->started = true; }
    int diff = (int16_t)(seq - q->expected);
    int result = X4_LIVE_REORDER_STORED;
    if (diff < 0) {
        q->last_insert_reason = X4_REORDER_INSERT_LATE;
        return X4_LIVE_REORDER_LATE;
    }
    if ((unsigned)diff >= q->window) {
        x4_live_reorder_reset(q);
        q->expected = seq;
        q->started = true;
        q->last_insert_reason = X4_REORDER_INSERT_DISTANCE;
        result = X4_LIVE_REORDER_JUMP;
    }
    unsigned at = seq & (q->window - 1u);
    if (q->slots[at].used) {
        q->last_insert_reason = X4_REORDER_INSERT_DUPLICATE;
        return X4_LIVE_REORDER_LATE;
    }
    memcpy(q->data + (size_t)at * X4_LIVE_RTP_MAX, p, size);
    q->slots[at] = (X4LiveReorderSlot){.sequence = seq,
        .size = (uint16_t)size, .used = true, .arrival = now};
    ++q->buffered;
    q->resident_bytes += (uint32_t)size;
    return result;
}

static int insert_ticket(X4LiveReorder *q, const uint8_t *p, size_t size,
    uint16_t seq, uint64_t now, const X4IngressTicket *ticket, uint8_t grace_ms)
{
    if (!q || !q->window || !q->slots || !q->data || !p || !size ||
        size > X4_LIVE_RTP_MAX) {
        if (q) {
            q->last_insert_reason = X4_REORDER_INSERT_INVALID;
            q->last_insert_us = now;
        }
        return q && q->sparse ? X4_LIVE_REORDER_INVALID : X4_LIVE_REORDER_LATE;
    }
    q->last_insert_reason = X4_REORDER_INSERT_NONE;
    q->last_insert_us = now;
    if (!q->sparse) return insert_legacy(q, p, size, seq, now);
    if (!q->started) { q->expected = seq; q->started = true; }
    uint32_t offset = (uint16_t)(seq - q->expected);
    if (offset == 32768u) {
        q->last_insert_reason = X4_REORDER_INSERT_HALFSPAN;
        return X4_LIVE_REORDER_DISCONTINUITY;
    }
    if (offset > 32768u) {
        q->last_insert_reason = X4_REORDER_INSERT_LATE;
        return X4_LIVE_REORDER_LATE;
    }
    int result = X4_LIVE_REORDER_STORED;
    if (offset >= q->max_forward) {
        q->last_insert_reason = X4_REORDER_INSERT_DISTANCE;
        if (q->discontinuity_rebases >= X4_LIVE_VIDEO_REORDER_REBASE_LIMIT)
            return X4_LIVE_REORDER_DISTANCE;
        ++q->discontinuity_rebases;
        x4_live_reorder_reset(q);
        q->expected = seq;
        q->started = true;
        result = X4_LIVE_REORDER_JUMP;
        offset = 0;
    }
    if (x4_live_reorder_find(q, seq) >= 0) {
        q->last_insert_reason = X4_REORDER_INSERT_DUPLICATE;
        return X4_LIVE_REORDER_DUPLICATE;
    }
    uint32_t byte_limit = (uint32_t)q->window * X4_LIVE_RTP_MAX;
    if (q->buffered >= q->window || q->resident_bytes > byte_limit - size) {
        /* Full expected bypass is a real worker acceptance, not storage. Its
         * borrowed input remains in the already-existing worker stack buffer.
         * The owner consumes it or records an explicit budget/stop discard
         * and recovery before any following reorder operation. */
        if (offset == 0 && q->buffered == q->window &&
            q->resident_bytes <= byte_limit) {
            ++q->expected;
            ++q->direct_packets;
            q->last_insert_reason = X4_REORDER_INSERT_DIRECT;
            return X4_LIVE_REORDER_DIRECT;
        }
        q->last_insert_reason = X4_REORDER_INSERT_CAPACITY;
        return X4_LIVE_REORDER_FULL;
    }
    unsigned at = q->window;
    for (unsigned i = 0; i < q->window; ++i) if (!q->slots[i].used) {
        at = i;
        break;
    }
    if (at == q->window) {
        q->last_insert_reason = X4_REORDER_INSERT_CAPACITY;
        return X4_LIVE_REORDER_FULL;
    }
    memcpy(q->data + (size_t)at * X4_LIVE_RTP_MAX, p, size);
    q->slots[at] = (X4LiveReorderSlot){.sequence = seq,
        .size = (uint16_t)size, .used = true, .video_grace_ms = grace_ms, .arrival = now};
    q->tickets[at] = ticket ? *ticket : (X4IngressTicket){0};
    ++q->buffered;
    q->resident_bytes += (uint32_t)size;
    return result;
}

int x4_live_reorder_insert_ticket(X4LiveReorder *q, const uint8_t *p,
    size_t size, uint16_t seq, uint64_t now, const X4IngressTicket *ticket)
{ return insert_ticket(q, p, size, seq, now, ticket, 60); }

int x4_live_reorder_insert_video_ticket(X4LiveReorder *q, const uint8_t *p,
    size_t size, uint16_t seq, uint64_t now, const X4IngressTicket *ticket,
    unsigned grace_ms)
{
    if (grace_ms < 45) grace_ms = 45;
    if (grace_ms > X4_NACK_STARTUP_GRACE_MS) grace_ms = X4_NACK_STARTUP_GRACE_MS;
    return insert_ticket(q, p, size, seq, now, ticket, (uint8_t)grace_ms);
}

int x4_live_reorder_insert(X4LiveReorder *q, const uint8_t *p, size_t size,
    uint16_t seq, uint64_t now)
{ return x4_live_reorder_insert_ticket(q, p, size, seq, now, NULL); }

static uint32_t nearest_gap(const X4LiveReorder *q, uint64_t *oldest)
{
    uint32_t gap = x4_live_reorder_advance_bound(q);
    for (unsigned i = 0; i < q->window; ++i) if (q->slots[i].used) {
        uint32_t offset = (uint16_t)(q->slots[i].sequence - q->expected);
        if (offset < gap) gap = offset;
        if (oldest && q->slots[i].arrival < *oldest) *oldest = q->slots[i].arrival;
    }
    return gap;
}

static uint32_t video_gap(const X4LiveReorder *q, unsigned current_ms,
    uint64_t *deadline_us, bool *startup)
{
    uint32_t gap = q->max_forward;
    const X4LiveReorderSlot *first = NULL;
    unsigned remaining = q->buffered;
    for (unsigned i = 0; i < q->window && remaining; ++i) {
        const X4LiveReorderSlot *slot = &q->slots[i];
        if (!slot->used) continue;
        --remaining;
        uint32_t offset = (uint16_t)(slot->sequence - q->expected);
        if (!offset) return 0;
        if (offset < gap) gap = offset;
        if (!first || slot->arrival < first->arrival ||
            (slot->arrival == first->arrival && slot->video_grace_ms < first->video_grace_ms))
            first = slot;
    }
    if (gap >= q->max_forward || !first) return 0;
    if (deadline_us) {
        bool during_startup = first->video_grace_ms == X4_NACK_STARTUP_GRACE_MS;
        if (startup) *startup = during_startup;
        uint64_t grace_us = 1000u * (uint64_t)(current_ms && !during_startup ?
            current_ms : first->video_grace_ms);
        *deadline_us = first->arrival > UINT64_MAX - grace_us ?
            UINT64_MAX : first->arrival + grace_us;
    }
    return gap;
}

uint32_t x4_live_reorder_video_gap_policy(const X4LiveReorder *q, unsigned current_ms,
    uint64_t *deadline_us, bool *startup)
{
    if (deadline_us) *deadline_us = 0;
    if (startup) *startup = false;
    if (!q || !q->sparse || q->window != X4_LIVE_VIDEO_REORDER_CAPACITY ||
        !q->started || !q->buffered) return 0;
    return video_gap(q, current_ms, deadline_us, startup);
}

uint32_t x4_live_reorder_video_gap(const X4LiveReorder *q, uint64_t *deadline_us)
{ return x4_live_reorder_video_gap_policy(q, 0, deadline_us, NULL); }

uint32_t x4_live_reorder_abandon_gap_limited(X4LiveReorder *q,
    uint32_t advance_limit)
{
    if (!q || !q->started || !q->buffered ||
        x4_live_reorder_has(q, q->expected)) return 0;
    uint32_t gap = nearest_gap(q, NULL);
    if (gap >= x4_live_reorder_advance_bound(q)) return 0;
    if (advance_limit < gap) gap = advance_limit;
    if (!gap) return 0;
    q->expected = (uint16_t)(q->expected + gap);
    return gap;
}

uint32_t x4_live_reorder_abandon_gap(X4LiveReorder *q)
{ return x4_live_reorder_abandon_gap_limited(q, x4_live_reorder_advance_bound(q)); }

static const uint8_t *emit_slot(X4LiveReorder *q, unsigned at, size_t *size,
    X4IngressTicket *ticket)
{
    X4LiveReorderSlot *slot = &q->slots[at];
    *size = slot->size;
    if (q->tickets) {
        if (ticket) *ticket = q->tickets[at];
        q->tickets[at] = (X4IngressTicket){0};
    }
    q->resident_bytes -= slot->size;
    slot->used = false;
    --q->buffered;
    ++q->expected;
    return q->data + (size_t)at * X4_LIVE_RTP_MAX;
}

const uint8_t *x4_live_reorder_next_ticket_limited(X4LiveReorder *q,
    uint64_t now, size_t *size, uint32_t *lost, unsigned *reason,
    bool force_gap, uint32_t advance_limit, X4IngressTicket *ticket)
{
    if (reason) *reason = 0;
    if (ticket) *ticket = (X4IngressTicket){0};
    if (!size || !lost) return NULL;
    *size = 0;
    *lost = 0;
    if (!q || !q->started || !q->buffered) return NULL;
    int at = x4_live_reorder_find(q, q->expected);
    if (at < 0) {
        uint64_t oldest = now;
        uint32_t gap = nearest_gap(q, &oldest);
        /* now0 suppresses only ordinary expiry; depth still applies. Sparse
         * distance is an independent bound, not a count of occupied slots. */
        if (gap >= x4_live_reorder_advance_bound(q) ||
            (!force_gap && q->buffered < q->depth &&
            (!now || now - oldest < q->max_wait))) return NULL;
        bool limited = advance_limit < gap;
        if (limited) gap = advance_limit;
        if (!gap) return NULL;
        if (reason) *reason = (q->buffered >= q->depth ? 1u : 0u) |
            (now && now - oldest >= q->max_wait ? 2u : 0u) |
            (force_gap ? 8u : 0u) | (limited ? 16u : 0u);
        q->expected = (uint16_t)(q->expected + gap);
        *lost = gap;
        at = x4_live_reorder_find(q, q->expected);
    }
    return at < 0 ? NULL : emit_slot(q, (unsigned)at, size, ticket);
}

const uint8_t *x4_live_reorder_next_video_ticket_limited(X4LiveReorder *q,
    uint64_t now, size_t *size, uint32_t *lost, unsigned *reason,
    bool force_gap, uint32_t advance_limit, X4IngressTicket *ticket)
{
    if (reason) *reason = 0;
    if (ticket) *ticket = (X4IngressTicket){0};
    if (!size || !lost) return NULL;
    *size = 0;
    *lost = 0;
    if (!q || !q->sparse || q->window != X4_LIVE_VIDEO_REORDER_CAPACITY ||
        !q->started || !q->buffered) return NULL;
    int at = x4_live_reorder_find(q, q->expected);
    if (at < 0) {
        uint64_t deadline = 0;
        uint32_t gap = video_gap(q, 0, &deadline, NULL);
        bool too_long = gap > q->window;
        bool expired = now && now >= deadline;
        if (!gap || (!force_gap && !expired && !too_long)) return NULL;
        bool limited = advance_limit < gap;
        if (limited) gap = advance_limit;
        if (!gap) return NULL;
        if (reason) *reason = (expired ? 2u : 0u) |
            (force_gap || too_long ? 8u : 0u) | (limited ? 16u : 0u);
        q->expected = (uint16_t)(q->expected + gap);
        *lost = gap;
        at = x4_live_reorder_find(q, q->expected);
    }
    return at < 0 ? NULL : emit_slot(q, (unsigned)at, size, ticket);
}

const uint8_t *x4_live_reorder_next_limited(X4LiveReorder *q, uint64_t now,
    size_t *size, uint32_t *lost, unsigned *reason, bool force_gap,
    uint32_t advance_limit)
{ return x4_live_reorder_next_ticket_limited(q, now, size, lost, reason,
    force_gap, advance_limit, NULL); }

const uint8_t *x4_live_reorder_next_reason(X4LiveReorder *q, uint64_t now,
    size_t *size, uint32_t *lost, unsigned *reason, bool force_gap)
{ return x4_live_reorder_next_limited(q, now, size, lost, reason, force_gap,
    x4_live_reorder_advance_bound(q)); }

const uint8_t *x4_live_reorder_next(X4LiveReorder *q, uint64_t now,
    size_t *size, uint32_t *lost)
{ return x4_live_reorder_next_reason(q, now, size, lost, NULL, false); }
