/* SPDX-License-Identifier: GPL-3.0-only */
/* Portable POSIX host tests. No PS4 app/decoder/network execution. */
#define _POSIX_C_SOURCE 200809L
#include "video_ingress.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { CAPACITY = 256, PRODUCERS = 4, PER_PRODUCER = 1000,
    MAX_PACKET_RETRIES = 100000, MAX_CONSUMER_POLLS = 5000000 };

static void fail(const char *expression, const char *file, int line)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}
#define CHECK(expression) do { if (!(expression)) fail(#expression, __FILE__, __LINE__); } while (0)

uint64_t x4_ingress_test_now_us(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static void alarm_expired(int signal_number)
{
    (void)signal_number;
    static const char message[] = "FAIL host test process deadline expired\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(124);
}

static void timed_wait(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    struct timespec deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    int rc = pthread_cond_timedwait(condition, mutex, &deadline);
    CHECK(rc == 0); /* A deadline is a test failure, never an endless wait. */
}

static void packet(uint8_t *out, size_t size, uint32_t producer, uint32_t sequence)
{
    CHECK(size >= 12 && size <= X4_INGRESS_PACKET_MAX);
    for (size_t i = 0; i < size; ++i)
        out[i] = (uint8_t)(producer * 29u + sequence * 17u + (uint32_t)i);
    uint32_t magic = 0x58444951u;
    memcpy(out, &magic, 4);
    memcpy(out + 4, &producer, 4);
    memcpy(out + 8, &sequence, 4);
}

static uint64_t packet_arrival(uint32_t producer, uint32_t sequence)
{ return ((uint64_t)producer << 32) + sequence + 1u; }

static void verify(const uint8_t *data, size_t size, uint32_t producer, uint32_t sequence)
{
    uint8_t expected[X4_INGRESS_PACKET_MAX];
    packet(expected, size, producer, sequence);
    CHECK(memcmp(data, expected, size) == 0);
}

static bool push_packet(X4VideoIngress *q, uint32_t producer, uint32_t sequence,
    size_t size, X4IngressResult *result)
{
    uint8_t data[X4_INGRESS_PACKET_MAX];
    packet(data, size, producer, sequence);
    return x4_video_ingress_push(q, data, size, packet_arrival(producer, sequence), result);
}

static void pop_packet(X4VideoIngress *q, uint32_t producer, uint32_t sequence, size_t size)
{
    uint8_t data[X4_INGRESS_PACKET_MAX];
    uint64_t arrival = 0;
    CHECK(x4_video_ingress_pop(q, data, &arrival) == size);
    verify(data, size, producer, sequence);
    CHECK(arrival == packet_arrival(producer, sequence));
}

static void dispose(X4VideoIngress *q)
{
    x4_video_ingress_stop(q);
    CHECK(atomic_load(&q->active_ops) == 0);
    CHECK(x4_video_ingress_free(q));
    CHECK(q->slots == NULL && q->capacity == 0);
}

/* One deterministic held hook; configured only while other operations stop. */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    X4VideoIngress *queue;
    unsigned event;
    uint64_t position;
    bool armed, entered, released;
} gate = { .mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER };

/* Exhaustion uses real nested successful reservations, not forged indices. */
static bool exhaust_cas;
static X4VideoIngress *exhaust_queue;
static unsigned competitor_sequence;
static _Thread_local bool in_competing_push;

void x4_ingress_test_hook(X4VideoIngress *q, unsigned event, uint64_t position)
{
    if (exhaust_cas && !in_competing_push && q == exhaust_queue && event == X4_INGRESS_BEFORE_CAS) {
        X4IngressResult result;
        in_competing_push = true;
        CHECK(push_packet(q, 9, competitor_sequence++, 64, &result));
        CHECK(result.reason == X4_INGRESS_OK);
        in_competing_push = false;
    }
    CHECK(pthread_mutex_lock(&gate.mutex) == 0);
    if (gate.armed && gate.queue == q && gate.event == event && gate.position == position) {
        gate.entered = true;
        CHECK(pthread_cond_broadcast(&gate.condition) == 0);
        while (!gate.released) timed_wait(&gate.condition, &gate.mutex);
    }
    CHECK(pthread_mutex_unlock(&gate.mutex) == 0);
}

static void hold_at(X4VideoIngress *q, unsigned event, uint64_t position)
{
    CHECK(pthread_mutex_lock(&gate.mutex) == 0);
    gate.queue = q; gate.event = event; gate.position = position;
    gate.armed = true; gate.entered = gate.released = false;
    CHECK(pthread_mutex_unlock(&gate.mutex) == 0);
}

