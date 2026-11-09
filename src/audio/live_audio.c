/* SPDX-License-Identifier: GPL-3.0-only */
#include "live_audio.h"
#include "../core/module.h"
#include <orbis/libkernel.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <opus/opus.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
enum { PCM_CAPACITY = 8192, AUDIO_BLOCK = 1024 };
enum { AUDIO_NEW_BLOCK = 512, AUDIO_TARGET = 2400, AUDIO_TARGET_MIN = 1920,
       AUDIO_TARGET_MAX = 4800, AUDIO_TRIM = 480, AUDIO_TARGET_BINS = 101,
       AUDIO_VALLEY_BUCKETS = 1001, AUDIO_PLC_MAX = 2880, AUDIO_FADE = 240,
       BUFFER_BINS = 172, DURATION_BINS = 49 };
struct X4LiveAudio {
    X4LiveRing ring;
    X4LiveReorder reorder;
    X4LiveTrack track;
    OpusDecoder *decoder;
    OrbisPthread thread;
    int handle, running;
    int32_t (*output)(int32_t, const void *);
    int32_t (*close)(int32_t);
    atomic_bool stop, muted, playing;
    /* Reception epoch only. The audio owner resets its RTP queue/track;
     * AudioOut, PCM policy, PLC and decoder allocation are unchanged. */
    atomic_bool epoch_hold;
    atomic_uint epoch_callbacks;
    atomic_uint_fast64_t epoch_requested,epoch_applied,retired_source;
    atomic_int error;
    atomic_uint_fast64_t packets, bytes, frames, dropped, lost, bad_frames, underflows;
    int16_t pcm[PCM_CAPACITY * 2];
    /* Native AudioOut may retain this buffer until wait/close succeeds.
     * Keep it in the context, not on the worker's disappearing stack. */
    int16_t output_block[AUDIO_BLOCK * 2];
    unsigned pcm_head, pcm_count;
    bool previous;
    atomic_uint_fast64_t plc_samples, pcm_overflows, decode_errors, trimmed_samples, restarts;
    atomic_uint_fast64_t underflow_episodes, underflow_interval_count, underflow_interval_sum_us;
    atomic_uint_fast64_t underflow_interval_min_us, underflow_interval_max_us;
    atomic_uint_fast64_t buffer_hist[BUFFER_BINS], buffer_max_samples;
    atomic_uint_fast64_t duration_hist[DURATION_BINS], duration_max_samples;
    atomic_uint_fast64_t target_hist[AUDIO_TARGET_BINS], target_max_samples, target_adjustments;
    atomic_uint target_samples;
    atomic_uint_fast64_t trim_events;
    unsigned block_samples;
    int open_512_rc, open_1024_rc;
    bool open_512_attempted, open_1024_attempted;
    /* Worker-owned state; snapshots only read the atomic metrics above. */
    uint64_t last_underflow_us, target_stable_since_us, last_trim_us;
    uint64_t valley_started_us, valley_last_us;
    /* One minimum per millisecond. Expiry includes the oldest partial ms,
     * conservatively retaining it for <1 ms extra. Arbitrarily fast renders
     * cannot evict a valid minimum or cause a false high-valley trim. */
    uint64_t valley_ms[AUDIO_VALLEY_BUCKETS];
    unsigned valley_min[AUDIO_VALLEY_BUCKETS];
    uint32_t expected_timestamp;
    unsigned last_packet_samples, plc_run_samples, concealed_ahead;
    bool timestamp_valid, valley_valid, trimming, have_started, in_underflow;
    int16_t last_left, last_right;
    atomic_uint_fast64_t fec_packets, fec_recovered_samples, fec_attempts, lbrr_packets;
    bool last_packet_celt_only;
};
static void audio_epoch_service(X4LiveAudio *a)
{
    uint64_t requested=atomic_load_explicit(&a->epoch_requested,memory_order_acquire);
    if(requested==atomic_load_explicit(&a->epoch_applied,memory_order_relaxed))return;
    /* Publisher already gated/drained receives. Sole audio consumer owns all
     * queue/reorder/track writes. Leave PCM/render/PLC state untouched here. */
    if(a->track.locked)atomic_store_explicit(&a->retired_source,
        (UINT64_C(1)<<63)|a->track.ssrc,memory_order_release);
    uint8_t discarded[X4_LIVE_RTP_MAX];
    while(x4_live_ring_pop(&a->ring,discarded)){}
    x4_live_track_init(&a->track,-1);x4_live_reorder_reset(&a->reorder);
    atomic_store_explicit(&a->epoch_applied,requested,memory_order_release);
}
int x4_live_audio_begin_epoch(X4LiveAudio *a)
{
    if(!a)return 0; /* Audio unavailable remains unavailable; no reopen. */
    if(atomic_load(&a->stop)||atomic_load(&a->error))return X4_LIVE_ERR_STATE;
    if(atomic_exchange_explicit(&a->epoch_hold,true,memory_order_seq_cst))return X4_LIVE_ERR_STATE;
    uint64_t began=sceKernelGetProcessTime();
    while(atomic_load_explicit(&a->epoch_callbacks,memory_order_seq_cst)){
        if(atomic_load(&a->stop)||sceKernelGetProcessTime()-began>=500000)return X4_LIVE_ERR_STATE;
        sceKernelUsleep(500);
    }
    uint64_t requested=atomic_fetch_add_explicit(&a->epoch_requested,1,memory_order_acq_rel)+1;
    while(atomic_load_explicit(&a->epoch_applied,memory_order_acquire)!=requested){
        if(atomic_load(&a->stop)||sceKernelGetProcessTime()-began>=500000)return X4_LIVE_ERR_STATE;
        sceKernelUsleep(500);
    }
    atomic_store_explicit(&a->epoch_hold,false,memory_order_seq_cst);
    return 0;
}

