/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "mic_capture.h"

#include <opus/opus.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef X4_OPENORBIS
#include <orbis/libkernel.h>
#include <orbis/_types/sysmodule.h>
#endif

#if X4_PUBLIC
#define MIC_LOG(...) ((void)0)
#else
#define MIC_LOG(...) printf(__VA_ARGS__)
#endif

enum {
    GRAIN = 256, RATE_HIGH = 48000, RATE_LOW = 16000, MAX_FRAME = 960,
    QUEUE_CAP = 4, AGE_LIMIT_US = 40000, WORKER_STACK = 256 * 1024,
    NO_DEVICE = 1, USER_MUTED = 4
};
enum { RETRY_US = 2000000 };

typedef struct {
    uint64_t generation, peer, epoch;
    int32_t user;
    unsigned frame_ms;
} Capture;

static struct {
#ifdef X4_OPENORBIS
    OrbisPthreadMutex lock;
    OrbisPthreadCond wake;
#else
    pthread_mutex_t lock;
    pthread_cond_t wake;
#endif
    Capture wanted;
    uint64_t next_generation, last_peer;
    bool muted, running;
    unsigned head, count;
    X4MicFrame frames[QUEUE_CAP];
} mic
#ifdef X4_OPENORBIS
;
#else
= {.lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};
#endif

/* All control writes are serialized by the short queue mutex. Native I/O,
 * encoding, allocation and logging are outside it. Readers in the main loop
 * need only the single packed status, and the sender can recheck after pop. */
static _Atomic uint64_t request_word, peer_word, capture_epoch, send_epoch;
static _Atomic unsigned status_word;

#ifdef X4_OPENORBIS
static pthread_once_t sync_once = PTHREAD_ONCE_INIT;
static atomic_bool sync_ready;
_Static_assert(sizeof(OrbisPthreadMutex) >= sizeof(void *) &&
               sizeof(OrbisPthreadCond) >= sizeof(void *), "Native pthread handles require pointer storage");

static void prepare_sync(void)
{
    int rc = scePthreadMutexInit(&mic.lock, NULL, "x4-mic");
    if (!rc) {
        rc = scePthreadCondInit(&mic.wake, NULL, "x4-mic");
        if (rc) (void)scePthreadMutexDestroy(&mic.lock);
    }
    atomic_store(&sync_ready, rc == 0);
    if (rc) MIC_LOG("XCloud4: mic sync rc=0x%08x\n", (unsigned)rc);
}

static bool sync_available(void) { return atomic_load(&sync_ready); }
#else
static bool sync_available(void) { return true; }
#endif

static void wipe(void *memory, size_t size)
{
    volatile uint8_t *p = memory;
    while (size--) *p++ = 0;
}

static void lock_mic(void)
{
#ifdef X4_OPENORBIS
    (void)scePthreadMutexLock(&mic.lock);
#else
    (void)pthread_mutex_lock(&mic.lock);
#endif
}

static void unlock_mic(void)
{
#ifdef X4_OPENORBIS
    (void)scePthreadMutexUnlock(&mic.lock);
#else
    (void)pthread_mutex_unlock(&mic.lock);
#endif
}

static void signal_mic(void)
{
#ifdef X4_OPENORBIS
    (void)scePthreadCondSignal(&mic.wake);
#else
    (void)pthread_cond_signal(&mic.wake);
#endif
}

static void wait_mic(void)
{
#ifdef X4_OPENORBIS
    (void)scePthreadCondWait(&mic.wake, &mic.lock);
#else
    (void)pthread_cond_wait(&mic.wake, &mic.lock);
#endif
}

static void timed_wait_mic(unsigned delay_us)
{
#ifdef X4_OPENORBIS
    (void)scePthreadCondTimedwait(&mic.wake, &mic.lock, delay_us);
#else
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline)) return;
    deadline.tv_sec += delay_us / 1000000;
    deadline.tv_nsec += (long)(delay_us % 1000000) * 1000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_nsec -= 1000000000;
        ++deadline.tv_sec;
    }
    (void)pthread_cond_timedwait(&mic.wake, &mic.lock, &deadline);
#endif
}

