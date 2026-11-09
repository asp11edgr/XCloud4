/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_media.h"
#include "receiver_nack_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SOURCE_PACKETS = 1000000, DELAY_LIMIT_US = 110000 };
static const uint8_t payload[12] = {0x80, 102};
static bool forbid_heap;

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);

void *__wrap_malloc(size_t n) { assert(!forbid_heap); return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { assert(!forbid_heap); return __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t n) { assert(!forbid_heap); return __real_realloc(p, n); }
void __wrap_free(void *p) { assert(!forbid_heap); __real_free(p); }

typedef struct {
    X4LiveReorder queue;
    X4IngressTicket tickets[X4_LIVE_VIDEO_REORDER_CAPACITY];
    X4ReceiverNackState nack;
    unsigned retry_sent, retry_allowed, retry_delivered, expired, returned, late;
    bool modern;
} Replay;

static uint32_t rng = 20261106u;
static unsigned draw(unsigned range)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng % range;
}

static unsigned percentile_delay(unsigned rank, bool console)
{
    unsigned low = console ? 17000u : 7000u;
    unsigned middle = console ? 43000u : 14000u;
    unsigned high = console ? 83000u : 25000u;
    unsigned tail = console ? DELAY_LIMIT_US : 31000u;
    if (rank < 500u) return low + (middle - low) * rank / 500u;
    if (rank < 950u) return middle + (high - middle) * (rank - 500u) / 450u;
    return high + (tail - high) * (rank - 950u) / 50u;
}

static bool sender_allows_retry(uint64_t last_send, uint64_t new_request, unsigned sender_rtt_us)
{
    /* Requested emulation contract. Upstream VerifyRtt compares to its RTT
     * and also checks the pending-send state. The 5 ms margin is a model
     * assumption from the task, not a quoted upstream constant. */
    return new_request >= last_send && new_request - last_send >= sender_rtt_us + 5000u;
}

static void initialize(Replay *r, bool modern)
{
    memset(r, 0, sizeof(*r));
    r->modern = modern;
    assert(!x4_live_reorder_init_video(&r->queue, X4_LIVE_VIDEO_REORDER_CAPACITY,
        32, 25000, r->tickets));
    if (modern) x4_receiver_nack_init_46b(&r->nack, true);
    else x4_receiver_nack_init_policy(&r->nack, true, false, true);
}