static void append_pcm(X4LiveAudio *a, const int16_t *samples, unsigned count)
{
    if (a->pcm_count + count > PCM_CAPACITY) {
        unsigned discard = a->pcm_count + count - PCM_CAPACITY;
        a->pcm_head = (a->pcm_head + discard) % PCM_CAPACITY;
        a->pcm_count -= discard;
        atomic_fetch_add(&a->bad_frames, 1);
    }
    for (unsigned i = 0; i < count; ++i) {
        unsigned at = (a->pcm_head + a->pcm_count + i) % PCM_CAPACITY;
        a->pcm[at * 2] = samples[i * 2]; a->pcm[at * 2 + 1] = samples[i * 2 + 1];
    }
    a->pcm_count += count;
}
static void *play(void *opaque)
{
    X4LiveAudio *a = opaque;
    uint8_t packet[X4_LIVE_RTP_MAX];
    int16_t decoded[5760 * 2];
    int16_t *block = a->output_block;
    bool first = true;
    while (!atomic_load(&a->stop)) {
        audio_epoch_service(a);
        for (unsigned n = 0; n < 64; ++n) {
            size_t size = x4_live_ring_pop(&a->ring, packet);
            if (!size) break;
            X4LiveRtp r;
            if (x4_live_rtp_parse(packet, size, &r) < 0) { atomic_fetch_add(&a->dropped, 1); continue; }
            int track = x4_live_track_accept(&a->track, &r);
            if (track < 0) { atomic_fetch_add(&a->dropped, 1); continue; }
            if (track == X4_LIVE_TRACK_NEW) {
                x4_live_reorder_reset(&a->reorder); a->pcm_count = a->pcm_head = 0;
                opus_decoder_ctl(a->decoder, OPUS_RESET_STATE);
            }
            int rc = x4_live_reorder_insert(&a->reorder, packet, size, r.sequence, sceKernelGetProcessTime());
            if (rc == X4_LIVE_REORDER_LATE) atomic_fetch_add(&a->dropped, 1);
            else if (rc == X4_LIVE_REORDER_JUMP) { atomic_fetch_add(&a->lost, 1); opus_decoder_ctl(a->decoder, OPUS_RESET_STATE); }
        }
        for (unsigned n = 0; n < 64; ++n) {
            size_t size = 0; uint32_t lost = 0;
            const uint8_t *p = x4_live_reorder_next(&a->reorder, sceKernelGetProcessTime(), &size, &lost);
            if (!p) break;
            if (lost) { atomic_fetch_add(&a->lost, lost); opus_decoder_ctl(a->decoder, OPUS_RESET_STATE); }
            X4LiveRtp r;
            if (x4_live_rtp_parse(p, size, &r) < 0) { atomic_fetch_add(&a->dropped, 1); continue; }
            int frames = opus_decode(a->decoder, r.payload, (opus_int32)r.size, decoded, 5760, 0);
            if (frames < 0) { atomic_fetch_add(&a->bad_frames, 1); continue; }
            if (!frames) continue;
            append_pcm(a, decoded, (unsigned)frames);
            atomic_fetch_add(&a->frames, 1);
            if (first) { printf("XCloud4: primer paquete Opus decodificado, muestras=%d\n", frames); first = false; }
        }
        if (!atomic_load(&a->playing) && a->pcm_count < AUDIO_BLOCK) { sceKernelUsleep(2000); continue; }
        int rc = a->output(a->handle, NULL);
        if (rc < 0) { atomic_store(&a->error, rc); break; }
        atomic_store(&a->playing, true);
        unsigned real = a->pcm_count < AUDIO_BLOCK ? a->pcm_count : AUDIO_BLOCK;
        bool mute = atomic_load(&a->muted);
        memset(block, 0, sizeof(a->output_block));
        for (unsigned i = 0; i < real; ++i) {
            unsigned at = (a->pcm_head + i) % PCM_CAPACITY;
            if (!mute) { block[i * 2] = a->pcm[at * 2]; block[i * 2 + 1] = a->pcm[at * 2 + 1]; }
        }
        a->pcm_head = (a->pcm_head + real) % PCM_CAPACITY; a->pcm_count -= real;
        if (real < AUDIO_BLOCK) atomic_fetch_add(&a->underflows, 1);
        rc = a->output(a->handle, block);
        if (rc < 0) { atomic_store(&a->error, rc); break; }
    }
    int rc = a->output(a->handle, NULL);
    if (rc < 0) atomic_store(&a->error, rc);
    atomic_store(&a->playing, false);
    return NULL;
}
static void audio_max(atomic_uint_fast64_t *value, uint64_t sample)
{
    uint64_t old = atomic_load(value);
    while (old < sample && !atomic_compare_exchange_weak(value, &old, sample)) {}
}
static void new_target(X4LiveAudio *a, unsigned target)
{
    unsigned previous = atomic_load(&a->target_samples);
    if (target == previous) return;
    atomic_store(&a->target_samples, target);
    audio_max(&a->target_max_samples, target);
    atomic_fetch_add(&a->target_adjustments, 1);
}
static void new_valley_reset(X4LiveAudio *a)
{
    a->valley_valid = false; a->trimming = false;
    memset(a->valley_min, 0xff, sizeof(a->valley_min));
}
/* Called only after consuming a rendered block, before any deliberate trim.
 * Rebuffering starts a fresh full one-second coverage interval. */
