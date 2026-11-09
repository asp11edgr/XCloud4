/* SPDX-License-Identifier: GPL-3.0-only */
/* Real MOVNTDQA helper, host byte/bounds evidence only, not WC/GPU timing. */
#include "../src/video/video_pipeline.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !X4_PUBLIC
static void run_pitch(unsigned pitch)
{
    const size_t source_pixels=(size_t)pitch*1080,slice_pixels=1920u*16u;
    uint32_t *owned=malloc(source_pixels*4+128),*out_owned=malloc(slice_pixels*4+132);
    uint32_t *reference=malloc(slice_pixels*4);assert(owned&&out_owned&&reference);
    uint32_t *source=owned+16,*out=out_owned+17;
    assert(!((uintptr_t)source&15u));
    for(unsigned i=0;i<16;++i){owned[i]=owned[source_pixels+16+i]=0xa1b2c3d4;
        out_owned[i]=out_owned[slice_pixels+17+i]=0xcafef00d;}
    out_owned[16]=0xcafef00d;
    for(size_t i=0;i<source_pixels;++i)source[i]=(uint32_t)(i*UINT32_C(2654435761))^(uint32_t)(i>>13);
    const unsigned firsts[]={0,532,1064,1079};
    for(unsigned window=0;window<4;++window){
        unsigned rows=window==3?1:16,first=firsts[window];
        memset(out,0x6b,slice_pixels*4);
        for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<1920;++x)
            reference[(size_t)y*1920+x]=source[(size_t)(first+y)*pitch+x];
        assert(!x4_video_pipeline_gpu_copy_rows(source,source_pixels*4,pitch,out,slice_pixels*4,first,rows));
        assert(!memcmp(reference,out,(size_t)rows*1920*4));
        for(size_t i=(size_t)rows*1920;i<slice_pixels;++i)assert(out[i]==0x6b6b6b6b);
    }
    memset(out,0x6b,slice_pixels*4);
#define BAD(s,b,p,d,n,f,r) assert(x4_video_pipeline_gpu_copy_rows((s),(b),(p),(d),(n),(f),(r))==X4_PIPELINE_INVALID)
    BAD(NULL,source_pixels*4,pitch,out,slice_pixels*4,0,16);
    BAD(source,source_pixels*4,pitch,NULL,slice_pixels*4,0,16);
    BAD(source,source_pixels*4,pitch,out,slice_pixels*4,0,0);
    BAD(source,source_pixels*4,pitch,out,slice_pixels*4,0,17);
    BAD(source,source_pixels*4,pitch,out,slice_pixels*4,1065,16);
    BAD(source,source_pixels*4,pitch,out,slice_pixels*4,1080,1);
    BAD(source,source_pixels*4,pitch,out,slice_pixels*4,UINT_MAX,1);
    BAD(source,source_pixels*4,1919,out,slice_pixels*4,0,16);
    BAD(source,source_pixels*4,1921,out,slice_pixels*4,0,16);
    BAD(source,source_pixels*4,8193,out,slice_pixels*4,0,16);
    BAD(source,source_pixels*4,pitch,out,slice_pixels*4-1,0,16);
    size_t needed=((size_t)1079*pitch+1920)*4;
    BAD(source,needed-1,pitch,out,slice_pixels*4,1064,16);
    BAD(source+1,source_pixels*4-4,pitch,out,slice_pixels*4,0,16);
    BAD(source,source_pixels*4,pitch,source,slice_pixels*4,0,16);
    BAD(source,source_pixels*4,pitch,(uint32_t *)((uint8_t *)out+1),slice_pixels*4,0,16);
    BAD((const uint32_t *)(UINTPTR_MAX-15u),source_pixels*4,pitch,out,slice_pixels*4,0,16);
#undef BAD
    for(size_t i=0;i<slice_pixels;++i)assert(out[i]==0x6b6b6b6b);
    for(size_t i=0;i<source_pixels;++i)
        assert(source[i]==((uint32_t)(i*UINT32_C(2654435761))^(uint32_t)(i>>13)));
    for(unsigned i=0;i<16;++i){assert(owned[i]==0xa1b2c3d4&&owned[source_pixels+16+i]==0xa1b2c3d4);
        assert(out_owned[i]==0xcafef00d&&out_owned[slice_pixels+17+i]==0xcafef00d);}
    assert(out_owned[16]==0xcafef00d);
    free(owned);free(out_owned);free(reference);
}
#endif
int main(void)
{
#if !X4_PUBLIC
    run_pitch(1920);run_pitch(1936);
    puts("PASS real 16-byte scanout readback equals scalar rows, top/middle/bottom, pitch1920/1936, canaries, bounds and unaligned cached destination");
#else
    puts("PASS Public builds without Lab readback calls");
#endif
    return 0;
}