static void wait_held(void)
{
    CHECK(pthread_mutex_lock(&gate.mutex) == 0);
    while (!gate.entered) timed_wait(&gate.condition, &gate.mutex);
    CHECK(pthread_mutex_unlock(&gate.mutex) == 0);
}

static void release_held(void)
{
    CHECK(pthread_mutex_lock(&gate.mutex) == 0);
    gate.released = true;
    CHECK(pthread_cond_broadcast(&gate.condition) == 0);
    CHECK(pthread_mutex_unlock(&gate.mutex) == 0);
}

static void disarm(void)
{
    CHECK(pthread_mutex_lock(&gate.mutex) == 0);
    gate.armed = false; gate.queue = NULL;
    CHECK(pthread_mutex_unlock(&gate.mutex) == 0);
}

typedef struct {
    X4VideoIngress *queue;
    uint32_t producer, sequence;
    size_t size, popped;
    uint64_t arrival;
    uint8_t data[X4_INGRESS_PACKET_MAX];
    X4IngressResult result;
    bool pushed;
} Job;

static void *push_job(void *context)
{
    Job *job = context;
    job->pushed = push_packet(job->queue, job->producer, job->sequence, job->size, &job->result);
    return NULL;
}

static void *pop_job(void *context)
{
    Job *job = context;
    job->popped = x4_video_ingress_pop(job->queue, job->data, &job->arrival);
    return NULL;
}

static void invalid_and_full(void)
{
    X4VideoIngress q = {0}; X4IngressResult result; uint8_t byte = 7;
    CHECK(x4_video_ingress_free(NULL));
    CHECK(x4_video_ingress_free(&q));
    CHECK(x4_video_ingress_init(NULL, CAPACITY) < 0);
    CHECK(x4_video_ingress_init(&q, 0) < 0); CHECK(x4_video_ingress_free(&q));
    CHECK(x4_video_ingress_init(&q, 3) < 0); CHECK(x4_video_ingress_free(&q));
    CHECK(x4_video_ingress_init(&q, 1025) < 0); CHECK(x4_video_ingress_free(&q));
    CHECK(x4_video_ingress_init(&q, CAPACITY) == 0);
    CHECK(!x4_video_ingress_free(&q));
    CHECK(!x4_video_ingress_push(&q, NULL, 1, 1, &result)); CHECK(result.reason == X4_INGRESS_INVALID);
    CHECK(!x4_video_ingress_push(&q, &byte, 0, 1, &result)); CHECK(result.reason == X4_INGRESS_INVALID);
    CHECK(!x4_video_ingress_push(&q, &byte, X4_INGRESS_PACKET_MAX + 1u, 1, &result));
    CHECK(result.reason == X4_INGRESS_INVALID && atomic_load(&q.invalid) == 3);
    CHECK(x4_video_ingress_depth(&q) == 0);
    for (unsigned i = 0; i < CAPACITY; ++i) {
        CHECK(push_packet(&q, 0, i, X4_INGRESS_PACKET_MAX, &result));
        CHECK(result.reason == X4_INGRESS_OK);
    }
    CHECK(x4_video_ingress_depth(&q) == CAPACITY);
    CHECK(x4_video_ingress_oldest(&q) == 1);
    CHECK(!push_packet(&q, 0, CAPACITY, 64, &result));
    CHECK(result.reason == X4_INGRESS_FULL && atomic_load(&q.full) == 1);
    CHECK(x4_video_ingress_depth(&q) == CAPACITY);
    for (unsigned i = 0; i < CAPACITY; ++i) pop_packet(&q, 0, i, X4_INGRESS_PACKET_MAX);
    CHECK(x4_video_ingress_oldest(&q) == 0 && x4_video_ingress_depth(&q) == 0);
    dispose(&q);
    puts("PASS invalid arguments / true full queue / maximum packet integrity");
}

