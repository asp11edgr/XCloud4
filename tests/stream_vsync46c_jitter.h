/* SPDX-License-Identifier: GPL-3.0-only */
/* Included by the host fixture that embeds the actual native-submit function.
 * The clock, decoder work and conversion completion are synthetic. Production
 * ring/scanout ownership, native-mode selection and metrics are real. */

enum {
    JITTER_BLOCK_SIZE = 20,
    JITTER_BLOCKS = 120,
    JITTER_INTERVALS = JITTER_BLOCK_SIZE * JITTER_BLOCKS,
    JITTER_AUS = JITTER_INTERVALS + 1,
    JITTER_DECODE_US = 2000,
    JITTER_CONVERT_US = 3000
};

static int compare_time(const void *lhs,const void *rhs)
{
    uint64_t a=*(const uint64_t *)lhs,b=*(const uint64_t *)rhs;
    return (a>b)-(a<b);
}

static void jitter_arrivals(uint64_t arrivals[JITTER_AUS],unsigned phase)
{
    /* Third-microsecond ticks make an exact average of 60 AU/s. Each
     * independently shuffled 20-interval block has 16 nominal periods,
     * two 8.333 ms intervals and two 25 ms intervals. */
    uint32_t seed=20261107u;
    uint64_t ticks=(uint64_t)phase*3;
    arrivals[0]=ticks/3;
    for(unsigned block=0;block<JITTER_BLOCKS;++block){
        unsigned gaps[JITTER_BLOCK_SIZE];
        for(unsigned i=0;i<16;++i)gaps[i]=50000;
        gaps[16]=gaps[17]=75000;
        gaps[18]=gaps[19]=25000;
        for(unsigned i=JITTER_BLOCK_SIZE-1;i>0;--i){
            seed=seed*UINT32_C(1664525)+UINT32_C(1013904223);
            unsigned j=seed%(i+1),previous=gaps[i];
            gaps[i]=gaps[j];gaps[j]=previous;
        }
        for(unsigned i=0;i<JITTER_BLOCK_SIZE;++i){
            ticks+=gaps[i];
            arrivals[block*JITTER_BLOCK_SIZE+i+1]=ticks/3;
        }
    }
    assert(arrivals[JITTER_INTERVALS]-arrivals[0]==40000000);
}

static uint64_t earliest(uint64_t a,uint64_t b){return a<b?a:b;}

