/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { X4_UI_WIDTH=1920, X4_UI_HEIGHT=1080, X4_UI_TILE=32,
    X4_UI_SCANOUTS=2 };
typedef struct { int x,y,w,h; } X4UiRect;
typedef struct { X4UiRect bounds; int dx, dy; uint32_t fill; } X4UiSlide;
typedef struct {
    uint64_t bytes;
    uint64_t elapsed_us; /* supplied by the display owner, zero in the core */
    unsigned tiles,runs;
} X4UiCopyStats;
typedef struct X4UiSurface X4UiSurface;

/* Main/UI owner only. A normal, cacheable heap canvas is distinct from both
 * VideoOut buffers. Neither this API nor any drawing API returns scanout. */
X4UiSurface *x4_ui_surface_create(void);
void x4_ui_surface_destroy(X4UiSurface *surface);
/* Compatibility-safe write access: assumes full damage until set_damage
 * narrows it after a renderer has described all writes for this frame. */
uint32_t *x4_ui_surface_pixels(X4UiSurface *surface);
void x4_ui_surface_mark_full(X4UiSurface *surface);
/* Replace only the current canvas damage. Zero rectangles add no new damage;
 * changes pending in either VideoOut buffer are deliberately preserved. */
void x4_ui_surface_set_damage(X4UiSurface *surface,const X4UiRect *rects,size_t count);
/* The caller owns a safe, inactive destination. Damage accumulates separately
 * for each of two scanouts; an older buffer receives the latest canvas values
 * for the union of changes since its last copy. No destination pixel is read. */
int x4_ui_surface_publish(X4UiSurface *surface,uint32_t *destination,
    unsigned buffer_index,X4UiCopyStats *stats);
/* Compose a short slide into scanout while keeping the cached UI stationary. */
int x4_ui_surface_publish_slide(X4UiSurface *surface,uint32_t *destination,
    unsigned buffer_index,X4UiCopyStats *stats,const X4UiSlide *slide);
size_t x4_ui_surface_heap_bytes(void);

/* Separate uninlined leaves for native/host disassembly. The low-level row
 * copy permits arbitrary source/destination alignment. Call finish_writes
 * once after the final row, before reading or submitting an NT destination. */
void x4_ui_surface_copy_row_nt(uint32_t *restrict destination,
    const uint32_t *restrict source,size_t pixels);
void x4_ui_surface_finish_writes(void);

#if X4_UI_SURFACE_TEST_HOOKS
/* Host trap only: substitutes stores, never loads or the damage algorithm.
 * The supplied destination can be PROT_NONE so any accidental load faults. */
void x4_ui_surface_test_store(uint32_t *destination,const uint32_t *values,size_t pixels);
#endif
