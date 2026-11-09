/* SPDX-License-Identifier: GPL-3.0-only */
/* Synthetic seeded replay; no original packet trace was available. The
 * one-hole trials isolate expiry from queue pressure. RTT order, dispatch
 * delay and intra-quantile times are modeled; drops also affect both RTX. */
#include "live_media.h"
#include "receiver_nack_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { SOURCE_PACKETS = 484300, DELAY_BUCKETS = 27001, STARTUP_LOSSES = 306 };
static const uint8_t payload[12] = {0x80, 102};
typedef struct {
    X4LiveReorder queue;
    X4IngressTicket tickets[X4_LIVE_VIDEO_REORDER_CAPACITY];
    X4ReceiverNackState nack;
    bool modern;
    unsigned grace;
} Replay;

static uint32_t rng = 20261105;
static unsigned draw(unsigned modulus)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng % modulus;
}

static unsigned rtx_delay(unsigned p)
{
    if (p < 500) return 7000 + 7000 * p / 500;
    if (p < 950) return 14000 + 7000 * (p - 500) / 450;
    return 21000 + 5000 * (p - 950) / 50;
}

static void initialize(Replay *r, bool modern)
{
    memset(r, 0, sizeof(*r));
    r->modern = modern;
    assert(!x4_live_reorder_init_video(&r->queue, X4_LIVE_VIDEO_REORDER_CAPACITY,
        32, 25000, r->tickets));
    x4_receiver_nack_init_policy(&r->nack, true, false, modern);
}

static void drain(Replay *r, uint64_t now)
{
    for (;;) {
        uint64_t deadline;
        uint32_t missing = x4_live_reorder_video_gap_policy(&r->queue,
            r->modern ? r->grace : 0, &deadline, NULL);
        if (missing && now >= deadline)
            assert(x4_live_reorder_abandon_gap(&r->queue) == missing);
        size_t bytes;
        uint32_t lost;
        X4IngressTicket ticket;
        if (!x4_live_reorder_next_video_ticket_limited(&r->queue, 0,
                &bytes, &lost, NULL, false, r->queue.max_forward, &ticket)) return;
        assert(bytes == sizeof(payload) && !lost);
    }
}

static bool late_arrival(Replay *r, uint16_t seq, uint64_t now, unsigned insert_grace)
{
    drain(r, now);
    X4ReceiverNackObservation observation;
    x4_receiver_nack_observe(&r->nack, 1, 102, seq, now, &observation);
    int rc = x4_live_reorder_insert_video_ticket(&r->queue, payload, sizeof(payload),
        seq, now, NULL, insert_grace);
    assert(rc == X4_LIVE_REORDER_STORED || rc == X4_LIVE_REORDER_LATE);
    drain(r, now);
    return rc == X4_LIVE_REORDER_LATE;
}

static bool dispatch(Replay *r, uint64_t when, bool retry_expected)
{
    X4ReceiverNackGap gap;
    uint64_t pending[X4_RECEIVER_NACK_MASK_WORDS];
    bool retry;
    if (!x4_receiver_nack_next(&r->nack, when, &gap, pending, &retry)) return false;
    assert(pending[0] && retry == retry_expected);
    return true;
}

static unsigned percentile(const unsigned *hist, unsigned n, unsigned p)
{
    unsigned rank = (n * p + 99) / 100, count = 0;
    for (unsigned i = 0; i < DELAY_BUCKETS; ++i) {
        count += hist[i];
        if (count >= rank) return i;
    }
    assert(0);
    return 0;
}

static void timing(Replay *r, int rtt, uint64_t now, bool startup)
{
    r->grace = x4_nack_grace_ms(rtt, r->modern);
    if (r->modern) x4_receiver_nack_refresh(&r->nack, rtt, now, startup);
    else x4_receiver_nack_set_rtt(&r->nack, rtt);
}

