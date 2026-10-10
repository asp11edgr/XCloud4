/* SPDX-License-Identifier: GPL-3.0-only */
/* Requested portable diagnostic checks; no app, network or decoder execution. */
#define _POSIX_C_SOURCE 200809L
#include "live_monitor.h"
#include "live_trace.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { TEST_BEFORE_SAMPLE = 1, TEST_AFTER_SAMPLE = 2,
    TEST_BETWEEN_QUEUE_WITNESSES = 3, TEST_AFTER_EVENT_GATE = 4,
    WRITERS = 4, WRITER_CALLS = 4000 };

static void fail(const char *expression, const char *file, int line)
{
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
    exit(1);
}
#define CHECK(e) do { if (!(e)) fail(#e, __FILE__, __LINE__); } while (0)

static void alarm_expired(int number)
{
    (void)number;
    static const char message[] = "FAIL progress monitor test deadline expired\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(124);
}
static void wait_condition(pthread_cond_t *condition, pthread_mutex_t *mutex)
{
    struct timespec deadline;
    CHECK(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 5;
    CHECK(pthread_cond_timedwait(condition, mutex, &deadline) == 0);
}

/* One sampler per test. Clock changes and wakeups are controlled by the test,
 * rather than wall-clock sleeps. Every wait has a real process deadline. */
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    atomic_uint_fast64_t now;
    unsigned samples, sleeps, joins, forced_wakes;
    uint64_t last_target, last_actual, wake_at;
    bool waiting, release_sleep, fail_create, fail_join;
} host = { .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER };
typedef struct { pthread_t thread; } HostThread;

uint64_t x4_monitor_host_now_us(void)
{ return atomic_load_explicit(&host.now, memory_order_relaxed); }
uint64_t x4_ingress_test_now_us(void)
{ return x4_monitor_host_now_us(); }

void x4_monitor_host_sleep_us(uint64_t duration)
{
    CHECK(pthread_mutex_lock(&host.mutex) == 0);
    uint64_t now = x4_monitor_host_now_us();
    host.wake_at = now + duration;
    CHECK(host.wake_at >= now);
    host.sleeps++; host.waiting = true;
    CHECK(pthread_cond_broadcast(&host.condition) == 0);
    while (!host.release_sleep && !host.forced_wakes && x4_monitor_host_now_us() < host.wake_at)
        wait_condition(&host.condition, &host.mutex);
    if (host.forced_wakes) --host.forced_wakes;
    host.waiting = false;
    CHECK(pthread_mutex_unlock(&host.mutex) == 0);
}
int x4_monitor_host_create(X4MonitorThread *out, void *(*entry)(void *), void *arg)
{
    CHECK(out && entry);
    if (host.fail_create) return EAGAIN;
    HostThread *box = malloc(sizeof(*box));
    CHECK(box != NULL);
    int rc = pthread_create(&box->thread, NULL, entry, arg);
    if (rc) { free(box); return rc; }
    *out = (uintptr_t)box;
    return 0;
}
int x4_monitor_host_join(X4MonitorThread handle)
{
    HostThread *box = (HostThread *)handle;
    CHECK(box != NULL);
    CHECK(pthread_mutex_lock(&host.mutex) == 0);
    host.joins++;
    CHECK(pthread_cond_broadcast(&host.condition) == 0);
    CHECK(pthread_mutex_unlock(&host.mutex) == 0);
    int rc = pthread_join(box->thread, NULL);
    free(box);
    /* Report failure only after a real join: no host thread is abandoned.
     * The failed-return retention case runs in an isolated child process. */
    return rc ? rc : (host.fail_join ? EIO : 0);
}
static void clock_reset(void)
{
    CHECK(pthread_mutex_lock(&host.mutex) == 0);
    atomic_store(&host.now, 1000000);
    host.samples = host.sleeps = host.joins = host.forced_wakes = 0;
    host.last_target = host.last_actual = host.wake_at = 0;
    host.waiting = host.release_sleep = host.fail_create = host.fail_join = false;
    CHECK(pthread_mutex_unlock(&host.mutex) == 0);
}
static void await_sleep(void)
{
    CHECK(pthread_mutex_lock(&host.mutex) == 0);
    while (!host.waiting) wait_condition(&host.condition, &host.mutex);
    CHECK(pthread_mutex_unlock(&host.mutex) == 0);
}
static void advance_sample(uint64_t actual)
{
    CHECK(pthread_mutex_lock(&host.mutex) == 0);
    while (!host.waiting) wait_condition(&host.condition, &host.mutex);
    unsigned previous = host.samples;
    atomic_store(&host.now, actual);
    CHECK(pthread_cond_broadcast(&host.condition) == 0);
    while (host.samples == previous) wait_condition(&host.condition, &host.mutex);
    CHECK(host.samples == previous + 1);
    CHECK(pthread_mutex_unlock(&host.mutex) == 0);
    await_sleep(); /* An overdue target must not produce an endless catch-up. */
}
static int stop_result;
static void *stop_worker(void *context)
{ stop_result = x4_monitor_stop(context); return NULL; }
static int stop_monitor(X4Monitor *monitor)
{
    pthread_t stopper;
    CHECK(pthread_create(&stopper, NULL, stop_worker, monitor) == 0);
    CHECK(pthread_mutex_lock(&host.mutex) == 0);
    while (!host.joins) wait_condition(&host.condition, &host.mutex);
    host.release_sleep = true;
    CHECK(pthread_cond_broadcast(&host.condition) == 0);
    CHECK(pthread_mutex_unlock(&host.mutex) == 0);
    CHECK(pthread_join(stopper, NULL) == 0);
    return stop_result;
}

/* Test-only observer changes are quiescent metadata changes, not concurrent
 * payload reads/writes. They prove the second-witness rejection branch. */
