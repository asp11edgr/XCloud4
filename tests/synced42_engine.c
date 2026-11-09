/* SPDX-License-Identifier: GPL-3.0-only */
#include "synced42_sim.h"
#include "../src/video/video_pipeline.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static uint64_t next_vblank(uint64_t now)
{
    /* Exactly 59.94 Hz; integer ceil keeps each event within one microsecond. */
    uint64_t tick=now*59940u/1000000000u+1u;
    return (tick*1000000000u+59939u)/59940u;
}

void synced42_engine(enum Sync42Mode mode,const Sync42Arrival *arrivals,size_t count,Sync42Result *result)
{
    assert(arrivals&&count&&count<=SYNC42_FRAMES&&result);
    memset(result,0,sizeof(*result));
    result->display_size=sizeof(X4VideoPipelineDisplay);result->scanout_size=sizeof(X4VideoPipelineScanout);
    result->pending_offset=offsetof(X4VideoPipelineDisplay,pending);result->screen_offset=offsetof(X4VideoPipelineDisplay,screen);
    X4VideoPipelineRing ring;assert(!x4_video_pipeline_ring_init_outputs(&ring,1,5));
    X4VideoPipelineDisplay display;
#ifndef T42_LEGACY
    X4VideoPipelineSyncHistory history;x4_video_pipeline_sync_history_init(&history);
    if(mode==SYNC42_SYNCED)assert(!x4_video_pipeline_display_init_synced(&display,1));
    else
#else
    assert(mode!=SYNC42_SYNCED);
#endif
        assert(!x4_video_pipeline_display_init_mode(&display,1,mode==SYNC42_MAILBOX?4:3,mode==SYNC42_MAILBOX));
    if(mode==SYNC42_MAILBOX)assert(!x4_video_pipeline_display_mailbox_admission(&display,true));
    X4VideoPipelineMailboxClock clock;x4_video_pipeline_mailbox_clock_init(&clock);
    bool converting=false,driver_pending=false;
    X4VideoPipelineLease scanout={0},image={0};
    uint64_t complete_us=0,driver_due=0,driver_arg=0,observed_arg=0,observed_ptime=0,next_arg=1,last_ptime=0;
    uint64_t begin=arrivals[0].source_us,now=begin,blank=next_vblank(now);
    uint64_t deadline=arrivals[count-1].ready_us+2000000u;
    size_t input=0;
    const bool vsync=mode!=SYNC42_HSYNC&&mode!=SYNC42_AUTO;
    while(now<=deadline){
        while(blank<=now){
            ++result->vblanks;
            if(driver_pending&&driver_due<=blank){
                observed_arg=driver_arg;observed_ptime=blank;driver_pending=false;
            }else ++result->repeats;
            blank=next_vblank(blank);
        }
        if(!vsync&&driver_pending&&driver_due<=now){observed_arg=driver_arg;observed_ptime=driver_due;driver_pending=false;}
        while(input<count&&arrivals[input].ready_us<=now){
            X4VideoPipelineLease lease;uint64_t dropped=0;
            int rc=x4_video_pipeline_acquire_decode(&ring,&lease,&dropped);
            assert(rc==0||rc==X4_PIPELINE_WAIT);
            if(!rc)assert(!x4_video_pipeline_publish_ready(&ring,lease,input+1u,arrivals[input].ready_us));
            ++input;
        }
        /* The production main-owner sequence is completion, exact flip
         * observation, submission, then admission of one new conversion. */
        if(converting&&complete_us<=now){
            X4VideoPipelineOutput *source=&ring.slots[image.slot];
            assert(!x4_video_pipeline_finish_scanout(&display,scanout,source->output_id,source->decode_end_us,now));
            assert(!x4_video_pipeline_release_conversion(&ring,image));converting=false;
        }
        if(display.pending>=0){
            X4VideoPipelineLease presented;
            int rc=x4_video_pipeline_observe_flip(&display,observed_arg,now,&presented);
            assert(rc==0||rc==X4_PIPELINE_WAIT);
            if(!rc){
                const X4VideoPipelineScanout *s=&display.slots[presented.slot];
                assert(result->shown<SYNC42_SAMPLES);
                size_t n=(size_t)result->shown;
                result->ids[n]=s->output_id;result->times[n]=now;
                /* The simulated driver supplies exact physical flip time.
                 * Keep observation times above for the legacy sequence check. */
                assert(observed_ptime>=s->converted_us);
                result->waits[n]=(unsigned)(observed_ptime-s->converted_us);
                if(n)result->intervals[n-1]=(unsigned)(observed_ptime-last_ptime);
                last_ptime=observed_ptime;
                if(!n)result->first_us=now;result->last_us=now;++result->shown;
                x4_video_pipeline_mailbox_anchor(&clock,observed_ptime,s->submitted_us,now,vsync);
#ifndef T42_LEGACY
                if(mode==SYNC42_SYNCED)assert(!x4_video_pipeline_synced_observed(&display,&history,now));
#endif
            }
        }
        X4VideoPipelineLease ready;
        int rc=x4_video_pipeline_ready_scanout(&display,&ready);
        assert(rc==0||rc==X4_PIPELINE_WAIT);
        if(!rc){
            uint64_t target=0;
            if(mode==SYNC42_MAILBOX)rc=x4_video_pipeline_mailbox_due(&clock,now,display.ready_since_us,&target);
            assert(rc==0||rc==X4_PIPELINE_WAIT);
            if(!rc){
                assert(!driver_pending);
                driver_due=vsync?next_vblank(now):now+250u;driver_arg=next_arg++;driver_pending=true;
                assert(!x4_video_pipeline_mark_submitted(&display,ready,driver_arg,now));
            }
        }
        if(!converting){
            rc=x4_video_pipeline_acquire_scanout(&display,&scanout);
            assert(rc==0||rc==X4_PIPELINE_WAIT);
            if(!rc){
                rc=x4_video_pipeline_take_ready(&ring,&image,NULL);
                assert(rc==0||rc==X4_PIPELINE_WAIT);
                if(!rc){converting=true;complete_us=now+1500u;}
                else assert(!x4_video_pipeline_abort_scanout(&display,scanout));
            }
        }
        X4VideoPipelineDisplayOccupancy occupancy;
        assert(!x4_video_pipeline_display_occupancy(&display,&occupancy));
        assert(occupancy.pending<=1u&&occupancy.writing<=1u&&occupancy.screen<=1u);
        if(input==count&&!ring.ready&&!converting&&!occupancy.ready&&!occupancy.pending)break;
        now+=1000u;
    }
    result->ring_drops=ring.discarded_ready+ring.discarded_latency;
    result->mailbox_drops=display.mailbox_replaced+display.mailbox_discarded_old;
#ifndef T42_LEGACY
    result->recovery_drops=history.synced_recovery_drops;
#endif
    assert(result->shown+result->ring_drops+result->mailbox_drops+result->recovery_drops==count);
}
