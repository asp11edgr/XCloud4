/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
enum { X4_SAMPLE_WIDTH = 640, X4_SAMPLE_HEIGHT = 368 };
typedef struct {
    void *state;
    uint32_t *pixels;
    int error, complete;
    unsigned frames, total;
    char stage[48];
} X4DemoVideo;
int x4_video_start(X4DemoVideo *video);
void x4_video_tick(X4DemoVideo *video);
int x4_video_stop(X4DemoVideo *video);