static X4VideoIngress *mutate_queue;
static unsigned queue_mutation;
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    X4Monitor *monitor;
    bool entered, release;
} diagnostic_hold = { .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER };
void x4_monitor_test_hook(unsigned event, void *context, uint64_t a, uint64_t b)
{
    if (event == TEST_BETWEEN_QUEUE_WITNESSES && context == mutate_queue) {
        if (queue_mutation == 1) atomic_fetch_add(&mutate_queue->dequeue, 1);
        if (queue_mutation == 2) atomic_fetch_add(&mutate_queue->enqueue, 1);
        if (queue_mutation == 3) {
            X4IngressSlot *s = &mutate_queue->slots[a & (mutate_queue->capacity - 1)];
            atomic_store(&s->arrival_us, b + 1);
        }
        queue_mutation = 0;
    }
    if (event == TEST_AFTER_EVENT_GATE) {
        CHECK(pthread_mutex_lock(&diagnostic_hold.mutex) == 0);
        if (context == diagnostic_hold.monitor) {
            diagnostic_hold.entered = true;
            CHECK(pthread_cond_broadcast(&diagnostic_hold.condition) == 0);
            while (!diagnostic_hold.release)
                wait_condition(&diagnostic_hold.condition, &diagnostic_hold.mutex);
        }
        CHECK(pthread_mutex_unlock(&diagnostic_hold.mutex) == 0);
    }
    if (event == TEST_AFTER_SAMPLE) {
        CHECK(pthread_mutex_lock(&host.mutex) == 0);
        host.last_target = a; host.last_actual = b; host.samples++;
        CHECK(pthread_cond_broadcast(&host.condition) == 0);
        CHECK(pthread_mutex_unlock(&host.mutex) == 0);
    }
}

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    X4VideoIngress *queue;
    bool entered, release;
} ingress_hold = { .mutex = PTHREAD_MUTEX_INITIALIZER,
    .condition = PTHREAD_COND_INITIALIZER };
void x4_ingress_test_hook(X4VideoIngress *q, unsigned event, uint64_t position)
{
    (void)position;
    CHECK(pthread_mutex_lock(&ingress_hold.mutex) == 0);
    if (q == ingress_hold.queue && event == X4_INGRESS_AFTER_RESERVE) {
        ingress_hold.entered = true;
        CHECK(pthread_cond_broadcast(&ingress_hold.condition) == 0);
        while (!ingress_hold.release)
            wait_condition(&ingress_hold.condition, &ingress_hold.mutex);
    }
    CHECK(pthread_mutex_unlock(&ingress_hold.mutex) == 0);
}

typedef struct { uint8_t *data; size_t size, critical_offset; uint64_t h[128], ch[32]; } Dump;
static uint64_t le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned n = 0; n < 8; ++n) value |= (uint64_t)p[n] << (8 * n);
    return value;
}
static uint64_t word(const Dump *d, size_t offset)
{ CHECK(offset <= d->size && d->size - offset >= 8); return le64(d->data + offset); }
enum { CONFIG_OFFSET = 1024, ATTEMPT_OFFSET = 1152, DROP_OFFSET = 5248,
    RESET_OFFSET = 9344, HIST_OFFSET = 9856, EPOCH_OFFSET = 10176,
    RX_OFFSET = 12224, SAMPLE_OFFSET = 14272 };
static unsigned event_index(unsigned event)
{
    unsigned stage = event >> 8, item = event & 255;
    return stage < 8 && item < 64 ? stage * 64 + item : 63;
}
/* Optional copies of the actual C writer's synthetic files for an independent
 * Python-reader check. The caller supplies an existing private directory.
 * Each campaign modality uses a separate directory and never overwrites. */
static void retain_fixture(const Dump *d)
{
    const char *directory = getenv("X4_MONITOR_EXPORT_FIXTURES");
    if (!directory || !*directory) return;
    static unsigned ordinal;
    size_t length = strlen(directory);
    CHECK(length <= 4096);
    size_t capacity = length + 64;
    char *path = malloc(capacity); CHECK(path != NULL);
    int printed = snprintf(path, capacity, "%s/synthetic-%03u.progress.bin",
        directory, ++ordinal);
    CHECK(printed > 0 && (size_t)printed < capacity);
    FILE *file = fopen(path, "wbx"); CHECK(file != NULL);
    CHECK(fwrite(d->data, 1, d->size, file) == d->size);
    CHECK(fclose(file) == 0);
    printf("FIXTURE synthetic-%03u.progress.bin bytes=%zu\n", ordinal, d->size);
    free(path);
}
static Dump export_dump(X4Monitor *m)
{
    char dir[] = "/tmp/xcloud4-progress-host.XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char base[256], path[280];
    CHECK(snprintf(base, sizeof(base), "%s/trace", dir) > 0);
    CHECK(snprintf(path, sizeof(path), "%s.progress.bin", base) > 0);
    CHECK(x4_monitor_dump(m, base) == 0);
    FILE *f = fopen(path, "rb"); CHECK(f != NULL);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long length = ftell(f); CHECK(length >= SAMPLE_OFFSET && length < 13 * 1024 * 1024);
    CHECK(fseek(f, 0, SEEK_SET) == 0);
    Dump d = { .size = (size_t)length, .data = malloc((size_t)length) };
    CHECK(d.data != NULL && fread(d.data, 1, d.size, f) == d.size);
    CHECK(fclose(f) == 0);
    CHECK(memcmp(d.data, "X4PROG2\0", 8) == 0);
    for (unsigned i = 0; i < 128; ++i) d.h[i] = word(&d, i * 8);
    CHECK(d.h[1] == 2 && d.h[2] == 1024);
    CHECK(d.h[10] <= X4_MON_SAMPLE_CAP && d.h[12] <= X4_MON_CADENCE_CAP);
    CHECK(d.h[14] <= X4_MON_HISTORY_CAP && d.h[16] <= X4_MON_WINDOW_CAP);
    CHECK(d.h[34] == X4_MON_SAMPLE_CAP && d.h[43] == sizeof(X4MonitorSample));
    CHECK(d.h[44] == sizeof(X4MonitorCadence) && d.h[45] == sizeof(X4MonitorRecord));
    CHECK(d.h[46] == X4_MON_ACTORS && d.h[47] == X4_MON_RX_KINDS);
    CHECK(d.h[89] <= 64 && d.h[91] == 32);
    size_t cursor = SAMPLE_OFFSET + d.h[10] * sizeof(X4MonitorSample) + d.h[12] * 32 + d.h[14] * 32;
    CHECK(cursor <= d.size);
    for (uint64_t n = 0; n < d.h[16]; ++n) {
        uint64_t records = word(&d, cursor + 8);
        CHECK(records <= X4_MON_WINDOW_RECORD_CAP);
        cursor += 128 + records * 32;
        CHECK(cursor <= d.size);
    }
    d.critical_offset=cursor;
    CHECK(cursor+256<=d.size && memcmp(d.data+cursor,"X4CRIT2\0",8)==0);
    for(unsigned i=0;i<32;++i)d.ch[i]=word(&d,cursor+i*8);
    CHECK(d.ch[1]==2 && d.ch[2]==256 && d.ch[3]==64 && d.ch[4]==4096 && d.ch[5]<=4096);
    CHECK(d.ch[9]==16 && d.ch[10]==16 && d.ch[11]==24 && d.ch[12]==32);
    CHECK(d.ch[16]==32 && d.ch[17]==16 && d.ch[24]==16 && d.ch[26]==8);
    cursor+=256+2*32*8+16*16*8+16*32*8+16*8+8*8+16*16*24*8+d.ch[5]*64;
    CHECK(cursor == d.size); /* No undocumented trailing data. */
    CHECK(x4_monitor_dump(m, base) != 0); /* Existing export must survive. */
    f = fopen(path, "rb"); CHECK(f != NULL);
    uint8_t *again = malloc(d.size); CHECK(again != NULL);
    CHECK(fread(again, 1, d.size, f) == d.size && fgetc(f) == EOF);
    CHECK(fclose(f) == 0 && memcmp(again, d.data, d.size) == 0);
    free(again);
    retain_fixture(&d);
    CHECK(unlink(path) == 0 && rmdir(dir) == 0);
    return d;
}
static uint64_t sample_word(const Dump *d, unsigned sample, unsigned field)
{
    CHECK(sample < d->h[10] && field < X4_MON_SAMPLE_WORDS);
    return word(d, SAMPLE_OFFSET + (size_t)sample * sizeof(X4MonitorSample) + field * 8);
}
static void finish_unstarted(X4Monitor *m)
{
    CHECK(x4_monitor_stop(m) == 0);
    CHECK(x4_monitor_stopped(m));
    x4_monitor_end(m);
}
static void dispose_queue(X4VideoIngress *q)
{ x4_video_ingress_stop(q); CHECK(x4_video_ingress_free(q)); }