static void consume_overlap_with_space(void)
{
    X4VideoIngress q; X4IngressResult result; pthread_t thread;
    CHECK(x4_video_ingress_init(&q, CAPACITY) == 0);
    CHECK(push_packet(&q, 0, 0, 64, &result));
    Job pop = {.queue = &q};
    hold_at(&q, X4_INGRESS_BEFORE_RELEASE, 0);
    CHECK(pthread_create(&thread, NULL, pop_job, &pop) == 0); wait_held();
    CHECK(atomic_load(&q.active_ops) == 1 && x4_video_ingress_depth(&q) == 1);
#ifdef X4_INGRESS_BASELINE
    CHECK(!push_packet(&q, 0, 1, 64, &result));
    CHECK(result.reason == X4_INGRESS_CONTENDED && result.owner_sample == 2);
    CHECK(x4_video_ingress_depth(&q) == 1 && atomic_load(&q.contended) == 1);
#else
    CHECK(push_packet(&q, 0, 1, 64, &result));
    CHECK(result.reason == X4_INGRESS_OK && result.retries == 0);
    CHECK(x4_video_ingress_depth(&q) == 2 && atomic_load(&q.contended) == 0);
#endif
    release_held(); CHECK(pthread_join(thread, NULL) == 0); disarm();
    CHECK(pop.popped == 64); verify(pop.data, 64, 0, 0); CHECK(pop.arrival == 1);
#ifdef X4_INGRESS_BASELINE
    CHECK(push_packet(&q, 0, 1, 64, &result));
#endif
    pop_packet(&q, 0, 1, 64); dispose(&q);
#ifdef X4_INGRESS_BASELINE
    puts("PASS baseline reproduces push contention with space / sampled owner=consumer");
#else
    puts("PASS push during held consumer release with space: no local contention loss");
#endif
}

#ifndef X4_INGRESS_BASELINE
static void unpublished_cannot_be_bypassed(void)
{
    X4VideoIngress q; X4IngressResult result; pthread_t thread; uint8_t out[X4_INGRESS_PACKET_MAX];
    CHECK(x4_video_ingress_init(&q, CAPACITY) == 0);
    Job first = {.queue=&q, .producer=0, .sequence=0, .size=64};
    hold_at(&q, X4_INGRESS_AFTER_RESERVE, 0);
    CHECK(pthread_create(&thread, NULL, push_job, &first) == 0); wait_held();
    CHECK(push_packet(&q, 0, 1, 64, &result));
    CHECK(x4_video_ingress_depth(&q) == 2 && x4_video_ingress_oldest(&q) == 0);
    CHECK(x4_video_ingress_pop(&q, out, NULL) == 0);
    release_held(); CHECK(pthread_join(thread, NULL) == 0); disarm();
    CHECK(first.pushed && first.result.reason == X4_INGRESS_OK);
    pop_packet(&q, 0, 0, 64); pop_packet(&q, 0, 1, 64); dispose(&q);
    puts("PASS later published reservation cannot bypass unpublished FIFO head");
}
#endif

static void held_slot_is_not_overwritten(void)
{
    X4VideoIngress q; X4IngressResult result; pthread_t thread;
    CHECK(x4_video_ingress_init(&q, CAPACITY) == 0);
    for (unsigned i=0; i<CAPACITY; ++i) CHECK(push_packet(&q, 0, i, 64, &result));
    Job pop = {.queue=&q};
    hold_at(&q, X4_INGRESS_BEFORE_RELEASE, 0);
    CHECK(pthread_create(&thread, NULL, pop_job, &pop) == 0); wait_held();
    CHECK(!push_packet(&q, 0, CAPACITY, 64, &result));
#ifdef X4_INGRESS_BASELINE
    CHECK(result.reason == X4_INGRESS_CONTENDED && result.owner_sample == 2);
#else
    CHECK(result.reason == X4_INGRESS_FULL);
#endif
    CHECK(atomic_load(&q.slots[0].ready)); verify(q.slots[0].data, 64, 0, 0);
    release_held(); CHECK(pthread_join(thread, NULL) == 0); disarm();
    CHECK(pop.popped == 64); verify(pop.data, 64, 0, 0);
    CHECK(push_packet(&q, 0, CAPACITY, 64, &result));
    for (unsigned i=1; i<=CAPACITY; ++i) pop_packet(&q, 0, i, 64);
    dispose(&q); puts("PASS held full-ring slot is retained until consumer release");
}

