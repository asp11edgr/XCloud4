/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/video_pipeline.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* T27 uses this real WB reference in both Lab and Public. No settings word,
 * GPU submit, dither or Lab-only WC readback is substituted by this test. */
enum { PIXELS=1920*1080, GUARD=16, ROWS=16 };
static X4VideoNv12Layout layout(unsigned pitch)
{
    return (X4VideoNv12Layout){.visible_width=1280,.visible_height=720,
        .coded_width=1280,.coded_height=720,.storage_height=720,.pitch=pitch,
        .y_bytes=(size_t)pitch*720,.uv_bytes=(size_t)pitch*360,
        .uv_plane_offset=(size_t)pitch*720,.visible_uv_offset=(size_t)pitch*720,
        .total_nv12_bytes=(size_t)pitch*1080,.rgb_pixels=1280*720,.rgb_bytes=1280*720*4};
}
static void fixture(uint8_t *p,const X4VideoNv12Layout *l,unsigned salt)
{
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<l->pitch;++x)
        p[(size_t)y*l->pitch+x]=(uint8_t)(x*23+y*41+salt);
    for(unsigned y=0;y<360;++y)for(unsigned x=0;x<l->pitch;++x)
        p[l->uv_plane_offset+(size_t)y*l->pitch+x]=(uint8_t)(x*7+y*29+salt*3);
}
static uint32_t *owned(void)
{uint32_t *p=malloc((PIXELS+GUARD*2u)*4);assert(p);return p;}
static void guard(const uint32_t *p)
{for(unsigned i=0;i<GUARD;++i)assert(p[i]==0x6b6b6b6b&&p[GUARD+PIXELS+i]==0x6b6b6b6b);}
static void compare(const uint32_t *scratch,const uint32_t *full,unsigned first,unsigned rows)
{
    assert(!memcmp(scratch+(size_t)first*1920,full+(size_t)first*1920,(size_t)rows*1920*4));
    for(size_t i=0;i<(size_t)first*1920;++i)assert(scratch[i]==0x6b6b6b6b);
    for(size_t i=(size_t)(first+rows)*1920;i<PIXELS;++i)assert(scratch[i]==0x6b6b6b6b);
}
typedef struct {const X4VideoNv12Layout *l;const uint8_t *source;uint32_t *dst;
    X4VideoPipelineColor color;unsigned worker;atomic_uint *started;} Worker;
