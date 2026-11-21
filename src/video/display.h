/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <orbis/libkernel.h>
#include "../media/live_trace.h"
#include "../ui/ui_surface.h"
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
    X4UiSurface *ui;
    X4UiCopyStats copy_stats;
} X4Display;
int x4_display_open(X4Display *display);
uint32_t *x4_display_pixels(X4Display *display);
/* After a bounded UI renderer, narrow the pixels-getter's safe full damage.
 * Zero rectangles reuse the canvas but do not discard either GPU backlog. */
void x4_display_set_damage(X4Display *display,const X4UiRect *rects,size_t count);
void x4_display_copy_stats(const X4Display *display,X4UiCopyStats *stats);
int x4_display_present(X4Display *display);
int x4_display_present_slide(X4Display *display,const X4UiSlide *slide);
/* Optional live-only numeric observations; the native flip/wait path is the
 * same as present(). The tag belongs to this call and is never retained. */
int x4_display_present_trace(X4Display *display, X4Trace *trace,
                            uint64_t drawn_generation, bool new_generation);
void x4_display_close(X4Display *display);
#if X4_STREAM_PROFILE
/* UI -> streaming ownership transition: retain allocations on native failure. */
int x4_display_close_checked(X4Display *display);
#endif