static void test_queue_observer(void)
{
    X4MonitorQueue o;
    x4_monitor_observe_queue(NULL, 1000000, &o);
    CHECK(o.flags & X4_MON_Q_UNBOUND);
    X4VideoIngress q; CHECK(x4_video_ingress_init(&q, 256) == 0);
    x4_monitor_observe_queue(&q, 1000000, &o);
    CHECK(o.depth == 0 && (o.flags & (X4_MON_Q_VALID | X4_MON_Q_EMPTY)) ==
        (X4_MON_Q_VALID | X4_MON_Q_EMPTY));
    uint8_t packet[16] = { 0 }; X4IngressResult r;
    CHECK(x4_video_ingress_push(&q, packet, sizeof(packet), 900000, &r));
    x4_monitor_observe_queue(&q, 1000000, &o);
    CHECK(o.depth == 1 && o.oldest_us == 900000 && o.flags == X4_MON_Q_VALID);
    atomic_store(&q.slots[0].arrival_us, 1000001);
    x4_monitor_observe_queue(&q, 1000000, &o);
    CHECK(o.flags & X4_MON_Q_CLOCK);
    atomic_store(&q.slots[0].arrival_us, 900000);
    for (unsigned change = 1; change <= 3; ++change) {
        atomic_store(&q.dequeue, 0); atomic_store(&q.enqueue, 1);
        atomic_store(&q.slots[0].ready, true);
        atomic_store(&q.slots[0].arrival_us, 900000);
        mutate_queue = &q; queue_mutation = change;
        x4_monitor_observe_queue(&q, 1000000, &o);
        CHECK(queue_mutation == 0 && (o.flags & X4_MON_Q_UNSTABLE));
    }
    mutate_queue = NULL;
    atomic_store(&q.dequeue, 0); atomic_store(&q.enqueue, 257);
    x4_monitor_observe_queue(&q, 1000000, &o);
    CHECK(o.flags & X4_MON_Q_RANGE);
    uint64_t near_wrap = UINT64_MAX - 255;
    atomic_store(&q.dequeue, near_wrap); atomic_store(&q.enqueue, 0);
    atomic_store(&q.slots[0].arrival_us, 900000);
    x4_monitor_observe_queue(&q, 1000000, &o);
    CHECK(o.head == near_wrap && o.tail == 0 && o.depth == 256 &&
        o.oldest_us == 900000 && o.flags == X4_MON_Q_VALID);
    dispose_queue(&q);
    puts("PASS atomic queue witness flags, metadata changes and full-width wrap");
}
typedef struct { X4VideoIngress *q; bool result; } PendingPush;
static void *pending_push(void *context)
{
    PendingPush *j = context; uint8_t p[16] = { 0 }; X4IngressResult r;
    j->result = x4_video_ingress_push(j->q, p, sizeof(p), 900000, &r);
    return NULL;
}
static void test_unpublished_observer(void)
{
    X4VideoIngress q; CHECK(x4_video_ingress_init(&q, 256) == 0);
    CHECK(pthread_mutex_lock(&ingress_hold.mutex) == 0);
    ingress_hold.queue = &q; ingress_hold.entered = ingress_hold.release = false;
    CHECK(pthread_mutex_unlock(&ingress_hold.mutex) == 0);
    PendingPush j = { .q = &q }; pthread_t t;
    CHECK(pthread_create(&t, NULL, pending_push, &j) == 0);
    CHECK(pthread_mutex_lock(&ingress_hold.mutex) == 0);
    while (!ingress_hold.entered) wait_condition(&ingress_hold.condition, &ingress_hold.mutex);
    CHECK(pthread_mutex_unlock(&ingress_hold.mutex) == 0);
    X4MonitorQueue o; x4_monitor_observe_queue(&q, 1000000, &o);
    CHECK(o.depth == 1 && (o.flags & X4_MON_Q_UNPUBLISHED) &&
        !(o.flags & X4_MON_Q_EMPTY) && o.oldest_us == 0);
    CHECK(pthread_mutex_lock(&ingress_hold.mutex) == 0);
    ingress_hold.release = true;
    CHECK(pthread_cond_broadcast(&ingress_hold.condition) == 0);
    CHECK(pthread_mutex_unlock(&ingress_hold.mutex) == 0);
    CHECK(pthread_join(t, NULL) == 0 && j.result);
    ingress_hold.queue = NULL;
    x4_monitor_observe_queue(&q, 1000000, &o);
    CHECK(o.depth == 1 && o.oldest_us == 900000 && o.flags == X4_MON_Q_VALID);
    dispose_queue(&q);
    puts("PASS sampler observer distinguishes a real unpublished reservation");
}

