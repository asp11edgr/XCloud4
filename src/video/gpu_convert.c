/* SPDX-License-Identifier: GPL-3.0-only */
/* PM4/cache layout adapted from VCFG (MIT), commit
 * 955d3f8a8afed278d7acdd8900635797795999ef. Copyright (c) 2026 VCFG
 * contributors. Full notice is in THIRD_PARTY_NOTICES.md/docs/licenses.
 * Owned compute queue only: no hooks, game memory or register restoration.
 */
#include "gpu_convert.h"
#include "gpu/gpu_shaders.h"
#include "gpu/verify/verify_rows_shader.h"
#include "gpu_quality_ref.h"
#include <orbis/libkernel.h>
#include <orbis/GnmDriver.h>
#include <emmintrin.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

enum { DCB_BYTES=4096, CONTROL_ALIGNMENT=65536,
    TIMESTAMP_PENDING_CAPACITY=4096, TIMESTAMP_PROBE_SAMPLES=300 };
#define TIMESTAMP_CALIBRATION_US UINT64_C(10000000)
#define PKT3(op,n,c) (0xc0000000u|(((uint32_t)(n)-1u)<<16)|((uint32_t)(op)<<8)|((uint32_t)(c)<<1))
typedef struct {
    const uint32_t *words;size_t bytes;uint32_t rsrc1,rsrc2;
} Shader;
static const Shader shaders[8]={
    {x4_gpu_original_code,sizeof(x4_gpu_original_code),X4_GPU_ORIGINAL_RSRC1,X4_GPU_ORIGINAL_RSRC2},
    {x4_gpu_smooth0_code,sizeof(x4_gpu_smooth0_code),X4_GPU_SMOOTH0_RSRC1,X4_GPU_SMOOTH0_RSRC2},
    {x4_gpu_smooth1_code,sizeof(x4_gpu_smooth1_code),X4_GPU_SMOOTH1_RSRC1,X4_GPU_SMOOTH1_RSRC2},
    {x4_gpu_smooth2_code,sizeof(x4_gpu_smooth2_code),X4_GPU_SMOOTH2_RSRC1,X4_GPU_SMOOTH2_RSRC2},
    {x4_gpu_original720_code,sizeof(x4_gpu_original720_code),X4_GPU_ORIGINAL720_RSRC1,X4_GPU_ORIGINAL720_RSRC2},
    {x4_gpu_smooth720_0_code,sizeof(x4_gpu_smooth720_0_code),X4_GPU_SMOOTH720_0_RSRC1,X4_GPU_SMOOTH720_0_RSRC2},
    {x4_gpu_smooth720_1_code,sizeof(x4_gpu_smooth720_1_code),X4_GPU_SMOOTH720_1_RSRC1,X4_GPU_SMOOTH720_1_RSRC2},
    {x4_gpu_smooth720_2_code,sizeof(x4_gpu_smooth720_2_code),X4_GPU_SMOOTH720_2_RSRC1,X4_GPU_SMOOTH720_2_RSRC2}
};
/* Separate code/control storage preserves every legacy arena offset and DCB
 * byte when quality is off, including after quality was used in this session. */
static const Shader quality_shaders[]={
    {x4_gpu_quality_direct_code,sizeof(x4_gpu_quality_direct_code),X4_GPU_QUALITY_DIRECT_RSRC1,X4_GPU_QUALITY_DIRECT_RSRC2},
#if !X4_PUBLIC
    {x4_gpu_quality_prepare_code,sizeof(x4_gpu_quality_prepare_code),X4_GPU_QUALITY_PREPARE_RSRC1,X4_GPU_QUALITY_PREPARE_RSRC2},
    {x4_gpu_quality_finish_code,sizeof(x4_gpu_quality_finish_code),X4_GPU_QUALITY_FINISH_RSRC1,X4_GPU_QUALITY_FINISH_RSRC2}
#endif
};
static const Shader verify_shader={x4_gpu_verify_rows_code,sizeof(x4_gpu_verify_rows_code),
    X4_GPU_VERIFY_ROWS_RSRC1,X4_GPU_VERIFY_ROWS_RSRC2};
