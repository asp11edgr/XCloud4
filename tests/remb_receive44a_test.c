/* SPDX-License-Identifier: GPL-3.0-only */
#include "rtc_receive_diag.h"
#include "live_trace.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

static atomic_uint_fast64_t ticks;

uint64_t x4_receive_test_clock(void) { return atomic_fetch_add(&ticks, 1); }
void x4_receive_test_sleep(unsigned us) { usleep(us); }
uint64_t x4_receive_test_threadid(void) { return 1; }

void x4_trace_monitor_transport_packet(X4Trace *trace, unsigned stage, bool valid, uint32_t ssrc,
    uint16_t seq, unsigned pt, size_t bytes, uint64_t time, uint64_t prior, uint32_t flags)
{
    (void)trace; (void)stage; (void)valid; (void)ssrc; (void)seq;
    (void)pt; (void)bytes; (void)time; (void)prior; (void)flags;
    assert(false);
}
void x4_trace_monitor_transport_reject(X4Trace *trace, unsigned stage, unsigned reason,
    int result, bool valid, uint32_t ssrc, uint16_t seq, uint64_t time)
{
    (void)trace; (void)stage; (void)reason; (void)result; (void)valid; (void)ssrc; (void)seq; (void)time;
    assert(false);
}
void x4_trace_monitor_critical(X4Trace *trace, uint64_t time, unsigned event, unsigned stage,
    uint32_t flags, uint64_t identity, uint64_t source, uint64_t a, uint64_t b, uint64_t c)
{
    (void)trace; (void)time; (void)event; (void)stage; (void)flags;
    (void)identity; (void)source; (void)a; (void)b; (void)c;
    assert(false);
}

static void packet(unsigned stage, uint32_t flags, uint8_t pt, uint32_t ssrc,
    uint16_t sequence, uint32_t timestamp)
{
    const uint8_t bytes[32] = {0x80, pt, sequence >> 8, sequence & 255u,
        timestamp >> 24, (timestamp >> 16) & 255u, (timestamp >> 8) & 255u, timestamp & 255u,
        ssrc >> 24, (ssrc >> 16) & 255u, (ssrc >> 8) & 255u, ssrc & 255u};
    (void)x4_rtc_receive_packet(stage, bytes, sizeof(bytes), 0, flags);
}

static void video(uint16_t sequence, uint32_t timestamp, uint32_t flags)
{
    packet(X4_RD_PEER_OUT, X4_RD_AUTHENTICATED | flags, 102, 7, sequence, timestamp);
}

static void sources_and_wrap(void)
{
    X4RembReceiveWindow window = {0};
    x4_rtc_diag_remb_begin(1, 7, 102);
    video(65535, 0, X4_RD_RTX_NORMALIZED);
    packet(X4_RD_PEER_OUT, X4_RD_AUTHENTICATED, 111, 7, 65535, 0);
    packet(X4_RD_PEER_OUT, X4_RD_AUTHENTICATED, 102, 8, 65535, 0);
    packet(X4_RD_PEER_OUT, X4_RD_UNAUTHENTICATED, 102, 7, 65535, 0);
    packet(X4_RD_SRTP_VALID, X4_RD_AUTHENTICATED, 102, 7, 65535, 0);
    assert(!x4_rtc_diag_remb_take(1, &window));
    atomic_store(&ticks, 1000000);
    video(65535, UINT32_MAX - 899u, 0);
    atomic_store(&ticks, 1050000);
    video(0, 0, 0);
    video(0, 0, X4_RD_RTX_NORMALIZED);
    video(1, UINT32_MAX - 6000u, X4_RD_FEC_RECOVERED);
    video(1, UINT32_MAX - 6000u, X4_RD_RTX_NORMALIZED);
    video(2, 0, X4_RD_RETRANSMISSION);
    assert(x4_rtc_diag_remb_take(1, &window));
    assert(window.video_ssrc == 7 && window.packets == 3 && window.bytes == 60);
    assert(window.delay_valid && window.delay_us == 10000);
    video(0, 0, X4_RD_RTX_NORMALIZED);
    assert(x4_rtc_diag_remb_take(1, &window));
    assert(!window.bytes && !window.packets && !window.delay_valid);
    /* 16-byte header with CSRC, 8-byte extension, 4 payload bytes and 4 padding bytes. */
    uint8_t extended[32] = {0xb1, 102, 0, 2, [11] = 7, [19] = 1, [31] = 4};
    extended[19] = 4;
    (void)x4_rtc_receive_packet(X4_RD_PEER_OUT, extended, sizeof(extended), 0, X4_RD_AUTHENTICATED);
    extended[19] = 1;
    (void)x4_rtc_receive_packet(X4_RD_PEER_OUT, extended, sizeof(extended), 0, X4_RD_AUTHENTICATED);
    assert(x4_rtc_diag_remb_take(1, &window));
    assert(window.bytes == 4 && window.packets == 1 && !window.delay_valid);
    x4_rtc_diag_remb_end(1);
    assert(!x4_rtc_diag_remb_take(1, &window));
    puts("PASS REMB confirmed primary only, normalized repairs, duplicates across windows, RTP wraps");
}

static void filtered_delay(void)
{
    X4RembReceiveWindow window;
    x4_rtc_diag_remb_begin(2, 0, 102);
    for (unsigned i = 0; i <= 60; ++i) {
        atomic_store(&ticks, UINT64_C(1000000) + (UINT64_C(1040000) * i) / 60u);
        video((uint16_t)i, i * 1500u, 0);
    }
    assert(x4_rtc_diag_remb_take(2, &window));
    assert(window.video_ssrc == 7 && window.packets == 61 && window.bytes == 61 * 20);
    assert(window.delay_valid && window.delay_us > 30000 && window.delay_us < 40000);
    x4_rtc_diag_remb_end(1); /* A stale close cannot detach the next peer. */
    assert(x4_rtc_diag_remb_take(2, &window));
    x4_rtc_diag_remb_end(2);
    puts("PASS REMB filtered +40ms transit slope and peer isolation");
}

static void *receive(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 30000; ++i)
        video((uint16_t)i, i * 1500u, 0);
    return NULL;
}

static void close_race(void)
{
    pthread_t receiver;
    X4RembReceiveWindow window;
    assert(!pthread_create(&receiver, NULL, receive, NULL));
    for (unsigned epoch = 3; epoch < 100; ++epoch) {
        x4_rtc_diag_remb_begin(epoch, 7, 102);
        if (x4_rtc_diag_remb_take(epoch, &window)) {
            assert(window.video_ssrc == 7 && window.bytes == 20 * window.packets);
        }
        x4_rtc_diag_remb_end(epoch);
        assert(!x4_rtc_diag_remb_take(epoch, &window));
    }
    assert(!pthread_join(receiver, NULL));
    puts("PASS REMB bounded observer versus window drain and peer close");
}

int main(void)
{
    sources_and_wrap();
    filtered_delay();
    close_race();
    return 0;
}
