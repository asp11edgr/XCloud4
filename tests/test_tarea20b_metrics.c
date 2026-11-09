/* SPDX-License-Identifier: GPL-3.0-only */
/* Production pipeline and exporter with the established native doubles.
 * These observations validate accounting, never native PS4 performance. */
#define main previous_pipeline_fixture_main
#include "stream_pipeline_runtime_test.c"
#undef main

/* Link dependency of stream_profile.c's unrelated /play JSON builder.
 * This fixture never invokes /play or locale selection. The menu hooks,
 * ring operations, histogram and pipeline JSON exporter remain production
 * implementations; no metric or exporter is replaced by this locale stub. */

static void ui_perf_bins(void)
{
    X4UiPerf h={0};
    assert(!x4_ui_perf_percentile(&h,50));
    x4_ui_perf_record(&h,0);x4_ui_perf_record(&h,49);
    x4_ui_perf_record(&h,50);x4_ui_perf_record(&h,100000);
    assert(h.n==4 && h.total==100099 && h.max==100000);
    assert(x4_ui_perf_percentile(&h,50)==50);
    assert(x4_ui_perf_percentile(&h,95)==100000);
    puts("PASS UI histograms: 50us upper bins, empty and overflow use actual maximum");
}

static void menu_submit_one(X4StreamPipeline *p,uint64_t sequence)
{
    const uint8_t sps[]={0,0,0,1,0x67},pps[]={0,0,0,1,0x68,0x80};
    x4_stream_pipeline_au_received(p,false,sceKernelGetProcessTime());
    assert(!x4_stream_pipeline_submit_parameters(p,(const uint8_t *)&sequence,8,
        sequence*16667,sequence,sps,sizeof(sps),pps,sizeof(pps)));
    uint64_t deadline=sceKernelGetProcessTime()+3000000;
    while(atomic_load_explicit(&p->consumed,memory_order_acquire)<=sequence) {
        assert(!atomic_load(&p->error) && sceKernelGetProcessTime()<deadline);usleep(50);
    }
}

static void menu_pump_one(X4StreamPipeline *p,uint64_t presentations)
{
    uint64_t deadline=sceKernelGetProcessTime()+5000000;
    while(p->view.presentations<presentations || p->converting || p->display.pending>=0) {
        assert(!x4_stream_pipeline_present(p,overlay,NULL));
        assert(sceKernelGetProcessTime()<deadline);usleep(100);
    }
}

static void real_menu_accounting(void)
{
    reset();atomic_bool stop;X4StreamPipeline *p=new_pipeline(&stop,220);
    assert(!x4_stream_pipeline_start(p));assert(!x4_stream_pipeline_display_open(p));
    x4_stream_pipeline_note_menu_open(p,false);
    menu_submit_one(p,0);menu_pump_one(p,1);
    assert(p->conversion_menu[0].n==1 && !p->conversion_menu[1].n && !p->menu_presentations);

    x4_stream_pipeline_note_menu_open(p,true);
    /* Decoder publishes six pictures while main deliberately does not consume
     * them. Four output slots cause two full-ring drops; latest-ready then
     * retires the other three READY pictures. These are real ring operations. */
    for(uint64_t sequence=1;sequence<=6;++sequence)menu_submit_one(p,sequence);
    lock(&p->ring_gate);
    assert(p->menu_discarded_full==2 && p->ring.discarded_ready==2);
    assert(!p->menu_discarded_latency);
    unlock(&p->ring_gate);
    menu_pump_one(p,2);
    assert(p->conversion_menu[0].n==1 && p->conversion_menu[1].n==1);
    assert(p->menu_presentations==1);
    lock(&p->ring_gate);
    assert(p->menu_discarded_latency==3 && p->ring.discarded_latency==3);
    unlock(&p->ring_gate);

    /* Subsequent full/latest drops with the menu closed remain in the global
     * ring totals, and must not leak into the while-open counters. */
    x4_stream_pipeline_note_menu_open(p,false);
    for(uint64_t sequence=7;sequence<=12;++sequence)menu_submit_one(p,sequence);
    menu_pump_one(p,3);
    assert(p->conversion_menu[0].n==2 && p->conversion_menu[1].n==1);
    assert(p->menu_presentations==1 && p->menu_discarded_full==2 && p->menu_discarded_latency==3);
    assert(p->ring.discarded_ready==4 && p->ring.discarded_latency==6);
    assert(p->view.presentations==3 && p->view.outputs==13 && overlays==3);
    assert(p->display.peak_in_flight==1);

    assert(!x4_stream_pipeline_close(p));
    assert(join_count==3 && native_closes==1 && !allocated && !handle_open && !p->retained);
    uint64_t native_wait_calls=x4_stream_wait_test_api_calls();
    x4_stream_pipeline_note_menu_open(NULL,true);
    x4_stream_pipeline_note_menu_open(p,true);
    assert(!atomic_load(&p->menu_open));
    assert(x4_stream_wait_test_api_calls()==native_wait_calls);
    assert(!x4_stream_pipeline_export(p,NULL));
    assert(!x4_stream_pipeline_free(p));
    puts("PASS real menu accounting: closed conversion n=2; open n=1; menu flips=1; open full=2/latest=3; global full=4/latest=6; joined close and real JSON export");
}

int main(int argc,char **argv)
{
    assert(argc==2);report_directory=argv[1];
    assert(!setvbuf(stdout,NULL,_IOLBF,0));
    ui_perf_bins();real_menu_accounting();
    puts("PASS TAREA20b menu pipeline metrics (native operations are mocks)");return 0;
}