static void test_independent_sampling(void)
{
    clock_reset(); X4VideoIngress q;
    CHECK(x4_video_ingress_init(&q, 256) == 0);
    X4Monitor *m = x4_monitor_create(1, 4); CHECK(m != NULL);
    CHECK(x4_monitor_bind_queue(m, &q));
    X4MonitorConfig config = { .width = 960, .height = 540, .max_fps = 30,
        .bitrate_kbps = 5000, .readers = 4, .budget_us = 16000, .decode_limit = 4,
        .queue_capacity = 256, .reorder_capacity = 128, .reorder_depth = 32,
        .reorder_wait_us = 25000, .h264_profile = 0x42e01f,
        .max_fs = 3600, .max_mbps = 108000 };
    CHECK(x4_monitor_config(m, &config));
    x4_monitor_state(m, X4_MON_ACTOR_WORKER, X4_MON_PHASE_DECODE, 1000000);
    x4_monitor_epoch(m, X4_MON_ACTOR_SESSION, X4_MON_EPOCH_STABLE);
    x4_monitor_epoch(m, X4_MON_ACTOR_SESSION, X4_MON_EPOCH_STABLE); /* no-op */
    /* Synthetic observations only: independent IDs do not imply an AU/output
     * association. Sequence wrap is a local observation, never network loss. */
    x4_monitor_rx(m, X4_MON_RX_VIDEO, true, 42, UINT16_MAX, 64, 1000000);
    x4_monitor_rx(m, X4_MON_RX_VIDEO, true, 42, 0, 65, 1000000);
    x4_monitor_rx(m, X4_MON_RX_AUDIO, true, 43, 1, 128, 1000000);
    x4_monitor_record(m, 1000000, X4_TRACE_AU_VALID, 0, 1001, 128);
    x4_monitor_record(m, 1000000, X4_TRACE_DECODE_END, 0, 1002, 0);
    x4_monitor_record(m, 1000000, X4_TRACE_OUTPUT_VALID, 0, 2001, 1);
    x4_monitor_record(m, 1000000, X4_TRACE_RGB_PUBLICATION, 0, 3001, 2001);
    x4_monitor_record(m, 1000000, X4_TRACE_COPY_END, 0, 2001, 0);
    x4_monitor_record(m, 1000000, X4_TRACE_CONVERT_END, 0, 2001, 0);
    x4_monitor_active(m, true, 1000000);
    x4_monitor_present(m, 1, true, 1000000);
    CHECK(x4_monitor_start(m) == 0); await_sleep();
    advance_sample(1100000);
    /* No worker calls occur between samples. A late wake must still observe
     * the same phase and expose scheduling delay, not invent stage progress. */
    advance_sample(1450000);
    advance_sample(1500000);
    CHECK(stop_monitor(m) == 0 && x4_monitor_stopped(m));
    x4_monitor_end(m); Dump d = export_dump(m);
    CHECK(d.h[10] == 3 && d.h[22] == 1 && d.h[27] == 0);
    CHECK(d.h[68] == 1 && d.h[89] == 1);
    CHECK(d.h[48] == 2 && d.h[49] == 2 && d.h[50] == 129 && d.h[53] == 1);
    CHECK(sample_word(&d, 0, 0) == 1100000 && sample_word(&d, 0, 1) == 1100000);
    CHECK(sample_word(&d, 1, 1) == 1450000 && sample_word(&d, 1, 2) > 0);
    CHECK(d.h[20] > 0 && d.h[21] >= 250000);
    CHECK(sample_word(&d, 2, 0) > sample_word(&d, 1, 0));
    for (unsigned n = 0; n < 3; ++n) {
        CHECK(sample_word(&d, n, 32) == X4_MON_PHASE_DECODE);
        CHECK(sample_word(&d, n, 34) == 1000000);
        CHECK(sample_word(&d, n, 6) & (1u << (X4_MON_S_ACTOR_SHIFT + X4_MON_ACTOR_WORKER)));
        CHECK(sample_word(&d, n, 9) & X4_MON_Q_EMPTY);
        CHECK(sample_word(&d, n, 18) == 1 && sample_word(&d, n, 19) == 1);
        CHECK(sample_word(&d, n, 15) == UINT64_C(0x10000));
        CHECK(sample_word(&d, n, 31) == (UINT64_C(1) << 32 | 42));
        CHECK(sample_word(&d, n, 6) & X4_MON_S_RX_STABLE);
        CHECK(sample_word(&d, n, 6) & X4_MON_S_HISTORY_STABLE);
        CHECK(sample_word(&d, n, 5) != UINT64_MAX);
        CHECK(sample_word(&d, n, 30) == sample_word(&d, n, 1));
        for (unsigned field = 73; field <= 78; ++field)
            CHECK(sample_word(&d, n, field) == 1000000);
    }
    CHECK(word(&d, CONFIG_OFFSET) == 960 && word(&d, CONFIG_OFFSET + 8) == 540);
    free(d.data); CHECK(x4_monitor_free(m)); dispose_queue(&q);
    puts("PASS independent 100ms sampling, frozen worker and delayed target/actual clock");
}

