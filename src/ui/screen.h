/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../input/controller.h"
#include "../video/h264_demo.h"
#include "../audio/demo_audio.h"
typedef struct { int page; int selected; int exit_requested; int exit_error; } X4Screen;
void x4_screen_update(X4Screen *screen, uint32_t pressed, uint32_t held);
void x4_screen_draw(const X4Screen *screen, const X4Controller *controller, uint32_t *pixels);
void x4_media_draw(const X4DemoVideo *video, const X4DemoAudio *audio, uint32_t *pixels);
void x4_exit_error_draw(int error, uint32_t *pixels);
