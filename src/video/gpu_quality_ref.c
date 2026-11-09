/* SPDX-License-Identifier: GPL-3.0-only */
/* CAS no-scaling adaptation: AMD FidelityFX CAS, MIT, commit
 * 9fabcc9a2c45f958aff55ddfda337e74ef894b7f. Copyright (c) 2017-2019
 * Advanced Micro Devices, Inc.; Copyright (c) <2014> <Michal Drobot>.
 * Full permission notice: docs/licenses/AMD_FidelityFX_CAS.txt.
 * Deband and the deterministic blue-noise generator are original code.
 */
#include "gpu_quality_ref.h"
#include "gpu/gpu_quality_noise.h"
#include <string.h>
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#if !X4_PUBLIC
unsigned x4_gpu_quality_settings_get(void);
bool x4_gpu_quality_settings_set(unsigned word);
#endif

uint32_t x4_gpu_quality_normalize(uint32_t word)
{
#if X4_PUBLIC
    return word&X4_GPU_QUALITY_DITHER;
#else
    word&=X4_GPU_QUALITY_MASK;
    if((word&X4_GPU_QUALITY_DEBAND_MASK)==X4_GPU_QUALITY_DEBAND_MASK)
        word&=~X4_GPU_QUALITY_DEBAND_MASK;
    return word;
#endif
}
bool x4_gpu_quality_active(uint32_t word,unsigned sharpness)
{
#if X4_PUBLIC
    (void)sharpness;
    return (word&X4_GPU_QUALITY_DITHER)!=0;
#else
    word=x4_gpu_quality_normalize(word);
    return (word&(X4_GPU_QUALITY_DITHER|X4_GPU_QUALITY_DEBAND_MASK))!=0 ||
        ((word&X4_GPU_QUALITY_CAS)!=0 && sharpness!=0);
#endif
}
size_t x4_gpu_quality_reference_scratch_bytes(unsigned rows)
{
    return rows && rows<=16?(size_t)(rows+2)*1920*4*sizeof(float):0;
}
const uint16_t *x4_gpu_quality_noise_ranks(void)
{return x4_gpu_quality_noise;}
void x4_gpu_quality_parameters(X4GpuQualityParameters *p,unsigned pitch,
    unsigned dpitch,unsigned ipitch,const X4VideoPipelineColor *c,uint32_t word)
{
    static const int32_t coefficients[4][6]={
        {9539,13075,-3209,-6660,16525,16},
        {9539,14686,-1747,-4366,17305,16},
        {8192,11485,-2819,-5850,14516,0},
        {8192,12901,-1535,-3835,15201,0}};
    unsigned matrix=(c->full_range?2u:0u)+(c->matrix_coefficients==1?1u:0u);
    memset(p,0,sizeof(*p));p->source_pitch=pitch;p->destination_pitch=dpitch;
    p->y=coefficients[matrix][0];p->rv=coefficients[matrix][1];
    p->gu=coefficients[matrix][2];p->gv=coefficients[matrix][3];
    p->bu=coefficients[matrix][4];p->y_bias=coefficients[matrix][5];
    p->quality_word=x4_gpu_quality_normalize(word);p->sharpness=c->sharpness;
    p->intermediate_pitch=ipitch;
}
static int clampi(int x,int high){return x<0?0:x>high?high:x;}
static float clampf(float x,float high){return x<0.0f?0.0f:x>high?high:x;}
#if !X4_PUBLIC
static uint32_t float_bits(float value){uint32_t bits;memcpy(&bits,&value,4);return bits;}
static float from_bits(uint32_t bits){float value;memcpy(&value,&bits,4);return value;}
/* Deliberately ordered binary32 operations, no hardware approximate special
 * instruction. Integer seeds originate from AMD ffx_a.h:1454-1457. Three
 * Newton refinements are explicit and identical in the generated GPU IR.
 * Inputs here are finite normal values, or zero handled before the seed.
 */
