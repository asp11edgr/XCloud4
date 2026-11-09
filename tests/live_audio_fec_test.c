/* SPDX-License-Identifier: GPL-3.0-only */
/* Real audio decode and RTP reorder; simulated clock/thread/AudioOut only. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include X4_MEDIA_IMPLEMENTATION
#pragma GCC diagnostic pop
#include X4_AUDIO_IMPLEMENTATION
#include <assert.h>
#include <math.h>
#include <stdarg.h>

enum { RATE = 48000, CHANNELS = 2, PACKET_SAMPLES = 960, PACKET_US = 20000,
       PACKETS = 500, CLIP_SAMPLES = PACKETS * PACKET_SAMPLES,
       RTP_HEADER = 12, PAYLOAD_MAX = 1275, INITIAL_US = 1000000 };
typedef struct { size_t size; uint8_t data[RTP_HEADER + PAYLOAD_MAX]; } Packet;
typedef struct {
    const char *name;
    unsigned groups, run;
    bool fec_encoder, late;
} Case;
static int16_t original[CLIP_SAMPLES * CHANNELS], played[CLIP_SAMPLES * CHANNELS];
static int16_t clean_decode[2][CLIP_SAMPLES * CHANNELS];
static Packet tapes[2][PACKETS];
static bool losses[PACKETS];
static unsigned lookahead[2], aligned_delay[2], late_index;
static uint64_t clock_us, first_audio;
static unsigned arrivals, outputs;
static X4LiveAudio *audio;
static void *(*worker)(void *);

static int32_t out_init(void) { return 0; }
static int32_t out_open(OrbisUserServiceUserId user, enum OrbisAudioOutPort port,
    int32_t index, uint32_t samples, uint32_t rate, uint32_t format)
{
    (void)user; (void)port; (void)index; (void)format;
    assert(samples == AUDIO_NEW_BLOCK && rate == RATE);
    return 7;
}
static int32_t out_write(int32_t handle, const void *data)
{
    assert(handle == 7);
    if (data) {
        assert(++outputs == 1);
        first_audio = clock_us - INITIAL_US;
        atomic_store(&audio->stop, true);
    }
    return 0;
}
static int32_t out_close(int32_t handle) { assert(handle == 7); return 0; }
int x4_module_open(const char *name) { assert(!strcmp(name, "libSceAudioOut")); return 1; }
int x4_module_symbol(int handle, const char *name, void **value)
{
    assert(handle == 1);
    if (!strcmp(name, "sceAudioOutInit")) *value = (void *)out_init;
    else if (!strcmp(name, "sceAudioOutOpen")) *value = (void *)out_open;
    else if (!strcmp(name, "sceAudioOutOutput")) *value = (void *)out_write;
    else if (!strcmp(name, "sceAudioOutClose")) *value = (void *)out_close;
    else assert(0);
    return 0;
}
int x4_log_printf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vprintf(fmt, args);
    va_end(args);
    return n;
}
int scePthreadCreate(OrbisPthread *thread, const pthread_attr_t *attrs,
    void *(*fn)(void *), void *opaque, const char *name)
{
    (void)thread; (void)attrs; (void)opaque;
    assert(!strcmp(name, "x4-opus"));
    worker = fn;
    return 0;
}
int scePthreadJoin(OrbisPthread thread, void **result)
{ (void)thread; (void)result; return 0; }
uint64_t sceKernelGetProcessTime(void) { return clock_us; }
int sceKernelUsleep(unsigned delay)
{
    clock_us += delay;
    assert(clock_us <= INITIAL_US + 150000);
    while (arrivals < 8 && INITIAL_US + arrivals * PACKET_US <= clock_us) {
        const Packet *packet = &tapes[1][arrivals++];
        x4_live_audio_receive(audio, packet->data, packet->size);
    }
    return 0;
}

static void generate_signal(void)
{
    const double tau = 6.283185307179586;
    double phase = 0.0;
    for (unsigned i = 0; i < CLIP_SAMPLES; ++i) {
        double t = (double)i / RATE;
        double pitch = 137.0 + 29.0 * sin(tau * 1.21 * t) + 22.0 * sin(tau * 4.43 * t);
        phase += tau * pitch / RATE;
        double envelope = 0.59 + 0.35 * sin(tau * 3.09 * t);
        double first = 0.60 * sin(phase), second = 0.20 * sin(2.0 * phase);
        double third = (0.14 + 0.08 * sin(tau * 2.21 * t)) * sin(3.0 * phase);
        double vowel = 0.06 * sin(5.0 * phase) + 0.03 * sin(9.0 * phase);
        original[i * CHANNELS] = (int16_t)lrint(23000.0 * envelope * (first + second + third + vowel));
        original[i * CHANNELS + 1] = (int16_t)lrint(21000.0 * envelope * (first + second - 0.4 * third + vowel));
    }
}
static void put32be(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (24 - 8 * i));
}
static void encode(bool fec)
{
    int rc = -1, delay = 0;
    OpusEncoder *encoder = opus_encoder_create(RATE, CHANNELS, OPUS_APPLICATION_VOIP, &rc);
    assert(encoder && rc == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(28000)) == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_FORCE_CHANNELS(CHANNELS)) == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND)) == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND)) == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(fec ? 1 : 0)) == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10)) == OPUS_OK);
    assert(opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&delay)) == OPUS_OK);
    assert(delay > 0 && delay < PACKET_SAMPLES);
    lookahead[fec] = (unsigned)delay;
    unsigned lbrr = 0;
    for (unsigned i = 0; i < PACKETS; ++i) {
        Packet *p = &tapes[fec][i];
        int bytes = opus_encode(encoder, &original[i * PACKET_SAMPLES * CHANNELS],
            PACKET_SAMPLES, p->data + RTP_HEADER, PAYLOAD_MAX);
        assert(bytes > 0);
        p->data[0] = 0x80;
        p->data[1] = 111;
        p->data[2] = (uint8_t)(i >> 8);
        p->data[3] = (uint8_t)i;
        put32be(p->data + 4, i * PACKET_SAMPLES);
        put32be(p->data + 8, UINT32_C(0x37373737));
        p->size = RTP_HEADER + (size_t)bytes;
        int has_lbrr = opus_packet_has_lbrr(p->data + RTP_HEADER, bytes);
        assert(has_lbrr == 0 || has_lbrr == 1);
        lbrr += (unsigned)has_lbrr;
        if (fec && has_lbrr && i >= 61 && !late_index) late_index = i - 1;
    }
    assert(fec ? lbrr && late_index : !lbrr);
    printf("ENCODER fec=%d packets=%u lbrr=%u delay_samples=%u\n", fec, PACKETS, lbrr, lookahead[fec]);
    opus_encoder_destroy(encoder);
}
static void calibrate(bool fec)
{
    int rc = -1;
    OpusDecoder *decoder = opus_decoder_create(RATE, CHANNELS, &rc);
    assert(decoder && rc == OPUS_OK);
    int16_t *clean = clean_decode[fec];
    for (unsigned i = 0; i < PACKETS; ++i) {
        const Packet *p = &tapes[fec][i];
        int frames = opus_decode(decoder, p->data + RTP_HEADER, (opus_int32)(p->size - RTP_HEADER),
            clean + i * PACKET_SAMPLES * CHANNELS, PACKET_SAMPLES, 0);
        assert(frames == PACKET_SAMPLES);
    }
    opus_decoder_destroy(decoder);

    /* Align once from an independent clean decode. Every loss pattern and
     * both implementations then use the same delay for this encoded tape. */
    double best = -2.0, nominal = 0.0;
    for (unsigned lag = 0; lag <= PACKET_SAMPLES; ++lag) {
        double covariance = 0, reference_power = 0, decoded_power = 0;
        for (unsigned i = 2 * PACKET_SAMPLES; i < CLIP_SAMPLES; i += 16) {
            for (unsigned ch = 0; ch < CHANNELS; ++ch) {
                double ref = original[(i - lag) * CHANNELS + ch];
                double got = clean[i * CHANNELS + ch];
                covariance += ref * got;
                reference_power += ref * ref;
                decoded_power += got * got;
            }
        }
        double correlation = covariance / sqrt(reference_power * decoded_power);
        if (lag == lookahead[fec]) nominal = correlation;
        if (correlation > best) { best = correlation; aligned_delay[fec] = lag; }
    }
    assert(aligned_delay[fec] > 0 && aligned_delay[fec] < PACKET_SAMPLES);
    printf("ALIGNMENT fec=%d lookahead=%u measured=%u correlation_nominal=%.9f correlation_measured=%.9f\n",
        fec, lookahead[fec], aligned_delay[fec], nominal, best);
}

