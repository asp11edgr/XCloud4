/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <orbis/libkernel.h>
#define X4_WIDTH 1920
#define X4_HEIGHT 1080
typedef struct {
    int handle;
    void *memory;
    off_t offset;
    size_t size;
    int allocated;
    int index;
    int64_t frame;
    uint32_t *buffers[2];
} X4Display;
int x4_display_open(X4Display *display);
uint32_t *x4_display_pixels(X4Display *display);
int x4_display_present(X4Display *display);
void x4_display_close(X4Display *display);
