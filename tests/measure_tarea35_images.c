/* SPDX-License-Identifier: GPL-3.0-only
 * T35 Phase A, host only. This does not change the production decoder/cache.
 * Include the unmodified T31 implementation to measure its real stb_image
 * decoder and to reproduce its first resampling step, not an approximation.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef union { struct { size_t bytes; uint64_t magic; } h; max_align_t align; } Track;
static size_t requested_live, requested_peak;
static uint64_t tracked_allocations;
static void *tracked_malloc(size_t bytes)
{
    if (bytes > SIZE_MAX - sizeof(Track)) return NULL;
    Track *p = malloc(sizeof(*p) + bytes);
    if (!p) return NULL;
    p->h.bytes = bytes; p->h.magic = UINT64_C(0x5834543335484541);
    requested_live += bytes;
    if (requested_live > requested_peak) requested_peak = requested_live;
    ++tracked_allocations;
    return p + 1;
}
static void tracked_free(void *pointer)
{
    if (!pointer) return;
    Track *p = (Track *)pointer - 1;
    assert(p->h.magic == UINT64_C(0x5834543335484541));
    assert(p->h.bytes <= requested_live);
    requested_live -= p->h.bytes; p->h.magic = 0; free(p);
}
#define malloc tracked_malloc
#define free tracked_free
#include "../src/ui/product_metadata.c"
#undef malloc
#undef free
#include "../src/ui/apple_ui.h"

enum { CASES_MAX = 64, TRIALS = 9, FRAME_W = 1920, FRAME_H = 1080,
       ROW_W = 384, ROW_H = 216, RADIUS = 18, CROP_W = 256, CROP_H = 144 };
typedef struct { char id[32], surface[16], path[2048]; unsigned w, h; } Case;
static Case cases[CASES_MAX];
static unsigned case_count;
/* Common UI framebuffer is BSS, shared in both models, not new cache memory. */
static uint32_t framebuffer[FRAME_W * FRAME_H];

