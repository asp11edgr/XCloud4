/* SPDX-License-Identifier: GPL-3.0-only */
/* Native WebRTC and AudioIn are test doubles. Exercise the production RTC
 * offer/answer gate and sender without a network or any real credentials. */
#define X4_MIC_HOST_TEST 1
#include "../src/streaming/rtc_transport.c"
#include <assert.h>
#include <errno.h>

static X4MicRequest request;
static unsigned random_calls, thread_calls, joined, attaches, detaches, rejects, sent_rtp, sent_rtcp, taken;
static uint64_t clock_us;
static X4Rtc *fixture;
static bool open_track, mute, rng_error;

X4MicRequest x4_mic_request(void) { return request; }
int x4_native_random(void *data, size_t size)
{
    ++random_calls;
    if (rng_error) return -1;
    assert(size == 10);
    uint8_t *out = data;
    for (size_t i = 0; i < size; ++i) out[i] = (uint8_t)(random_calls + i);
    return 0;
}
bool x4_mic_capture_attach(uint64_t generation, uint64_t peer)
{
    assert(generation == request.generation && peer == fixture->nack_epoch && open_track);
    ++attaches;
    return true;
}
void x4_mic_capture_detach(uint64_t generation, uint64_t peer)
{
    assert(generation == request.generation && peer == fixture->nack_epoch);
    ++detaches;
}
void x4_mic_capture_reject(uint64_t generation)
{
    assert(generation == request.generation);
    ++rejects;
}
bool x4_mic_capture_take(uint64_t generation, uint64_t peer, uint64_t now, X4MicFrame *frame)
{
    assert(generation == request.generation && peer == fixture->nack_epoch && now == clock_us);
    unsigned index = taken++;
    if (index >= 4) {
        if (index > 4) atomic_store(&fixture->mic_stop, true);
        return false;
    }
    *frame = (X4MicFrame){.captured_us = now, .sample_us = now,
        .epoch = index >= 3 ? 2 : 1, .samples = fixture->mic_clock.frame_samples,
        .size = 4, .data = {0x12, 0x34, 0x56, 0x78}};
    mute = index == 2;
    clock_us += fixture->mic_clock.frame_samples / 48u * 1000u;
    if (index == 3) clock_us += 6000000;
    return true;
}
bool x4_mic_capture_send_allowed(uint64_t generation, uint64_t peer, uint64_t epoch)
{
    assert(generation == request.generation && peer == fixture->nack_epoch && (epoch == 1 || epoch == 2));
    return !mute;
}
bool rtcIsOpen(int id) { assert(id == fixture->audio); return open_track; }
uint64_t sceKernelGetProcessTime(void) { return clock_us; }
int sceKernelUsleep(unsigned delay) { clock_us += delay; return 0; }
int scePthreadAttrInit(OrbisPthreadAttr *attr) { return pthread_attr_init(attr); }
int scePthreadAttrSetstacksize(OrbisPthreadAttr *attr, size_t size)
{
    assert(size == 256u * 1024u);
    return pthread_attr_setstacksize(attr, size);
}
int scePthreadAttrDestroy(OrbisPthreadAttr *attr) { return pthread_attr_destroy(attr); }
int scePthreadCreate(OrbisPthread *thread, const pthread_attr_t *attr, void *(*entry)(void *), void *arg, const char *name)
{
    (void)thread; (void)attr;
    assert(entry == microphone_sender && arg == fixture && !strcmp(name, "x4-mic-rtp"));
    ++thread_calls;
    return 0;
}
int scePthreadJoin(OrbisPthread thread, void **value) { (void)thread; (void)value; ++joined; return 0; }
#if !X4_PUBLIC
int x4_log_printf(const char *format, ...) { assert(strstr(format, "XCloud4: microphone ")); return 0; }
#endif

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int rtcSendMessage(int id, const char *data, int size)
{
    assert(id == fixture->audio);
    const uint8_t *packet = (const uint8_t *)data;
    if (packet[1] == 200) {
        assert(size == X4_MIC_RTCP_SR_BYTES && be32(packet + 20) == 3 && be32(packet + 24) == 12);
        assert(be32(packet + 4) == fixture->mic_clock.seed.ssrc && packet[29] == 202);
        ++sent_rtcp;
    } else {
        assert(size == 16 && (packet[1] & 0x7fu) == 111 && be32(packet + 8) == fixture->mic_clock.seed.ssrc);
        unsigned seq = ((unsigned)packet[2] << 8) | packet[3];
        assert(seq == (uint16_t)(fixture->mic_clock.seed.sequence + sent_rtp));
        uint32_t step = fixture->mic_clock.frame_samples;
        assert(be32(packet + 4) == fixture->mic_clock.seed.timestamp + (sent_rtp < 2 ? sent_rtp : 3u) * step);
        assert(((packet[1] & 0x80u) != 0) == (sent_rtp != 1));
        ++sent_rtp;
    }
    return 0;
}

