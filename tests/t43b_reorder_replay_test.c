/* SPDX-License-Identifier: GPL-3.0-only */
/* Same synthetic input for both policies. Console aggregates anchor the burst
 * sizes and the return quantiles. Intra-bin arrival times and simultaneous,
 * ordered returns are modeled because the original timestamps are unavailable
 * here. The controlled replay feeds returns for every size, even where legacy
 * NACK would suppress the request. Suppression is tested in the NACK suite. */
#include "live_media.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { START_US = 1000, FIRST_SEQUENCE = 65521, PEAK_PACKETS = 620 };
static const unsigned return_us[] = {
    7000, 8000, 9000, 10000, 11000, 12000, 13000, 14000, 15000, 16000,
    16000, 18000, 20000, 23000, 26000, 30000, 34000, 38000, 43000, 43500
};
typedef struct {
    X4LiveReorder q;
    X4IngressTicket tickets[X4_LIVE_VIDEO_REORDER_CAPACITY];
    bool modern;
    unsigned burst, peak, capacity, received_returns, late_returns;
} Fixture;

static int insert(Fixture *f, uint16_t seq, uint64_t now, unsigned grace_ms)
{
    uint8_t bytes[X4_LIVE_RTP_MAX] = {0};
    bytes[0] = (uint8_t)(seq >> 8);
    bytes[1] = (uint8_t)seq;
    X4IngressTicket ticket = {.extended_sequence = seq, .identity = (uint64_t)seq + 1u};
    int result = f->modern ?
        x4_live_reorder_insert_video_ticket(&f->q, bytes, sizeof(bytes), seq, now, &ticket, grace_ms) :
        x4_live_reorder_insert_ticket(&f->q, bytes, sizeof(bytes), seq, now, &ticket);
    if (f->q.buffered > f->peak) f->peak = f->q.buffered;
    assert(f->q.buffered <= f->q.window);
    assert(f->q.resident_bytes == f->q.buffered * X4_LIVE_RTP_MAX);
    return result;
}

static bool initial_gap(const Fixture *f)
{
    return (uint16_t)(f->q.expected - FIRST_SEQUENCE) < f->burst &&
        !x4_live_reorder_has(&f->q, f->q.expected);
}

static void drain(Fixture *f, uint64_t now)
{
    for (;;) {
        size_t size;
        uint32_t lost;
        X4IngressTicket ticket;
        const uint8_t *p;
        if (f->modern) {
            p = x4_live_reorder_next_video_ticket_limited(&f->q, now, &size, &lost,
                NULL, false, f->q.max_forward, &ticket);
        } else {
            /* Legacy grace supersedes ordinary depth and expiry until real
             * capacity failure, completion or the absolute 45 ms deadline. */
            bool grace = initial_gap(f);
            if (grace && now < START_US + 45000u) return;
            p = x4_live_reorder_next_ticket_limited(&f->q, now, &size, &lost,
                NULL, grace, f->q.max_forward, &ticket);
        }
        if (!p) return;
        assert(size == X4_LIVE_RTP_MAX);
        uint16_t seq = (uint16_t)(((unsigned)p[0] << 8) | p[1]);
        assert((uint16_t)(f->q.expected - 1u) == seq);
        assert(ticket.extended_sequence == seq && ticket.identity == (uint64_t)seq + 1u);
    }
}

static void fixture_init(Fixture *f, bool modern, unsigned burst)
{
    memset(f, 0, sizeof(*f));
    f->modern = modern;
    f->burst = burst;
    unsigned window = modern ? X4_LIVE_VIDEO_REORDER_CAPACITY : X4_LIVE_VIDEO_REORDER_LEGACY_CAPACITY;
    assert(!x4_live_reorder_init_video(&f->q, (uint16_t)window, 32, 25000, f->tickets));
    assert(insert(f, FIRST_SEQUENCE - 1u, START_US - 1u, 67) == X4_LIVE_REORDER_STORED);
    drain(f, START_US - 1u);
    assert(f->q.started && f->q.expected == FIRST_SEQUENCE && !f->q.buffered);
}

static void receive(Fixture *f, unsigned offset, uint64_t now, bool repair)
{
    drain(f, now);
    int result = insert(f, (uint16_t)(FIRST_SEQUENCE + offset), now, 67);
    if (repair) {
        ++f->received_returns;
        if (result == X4_LIVE_REORDER_LATE) ++f->late_returns;
        else assert(result == X4_LIVE_REORDER_STORED || result == X4_LIVE_REORDER_DIRECT);
    }
    if (result == X4_LIVE_REORDER_FULL) {
        ++f->capacity;
        assert(x4_live_reorder_abandon_gap(&f->q) != 0);
    }
    drain(f, now);
}