static uint64_t clock_us(void)
{
    struct timespec t; assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * UINT64_C(1000000) + (uint64_t)t.tv_nsec / 1000u;
}
static int u64_order(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static unsigned integer(const char *s)
{
    char *end = NULL; errno = 0; unsigned long n = strtoul(s, &end, 10);
    assert(!errno && end != s && !*end && n <= UINT_MAX); return (unsigned)n;
}
static void path_join(char out[4096], const char *directory, const char *name)
{
    int n = snprintf(out, 4096, "%s/%s", directory, name);
    assert(n > 0 && n < 4096);
}
static void load_cases(const char *tsv)
{
    char base[4096]; assert(strlen(tsv) < sizeof(base)); strcpy(base, tsv);
    char *slash = strrchr(base, '/');
    if (slash) *slash = 0; else strcpy(base, ".");
    FILE *f = fopen(tsv, "rb"); assert(f);
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        assert(strchr(line, '\n') || feof(f));
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *save = NULL, *part[6] = {0}; unsigned n = 0;
        for (char *p = strtok_r(line, "\t\r\n", &save); p && n < 6;
             p = strtok_r(NULL, "\t\r\n", &save)) part[n++] = p;
        assert(n == 5 && case_count < CASES_MAX);
        Case *c = &cases[case_count++];
        assert(x4_product_id_valid(part[0]) && strlen(part[0]) < sizeof(c->id));
        assert(strlen(part[1]) < sizeof(c->surface));
        strcpy(c->id, part[0]); strcpy(c->surface, part[1]);
        c->w = integer(part[2]); c->h = integer(part[3]);
        char full[4096]; path_join(full, base, part[4]);
        assert(strlen(full) < sizeof(c->path)); strcpy(c->path, full);
        assert((!strcmp(c->surface,"row") && c->w==384 && c->h==216) ||
               (!strcmp(c->surface,"grid") && c->w==356 && c->h==200) ||
               (!strcmp(c->surface,"detail") && c->w==524 && c->h==295) ||
               (!strcmp(c->surface,"recent") && c->w==96 && c->h==96) ||
               (!strcmp(c->surface,"hero") && c->w==1920 && c->h==1080) ||
               (!strcmp(c->surface,"old512") && c->w==512 && c->h==0));
    }
    assert(!ferror(f) && fclose(f)==0 && case_count);
}
static unsigned char *read_image(const char *path, size_t *length, size_t reserve)
{
    FILE *f = fopen(path,"rb"); assert(f && fseek(f,0,SEEK_END)==0);
    long n = ftell(f); assert(n>0 && (unsigned long)n<=X4_HTTP_IMAGE_MAX);
    *length=(size_t)n; assert(fseek(f,0,SEEK_SET)==0);
    if (reserve < *length) reserve=*length;
    unsigned char *p=tracked_malloc(reserve); assert(p);
    assert(fread(p,1,*length,f)==*length && fclose(f)==0); return p;
}
static uint32_t *pack_exact(const unsigned char *decoded,unsigned w,unsigned h)
{
    uint32_t *p=tracked_malloc((size_t)w*h*4u); assert(p);
    for(size_t i=0;i<(size_t)w*h;++i) p[i]=
        (uint32_t)decoded[i*4] | (uint32_t)decoded[i*4+1]<<8 |
        (uint32_t)decoded[i*4+2]<<16 | (uint32_t)decoded[i*4+3]<<24;
    return p;
}
static size_t compare_exact(const unsigned char *decoded,const uint32_t *packed,unsigned w,unsigned h)
{
    size_t mismatches=0;
    for(size_t i=0;i<(size_t)w*h;++i) for(unsigned channel=0;channel<4;++channel)
        if((unsigned char)(packed[i]>>(8u*channel))!=decoded[i*4+channel]) ++mismatches;
    return mismatches;
}
static void ppm(const char *directory,const char *id,const char *suffix,
    const uint32_t *p,unsigned stride,unsigned x,unsigned y,unsigned w,unsigned h)
{
    assert(w<=1920 && x+w<=stride);
    char name[256],path[4096]; int n=snprintf(name,sizeof(name),"%s-%s.ppm",id,suffix);
    assert(n>0 && (size_t)n<sizeof(name)); path_join(path,directory,name);
    FILE *f=fopen(path,"wb"); assert(f);
    assert(fprintf(f,"P6\n%u %u\n255\n",w,h)>0);
    unsigned char row[1920*3];
    for(unsigned py=0;py<h;++py){
        for(unsigned px=0;px<w;++px){uint32_t c=p[(size_t)(y+py)*stride+x+px];
            row[px*3]=(unsigned char)c;row[px*3+1]=(unsigned char)(c>>8);row[px*3+2]=(unsigned char)(c>>16);}
        assert(fwrite(row,3,w,f)==w);
    }
    assert(fclose(f)==0);
}
static const Case *find_case(const char *id,const char *surface)
{
    for(unsigned i=0;i<case_count;++i)
        if(!strcmp(cases[i].id,id)&&!strcmp(cases[i].surface,surface))return &cases[i];
    return NULL;
}
static size_t renderer_exact(const unsigned char *decoded,const X4CoverRaster *r)
{
    memset(framebuffer,0,sizeof(framebuffer));
    /* Same real T31 drawing entry point, with a target-size raster. The visible
     * unrounded interior must remain identical; corners intentionally blend. */
    x4_apple_blit_image_rounded(framebuffer,0,0,(int)r->width,(int)r->height,
        RADIUS,r,X4_GRAD_NONE);
    size_t mismatches=0;
    for(unsigned y=RADIUS;y+RADIUS<r->height;++y)
        for(unsigned x=RADIUS;x+RADIUS<r->width;++x){
            uint32_t got=framebuffer[(size_t)y*FRAME_W+x];
            size_t offset=((size_t)y*r->width+x)*4u;
            for(unsigned channel=0;channel<3;++channel)
                if((unsigned char)(got>>(8u*channel))!=decoded[offset+channel])++mismatches;
        }
    return mismatches;
}
static void comparison(const Case *c,const X4CoverRaster *new_raster,const char *directory)
{
    const Case *old=find_case(c->id,"old512"); assert(old);
    /* Save the new card already drawn by renderer_exact before reusing frame. */
    ppm(directory,c->id,"after",framebuffer,FRAME_W,0,0,ROW_W,ROW_H);
    ppm(directory,c->id,"after-crop100",framebuffer,FRAME_W,
        (ROW_W-CROP_W)/2,(ROW_H-CROP_H)/2,CROP_W,CROP_H);
    size_t length; unsigned char *encoded=read_image(old->path,&length,0);
    X4CoverRasters legacy={0}; assert(x4_cover_decode(encoded,length,&legacy)==0);
    assert(legacy.surface[X4_COVER_GRID].width==320 && legacy.surface[X4_COVER_GRID].height==224);
    memset(framebuffer,0,sizeof(framebuffer));
    x4_apple_blit_image_rounded(framebuffer,0,0,ROW_W,ROW_H,RADIUS,
        &legacy.surface[X4_COVER_GRID],X4_GRAD_NONE);
    ppm(directory,c->id,"before",framebuffer,FRAME_W,0,0,ROW_W,ROW_H);
    ppm(directory,c->id,"before-crop100",framebuffer,FRAME_W,
        (ROW_W-CROP_W)/2,(ROW_H-CROP_H)/2,CROP_W,CROP_H);
    printf("{\"type\":\"comparison\",\"id\":\"%s\",\"before\":\"T31 legacy-selected old512 JPEG -> x4_cover_decode 320x224 -> real apple blit 384x216\","
        "\"after\":\"horizontal JPEG 384x216 -> exact RGBA -> real apple blit 384x216\","
        "\"display_size\":[384,216],\"crop_native_size\":[256,144],\"gradients_disabled_for_both\":true,\"new_raster_bytes\":%zu}\n",
        c->id,(size_t)new_raster->width*new_raster->height*4u);
    x4_cover_rasters_free(&legacy); tracked_free(encoded);
}
static size_t measure_image(const Case *c,const char *directory,unsigned *compared)
{
    assert(requested_live==0 && decode_allocated==0);
    size_t length; unsigned char *encoded=read_image(c->path,&length,0);
    int w=0,h=0,channels=0; assert(stbi_info_from_memory(encoded,(int)length,&w,&h,&channels));
    assert(w==(int)c->w && (!c->h || h==(int)c->h));
    requested_peak=requested_live; uint64_t times[TRIALS],total=0; unsigned char *decoded=NULL;
    for(unsigned trial=0;trial<TRIALS;++trial){
        uint64_t start=clock_us();
        decoded=stbi_load_from_memory(encoded,(int)length,&w,&h,&channels,4);
        times[trial]=clock_us()-start;total+=times[trial];assert(decoded);
        if(trial+1<TRIALS)stbi_image_free(decoded);
    }
    size_t decode_peak=requested_peak; qsort(times,TRIALS,sizeof(times[0]),u64_order);
    uint32_t *packed=pack_exact(decoded,(unsigned)w,(unsigned)h);
    size_t exact=compare_exact(decoded,packed,(unsigned)w,(unsigned)h);assert(exact==0);
    size_t visible=0; X4CoverRaster r={(unsigned)w,(unsigned)h,packed};
    if(!strcmp(c->surface,"row")){
        visible=renderer_exact(decoded,&r);assert(visible==0);
    }
    /* Exactness checks no longer need the STB-owned pixels. Retire them before
     * entering the legacy decoder, whose production serial guard requires
     * decode_allocated==0. The separately packed raster remains owned here. */
    stbi_image_free(decoded); decoded=NULL; assert(decode_allocated==0);
    if(!strcmp(c->surface,"row")&&find_case(c->id,"old512")){
        comparison(c,&r,directory);++*compared;
    }
    printf("{\"type\":\"image\",\"id\":\"%s\",\"surface\":\"%s\",\"width\":%d,\"height\":%d,"
        "\"encoded_bytes\":%zu,\"rgba_bytes\":%zu,\"trials\":%u,\"decode_us_p50\":%" PRIu64 ","
        "\"decode_us_mean\":%.3f,\"decode_us_p95\":%" PRIu64 ",\"decode_us_max\":%" PRIu64 ","
        "\"decode_peak_requested_bytes_including_encoded\":%zu,\"cache_copy_mismatches\":%zu,\"row_renderer_interior_mismatches\":%zu}\n",
        c->id,c->surface,w,h,length,(size_t)w*(unsigned)h*4u,TRIALS,times[TRIALS/2],(double)total/TRIALS,
        times[TRIALS-1],times[TRIALS-1],decode_peak,exact,visible);
    tracked_free(packed);stbi_image_free(decoded);tracked_free(encoded);
    assert(requested_live==0 && decode_allocated==0);
    return decode_peak-length;
}
static size_t memory_model(const Case *source,int new_model)
{
    assert(requested_live==0 && decode_allocated==0);requested_peak=0;
    const size_t cache_bytes=24u*1024u*1024u, frame_bytes=1920u*1080u*4u;
    void *cache=tracked_malloc(cache_bytes),*metadata=tracked_malloc(256u*1024u+1u);
    assert(cache && metadata);memset(cache,0,cache_bytes);memset(metadata,0,256u*1024u+1u);
    void *background[2]={NULL,NULL},*rows=NULL;
    if(new_model){
        for(unsigned i=0;i<2;++i){background[i]=tracked_malloc(frame_bytes);assert(background[i]);memset(background[i],0,frame_bytes);}
        rows=tracked_malloc(1024u*1024u);assert(rows);memset(rows,0,1024u*1024u);
    }
    size_t length;unsigned char *encoded=read_image(source->path,&length,X4_HTTP_IMAGE_MAX+1u);
    size_t persistent=requested_live;int w,h,channels;
    if(new_model){
        unsigned char *decoded=stbi_load_from_memory(encoded,(int)length,&w,&h,&channels,4);assert(decoded);
        assert(w==1920 && h==1080);memcpy(background[1],decoded,frame_bytes);stbi_image_free(decoded);
    }else{
        X4CoverRasters old={0};assert(x4_cover_decode(encoded,length,&old)==0);x4_cover_rasters_free(&old);
    }
    size_t peak=requested_peak;
    printf("{\"type\":\"memory\",\"model\":\"%s\",\"scope\":\"allocated policy-full-cache simulation plus real serial decoder; not live PS4 RSS\","
        "\"LRU_reserved_bytes\":%zu,\"background_cache_count\":%u,\"background_cache_bytes\":%zu,"
        "\"persistent_requested_bytes\":%zu,\"peak_requested_bytes\":%zu,\"shared_framebuffer_BSS_excluded_bytes\":%zu,"
        "\"tracker_headers_libc_stdio_allocator_overhead_excluded\":true}\n",
        new_model?"T35 proposed":"T31 baseline",cache_bytes,new_model?2u:0u,new_model?frame_bytes*2u:0u,persistent,peak,sizeof(framebuffer));
    tracked_free(encoded);tracked_free(rows);
    for(unsigned i=0;i<2;++i)tracked_free(background[i]);
    tracked_free(metadata);tracked_free(cache);assert(requested_live==0 && decode_allocated==0);return peak;
}
int main(int argc,char **argv)
{
    assert(argc==3);load_cases(argv[1]);unsigned compared=0;const Case *hero=NULL,*old=NULL;
    size_t highest_hero_decode_peak=0;
    for(unsigned i=0;i<case_count;++i){
        size_t peak=measure_image(&cases[i],argv[2],&compared);
        if(!strcmp(cases[i].surface,"hero")&&(!hero||peak>highest_hero_decode_peak)){
            hero=&cases[i];highest_hero_decode_peak=peak;
        }
        if(!old&&!strcmp(cases[i].surface,"old512"))old=&cases[i];
    }
    assert(compared==3 && hero && old);
    size_t base=memory_model(old,0),next=memory_model(hero,1),delta=next>base?next-base:0;
    assert(delta<=64u*1024u*1024u);
    const size_t bound=24u*1024u*1024u+2u*1920u*1080u*4u+16u*1024u*1024u+
        X4_HTTP_IMAGE_MAX+1u+256u*1024u+1u+1024u*1024u;
    printf("{\"type\":\"summary\",\"images\":%u,\"compared_games\":%u,\"incremental_peak_requested_bytes\":%zu,"
        "\"incremental_limit_bytes\":%u,\"proposed_policy_accounting_bound_bytes\":%zu,\"cache_copy_all_equal\":true,"
        "\"row_visible_interior_all_equal\":true,\"retained_requested_bytes\":%zu,\"tracked_allocations\":%" PRIu64 ","
        "\"note\":\"host decode timings only; no PS4 timing claim; new row cache and RGB visible interior preserve decoded pixels exactly\"}\n",
        case_count,compared,delta,64u*1024u*1024u,bound,requested_live,tracked_allocations);
    return 0;
}
