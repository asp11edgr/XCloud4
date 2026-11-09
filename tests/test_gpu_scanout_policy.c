/* SPDX-License-Identifier: GPL-3.0-only */
/* Real pure pipeline policy + compact CPU verification rows. The EOP owner
 * below is simulated: this does not execute a shader, driver or console. */
#include "../src/video/video_pipeline.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do{++checks;if(!(value)){fprintf(stderr,"scanout check %d: %s\n",__LINE__,#value);abort();}}while(0)
static bool same_lease(X4VideoPipelineLease a,X4VideoPipelineLease b)
{return a.slot==b.slot&&a.epoch==b.epoch&&a.generation==b.generation;}
static X4VideoPipelineDisplayOccupancy occupancy(X4VideoPipelineDisplay *d)
{
    X4VideoPipelineDisplayOccupancy o;
    CHECK(!x4_video_pipeline_display_occupancy(d,&o));
    CHECK(o.in_flight==o.writing+o.ready+o.pending&&o.in_flight<=1&&o.screen<=1);
    CHECK(o.free+o.writing+o.ready+o.pending+o.screen
#if !X4_PUBLIC
        +o.quarantined
#endif
        ==X4_PIPELINE_SCANOUTS);
    return o;
}
static void present_cpu(X4VideoPipelineDisplay *d,uint64_t id,unsigned excluded)
{
    X4VideoPipelineLease writing,ready,shown,none;
    int old_screen=d->screen;
    CHECK(!x4_video_pipeline_acquire_scanout(d,&writing));
    CHECK(writing.slot!=excluded&&(old_screen<0||writing.slot!=(unsigned)old_screen));
    CHECK(x4_video_pipeline_acquire_scanout(d,&none)==X4_PIPELINE_WAIT&&!none.generation);
    X4VideoPipelineDisplayOccupancy o=occupancy(d);CHECK(o.writing==1&&!o.ready&&!o.pending);
    CHECK(!x4_video_pipeline_finish_scanout(d,writing,id,id*100,id*100+10));
    CHECK(!x4_video_pipeline_ready_scanout(d,&ready)&&same_lease(ready,writing));
    CHECK(x4_video_pipeline_acquire_scanout(d,&none)==X4_PIPELINE_WAIT);
    CHECK(!x4_video_pipeline_mark_submitted(d,ready,id,id*100+11));
    CHECK(x4_video_pipeline_ready_scanout(d,&none)==X4_PIPELINE_WAIT);
    CHECK(x4_video_pipeline_acquire_scanout(d,&none)==X4_PIPELINE_WAIT);
    CHECK(x4_video_pipeline_observe_flip(d,id+10000,id*100+12,&none)==X4_PIPELINE_WAIT);
    CHECK(d->screen==old_screen&&d->pending==(int)writing.slot);
    CHECK(!x4_video_pipeline_observe_flip(d,id,id*100+13,&shown)&&same_lease(shown,writing));
    CHECK(d->screen==(int)writing.slot&&d->pending==-1);
    if(old_screen>=0)CHECK(d->slots[old_screen].state==X4_SCANOUT_FREE);
    o=occupancy(d);CHECK(o.screen==1&&!o.in_flight);
}
static void cpu_default(void)
{
    X4VideoPipelineDisplay d;CHECK(!x4_video_pipeline_display_init(&d,7));
    for(uint64_t frame=1;frame<=100;++frame)present_cpu(&d,frame,UINT_MAX);
    CHECK(d.submitted==100&&d.observed==100&&d.peak_in_flight==1&&d.peak_occupied==2);
    X4VideoPipelineDisplayOccupancy o=occupancy(&d);CHECK(o.free==2&&o.screen==1);
#if !X4_PUBLIC
    CHECK(!o.quarantined);
#endif
    /* Caller models successful VideoOutClose, after CPU owners have joined. */
    CHECK(!x4_video_pipeline_display_closed(&d));o=occupancy(&d);
    CHECK(o.free==3&&!o.occupied&&!o.in_flight&&d.screen==-1&&d.pending==-1);
}

