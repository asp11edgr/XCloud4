/* SPDX-License-Identifier: GPL-3.0-only */
#include "rtc_receive_diag.h"
#include "../media/live_trace.h"
#include <errno.h>
#include <stdatomic.h>
#ifndef X4_RECEIVE_DIAG_HOST_TEST
#include <orbis/libkernel.h>
/* Already used by rtc_pthread.c's native syscall(20) adapter. */
extern int32_t scePthreadGetthreadid(void);
#else
extern uint64_t x4_receive_test_clock(void);
extern void x4_receive_test_sleep(unsigned);
extern uint64_t x4_receive_test_threadid(void);
#endif

/* XCloud4 owns exactly one RTC session at a time. A process-lifetime bridge
 * observes only that session. Clearing the sink then draining entered readers
 * prevents diagnostic storage from being freed under an upstream producer.
 * New producers after detach see NULL. Producers never spin or sleep. */
static _Atomic(X4Trace *) sink;
static atomic_uint producers;
/* Compiler TLS is not assumed to work in an OpenOrbis SELF. These fixed,
 * process-lifetime cells use the existing native pthread identity API.
 * Each identity owns its timestamps; an exhausted lookup returns unknown.
 * No allocation, lock, wait or unbounded retry occurs on a receive path. */
enum { RECEIVE_THREAD_CAP = 32 };
typedef struct {
    atomic_uint_fast64_t key, socket_time, delivery_time;
} ReceiveThread;
static ReceiveThread thread_slots[RECEIVE_THREAD_CAP];
static atomic_uint_fast64_t thread_omissions;
static atomic_uint_fast64_t binding_count;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2 &&
    ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
    "Receive diagnostics require lock-free bridge atomics");
_Static_assert((int)X4_RD_APP == (int)X4_MON_T_APP_CALLBACK &&
    (int)X4_RD_RESERVED == (int)X4_MON_T_RESERVED,
    "Receive stage ABI must match the monitor");

static ReceiveThread *thread_slot(void)
{
    int saved = errno;
#ifndef X4_RECEIVE_DIAG_HOST_TEST
    int32_t native_id = scePthreadGetthreadid();
    uint64_t key = native_id > 0 ? (uint32_t)native_id : 0;
#else
    uint64_t key = x4_receive_test_threadid();
#endif
    errno = saved;
    if (key) for (unsigned i = 0; i < RECEIVE_THREAD_CAP; ++i) {
        uint64_t observed = atomic_load_explicit(&thread_slots[i].key, memory_order_relaxed);
        if (!observed) {
            uint64_t empty = 0;
            if (atomic_compare_exchange_strong_explicit(&thread_slots[i].key, &empty,
                key, memory_order_relaxed, memory_order_relaxed)) observed = key;
            else observed = empty;
        }
        if (observed == key) return &thread_slots[i];
    }
    atomic_fetch_add_explicit(&thread_omissions, 1, memory_order_relaxed);
    return NULL;
}

uint64_t x4_rtc_receive_clock(void)
{
    int saved = errno;
#ifndef X4_RECEIVE_DIAG_HOST_TEST
    uint64_t now = sceKernelGetProcessTime();
#else
    uint64_t now = x4_receive_test_clock();
#endif
    errno = saved;
    return now;
}

/* Only fixed RTP header fields are inspected. Header-extension and padding
 * bytes may still be encrypted in SRTP: no payload, extension, OSN or key is
 * read here. A pre-authentication tuple is explicitly untrusted metadata. */
static bool tuple(const void *packet, size_t size, uint32_t *ssrc,
    uint16_t *seq, unsigned *pt, uint32_t *flags)
{
    const uint8_t *p = packet;
    if (!p || size < 2 || (p[0] >> 6) != 2) return false;
    if (p[1] >= 192 && p[1] <= 223) {
        *flags |= X4_RD_RTCP_FLAG;
        return false;
    }
    if (size < 12 || size < 12u + (p[0] & 15u) * 4u) return false;
    *seq = ((uint16_t)p[2] << 8) | p[3];
    *ssrc = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
        ((uint32_t)p[10] << 8) | p[11];
    *pt = p[1] & 127u;
    return true;
}

static X4Trace *enter(void)
{
    /* seq_cst is intentional: detach's null publication and count drain must
     * order against both the producer's count and its subsequent sink load. */
    atomic_fetch_add_explicit(&producers, 1, memory_order_seq_cst);
    return atomic_load_explicit(&sink, memory_order_seq_cst);
}
static void leave(void)
{ atomic_fetch_sub_explicit(&producers, 1, memory_order_seq_cst); }

