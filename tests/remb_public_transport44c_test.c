/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual public controller and RTP observer. Only the clock, repair counters,
 * random source, and protected-send bridge have deterministic host doubles. */
#define _POSIX_C_SOURCE 200809L
#define X4_RECOVERY43_TRANSPORT_TEST 1
#include "../src/streaming/rtc_transport.c"
#include <assert.h>
#include <stdarg.h>

enum { VIDEO_TRACK = 42, VIDEO_SSRC = 7, SENDER_SSRC = 9, EPOCH = 1 };
static uint64_t now_us, expired, attempt_us[8];
static uint32_t bitrate[8];
static unsigned attempts, sequence, logs;
static char last_log[256];

uint64_t sceKernelGetProcessTime(void) { return now_us; }
uint64_t x4_receive_test_clock(void) { return now_us; }
uint64_t x4_receive_test_threadid(void) { return 1; }
void x4_receive_test_sleep(unsigned us) { (void)us; assert(false); }

void x4_trace_monitor_transport_packet(X4Trace *trace, unsigned stage, bool valid,
    uint32_t ssrc, uint16_t seq, unsigned pt, size_t bytes, uint64_t time,
    uint64_t prior, uint32_t flags)
{
    (void)trace; (void)stage; (void)valid; (void)ssrc; (void)seq; (void)pt;
    (void)bytes; (void)time; (void)prior; (void)flags;
    assert(false); /* There is no sink attached in this fixture. */
}

int x4_native_random(void *out, size_t size)
{
    const uint32_t random = SENDER_SSRC;
    assert(size == sizeof(random));
    memcpy(out, &random, size);
    return 0;
}

bool x4_recovery43_snapshot(uint32_t epoch, X4Recovery43Snapshot *out)
{
    assert(epoch == EPOCH);
    *out = (X4Recovery43Snapshot){
        .epoch = epoch, .configured = true,
        .accepted = {.media_ssrc = VIDEO_SSRC, .media_pt = 102},
        .repair = {.gaps_expired = expired}
    };
    return true;
}

int x4_log_printf(const char *format, ...)
{
    ++logs;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(last_log, sizeof(last_log), format, args);
    va_end(args);
    assert(written > 0 && (size_t)written < sizeof(last_log));
    return written;
}

int x4_rtc_send_video_remb(int video_track, uint32_t sender_ssrc,
    uint32_t video_ssrc, uint32_t bps)
{
    assert(video_track == VIDEO_TRACK && sender_ssrc == SENDER_SSRC && video_ssrc == VIDEO_SSRC);
    assert(attempts < sizeof(bitrate) / sizeof(bitrate[0]));
    bitrate[attempts] = bps;
    attempt_us[attempts++] = now_us;
    return 1;
}

static void feed_video(unsigned count, uint64_t first_us)
{
    uint8_t packet[1012] = {0x80, 102, [11] = VIDEO_SSRC};
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t timestamp = sequence * 45; /* 0.5 ms at 90 kHz. */
        packet[2] = (uint8_t)(sequence >> 8);
        packet[3] = (uint8_t)sequence;
        packet[4] = (uint8_t)(timestamp >> 24);
        packet[5] = (uint8_t)(timestamp >> 16);
        packet[6] = (uint8_t)(timestamp >> 8);
        packet[7] = (uint8_t)timestamp;
        now_us = first_us + 500u * i;
        (void)x4_rtc_receive_packet(X4_RD_PEER_OUT, packet, sizeof(packet),
            now_us, X4_RD_AUTHENTICATED);
        ++sequence;
    }
}

static void tick(X4Rtc *rtc, uint64_t time, unsigned expected_attempts)
{
    /* The fresh process-clock reading in the real sender follows its tick.
     * A loss sample may thus arrive a few us before the regular send is due. */
    now_us = time + 5;
    remb44a_tick(rtc, time);
    assert(attempts == expected_attempts);
}