static void jitter_cola(unsigned phase)
{
    uint64_t arrivals[JITTER_AUS],gaps[JITTER_INTERVALS];
    jitter_arrivals(arrivals,phase);
    for(unsigned i=0;i<JITTER_INTERVALS;++i)gaps[i]=arrivals[i+1]-arrivals[i];
    qsort(gaps,JITTER_INTERVALS,sizeof(gaps[0]),compare_time);
    uint64_t p50_gap=gaps[JITTER_INTERVALS*50/100-1];
    uint64_t p95_gap=gaps[JITTER_INTERVALS*95/100-1];
    assert(p50_gap>=16666&&p50_gap<=16667&&p95_gap==25000);

    const uint64_t epoch=1000000,half=epoch+20000000;
    now=epoch;
    X4StreamPipeline p={.display_handle=7,.diagnostics=x4_stream_diagnostics_create(now)};
    assert(p.diagnostics&&x4_stream_diagnostics_vsync_effective(p.diagnostics));
    assert(!x4_video_pipeline_ring_init(&p.ring,1));
    assert(!x4_video_pipeline_display_init(&p.display,1));
    x4_video_pipeline_mailbox_clock_init(&p.mailbox_clock);
    X4VideoPipelineLease input={0},target={0};
    uint64_t conversion_done=UINT64_MAX,refresh=1,dropped_while_waiting=0;
    uint64_t shown=0,repeated=0,first_refresh=0,last_refresh=0;
    unsigned arrived=0,decoded=0,peak_ready[2]={0,0},peak_au=0;

    while(decoded<JITTER_AUS||conversion_done!=UINT64_MAX||p.ring.ready||p.display.pending>=0){
        uint64_t arrival=arrived<JITTER_AUS?epoch+arrivals[arrived]:UINT64_MAX;
        uint64_t decode=decoded<JITTER_AUS?epoch+arrivals[decoded]+JITTER_DECODE_US:UINT64_MAX;
        uint64_t blank=epoch+refresh*50000/3;
        now=earliest(earliest(arrival,decode),earliest(conversion_done,blank));
        assert(now<=epoch+arrivals[JITTER_INTERVALS]+50000);

        if(now==arrival){
            x4_stream_diagnostics_au_received(p.diagnostics,arrived==0,now);
            ++arrived;
            unsigned queued=arrived-decoded;
            if(queued>peak_au)peak_au=queued;
        }
        if(now==decode){
            X4VideoPipelineLease lease;
            assert(decoded<arrived);
            assert(!x4_video_pipeline_acquire_decode(&p.ring,&lease,NULL));
            assert(!x4_video_pipeline_publish_ready(&p.ring,lease,++decoded,now));
        }
        if(now==conversion_done){
            const X4VideoPipelineOutput *image=&p.ring.slots[input.slot];
            assert(!x4_video_pipeline_finish_scanout(&p.display,target,image->output_id,image->decode_end_us,now));
            assert(!x4_video_pipeline_release_conversion(&p.ring,input));
            conversion_done=UINT64_MAX;
        }
        if(now==blank){
            if(p.display.pending>=0){
                X4VideoPipelineLease scanout;
                assert(native_mode==ORBIS_VIDEO_OUT_FLIP_VSYNC);
                assert(!x4_video_pipeline_observe_flip(&p.display,arg,now,&scanout));
                const X4VideoPipelineScanout *screen=&p.display.slots[scanout.slot];
                assert(screen->presentation_mode==X4_PRESENT_VSYNC_COLA);
                p.last_flip_measured=true;
                p.last_flip_mode=screen->presentation_mode;
                p.last_flip_submitted_us=screen->submitted_us;
                p.last_flip_observed_us=now;
                x4_stream_diagnostics_presentation(p.diagnostics,screen->presentation_mode,
                    now-screen->decode_end_us,now-screen->submitted_us,now);
                x4_stream_diagnostics_pacing_observe(p.diagnostics,screen->presentation_mode,
                    screen->converted_us,now,now,16667);
                if(!shown)first_refresh=refresh;
                else repeated+=refresh-last_refresh-1;
                last_refresh=refresh;
                ++shown;
            }
            ++refresh;
        }

        assert(!submit_display(&p));
        unsigned half_index=now>=half;
        if(p.ring.ready>peak_ready[half_index])peak_ready[half_index]=p.ring.ready;
        if(p.ring.ready&&conversion_done==UINT64_MAX&&
            x4_video_pipeline_acquire_scanout(&p.display,&target)==X4_PIPELINE_OK){
            unsigned waited=ready_during_cola_vsync(&p);
            assert(!x4_video_pipeline_take_ready(&p.ring,&input,NULL));
            if(waited)dropped_while_waiting+=waited-
                decoded_during_flip(&p,p.ring.slots[input.slot].decode_end_us);
            conversion_done=now+JITTER_CONVERT_US;
        }
        X4VideoPipelineDisplayOccupancy occupancy;
        assert(!x4_video_pipeline_display_occupancy(&p.display,&occupancy));
        assert(occupancy.in_flight<=1&&occupancy.occupied<=2&&p.ring.ready<=2);
    }
    X4PipelinePresentationSummary summary;
    assert(x4_stream_diagnostics_presentation_summary(p.diagnostics,&summary));
    const X4PipelineTimingSummary *timing=&summary.mode[X4_PRESENT_VSYNC_COLA].decode_to_flip;
    assert(timing->samples==shown&&(timing->finite_mask&3u)==3u);
    assert(timing->p95_us*3<=100000); /* two 60 Hz refreshes */
    assert(peak_au==1&&peak_ready[0]==2&&peak_ready[1]==2&&x4_video_pipeline_ring_idle(&p.ring));
    assert(p.ring.discarded_ready==0&&p.ring.discarded_latency>0&&shown+p.ring.discarded_latency==JITTER_AUS);
    assert(repeated>0&&first_refresh+shown+repeated==last_refresh+1);
    assert(p.diagnostics->pacing.repeat_estimate==repeated);
    assert(dropped_while_waiting<=p.ring.discarded_latency);
    if(phase==1000||phase==4000)assert(dropped_while_waiting>0);
    assert(!x4_video_pipeline_display_closed(&p.display));

    printf("PASS 60Hz Cola jitter phase_us=%u au=%u arrival_us_p50_p95=%llu/%llu "
        "peak_au=%u peak_ready_halves=%u/%u presented=%llu drops_latest=%llu "
        "drops_during_vsync_wait=%llu repeated=%llu decode_flip_us_p50_p95_max=%llu/%llu/%llu\n",
        phase,JITTER_AUS,(unsigned long long)p50_gap,(unsigned long long)p95_gap,
        peak_au,peak_ready[0],peak_ready[1],(unsigned long long)shown,
        (unsigned long long)p.ring.discarded_latency,(unsigned long long)dropped_while_waiting,
        (unsigned long long)repeated,(unsigned long long)timing->p50_us,
        (unsigned long long)timing->p95_us,(unsigned long long)timing->maximum_us);
    x4_stream_diagnostics_free(p.diagnostics);
}

static void jitter_cases(void)
{
    const unsigned phases[]={1000,4000,9000,15000};
    for(unsigned i=0;i<sizeof(phases)/sizeof(phases[0]);++i)jitter_cola(phases[i]);
}
