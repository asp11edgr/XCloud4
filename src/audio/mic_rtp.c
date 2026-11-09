/* SPDX-License-Identifier: GPL-3.0-only */
#include "mic_rtp.h"
#include <stdatomic.h>
#include <string.h>

static uint32_t read32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

bool x4_mic_rtp_seed(X4MicRtpSeed *seed, int (*random_bytes)(void *, size_t))
{
    *seed = (X4MicRtpSeed){0};
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t random[10];
        if (random_bytes(random, sizeof(random))) return false;
        uint32_t ssrc = read32(random);
        if (!ssrc) continue;
        *seed = (X4MicRtpSeed){ssrc, read32(random + 4), (uint16_t)((random[8] << 8) | random[9])};
        return true;
    }
    return false;
}

bool x4_mic_rtp_header(X4MicRtpClock *clock, uint64_t sample_us, uint64_t epoch,
    uint8_t header[X4_MIC_RTP_HEADER])
{
    if (!clock->seed.ssrc || (clock->frame_samples != 480 && clock->frame_samples != 960)) return false;
    bool marker = !clock->started || clock->epoch != epoch;
    if (!clock->started) {
        clock->first_sample_us = sample_us;
        clock->started = true;
    } else {
        uint64_t elapsed = sample_us >= clock->first_sample_us ? sample_us - clock->first_sample_us : 0;
        unsigned frame_us = clock->frame_samples == 480 ? 10000u : 20000u;
        uint64_t index = elapsed / frame_us + (elapsed % frame_us >= frame_us / 2u);
        if (index <= clock->frame_index) index = clock->frame_index + 1u;
        marker |= index > clock->frame_index + 1u;
        clock->frame_index = index;
    }
    clock->epoch = epoch;
    header[0] = 0x80;
    header[1] = (uint8_t)(111u | (marker ? 0x80u : 0u));
    header[2] = (uint8_t)(clock->seed.sequence >> 8);
    header[3] = (uint8_t)clock->seed.sequence++;
    write32(header + 4, clock->seed.timestamp + (uint32_t)clock->frame_index * clock->frame_samples);
    write32(header + 8, clock->seed.ssrc);
    return true;
}

uint32_t x4_mic_rtp_time_at(const X4MicRtpClock *clock, uint64_t now_us)
{
    uint64_t elapsed = now_us >= clock->first_sample_us ? now_us - clock->first_sample_us : 0;
    uint64_t ticks = (elapsed / 1000u) * 48u + (elapsed % 1000u) * 48u / 1000u;
    return clock->seed.timestamp + (uint32_t)ticks;
}

void x4_mic_rtcp_sender_report(uint8_t packet[X4_MIC_RTCP_SR_BYTES], uint32_t ssrc,
    uint64_t ntp, uint32_t rtp_timestamp, uint32_t packets, uint32_t payload_bytes)
{
    memset(packet, 0, X4_MIC_RTCP_SR_BYTES);
    packet[0] = 0x80; packet[1] = 200; packet[3] = 6; /* SR, 28 B. */
    write32(packet + 4, ssrc);
    write32(packet + 8, (uint32_t)(ntp >> 32));
    write32(packet + 12, (uint32_t)ntp);
    write32(packet + 16, rtp_timestamp);
    write32(packet + 20, packets);
    write32(packet + 24, payload_bytes);
    packet[28] = 0x81; packet[29] = 202; packet[31] = 3; /* One SDES chunk, 16 B. */
    write32(packet + 32, ssrc);
    packet[36] = 1; packet[37] = 5;
    memcpy(packet + 38, "audio", 5);
}

typedef struct { const char *text; size_t size; } Span;
static bool equal(Span span, const char *literal)
{
    size_t size = strlen(literal);
    return span.size == size && !memcmp(span.text, literal, size);
}

static Span word(Span *line)
{
    while (line->size && (line->text[0] == ' ' || line->text[0] == '\t')) {
        ++line->text;
        --line->size;
    }
    size_t n = 0;
    while (n < line->size && line->text[n] != ' ' && line->text[n] != '\t') ++n;
    Span value = {line->text, n};
    line->text += n;
    line->size -= n;
    return value;
}