static uint64_t now_us(void)
{
#ifdef X4_OPENORBIS
    return sceKernelGetProcessTime();
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
#endif
}

#if !X4_PUBLIC
static uint64_t cpu_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
#endif

static void publish_locked(X4MicStatus status, unsigned rate, unsigned level)
{
    unsigned word = (unsigned)status | ((unsigned)mic.muted << 3) | (level << 4) |
                    ((rate == RATE_HIGH ? 2u : rate == RATE_LOW ? 1u : 0u) << 11) |
                    ((unsigned)(mic.wanted.frame_ms == 20) << 13);
    atomic_store(&status_word, word);
}

static void invalidate_locked(void)
{
    atomic_store(&send_epoch, 0);
    ++mic.wanted.epoch;
    atomic_store(&capture_epoch, mic.wanted.epoch);
    mic.head = mic.count = 0;
    wipe(mic.frames, sizeof(mic.frames));
}

static bool usable_user(int32_t user) { return user > 0 && user != 0xff; }

uint64_t x4_mic_session_begin(bool enabled, int32_t initial_user, unsigned frame_ms)
{
    if (!enabled) {
        x4_mic_session_end();
        return 0;
    }
#ifdef X4_OPENORBIS
    (void)pthread_once(&sync_once, prepare_sync);
    if (!sync_available()) {
        atomic_store(&status_word, X4_MIC_UNAVAILABLE);
        return 0;
    }
#endif
    lock_mic();
    invalidate_locked();
    mic.wanted.generation = ++mic.next_generation;
    mic.wanted.peer = 0;
    mic.last_peer = 0;
    mic.wanted.user = initial_user;
    mic.wanted.frame_ms = !X4_PUBLIC && frame_ms == 20 ? 20 : 10;
    mic.muted = false;
    uint64_t generation = mic.wanted.generation;
    bool usable = usable_user(initial_user);
    atomic_store(&peer_word, 0);
    atomic_store(&request_word, usable ? (generation << 1) | (mic.wanted.frame_ms == 20) : 0);
    publish_locked(usable ? X4_MIC_WAITING : X4_MIC_UNAVAILABLE, 0, 0);
    signal_mic();
    unlock_mic();
    return generation;
}

void x4_mic_session_end(void)
{
    if (!atomic_load(&status_word)) return;
    if (!sync_available()) {
        atomic_store(&status_word, X4_MIC_OFF);
        return;
    }
    lock_mic();
    invalidate_locked();
    mic.wanted.generation = mic.wanted.peer = 0;
    mic.wanted.user = 0;
    mic.wanted.frame_ms = 10;
    mic.muted = false;
    atomic_store(&peer_word, 0);
    atomic_store(&request_word, 0);
    publish_locked(X4_MIC_OFF, 0, 0);
    signal_mic();
    unlock_mic();
}

void x4_mic_toggle_mute(void)
{
    if (!sync_available()) return;
    lock_mic();
    if (mic.wanted.generation && usable_user(mic.wanted.user)) {
        X4MicStatus prior = (X4MicStatus)(atomic_load(&status_word) & 7u);
        mic.muted = !mic.muted;
        invalidate_locked();
        X4MicStatus next = prior == X4_MIC_REJECTED ? prior :
                           mic.muted ? X4_MIC_MUTED : X4_MIC_WAITING;
        publish_locked(next, 0, 0);
        signal_mic();
    }
    unlock_mic();
}

X4MicSnapshot x4_mic_snapshot(void)
{
    unsigned word = atomic_load(&status_word), rate = (word >> 11) & 3u;
    X4MicStatus status = (X4MicStatus)(word & 7u);
    return (X4MicSnapshot){.enabled = status != X4_MIC_OFF, .muted = (word & 8u) != 0,
        .active = status == X4_MIC_ACTIVE, .status = status, .level = (word >> 4) & 127u,
        .rate = rate == 2 ? RATE_HIGH : rate == 1 ? RATE_LOW : 0,
        .frame_ms = word & (1u << 13) ? 20 : 10};
}

X4MicRequest x4_mic_request(void)
{
    uint64_t word = atomic_load(&request_word);
    return (X4MicRequest){.generation = word >> 1, .frame_ms = word & 1u ? 20 : 10};
}

