/* SPDX-License-Identifier: GPL-3.0-only */
#include "synced42_sim.h"
#include "../src/video/video_pipeline.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ascending(const void *a,const void *b)
{unsigned x=*(const unsigned *)a,y=*(const unsigned *)b;return (x>y)-(x<y);}
static unsigned percentile(unsigned *values,size_t count,unsigned p)
{assert(count);qsort(values,count,sizeof(*values),ascending);return values[(count*p+99u)/100u-1u];}

static uint32_t random_state=UINT32_C(0x42a73119);
static unsigned draw(void)
{random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}

static void arrivals(Sync42Arrival *frames,unsigned *decode)
{
    uint64_t previous=0;
    for(unsigned i=0;i<SYNC42_FRAMES;++i){
        unsigned q=draw()%100u;
        unsigned duration=q<50u?3000u+q*60u:q<95u?6000u+(q-50u)*200u:q<99u?15000u+(q-95u)*2400u:24000u;
        uint64_t source=1000000u+(uint64_t)i*1000000u/60u;
        uint64_t ready=source+duration+draw()%2001u;
        /* One 50ms delivery hold per 15s, followed by a 2/3-image burst.
         * Decode durations stay measurable independently of delivery jitter. */
        unsigned phase=i%900u;
        if(phase>=897u)ready=1000000u+(uint64_t)(i+900u-phase)*1000000u/60u+5000u;
        if(ready<=previous)ready=previous+1u;
        frames[i]=(Sync42Arrival){source,ready,duration};decode[i]=duration;previous=ready;
    }
}

static X4VideoPipelineLease ready(X4VideoPipelineDisplay *d,uint64_t id,uint64_t at)
{
    X4VideoPipelineLease lease;assert(!x4_video_pipeline_acquire_scanout(d,&lease));
    assert(!x4_video_pipeline_finish_scanout(d,lease,id,at-1,at));return lease;
}

static void invariants(void)
{
    X4VideoPipelineDisplay d;X4VideoPipelineSyncHistory h;
    assert(!x4_video_pipeline_display_init_synced(&d,1));x4_video_pipeline_sync_history_init(&h);
    X4VideoPipelineLease first=ready(&d,1,1),second=ready(&d,2,2),take;
    assert(!x4_video_pipeline_ready_scanout(&d,&take)&&take.slot==first.slot);
    assert(!x4_video_pipeline_mark_submitted(&d,first,1,3));
    assert(d.ready_since_us==2&&d.slots[second.slot].state==X4_SCANOUT_READY);
    assert(x4_video_pipeline_cancel_ready_lease(&d,first)==X4_PIPELINE_STATE);
    X4VideoPipelineLease shown;
    assert(x4_video_pipeline_observe_flip(&d,44,16684,&shown)==X4_PIPELINE_WAIT);
    assert(!h.synced_history_count);
    assert(!x4_video_pipeline_observe_flip(&d,1,16684,&shown));
    assert(!x4_video_pipeline_synced_observed(&d,&h,16684));
    assert(h.synced_history_count==1&&!h.synced_recovery_drops);
    uint64_t next=3,now=16684,arg=1;
    for(unsigned cycle=1;cycle<61u;++cycle){
        if(x4_video_pipeline_ready_scanout(&d,&take)==X4_PIPELINE_WAIT)take=ready(&d,next++,now+1);
        assert(!x4_video_pipeline_mark_submitted(&d,take,++arg,now+2));
        (void)ready(&d,next++,now+3);
        now+=16684u;assert(!x4_video_pipeline_observe_flip(&d,arg,now,&shown));
        uint64_t before=h.synced_recovery_drops;
        assert(!x4_video_pipeline_synced_observed(&d,&h,now));
        assert(h.synced_recovery_drops==(cycle==29u||cycle==59u?before+1u:before));
        if(h.synced_recovery_drops!=before)assert(!h.synced_history_count);
    }
    assert(h.synced_recovery_drops==2u);
    /* Missing READY breaks continuity. A 60ms gap makes old samples stale. */
    (void)x4_video_pipeline_cancel_ready_scanout(&d);
    assert(!x4_video_pipeline_synced_observed(&d,&h,now+16684u));
    assert(h.synced_history[h.synced_history_next? h.synced_history_next-1u:29u]==0);
    assert(!x4_video_pipeline_synced_observed(&d,&h,now+80000u));
    assert(h.synced_history_count==1u&&h.synced_recovery_drops==2u);
    puts("PASS T42 ring: oldest-ready, one native pending, 30-sample recovery/reset, stale gap, exact lease ownership");
}