static bool audio_media(Span rest)
{
    Span port = word(&rest);
    unsigned value = 0;
    for (size_t i = 0; i < port.size; ++i) {
        unsigned digit = (unsigned)(port.text[i] - '0');
        if (digit > 9 || value > (65535u - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    if (!value || !equal(word(&rest), "UDP/TLS/RTP/SAVPF")) return false;
    while (rest.size) if (equal(word(&rest), "111")) return true;
    return false;
}

static bool opus_mapping(Span line)
{
    static const char fmt[] = "opus/48000/2";
    if (line.size != sizeof(fmt) - 1) return false;
    for (size_t i = 0; i < sizeof(fmt) - 1; ++i) {
        unsigned char value = (unsigned char)line.text[i];
        if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
        if (value != (unsigned char)fmt[i]) return false;
    }
    return true;
}

enum Direction { IMPLICIT, ACCEPTS, REFUSES, CONFLICT };
static enum Direction direction(Span line)
{
    if (equal(line, "a=recvonly") || equal(line, "a=sendrecv")) return ACCEPTS;
    if (equal(line, "a=sendonly") || equal(line, "a=inactive")) return REFUSES;
    return IMPLICIT;
}

typedef struct {
    bool audio, port_and_pt, mid_seen, mid_zero, mapping, ambiguous;
    enum Direction mode;
} Media;

static bool finish_media(Media media, enum Direction session, unsigned *mid_zero, bool *accepted)
{
    if (!media.mid_zero) return true;
    if (++*mid_zero != 1 || media.ambiguous || media.mode == CONFLICT) return false;
    enum Direction mode = media.mode == IMPLICIT ? session : media.mode;
    *accepted = media.audio && media.port_and_pt && media.mapping && (mode == IMPLICIT || mode == ACCEPTS);
    return true;
}

bool x4_mic_sdp_can_send(const char *sdp, size_t size)
{
    if (!sdp || !size || size >= 32768u) return false;
    enum Direction session = IMPLICIT;
    bool in_media = false, accepted = false;
    unsigned mid_zero = 0;
    Media media = {0};
    for (size_t at = 0; at < size;) {
        const char *newline = memchr(sdp + at, '\n', size - at);
        size_t length = newline ? (size_t)(newline - sdp - at) : size - at;
        Span line = {sdp + at, length};
        at += length + (newline != NULL);
        if (line.size && line.text[line.size - 1] == '\r') --line.size;
        if (line.size >= 2 && !memcmp(line.text, "m=", 2)) {
            if (!finish_media(media, session, &mid_zero, &accepted)) return false;
            in_media = true;
            Span rest = {line.text + 2, line.size - 2};
            bool audio = equal(word(&rest), "audio");
            media = (Media){.audio = audio, .port_and_pt = audio && audio_media(rest)};
        } else if (in_media && line.size >= 6 && !memcmp(line.text, "a=mid:", 6)) {
            media.ambiguous |= media.mid_seen;
            media.mid_seen = true;
            media.mid_zero |= equal(line, "a=mid:0");
        } else if (in_media && media.audio && line.size >= 9 && !memcmp(line.text, "a=rtpmap:", 9)) {
            Span rest = {line.text + 9, line.size - 9};
            if (equal(word(&rest), "111")) {
                media.ambiguous |= media.mapping || !opus_mapping(word(&rest));
                media.mapping = true;
            }
        } else {
            enum Direction mode = direction(line);
            if (mode != IMPLICIT) {
                enum Direction *target = in_media ? &media.mode : &session;
                *target = *target == IMPLICIT ? mode : CONFLICT;
            }
        }
    }
    return finish_media(media, session, &mid_zero, &accepted) && accepted;
}

typedef struct {
    atomic_uint ssrc, reports, last_sequence;
    atomic_uint_fast64_t loss_jitter;
} ReceiverSlot;

_Static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2,
    "The authenticated RTCP observer must use lock-free atomics");
static ReceiverSlot receivers[X4_MIC_RR_SLOTS];
/* Index+1. Slots are retired permanently; late observers cannot affect a
 * new peer or touch storage with a shorter lifetime than the callback. */
static atomic_uint current_receiver;

bool x4_mic_rr_bind(unsigned slot, uint32_t ssrc)
{
    if (slot >= X4_MIC_RR_SLOTS || !ssrc) return false;
    unsigned empty = 0;
    if (!atomic_compare_exchange_strong_explicit(&receivers[slot].ssrc, &empty, ssrc,
            memory_order_relaxed, memory_order_relaxed)) return false;
    empty = 0;
    return atomic_compare_exchange_strong_explicit(&current_receiver, &empty, slot + 1u,
        memory_order_release, memory_order_relaxed);
}

void x4_mic_rr_unbind(unsigned slot)
{
    if (slot >= X4_MIC_RR_SLOTS) return;
    unsigned expected = slot + 1u;
    (void)atomic_compare_exchange_strong_explicit(&current_receiver, &expected, 0,
        memory_order_acq_rel, memory_order_relaxed);
}

void x4_mic_rr_observe(const void *packet, size_t bytes)
{
    unsigned current = atomic_load_explicit(&current_receiver, memory_order_acquire);
    if (!current || !packet || bytes < 8 || bytes > 2048 || (bytes & 3u)) return;
    ReceiverSlot *slot = &receivers[current - 1u];
    const uint8_t *data = packet;
    uint32_t ssrc = atomic_load_explicit(&slot->ssrc, memory_order_relaxed);
    unsigned compounds = 0, blocks_seen = 0, reports = 0;
    uint32_t loss = 0, sequence = 0, jitter = 0;
    while (bytes >= 4) {
        if (++compounds > 32 || (data[0] >> 6) != 2 || data[1] < 192 || data[1] > 223) return;
        unsigned words = ((unsigned)data[2] << 8) | data[3];
        size_t size = (words + 1u) * 4u;
        if (size > bytes) return;
        size_t body = size;
        if (data[0] & 0x20u) {
            unsigned padding = data[size - 1];
            if (size != bytes || !padding || (padding & 3u) || padding > size - 4u) return;
            body -= padding;
        }
        if (data[1] == 200 || data[1] == 201) {
            unsigned blocks = data[0] & 31u;
            size_t header = data[1] == 200 ? 28u : 8u;
            blocks_seen += blocks;
            if (blocks_seen > 64 || body < header + 24u * blocks) return;
            for (unsigned i = 0; i < blocks; ++i) {
                const uint8_t *block = data + header + 24u * i;
                if (read32(block) != ssrc) continue;
                ++reports;
                loss = read32(block + 4);
                sequence = read32(block + 8);
                jitter = read32(block + 12);
            }
        }
        data += size;
        bytes -= size;
    }
    /* A malformed suffix invalidates the whole datagram. An entered callback
     * can race retirement, but any in-flight stores affect only its own slot. */
    if (bytes || !reports || current != atomic_load_explicit(&current_receiver, memory_order_acquire)) return;
    atomic_store_explicit(&slot->loss_jitter, ((uint64_t)loss << 32) | jitter, memory_order_relaxed);
    atomic_store_explicit(&slot->last_sequence, sequence, memory_order_relaxed);
    atomic_fetch_add_explicit(&slot->reports, reports, memory_order_release);
}

X4MicReceiverStats x4_mic_rr_snapshot(unsigned slot)
{
    if (slot >= X4_MIC_RR_SLOTS) return (X4MicReceiverStats){0};
    const ReceiverSlot *receiver = &receivers[slot];
    unsigned reports = atomic_load_explicit(&receiver->reports, memory_order_acquire);
    uint64_t values = atomic_load_explicit(&receiver->loss_jitter, memory_order_relaxed);
    uint32_t lost = (uint32_t)(values >> 32) & 0xffffffu;
    return (X4MicReceiverStats){.reports = reports, .fraction_lost = (unsigned)(values >> 56),
        .total_lost = (int32_t)lost - (lost & 0x800000u ? 0x1000000 : 0),
        .jitter = (uint32_t)values,
        .last_sequence = atomic_load_explicit(&receiver->last_sequence, memory_order_relaxed)};
}