static void new_level(X4LiveAudio *a, uint64_t now)
{
    unsigned bin = a->pcm_count / 48u;
    if (bin >= BUFFER_BINS) bin = BUFFER_BINS - 1;
    atomic_fetch_add(&a->buffer_hist[bin], 1);
    audio_max(&a->buffer_max_samples, a->pcm_count);
    if (a->target_stable_since_us && now >= a->target_stable_since_us && now - a->target_stable_since_us >= 10000000u) {
        uint64_t steps = (now - a->target_stable_since_us) / 10000000u;
        unsigned target = atomic_load(&a->target_samples);
        while (steps-- && target > AUDIO_TARGET_MIN) {
            target = target > AUDIO_TRIM ? target - AUDIO_TRIM : 0;
            if (target < AUDIO_TARGET_MIN) target = AUDIO_TARGET_MIN;
            new_target(a, target);
        }
        a->target_stable_since_us = now;
    }
    unsigned target = atomic_load(&a->target_samples);
    atomic_fetch_add(&a->target_hist[target / 48u], 1);
    if (!a->valley_valid || now < a->valley_last_us || now - a->valley_last_us >= 1000000u) {
        new_valley_reset(a); a->valley_valid = true; a->valley_started_us = now;
    }
    a->valley_last_us = now;
    uint64_t ms = now / 1000u;
    unsigned at = (unsigned)(ms % AUDIO_VALLEY_BUCKETS);
    if (a->valley_min[at] == UINT32_MAX || a->valley_ms[at] != ms) {
        a->valley_ms[at] = ms; a->valley_min[at] = a->pcm_count;
    } else if (a->pcm_count < a->valley_min[at]) a->valley_min[at] = a->pcm_count;
    if (now - a->valley_started_us < 1000000u) return;
    unsigned minimum = UINT32_MAX;
    for (unsigned i = 0; i < AUDIO_VALLEY_BUCKETS; ++i)
        if (a->valley_min[i] != UINT32_MAX && ms >= a->valley_ms[i] &&
            ms - a->valley_ms[i] <= 1000u && a->valley_min[i] < minimum)
            minimum = a->valley_min[i];
    if (minimum > target + 960u) a->trimming = true;
    if (!a->trimming) return;
    if (a->pcm_count <= target) { a->trimming = false; return; }
    if (a->last_trim_us && now >= a->last_trim_us && now - a->last_trim_us < 200000u) return;
    unsigned trimmed = a->pcm_count - target;
    if (trimmed > AUDIO_TRIM) trimmed = AUDIO_TRIM;
    if (trimmed < 120u || a->pcm_count - trimmed < 120u) return;
    for (unsigned i = 0; i < 120u; ++i) {
        unsigned drop_idx = (a->pcm_head + i) % PCM_CAPACITY;
        unsigned keep_idx = (a->pcm_head + trimmed + i) % PCM_CAPACITY;
        int32_t d_left = a->pcm[drop_idx * 2];
        int32_t d_right = a->pcm[drop_idx * 2 + 1];
        int32_t k_left = a->pcm[keep_idx * 2];
        int32_t k_right = a->pcm[keep_idx * 2 + 1];
        a->pcm[keep_idx * 2] = (int16_t)((d_left * (120 - (int32_t)i) + k_left * (int32_t)i) / 120);
        a->pcm[keep_idx * 2 + 1] = (int16_t)((d_right * (120 - (int32_t)i) + k_right * (int32_t)i) / 120);
    }
    a->pcm_head = (a->pcm_head + trimmed) % PCM_CAPACITY;
    a->pcm_count -= trimmed;
    a->last_trim_us = now;
    atomic_fetch_add(&a->trimmed_samples, trimmed);
    atomic_fetch_add(&a->trim_events, 1);
    if (a->pcm_count <= target) a->trimming = false;
}
static void new_append(X4LiveAudio *a, const int16_t *samples, unsigned count)
{
    if (count > PCM_CAPACITY) { samples += (count - PCM_CAPACITY) * 2; count = PCM_CAPACITY; }
    if (a->pcm_count + count > PCM_CAPACITY) {
        unsigned discard = a->pcm_count + count - PCM_CAPACITY;
        a->pcm_head = (a->pcm_head + discard) % PCM_CAPACITY;
        a->pcm_count -= discard;
        atomic_fetch_add(&a->pcm_overflows, 1);
        atomic_fetch_add(&a->trimmed_samples, discard);
    }
    for (unsigned i = 0; i < count; ++i) {
        unsigned at = (a->pcm_head + a->pcm_count + i) % PCM_CAPACITY;
        a->pcm[at * 2] = samples[i * 2]; a->pcm[at * 2 + 1] = samples[i * 2 + 1];
    }
    a->pcm_count += count;
}
/* PLC is bounded per uninterrupted concealment run, not per output block.
 * A delayed real packet is still decoded for Opus state, but its already
 * concealed prefix is not queued twice. Clip only when reserving an exact
 * boundary for FEC. Ordinary PLC retains its existing packet-sized output. */