#if !X4_PUBLIC
static int retire_after_exact_eop(X4VideoPipelineDisplay *d,X4VideoPipelineLease lease,
    volatile const uint64_t *label,uint64_t sequence)
{
    /* EOP proof belongs to the driver owner, not to the pure display model. */
    if(*label!=sequence)return X4_PIPELINE_WAIT;
    return x4_video_pipeline_retire_quarantine(d,lease);
}
static void quarantine_and_cpu_fallback(void)
{
    X4VideoPipelineDisplay d;CHECK(!x4_video_pipeline_display_init(&d,11));
    present_cpu(&d,1,UINT_MAX);
    X4VideoPipelineLease gpu;CHECK(!x4_video_pipeline_acquire_scanout(&d,&gpu));
    CHECK(!x4_video_pipeline_quarantine_scanout(&d,gpu));
    X4VideoPipelineDisplayOccupancy o=occupancy(&d);
    CHECK(o.quarantined==1&&o.screen==1&&o.free==1&&!o.in_flight&&o.occupied==2);
    CHECK(x4_video_pipeline_abort_scanout(&d,gpu)==X4_PIPELINE_STATE);
    CHECK(x4_video_pipeline_finish_scanout(&d,gpu,2,200,210)==X4_PIPELINE_STATE);
    CHECK(x4_video_pipeline_mark_submitted(&d,gpu,2,211)==X4_PIPELINE_STATE);
    CHECK(x4_video_pipeline_quarantine_scanout(&d,gpu)==X4_PIPELINE_STATE);
    CHECK(!x4_video_pipeline_cancel_ready_scanout(&d));
    X4VideoPipelineLease none;CHECK(x4_video_pipeline_ready_scanout(&d,&none)==X4_PIPELINE_WAIT);
    CHECK(x4_video_pipeline_display_closed(&d)==X4_PIPELINE_STATE);
    CHECK(d.slots[gpu.slot].state==X4_SCANOUT_QUARANTINED&&d.screen>=0);

    X4VideoPipelineRing ring;CHECK(!x4_video_pipeline_ring_init_outputs(&ring,11,5));
    X4VideoPipelineLease held[2],offered,image;uint64_t discard;
    for(unsigned i=0;i<2;++i)CHECK(!x4_video_pipeline_acquire_decode(&ring,&held[i],&discard));
    CHECK(!x4_video_pipeline_acquire_decode(&ring,&offered,&discard));
    CHECK(!x4_video_pipeline_publish_ready(&ring,offered,3,300));
    CHECK(!x4_video_pipeline_take_ready(&ring,&image,NULL)&&same_lease(image,offered));
    unsigned used=0;
    for(uint64_t frame=4;frame<=103;++frame){
        X4VideoPipelineLease cpu;
        CHECK(!x4_video_pipeline_acquire_decode(&ring,&cpu,&discard));
        CHECK(cpu.slot!=image.slot&&cpu.slot!=held[0].slot&&cpu.slot!=held[1].slot);
        CHECK(!x4_video_pipeline_publish_ready(&ring,cpu,frame,frame*100));
        X4VideoPipelineLease taken;CHECK(!x4_video_pipeline_take_ready(&ring,&taken,NULL)&&same_lease(cpu,taken));
        present_cpu(&d,frame,gpu.slot);used|=1u<<(unsigned)d.screen;
        CHECK(!x4_video_pipeline_release_conversion(&ring,taken));
        CHECK(ring.slots[image.slot].state==X4_PIPELINE_CONVERTING&&
            ring.slots[image.slot].generation==image.generation);
        for(unsigned i=0;i<2;++i)CHECK(ring.slots[held[i].slot].state==X4_PIPELINE_DECODING&&
            ring.slots[held[i].slot].generation==held[i].generation);
        o=occupancy(&d);CHECK(o.quarantined==1&&o.screen==1&&!o.in_flight);
    }
    CHECK(used==(((1u<<X4_PIPELINE_SCANOUTS)-1u)&~(1u<<gpu.slot)));
    CHECK(d.peak_in_flight==1&&d.peak_occupied==3);
    X4VideoPipelineLease cancelled;CHECK(!x4_video_pipeline_acquire_scanout(&d,&cancelled));
    CHECK(cancelled.slot!=gpu.slot);
    CHECK(!x4_video_pipeline_finish_scanout(&d,cancelled,104,10400,10410));
    CHECK(x4_video_pipeline_cancel_ready_scanout(&d)==1&&d.slots[gpu.slot].state==X4_SCANOUT_QUARANTINED);
    CHECK(x4_video_pipeline_display_closed(&d)==X4_PIPELINE_STATE);

    volatile uint64_t eop=0;const uint64_t sequence=42;
    CHECK(retire_after_exact_eop(&d,gpu,&eop,sequence)==X4_PIPELINE_WAIT);
    eop=sequence-1;CHECK(retire_after_exact_eop(&d,gpu,&eop,sequence)==X4_PIPELINE_WAIT);
    CHECK(ring.slots[image.slot].state==X4_PIPELINE_CONVERTING&&!x4_video_pipeline_ring_idle(&ring));
    eop=sequence;CHECK(!retire_after_exact_eop(&d,gpu,&eop,sequence));
    CHECK(!x4_video_pipeline_release_conversion(&ring,image));
    /* Native destruction succeeds only after the GPU reader has retired. */
    for(unsigned i=0;i<2;++i)CHECK(!x4_video_pipeline_abort_decode(&ring,held[i]));
    CHECK(x4_video_pipeline_ring_idle(&ring));
    CHECK(!x4_video_pipeline_display_closed(&d));o=occupancy(&d);
    CHECK(o.free==3&&!o.quarantined&&!o.occupied);
}
static void stale_and_cancel(void)
{
    X4VideoPipelineDisplay d;CHECK(!x4_video_pipeline_display_init(&d,71));
    X4VideoPipelineLease old,fresh;
    CHECK(!x4_video_pipeline_acquire_scanout(&d,&old));
    CHECK(!x4_video_pipeline_quarantine_scanout(&d,old));
    volatile uint64_t eop=77;CHECK(!retire_after_exact_eop(&d,old,&eop,77));
    CHECK(!x4_video_pipeline_acquire_scanout(&d,&fresh));
    CHECK(old.slot==fresh.slot&&old.generation!=fresh.generation);
    CHECK(x4_video_pipeline_quarantine_scanout(&d,old)==X4_PIPELINE_STALE);
    CHECK(x4_video_pipeline_retire_quarantine(&d,old)==X4_PIPELINE_STALE);
    CHECK(x4_video_pipeline_abort_scanout(&d,old)==X4_PIPELINE_STALE);
    CHECK(x4_video_pipeline_finish_scanout(&d,old,1,1,2)==X4_PIPELINE_STALE);
    CHECK(d.slots[fresh.slot].state==X4_SCANOUT_WRITING);
    CHECK(x4_video_pipeline_display_closed(&d)==X4_PIPELINE_STATE);
    CHECK(!x4_video_pipeline_cancel_ready_scanout(&d));
    CHECK(!x4_video_pipeline_abort_scanout(&d,fresh));
    CHECK(!x4_video_pipeline_display_closed(&d));
    CHECK(!x4_video_pipeline_display_init(&d,72));
    CHECK(!x4_video_pipeline_acquire_scanout(&d,&fresh));
    CHECK(x4_video_pipeline_quarantine_scanout(&d,old)==X4_PIPELINE_STALE);
    CHECK(!x4_video_pipeline_quarantine_scanout(&d,fresh));
    CHECK(retire_after_exact_eop(&d,old,&eop,77)==X4_PIPELINE_STALE);
    CHECK(d.slots[fresh.slot].state==X4_SCANOUT_QUARANTINED);
    CHECK(!retire_after_exact_eop(&d,fresh,&eop,77));
    CHECK(!x4_video_pipeline_display_closed(&d));
}

