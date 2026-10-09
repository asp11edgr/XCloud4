/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include <orbis/libkernel.h>
typedef struct {
    int handle, running;
    OrbisPthread thread;
    _Atomic int stop, muted, error, finished;
    int32_t (*output)(int32_t, const void *);
    int32_t (*close)(int32_t);
} X4DemoAudio;
int x4_audio_start(X4DemoAudio *audio);
void x4_audio_stop(X4DemoAudio *audio);
