/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <orbis/libkernel.h>
#include "../media/live_trace.h"
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
/* Optional live-only numeric observations; the native flip/wait path is the
 * same as present(). The tag belongs to this call and is never retained. */
int x4_display_present_trace(X4Display *display, X4Trace *trace,
                            uint64_t drawn_generation, bool new_generation);
void x4_display_close(X4Display *display);