static bool current(Capture capture) { return atomic_load(&capture_epoch) == capture.epoch; }

static bool binding_locked(uint64_t generation, uint64_t peer)
{
    return generation && peer && mic.wanted.generation == generation && mic.wanted.peer == peer;
}

bool x4_mic_capture_send_allowed(uint64_t generation, uint64_t peer, uint64_t epoch)
{
    return generation && peer && epoch && x4_mic_request().generation == generation &&
           atomic_load(&peer_word) == peer && atomic_load(&send_epoch) == epoch;
}

bool x4_mic_capture_take(uint64_t generation, uint64_t peer, uint64_t now, X4MicFrame *frame)
{
    if (!frame || !generation || atomic_load(&send_epoch) == 0) return false;
    bool found = false;
    lock_mic();
    while (binding_locked(generation, peer) && !mic.muted && mic.count) {
        X4MicFrame *slot = &mic.frames[mic.head];
        if (now < slot->captured_us) break;
        found = now - slot->captured_us <= AGE_LIMIT_US;
        if (found) *frame = *slot;
        wipe(slot, sizeof(*slot));
        mic.head = (mic.head + 1) % QUEUE_CAP;
        --mic.count;
        if (found) break;
    }
    unlock_mic();
    return found;
}

static void publish_frame(Capture capture, unsigned rate, unsigned level, const X4MicFrame *frame)
{
    lock_mic();
    if (current(capture)) {
        publish_locked(X4_MIC_ACTIVE, rate, level);
        if (frame->size > 2) {
            if (mic.count == QUEUE_CAP) {
                mic.head = (mic.head + 1) % QUEUE_CAP;
                --mic.count;
            }
            mic.frames[(mic.head + mic.count) % QUEUE_CAP] = *frame;
            ++mic.count;
            atomic_store(&send_epoch, capture.epoch);
        }
    }
    unlock_mic();
}

#ifdef X4_OPENORBIS
typedef struct {
    int (*init)(void);
    int (*open)(int32_t, unsigned, unsigned, unsigned, unsigned, unsigned);
    int (*read)(int, void *);
    int (*state)(int);
    int (*close)(int);
} AudioIn;

/* Only the sole capture owner resolves and calls AudioIn. The module is
 * process-lifetime once initialized. The shared loader's network state is
 * untouched, and there is no AudioIn import or work in the off route. */
static AudioIn audio_in;

static int find_symbol(const char *name, void **address)
{
    OrbisKernelModule handles[256];
    size_t count = 0;
    int rc = sceKernelGetModuleList(handles, sizeof(handles) / sizeof(handles[0]), &count);
    if (rc < 0) return rc;
    if (count > sizeof(handles) / sizeof(handles[0])) count = sizeof(handles) / sizeof(handles[0]);
    for (size_t i = 0; i < count; ++i) {
        *address = NULL;
        if (sceKernelDlsym(handles[i], name, address) >= 0 && *address) return (int)handles[i];
    }
    return -1;
}

static int audio_prepare(void)
{
    if (audio_in.open) return 0;
    AudioIn api = {0};
    void *address = NULL;
    int handle = find_symbol("sceAudioInOpen", &address);
    if (handle < 0 && find_symbol("sceSysmoduleLoadModuleInternal", &address) >= 0) {
        int (*load)(uint32_t) = (int (*)(uint32_t))address;
        int rc = load(ORBIS_SYSMODULE_INTERNAL_AUDIOIN);
        MIC_LOG("XCloud4: mic AudioIn module rc=0x%08x\n", (unsigned)rc);
        (void)rc;
        handle = find_symbol("sceAudioInOpen", &address);
    }
    if (handle < 0) return handle;
    api.open = (int (*)(int32_t, unsigned, unsigned, unsigned, unsigned, unsigned))address;
    if (sceKernelDlsym(handle, "sceAudioInInit", &address) < 0 || !address) return -1;
    api.init = (int (*)(void))address;
    if (sceKernelDlsym(handle, "sceAudioInInput", &address) < 0 || !address) return -1;
    api.read = (int (*)(int, void *))address;
    if (sceKernelDlsym(handle, "sceAudioInClose", &address) < 0 || !address) return -1;
    api.close = (int (*)(int))address;
    if (sceKernelDlsym(handle, "sceAudioInGetSilentState", &address) < 0 || !address) return -1;
    api.state = (int (*)(int))address;
    int rc = api.init();
    MIC_LOG("XCloud4: mic AudioIn init rc=0x%08x\n", (unsigned)rc);
    /* Existing initialization is permissible in the tested PS4 API path. */
    if (rc < 0 && (uint32_t)rc != 0x8026000cu && (uint32_t)rc != 0x8026000eu) return rc;
    audio_in = api;
    return 0;
}