static const char yes[] = "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\n"
    "a=rtpmap:111 opus/48000/2\r\na=recvonly\r\n";
static const char no[] = "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\n"
    "a=rtpmap:111 opus/48000/2\r\na=sendonly\r\n";

static rtcTrackInit t41_audio(void)
{
    return (rtcTrackInit){.direction=RTC_DIRECTION_RECVONLY,.codec=RTC_CODEC_OPUS,.payloadType=111,.mid="0",
        .profile="minptime=10;useinbandfec=1;stereo=1"};
}

int main(void)
{
    fixture = calloc(1, sizeof(*fixture));
    assert(fixture);
    fixture->audio = 41;
    fixture->nack_epoch = 1;
    atomic_init(&fixture->state, X4_RTC_CONNECTED);
    rtcTrackInit base = t41_audio(), audio;
    memcpy(&audio, &base, sizeof(audio));
    microphone_offer(fixture, &audio);
    assert(!memcmp(&base, &audio, sizeof(base)) && !random_calls);
    microphone_answer(fixture, yes, 0);
    assert(!thread_calls && !rejects && !microphone_close(fixture));

    request = (X4MicRequest){.generation=12, .frame_ms=20};
    rng_error = true;
    microphone_offer(fixture, &audio);
    assert(!memcmp(&base, &audio, sizeof(base)) && random_calls == 1 && rejects == 1);
    rng_error = false;
    microphone_offer(fixture, &audio);
    assert(audio.direction == RTC_DIRECTION_SENDRECV && audio.ssrc && !strcmp(audio.name, "audio"));
    audio.direction = base.direction;
    audio.ssrc = base.ssrc;
    audio.name = base.name;
    assert(!memcmp(&base, &audio, sizeof(base)) && random_calls == 2);
    assert(fixture->mic_clock.frame_samples == (X4_PUBLIC ? 480 : 960));

    microphone_answer(fixture, yes, -1);
    microphone_answer(fixture, no, 0);
    assert(rejects == 3 && !thread_calls);
    microphone_answer(fixture, yes, 0);
    assert(thread_calls == 1 && fixture->mic_thread_started && !attaches);
    /* Before audio-track open the sender cannot capture or send. */
    microphone_sender(fixture);
    assert(!attaches && !sent_rtp && rejects == 4);
    assert(!microphone_close(fixture) && joined == 1 && detaches == 1);

    microphone_offer(fixture, &audio);
    microphone_answer(fixture, yes, 0);
    open_track = true;
    microphone_sender(fixture);
    assert(attaches == 1 && sent_rtp == 3 && sent_rtcp == 1 && detaches == 2);
    assert(!microphone_close(fixture) && joined == 2);

    microphone_offer(fixture, &audio);
    microphone_answer(fixture, yes, 0);
    assert(microphone_close(fixture) == RTC_ERR_NOT_AVAIL && fixture->mic_thread_started);
    atomic_store(&fixture->mic_done, true);
    assert(!microphone_close(fixture) && joined == 3 && !fixture->mic_thread_started);
    free(fixture);
    puts("PASS T34 mic transport: T41 track off byte-exact, on-only direction/SSRC/CNAME, answer/open gates, mute, raw RTP/SR, close retention, edition frame policy");
}
