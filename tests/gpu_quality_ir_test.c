/* SPDX-License-Identifier: GPL-3.0-only */
/* Real delivered LLVM arithmetic on the host raw-buffer ABI, compared with
 * the independent compact-row CPU reference. No GPU or PS4 is executed. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/video/gpu_quality_ref.h"
typedef uint32_t V4 __attribute__((ext_vector_type(4)));
typedef void (*Kernel)(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
extern void x4_gpu_test_quality_direct(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
extern void x4_gpu_test_quality_prepare(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
extern void x4_gpu_test_quality_finish(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
static unsigned maximum;
static uint64_t outside,pixels,cases,stores,changed;
static uintptr_t expected_store_base;
static uint32_t expected_store_bytes;
static uintptr_t raw_address(V4 v){return v[0]|((uint64_t)v[1]<<32);}
uint32_t x4_gpu_test_raw_load(V4 d,uint32_t offset,uint32_t scalar,uint32_t flags)
{
    assert(!scalar && !flags && !(offset&3u) && offset<=d[2] && 4<=d[2]-offset);
    uint32_t value;memcpy(&value,(const void *)(raw_address(d)+offset),4);return value;
}
void x4_gpu_test_raw_store(uint32_t value,V4 d,uint32_t offset,uint32_t scalar,uint32_t flags)
{
    assert(!scalar && !flags && !(offset&3u) && offset<=d[2] && 4<=d[2]-offset);
    assert(raw_address(d)==expected_store_base && d[2]==expected_store_bytes);
    ++stores;memcpy((void *)(raw_address(d)+offset),&value,4);
}
static V4 descriptor(const void *p,size_t bytes)
{
    uintptr_t address=(uintptr_t)p;assert(bytes<=UINT32_MAX);
    return (V4){(uint32_t)address,(uint32_t)(address>>32),(uint32_t)bytes,0};
}
static X4VideoNv12Layout layout(unsigned pitch)
{
    size_t y=(size_t)pitch*1088,uv=y/2;
    return (X4VideoNv12Layout){.visible_width=1920,.visible_height=1080,
        .coded_width=1920,.coded_height=1088,.storage_height=1088,.pitch=pitch,
        .y_bytes=y,.uv_bytes=uv,.uv_plane_offset=y,.total_nv12_bytes=y+uv,
        .visible_uv_offset=y,.rgb_pixels=2073600,.rgb_bytes=8294400};
}
static void dispatch(Kernel kernel,V4 a,V4 b,V4 out,V4 params)
{
    expected_store_base=raw_address(out);expected_store_bytes=out[2];
    for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1920;++x)
        kernel(a,b,out,params,x>>3,y>>3,x&7,y&7);
    uint64_t before=stores;
    kernel(a,b,out,params,240,0,0,0);kernel(a,b,out,params,0,135,0,0);
    kernel(a,b,out,params,UINT32_MAX,UINT32_MAX,7,7);
    assert(stores==before);
}
static void guards(const void *buffer,size_t bytes)
{for(size_t i=bytes;i<bytes+64;++i)assert(((const uint8_t *)buffer)[i]==0xa6);}
static void compare(const uint8_t *source,const X4VideoNv12Layout *l,
    unsigned matrix,bool full,const char *name,unsigned dpitch)
{
    size_t output_bytes=(size_t)dpitch*1080*4;
    uint32_t *actual=malloc(output_bytes+64),*reference=malloc(1920u*16*4+64);
    float *intermediate=malloc(X4_GPU_QUALITY_INTERMEDIATE_BYTES+64);
    uint8_t *parameter_data=calloc(1,X4_GPU_QUALITY_DATA_BYTES);
    size_t scratch_bytes=x4_gpu_quality_reference_scratch_bytes(16);
    void *scratch=malloc(scratch_bytes);assert(actual&&reference&&intermediate&&parameter_data&&scratch);
    memcpy(parameter_data+64,x4_gpu_quality_noise_ranks(),X4_GPU_QUALITY_NOISE_BYTES);
    V4 yd=descriptor(source,l->y_bytes),uvd=descriptor(source+l->uv_plane_offset,l->uv_bytes);
    V4 out=descriptor(actual,output_bytes),pd=descriptor(parameter_data,X4_GPU_QUALITY_DATA_BYTES);
    V4 rgb=descriptor(intermediate,X4_GPU_QUALITY_INTERMEDIATE_BYTES);
    V4 noise=descriptor(parameter_data+64,X4_GPU_QUALITY_NOISE_BYTES);
    for(unsigned dither=0;dither<2;++dither)for(unsigned deband=0;deband<3;++deband)
    for(unsigned cas=0;cas<2;++cas)for(unsigned sharpness=0;sharpness<3;++sharpness){
        uint32_t word=dither|(deband<<1)|(cas?X4_GPU_QUALITY_CAS:0u);
        bool two_pass=cas && sharpness!=0;
        X4VideoPipelineColor color={matrix,full,X4_CONVERT_SMOOTH,sharpness};
        x4_gpu_quality_parameters((X4GpuQualityParameters *)parameter_data,l->pitch,dpitch,1920,&color,word);
        memset(actual,0xa6,output_bytes+64);
        memset(intermediate,0xa6,X4_GPU_QUALITY_INTERMEDIATE_BYTES+64);
        if(two_pass){
            dispatch(x4_gpu_test_quality_prepare,yd,uvd,rgb,pd);
            guards(intermediate,X4_GPU_QUALITY_INTERMEDIATE_BYTES);
            for(size_t i=3;i<2073600u*4;i+=4)assert(intermediate[i]==0.0f);
            dispatch(x4_gpu_test_quality_finish,rgb,noise,out,pd);
        }else dispatch(x4_gpu_test_quality_direct,yd,uvd,out,pd);
        unsigned local_max=0;uint64_t local_outside=0,local_changed=0;
        for(unsigned first=0;first<1080;first+=16){
            unsigned rows=1080-first<16?1080-first:16;
            memset(reference,0xa6,1920u*16*4+64);
            assert(!x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,reference,
                1920u*rows*4,first,rows,&color,word,scratch,scratch_bytes));
            for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<1920;++x){
                uint32_t a=actual[(size_t)(first+y)*dpitch+x],b=reference[(size_t)y*1920+x];
                bool bad=false;if(a!=b)++local_changed;
                for(unsigned channel=0;channel<4;++channel){
                    int delta=(int)((a>>(channel*8))&255)-(int)((b>>(channel*8))&255);
                    unsigned difference=(unsigned)(delta<0?-delta:delta);
                    if(difference>local_max)local_max=difference;if(difference>1)bad=true;
                }
                if(bad)++local_outside;
            }
            for(size_t i=(size_t)rows*1920*4;i<1920u*16*4+64;++i)assert(((uint8_t *)reference)[i]==0xa6);
        }
        guards(actual,output_bytes);
        if(dpitch>1920)for(unsigned y=0;y<1080;++y)for(unsigned x=1920;x<dpitch;++x)
            assert(actual[(size_t)y*dpitch+x]==UINT32_C(0xa6a6a6a6));
        if(dither==1 && deband==1 && cas==1 && sharpness==2){
            uint32_t *before=malloc(output_bytes);assert(before);memcpy(before,actual,output_bytes);
            ((X4GpuQualityParameters *)parameter_data)->quality_word|=X4_GPU_QUALITY_REPEAT;
            dispatch(x4_gpu_test_quality_prepare,yd,uvd,rgb,pd);
            dispatch(x4_gpu_test_quality_finish,rgb,noise,out,pd);
            assert(!memcmp(before,actual,output_bytes));free(before);
        }
        printf("{\"fixture\":\"%s\",\"word\":%u,\"sharpness\":%u,\"matrix\":%u,\"full\":%u,\"pixels\":2073600,\"maximum\":%u,\"different_pixels\":%llu,\"outside\":%llu}\n",
            name,word,sharpness,matrix,full?1u:0u,local_max,(unsigned long long)local_changed,(unsigned long long)local_outside);
        fflush(stdout);if(local_max>maximum)maximum=local_max;
        outside+=local_outside;changed+=local_changed;pixels+=2073600;++cases;
        assert(local_max<=1 && !local_outside);
    }
    free(scratch);free(parameter_data);free(intermediate);free(reference);free(actual);
}
static void synthetic(uint8_t *source,const X4VideoNv12Layout *l)
{
    memset(source,0xbd,l->total_nv12_bytes);
    uint32_t state=UINT32_C(0xa8db5291);
    for(unsigned y=0;y<1080;++y)for(unsigned x=0;x<1920;++x){
        state^=state<<13;state^=state>>17;state^=state<<5;
        unsigned level=y<360?16u+(x>>6):y<720?125u+(state%7):((x/17+y/19)&1)?235u:16u;
        if(x==0 || x==1919 || y==0 || y==1079)level=((x+y)&1)?0u:255u;
        if(y==1 || y==1078)level=(x%12==0)?0u:(x%12==1)?255u:x%256;
        source[(size_t)y*l->pitch+x]=(uint8_t)level;
    }
    for(unsigned y=0;y<540;++y)for(unsigned x=0;x<960;++x){
        size_t at=l->uv_plane_offset+(size_t)y*l->pitch+2*x;
        source[at]=(uint8_t)(y<180?128u:y<360?125u+(x+y)%7:((x/11)&1)?0u:255u);
        source[at+1]=(uint8_t)(y<180?128u:y<360?125u+(3*x+y)%7:((y/13)&1)?255u:0u);
    }
}
static void reference_contract(const uint8_t *source,const X4VideoNv12Layout *l)
{
    size_t bytes=x4_gpu_quality_reference_scratch_bytes(16);uint8_t *scratch=malloc(bytes+1);
    uint32_t *destination=malloc(1920u*16*4+64);assert(scratch&&destination);
    memset(destination,0xa6,1920u*16*4+64);
    X4VideoPipelineColor color={1,false,X4_CONVERT_SMOOTH,2};
    assert(!x4_gpu_quality_reference_scratch_bytes(0) && !x4_gpu_quality_reference_scratch_bytes(17));
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4,0,0,&color,1,scratch,bytes)!=0);
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4,0,17,&color,1,scratch,bytes)!=0);
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4,1079,2,&color,1,scratch,bytes)!=0);
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4,1080,1,&color,1,scratch,bytes)!=0);
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes-1,destination,1920u*16*4,0,16,&color,1,scratch,bytes)!=0);
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4-1,0,16,&color,1,scratch,bytes)!=0);
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4,0,16,&color,1,scratch,bytes-1)!=0);
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4,0,16,&color,1,scratch+1,bytes)!=0);
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4,0,16,&color,32,scratch,bytes)!=0);
    assert(x4_gpu_quality_reference_rows(NULL,source,l->total_nv12_bytes,destination,1920u*16*4,0,16,&color,1,scratch,bytes)!=0);
    color.convert_mode=X4_CONVERT_ORIGINAL;
    assert(x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,destination,1920u*16*4,0,16,&color,1,scratch,bytes)!=0);
    for(size_t i=0;i<1920u*16*4+64;++i)assert(((uint8_t *)destination)[i]==0xa6);
    free(destination);free(scratch);
}
int main(int argc,char **argv)
{
    X4VideoNv12Layout l=layout(1920);uint8_t *source=malloc(l.total_nv12_bytes);assert(source);
    synthetic(source,&l);
    reference_contract(source,&l);
    for(unsigned combination=0;combination<4;++combination)
        compare(source,&l,(combination&1)?1u:6u,(combination&2)!=0,"synthetic_dark_flat_noise_extreme_edges",1920);
    assert((argc-1)%3==0 && argc>=10);
    for(int i=1;i<argc;i+=3){
        FILE *file=fopen(argv[i],"rb");assert(file);
        assert(fread(source,1,l.total_nv12_bytes,file)==l.total_nv12_bytes && fgetc(file)==EOF && !fclose(file));
        const char *name=strrchr(argv[i],'/');name=name?name+1:argv[i];
        compare(source,&l,(unsigned)strtoul(argv[i+1],NULL,10),strtoul(argv[i+2],NULL,10)!=0,name,1920);
    }
    X4VideoNv12Layout padded=layout(2048);uint8_t *pad=malloc(padded.total_nv12_bytes);assert(pad);
    synthetic(pad,&padded);compare(pad,&padded,1,false,"source_pitch_2048_destination_pitch_1936",1936);
    printf("PASS actual LLVM quality arithmetic against compact CPU reference: cases=%llu pixels=%llu maximum=%u different=%llu outside=%llu\n",
        (unsigned long long)cases,(unsigned long long)pixels,maximum,(unsigned long long)changed,(unsigned long long)outside);
    free(pad);free(source);return 0;
}
