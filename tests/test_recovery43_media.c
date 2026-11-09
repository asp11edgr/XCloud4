/* SPDX-License-Identifier: GPL-3.0-only */
/* Exact production media TU with its established native/clock doubles. */
#define X4_RECOVERY43_MEDIA_HOST_TEST 1
#define main x4_existing_media_frontier_fixture
#include "test_live_media_stream_frontier.c"
#undef main

static uint64_t noted_incomplete,noted_discarded,noted_pli,noted_freezes;
static unsigned present_attempts,delivery_calls,delivery_reason;
static uint32_t noted_epoch,delivery_epoch,delivery_timestamp;
static uint16_t delivery_sequence;
static int fake_present_result;

void x4_recovery43_note_media(uint32_t epoch,uint64_t incomplete,uint64_t discarded,
    uint64_t pli,uint64_t freezes)
{
    noted_epoch=epoch;noted_incomplete+=incomplete;noted_discarded+=discarded;
    noted_pli+=pli;noted_freezes+=freezes;
}
void x4_recovery43_delivery(uint32_t epoch,uint16_t sequence,uint32_t timestamp,unsigned reason)
{
    ++delivery_calls;delivery_epoch=epoch;delivery_sequence=sequence;
    delivery_timestamp=timestamp;delivery_reason=reason;
}
int x4_stream_pipeline_present(X4StreamPipeline *p,X4StreamOverlay overlay,void *context)
{(void)p;(void)overlay;(void)context;++present_attempts;return fake_present_result;}
void x4_stream_pipeline_request_stop(X4StreamPipeline *p)
{(void)p;assert(!"unexpected pipeline stop in observation-only test");}
void x4_stream_pipeline_rtp_gap(X4StreamPipeline *p,uint64_t now)
{(void)p;assert(now==clock_us);}
void x4_grace_inbox_test_sleep(unsigned us){(void)us;abort();}
void x4_grace_inbox_test_after_enqueue_load(X4NackGraceInbox *q,uint_fast64_t at)
{(void)q;(void)at;}

static void test_worker_totals_are_deltas(void)
{
    X4LiveMedia *m=fixture();x4_nack_grace_inbox_init(&m->grace_inbox);
    atomic_store(&m->grace_inbox.epoch,7);
    noted_incomplete=noted_discarded=noted_pli=noted_freezes=0;
    m->au_open=true;m->au_size=1;reset_au(m,true,RESET_MARKER,0);
    X4LiveMediaSnapshot out;prepare_worker_view(m,&out);
    assert(noted_epoch==7&&noted_incomplete==1&&noted_discarded==1&&noted_pli==0);
    prepare_worker_view(m,&out);
    assert(noted_incomplete==1&&noted_discarded==1);
    /* The existing WAIT-IDR discard is intentionally visible in both exact
     * counters; the metrics do not relabel it as another network loss. */
    au(m,90000,false);prepare_worker_view(m,&out);
    assert(noted_incomplete==2&&noted_discarded==2&&noted_pli==0);
    cleanup(m);
}
static void test_main_presentation_observation(void)
{
    X4LiveMedia *m=fixture();x4_nack_grace_inbox_init(&m->grace_inbox);
    atomic_store(&m->grace_inbox.epoch,8);snapshot_available=true;
    noted_freezes=0;present_attempts=0;fake_present_result=23;
    assert(x4_live_media_stream_present(NULL,NULL,NULL)==X4_LIVE_ERR_ARGUMENT);
    assert(present_attempts==0);
    assert(x4_live_media_stream_present(m,NULL,NULL)==23);
    assert(present_attempts==1&&noted_freezes==0);
    pipeline_shadow.presentations=1;clock_us=1000;
    assert(x4_live_media_stream_present(m,NULL,NULL)==23);
    pipeline_shadow.presentations=2;clock_us=101000;
    assert(x4_live_media_stream_present(m,NULL,NULL)==23);
    assert(noted_freezes==0); /* Strictly greater than100ms. */
    pipeline_shadow.presentations=3;clock_us=201001;
    assert(x4_live_media_stream_present(m,NULL,NULL)==23);
    assert(noted_freezes==1&&noted_epoch==8);
    clock_us=400000;assert(x4_live_media_stream_present(m,NULL,NULL)==23);
    assert(noted_freezes==1); /* A repeated flip is no new image. */
    snapshot_available=false;pipeline_shadow.presentations=4;clock_us=500000;
    assert(x4_live_media_stream_present(m,NULL,NULL)==23);
    snapshot_available=true;pipeline_shadow.presentations=5;clock_us=600000;
    assert(x4_live_media_stream_present(m,NULL,NULL)==23);
    assert(noted_freezes==2&&present_attempts==7);cleanup(m);
}
static void test_real_worker_admission_result(void)
{
    X4LiveMedia *m=fixture();
    assert(!x4_live_reorder_init_video(&m->reorder,128,32,25000,m->reorder_tickets));
    x4_nack_grace_inbox_init(&m->grace_inbox);x4_nack_grace_init(&m->grace,1,false);
    atomic_store(&m->grace_inbox.epoch,99);
    x4_nack_binding_init(&m->grace_binding,99,((uint64_t)7<<32)|102);
    m->track=(X4LiveTrack){.forced_type=102,.payload_type=102,.ssrc=7,.locked=true};
    m->reorder.started=true;m->reorder.expected=10;
    uint8_t packet[14]={0x80,0x66,0,9,0,1,0,0,0,0,0,7,0x61,0x11};
    X4IngressTicket ticket={0};ticket.sequence.transport_epoch=7;
    delivery_calls=0;
    assert(x4_video_ingress_push_ticket(&m->ingress,packet,sizeof(packet),clock_us,NULL,&ticket));
    video_batch(m);
    assert(delivery_calls==1&&delivery_epoch==7&&delivery_sequence==9&&
        delivery_timestamp==65536&&delivery_reason==X4_RECOVERY43_DELIVERY_LATE);
    packet[3]=11;
    assert(x4_live_reorder_insert_ticket(&m->reorder,packet,sizeof(packet),11,clock_us,&ticket)==X4_LIVE_REORDER_STORED);
    assert(x4_video_ingress_push_ticket(&m->ingress,packet,sizeof(packet),clock_us,NULL,&ticket));
    video_batch(m);
    assert(delivery_calls==2&&delivery_sequence==11&&delivery_reason==X4_RECOVERY43_DELIVERY_DUPLICATE);
    /* Unknown provenance is never attributed to the active newer peer. */
    ticket.sequence.transport_epoch=0;
    assert(x4_video_ingress_push_ticket(&m->ingress,packet,sizeof(packet),clock_us,NULL,&ticket));
    video_batch(m);assert(delivery_calls==2);
    x4_live_reorder_free(&m->reorder);cleanup(m);
}
int main(void)
{
    test_worker_totals_are_deltas();test_main_presentation_observation();
    test_real_worker_admission_result();
    puts("PASS recovery43 actual worker AU deltas/admission and observed presentation gaps; no render-policy changes");
    return 0;
}
