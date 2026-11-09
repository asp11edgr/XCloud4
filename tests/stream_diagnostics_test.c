/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/video/stream_diagnostics.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
static void *receive(void *arg)
{ for(unsigned i=0;i<1000;++i)x4_stream_diagnostics_received(arg,100,1000100); return NULL; }
static void diagnostics_storage_contract(void)
{
    /* Preserve every old numeric ID and its historical prefix allowance.
     * T27 adds fixed heap diagnostics; native/stack budgets are unaffected. */
    const X4StreamStage legacy_ids[]={X4_STREAM_DECODE,X4_STREAM_CONVERSION,
        X4_STREAM_STRIPE0,X4_STREAM_STRIPE1,X4_STREAM_DECODE_TO_FLIP,
        X4_STREAM_CONVERT_TO_SUBMIT,X4_STREAM_FLIP_CALL,X4_STREAM_SUBMIT_TO_OBSERVATION,
        X4_STREAM_PRESENT_INTERVAL,X4_STREAM_DECODE_IDR,X4_STREAM_DECODE_NON_IDR,
        X4_STREAM_FLIP_END_TO_CONVERSION,X4_STREAM_CONTROLLER_READ_INTERVAL,
        X4_STREAM_DECODE_WITH_OUTPUT,X4_STREAM_DECODE_WITHOUT_OUTPUT,X4_STREAM_NATIVE_OUTPUT_INTERVAL};
    for(unsigned i=0;i<sizeof(legacy_ids)/sizeof(legacy_ids[0]);++i)assert((unsigned)legacy_ids[i]==i);
    assert(X4_STREAM_DECODER_SLOT_WAIT==16&&X4_STREAM_DECODER_RING_GATE_WAIT==17&&X4_STREAM_STAGE_COUNT==18);
    /* Typed mirrors account for ABI padding/atomic storage instead of raising
     * the old <320 KiB assertion to an unexamined larger number. */
    typedef struct {uint64_t bins[X4_STREAM_METRIC_BINS],count,sum,maximum,overflow;} StageStorage;
    typedef struct {StageStorage timing[3];uint64_t frames,first_us,last_us;} ModeStorage;
    typedef struct {
        unsigned policy,near30_streak,near60_streak;bool have_bucket;
        uint64_t bucket,current_count,complete_buckets,zero_buckets,other_buckets,late_events;
        uint64_t state_since_us,state_us[2],transitions,history_omitted,notification_omitted;
        X4StreamVsyncTransition history[1024],events[64];
        atomic_bool effective;atomic_uint_fast64_t written,read;
    } AutoStorage;
    typedef struct {uint64_t samples,held_native[6],occupied_ring[6],ring_full_drops;} OwnershipStorage;
    typedef struct {
        StageStorage ready,interval;
        uint64_t ready_ms[32],ready_depth[5],driver_depth[9],gpu_depth[9];
        uint64_t mode_frames[4];
        uint64_t frames,first_us,last_us,last_vsync_ptime,over20,interval_invalid;
        uint64_t native_times,fallback_times,ready_invalid,repeat_estimate,repeat_intervals,repeat_unknown;
        uint64_t queue_samples,queue_invalid,recovery_drops,mailbox_drops,ring_full_drops,ring_latest_drops;
        uint64_t next_log_us,log_omitted;
        unsigned ready_max,driver_max,gpu_max;
        bool final_logged;
    } PacingStorage;
    const size_t base_bytes=326104;
    const size_t stage_bytes=(X4_STREAM_METRIC_BINS+4u)*sizeof(uint64_t);
    const size_t old_depth_growth=3u*stage_bytes;
    const size_t legacy_bytes=base_bytes+old_depth_growth;
    const size_t new_stage_bytes=2u*stage_bytes;
    const size_t mode_bytes=3u*sizeof(ModeStorage);
    const size_t owner_wait_bytes=2u*stage_bytes;
    const size_t t27_growth=new_stage_bytes+sizeof(AutoStorage)+mode_bytes+owner_wait_bytes+sizeof(OwnershipStorage);
    const size_t t42_growth=sizeof(ModeStorage)+sizeof(PacingStorage)+sizeof(atomic_uint_fast64_t);
    const size_t actual=x4_stream_diagnostics_storage_bytes();
    assert(sizeof(StageStorage)==stage_bytes&&stage_bytes==16416&&old_depth_growth==49248);
    assert(legacy_bytes==375352&&legacy_bytes-old_depth_growth<320u*1024u);
    assert(sizeof(X4StreamVsyncTransition)==40&&sizeof(AutoStorage)==43656);
    assert(sizeof(ModeStorage)==49272&&mode_bytes==147816&&sizeof(OwnershipStorage)==112);
    assert(new_stage_bytes==32832&&owner_wait_bytes==32832&&t27_growth==257248);
    assert(legacy_bytes+t27_growth==632600);
    assert(sizeof(PacingStorage)==33480&&t42_growth==82760);
    assert(actual==legacy_bytes+t27_growth+t42_growth&&actual==715360);
    printf("actual diagnostics storage: %zu bytes; legacy %zu (base %zu + depth stages %zu), T27 %zu (new stages %zu + Auto %zu + modes %zu + ownership waits %zu + counters %zu); old prefix allowance 327680\n",
        actual,legacy_bytes,base_bytes,old_depth_growth,t27_growth,new_stage_bytes,sizeof(AutoStorage),mode_bytes,owner_wait_bytes,sizeof(OwnershipStorage));
    printf("T42 fixed diagnostics growth %zu = fourth presentation mode %zu + pacing %zu + atomic AU mirror %zu\n",
        t42_growth,sizeof(ModeStorage),sizeof(PacingStorage),sizeof(atomic_uint_fast64_t));
}
static void new_diagnostics(const char *path)
{
    X4StreamDiagnostics *d=x4_stream_diagnostics_create(0);assert(d);
    diagnostics_storage_contract();
    x4_stream_diagnostics_keyframe_request(d,100);
    x4_stream_diagnostics_keyframe_request(d,300100);
    x4_stream_diagnostics_keyframe_request(d,600100);
    for(unsigned i=0;i<30;++i)x4_stream_diagnostics_au_received(d,false,1000+i);
    for(unsigned i=0;i<60;++i)x4_stream_diagnostics_au_received(d,false,1001000+i);
    x4_stream_diagnostics_au_received(d,true,2000100);
    x4_stream_diagnostics_keyframe_request(d,1900100); /* Simulated late publication across prior IDR. */
    x4_stream_diagnostics_au_received(d,true,2100100);
    for(unsigned i=0;i<38;++i)x4_stream_diagnostics_au_received(d,false,2101000+i);
    x4_stream_diagnostics_au_received(d,false,4001000);
    x4_stream_diagnostics_present(d,1000);
    x4_stream_diagnostics_present(d,201001); /* unknown */
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_RTP_GAP);
    x4_stream_diagnostics_present(d,402002);
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_WAIT_IDR|X4_STREAM_PAUSE_DECODER);
    x4_stream_diagnostics_present(d,603003); /* mixed, not asserted causality */
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_WAIT_IDR);
    x4_stream_diagnostics_present(d,804004);
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_DECODER);
    x4_stream_diagnostics_present(d,1005005);
    x4_stream_diagnostics_evidence(d,X4_STREAM_PAUSE_PRESENTATION);
    x4_stream_diagnostics_present(d,1206006);
    /* Pending requests and over-capacity events remain explicit. */
    for(unsigned i=0;i<131;++i)x4_stream_diagnostics_keyframe_request(d,3000000+i);
    char name[1024];int n=snprintf(name,sizeof(name),"%s.new",path);assert(n>0&&(size_t)n<sizeof(name));
    FILE *f=fopen(name,"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,4500000));assert(!fclose(f));
    x4_stream_diagnostics_free(d);
    d=x4_stream_diagnostics_create(0);assert(d);
    for(unsigned i=0;i<260;++i){x4_stream_diagnostics_evidence(d,i&15u);x4_stream_diagnostics_present(d,1000+(uint64_t)i*200001);}
    n=snprintf(name,sizeof(name),"%s.retention",path);assert(n>0&&(size_t)n<sizeof(name));
    f=fopen(name,"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,53000000));assert(!fclose(f));
    x4_stream_diagnostics_free(d);
    d=x4_stream_diagnostics_create(0);assert(d);
    for(unsigned i=0;i<65536;++i)x4_stream_diagnostics_au_received(d,false,1000);
    x4_stream_diagnostics_au_received(d,false,UINT64_C(3600000001));
    n=snprintf(name,sizeof(name),"%s.capacity",path);assert(n>0&&(size_t)n<sizeof(name));
    f=fopen(name,"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,UINT64_C(3601000000)));assert(!fclose(f));
    x4_stream_diagnostics_free(d);
    d=x4_stream_diagnostics_create(0);assert(d);
    for(unsigned second=0;second<125;++second)
        for(unsigned i=0;i<1+second%3;++i)
            x4_stream_diagnostics_au_received(d,true,(uint64_t)second*1000000+1000+i*10000);
    x4_stream_diagnostics_au_received(d,true,125100000);
    n=snprintf(name,sizeof(name),"%s.idr-minute",path);assert(n>0&&(size_t)n<sizeof(name));
    f=fopen(name,"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,125500000));assert(!fclose(f));
    x4_stream_diagnostics_free(d);
}
int main(int argc,char **argv)
{
    assert(argc==2); X4StreamDiagnostics *d=x4_stream_diagnostics_create(100);
    assert(d); diagnostics_storage_contract();
    for(unsigned i=0;i<100;++i){
        x4_stream_diagnostics_record(d,X4_STREAM_DECODE,1000+i*100);
        x4_stream_diagnostics_record(d,i&1?X4_STREAM_DECODE_IDR:X4_STREAM_DECODE_NON_IDR,1000+i*100);
    }
    x4_stream_diagnostics_record(d,X4_STREAM_CONVERSION,999999);
    pthread_t threads[4];for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,receive,d));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));
    x4_stream_diagnostics_received(d,7,UINT64_C(3600000100));
    x4_stream_diagnostics_present(d,1000); x4_stream_diagnostics_present(d,101000);
    x4_stream_diagnostics_present(d,201001); /* strict >100 ms only */
    FILE *f=fopen(argv[1],"wb");assert(f);assert(!x4_stream_diagnostics_write(f,d,2000100));assert(!fclose(f));
    assert(x4_stream_diagnostics_presentations(d)==3 && x4_stream_diagnostics_first_present(d)==1000);
    x4_stream_diagnostics_free(d);new_diagnostics(argv[1]);puts("PASS cumulative metrics, AU cadence, pending/overflow keyframe-IDR timing and bounded unknown/mixed pause evidence");return 0;
}