static int audio_open(int32_t user, unsigned rate, unsigned port)
{ return audio_in.open(user, port, 0, GRAIN, rate, 0); }
static int audio_read(int handle, int16_t pcm[GRAIN]) { return audio_in.read(handle, pcm); }
static int audio_state(int handle) { return audio_in.state(handle); }
static int audio_close(int handle) { return audio_in.close(handle); }
#elif defined(X4_MIC_CAPTURE_TEST)
static int audio_prepare(void) { return 0; }
static int audio_open(int32_t user, unsigned rate, unsigned port)
{ return x4_mic_test_audio_open(user, rate, port, GRAIN); }
static int audio_read(int handle, int16_t pcm[GRAIN]) { return x4_mic_test_audio_read(handle, pcm); }
static int audio_state(int handle) { return x4_mic_test_audio_state(handle); }
static int audio_close(int handle) { return x4_mic_test_audio_close(handle); }
#else
static int audio_prepare(void) { return -1; }
static int audio_open(int32_t user, unsigned rate, unsigned port)
{ (void)user; (void)rate; (void)port; return -1; }
static int audio_read(int handle, int16_t pcm[GRAIN]) { (void)handle; (void)pcm; return -1; }
static int audio_state(int handle) { (void)handle; return -1; }
static int audio_close(int handle) { (void)handle; return -1; }
#endif

/* Sole worker ownership includes any handle whose close did not complete.
 * Retry it before opening another port, even after a session restart. */
static int retained_handle = -1;
/* Sole capture-worker state; unsupported 48kHz is process-lifetime, including
 * later sessions and peers. Never retry that rejected rate every two seconds. */
static bool high_rate_rejected;
#if !X4_PUBLIC
static bool open_logged[2][2],state_logged[2],selected_port_logged;
static int open_result_logged[2][2],state_result_logged[2];
static unsigned selected_port,selected_port_last;
static void note_open(unsigned rate,unsigned port,int handle)
{
    unsigned index=rate==RATE_LOW?1u:0u;int result=handle<0?handle:0;
    if(open_logged[index][port]&&open_result_logged[index][port]==result)return;
    open_logged[index][port]=true;open_result_logged[index][port]=result;
    MIC_LOG("XCloud4: mic AudioIn open rate=%u port=%u rc=0x%08x\n",rate,port,(unsigned)result);
}
static void note_state(unsigned port,int state)
{
    if(state_logged[port]&&state_result_logged[port]==state)return;
    state_logged[port]=true;state_result_logged[port]=state;
    MIC_LOG("XCloud4: mic AudioIn silent_state port=%u value=0x%08x\n",port,(unsigned)state);
}
static void note_selected_port(unsigned port)
{
    selected_port=port;
    if(selected_port_logged&&selected_port_last==port)return;
    selected_port_logged=true;selected_port_last=port;
    MIC_LOG("XCloud4: mic AudioIn selected_port=%u\n",port);
}
#endif

static bool close_port(int handle)
{
    int rc = audio_close(handle);
    MIC_LOG("XCloud4: mic AudioIn close rc=0x%08x\n", (unsigned)rc);
    bool closed = rc >= 0 || (uint32_t)rc == 0x80260101u || (uint32_t)rc == 0x80260109u;
    retained_handle = closed ? -1 : handle;
    return closed;
}