typedef struct { X4Monitor *m; unsigned writer; } Writer;
static void *write_events(void *context)
{
    Writer *w = context;
    for (unsigned n = 0; n < WRITER_CALLS; ++n) {
        uint64_t now = 1000000 + n;
        x4_monitor_record(w->m, now, (uint16_t)(0x230 + w->writer), 0,
            w->writer, n);
        x4_monitor_rx(w->m, X4_MON_RX_VIDEO, true, 100 + w->writer,
            (uint16_t)n, 64, now);
        /* Same transport source has concurrent observers: sequence trygate
         * omissions/ambiguity remain explicit and no writer can wait. */
        x4_monitor_transport_packet(w->m,X4_MON_T_APP_CALLBACK,true,777,
            (uint16_t)n,102,64,now,0,2);
        if(n%97==0)x4_monitor_critical(w->m,now,X4_MON_CR_REJECT,
            X4_MON_T_SRTP_INPUT,0,w->writer,777,w->writer,n,7);
        /* Each tested actor has exactly one owning writer. */
        x4_monitor_state(w->m, X4_MON_ACTOR_HELPER0 + w->writer,
            X4_MON_PHASE_COPY, now);
    }
    return NULL;
}
static void test_concurrent_counters(void)
{
    clock_reset(); X4Monitor *m = x4_monitor_create(2, 4); CHECK(m != NULL);
    Writer jobs[WRITERS]; pthread_t threads[WRITERS];
    for (unsigned i = 0; i < WRITERS; ++i) {
        jobs[i] = (Writer){ .m = m, .writer = i };
        CHECK(pthread_create(&threads[i], NULL, write_events, &jobs[i]) == 0);
    }
    for (unsigned i = 0; i < WRITERS; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    atomic_store(&host.now, 1000000 + WRITER_CALLS - 1);
    finish_unstarted(m); Dump d = export_dump(m);
    CHECK(d.h[48] == WRITERS * WRITER_CALLS && d.h[49] == d.h[48]);
    CHECK(d.h[50] == (uint64_t)WRITERS * WRITER_CALLS * 64);
    uint64_t attempts = 0, drops = 0;
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t a = word(&d, ATTEMPT_OFFSET + i * 8);
        uint64_t b = word(&d, DROP_OFFSET + i * 8);
        CHECK(b <= a); attempts += a; drops += b;
    }
    CHECK(attempts == d.h[18] && drops == d.h[19]);
    size_t transport=d.critical_offset+256+2*32*8+X4_MON_T_APP_CALLBACK*16*8;
    CHECK(word(&d,transport)==WRITERS*WRITER_CALLS);
    CHECK(word(&d,transport+8)==(uint64_t)WRITERS*WRITER_CALLS*64);
    CHECK(word(&d,d.critical_offset+256+X4_MON_CR_REJECT*8)==
        WRITERS*((WRITER_CALLS-1)/97+1));
    CHECK(d.ch[6]==d.ch[5]+d.ch[7]+d.ch[8]);
    for (unsigned i = 0; i < WRITERS; ++i)
        CHECK(word(&d, ATTEMPT_OFFSET + event_index(0x230 + i) * 8) == WRITER_CALLS);
    size_t history = SAMPLE_OFFSET + d.h[10] * sizeof(X4MonitorSample) + d.h[12] * 32;
    for (uint64_t i = 0; i < d.h[14]; ++i) {
        const uint8_t *p = d.data + history + i * 32;
        unsigned event = p[28] | ((unsigned)p[29] << 8);
        if (event >= 0x230 && event < 0x230 + WRITERS) {
            CHECK(le64(p + 8) == event - 0x230 && le64(p + 16) < WRITER_CALLS);
            CHECK(le64(p) == 1000000 + le64(p + 16));
        }
    }
    free(d.data); CHECK(x4_monitor_free(m));
    puts("PASS concurrent numeric event/RX counts and admitted record integrity");
}