static float reciprocal(float x)
{
    float r=from_bits(UINT32_C(0x7ef07ebb)-float_bits(x));
    for(unsigned i=0;i<3;++i){float a=x*r;float b=2.0f-a;r=r*b;}
    return r;
}
static float square_root(float x)
{
    if(x<=0.0f)return 0.0f;
    float r=from_bits((float_bits(x)>>1)+UINT32_C(0x1fbc4639));
    for(unsigned i=0;i<3;++i){float a=x*reciprocal(r);float b=r+a;r=0.5f*b;}
    return r;
}
static uint32_t spatial_hash(unsigned x,unsigned y,unsigned plane)
{
    uint32_t h=x*UINT32_C(0x9e3779b9)^y*UINT32_C(0x85ebca6b)^
        plane*UINT32_C(0xc2b2ae35)^UINT32_C(0x58433425);
    h^=h>>16;h*=UINT32_C(0x7feb352d);h^=h>>15;
    h*=UINT32_C(0x846ca68b);return h^(h>>16);
}
static void direction(unsigned hash,int radius,int *dx,int *dy)
{
    unsigned a=hash&3u;
    *dx=a==0?radius:a==1?(radius*7+4)/8:a==2?(radius*3+2)/4:(radius*3+4)/8;
    *dy=a==0?0:a==1?(radius*3+4)/8:a==2?(radius*3+2)/4:(radius*7+4)/8;
}
#endif
static float plane_sample(const uint8_t *base,unsigned pitch,int x,int y,
    unsigned component,bool chroma)
{
    x=clampi(x,chroma?959:1919);y=clampi(y,chroma?539:1079);
    return (float)base[(size_t)y*pitch+(unsigned)x*(chroma?2u:1u)+component];
}
#if !X4_PUBLIC
static float deband_sample(const uint8_t *base,unsigned pitch,int x,int y,
    unsigned component,bool chroma,unsigned strength)
{
    float center=plane_sample(base,pitch,x,y,component,chroma);
    if(!strength)return center;
    int radius=chroma?(strength==1?2:4):(strength==1?4:8),dx,dy;
    direction(spatial_hash((unsigned)x,(unsigned)y,chroma?1u:0u),radius,&dx,&dy);
    float a=plane_sample(base,pitch,x+dx,y+dy,component,chroma);
    float b=plane_sample(base,pitch,x-dy,y+dx,component,chroma);
    float c=plane_sample(base,pitch,x-dx,y-dy,component,chroma);
    float d=plane_sample(base,pitch,x+dy,y-dx,component,chroma);
    float threshold=chroma?(strength==1?1.5f:3.0f):(strength==1?2.0f:4.0f);
    float da=a-center,db=b-center,dc=c-center,dd=d-center;
    if(da<0)da=-da;if(db<0)db=-db;if(dc<0)dc=-dc;if(dd<0)dd=-dd;
    if(da>=threshold || db>=threshold || dc>=threshold || dd>=threshold)return center;
    float sum=a+b;sum=sum+c;sum=sum+d;
    float average=sum*0.25f;
    float mix=strength==1?0.5f:0.75f;
    return center+(average-center)*mix;
}
#else
/* Public candidates have no deband implementation or selector. */
#define deband_sample(base,pitch,x,y,component,chroma,strength) \
    plane_sample((base),(pitch),(x),(y),(component),(chroma))