enum { VERIFY_FREE,VERIFY_GPU,VERIFY_READY,VERIFY_CLAIMED,VERIFY_COMPLETE };
static atomic_uint_fast64_t verification_owner_epoch=0;
typedef struct {
    uint64_t pending_ticks[TIMESTAMP_PENDING_CAPACITY];
    unsigned pending_count,first_pending_count,first_invalid;
} TimestampLedger;
struct X4GpuConvert {
    uint8_t *arena,*code[8];uint32_t *dcb,*parameters;
    volatile uint64_t *label,*timestamp_start,*timestamp_end;
    off_t physical;bool allocated,mapped;
    uint32_t sequence;OrbisPthread submitter;
    TimestampLedger timestamps[X4_GPU_RESOLUTION_COUNT];
    X4GpuConvertStatus status;
    uint8_t *quality_arena,*quality_code[3];
    uint32_t *quality_dcb,*quality_parameters;
    off_t quality_physical,intermediate_physical;
    void *intermediate;
    bool quality_allocated,quality_mapped,quality_attempted,quality_failed;
    bool intermediate_allocated,intermediate_mapped;
    uint8_t *verify_control,*verify_pixels,*verify_code;
    uint32_t *verify_dcb,*verify_parameters;
    volatile uint64_t *verify_label,*verify_start,*verify_end;
    off_t verify_control_physical,verify_pixels_physical;
    bool verify_control_allocated,verify_control_mapped;
    bool verify_pixels_allocated,verify_pixels_mapped,verify_attempted;
    bool verify_requested,verify_this_frame,verify_failure_notice;
    unsigned verify_first,verify_rows;
    uint64_t verify_generation,verify_epoch;
    atomic_uint verify_state;
    X4GpuVerifySnapshot verify_snapshot;
    X4GpuVerifyResult verify_result;
};
static size_t aligned(size_t value,size_t alignment)
{return (value+alignment-1)&~(alignment-1);}
static void flush_cpu(const void *address,size_t bytes)
{
#ifdef X4_GPU_CONVERT_TEST_FLUSH
    X4_GPU_CONVERT_TEST_FLUSH(address,bytes);
#endif
    uintptr_t end=(uintptr_t)address+bytes;
    for(uintptr_t p=(uintptr_t)address&~(uintptr_t)63;p<end;p+=64)
        _mm_clflush((const void *)p);
    _mm_mfence();
}
static void disable(X4GpuConvert *g,const char *reason)
{g->status.disabled=true;g->status.reason=reason;}
static int cpu_fallback(X4GpuConvert *g,const char *reason,bool permanent)
{
    ++g->status.cpu_fallbacks;
    if(permanent)disable(g,reason);else g->status.reason=reason;
    return X4_GPU_CONVERT_CPU;
}
static void pause_observe(X4GpuConvert *g,uint64_t now,bool finish)
{
    if(!g->status.pause_active)return;
    uint64_t duration=now>=g->status.pause_started_us?now-g->status.pause_started_us:0;
    if(duration>g->status.pause_max_us)g->status.pause_max_us=duration;
    if(finish){
        g->status.pause_last_us=duration;
        g->status.pause_last_frames=g->status.pause_current_frames;
        g->status.pause_current_frames=0;g->status.pause_active=false;
    }
}
X4GpuConvert *x4_gpu_convert_open(void)
{
    X4GpuConvert *g=calloc(1,sizeof(*g));if(!g)return NULL;
    g->status.reason="no";g->status.arena_bytes=X4_GPU_CONVERT_ARENA_BYTES;
    g->status.map_result=g->status.submit_result=g->status.submit_done_result=
        g->status.unmap_result=g->status.release_result=g->status.allowed_result=X4_GPU_CONVERT_NOT_RUN;
    g->status.quality_reason="off";
    g->status.verification_last_result=X4_GPU_CONVERT_NOT_RUN;
    atomic_init(&g->verify_state,VERIFY_FREE);
    g->verify_epoch=atomic_fetch_add_explicit(&verification_owner_epoch,1,memory_order_relaxed)+1;
    g->status.verification_reserve_result=g->status.verification_map_result=
        g->status.verification_unmap_result=g->status.verification_release_result=
        g->status.verification_control_reserve_result=g->status.verification_control_map_result=
        g->status.verification_control_unmap_result=g->status.verification_control_release_result=X4_GPU_CONVERT_NOT_RUN;
    g->status.quality_reserve_result=g->status.quality_map_result=
        g->status.quality_unmap_result=g->status.quality_release_result=
        g->status.intermediate_reserve_result=g->status.intermediate_map_result=
        g->status.intermediate_unmap_result=g->status.intermediate_release_result=X4_GPU_CONVERT_NOT_RUN;
    g->status.reserve_result=sceKernelAllocateDirectMemory(0,
        (off_t)sceKernelGetDirectMemorySize(),X4_GPU_CONVERT_ARENA_BYTES,
        CONTROL_ALIGNMENT,0,&g->physical);
    if(g->status.reserve_result){disable(g,"prepare_reserve");return g;}
    g->allocated=true;
    g->status.map_result=sceKernelMapDirectMemory((void **)&g->arena,
        X4_GPU_CONVERT_ARENA_BYTES,0x33,0,g->physical,CONTROL_ALIGNMENT);
    if(g->status.map_result){disable(g,"prepare_map");return g;}
    g->mapped=true;memset(g->arena,0,X4_GPU_CONVERT_ARENA_BYTES);
    size_t offset=0;
    for(unsigned i=0;i<8;++i){
        offset=aligned(offset,256);
        if(offset>X4_GPU_CONVERT_ARENA_BYTES ||
            shaders[i].bytes>X4_GPU_CONVERT_ARENA_BYTES-offset){
            disable(g,"prepare_code_bounds");return g;
        }
        g->code[i]=g->arena+offset;
        memcpy(g->code[i],shaders[i].words,shaders[i].bytes);
        offset+=shaders[i].bytes;
    }
    offset=aligned(offset,256);
    if(offset>X4_GPU_CONVERT_ARENA_BYTES ||
        DCB_BYTES+256u>X4_GPU_CONVERT_ARENA_BYTES-offset){
        disable(g,"prepare_control_bounds");return g;
    }
    g->dcb=(uint32_t *)(g->arena+offset);offset+=DCB_BYTES;
    g->parameters=(uint32_t *)(g->arena+offset);offset+=64;
    g->label=(volatile uint64_t *)(g->arena+offset);offset+=64;
    g->timestamp_start=(volatile uint64_t *)(g->arena+offset);offset+=64;
    g->timestamp_end=(volatile uint64_t *)(g->arena+offset);
    flush_cpu(g->arena,X4_GPU_CONVERT_ARENA_BYTES);
    g->status.prepared=true;return g;
}
bool x4_gpu_convert_admit(const X4VideoNv12Layout *l,X4VideoFormat format,
    const X4VideoPipelineColor *color,const char **reason)
{
    const char *why="no";
    if(format!=X4_VIDEO_FORMAT_NV12)why="format_not_nv12_8bit";
    else if(!l || !((l->visible_width==1920 && l->visible_height==1080 &&
        l->coded_width==1920 && l->coded_height==1088 && l->storage_height==1088) ||
        (l->visible_width==1280 && l->visible_height==720 && l->coded_width==1280 &&
        l->coded_height==720 && l->storage_height==720)) ||
        l->visible_y_offset || l->pitch<l->coded_width || l->pitch>X4_VIDEO_MAX_PITCH ||
        (l->pitch&3u))why="geometry_not_supported";
    else {
        size_t y=(size_t)l->pitch*l->storage_height,uv=y/2;
        if(l->y_bytes!=y || l->uv_bytes!=uv || l->uv_plane_offset!=y ||
            l->visible_uv_offset!=y || l->total_nv12_bytes!=y+uv ||
            l->rgb_pixels!=(size_t)l->visible_width*l->visible_height ||
            l->rgb_bytes!=(size_t)l->visible_width*l->visible_height*4)
            why="layout_bounds";
        else if(!color || color->convert_mode>X4_CONVERT_SMOOTH ||
            color->sharpness>X4_SHARPNESS_MED)why="picture_mode";
    }
    if(reason)*reason=why;
    return strcmp(why,"no")==0;
}
static bool address_ok(const void *address,size_t bytes)
{
    uintptr_t a=(uintptr_t)address;
    return address && bytes && !(a&3u) && bytes<=UINT32_MAX &&
        a<=UINT64_C(0xffffffffffff) && bytes-1<=UINT64_C(0xffffffffffff)-a;
}
static void descriptor(uint32_t d[4],const void *address,uint32_t bytes,uint32_t format)
{
    uintptr_t a=(uintptr_t)address;
    d[0]=(uint32_t)a;d[1]=(uint32_t)(a>>32)&0xffffu;d[2]=bytes;d[3]=format;
}
static uint32_t *acquire(uint32_t *p,uint32_t coherence)
{
    *p++=PKT3(0x58,6,0);*p++=coherence;*p++=UINT32_MAX;*p++=0;
    *p++=0;*p++=0;*p++=10;return p;
}
static bool timestamp_address_ok(const X4GpuConvert *g,const volatile uint64_t *slot)
{
    uintptr_t a=(uintptr_t)slot,base=(uintptr_t)g->arena;
    return !(a&7u) && a>=base && a-base<=X4_GPU_CONVERT_ARENA_BYTES-sizeof(*slot);
}
static uint32_t *timestamp(uint32_t *p,const volatile uint64_t *slot)
{
    /* Mesa 234055882d7a: sid.h:202-214, si_query.c:847-849/962-967;
     * VCFG 955d3f8a8afe: a20_copy_impl.h:289-294. DW3 has DATA_SEL=3
     * (64-bit timestamp), INT_SEL=0 and the low 16 address-high bits.
     * No frequency is implied by this packet or by the shader clock. */
    uintptr_t a=(uintptr_t)slot;
    *p++=PKT3(0x47,5,0);*p++=0x528;*p++=(uint32_t)a;
    *p++=0x60000000u|((uint32_t)(a>>32)&0xffffu);*p++=0;*p++=0;
    return p;
}
static uint32_t commands(X4GpuConvert *g,const X4GpuConvertFrame *f,unsigned shader)
{
    uint32_t users[16];const X4VideoNv12Layout *l=&f->layout;
    descriptor(users,f->source,(uint32_t)l->y_bytes,0x0000cfacu);
    descriptor(users+4,f->source+l->uv_plane_offset,(uint32_t)l->uv_bytes,0x0001cfacu);
    descriptor(users+8,f->destination,(uint32_t)f->destination_bytes,0x00024facu);
    descriptor(users+12,g->parameters,32,0x00024facu);
    uintptr_t code=(uintptr_t)g->code[shader],label=(uintptr_t)g->label;
    if((code&255u)||(label&7u) || !timestamp_address_ok(g,g->timestamp_start) ||
        !timestamp_address_ok(g,g->timestamp_end))return 0;
    uint32_t *p=g->dcb;
    p=acquire(p,0x80c40000u);p=acquire(p,0x88400000u);
    *p++=PKT3(0x76,4,1);*p++=0x204;*p++=0;*p++=0;*p++=0;
    *p++=PKT3(0x76,4,1);*p++=0x207;*p++=8;*p++=8;*p++=1;
    *p++=PKT3(0x76,3,1);*p++=0x20c;*p++=(uint32_t)(code>>8);*p++=(uint32_t)(code>>40);
    *p++=PKT3(0x76,3,1);*p++=0x212;*p++=shaders[shader].rsrc1;*p++=shaders[shader].rsrc2;
    *p++=PKT3(0x76,4,1);*p++=0x215;*p++=0;*p++=UINT32_MAX;*p++=UINT32_MAX;
    *p++=PKT3(0x76,2,1);*p++=0x218;*p++=0;
    *p++=PKT3(0x76,17,1);*p++=0x240;
    for(unsigned i=0;i<16;++i)*p++=users[i];
    p=timestamp(p,g->timestamp_start);
    *p++=PKT3(0x15,4,1);*p++=240;*p++=135;*p++=1;*p++=1;
    *p++=PKT3(0x46,1,0);*p++=0x407;
    p=acquire(p,0x80c40000u);
    p=timestamp(p,g->timestamp_end);
    *p++=PKT3(0x47,5,0);*p++=0x528;*p++=(uint32_t)label;
    *p++=0x40000000u|((uint32_t)(label>>32)&0xffffu);*p++=g->sequence;*p++=0;
    size_t bytes=(size_t)(p-g->dcb)*4;
    return bytes<=DCB_BYTES?(uint32_t)bytes:0;
}
static void parameters(X4GpuConvert *g,const X4GpuConvertFrame *f)
{
    /* Same Q13 inverse matrices as the unchanged CPU oracle. Matrix1=709;
     * every other value follows the CPU's BT.601 compatibility assumption.
     */
    static const int32_t coefficient[4][6]={
        {9539,13075,-3209,-6660,16525,16},
        {9539,14686,-1747,-4366,17305,16},
        {8192,11485,-2819,-5850,14516,0},
        {8192,12901,-1535,-3835,15201,0}
    };
    unsigned matrix=(f->color.full_range?2u:0u)+(f->color.matrix_coefficients==1?1u:0u);
    g->parameters[0]=f->layout.pitch;g->parameters[1]=f->destination_pitch;
    for(unsigned i=0;i<6;++i)g->parameters[i+2]=(uint32_t)coefficient[matrix][i];
}
static bool quality_failure(X4GpuConvert *g,const char *reason)
{
    /* No driver submit occurred: preserve any partial native allocation for
     * close, avoid per-frame retry storms, and leave legacy GPU usable. */
    g->quality_failed=true;g->status.quality_failed=true;
    g->status.quality_reason=reason;return false;
}
static bool quality_prepare(X4GpuConvert *g,bool cas)
{
    if(g->quality_failed)return false;
    if(!g->quality_attempted){
        g->quality_attempted=true;
        g->status.quality_reserve_result=sceKernelAllocateDirectMemory(0,
            (off_t)sceKernelGetDirectMemorySize(),X4_GPU_CONVERT_QUALITY_ARENA_BYTES,
            CONTROL_ALIGNMENT,0,&g->quality_physical);
        if(g->status.quality_reserve_result)return quality_failure(g,"quality_reserve");
        g->quality_allocated=true;
        g->status.quality_arena_bytes=X4_GPU_CONVERT_QUALITY_ARENA_BYTES;
        g->status.quality_map_result=sceKernelMapDirectMemory((void **)&g->quality_arena,
            X4_GPU_CONVERT_QUALITY_ARENA_BYTES,0x33,0,g->quality_physical,CONTROL_ALIGNMENT);
        if(g->status.quality_map_result)return quality_failure(g,"quality_map");
        g->quality_mapped=true;memset(g->quality_arena,0,X4_GPU_CONVERT_QUALITY_ARENA_BYTES);
        size_t offset=0;
        for(unsigned i=0;i<sizeof(quality_shaders)/sizeof(quality_shaders[0]);++i){
            offset=aligned(offset,256);
            if(offset>X4_GPU_CONVERT_QUALITY_ARENA_BYTES ||
                quality_shaders[i].bytes>X4_GPU_CONVERT_QUALITY_ARENA_BYTES-offset)
                return quality_failure(g,"quality_code_bounds");
            g->quality_code[i]=g->quality_arena+offset;
            memcpy(g->quality_code[i],quality_shaders[i].words,quality_shaders[i].bytes);
            offset+=quality_shaders[i].bytes;
        }
        offset=aligned(offset,256);
        if(offset>X4_GPU_CONVERT_QUALITY_ARENA_BYTES ||
            DCB_BYTES+X4_GPU_QUALITY_DATA_BYTES>X4_GPU_CONVERT_QUALITY_ARENA_BYTES-offset)
            return quality_failure(g,"quality_control_bounds");
        g->quality_dcb=(uint32_t *)(g->quality_arena+offset);offset+=DCB_BYTES;
        g->quality_parameters=(uint32_t *)(g->quality_arena+offset);
        memcpy((uint8_t *)g->quality_parameters+X4_GPU_QUALITY_NOISE_OFFSET,
            x4_gpu_quality_noise_ranks(),X4_GPU_QUALITY_NOISE_BYTES);
        flush_cpu(g->quality_arena,X4_GPU_CONVERT_QUALITY_ARENA_BYTES);
    }
#if !X4_PUBLIC
    if(cas&&!g->intermediate_mapped){
        g->status.intermediate_reserve_result=sceKernelAllocateDirectMemory(0,
            (off_t)sceKernelGetDirectMemorySize(),X4_GPU_CONVERT_QUALITY_INTERMEDIATE_NATIVE_BYTES,
            CONTROL_ALIGNMENT,0,&g->intermediate_physical);
        if(g->status.intermediate_reserve_result)return quality_failure(g,"quality_intermediate_reserve");
        g->intermediate_allocated=true;
        g->status.quality_intermediate_bytes=X4_GPU_CONVERT_QUALITY_INTERMEDIATE_NATIVE_BYTES;
        g->status.intermediate_map_result=sceKernelMapDirectMemory(&g->intermediate,
            X4_GPU_CONVERT_QUALITY_INTERMEDIATE_NATIVE_BYTES,0x33,0,g->intermediate_physical,CONTROL_ALIGNMENT);
        if(g->status.intermediate_map_result)return quality_failure(g,"quality_intermediate_map");
        g->intermediate_mapped=true;
        /* Only the GPU writes it, over the entire visible 1920x1080 extent.
         * No full-frame CPU memset or cache flush enters the steady path. */
    }
#else
    (void)cas;
#endif
    g->status.quality_prepared=true;return true;
}
static uint32_t *quality_bind(uint32_t *p,const uint32_t users[16],uintptr_t code,
    const Shader *shader)
{
    *p++=PKT3(0x76,4,1);*p++=0x204;*p++=0;*p++=0;*p++=0;
    *p++=PKT3(0x76,4,1);*p++=0x207;*p++=8;*p++=8;*p++=1;
    *p++=PKT3(0x76,3,1);*p++=0x20c;*p++=(uint32_t)(code>>8);*p++=(uint32_t)(code>>40);
    *p++=PKT3(0x76,3,1);*p++=0x212;*p++=shader->rsrc1;*p++=shader->rsrc2;
    *p++=PKT3(0x76,4,1);*p++=0x215;*p++=0;*p++=UINT32_MAX;*p++=UINT32_MAX;
    *p++=PKT3(0x76,2,1);*p++=0x218;*p++=0;
    *p++=PKT3(0x76,17,1);*p++=0x240;
    for(unsigned i=0;i<16;++i)*p++=users[i];
    return p;
}
static uint32_t *quality_dispatch(uint32_t *p)
{*p++=PKT3(0x15,4,1);*p++=240;*p++=135;*p++=1;*p++=1;return p;}
static uint32_t *quality_barrier(uint32_t *p)
{
    /* GFX7: CS_PARTIAL_FLUSH waits for compute writes, then invalidate TC and
     * TCL1 for the following raw-buffer reads. No unsupported GFX8 TC_WB bit.
     * AMD SI_3D_registers, CP_COHER_CNTL bits22/23; Mesa fixed at
     * 234055882d7a, si_gfx_cs.c:1080-1091 (GFX6/7 no L2 write-back).
     * The unchanged legacy commands() deliberately retains its old packets. */
    *p++=PKT3(0x46,1,0);*p++=0x407;
    return acquire(p,0x80c00000u);
}
static uint32_t quality_commands(X4GpuConvert *g,const X4GpuConvertFrame *f,bool cas)
{
#if X4_PUBLIC
    if(cas)return 0; /* No public CAS shader/code array is linked. */
#endif
    uint32_t users[16];const X4VideoNv12Layout *l=&f->layout;
    descriptor(users,f->source,(uint32_t)l->y_bytes,0x0000cfacu);
    descriptor(users+4,f->source+l->uv_plane_offset,(uint32_t)l->uv_bytes,0x0001cfacu);
    descriptor(users+8,cas?g->intermediate:(void *)f->destination,
        cas?X4_GPU_QUALITY_INTERMEDIATE_BYTES:(uint32_t)f->destination_bytes,0x00024facu);
    descriptor(users+12,g->quality_parameters,X4_GPU_QUALITY_DATA_BYTES,0x00024facu);
    unsigned first=cas?1u:0u;
    if(((uintptr_t)g->quality_code[first]&255u) ||
        !address_ok(g->quality_parameters,X4_GPU_QUALITY_DATA_BYTES) ||
        !timestamp_address_ok(g,g->timestamp_start)||!timestamp_address_ok(g,g->timestamp_end))return 0;
    uint32_t *p=g->quality_dcb;
    p=acquire(p,0x80c00000u);p=acquire(p,0x88400000u);
    p=quality_bind(p,users,(uintptr_t)g->quality_code[first],&quality_shaders[first]);
    p=timestamp(p,g->timestamp_start);p=quality_dispatch(p);
#if !X4_PUBLIC
    if(cas){
        p=quality_barrier(p);
        descriptor(users,g->intermediate,X4_GPU_QUALITY_INTERMEDIATE_BYTES,0x00024facu);
        descriptor(users+4,(uint8_t *)g->quality_parameters+X4_GPU_QUALITY_NOISE_OFFSET,
            X4_GPU_QUALITY_NOISE_BYTES,0x00024facu);
        descriptor(users+8,f->destination,(uint32_t)f->destination_bytes,0x00024facu);
        p=quality_bind(p,users,(uintptr_t)g->quality_code[2],&quality_shaders[2]);
        p=quality_dispatch(p);
    }
#endif
    p=quality_barrier(p);p=timestamp(p,g->timestamp_end);
    uintptr_t label=(uintptr_t)g->label;
    *p++=PKT3(0x47,5,0);*p++=0x528;*p++=(uint32_t)label;
    *p++=0x40000000u|((uint32_t)(label>>32)&0xffffu);*p++=g->sequence;*p++=0;
    size_t bytes=(size_t)(p-g->quality_dcb)*4;
    return bytes<=DCB_BYTES?(uint32_t)bytes:0;
}
static int verification_failure(X4GpuConvert *g,const char *reason,int error)
{
    g->status.verification_failed=true;
    disable(g,reason);
    return error?error:X4_PIPELINE_STATE;
}
int x4_gpu_convert_verification_prepare(X4GpuConvert *g)
{
    if(!g||!g->status.prepared||g->status.pending)return X4_PIPELINE_STATE;
    if(g->status.verification_prepared)return 0;
    if(g->verify_attempted)return X4_PIPELINE_STATE;
    g->verify_attempted=true;
    g->status.verification_control_reserve_result=sceKernelAllocateDirectMemory(0,
        (off_t)sceKernelGetDirectMemorySize(),X4_GPU_VERIFY_CONTROL_BYTES,CONTROL_ALIGNMENT,0,&g->verify_control_physical);
    if(g->status.verification_control_reserve_result)
        return verification_failure(g,"verification_control_reserve",g->status.verification_control_reserve_result);
    g->verify_control_allocated=true;g->status.verification_control_bytes=X4_GPU_VERIFY_CONTROL_BYTES;
    g->status.verification_control_map_result=sceKernelMapDirectMemory((void **)&g->verify_control,
        X4_GPU_VERIFY_CONTROL_BYTES,0x33,0,g->verify_control_physical,CONTROL_ALIGNMENT);
    if(g->status.verification_control_map_result){
        if(g->verify_control)g->verify_control_mapped=true;
        return verification_failure(g,"verification_control_map",g->status.verification_control_map_result);
    }
    g->verify_control_mapped=true;
    if(!address_ok(g->verify_control,X4_GPU_VERIFY_CONTROL_BYTES)||((uintptr_t)g->verify_control&65535u))
        return verification_failure(g,"verification_control_address",X4_PIPELINE_INVALID);
    memset(g->verify_control,0,X4_GPU_VERIFY_CONTROL_BYTES);
    size_t offset=aligned(verify_shader.bytes,256);
    if(offset>X4_GPU_VERIFY_CONTROL_BYTES||DCB_BYTES+256u>X4_GPU_VERIFY_CONTROL_BYTES-offset)
        return verification_failure(g,"verification_control_bounds",X4_PIPELINE_INVALID);
    g->verify_code=g->verify_control;
    memcpy(g->verify_code,verify_shader.words,verify_shader.bytes);
    g->verify_dcb=(uint32_t *)(g->verify_control+offset);offset+=DCB_BYTES;
    g->verify_parameters=(uint32_t *)(g->verify_control+offset);offset+=64;
    g->verify_label=(volatile uint64_t *)(g->verify_control+offset);offset+=64;
    g->verify_start=(volatile uint64_t *)(g->verify_control+offset);offset+=64;
    g->verify_end=(volatile uint64_t *)(g->verify_control+offset);
    flush_cpu(g->verify_control,X4_GPU_VERIFY_CONTROL_BYTES);
    g->status.verification_reserve_result=sceKernelAllocateDirectMemory(0,
        (off_t)sceKernelGetDirectMemorySize(),X4_GPU_VERIFY_NATIVE_BYTES,CONTROL_ALIGNMENT,0,&g->verify_pixels_physical);
    if(g->status.verification_reserve_result)
        return verification_failure(g,"verification_reserve",g->status.verification_reserve_result);
    g->verify_pixels_allocated=true;g->status.verification_native_bytes=X4_GPU_VERIFY_NATIVE_BYTES;
    g->status.verification_map_result=sceKernelMapDirectMemory((void **)&g->verify_pixels,
        X4_GPU_VERIFY_NATIVE_BYTES,0x33,0,g->verify_pixels_physical,CONTROL_ALIGNMENT);
    if(g->status.verification_map_result){
        if(g->verify_pixels)g->verify_pixels_mapped=true;
        return verification_failure(g,"verification_map",g->status.verification_map_result);
    }
    g->verify_pixels_mapped=true;
    if(!address_ok(g->verify_pixels,X4_GPU_VERIFY_NATIVE_BYTES)||((uintptr_t)g->verify_pixels&65535u)||!g->verify_epoch)
        return verification_failure(g,"verification_address",X4_PIPELINE_INVALID);
    /* GPU writes every pixel that the reference may read. No full-frame
     * CPU clear/copy enters preparation or steady-state presentation. */
    g->status.verification_prepared=true;
    return 0;
}
bool x4_gpu_convert_verification_request(X4GpuConvert *g,unsigned first,unsigned rows)
{
    if(!g||!g->status.verification_prepared||g->status.verification_failed||
        !rows||rows>X4_GPU_VERIFY_ROWS||first>=1080||rows>1080-first)return false;
    if(g->verify_requested||atomic_load_explicit(&g->verify_state,memory_order_acquire)!=VERIFY_FREE){
        ++g->status.verification_skipped;return false;
    }
    g->verify_requested=true;g->verify_first=first;g->verify_rows=rows;
    return true;
}
static uint32_t *verify_timestamp(uint32_t *p,const volatile uint64_t *slot)
{
    uintptr_t a=(uintptr_t)slot;
    *p++=PKT3(0x47,5,0);*p++=0x528;*p++=(uint32_t)a;
    *p++=0x60000000u|((uint32_t)(a>>32)&0xffffu);*p++=0;*p++=0;
    return p;
}
static uint32_t verification_commands(X4GpuConvert *g,const X4GpuConvertFrame *f,uint32_t effective)
{
    if(!g->verify_requested)return 0;
    g->verify_requested=false;
    if(atomic_load_explicit(&g->verify_state,memory_order_acquire)!=VERIFY_FREE){
        ++g->status.verification_skipped;return 0;
    }
    if(!g->status.verification_prepared||g->verify_generation==UINT64_MAX){
        (void)verification_failure(g,"verification_sequence_exhausted",X4_PIPELINE_EXHAUSTED);return 0;
    }
    X4GpuVerifySnapshot *s=&g->verify_snapshot;
    if(x4_gpu_quality_verify_plan(&f->layout,&f->color,effective,g->verify_first,g->verify_rows,s)){
        (void)verification_failure(g,"verification_plan",X4_PIPELINE_INVALID);return 0;
    }
    s->token=++g->verify_generation;s->owner_epoch=g->verify_epoch;
    s->source=g->verify_pixels;s->actual=(const uint32_t *)(g->verify_pixels+X4_GPU_VERIFY_NV12_CAPACITY);
    X4VideoNv12Layout *l=&s->layout;
    uint32_t parameters[16]={f->layout.pitch,l->pitch,(uint32_t)f->layout.uv_plane_offset,
        (uint32_t)l->uv_plane_offset,X4_GPU_VERIFY_NV12_CAPACITY,f->destination_pitch,
        s->first_y,s->rows_y,s->first_uv,s->rows_uv,s->first_row,s->rows,0,0,0,0};
    memcpy(g->verify_parameters,parameters,sizeof(parameters));
    *g->verify_label=0;*g->verify_start=0;*g->verify_end=0;
    uint32_t users[16];
    descriptor(users,f->source,(uint32_t)f->layout.total_nv12_bytes,0x00024facu);
    descriptor(users+4,f->destination,(uint32_t)f->destination_bytes,0x00024facu);
    descriptor(users+8,g->verify_pixels,X4_GPU_VERIFY_NATIVE_BYTES,0x00024facu);
    descriptor(users+12,g->verify_parameters,64,0x00024facu);
    if(!address_ok(g->verify_pixels,X4_GPU_VERIFY_NATIVE_BYTES)||
        !address_ok(g->verify_control,X4_GPU_VERIFY_CONTROL_BYTES)||((uintptr_t)g->verify_code&255u))return 0;
    uint32_t *p=g->verify_dcb;
    p=acquire(p,0x80c40000u);p=acquire(p,0x88400000u);
    p=quality_bind(p,users,(uintptr_t)g->verify_code,&verify_shader);
    p=verify_timestamp(p,g->verify_start);
    unsigned height=s->rows_y;if(height<s->rows_uv)height=s->rows_uv;if(height<s->rows)height=s->rows;
    *p++=PKT3(0x15,4,1);*p++=240;*p++=(height+7)/8;*p++=1;*p++=1;
    *p++=PKT3(0x46,1,0);*p++=0x407;p=acquire(p,0x80c40000u);
    p=verify_timestamp(p,g->verify_end);
    uintptr_t label=(uintptr_t)g->verify_label;
    *p++=PKT3(0x47,5,0);*p++=0x528;*p++=(uint32_t)label;
    *p++=0x40000000u|((uint32_t)(label>>32)&0xffffu);*p++=g->sequence;*p++=0;
    size_t bytes=(size_t)(p-g->verify_dcb)*4;
    if(bytes>DCB_BYTES)return 0;
    flush_cpu(g->verify_parameters,64);flush_cpu((const void *)g->verify_label,64);
    flush_cpu((const void *)g->verify_start,64);flush_cpu((const void *)g->verify_end,64);
    flush_cpu(g->verify_dcb,bytes);
    atomic_store_explicit(&g->verify_state,VERIFY_GPU,memory_order_release);
    ++g->status.verification_captures;
    return (uint32_t)bytes;
}
bool x4_gpu_convert_verification_take(X4GpuConvert *g,X4GpuVerifySnapshot *out)
{
    if(!g||!out)return false;
    unsigned expected=VERIFY_READY;
    if(!atomic_compare_exchange_strong_explicit(&g->verify_state,&expected,VERIFY_CLAIMED,
        memory_order_acq_rel,memory_order_acquire))return false;
    *out=g->verify_snapshot;
    /* This function is worker-only. CPU cache maintenance is not on the
     * image path, and never touches native decoder or scanout storage. */
    flush_cpu(out->source+(size_t)out->first_y*out->layout.pitch,(size_t)out->rows_y*out->layout.pitch);
    flush_cpu(out->source+out->layout.uv_plane_offset+(size_t)out->first_uv*out->layout.pitch,
        (size_t)out->rows_uv*out->layout.pitch);
    flush_cpu(out->actual,(size_t)out->rows*1920*4);
    return true;
}
int x4_gpu_convert_verification_complete(X4GpuConvert *g,const X4GpuVerifyResult *result)
{
    if(!g||!result||atomic_load_explicit(&g->verify_state,memory_order_acquire)!=VERIFY_CLAIMED||
        result->token!=g->verify_snapshot.token||result->owner_epoch!=g->verify_snapshot.owner_epoch)return X4_PIPELINE_STALE;
    g->verify_result=*result;
    atomic_store_explicit(&g->verify_state,VERIFY_COMPLETE,memory_order_release);
    return 0;
}
bool x4_gpu_convert_verification_collect(X4GpuConvert *g,X4GpuVerifyResult *out)
{
    if(!g||atomic_load_explicit(&g->verify_state,memory_order_acquire)!=VERIFY_COMPLETE)return false;
    X4GpuVerifyResult r=g->verify_result;
    if(out)*out=r;
    ++g->status.verification_checks;
    g->status.verification_last_result=r.result;
    g->status.verification_last_worker_us=r.worker_us;
    g->status.verification_pixels+=r.checked_pixels;
    g->status.verification_outside+=r.pixels_outside;
    if(r.max_channel_difference>g->status.verification_max_difference)
        g->status.verification_max_difference=r.max_channel_difference;
    if(r.result)++g->status.verification_errors;
    if(r.result||r.pixels_outside){
        g->status.verification_failed=true;
        if(g->status.pending&&!g->status.quarantined)g->verify_failure_notice=true;
        disable(g,r.result?"verification_error":"verification_mismatch");
    }
    atomic_store_explicit(&g->verify_state,VERIFY_FREE,memory_order_release);
    return true;
}
bool x4_gpu_convert_verification_cancel(X4GpuConvert *g)
{
    if(!g)return true;
    (void)x4_gpu_convert_verification_collect(g,NULL);
    unsigned state=atomic_load_explicit(&g->verify_state,memory_order_acquire);
    if(state==VERIFY_GPU||state==VERIFY_CLAIMED)return false;
    if(state==VERIFY_READY){
        unsigned expected=VERIFY_READY;
        if(!atomic_compare_exchange_strong_explicit(&g->verify_state,&expected,VERIFY_FREE,
            memory_order_acq_rel,memory_order_acquire))return false;
    }
    g->verify_requested=false;
    return true;
}
static bool timestamp_duration(uint64_t ticks,uint64_t hz,uint64_t *us)
{
    if(!hz)return false;
    double duration=(double)ticks*1000000.0/(double)hz;
    if(duration>50000.0)return false;
    if(us)*us=(uint64_t)(duration+0.5);
    return true;
}
static void timestamp_invalid(X4GpuTimestampStatus *t,TimestampLedger *ledger,bool first)
{
    ++t->invalid;
    if(first)++ledger->first_invalid;
    if(t->samples>=TIMESTAMP_PROBE_SAMPLES &&
        ledger->first_invalid==TIMESTAMP_PROBE_SAMPLES)t->unsupported=true;
}
static void timestamp_observe(X4GpuConvert *g,uint64_t now)
{
    unsigned resolution=g->status.last_resolution;
    X4GpuTimestampStatus *t=&g->status.timestamps[resolution];
    TimestampLedger *ledger=&g->timestamps[resolution];
    flush_cpu((const void *)g->timestamp_start,64);
    flush_cpu((const void *)g->timestamp_end,64);
    uint64_t start=*g->timestamp_start,end=*g->timestamp_end;
    g->status.last_start_ticks=start;g->status.last_end_ticks=end;
    g->status.last_real_valid=false;g->status.last_real_us=0;++t->samples;
    if(!start || !end || end<start){
        timestamp_invalid(t,ledger,t->samples<=TIMESTAMP_PROBE_SAMPLES);
        return;
    }
    if(t->unsupported)return; /* Session latch: raw marks only, never new Hz/us. */
    if(t->first_end_ticks && (end<t->last_end_ticks || now<t->last_observed_us)){
        timestamp_invalid(t,ledger,t->samples<=TIMESTAMP_PROBE_SAMPLES);return;
    }
    if(!t->first_end_ticks){t->first_end_ticks=end;t->first_observed_us=now;}
    t->last_end_ticks=end;t->last_observed_us=now;
    uint64_t span_us=now-t->first_observed_us;
    if(span_us>=TIMESTAMP_CALIBRATION_US && end>t->first_end_ticks){
        /* Endpoint slope over >=10 seconds, against the observed process
         * clock. Observation jitter remains part of this measured estimate.
         * Never substitute an assumed ASIC or shader frequency. */
        double hz=(double)(end-t->first_end_ticks)*1000000.0/(double)span_us;
        if(hz>=1.0 && hz<(double)UINT64_MAX){
            t->clock_hz=(uint64_t)(hz+0.5);t->clock_calibrated=true;
        }
    }
    uint64_t ticks=end-start;
    if(!t->clock_calibrated){
        if(ledger->pending_count<TIMESTAMP_PENDING_CAPACITY){
            ledger->pending_ticks[ledger->pending_count++]=ticks;++t->pending_samples;
            if(t->samples<=TIMESTAMP_PROBE_SAMPLES)++ledger->first_pending_count;
        }else ++t->pending_overflow;
        return;
    }
    /* Backfill validation, not invented zero-us histogram entries. The
     * pipeline's real-us histogram starts with calibrated completions. */
    for(unsigned i=0;i<ledger->pending_count;++i){
        if(timestamp_duration(ledger->pending_ticks[i],t->clock_hz,NULL))++t->valid_samples;
        else timestamp_invalid(t,ledger,i<ledger->first_pending_count);
    }
    ledger->pending_count=0;ledger->first_pending_count=0;t->pending_samples=0;
    if(t->unsupported)return; /* Backfill may have rejected all first 300. */
    uint64_t us=0;
    if(timestamp_duration(ticks,t->clock_hz,&us)){
        ++t->valid_samples;g->status.last_real_us=us;g->status.last_real_valid=true;
    }else timestamp_invalid(t,ledger,t->samples<=TIMESTAMP_PROBE_SAMPLES);
}
int x4_gpu_convert_start(X4GpuConvert *g,const X4GpuConvertFrame *f,uint64_t now)
{
    if(!g)return X4_GPU_CONVERT_CPU;
    (void)x4_gpu_convert_verification_collect(g,NULL);
    if(g->status.disabled)return cpu_fallback(g,g->status.reason,true);
    if(g->status.pending)return cpu_fallback(g,"busy",false);
    if(!g->status.prepared || !f)return cpu_fallback(g,"prepare",true);
    const char *reason;
    if(!x4_gpu_convert_admit(&f->layout,f->format,&f->color,&reason))
        return cpu_fallback(g,reason,false);
    if(f->source_bytes<f->layout.total_nv12_bytes || f->destination_pitch<1920 ||
        f->destination_pitch>8192 ||
        f->destination_bytes<(size_t)f->destination_pitch*1080*4 ||
        !address_ok(f->source,f->layout.total_nv12_bytes) ||
        !address_ok(f->destination,f->destination_bytes))return cpu_fallback(g,"buffer_bounds",false);
    OrbisPthread self=scePthreadSelf();
    if(g->status.owner_bound && g->submitter!=self){
        ++g->status.wrong_thread_submits;return cpu_fallback(g,"submit_thread",true);
    }
    uint32_t quality=x4_gpu_quality_normalize(f->quality_word);
#if X4_PUBLIC
    quality&=X4_GPU_QUALITY_DITHER;
#endif
    bool applicable=f->layout.visible_width==1920&&f->layout.visible_height==1080&&
        f->color.convert_mode==X4_CONVERT_SMOOTH;
    bool enhanced=applicable&&x4_gpu_quality_active(quality,f->color.sharpness);
    bool cas=false;
#if !X4_PUBLIC
    cas=enhanced&&(quality&X4_GPU_QUALITY_CAS)&&f->color.sharpness;
#endif
    g->status.quality_requested_word=quality;g->status.quality_effective_word=0;
    g->status.quality_active=enhanced;g->status.quality_cas=cas;
    g->status.quality_key=enhanced?((quality&15u)|(f->color.sharpness<<4)):0;
    g->status.quality_reason=quality&&!applicable?"mejoras_solo_1080p_suave":enhanced?"pending":"off";
    int allowed=sceGnmAreSubmitsAllowed();
    g->status.allowed_result=allowed;
    if(!allowed){
        if(!g->status.pause_active){
            g->status.pause_active=true;g->status.pause_started_us=now;
            g->status.pause_current_frames=0;++g->status.pause_episodes;
        }
        if(enhanced&&(quality&X4_GPU_QUALITY_REPEAT)){
            ++g->status.pausa_repetidas;
            pause_observe(g,now,false);g->status.reason="submits_paused";
            g->status.quality_reason="pause_repeat";return X4_GPU_CONVERT_REPEAT;
        }
        ++g->status.pause_cpu_frames;++g->status.pause_current_frames;
        pause_observe(g,now,false);
        if(enhanced)g->status.quality_reason="pause_cpu";
        return cpu_fallback(g,"submits_paused",false);
    }
    pause_observe(g,now,true);
    if(allowed<0)return cpu_fallback(g,"submits_error",true);
    if(enhanced&&!quality_prepare(g,cas))return cpu_fallback(g,g->status.quality_reason,false);
    if(g->sequence==UINT32_MAX)return cpu_fallback(g,"sequence_exhausted",true);
    if(!timestamp_address_ok(g,g->timestamp_start) || !timestamp_address_ok(g,g->timestamp_end))
        return cpu_fallback(g,"command_bounds",true);
    if(!g->status.owner_bound){g->submitter=self;g->status.owner_bound=true;}
    ++g->sequence;*g->label=0;*g->timestamp_start=0;*g->timestamp_end=0;parameters(g,f);
    unsigned shader=f->color.convert_mode==X4_CONVERT_ORIGINAL?0u:1u+f->color.sharpness;
    if(f->layout.visible_width==1280)shader+=4;
    uint32_t *dcb=g->dcb;
    uint32_t bytes;
    if(enhanced){
        x4_gpu_quality_parameters((X4GpuQualityParameters *)g->quality_parameters,
            f->layout.pitch,f->destination_pitch,1920,&f->color,quality);
        bytes=quality_commands(g,f,cas);dcb=g->quality_dcb;
    }else bytes=commands(g,f,shader);
    if(!bytes)return cpu_fallback(g,"command_bounds",true);
    g->verify_this_frame=false;
    uint32_t verify_bytes=0;
    if(g->verify_requested){
        uint64_t verify_begin=sceKernelGetProcessTime();
        verify_bytes=verification_commands(g,f,enhanced?quality:0);
        uint64_t verify_end=sceKernelGetProcessTime();
        if(verify_bytes){
            g->verify_this_frame=true;
            g->status.verification_last_enqueue_us=verify_end>=verify_begin?verify_end-verify_begin:0;
            g->status.verification_last_copy_real_valid=false;
        }
        if(g->status.disabled)return cpu_fallback(g,g->status.reason,true);
    }
    /* Decoder outputs use type 0 WB_ONION (live_h264.c:1522), mapped 0x33
     * (:1428-1433). Decode supplies pFrameBuffer (:1764); the CPU does not
     * write NV12 after Decode, so only CPU-written controls need clflush. */
    if(enhanced)flush_cpu(g->quality_parameters,X4_GPU_QUALITY_PARAMETER_BYTES);
    else flush_cpu(g->parameters,64);
    flush_cpu((const void *)g->label,64);
    flush_cpu((const void *)g->timestamp_start,64);flush_cpu((const void *)g->timestamp_end,64);
    flush_cpu(dcb,bytes);_mm_sfence();
    g->status.pending=true;g->status.quarantined=false;g->status.submitted_us=now;
    g->status.last_resolution=f->layout.visible_width==1280?X4_GPU_RESOLUTION_720:X4_GPU_RESOLUTION_1080;
    g->status.last_real_valid=false;
    g->status.quality_effective_word=enhanced?quality:0;
    if(enhanced)g->status.quality_reason="active";
    g->status.reason="no";++g->status.accepted;
    void *commands_to_submit[2]={dcb,g->verify_dcb};uint32_t sizes[2]={bytes,verify_bytes};
    g->status.submit_result=sceGnmSubmitCommandBuffers(verify_bytes?2:1,commands_to_submit,sizes,NULL,NULL);
    g->status.submit_returned_us=sceKernelGetProcessTime();
    g->status.last_prep_us=g->status.submit_returned_us>=now?g->status.submit_returned_us-now:0;
    g->status.last_eop_us=0;
    /* Once the driver was entered, even a nonzero return is conservatively
     * treated as uncertain ownership. Only EOP may retire these resources.
     */
    if(g->status.submit_result)disable(g,"submit_error");
    g->status.submit_done_result=sceGnmSubmitDone();
    if(g->status.submit_done_result && !g->status.submit_result)disable(g,"submit_done_error");
    return X4_GPU_CONVERT_ACCEPTED;
}
X4GpuConvertPoll x4_gpu_convert_poll(X4GpuConvert *g,uint64_t now,uint64_t *elapsed)
{
    if(elapsed)*elapsed=0;
    if(g)(void)x4_gpu_convert_verification_collect(g,NULL);
    if(!g || !g->status.pending)return X4_GPU_CONVERT_IDLE;
    if(g->verify_failure_notice){
        g->verify_failure_notice=false;g->status.quarantined=true;++g->status.quarantines;
        return X4_GPU_CONVERT_VERIFY_QUARANTINE;
    }
    flush_cpu((const void *)g->label,64);
    if(g->verify_this_frame)flush_cpu((const void *)g->verify_label,64);
    uint64_t duration=now>=g->status.submitted_us?now-g->status.submitted_us:0;
    if(*g->label==g->sequence && (!g->verify_this_frame || *g->verify_label==g->sequence)){
        _mm_mfence();bool late=g->status.quarantined;
        /* An already-completed label first observed beyond the limit is also
         * a late result, and disables GPU exactly like an unobserved timeout.
         */
        if(!late && duration>=X4_GPU_CONVERT_TIMEOUT_US){
            ++g->status.timeouts;++g->status.quarantines;late=true;disable(g,"eop_timeout");
        }
        g->status.pending=false;g->status.quarantined=false;
        g->status.last_elapsed_us=duration;++g->status.completed;
        g->status.last_eop_us=now>=g->status.submit_returned_us?now-g->status.submit_returned_us:0;
        timestamp_observe(g,now);
        if(g->verify_this_frame){
            ++g->status.verification_copy_completed;
            flush_cpu((const void *)g->verify_start,64);flush_cpu((const void *)g->verify_end,64);
            const X4GpuTimestampStatus *timing=&g->status.timestamps[g->status.last_resolution];
            uint64_t first=*g->verify_start,last=*g->verify_end,us=0;
            if(timing->clock_calibrated&&!timing->unsupported&&first&&last>=first&&
                timestamp_duration(last-first,timing->clock_hz,&us)){
                g->status.verification_last_copy_real_us=us;
                g->status.verification_last_copy_real_valid=true;
            }
            atomic_store_explicit(&g->verify_state,late?VERIFY_FREE:VERIFY_READY,memory_order_release);
            g->verify_this_frame=false;
        }
        if(late)++g->status.late_completed;
        if(elapsed)*elapsed=duration;
        return late?X4_GPU_CONVERT_LATE_DONE:X4_GPU_CONVERT_DONE;
    }
    if(!g->status.quarantined && duration>=X4_GPU_CONVERT_TIMEOUT_US){
        g->status.quarantined=true;++g->status.timeouts;++g->status.quarantines;
        disable(g,"eop_timeout");if(elapsed)*elapsed=duration;
        return X4_GPU_CONVERT_TIMEOUT;
    }
    return X4_GPU_CONVERT_WAIT;
}
bool x4_gpu_convert_label_ready(X4GpuConvert *g)
{
    if(!g || !g->status.pending || !g->status.owner_bound ||
        g->submitter!=scePthreadSelf())return false;
    flush_cpu((const void *)g->label,64);
    if(g->verify_this_frame)flush_cpu((const void *)g->verify_label,64);
    return *g->label==g->sequence && (!g->verify_this_frame || *g->verify_label==g->sequence);
}
bool x4_gpu_convert_pending(const X4GpuConvert *g)
{return g&&g->status.pending;}
bool x4_gpu_convert_disabled(const X4GpuConvert *g)
{return !g||g->status.disabled;}
const char *x4_gpu_convert_reason(const X4GpuConvert *g)
{return g?g->status.reason:"prepare_heap";}
void x4_gpu_convert_status(const X4GpuConvert *g,X4GpuConvertStatus *out)
{
    if(out)*out=g?g->status:(X4GpuConvertStatus){.disabled=true,.reason="prepare_heap",
        .reserve_result=X4_GPU_CONVERT_NOT_RUN,.map_result=X4_GPU_CONVERT_NOT_RUN,
        .submit_result=X4_GPU_CONVERT_NOT_RUN,.submit_done_result=X4_GPU_CONVERT_NOT_RUN,
        .unmap_result=X4_GPU_CONVERT_NOT_RUN,.release_result=X4_GPU_CONVERT_NOT_RUN,
        .allowed_result=X4_GPU_CONVERT_NOT_RUN,.quality_reason="off",
        .quality_reserve_result=X4_GPU_CONVERT_NOT_RUN,.quality_map_result=X4_GPU_CONVERT_NOT_RUN,
        .quality_unmap_result=X4_GPU_CONVERT_NOT_RUN,.quality_release_result=X4_GPU_CONVERT_NOT_RUN,
        .intermediate_reserve_result=X4_GPU_CONVERT_NOT_RUN,.intermediate_map_result=X4_GPU_CONVERT_NOT_RUN,
        .intermediate_unmap_result=X4_GPU_CONVERT_NOT_RUN,.intermediate_release_result=X4_GPU_CONVERT_NOT_RUN,
        .verification_reserve_result=X4_GPU_CONVERT_NOT_RUN,.verification_map_result=X4_GPU_CONVERT_NOT_RUN,
        .verification_unmap_result=X4_GPU_CONVERT_NOT_RUN,.verification_release_result=X4_GPU_CONVERT_NOT_RUN,
        .verification_control_reserve_result=X4_GPU_CONVERT_NOT_RUN,.verification_control_map_result=X4_GPU_CONVERT_NOT_RUN,
        .verification_control_unmap_result=X4_GPU_CONVERT_NOT_RUN,.verification_control_release_result=X4_GPU_CONVERT_NOT_RUN,
        .verification_last_result=X4_GPU_CONVERT_NOT_RUN};
    if(out&&g)out->verification_pending=atomic_load_explicit(&g->verify_state,memory_order_acquire)!=VERIFY_FREE;
}
bool x4_gpu_convert_close(X4GpuConvert **state,uint64_t now)
{
    if(!state || !*state)return true;
    X4GpuConvert *g=*state;
    pause_observe(g,now,true);
    (void)x4_gpu_convert_poll(g,now,NULL);
    if(g->status.pending)return false;
    if(!x4_gpu_convert_verification_cancel(g))return false;
    if(g->verify_pixels_mapped){
        g->status.verification_unmap_result=sceKernelMunmap(g->verify_pixels,X4_GPU_VERIFY_NATIVE_BYTES);
        if(g->status.verification_unmap_result)return false;
        g->verify_pixels_mapped=false;g->verify_pixels=NULL;
    }
    if(g->verify_pixels_allocated){
        g->status.verification_release_result=sceKernelReleaseDirectMemory(g->verify_pixels_physical,X4_GPU_VERIFY_NATIVE_BYTES);
        if(g->status.verification_release_result)return false;
        g->verify_pixels_allocated=false;
    }
    if(g->verify_control_mapped){
        g->status.verification_control_unmap_result=sceKernelMunmap(g->verify_control,X4_GPU_VERIFY_CONTROL_BYTES);
        if(g->status.verification_control_unmap_result)return false;
        g->verify_control_mapped=false;g->verify_control=NULL;
    }
    if(g->verify_control_allocated){
        g->status.verification_control_release_result=sceKernelReleaseDirectMemory(g->verify_control_physical,X4_GPU_VERIFY_CONTROL_BYTES);
        if(g->status.verification_control_release_result)return false;
        g->verify_control_allocated=false;
    }
    /* The intermediate and quality controls share the accepted job's exact
     * EOP lifetime, including submit errors, timeout and late completion. */
    if(g->intermediate_mapped){
        g->status.intermediate_unmap_result=sceKernelMunmap(g->intermediate,X4_GPU_CONVERT_QUALITY_INTERMEDIATE_NATIVE_BYTES);
        if(g->status.intermediate_unmap_result)return false;
        g->intermediate_mapped=false;g->intermediate=NULL;
    }
    if(g->intermediate_allocated){
        g->status.intermediate_release_result=sceKernelReleaseDirectMemory(g->intermediate_physical,X4_GPU_CONVERT_QUALITY_INTERMEDIATE_NATIVE_BYTES);
        if(g->status.intermediate_release_result)return false;
        g->intermediate_allocated=false;
    }
    if(g->quality_mapped){
        g->status.quality_unmap_result=sceKernelMunmap(g->quality_arena,X4_GPU_CONVERT_QUALITY_ARENA_BYTES);
        if(g->status.quality_unmap_result)return false;
        g->quality_mapped=false;g->quality_arena=NULL;
    }
    if(g->quality_allocated){
        g->status.quality_release_result=sceKernelReleaseDirectMemory(g->quality_physical,X4_GPU_CONVERT_QUALITY_ARENA_BYTES);
        if(g->status.quality_release_result)return false;
        g->quality_allocated=false;
    }
    if(g->mapped){
        g->status.unmap_result=sceKernelMunmap(g->arena,X4_GPU_CONVERT_ARENA_BYTES);
        if(g->status.unmap_result)return false;
        g->mapped=false;g->arena=NULL;
    }
    if(g->allocated){
        g->status.release_result=sceKernelReleaseDirectMemory(g->physical,X4_GPU_CONVERT_ARENA_BYTES);
        if(g->status.release_result)return false;
        g->allocated=false;
    }
    free(g);*state=NULL;return true;
}
