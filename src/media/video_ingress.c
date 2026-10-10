/* SPDX-License-Identifier: GPL-3.0-only */
#include "video_ingress.h"
#include <stdlib.h>
#include <string.h>
#ifdef X4_INGRESS_HOST_TEST
extern uint64_t x4_ingress_test_now_us(void);
#define ingress_now x4_ingress_test_now_us
#else
#include <orbis/libkernel.h>
#define ingress_now sceKernelGetProcessTime
#endif
#ifdef X4_INGRESS_TEST_HOOKS
#define HOOK(q, event, pos) x4_ingress_test_hook(q, event, pos)
#else
#define HOOK(q, event, pos) ((void)0)
#endif

int x4_video_ingress_init(X4VideoIngress *q, uint32_t capacity)
{
    if (!q) return -1;
    memset(q, 0, sizeof(*q));
    atomic_init(&q->enqueue, 0); atomic_init(&q->dequeue, 0);
    atomic_init(&q->accepting, false); atomic_init(&q->active_ops, 0);
    atomic_init(&q->highwater, 0); atomic_init(&q->full, 0);
    atomic_init(&q->contended, 0); atomic_init(&q->invalid, 0);
    atomic_init(&q->stopped, 0); atomic_init(&q->retries, 0);
    atomic_init(&q->baseline_owner, 0);
    atomic_init(&q->pop_contended, 0);
    for (unsigned i = 0; i < 3; ++i) atomic_init(&q->contended_owner[i], 0);
    if (!capacity || capacity > 1024 || (capacity & (capacity - 1))) return -1;
    /* PS4 x86-64 must use native atomics, never a hidden locking fallback. */
    if (!atomic_is_lock_free(&q->enqueue) || !atomic_is_lock_free(&q->accepting)) return -1;
    q->slots = calloc(capacity, sizeof(*q->slots));
    if (!q->slots) return -1;
    q->capacity = capacity;
    for (uint32_t i = 0; i < capacity; ++i) {
        atomic_init(&q->slots[i].ready, false);
        atomic_init(&q->slots[i].arrival_us, 0);
    }
    atomic_store_explicit(&q->accepting, true, memory_order_release);
    return 0;
}
uint32_t x4_video_ingress_depth(const X4VideoIngress *q)
{
    if (!q || !q->capacity) return 0;
    uint64_t head = atomic_load_explicit(&q->dequeue, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&q->enqueue, memory_order_acquire);
    uint64_t depth = tail - head;
    /* Independently sampled, includes reserved but not yet published slots. */
    return depth > q->capacity ? q->capacity : (uint32_t)depth;
}
uint64_t x4_video_ingress_oldest(const X4VideoIngress *q)
{
    if (!q || !q->capacity) return 0;
    uint64_t head = atomic_load_explicit(&q->dequeue, memory_order_acquire);
    const X4IngressSlot *slot = &q->slots[head & (q->capacity - 1)];
    if (!atomic_load_explicit(&slot->ready, memory_order_acquire)) return 0;
    uint64_t arrival = atomic_load_explicit(&slot->arrival_us, memory_order_relaxed);
    /* Atomic metadata only: a reused slot cannot cause a C data race. */
    return head == atomic_load_explicit(&q->dequeue, memory_order_acquire) ? arrival : 0;
}
static void highwater(X4VideoIngress *q, unsigned depth)
{
    unsigned old = atomic_load_explicit(&q->highwater, memory_order_relaxed);
    /* A failed strong CAS means another update increased this value. It
     * cannot advance more than capacity times; clamp samples to capacity. */
    for (unsigned n = 0; n < q->capacity && old < depth; ++n)
        if (atomic_compare_exchange_strong_explicit(&q->highwater, &old, depth,
            memory_order_relaxed, memory_order_relaxed)) break;
}
bool x4_video_ingress_push(X4VideoIngress *q, const uint8_t *p, size_t size,
    uint64_t arrival, X4IngressResult *result)
{
    X4IngressResult r = { .reason = X4_INGRESS_INVALID };
    if (!q || !q->capacity) { if (result) *result = r; return false; }
    atomic_fetch_add_explicit(&q->active_ops, 1, memory_order_acquire);
    if (!p || !size || size > X4_INGRESS_PACKET_MAX) {
        atomic_fetch_add_explicit(&q->invalid, 1, memory_order_relaxed); goto done;
    }
    uint64_t begin = ingress_now();
    uint64_t position = atomic_load_explicit(&q->enqueue, memory_order_relaxed);
#ifdef X4_INGRESS_BASELINE
    /* Instrumented A/B/A baseline: same try-only, copy-under-gate behavior
     * as 0.7.29. Encode ownership in the gate itself, so failed CAS supplies
     * the actual owner observed at that operation (1 push, 2 pop). */
    unsigned owner = 0;
    if (!atomic_load_explicit(&q->accepting, memory_order_acquire)) r.reason = X4_INGRESS_STOPPED;
    else if (!atomic_compare_exchange_strong_explicit(&q->baseline_owner, &owner, 1,
        memory_order_acquire, memory_order_relaxed)) {
        r.reason = X4_INGRESS_CONTENDED;
        r.owner_sample = owner;
    } else {
        position = atomic_load_explicit(&q->enqueue, memory_order_relaxed);
        uint64_t head = atomic_load_explicit(&q->dequeue, memory_order_relaxed);
        if (position - head == q->capacity) r.reason = X4_INGRESS_FULL;
        else {
            r.reason = X4_INGRESS_OK; r.reserve_us = ingress_now() - begin;
            HOOK(q, X4_INGRESS_AFTER_RESERVE, position);
            X4IngressSlot *slot = &q->slots[position & (q->capacity - 1)];
            memcpy(slot->data, p, size); slot->size = (uint16_t)size;
            atomic_store_explicit(&slot->arrival_us, arrival, memory_order_relaxed);
            atomic_store_explicit(&slot->ready, true, memory_order_release);
            atomic_store_explicit(&q->enqueue, position + 1, memory_order_relaxed);
            highwater(q, (unsigned)(position - head) + 1);
        }
        atomic_store_explicit(&q->baseline_owner, 0, memory_order_release);
        if (r.reason == X4_INGRESS_OK) goto done;
    }
#else
    r.reason = X4_INGRESS_CONTENDED;
    for (unsigned n = 0; n < X4_INGRESS_RESERVE_ATTEMPTS; ++n) {
        if (!atomic_load_explicit(&q->accepting, memory_order_acquire)) {
            r.reason = X4_INGRESS_STOPPED; break;
        }
        uint64_t head = atomic_load_explicit(&q->dequeue, memory_order_acquire);
        uint64_t distance = position - head; /* unsigned modulo, wrap safe */
        if (distance == q->capacity) { r.reason = X4_INGRESS_FULL; break; }
        if (distance > q->capacity) {
            /* Another producer and the owner passed our stale position. */
            position = atomic_load_explicit(&q->enqueue, memory_order_relaxed);
            ++r.retries; continue;
        }
        HOOK(q, X4_INGRESS_BEFORE_CAS, position);
        if (atomic_compare_exchange_strong_explicit(&q->enqueue, &position, position + 1,
            memory_order_relaxed, memory_order_relaxed)) {
            r.reason = X4_INGRESS_OK;
            r.reserve_us = ingress_now() - begin;
            highwater(q, (unsigned)distance + 1);
            HOOK(q, X4_INGRESS_AFTER_RESERVE, position);
            X4IngressSlot *slot = &q->slots[position & (q->capacity - 1)];
            memcpy(slot->data, p, size); slot->size = (uint16_t)size;
            atomic_store_explicit(&slot->arrival_us, arrival, memory_order_relaxed);
            atomic_store_explicit(&slot->ready, true, memory_order_release);
            goto done;
        }
        ++r.retries;
    }
#endif
    r.reserve_us = ingress_now() - begin;
    if (r.reason == X4_INGRESS_FULL) atomic_fetch_add_explicit(&q->full, 1, memory_order_relaxed);
    else if (r.reason == X4_INGRESS_STOPPED) atomic_fetch_add_explicit(&q->stopped, 1, memory_order_relaxed);
    else {
        atomic_fetch_add_explicit(&q->contended, 1, memory_order_relaxed);
#ifdef X4_INGRESS_BASELINE
        atomic_fetch_add_explicit(&q->contended_owner[r.owner_sample <= 2 ? r.owner_sample : 0], 1,
            memory_order_relaxed);
#endif
    }
done:
    atomic_fetch_add_explicit(&q->retries, r.retries, memory_order_relaxed);
    atomic_fetch_sub_explicit(&q->active_ops, 1, memory_order_release);
    if (result) *result = r;
    return r.reason == X4_INGRESS_OK;
}
size_t x4_video_ingress_pop(X4VideoIngress *q, uint8_t *out, uint64_t *arrival)
{
    if (arrival) *arrival = 0;
    if (!q || !q->capacity || !out) return 0;
    atomic_fetch_add_explicit(&q->active_ops, 1, memory_order_acquire);
#ifdef X4_INGRESS_BASELINE
    unsigned owner = 0;
    if (!atomic_compare_exchange_strong_explicit(&q->baseline_owner, &owner, 2,
        memory_order_acquire, memory_order_relaxed)) {
        atomic_fetch_add_explicit(&q->pop_contended, 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(&q->active_ops, 1, memory_order_release); return 0;
    }
#endif
    uint64_t head = atomic_load_explicit(&q->dequeue, memory_order_relaxed);
    X4IngressSlot *slot = &q->slots[head & (q->capacity - 1)];
    size_t size = 0;
    if (atomic_load_explicit(&slot->ready, memory_order_acquire)) {
        size = slot->size; memcpy(out, slot->data, size);
        if (arrival) *arrival = atomic_load_explicit(&slot->arrival_us, memory_order_relaxed);
        HOOK(q, X4_INGRESS_BEFORE_RELEASE, head);
        atomic_store_explicit(&slot->ready, false, memory_order_relaxed);
        atomic_store_explicit(&q->dequeue, head + 1, memory_order_release);
    }
#ifdef X4_INGRESS_BASELINE
    atomic_store_explicit(&q->baseline_owner, 0, memory_order_release);
#endif
    atomic_fetch_sub_explicit(&q->active_ops, 1, memory_order_release);
    return size;
}
void x4_video_ingress_stop(X4VideoIngress *q)
{ if (q) atomic_store_explicit(&q->accepting, false, memory_order_release); }
bool x4_video_ingress_free(X4VideoIngress *q)
{
    if (!q) return true;
    if (atomic_load_explicit(&q->accepting, memory_order_acquire) ||
        atomic_load_explicit(&q->active_ops, memory_order_acquire)) return false;
    free(q->slots); q->slots = NULL; q->capacity = 0;
    return true;
}