#endif
static float luma_sample(const uint8_t *y,const X4GpuQualityParameters *p,int x,int row)
{
    x=clampi(x,1919);row=clampi(row,1079);
    return deband_sample(y,p->source_pitch,x,row,0,false,(p->quality_word>>1)&3u);
}
static float vertical_chroma(const uint8_t *uv,const X4GpuQualityParameters *p,
    int column,int row,int other,unsigned component)
{
    unsigned strength=(p->quality_word>>1)&3u;
#if X4_PUBLIC
    strength=0;
#endif
    float a=deband_sample(uv,p->source_pitch,column,row,component,true,strength);
    float b=deband_sample(uv,p->source_pitch,column,other,component,true,strength);
    float sum=3.0f*a+b;
    return strength?sum*0.25f:(float)(((unsigned)sum+2u)>>2);
}
void x4_gpu_quality_prepare_pixel(const uint8_t *y,const uint8_t *uv,
    const X4GpuQualityParameters *p,unsigned x,unsigned row,float rgb[3])
{
    bool cas=false;
#if !X4_PUBLIC
    cas=(p->quality_word&X4_GPU_QUALITY_CAS)!=0 && p->sharpness!=0;
#endif
    float luma=luma_sample(y,p,(int)x,(int)row);
    if(!cas && p->sharpness){
        float left=luma_sample(y,p,(int)x-1,(int)row);
        float right=luma_sample(y,p,(int)x+1,(int)row);
        float up=luma_sample(y,p,(int)x,(int)row-1);
        float down=luma_sample(y,p,(int)x,(int)row+1);
        float delta=4.0f*luma-left;delta=delta-right;delta=delta-up;delta=delta-down;
        float adjustment=delta*(p->sharpness==1?0.0625f:0.125f)+0.5f;
        int whole=(int)adjustment;if((float)whole>adjustment)--whole;
        luma=clampf(luma+(float)whole,255.0f);
    }
    int j=(int)row>>1;
    int other=clampi(j+((row&1u)?1:-1),539);
    int i=(int)x>>1,inext=clampi(i+1,959);
    float u0=vertical_chroma(uv,p,i,j,other,0),v0=vertical_chroma(uv,p,i,j,other,1);
    float u=u0,v=v0;
    if(x&1u){
        float u1=vertical_chroma(uv,p,inext,j,other,0),v1=vertical_chroma(uv,p,inext,j,other,1);
        if(p->quality_word&X4_GPU_QUALITY_DEBAND_MASK){u=(u0+u1)*0.5f;v=(v0+v1)*0.5f;}
        else {u=(float)(((unsigned)u0+(unsigned)u1+1u)>>1);v=(float)(((unsigned)v0+(unsigned)v1+1u)>>1);}
    }
    float yc=(luma-(float)p->y_bias)*(float)p->y;
    float d=u-128.0f,e=v-128.0f;
    float r=yc+e*(float)p->rv;
    float g=yc+d*(float)p->gu;g=g+e*(float)p->gv;
    float b=yc+d*(float)p->bu;
    rgb[0]=clampf(r*0.0001220703125f,255.0f);
    rgb[1]=clampf(g*0.0001220703125f,255.0f);
    rgb[2]=clampf(b*0.0001220703125f,255.0f);
#if !X4_PUBLIC
    if(cas)for(unsigned k=0;k<3;++k){float n=rgb[k]*0.0039215688593685626983642578125f;rgb[k]=n*n;}
#endif
}
#if !X4_PUBLIC
static const float *intermediate_at(const float *rgb,unsigned pitch,unsigned first,
    unsigned count,int x,int row)
{
    x=clampi(x,1919);row=clampi(row,1079);
    unsigned relative=(unsigned)row>=first?(unsigned)row-first:0;
    if(relative>=count)relative=count-1;
    return rgb+((size_t)relative*pitch+(unsigned)x)*4;
}
#endif
uint32_t x4_gpu_quality_finish_pixel(const float *rgb,unsigned pitch,
    unsigned first,unsigned count,unsigned x,unsigned row,const X4GpuQualityParameters *p)
{
#if !X4_PUBLIC
    const float *e=intermediate_at(rgb,pitch,first,count,(int)x,(int)row);
#else
    unsigned relative=row>=first?row-first:0;if(relative>=count)relative=count-1;
    const float *e=rgb+((size_t)relative*pitch+x)*4;
#endif
    float result[3]={e[0],e[1],e[2]};
#if !X4_PUBLIC
    bool cas=(p->quality_word&X4_GPU_QUALITY_CAS)!=0 && p->sharpness!=0;
    if(cas){
        const float *b=intermediate_at(rgb,pitch,first,count,(int)x,(int)row-1);
        const float *d=intermediate_at(rgb,pitch,first,count,(int)x-1,(int)row);
        const float *f=intermediate_at(rgb,pitch,first,count,(int)x+1,(int)row);
        const float *h=intermediate_at(rgb,pitch,first,count,(int)x,(int)row+1);
        float mn=e[1],mx=e[1];
        const float green[4]={b[1],d[1],f[1],h[1]};
        for(unsigned k=0;k<4;++k){if(green[k]<mn)mn=green[k];if(green[k]>mx)mx=green[k];}
        float distance=1.0f-mx;if(mn<distance)distance=mn;
        float amp=mx>0.0f?clampf(distance*reciprocal(mx),1.0f):0.0f;
        float peak=p->sharpness==1?-0.14705882966518402099609375f:-0.1694915294647216796875f;
        float w=square_root(amp)*peak;
        float denom=1.0f+4.0f*w;
        float rcp=reciprocal(denom);
        for(unsigned k=0;k<3;++k){
            float sum=b[k]*w+d[k]*w;sum=sum+f[k]*w;sum=sum+h[k]*w;sum=sum+e[k];
            float value=clampf(sum*rcp,1.0f);
            result[k]=square_root(value)*255.0f;
        }
    }
#endif
    unsigned noise_index=((row&63u)<<6)|(x&63u);
    float threshold=(p->quality_word&X4_GPU_QUALITY_DITHER)?
        ((float)x4_gpu_quality_noise[noise_index]+0.5f)*0.000244140625f:0.5f;
    uint32_t r=(uint32_t)clampf(result[0]+threshold,255.0f);
    uint32_t g=(uint32_t)clampf(result[1]+threshold,255.0f);
    uint32_t b=(uint32_t)clampf(result[2]+threshold,255.0f);
    return UINT32_C(0xff000000)|r|(g<<8)|(b<<16);
}
int x4_gpu_quality_reference_rows(const X4VideoNv12Layout *l,const uint8_t *source,
    size_t source_bytes,uint32_t *dest,size_t dest_bytes,unsigned first,unsigned rows,
    const X4VideoPipelineColor *color,uint32_t word,void *scratch,size_t scratch_bytes)
{
    size_t need=x4_gpu_quality_reference_scratch_bytes(rows);
    if(!l || !source || !dest || !color || !scratch || !need ||
        ((uintptr_t)scratch%_Alignof(float)) || scratch_bytes<need ||
        first>=1080 || rows>1080-first || dest_bytes<(size_t)rows*1920*4 ||
        l->visible_width!=1920 || l->visible_height!=1080 || l->coded_width!=1920 ||
        l->coded_height!=1088 || l->storage_height!=1088 || l->pitch<1920 ||
        l->pitch>X4_VIDEO_MAX_PITCH || (l->pitch&3u) || l->visible_y_offset ||
        l->y_bytes!=(size_t)l->pitch*1088 || l->uv_bytes!=l->y_bytes/2 ||
        l->uv_plane_offset!=l->y_bytes || l->visible_uv_offset!=l->y_bytes ||
        l->total_nv12_bytes!=l->y_bytes+l->uv_bytes || source_bytes<l->total_nv12_bytes ||
        color->convert_mode!=X4_CONVERT_SMOOTH || color->sharpness>2 ||
        word!=x4_gpu_quality_normalize(word))return X4_PIPELINE_INVALID;
    X4GpuQualityParameters p;x4_gpu_quality_parameters(&p,l->pitch,1920,1920,color,word);
    bool cas=false;
#if !X4_PUBLIC
    cas=(word&X4_GPU_QUALITY_CAS)!=0 && color->sharpness!=0;
#endif
    unsigned start=cas && first?first-1:first;
    unsigned end=first+rows;if(cas && end<1080)++end;
    float *surface=scratch;
    for(unsigned row=start;row<end;++row)for(unsigned x=0;x<1920;++x){
        float *out=surface+((size_t)(row-start)*1920+x)*4;
        x4_gpu_quality_prepare_pixel(source,source+l->uv_plane_offset,&p,x,row,out);out[3]=0;
    }
    for(unsigned row=first;row<first+rows;++row)for(unsigned x=0;x<1920;++x)
        dest[(size_t)(row-first)*1920+x]=x4_gpu_quality_finish_pixel(surface,1920,start,end-start,x,row,&p);
    return X4_PIPELINE_OK;
}