int main(void)
{
    invariants();
    Sync42Arrival *input=calloc(SYNC42_FRAMES,sizeof(*input));
    unsigned *decode=calloc(SYNC42_FRAMES,sizeof(*decode));
    Sync42Result *current=calloc(1,sizeof(*current)),*old=calloc(1,sizeof(*old));
    assert(input&&decode&&current&&old);arrivals(input,decode);
    printf("INPUT source_hz=60.00 display_hz=59.94 poll_us=1000 conversion_us=1500 frames=%u decode_p50_us=%u decode_p95_us=%u decode_p99_us=%u network_jitter_us=0..2000 delivery_hold_us=50000 every_frames=900\n",
        SYNC42_FRAMES,percentile(decode,SYNC42_FRAMES,50),percentile(decode,SYNC42_FRAMES,95),percentile(decode,SYNC42_FRAMES,99));
    const char *names[]={"FIFO","Mailbox","Synced","HSYNC","Auto60"};
    bool target=false;
    for(enum Sync42Mode mode=SYNC42_FIFO;mode<=SYNC42_AUTO;mode++){
        synced42_engine(mode,input,SYNC42_FRAMES,current);
        if(mode!=SYNC42_SYNCED){
            legacy42_engine(mode,input,SYNC42_FRAMES,old);
            assert(current->shown==old->shown&&current->ring_drops==old->ring_drops&&current->mailbox_drops==old->mailbox_drops);
            assert(!memcmp(current->ids,old->ids,sizeof(current->ids))&&!memcmp(current->times,old->times,sizeof(current->times)));
            assert(current->display_size==old->display_size&&current->scanout_size==old->scanout_size&&
                current->pending_offset==old->pending_offset&&current->screen_offset==old->screen_offset);
        }
        unsigned p50=percentile(current->waits,(size_t)current->shown,50),p95=percentile(current->waits,(size_t)current->shown,95);
        unsigned interval=percentile(current->intervals,(size_t)current->shown-1u,95);
        double ratio=100.0*(double)current->shown/SYNC42_FRAMES;
        double duration=(double)(input[SYNC42_FRAMES-1u].source_us-input[0].source_us)/1000000.0;
        double drops=(double)(current->ring_drops+current->mailbox_drops+current->recovery_drops)/duration;
        printf("RESULT mode=%s wait_p50_us=%u wait_p95_us=%u shown=%llu percent=%.5f drops_per_s=%.5f ring=%llu mailbox=%llu recovery=%llu repeats=%llu interval_p95_us=%u\n",
            names[mode],p50,p95,(unsigned long long)current->shown,ratio,drops,
            (unsigned long long)current->ring_drops,(unsigned long long)current->mailbox_drops,
            (unsigned long long)current->recovery_drops,(unsigned long long)current->repeats,interval);
        if(mode==SYNC42_SYNCED)target=p50<=12000u&&ratio>=99.0&&interval<=18000u&&drops<=.5;
    }
    printf("T42_TARGETS_%s\n",target?"PASS":"MISS");
    puts("PASS T42 exact legacy flip IDs/times and display/scanout ABI on identical arrivals");
    free(input);free(decode);free(current);free(old);return 0;
}
