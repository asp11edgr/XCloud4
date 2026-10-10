/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { X4_INGRESS_PACKET_MAX = 2048, X4_INGRESS_RESERVE_ATTEMPTS = 16 };
enum { X4_INGRESS_OK, X4_INGRESS_INVALID, X4_INGRESS_CONTENDED,
    X4_INGRESS_FULL, X4_INGRESS_STOPPED };
typedef struct {
    atomic_bool ready;
    uint16_t size;
    atomic_uint_fast64_t arrival_us; /* independently sampled diagnostics */
    uint8_t data[X4_INGRESS_PACKET_MAX];
} X4IngressSlot;
typedef struct {
    X4IngressSlot *slots;
    uint32_t capacity;
    atomic_uint_fast64_t enqueue, dequeue;
    atomic_bool accepting;
    atomic_uint active_ops, highwater;
    atomic_uint_fast64_t full, contended, invalid, stopped, retries;
    /* Diagnostic baseline only: candidate never acquires this gate. */
    atomic_uint baseline_owner;
    atomic_uint_fast64_t pop_contended;
    atomic_uint_fast64_t contended_owner[3]; /* failed baseline CAS: 0 unknown, 1 push, 2 pop */
} X4VideoIngress;
typedef struct { unsigned reason, retries, owner_sample; uint64_t reserve_us; } X4IngressResult;

/* MPSC, one video owner consumes. Allocate once, before callbacks start.
 * A successful CAS reserves one slot; ready-release publishes its payload.
 * The owner releases dequeue only AFTER copying and clearing ready, so a
 * producer cannot overwrite a slot being read. Producers do not take a
 * consumer lock. An unpublished reservation preserves FIFO and returns no
 * packet to the consumer; it is not a loss or an empty queue.
 * Reservation attempts are bounded; there is no per-packet allocation,
 * sleep, blocking mutex or retry-until-success. */
int x4_video_ingress_init(X4VideoIngress *q, uint32_t capacity);
bool x4_video_ingress_push(X4VideoIngress *q, const uint8_t *p, size_t size,
    uint64_t arrival_us, X4IngressResult *result);
size_t x4_video_ingress_pop(X4VideoIngress *q, uint8_t *out, uint64_t *arrival_us);
uint32_t x4_video_ingress_depth(const X4VideoIngress *q);
uint64_t x4_video_ingress_oldest(const X4VideoIngress *q);
void x4_video_ingress_stop(X4VideoIngress *q);
/* Stop, join all producers AND the sole consumer, then free. An active-op
 * check retains memory on a violated precondition; it does not authorize
 * racing free with new callers. In-flight reservations may finish at stop. */
bool x4_video_ingress_free(X4VideoIngress *q);

#ifdef X4_INGRESS_TEST_HOOKS
enum { X4_INGRESS_AFTER_RESERVE = 1, X4_INGRESS_BEFORE_RELEASE,
    X4_INGRESS_BEFORE_CAS };
void x4_ingress_test_hook(X4VideoIngress *q, unsigned event, uint64_t position);
#endif
