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
enum { PCM_CAPACITY = 8192, AUDIO_BLOCK = 1024 };
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
    atomic_int error;
    atomic_uint_fast64_t packets, frames, dropped, lost, bad_frames, underflows;
    int16_t pcm[PCM_CAPACITY * 2];
    /* Native AudioOut may retain this buffer until wait/close succeeds.
     * Keep it in the context, not on the worker's disappearing stack. */
    int16_t output_block[AUDIO_BLOCK * 2];
    unsigned pcm_head, pcm_count;
};
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
X4LiveAudio *x4_live_audio_start(int *error, int payload_type)
{
    if (payload_type < -1 || payload_type > 127) { if (error) *error = X4_LIVE_ERR_ARGUMENT; return NULL; }
    int rc = X4_LIVE_ERR_MEMORY;
    X4LiveAudio *a = calloc(1, sizeof(*a));
    if (!a) { if (error) *error = rc; return NULL; }
    a->handle = -1;
    atomic_init(&a->stop, false); atomic_init(&a->muted, false); atomic_init(&a->playing, false);
    atomic_init(&a->error, 0); atomic_init(&a->packets, 0); atomic_init(&a->frames, 0);
    atomic_init(&a->dropped, 0); atomic_init(&a->lost, 0); atomic_init(&a->bad_frames, 0); atomic_init(&a->underflows, 0);
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
    a->handle = open(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, AUDIO_BLOCK, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    rc = a->handle; if (rc < 0) goto fail;
    rc = scePthreadCreate(&a->thread, NULL, play, a, "x4-opus");
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
void x4_live_audio_receive(X4LiveAudio *a, const uint8_t *packet, size_t size)
{
    if (!a || atomic_load(&a->stop) || atomic_load(&a->error)) return;
    X4LiveRtp r;
    if (x4_live_rtp_parse(packet, size, &r) < 0 || !x4_live_ring_push(&a->ring, packet, size)) atomic_fetch_add(&a->dropped, 1);
    else atomic_fetch_add(&a->packets, 1);
}
void x4_live_audio_snapshot(const X4LiveAudio *a, X4LiveAudioSnapshot *s)
{
    memset(s, 0, sizeof(*s)); if (!a) return;
    s->packets = atomic_load(&a->packets); s->frames = atomic_load(&a->frames);
    s->dropped = atomic_load(&a->dropped); s->lost = atomic_load(&a->lost);
    s->bad_frames = atomic_load(&a->bad_frames); s->underflows = atomic_load(&a->underflows);
    s->playing = atomic_load(&a->playing); s->error = atomic_load(&a->error);
}
void x4_live_audio_mute(X4LiveAudio *a, bool mute) { if (a) atomic_store(&a->muted, mute); }
int x4_live_audio_stop(X4LiveAudio *a)
{
    if (!a) return 0;
    atomic_store(&a->stop, true);
    if (a->running) { int rc = scePthreadJoin(a->thread, NULL); if (rc) return rc < 0 ? rc : -rc; a->running = 0; }
    if (a->handle >= 0 && a->close) { int rc = a->close(a->handle); if (rc < 0) return rc; a->handle = -1; }
    if (a->decoder) opus_decoder_destroy(a->decoder);
    x4_live_ring_free(&a->ring); x4_live_reorder_free(&a->reorder); free(a);
    return 0;
}