static void new_conceal(X4LiveAudio *a, int16_t *decoded, unsigned wanted, bool exact)
{
    while (wanted && a->plc_run_samples < AUDIO_PLC_MAX) {
        unsigned duration = a->last_packet_samples ? a->last_packet_samples : 960u;
        unsigned remaining = AUDIO_PLC_MAX - a->plc_run_samples;
        if (duration > remaining) duration = remaining;
        if (exact && duration > wanted) duration = wanted;
        duration -= duration % 120u; /* Opus PLC accepts 2.5 ms multiples. */
        if (!duration) break;
        int frames = opus_decode(a->decoder, NULL, 0, decoded, (int)duration, 0);
        if (frames <= 0 || (unsigned)frames > duration) {
            atomic_fetch_add(&a->decode_errors, 1); break;
        }
        new_append(a, decoded, (unsigned)frames);
        a->plc_run_samples += (unsigned)frames;
        a->concealed_ahead += (unsigned)frames;
        atomic_fetch_add(&a->plc_samples, (unsigned)frames);
        if ((unsigned)frames >= wanted) break;
        wanted -= (unsigned)frames;
    }
}
static void new_gap_silence(X4LiveAudio *a, int16_t *decoded, unsigned silence)
{
    /* A declared gap longer than the PLC budget gets a short fade and
     * silence. Do not manufacture an unbounded queue for a discontinuity. */
    if (silence > PCM_CAPACITY) silence = PCM_CAPACITY;
    int16_t left = a->last_left, right = a->last_right;
    if (a->pcm_count) {
        unsigned last = (a->pcm_head + a->pcm_count - 1) % PCM_CAPACITY;
        left = a->pcm[last * 2]; right = a->pcm[last * 2 + 1];
    }
    while (silence) {
        unsigned count = silence > 5760u ? 5760u : silence;
        memset(decoded, 0, count * 2 * sizeof(*decoded));
        unsigned fade = count < AUDIO_FADE ? count : AUDIO_FADE;
        for (unsigned i = 0; i < fade; ++i) {
            decoded[i * 2] = (int16_t)((int32_t)left * (int32_t)(fade - i - 1) / (int32_t)fade);
            decoded[i * 2 + 1] = (int16_t)((int32_t)right * (int32_t)(fade - i - 1) / (int32_t)fade);
        }
        new_append(a, decoded, count); a->concealed_ahead += count;
        silence -= count; left = right = 0;
    }
}
static bool new_recover_fec(X4LiveAudio *a, const X4LiveRtp *r, int16_t *decoded, unsigned duration)
{
    atomic_fetch_add(&a->fec_attempts, 1);
    int frames = opus_decode(a->decoder, r->payload, (opus_int32)r->size, decoded, (int)duration, 1);
    if (frames != (int)duration) return false;

    /* Even a packet with LBRR can yield PLC after CELT. In libopus the
     * no-data path leaves a zero final range, unlike decoding the LBRR. */
    opus_uint32 range = 0;
    if (opus_decoder_ctl(a->decoder, OPUS_GET_FINAL_RANGE(&range)) == OPUS_OK && range) {
        atomic_fetch_add(&a->fec_packets, 1);
        atomic_fetch_add(&a->fec_recovered_samples, duration);
        a->plc_run_samples = 0;
    } else {
        unsigned remaining = AUDIO_PLC_MAX - a->plc_run_samples;
        if (duration > remaining) duration = remaining;
        a->plc_run_samples += duration;
        atomic_fetch_add(&a->plc_samples, duration);
    }
    new_append(a, decoded, duration);
    a->concealed_ahead += duration;
    return true;
}
static void new_decode_packet(X4LiveAudio *a, const X4LiveRtp *r, int16_t *decoded)
{
    if (a->have_started && !atomic_load(&a->playing)) {
        /* The final faded block has already left AudioOut silent. During
         * rebuffering, old RTP time is not new PCM to play: start a fresh
         * timeline from received packets and collect the current target. */
        a->timestamp_valid = false; a->concealed_ahead = 0; a->plc_run_samples = 0;
    }
    int duration = opus_packet_get_nb_samples(r->payload, (opus_int32)r->size, 48000);
    unsigned fec_duration = 0;
    if (duration > 0 && duration <= 5760) {
        unsigned bin = (unsigned)duration / 120u;
        if (bin >= DURATION_BINS) bin = DURATION_BINS - 1;
        atomic_fetch_add(&a->duration_hist[bin], 1);
        audio_max(&a->duration_max_samples, (unsigned)duration);
        if (opus_packet_has_lbrr(r->payload, (opus_int32)r->size) > 0) {
            atomic_fetch_add(&a->lbrr_packets, 1);
            fec_duration = (unsigned)opus_packet_get_samples_per_frame(r->payload, 48000);
        }
    }
    int32_t gap = a->timestamp_valid ? (int32_t)(r->timestamp - a->expected_timestamp) : 0;
    if (gap > 0 && (uint32_t)gap > a->concealed_ahead) {
        unsigned missing = (uint32_t)gap - a->concealed_ahead;
        /* FEC reconstructs one complete frame immediately before this
         * packet. Never decode a partial frame over already played audio. */
        if (!a->last_packet_celt_only && fec_duration && fec_duration <= missing &&
            missing <= PCM_CAPACITY && fec_duration % 120u == 0) {
            unsigned before_fec = (uint32_t)gap - fec_duration;
            new_conceal(a, decoded, missing - fec_duration, true);
            if (before_fec > a->concealed_ahead)
                new_gap_silence(a, decoded, before_fec - a->concealed_ahead);
            if (!new_recover_fec(a, r, decoded, fec_duration))
                new_conceal(a, decoded, fec_duration, true);
        } else {
            new_conceal(a, decoded, missing, false);
        }
        if ((uint32_t)gap > a->concealed_ahead) {
            new_gap_silence(a, decoded, (uint32_t)gap - a->concealed_ahead);
        }
    }
    int frames = opus_decode(a->decoder, r->payload, (opus_int32)r->size, decoded, 5760, 0);
    if (frames < 0) { atomic_fetch_add(&a->decode_errors, 1); atomic_fetch_add(&a->bad_frames, 1); return; }
    if (!frames) return;
    unsigned credit = a->concealed_ahead;
    if (gap > 0) credit = credit > (uint32_t)gap ? credit - (uint32_t)gap : 0;
    unsigned covered = credit < (unsigned)frames ? credit : (unsigned)frames;
    unsigned target = atomic_load(&a->target_samples);
    unsigned excess = a->pcm_count + (unsigned)frames > target ? a->pcm_count + (unsigned)frames - target : 0;
    unsigned skip = covered < excess ? covered : excess;
    a->concealed_ahead = credit - covered;
    if ((unsigned)frames > skip) {
        new_append(a, decoded + skip * 2, (unsigned)frames - skip);
        a->plc_run_samples = 0; a->in_underflow = false;
    }
    a->expected_timestamp = r->timestamp + (uint32_t)frames;
    a->timestamp_valid = true;
    a->last_packet_samples = (unsigned)frames;
    /* The high TOC bit identifies CELT-only configurations. Opus would
     * silently run PLC instead of LBRR when coming from that mode. */
    a->last_packet_celt_only = (r->payload[0] & 0x80u) != 0;
    atomic_fetch_add(&a->frames, 1);
}
static void new_underflow(X4LiveAudio *a, uint64_t now)
{
    atomic_fetch_add(&a->underflows, 1);
    if (!a->in_underflow) {
        atomic_fetch_add(&a->underflow_episodes, 1); a->in_underflow = true;
        unsigned target = atomic_load(&a->target_samples) + 480u;
        if (target > AUDIO_TARGET_MAX) target = AUDIO_TARGET_MAX;
        new_target(a, target);
    }
    a->target_stable_since_us = now;
    if (a->last_underflow_us) {
        uint64_t interval = now - a->last_underflow_us;
        atomic_fetch_add(&a->underflow_interval_sum_us, interval);
        uint64_t old = atomic_load(&a->underflow_interval_min_us);
        while (interval < old && !atomic_compare_exchange_weak(&a->underflow_interval_min_us, &old, interval)) {}
        audio_max(&a->underflow_interval_max_us, interval);
        /* Publish the interval only once min/max/sum have been initialized. */
        atomic_fetch_add(&a->underflow_interval_count, 1);
    }
    a->last_underflow_us = now;
}
static void new_render(X4LiveAudio *a, int16_t *decoded, uint64_t now)
{
    unsigned block_samples = a->block_samples;
    if (a->pcm_count < block_samples) {
        new_underflow(a, now);
        new_conceal(a, decoded, block_samples - a->pcm_count, false);
    }
    unsigned real = a->pcm_count < block_samples ? a->pcm_count : block_samples;
    bool mute = atomic_load(&a->muted);
    int16_t *block = a->output_block;
    memset(block, 0, sizeof(a->output_block));
    for (unsigned i = 0; i < real; ++i) {
        unsigned at = (a->pcm_head + i) % PCM_CAPACITY;
        a->last_left = a->pcm[at * 2]; a->last_right = a->pcm[at * 2 + 1];
        if (!mute) { block[i * 2] = a->last_left; block[i * 2 + 1] = a->last_right; }
    }
    a->pcm_head = (a->pcm_head + real) % PCM_CAPACITY; a->pcm_count -= real;
    if (real < block_samples) {
        unsigned fade = block_samples - real;
        if (fade > AUDIO_FADE) fade = AUDIO_FADE;
        for (unsigned i = 0; i < fade; ++i) {
            if (!mute) {
                block[(real + i) * 2] = (int16_t)((int32_t)a->last_left * (int32_t)(fade - i - 1) / (int32_t)fade);
                block[(real + i) * 2 + 1] = (int16_t)((int32_t)a->last_right * (int32_t)(fade - i - 1) / (int32_t)fade);
            }
        }
        a->last_left = a->last_right = 0;
        a->concealed_ahead += block_samples - real;
        /* The final silent block is submitted; restart uses the current target. */
        atomic_store(&a->playing, false);
    }
    new_level(a, now);
}
static void *play_new(void *opaque)
{
    X4LiveAudio *a = opaque;
    uint8_t packet[X4_LIVE_RTP_MAX];
    int16_t decoded[5760 * 2];
    bool first = true;
    while (!atomic_load(&a->stop)) {
        audio_epoch_service(a);
        for (unsigned n = 0; n < 64; ++n) {
            size_t size = x4_live_ring_pop(&a->ring, packet);
            if (!size) break;
            X4LiveRtp r;
            if (x4_live_rtp_parse(packet, size, &r) < 0) { atomic_fetch_add(&a->dropped, 1); continue; }
            int track = x4_live_track_accept(&a->track, &r);
            if (track < 0) { atomic_fetch_add(&a->dropped, 1); continue; }
            if (track == X4_LIVE_TRACK_NEW) {
                x4_live_reorder_reset(&a->reorder); a->pcm_count = a->pcm_head = 0;
                opus_decoder_ctl(a->decoder, OPUS_RESET_STATE);
                a->timestamp_valid = false; a->plc_run_samples = a->concealed_ahead = 0;
                a->last_packet_samples = 960; a->last_left = a->last_right = 0;
                new_valley_reset(a); a->in_underflow = false;
                atomic_store(&a->playing, false);
            }
            int rc = x4_live_reorder_insert(&a->reorder, packet, size, r.sequence, sceKernelGetProcessTime());
            if (rc == X4_LIVE_REORDER_LATE) atomic_fetch_add(&a->dropped, 1);
            else if (rc == X4_LIVE_REORDER_JUMP) atomic_fetch_add(&a->lost, 1);
        }
        for (unsigned n = 0; n < 64; ++n) {
            size_t size = 0; uint32_t lost = 0;
            const uint8_t *p = x4_live_reorder_next(&a->reorder, sceKernelGetProcessTime(), &size, &lost);
            if (!p) break;
            if (lost) atomic_fetch_add(&a->lost, lost);
            X4LiveRtp r;
            if (x4_live_rtp_parse(p, size, &r) < 0) { atomic_fetch_add(&a->dropped, 1); continue; }
            new_decode_packet(a, &r, decoded);
            if (first && atomic_load(&a->frames)) {
                printf("XCloud4: primer paquete Opus decodificado, audio nuevo objetivo=%ums bloque=%u PLC_max=60ms FEC=1\n",
                       atomic_load(&a->target_samples) / 48u, a->block_samples); first = false;
            }
        }
        if (!atomic_load(&a->playing)) {
            if (a->pcm_count < atomic_load(&a->target_samples)) { sceKernelUsleep(2000); continue; }
            if (a->have_started) atomic_fetch_add(&a->restarts, 1);
            new_valley_reset(a);
            a->target_stable_since_us = sceKernelGetProcessTime();
            a->have_started = true; atomic_store(&a->playing, true);
        }
        int rc = a->output(a->handle, NULL);
        if (rc < 0) { atomic_store(&a->error, rc); break; }
        new_render(a, decoded, sceKernelGetProcessTime());
        rc = a->output(a->handle, a->output_block);
        if (rc < 0) { atomic_store(&a->error, rc); break; }
    }
    int rc = a->output(a->handle, NULL);
    if (rc < 0) atomic_store(&a->error, rc);
    atomic_store(&a->playing, false);
    return NULL;
}
X4LiveAudio *x4_live_audio_start_mode(int *error, int payload_type, bool previous)
{
    if (payload_type < -1 || payload_type > 127) { if (error) *error = X4_LIVE_ERR_ARGUMENT; return NULL; }
    int rc = X4_LIVE_ERR_MEMORY;
    X4LiveAudio *a = calloc(1, sizeof(*a));
    if (!a) { if (error) *error = rc; return NULL; }
    a->handle = -1;
    a->previous = previous; a->last_packet_samples = 960;
    a->block_samples = previous ? AUDIO_BLOCK : AUDIO_NEW_BLOCK;
    atomic_init(&a->target_samples, AUDIO_TARGET);
    atomic_init(&a->target_max_samples, AUDIO_TARGET); atomic_init(&a->target_adjustments, 0);
    atomic_init(&a->trim_events, 0);
    a->target_stable_since_us = 0;
    a->last_trim_us = 0;
    new_valley_reset(a);
    atomic_init(&a->stop, false); atomic_init(&a->muted, false); atomic_init(&a->playing, false);
    atomic_init(&a->epoch_hold,false);atomic_init(&a->epoch_callbacks,0);
    atomic_init(&a->epoch_requested,0);atomic_init(&a->epoch_applied,0);atomic_init(&a->retired_source,0);
    atomic_init(&a->error, 0); atomic_init(&a->packets, 0); atomic_init(&a->bytes, 0); atomic_init(&a->frames, 0);
    atomic_init(&a->dropped, 0); atomic_init(&a->lost, 0); atomic_init(&a->bad_frames, 0); atomic_init(&a->underflows, 0);
    atomic_init(&a->plc_samples, 0); atomic_init(&a->pcm_overflows, 0); atomic_init(&a->decode_errors, 0);
    atomic_init(&a->fec_packets, 0); atomic_init(&a->fec_recovered_samples, 0);
    atomic_init(&a->fec_attempts, 0); atomic_init(&a->lbrr_packets, 0);
    atomic_init(&a->trimmed_samples, 0); atomic_init(&a->restarts, 0);
    atomic_init(&a->underflow_episodes, 0); atomic_init(&a->underflow_interval_count, 0);
    atomic_init(&a->underflow_interval_sum_us, 0); atomic_init(&a->underflow_interval_min_us, UINT64_MAX);
    atomic_init(&a->underflow_interval_max_us, 0); atomic_init(&a->buffer_max_samples, 0);
    atomic_init(&a->duration_max_samples, 0);
    for (unsigned i = 0; i < BUFFER_BINS; ++i) atomic_init(&a->buffer_hist[i], 0);
    for (unsigned i = 0; i < DURATION_BINS; ++i) atomic_init(&a->duration_hist[i], 0);
    for (unsigned i = 0; i < AUDIO_TARGET_BINS; ++i) atomic_init(&a->target_hist[i], 0);
    x4_live_track_init(&a->track, payload_type);
    rc = x4_live_ring_init(&a->ring, 96); if (rc < 0) goto fail;
    rc = x4_live_reorder_init(&a->reorder, 64, 8, 30000); if (rc < 0) goto fail;
    a->decoder = opus_decoder_create(48000, 2, &rc); if (!a->decoder || rc < 0) goto fail;
    int handle = x4_module_open("libSceAudioOut"); rc = handle; if (rc < 0) goto fail;
    int32_t (*init)(void) = NULL;
    int32_t (*open)(OrbisUserServiceUserId, enum OrbisAudioOutPort, int32_t, uint32_t, uint32_t, uint32_t) = NULL;
#define SYMBOL(name, target) do { rc = x4_module_symbol(handle, name, (void **)&target); if (rc < 0) goto fail; } while (0)
    SYMBOL("sceAudioOutInit", init); SYMBOL("sceAudioOutOpen", open);
    SYMBOL("sceAudioOutOutput", a->output); SYMBOL("sceAudioOutClose", a->close);
#undef SYMBOL
    rc = init(); if (rc < 0 && (uint32_t)rc != 0x8026000e) goto fail;
    a->handle = open(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, a->block_samples, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (previous) { a->open_1024_attempted = true; a->open_1024_rc = a->handle; }
    else {
        a->open_512_attempted = true; a->open_512_rc = a->handle;
        printf("XCloud4: AudioOut Open bloque=512 retorno=%d\n", a->open_512_rc);
        if (a->handle < 0) {
            a->block_samples = AUDIO_BLOCK;
            a->handle = open(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, AUDIO_BLOCK, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
            a->open_1024_attempted = true; a->open_1024_rc = a->handle;
            printf("XCloud4: AudioOut fallback bloque=1024 retorno=%d\n", a->open_1024_rc);
        }
    }
    rc = a->handle; if (rc < 0) goto fail;
    rc = scePthreadCreate(&a->thread, NULL, previous ? play : play_new, a, "x4-opus");
    if (rc) { if (rc > 0) rc = -rc; goto fail; }
    a->running = 1;
    if (error) *error = 0;
    printf("XCloud4: audio real Opus 48 kHz estereo listo\n");
    return a;
fail:
    if (error) *error = rc;
    x4_live_audio_stop(a);
    return NULL;
}
X4LiveAudio *x4_live_audio_start(int *error, int payload_type)
{
    return x4_live_audio_start_mode(error, payload_type, true);
}
static void audio_receive_epoch(X4LiveAudio *a,const uint8_t *packet,size_t size,bool proven,uint64_t epoch)
{
    if (!a || atomic_load(&a->stop) || atomic_load(&a->error)) return;
    atomic_fetch_add_explicit(&a->epoch_callbacks,1,memory_order_seq_cst);
    X4LiveRtp r;
    uint64_t retired=atomic_load_explicit(&a->retired_source,memory_order_acquire);
    if(atomic_load_explicit(&a->epoch_hold,memory_order_seq_cst)||
        (proven&&epoch!=atomic_load_explicit(&a->epoch_applied,memory_order_acquire))||
        x4_live_rtp_parse(packet,size,&r)<0||(!proven&&retired&&(uint32_t)retired==r.ssrc)||
        !x4_live_ring_push(&a->ring,packet,size))atomic_fetch_add(&a->dropped,1);
    else { atomic_fetch_add(&a->packets,1); atomic_fetch_add(&a->bytes,size); }
    atomic_fetch_sub_explicit(&a->epoch_callbacks,1,memory_order_seq_cst);
}
void x4_live_audio_receive(X4LiveAudio *a,const uint8_t *packet,size_t size)
{audio_receive_epoch(a,packet,size,false,0);}
void x4_live_audio_receive_for_epoch(X4LiveAudio *a,const uint8_t *packet,size_t size,uint64_t epoch)
{audio_receive_epoch(a,packet,size,true,epoch);}
static uint64_t audio_percentile(const atomic_uint_fast64_t *hist, unsigned bins, unsigned percentile, uint64_t step_us, uint64_t *count)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < bins; ++i) total += atomic_load(&hist[i]);
    if (count) *count = total;
    if (!total) return 0;
    uint64_t rank = (total * percentile + 99u) / 100u, seen = 0;
    for (unsigned i = 0; i < bins; ++i) {
        seen += atomic_load(&hist[i]);
        if (seen >= rank) return i * step_us;
    }
    return (bins - 1u) * step_us;
}
void x4_live_audio_snapshot(const X4LiveAudio *a, X4LiveAudioSnapshot *s)
{
    memset(s, 0, sizeof(*s)); if (!a) return;
    s->packets = atomic_load(&a->packets); s->bytes = atomic_load(&a->bytes); s->frames = atomic_load(&a->frames);
    s->dropped = atomic_load(&a->dropped); s->lost = atomic_load(&a->lost);
    s->bad_frames = atomic_load(&a->bad_frames); s->underflows = atomic_load(&a->underflows);
    s->playing = atomic_load(&a->playing); s->error = atomic_load(&a->error);
    s->previous = a->previous; s->detailed_metrics_available = !a->previous;
    s->fec_implemented = !a->previous; s->fec_packets = atomic_load(&a->fec_packets);
    s->plc_samples = atomic_load(&a->plc_samples); s->pcm_overflows = atomic_load(&a->pcm_overflows);
    s->decode_errors = atomic_load(&a->decode_errors); s->trimmed_samples = atomic_load(&a->trimmed_samples);
    s->restarts = atomic_load(&a->restarts); s->underflow_episodes = atomic_load(&a->underflow_episodes);
    s->underflow_interval_count = atomic_load(&a->underflow_interval_count);
    s->underflow_interval_sum_us = atomic_load(&a->underflow_interval_sum_us);
    s->underflow_interval_min_us = atomic_load(&a->underflow_interval_min_us);
    if (!s->underflow_interval_count || s->underflow_interval_min_us == UINT64_MAX) s->underflow_interval_min_us = 0;
    s->underflow_interval_max_us = atomic_load(&a->underflow_interval_max_us);
    s->buffer_p5_us = audio_percentile(a->buffer_hist, BUFFER_BINS, 5, 1000, &s->buffer_observations);
    s->buffer_p50_us = audio_percentile(a->buffer_hist, BUFFER_BINS, 50, 1000, NULL);
    s->buffer_p95_us = audio_percentile(a->buffer_hist, BUFFER_BINS, 95, 1000, NULL);
    s->buffer_max_us = atomic_load(&a->buffer_max_samples) * 1000u / 48u;
    s->packet_duration_p50_us = audio_percentile(a->duration_hist, DURATION_BINS, 50, 2500, &s->packet_duration_count);
    s->packet_duration_p95_us = audio_percentile(a->duration_hist, DURATION_BINS, 95, 2500, NULL);
    s->packet_duration_max_us = atomic_load(&a->duration_max_samples) * 1000u / 48u;
    s->target_p50_us = audio_percentile(a->target_hist, AUDIO_TARGET_BINS, 50, 1000, &s->target_observations);
    s->target_max_us = atomic_load(&a->target_max_samples) * 1000u / 48u;
    s->target_adjustments = atomic_load(&a->target_adjustments);
    s->target_current_samples = atomic_load(&a->target_samples); s->output_block_samples = a->block_samples;
    s->open_512_attempted = a->open_512_attempted; s->open_1024_attempted = a->open_1024_attempted;
    s->open_512_rc = a->open_512_rc; s->open_1024_rc = a->open_1024_rc;
    s->trim_events = atomic_load(&a->trim_events);
}
void x4_live_audio_metrics_write(FILE *f, const X4LiveAudioSnapshot *s)
{
    if (!f || !s) return;
    fprintf(f, "{\"mode\":\"%s\",\"detailed_metrics_available\":%s,\"worker_joined\":%s,\"snapshot_scope\":\"%s\",\"underflows\":%" PRIu64
            ",\"underflow_unit\":\"output_blocks_short_before_concealment\",\"underflow_episodes\":%" PRIu64
            ",\"plc_ms\":%.3f,\"plc_scope\":\"Opus_samples_generated_including_any_later_trimmed\",\"fec_packets\":%" PRIu64
            ",\"fec_implemented\":%s,\"pcm_overflows\":%" PRIu64
            ",\"decode_errors\":%" PRIu64 ",\"trimmed_ms\":%.3f,\"trim_events\":%" PRIu64 ",\"restarts\":%" PRIu64,
            s->previous ? "previous" : "new", s->detailed_metrics_available ? "true" : "false",
            s->worker_joined ? "true" : "false", s->worker_joined ? "joined_final" : "live_atomic_approximation", s->underflows,
            s->underflow_episodes, (double)s->plc_samples / 48.0, s->fec_packets,
            s->fec_implemented ? "true" : "false", s->pcm_overflows, s->decode_errors,
            (double)s->trimmed_samples / 48.0, s->trim_events, s->restarts);
    if (!s->detailed_metrics_available) {
        fputs(",\"detail_status\":\"unavailable_in_unchanged_previous_worker\",\"buffer_ms\":null,\"underflow_intervals_ms\":null,\"packet_duration_ms\":null}", f); return;
    }
    fprintf(f, ",\"buffer_ms\":{\"n\":%" PRIu64 ",\"p5\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"max\":%.3f,\"quantization_ms\":1,\"scope\":\"post_render_valley_before_trim_including_PLC\"}",
            s->buffer_observations, s->buffer_p5_us / 1000.0, s->buffer_p50_us / 1000.0, s->buffer_p95_us / 1000.0, s->buffer_max_us / 1000.0);
    fprintf(f, ",\"valley_ms\":{\"n\":%" PRIu64 ",\"p5\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"scope\":\"post_render_before_trim_including_PLC\",\"window_ms\":1000,\"maximum_window_overhang_us\":999}",
            s->buffer_observations, s->buffer_p5_us / 1000.0, s->buffer_p50_us / 1000.0, s->buffer_p95_us / 1000.0);
    fprintf(f, ",\"target_ms\":{\"n\":%" PRIu64 ",\"p50\":%.3f,\"max\":%.3f,\"current\":%.3f,\"adjustments\":%" PRIu64 ",\"scope\":\"post_render_target\"},\"output_block_samples\":%u,\"audio_out_open\":{\"attempted_512\":%s,\"rc_512\":%d,\"attempted_1024\":%s,\"rc_1024\":%d}",
            s->target_observations, s->target_p50_us / 1000.0, s->target_max_us / 1000.0,
            s->target_current_samples / 48.0, s->target_adjustments, s->output_block_samples,
            s->open_512_attempted ? "true" : "false", s->open_512_rc,
            s->open_1024_attempted ? "true" : "false", s->open_1024_rc);
    fprintf(f, ",\"underflow_intervals_ms\":{\"n\":%" PRIu64 ",\"mean\":%.3f,\"min\":%.3f,\"max\":%.3f}",
            s->underflow_interval_count, s->underflow_interval_count ? (double)s->underflow_interval_sum_us / (double)s->underflow_interval_count / 1000.0 : 0.0,
            s->underflow_interval_min_us / 1000.0, s->underflow_interval_max_us / 1000.0);
    fprintf(f, ",\"packet_duration_ms\":{\"n\":%" PRIu64 ",\"p50\":%.3f,\"p95\":%.3f,\"max\":%.3f,\"scope\":\"valid_Opus_packet_headers_not_PLC\"}}",
            s->packet_duration_count, s->packet_duration_p50_us / 1000.0, s->packet_duration_p95_us / 1000.0, s->packet_duration_max_us / 1000.0);
}
void x4_live_audio_mute(X4LiveAudio *a, bool mute) { if (a) atomic_store(&a->muted, mute); }
int x4_live_audio_stop_report(X4LiveAudio *a, X4LiveAudioSnapshot *report)
{
    if (!a) { if (report) memset(report, 0, sizeof(*report)); return 0; }
    atomic_store(&a->stop, true);
    if (a->running) {
        int rc = scePthreadJoin(a->thread, NULL);
        if (rc) { if (report) x4_live_audio_snapshot(a, report); return rc < 0 ? rc : -rc; }
        a->running = 0;
    }
    if (report) { x4_live_audio_snapshot(a, report); report->worker_joined = true; }
    if (a->handle >= 0 && a->close) { int rc = a->close(a->handle); if (rc < 0) return rc; a->handle = -1; }
#if !X4_PUBLIC
    if (!a->previous)
        printf("XCloud4: audio FEC lost=%" PRIu64 " plc_samples=%" PRIu64 " fec_attempts=%" PRIu64
               " fec_packets=%" PRIu64 " fec_recovered_samples=%" PRIu64 " lbrr_packets=%" PRIu64 "\n",
               (uint64_t)atomic_load(&a->lost), (uint64_t)atomic_load(&a->plc_samples),
               (uint64_t)atomic_load(&a->fec_attempts), (uint64_t)atomic_load(&a->fec_packets),
               (uint64_t)atomic_load(&a->fec_recovered_samples), (uint64_t)atomic_load(&a->lbrr_packets));
#endif
    if (a->decoder) opus_decoder_destroy(a->decoder);
    x4_live_ring_free(&a->ring); x4_live_reorder_free(&a->reorder); free(a);
    return 0;
}
int x4_live_audio_stop(X4LiveAudio *a)
{
    return x4_live_audio_stop_report(a, NULL);
}
