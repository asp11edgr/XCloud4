/* SPDX-License-Identifier: GPL-3.0-only */
/* GPU kernel input for gen_gpu_quality_ir.py, not a CPU test oracle.
 * AMD FidelityFX CAS adaptation MIT, fixed commit
 * 9fabcc9a2c45f958aff55ddfda337e74ef894b7f, ffx_cas.h/ffx_a.h.
 * Copyright (c) 2017-2019 Advanced Micro Devices, Inc.
 * Copyright (c) <2014> <Michal Drobot>.
 * See docs/licenses/AMD_FidelityFX_CAS.txt for the full MIT notice.
 */
typedef unsigned U;
typedef U V4 __attribute__((ext_vector_type(4)));
typedef struct {float x,y,z;} F3;
#define INLINE static inline __attribute__((always_inline))
extern U raw_load(V4,U,U,U) __asm__("llvm.amdgcn.raw.buffer.load.i32");
extern void raw_store(U,V4,U,U,U) __asm__("llvm.amdgcn.raw.buffer.store.i32");
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
typedef struct {U pitch,dpitch;int ym,rv,gu,gv,bu,bias;U word,sharp,ipitch;} Params;
INLINE U bits(float f){union {float f;U u;} p;p.f=f;return p.u;}
INLINE float flt(U u){union {float f;U u;} p;p.u=u;return p.f;}
INLINE float clamp(float f,float high){return f<0.0f?0.0f:f>high?high:f;}
INLINE int edge(int x,int high){return x<0?0:x>high?high:x;}
INLINE float inv(float x){
    float r=flt(0x7ef07ebbu-bits(x));
    for(U i=0;i<3;++i){float a=x*r;float b=2.0f-a;r=r*b;}return r;
}
INLINE float root(float x){
    if(x<=0.0f)return 0.0f;
    float r=flt((bits(x)>>1)+0x1fbc4639u);
    for(U i=0;i<3;++i){float a=x*inv(r);float b=r+a;r=0.5f*b;}return r;
}
INLINE U rd(V4 buffer,U byte){U word=raw_load(buffer,byte&~3u,0u,0u);return (word>>((byte&3u)*8u))&255u;}
INLINE U hash(U x,U y,U plane){
    U h=x*0x9e3779b9u^y*0x85ebca6bu^plane*0xc2b2ae35u^0x58433425u;
    h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;return h^(h>>16);
}
INLINE float sample(V4 buffer,U pitch,int x,int row,U comp,U chroma){
    x=edge(x,chroma?959:1919);row=edge(row,chroma?539:1079);
    return (float)rd(buffer,(U)row*pitch+(U)x*(chroma?2u:1u)+comp);
}
INLINE float filter(V4 buffer,U pitch,int x,int row,U comp,U chroma,U strength){
    float c=sample(buffer,pitch,x,row,comp,chroma);
    if(!strength)return c;
    int r=chroma?(strength==1?2:4):(strength==1?4:8);
    U a=hash((U)x,(U)row,chroma)&3u;
    int dx=a==0?r:a==1?(r*7+4)/8:a==2?(r*3+2)/4:(r*3+4)/8;
    int dy=a==0?0:a==1?(r*3+4)/8:a==2?(r*3+2)/4:(r*7+4)/8;
    float s0=sample(buffer,pitch,x+dx,row+dy,comp,chroma);
    float s1=sample(buffer,pitch,x-dy,row+dx,comp,chroma);
    float s2=sample(buffer,pitch,x-dx,row-dy,comp,chroma);
    float s3=sample(buffer,pitch,x+dy,row-dx,comp,chroma);
    float d0=s0-c,d1=s1-c,d2=s2-c,d3=s3-c;
    if(d0<0)d0=-d0;if(d1<0)d1=-d1;if(d2<0)d2=-d2;if(d3<0)d3=-d3;
    float threshold=chroma?(strength==1?1.5f:3.0f):(strength==1?2.0f:4.0f);
    if(d0>=threshold || d1>=threshold || d2>=threshold || d3>=threshold)return c;
    float sum=s0+s1;sum=sum+s2;sum=sum+s3;
    float avg=sum*0.25f;float mix=strength==1?0.5f:0.75f;
    return c+(avg-c)*mix;
}
INLINE float y_at(V4 buffer,Params p,int x,int row){
    return filter(buffer,p.pitch,edge(x,1919),edge(row,1079),0u,0u,(p.word>>1)&3u);
}
INLINE float uv_at(V4 buffer,Params p,int i,int j,int other,U comp){
    U strength=(p.word>>1)&3u;
    float a=filter(buffer,p.pitch,i,j,comp,1u,strength);
    float b=filter(buffer,p.pitch,i,other,comp,1u,strength);
    float sum=3.0f*a+b;
    return strength?sum*0.25f:(float)(((U)sum+2u)>>2);
}
INLINE F3 prepare(V4 yb,V4 uvb,Params p,U x,U row){
    U cas=(p.word&8u) && p.sharp;
    float luma=y_at(yb,p,(int)x,(int)row);
    if(!cas && p.sharp){
        float left=y_at(yb,p,(int)x-1,(int)row),right=y_at(yb,p,(int)x+1,(int)row);
        float up=y_at(yb,p,(int)x,(int)row-1),down=y_at(yb,p,(int)x,(int)row+1);
        float delta=4.0f*luma-left;delta=delta-right;delta=delta-up;delta=delta-down;
        float adjust=delta*(p.sharp==1?0.0625f:0.125f)+0.5f;
        int whole=(int)adjust;if((float)whole>adjust)--whole;
        luma=clamp(luma+(float)whole,255.0f);
    }
    int j=(int)row>>1,other=edge(j+((row&1u)?1:-1),539),i=(int)x>>1;
    float u=uv_at(uvb,p,i,j,other,0u),v=uv_at(uvb,p,i,j,other,1u);
    if(x&1u){
        float u1=uv_at(uvb,p,edge(i+1,959),j,other,0u);
        float v1=uv_at(uvb,p,edge(i+1,959),j,other,1u);
        if(p.word&6u){u=(u+u1)*0.5f;v=(v+v1)*0.5f;}
        else {u=(float)(((U)u+(U)u1+1u)>>1);v=(float)(((U)v+(U)v1+1u)>>1);}
    }
    float yc=(luma-(float)p.bias)*(float)p.ym,du=u-128.0f,dv=v-128.0f;
    float r=yc+dv*(float)p.rv;
    float g=yc+du*(float)p.gu;g=g+dv*(float)p.gv;
    float b=yc+du*(float)p.bu;
    F3 rgb=(F3){clamp(r*0.0001220703125f,255.0f),clamp(g*0.0001220703125f,255.0f),clamp(b*0.0001220703125f,255.0f)};
    if(cas){float r=rgb.x*0.0039215688593685626983642578125f;float g=rgb.y*0.0039215688593685626983642578125f;float b=rgb.z*0.0039215688593685626983642578125f;rgb=(F3){r*r,g*g,b*b};}return rgb;
}
INLINE Params params(V4 buffer){
    Params p;
    p.pitch=raw_load(buffer,0u,0u,0u);p.dpitch=raw_load(buffer,4u,0u,0u);
    p.ym=(int)raw_load(buffer,8u,0u,0u);p.rv=(int)raw_load(buffer,12u,0u,0u);
    p.gu=(int)raw_load(buffer,16u,0u,0u);p.gv=(int)raw_load(buffer,20u,0u,0u);
    p.bu=(int)raw_load(buffer,24u,0u,0u);p.bias=(int)raw_load(buffer,28u,0u,0u);
    p.word=raw_load(buffer,32u,0u,0u);p.sharp=raw_load(buffer,36u,0u,0u);
    p.ipitch=raw_load(buffer,40u,0u,0u);return p;
}
INLINE F3 rgb_at(V4 buffer,U pitch,int x,int row){
    U offset=((U)edge(row,1079)*pitch+(U)edge(x,1919))*16u;
    return (F3){flt(raw_load(buffer,offset,0u,0u)),flt(raw_load(buffer,offset+4u,0u,0u)),flt(raw_load(buffer,offset+8u,0u,0u))};
}
/* Finite CAS values only. Saturation is equivalent to clamp(x,1), including
 * negative values -> positive zero. No approximate FP instruction is used. */