static void loss_replay(void)
{
    static Replay replays[2];
    static unsigned hist[DELAY_BUCKETS];
    unsigned gaps = 0, first_dropped = 0, first_received = 0, retry_dropped[2] = {0};
    unsigned expired[2] = {0}, late[2] = {0};
    for (unsigned mode = 0; mode < 2; ++mode) {
        initialize(&replays[mode], mode != 0);
        assert(!late_arrival(&replays[mode], 0, 1, 80));
    }
    uint16_t seq = 0;
    for (unsigned i = 0; i < SOURCE_PACKETS; ++i) {
        if (draw(10000) >= 300) continue;
        ++gaps;
        seq += 2;
        int rtt = 16 + (int)draw(15);
        unsigned offset = 7000 + draw(5) * 1000; /* modeled observation to first dispatch */
        bool first_drop = draw(10000) < 300;
        unsigned first_delay = rtx_delay(draw(1000));
        bool retry_drop = draw(10000) < 300;
        unsigned retry_delay = rtx_delay(draw(1000));
        if (first_drop) ++first_dropped;
        else { ++hist[first_delay]; ++first_received; }
        uint64_t now = (uint64_t)gaps * 300000;
        for (unsigned mode = 0; mode < 2; ++mode) {
            Replay *r = &replays[mode];
            timing(r, rtt, now, false);
            assert(!late_arrival(r, seq, now, r->grace));
            uint64_t end = now + 1000u * r->grace, first_send = now + offset;
            uint64_t retry_at = first_send + x4_nack_retry_delay_us(rtt);
            assert(dispatch(r, first_send, false));
            uint64_t returned = first_drop ? UINT64_MAX : first_send + first_delay;
            if (retry_at < returned && retry_at < end && dispatch(r, retry_at, true)) {
                if (retry_drop) ++retry_dropped[mode];
                else if (retry_at + retry_delay < returned) returned = retry_at + retry_delay;
            }
            if (returned >= end) {
                ++expired[mode];
                assert(!dispatch(r, end, true));
                drain(r, end);
            }
            if (returned != UINT64_MAX) {
                bool is_late = late_arrival(r, seq - 1, returned, r->grace);
                late[mode] += is_late;
                assert(is_late == (returned >= end));
            }
            assert(!r->queue.buffered && r->queue.expected == (uint16_t)(seq + 1));
        }
    }
    unsigned p50 = percentile(hist, first_received, 50), p95 = percentile(hist, first_received, 95);
    assert(gaps * 1000 >= SOURCE_PACKETS * 29 && gaps * 1000 <= SOURCE_PACKETS * 31);
    assert(first_dropped * 1000 >= gaps * 27 && first_dropped * 1000 <= gaps * 33);
    assert(p50 >= 13500 && p50 <= 14500 && p95 >= 20500 && p95 <= 21500);
    assert(expired[0] * 1000 >= gaps * 7 && expired[0] * 1000 <= gaps * 9);
    assert(expired[1] * 1000 <= gaps);
    printf("replay source=%u lost=%u first_rtx_dropped=%u rtx_p50_us=%u rtx_p95_us=%u\n",
        SOURCE_PACKETS, gaps, first_dropped, p50, p95);
    for (unsigned mode = 0; mode < 2; ++mode) {
        X4ReceiverNackMetrics m;
        x4_receiver_nack_snapshot(&replays[mode].nack, &m);
        assert(m.expired_no_first + m.expired_retry_late +
            m.expired_retry_no_response + m.expired_unsent == expired[mode]);
        assert(m.expired_retry_late == late[mode]);
        printf("replay policy=T43%s expired=%u/%u fraction_pct=%.4f late=%u no_response=%llu retry_dropped=%u retry_p50_us=%llu retry_p95_us=%llu\n",
            mode ? "c" : "b", expired[mode], gaps, 100.0 * expired[mode] / gaps, late[mode],
            (unsigned long long)m.expired_retry_no_response, retry_dropped[mode],
            (unsigned long long)m.retry_return_p50_us, (unsigned long long)m.retry_return_p95_us);
        x4_live_reorder_free(&replays[mode].queue);
    }
}

static void startup_replay(bool modern)
{
    Replay r;
    initialize(&r, modern);
    const uint64_t first = 100000, begin = first + 1000;
    timing(&r, 16, begin, true);
    unsigned grace = modern ? X4_NACK_STARTUP_GRACE_MS : r.grace;
    assert(!late_arrival(&r, 0, first, grace));
    assert(!late_arrival(&r, STARTUP_LOSSES + 1, begin, grace));
    assert(dispatch(&r, begin + 1000, false));
    unsigned late = 0;
    for (unsigned i = 1; i <= STARTUP_LOSSES; ++i)
        late += late_arrival(&r, (uint16_t)i, begin + 184000 + (i - 1) * 9000 / (STARTUP_LOSSES - 1), grace);
    assert(late == (modern ? 0 : STARTUP_LOSSES));
    assert(!r.queue.buffered);
    uint64_t after = first + X4_NACK_STARTUP_WINDOW_US;
    timing(&r, 16, after, false);
    assert(!late_arrival(&r, STARTUP_LOSSES + 3, after, r.grace));
    uint64_t deadline;
    bool startup;
    assert(x4_live_reorder_video_gap_policy(&r.queue, modern ? r.grace : 0,
        &deadline, &startup) == 1 && deadline == after + 1000u * r.grace && !startup);
    assert(r.nack.metrics.grace_us == 1000u * r.grace);
    printf("startup policy=T43%s missing=%u delay_us=184000-193000 late=%u next_grace_ms=%u\n",
        modern ? "c" : "b", STARTUP_LOSSES, late, r.grace);
    x4_live_reorder_free(&r.queue);
}

static void update_held_gap(void)
{
    Replay r;
    initialize(&r, true);
    assert(x4_nack_grace_ms(-1, true) == 80 && x4_nack_grace_ms(16, true) == 70);
    assert(x4_nack_grace_ms(26, true) == 95 && x4_nack_grace_ms(30, true) == 105);
    assert(x4_nack_grace_ms(1, true) == 60 && x4_nack_grace_ms(100, true) == 120);
    timing(&r, 16, 1, false);
    assert(!late_arrival(&r, 0, 1, 70) && !late_arrival(&r, 2, 1000, 70));
    assert(dispatch(&r, 1001, false));
    timing(&r, 26, 6000, false);
    uint64_t deadline;
    assert(x4_live_reorder_video_gap_policy(&r.queue, 95, &deadline, NULL) == 1);
    assert(deadline == 96000 && r.nack.next_send_us == 40001);
    assert(!late_arrival(&r, 1, 88000, 95));
    assert(!r.nack.metrics.expired_no_first && !r.queue.buffered);
    x4_live_reorder_free(&r.queue);
}

int main(void)
{
    loss_replay();
    startup_replay(false);
    startup_replay(true);
    update_held_gap();
    puts("PASS T43c loss replay, startup burst, post-startup grace and live RTT updates");
    return 0;
}