size_t x4_gpu_quality_verify_scratch_bytes(void)
{return X4_GPU_VERIFY_CPU_BYTES;}

int x4_gpu_quality_verify_plan(const X4VideoNv12Layout *input,const X4VideoPipelineColor *color,
    uint32_t word,unsigned first,unsigned rows,X4GpuVerifySnapshot *s)
{
    if(!input||!color||!s||!rows||rows>X4_GPU_VERIFY_ROWS||first>=1080||rows>1080-first||
        word!=x4_gpu_quality_normalize(word))return X4_PIPELINE_INVALID;
    bool hd=input->visible_width==1920&&input->visible_height==1080;
    bool small=input->visible_width==1280&&input->visible_height==720;
    unsigned storage=hd?1088u:720u;
    if((!hd&&!small)||input->coded_width!=input->visible_width||input->coded_height!=storage||
        input->storage_height!=storage||input->pitch<input->visible_width||input->pitch>X4_VIDEO_MAX_PITCH||
        (input->pitch&3u)||input->visible_y_offset||input->y_bytes!=(size_t)input->pitch*storage||
        input->uv_bytes!=input->y_bytes/2||input->uv_plane_offset!=input->y_bytes||
        input->visible_uv_offset!=input->y_bytes||input->total_nv12_bytes!=input->y_bytes+input->uv_bytes)
        return X4_PIPELINE_INVALID;
    *s=(X4GpuVerifySnapshot){.layout=*input,.color=*color,.quality_word=word,.first_row=first,.rows=rows};
    X4VideoNv12Layout *l=&s->layout;
    l->pitch=l->visible_width;l->y_bytes=(size_t)l->pitch*l->storage_height;
    l->uv_bytes=l->y_bytes/2;l->uv_plane_offset=l->y_bytes;
    l->total_nv12_bytes=l->y_bytes+l->uv_bytes;l->visible_y_offset=0;l->visible_uv_offset=l->y_bytes;
    s->source_bytes=l->total_nv12_bytes;
    /* Largest Lab filter support: deband luma radius 8 plus one CAS/sharp
     * neighbour; chroma radius 4 plus vertical interpolation and CAS. The
     * 720 scaling groups can reach two source rows past simple mapping.
     * Conservative 16/8 halos cover all old arithmetic without changing it. */
    unsigned source_first=first*l->visible_height/1080u;
    unsigned source_end=((first+rows)*l->visible_height+1079u)/1080u;
    s->first_y=source_first>16?source_first-16:0;
    unsigned end_y=source_end+16;if(end_y>l->visible_height)end_y=l->visible_height;
    s->rows_y=end_y-s->first_y;
    unsigned uv_first=source_first/2,uv_end=(source_end+1)/2;
    s->first_uv=uv_first>8?uv_first-8:0;
    uv_end+=8;if(uv_end>l->visible_height/2)uv_end=l->visible_height/2;
    s->rows_uv=uv_end-s->first_uv;
    return 0;
}