INLINE float cas_clamp(float x){U u=bits(x);if(u&0x80000000u)return 0.0f;if(u>0x3f800000u)u=0x3f800000u;return flt(u);}
INLINE F3 cas_filter(V4 buffer,Params p,U x,U row){
    F3 b=rgb_at(buffer,p.ipitch,(int)x,(int)row-1),d=rgb_at(buffer,p.ipitch,(int)x-1,(int)row);
    F3 e=rgb_at(buffer,p.ipitch,(int)x,(int)row),f=rgb_at(buffer,p.ipitch,(int)x+1,(int)row);
    F3 h=rgb_at(buffer,p.ipitch,(int)x,(int)row+1);
    /* prepare stores finite, nonnegative squared RGB. For this domain,
     * unsigned IEEE bits have exactly the same ordering as float values.
     * Integer min/max avoids the LLVM 21.1.8 gfx700 DAG-combine crash reduced
     * to nested fcmp/select in entrega-25/artifacts/reduce-finish-r1.
     * This changes representation only, never the filter arithmetic. */
    U minimum=bits(e.y)&0x7fffffffu,maximum=minimum;
    U value=bits(b.y)&0x7fffffffu;if(value<minimum)minimum=value;if(value>maximum)maximum=value;
    value=bits(d.y)&0x7fffffffu;if(value<minimum)minimum=value;if(value>maximum)maximum=value;
    value=bits(f.y)&0x7fffffffu;if(value<minimum)minimum=value;if(value>maximum)maximum=value;
    value=bits(h.y)&0x7fffffffu;if(value<minimum)minimum=value;if(value>maximum)maximum=value;
    float mx=flt(maximum),distance=1.0f-mx;
    U distance_bits=bits(distance);if(minimum<distance_bits)distance_bits=minimum;
    distance=flt(distance_bits);
    float amp=maximum?cas_clamp(distance*inv(mx)):0.0f;
    float peak=p.sharp==1?-0.14705882966518402099609375f:-0.1694915294647216796875f;
    float w=root(amp)*peak,denom=1.0f+4.0f*w,rcp=inv(denom);
    float r=b.x*w+d.x*w;r=r+f.x*w;r=r+h.x*w;r=r+e.x;r=r*rcp;
    float g=b.y*w+d.y*w;g=g+f.y*w;g=g+h.y*w;g=g+e.y;g=g*rcp;
    float blue=b.z*w+d.z*w;blue=blue+f.z*w;blue=blue+h.z*w;blue=blue+e.z;blue=blue*rcp;
    return (F3){root(cas_clamp(r))*255.0f,root(cas_clamp(g))*255.0f,root(cas_clamp(blue))*255.0f};
}
INLINE U quantize(F3 rgb,V4 buffer,U word,U x,U row){
    float threshold=0.5f;
    if(word&1u){
        U index=((row&63u)<<6)|(x&63u),offset=64u+index*2u;
        U ranks=raw_load(buffer,offset&~3u,0u,0u),rank=(ranks>>((offset&2u)*8u))&65535u;
        threshold=((float)rank+0.5f)*0.000244140625f;
    }
    U r=(U)clamp(rgb.x+threshold,255.0f),g=(U)clamp(rgb.y+threshold,255.0f),b=(U)clamp(rgb.z+threshold,255.0f);
    return 0xff000000u|r|(g<<8)|(b<<16);
}
void main(V4 yb,V4 uvb,V4 out,V4 prm,U gx,U gy,U tx,U ty){
    U x=gx*8u+tx,row=gy*8u+ty;
    if(x>=1920u || row>=1080u)return;
    Params p=params(prm);
#if X4_QUALITY_PASS == 2
    F3 rgb=cas_filter(yb,p,x,row);
    raw_store(quantize(rgb,prm,p.word,x,row),out,(row*p.dpitch+x)*4u,0u,0u);
#else
    F3 rgb=prepare(yb,uvb,p,x,row);
#if X4_QUALITY_PASS == 1
    U offset=(row*p.ipitch+x)*16u;
    raw_store(bits(rgb.x),out,offset,0u,0u);raw_store(bits(rgb.y),out,offset+4u,0u,0u);
    raw_store(bits(rgb.z),out,offset+8u,0u,0u);raw_store(0u,out,offset+12u,0u,0u);
#else
    raw_store(quantize(rgb,prm,p.word,x,row),out,(row*p.dpitch+x)*4u,0u,0u);
#endif
#endif
}


