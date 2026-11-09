/* SPDX-License-Identifier: GPL-3.0-only */
/* Real new LLVM copy entry + unchanged row references on x86. This is not
 * native GPU execution, PM4 validation or a native latency measurement. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/video/gpu_quality_ref.h"
typedef uint32_t V4 __attribute__((ext_vector_type(4)));
extern void x4_gpu_verify_rows_host(V4,V4,V4,V4,uint32_t,uint32_t,uint32_t,uint32_t);
static uint8_t *write_base;static uint64_t cases,stores,plans,compared_pixels;
static uintptr_t raw_address(V4 d){return d[0]|((uint64_t)d[1]<<32);}
uint32_t x4_verify_raw_load(V4 d,uint32_t offset,uint32_t scalar,uint32_t flags)
{
    assert(!scalar&&!flags&&!(offset&3u)&&offset<=d[2]&&4<=d[2]-offset);
    uint32_t value;memcpy(&value,(const void *)(raw_address(d)+offset),4);return value;
}
void x4_verify_raw_store(uint32_t value,V4 d,uint32_t offset,uint32_t scalar,uint32_t flags)
{
    assert(!scalar&&!flags&&!(offset&3u)&&offset<=d[2]&&4<=d[2]-offset);
    assert(raw_address(d)==(uintptr_t)write_base);++stores;
    memcpy(write_base+offset,&value,4);
}
static V4 raw_descriptor(const void *p,size_t bytes)
{uintptr_t a=(uintptr_t)p;assert(bytes<=UINT32_MAX);return (V4){(uint32_t)a,(uint32_t)(a>>32),(uint32_t)bytes,0};}
static X4VideoNv12Layout layout(bool hd)
{
    unsigned w=hd?1920:1280,h=hd?1080:720,storage=hd?1088:720,pitch=hd?2048:1408;
    size_t y=(size_t)pitch*storage;
    return (X4VideoNv12Layout){.visible_width=w,.visible_height=h,.coded_width=w,.coded_height=storage,
        .storage_height=storage,.pitch=pitch,.y_bytes=y,.uv_bytes=y/2,.uv_plane_offset=y,.visible_uv_offset=y,
        .total_nv12_bytes=y+y/2,.rgb_pixels=(size_t)w*h,.rgb_bytes=(size_t)w*h*4};
}
static unsigned clamp_row(int row,unsigned last){return row<0?0:(unsigned)row>last?last:(unsigned)row;}
static void assert_band(const X4GpuVerifySnapshot *s,unsigned y,unsigned uv)
{
    assert(y>=s->first_y&&y<s->first_y+s->rows_y);
    assert(uv>=s->first_uv&&uv<s->first_uv+s->rows_uv);
}
static void exhaustive_bounds(void)
{
    X4VideoPipelineColor color={.matrix_coefficients=1,.convert_mode=X4_CONVERT_SMOOTH,.sharpness=2};
    for(unsigned hd=0;hd<2;++hd){X4VideoNv12Layout l=layout(hd);
        for(unsigned first=0;first<1080;++first)for(unsigned n=1;n<=16&&n<=1080-first;++n){
            X4GpuVerifySnapshot s;assert(!x4_gpu_quality_verify_plan(&l,&color,0,first,n,&s));++plans;
            assert(s.layout.pitch==l.visible_width&&s.source_bytes<=X4_GPU_VERIFY_NV12_CAPACITY);
            assert(s.first_y+s.rows_y<=l.visible_height&&s.first_uv+s.rows_uv<=l.visible_height/2);
            for(unsigned row=first;row<first+n;++row){
                if(hd){
                    /* CAS neighbours ±1, luma deband ±8; UV neighbour of
                     * vertical reconstruction ±1 then deband ±4. */
                    for(int delta=-9;delta<=9;++delta)
                        assert_band(&s,clamp_row((int)row+delta,1079),clamp_row((int)(row/2),539));
                    for(int delta=-6;delta<=6;++delta)
                        assert_band(&s,row,clamp_row((int)(row/2)+delta,539));
                }else{
                    /* Smooth 2→3 interpolation computes all three source
                     * rows in the group even when only one output is kept. */
                    unsigned group=row/3;
                    for(unsigned k=0;k<3;++k){unsigned sy=2*group+k;if(sy>719)sy=719;
                        for(int dy=-1;dy<=1;++dy)for(int duv=-1;duv<=1;++duv)
                            assert_band(&s,clamp_row((int)sy+dy,719),clamp_row((int)(sy/2)+duv,359));
                    }
                }
            }
        }
    }
}
static void make_reference(const X4VideoNv12Layout *l,const uint8_t *source,uint32_t *rgb,
    unsigned first,unsigned rows,const X4VideoPipelineColor *color,uint32_t word,void *scratch)
{
    if(x4_gpu_quality_active(word,color->sharpness))
        assert(!x4_gpu_quality_reference_rows(l,source,l->total_nv12_bytes,rgb+(size_t)first*1920,
            (size_t)rows*1920*4,first,rows,color,word,scratch,X4_GPU_QUALITY_REFERENCE_SCRATCH_BYTES));
    else if(l->visible_width==1280)
        assert(!x4_video_pipeline_gpu_reference_720_rows(l,source,l->total_nv12_bytes,rgb,1920u*1080u*4u,first,rows,color));
    else assert(!x4_video_pipeline_gpu_reference_rows(l,source,l->total_nv12_bytes,rgb+(size_t)first*1920,
        (size_t)rows*1920*4,first,rows,color));
}
static void one(const X4VideoNv12Layout *l,const uint8_t *source,uint32_t *rgb,uint32_t *padded,
    uint8_t *snapshot,void *scratch,void *reference_scratch,unsigned first,unsigned rows,
    X4VideoPipelineColor color,uint32_t word)
{
    X4GpuVerifySnapshot s;assert(!x4_gpu_quality_verify_plan(l,&color,word,first,rows,&s));
    s.token=++cases;s.owner_epoch=19;s.source=snapshot;
    s.actual=(const uint32_t *)(snapshot+X4_GPU_VERIFY_NV12_CAPACITY);
    make_reference(l,source,rgb,first,rows,&color,word,reference_scratch);
    for(unsigned y=first;y<first+rows;++y)memcpy(padded+(size_t)y*2048,rgb+(size_t)y*1920,1920*4);
    uint32_t p[16]={l->pitch,s.layout.pitch,(uint32_t)l->uv_plane_offset,(uint32_t)s.layout.uv_plane_offset,
        X4_GPU_VERIFY_NV12_CAPACITY,2048,s.first_y,s.rows_y,s.first_uv,s.rows_uv,first,rows,0,0,0,0};
    unsigned height=s.rows_y;if(height<s.rows_uv)height=s.rows_uv;if(height<rows)height=rows;
    V4 native=raw_descriptor(source,l->total_nv12_bytes),scanout=raw_descriptor(padded,2048u*1080*4);
    V4 owned=raw_descriptor(snapshot,X4_GPU_VERIFY_NATIVE_BYTES),params=raw_descriptor(p,sizeof(p));
    for(unsigned poison=0;poison<2;++poison){
        memset(snapshot,poison?0xe1:0x19,X4_GPU_VERIFY_NATIVE_BYTES+64);write_base=snapshot;
        uint64_t before=stores;
        for(unsigned gy=0;gy<(height+7)/8;++gy)for(unsigned gx=0;gx<240;++gx)
            for(unsigned ty=0;ty<8;++ty)for(unsigned tx=0;tx<8;++tx)
                x4_gpu_verify_rows_host(native,scanout,owned,params,gx,gy,tx,ty);
        assert(stores-before==(uint64_t)(s.rows_y+s.rows_uv)*s.layout.pitch/4+(uint64_t)rows*1920);
        for(unsigned row=0;row<s.layout.storage_height;++row){
            const uint8_t *copy=snapshot+(size_t)row*s.layout.pitch;
            if(row>=s.first_y&&row<s.first_y+s.rows_y)assert(!memcmp(copy,source+(size_t)row*l->pitch,s.layout.pitch));
            else for(unsigned x=0;x<s.layout.pitch;++x)assert(copy[x]==(poison?0xe1:0x19));
        }
        for(unsigned row=0;row<s.layout.storage_height/2;++row){
            const uint8_t *copy=snapshot+s.layout.uv_plane_offset+(size_t)row*s.layout.pitch;
            if(row>=s.first_uv&&row<s.first_uv+s.rows_uv)assert(!memcmp(copy,source+l->uv_plane_offset+(size_t)row*l->pitch,s.layout.pitch));
            else for(unsigned x=0;x<s.layout.pitch;++x)assert(copy[x]==(poison?0xe1:0x19));
        }
        for(unsigned i=0;i<64;++i)assert(snapshot[X4_GPU_VERIFY_NATIVE_BYTES+i]==(poison?0xe1:0x19));
        X4GpuVerifyResult result;assert(!x4_gpu_quality_verify_snapshot(&s,&result,scratch,X4_GPU_VERIFY_CPU_BYTES));
        assert(result.token==s.token&&result.owner_epoch==s.owner_epoch&&result.checked_pixels==rows*1920);
        if(result.pixels_outside){fprintf(stderr,"poison mismatch hd=%u first=%u rows=%u mode=%u sharp=%u word=%u max=%u outside=%u\n",
            l->visible_width,first,rows,color.convert_mode,color.sharpness,word,result.max_channel_difference,result.pixels_outside);abort();}
        assert(!result.max_channel_difference);compared_pixels+=result.checked_pixels;
    }
    /* Alpha is always 255. Complementing an arbitrary RGB value could make
     * 127 -> 128, which is deliberately inside the ±1 acceptance tolerance. */
    ((uint32_t *)(snapshot+X4_GPU_VERIFY_NV12_CAPACITY))[0]^=UINT32_C(0xff000000);
    X4GpuVerifyResult bad;assert(!x4_gpu_quality_verify_snapshot(&s,&bad,scratch,X4_GPU_VERIFY_CPU_BYTES));
    assert(bad.pixels_outside==1&&bad.max_channel_difference>1);
}
int main(void)
{
    exhaustive_bounds();
    uint8_t *source=malloc(2048u*1088u*3u/2u),*snapshot=malloc(X4_GPU_VERIFY_NATIVE_BYTES+64);
    uint32_t *rgb=malloc(1920u*1080u*4u),*padded=malloc(2048u*1080u*4u);
    void *scratch=malloc(X4_GPU_VERIFY_CPU_BYTES),*ref=malloc(X4_GPU_QUALITY_REFERENCE_SCRATCH_BYTES);
    assert(source&&snapshot&&rgb&&padded&&scratch&&ref);
    static const unsigned firsts[]={0,1,2,3,7,15,16,17,31,32,63,64,123,255,511,512,719,720,1001,1063,1064,1079};
#if X4_PUBLIC
    static const unsigned words[]={0,1};
    for(unsigned i=0;i<256;++i)assert(x4_gpu_quality_normalize(i)==(i&1u));
#else
    static const unsigned words[]={0,1,2,3,4,5,8,9,10,11,12,13};
#endif
    for(unsigned hd=0;hd<2;++hd){X4VideoNv12Layout l=layout(hd);
        for(size_t i=0;i<l.total_nv12_bytes;++i)source[i]=(uint8_t)((i*67u+(i>>7)*19u+(i>>13)*29u)%256);
        for(unsigned w=0;w<(hd?sizeof(words)/sizeof(words[0]):1u);++w)
            for(unsigned mode=0;mode<4;++mode){
                if(words[w]&&!mode)continue;
                X4VideoPipelineColor color={.matrix_coefficients=(mode&1)?1:6,.full_range=(mode&2)!=0,
                    .convert_mode=mode?X4_CONVERT_SMOOTH:X4_CONVERT_ORIGINAL,.sharpness=mode?mode-1:0};
                for(unsigned f=0;f<sizeof(firsts)/sizeof(firsts[0]);++f){unsigned first=firsts[f],rows=1080-first;
                    if(rows>16)rows=16;one(&l,source,rgb,padded,snapshot,scratch,ref,first,rows,color,words[w]);
                }
            }
    }
    free(source);free(snapshot);free(rgb);free(padded);free(scratch);free(ref);
    printf("PASS sparse verification actual LLVM copy: plans=%llu cases=%llu compared_pixels=%llu stores=%llu, two poison patterns, 720/1080 padded source/scanout, edges+all old quality filters; no native performance claim\n",
        (unsigned long long)plans,(unsigned long long)cases,(unsigned long long)compared_pixels,(unsigned long long)stores);
    return 0;
}