static void stop_during_activity(void)
{
    X4VideoIngress q; X4IngressResult result; pthread_t thread;
    CHECK(x4_video_ingress_init(&q, CAPACITY) == 0);
    Job push = {.queue=&q, .producer=0, .sequence=7, .size=64};
    hold_at(&q, X4_INGRESS_AFTER_RESERVE, 0);
    CHECK(pthread_create(&thread, NULL, push_job, &push) == 0); wait_held();
    X4IngressSlot *saved = q.slots;
    x4_video_ingress_stop(&q);
    CHECK(!x4_video_ingress_free(&q)); CHECK(q.slots == saved && q.capacity == CAPACITY);
    CHECK(!push_packet(&q, 0, 8, 64, &result)); CHECK(result.reason == X4_INGRESS_STOPPED);
#ifdef X4_INGRESS_BASELINE
    CHECK(atomic_load(&q.enqueue) == 0); /* Baseline publishes its tail after copy. */
#else
    CHECK(atomic_load(&q.enqueue) == 1);
#endif
    release_held(); CHECK(pthread_join(thread, NULL) == 0); disarm();
    CHECK(push.pushed && push.result.reason == X4_INGRESS_OK); /* Existing reservation may finish. */
    pop_packet(&q, 0, 7, 64); dispose(&q);
    CHECK(x4_video_ingress_init(&q, CAPACITY) == 0);
    CHECK(push_packet(&q, 0, 9, 64, &result)); Job pop = {.queue=&q};
    hold_at(&q, X4_INGRESS_BEFORE_RELEASE, 0);
    CHECK(pthread_create(&thread, NULL, pop_job, &pop) == 0); wait_held();
    x4_video_ingress_stop(&q); CHECK(!x4_video_ingress_free(&q));
    release_held(); CHECK(pthread_join(thread, NULL) == 0); disarm();
    CHECK(pop.popped == 64); verify(pop.data, 64, 0, 9); dispose(&q);
    puts("PASS stop during producer/consumer activity retains memory until joins");
}

static void wrap_and_bounded_exhaustion(void)
{
    X4VideoIngress q; X4IngressResult result;
    CHECK(x4_video_ingress_init(&q, CAPACITY) == 0);
    uint64_t base = UINT64_MAX - (CAPACITY - 1u); /* Quiescent, capacity-aligned. */
    CHECK((base & (CAPACITY - 1u)) == 0);
    atomic_store(&q.enqueue, base); atomic_store(&q.dequeue, base);
    for (unsigned i=0; i<2*CAPACITY; ++i) {
        CHECK(push_packet(&q, 0, i, 64, &result)); pop_packet(&q, 0, i, 64);
    }
    CHECK(atomic_load(&q.enqueue) == base + 2u*CAPACITY);
    CHECK(atomic_load(&q.enqueue) == atomic_load(&q.dequeue)); dispose(&q);
#ifndef X4_INGRESS_BASELINE
    CHECK(x4_video_ingress_init(&q, CAPACITY) == 0);
    exhaust_queue=&q; competitor_sequence=0; exhaust_cas=true;
    CHECK(!push_packet(&q, 0, 99, 64, &result)); exhaust_cas=false;
    CHECK(result.reason == X4_INGRESS_CONTENDED && result.retries == X4_INGRESS_RESERVE_ATTEMPTS);
    CHECK(competitor_sequence == X4_INGRESS_RESERVE_ATTEMPTS);
    CHECK(x4_video_ingress_depth(&q) == X4_INGRESS_RESERVE_ATTEMPTS);
    CHECK(atomic_load(&q.contended) == 1 && atomic_load(&q.retries) == X4_INGRESS_RESERVE_ATTEMPTS);
    for (unsigned i=0; i<X4_INGRESS_RESERVE_ATTEMPTS; ++i) pop_packet(&q, 9, i, 64);
    CHECK(push_packet(&q, 0, 100, 64, &result)); pop_packet(&q, 0, 100, 64);
    dispose(&q); exhaust_queue=NULL;
    puts("PASS uint64 wrap / bounded real competing-CAS exhaustion without corruption");
#else
    puts("PASS uint64 wrap; MPSC reservation exhaustion is candidate-only");
#endif
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    unsigned arrived, needed;
} StartGate;

static void start_together(StartGate *start)
{
    CHECK(pthread_mutex_lock(&start->mutex) == 0);
    ++start->arrived;
    if (start->arrived == start->needed) CHECK(pthread_cond_broadcast(&start->condition) == 0);
    while (start->arrived != start->needed) timed_wait(&start->condition, &start->mutex);
    CHECK(pthread_mutex_unlock(&start->mutex) == 0);
}

typedef struct {
    X4VideoIngress queue;
    StartGate start;
    atomic_uint done;
    atomic_uint_fast64_t full_retries, contention_retries;
} Stress;
typedef struct { Stress *stress; unsigned producer; } Producer;

static size_t stress_size(unsigned producer, unsigned sequence)
{ return 64u + ((sequence + producer) % 31u) * 64u; }

