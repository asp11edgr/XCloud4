/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable host verification. No controller, RTC, app, or decoder execution. */
#define _POSIX_C_SOURCE 200809L
#include "rumble.h"
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *s, int line)
{
    fprintf(stderr, "FAIL rumble_test.c:%d %s\n", line, s);
    exit(1);
}
#define CHECK(s) do { if (!(s)) fail(#s, __LINE__); } while (0)
static void timeout(int signal_number)
{
    (void)signal_number;
    static const char text[] = "FAIL rumble test deadline\n";
    ssize_t result = write(STDERR_FILENO, text, sizeof(text) - 1);
    (void)result;
    _exit(2);
}
static uint8_t modern[13] = {128, 0, 0, 0, 100, 50, 7, 9, 100, 0, 30, 0, 2};
static pthread_mutex_t hook_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hook_condition = PTHREAD_COND_INITIALIZER;
static bool hook_enabled, hook_entered, hook_release;
void x4_rumble_test_after_gate(X4Rumble *r)
{
    (void)r;
    CHECK(pthread_mutex_lock(&hook_lock) == 0);
    if (hook_enabled) {
        hook_entered = true;
        CHECK(pthread_cond_broadcast(&hook_condition) == 0);
        while (!hook_release) CHECK(pthread_cond_wait(&hook_condition, &hook_lock) == 0);
    }
    CHECK(pthread_mutex_unlock(&hook_lock) == 0);
}
static X4Rumble *fresh(void)
{
    X4Rumble *r = x4_rumble_new();
    CHECK(r && x4_rumble_begin(r));
    return r;
}
static void dispose(X4Rumble *r)
{
    x4_rumble_disable(r);
    CHECK(x4_rumble_free(r));
}
static void test_decode(void)
{
    X4RumbleEvent e;
    uint8_t p[13];
    memcpy(p, modern, sizeof(p));
    p[8] = 0x34; p[9] = 0x12; p[10] = 0x78; p[11] = 0x56; p[12] = 255;
    CHECK(x4_rumble_parse(p, sizeof(p), &e) == X4_RUMBLE_VALID);
    CHECK(e.gamepad_index == 0 && e.left_motor == 100 && e.right_motor == 50);
    CHECK(e.left_trigger == 7 && e.right_trigger == 9);
    CHECK(e.duration_ms == 0x1234 && e.delay_ms == 0x5678 && e.repeat == 255);
    uint8_t legacy[12] = {128, 0, 0, 100, 50, 7, 9, 0x34, 0x12, 0x78, 0x56, 255};
    CHECK(x4_rumble_parse(legacy, sizeof(legacy), &e) == X4_RUMBLE_VALID);
    CHECK(e.left_motor == 100 && e.right_motor == 50 && e.duration_ms == 0x1234);
    CHECK(e.delay_ms == 0x5678 && e.repeat == 255);
}
static void test_bounds(void)
{
    uint8_t p[64] = {128};
    X4RumbleEvent e;
    for (size_t n = 0; n <= sizeof(p); ++n) {
        if (n == 12 || n == 13) continue;
        CHECK(x4_rumble_parse(p, n, &e) == X4_RUMBLE_MALFORMED);
    }
    CHECK(x4_rumble_parse(NULL, 13, &e) == X4_RUMBLE_MALFORMED);
    CHECK(x4_rumble_parse(modern, 13, NULL) == X4_RUMBLE_MALFORMED);
    memcpy(p, modern, 13); p[0] = 16;
    CHECK(x4_rumble_parse(p, 13, &e) == X4_RUMBLE_OTHER);
    memcpy(p, modern, 13); p[1] = 2;
    CHECK(x4_rumble_parse(p, 13, &e) == X4_RUMBLE_UNSUPPORTED);
    memcpy(p, modern, 13); p[2] = 1;
    CHECK(x4_rumble_parse(p, 13, &e) == X4_RUMBLE_UNSUPPORTED);
    memcpy(p, modern, 13); p[3] = 1;
    CHECK(x4_rumble_parse(p, 13, &e) == X4_RUMBLE_UNSUPPORTED);
    for (unsigned at = 4; at <= 7; ++at) {
        memcpy(p, modern, 13); p[at] = 101;
        CHECK(x4_rumble_parse(p, 13, &e) == X4_RUMBLE_MALFORMED);
    }
}
static void test_motor_mapping(void)
{
    CHECK(x4_rumble_motor_byte(0) == 0);
    CHECK(x4_rumble_motor_byte(1) == 3);
    CHECK(x4_rumble_motor_byte(50) == 128);
    CHECK(x4_rumble_motor_byte(100) == 255);
    CHECK(x4_rumble_motor_byte(255) == 255);
    for (unsigned i = 1; i <= 100; ++i)
        CHECK(x4_rumble_motor_byte((uint8_t)i) >= x4_rumble_motor_byte((uint8_t)(i - 1)));
}
static void test_timing(void)
{
    X4Rumble *r = fresh();
    X4RumbleOutput out;
    CHECK(x4_rumble_receive(r, modern, 13, 1000) == X4_RUMBLE_VALID);
    CHECK(x4_rumble_poll(r, 1000, &out) && out.lgMotor == 255 && out.smMotor == 128);
    CHECK(!x4_rumble_poll(r, 100999, &out) && out.lgMotor == 255);
    CHECK(x4_rumble_poll(r, 101000, &out) && out.lgMotor == 0 && out.smMotor == 0);
    CHECK(!x4_rumble_poll(r, 101001, &out));
    CHECK(x4_rumble_receive(r, modern, 13, 200000) == X4_RUMBLE_VALID);
    CHECK(!x4_rumble_poll(r, 400000, &out) && !out.lgMotor); /* Already expired. */
    dispose(r);
}
static void test_fresh_poll_after_frame_start(void)
{
    /* Module fixture for the caller's clock contract, not execution of main:
     * the callback receives a pulse after an earlier input/frame timestamp. */
    const uint64_t frame_start = 1000, received = 2500, fresh_poll = 3000;
    CHECK(frame_start < received && received < fresh_poll);
    X4Rumble *r = fresh();
    X4RumbleOutput out;
    CHECK(x4_rumble_receive(r, modern, sizeof(modern), received) == X4_RUMBLE_VALID);
    CHECK(x4_rumble_poll(r, fresh_poll, &out));
    CHECK(out.lgMotor == 255 && out.smMotor == 128 && out.sequence == 1);
    CHECK(!x4_rumble_poll(r, received + 99999, &out) && out.lgMotor == 255);
    CHECK(x4_rumble_poll(r, received + 100000, &out));
    CHECK(!out.lgMotor && !out.smMotor); /* Receipt anchor is not renewed. */
    dispose(r);
}
static void test_deferred_pending_time(void)
{
    /* Deterministic module boundary: a producer publishes after the caller's
     * clock sample, before poll acquires the slot. No hardware/main execution. */
    X4Rumble *r = fresh();
    X4RumbleOutput out;
    CHECK(x4_rumble_receive(r, modern, sizeof(modern), 2000) == X4_RUMBLE_VALID);
    CHECK(!x4_rumble_poll(r, 1999, &out) && !out.lgMotor && !out.smMotor);
    CHECK(out.sequence == 0);
    CHECK(!x4_rumble_poll(r, 1999, &out) && out.sequence == 0);
    CHECK(x4_rumble_poll(r, 2000, &out));
    CHECK(out.lgMotor == 255 && out.smMotor == 128 && out.sequence == 1);
    X4RumbleSnapshot stats; x4_rumble_snapshot(r, &stats);
    CHECK(stats.accepted == 1 && stats.superseded == 0);

    uint8_t stop[13]; memcpy(stop, modern, sizeof(stop)); stop[8] = stop[9] = 0;
    CHECK(x4_rumble_receive(r, stop, sizeof(stop), 3000) == X4_RUMBLE_VALID);
    CHECK(!x4_rumble_poll(r, 2999, &out)); /* Existing pulse keeps its age rule. */
    CHECK(out.lgMotor == 255 && out.smMotor == 128 && out.sequence == 1);
    CHECK(x4_rumble_poll(r, 3000, &out));
    CHECK(!out.lgMotor && !out.smMotor && out.sequence == 2);

    CHECK(x4_rumble_receive(r, modern, sizeof(modern), UINT64_MAX - 10) == X4_RUMBLE_VALID);
    CHECK(!x4_rumble_poll(r, 4000, &out) && !out.lgMotor && out.sequence == 2);
    CHECK(!x4_rumble_poll(r, UINT64_MAX - 10, &out));
    CHECK(!out.lgMotor && !out.smMotor && out.sequence == 3); /* Expiry overflow stays zero. */

    CHECK(x4_rumble_receive(r, modern, sizeof(modern), 10000) == X4_RUMBLE_VALID);
    CHECK(!x4_rumble_poll(r, 9999, &out) && !out.lgMotor);
    x4_rumble_disable(r);
    CHECK(!x4_rumble_poll(r, 10000, &out) && !out.lgMotor && !out.smMotor);
    CHECK(x4_rumble_begin(r)); /* No producer is running; pending old epoch is cleared. */
    CHECK(!x4_rumble_poll(r, 10000, &out) && !out.lgMotor && out.sequence == 0 && out.epoch == 2);
    dispose(r);
}
static void test_latest_and_epoch(void)
{
    X4Rumble *r = fresh();
    uint8_t p[13]; memcpy(p, modern, 13); p[4] = 20; p[5] = 80;
    CHECK(x4_rumble_receive(r, modern, 13, 1000) == 1);
    CHECK(x4_rumble_receive(r, p, 13, 2000) == 1);
    X4RumbleOutput out;
    CHECK(x4_rumble_poll(r, 3000, &out) && out.lgMotor == 51 && out.smMotor == 204);
    CHECK(out.epoch == 1 && out.sequence == 2);
    X4RumbleSnapshot stats; x4_rumble_snapshot(r, &stats);
    CHECK(stats.valid == 2 && stats.accepted == 2 && stats.superseded == 1);
    x4_rumble_disable(r);
    CHECK(x4_rumble_poll(r, 4000, &out) && !out.lgMotor && !out.smMotor);
    CHECK(x4_rumble_begin(r)); /* Producers/poller are quiescent. */
    CHECK(!x4_rumble_poll(r, 5000, &out) && out.epoch == 2 && out.sequence == 0);
    x4_rumble_snapshot(r, &stats);
    CHECK(stats.accepted == 0 && stats.epoch == 2);
    dispose(r);
}
static void test_cap_and_zero(void)
{
    X4Rumble *r = fresh();
    uint8_t p[13]; memcpy(p, modern, 13); p[8] = p[9] = 255;
    X4RumbleOutput out;
    CHECK(x4_rumble_receive(r, p, 13, 1000) == 1);
    CHECK(x4_rumble_poll(r, 1000, &out) && out.lgMotor == 255);
    CHECK(!x4_rumble_poll(r, 5000999, &out) && out.lgMotor == 255);
    CHECK(x4_rumble_poll(r, 5001000, &out) && !out.lgMotor);
    CHECK(x4_rumble_receive(r, modern, 13, 6000000) == 1);
    CHECK(x4_rumble_poll(r, 6000000, &out) && out.lgMotor == 255);
    p[8] = p[9] = 0;
    CHECK(x4_rumble_receive(r, p, 13, 6000001) == 1);
    CHECK(x4_rumble_poll(r, 6000001, &out) && !out.lgMotor && !out.smMotor);
    dispose(r);
}
static void test_ignored_fields(void)
{
    X4Rumble *r = fresh();
    uint8_t p[13]; memcpy(p, modern, 13); p[10] = p[11] = p[12] = 255;
    p[6] = p[7] = 100;
    X4RumbleOutput out;
    CHECK(x4_rumble_receive(r, p, 13, 1000) == 1);
    CHECK(x4_rumble_poll(r, 1000, &out) && out.lgMotor == 255 && out.smMotor == 128);
    CHECK(x4_rumble_poll(r, 101000, &out) && !out.lgMotor); /* No repeat/delay. */
    p[4] = p[5] = 0;
    CHECK(x4_rumble_receive(r, p, 13, 102000) == 1);
    CHECK(!x4_rumble_poll(r, 102000, &out) && !out.lgMotor && !out.smMotor);
    dispose(r);
}
static void test_clock_safety(void)
{
    X4Rumble *r = fresh(); X4RumbleOutput out;
    CHECK(x4_rumble_receive(r, modern, 13, 1000) == 1);
    CHECK(!x4_rumble_poll(r, 999, &out) && !out.lgMotor);
    CHECK(x4_rumble_poll(r, 1000, &out) && out.lgMotor == 255);
    CHECK(x4_rumble_poll(r, 999, &out) && !out.lgMotor && !out.smMotor);
    CHECK(!x4_rumble_poll(r, 1000, &out) && !out.lgMotor); /* Active clock reversal retires pulse. */
    CHECK(x4_rumble_receive(r, modern, 13, UINT64_MAX - 10) == 1);
    CHECK(!x4_rumble_poll(r, UINT64_MAX - 10, &out) && !out.lgMotor);
    dispose(r);
}
static void test_null_lifecycle(void)
{
    X4RumbleOutput out; memset(&out, 255, sizeof(out));
    CHECK(!x4_rumble_begin(NULL)); x4_rumble_disable(NULL);
    CHECK(x4_rumble_free(NULL));
    CHECK(!x4_rumble_poll(NULL, 0, &out) && !out.lgMotor && out.epoch == 0);
    CHECK(x4_rumble_receive(NULL, modern, 13, 0) == X4_RUMBLE_INACTIVE);
    X4RumbleSnapshot stats; memset(&stats, 255, sizeof(stats));
    x4_rumble_snapshot(NULL, &stats); CHECK(stats.accepted == 0 && !stats.enabled);
    X4Rumble *r = fresh(); CHECK(!x4_rumble_begin(r) && !x4_rumble_free(r));
    dispose(r);
}
typedef struct { X4Rumble *r; int result; } BlockedProducer;
static void *blocked_receive(void *arg)
{
    BlockedProducer *p = arg;
    p->result = x4_rumble_receive(p->r, modern, 13, 1000);
    return NULL;
}
static void test_contention_and_disable(void)
{
    X4Rumble *r = fresh();
    CHECK(pthread_mutex_lock(&hook_lock) == 0);
    hook_enabled = true; hook_entered = hook_release = false;
    CHECK(pthread_mutex_unlock(&hook_lock) == 0);
    BlockedProducer p = {.r = r}; pthread_t thread;
    CHECK(pthread_create(&thread, NULL, blocked_receive, &p) == 0);
    CHECK(pthread_mutex_lock(&hook_lock) == 0);
    while (!hook_entered) CHECK(pthread_cond_wait(&hook_condition, &hook_lock) == 0);
    CHECK(pthread_mutex_unlock(&hook_lock) == 0);
    X4RumbleOutput out;
    CHECK(!x4_rumble_poll(r, 1000, &out) && !out.lgMotor); /* Never waits. */
    CHECK(x4_rumble_receive(r, modern, 13, 1000) == X4_RUMBLE_CONTENDED);
    x4_rumble_disable(r);
    CHECK(!x4_rumble_poll(r, 1000, &out) && !out.lgMotor);
    CHECK(pthread_mutex_lock(&hook_lock) == 0);
    hook_release = true;
    CHECK(pthread_cond_broadcast(&hook_condition) == 0);
    CHECK(pthread_mutex_unlock(&hook_lock) == 0);
    CHECK(pthread_join(thread, NULL) == 0 && p.result == X4_RUMBLE_INACTIVE);
    CHECK(pthread_mutex_lock(&hook_lock) == 0); hook_enabled = false;
    CHECK(pthread_mutex_unlock(&hook_lock) == 0);
    X4RumbleSnapshot stats; x4_rumble_snapshot(r, &stats);
    CHECK(stats.valid == 2 && stats.accepted == 0);
    CHECK(stats.contention_dropped == 1 && stats.inactive_dropped == 1);
    CHECK(x4_rumble_free(r)); /* All external producers joined. */
}
enum { PRODUCERS = 4, MESSAGES = 5000 };
static atomic_uint producers_done;
typedef struct { X4Rumble *r; unsigned index; } Producer;
static void *spam(void *arg)
{
    Producer *p = arg;
    for (unsigned i = 0; i < MESSAGES; ++i) {
        uint8_t packet[13]; memcpy(packet, modern, 13);
        packet[4] = (uint8_t)((i + p->index) % 101);
        packet[5] = (uint8_t)(100 - packet[4]);
        int rc = x4_rumble_receive(p->r, packet, 13, 1000);
        CHECK(rc == X4_RUMBLE_VALID || rc == X4_RUMBLE_CONTENDED);
    }
    atomic_fetch_add_explicit(&producers_done, 1, memory_order_release);
    return NULL;
}
static void test_concurrent_integrity(void)
{
    X4Rumble *r = fresh(); pthread_t threads[PRODUCERS]; Producer p[PRODUCERS];
    atomic_init(&producers_done, 0);
    for (unsigned i = 0; i < PRODUCERS; ++i) {
        p[i] = (Producer){r, i}; CHECK(pthread_create(&threads[i], NULL, spam, &p[i]) == 0);
    }
    for (unsigned polls = 0; atomic_load_explicit(&producers_done, memory_order_acquire) < PRODUCERS; ++polls) {
        CHECK(polls < 5000000);
        X4RumbleOutput out; (void)x4_rumble_poll(r, 2000, &out);
        if (out.sequence) CHECK((unsigned)out.lgMotor + out.smMotor >= 255 &&
            (unsigned)out.lgMotor + out.smMotor <= 256);
        sched_yield();
    }
    for (unsigned i = 0; i < PRODUCERS; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    X4RumbleSnapshot s; x4_rumble_snapshot(r, &s);
    CHECK(s.binary_messages == PRODUCERS * MESSAGES && s.valid == s.binary_messages);
    CHECK(s.accepted + s.contention_dropped == s.valid && s.accepted > 0);
    CHECK(!s.inactive_dropped && !s.malformed && !s.unsupported);
    dispose(r);
}
int main(void)
{
    CHECK(signal(SIGALRM, timeout) != SIG_ERR); alarm(60);
#define RUN(test) do { test(); puts("PASS " #test); } while (0)
    RUN(test_decode); RUN(test_bounds); RUN(test_motor_mapping); RUN(test_timing);
    RUN(test_fresh_poll_after_frame_start);
    RUN(test_deferred_pending_time);
    RUN(test_latest_and_epoch); RUN(test_cap_and_zero); RUN(test_ignored_fields);
    RUN(test_clock_safety); RUN(test_null_lifecycle);
    RUN(test_contention_and_disable); RUN(test_concurrent_integrity);
    puts("PASS all13 portable rumble tests (no PS4 behavior claim)");
    return 0;
}
