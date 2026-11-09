/* SPDX-License-Identifier: GPL-3.0-only */
#include "probe_t26_backend.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int x4_t26_backend_nv12_layout(const X4StreamSps *s,unsigned width,unsigned height,
    unsigned pitch,size_t returned,size_t owned,X4VideoNv12Layout *out)
{
    if(out)memset(out,0,sizeof(*out));
    if(!s||!out||s->chroma_format_idc!=1||s->bit_depth_luma!=8||s->bit_depth_chroma!=8||
        s->frame_mbs_only_flag!=1||width!=s->coded_width||height!=s->coded_height||
        !width||!height||width>1920||height>1088||!s->visible_width||!s->visible_height||
        s->crop_left>width||s->crop_right>width-s->crop_left||
        s->visible_width!=width-s->crop_left-s->crop_right||
        s->crop_top>height||s->crop_bottom>height-s->crop_top||
        s->visible_height!=height-s->crop_top-s->crop_bottom)return X4_T26_BACKEND_LAYOUT;
    X4VideoCapacity capacity;
    if(x4_video_capacity_plan(1,&capacity))return X4_T26_BACKEND_LAYOUT;
    X4VideoNv12Request request={.visible_width=s->visible_width,.visible_height=s->visible_height,
        .coded_width=width,.coded_height=height,.storage_height=height,.pitch=pitch,
        .crop_x=s->crop_left,.crop_y=s->crop_top,.format=X4_VIDEO_FORMAT_NV12,
        .mapping=X4_VIDEO_MAPPING_EXPLICIT_NV12,.source_bytes=returned,.staging_bytes=owned};
    return x4_video_nv12_layout(&request,&capacity,capacity.rgb_pixels,out)?X4_T26_BACKEND_LAYOUT:0;
}