static void *stress_producer(void *context)
{
    Producer *p=context; Stress *s=p->stress; start_together(&s->start);
    for (unsigned sequence=0; sequence<PER_PRODUCER; ++sequence) {
        bool accepted=false;
        for (unsigned n=0; n<MAX_PACKET_RETRIES; ++n) {
            X4IngressResult result;
            if (push_packet(&s->queue, p->producer, sequence, stress_size(p->producer, sequence), &result)) {
                CHECK(result.reason == X4_INGRESS_OK); accepted=true; break;
            }
            CHECK(result.reason == X4_INGRESS_FULL || result.reason == X4_INGRESS_CONTENDED);
            atomic_fetch_add(result.reason == X4_INGRESS_FULL ? &s->full_retries : &s->contention_retries, 1);
            CHECK(sched_yield() == 0);
        }
        CHECK(accepted);
    }
    atomic_fetch_add_explicit(&s->done, 1, memory_order_release); return NULL;
}

static void *stress_consumer(void *context)
{
    Stress *s=context; unsigned expected[PRODUCERS]={0}; unsigned consumed=0;
    uint8_t out[X4_INGRESS_PACKET_MAX]; start_together(&s->start);
    for (unsigned poll=0; poll<MAX_CONSUMER_POLLS && consumed<PRODUCERS*PER_PRODUCER; ++poll) {
        uint64_t arrival=0; size_t size=x4_video_ingress_pop(&s->queue, out, &arrival);
        if (!size) { CHECK(sched_yield() == 0); continue; }
        CHECK(size >= 12); uint32_t producer, sequence;
        memcpy(&producer,out+4,4); memcpy(&sequence,out+8,4);
        CHECK(producer<PRODUCERS && sequence==expected[producer]);
        CHECK(size==stress_size(producer,sequence)); verify(out,size,producer,sequence);
        CHECK(arrival==packet_arrival(producer,sequence)); ++expected[producer]; ++consumed;
    }
    CHECK(consumed == PRODUCERS*PER_PRODUCER);
    for (unsigned i=0; i<PRODUCERS; ++i) CHECK(expected[i] == PER_PRODUCER);
    return NULL;
}

static void concurrent_producers(void)
{
    Stress s = { .start = { .mutex=PTHREAD_MUTEX_INITIALIZER,
        .condition=PTHREAD_COND_INITIALIZER, .needed=PRODUCERS+2 } };
    atomic_init(&s.done,0); atomic_init(&s.full_retries,0); atomic_init(&s.contention_retries,0);
    CHECK(x4_video_ingress_init(&s.queue,CAPACITY)==0);
    Producer producers[PRODUCERS]; pthread_t threads[PRODUCERS], consumer;
    for (unsigned i=0; i<PRODUCERS; ++i) {
        producers[i]=(Producer){.stress=&s,.producer=i};
        CHECK(pthread_create(&threads[i],NULL,stress_producer,&producers[i])==0);
    }
    CHECK(pthread_create(&consumer,NULL,stress_consumer,&s)==0); start_together(&s.start);
    for (unsigned i=0; i<PRODUCERS; ++i) CHECK(pthread_join(threads[i],NULL)==0);
    CHECK(pthread_join(consumer,NULL)==0);
    CHECK(atomic_load(&s.done)==PRODUCERS && x4_video_ingress_depth(&s.queue)==0);
    CHECK(atomic_load(&s.queue.active_ops)==0 && atomic_load(&s.queue.highwater)<=CAPACITY);
    printf("PASS MPSC four persistent producers: %u packets, ordered/integral per producer, retry_full=%llu retry_contention=%llu\n",
        PRODUCERS*PER_PRODUCER,(unsigned long long)atomic_load(&s.full_retries),
        (unsigned long long)atomic_load(&s.contention_retries));
    dispose(&s.queue); CHECK(pthread_cond_destroy(&s.start.condition)==0);
    CHECK(pthread_mutex_destroy(&s.start.mutex)==0);
}

int main(void)
{
    CHECK(signal(SIGALRM,alarm_expired)!=SIG_ERR); alarm(30);
#ifdef X4_INGRESS_BASELINE
    puts("MODE instrumented baseline (shared try-only gate)");
#else
    puts("MODE candidate MPSC (no consumer gate on push)");
#endif
    invalid_and_full(); consume_overlap_with_space();
#ifndef X4_INGRESS_BASELINE
    unpublished_cannot_be_bypassed();
#endif
    held_slot_is_not_overwritten(); stop_during_activity(); wrap_and_bounded_exhaustion();
    concurrent_producers();
    CHECK(pthread_cond_destroy(&gate.condition)==0); CHECK(pthread_mutex_destroy(&gate.mutex)==0);
    alarm(0); puts("PASS video ingress host harness; no console/app execution"); return 0;
}