static int open_port(Capture capture, unsigned *rate, int *state)
{
    const unsigned rates[] = {RATE_HIGH, RATE_LOW};
    if (!current(capture)) return -1;
    if (retained_handle >= 0 && !close_port(retained_handle)) return -1;
    int rc = audio_prepare();
    if (rc < 0) {
        MIC_LOG("XCloud4: mic AudioIn prepare rc=0x%08x\n", (unsigned)rc);
        return -1;
    }
    for (size_t i = high_rate_rejected?1u:0u; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        for (unsigned port = 0; port < 2 && current(capture); ++port) {
            int handle = audio_open(capture.user, rates[i], port);
            #if !X4_PUBLIC
            note_open(rates[i],port,handle);
            #endif
            if(handle<0&&rates[i]==RATE_HIGH&&(uint32_t)handle==0x80260103u){
                high_rate_rejected=true;break;
            }
            if (handle < 0) continue;
            int observed = audio_state(handle);
            #if !X4_PUBLIC
            note_state(port,observed);
            #endif
            if (observed >= 0 && !(observed & NO_DEVICE)) {
                #if !X4_PUBLIC
                note_selected_port(port);
                #endif
                *rate = rates[i];
                *state = observed;
                return handle;
            }
            if (!close_port(handle)) return -1;
        }
    }
    return -1;
}

typedef struct {
    int16_t pcm[MAX_FRAME];
    unsigned used;
    int16_t previous;
    bool primed;
    uint64_t sample_us;
} Accumulator;

static unsigned level_percent(const int16_t *pcm, unsigned count)
{
    int peak = 0;
    for (unsigned i = 0; i < count; ++i) {
        int value = pcm[i];
        if (value < 0) value = -value;
        if (value > peak) peak = value;
    }
    return (unsigned)peak * 100 / 32768;
}

static bool accumulate(Capture capture, Accumulator *acc, OpusEncoder *encoder,
                       unsigned rate, const int16_t *grain, unsigned count, uint64_t captured,
                       unsigned *encoded)
{
    const unsigned samples = capture.frame_ms * 48;
    const unsigned factor = rate == RATE_LOW ? 3 : 1;
    if (!acc->primed) {
        uint64_t grain_us = (uint64_t)count * 1000000 / rate;
        acc->sample_us = captured > grain_us ? captured - grain_us : 0;
        acc->previous = grain[0];
        acc->primed = true;
    }
    for (unsigned i = 0; i < count; ++i) {
        const int32_t sample = grain[i], prior = acc->previous;
        const int32_t interpolated[] = {(2 * prior + sample) / 3, (prior + 2 * sample) / 3, sample};
        for (unsigned j = 3 - factor; j < 3; ++j) {
            acc->pcm[acc->used++] = (int16_t)interpolated[j];
            if (acc->used != samples) continue;
            X4MicFrame frame = {.captured_us = captured, .sample_us = acc->sample_us,
                .epoch = capture.epoch, .samples = (uint16_t)samples};
            int length = opus_encode(encoder, acc->pcm, (int)samples, frame.data, sizeof(frame.data));
            if (length < 0) {
                MIC_LOG("XCloud4: mic Opus encode rc=%d\n", length);
                wipe(&frame, sizeof(frame));
                return false;
            }
            frame.size = (uint16_t)length;
            publish_frame(capture, rate, level_percent(acc->pcm, samples), &frame);
            wipe(&frame, sizeof(frame));
            acc->used = 0;
            acc->sample_us += (uint64_t)capture.frame_ms * 1000;
            ++*encoded;
        }
        acc->previous = (int16_t)sample;
    }
    return true;
}

