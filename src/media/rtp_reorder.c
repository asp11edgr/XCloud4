/* SPDX-License-Identifier: GPL-3.0-only */
/* Existing worker-owned RTP reorder extracted for portable host validation.
 * No clock call, transport, trace, recovery or decode policy is added here. */
#include "live_media.h"
#include <stdlib.h>
#include <string.h>

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
uint32_t x4_live_reorder_abandon_gap_limited(X4LiveReorder *q,uint32_t advance_limit)
{
    if(!q||!q->started||!q->buffered) return 0;
    unsigned at=q->expected&(q->window-1);
    if(q->slots[at].used&&q->slots[at].sequence==q->expected) return 0;
    unsigned gap=q->window;
    for(unsigned i=0;i<q->window;++i) if(q->slots[i].used) {
        unsigned offset=(uint16_t)(q->slots[i].sequence-q->expected);
        if(offset<gap) gap=offset;
    }
    if(gap>=q->window) return 0;
    if(advance_limit<gap) gap=advance_limit;
    if(!gap) return 0;
    q->expected+=gap;
    return gap;
}
uint32_t x4_live_reorder_abandon_gap(X4LiveReorder *q)
{ return x4_live_reorder_abandon_gap_limited(q,q?q->window:0); }
const uint8_t *x4_live_reorder_next_limited(X4LiveReorder *q, uint64_t now, size_t *size, uint32_t *lost, unsigned *reason, bool force_gap, uint32_t advance_limit)
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
        if (gap >= q->window || (!force_gap && q->buffered < q->depth && (!now || now - oldest < q->max_wait))) return NULL;
        bool limited=advance_limit<gap;
        if(limited) gap=advance_limit;
        if(!gap) return NULL;
        if (reason) *reason = (q->buffered >= q->depth ? 1u : 0u) |
            (now && now - oldest >= q->max_wait ? 2u : 0u) | (force_gap ? 8u : 0u) | (limited ? 16u : 0u);
        q->expected += gap; *lost = gap; at = q->expected & (q->window - 1);
    }
    X4LiveReorderSlot *s = &q->slots[at];
    if (!s->used || s->sequence != q->expected) return NULL;
    *size = s->size; s->used = false; --q->buffered; ++q->expected;
    return q->data + (size_t)at * X4_LIVE_RTP_MAX;
}
const uint8_t *x4_live_reorder_next_reason(X4LiveReorder *q, uint64_t now, size_t *size, uint32_t *lost, unsigned *reason, bool force_gap)
{ return x4_live_reorder_next_limited(q,now,size,lost,reason,force_gap,q?q->window:0); }
const uint8_t *x4_live_reorder_next(X4LiveReorder *q, uint64_t now, size_t *size, uint32_t *lost)
{ return x4_live_reorder_next_reason(q, now, size, lost, NULL, false); }