static void *hold_record(void *context)
{
    x4_monitor_record(context, 1000000, 0x230, 0, 123, 456);
    return NULL;
}
static void test_diagnostic_admission_drop(void)
{
    clock_reset(); X4Monitor *m = x4_monitor_create(7, 4); CHECK(m != NULL);
    CHECK(x4_monitor_start(m) == 0); await_sleep();
    CHECK(pthread_mutex_lock(&diagnostic_hold.mutex) == 0);
    diagnostic_hold.monitor = m;
    diagnostic_hold.entered = diagnostic_hold.release = false;
    CHECK(pthread_mutex_unlock(&diagnostic_hold.mutex) == 0);
    pthread_t t; CHECK(pthread_create(&t, NULL, hold_record, m) == 0);
    CHECK(pthread_mutex_lock(&diagnostic_hold.mutex) == 0);
    while (!diagnostic_hold.entered)
        wait_condition(&diagnostic_hold.condition, &diagnostic_hold.mutex);
    CHECK(pthread_mutex_unlock(&diagnostic_hold.mutex) == 0);
    /* This returns while a different producer holds the diagnostic gate.
     * Its event is dropped, but the attempt/reset counter is preserved. */
    x4_monitor_record(m, 1000001, X4_TRACE_AU_RESET, 0, 9, UINT64_C(7) << 48);
    advance_sample(1100000); /* Sampler cannot acquire held diagnostic history. */
    CHECK(pthread_mutex_lock(&diagnostic_hold.mutex) == 0);
    diagnostic_hold.release = true;
    CHECK(pthread_cond_broadcast(&diagnostic_hold.condition) == 0);
    CHECK(pthread_mutex_unlock(&diagnostic_hold.mutex) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    diagnostic_hold.monitor = NULL;
    CHECK(stop_monitor(m) == 0); x4_monitor_end(m); Dump d = export_dump(m);
    CHECK(d.h[19] == 1 && (d.h[33] & 16u));
    CHECK(d.h[10] == 1 && d.h[86] == 1 && (d.h[33] & 32u));
    CHECK(sample_word(&d, 0, 5) == UINT64_MAX);
    CHECK(!(sample_word(&d, 0, 6) & X4_MON_S_HISTORY_STABLE));
    CHECK(word(&d, ATTEMPT_OFFSET + event_index(X4_TRACE_AU_RESET) * 8) == 1);
    CHECK(word(&d, DROP_OFFSET + event_index(X4_TRACE_AU_RESET) * 8) == 1);
    CHECK(word(&d, RESET_OFFSET + 7 * 8) == 1);
    free(d.data); CHECK(x4_monitor_free(m));
    puts("PASS try-only diagnostic admission drop preserves cumulative reasons");
}

static void test_cadence_caps_and_reset_histogram(void)
{
    clock_reset(); X4Monitor *m = x4_monitor_create(3, 4); CHECK(m != NULL);
    x4_monitor_active(m, true, 1000000);
    uint64_t now = 1000000;
    for (unsigned n = 0; n < X4_MON_CADENCE_CAP + 6; ++n) {
        atomic_store(&host.now, now);
        x4_monitor_present(m, (uint64_t)n + 1, true, now);
        x4_monitor_record(m, now, X4_TRACE_PRESENT_NEW, X4_TRACE_F_NEW,
            (uint64_t)n + 1, 0);
        x4_monitor_present(m, (uint64_t)n + 1, false, now + 1);
        now += 40000;
    }
    atomic_store(&host.now, now);
    for (unsigned i = 0; i < 65; ++i) {
        x4_monitor_record(m, now, X4_TRACE_AU_RESET, 0, i, (uint64_t)i << 48);
        x4_monitor_record(m, now, X4_TRACE_AU_RESET, 3, i, (uint64_t)i << 48);
    }
    finish_unstarted(m); Dump d = export_dump(m);
    CHECK(d.h[22] == X4_MON_CADENCE_CAP + 6 && d.h[27] == d.h[22] - 1);
    CHECK(d.h[12] == X4_MON_CADENCE_CAP && d.h[13] == 5);
    CHECK(d.h[33] & 2u); /* cadence cap */
    CHECK(d.h[15] > 0 && (d.h[33] & 4u)); /* bounded overwritten prehistory */
    uint64_t histogram_count = 0;
    for (unsigned i = 0; i < 32; ++i) histogram_count += word(&d, HIST_OFFSET + i * 8);
    CHECK(histogram_count == d.h[27] && word(&d, HIST_OFFSET + 15 * 8) == d.h[27]);
    for (unsigned i = 0; i < 31; ++i) {
        CHECK(word(&d, RESET_OFFSET + i * 8) == 2);
        CHECK(word(&d, RESET_OFFSET + (32 + i) * 8) == 1);
    }
    CHECK(word(&d, RESET_OFFSET + 31 * 8) == 68);
    CHECK(word(&d, RESET_OFFSET + 63 * 8) == 34);
    CHECK(word(&d, ATTEMPT_OFFSET + event_index(X4_TRACE_AU_RESET) * 8) == 130);
    free(d.data); CHECK(x4_monitor_free(m));
    puts("PASS cumulative cadence histogram/reset reasons beyond detail capacity");
}

static void test_sample_cap(void)
{
    clock_reset(); X4Monitor *m = x4_monitor_create(4, 4); CHECK(m != NULL);
    CHECK(x4_monitor_start(m) == 0); await_sleep();
    for (unsigned n = 1; n <= X4_MON_SAMPLE_CAP + 3; ++n)
        advance_sample(1000000 + (uint64_t)n * X4_MON_PERIOD_US);
    CHECK(stop_monitor(m) == 0); x4_monitor_end(m);
    Dump d = export_dump(m);
    CHECK(d.h[10] == X4_MON_SAMPLE_CAP && d.h[11] == 3 && (d.h[33] & 1u));
    CHECK(d.h[20] == 0 && d.h[21] == 0);
    CHECK(sample_word(&d, 0, 1) == 1000000 + UINT64_C(4) * X4_MON_PERIOD_US);
    CHECK(sample_word(&d, X4_MON_SAMPLE_CAP - 1, 1) ==
        1000000 + (uint64_t)(X4_MON_SAMPLE_CAP + 3) * X4_MON_PERIOD_US);
    free(d.data); CHECK(x4_monitor_free(m));
    puts("PASS bounded sampler detail capacity with explicit omitted count");
}

static void test_sampler_clock_backwards(void)
{
    clock_reset(); X4Monitor *m = x4_monitor_create(9, 4); CHECK(m != NULL);
    CHECK(x4_monitor_start(m) == 0); await_sleep();
    CHECK(pthread_mutex_lock(&host.mutex) == 0);
    unsigned old_sleeps = host.sleeps;
    atomic_store(&host.now, 900000); ++host.forced_wakes;
    CHECK(pthread_cond_broadcast(&host.condition) == 0);
    while (host.sleeps == old_sleeps) wait_condition(&host.condition, &host.mutex);
    CHECK(host.samples == 0);
    CHECK(pthread_mutex_unlock(&host.mutex) == 0);
    advance_sample(1000000);
    CHECK(stop_monitor(m) == 0); x4_monitor_end(m); Dump d = export_dump(m);
    CHECK(d.h[10] == 1 && d.h[20] == 0 && d.h[21] == 0);
    CHECK(d.h[88] == 1 && d.h[29] >= 1 && (d.h[33] & 2048u));
    CHECK(sample_word(&d, 0, 0) == 1000000 && sample_word(&d, 0, 1) == 1000000);
    free(d.data); CHECK(x4_monitor_free(m));
    puts("PASS backward sampler clock reanchors without fabricated missed samples");
}

static void test_epoch_ledger(void)
{
    clock_reset(); X4Monitor *m = x4_monitor_create(10, 4); CHECK(m != NULL);
    uint64_t now = 1000000;
    x4_monitor_active(m, true, now);
    x4_monitor_epoch(m, X4_MON_ACTOR_SESSION, X4_MON_EPOCH_STABLE);
    x4_monitor_present(m, 1, true, now);
    now += 10000; atomic_store(&host.now, now);
    x4_monitor_epoch(m, X4_MON_ACTOR_SESSION, X4_MON_EPOCH_TRANSITION);
    now += 10000; atomic_store(&host.now, now);
    x4_monitor_epoch(m, X4_MON_ACTOR_SESSION, X4_MON_EPOCH_STABLE);
    now += 10000; atomic_store(&host.now, now);
    x4_monitor_present(m, 2, true, now);
    /* Identical endpoint epochs hide a transition unless its revision is kept. */
    for (unsigned n = 0; n < X4_MON_EPOCH_CAP + 3; ++n) {
        now += 1000; atomic_store(&host.now, now);
        unsigned epoch = n & 1 ? X4_MON_EPOCH_STABLE : X4_MON_EPOCH_TRANSITION;
        x4_monitor_epoch(m, X4_MON_ACTOR_SESSION, epoch);
    }
    finish_unstarted(m); Dump d = export_dump(m);
    CHECK(d.h[89] == X4_MON_EPOCH_CAP && d.h[90] > 0);
    CHECK(d.h[68] == d.h[89] + d.h[90]);
    CHECK(word(&d, EPOCH_OFFSET) == 1000000 && word(&d, EPOCH_OFFSET + 16) == X4_MON_EPOCH_STABLE);
    CHECK(d.h[12] == 1);
    size_t cadence = SAMPLE_OFFSET + d.h[10] * sizeof(X4MonitorSample);
    CHECK(word(&d, cadence + 24) & (UINT64_C(1) << 63));
    free(d.data); CHECK(x4_monitor_free(m));
    puts("PASS manual epoch ledger cap and mixed-transition cadence qualification");
}

static void test_window_caps(void)
{
    clock_reset(); X4Monitor *m = x4_monitor_create(8, 4); CHECK(m != NULL);
    uint64_t now = 1000000;
    x4_monitor_active(m, true, now);
    x4_monitor_present(m, 1, true, now);
    for (unsigned n = 0; n < X4_MON_WINDOW_CAP + 3; ++n) {
        now += 200001; atomic_store(&host.now, now);
        x4_monitor_present(m, (uint64_t)n + 2, true, now);
        for (unsigned i = 0; i < X4_MON_WINDOW_RECORD_CAP + 1; ++i)
            x4_monitor_record(m, now, 0x230, 0, n, i);
    }
    finish_unstarted(m); Dump d = export_dump(m);
    CHECK(d.h[25] == X4_MON_WINDOW_CAP + 3);
    CHECK(d.h[16] == X4_MON_WINDOW_CAP && d.h[17] > 0);
    CHECK((d.h[33] & (8u | 4096u)) == (8u | 4096u));
    CHECK(d.h[70] <= d.h[25]);
    free(d.data); CHECK(x4_monitor_free(m));
    puts("PASS bounded gap windows/event caps leave exact cadence counters intact");
}

static void test_second_gap_during_post_window(void)
{
    clock_reset(); X4Monitor *m = x4_monitor_create(11, 4); CHECK(m != NULL);
    x4_monitor_active(m, true, 1000000);
    x4_monitor_present(m, 1, true, 1000000);
    CHECK(x4_monitor_start(m) == 0); await_sleep();
    advance_sample(1200000); /* First gap observed while independently sampling. */
    atomic_store(&host.now, 1250000);
    x4_monitor_present(m, 2, true, 1250000); /* Old POST deadline is 1750000. */
    advance_sample(1500000); /* Second gap starts before that old deadline. */
    advance_sample(1800000); /* Still open, after the obsolete deadline. */
    CHECK(stop_monitor(m) == 0); x4_monitor_end(m);
    Dump d = export_dump(m);
    CHECK(d.h[25] == 1 && d.h[16] == 2 && d.h[69] == 1);
    size_t window = SAMPLE_OFFSET + d.h[10] * sizeof(X4MonitorSample) +
        d.h[12] * sizeof(X4MonitorCadence) + d.h[14] * sizeof(X4MonitorRecord);
    CHECK(word(&d,window+3*8)==2); /* First pause retained once. */
    window+=128+word(&d,window+8)*32;
    CHECK(word(&d,window+3*8)==3); /* Separate second-pause identity. */
    CHECK(word(&d, window + 6 * 8) == 0); /* No completed second gap. */
    CHECK(word(&d, window + 8 * 8) == 4); /* Frozen by stop, not old POST. */
    CHECK(word(&d, window + 12 * 8) == 3);
    CHECK(word(&d, window + 13 * 8) == 0); /* Only first gap actually closed. */
    CHECK(word(&d, window + 2 * 8) & 8192u);
    free(d.data); CHECK(x4_monitor_free(m));
    puts("PASS each pause has one context and an ongoing second pause remains open");
}

static size_t critical_records_offset(const Dump*d)
{return d->critical_offset+256+2*32*8+16*16*8+16*32*8+16*8+8*8+16*16*24*8;}
static void test_transport_wrap_and_late(void)
{
    clock_reset();X4Monitor*m=x4_monitor_create(12,4);CHECK(m!=NULL);
    unsigned stage=X4_MON_T_SRTP_VALID;
    x4_monitor_transport_packet(m,stage,true,42,65534,102,1200,1000000,999000,2);
    x4_monitor_transport_packet(m,stage,true,42,2,102,1200,1000001,0,2);
    x4_monitor_transport_packet(m,stage,true,42,0,102,1200,1000002,0,2);
    x4_monitor_transport_packet(m,stage,true,42,0,102,1200,1000003,0,2);
    x4_monitor_transport_packet(m,stage,true,42,3,102,1200,1000004,0,2);
    x4_monitor_transport_packet(m,stage,true,42,3,102,1200,1000005,0,2);
    /* RTX on another payload type is an independent sequence namespace. */
    x4_monitor_transport_packet(m,stage,true,42,999,104,1200,1000006,0,4);
    x4_monitor_transport_packet(m,stage,true,42,32771,102,1200,1000007,0,2);
    x4_monitor_transport_packet(m,stage,true,42,32772,102,1200,1000008,0,2);
    x4_monitor_transport_reject(m,X4_MON_T_DATAGRAM,25,-11,false,0,0,1000010);
    x4_monitor_transport_reject(m,stage,7,7,true,42,4,1000011);
    finish_unstarted(m);Dump d=export_dump(m);
    size_t counters=d.critical_offset+256+2*32*8+stage*16*8;
    CHECK(word(&d,counters)==9 && word(&d,counters+5*8)==1 && word(&d,counters+6*8)==3);
    CHECK(word(&d,counters+7*8)==2 && word(&d,counters+8*8)==1);
    CHECK(word(&d,counters+12*8)==2 && word(&d,counters+13*8)==1);
    CHECK(d.ch[15]==1 && d.ch[5]==5); /* gap, late twice, ambiguous, auth reject */
    size_t records=critical_records_offset(&d);
    CHECK((word(&d,records+2*8)&0xffffu)==X4_MON_CR_SEQUENCE_GAP);
    CHECK(word(&d,records+4*8)==65535 && word(&d,records+5*8)==65538 && word(&d,records+6*8)==3);
    CHECK((word(&d,records+64+2*8)&0xffffu)==X4_MON_CR_LATE_POSITION);
    CHECK(word(&d,records+64+8)==word(&d,records+8));
    CHECK(word(&d,records+128+2*8)&(UINT64_C(1)<<62)); /* repeated late position */
    size_t sources=d.critical_offset+256+2*32*8+16*16*8+16*32*8+16*8+8*8+stage*16*24*8;
    CHECK(word(&d,sources+7*8)==3 && word(&d,sources+8*8)==1);
    CHECK((word(&d,sources+5*8)&0xffffu)==32772);
    CHECK(word(&d,sources+24*8)!=word(&d,sources));
    free(d.data);CHECK(x4_monitor_free(m));
    puts("PASS modulo sequence gaps, late reappearance, RTX namespace and counter-only drain");
}
static void test_critical_ring_and_au_separation(void)
{
    clock_reset();X4Monitor*m=x4_monitor_create(13,4);CHECK(m!=NULL);
    for(unsigned i=0;i<X4_MON_CR_CAP+17;++i)
        x4_monitor_critical(m,1000000+i,X4_MON_CR_REJECT,5,0,i,42,7,7,i);
    x4_monitor_record(m,2000000,X4_TRACE_AU_STRUCTURAL,2,100,1000);
    x4_monitor_record(m,2000000,X4_TRACE_AU_FILTER_REJECT,5,100,1000);
    x4_monitor_record(m,2000001,X4_TRACE_AU_STRUCTURAL,1,101,1000);
    x4_monitor_record(m,2000001,X4_TRACE_AU_VALID,2,101,1200);
    finish_unstarted(m);Dump d=export_dump(m);
    CHECK(d.ch[5]==4096 && d.ch[8]==18 && d.ch[23]==4114);
    CHECK(word(&d,ATTEMPT_OFFSET+event_index(X4_TRACE_AU_STRUCTURAL)*8)==2);
    CHECK(word(&d,ATTEMPT_OFFSET+event_index(X4_TRACE_AU_FILTER_REJECT)*8)==1);
    CHECK(word(&d,ATTEMPT_OFFSET+event_index(X4_TRACE_AU_VALID)*8)==1);
    size_t filter=d.critical_offset+256+2*32*8+16*16*8+16*32*8+16*8;
    CHECK(word(&d,filter+5*8)==1);
    size_t records=critical_records_offset(&d);
    CHECK(word(&d,records+7*8)==19);
    CHECK(word(&d,records+(4096-1)*64+7*8)==4114);
    free(d.data);CHECK(x4_monitor_free(m));
    puts("PASS independent bounded critical ring and structural/filter/Decode admission totals");
}
static void test_window_pre_reservation_and_duplicate(void)
{
    clock_reset();X4Monitor*m=x4_monitor_create(14,4);CHECK(m!=NULL);
    x4_monitor_active(m,true,1000000);x4_monitor_present(m,1,true,1000000);
    for(unsigned i=0;i<6000;++i)x4_monitor_record(m,1000000,0x230,0,i,0);
    CHECK(x4_monitor_start(m)==0);await_sleep();advance_sample(1200000);
    advance_sample(3400000); /* original window freezes at its time bound */
    atomic_store(&host.now,3500000);x4_monitor_present(m,2,true,3500000);
    CHECK(stop_monitor(m)==0);x4_monitor_end(m);Dump d=export_dump(m);
    CHECK(d.h[16]==1 && d.ch[21]==1); /* end never reopens the same pause */
    size_t window=SAMPLE_OFFSET+d.h[10]*640+d.h[12]*32+d.h[14]*32;
    CHECK(word(&d,window+11*8)<=512 && word(&d,window+11*8)>0);
    CHECK(word(&d,window+14*8)>0 && word(&d,window+8)<=4096);
    free(d.data);CHECK(x4_monitor_free(m));
    puts("PASS bounded prehistory reserves episode capacity and prevents duplicate windows");
}
static void test_disabled_and_lifetime(void)
{
    clock_reset();
    x4_monitor_record(NULL, 0, 0, 0, 0, 0);
    x4_monitor_rx(NULL, 0, false, 0, 0, 0, 0);
    x4_monitor_state(NULL, 0, 0, 0); x4_monitor_active(NULL, false, 0);
    x4_monitor_present(NULL, 0, false, 0); x4_monitor_end(NULL);
    CHECK(x4_monitor_free(NULL));
    X4Monitor *m = x4_monitor_create(5, 4); CHECK(m != NULL);
    CHECK(!x4_monitor_free(m));
    host.fail_create = true;
    CHECK(x4_monitor_start(m) == 1);
    CHECK(x4_monitor_stop(m) == 0 && x4_monitor_stopped(m));
    x4_monitor_end(m); Dump d = export_dump(m);
    CHECK(d.h[10] == 0 && (d.h[33] & 128u));
    free(d.data); CHECK(x4_monitor_free(m));

    /* Fork before starting the sampler. Failure is a reported join failure;
     * actual pthread retirement prevents an orphan and intentional retained
     * storage is scoped to this child, which terminates without leak checking. */
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        clock_reset(); X4Monitor *r = x4_monitor_create(6, 4); CHECK(r != NULL);
        host.fail_join = true;
        CHECK(x4_monitor_start(r) == 0); await_sleep();
        CHECK(stop_monitor(r) < 0 && !x4_monitor_stopped(r));
        x4_monitor_end(r);
        CHECK(!x4_monitor_free(r));
        CHECK(x4_monitor_dump(r, "/tmp/xcloud4-retained-forbidden") != 0);
        _exit(0);
    }
    int status; CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("PASS optional start disable, NULL paths and failed-join retained lifetime");
}

int main(void)
{
    signal(SIGALRM, alarm_expired); alarm(60);
    clock_reset();
    test_queue_observer();
    test_unpublished_observer();
    test_independent_sampling();
    test_concurrent_counters();
    test_diagnostic_admission_drop();
    test_cadence_caps_and_reset_histogram();
    test_sample_cap();
    test_sampler_clock_backwards();
    test_epoch_ledger();
    test_window_caps();
    test_second_gap_during_post_window();
    test_transport_wrap_and_late();
    test_critical_ring_and_au_separation();
    test_window_pre_reservation_and_duplicate();
    test_disabled_and_lifetime();
    alarm(0);
    puts("PASS progress-monitor host campaign; native scheduling/overhead unverified");
    return 0;
}
