/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/probe_t26_backend.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct X4T26Platform {int unused;};
struct X4T26V1 {int unused;};
typedef struct {uint8_t *output[5];unsigned held,decode_calls,previous;bool pending;} Native;
static unsigned native_calls,delete_calls,marker_calls;static int mode,delete_rc;
static const size_t frame_bytes=(size_t)2048*1088*3/2;
static X4T26Config opened_config;
static int marker(void *ctx,const char *operation)
{assert(ctx==(void *)0x1234&&operation&&*operation);++marker_calls;return 0;}
int x4_stream_sps_inspect(const uint8_t *p,size_t n,X4StreamSps *out,bool *complete)
{
    assert(p&&n==1);*complete=true;
    *out=(X4StreamSps){.profile_idc=77,.level_idc=42,.coded_width=1920,.coded_height=1088,
        .visible_width=1920,.visible_height=1080,.crop_bottom=8,.chroma_format_idc=1,
        .bit_depth_luma=8,.bit_depth_chroma=8,.frame_mbs_only_flag=1,
        .max_num_ref_frames=1,.pic_order_cnt_type=2};return 0;
}
int x4_t26_platform_create(X4T26Platform **p,X4T26BeforeNative before,void *ctx)
{assert(before==marker&&ctx==(void *)0x1234);*p=calloc(1,sizeof(**p));return *p?0:-1;}
void x4_t26_platform_free(X4T26Platform **p){free(*p);*p=NULL;}
void x4_t26_platform_v1_ops(X4T26Platform *p,X4T26V1Ops *ops){(void)p;memset(ops,0,sizeof(*ops));}
void x4_t26_platform_info(const X4T26Platform *p,X4T26PlatformInfo *out){assert(p);memset(out,0,sizeof(*out));}
void x4_t26_platform_cpu(X4T26Platform *p,X4T26CpuSnapshot *out)
{(void)p;*out=(X4T26CpuSnapshot){.resolve_rc=-1,.process_rc=X4_T26_PLATFORM_NOT_RUN,.thread_rc=X4_T26_PLATFORM_NOT_RUN};}
int x4_t26_platform_query(X4T26Platform *p,int64_t offset,X4T26DirectQuery *out,bool *called)
{assert(p&&offset>=0);*called=true;*out=(X4T26DirectQuery){.start=offset,.end=offset+(int64_t)frame_bytes,.memory_type=0};return 0;}
int x4_t26_platform_largest(X4T26Platform *p,uint64_t *bytes,bool *called)
{assert(p);*called=true;*bytes=1024*1024;return 0;}
bool x4_t26_query_contains(const X4T26DirectQuery *q,const X4T26Allocation *a)
{return q->start<=a->physical_offset&&q->end>=a->physical_offset+(int64_t)a->bytes&&q->memory_type==a->memory_type;}
bool x4_t26_query_release_evidence(bool valid,int rc){return valid&&(uint32_t)rc==UINT32_C(0x8002000d);}
int x4_live_video_t26_raw_start(X4LiveVideo *v,const uint8_t *s,size_t n,const atomic_bool *cancel,const X4T26NativeOptions *o)
{
    assert(s&&n==1&&cancel&&o&&o->before==marker);opened_config=o->config;
    Native *state=calloc(1,sizeof(*state));assert(state);
    for(unsigned i=0;i<5;++i){state->output[i]=malloc(frame_bytes);assert(state->output[i]);}
    v->state=state;v->stream_info.active_depth=o->config.depth;return 0;
}
int x4_live_video_stream_raw_decode(X4LiveVideo *v,unsigned offered,const uint8_t *au,size_t bytes,
    uint64_t pts,bool flush,X4ProbeNativeOutput *out)
{
    Native *state=v->state;assert(state&&offered<5);(void)pts;
    memset(out,0,sizeof(*out));
    if(mode==2){out->control_result=X4_SPS_SIZE;return 0;}
    assert(flush|| (au&&bytes==1));++native_calls;
    out->t26_native_called=true;out->decode_end_us=1000+native_calls*100;
    out->decode_us=100;out->native_result=mode==1?-456:0;
    if(!flush){++state->decode_calls;state->held|=1u<<offered;}
    out->native_call=state->decode_calls;
    if(mode==1)return 0;
    if(state->pending){
        unsigned returned=state->previous;out->t26_native_valid=1;out->t26_picture_count=1;
        out->valid=true;out->layout_verified=true;out->pixels=state->output[returned];
        out->slot=returned;out->owned_bytes=frame_bytes;out->returned_bytes=frame_bytes;
        out->t26_native_pointer=state->output[returned];out->t26_native_bytes=frame_bytes;
        out->native_width=1920;out->native_height=mode==3?1080:1088;out->pitch=2048;
        state->held&=~(1u<<returned);
    }
    state->pending=!flush;state->previous=offered;return 0;
}
unsigned x4_live_video_stream_output_held_mask(const X4LiveVideo *v)
{return v&&v->state?((Native *)v->state)->held:0;}
void x4_live_video_stream_info(const X4LiveVideo *v,X4StreamVideoInfo *out){*out=v->stream_info;}
unsigned x4_live_video_t26_allocations(const X4LiveVideo *v,X4T26Allocation out[16])
{
    if(!v->state)return 0;
    for(unsigned i=0;i<5;++i)out[i]=(X4T26Allocation){.physical_offset=(int64_t)i*0x400000,
        .bytes=frame_bytes,.memory_type=0};
    return 5;
}
int x4_live_video_stop(X4LiveVideo *v)
{
    ++delete_calls;if(delete_rc){v->stream_info.retained=true;return delete_rc;}
    Native *state=v->state;assert(state);
    for(unsigned i=0;i<5;++i)free(state->output[i]);
    free(state);v->state=NULL;
    v->stream_info.retained=false;return 0;
}
int x4_t26_v1_create(X4T26V1 **p,const X4T26V1Ops *ops,const X4T26V1Options *o)
{(void)p;(void)ops;(void)o;assert(0&&"Unexpected v1 test path");return -1;}
int x4_t26_v1_open(X4T26V1 *v){(void)v;return -1;}
int x4_t26_v1_decode(X4T26V1 *v,unsigned offered,const void *p,size_t n,uint64_t pts,X4T26V1Output *o)
{(void)v;(void)offered;(void)p;(void)n;(void)pts;(void)o;return -1;}
int x4_t26_v1_flush(X4T26V1 *v,unsigned offered,X4T26V1Output *o){(void)v;(void)offered;(void)o;return -1;}
uint32_t x4_t26_v1_held_mask(const X4T26V1 *v){(void)v;return 0;}
void x4_t26_v1_info(const X4T26V1 *v,X4T26V1Info *o){(void)v;memset(o,0,sizeof(*o));}
size_t x4_t26_v1_allocations(const X4T26V1 *v,X4T26V1Memory *m,size_t n){(void)v;(void)m;(void)n;return 0;}
int x4_t26_v1_close(X4T26V1 *v){(void)v;return 0;}
int x4_t26_v1_destroy(X4T26V1 **v){(void)v;return 0;}
static X4T26Backend *create(atomic_bool *cancel)
{
    uint8_t sps=0x67;X4T26BackendOptions options={.sps=&sps,.sps_bytes=1,.cancel=cancel,
        .before=marker,.context=(void *)0x1234};x4_t26_config_base(&options.config);
    X4T26Backend *b=NULL;assert(!x4_t26_backend_create(&b,&options)&&b);return b;
}
int main(void)
{
    atomic_bool cancel=false;uint8_t au=1;X4T26Output output;X4T26BackendInfo info;
    X4T26Backend *b=create(&cancel);assert(!x4_t26_backend_open(b));
    assert(opened_config.api==2&&opened_config.depth==2&&opened_config.priority==700);
    assert(!x4_t26_backend_decode(b,0,&au,1,1,&output));
    assert(output.native_called&&!output.valid&&output.native_rc==0&&output.decode_call==1&&
        output.native_call==1&&x4_t26_backend_held_mask(b)==1);
    assert(!x4_t26_backend_decode(b,1,&au,1,2,&output));
    assert(output.valid&&output.layout_verified&&output.slot==0&&output.native_valid==1&&
        output.picture_count_native&&output.picture_count==1&&output.decode_call==2&&output.decode_us==100&&
        output.decode_begin_us+100==output.decode_end_us&&x4_t26_backend_held_mask(b)==2);
    assert(!x4_t26_backend_flush(b,2,&output));
    assert(output.from_flush&&output.valid&&output.slot==1&&output.decode_call==2&&output.native_call==3);
    assert(!x4_t26_backend_close(b));x4_t26_backend_info(b,&info);
    assert(info.closed&&info.owned_clean&&!info.retained&&info.allocations_before==5&&
        info.allocations_after==0&&info.owned_bytes_before==frame_bytes*5&&info.owned_bytes_after==0);
    /* A reused physical block is not an unallocated-after Query observation. */
    assert(info.allocation_checks[0].after_valid&&!info.allocation_checks[0].release_evidence);
    assert(!x4_t26_backend_destroy(&b)&&!b);
    b=create(&cancel);assert(!x4_t26_backend_open(b));mode=1;
    assert(!x4_t26_backend_decode(b,0,&au,1,1,&output));
    assert(output.native_called&&output.native_rc==-456&&output.local_result==0);
    assert(!x4_t26_backend_close(b)&&!x4_t26_backend_destroy(&b));mode=0;
    b=create(&cancel);assert(!x4_t26_backend_open(b));mode=2;
    assert(x4_t26_backend_decode(b,0,&au,1,1,&output)==X4_SPS_SIZE);
    assert(!output.native_called&&output.native_rc==X4_T26_PLATFORM_NOT_RUN&&output.local_result==X4_SPS_SIZE);
    assert(!x4_t26_backend_close(b)&&!x4_t26_backend_destroy(&b));mode=0;
    b=create(&cancel);assert(!x4_t26_backend_open(b));mode=3;
    assert(!x4_t26_backend_decode(b,0,&au,1,1,&output));
    assert(x4_t26_backend_decode(b,1,&au,1,2,&output)==X4_T26_BACKEND_LAYOUT&&
        output.native_called&&output.native_rc==0&&output.native_valid==1&&!output.layout_verified&&
        output.native_pointer&&output.native_bytes==frame_bytes);
    assert(!x4_t26_backend_close(b)&&!x4_t26_backend_destroy(&b));mode=0;
    b=create(&cancel);assert(!x4_t26_backend_open(b));delete_rc=-19;
    unsigned before_deletes=delete_calls;
    assert(x4_t26_backend_close(b)==-19);x4_t26_backend_info(b,&info);
    assert(info.retained&&!info.closed&&!info.owned_clean&&info.allocations_after==5);
    assert(x4_t26_backend_destroy(&b)==X4_T26_BACKEND_RETAINED&&b&&delete_calls==before_deletes+1);
    delete_rc=0;assert(!x4_t26_backend_close(b)&&!x4_t26_backend_destroy(&b));
    b=create(&cancel);assert(!x4_t26_backend_open(b));atomic_store(&cancel,true);
    unsigned before_calls=native_calls;
    assert(x4_t26_backend_decode(b,0,&au,1,1,&output)==X4_T26_BACKEND_CANCELLED&&
        !output.native_called&&native_calls==before_calls&&output.native_rc==X4_T26_PLATFORM_NOT_RUN);
    assert(!x4_t26_backend_close(b)&&!x4_t26_backend_destroy(&b));
    b=create(&cancel);assert(x4_t26_backend_open(b)==X4_T26_BACKEND_CANCELLED);
    assert(!x4_t26_backend_close(b)&&!x4_t26_backend_destroy(&b));
    assert(marker_calls>=6);
    puts("PASS T26 common backend delayed output, exact native/local results, cancellation and retained close");return 0;
}
