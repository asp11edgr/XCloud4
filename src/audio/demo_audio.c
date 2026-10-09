/* SPDX-License-Identifier: GPL-3.0-only */
#include "demo_audio.h"
#include "../core/module.h"
#include <stdio.h>
#include <string.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>

static void *play(void *opaque)
{
    X4DemoAudio *a = opaque;
    int16_t samples[1024 * 2];
    unsigned position = 0, phase = 0;
    while (!atomic_load(&a->stop) && position < 48000 * 8) {
        int muted = atomic_load(&a->muted);
        for (unsigned i = 0; i < 1024; ++i, ++position) {
            unsigned within = position % 48000;
            phase = (phase + 440) % 48000;
            int triangle = phase < 24000 ? (int)phase : 48000 - (int)phase;
            int value = (triangle - 12000) / 10;
            /* Short, quiet tones: alternate L/R with silence and soft edges. */
            int gain = within < 24000 ? 256 : 0;
            if (within < 256) gain = (int)within;
            if (within >= 23744 && within < 24000) gain = 24000 - (int)within;
            value = muted ? 0 : value * gain / 256;
            unsigned channel = (position / 48000) & 1;
            samples[i * 2] = channel == 0 ? (int16_t)value : 0;
            samples[i * 2 + 1] = channel == 1 ? (int16_t)value : 0;
        }
        int rc = a->output(a->handle, samples);
        if (rc < 0) {
            atomic_store(&a->error, rc);
            printf("XCloud4: AudioOutOutput 0x%08x\n", (unsigned)rc);
            break;
        }
    }
    a->output(a->handle, NULL);
    atomic_store(&a->finished, 1);
    return NULL;
}

int x4_audio_start(X4DemoAudio *a)
{
    if (a->running) { atomic_store(&a->error, -9); return -9; }
    int muted = atomic_load(&a->muted);
    memset(a, 0, sizeof(*a));
    a->handle = -1;
    atomic_init(&a->stop, 0); atomic_init(&a->muted, muted);
    atomic_init(&a->error, 0); atomic_init(&a->finished, 0);
    int handle = x4_module_open("libSceAudioOut"), rc = handle;
    int32_t (*init)(void) = NULL;
    int32_t (*open)(OrbisUserServiceUserId, enum OrbisAudioOutPort, int32_t, uint32_t, uint32_t, uint32_t) = NULL;
    if (rc < 0) goto fail;
#define SYMBOL(name, target) do { rc = x4_module_symbol(handle, name, (void **)&target); if (rc < 0) goto fail; } while (0)
    SYMBOL("sceAudioOutInit", init);
    SYMBOL("sceAudioOutOpen", open);
    SYMBOL("sceAudioOutOutput", a->output);
    SYMBOL("sceAudioOutClose", a->close);
#undef SYMBOL
    rc = init();
    if (rc < 0 && (uint32_t)rc != 0x8026000e) goto fail;
    OrbisUserServiceUserId user;
    rc = sceUserServiceGetInitialUser(&user);
    if (rc < 0) goto fail;
    a->handle = open(user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 1024, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    rc = a->handle;
    if (rc < 0) goto fail;
    rc = scePthreadCreate(&a->thread, NULL, play, a, "x4-pcm");
    if (rc != 0) { if (rc > 0) rc = -rc; goto fail; }
    a->running = 1;
    printf("XCloud4: PCM 48000 Hz estereo iniciado\n");
    return 0;
fail:
    atomic_store(&a->error, rc);
    x4_audio_stop(a);
    return rc;
}

void x4_audio_stop(X4DemoAudio *a)
{
    if (a->running) {
        atomic_store(&a->stop, 1);
        int rc = scePthreadJoin(a->thread, NULL);
        if (rc != 0) {
            atomic_store(&a->error, rc < 0 ? rc : -rc);
            printf("XCloud4: audio join 0x%08x\n", (unsigned)rc);
            return;
        }
        a->running = 0;
    }
    if (a->handle >= 0 && a->close) { a->close(a->handle); a->handle = -1; }
}
