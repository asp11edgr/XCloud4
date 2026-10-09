/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>
enum { X4_LIVE_WIDTH = 1280, X4_LIVE_HEIGHT = 720, X4_LIVE_AU_MAX = 2 * 1024 * 1024 };
typedef struct {
    void *state;
    uint32_t *pixels;
    unsigned width, height;
    uint64_t frames;
    int error;
    char stage[48];
} X4LiveVideo;
int x4_live_video_start(X4LiveVideo *);
int x4_live_video_feed(X4LiveVideo *, const uint8_t *, size_t, uint64_t pts);
int x4_live_video_stop(X4LiveVideo *);
