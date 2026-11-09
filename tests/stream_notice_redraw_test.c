/* SPDX-License-Identifier: GPL-3.0-only */
/* Reuse only host native/VideoOut doubles; do not run the resilience battery. */
#define X4_STREAM_RESILIENCE_NO_MAIN 1
#include "stream_resilience_integration_test.c"
#undef X4_STREAM_RESILIENCE_NO_MAIN
static void notice_overlay(uint32_t *pixels,void *opaque)
{
    unsigned index=*(unsigned*)opaque;
    assert(index<2);pixels[index]=index?0xff00ff00u:0xffff0000u;
}
static void redraw_case(bool mailbox)
{
    reset_host();X4StreamPipeline *p=calloc(1,sizeof(*p));assert(p);
    p->session=1;p->display_handle=7;p->scanout_count=mailbox?4u:3u;
    video_handle=7;p->diagnostics=x4_stream_diagnostics_create(now_us());assert(p->diagnostics);
    x4_stream_diagnostics_vsync_init(p->diagnostics,0);
    p->au_queue=x4_stream_au_queue_create();assert(p->au_queue);
    assert(!x4_stream_mutex_init(&p->status_gate,"notice-status"));
    assert(!x4_stream_mutex_init(&p->ring_gate,"notice-ring"));
    assert(!x4_stream_wait_init(&p->decode_wait,"notice-decode"));
    assert(!x4_video_pipeline_ring_init(&p->ring,1));
    /* Real reconnect bridge while a Decode AU lease is pinned. */
    uint8_t au_bytes[16]={0x65,0x55};X4StreamAuInput au={.data=au_bytes,.sps=au_bytes,.sps_bytes=1,.bytes=sizeof(au_bytes),.is_idr=true,.identity=1};
    X4StreamAuOutcome outcome;X4StreamAuLease active;X4StreamAuView input;
    assert(x4_stream_au_queue_try_push(p->au_queue,&au,1,&outcome)==X4_STREAM_AU_OK);
    assert(x4_stream_au_queue_try_take(p->au_queue,2,&active,&input,&outcome)==X4_STREAM_AU_OK);
    au.identity=2;au.is_idr=false;assert(x4_stream_au_queue_try_push(p->au_queue,&au,3,&outcome)==X4_STREAM_AU_OK);
    assert(!x4_stream_pipeline_begin_receive_epoch(p));
    X4StreamAuStats stats;assert(x4_stream_au_queue_try_snapshot(p->au_queue,&stats)==X4_STREAM_AU_OK);
    assert(stats.inflight_entries==1&&!stats.ready_entries&&stats.waiting_idr);
    assert(!memcmp(input.data,au_bytes,sizeof(au_bytes)));
    assert(x4_stream_au_queue_release(p->au_queue,active)==X4_STREAM_AU_OK);

    assert(!x4_video_pipeline_display_init_mode(&p->display,1,p->scanout_count,mailbox));
    p->display.mailbox_admission=mailbox;p->display.screen=screen_slot=0;
    p->display.last_submitted_output_id=100;
    p->display.slots[0]=(X4VideoPipelineScanout){.state=X4_SCANOUT_SCREEN,.output_id=100,.decode_end_us=now_us()-1000};
    for(unsigned n=0;n<p->scanout_count;++n){
        p->display_pixels[n]=malloc(STREAM_SCANOUT_BYTES);assert(p->display_pixels[n]);
        for(unsigned i=0;i<STREAM_SCANOUT_BYTES/4;++i)p->display_pixels[n][i]=0xff112233u;
        if(n<3)scanout[n]=p->display_pixels[n];
    }
    screen_pixel=0xff112233u;
    unsigned index=0;assert(!x4_stream_pipeline_redraw_notice(p,notice_overlay,&index));
    assert(p->notice_backing&&p->display.pending!=0&&p->display_pixels[0][0]==0xff112233u);
    int pending=p->display.pending;uint32_t pending_first=p->display_pixels[pending][0];
    assert(x4_stream_pipeline_redraw_notice(p,notice_overlay,&index)==X4_PIPELINE_WAIT);
    assert(p->display_pixels[pending][0]==pending_first&&p->display_pixels[0][0]==0xff112233u);
    while(p->display.pending>=0)assert(!observe_display(p));
    assert(p->display.mailbox_capable==mailbox&&p->display.mailbox_admission==mailbox);
    assert(!p->view.presentations&&p->display.slots[p->display.screen].output_id==100);
    index=1;assert(!x4_stream_pipeline_redraw_notice(p,notice_overlay,&index));
    pending=p->display.pending;
    assert(p->display_pixels[pending][0]==0xff112233u); /* previous notice erased */
    assert(p->display_pixels[pending][1]==0xff00ff00u);
    while(p->display.pending>=0)assert(!observe_display(p));
    assert(!p->view.presentations&&p->display.mailbox_capable==mailbox);
    /* Resume one real completed conversion with the next native output ID.
     * The real scheduler helper must release snapshot and accept ID101. */
    assert(!x4_video_pipeline_acquire_scanout(&p->display,&p->scanout_lease));
    X4VideoPipelineLease decoded;assert(!x4_video_pipeline_acquire_decode(&p->ring,&decoded,NULL));
    uint64_t now=now_us();assert(!x4_video_pipeline_publish_ready(&p->ring,decoded,101,now-100));
    assert(!x4_video_pipeline_take_ready(&p->ring,&p->image_lease,NULL));
    p->converting=true;p->converting_image.output_id=101;p->converting_image.decode_end_us=now-100;
    p->conversion_begin=now-100;atomic_init(&p->conversion_readers,1);
    for(unsigned i=0;i<2;++i)atomic_init(&p->converters[i].completed,0);
    assert(!finish_conversion(p,NULL,NULL,false));assert(!p->notice_backing);
    assert(!submit_display(p));while(p->display.pending>=0)assert(!observe_display(p));
    assert(p->display.slots[p->display.screen].output_id==101&&p->view.presentations==1);
    for(unsigned n=0;n<p->scanout_count;++n)free(p->display_pixels[n]);
    x4_stream_au_queue_stop(p->au_queue);assert(x4_stream_au_queue_destroy(p->au_queue));
    assert(!x4_stream_wait_destroy(&p->decode_wait));assert(!x4_stream_mutex_destroy(&p->ring_gate));
    assert(!x4_stream_mutex_destroy(&p->status_gate));x4_stream_diagnostics_free(p->diagnostics);free(p);
    video_handle=0;screen_slot=pending_slot=-1;
    printf("PASS frozen notice: mailbox=%u FREE-only writes, pending/SCREEN immutable, no real-image metrics, snapshot erased, native ID101 resumes\n",mailbox);
}
int main(void){
    /* Native doubles are shared; these unrelated scenarios are not executed. */
    (void)intermittent_errors;
    (void)persistent_errors;
    (void)delayed_idr_recovers;
    (void)bad_idrs_never_renew_deadline;
    (void)resolution_changes;
    (void)attempts_and_budget;
    (void)oversized_sps;
    (void)cancel_reconfiguration;
    (void)persistent_native_errors;
    (void)no_image_does_not_reset_errors;
    (void)retained_reconfiguration;
    (void)retained_display_close;
    (void)waiting_idr_eof;
    (void)depth_two_continuous;
    (void)depth_two_waiting_idr_and_bad_frame;
    (void)depth_two_native_restart;
    (void)depth_auto_resolution_changes;
    (void)depth_two_cancel_in_native;
    (void)depth_two_failed_delete;
    (void)depth_two_fallbacks;
    redraw_case(false);redraw_case(true);return 0;
}

