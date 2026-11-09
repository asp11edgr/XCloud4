/* SPDX-License-Identifier: GPL-3.0-only */
/* HOST ONLY: execute the actual T26 entry and display.c. Native VideoOut,
 * drawing, thread creation and LoadExec are explicit simulation boundaries.
 * No decoder thread, PS4 syscall, console UI or firmware claim is made. */
#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
#error "Assertions must remain enabled"
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef X4_T26_STACK_HEADER
#define X4_T26_STACK_HEADER "../src/video/probe_stack.h"
#endif
#include X4_T26_STACK_HEADER
static void *host_calloc(size_t,size_t);
static void *host_malloc(size_t);
static void host_free(void *);
static int host_create(OrbisPthread *,X4ProbeThread *,void *,const char *);
/* The wrapper's entry is deliberately not executed in this startup fixture.
 * Discarding its argument here makes the actual worker unreachable by linker
 * GC, rather than replacing its durable protocol with a different model. */
#define x4_probe_thread_create(id,thread,entry,arg,name) host_create(id,thread,arg,name)
#define calloc(n,size) host_calloc(n,size)
#define malloc(size) host_malloc(size)
#define free(ptr) host_free(ptr)
#ifndef X4_T26_CONTROLLER_SOURCE
#define X4_T26_CONTROLLER_SOURCE "../src/video/probe_t26_controller.c"
#endif
#include X4_T26_CONTROLLER_SOURCE
#undef x4_probe_thread_create
#undef calloc
#undef malloc
#undef free
#include <orbis/VideoOut.h>

enum { NORMAL, ALLOC_BANK, ALLOC_READER, WAIT_MAIN, WAIT_WORKER,
    VIDEO_OPEN, FIRST_PRESENT, CREATE_THREAD, CREATE_PARTIAL, JOIN_PARTIAL,
    DESTROY_WAIT, CLOSE_VIDEO, PREPARE_NEGATIVE, PREPARE_POSITIVE,
    REQUEST_ERROR, PIPELINE_OPEN, CASE_COUNT };
static unsigned fixture,alloc_calls,free_calls,wait_inits,wait_destroys;
static unsigned prepared,requested,preparation_sleeps,terminal_sleeps;
static unsigned draw_calls,pad_reads,pad_closes,thread_creates,joins;
static unsigned pipeline_opens,pipeline_presents,pipeline_closes,handoff_phase;
static unsigned total_cases;
static uint64_t now_us;
static jmp_buf escaped;
static Bank *bank;
static void *owned[8];static unsigned owned_count;
static struct {
    bool opened,allocated,mapped;
    unsigned opens,closes,flips;
    void *memory;size_t bytes;X4UiSurface *ui;int64_t last_flip;
} native;
static uint32_t pixels[1];

