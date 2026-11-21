/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui_surface.h"
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

enum { COLS=(X4_UI_WIDTH+X4_UI_TILE-1)/X4_UI_TILE,
    ROWS=(X4_UI_HEIGHT+X4_UI_TILE-1)/X4_UI_TILE,
    TILES=COLS*ROWS, WORDS=(TILES+63)/64 };
struct X4UiSurface {
    void *allocation;
    uint32_t *pixels;
    uint64_t damage[WORDS],pending[X4_UI_SCANOUTS][WORDS];
};

static void fill_damage(uint64_t bits[WORDS])
{
    for(unsigned i=0;i<WORDS;++i)bits[i]=UINT64_MAX;
    if(TILES%64)bits[WORDS-1]=(UINT64_C(1)<<(TILES%64))-1;
}
static int tile_set(const uint64_t bits[WORDS],unsigned tile)
{return (bits[tile/64]>>(tile%64))&1u;}

X4UiSurface *x4_ui_surface_create(void)
{
    X4UiSurface *s=calloc(1,sizeof(*s));
    if(!s)return NULL;
    /* malloc/calloc is ordinary process memory, not GPU direct memory. Keep
     * a 64-byte aligned source without relying on a platform aligned_alloc. */
    size_t bytes=(size_t)X4_UI_WIDTH*X4_UI_HEIGHT*sizeof(uint32_t);
    s->allocation=calloc(1,bytes+63u);
    if(!s->allocation){free(s);return NULL;}
    s->pixels=(uint32_t *)(((uintptr_t)s->allocation+63u)&~(uintptr_t)63u);
    for(unsigned i=0;i<X4_UI_SCANOUTS;++i)fill_damage(s->pending[i]);
    return s;
}
void x4_ui_surface_destroy(X4UiSurface *s)
{if(s){free(s->allocation);free(s);}}
size_t x4_ui_surface_heap_bytes(void)
{return sizeof(X4UiSurface)+(size_t)X4_UI_WIDTH*X4_UI_HEIGHT*sizeof(uint32_t)+63u;}
void x4_ui_surface_mark_full(X4UiSurface *s)
{if(s)fill_damage(s->damage);}
uint32_t *x4_ui_surface_pixels(X4UiSurface *s)
{if(!s)return NULL;x4_ui_surface_mark_full(s);return s->pixels;}

static void mark_rect(uint64_t bits[WORDS], X4UiRect r)
{
    if (r.w <= 0 || r.h <= 0) return;
    /* 64-bit addition makes clipping defined even for INT_MIN/INT_MAX. */
    int64_t x0=r.x,y0=r.y,x1=x0+(int64_t)r.w,y1=y0+(int64_t)r.h;
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>X4_UI_WIDTH)x1=X4_UI_WIDTH;
    if(y1>X4_UI_HEIGHT)y1=X4_UI_HEIGHT;
    if(x0>=x1||y0>=y1)return;
    unsigned tx0=(unsigned)x0/X4_UI_TILE,ty0=(unsigned)y0/X4_UI_TILE;
    unsigned tx1=(unsigned)(x1-1)/X4_UI_TILE,ty1=(unsigned)(y1-1)/X4_UI_TILE;
    for(unsigned y=ty0;y<=ty1;++y)for(unsigned x=tx0;x<=tx1;++x){
        unsigned tile=y*COLS+x;bits[tile/64]|=UINT64_C(1)<<(tile%64);
    }
}

