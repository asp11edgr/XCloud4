/* SPDX-License-Identifier: GPL-3.0-only */
/* Actual Lab converter with simulated native ABI. This never executes a GPU. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void record_flush(const void *address,size_t bytes);
#define X4_GPU_CONVERT_TEST_FLUSH(address,bytes) record_flush(address,bytes)
#include "../src/video/gpu_convert.c"

static int reserve_rc,map_rc,submit_rc,done_rc,unmap_rc,release_rc,allowed=1;
static unsigned reserves,maps,submits,done_calls,unmaps,releases,allowed_calls,clock_calls;
static uint64_t mock_now,mock_prep_us,mock_done_us;
static struct {const void *address;size_t bytes;} flushes[256];
static unsigned flush_count;
static uint32_t submitted_dcb_bytes;
static uintptr_t thread_cookie=1;
static bool inject_label;
static void *control;
static struct {off_t physical;size_t bytes;void *pointer;bool reserved;} native_memory[6];
static unsigned native_memory_count,reserve_fail_at,map_fail_at;
static volatile uint64_t *submitted_label;
static volatile uint64_t *submitted_timestamp_start,*submitted_timestamp_end;
static uintptr_t submitted_code;
static uint32_t submitted_sequence;
static X4GpuConvertFrame frame;
static X4GpuConvert *submitted_converter;
static X4GpuConvertFrame submitted_frame;
static uint8_t *source;static uint32_t *destination;
#include "gpu_convert_r3_packets.h"

static void record_flush(const void *p,size_t bytes)
{
    assert(bytes && flush_count<sizeof(flushes)/sizeof(flushes[0]));
    uintptr_t at=(uintptr_t)p,src=(uintptr_t)source;
    assert(!source || at+bytes<=src || at>=src+frame.layout.total_nv12_bytes);
    flushes[flush_count].address=p;flushes[flush_count++].bytes=bytes;
}
static bool was_flushed(const void *p,size_t bytes)
{
    for(unsigned i=0;i<flush_count;++i)
        if(flushes[i].address==p && flushes[i].bytes==bytes)return true;
    return false;
}
static int start(X4GpuConvert *g,const X4GpuConvertFrame *f,uint64_t now)
{mock_now=now;submitted_converter=g;if(f)submitted_frame=*f;return x4_gpu_convert_start(g,f,now);}
uint64_t sceKernelGetProcessTime(void){++clock_calls;return mock_now;}


static uint64_t address(const uint32_t *d)
{return d[0]|((uint64_t)(d[1]&0xffffu)<<32);}
static void inspect_quality_commands(const uint32_t *d,size_t bytes)
{
    X4GpuConvert *g=submitted_converter;const X4GpuConvertFrame *f=&submitted_frame;
    bool cas=(f->quality_word&X4_GPU_QUALITY_CAS)!=0 && f->color.sharpness!=0;
    unsigned expected_dispatches=cas?2u:1u,dispatches=0,user_blocks=0,marks=0;
    bool flush=false,post_acquire=false,barrier=false,release=false;
    size_t cursor=0,words=bytes/4;
    assert(g && d==g->quality_dcb && g->quality_arena && g->quality_mapped);
    assert((uintptr_t)d>=(uintptr_t)g->quality_arena && bytes<=262144-(size_t)((const uint8_t *)d-g->quality_arena));
    uintptr_t active_code=0;
    while(cursor<words){
        uint32_t head=d[cursor++];assert((head>>30)==3);
        unsigned n=((head>>16)&0x3fffu)+1,op=(head>>8)&255u;
        assert(n<=words-cursor);const uint32_t *p=d+cursor;
        if(op==0x76 && p[0]==0x20c){
            assert(n==3);active_code=((uintptr_t)p[1]<<8)|((uintptr_t)p[2]<<40);submitted_code=active_code;
        }else if(op==0x76 && p[0]==0x240){
            assert(n==17 && user_blocks==dispatches);++user_blocks;
            assert(address(p+13)==(uintptr_t)g->quality_parameters && p[15]==X4_GPU_QUALITY_DATA_BYTES);
            const X4GpuQualityParameters *parameters=(const X4GpuQualityParameters *)g->quality_parameters;
            assert(parameters->source_pitch==f->layout.pitch && parameters->destination_pitch==f->destination_pitch);
            assert(!memcmp(parameters,g->parameters,32));
            assert(parameters->quality_word==x4_gpu_quality_normalize(f->quality_word) && parameters->sharpness==f->color.sharpness);
            assert(parameters->intermediate_pitch==1920);
            assert(!memcmp((const uint8_t *)parameters+64,x4_gpu_quality_noise_ranks(),8192));
            if(!dispatches){
                assert(address(p+1)==(uintptr_t)f->source && p[3]==f->layout.y_bytes);
                assert(address(p+5)==(uintptr_t)(f->source+f->layout.uv_plane_offset) && p[7]==f->layout.uv_bytes);
                assert(address(p+9)==(cas?(uintptr_t)g->intermediate:(uintptr_t)f->destination));
                assert(p[11]==(cas?X4_GPU_QUALITY_INTERMEDIATE_BYTES:f->destination_bytes));
            }else{
                assert(cas && address(p+1)==(uintptr_t)g->intermediate && p[3]==X4_GPU_QUALITY_INTERMEDIATE_BYTES);
                assert(address(p+5)==(uintptr_t)((const uint8_t *)parameters+64) && p[7]==8192);
                assert(address(p+9)==(uintptr_t)f->destination && p[11]==f->destination_bytes);
            }
        }else if(op==0x15){
            assert(n==4 && p[0]==240 && p[1]==135 && p[2]==1 && p[3]==1 && marks==1);
            assert(user_blocks==dispatches+1 && dispatches<expected_dispatches);
            unsigned shader=cas?(dispatches?2u:1u):0u;
            assert(active_code==(uintptr_t)g->quality_code[shader]);
            if(dispatches)assert(barrier);
            ++dispatches;flush=post_acquire=false;
        }else if(op==0x46){
            assert(n==1 && p[0]==0x407 && dispatches);flush=true;
        }else if(op==0x58){
            assert(n==6);
            if(dispatches && flush){
                assert((p[0]&0x00c00000u)==0x00c00000u);post_acquire=true;
                if(dispatches<expected_dispatches){assert(p[0]==0x80c00000u);barrier=true;}
            }
        }else if(op==0x47){
            assert(n==5 && p[0]==0x528);
            volatile uint64_t *target=(volatile uint64_t *)(uintptr_t)(p[1]|((uint64_t)(p[2]&0xffffu)<<32));
            assert(!((uintptr_t)target&7u) && (uintptr_t)target>=(uintptr_t)control && (uintptr_t)target<=(uintptr_t)control+65536-8);
            if((p[2]&0xffff0000u)==0x60000000u){
                assert(p[3]==0 && p[4]==0);
                if(!marks){assert(!dispatches);submitted_timestamp_start=target;}
                else {assert(marks==1 && dispatches==expected_dispatches && flush && post_acquire);submitted_timestamp_end=target;}
                ++marks;
            }else{
                assert(p[2]==(0x40000000u|((uint32_t)((uintptr_t)target>>32)&0xffffu)));
                assert(dispatches==expected_dispatches && flush && post_acquire && marks==2 && cursor+n==words);
                submitted_label=target;submitted_sequence=p[3];assert(p[4]==0 && submitted_sequence);release=true;
            }
        }else assert(op==0x76);
        cursor+=n;
    }
    assert(cursor==words && release && marks==2 && dispatches==expected_dispatches && user_blocks==expected_dispatches);
    assert(submitted_timestamp_start==g->timestamp_start && submitted_timestamp_end==g->timestamp_end && submitted_label==g->label);
    assert(submitted_timestamp_start!=submitted_timestamp_end && submitted_label!=submitted_timestamp_start && submitted_label!=submitted_timestamp_end);
}
static void inspect_commands(const uint32_t *d,size_t bytes)
{
    if(submitted_converter && d==submitted_converter->quality_dcb){inspect_quality_commands(d,bytes);return;}
    size_t cursor=0,words=bytes/4;bool dispatch=false,flush=false,release=false,post_acquire=false;
    unsigned user_blocks=0,marks=0,previous_op=0;
    while(cursor<words){
        uint32_t head=d[cursor++];assert((head>>30)==3);
        unsigned n=((head>>16)&0x3fffu)+1,op=(head>>8)&255u;
        assert(n<=words-cursor);const uint32_t *p=d+cursor;
        if(op==0x76 && p[0]==0x240){
            assert(n==17);++user_blocks;
            assert(address(p+1)==(uintptr_t)source);
            assert(address(p+5)==(uintptr_t)(source+frame.layout.uv_plane_offset));
            assert(address(p+9)==(uintptr_t)destination);
            assert(p[3]==frame.layout.y_bytes && p[7]==frame.layout.uv_bytes);
            const uint32_t *params=(const uint32_t *)(uintptr_t)address(p+13);
            assert(params[0]==frame.layout.pitch && params[1]==frame.destination_pitch);
            assert((int32_t)params[2]==(frame.color.full_range?8192:9539));
        }else if(op==0x76 && p[0]==0x20c){
            assert(n==3);submitted_code=((uintptr_t)p[1]<<8)|((uintptr_t)p[2]<<40);
        }else if(op==0x15){
            assert(marks==1 && previous_op==0x47);
            assert(n==4 && p[0]==240 && p[1]==135 && p[2]==1 && p[3]==1);dispatch=true;
        }
        else if(op==0x46){assert(dispatch && p[0]==0x407);flush=true;}
        else if(op==0x47){
            assert(n==5 && p[0]==0x528);
            volatile uint64_t *target=(volatile uint64_t *)(uintptr_t)(p[1]|((uint64_t)(p[2]&0xffffu)<<32));
            assert(!((uintptr_t)target&7u) && (uintptr_t)target>=(uintptr_t)control &&
                (uintptr_t)target<=(uintptr_t)control+65536-8);
            if((p[2]&0xffff0000u)==0x60000000u){
                assert(p[3]==0 && p[4]==0);
                if(!marks){assert(!dispatch);submitted_timestamp_start=target;}
                else {assert(marks==1 && dispatch && flush && post_acquire && previous_op==0x58);
                    submitted_timestamp_end=target;}
                ++marks;
            }else{
                assert(p[2]==(0x40000000u|((uint32_t)((uintptr_t)target>>32)&0xffffu)));
                assert(dispatch && flush && marks==2 && previous_op==0x47 && cursor+n==words);
                submitted_label=target;submitted_sequence=p[3];assert(p[4]==0 && submitted_sequence);
                release=true;
            }
        }else assert(op==0x58 || op==0x76);
        if(op==0x58 && dispatch && flush)post_acquire=true;
        previous_op=op;
        cursor+=n;
    }
    assert(cursor==words && user_blocks==1 && release && marks==2);
    assert(submitted_timestamp_start!=submitted_timestamp_end && submitted_timestamp_start!=submitted_label &&
        submitted_timestamp_end!=submitted_label);
}
OrbisPthread scePthreadSelf(void){return (OrbisPthread)thread_cookie;}
size_t sceKernelGetDirectMemorySize(void){return 1024u*1024u*1024u;}
int sceKernelAllocateDirectMemory(off_t lo,off_t hi,size_t bytes,size_t alignment,int type,off_t *physical)
{
    ++reserves;assert(lo==0 && hi>0 && (bytes==65536 || bytes==262144 || bytes==33226752 || bytes==X4_GPU_VERIFY_NATIVE_BYTES) && alignment==65536 && type==0);
    if(reserve_rc || reserves==reserve_fail_at)return reserve_rc?reserve_rc:-71;
    assert(native_memory_count<6);unsigned index=native_memory_count++;
    *physical=(off_t)(123u+index*1024u)*65536;
    native_memory[index].physical=*physical;native_memory[index].bytes=bytes;native_memory[index].reserved=true;return 0;
}
int sceKernelMapDirectMemory(void **p,size_t bytes,int prot,int flags,off_t physical,size_t alignment)
{
    ++maps;assert(prot==0x33 && flags==0 && alignment==65536);
    unsigned index=0;while(index<native_memory_count && native_memory[index].physical!=physical)++index;
    assert(index<native_memory_count && native_memory[index].reserved && native_memory[index].bytes==bytes);
    if(map_rc || maps==map_fail_at)return map_rc?map_rc:-72;
    assert(!posix_memalign(&native_memory[index].pointer,65536,bytes));*p=native_memory[index].pointer;
    if(index==0){assert(bytes==65536);control=*p;}return 0;
}
int sceKernelMunmap(void *p,size_t bytes)
{
    ++unmaps;unsigned index=0;while(index<native_memory_count && native_memory[index].pointer!=p)++index;
    assert(index<native_memory_count && native_memory[index].bytes==bytes && p);
    if(unmap_rc)return unmap_rc;free(p);native_memory[index].pointer=NULL;if(index==0)control=NULL;return 0;
}
int sceKernelReleaseDirectMemory(off_t p,size_t bytes)
{
    ++releases;unsigned index=0;while(index<native_memory_count && native_memory[index].physical!=p)++index;
    assert(index<native_memory_count && native_memory[index].reserved && native_memory[index].bytes==bytes && !native_memory[index].pointer);
    if(release_rc)return release_rc;native_memory[index].reserved=false;return 0;
}
int sceGnmAreSubmitsAllowed(void){++allowed_calls;return allowed;}
static void inspect_verify_commands(const uint32_t *d,size_t bytes)
{
    X4GpuConvert *g=submitted_converter;assert(g&&d==g->verify_dcb&&g->verify_this_frame);
    assert(atomic_load(&g->verify_state)==VERIFY_GPU);
    const uint32_t *q=g->verify_parameters;uint32_t users[16]={0};
    unsigned dispatches=0,stamps=0,labels=0;size_t at=0,words=bytes/4;
    while(at<words){
        uint32_t head=d[at++];assert((head>>30)==3);
        unsigned n=((head>>16)&0x3fffu)+1,op=(head>>8)&255u;assert(n<=words-at);
        const uint32_t *p=d+at;
        if(op==0x76&&p[0]==0x240){assert(n==17);memcpy(users,p+1,sizeof(users));}
        else if(op==0x15){assert(n==4&&p[0]==240&&p[1]&&p[2]==1&&p[3]==1);++dispatches;}
        else if(op==0x47){
            assert(n==5&&p[0]==0x528);uintptr_t target=(uintptr_t)(p[1]|((uint64_t)(p[2]&65535u)<<32));
            if((p[2]&0xffff0000u)==0x60000000u){assert(target==(uintptr_t)(stamps?g->verify_end:g->verify_start));++stamps;}
            else {assert(target==(uintptr_t)g->verify_label&&p[3]==g->sequence);++labels;}
        }else assert(op==0x76||op==0x58||op==0x46);
        at+=n;
    }
    assert(at==words&&dispatches==1&&stamps==2&&labels==1);
    assert(address(users)==(uintptr_t)submitted_frame.source&&users[2]==submitted_frame.layout.total_nv12_bytes);
    assert(address(users+4)==(uintptr_t)submitted_frame.destination&&users[6]==submitted_frame.destination_bytes);
    assert(address(users+8)==(uintptr_t)g->verify_pixels&&users[10]==X4_GPU_VERIFY_NATIVE_BYTES);
    assert(address(users+12)==(uintptr_t)q&&users[14]==64);
    assert(q[0]==submitted_frame.layout.pitch&&q[1]==submitted_frame.layout.visible_width&&q[4]==X4_GPU_VERIFY_NV12_CAPACITY);
    assert(q[6]+q[7]<=submitted_frame.layout.visible_height&&q[8]+q[9]<=submitted_frame.layout.visible_height/2);
    /* ABI substitute models only this copy, never claims a GPU execution. */
    for(unsigned row=q[6];row<q[6]+q[7];++row)
        memcpy(g->verify_pixels+(size_t)row*q[1],submitted_frame.source+(size_t)row*q[0],q[1]);
    for(unsigned row=q[8];row<q[8]+q[9];++row)
        memcpy(g->verify_pixels+q[3]+(size_t)row*q[1],submitted_frame.source+q[2]+(size_t)row*q[0],q[1]);
    for(unsigned row=0;row<q[11];++row)
        memcpy(g->verify_pixels+q[4]+(size_t)row*1920*4,submitted_frame.destination+(size_t)(q[10]+row)*q[5],1920*4);
}
int sceGnmSubmitCommandBuffers(uint32_t count,void **dcb,uint32_t *sizes,void **ccb,uint32_t *csizes)
{
    ++submits;assert((count==1||count==2) && dcb && sizes && !ccb && !csizes);
    assert(sizes[0] && sizes[0]<=4096 && !(sizes[0]&3));inspect_commands(dcb[0],sizes[0]);
    if(count==2){assert(sizes[1]&&sizes[1]<=4096&&!(sizes[1]&3));inspect_verify_commands(dcb[1],sizes[1]);}
    submitted_dcb_bytes=sizes[0];mock_now+=mock_prep_us;
    if(inject_label)*submitted_label=submitted_sequence;
    return submit_rc;
}
int sceGnmSubmitDone(void){++done_calls;mock_now+=mock_done_us;return done_rc;}
static void reset(void)
{
    assert(!control);for(unsigned i=0;i<native_memory_count;++i)assert(!native_memory[i].reserved && !native_memory[i].pointer);
    memset(native_memory,0,sizeof(native_memory));native_memory_count=reserve_fail_at=map_fail_at=0;
    reserve_rc=map_rc=submit_rc=done_rc=unmap_rc=release_rc=0;
    reserves=maps=submits=done_calls=unmaps=releases=allowed_calls=clock_calls=0;allowed=1;thread_cookie=1;
    mock_now=mock_prep_us=mock_done_us=0;flush_count=0;submitted_dcb_bytes=0;
    inject_label=false;submitted_label=NULL;submitted_sequence=0;
    submitted_converter=NULL;memset(&submitted_frame,0,sizeof(submitted_frame));
    submitted_timestamp_start=submitted_timestamp_end=NULL;submitted_code=0;
}
static X4GpuConvert *opened(void)
{
    X4GpuConvert *g=x4_gpu_convert_open();assert(g && g->status.prepared && !g->status.disabled);
    assert(g->status.reserve_result==0 && g->status.map_result==0 &&
        g->status.allowed_result==X4_GPU_CONVERT_NOT_RUN);
    assert(flush_count==1 && was_flushed(g->arena,X4_GPU_CONVERT_ARENA_BYTES));
    assert(g->status.submit_result==X4_GPU_CONVERT_NOT_RUN && g->status.submit_done_result==X4_GPU_CONVERT_NOT_RUN);
    assert(g->status.unmap_result==X4_GPU_CONVERT_NOT_RUN && g->status.release_result==X4_GPU_CONVERT_NOT_RUN);
    return g;
}
static void completed(X4GpuConvert *g,uint64_t time)
{*submitted_label=submitted_sequence;assert(x4_gpu_convert_poll(g,time,NULL)==X4_GPU_CONVERT_DONE);}
static void stamped(X4GpuConvert *g,uint64_t now,uint64_t start_tick,uint64_t end_tick)
{
    flush_count=0;assert(!start(g,&frame,now));
    assert(*submitted_timestamp_start==0 && *submitted_timestamp_end==0);
    *submitted_timestamp_start=start_tick;*submitted_timestamp_end=end_tick;completed(g,now+100);
}
static void quality_state(const X4GpuConvertFrame *baseline)
{
    frame=*baseline;frame.quality_word=X4_GPU_QUALITY_DITHER|X4_GPU_QUALITY_REPEAT;
    reset();X4GpuConvert *g=opened();allowed=0;
    assert(start(g,&frame,1000)==X4_GPU_CONVERT_REPEAT);
    assert(g->status.pausa_repetidas==1 && !g->status.cpu_fallbacks && !submits && reserves==1 && maps==1);
    assert(!g->quality_attempted && !g->sequence && !g->status.pending);
    frame.quality_word=X4_GPU_QUALITY_DITHER;
    assert(start(g,&frame,1100)==X4_GPU_CONVERT_CPU && g->status.cpu_fallbacks==1 && reserves==1);
    allowed=1;assert(start(g,&frame,1200)==X4_GPU_CONVERT_ACCEPTED);
    assert(g->quality_mapped && g->status.quality_prepared && !g->intermediate && reserves==2 && maps==2);
    assert(g->status.quality_active && !g->status.quality_cas);
    assert(!memcmp(g->quality_code[0],x4_gpu_quality_direct_code,sizeof(x4_gpu_quality_direct_code)));
    assert(!memcmp(g->quality_code[1],x4_gpu_quality_prepare_code,sizeof(x4_gpu_quality_prepare_code)));
    assert(!memcmp(g->quality_code[2],x4_gpu_quality_finish_code,sizeof(x4_gpu_quality_finish_code)));
    uint8_t *quality_arena=g->quality_arena;uint32_t *quality_parameters=g->quality_parameters;
    completed(g,1300);check_r3_layout_and_packets(g);
    frame.quality_word=0;assert(!start(g,&frame,1400));
    assert(submitted_code==(uintptr_t)g->code[3] && !g->status.quality_active && reserves==2);
    completed(g,1500);check_r3_layout_and_packets(g);
    frame.quality_word=X4_GPU_QUALITY_DITHER|(1u<<1)|X4_GPU_QUALITY_CAS;
    assert(!start(g,&frame,1600) && reserves==3 && maps==3 && g->intermediate && g->status.quality_cas);
    assert(g->quality_arena==quality_arena && g->quality_parameters==quality_parameters);
    void *intermediate=g->intermediate;completed(g,1700);
    frame.quality_word=X4_GPU_QUALITY_DITHER|(2u<<1)|X4_GPU_QUALITY_CAS|X4_GPU_QUALITY_REPEAT;
    assert(!start(g,&frame,1800) && reserves==3 && maps==3 && g->intermediate==intermediate);
    completed(g,1900);check_r3_layout_and_packets(g);
    assert(x4_gpu_convert_close(&g,2000) && unmaps==3 && releases==3);

    /* CAS selected with sharpness No is a no-op unless another filter is on;
     * merely choosing repeat never changes the current OFF engine/packets. */
    reset();g=opened();frame=*baseline;frame.color.sharpness=0;
    frame.quality_word=X4_GPU_QUALITY_CAS|X4_GPU_QUALITY_REPEAT;
    assert(!start(g,&frame,100) && reserves==1 && !g->quality_attempted && !g->status.quality_active);
    assert(submitted_code==(uintptr_t)g->code[1]);completed(g,150);
    frame.quality_word|=X4_GPU_QUALITY_DITHER;
    assert(!start(g,&frame,200) && reserves==2 && !g->intermediate && !g->status.quality_cas);
    completed(g,250);assert(x4_gpu_convert_close(&g,300));

    /* A quality-only allocation failure latches quality fallback while the
     * original GPU conversion remains available and byte-identical. */
    for(unsigned failure=0;failure<4;++failure){
        reset();g=opened();frame=*baseline;frame.quality_word=X4_GPU_QUALITY_DITHER|X4_GPU_QUALITY_CAS;
        if(failure==0)reserve_fail_at=2;else if(failure==1)map_fail_at=2;
        else if(failure==2)reserve_fail_at=3;else map_fail_at=3;
        assert(start(g,&frame,1000)==X4_GPU_CONVERT_CPU && !submits && !g->status.pending);
        assert(g->quality_failed && g->status.quality_failed && !g->status.disabled);
        unsigned old_reserves=reserves;assert(start(g,&frame,1100)==X4_GPU_CONVERT_CPU && reserves==old_reserves);
        frame.quality_word=0;assert(!start(g,&frame,1200) && reserves==old_reserves);
        completed(g,1300);check_r3_layout_and_packets(g);
        assert(x4_gpu_convert_close(&g,1400));
    }

    /* New intermediate, controls and original resources share one ownership
     * epoch. Timeout/cancel cannot free any part before the exact EOP. */
    reset();g=opened();frame=*baseline;frame.quality_word=X4_GPU_QUALITY_DITHER|X4_GPU_QUALITY_CAS;
    assert(!start(g,&frame,1000));quality_arena=g->quality_arena;intermediate=g->intermediate;
    assert(x4_gpu_convert_poll(g,51000,NULL)==X4_GPU_CONVERT_TIMEOUT);
    assert(!x4_gpu_convert_close(&g,52000) && !unmaps && !releases && g->quality_arena==quality_arena && g->intermediate==intermediate);
    *submitted_label=submitted_sequence-1;assert(!x4_gpu_convert_close(&g,53000) && !unmaps && !releases);
    *submitted_label=submitted_sequence;assert(x4_gpu_convert_poll(g,54000,NULL)==X4_GPU_CONVERT_LATE_DONE);
    assert(x4_gpu_convert_close(&g,55000) && unmaps==3 && releases==3);

    reset();g=opened();frame=*baseline;frame.quality_word=X4_GPU_QUALITY_CAS;
    submit_rc=-20;assert(!start(g,&frame,1000) && g->status.pending && g->status.disabled);
    assert(!x4_gpu_convert_close(&g,2000) && !unmaps && !releases);
    *submitted_label=submitted_sequence;assert(x4_gpu_convert_close(&g,3000) && unmaps==3 && releases==3);

    /* Native unmap/release failures are recoverable close states. They must
     * retain ownership records rather than discard an allocated arena. */
    reset();g=opened();frame=*baseline;frame.quality_word=X4_GPU_QUALITY_CAS;
    assert(!start(g,&frame,1000));completed(g,1100);unmap_rc=-22;
    assert(!x4_gpu_convert_close(&g,1200) && g);
    unmap_rc=0;release_rc=-23;assert(!x4_gpu_convert_close(&g,1300) && g);
    release_rc=0;assert(x4_gpu_convert_close(&g,1400));
    frame=*baseline;
    printf("PASS quality converter ABI/state: lazy direct/CAS resources, two dispatch barrier, noise identity, repeat-before-allocation, OFF byte comparisons=%u, allocation latches, exact-EOP quarantine and recoverable close\n",off_packet_comparisons);
}
static void verification_state(const X4GpuConvertFrame *baseline)
{
    frame=*baseline;reset();X4GpuConvert *g=opened();
    assert(!x4_gpu_convert_verification_prepare(g));
    assert(g->status.verification_native_bytes==X4_GPU_VERIFY_NATIVE_BYTES&&
        g->status.verification_control_bytes==X4_GPU_VERIFY_CONTROL_BYTES);
    /* First capture: conversion EOP alone cannot release either live lease. */
    flush_count=0;assert(x4_gpu_convert_verification_request(g,517,16));
    assert(!start(g,&frame,100));assert(!x4_gpu_convert_verification_cancel(g));
    *g->label=g->sequence;assert(!x4_gpu_convert_label_ready(g));
    assert(x4_gpu_convert_poll(g,200,NULL)==X4_GPU_CONVERT_WAIT);
    assert(!x4_gpu_convert_close(&g,200)&&g&&g->status.pending);
    *g->verify_label=g->sequence;assert(x4_gpu_convert_label_ready(g));
    assert(x4_gpu_convert_poll(g,300,NULL)==X4_GPU_CONVERT_DONE);
    X4GpuVerifySnapshot snapshot;assert(x4_gpu_convert_verification_take(g,&snapshot));
    assert(snapshot.source==g->verify_pixels&&snapshot.actual==(uint32_t *)(g->verify_pixels+X4_GPU_VERIFY_NV12_CAPACITY));
    assert(snapshot.source!=frame.source&&snapshot.actual!=frame.destination&&snapshot.token&&snapshot.owner_epoch);
    assert(!x4_gpu_convert_verification_request(g,0,16));
    assert(!x4_gpu_convert_close(&g,300)&&g); /* worker still owns immutable snapshot */
    X4GpuVerifyResult result={.token=snapshot.token,.owner_epoch=snapshot.owner_epoch,.checked_pixels=16*1920,.worker_us=123};
    ++result.owner_epoch;assert(x4_gpu_convert_verification_complete(g,&result)==X4_PIPELINE_STALE);
    --result.owner_epoch;++result.token;assert(x4_gpu_convert_verification_complete(g,&result)==X4_PIPELINE_STALE);
    --result.token;assert(!x4_gpu_convert_verification_complete(g,&result));
    assert(x4_gpu_convert_verification_collect(g,NULL));
    assert(g->status.verification_checks==1&&!g->status.verification_errors&&!g->status.disabled);
    assert(g->status.verification_last_worker_us==123&&g->status.verification_skipped==1);
    /* A slow background comparison never prevents another conversion. Its
     * mismatch can arrive with a NEW frame in flight: quarantine, wait EOP,
     * cancel its display and retain only then. This is not a timeout. */
    flush_count=0;assert(x4_gpu_convert_verification_request(g,10,16));assert(!start(g,&frame,1000));
    *g->label=*g->verify_label=g->sequence;assert(x4_gpu_convert_poll(g,1100,NULL)==X4_GPU_CONVERT_DONE);
    assert(x4_gpu_convert_verification_take(g,&snapshot));
    flush_count=0;assert(!start(g,&frame,1200));
    result=(X4GpuVerifyResult){.token=snapshot.token,.owner_epoch=snapshot.owner_epoch,
        .checked_pixels=16*1920,.pixels_outside=1,.max_channel_difference=7};
    assert(!x4_gpu_convert_verification_complete(g,&result));
    assert(x4_gpu_convert_poll(g,1300,NULL)==X4_GPU_CONVERT_VERIFY_QUARANTINE);
    assert(g->status.disabled&&g->status.verification_failed&&g->status.pending&&g->status.quarantined&&!g->status.timeouts);
    assert(!x4_gpu_convert_close(&g,1300));
    *g->label=g->sequence;assert(x4_gpu_convert_poll(g,1400,NULL)==X4_GPU_CONVERT_LATE_DONE);
    assert(start(g,&frame,1500)==X4_GPU_CONVERT_CPU&&g->status.verification_checks==2);
    assert(x4_gpu_convert_close(&g,1500));
    /* Copy EOP timeout conservatively retains both command buffers; a late
     * EOP retires ownership but must never publish a stale worker snapshot. */
    reset();g=opened();assert(!x4_gpu_convert_verification_prepare(g));
    flush_count=0;assert(x4_gpu_convert_verification_request(g,0,16));assert(!start(g,&frame,100));
    *g->label=g->sequence;assert(x4_gpu_convert_poll(g,50100,NULL)==X4_GPU_CONVERT_TIMEOUT);
    assert(!x4_gpu_convert_close(&g,50100));*g->verify_label=g->sequence;
    assert(x4_gpu_convert_poll(g,50200,NULL)==X4_GPU_CONVERT_LATE_DONE);
    assert(!x4_gpu_convert_verification_take(g,&snapshot)&&x4_gpu_convert_close(&g,50300));
    /* Partial native allocation/map failures remain releasable, and an
     * unsuccessful unmap cannot be followed by release/free. */
    for(unsigned fail=2;fail<=3;++fail){
        reset();g=opened();reserve_fail_at=fail;assert(x4_gpu_convert_verification_prepare(g));
        assert(g->status.verification_failed&&g->status.disabled&&x4_gpu_convert_close(&g,0));
        reset();g=opened();map_fail_at=fail;assert(x4_gpu_convert_verification_prepare(g));
        assert(g->status.verification_failed&&x4_gpu_convert_close(&g,0));
    }
    reset();g=opened();assert(!x4_gpu_convert_verification_prepare(g));unmap_rc=-73;
    assert(!x4_gpu_convert_close(&g,0)&&g&&!releases);unmap_rc=0;assert(x4_gpu_convert_close(&g,0));
    puts("PASS sparse verification state: two EOP leases, detached worker lifetime, stale epoch/token rejection, concurrent mismatch quarantine/CPU, late copy retirement and native failure retention (ABI doubles, no GPU execution)");
}
int main(void)
{
    assert(!posix_memalign((void **)&source,64,3133440));memset(source,91,3133440);
    assert(!posix_memalign((void **)&destination,64,8388608));memset(destination,0,8388608);
    frame=(X4GpuConvertFrame){.layout={.visible_width=1920,.visible_height=1080,
        .coded_width=1920,.coded_height=1088,.storage_height=1088,.pitch=1920,
        .y_bytes=2088960,.uv_bytes=1044480,.uv_plane_offset=2088960,.total_nv12_bytes=3133440,
        .visible_y_offset=0,.visible_uv_offset=2088960,.rgb_pixels=2073600,.rgb_bytes=8294400},
        .format=X4_VIDEO_FORMAT_NV12,.color={1,false,X4_CONVERT_SMOOTH,X4_SHARPNESS_MED},
        .source=source,.source_bytes=3133440,.destination=destination,.destination_bytes=8388608,.destination_pitch=1920};
    const char *reason=NULL;
    assert(x4_gpu_convert_admit(&frame.layout,frame.format,&frame.color,&reason) && !strcmp(reason,"no"));
    X4VideoNv12Layout bad=frame.layout;bad.visible_width=1280;
    assert(!x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason) && !strcmp(reason,"geometry_not_supported"));
    assert(!x4_gpu_convert_admit(&frame.layout,X4_VIDEO_FORMAT_P010,&frame.color,&reason));
    bad=frame.layout;bad.visible_y_offset=2;assert(!x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason));
    reset();X4GpuConvert *baseline=opened();check_r3_layout_and_packets(baseline);
    assert(x4_gpu_convert_close(&baseline,0));
    reset();reserve_rc=-1;X4GpuConvert *g=x4_gpu_convert_open();assert(g && x4_gpu_convert_disabled(g));
    assert(g->status.reserve_result==-1 && g->status.map_result==X4_GPU_CONVERT_NOT_RUN &&
        g->status.submit_result==X4_GPU_CONVERT_NOT_RUN && g->status.submit_done_result==X4_GPU_CONVERT_NOT_RUN);
    assert(x4_gpu_convert_close(&g,0) && !g && !maps && !releases);
    reset();map_rc=-2;g=x4_gpu_convert_open();assert(g && x4_gpu_convert_disabled(g));
    assert(g->status.reserve_result==0 && g->status.map_result==-2 && g->status.submit_result==X4_GPU_CONVERT_NOT_RUN);
    assert(x4_gpu_convert_close(&g,0) && releases==1 && !unmaps);
    reset();g=opened();assert(start(g,&frame,1000)==0 && submits==1 && done_calls==1);
    assert(x4_gpu_convert_pending(g));assert(x4_gpu_convert_poll(g,2000,NULL)==X4_GPU_CONVERT_WAIT);
    assert(start(g,&frame,2100)==1 && submits==1);
    completed(g,2200);assert(!x4_gpu_convert_pending(g));assert(x4_gpu_convert_close(&g,2300));
    reset();g=opened();assert(!start(g,&frame,1000));
    *submitted_label=submitted_sequence-1;assert(x4_gpu_convert_poll(g,50999,NULL)==X4_GPU_CONVERT_WAIT);
    assert(x4_gpu_convert_poll(g,51000,NULL)==X4_GPU_CONVERT_TIMEOUT);
    assert(g->status.timeouts==1 && g->status.quarantines==1 && g->status.pending && g->status.disabled);
    assert(x4_gpu_convert_poll(g,52000,NULL)==X4_GPU_CONVERT_WAIT);
    assert(start(g,&frame,52000)==1 && submits==1);
    assert(!x4_gpu_convert_close(&g,53000) && !unmaps && !releases && g);
    *submitted_label=submitted_sequence;assert(x4_gpu_convert_poll(g,54000,NULL)==X4_GPU_CONVERT_LATE_DONE);
    assert(!g->status.pending && g->status.disabled && g->status.late_completed==1);
    assert(x4_gpu_convert_close(&g,55000));
    reset();g=opened();inject_label=true;assert(!start(g,&frame,1000));
    assert(x4_gpu_convert_poll(g,51001,NULL)==X4_GPU_CONVERT_LATE_DONE && g->status.timeouts==1);
    assert(x4_gpu_convert_close(&g,52000));
    reset();g=opened();assert(!start(g,&frame,1000));completed(g,2000);
    thread_cookie=2;assert(start(g,&frame,3000)==1 && submits==1 && g->status.wrong_thread_submits==1);
    assert(x4_gpu_convert_close(&g,4000));
    reset();g=opened();submit_rc=-20;assert(!start(g,&frame,1000));
    assert(g->status.pending && g->status.disabled && done_calls==1);
    assert(!x4_gpu_convert_close(&g,2000));completed(g,3000);assert(x4_gpu_convert_close(&g,4000));
    reset();g=opened();done_rc=-21;inject_label=true;assert(!start(g,&frame,1000));
    assert(x4_gpu_convert_poll(g,2000,NULL)==X4_GPU_CONVERT_DONE && g->status.disabled);
    assert(x4_gpu_convert_close(&g,3000));
    reset();g=opened();unmap_rc=-22;assert(!x4_gpu_convert_close(&g,0) && g);
    assert(g->status.unmap_result==-22 && g->status.release_result==X4_GPU_CONVERT_NOT_RUN);
    unmap_rc=0;release_rc=-23;assert(!x4_gpu_convert_close(&g,0) && g && !g->mapped);
    release_rc=0;assert(x4_gpu_convert_close(&g,0));
    reset();g=opened();assert(!start(g,&frame,1000));
    assert(!x4_gpu_convert_close(&g,2000) && !unmaps);*submitted_label=submitted_sequence;
    assert(x4_gpu_convert_close(&g,3000) && !g && unmaps==1 && releases==1);
    reset();g=opened();g->sequence=UINT32_MAX;assert(start(g,&frame,0)==1 && !submits);
    assert(x4_gpu_convert_close(&g,0));
    reset();g=opened();g->timestamp_start=(volatile uint64_t *)(g->arena+65536-4);
    assert(start(g,&frame,0)==1 && !submits && g->status.disabled && !g->status.pending);
    assert(x4_gpu_convert_close(&g,0));
    reset();g=opened();g->timestamp_end=(volatile uint64_t *)(g->arena+65536);
    assert(start(g,&frame,0)==1 && !submits && g->status.disabled && !g->status.pending);
    assert(x4_gpu_convert_close(&g,0));
    /* One probe per frame, no command build/flush/clock call or permanent
     * disable during a temporary pause; the same object resumes submission. */
    reset();g=opened();allowed=0;flush_count=0;
    const unsigned n=240;const uint64_t base=1000000,step=16667;
    for(unsigned i=0;i<n;++i){
        assert(start(g,&frame,base+i*step)==X4_GPU_CONVERT_CPU);
        assert(!g->status.disabled && !g->status.pending && !g->status.owner_bound);
        assert(g->status.allowed_result==0 && !strcmp(g->status.reason,"submits_paused"));
        assert(allowed_calls==i+1 && !submits && !done_calls && !clock_calls && !flush_count);
    }
    X4GpuConvertStatus paused={0};x4_gpu_convert_status(g,&paused);
    assert(paused.pause_active && paused.pause_episodes==1 && paused.pause_cpu_frames==n &&
        paused.pause_current_frames==n && paused.pause_started_us==base && paused.cpu_fallbacks==n);
    assert(paused.pause_max_us==(n-1)*step && !paused.pause_last_us);
    allowed=1;assert(start(g,&frame,base+n*step)==X4_GPU_CONVERT_ACCEPTED);
    assert(!g->status.pause_active && !g->status.disabled && g->status.allowed_result==1);
    assert(g->status.pause_last_us==n*step && g->status.pause_max_us==n*step &&
        g->status.pause_last_frames==n && !g->status.pause_current_frames &&
        g->status.pause_episodes==1 && g->status.pause_cpu_frames==n && allowed_calls==n+1);
    completed(g,base+n*step+100);assert(x4_gpu_convert_close(&g,base+n*step+200));
    /* Separate episodes preserve cumulative selections and the longest
     * observed episode; close finalizes a pause even without a resumed submit. */
    reset();g=opened();allowed=0;assert(start(g,&frame,1000)==1);assert(start(g,&frame,1100)==1);
    allowed=1;assert(!start(g,&frame,1200));completed(g,1250);
    assert(g->status.pause_last_us==200 && g->status.pause_last_frames==2);
    allowed=0;assert(start(g,&frame,2000)==1);assert(start(g,&frame,2050)==1);assert(start(g,&frame,2600)==1);
    allowed=1;assert(!start(g,&frame,3100));completed(g,3150);
    assert(g->status.pause_episodes==2 && g->status.pause_cpu_frames==5 &&
        g->status.pause_last_us==1100 && g->status.pause_max_us==1100 && g->status.pause_last_frames==3);
    allowed=0;assert(start(g,&frame,4000)==1);unmap_rc=-22;
    assert(!x4_gpu_convert_close(&g,4500) && g && !g->status.pause_active);
    assert(g->status.pause_last_us==500 && g->status.pause_last_frames==1 &&
        g->status.pause_max_us==1100 && g->status.pause_episodes==3 && g->status.pause_cpu_frames==6);
    unmap_rc=0;assert(x4_gpu_convert_close(&g,4600));
    /* A negative native result remains distinct and permanent; no next
     * frame probes the driver again. A preceding pause is finalized too. */
    reset();g=opened();allowed=0;assert(start(g,&frame,1000)==1);
    allowed=-12;assert(start(g,&frame,1500)==1 && g->status.disabled && !g->status.pending);
    assert(g->status.allowed_result==-12 && !strcmp(g->status.reason,"submits_error") &&
        !g->status.pause_active && g->status.pause_last_us==500 && g->status.pause_last_frames==1);
    assert(start(g,&frame,2000)==1 && allowed_calls==2 && !submits && !clock_calls);
    assert(x4_gpu_convert_close(&g,2100));
    /* The only per-frame flushes are CPU-written controls. SubmitDone cost
     * belongs to eop_us: the clock is sampled immediately after Submit. */
    reset();g=opened();flush_count=0;mock_prep_us=137;mock_done_us=163;
    assert(!start(g,&frame,1000) && clock_calls==1 && g->status.submit_returned_us==1137);
    assert(g->status.last_prep_us==137 && !g->status.last_eop_us && mock_now==1300);
    assert(flush_count==5 && was_flushed(g->parameters,64) &&
        was_flushed((const void *)g->label,64) && was_flushed(g->dcb,submitted_dcb_bytes) &&
        was_flushed((const void *)g->timestamp_start,64) && was_flushed((const void *)g->timestamp_end,64));
    assert(x4_gpu_convert_poll(g,1400,NULL)==X4_GPU_CONVERT_WAIT && flush_count==6);
    completed(g,1500);assert(g->status.last_prep_us==137 && g->status.last_eop_us==363 &&
        g->status.last_elapsed_us==500 && g->status.last_prep_us+g->status.last_eop_us==g->status.last_elapsed_us);
    assert(x4_gpu_convert_poll(g,1600,NULL)==X4_GPU_CONVERT_IDLE && g->status.last_eop_us==363);
    assert(x4_gpu_convert_close(&g,1700));
    for(unsigned matrix=0;matrix<4;++matrix){
        reset();g=opened();frame.color.matrix_coefficients=(matrix&1)?1:6;frame.color.full_range=(matrix&2)!=0;
        inject_label=true;assert(!start(g,&frame,100));completed(g,200);assert(x4_gpu_convert_close(&g,300));
    }
    /* Readiness probing never consumes an EOP, mutates status, creates a
     * timeout or allows another thread to observe the owner's label. */
    reset();g=opened();assert(!x4_gpu_convert_label_ready(g));assert(!start(g,&frame,1000));
    X4GpuConvertStatus before={0},after={0};x4_gpu_convert_status(g,&before);
    assert(!x4_gpu_convert_label_ready(g));x4_gpu_convert_status(g,&after);
    assert(!memcmp(&before,&after,sizeof(before)));
    *submitted_label=submitted_sequence;thread_cookie=2;unsigned old_flush=flush_count;
    assert(!x4_gpu_convert_label_ready(g) && old_flush==flush_count);
    thread_cookie=1;assert(x4_gpu_convert_label_ready(g));x4_gpu_convert_status(g,&after);
    assert(!memcmp(&before,&after,sizeof(before)) && after.pending && !after.completed);
    completed(g,2000);assert(!x4_gpu_convert_label_ready(g));assert(x4_gpu_convert_close(&g,2100));
    /* Both geometries dispatch the same 1920x1080 output grid. The second
     * set of four shader programs is used only for exact admitted 720p. */
    X4GpuConvertFrame frame1080=frame;
    frame.layout=(X4VideoNv12Layout){.visible_width=1280,.visible_height=720,
        .coded_width=1280,.coded_height=720,.storage_height=720,.pitch=1280,
        .y_bytes=921600,.uv_bytes=460800,.uv_plane_offset=921600,.total_nv12_bytes=1382400,
        .visible_y_offset=0,.visible_uv_offset=921600,.rgb_pixels=921600,.rgb_bytes=3686400};
    X4GpuConvertFrame frame720=frame;
    assert(x4_gpu_convert_admit(&frame.layout,frame.format,&frame.color,&reason));
    bad=frame.layout;bad.pitch=1408;bad.y_bytes=bad.uv_plane_offset=bad.visible_uv_offset=1408*720;
    bad.uv_bytes=bad.y_bytes/2;bad.total_nv12_bytes=bad.y_bytes+bad.uv_bytes;
    assert(x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason));
    bad=frame.layout;bad.pitch=1276;assert(!x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason));
    bad=frame.layout;bad.pitch=1282;assert(!x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason));
    bad=frame.layout;bad.storage_height=736;assert(!x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason));
    bad=frame.layout;bad.visible_y_offset=2;assert(!x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason));
    bad=frame.layout;bad.visible_uv_offset+=64;assert(!x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason));
    bad=frame.layout;bad.total_nv12_bytes--;assert(!x4_gpu_convert_admit(&bad,frame.format,&frame.color,&reason));
    for(unsigned resolution=0;resolution<2;++resolution){
        X4GpuConvertFrame admitted=frame;
        if(resolution)frame=frame1080;
        reset();g=opened();
        for(unsigned mode=0;mode<4;++mode){
            frame.color.convert_mode=mode?X4_CONVERT_SMOOTH:X4_CONVERT_ORIGINAL;
            frame.color.sharpness=mode?mode-1:0;flush_count=0;
            assert(!start(g,&frame,100+mode*100));
            assert(submitted_code==(uintptr_t)g->code[(resolution?0:4)+mode]);
            assert(g->status.last_resolution==(resolution?X4_GPU_RESOLUTION_1080:X4_GPU_RESOLUTION_720));
            completed(g,150+mode*100);
        }
        assert(x4_gpu_convert_close(&g,1000));frame=admitted;
    }
    frame=frame1080;
    /* Four unrelated simulated clock rates and +/-2ms observation jitter:
     * no assumed MHz, and no real-us sample before the measured 10s span. */
    const uint64_t frequencies[]={1000000,27000000,100000000,1000000000};
    for(unsigned clock=0;clock<sizeof(frequencies)/sizeof(frequencies[0]);++clock){
        unsigned resolution=clock&1;frame=resolution?frame1080:frame720;
        reset();g=opened();uint64_t hz=frequencies[clock];
        for(unsigned i=0;i<=602;++i){
            uint64_t nominal=100000+(uint64_t)i*16667;
            int64_t jitter=((int64_t)(i%5)-2)*1000;
            uint64_t tick=1000000+nominal*hz/1000000;
            stamped(g,(uint64_t)((int64_t)nominal+jitter),tick,tick+1370*hz/1000000);
            X4GpuTimestampStatus *ts=&g->status.timestamps[resolution];
            assert(ts->samples==i+1 && !ts->invalid && !ts->unsupported);
            if(i<600)assert(!ts->clock_calibrated && !ts->clock_hz &&
                !g->status.last_real_valid && ts->pending_samples==i+1 && !ts->valid_samples);
        }
        X4GpuTimestampStatus *ts=&g->status.timestamps[resolution];
        assert(ts->clock_calibrated && ts->clock_hz>hz-hz/1000 && ts->clock_hz<hz+hz/1000);
        assert(ts->valid_samples==603 && !ts->pending_samples && !ts->pending_overflow);
        assert(g->status.last_real_valid && g->status.last_real_us>=1367 && g->status.last_real_us<=1373);
        assert(!g->status.timestamps[!resolution].samples);
        printf("calibration hz=%llu measured=%llu tick_delta=%llu observed_delta_us=%llu real_us=%llu\n",
            (unsigned long long)hz,(unsigned long long)ts->clock_hz,
            (unsigned long long)(ts->last_end_ticks-ts->first_end_ticks),
            (unsigned long long)(ts->last_observed_us-ts->first_observed_us),
            (unsigned long long)g->status.last_real_us);
        assert(x4_gpu_convert_close(&g,12000000));
    }
    frame=frame1080;
    uint64_t exact_us=0;assert(timestamp_duration(50000,1000000,&exact_us) && exact_us==50000);
    assert(!timestamp_duration(50001,1000000,&exact_us) && !timestamp_duration(1,0,&exact_us));
    /* Pending >50ms marks can only be classified after calibration. Zero,
     * inverted and long marks never disable a functioning converter. */
    reset();g=opened();stamped(g,100000,129999,200000);
    assert(g->status.timestamps[1].pending_samples==1 && !g->status.timestamps[1].invalid);
    stamped(g,10100000,10198000,10200000);
    assert(g->status.timestamps[1].clock_hz==1000000 && g->status.timestamps[1].invalid==1 &&
        g->status.timestamps[1].valid_samples==1 && g->status.last_real_us==2000);
    stamped(g,10200000,0,0);stamped(g,10300000,10600000,10500000);
    stamped(g,10400000,10449999,10500000);
    assert(g->status.timestamps[1].invalid==4 && !g->status.last_real_valid &&
        !g->status.disabled && !g->status.cpu_fallbacks && g->status.completed==5);
    assert(x4_gpu_convert_close(&g,10410000));
    /* Exactly the first 300 structurally invalid observations mark timestamp
     * support unavailable; this is a metric flag, never a CPU fallback. */
    reset();g=opened();
    for(unsigned i=0;i<300;++i){
        stamped(g,1000+i*1000,0,0);
        assert(g->status.timestamps[1].unsupported==(i>=299));
        assert(!g->status.disabled && !g->status.pending && !g->status.cpu_fallbacks);
    }
    assert(g->status.timestamps[1].samples==300 && g->status.timestamps[1].invalid==300 &&
        !g->status.timestamps[1].clock_hz && !g->status.timestamps[1].pending_samples);
    stamped(g,400000,400000,400100); /* Sample 301 is valid but latched off. */
    stamped(g,10400000,10400000,10400100); /* >10s cannot revive calibration. */
    assert(g->status.timestamps[1].samples==302 && g->status.timestamps[1].invalid==300 &&
        !g->status.timestamps[1].clock_hz && !g->status.timestamps[1].clock_calibrated &&
        !g->status.timestamps[1].first_end_ticks && !g->status.timestamps[1].last_end_ticks &&
        !g->status.timestamps[1].pending_samples && !g->status.timestamps[1].valid_samples &&
        !g->status.last_real_valid && !g->status.disabled && !g->status.cpu_fallbacks);
    assert(g->status.last_start_ticks==10400000 && g->status.last_end_ticks==10400100);
    assert(x4_gpu_convert_close(&g,10410000));
    /* First-300 long durations also mean unsupported, but only once the
     * >=10s frequency estimate allows their pending marks to be classified. */
    reset();g=opened();
    for(unsigned i=0;i<=600;++i){
        uint64_t now=100000+(uint64_t)i*16667,end=1000000+now;
        stamped(g,now,end-(i<300?60001:1370),end);
        assert(g->status.timestamps[1].unsupported==(i==600));
        if(i<600)assert(!g->status.timestamps[1].invalid && !g->status.last_real_valid);
    }
    assert(g->status.timestamps[1].invalid==300 && g->status.timestamps[1].valid_samples==300 &&
        !g->status.last_real_valid && !g->status.disabled && !g->status.cpu_fallbacks);
    X4GpuTimestampStatus latched=g->status.timestamps[1];
    stamped(g,11200000,12100000,12101370);
    assert(g->status.timestamps[1].clock_hz==latched.clock_hz &&
        g->status.timestamps[1].last_end_ticks==latched.last_end_ticks &&
        g->status.timestamps[1].last_observed_us==latched.last_observed_us &&
        g->status.timestamps[1].valid_samples==latched.valid_samples &&
        !g->status.last_real_valid && !g->status.disabled && !g->status.cpu_fallbacks);
    assert(x4_gpu_convert_close(&g,11210000));
    /* Before 10s a pathological submission rate cannot grow the pending
     * validation ledger without bound or fabricate invalid samples. */
    reset();g=opened();
    for(unsigned i=0;i<TIMESTAMP_PENDING_CAPACITY+2;++i)
        stamped(g,100000+i*1000,1000000+i*1000,1000100+i*1000);
    assert(g->status.timestamps[1].pending_samples==TIMESTAMP_PENDING_CAPACITY &&
        g->status.timestamps[1].pending_overflow==2 && !g->status.timestamps[1].invalid);
    stamped(g,10100000,11000000,11000100);
    assert(g->status.timestamps[1].clock_calibrated && !g->status.timestamps[1].pending_samples &&
        g->status.timestamps[1].valid_samples==TIMESTAMP_PENDING_CAPACITY+1 &&
        g->status.timestamps[1].pending_overflow==2 && !g->status.timestamps[1].invalid);
    assert(x4_gpu_convert_close(&g,10110000));
    X4GpuConvertStatus missing={0};x4_gpu_convert_status(NULL,&missing);
    assert(missing.disabled && missing.reserve_result==X4_GPU_CONVERT_NOT_RUN &&
        missing.map_result==X4_GPU_CONVERT_NOT_RUN && missing.submit_result==X4_GPU_CONVERT_NOT_RUN &&
        missing.submit_done_result==X4_GPU_CONVERT_NOT_RUN && missing.unmap_result==X4_GPU_CONVERT_NOT_RUN &&
        missing.release_result==X4_GPU_CONVERT_NOT_RUN && missing.allowed_result==X4_GPU_CONVERT_NOT_RUN);
    verification_state(&frame1080);
    quality_state(&frame1080);
    free(source);free(destination);puts("PASS GPU converter ABI/state: 720/1080 admission and eight immutable shaders, quality direct/CAS, ordered timestamp/label packets, measured clocks/jitter/backfill/bounded ledger, metric-only invalid/unsupported marks, owner-only readiness, timeout/quarantine, native failures, temporary pauses and split timing");return 0;
}
