/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Inspect fixed ownership/counters and intercept only this translation unit's
 * Lab log; exporter bytes still pass through the real stdio implementation. */
static unsigned log_count;
#if !X4_PUBLIC
static char last_log[1024];
static int capture_printf(const char *format,...)
{
    va_list args;va_start(args,format);
    int n=vsnprintf(last_log,sizeof(last_log),format,args);va_end(args);
    assert(n>0&&(size_t)n<sizeof(last_log)&&last_log[n-1]=='\n');
    ++log_count;return n;
}
#define printf capture_printf
#endif
#include "../src/video/stream_diagnostics.c"
#undef printf

static void policy_and_samples(const char *path)
{
    assert(X4_PRESENT_MODE_COUNT==3&&X4_PRESENT_METRIC_MODE_COUNT==4);
    assert(offsetof(X4StreamDiagnostics,presentation)==451840);
    assert(offsetof(X4StreamDiagnostics,slot_wait)==599656);
    assert(offsetof(X4StreamDiagnostics,ring_gate_wait)==616072);
    assert(offsetof(X4StreamDiagnostics,ownership_samples)==632488);
    assert(offsetof(X4StreamDiagnostics,synced_presentation)==632600);
    X4StreamDiagnostics *d=x4_stream_diagnostics_create(100);assert(d);
    x4_stream_diagnostics_vsync_init(d,3);
    assert(x4_stream_diagnostics_vsync_effective(d));
    for(unsigned second=0;second<8;++second)
        for(unsigned i=0;i<60;++i)
            x4_stream_diagnostics_au_received(d,false,100+(uint64_t)second*1000000+i*10000);
    assert(x4_stream_diagnostics_vsync_effective(d)&&d->vsync.transitions==0);
    const uint64_t flips[]={100000,116684,150052,166736};
    const uint64_t waits[]={0,1000,2000,40000};
    for(unsigned i=0;i<4;++i)
        x4_stream_diagnostics_pacing_observe(d,X4_PRESENT_VSYNC_SYNC,
            flips[i]-waits[i],flips[i],flips[i]+500,16684);
    assert(d->pacing.frames==4&&d->pacing.mode_frames[3]==4);
    assert(d->pacing.ready.count==4&&d->pacing.ready.maximum==40000);
    assert(d->pacing.ready_ms[0]==1&&d->pacing.ready_ms[1]==1&&
        d->pacing.ready_ms[2]==1&&d->pacing.ready_ms[31]==1);
    assert(d->pacing.interval.count==3&&d->pacing.over20==1);
    assert(d->pacing.repeat_estimate==1&&d->pacing.repeat_intervals==3);
    x4_stream_diagnostics_pacing_observe(d,X4_PRESENT_HSYNC,170000,0,175000,16684);
    assert(d->pacing.fallback_times==1&&d->pacing.last_vsync_ptime==0);
    /* Missing READY invalidates only that timing, never the known real flip. */
    x4_stream_diagnostics_presentation(d,X4_PRESENT_VSYNC_COLA,23400,15200,190100);
    x4_stream_diagnostics_pacing_observe(d,X4_PRESENT_VSYNC_COLA,0,190000,190100,16684);
    assert(d->pacing.frames==6&&d->pacing.ready_invalid==1&&d->pacing.ready.count==5);
    x4_stream_diagnostics_pacing_queue(d,0,0,0);
    x4_stream_diagnostics_pacing_queue(d,2,1,3);
    x4_stream_diagnostics_pacing_queue(d,99,99,99);
    x4_stream_diagnostics_pacing_queue(d,1,-1,0);
    assert(d->pacing.queue_samples==4&&d->pacing.queue_invalid==1);
    assert(d->pacing.ready_depth[4]==1&&d->pacing.driver_depth[8]==1&&d->pacing.gpu_depth[8]==1);
    assert(d->pacing.ready_max==99&&d->pacing.driver_max==99&&d->pacing.gpu_max==99);
    x4_stream_diagnostics_pacing_drops(d,1,2,3,4,2);
    x4_stream_diagnostics_pacing_drops(d,1,2,3,4,2); /* cumulative snapshots, not increments */
    assert(d->pacing.recovery_drops==1&&d->pacing.mailbox_drops==2&&
        d->pacing.ring_full_drops==3&&d->pacing.ring_latest_drops==4&&
        d->pacing.cola_vsync_wait_drops==2);
    x4_stream_diagnostics_pacing_log(d,10000099,false);assert(log_count==0);
    x4_stream_diagnostics_pacing_log(d,10000100,false);
#if !X4_PUBLIC
    assert(log_count==1&&strstr(last_log,"XCloud4 present 10s policy=3"));
    assert(strstr(last_log,"mode=VSYNC_Cola effective=VSYNC decode_flip_us=23500/23500 decode_finite=3"));
    assert(strstr(last_log,"cola_vsync_wait_drop=2 repeat_est=1")&&strstr(last_log,"hist1ms="));
    x4_stream_diagnostics_pacing_log(d,10000100,false);assert(log_count==1);
    x4_stream_diagnostics_pacing_log(d,20000100,false);assert(log_count==2);
    x4_stream_diagnostics_pacing_log(d,20000101,true);assert(log_count==3);
    assert(strstr(last_log,"XCloud4 present final"));
    x4_stream_diagnostics_pacing_log(d,30000100,false);
    x4_stream_diagnostics_pacing_log(d,30000100,true);assert(log_count==3);
#else
    assert(log_count==0);
#endif
    FILE *f=fopen(path,"wb");assert(f);
    assert(!x4_stream_diagnostics_write(f,d,30000000)&&!fclose(f));
    x4_stream_diagnostics_free(d);
}
static void boundary_samples(void)
{
    X4StreamDiagnostics *d=x4_stream_diagnostics_create(1);assert(d);
    x4_stream_diagnostics_pacing_observe(d,99,1,2,3,16684);
    assert(d->pacing.frames==0);
    x4_stream_diagnostics_pacing_observe(d,3,10,5,20,16684);
    assert(d->pacing.fallback_times==1&&d->pacing.ready.sum==10);
    x4_stream_diagnostics_pacing_observe(d,3,30,31,31,14000);
    assert(d->pacing.repeat_unknown==2&&d->pacing.last_vsync_ptime==0);
    x4_stream_diagnostics_pacing_observe(d,3,40,41,41,16684);
    x4_stream_diagnostics_pacing_observe(d,3,UINT64_MAX-2,UINT64_MAX-1,UINT64_MAX,16684);
    /* Rounded cycle multiplication cannot overflow with arbitrary bad stamps. */
    assert(d->pacing.native_times==3&&d->pacing.frames==4);
    x4_stream_diagnostics_pacing_observe(d,3,1,100,100,16684);
    assert(d->pacing.interval_invalid==1);
    x4_stream_diagnostics_free(d);
}
typedef struct {X4StreamDiagnostics *d;atomic_bool done;} Concurrent;
static void *assemble(void *opaque)
{
    Concurrent *c=opaque;
    for(unsigned i=0;i<100000;++i)x4_stream_diagnostics_au_received(c->d,false,1000+i);
    atomic_store_explicit(&c->done,true,memory_order_release);return NULL;
}
static void concurrent_mirror(void)
{
    Concurrent c={.d=x4_stream_diagnostics_create(1)};assert(c.d);atomic_init(&c.done,false);
    pthread_t thread;assert(!pthread_create(&thread,NULL,assemble,&c));
    uint64_t observed=0,now=10000001;
    while(!atomic_load_explicit(&c.done,memory_order_acquire)){
        uint64_t n=atomic_load_explicit(&c.d->pacing_received_aus,memory_order_relaxed);
        assert(n>=observed);observed=n;
        x4_stream_diagnostics_pacing_log(c.d,now,false);now+=10000000;
    }
    assert(!pthread_join(thread,NULL));
    assert(c.d->au_count==100000&&atomic_load(&c.d->pacing_received_aus)==100000);
    x4_stream_diagnostics_free(c.d);
}
int main(int argc,char **argv)
{
    assert(argc==2);policy_and_samples(argv[1]);boundary_samples();concurrent_mirror();
    puts("PASS T42 fixed pacing histograms, timestamps, inferred repeats, bounded logs and atomic AU mirror");
    return 0;
}