static unsigned elapsed_for(unsigned index)
{
    /* 160 at the start of an IDR-style burst, 620 across the 100 ms bin. */
    if (index < 160u) return index * 12000u / 160u;
    return 12000u + (index - 160u) * 88000u / (PEAK_PACKETS - 160u);
}

static void replay(bool modern, unsigned burst, unsigned delay_us, Fixture *f)
{
    fixture_init(f, modern, burst);
    bool returned = false;
    for (unsigned i = 0; i < PEAK_PACKETS; ++i) {
        unsigned elapsed = elapsed_for(i);
        if (!returned && elapsed >= delay_us) {
            for (unsigned j = 0; j < burst; ++j) receive(f, j, START_US + delay_us, true);
            returned = true;
        }
        receive(f, burst + i, START_US + elapsed, false);
    }
    assert(returned && f->received_returns == burst);
    drain(f, START_US + 200000u);
    assert(!f->q.buffered);
    x4_live_reorder_free(&f->q);
}

static void test_direct(void)
{
    Fixture f;
    fixture_init(&f, true, 1);
    for (unsigned i = 1; i <= X4_LIVE_VIDEO_REORDER_CAPACITY; ++i) {
        assert(insert(&f, (uint16_t)(FIRST_SEQUENCE + i), START_US, 67) == X4_LIVE_REORDER_STORED);
    }
    assert(f.q.resident_bytes == X4_LIVE_VIDEO_REORDER_BYTE_LIMIT);
    drain(&f, START_US + 66999u);
    assert(f.q.expected == FIRST_SEQUENCE && f.q.buffered == X4_LIVE_VIDEO_REORDER_CAPACITY);
    assert(insert(&f, (uint16_t)(FIRST_SEQUENCE + X4_LIVE_VIDEO_REORDER_CAPACITY + 1),
        START_US + 66999u, 67) == X4_LIVE_REORDER_FULL);
    assert(f.q.expected == FIRST_SEQUENCE && f.q.buffered == X4_LIVE_VIDEO_REORDER_CAPACITY);
    assert(insert(&f, FIRST_SEQUENCE, START_US + 66999u, 67) == X4_LIVE_REORDER_DIRECT);
    assert(f.q.expected == FIRST_SEQUENCE + 1 && f.q.buffered == X4_LIVE_VIDEO_REORDER_CAPACITY);
    drain(&f, START_US + 66999u);
    assert(!f.q.buffered);
    x4_live_reorder_free(&f.q);
}

static void test_deadline(void)
{
    Fixture f;
    uint64_t deadline = 0;
    fixture_init(&f, true, 1);
    assert(insert(&f, FIRST_SEQUENCE + 1u, START_US, 45) == X4_LIVE_REORDER_STORED);
    assert(insert(&f, FIRST_SEQUENCE + 2u, START_US + 10000u, 100) == X4_LIVE_REORDER_STORED);
    assert(x4_live_reorder_video_gap(&f.q, &deadline) == 1 && deadline == START_US + 45000u);
    drain(&f, deadline - 1);
    assert(f.q.expected == FIRST_SEQUENCE);
    drain(&f, deadline);
    assert(f.q.expected == FIRST_SEQUENCE + 3u && !f.q.buffered);
    assert(insert(&f, (uint16_t)(f.q.expected + X4_LIVE_VIDEO_REORDER_CAPACITY + 1u),
        deadline + 1, 100) == X4_LIVE_REORDER_STORED);
    drain(&f, deadline + 1);
    assert(!f.q.buffered);
    x4_live_reorder_free(&f.q);
}

int main(void)
{
    const unsigned bursts[] = {15, 29, 207, 256};
    printf("burst,legacy_late,modern_late,returns,legacy_peak,modern_peak\n");
    for (unsigned i = 0; i < sizeof(bursts) / sizeof(*bursts); ++i) {
        unsigned late[2] = {0}, peak[2] = {0}, received[2] = {0};
        for (unsigned mode = 0; mode < 2; ++mode) {
            for (unsigned j = 0; j < sizeof(return_us) / sizeof(*return_us); ++j) {
                Fixture f;
                replay(mode != 0, bursts[i], return_us[j], &f);
                late[mode] += f.late_returns;
                received[mode] += f.received_returns;
                if (f.peak > peak[mode]) peak[mode] = f.peak;
            }
        }
        assert(received[0] == received[1] && received[0] != 0);
        printf("%u,%u,%u,%u,%u,%u\n", bursts[i], late[0], late[1], received[0], peak[0], peak[1]);
        assert(late[0] * 100u >= received[0] * 80u);
        assert(late[1] * 100u <= received[1] * 5u);
    }
    test_direct();
    test_deadline();
    puts("PASS T43b replay, capacity, DIRECT, absolute grace and range boundary");
    return 0;
}
