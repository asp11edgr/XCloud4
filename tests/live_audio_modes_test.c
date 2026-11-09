/* SPDX-License-Identifier: GPL-3.0-only */
/* Real audio helpers/worker, deterministic Opus/AudioOut/RTP doubles. No video. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../src/audio/live_audio.c"

static uint64_t clock_us;
static int decode_fail;
static void *(*created_worker)(void *);
static unsigned output_calls;
static X4LiveAudio *worker_audio;
static int join_result, close_result;
static bool underflow_on_join;
static unsigned destroyed_decoders;
static int reject_512, reject_1024;
static unsigned open_calls, open_sizes[4];
OpusDecoder *opus_decoder_create(int rate, int channels, int *rc)
{ assert(rate == 48000 && channels == 2); *rc = 0; return calloc(1, sizeof(OpusDecoder)); }
void opus_decoder_destroy(OpusDecoder *d) { ++destroyed_decoders; free(d); }
int opus_decoder_ctl(OpusDecoder *d, int command, ...)
{ assert(command == OPUS_RESET_STATE); ++d->resets; return 0; }
int opus_packet_get_nb_samples(const unsigned char *p, opus_int32 n, int rate)
{ assert(rate == 48000); return n ? (int)p[0] * 120 : -1; }
int opus_packet_get_samples_per_frame(const unsigned char *p, opus_int32 rate)
{ return opus_packet_get_nb_samples(p, 1, rate); }
int opus_packet_has_lbrr(const unsigned char *p, opus_int32 n)
{ assert(p && n > 0); return 0; }
int opus_decode(OpusDecoder *d, const unsigned char *p, opus_int32 size, int16_t *out, int capacity, int fec)
{
    assert(!fec); ++d->calls;
    if (decode_fail) return -4;
    int count = p ? opus_packet_get_nb_samples(p, size, 48000) : capacity;
    assert(count <= capacity);
    if (!p) ++d->plc_calls;
    for (int i = 0; i < count * 2; ++i) out[i] = p ? 12000 : 9000;
    return count;
}
uint64_t sceKernelGetProcessTime(void) { return clock_us; }
int sceKernelUsleep(unsigned us) { clock_us += us; atomic_store(&worker_audio->stop, true); return 0; }
int scePthreadCreate(OrbisPthread *t, const pthread_attr_t *at, void *(*fn)(void *), void *opaque, const char *name)
{ (void)t; (void)at; (void)opaque; assert(!strcmp(name, "x4-opus")); created_worker = fn; return 0; }
int scePthreadJoin(OrbisPthread t, void **result)
{
    (void)t; (void)result;
    if (!join_result && underflow_on_join) new_underflow(worker_audio, clock_us + 2000);
    return join_result;
}
int x4_live_ring_init(X4LiveRing *q, uint32_t cap) { (void)q; assert(cap == 96); return 0; }
void x4_live_ring_free(X4LiveRing *q) { (void)q; }
size_t x4_live_ring_pop(X4LiveRing *q, uint8_t *p) { (void)q; (void)p; return 0; }
bool x4_live_ring_push(X4LiveRing *q, const uint8_t *p, size_t n) { (void)q; (void)p; (void)n; return true; }
int x4_live_reorder_init(X4LiveReorder *q, uint16_t w, uint16_t d, uint64_t wait)
{ (void)q; assert(w == 64 && d == 8 && wait == 30000); return 0; }
void x4_live_reorder_free(X4LiveReorder *q) { (void)q; }
void x4_live_reorder_reset(X4LiveReorder *q) { (void)q; }
int x4_live_reorder_insert(X4LiveReorder *q, const uint8_t *p, size_t n, uint16_t seq, uint64_t now)
{ (void)q; (void)p; (void)n; (void)seq; (void)now; return 0; }
const uint8_t *x4_live_reorder_next(X4LiveReorder *q, uint64_t now, size_t *n, uint32_t *lost)
{ (void)q; (void)now; *n = 0; *lost = 0; return NULL; }
void x4_live_track_init(X4LiveTrack *t, int type) { memset(t, 0, sizeof(*t)); t->forced_type = type; }
int x4_live_track_accept(X4LiveTrack *t, const X4LiveRtp *r) { (void)t; (void)r; return 0; }
int x4_live_rtp_parse(const uint8_t *p, size_t n, X4LiveRtp *r) { (void)p; (void)n; (void)r; return -1; }
static int32_t out_init(void) { return 0; }
static int32_t out_open(OrbisUserServiceUserId u, enum OrbisAudioOutPort port, int32_t index, uint32_t samples, uint32_t rate, uint32_t format)
{
    (void)u; (void)port; (void)index; (void)format;
    assert((samples == AUDIO_BLOCK || samples == AUDIO_NEW_BLOCK) && rate == 48000);
    assert(open_calls < 4); open_sizes[open_calls++] = samples;
    return samples == AUDIO_NEW_BLOCK && reject_512 ? reject_512 : samples == AUDIO_BLOCK && reject_1024 ? reject_1024 : 7;
}
static int32_t out_write(int32_t h, const void *p)
{ assert(h == 7); if (p) { ++output_calls; atomic_store(&worker_audio->stop, true); } return 0; }
static int32_t out_close(int32_t h) { assert(h == 7); return close_result; }
int x4_module_open(const char *name) { assert(!strcmp(name, "libSceAudioOut")); return 1; }
int x4_module_symbol(int h, const char *name, void **value)
{
    assert(h == 1);
    if (!strcmp(name, "sceAudioOutInit")) *value = (void *)out_init;
    else if (!strcmp(name, "sceAudioOutOpen")) *value = (void *)out_open;
    else if (!strcmp(name, "sceAudioOutOutput")) *value = (void *)out_write;
    else if (!strcmp(name, "sceAudioOutClose")) *value = (void *)out_close;
    else assert(0);
    return 0;
}
static X4LiveAudio *audio(bool previous)
{
    open_calls = 0;
    int rc = -1; X4LiveAudio *a = x4_live_audio_start_mode(&rc, 111, previous);
    assert(a && !rc && created_worker == (previous ? play : play_new));
    return a;
}
static void packet(X4LiveAudio *a, unsigned timestamp, unsigned duration, int16_t *decoded)
{
    uint8_t payload = (uint8_t)(duration / 120);
    X4LiveRtp r = { .payload = &payload, .size = 1, .timestamp = timestamp };
    new_decode_packet(a, &r, decoded);
}
static void test_threshold_and_legacy(void)
{
    int16_t samples[AUDIO_TARGET * 2] = {0};
    X4LiveAudio *a = audio(false); worker_audio = a; output_calls = 0;
    new_append(a, samples, 1024); play_new(a);
    assert(!output_calls && !atomic_load(&a->playing)); x4_live_audio_stop(a);
    a = audio(false); worker_audio = a; output_calls = 0;
    new_append(a, samples, AUDIO_TARGET); play_new(a);
    assert(output_calls == 1 && a->pcm_count == AUDIO_TARGET - AUDIO_NEW_BLOCK && !atomic_load(&a->restarts)); x4_live_audio_stop(a);
    a = audio(false); worker_audio = a; output_calls = 0; a->have_started = true;
    new_append(a, samples, AUDIO_TARGET); play_new(a);
    assert(output_calls == 1 && atomic_load(&a->restarts) == 1); x4_live_audio_stop(a);
    a = audio(true); worker_audio = a; output_calls = 0;
    append_pcm(a, samples, 1024); play(a);
    assert(output_calls == 1); X4LiveAudioSnapshot s; x4_live_audio_snapshot(a, &s);
    assert(s.previous && !s.detailed_metrics_available); x4_live_audio_stop(a);
    int rc; open_calls = 0; a = x4_live_audio_start(&rc, 111); assert(a && a->previous && created_worker == play); x4_live_audio_stop(a);
    puts("PASS audio target50ms, restart current target and unchanged previous worker/wrapper");
}
static void test_jitter(void)
{
    X4LiveAudio *a = audio(false); int16_t decoded[5760 * 2];
    /* Alternate +/-6 ms arrival jitter at 20 ms packet cadence, 50 ms prebuffer. */
    unsigned next = 0; uint64_t next_output = 0; bool started = false;
    for (uint64_t now = 0; now < 3000000; now += 100) {
        uint64_t arrival = (uint64_t)next * 20000u + (next % 2 ? 6000u : 0u);
        if (now >= arrival) { packet(a, next * 960u, 960, decoded); ++next; }
        if (!started && a->pcm_count >= AUDIO_TARGET) { started = true; next_output = now; }
        if (started && now >= next_output) { new_render(a, decoded, now); next_output += 10667u; }
    }
    assert(started && !atomic_load(&a->underflows) && !atomic_load(&a->plc_samples));
    assert(!a->decoder->resets); x4_live_audio_stop(a);
    puts("PASS real new helpers: +/-6ms jitter, no underflow with50ms and512 samples");
}
static void test_loss_plc_and_credit(void)
{
    X4LiveAudio *a = audio(false); int16_t decoded[5760 * 2];
    packet(a, 0, 960, decoded); packet(a, 1920, 960, decoded);
    assert(atomic_load(&a->plc_samples) == 960 && a->pcm_count == 2880 && !a->decoder->resets);
    a->pcm_count = 0; a->plc_run_samples = 0; a->concealed_ahead = 0;
    a->have_started = true; atomic_store(&a->playing, true);
    new_render(a, decoded, 100000);
    unsigned old = a->pcm_count, credit = a->concealed_ahead;
    assert(credit > 0);
    /* Rule 1: with buffer below target, delayed packet is kept entirely: pcm_count == old + 960 and concealed_ahead == 0 */
    packet(a, a->expected_timestamp, 960, decoded);
    assert(a->pcm_count == old + 960 && !a->concealed_ahead);

    /* Rule 2: with buffer near target, only excess is discarded: pcm_count == target */
    unsigned target = atomic_load(&a->target_samples);
    a->pcm_count = target - 400;
    a->concealed_ahead = 600;
    packet(a, a->expected_timestamp, 960, decoded);
    assert(a->pcm_count == target && !a->concealed_ahead);

    /* Test 60ms cap / fade / rebuffer */
    a->pcm_count = 0; a->plc_run_samples = 0; a->concealed_ahead = 0;
    a->have_started = true; atomic_store(&a->playing, true);
    uint64_t plc_before = atomic_load(&a->plc_samples);
    for (unsigned n = 1; n < 20 && atomic_load(&a->playing); ++n)
        new_render(a, decoded, 100000u + n * 10667u);
    assert(a->plc_run_samples == AUDIO_PLC_MAX && !atomic_load(&a->playing));
    assert(a->output_block[AUDIO_BLOCK * 2 - 1] == 0 && !a->decoder->resets);
    assert(atomic_load(&a->plc_samples) == plc_before + AUDIO_PLC_MAX);
    decode_fail = 1; packet(a, a->expected_timestamp, 960, decoded); decode_fail = 0;
    assert(atomic_load(&a->decode_errors) == 1 && atomic_load(&a->bad_frames) == 1);
    x4_live_audio_stop(a);
    puts("PASS loss->PLC without reset,60ms cap/fade/rebuffer,late credit,decode error");
}
static void test_long_declared_gap(void)
{
    X4LiveAudio *a = audio(false); int16_t decoded[5760 * 2];
    packet(a, 0, 960, decoded); packet(a, 5760, 960, decoded);
    assert(atomic_load(&a->plc_samples) == 2880 && a->pcm_count == 6720);
    assert(!a->decoder->resets && !atomic_load(&a->pcm_overflows));
    /* 60 ms PLC, then 5ms fade followed by silence for the rest of the 100ms gap. */
    unsigned silence = 960 + 2880;
    assert(a->pcm[silence * 2] > 0 && a->pcm[silence * 2] < 9000);
    assert(a->pcm[(silence + 239) * 2] == 0 && a->pcm[(5760 - 1) * 2] == 0);
    x4_live_audio_stop(a);
    puts("PASS declared100ms loss:60ms PLC then fade/silence,without reset");
}
static void test_rebuffer_does_not_replay_old_silence(void)
{
    X4LiveAudio *a = audio(false); int16_t decoded[5760 * 2];
    packet(a, 0, 960, decoded); packet(a, 960, 960, decoded);
    a->have_started = true; atomic_store(&a->playing, true);
    for (unsigned n = 0; n < 25 && atomic_load(&a->playing); ++n)
        new_render(a, decoded, 100000u + n * 10667u);
    assert(!atomic_load(&a->playing) && !a->pcm_count);
    assert(a->plc_run_samples == AUDIO_PLC_MAX && atomic_load(&a->plc_samples) == AUDIO_PLC_MAX);
    assert(a->concealed_ahead && a->output_block[AUDIO_BLOCK * 2 - 1] == 0);
    /* 500ms of RTP time has elapsed while AudioOut was already silent. */
    packet(a, 24000, 960, decoded);
    assert(a->pcm_count == 960 && !a->concealed_ahead && !a->plc_run_samples);
    assert(atomic_load(&a->plc_samples) == AUDIO_PLC_MAX && !atomic_load(&a->pcm_overflows));
    worker_audio = a; output_calls = 0; play_new(a);
    assert(!output_calls && a->pcm_count == 960); /* 20ms real cannot restart. */
    atomic_store(&a->stop, false);
    /* Underflow raised current target from 50ms (2400) by +10ms (480) to 60ms (2880). */
    packet(a, 24960, 960, decoded); assert(a->pcm_count == 1920);
    play_new(a); assert(!output_calls && a->pcm_count == 1920); /* 40ms real cannot restart at 60ms target. */
    atomic_store(&a->stop, false); packet(a, 25920, 960, decoded); assert(a->pcm_count == 2880);
    play_new(a); assert(output_calls == 1 && atomic_load(&a->restarts) == 1 && a->pcm_count == 2880 - AUDIO_NEW_BLOCK);
    for (unsigned i = 0; i < AUDIO_NEW_BLOCK * 2; ++i) assert(a->output_block[i] == 12000);
    assert(!atomic_load(&a->pcm_overflows) && !atomic_load(&a->trimmed_samples));
    x4_live_audio_stop(a);
    puts("PASS gap500ms after PLC exhaustion: no old silence, restart with current60ms target");
}
static void test_joined_final_snapshot_and_close_retention(void)
{
    X4LiveAudio *a = audio(false); worker_audio = a; clock_us = 7000;
    new_underflow(a, 1000); X4LiveAudioSnapshot before, final;
    x4_live_audio_snapshot(a, &before); assert(before.underflows == 1 && !before.worker_joined);
    underflow_on_join = true;
    assert(x4_live_audio_stop_report(a, &final) == 0);
    underflow_on_join = false;
    assert(final.worker_joined && final.underflows == 2 && final.underflow_interval_count == 1);
    assert(final.underflow_interval_min_us == 8000 && final.underflow_interval_max_us == 8000);
    a = audio(false); worker_audio = a; join_result = 5; unsigned prior = destroyed_decoders;
    assert(x4_live_audio_stop_report(a, &final) == -5 && !final.worker_joined);
    assert(a->running == 1 && a->decoder && destroyed_decoders == prior);
    join_result = 0; close_result = -77;
    assert(x4_live_audio_stop_report(a, &final) == -77 && final.worker_joined);
    assert(!a->running && a->decoder && a->handle == 7 && destroyed_decoders == prior);
    close_result = 0; assert(!x4_live_audio_stop_report(a, &final));
    assert(final.worker_joined && destroyed_decoders == prior + 1);
    puts("PASS final snapshot after join includes final underflow; join/AudioOut-close failure retains ownership");
}
static void test_valley_target_and_bounded_trim(void)
{
    X4LiveAudio *a = audio(false); int16_t decoded[5760 * 2] = {0};
    /* 1. Target values: 50 ms init, +10 ms per episode up to 100 ms, -10 ms after 10 s down to 40 ms. */
    assert(atomic_load(&a->target_samples) == AUDIO_TARGET); // 2400 (50 ms)
    new_underflow(a, 1000); assert(atomic_load(&a->target_samples) == 2880); // +10 ms -> 60 ms
    new_underflow(a, 1020); assert(atomic_load(&a->target_samples) == 2880); // same episode -> unchanged
    a->in_underflow = false; new_underflow(a, 2000); assert(atomic_load(&a->target_samples) == 3360); // +10 ms -> 70 ms
    a->in_underflow = false; new_underflow(a, 3000); assert(atomic_load(&a->target_samples) == 3840); // +10 ms -> 80 ms
    a->in_underflow = false; new_underflow(a, 4000); assert(atomic_load(&a->target_samples) == 4320); // +10 ms -> 90 ms
    a->in_underflow = false; new_underflow(a, 5000); assert(atomic_load(&a->target_samples) == 4800); // +10 ms -> 100 ms (cap)
    a->in_underflow = false; new_underflow(a, 6000); assert(atomic_load(&a->target_samples) == 4800); // capped at 100 ms
    a->pcm_count = 0; new_level(a, 10005999); assert(atomic_load(&a->target_samples) == 4800); // <10 s -> no change
    new_level(a, 10006000); assert(atomic_load(&a->target_samples) == 4320); // 10 s -> -10 ms to 90 ms
    new_level(a, 20006000); assert(atomic_load(&a->target_samples) == 3840); // 20 s -> -10 ms to 80 ms
    new_level(a, 30006000); assert(atomic_load(&a->target_samples) == 3360); // 30 s -> -10 ms to 70 ms
    new_level(a, 40006000); assert(atomic_load(&a->target_samples) == 2880); // 40 s -> -10 ms to 60 ms
    new_level(a, 50006000); assert(atomic_load(&a->target_samples) == 2400); // 50 s -> -10 ms to 50 ms
    new_level(a, 60006000); assert(atomic_load(&a->target_samples) == 1920); // 60 s -> -10 ms to 40 ms (floor)
    new_level(a, 90006000); assert(atomic_load(&a->target_samples) == AUDIO_TARGET_MIN); // cannot drop below floor 40 ms
    x4_live_audio_stop(a);
    puts("PASS per-episode target+10ms cap100; stable10s target-10ms floor40");

    /* 2. 30 s pre-playback loading does not change target; 10 s stable playback steps down */
    a = audio(false);
    assert(atomic_load(&a->target_samples) == AUDIO_TARGET);
    a->pcm_count = 0;
    new_level(a, 30000000u); // 30 s load before playing starts
    assert(atomic_load(&a->target_samples) == AUDIO_TARGET);
    assert(atomic_load(&a->target_adjustments) == 0);
    // Playback starts at 30 s:
    a->target_stable_since_us = 30000000u;
    new_level(a, 39999000u); assert(atomic_load(&a->target_samples) == AUDIO_TARGET);
    new_level(a, 40000000u); assert(atomic_load(&a->target_samples) == AUDIO_TARGET - AUDIO_TRIM);
    assert(atomic_load(&a->target_adjustments) == 1);
    x4_live_audio_stop(a);
    puts("PASS 30s load before sounding does not change target; 10s playback steps down");

    /* 3. Bounded trim <= 10 ms every 200 ms, never two cuts < 200 ms, keep0 == drop0 */
    a = audio(false);
    a->target_stable_since_us = 0;
    // Establish valley with pcm_count = 4800 (100 ms) sustained for 1 s:
    for (unsigned n = 0; n < 1000; ++n) {
        a->pcm_count = 4800;
        new_level(a, (uint64_t)n * 1000u);
    }
    // Seed recognizable samples into buffer:
    for (unsigned i = 0; i < 4800; ++i) {
        a->pcm[i * 2] = (int16_t)(1000 + i);
        a->pcm[i * 2 + 1] = (int16_t)(2000 + i);
    }
    a->pcm_head = 0; a->pcm_count = 4800;
    int16_t drop0_l = a->pcm[0], drop0_r = a->pcm[1];
    // First trim at t = 1000000 us:
    new_level(a, 1000000u);
    assert(atomic_load(&a->trim_events) == 1);
    assert(atomic_load(&a->trimmed_samples) == AUDIO_TRIM); // exactly 480 samples (10 ms)
    assert(a->pcm_count == 4800 - AUDIO_TRIM);
    assert(a->pcm_head == AUDIO_TRIM);
    // Verify first sample after cut equals drop[0]:
    assert(a->pcm[a->pcm_head * 2] == drop0_l);
    assert(a->pcm[a->pcm_head * 2 + 1] == drop0_r);
    // Attempt trim before 200 ms has elapsed (at 199.999 ms later):
    new_level(a, 1000000u + 199999u);
    assert(atomic_load(&a->trim_events) == 1); // No new trim event!
    assert(atomic_load(&a->trimmed_samples) == AUDIO_TRIM);
    assert(a->pcm_count == 4800 - AUDIO_TRIM);
    // Now at exactly 200 ms after previous cut (t = 1200000 us):
    new_level(a, 1000000u + 200000u);
    assert(atomic_load(&a->trim_events) == 2); // Second trim event!
    assert(atomic_load(&a->trimmed_samples) == AUDIO_TRIM * 2);
    assert(a->pcm_count == 4800 - AUDIO_TRIM * 2);
    x4_live_audio_stop(a);
    puts("PASS bounded trim <=10ms/200ms, crossfade keep0==drop0, throttle <200ms enforced");

    /* 4. Fast renders cannot evict low valley */
    a = audio(false); a->target_stable_since_us = 0;
    a->pcm_count = 0; new_level(a, 0);
    /* >1000 renders in 1ms cannot evict the true low valley still in 1s. */
    a->pcm_count = 4800;
    for (unsigned n = 1; n < 5000; ++n) new_level(a, n);
    new_level(a, 1000000); assert(!atomic_load(&a->trimmed_samples));
    new_level(a, 1001000); assert(atomic_load(&a->trimmed_samples) == 480);
    x4_live_audio_stop(a);
    puts("PASS fast renders cannot evict low valley or falsely trigger trimming");
    (void)decoded;
}
static void test_open_fallback(void)
{
    X4LiveAudio *a = audio(false); X4LiveAudioSnapshot s;
    x4_live_audio_snapshot(a, &s);
    assert(open_calls == 1 && open_sizes[0] == 512 && s.output_block_samples == 512);
    assert(s.open_512_attempted && s.open_512_rc == 7 && !s.open_1024_attempted);
    x4_live_audio_stop(a);
    reject_512 = -512; a = audio(false); x4_live_audio_snapshot(a, &s);
    assert(open_calls == 2 && open_sizes[0] == 512 && open_sizes[1] == 1024);
    assert(s.output_block_samples == 1024 && s.open_512_rc == -512 && s.open_1024_rc == 7);
    assert(s.open_512_attempted && s.open_1024_attempted);
    worker_audio = a; output_calls = 0; a->pcm_count = AUDIO_TARGET; play_new(a);
    assert(output_calls == 1 && a->pcm_count == AUDIO_TARGET - AUDIO_BLOCK);
    x4_live_audio_stop(a);
    a = audio(true); assert(open_calls == 1 && open_sizes[0] == 1024 && !a->open_512_attempted); x4_live_audio_stop(a);
    reject_1024 = -1024; open_calls = 0; int rc = 0;
    a = x4_live_audio_start_mode(&rc, 111, false);
    assert(!a && rc == -1024 && open_calls == 2);
    reject_512 = reject_1024 = 0;
    puts("PASS Open512 accepted, rejection falls back1024 with exact rc; previous only1024; both fail safe");
}
static void test_burst_and_metrics(void)
{
    X4LiveAudio *a = audio(false); int16_t decoded[5760 * 2] = {0};
    a->pcm_count = 4800; new_level(a, 1);
    a->pcm_count = 3840; new_level(a, 100001);
    a->pcm_count = 1920; new_level(a, 200001);
    a->pcm_count = 0;
    new_append(a, decoded, 5760); new_append(a, decoded, 2880);
    assert(atomic_load(&a->pcm_overflows) == 1 && !atomic_load(&a->bad_frames));
    new_underflow(a, 1000000); new_underflow(a, 1020000);
    X4LiveAudioSnapshot s; x4_live_audio_snapshot(a, &s);
    assert(s.underflows == 2 && s.underflow_episodes == 1 && s.underflow_interval_count == 1);
    assert(s.underflow_interval_min_us == 20000 && s.underflow_interval_max_us == 20000);
    assert(s.buffer_observations == 3 && s.buffer_max_us == 100000);
    assert(s.target_observations == 3 && s.target_p50_us == 50000 && s.target_max_us == 60000 && s.target_adjustments == 1);
    assert(s.output_block_samples == 512 && s.open_512_attempted && s.open_512_rc == 7);
    atomic_store(&a->underflow_interval_min_us, UINT64_MAX);
    x4_live_audio_snapshot(a, &s); assert(s.underflow_interval_min_us == 0);
    atomic_store(&a->underflow_interval_min_us, 20000);
    x4_live_audio_snapshot(a, &s);
    FILE *f = tmpfile(); assert(f); x4_live_audio_metrics_write(f, &s); rewind(f);
    char json[4096] = {0}; assert(fread(json, 1, sizeof(json)-1, f)); fclose(f);
    assert(strstr(json, "\"fec_implemented\":true") && strstr(json, "\"pcm_overflows\":1"));
    printf("AUDIO_METRICS_JSON=%s\n", json);
    x4_live_audio_stop(a);
    puts("PASS valley/target/block/Open metrics,overflow separate,atomic snapshot/JSON");
}
static void test_stall_buffer_growth(void)
{
    X4LiveAudio *a = audio(false); int16_t decoded[5760 * 2] = {0};
    a->have_started = true; atomic_store(&a->playing, true);
    /* Establish steady state with buffer around 15 ms (720 samples). */
    a->pcm_count = 720;
    a->expected_timestamp = 0; a->timestamp_valid = true;
    uint32_t ts = 0;
    uint64_t now = 100000;

    /* First stall: 40 ms without incoming packets.
     * In 40 ms (40000 us), AudioOut consumes 3 blocks of 512 samples.
     * With only 720 samples in the buffer:
     * - Render 1 (10.67 ms): 512 samples consumed (208 remain).
     * - Render 2 (21.33 ms): needs 512, only 208 real -> triggers underflow, 304 PLC.
     * - Render 3 (32.00 ms): needs 512, 0 real -> 512 PLC. */
    assert(atomic_load(&a->underflow_episodes) == 0);
    new_render(a, decoded, now + 10667u);
    new_render(a, decoded, now + 21334u);
    new_render(a, decoded, now + 32001u);

    /* 1 underflow episode has occurred during the 40 ms stall: */
    assert(atomic_load(&a->underflow_episodes) == 1);
    uint64_t underflows1 = atomic_load(&a->underflows);
    assert(underflows1 > 0);

    /* Burst of delayed packets arrives at the end of the 40 ms stall:
     * 2 packets of 20 ms (960 samples each = 1920 samples). */
    packet(a, ts, 960, decoded); ts += 960;
    packet(a, ts, 960, decoded); ts += 960;

    /* After the burst, the buffer is higher (grown from ~15 ms to >40 ms),
     * but does not exceed target (2880 samples): */
    unsigned target = atomic_load(&a->target_samples);
    assert(a->pcm_count == 2064);
    assert(a->pcm_count > 720 && a->pcm_count <= target);

    /* Second identical 40 ms stall:
     * Same 3 renders of 512 samples (total 1536 samples consumed).
     * Since buffer has 2064 samples, all renders have full real audio (528 samples remain). */
    now += 40000u;
    new_render(a, decoded, now + 10667u);
    new_render(a, decoded, now + 21334u);
    new_render(a, decoded, now + 32001u);
    assert(a->pcm_count == 2064 - 1536);

    /* The second identical stall does NOT produce any underflow: */
    assert(atomic_load(&a->underflow_episodes) == 1);
    assert(atomic_load(&a->underflows) == underflows1);

    /* Delayed burst arrives after second stall: */
    packet(a, ts, 960, decoded); ts += 960;
    packet(a, ts, 960, decoded); ts += 960;
    assert(a->pcm_count == 528 + 1920 && a->pcm_count <= target);

    x4_live_audio_stop(a);
    puts("PASS stall40ms underflow->buffer grows to target headroom; identical second stall no underflow");
}
int main(void)
{
    test_threshold_and_legacy(); test_jitter(); test_loss_plc_and_credit(); test_long_declared_gap();
    test_rebuffer_does_not_replay_old_silence(); test_joined_final_snapshot_and_close_retention();
    test_valley_target_and_bounded_trim(); test_open_fallback(); test_stall_buffer_growth(); test_burst_and_metrics();
    return 0;
}