static void *host_calloc(size_t n,size_t size)
{
    ++alloc_calls;if(fixture==ALLOC_BANK&&alloc_calls==1)return NULL;
    void *p=calloc(n,size);assert(p&&owned_count<8);owned[owned_count++]=p;return p;
}
static void *host_malloc(size_t size)
{
    ++alloc_calls;if(fixture==ALLOC_READER&&alloc_calls==2)return NULL;
    void *p=malloc(size);assert(p&&owned_count<8);owned[owned_count++]=p;return p;
}
static void host_free(void *p)
{
    if(!p)return;
    for(unsigned i=0;i<owned_count;++i)if(owned[i]==p){owned[i]=NULL;++free_calls;free(p);return;}
    assert(!"controller freed unknown storage");
}
static int host_create(OrbisPthread *id,X4ProbeThread *thread,void *argument,const char *name)
{
    assert(id&&thread&&name&&!thread->created);++thread_creates;bank=argument;
    if(fixture==CREATE_THREAD)return -801;
    if(fixture==CREATE_PARTIAL||fixture==JOIN_PARTIAL){thread->created=true;*id=(OrbisPthread)1;return -802;}
    if(fixture==PIPELINE_OPEN){
        bank->requested_pipeline=(X4StreamPipeline *)(uintptr_t)0x1234;
        atomic_store(&bank->command,BANK_DISPLAY_OPEN);
    }else atomic_store(&bank->finished,true);
    return 0;
}
int scePthreadJoin(OrbisPthread id,void **result)
{(void)result;assert(id==(OrbisPthread)1);++joins;return fixture==JOIN_PARTIAL?-803:0;}
void x4_probe_thread_joined(X4ProbeThread *thread,int rc)
{assert(thread->created);thread->report.join_rc=rc;if(!rc)thread->created=false;}
uint64_t sceKernelGetProcessTime(void){return now_us;}
int sceKernelUsleep(unsigned us)
{
    now_us+=us;
    if(us==100000){
        if(prepared){++terminal_sleeps;longjmp(escaped,1);}
        ++preparation_sleeps;
    }
    return 0;
}
int x4_exit_prepare(void){++prepared;return fixture==PREPARE_NEGATIVE?-901:fixture==PREPARE_POSITIVE?1:0;}
int x4_exit_request(void){assert(prepared==1);++requested;return fixture==REQUEST_ERROR?-902:0;}
int x4_stream_wait_init(X4StreamWait *w,const char *name)
{
    assert(w&&name&&!w->initialized);++wait_inits;
    if((fixture==WAIT_MAIN&&wait_inits==1)||(fixture==WAIT_WORKER&&wait_inits==2))return -804;
    w->initialized=true;return 0;
}
int x4_stream_wait_destroy(X4StreamWait *w)
{
    if(!w->initialized)return 0;++wait_destroys;
    if(fixture==DESTROY_WAIT&&wait_destroys==1)return -805;
    w->initialized=false;return 0;
}
int x4_stream_wait_notify(X4StreamWait *w)
{
    assert(w&&w->initialized);
    if(fixture==PIPELINE_OPEN&&bank&&w==&bank->worker_wait){
        if(handoff_phase++==0){
            assert(bank->handoff_result==-806);
            atomic_store(&bank->command,BANK_DISPLAY_CLOSE);
        }else{atomic_store(&bank->finished,true);bank->exit_requested=true;}
    }
    return 0;
}
int x4_stream_wait_block(X4StreamWait *w,unsigned us)
{assert(w&&w->initialized&&us<=20000);now_us+=us;return X4_STREAM_WAIT_TIMEOUT;}
void x4_controller_init(X4Controller *p){memset(p,0,sizeof(*p));p->handle=-1;}
void x4_controller_read(X4Controller *p,unsigned frame)
{
    assert(p&&frame);++pad_reads;
    p->pressed=fixture==PIPELINE_OPEN?0:ORBIS_PAD_BUTTON_OPTIONS;
    assert(pad_reads<16);
}
void x4_controller_close(X4Controller *p){assert(p);++pad_closes;}
int x4_font_init(const char *path){assert(path&&strstr(path,"noto_sans.x4font"));return 0;}
void x4_font_close(void){}
int x4_t26_view_draw(uint32_t *p,const X4T26ViewSnapshot *view,X4T26ViewMetrics *metrics)
{assert(p==pixels&&view);(void)metrics;++draw_calls;return 0;}
int x4_stream_pipeline_display_open(X4StreamPipeline *p)
{assert(p==(X4StreamPipeline *)(uintptr_t)0x1234&&!native.opened);++pipeline_opens;return -806;}
void x4_stream_pipeline_request_stop(X4StreamPipeline *p){assert(p==(X4StreamPipeline *)(uintptr_t)0x1234);}
int x4_stream_pipeline_close(X4StreamPipeline *p)
{assert(p==(X4StreamPipeline *)(uintptr_t)0x1234);++pipeline_closes;return 0;}
void x4_stream_pipeline_snapshot(X4StreamPipeline *p,X4StreamPipelineSnapshot *s)
{assert(p==(X4StreamPipeline *)(uintptr_t)0x1234);memset(s,0,sizeof(*s));s->display_detached=true;}
int x4_stream_pipeline_present(X4StreamPipeline *p,X4StreamOverlay overlay,void *context)
{(void)p;(void)overlay;(void)context;++pipeline_presents;assert(!"present after failed display_open");return -1;}
unsigned x4_stream_pipeline_main_state(const X4StreamPipeline *p){(void)p;return 0;}
int x4_stream_pipeline_wait_present(X4StreamPipeline *p,unsigned us){(void)p;(void)us;return 0;}