static X4MicStatus capture_port(Capture capture, int handle, unsigned rate, int initial_state)
{
    if (initial_state & USER_MUTED) return X4_MIC_MUTED;
    if (!current(capture)) return X4_MIC_WAITING;
    const int encoder_size = opus_encoder_get_size(1);
    if (encoder_size <= 0) return X4_MIC_UNAVAILABLE;
    OpusEncoder *encoder = malloc((size_t)encoder_size);
    if (!encoder) return X4_MIC_UNAVAILABLE;
    int rc = opus_encoder_init(encoder, RATE_HIGH, 1, OPUS_APPLICATION_VOIP);
    if (rc == OPUS_OK) rc = opus_encoder_ctl(encoder, OPUS_SET_BITRATE(28000));
    if (rc == OPUS_OK) rc = opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
    if (rc == OPUS_OK) rc = opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    if (rc == OPUS_OK) rc = opus_encoder_ctl(encoder, OPUS_SET_DTX(1));
    X4MicStatus result = X4_MIC_UNAVAILABLE;
    Accumulator accumulator = {0};
    int16_t grain[GRAIN] = {0};
    unsigned encoded = 0;
#if !X4_PUBLIC
    unsigned reads = 0, cpu_grains = 0, cpu_frames = 0;
    uint64_t cpu_total = 0, cpu_max_grain = 0, read_max = 0;
#endif
    if (rc != OPUS_OK) MIC_LOG("XCloud4: mic Opus init rc=%d\n", rc);
    while (rc == OPUS_OK && current(capture)) {
#if !X4_PUBLIC
        uint64_t cpu_before = cpu_ns(), before_read = now_us();
#endif
        memset(grain, 0, sizeof(grain));
        int read = audio_read(handle, grain);
        uint64_t captured = now_us();
        if (!current(capture)) break;
        int state = audio_state(handle);
        #if !X4_PUBLIC
        note_state(selected_port,state);
        #endif
        if (read < 0 || read > GRAIN || state < 0 || state & NO_DEVICE) {
            atomic_store(&send_epoch, 0);
            MIC_LOG("XCloud4: mic AudioIn input rc=0x%08x\n", (unsigned)read);
            break;
        }
        if (state & USER_MUTED) {
            atomic_store(&send_epoch, 0);
            result = X4_MIC_MUTED;
            break;
        }
        unsigned count = read ? (unsigned)read : GRAIN;
#if !X4_PUBLIC
        if (!reads) MIC_LOG("XCloud4: mic AudioIn first input rc=%d samples=%u\n",read,count);
        unsigned encoded_before = encoded;
#endif
        if (!accumulate(capture, &accumulator, encoder, rate, grain, count, captured, &encoded)) break;
#if !X4_PUBLIC
        uint64_t after = cpu_ns();
        if (cpu_before && after >= cpu_before) {
            uint64_t elapsed = after - cpu_before;
            cpu_total += elapsed;
            if (elapsed > cpu_max_grain) cpu_max_grain = elapsed;
            ++cpu_grains;
            cpu_frames += encoded - encoded_before;
        }
        uint64_t read_time = captured >= before_read ? captured - before_read : 0;
        if (read_time > read_max) read_max = read_time;
        ++reads;
#endif
    }
#if !X4_PUBLIC
    MIC_LOG("XCloud4: mic capture rate=%u frame_ms=%u reads=%u frames=%u input_max_us=%llu\n",
            rate, capture.frame_ms, reads, encoded, (unsigned long long)read_max);
    if (cpu_frames) MIC_LOG("XCloud4: mic capture CPU ns=%llu grains=%u frames=%u ns_per_frame=%llu max_grain_ns=%llu\n",
            (unsigned long long)cpu_total, cpu_grains, cpu_frames,
            (unsigned long long)(cpu_total / cpu_frames), (unsigned long long)cpu_max_grain);
    else MIC_LOG("XCloud4: mic capture CPU unavailable\n");
#endif
    wipe(grain, sizeof(grain));
    wipe(&accumulator, sizeof(accumulator));
    wipe(encoder, (size_t)encoder_size);
    free(encoder);
    return result;
}

static void retry_wait_locked(uint64_t retry_epoch)
{
    uint64_t deadline=now_us()+RETRY_US;
    while(mic.wanted.epoch==retry_epoch){
        uint64_t now=now_us();if(now>=deadline)break;
        timed_wait_mic((unsigned)(deadline-now));
    }
}