int x4_gpu_quality_verify_snapshot(const X4GpuVerifySnapshot *s,X4GpuVerifyResult *r,
    void *scratch,size_t scratch_bytes)
{
    if(!r)return X4_PIPELINE_INVALID;
    memset(r,0,sizeof(*r));
    if(s){r->token=s->token;r->owner_epoch=s->owner_epoch;}
    r->result=X4_PIPELINE_INVALID;
    if(!s||!s->token||!s->owner_epoch||!s->source||!s->actual||!scratch||
        ((uintptr_t)scratch%_Alignof(float))||scratch_bytes<X4_GPU_VERIFY_CPU_BYTES||
        !s->rows||s->rows>X4_GPU_VERIFY_ROWS||s->first_row>=1080||s->rows>1080-s->first_row||
        s->quality_word!=x4_gpu_quality_normalize(s->quality_word))return r->result;
    uint32_t *reference=scratch;
    int rc;
    if(x4_gpu_quality_active(s->quality_word,s->color.sharpness)){
        void *quality_scratch=(uint8_t *)scratch+1920u*1080u*4u;
        rc=x4_gpu_quality_reference_rows(&s->layout,s->source,s->source_bytes,reference,
            X4_GPU_VERIFY_RGBA_BYTES,s->first_row,s->rows,&s->color,s->quality_word,
            quality_scratch,X4_GPU_QUALITY_REFERENCE_SCRATCH_BYTES);
    }else if(s->layout.visible_width==1280){
        rc=x4_video_pipeline_gpu_reference_720_rows(&s->layout,s->source,s->source_bytes,
            reference,1920u*1080u*4u,s->first_row,s->rows,&s->color);
        reference+=(size_t)s->first_row*1920;
    }else{
        rc=x4_video_pipeline_gpu_reference_rows(&s->layout,s->source,s->source_bytes,
            reference,X4_GPU_VERIFY_RGBA_BYTES,s->first_row,s->rows,&s->color);
    }
    r->result=rc;
    if(rc)return rc;
    for(size_t i=0;i<(size_t)s->rows*1920;++i){
        bool bad=false;
        for(unsigned channel=0;channel<4;++channel){
            int delta=(int)((s->actual[i]>>(channel*8))&255u)-(int)((reference[i]>>(channel*8))&255u);
            unsigned difference=(unsigned)(delta<0?-delta:delta);
            if(difference>r->max_channel_difference)r->max_channel_difference=difference;
            if(difference>1)bad=true;
        }
        ++r->checked_pixels;
        if(bad)++r->pixels_outside;
    }
    return 0;
}