X4UiSurface *x4_ui_surface_create(void)
{assert(!native.ui);native.ui=(X4UiSurface *)malloc(1);assert(native.ui);return native.ui;}
void x4_ui_surface_destroy(X4UiSurface *s){assert(!s||s==native.ui);free(s);native.ui=NULL;}
uint32_t *x4_ui_surface_pixels(X4UiSurface *s){assert(s==native.ui);return pixels;}
void x4_ui_surface_set_damage(X4UiSurface *s,const X4UiRect *r,size_t n){(void)s;(void)r;(void)n;}
int x4_ui_surface_publish(X4UiSurface *s,uint32_t *p,unsigned i,X4UiCopyStats *stats)
{assert(s==native.ui&&p&&i<2);if(stats)*stats=(X4UiCopyStats){0};return 0;}
int sceVideoOutOpen(int a,int b,int c,const void *d)
{(void)a;(void)b;(void)c;(void)d;++native.opens;assert(!native.opened);if(fixture==VIDEO_OPEN)return -807;native.opened=true;return 73;}
int sceVideoOutClose(int handle)
{assert(handle==73&&native.opened);++native.closes;if(fixture==CLOSE_VIDEO)return -808;native.opened=false;return 0;}
size_t sceKernelGetDirectMemorySize(void){return 256u*1024u*1024u;}
int sceKernelAllocateDirectMemory(off_t low,off_t high,size_t bytes,size_t alignment,int type,off_t *offset)
{(void)low;(void)high;assert(!native.allocated&&alignment==0x200000&&type==3);native.memory=malloc(bytes);assert(native.memory);native.bytes=bytes;native.allocated=true;*offset=0x200000;return 0;}
int sceKernelMapDirectMemory(void **address,size_t bytes,int prot,int flags,off_t offset,size_t alignment)
{(void)prot;(void)flags;(void)alignment;assert(native.allocated&&!native.mapped&&bytes==native.bytes&&offset==0x200000);*address=native.memory;native.mapped=true;return 0;}
int sceKernelMunmap(void *address,size_t bytes)
{assert(!native.opened&&native.mapped&&address==native.memory&&bytes==native.bytes);native.mapped=false;return 0;}
int sceKernelReleaseDirectMemory(off_t offset,size_t bytes)
{assert(!native.opened&&!native.mapped&&native.allocated&&offset==0x200000&&bytes==native.bytes);free(native.memory);native.memory=NULL;native.allocated=false;return 0;}
void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *a,int f,int t,int ar,unsigned w,unsigned h,unsigned pitch)
{(void)f;(void)t;(void)ar;*a=(OrbisVideoOutBufferAttribute){w,h,pitch};}
int sceVideoOutRegisterBuffers(int h,int first,void *const *buffers,int n,const OrbisVideoOutBufferAttribute *a)
{assert(h==73&&first==0&&n==2&&buffers[0]==native.memory&&buffers[1]&&a->width==1920&&a->height==1080);return 0;}
int sceVideoOutSetFlipRate(int h,int rate){(void)rate;assert(h==73);return 0;}
int sceVideoOutSubmitFlip(int h,int i,int mode,int64_t arg)
{assert(h==73&&i>=0&&i<2&&mode==ORBIS_VIDEO_OUT_FLIP_VSYNC);++native.flips;if(fixture==FIRST_PRESENT)return -809;native.last_flip=arg;return 0;}
int sceVideoOutGetFlipStatus(int h,OrbisVideoOutFlipStatus *s)
{assert(h==73);*s=(OrbisVideoOutFlipStatus){.flipArg=native.last_flip,.num=native.flips};return 0;}
int sceGnmSubmitDone(void){return 0;}
void x4_trace_monitor_state(X4Trace *t,unsigned actor,unsigned phase,uint64_t begin)
{(void)t;(void)actor;(void)phase;(void)begin;}
void x4_trace_record(X4Trace *t,uint16_t event,uint16_t flags,uint64_t a,uint64_t b)
{(void)t;(void)event;(void)flags;(void)a;(void)b;}
void x4_trace_poll(X4Trace *t){(void)t;}
void x4_trace_present_complete(X4Trace *t,uint64_t gen,bool fresh,uint64_t num,uint64_t arg)
{(void)t;(void)gen;(void)fresh;(void)num;(void)arg;}

