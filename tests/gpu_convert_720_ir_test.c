/* SPDX-License-Identifier: GPL-3.0-only */
/* 720p -> 1080p: execute the actual .ll arithmetic on x86 with bounds-checked raw-buffer ABI.
 * No C clone substitutes for the shader: the test runner changes only entry
 * calling convention and raw I/O intrinsics. This is still not a PS4 run.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/video/video_pipeline.h"
typedef uint32_t V4 __attribute__((ext_vector_type(4)));
typedef void (*Kernel)(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
extern void x4_gpu_test_original720(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
extern void x4_gpu_test_smooth720_0(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
extern void x4_gpu_test_smooth720_1(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
extern void x4_gpu_test_smooth720_2(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
static Kernel kernels[4]={x4_gpu_test_original720,x4_gpu_test_smooth720_0,x4_gpu_test_smooth720_1,x4_gpu_test_smooth720_2};
static unsigned maximum;static uint64_t outside,pixels,cases,stores;
static uintptr_t raw_address(V4 v){return v[0]|((uint64_t)v[1]<<32);}
uint32_t x4_gpu_test_raw_load(V4 d,uint32_t offset,uint32_t scalar,uint32_t flags)
{
    assert(!scalar && !flags && offset<=d[2] && 4<=d[2]-offset && !(offset&3));
    uint32_t v;memcpy(&v,(const void *)(raw_address(d)+offset),4);return v;
}
void x4_gpu_test_raw_store(uint32_t value,V4 d,uint32_t offset,uint32_t scalar,uint32_t flags)
{
    assert(!scalar && !flags && offset<=d[2] && 4<=d[2]-offset && !(offset&3));
    ++stores;
    memcpy((void *)(raw_address(d)+offset),&value,4);
}
static V4 descriptor(const void *p,size_t bytes)
{uintptr_t a=(uintptr_t)p;assert(bytes<=UINT32_MAX);return (V4){(uint32_t)a,(uint32_t)(a>>32),(uint32_t)bytes,0};}
static X4VideoNv12Layout layout(unsigned pitch)
{
    size_t y=(size_t)pitch*720,uv=y/2;
    return (X4VideoNv12Layout){.visible_width=1280,.visible_height=720,
        .coded_width=1280,.coded_height=720,.storage_height=720,.pitch=pitch,
        .y_bytes=y,.uv_bytes=uv,.uv_plane_offset=y,.total_nv12_bytes=y+uv,
        .visible_uv_offset=y,.rgb_pixels=921600,.rgb_bytes=3686400};
}
static void compare(const uint8_t *source,const X4VideoNv12Layout *l,
    unsigned matrix,bool full,const char *name,unsigned dpitch)
{
    static const int32_t coefficients[4][6]={
        {9539,13075,-3209,-6660,16525,16},{9539,14686,-1747,-4366,17305,16},
        {8192,11485,-2819,-5850,14516,0},{8192,12901,-1535,-3835,15201,0}};
    const int32_t *m=coefficients[(full?2:0)+(matrix==1?1:0)];
    uint32_t params[8]={l->pitch,dpitch};
    for(unsigned i=0;i<6;++i)params[i+2]=(uint32_t)m[i];
    size_t bytes=(size_t)dpitch*1080*4;
    uint32_t *actual=malloc(bytes+64),*reference=malloc(bytes+64);assert(actual&&reference);
    V4 yd=descriptor(source,l->y_bytes),uvd=descriptor(source+l->uv_plane_offset,l->uv_bytes);
    V4 out=descriptor(actual,bytes),pd=descriptor(params,sizeof(params));
    for(unsigned mode=0;mode<4;++mode){
        memset(actual,0xa6,bytes+64);memset(reference,0xa6,bytes+64);
        X4VideoPipelineColor color={matrix,full,mode?X4_CONVERT_SMOOTH:X4_CONVERT_ORIGINAL,mode?mode-1:0};
        for(unsigned stripe=0;stripe<2;++stripe)
            assert(!x4_video_pipeline_convert_stripe_color(l,source,l->total_nv12_bytes,
                reference,bytes,dpitch,2,stripe,&color));
        Kernel kernel=kernels[mode];
        for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1920;++x)
            kernel(yd,uvd,out,pd,x>>3,y>>3,x&7,y&7);
        unsigned local_max=0;uint64_t local_outside=0;
        for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1920;++x){
            uint32_t a=actual[(size_t)y*dpitch+x],b=reference[(size_t)y*dpitch+x];bool bad=false;
            for(unsigned ch=0;ch<4;++ch){int delta=(int)((a>>(ch*8))&255)-(int)((b>>(ch*8))&255);
                unsigned d=(unsigned)(delta<0?-delta:delta);if(d>local_max)local_max=d;if(d)bad=true;}
            if(bad) {
                if(!local_outside)fprintf(stderr,"720p mismatch %s mode=%u x=%u y=%u actual=%08x CPU=%08x\n",name,mode,x,y,a,b);
                ++local_outside;
            }
        }
        for(size_t i=bytes;i<bytes+64;++i)assert(((uint8_t *)actual)[i]==0xa6 && ((uint8_t *)reference)[i]==0xa6);
        if(dpitch>1920)for(unsigned y=0;y<1080;++y)for(unsigned x=1920;x<dpitch;++x)
            assert(actual[(size_t)y*dpitch+x]==UINT32_C(0xa6a6a6a6));
        /* Out-of-range invocations are deliberately dispatched by the host;
         * the same early guard must preserve all pixels and canaries.
         */
        uint64_t before=stores;kernel(yd,uvd,out,pd,240,0,0,0);kernel(yd,uvd,out,pd,0,135,0,0);
        assert(stores==before);
        printf("{\"fixture\":\"%s\",\"mode\":%u,\"matrix\":%u,\"full\":%u,\"max_channel_difference\":%u,\"pixels_outside\":%llu}\n",
            name,mode,matrix,full?1u:0u,local_max,(unsigned long long)local_outside);fflush(stdout);
        if(local_max>maximum)maximum=local_max;outside+=local_outside;pixels+=2073600;++cases;
        assert(!local_outside && local_max==0);
    }
    free(actual);free(reference);
}
static void synthetic(uint8_t *p,size_t bytes)
{
    uint32_t state=UINT32_C(0xf2134927);
    for(size_t i=0;i<bytes;++i){state^=state<<13;state^=state>>17;state^=state<<5;p[i]=(uint8_t)state;}
    static const uint8_t yvalues[]={0,1,15,16,17,127,128,234,235,236,254,255};
    for(unsigned y=0;y<720;++y)for(unsigned x=0;x<1280;++x) {
        if(x<16 || x>=1264 || y<2 || y>=718)
            p[(size_t)y*1280+x]=yvalues[(x+y)%sizeof(yvalues)];
    }
    /* Distinct last rows/column expose incorrect source borders, order of
     * interpolation/rounding, or sampling of NV12 padding. Chroma extremes
     * cover both horizontal and vertical smoothing at the clamped border. */
    uint8_t *uv=p+1280u*720u;
    for(unsigned y=0;y<360;++y)for(unsigned x=0;x<1280;x+=2) {
        if(x<16 || x>=1264 || y<2 || y>=358) {
            uv[(size_t)y*1280+x]=(uint8_t)((x+y)&1?0:255);
            uv[(size_t)y*1280+x+1]=(uint8_t)((x+y)&1?255:0);
        }
    }
}
int main(int argc,char **argv)
{
    X4VideoNv12Layout l=layout(1280);uint8_t *source=malloc(l.total_nv12_bytes);assert(source);
    synthetic(source,l.total_nv12_bytes);
    for(unsigned matrix=0;matrix<4;++matrix)compare(source,&l,(matrix&1)?1:6,(matrix&2)!=0,"synthetic_edges_random",1920);
    assert((argc-1)%3==0);
    for(int i=1;i<argc;i+=3){
        FILE *f=fopen(argv[i],"rb");assert(f);assert(fread(source,1,l.total_nv12_bytes,f)==l.total_nv12_bytes);
        assert(fgetc(f)==EOF);assert(!fclose(f));
        const char *name=strrchr(argv[i],'/');name=name?name+1:argv[i];
        compare(source,&l,(unsigned)strtoul(argv[i+1],NULL,10),strtoul(argv[i+2],NULL,10)!=0,name,1920);
    }
    X4VideoNv12Layout padded=layout(1344);uint8_t *pad=calloc(1,padded.total_nv12_bytes);assert(pad);
    for(unsigned y=0;y<720;++y)memcpy(pad+(size_t)y*1344,source+(size_t)y*1280,1280);
    for(unsigned y=0;y<360;++y)memcpy(pad+padded.uv_plane_offset+(size_t)y*1344,source+l.uv_plane_offset+(size_t)y*1280,1280);
    compare(pad,&padded,1,false,"pitch_1344_destination_1936",1936);
    printf("PASS actual LLVM 720p shader arithmetic against CPU: cases=%llu pixels=%llu maximum=%u outside=%llu\n",
        (unsigned long long)cases,(unsigned long long)pixels,maximum,(unsigned long long)outside);
    free(pad);free(source);return 0;
}
