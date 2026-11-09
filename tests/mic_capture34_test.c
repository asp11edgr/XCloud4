/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/audio/mic_capture.h"

#include <assert.h>
#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

enum { PRESENT, UNPLUGGED, HARD_MUTED };
static atomic_int device;
static atomic_bool low_only, silent, hold, held, fail_read, fail_close;
static atomic_uint opens, closes, ports, attempts_high, attempts_low, frames_read;
static atomic_int observed_user;
static atomic_uint_fast64_t cpu_total_ns, cpu_last_ns;

static uint64_t ns(clockid_t clock)
{
    struct timespec t;
    assert(!clock_gettime(clock, &t));
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

static uint64_t clock_us(void) { return ns(CLOCK_MONOTONIC) / 1000; }

static void sleep_us(unsigned us)
{
    struct timespec delay = {.tv_sec = us / 1000000, .tv_nsec = (long)(us % 1000000) * 1000};
    (void)nanosleep(&delay, NULL);
}

int x4_mic_test_audio_open(int32_t user, unsigned rate, unsigned port, unsigned samples)
{
    assert(user == 41 || user == 42);
    assert((rate == 48000 || rate == 16000) && port <= 1 && samples == 256);
    atomic_store(&observed_user, user);
    if (rate == 48000) {
        atomic_fetch_add(&attempts_high, 1);
        if (atomic_load(&low_only)) return -1;
    } else {
        atomic_fetch_add(&attempts_low, 1);
    }
    atomic_store(&cpu_last_ns, 0);
    assert(atomic_fetch_add(&ports, 1) == 0);
    atomic_fetch_add(&opens, 1);
    return (int)rate;
}

int x4_mic_test_audio_read(int handle, int16_t *pcm)
{
    assert(atomic_load(&ports) == 1);
    uint64_t cpu_now = ns(CLOCK_THREAD_CPUTIME_ID);
    uint64_t last = atomic_exchange(&cpu_last_ns, cpu_now);
    if (last) atomic_fetch_add(&cpu_total_ns, cpu_now - last);
    if (atomic_load(&hold)) {
        atomic_store(&held, true);
        while (atomic_load(&hold)) sleep_us(200);
        atomic_store(&held, false);
    }
    sleep_us(256000000u / (unsigned)handle);
    if (atomic_exchange(&fail_read, false)) return -1;
    unsigned block = atomic_fetch_add(&frames_read, 1);
    for (unsigned i = 0; i < 256; ++i) {
        unsigned position = block * 256 + i, phase = position % 97;
        int triangle = phase < 48 ? (int)phase : 96 - (int)phase;
        pcm[i] = atomic_load(&silent) ? 0 : (int16_t)((triangle - 24) * 500);
    }
    /* Accept status-style zero and explicit sample counts. */
    return block & 1 ? 0 : 256;
}

int x4_mic_test_audio_state(int handle)
{
    assert(handle == 48000 || handle == 16000);
    switch (atomic_load(&device)) {
    case UNPLUGGED: return 1;
    case HARD_MUTED: return 4;
    default: return 0;
    }
}

int x4_mic_test_audio_close(int handle)
{
    assert(handle == 48000 || handle == 16000);
    if (atomic_exchange(&fail_close, false)) return -1;
    assert(atomic_fetch_sub(&ports, 1) == 1);
    atomic_fetch_add(&closes, 1);
    return 0;
}

static void expect_status(X4MicStatus status)
{
    uint64_t deadline = clock_us() + 4000000;
    while (x4_mic_snapshot().status != status && clock_us() < deadline) sleep_us(250);
    assert(x4_mic_snapshot().status == status);
}

static void expect_idle(void)
{
    uint64_t deadline = clock_us() + 4000000;
    while (!x4_mic_capture_test_idle() && clock_us() < deadline) sleep_us(250);
    assert(x4_mic_capture_test_idle() && atomic_load(&ports) == 0);
}

static void expect_held(void)
{
    uint64_t deadline = clock_us() + 4000000;
    while (!atomic_load(&held) && clock_us() < deadline) sleep_us(250);
    assert(atomic_load(&held));
}

static X4MicFrame next_frame(uint64_t generation, uint64_t peer, unsigned samples)
{
    X4MicFrame frame = {0};
    uint64_t deadline = clock_us() + 4000000;
    while (clock_us() < deadline) {
        if (x4_mic_capture_take(generation, peer, clock_us(), &frame)) break;
        sleep_us(250);
    }
    assert(frame.size > 2 && frame.size <= X4_MIC_PAYLOAD_CAP && frame.samples == samples);
    assert(opus_packet_get_nb_samples(frame.data, frame.size, 48000) == (int)samples);
    assert(x4_mic_capture_send_allowed(generation, peer, frame.epoch));
    return frame;
}

static void disabled_and_invalid(void)
{
    assert(x4_mic_session_begin(false, 41, 20) == 0);
    assert(!x4_mic_request().generation && !x4_mic_snapshot().enabled);
    assert(!x4_mic_capture_attach(0, 1));
    for (int user = -1; user <= 0; ++user) {
        assert(x4_mic_session_begin(true, user, 10));
        assert(!x4_mic_request().generation && !x4_mic_capture_attach(1, 1));
        assert(x4_mic_snapshot().status == X4_MIC_UNAVAILABLE);
    }
    assert(x4_mic_session_begin(true, 0xff, 10) && !x4_mic_request().generation);
    assert(!atomic_load(&opens));
    x4_mic_session_end();
}

static void waiting_and_device_recovery(void)
{
    uint64_t generation = x4_mic_session_begin(true, 41, 10), peer = 1;
    assert(x4_mic_request().generation == generation && x4_mic_request().frame_ms == 10);
    x4_mic_capture_reject(generation);
    assert(x4_mic_snapshot().status == X4_MIC_REJECTED && !atomic_load(&opens));
    atomic_store(&device, UNPLUGGED);
    assert(x4_mic_capture_attach(generation, peer));
    expect_status(X4_MIC_UNAVAILABLE);
    atomic_store(&device, PRESENT);
    X4MicFrame first = next_frame(generation, peer, 480);
    assert(x4_mic_snapshot().rate == 48000 && x4_mic_snapshot().active);
    atomic_store(&fail_read, true);
    expect_status(X4_MIC_UNAVAILABLE);
    assert(!x4_mic_capture_send_allowed(generation, peer, first.epoch));
    X4MicFrame recovered = next_frame(generation, peer, 480);
    assert(recovered.epoch != first.epoch);
    atomic_store(&device, HARD_MUTED);
    expect_status(X4_MIC_MUTED);
    assert(!x4_mic_capture_send_allowed(generation, peer, recovered.epoch));
    atomic_store(&device, PRESENT);
    recovered = next_frame(generation, peer, 480);
    atomic_store(&fail_close, true);
    x4_mic_toggle_mute();
    assert(x4_mic_snapshot().muted && !x4_mic_capture_send_allowed(generation, peer, recovered.epoch));
    assert(!x4_mic_capture_take(generation, peer, clock_us(), &first));
    x4_mic_toggle_mute();
    recovered = next_frame(generation, peer, 480);
    assert(!x4_mic_snapshot().muted && recovered.epoch != first.epoch);
    x4_mic_capture_detach(generation, peer);
    assert(!x4_mic_capture_attach(generation, peer));
    assert(!x4_mic_capture_send_allowed(generation, peer, recovered.epoch));
    ++peer;
    assert(x4_mic_capture_attach(generation, peer));
    recovered = next_frame(generation, peer, 480);
    atomic_store(&device, UNPLUGGED);
    expect_status(X4_MIC_UNAVAILABLE);
    assert(!x4_mic_capture_send_allowed(generation, peer, recovered.epoch));
    x4_mic_session_end();
    expect_idle();
    assert(atomic_load(&opens) == atomic_load(&closes));
}

static void blocked_read_does_not_keep_peer(void)
{
    uint64_t generation = x4_mic_session_begin(true, 41, 10), peer = 11;
    atomic_store(&device, PRESENT);
    atomic_store(&hold, true);
    assert(x4_mic_capture_attach(generation, peer));
    expect_held();
    unsigned opens_before = atomic_load(&opens);
    x4_mic_capture_detach(generation, peer);
    x4_mic_session_end();
    assert(!x4_mic_snapshot().enabled && !x4_mic_capture_test_idle());

    uint64_t new_generation = x4_mic_session_begin(true, 42, 20), new_peer = 12;
    assert(new_generation != generation && x4_mic_capture_attach(new_generation, new_peer));
    assert(!x4_mic_capture_attach(generation, peer));
    x4_mic_capture_detach(generation, peer);
    assert(atomic_load(&opens) == opens_before && atomic_load(&observed_user) == 41);
    atomic_store(&hold, false);
    X4MicFrame fresh = next_frame(new_generation, new_peer, X4_PUBLIC ? 480 : 960);
    assert(atomic_load(&observed_user) == 42 && atomic_load(&opens) == opens_before + 1);
    assert(!x4_mic_capture_send_allowed(generation, peer, fresh.epoch));
    x4_mic_session_end();
    expect_idle();
    assert(atomic_load(&opens) == atomic_load(&closes));
}

static void fallback_dtx_cpu_and_expiry(void)
{
    atomic_store(&low_only, true);
    atomic_store(&silent, true);
    uint64_t generation = x4_mic_session_begin(true, 42, 20), peer = 22;
    unsigned samples = X4_PUBLIC ? 480 : 960;
    assert(x4_mic_request().frame_ms == (X4_PUBLIC ? 10u : 20u));
    unsigned before_high = atomic_load(&attempts_high), before_low = atomic_load(&attempts_low);
    assert(x4_mic_capture_attach(generation, peer));
    expect_status(X4_MIC_ACTIVE);
    assert(x4_mic_snapshot().rate == 16000 && x4_mic_snapshot().level == 0);
    assert(atomic_load(&attempts_high) >= before_high + 2 && atomic_load(&attempts_low) > before_low);

    unsigned received = 0;
    uint64_t end = clock_us() + 1000000;
    X4MicFrame frame;
    while (clock_us() < end) {
        received += x4_mic_capture_take(generation, peer, clock_us(), &frame);
        sleep_us(1000);
    }
    assert(received < (X4_PUBLIC ? 55u : 30u));
    atomic_store(&silent, false);
    unsigned measured_before = atomic_load(&frames_read);
    uint64_t cpu_before = atomic_load(&cpu_total_ns);
    X4MicFrame last = next_frame(generation, peer, samples);
    for (unsigned i = 0; i < 25; ++i) {
        X4MicFrame next = next_frame(generation, peer, samples);
        assert(next.sample_us > last.sample_us);
        assert((next.sample_us - last.sample_us) % (samples / 48 * 1000) == 0);
        last = next;
    }
    unsigned measured = atomic_load(&frames_read) - measured_before;
    uint64_t cpu = atomic_load(&cpu_total_ns) - cpu_before;
    assert(measured > 10 && cpu > 0);
    uint64_t per10 = cpu * 10000 / ((uint64_t)measured * 16000);
    printf("T34 capture CPU ns_per_10ms=%llu input_grains=%u\n", (unsigned long long)per10, measured);
    uint64_t deadline = clock_us() + 2000000;
    while (x4_mic_capture_test_pending() < 2 && clock_us() < deadline) sleep_us(500);
    atomic_store(&hold, true);
    expect_held();
    assert(x4_mic_capture_test_pending() >= 2);
    assert(!x4_mic_capture_take(generation, peer, clock_us() + 40001, &frame));
    assert(!x4_mic_capture_test_pending());
    x4_mic_session_end();
    atomic_store(&hold, false);
    expect_idle();
    assert(atomic_load(&opens) == atomic_load(&closes));
}

typedef struct {
    uint64_t generation, peer;
    atomic_bool stop;
    atomic_uint taken;
} Sender;

static void *consume(void *arg)
{
    Sender *sender = arg;
    X4MicFrame frame;
    while (!atomic_load(&sender->stop)) {
        if (x4_mic_capture_take(sender->generation, sender->peer, clock_us(), &frame)) {
            assert(frame.epoch && frame.samples == 480 && frame.size > 2);
            (void)x4_mic_capture_send_allowed(sender->generation, sender->peer, frame.epoch);
            atomic_fetch_add(&sender->taken, 1);
        }
        sleep_us(200);
    }
    return NULL;
}

static void close_while_consuming(void)
{
    atomic_store(&low_only, false);
    Sender sender = {.generation = x4_mic_session_begin(true, 41, 10), .peer = 32};
    assert(x4_mic_capture_attach(sender.generation, sender.peer));
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, consume, &sender));
    uint64_t deadline = clock_us() + 4000000;
    while (atomic_load(&sender.taken) < 4 && clock_us() < deadline) sleep_us(250);
    assert(atomic_load(&sender.taken) >= 4);
    x4_mic_toggle_mute();
    assert(x4_mic_snapshot().muted);
    x4_mic_toggle_mute();
    x4_mic_capture_detach(sender.generation, sender.peer);
    x4_mic_session_end();
    atomic_store(&sender.stop, true);
    assert(!pthread_join(thread, NULL));
    expect_idle();
}

int main(void)
{
    disabled_and_invalid();
    waiting_and_device_recovery();
    blocked_read_does_not_keep_peer();
    fallback_dtx_cpu_and_expiry();
    close_while_consuming();
    printf("PASS T34 capture: 48/16 kHz, %u ms, consent, device absence and recovery, mute, DTX, age and in-flight close\n",
           X4_PUBLIC ? 10u : 20u);
    return 0;
}