#ifndef X4_T26_BACKEND_HELPERS_TEST
struct X4T26Backend {
    X4T26BackendOptions options;uint8_t sps[X4_T26_BACKEND_SPS_MAX];
    X4T26BackendInfo info;X4T26Platform *platform;X4T26V1 *v1;X4LiveVideo v2;
    bool snapshot_taken;uint64_t native_call;
};
static unsigned allocations(X4T26Backend *b,X4T26Allocation *out)
{
    if(b->options.config.api==X4_T26_API_V2)return x4_live_video_t26_allocations(&b->v2,out);
    X4T26V1Memory memory[X4_T26_V1_MEMORY_COUNT];
    size_t count=x4_t26_v1_allocations(b->v1,memory,X4_T26_V1_MEMORY_COUNT);
    if(count>X4_T26_V1_MEMORY_COUNT)return X4_T26_ALLOCATION_MAX+1;
    for(size_t i=0;i<count;++i)out[i]=(X4T26Allocation){.physical_offset=memory[i].offset,
        .bytes=memory[i].bytes,.memory_type=0};
    return (unsigned)count;
}
static uint64_t total_bytes(const X4T26Allocation *a,unsigned count)
{
    uint64_t sum=0;if(count>X4_T26_ALLOCATION_MAX)return UINT64_MAX;
    for(unsigned i=0;i<count;++i){if(a[i].bytes>UINT64_MAX-sum)return UINT64_MAX;sum+=a[i].bytes;}
    return sum;
}
static void refresh(X4T26Backend *b)
{
    if(b->options.config.api==X4_T26_API_V1)x4_t26_v1_info(b->v1,&b->info.v1);
    else x4_live_video_stream_info(&b->v2,&b->info.v2);
    x4_t26_platform_info(b->platform,&b->info.platform);
}
int x4_t26_backend_create(X4T26Backend **out,const X4T26BackendOptions *options)
{
    if(!out)return X4_T26_BACKEND_ARGUMENT;
    *out=NULL;
    if(!options||!options->before||!options->cancel||!options->sps||!options->sps_bytes||
        options->sps_bytes>X4_T26_BACKEND_SPS_MAX||!x4_t26_config_valid(&options->config)||
        (options->config.api==X4_T26_API_V1&&!options->input_arena_bytes))return X4_T26_BACKEND_ARGUMENT;
    X4T26Backend *b=calloc(1,sizeof(*b));if(!b)return X4_T26_BACKEND_MEMORY;
    b->options=*options;memcpy(b->sps,options->sps,options->sps_bytes);b->options.sps=b->sps;
    b->info.config=options->config;b->info.open_result=b->info.close_result=X4_T26_PLATFORM_NOT_RUN;
    b->info.largest_before_rc=b->info.largest_after_rc=X4_T26_PLATFORM_NOT_RUN;
    bool complete=false;int rc=x4_stream_sps_inspect(b->sps,options->sps_bytes,&b->info.sps,&complete);
    if(!rc&&(!complete||b->info.sps.profile_idc!=77||b->info.sps.level_idc!=42||
        b->info.sps.max_num_ref_frames!=1||b->info.sps.pic_order_cnt_type!=2||
        b->info.sps.num_reorder_frames!=0))rc=X4_T26_BACKEND_ARGUMENT;
    if(!rc)rc=x4_t26_platform_create(&b->platform,options->before,options->context);
    if(!rc&&options->config.api==X4_T26_API_V1){
        X4T26V1Ops ops;x4_t26_platform_v1_ops(b->platform,&ops);
        X4T26V1Options v1={.width=b->info.sps.coded_width,.coded_height=b->info.sps.coded_height,
            .input_arena_bytes=options->input_arena_bytes,.native_budget_bytes=X4_T26_V1_NATIVE_LIMIT};
        rc=x4_t26_v1_create(&b->v1,&ops,&v1);
    }
    if(rc){x4_t26_platform_free(&b->platform);free(b);return rc;}
    b->info.output_count=options->config.api==X4_T26_API_V1?5:options->config.depth+3;
    *out=b;return 0;
}
int x4_t26_backend_open(X4T26Backend *b)
{
    if(!b||b->info.attempted)return X4_T26_BACKEND_ARGUMENT;
    b->info.attempted=true;
    if(atomic_load_explicit(b->options.cancel,memory_order_acquire))
        return b->info.open_result=X4_T26_BACKEND_CANCELLED;
    int rc;
    if(b->options.config.api==X4_T26_API_V1)rc=x4_t26_v1_open(b->v1);
    else{
        X4T26NativeOptions options={.config=b->options.config,.before=b->options.before,.context=b->options.context};
        rc=x4_live_video_t26_raw_start(&b->v2,b->sps,b->options.sps_bytes,b->options.cancel,&options);
    }
    b->info.open_result=rc;b->info.opened=rc==0;refresh(b);return rc;
}
static void initialize_output(X4T26Output *out,bool flush)
{
    *out=(X4T26Output){.native_rc=X4_T26_PLATFORM_NOT_RUN,.slot=UINT_MAX,.from_flush=flush};
}
static size_t v1_owned_bytes(X4T26Backend *b,const void *pointer)
{
    X4T26V1Memory m[X4_T26_V1_MEMORY_COUNT];size_t n=x4_t26_v1_allocations(b->v1,m,X4_T26_V1_MEMORY_COUNT);
    if(n>X4_T26_V1_MEMORY_COUNT)return 0;
    for(size_t i=0;i<n;++i)if(m[i].pointer==pointer&&m[i].allocated&&m[i].bytes<=SIZE_MAX)return (size_t)m[i].bytes;
    return 0;
}
static int decode_or_flush(X4T26Backend *b,unsigned offered,const void *au,size_t bytes,
    uint64_t pts,bool flush,X4T26Output *out)
{
    if(!out)return X4_T26_BACKEND_ARGUMENT;
    initialize_output(out,flush);
    if(!b||!b->info.opened||b->info.closed||offered>=b->info.output_count||(!flush&&(!au||!bytes)))
        return out->local_result=X4_T26_BACKEND_ARGUMENT;
    if(atomic_load_explicit(b->options.cancel,memory_order_acquire))
        return out->local_result=X4_T26_BACKEND_CANCELLED;
    int rc;
    if(b->options.config.api==X4_T26_API_V1){
        X4T26V1Output native;
        rc=flush?x4_t26_v1_flush(b->v1,offered,&native):x4_t26_v1_decode(b->v1,offered,au,bytes,pts,&native);
        out->native_called=native.native_call!=0;
        if(out->native_called)out->native_rc=native.native_rc;
        out->local_result=rc&&(!out->native_called||rc!=native.native_rc)?rc:0;
        out->valid=native.native_valid!=0;out->error=native.native_error!=0;
        out->native_valid=native.native_valid;out->native_error=native.native_error;
        out->picture_count=native.native_valid?1:0;out->picture_count_native=false;
        out->native_pointer=native.pointer;
        if(native.bytes<=SIZE_MAX)out->native_bytes=(size_t)native.bytes;
        out->pointer=native.pointer;out->slot=native.slot;out->width=native.width;out->height=native.height;out->pitch=native.pitch;
        out->owned_bytes=v1_owned_bytes(b,native.pointer);
        if(native.bytes<=SIZE_MAX)out->returned_bytes=(size_t)native.bytes;
        else out->local_result=X4_T26_BACKEND_LAYOUT;
        out->decode_begin_us=native.decode_begin_us;out->decode_end_us=native.decode_end_us;
        out->decode_us=native.decode_us;out->native_call=native.native_call;out->decode_call=native.decode_call;
        out->v1_metadata=native.valid;out->v1_pts=native.pts;out->v1_attached=native.attached;out->v1_avc=native.avc;
    }else{
        X4ProbeNativeOutput native={0};
        rc=x4_live_video_stream_raw_decode(&b->v2,offered,au,bytes,pts,flush,&native);
        out->native_called=native.t26_native_called;
        if(out->native_called){out->native_rc=native.native_result;out->native_call=++b->native_call;}
        out->native_valid=native.t26_native_valid;out->native_error=native.t26_native_error;
        out->picture_count=native.t26_picture_count;out->picture_count_native=true;
        out->native_pointer=native.t26_native_pointer;out->native_bytes=native.t26_native_bytes;
        out->valid=out->native_valid!=0;out->error=out->native_error!=0;
        out->local_result=native.control_result;
        if(rc&&(!out->native_called||rc!=native.native_result))out->local_result=rc;
        out->pointer=native.pixels;out->slot=native.slot;out->width=native.native_width;out->height=native.native_height;
        out->pitch=native.pitch;out->owned_bytes=native.owned_bytes;out->returned_bytes=native.returned_bytes;
        out->decode_end_us=native.decode_end_us;out->decode_us=native.decode_us;out->decode_call=native.native_call;
        if(native.decode_end_us>=native.decode_us)out->decode_begin_us=native.decode_end_us-native.decode_us;
        else out->local_result=X4_T26_BACKEND_LAYOUT;
    }
    if(out->native_called&&out->native_rc==0&&out->valid&&!out->local_result){
        int layout=out->pointer?x4_t26_backend_nv12_layout(&b->info.sps,out->width,out->height,
            out->pitch,out->returned_bytes,out->owned_bytes,&out->layout):X4_T26_BACKEND_LAYOUT;
        if(layout)out->local_result=layout;
        else out->layout_verified=true;
    }
    if(out->local_result)b->info.local_result=out->local_result;
    return out->local_result?out->local_result:rc;
}
int x4_t26_backend_decode(X4T26Backend *b,unsigned offered,const void *au,size_t bytes,uint64_t pts,X4T26Output *out)
{return decode_or_flush(b,offered,au,bytes,pts,false,out);}
int x4_t26_backend_flush(X4T26Backend *b,unsigned offered,X4T26Output *out)
{return decode_or_flush(b,offered,NULL,0,0,true,out);}
uint32_t x4_t26_backend_held_mask(const X4T26Backend *b)
{
    if(!b)return 0;
    return b->options.config.api==X4_T26_API_V1?x4_t26_v1_held_mask(b->v1):x4_live_video_stream_output_held_mask(&b->v2);
}
void x4_t26_backend_info(X4T26Backend *b,X4T26BackendInfo *out)
{if(out){if(b){refresh(b);*out=b->info;}else memset(out,0,sizeof(*out));}}
void x4_t26_backend_cpu(X4T26Backend *b,X4T26CpuSnapshot *out)
{x4_t26_platform_cpu(b?b->platform:NULL,out);}
static void before_close(X4T26Backend *b)
{
    if(b->snapshot_taken)return;
    b->snapshot_taken=true;
    X4T26Allocation a[X4_T26_ALLOCATION_MAX];unsigned n=allocations(b,a);
    b->info.allocations_before=n;b->info.owned_bytes_before=total_bytes(a,n);
    b->info.largest_before_rc=x4_t26_platform_largest(b->platform,&b->info.largest_before_bytes,&b->info.largest_before_called);
    if(n>X4_T26_ALLOCATION_MAX){b->info.local_result=X4_T26_BACKEND_RETAINED;return;}
    for(unsigned i=0;i<n;++i){
        X4T26AllocationCheck *c=&b->info.allocation_checks[i];c->allocation=a[i];
        c->after_rc=X4_T26_PLATFORM_NOT_RUN;
        c->before_rc=x4_t26_platform_query(b->platform,a[i].physical_offset,&c->before,&c->before_called);
        c->before_valid=c->before_called&&c->before_rc==0&&x4_t26_query_contains(&c->before,&a[i]);
    }
}
int x4_t26_backend_close(X4T26Backend *b)
{
    if(!b)return X4_T26_BACKEND_ARGUMENT;
    if(b->info.closed)return 0;
    before_close(b);
    int rc=0;
    if(b->options.config.api==X4_T26_API_V1)rc=x4_t26_v1_close(b->v1);
    else if(b->v2.state){
        rc=b->options.before(b->options.context,"v2.delete");
        if(!rc)rc=x4_live_video_stop(&b->v2);
    }
    refresh(b);X4T26Allocation a[X4_T26_ALLOCATION_MAX];unsigned n=allocations(b,a);
    b->info.allocations_after=n;b->info.owned_bytes_after=total_bytes(a,n);
    bool live=b->options.config.api==X4_T26_API_V1?b->info.v1.decoder_live:b->v2.state!=NULL;
    b->info.owned_clean=rc==0&&n==0&&!live;
    b->info.closed=b->info.owned_clean;b->info.retained=!b->info.owned_clean;
    b->info.close_result=rc?rc:b->info.owned_clean?0:X4_T26_BACKEND_RETAINED;
    unsigned checks=b->info.allocations_before;
    if(checks>X4_T26_ALLOCATION_MAX)checks=X4_T26_ALLOCATION_MAX;
    for(unsigned i=0;i<checks;++i){
        X4T26AllocationCheck *c=&b->info.allocation_checks[i];
        c->after_rc=x4_t26_platform_query(b->platform,c->allocation.physical_offset,&c->after,&c->after_called);
        c->after_valid=c->after_called&&c->after_rc==0&&x4_t26_query_contains(&c->after,&c->allocation);
        c->release_evidence=b->info.owned_clean&&c->after_called&&x4_t26_query_release_evidence(c->before_valid,c->after_rc);
    }
    b->info.largest_after_rc=x4_t26_platform_largest(b->platform,&b->info.largest_after_bytes,&b->info.largest_after_called);
    refresh(b);return b->info.close_result;
}
int x4_t26_backend_destroy(X4T26Backend **pointer)
{
    if(!pointer||!*pointer)return X4_T26_BACKEND_ARGUMENT;
    X4T26Backend *b=*pointer;
    if((b->info.attempted&&!b->info.closed)||b->info.retained||b->v2.state)return X4_T26_BACKEND_RETAINED;
    if(b->v1){
        X4T26V1Info info;x4_t26_v1_info(b->v1,&info);
        if(info.decoder_live||info.allocation_count)return X4_T26_BACKEND_RETAINED;
        int rc=x4_t26_v1_destroy(&b->v1);if(rc)return rc;
    }
    x4_t26_platform_free(&b->platform);free(b);*pointer=NULL;return 0;
}
#endif
#endif