static void check_public_report(X4Rtc *rtc)
{
    const char *expected = "XCloud4: remb modo=adaptable enviados=5 actual_mbps=14.688000 "
        "min_mbps=13.600000 max_mbps=20.000000 bajadas=1 subidas=1 R_mbps=16.000000 "
        "L_max_ppm=30067 D_max_ms=0.000\n";
    remb44c_periodic(rtc, 30000099);
    assert(!logs);
    remb44c_periodic(rtc, 30000100);
    assert(logs == 1 && !strcmp(last_log, expected));
    remb44c_periodic(rtc, 60000099);
    assert(logs == 1);
    remb44c_periodic(rtc, 60000100);
    assert(logs == 2 && !strcmp(last_log, expected));
    remb44c_finish_peer(rtc);
    open_diagnostics(true);
    X4RembSummary summary;
    x4_rtc_remb_finish_summary(NULL, 50, &summary);
    assert(logs == 3 && !strcmp(last_log, expected));
    assert(summary.sent == 5 && summary.sent_bps == 14688000 &&
        summary.min_sent_bps == 13600000 && summary.max_sent_bps == 20000000);
    assert(summary.decreases == 1 && summary.increases == 1 &&
        summary.received_bps == 16000000 && summary.max_loss_ppm == 30067 &&
        summary.have_max_delay && summary.max_delay_us == 0);
    x4_rtc_remb_finish_summary(NULL, 50, &summary);
    assert(logs == 3 && summary.sent == 5); /* An export retry does not log twice. */
    x4_rtc_remb_finish_summary(NULL, 101, &summary);
    assert(logs == 4 && !summary.sent); /* A later pipeline cannot see the old peer. */
    puts("PASS public REMB klog: 30-second interval, final privacy schema, no duplicate or stale summary");
}

int main(void)
{
    assert(x4_stream_settings_remb().mode == X4_REMB_ADAPTIVE);
    assert(remb44a_cap(X4_REQUEST_720P60) == 10000000);
    assert(remb44a_cap(X4_REQUEST_1080P_H) == 12000000);
    assert(remb44a_cap(X4_REQUEST_1080P_G) == 20000000);
    assert(remb44a_cap(X4_REQUEST_1080P_HQ) == 30000000);

    now_us = 100;
    open_diagnostics(false);
    X4Rtc rtc = {.video = VIDEO_TRACK, .nack_epoch = EPOCH,
        .request_config = {.profile = X4_REQUEST_1080P_G}};
    rtc.remb.negotiated = true;
    atomic_init(&rtc.state, X4_RTC_CONNECTED);
    atomic_init(&rtc.video_packets, 0);
    remb44a_begin(&rtc);
    tick(&rtc, 500, 0);
    feed_video(1, 900);
    atomic_store(&rtc.video_packets, 1);
    tick(&rtc, 1000, 1);
    assert(bitrate[0] == 20000000);

    tick(&rtc, 1000999, 1);
    feed_video(2000, 1001);
    expired = 62;
    tick(&rtc, 1001000, 2);
    assert(bitrate[1] == 13600000 && attempt_us[1] - attempt_us[0] == 1000000);
    assert(rtc.remb.decreases == 1 && rtc.remb.loss_ppm > 30000);

    feed_video(2000, 1001001);
    tick(&rtc, 2001000, 2);
    tick(&rtc, 2001004, 2);
    tick(&rtc, 2001005, 3);
    assert(bitrate[2] == 13600000);

    feed_video(2000, 2001001);
    tick(&rtc, 3001000, 3);
    tick(&rtc, 3001010, 4);
    assert(bitrate[3] == 13600000);

    feed_video(2000, 3001001);
    tick(&rtc, 4001000, 4);
    tick(&rtc, 4001015, 5);
    assert(bitrate[4] == 14688000 && rtc.remb.increases == 1);
    assert(attempt_us[2] - attempt_us[1] >= 1000000);
    assert(attempt_us[3] - attempt_us[2] >= 1000000);
    assert(attempt_us[4] - attempt_us[3] >= 1000000);
    x4_rtc_diag_remb_end(EPOCH);
    puts("PASS public REMB control: four caps, authenticated video, >5% drop before the next send slot, one-second cadence and +8% after three stable samples");
    check_public_report(&rtc);
    return 0;
}