uint64_t x4_rtc_receive_packet(unsigned stage, const void *packet, size_t bytes,
    uint64_t preceding_time, uint32_t flags)
{
    int saved = errno;
    uint64_t now = x4_rtc_receive_clock();
    if (stage == X4_RD_UDP || stage == X4_RD_TCP || stage == X4_RD_TRACK_POP) {
        ReceiveThread *slot = thread_slot();
        if (slot) atomic_store_explicit(stage == X4_RD_TRACK_POP ?
            &slot->delivery_time : &slot->socket_time, now, memory_order_relaxed);
    }
    X4Trace *trace = enter();
    if (trace) {
        uint32_t ssrc = 0; uint16_t seq = 0; unsigned pt = 0;
        bool valid = tuple(packet, bytes, &ssrc, &seq, &pt, &flags);
        x4_trace_monitor_transport_packet(trace, stage, valid, ssrc, seq, pt,
            bytes, now, preceding_time, flags);
    }
    leave();
    errno = saved;
    return now;
}

void x4_rtc_receive_reject(unsigned stage, unsigned reason, int result,
    const void *packet, size_t bytes)
{
    int saved = errno;
    uint64_t now = x4_rtc_receive_clock();
    X4Trace *trace = enter();
    if (trace) {
        uint32_t ssrc = 0, flags = 0; uint16_t seq = 0; unsigned pt = 0;
        bool valid = tuple(packet, bytes, &ssrc, &seq, &pt, &flags);
        x4_trace_monitor_transport_reject(trace, stage, reason, result,
            valid, ssrc, seq, now);
    }
    leave();
    errno = saved;
}
uint64_t x4_rtc_receive_socket_time(void)
{
    ReceiveThread *slot = thread_slot();
    return slot ? atomic_load_explicit(&slot->socket_time,memory_order_relaxed) : 0;
}
uint64_t x4_rtc_receive_delivery_time(void)
{
    ReceiveThread *slot = thread_slot();
    return slot ? atomic_load_explicit(&slot->delivery_time,memory_order_relaxed) : 0;
}
void x4_rtc_receive_setting(unsigned setting, uint64_t a, uint64_t b, uint64_t c)
{
    int saved = errno;
    uint64_t now = x4_rtc_receive_clock();
    X4Trace *trace = enter();
    if (trace) x4_trace_monitor_critical(trace, now, X4_MON_CR_RTC_SETTING,
        0xffffu, 0, setting, 0, a, b, c);
    leave();
    errno = saved;
}
void x4_rtc_receive_attach(X4Trace *trace)
{
    uint64_t bindings = atomic_fetch_add_explicit(&binding_count,1,memory_order_relaxed)+1;
    atomic_store_explicit(&sink, trace, memory_order_seq_cst);
    unsigned used=0;
    for(unsigned i=0;i<RECEIVE_THREAD_CAP;++i)
        if(atomic_load_explicit(&thread_slots[i].key,memory_order_relaxed))++used;
    x4_rtc_receive_setting(22,atomic_load_explicit(&thread_omissions,memory_order_relaxed),used,0);
    x4_rtc_receive_setting(23,bindings,1,0);
}
void x4_rtc_receive_detach(X4Trace *trace)
{
    X4Trace *expected = trace;
    if (!atomic_compare_exchange_strong_explicit(&sink, &expected, NULL,
        memory_order_seq_cst, memory_order_seq_cst)) return;
    while (atomic_load_explicit(&producers, memory_order_seq_cst)) {
#ifndef X4_RECEIVE_DIAG_HOST_TEST
        sceKernelUsleep(1000);
#else
        x4_receive_test_sleep(1000);
#endif
    }
    /* Owner still owns trace after draining entered hooks. Record final
     * coverage directly; never temporarily republish a detached sink.
     * The bridge has global scope, not an old/new transport epoch binding. */
    if (trace) {
        unsigned used=0;
        for(unsigned i=0;i<RECEIVE_THREAD_CAP;++i)
            if(atomic_load_explicit(&thread_slots[i].key,memory_order_relaxed))++used;
        uint64_t now=x4_rtc_receive_clock();
        x4_trace_monitor_critical(trace,now,X4_MON_CR_RTC_SETTING,0xffffu,0,22,0,
            atomic_load_explicit(&thread_omissions,memory_order_relaxed),used,1);
        x4_trace_monitor_critical(trace,now,X4_MON_CR_RTC_SETTING,0xffffu,0,23,0,
            atomic_load_explicit(&binding_count,memory_order_relaxed),1,0);
    }
}
