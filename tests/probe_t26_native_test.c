/* SPDX-License-Identifier: GPL-3.0-only */
/* Real live_h264.c + real depth ledger/SPS/capacity/plan. SDK loading,
 * dlsym, direct-memory and Videodec2 functions are host doubles, never PS4
 * acceptance, color or performance evidence. The existing harness is read
 * through a temporary copy with compute-double observation counters only. */
#define X4_NATIVE_MOCK_STREAM_FAULTS 1
#define main x4_t26_unused_legacy_native_main
#include "probe_native_mock_test.c"
#undef main
#include "../src/video/probe_t26_samples.h"
#include "../src/video/probe_t26_plan.h"

typedef struct {
    char operation[16][64];
    unsigned query_before[16],create_before[16],delete_before[16],release_before[16];
    unsigned count;
    const char *fail_operation;
    int failure;
    bool cancel_on_failure;
} T26NativeMarkers;
static T26NativeMarkers t26_markers;
static atomic_bool t26_cancel;
static int t26_before(void *context,const char *operation)
{
    CHECK(context==&t26_markers&&operation&&t26_markers.count<16);
    unsigned at=t26_markers.count++;
    int n=snprintf(t26_markers.operation[at],sizeof(t26_markers.operation[at]),"%s",operation);
    CHECK(n>0&&(size_t)n<sizeof(t26_markers.operation[at]));
    t26_markers.query_before[at]=query_calls;t26_markers.create_before[at]=create_calls;
    t26_markers.delete_before[at]=delete_calls;t26_markers.release_before[at]=release_calls;
    if(t26_markers.fail_operation&&!strcmp(operation,t26_markers.fail_operation)){
        if(t26_markers.cancel_on_failure)atomic_store_explicit(&t26_cancel,true,memory_order_release);
        return t26_markers.failure;
    }
    return 0;
}
static void t26_reset(void)
{
    reset_fixture();memset(&t26_markers,0,sizeof(t26_markers));atomic_store(&t26_cancel,false);
    t26_compute_queries=t26_queue_allocates=t26_queue_releases=0;
    memset(&t26_queue_config,0,sizeof(t26_queue_config));
}
static X4T26NativeOptions t26_options(const X4T26Config *config)
{
    return (X4T26NativeOptions){.config=*config,.before=t26_before,.context=&t26_markers};
}
static int t26_start_real(X4LiveVideo *v,const X4T26Config *config,unsigned clip)
{
    CHECK(clip<X4_T26_SAMPLE_COUNT&&x4_t26_config_valid(config));
    const X4T26GeneratedSample *sample=&x4_t26_generated_samples[clip];
    X4T26NativeOptions options=t26_options(config);
    return x4_live_video_t26_raw_start(v,sample->sps,sample->sps_size,&t26_cancel,&options);
}
static unsigned t26_marker_index(const char *name)
{
    for(unsigned i=0;i<t26_markers.count;++i)if(!strcmp(t26_markers.operation[i],name))return i;
    CHECK(false);return UINT_MAX;
}
static void t26_config_and_memory(const X4LiveVideo *v,const X4T26Config *config,unsigned clip)
{
    const X4T26GeneratedSample *sample=&x4_t26_generated_samples[clip];
    const VideoState *state=v->state;CHECK(state&&state->t26&&state->stream_raw);
    CHECK(query_calls==1&&create_calls==1&&!memcmp(&seen_config,&created_config,sizeof(seen_config)));
    CHECK(seen_config.thisSize==72&&seen_config.resourceType==config->resource_type&&seen_config.codecType==1);
    CHECK(seen_config.profile==77&&seen_config.maxLevel==42&&seen_config.maxDpbFrameCount==4);
    CHECK(seen_config.maxFrameWidth==(int)sample->coded_width&&seen_config.maxFrameHeight==(int)sample->coded_height);
    CHECK(seen_config.decodePipelineDepth==config->depth&&seen_config.cpuAffinityMask==config->affinity_mask&&
        seen_config.cpuThreadPriority==config->priority&&seen_config.optimizeProgressiveVideo&&!seen_config.checkMemoryType);
    CHECK(v->stream_info.attempt_count==1&&v->stream_info.active_depth==config->depth&&
        v->stream_info.requested_depth==config->depth&&!v->stream_info.depth_fallback_used&&!v->stream_info.fallback_used);
    unsigned inputs=config->depth==2?4:2,outputs=config->depth==2?5:4;
    CHECK(v->stream_info.input_count==inputs&&v->stream_info.output_count==outputs&&
        state->stream_depth.input_count==inputs&&state->stream_depth.output_count==outputs);
    CHECK(reserved_count()==inputs+outputs+3+(config->resource_type==1?1u:0u));
    for(unsigned i=0;i<inputs;++i)CHECK(state->input[i].allocated&&reservations[state->input[i].offset].type==0);
    for(unsigned i=0;i<outputs;++i)CHECK(state->output[i].allocated&&reservations[state->output[i].offset].type==config->output_memory_type);
    CHECK(reservations[state->cpu.offset].type==0&&reservations[state->gpu.offset].type==3&&
        reservations[state->shared.offset].type==0);
    CHECK(v->stream_info.decoder_cpu_priority==config->priority&&v->stream_info.decoder_cpu_affinity==config->affinity_mask);
    if(config->resource_type==1){
        CHECK(t26_compute_queries==1&&t26_queue_allocates==1&&seen_config.computeQueue==(void *)(uintptr_t)0x11);
        CHECK(t26_queue_config.thisSize==16&&t26_queue_config.computePipeId==config->pipe_id&&
            t26_queue_config.computeQueueId==config->queue_id&&!t26_queue_config.checkMemoryType);
        CHECK(t26_marker_index("v2.QueryComputeMemoryInfo")==0&&t26_marker_index("v2.AllocateComputeQueue")==1);
        CHECK(t26_marker_index("v2.QueryDecoderMemoryInfo")==2&&t26_marker_index("v2.CreateDecoder")==3);
    }else{
        CHECK(!t26_compute_queries&&!t26_queue_allocates&&!seen_config.computeQueue&&!state->queue&&!state->compute.allocated);
        CHECK(t26_marker_index("v2.QueryDecoderMemoryInfo")==0&&t26_marker_index("v2.CreateDecoder")==1);
    }
    unsigned q=t26_marker_index("v2.QueryDecoderMemoryInfo"),c=t26_marker_index("v2.CreateDecoder");
    CHECK(!t26_markers.query_before[q]&&!t26_markers.create_before[c]);
    CHECK(v->stream_info.query_result==0&&v->stream_info.create_result==0);
    X4T26Allocation allocations[X4_T26_ALLOCATION_MAX];unsigned count=x4_live_video_t26_allocations(v,allocations);
    CHECK(count==reserved_count());
    unsigned outputs_found=0;
    for(unsigned a=0;a<count;++a){
        CHECK(allocations[a].physical_offset>0&&allocations[a].physical_offset<32);
        const Reservation *reservation=&reservations[allocations[a].physical_offset];
        CHECK(reservation->reserved&&reservation->type==allocations[a].memory_type&&reservation->size==allocations[a].bytes);
        for(unsigned i=0;i<outputs;++i)if(state->output[i].offset==allocations[a].physical_offset)++outputs_found;
    }
    CHECK(outputs_found==outputs);
}
static void t26_decode_once(X4LiveVideo *v)
{
    static const uint8_t au[]={0,0,0,1,0x65,0x80};valid_output=true;
    const VideoState *state=v->state;const void *pointer=state->output[0].p;
    X4ProbeNativeOutput out={0};CHECK(!x4_live_video_stream_raw_decode(v,0,au,sizeof(au),123,false,&out));
    CHECK(decode_calls==1&&out.valid&&out.completed_output&&out.layout_verified&&out.native_result==0&&out.control_result==0);
    CHECK(out.t26_native_called&&out.t26_native_valid==1&&!out.t26_native_error&&out.t26_picture_count==1);
    CHECK(out.t26_native_pointer==pointer&&out.pixels==pointer&&out.t26_native_bytes==v->stream_info.queried_frame_bytes);
    CHECK(out.input_slot==0&&out.native_call==1&&!x4_live_video_stream_output_held_mask(v));
}
static void t26_close_clean(X4LiveVideo *v,const X4T26Config *config)
{
    unsigned before=t26_markers.count;
    CHECK(!x4_live_video_stop(v)&&!v->state&&!reserved_count());
    CHECK(delete_calls==1&&t26_markers.count==before+1&&!strcmp(t26_markers.operation[before],"DeleteDecoder"));
    CHECK(t26_markers.delete_before[before]==0&&t26_markers.release_before[before]==0);
    CHECK(t26_queue_releases==(config->resource_type==1?1u:0u));
    CHECK(v->stream_info.delete_decoder_result==0&&v->stream_info.close_result==0&&!v->stream_info.retained);
}
static void all_factor_configs(void)
{
    for(unsigned factor=0;factor<X4_T26_FACTOR_COUNT;++factor){
        t26_reset();X4T26Config config;CHECK(!x4_t26_factor_config(factor,&config));X4LiveVideo v={0};
        CHECK(!t26_start_real(&v,&config,factor==1?4:1));
        t26_config_and_memory(&v,&config,factor==1?4:1);t26_decode_once(&v);t26_close_clean(&v,&config);
        printf("PASS REAL_NATIVE_T26 factor=%u resource=%u depth=%u output_memory=%d priority=%d affinity=0x%llx pipe=%u queue=%u Query=0 Create=0 Decode=0 Delete=0 exact_same_config=1\n",
            factor,config.resource_type,config.depth,config.output_memory_type,config.priority,
            (unsigned long long)config.affinity_mask,config.pipe_id,config.queue_id);
    }
}
static void combined_resource_configs(void)
{
    for(unsigned resource=0;resource<=3;++resource)for(unsigned depth=1;depth<=2;++depth){
        t26_reset();X4T26Config config;x4_t26_config_base(&config);
        config.resource_type=resource;config.depth=depth;config.output_memory_type=3;
        config.affinity_mask=0x7f;config.priority=512;config.pipe_id=4;
        X4LiveVideo v={0};CHECK(!t26_start_real(&v,&config,1));
        t26_config_and_memory(&v,&config,1);t26_decode_once(&v);t26_close_clean(&v,&config);
    }
    puts("PASS all4 resource types x both depths with combined WC_GARLIC/affinity7f/priority512/pipe4: real allocation arguments and exact Query/Create");
}
static void query_create_rejections_do_not_fallback(void)
{
    for(unsigned resource=0;resource<=3;++resource)for(unsigned create=0;create<2;++create){
        t26_reset();X4T26Config config;x4_t26_config_base(&config);config.resource_type=resource;
        X4LiveVideo v={0};if(create)create_rc=-902;else query_rc=-901;
        int rc=t26_start_real(&v,&config,1);CHECK(rc==(create?-902:-901));
        CHECK(query_calls==1&&create_calls==(create?1u:0u)&&!decode_calls&&!delete_calls);
        CHECK(v.state&&v.stream_info.attempt_count==1&&!v.stream_info.depth_fallback_used&&!v.stream_info.fallback_used);
        CHECK(v.stream_info.requested_depth==2&&v.stream_info.active_depth==2&&seen_config.decodePipelineDepth==2);
        CHECK(v.stream_info.query_result==(create?0:-901));
        CHECK(v.stream_info.create_result==(create?-902:INT32_MIN));
        if(create)CHECK(!memcmp(&seen_config,&created_config,sizeof(seen_config)));
        CHECK(t26_compute_queries==(resource==1?1u:0u)&&t26_queue_allocates==(resource==1?1u:0u));
        unsigned markers=t26_markers.count;CHECK(!x4_live_video_stop(&v)&&!v.state&&!reserved_count());
        CHECK(!delete_calls&&t26_markers.count==markers&&t26_queue_releases==(resource==1?1u:0u));
        CHECK(query_calls==1&&create_calls==(create?1u:0u));
    }
    puts("PASS native Query/Create rejections for resource0/1/2/3: exact rc,one attempt,no fallback to another depth/profile/resource and clean caller-owned close");
}
static void delete_and_marker_failures_retain(void)
{
    for(unsigned marker=0;marker<2;++marker){
        t26_reset();X4T26Config config;x4_t26_config_base(&config);X4LiveVideo v={0};
        CHECK(!t26_start_real(&v,&config,1));unsigned owned=reserved_count();
        if(marker){t26_markers.fail_operation="DeleteDecoder";t26_markers.failure=-904;}
        else delete_rc=-903;
        CHECK(x4_live_video_stop(&v)==(marker?-904:-903));
        CHECK(v.state&&v.stream_info.retained&&reserved_count()==owned&&!release_calls&&!t26_queue_releases);
        CHECK(delete_calls==(marker?0u:1u));
        unsigned at=t26_marker_index("DeleteDecoder");CHECK(!t26_markers.delete_before[at]&&!t26_markers.release_before[at]);
        CHECK(v.stream_info.close_result==(marker?-904:-903));
        /* HOST fixture repair only, after proving production retains every
         * buffer. This is not a retry policy for a failed console teardown. */
        delete_rc=0;t26_markers.fail_operation=NULL;
        CHECK(!x4_live_video_stop(&v)&&!v.state&&!reserved_count());
    }
    puts("PASS real Delete failure and durable-marker failure: verbatim returns,zero premature release and retained native buffers");
}
static void partial_start_cleanup_and_uncertain_handle(void)
{
    for(unsigned cancel=0;cancel<2;++cancel){
        t26_reset();X4T26Config config;x4_t26_config_base(&config);X4LiveVideo v={0};
        if(cancel){t26_markers.fail_operation="v2.CreateDecoder";t26_markers.failure=3;
            t26_markers.cancel_on_failure=true;}
        else query_rc=-905;
        CHECK(t26_start_real(&v,&config,1)==(cancel?3:-905));
        VideoState *state=v.state;CHECK(state&&state->t26&&state->stream_raw&&state->queue&&!state->decoder);
        CHECK(t26_compute_queries==1&&t26_queue_allocates==1&&!t26_queue_releases&&query_calls==1&&
            !create_calls&&!decode_calls&&!delete_calls&&!release_calls);
        CHECK(v.stream_info.query_result==(cancel?0:-905)&&v.stream_info.create_result==INT32_MIN&&
            v.stream_info.delete_decoder_result==INT32_MIN&&!v.stream_info.creation_uncertain);
        if(cancel)CHECK(atomic_load_explicit(&t26_cancel,memory_order_acquire)&&
            state->input[3].allocated&&state->output[4].allocated&&state->cpu.allocated&&
            state->gpu.allocated&&state->shared.allocated);
        else CHECK(!atomic_load_explicit(&t26_cancel,memory_order_acquire)&&state->compute.allocated&&
            !state->input[0].allocated&&!state->output[0].allocated);
        unsigned owned=reserved_count(),markers=t26_markers.count;CHECK(owned);
        CHECK(!x4_live_video_stop(&v)&&!v.state&&!reserved_count());
        CHECK(release_calls==owned&&t26_queue_releases==1&&!delete_calls&&t26_markers.count==markers);
        CHECK(v.stream_info.query_result==(cancel?0:-905)&&v.stream_info.create_result==INT32_MIN&&
            v.stream_info.delete_decoder_result==INT32_MIN&&v.stream_info.release_queue_result==0&&
            v.stream_info.close_result==0&&!v.stream_info.retained&&v.stream_info.attempts[0].cleanup==0);
    }
    /* The caller-owned cleanup does not invent ownership for a successful
     * Create with no handle. The real stop must retain that uncertain case. */
    t26_reset();X4T26Config config;x4_t26_config_base(&config);X4LiveVideo v={0};
    stream_create_success_null=true;
    CHECK(t26_start_real(&v,&config,1)<0&&v.state&&v.stream_info.creation_uncertain&&
        v.stream_info.query_result==0&&v.stream_info.create_result==0);
    unsigned owned=reserved_count();CHECK(owned&&((VideoState *)v.state)->queue);
    CHECK(x4_live_video_stop(&v)<0&&v.state&&v.stream_info.retained&&reserved_count()==owned&&
        !delete_calls&&!release_calls&&!t26_queue_releases);
    /* HOST repair only after proving retention; never a console retry rule. */
    v.stream_info.creation_uncertain=false;
    CHECK(!x4_live_video_stop(&v)&&!v.state&&!reserved_count()&&t26_queue_releases==1&&!delete_calls);
    puts("PASS REAL_NATIVE_T26 partial start: Query rejection/pre-Create cancellation preserve native returns and release queue/buffers without Delete; uncertain Create handle still retains");
}
static void depth_one_no_output_requires_delete(void)
{
    static const uint8_t au[]={0,0,0,1,0x65,0x80};
    t26_reset();X4T26Config config;x4_t26_config_base(&config);config.depth=1;X4LiveVideo v={0};
    CHECK(!t26_start_real(&v,&config,1));unsigned owned=reserved_count();valid_output=false;
    VideoState *state=v.state;CHECK(state&&state->input_slot==0);
    X4ProbeNativeOutput out={0};int rc=x4_live_video_stream_raw_decode(&v,0,au,sizeof(au),1,false,&out);
    CHECK(rc<0&&out.native_result==0&&out.control_result==rc&&out.ownership_uncertain&&
        out.frame_class==X4_STREAM_FRAME_FATAL&&out.t26_native_called&&!out.t26_native_valid&&
        !out.t26_native_error&&!out.t26_picture_count&&!out.valid&&!out.layout_verified);
    CHECK(v.error==rc&&decode_calls==1&&v.no_picture_calls==1&&state->input_slot==0&&reserved_count()==owned);
    /* The same real raw function refuses any further offered slot before
     * Delete, even if the SDK double would now produce a valid picture. */
    valid_output=true;out=(X4ProbeNativeOutput){0};
    CHECK(x4_live_video_stream_raw_decode(&v,1,au,sizeof(au),2,false,&out)<0&&
        !out.t26_native_called&&decode_calls==1&&reserved_count()==owned);
    t26_close_clean(&v,&config);
    /* Empty EOF Flush remains an ordinary observation in T26 depth1. */
    t26_reset();v=(X4LiveVideo){0};CHECK(!t26_start_real(&v,&config,1));valid_output=false;out=(X4ProbeNativeOutput){0};
    CHECK(!x4_live_video_stream_raw_decode(&v,0,NULL,0,0,true,&out)&&out.t26_native_called&&
        !out.native_result&&!out.control_result&&!out.valid&&!v.error&&!decode_calls&&flush_calls==1);
    t26_close_clean(&v,&config);
    /* Delayed output is normal for T26 depth2 and retains the offered slot. */
    t26_reset();v=(X4LiveVideo){0};config.depth=2;CHECK(!t26_start_real(&v,&config,1));valid_output=false;out=(X4ProbeNativeOutput){0};
    CHECK(!x4_live_video_stream_raw_decode(&v,0,au,sizeof(au),1,false,&out)&&out.t26_native_called&&
        !out.native_result&&!out.control_result&&!out.valid&&!v.error&&decode_calls==1&&
        x4_live_video_stream_output_held_mask(&v)==1);
    t26_close_clean(&v,&config);
    /* A normal streaming start in this same TU keeps its original depth1
     * no-output behavior: both compile-time and per-instance guards apply. */
    t26_reset();v=(X4LiveVideo){0};valid_output=false;
    const X4T26GeneratedSample *sample=&x4_t26_generated_samples[1];
    CHECK(!x4_live_video_stream_raw_start_cpu_depth(&v,sample->sps,sample->sps_size,&t26_cancel,0,1));
    CHECK(v.state&&!((VideoState *)v.state)->t26);out=(X4ProbeNativeOutput){0};
    CHECK(!x4_live_video_stream_raw_decode(&v,0,au,sizeof(au),1,false,&out)&&!out.native_result&&
        !out.control_result&&!v.error&&decode_calls==1);
    CHECK(!x4_live_video_stop(&v)&&!v.state&&!reserved_count()&&delete_calls==1);
    puts("PASS REAL_NATIVE_T26 depth1 no-output: native_rc0/local contract,sticky rejection before another SDK Decode,Delete clean; depth2/Flush/normal streaming unchanged");
}
int main(void)
{
    CHECK(X4_VIDEO_PROBE==1&&X4_PROBE_T26==1&&X4_STREAM_PROFILE==1&&X4_T26_SAMPLE_COUNT==5);
    all_factor_configs();combined_resource_configs();query_create_rejections_do_not_fallback();delete_and_marker_failures_retain();
    partial_start_cleanup_and_uncertain_handle();depth_one_no_output_requires_delete();
    t26_reset();printf("PASS T26 REAL live_h264 native adapter: %u checks; SDK/native functions are host doubles; no console acceptance/throughput/color claim\n",atomic_load(&checks));
    return 0;
}