static X4VideoNv12Layout layout(unsigned pitch)
{
    const size_t y=(size_t)pitch*1088,pixels=1920u*1080u;
    return (X4VideoNv12Layout){.visible_width=1920,.visible_height=1080,
        .coded_width=1920,.coded_height=1088,.storage_height=1088,.pitch=pitch,
        .y_bytes=y,.uv_bytes=y/2,.uv_plane_offset=y,.total_nv12_bytes=y+y/2,
        .visible_y_offset=0,.visible_uv_offset=y,.rgb_pixels=pixels,.rgb_bytes=pixels*4};
}
static uint64_t hash_bytes(const uint8_t *bytes,size_t count)
{
    uint64_t value=UINT64_C(1469598103934665603);
    for(size_t i=0;i<count;++i)value=(value^bytes[i])*UINT64_C(1099511628211);
    return value;
}
static void fixture(uint8_t *source,const X4VideoNv12Layout *l,unsigned pattern)
{
    memset(source,0x5a,l->total_nv12_bytes);
    for(unsigned y=0;y<1088;++y)for(unsigned x=0;x<1920;++x){
        unsigned value=pattern==0?16u+(x*219u/1919u):pattern==1?(((x/11u+y/7u)&1u)?235u:16u):
            ((x*73u+y*199u+(x*y>>3))&255u);
        source[(size_t)y*l->pitch+x]=(uint8_t)(y<1080?value:((x+y)&1u?0xff:0));
    }
    for(unsigned y=0;y<544;++y)for(unsigned x=0;x<1920;x+=2){
        size_t at=l->uv_plane_offset+(size_t)y*l->pitch+x;
        source[at]=(uint8_t)(pattern==0?128u:pattern==1?((x/48u)&1u?16u:240u):(x*17u+y*61u));
        source[at+1]=(uint8_t)(pattern==0?128u:pattern==1?((y/9u)&1u?240u:16u):(x*43u+y*11u));
    }
}
static void guard_u32(const uint32_t *owned,size_t pixels)
{for(unsigned i=0;i<16;++i)CHECK(owned[i]==0xc0decafeu&&owned[pixels+16+i]==0xc0decafeu);}
static void invalid_rows(const X4VideoNv12Layout *l,uint8_t *source,uint32_t *scratch,size_t bytes)
{
    X4VideoPipelineColor color={.matrix_coefficients=1,.convert_mode=X4_CONVERT_SMOOTH,.sharpness=2};
    for(size_t i=0;i<bytes/4;++i)scratch[i]=0x6b6b6b6bu;
#define INVALID(layout_,source_,srcbytes_,dst_,dstbytes_,first_,rows_,color_) \
    CHECK(x4_video_pipeline_gpu_reference_rows((layout_),(source_),(srcbytes_),(dst_),(dstbytes_),(first_),(rows_),(color_))==X4_PIPELINE_INVALID)
    INVALID(NULL,source,l->total_nv12_bytes,scratch,bytes,0,64,&color);
    INVALID(l,NULL,l->total_nv12_bytes,scratch,bytes,0,64,&color);
    INVALID(l,source,l->total_nv12_bytes,NULL,bytes,0,64,&color);
    INVALID(l,source,l->total_nv12_bytes,scratch,bytes,0,64,NULL);
    INVALID(l,source,l->total_nv12_bytes,scratch,bytes,0,0,&color);
    INVALID(l,source,l->total_nv12_bytes,scratch,bytes,0,65,&color);
    INVALID(l,source,l->total_nv12_bytes,scratch,bytes,1080,1,&color);
    INVALID(l,source,l->total_nv12_bytes,scratch,bytes,UINT_MAX,64,&color);
    INVALID(l,source,l->total_nv12_bytes,scratch,bytes,1017,64,&color);
    INVALID(l,source,l->total_nv12_bytes-1,scratch,bytes,0,64,&color);
    INVALID(l,source,l->total_nv12_bytes,scratch,bytes-1,0,64,&color);
    INVALID(l,source,l->total_nv12_bytes,(uint32_t *)source,bytes,0,64,&color);
    INVALID(l,source,l->total_nv12_bytes,(uint32_t *)((uint8_t *)scratch+1),bytes-1,0,64,&color);
    X4VideoNv12Layout bad=*l;bad.visible_y_offset=l->pitch;
    INVALID(&bad,source,l->total_nv12_bytes,scratch,bytes,0,64,&color);
    bad=*l;bad.visible_width=1280;bad.rgb_pixels=1280u*1080;bad.rgb_bytes=bad.rgb_pixels*4;
    INVALID(&bad,source,l->total_nv12_bytes,scratch,bytes,0,64,&color);
    for(size_t i=0;i<bytes/4;++i)CHECK(scratch[i]==0x6b6b6b6bu);
#undef INVALID
}
static void reference_rows(void)
{
    const size_t full_pixels=1920u*1080u,slice_pixels=1920u*64u;
    const unsigned starts[]={0,509,1016};
    uint64_t compared=0;
    for(unsigned padded=0;padded<2;++padded){
        X4VideoNv12Layout l=layout(padded?2048u:1920u);
        uint8_t *source_owned=malloc(l.total_nv12_bytes+128);CHECK(source_owned);
        memset(source_owned,0xa5,l.total_nv12_bytes+128);uint8_t *source=source_owned+64;
        uint32_t *full_owned=malloc((full_pixels+32)*4),*slice_owned=malloc((slice_pixels+32)*4);
        CHECK(full_owned&&slice_owned);uint32_t *full=full_owned+16,*slice=slice_owned+16;
        for(unsigned i=0;i<16;++i){full_owned[i]=full_owned[full_pixels+16+i]=0xc0decafeu;
            slice_owned[i]=slice_owned[slice_pixels+16+i]=0xc0decafeu;}
        for(unsigned pattern=0;pattern<3;++pattern){
            fixture(source,&l,pattern);uint64_t source_hash=hash_bytes(source,l.total_nv12_bytes);
            for(unsigned choice=0;choice<4;++choice)for(unsigned color_case=0;color_case<2;++color_case){
                X4VideoPipelineColor color={.matrix_coefficients=color_case?6u:1u,.full_range=color_case!=0,
                    .convert_mode=choice?X4_CONVERT_SMOOTH:X4_CONVERT_ORIGINAL,.sharpness=(unsigned char)(choice?choice-1:0)};
                for(unsigned worker=0;worker<2;++worker)
                    CHECK(!x4_video_pipeline_convert_stripe_color(&l,source,l.total_nv12_bytes,full,full_pixels*4,1920,2,worker,&color));
                for(unsigned window=0;window<3;++window){
                    memset(slice,0x6b,slice_pixels*4);
                    CHECK(!x4_video_pipeline_gpu_reference_rows(&l,source,l.total_nv12_bytes,slice,slice_pixels*4,starts[window],64,&color));
                    CHECK(!memcmp(slice,full+(size_t)starts[window]*1920,slice_pixels*4));
                    compared+=slice_pixels;guard_u32(full_owned,full_pixels);guard_u32(slice_owned,slice_pixels);
                }
                memset(slice,0x6b,slice_pixels*4);
                CHECK(!x4_video_pipeline_gpu_reference_rows(&l,source,l.total_nv12_bytes,slice,slice_pixels*4,1079,1,&color));
                CHECK(!memcmp(slice,full+(size_t)1079*1920,1920u*4));
                for(size_t i=1920;i<slice_pixels;++i)CHECK(slice[i]==0x6b6b6b6bu);
            }
            CHECK(hash_bytes(source,l.total_nv12_bytes)==source_hash);
        }
        uint64_t before_invalid=hash_bytes(source,l.total_nv12_bytes);
        invalid_rows(&l,source,slice,slice_pixels*4);
        CHECK(hash_bytes(source,l.total_nv12_bytes)==before_invalid);
        for(unsigned i=0;i<64;++i)CHECK(source_owned[i]==0xa5&&source_owned[l.total_nv12_bytes+64+i]==0xa5);
        guard_u32(full_owned,full_pixels);guard_u32(slice_owned,slice_pixels);
        free(full_owned);free(slice_owned);free(source_owned);
    }
    printf("Reference rows: %llu pixels byte-exact vs real full CPU converter; Original/Smooth0/1/2, top/middle/bottom, tight/padded pitch\n",(unsigned long long)compared);
}
#endif
int main(void)
{
    cpu_default();
#if !X4_PUBLIC
    quarantine_and_cpu_fallback();stale_and_cancel();reference_rows();
#endif
    printf("PASS scanout policy: %u checks; CPU defaults, quarantine/late EOP/cancel/close/generation and compact references; host only\n",checks);
    return 0;
}