static X4LiveAudio *start(void)
{
    int rc = -1;
    X4LiveAudio *a = x4_live_audio_start_mode(&rc, 111, false);
    assert(a && rc == 0 && worker == play_new);
    return a;
}
static void test_first_audio(void)
{
    clock_us = INITIAL_US;
    first_audio = 0;
    arrivals = outputs = 0;
    audio = start();
    const Packet *p = &tapes[1][arrivals++];
    x4_live_audio_receive(audio, p->data, p->size);
    assert(!worker(audio));
    X4LiveAudioSnapshot s;
    assert(!x4_live_audio_stop_report(audio, &s));
    audio = NULL;
    assert(outputs == 1 && arrivals == 3 && s.frames == 3);
    assert(first_audio == 40000 && s.target_current_samples == 2400);
    assert(s.output_block_samples == 512 && !s.underflows && !s.plc_samples && !s.lost);
    printf("LATENCY first_audio_us=%" PRIu64 " target_samples=%u block_samples=%u\n",
        first_audio, s.target_current_samples, s.output_block_samples);
}

static void mark_losses(const Case *test)
{
    memset(losses, 0, sizeof(losses));
    if (test->late) { losses[late_index] = true; return; }
    for (unsigned group = 0; group < test->groups; ++group) {
        unsigned at = 16 + group * (PACKETS - 32) / test->groups;
        for (unsigned j = 0; j < test->run; ++j) losses[at + j] = true;
    }
}
static int receive_reorder(X4LiveAudio *a, const Packet *p, uint64_t now)
{
    X4LiveRtp r;
    assert(x4_live_rtp_parse(p->data, p->size, &r) == 0);
    assert(x4_live_track_accept(&a->track, &r) >= 0);
    return x4_live_reorder_insert(&a->reorder, p->data, p->size, r.sequence, now);
}
static void capture_pcm(X4LiveAudio *a, unsigned *samples)
{
    /* Record the exact PCM enqueued by new_decode_packet. The separate
     * AudioOut test checks actual startup without draining this FIFO early. */
    assert(*samples + a->pcm_count <= CLIP_SAMPLES);
    while (a->pcm_count) {
        memcpy(&played[*samples * CHANNELS], &a->pcm[a->pcm_head * CHANNELS], CHANNELS * sizeof(*played));
        ++*samples;
        a->pcm_head = (a->pcm_head + 1) % PCM_CAPACITY;
        --a->pcm_count;
    }
}
static void drain_reorder(X4LiveAudio *a, uint64_t now, unsigned *samples)
{
    int16_t scratch[5760 * CHANNELS];
    for (;;) {
        size_t size;
        uint32_t lost;
        const uint8_t *p = x4_live_reorder_next(&a->reorder, now, &size, &lost);
        if (!p) return;
        atomic_fetch_add(&a->lost, lost);
        X4LiveRtp r;
        assert(x4_live_rtp_parse(p, size, &r) == 0);
        new_decode_packet(a, &r, scratch);
        capture_pcm(a, samples);
    }
}
static void write_pcm(const char *out, const char *name)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s.pcm", out, name);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(played, sizeof(*played), CLIP_SAMPLES * CHANNELS, f) == CLIP_SAMPLES * CHANNELS);
    assert(!fclose(f));
}
static void measure(const Case *test, const char *out)
{
    mark_losses(test);
    X4LiveAudio *a = start();
    clock_us = INITIAL_US;
    unsigned samples = 0, missing = 0, late = 0;
    for (unsigned i = 0; i < PACKETS + 3; ++i) {
        uint64_t now = INITIAL_US + i * PACKET_US;
        drain_reorder(a, now, &samples);
        if (i < PACKETS) {
            if (losses[i]) ++missing;
            else assert(receive_reorder(a, &tapes[test->fec_encoder][i], now) == X4_LIVE_REORDER_STORED);
        }
        if (test->late && i == late_index + 5) {
            X4LiveAudioSnapshot before;
            x4_live_audio_snapshot(a, &before);
            assert(before.lost == 1 && before.fec_packets == (X4_FEC_BASELINE ? 0u : 1u));
            assert(receive_reorder(a, &tapes[1][late_index], now) == X4_LIVE_REORDER_LATE);
            ++late;
        }
        drain_reorder(a, now, &samples);
    }
    uint64_t attempts = 0, recovered_samples = 0, lbrr_packets = 0;
#if !X4_FEC_BASELINE
    attempts = atomic_load(&a->fec_attempts);
    recovered_samples = atomic_load(&a->fec_recovered_samples);
    lbrr_packets = atomic_load(&a->lbrr_packets);
#endif
    X4LiveAudioSnapshot s;
    assert(!x4_live_audio_stop_report(a, &s));
    assert(samples == CLIP_SAMPLES && s.frames == PACKETS - missing && s.lost == missing);
    assert(!s.decode_errors && !s.pcm_overflows && !s.bad_frames);
    assert(s.fec_implemented == !X4_FEC_BASELINE);
    assert(s.fec_packets <= missing);
    if (X4_FEC_BASELINE || !test->fec_encoder || !missing) assert(!s.fec_packets);
    assert(late == (unsigned)test->late);
    if (!missing) {
        assert(!memcmp(played, clean_decode[test->fec_encoder], sizeof(played)));
        puts("PASS lossless live_audio PCM equals independent libopus decode");
    }

    /* Score the original (once-aligned) synthetic input and the independent
     * decoded control. The second metric isolates loss recovery from coding. */
    double signal_total = 0, noise_total = 0, signal_lost = 0, noise_lost = 0;
    double clean_total = 0, clean_noise_total = 0, clean_lost = 0, clean_noise_lost = 0;
    unsigned delay = aligned_delay[test->fec_encoder];
    for (unsigned i = delay; i < CLIP_SAMPLES; ++i) {
        for (unsigned ch = 0; ch < CHANNELS; ++ch) {
            double ref = original[(i - delay) * CHANNELS + ch];
            double decoded_ref = clean_decode[test->fec_encoder][i * CHANNELS + ch];
            double actual = played[i * CHANNELS + ch];
            double err = ref - actual;
            double decoded_err = decoded_ref - actual;
            signal_total += ref * ref;
            noise_total += err * err;
            clean_total += decoded_ref * decoded_ref;
            clean_noise_total += decoded_err * decoded_err;
            if (losses[i / PACKET_SAMPLES]) {
                signal_lost += ref * ref;
                noise_lost += err * err;
                clean_lost += decoded_ref * decoded_ref;
                clean_noise_lost += decoded_err * decoded_err;
            }
        }
    }
    printf("CASE %s missing=%u recovered=%" PRIu64 " plc_samples=%" PRIu64
        " attempts=%" PRIu64 " recovered_samples=%" PRIu64 " lbrr=%" PRIu64
        " late=%u samples=%u snr=%.9f snr_lost=%.9f clean_snr=%.9f clean_snr_lost=%.9f\n",
        test->name, missing, s.fec_packets,
        s.plc_samples, attempts, recovered_samples, lbrr_packets, late, samples,
        10.0 * log10(signal_total / noise_total),
        missing ? 10.0 * log10(signal_lost / noise_lost) : 0.0,
        missing ? 10.0 * log10(clean_total / clean_noise_total) : 0.0,
        missing ? 10.0 * log10(clean_lost / clean_noise_lost) : 0.0);
    if (!missing || !test->fec_encoder) write_pcm(out, test->name);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    assert(!strcmp(opus_get_version_string(), argv[2]));
    printf("LIBOPUS %s base=%d sample_rate=%u channels=%u packet_samples=%u packets=%u\n",
        opus_get_version_string(), X4_FEC_BASELINE, RATE, CHANNELS, PACKET_SAMPLES, PACKETS);
    generate_signal();
    encode(false);
    encode(true);
    calibrate(false);
    calibrate(true);
    test_first_audio();
    static const Case cases[] = {
        { "no_loss", 0, 0, true, false },
        { "single_1pct", 5, 1, true, false },
        { "single_3pct", 15, 1, true, false },
        { "single_5pct", 25, 1, true, false },
        { "pairs", 12, 2, true, false },
        { "burst3", 8, 3, true, false },
        { "late", 0, 0, true, true },
        { "no_fec_5pct", 25, 1, false, false },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) measure(&cases[i], argv[1]);
    return 0;
}