static void drain(Replay *r, uint64_t now)
{
    for (;;) {
        uint64_t deadline;
        uint32_t missing = x4_live_reorder_video_gap_policy(&r->queue,
            r->nack.grace_us / 1000u, &deadline, NULL);
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

static void observe(X4ReceiverNackState *state, uint16_t seq, uint64_t now, bool measure)
{
    X4ReceiverNackObservation observation;
    forbid_heap = true;
    x4_receiver_nack_observe_return(state, 1, X4_RECEIVER_NACK_VIDEO_PT,
        seq, now, measure, &observation);
    forbid_heap = false;
}

static bool late_arrival(Replay *r, uint16_t seq, uint64_t now)
{
    drain(r, now);
    observe(&r->nack, seq, now, true);
    int rc = x4_live_reorder_insert_video_ticket(&r->queue, payload, sizeof(payload),
        seq, now, NULL, r->nack.grace_us / 1000u);
    assert(rc == X4_LIVE_REORDER_STORED || rc == X4_LIVE_REORDER_LATE);
    drain(r, now);
    return rc == X4_LIVE_REORDER_LATE;
}

static bool dispatch(X4ReceiverNackState *state, uint64_t selected_at, uint64_t sent_at,
    bool expect_retry)
{
    X4ReceiverNackGap gap;
    uint64_t pending[X4_RECEIVER_NACK_MASK_WORDS];
    bool retry;
    forbid_heap = true;
    bool selected = x4_receiver_nack_next(state, selected_at, &gap, pending, &retry);
    if (selected) {
        assert(pending[0] && retry == expect_retry);
        if (!retry) x4_receiver_nack_sent_first(state, gap.request_id, sent_at);
    }
    forbid_heap = false;
    return selected;
}

static unsigned percentile(const unsigned *histogram, unsigned count, unsigned p)
{
    unsigned rank = (count * p + 99u) / 100u, total = 0;
    for (unsigned i = 0; i <= DELAY_LIMIT_US; ++i) {
        total += histogram[i];
        if (total >= rank) return i;
    }
    assert(0);
    return 0;
}

static void loss_replay(bool console)
{
    static Replay replays[2];
    static unsigned histogram[DELAY_LIMIT_US + 1u];
    memset(histogram, 0, sizeof(histogram));
    unsigned gaps = 0, first_dropped = 0;
    for (unsigned i = 0; i < 2u; ++i) {
        initialize(&replays[i], i != 0);
        assert(!late_arrival(&replays[i], 0, 1));
    }
    uint16_t seq = 0;
    for (unsigned i = 0; i < SOURCE_PACKETS; ++i) {
        if (draw(10000u) >= 300u) continue;
        ++gaps;
        seq = (uint16_t)(seq + 2u);
        int rtt_ms = 15 + (int)draw(16u);
        unsigned sender_rtt_us = (unsigned)rtt_ms * (100u + draw(51u)) * 10u;
        unsigned dispatch_wait_us = 500u + draw(4u) * 500u;
        bool first_drop = draw(10000u) < 300u;
        unsigned first_delay = percentile_delay(draw(1000u), console);
        bool retry_drop = draw(10000u) < 300u;
        unsigned retry_delay = percentile_delay(draw(1000u), console);
        if (first_drop) ++first_dropped;
        else ++histogram[first_delay];
        /* Isolated loss positions under a shared seed. RTT is the estimate
         * used by each endpoint. Both directions contribute to the return
         * times, but that measurement cannot identify the pacer share.
         * Model 4 ms on the request leg; put 20% of the console delay above
         * its 17 ms floor plus 1 ms before send. The rest is after send.
         * The short case instead puts one eighth of delay before send. */
        unsigned sender_queue_us = console ? 1000u + (first_delay - 17000u) / 5u : first_delay / 8u;
        uint64_t now = (uint64_t)gaps * 300000u;
        uint64_t first_send = now + dispatch_wait_us;
        uint64_t sender_last_send = first_send + 4000u + sender_queue_us;
        for (unsigned mode = 0; mode < 2u; ++mode) {
            Replay *r = &replays[mode];
            x4_receiver_nack_refresh(&r->nack, rtt_ms, now, false);
            assert(!late_arrival(r, seq, now));
            uint64_t end;
            assert(x4_live_reorder_video_gap_policy(&r->queue,
                r->nack.grace_us / 1000u, &end, NULL) == 1u);
            assert(end == now + r->nack.grace_us);
            assert(dispatch(&r->nack, first_send, first_send, false));
            uint64_t retry_at = first_send + r->nack.retry_delay_us;
            uint64_t returned = first_drop ? UINT64_MAX : first_send + first_delay;
            if (retry_at < returned && retry_at < end) {
                assert(dispatch(&r->nack, retry_at, retry_at, true));
                ++r->retry_sent;
                if (sender_allows_retry(sender_last_send, retry_at + 4000u, sender_rtt_us)) {
                    ++r->retry_allowed;
                    if (!retry_drop) {
                        ++r->retry_delivered;
                        if (retry_at + retry_delay < returned) returned = retry_at + retry_delay;
                    }
                }
            }
            if (returned >= end) {
                ++r->expired;
                assert(!dispatch(&r->nack, end, end, true));
                drain(r, end);
            }
            if (returned != UINT64_MAX) {
                ++r->returned;
                bool late = late_arrival(r, (uint16_t)(seq - 1u), returned);
                r->late += late;
                assert(late == (returned >= end));
            }
            assert(!r->queue.buffered && r->queue.expected == (uint16_t)(seq + 1u));
        }
    }
    unsigned p50 = percentile(histogram, gaps - first_dropped, 50);
    unsigned p95 = percentile(histogram, gaps - first_dropped, 95);
    assert(gaps * 1000u >= SOURCE_PACKETS * 29u && gaps * 1000u <= SOURCE_PACKETS * 31u);
    assert(first_dropped * 1000u >= gaps * 27u && first_dropped * 1000u <= gaps * 33u);
    assert(console ? (p50 >= 42200u && p50 <= 43800u && p95 >= 81800u && p95 <= 84200u) :
        (p50 >= 13500u && p50 <= 14500u && p95 >= 24500u && p95 <= 25500u));
    printf("first_return distribution=%s source=%u missing=%u first_rtx_dropped=%u p50_us=%u p95_us=%u\n",
        console ? "consola" : "T43c", SOURCE_PACKETS, gaps, first_dropped, p50, p95);
    for (unsigned mode = 0; mode < 2u; ++mode) {
        Replay *r = &replays[mode];
        X4ReceiverNackMetrics m;
        x4_receiver_nack_snapshot(&r->nack, &m);
        assert(m.retries == r->retry_sent);
        assert(m.expired_no_first + m.expired_retry_late + m.expired_retry_no_response +
            m.expired_unsent == r->expired);
        printf("replay distribution=%s policy=%s retry_sent=%u retry_allowed=%u retry_delivered=%u "
            "expired=%u/%u late=%u/%u attributable_samples=%u attributable_p95_us=%u\n",
            console ? "consola" : "T43c", mode ? "T46b" : "T43c",
            r->retry_sent, r->retry_allowed, r->retry_delivered, r->expired, gaps,
            r->late, r->returned, m.first_return_samples, m.first_return_p95_us);
        if (mode) {
            assert(r->retry_sent && r->retry_delivered * 100u >= r->retry_sent * 90u);
            if (console) assert(r->late * 100u <= r->returned * 3u);
            else assert(r->expired * 10000u <= gaps * 15u);
        }
        x4_live_reorder_free(&r->queue);
    }
}

static void window_and_dispatch(void)
{
    X4ReceiverNackState state;
    x4_receiver_nack_init_46b(&state, true);
    assert(x4_nack_retry46b_ms(-1) == 30u && x4_nack_retry46b_ms(15) == 40u &&
        x4_nack_retry46b_ms(30) == 70u);
    assert(x4_nack_grace46b_ms(-1, 0, 0) == 80u &&
        x4_nack_grace46b_ms(15, 83000, 31) == 85u &&
        x4_nack_grace46b_ms(15, 83000, 32) == 98u &&
        x4_nack_grace46b_ms(30, 200000, 32) == 150u);
    uint16_t seq = 65500;
    observe(&state, seq, 1, true);
    for (unsigned i = 0; i < 320u; ++i) {
        uint64_t now = 10000u + (uint64_t)i * 300000u;
        uint64_t sent = now + 3000u;
        uint32_t delay = 70000u + i * 100u;
        seq = (uint16_t)(seq + 2u);
        x4_receiver_nack_refresh(&state, 15, now, false);
        observe(&state, seq, now, true);
        assert(dispatch(&state, now + 1000u, sent, false));
        assert(state.next_send_us == sent + 40000u);
        assert(!dispatch(&state, sent + 39999u, sent + 39999u, true));
        /* Skip actual dispatch of this retry to exercise late but
         * unambiguous first-return observations. */
        observe(&state, (uint16_t)(seq - 1u), sent + delay, true);
        if (i == 30u) {
            assert(state.metrics.first_return_samples == 31u);
            assert(state.metrics.first_return_p95_us == 0u && state.grace_us == 85000u);
        } else if (i == 31u) {
            assert(state.metrics.first_return_samples == 32u);
            assert(state.metrics.first_return_p95_us == 73000u);
            x4_receiver_nack_refresh(&state, 15, sent + delay, false);
            assert(state.grace_us == 88000u);
        }
    }
    assert(state.high_extended > UINT16_MAX);
    assert(state.metrics.first_return_samples == 256u && state.first_return_cursor == 64u);
    assert(state.metrics.first_return_p95_us == 100700u);
    unsigned cursor = state.first_return_cursor;
    for (unsigned i = 0; i < 2u; ++i) {
        uint64_t now = 100000000u + (uint64_t)i * 300000u;
        seq = (uint16_t)(seq + 2u);
        x4_receiver_nack_refresh(&state, 15, now, true);
        observe(&state, seq, now, true);
        assert(state.metrics.grace_us == 250000u);
        assert(dispatch(&state, now + 1000u, now + 1000u, false));
        if (i) assert(dispatch(&state, now + 41000u, now + 41000u, true));
        /* FEC can cancel a position but cannot enter the window. A normal
         * or RTX packet after an actual retry is ambiguous and excluded. */
        observe(&state, (uint16_t)(seq - 1u), now + 60000u, i != 0);
    }
    assert(state.first_return_cursor == cursor && state.metrics.first_return_p95_us == 100700u);
    puts("PASS ventana T46b 256/32, wrap RTP y ventana, reloj de envio, sin asignaciones, arranque 250 ms");
}

int main(void)
{
    assert(!sender_allows_retry(100000, 124999, 20000));
    assert(sender_allows_retry(100000, 125000, 20000));
    window_and_dispatch();
    loss_replay(false);
    loss_replay(true);
    return 0;
}