static void *capture_worker(void *ignored)
{
    (void)ignored;
    lock_mic();
    for (;;) {
        while (mic.wanted.generation && (!mic.wanted.peer || mic.muted)) wait_mic();
        if (!mic.wanted.generation) break;
        Capture capture = mic.wanted;
        unlock_mic();
        unsigned rate = 0;
        int state = 0;
        int handle = open_port(capture, &rate, &state);
        X4MicStatus status = handle < 0 ? X4_MIC_UNAVAILABLE : capture_port(capture, handle, rate, state);
        lock_mic();
        bool retry = current(capture);
        if (retry) {
            invalidate_locked();
            publish_locked(status, 0, 0);
        }
        uint64_t retry_epoch = mic.wanted.epoch;
        unlock_mic();
        if (handle >= 0) (void)close_port(handle);
        lock_mic();
        if (retry) {
            retry_wait_locked(retry_epoch);
        }
    }
    mic.running = false;
    unlock_mic();
    return NULL;
}

static int start_worker(void)
{
#ifdef X4_OPENORBIS
    OrbisPthreadAttr attr;
    OrbisPthread thread;
    int rc = scePthreadAttrInit(&attr);
    if (!rc) {
        rc = scePthreadAttrSetstacksize(&attr, WORKER_STACK);
        if (!rc) rc = scePthreadAttrSetdetachstate(&attr, ORBIS_PTHREAD_CREATE_DETACHED);
        if (!rc) rc = scePthreadCreate(&thread, &attr, capture_worker, NULL, "x4-mic");
        (void)scePthreadAttrDestroy(&attr);
    }
#else
    pthread_attr_t attr;
    pthread_t thread;
    int rc = pthread_attr_init(&attr);
    if (!rc) {
        rc = pthread_attr_setstacksize(&attr, WORKER_STACK);
        if (!rc) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (!rc) rc = pthread_create(&thread, &attr, capture_worker, NULL);
        (void)pthread_attr_destroy(&attr);
    }
#endif
    return rc;
}

bool x4_mic_capture_attach(uint64_t generation, uint64_t peer)
{
    if (!generation || !peer || !sync_available()) return false;
    bool attached = false, start = false;
    lock_mic();
    if (mic.wanted.generation == generation && usable_user(mic.wanted.user) &&
        (mic.wanted.peer == peer || (!mic.wanted.peer && peer > mic.last_peer))) {
        attached = true;
        if (!mic.wanted.peer) {
            invalidate_locked();
            mic.wanted.peer = mic.last_peer = peer;
            atomic_store(&peer_word, peer);
            publish_locked(mic.muted ? X4_MIC_MUTED : X4_MIC_WAITING, 0, 0);
        }
        if (!mic.running) {
            mic.running = true;
            start = true;
        }
        signal_mic();
    }
    unlock_mic();
    if (start) {
        int rc = start_worker();
        if (rc) {
            lock_mic();
            mic.running = false;
            if (mic.wanted.peer) {
                invalidate_locked();
                mic.wanted.peer = 0;
                atomic_store(&peer_word, 0);
                publish_locked(X4_MIC_UNAVAILABLE, 0, 0);
            }
            unlock_mic();
            MIC_LOG("XCloud4: mic capture worker rc=0x%08x\n", (unsigned)rc);
            attached = false;
        }
    }
    return attached;
}

void x4_mic_capture_detach(uint64_t generation, uint64_t peer)
{
    if (!generation || !peer || !sync_available()) return;
    lock_mic();
    if (binding_locked(generation, peer)) {
        invalidate_locked();
        mic.wanted.peer = 0;
        atomic_store(&peer_word, 0);
        publish_locked(mic.muted ? X4_MIC_MUTED : X4_MIC_WAITING, 0, 0);
        signal_mic();
    }
    unlock_mic();
}

void x4_mic_capture_reject(uint64_t generation)
{
    if (!generation || !sync_available()) return;
    lock_mic();
    if (mic.wanted.generation == generation && !mic.wanted.peer &&
        (atomic_load(&status_word) & 7u) != X4_MIC_UNAVAILABLE)
        publish_locked(X4_MIC_REJECTED, 0, 0);
    unlock_mic();
}

#ifdef X4_MIC_CAPTURE_TEST
bool x4_mic_capture_test_idle(void)
{
    lock_mic();
    bool idle = !mic.running;
    unlock_mic();
    return idle;
}

unsigned x4_mic_capture_test_pending(void)
{
    lock_mic();
    unsigned pending = mic.count;
    unlock_mic();
    return pending;
}
#endif