static void *convert(void *arg)
{
    Worker *w=arg;atomic_fetch_add(w->started,1);
    for(unsigned i=0;i<32;++i)
        assert(!x4_video_pipeline_convert_stripe_color(w->l,w->source,w->l->total_nv12_bytes,
            w->dst,PIXELS*4,1920,2,w->worker,&w->color));
    return NULL;
}
int main(void)
{
    uint64_t compared=0;
    const unsigned starts[]={0,1,2,509,1064,1079};
    for(unsigned padded=0;padded<2;++padded){
        X4VideoNv12Layout l=layout(padded?1344:1280);
        uint8_t *source=malloc(l.total_nv12_bytes);assert(source);fixture(source,&l,17);
        uint32_t *full=owned(),*slice=owned();
        for(unsigned mode=0;mode<4;++mode)for(unsigned matrix=0;matrix<2;++matrix)for(unsigned range=0;range<2;++range){
            X4VideoPipelineColor c={.matrix_coefficients=matrix?6:1,.full_range=range!=0,
                .convert_mode=mode?X4_CONVERT_SMOOTH:X4_CONVERT_ORIGINAL,.sharpness=(unsigned char)(mode?mode-1:0)};
            memset(full,0x6b,(PIXELS+GUARD*2u)*4);
            for(unsigned w=0;w<2;++w)assert(!x4_video_pipeline_convert_stripe_color(&l,source,l.total_nv12_bytes,
                full+GUARD,PIXELS*4,1920,2,w,&c));
            for(unsigned i=0;i<sizeof(starts)/sizeof(starts[0]);++i){
                unsigned first=starts[i],rows=first==1079?1:ROWS;
                memset(slice,0x6b,(PIXELS+GUARD*2u)*4);
                assert(!x4_video_pipeline_gpu_reference_720_rows(&l,source,l.total_nv12_bytes,
                    slice+GUARD,PIXELS*4,first,rows,&c));
                compare(slice+GUARD,full+GUARD,first,rows);guard(slice);guard(full);compared+=(uint64_t)rows*1920;
            }
        }
        X4VideoPipelineColor c={.matrix_coefficients=1,.convert_mode=X4_CONVERT_SMOOTH,.sharpness=2};
        memset(slice,0x6b,(PIXELS+GUARD*2u)*4);
#define BAD(l_,s_,n_,d_,b_,f_,r_,c_) assert(x4_video_pipeline_gpu_reference_720_rows((l_),(s_),(n_),(d_),(b_),(f_),(r_),(c_))==X4_PIPELINE_INVALID)
        BAD(NULL,source,l.total_nv12_bytes,slice+GUARD,PIXELS*4,0,16,&c);
        BAD(&l,NULL,l.total_nv12_bytes,slice+GUARD,PIXELS*4,0,16,&c);
        BAD(&l,source,l.total_nv12_bytes-1,slice+GUARD,PIXELS*4,0,16,&c);
        BAD(&l,source,l.total_nv12_bytes,slice+GUARD,PIXELS*4-1,0,16,&c);
        BAD(&l,source,l.total_nv12_bytes,slice+GUARD,PIXELS*4,0,0,&c);
        BAD(&l,source,l.total_nv12_bytes,slice+GUARD,PIXELS*4,0,17,&c);
        BAD(&l,source,l.total_nv12_bytes,slice+GUARD,PIXELS*4,1080,1,&c);
        BAD(&l,source,l.total_nv12_bytes,slice+GUARD,PIXELS*4,1070,16,&c);
        BAD(&l,source,l.total_nv12_bytes,slice+GUARD,PIXELS*4,0,16,NULL);
        X4VideoNv12Layout bad=l;bad.coded_height=bad.storage_height=736;
        BAD(&bad,source,l.total_nv12_bytes,slice+GUARD,PIXELS*4,0,16,&c);
        for(size_t i=0;i<PIXELS+GUARD*2u;++i)assert(slice[i]==0x6b6b6b6b);
#undef BAD
        /* Different pixels in the two production workers make accidental
         * reuse of their global scaling scratch observable. Main uses slot 2. */
        for(unsigned w=0;w<2;++w)assert(!x4_video_pipeline_convert_stripe_color(&l,source,l.total_nv12_bytes,
            full+GUARD,PIXELS*4,1920,2,w,&c));
        uint8_t *other=malloc(l.total_nv12_bytes);uint32_t *other_dst=owned();assert(other);fixture(other,&l,193);
        atomic_uint started;atomic_init(&started,0);pthread_t threads[2];Worker workers[2];
        for(unsigned w=0;w<2;++w){workers[w]=(Worker){&l,other,other_dst+GUARD,c,w,&started};
            assert(!pthread_create(&threads[w],NULL,convert,&workers[w]));}
        while(atomic_load(&started)!=2){}
        for(unsigned i=0;i<64;++i){unsigned first=(i*17)%(1080-ROWS+1);
            memset(slice,0x6b,(PIXELS+GUARD*2u)*4);
            assert(!x4_video_pipeline_gpu_reference_720_rows(&l,source,l.total_nv12_bytes,
                slice+GUARD,PIXELS*4,first,ROWS,&c));compare(slice+GUARD,full+GUARD,first,ROWS);guard(slice);
        }
        for(unsigned w=0;w<2;++w)assert(!pthread_join(threads[w],NULL));
        free(other);free(other_dst);free(full);free(slice);free(source);
    }
    printf("PASS 720p production reference: %llu pixels exact; partial-row guards, invalid geometry and concurrent scratch 0/1 versus reserved 2\n",(unsigned long long)compared);
    return 0;
}