static void reset(unsigned scenario)
{
    fixture=scenario;alloc_calls=free_calls=wait_inits=wait_destroys=0;
    prepared=requested=preparation_sleeps=terminal_sleeps=0;
    draw_calls=pad_reads=pad_closes=thread_creates=joins=0;
    pipeline_opens=pipeline_presents=pipeline_closes=handoff_phase=0;
    now_us=1000000;bank=NULL;owned_count=0;memset(owned,0,sizeof(owned));memset(&native,0,sizeof(native));
}
static void release_simulated_process(void)
{
    /* The test models process replacement only AFTER checking that production
     * retained uncertain owners. These frees are fixture teardown, not app
     * cleanup, and no actual native thread ever owned these simulated bytes. */
    free(native.ui);free(native.memory);
    for(unsigned i=0;i<owned_count;++i)free(owned[i]);
}
static int one(unsigned scenario)
{
    reset(scenario);
    if(!setjmp(escaped)){
        int rc=x4_t26_controller_run();
        fprintf(stderr,"FAIL T26 entry returned rc=%d; real display_open native_calls=%u (R6 precondition rejection is -9001)\n",rc,native.opens);
        release_simulated_process();return 41;
    }
    assert(prepared==1&&terminal_sleeps==1);
    assert(requested==((scenario==PREPARE_NEGATIVE||scenario==PREPARE_POSITIVE)?0u:1u));
    bool error_after_open=scenario==FIRST_PRESENT||scenario==CREATE_THREAD||scenario==CREATE_PARTIAL||scenario==JOIN_PARTIAL||scenario==DESTROY_WAIT||scenario==CLOSE_VIDEO;
    assert(preparation_sleeps==(error_after_open?50u:0u));
    if(scenario==ALLOC_BANK){assert(!native.opens&&alloc_calls==1&&free_calls==0);}
    else if(scenario==ALLOC_READER){assert(!native.opens&&alloc_calls==2&&free_calls==1);}
    else if(scenario==WAIT_MAIN||scenario==WAIT_WORKER){assert(!native.opens&&free_calls==2);}
    else{assert(native.opens==(scenario==PIPELINE_OPEN?2u:1u));}
    if(scenario==CREATE_PARTIAL||scenario==JOIN_PARTIAL)assert(joins==1);
    if(scenario==JOIN_PARTIAL||scenario==DESTROY_WAIT)assert(free_calls==0);
    else if(scenario!=ALLOC_BANK&&scenario!=ALLOC_READER)assert(free_calls==2);
    if(scenario==CLOSE_VIDEO)assert(native.opened&&native.allocated&&native.mapped&&native.ui);
    else assert(!native.opened&&!native.allocated&&!native.mapped&&!native.ui);
    if(scenario==PIPELINE_OPEN)assert(pipeline_opens==1&&pipeline_closes==1&&pipeline_presents==0&&pad_reads==2);
    printf("PASS startup scenario=%u real_display_open_calls=%u prepare=%u request=%u error_wait_us=%u preserved_bank=%u\n",scenario,native.opens,prepared,requested,preparation_sleeps*100000u,free_calls==0&&owned_count>0);
    release_simulated_process();++total_cases;return 0;
}
int main(void)
{
    for(unsigned i=0;i<CASE_COUNT;++i){int rc=one(i);if(rc)return rc;}
    printf("PASS %u actual T26 startup/lifecycle scenarios; real display.c SP1, native calls simulated; never console evidence\n",total_cases);
    return 0;
}
