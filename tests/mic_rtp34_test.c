/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/audio/mic_rtp.h"
#include "../src/media/live_trace.h"
#include "../src/streaming/rtc_receive_diag.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

uint64_t x4_receive_test_clock(void) { return 1000; }
uint64_t x4_receive_test_threadid(void) { return 1; }
void x4_trace_monitor_transport_packet(X4Trace *trace, unsigned stage, bool valid, uint32_t ssrc,
    uint16_t sequence, unsigned pt, size_t bytes, uint64_t time, uint64_t prior, uint32_t flags)
{
    (void)trace; (void)stage; (void)valid; (void)ssrc; (void)sequence;
    (void)pt; (void)bytes; (void)time; (void)prior; (void)flags;
    assert(false); /* Observer does not attach a diagnostic sink. */
}

static unsigned random_calls;
static bool random_zero, random_fail;
static int fake_random(void *out, size_t size)
{
    assert(size == 10);
    ++random_calls;
    if (random_fail) return -1;
    uint8_t *bytes = out;
    for (size_t i = 0; i < size; ++i) bytes[i] = (uint8_t)(i + random_calls);
    if (random_calls == 1 || random_zero) memset(bytes, 0, 4);
    return 0;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void seeds_and_rtp(void)
{
    X4MicRtpSeed a, b;
    assert(x4_mic_rtp_seed(&a, fake_random) && random_calls == 2);
    assert(a.ssrc == 0x02030405 && a.timestamp == 0x06070809 && a.sequence == 0x0a0b);
    assert(x4_mic_rtp_seed(&b, fake_random) && random_calls == 3);
    assert(a.ssrc != b.ssrc && a.timestamp != b.timestamp && a.sequence != b.sequence);
    random_zero = true;
    assert(!x4_mic_rtp_seed(&a, fake_random) && !a.ssrc && random_calls == 7);
    random_fail = true;
    assert(!x4_mic_rtp_seed(&a, fake_random) && !a.ssrc && random_calls == 8);

    for (unsigned duration = 10; duration <= 20; duration += 10) {
        unsigned samples = duration * 48, step = duration * 1000;
        X4MicRtpClock c = {.seed = {0x02030405, UINT32_MAX - 4, UINT16_MAX}, .frame_samples = samples};
        uint8_t wire[X4_MIC_RTP_HEADER];
        assert(x4_mic_rtp_header(&c, 100000, 1, wire));
        assert(wire[0] == 0x80 && wire[1] == 0xef && wire[2] == 0xff && wire[3] == 0xff);
        assert(be32(wire + 4) == UINT32_MAX - 4 && be32(wire + 8) == c.seed.ssrc);
        assert(x4_mic_rtp_header(&c, 100000 + step, 1, wire));
        assert(wire[1] == 111 && !wire[2] && !wire[3] && be32(wire + 4) == samples - 5);
        /* DTX/manual mute leave gaps in clock time, never invent sequence gaps. */
        assert(x4_mic_rtp_header(&c, 100000 + step * 4, 2, wire));
        assert(wire[1] == 0xef && !wire[2] && wire[3] == 1 && be32(wire + 4) == 4 * samples - 5);
        assert(x4_mic_rtp_time_at(&c, 100000 + step * 5) == 5 * samples - 5);
    }

    uint8_t sr[X4_MIC_RTCP_SR_BYTES];
    x4_mic_rtcp_sender_report(sr, 0x01020304, UINT64_C(0x0102030405060708), 12345, 67, 890);
    assert(be32(sr) == 0x80c80006 && be32(sr + 4) == 0x01020304);
    assert(be32(sr + 8) == 0x01020304 && be32(sr + 12) == 0x05060708);
    assert(be32(sr + 16) == 12345 && be32(sr + 20) == 67 && be32(sr + 24) == 890);
    assert(be32(sr + 28) == 0x81ca0003 && be32(sr + 32) == 0x01020304);
    assert(!memcmp(sr + 36, "\x01\x05" "audio\0", 8));
}

static bool offer_ok(const char *sdp) { return x4_mic_sdp_can_send(sdp, strlen(sdp)); }

static void sdp_answers(void)
{
#define AUDIO "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
#define MID0 "a=mid:0\r\n"
#define MAP "a=rtpmap:111 opus/48000/2\r\n"
    assert(offer_ok("v=0\r\n" AUDIO MID0 MAP "a=recvonly\r\n"));
    assert(offer_ok(AUDIO MID0 MAP "a=sendrecv\r\n"));
    assert(offer_ok("a=inactive\r\n" AUDIO MID0 MAP "a=recvonly\r\n"));
    assert(offer_ok("a=recvonly\r\n" AUDIO MID0 "a=rtpmap:111 OpUs/48000/2"));
    assert(offer_ok(AUDIO MID0 MAP)); /* Default direction is sendrecv. */
    assert(!offer_ok(AUDIO MID0 MAP "a=sendonly\r\n"));
    assert(!offer_ok("a=inactive\r\n" AUDIO MID0 MAP));
    assert(!offer_ok(AUDIO MID0 MAP "a=recvonly\r\na=inactive\r\n"));
    assert(!offer_ok("m=audio 0 UDP/TLS/RTP/SAVPF 111\r\n" MID0 MAP));
    assert(!offer_ok("m=audio 9 UDP/TLS/RTP/SAVPF 103\r\n" MID0 MAP));
    assert(!offer_ok("m=audio 65536 UDP/TLS/RTP/SAVPF 111\r\n" MID0 MAP));
    assert(!offer_ok(AUDIO MID0 "a=rtpmap:111 bogus/48000/2\r\n"));
    assert(!offer_ok(AUDIO MID0 "a=rtpmap:111 opus/16000/2\r\n"));
    assert(!offer_ok(AUDIO MID0 MAP "a=rtpmap:111 bogus/48000/2\r\n"));
    assert(!offer_ok(AUDIO "a=mid:1\r\n" MAP));
    assert(!offer_ok(AUDIO MID0 "a=mid:2\r\n" MAP));
    assert(!offer_ok(AUDIO MID0 MAP "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n" MID0));
    assert(!offer_ok(AUDIO MID0 "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n" MAP));
    assert(!x4_mic_sdp_can_send(NULL, 0));
#undef AUDIO
#undef MID0
#undef MAP
}

static const uint8_t rr[] = {
    0x81, 201, 0, 7, 0, 0, 0, 1,
    0x01, 0x02, 0x03, 0x04, 8, 0xff, 0xff, 0xfe, 0, 0, 0, 19,
    0, 0, 0, 24, 0, 0, 0, 0, 0, 0, 0, 0
};
static void authenticated(const void *packet, size_t size)
{
    x4_rtc_receive_packet(X4_RD_RTCP, packet, size, 0, X4_RD_AUTHENTICATED);
}

static void reports_and_bounds(void)
{
    authenticated(rr, sizeof(rr));
    assert(!x4_mic_rr_snapshot(0).reports);
    assert(!x4_mic_rr_bind(64, 1) && !x4_mic_rr_bind(0, 0));
    assert(x4_mic_rr_bind(0, 0x01020304));
    x4_rtc_receive_packet(X4_RD_RTCP, rr, sizeof(rr), 0, X4_RD_UNAUTHENTICATED);
    x4_rtc_receive_packet(X4_RD_SRTP_VALID, rr, sizeof(rr), 0, X4_RD_AUTHENTICATED);
    for (size_t i = 0; i < sizeof(rr); ++i) authenticated(rr, i);
    assert(!x4_mic_rr_snapshot(0).reports);
    authenticated(rr, sizeof(rr));
    X4MicReceiverStats stats = x4_mic_rr_snapshot(0);
    assert(stats.reports == 1 && stats.fraction_lost == 8 && stats.total_lost == -2 &&
        stats.last_sequence == 19 && stats.jitter == 24);

    uint8_t sr[52] = {0x81, 200, 0, 12};
    memcpy(sr + 28, rr + 8, 24);
    authenticated(sr, sizeof(sr));
    assert(x4_mic_rr_snapshot(0).reports == 2);

    uint8_t compound[68] = {0};
    memcpy(compound, rr, sizeof(rr));
    memcpy(compound + sizeof(rr), rr, sizeof(rr));
    compound[32] = 0xa1; compound[35] = 8; compound[67] = 4;
    authenticated(compound, sizeof(compound));
    assert(x4_mic_rr_snapshot(0).reports == 4);
    compound[67] = 0; authenticated(compound, sizeof(compound));
    compound[67] = 5; authenticated(compound, sizeof(compound));
    compound[67] = 36; authenticated(compound, sizeof(compound));
    compound[67] = 4; compound[32] = 0xa2; authenticated(compound, sizeof(compound));
    compound[32] = 0x61; authenticated(compound, sizeof(compound));
    compound[32] = 0xa1; compound[0] = 0xa1; authenticated(compound, sizeof(compound));
    assert(x4_mic_rr_snapshot(0).reports == 4);
    x4_mic_rr_unbind(0);
    authenticated(rr, sizeof(rr));
    assert(x4_mic_rr_snapshot(0).reports == 4 && !x4_mic_rr_bind(0, 0x01020304));
}

static atomic_bool stop;
static atomic_uint observes;
static void *rx_worker(void *arg)
{
    (void)arg;
    while (!atomic_load_explicit(&stop, memory_order_acquire)) {
        authenticated(rr, sizeof(rr));
        atomic_fetch_add_explicit(&observes, 1, memory_order_release);
    }
    return NULL;
}

static void close_race(void)
{
    assert(x4_mic_rr_bind(1, 0x01020304));
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, rx_worker, NULL));
    while (atomic_load_explicit(&observes, memory_order_acquire) < 1000) {}
    for (unsigned slot = 2; slot < 64; ++slot) {
        x4_mic_rr_unbind(slot - 1);
        assert(x4_mic_rr_bind(slot, 0x02030405));
        assert(!x4_mic_rr_snapshot(slot).reports);
    }
    atomic_store_explicit(&stop, true, memory_order_release);
    assert(!pthread_join(reader, NULL));
    assert(x4_mic_rr_snapshot(1).reports >= 1000);
    for (unsigned slot = 2; slot < 64; ++slot) assert(!x4_mic_rr_snapshot(slot).reports);
    x4_mic_rr_unbind(63);
}

int main(void)
{
    seeds_and_rtp();
    sdp_answers();
    reports_and_bounds();
    close_race();
    puts("PASS T34 mic RTP/RTCP: independent random seeds, 10/20 ms, sequence and RTP clock wrap, SDP eligibility, authenticated RR/SR bounds, close race");
}