void x4_ui_surface_set_damage(X4UiSurface *s,const X4UiRect *rects,size_t count)
{
    if(!s)return;
    memset(s->damage,0,sizeof(s->damage));
    if(count&&!rects){fill_damage(s->damage);return;}
    for(size_t i=0;i<count;++i)mark_rect(s->damage, rects[i]);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void x4_ui_surface_copy_row_nt(uint32_t *restrict dst,
    const uint32_t *restrict src,size_t pixels)
{
    /* Direct VideoOut allocations and tile-run starts are 16-byte aligned.
     * The scalar prefix/tail also make this leaf safe for host guard tests. */
    while(pixels&&((uintptr_t)dst&15u)){
        uint32_t value=*src++;
#if X4_UI_SURFACE_TEST_HOOKS
        x4_ui_surface_test_store(dst,&value,1);
#else
        *dst=value;
#endif
        ++dst;--pixels;
    }
#if defined(__SSE2__)
    while(pixels>=4){
        __m128i value=_mm_loadu_si128((const __m128i *)(const void *)src);
#if X4_UI_SURFACE_TEST_HOOKS
        _Alignas(16) uint32_t values[4];
        _mm_store_si128((__m128i *)(void *)values,value);
        x4_ui_surface_test_store(dst,values,4);
#else
        _mm_stream_si128((__m128i *)(void *)dst,value);
#endif
        src+=4;dst+=4;pixels-=4;
    }
#endif
    while(pixels--){
        uint32_t value=*src++;
#if X4_UI_SURFACE_TEST_HOOKS
        x4_ui_surface_test_store(dst,&value,1);
#else
        *dst=value;
#endif
        ++dst;
    }
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void x4_ui_surface_finish_writes(void)
{
#if defined(__SSE2__)
    _mm_sfence();
#endif
}

int x4_ui_surface_publish(X4UiSurface *s,uint32_t *destination,
    unsigned buffer_index,X4UiCopyStats *stats)
{
    X4UiCopyStats result={0};
    if(stats)*stats=result;
    if(!s||!s->pixels||!destination||buffer_index>=X4_UI_SCANOUTS)return -1;
    for(unsigned i=0;i<WORDS;++i){
        for(unsigned b=0;b<X4_UI_SCANOUTS;++b)s->pending[b][i]|=s->damage[i];
        s->damage[i]=0;
    }
    const uint64_t *dirty=s->pending[buffer_index];
    /* Adjacent dirty tiles become a single row span. An all-dirty frame
     * therefore performs one linear NT copy per row, not 2,040 tiny copies. */
    for(unsigned ty=0;ty<ROWS;++ty){
        unsigned tx=0;
        while(tx<COLS){
            while(tx<COLS&&!tile_set(dirty,ty*COLS+tx))++tx;
            if(tx==COLS)break;
            unsigned first=tx;
            do{++tx;}while(tx<COLS&&tile_set(dirty,ty*COLS+tx));
            unsigned x=first*X4_UI_TILE,y=ty*X4_UI_TILE;
            unsigned right=tx*X4_UI_TILE;if(right>X4_UI_WIDTH)right=X4_UI_WIDTH;
            unsigned height=X4_UI_TILE;if(y+height>X4_UI_HEIGHT)height=X4_UI_HEIGHT-y;
            size_t width=right-x;
            for(unsigned row=0;row<height;++row){
                size_t at=(size_t)(y+row)*X4_UI_WIDTH+x;
                x4_ui_surface_copy_row_nt(destination+at,s->pixels+at,width);
            }
            ++result.runs;result.tiles+=tx-first;
            result.bytes+=(uint64_t)width*height*sizeof(uint32_t);
        }
    }
    if(result.bytes)x4_ui_surface_finish_writes();
    memset(s->pending[buffer_index],0,sizeof(s->pending[buffer_index]));
    if(stats)*stats=result;
    return 0;
}

int x4_ui_surface_publish_slide(X4UiSurface *s,uint32_t *destination,
    unsigned buffer_index,X4UiCopyStats *stats,const X4UiSlide *slide)
{
    const int rc = x4_ui_surface_publish(s, destination, buffer_index, stats);
    if (rc || !slide || (!slide->dx && !slide->dy)) return rc;
    const X4UiRect r = slide->bounds;
    if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 ||
        r.w > X4_UI_WIDTH - r.x || r.h > X4_UI_HEIGHT - r.y ||
        slide->dx <= -r.w || slide->dx >= r.w || slide->dy <= -r.h || slide->dy >= r.h)
        return rc;

    /* Each scanout gets the canonical tiles back on its next publish. This
     * also repairs the old region if a new transition interrupts this one. */
    mark_rect(s->pending[buffer_index], r);
    uint32_t fill[X4_UI_WIDTH];
    const int left = slide->dx > 0 ? slide->dx : 0;
    const int right = slide->dx < 0 ? r.w + slide->dx : r.w;
    for (int x = 0; x < r.w; ++x) fill[x] = slide->fill;
    for (int y = 0; y < r.h; ++y) {
        uint32_t *dst = destination + (size_t)(r.y + y) * X4_UI_WIDTH + r.x;
        const int sy = y - slide->dy;
        if (sy < 0 || sy >= r.h) {
            x4_ui_surface_copy_row_nt(dst, fill, (size_t)r.w);
            continue;
        }
        const uint32_t *src = s->pixels + (size_t)(r.y + sy) * X4_UI_WIDTH + r.x;
        if (left) x4_ui_surface_copy_row_nt(dst, fill, (size_t)left);
        x4_ui_surface_copy_row_nt(dst + left, src + (left - slide->dx), (size_t)(right - left));
        if (right < r.w) x4_ui_surface_copy_row_nt(dst + right, fill, (size_t)(r.w - right));
    }
    x4_ui_surface_finish_writes();
    if (stats) stats->bytes += (uint64_t)r.w * r.h * sizeof(uint32_t);
    return 0;
}
