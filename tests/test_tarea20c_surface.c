/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/ui_surface.h"
#include "ui_surface_test_trap.h"
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIXELS ((size_t)X4_UI_WIDTH*X4_UI_HEIGHT)
static uint32_t state=0x7124abcd;
static uint32_t random_u32(void)
{state=state*1664525u+1013904223u;return state;}
static void paint(uint32_t *p,X4UiRect r,uint32_t color)
{
    int64_t x0=r.x,y0=r.y,x1=x0+(int64_t)r.w,y1=y0+(int64_t)r.h;
    if(r.w<=0||r.h<=0)return;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>X4_UI_WIDTH)x1=X4_UI_WIDTH;
    if(y1>X4_UI_HEIGHT)y1=X4_UI_HEIGHT;
    if(x0>=x1||y0>=y1)return;
    for(int64_t y=y0;y<y1;++y)for(int64_t x=x0;x<x1;++x)
        p[(size_t)y*X4_UI_WIDTH+(size_t)x]=color;
}
int main(void)
{
    X4UiSurface *surface=x4_ui_surface_create();assert(surface);
    uint32_t *canvas=x4_ui_surface_pixels(surface);assert(canvas&&!( (uintptr_t)canvas&63u));
    X4UiCopyStats stats;
#if X4_UI_SURFACE_TEST_HOOKS
    X4UiTestTrap trap;x4_ui_test_trap_open(&trap);x4_ui_test_trap_positive_read(&trap);
    uint32_t **dest=trap.destination,**shown=trap.mirror;
    puts("PASS positive control: a real destination read raises SIGSEGV");
#else
    uint32_t *dest[X4_UI_SCANOUTS],*shown[X4_UI_SCANOUTS];
    for(unsigned b=0;b<X4_UI_SCANOUTS;++b){dest[b]=malloc(PIXELS*4);assert(dest[b]);shown[b]=dest[b];}
#endif
    for(size_t i=0;i<PIXELS;++i)canvas[i]=UINT32_C(0xff000000)|random_u32();
    assert(!x4_ui_surface_publish(surface,dest[0],0,&stats));
    assert(stats.bytes==PIXELS*4&&stats.tiles==2040&&stats.runs==34);
    assert(!memcmp(shown[0],canvas,PIXELS*4));
    x4_ui_surface_set_damage(surface,NULL,0);
    assert(!x4_ui_surface_publish(surface,dest[1],1,&stats));assert(stats.bytes==PIXELS*4);
    assert(!memcmp(shown[1],canvas,PIXELS*4));
    assert(!x4_ui_surface_publish(surface,dest[0],0,&stats));assert(!stats.bytes);
    puts("PASS initial full copy for both scanouts; a clean frame performs zero stores");

    for(unsigned frame=0;frame<120;++frame){
        canvas=x4_ui_surface_pixels(surface); /* default damage can be narrowed */
        X4UiRect rects[2]={
            {(int)(random_u32()%2000)-40,(int)(random_u32()%1160)-40,
                1+(int)(random_u32()%171),1+(int)(random_u32()%103)},
            {1918,1078,INT_MAX,INT_MAX}};
        paint(canvas,rects[0],random_u32());paint(canvas,rects[1],frame|UINT32_C(0xff000000));
        x4_ui_surface_set_damage(surface,rects,2);
        /* Publish buffer0 repeatedly: buffer1 must retain every intermediate
         * dirty region, not just the latest frame's region. */
        unsigned b=frame%7?0:1;
        assert(!x4_ui_surface_publish(surface,dest[b],b,&stats));
        assert(stats.bytes>0&&stats.bytes<PIXELS*4);
        assert(!memcmp(shown[b],canvas,PIXELS*4));
#if X4_UI_SURFACE_TEST_HOOKS
        x4_ui_test_trap_assert_guards(&trap);
#endif
    }
    x4_ui_surface_set_damage(surface,NULL,0);
    for(unsigned b=0;b<2;++b){assert(!x4_ui_surface_publish(surface,dest[b],b,&stats));assert(!memcmp(shown[b],canvas,PIXELS*4));}
    puts("PASS 120 clipped dirty generations: both buffers equal the full redraw pixel-for-pixel, including delayed buffer reuse");

    X4UiRect none[]={{INT_MIN,INT_MIN,INT_MAX,INT_MAX},{INT_MAX,INT_MAX,INT_MAX,INT_MAX},{0,0,0,10},{0,0,10,-1}};
    x4_ui_surface_set_damage(surface,none,4);
    assert(!x4_ui_surface_publish(surface,dest[0],0,&stats));assert(!stats.bytes);
    assert(x4_ui_surface_publish(surface,NULL,0,&stats)<0&&x4_ui_surface_publish(surface,dest[0],2,&stats)<0);
    assert(!stats.bytes);
    canvas=x4_ui_surface_pixels(surface);canvas[12345]^=UINT32_C(0x00ffffff);
    assert(!x4_ui_surface_publish(surface,dest[0],0,&stats));assert(stats.bytes==PIXELS*4);
    assert(!memcmp(shown[0],canvas,PIXELS*4));
    x4_ui_surface_set_damage(surface,NULL,0);
    assert(!x4_ui_surface_publish(surface,dest[1],1,&stats));assert(stats.bytes==PIXELS*4);
    assert(!memcmp(shown[1],canvas,PIXELS*4));
    puts("PASS legacy pixels getter defaults to full damage without losing the other scanout's backlog");

    const X4UiSlide slides[] = {
        {{99, 127, 249, 93}, 31, 0, UINT32_C(0xff428751)},
        {{911, 205, 123, 107}, -11, 17, UINT32_C(0xff1c2523)},
    };
    for (unsigned b = 0; b < 2; ++b) {
        const X4UiSlide slide = slides[b];
        const X4UiRect r = slide.bounds;
        assert(!x4_ui_surface_publish_slide(surface, dest[b], b, &stats, &slide));
        assert(stats.bytes == (uint64_t)r.w * r.h * 4);
        for (int y = 0; y < X4_UI_HEIGHT; ++y) for (int x = 0; x < X4_UI_WIDTH; ++x) {
            size_t at = (size_t)y * X4_UI_WIDTH + x;
            if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) {
                assert(shown[b][at] == canvas[at]);
                continue;
            }
            const int sx = x - slide.dx, sy = y - slide.dy;
            const bool covered = sx >= r.x && sx < r.x + r.w && sy >= r.y && sy < r.y + r.h;
            assert(shown[b][at] == (covered ? canvas[(size_t)sy * X4_UI_WIDTH + sx] : slide.fill));
        }
    }
    for (unsigned b = 0; b < 2; ++b) {
        assert(!x4_ui_surface_publish(surface, dest[b], b, &stats));
        assert(!memcmp(shown[b], canvas, PIXELS * 4));
        assert(!x4_ui_surface_publish(surface, dest[b], b, &stats) && !stats.bytes);
    }
    puts("PASS slides are clipped, leave the canonical canvas intact, and repair both scanouts");
    /* Exercise the same copy leaf's scalar prefix/tail and unaligned source,
     * not just the aligned, multiple-of-four tile spans used by publish(). */
    uint32_t source[40];
    for(unsigned i=0;i<40;++i)source[i]=UINT32_C(0xbe000000)+i;
    const size_t at=(size_t)73*X4_UI_WIDTH;
    for(unsigned offset=0;offset<4;++offset)for(unsigned skip=0;skip<4;++skip)
        for(size_t count=0;count<=33;++count){
            for(size_t i=0;i<44;++i)shown[0][at+i]=UINT32_C(0x71abcdef);
            x4_ui_surface_copy_row_nt(dest[0]+at+4+offset,source+skip,count);
            x4_ui_surface_finish_writes();
            for(size_t i=0;i<44;++i){
                uint32_t expected=(i>=4+offset&&i<4+offset+count)?
                    source[skip+i-(4+offset)]:UINT32_C(0x71abcdef);
                assert(shown[0][at+i]==expected);
            }
        }
    puts("PASS NT leaf: all 4 destination/source alignments, zero-length and scalar tails, with surrounding canaries");
#if X4_UI_SURFACE_TEST_HOOKS
    assert(trap.stores&&trap.pixels_stored>=2*PIXELS);x4_ui_test_trap_close(&trap);
#else
    for(unsigned b=0;b<2;++b)free(dest[b]);
#endif
    x4_ui_surface_destroy(surface);
    printf("PASS TAREA20c WB canvas and write-only presentation (%s), heap_bytes=%zu\n",
#if X4_UI_SURFACE_TEST_HOOKS
        "PROT_NONE destination trap with only store substitution",
#else
        "actual SSE2 NT stores and sfence",
#endif
        x4_ui_surface_heap_bytes());
    return 0;
}
