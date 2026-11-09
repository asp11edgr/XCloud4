# SPDX-License-Identifier: GPL-3.0-only
"""Real submission, legacy Auto hysteresis and jittered 60 Hz Cola.
Host native doubles establish arguments/lifetimes, never physical scanout.
"""
from contextlib import ExitStack
from pathlib import Path
import os,shlex,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/video/stream_pipeline.c').read_text()
def source_function(signature):
 start=s.index(signature); end=s.index('{',start)+1; depth=1
 while depth:
  depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
submit=source_function('static int submit_display(')
wait_metrics='\n'.join(source_function(signature) for signature in (
 'static bool decoded_during_flip(', 'static unsigned ready_during_cola_vsync('))
assert submit.count('sceVideoOutSubmitFlip(')==1
assert 'x4_stream_diagnostics_vsync_effective' in submit
assert 'sceVideoOutGetVblankStatus' not in submit
fixture=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "src/streaming/stream_profile.h"
#include "src/video/video_pipeline.h"
#include "src/video/stream_diagnostics.c"
#include <orbis/VideoOut.h>
typedef struct {
 X4VideoPipelineDisplay display;
 X4VideoPipelineMailboxClock mailbox_clock;
 X4VideoPipelineRing ring;
 X4StreamDiagnostics *diagnostics;
 int display_handle;
 bool notice_scanout[X4_PIPELINE_SCANOUT_CAPACITY],notice_mode_active,notice_synced;
 bool last_flip_measured;
 unsigned last_flip_mode;
 uint64_t last_flip_submitted_us,last_flip_observed_us;
} X4StreamPipeline;
static uint64_t now=1000000,next_stream_flip=UINT64_C(1)<<61,arg;
static unsigned calls;static int native_mode,flip_error,done_error;
static uint64_t now_us(void){return now;}
static void duration(X4StreamPipeline *p,X4StreamStage s,uint64_t us){(void)p;(void)s;(void)us;}
int sceVideoOutSubmitFlip(int h,int slot,int mode,int64_t id){assert(h==7&&slot>=0&&slot<4);++calls;native_mode=mode;arg=id;return flip_error;}
int sceVideoOutGetFlipStatus(int h,OrbisVideoOutFlipStatus *status){assert(h==7);*status=(OrbisVideoOutFlipStatus){0};return 0;}
static int sceGnmSubmitDone(void){return done_error;}
@SUBMIT@
@WAIT_METRICS@
#include "tests/stream_vsync46c_jitter.h"
static void bucket(X4StreamDiagnostics*d,unsigned second,unsigned count)
{for(unsigned i=0;i<count;++i)x4_stream_diagnostics_au_received(d,false,1000000+(uint64_t)second*1000000+i*100);}
static X4VideoPipelineLease ready(X4StreamPipeline*p,uint64_t id)
{X4VideoPipelineLease l;assert(!x4_video_pipeline_acquire_scanout(&p->display,&l));assert(!x4_video_pipeline_finish_scanout(&p->display,l,id,now-100,now-20));return l;}
static void auto_policy(void)
{
 X4StreamDiagnostics*d=x4_stream_diagnostics_create(1000000);assert(d);
 assert(x4_stream_diagnostics_vsync_effective(d)); /* new default */
 x4_stream_diagnostics_vsync_init(d,X4_VSYNC_AUTO_T27);
 assert(!x4_stream_diagnostics_vsync_effective(d));
 bucket(d,0,30);bucket(d,1,30);assert(!x4_stream_diagnostics_vsync_effective(d));bucket(d,2,30);bucket(d,3,44);
 assert(x4_stream_diagnostics_vsync_effective(d)); /* third complete 30 bucket */
 bucket(d,4,60);bucket(d,6,60); /* empty5 preserves streak */
 bucket(d,7,42);bucket(d,8,60);assert(x4_stream_diagnostics_vsync_effective(d));bucket(d,9,29);
 assert(!x4_stream_diagnostics_vsync_effective(d));bucket(d,10,33);bucket(d,12,27);bucket(d,13,42);
 assert(x4_stream_diagnostics_vsync_effective(d));
 X4StreamVsyncTransition e;unsigned n=0;while(x4_stream_diagnostics_vsync_take_transition(d,&e)){++n;assert(e.ordinal==n);}
 assert(n==3);x4_stream_diagnostics_free(d);
 /* Rolling cadence continues beyond the one-hour diagnostic bucket array. */
 d=x4_stream_diagnostics_create(1000000);assert(d);x4_stream_diagnostics_vsync_init(d,X4_VSYNC_AUTO_T27);
 bucket(d,4000,30);bucket(d,4001,30);bucket(d,4002,30);bucket(d,4003,60);
 assert(x4_stream_diagnostics_vsync_effective(d));x4_stream_diagnostics_free(d);
 d=x4_stream_diagnostics_create(1000000);assert(d);x4_stream_diagnostics_vsync_init(d,X4_VSYNC_AUTO_T27);
 unsigned edge[]={27,26,33,34,30,44,55,54,65,66,60,44};
 for(unsigned i=0;i<6;++i)bucket(d,i,edge[i]);assert(x4_stream_diagnostics_vsync_effective(d));
 for(unsigned i=6;i<12;++i)bucket(d,i,edge[i]);assert(!x4_stream_diagnostics_vsync_effective(d));x4_stream_diagnostics_free(d);

}
static void ownership(void)
{
 X4VideoPipelineDisplay d;X4VideoPipelineLease l,pending,shown,writer,latest;
 assert(!x4_video_pipeline_display_init_mode(&d,1,4,true));assert(!x4_video_pipeline_display_mailbox_admission(&d,true));
 assert(!x4_video_pipeline_acquire_scanout(&d,&l));assert(!x4_video_pipeline_finish_scanout(&d,l,1,1,2));assert(!x4_video_pipeline_mark_submitted(&d,l,10,3));
 assert(!x4_video_pipeline_observe_flip(&d,10,4,&shown));unsigned screen=shown.slot;
 assert(!x4_video_pipeline_acquire_scanout(&d,&pending));assert(!x4_video_pipeline_finish_scanout(&d,pending,2,5,6));assert(!x4_video_pipeline_mark_submitted(&d,pending,11,7));
 assert(!x4_video_pipeline_acquire_scanout(&d,&l));assert(!x4_video_pipeline_finish_scanout(&d,l,3,8,9));
 assert(!x4_video_pipeline_acquire_scanout(&d,&writer));assert(writer.slot!=screen&&writer.slot!=pending.slot&&writer.slot!=l.slot);
 assert(!x4_video_pipeline_finish_scanout(&d,writer,4,10,11));assert(d.slots[l.slot].state==X4_SCANOUT_FREE&&d.mailbox_replaced==1);
 assert(d.slots[screen].state==X4_SCANOUT_SCREEN&&d.slots[pending.slot].state==X4_SCANOUT_PENDING);
 assert(x4_video_pipeline_ready_scanout(&d,&latest)==X4_PIPELINE_WAIT);
 assert(!x4_video_pipeline_observe_flip(&d,11,12,&shown));assert(!x4_video_pipeline_ready_scanout(&d,&latest));assert(latest.slot==writer.slot);
 /* Auto switches to HSYNC: stop extra admission, drain pre-owned READY. */
 assert(!x4_video_pipeline_display_mailbox_admission(&d,false));assert(x4_video_pipeline_acquire_scanout(&d,&l)==X4_PIPELINE_WAIT);
 assert(!x4_video_pipeline_mark_submitted(&d,latest,12,13));assert(!x4_video_pipeline_observe_flip(&d,12,14,&shown));
 assert(!x4_video_pipeline_acquire_scanout(&d,&l));assert(!x4_video_pipeline_quarantine_scanout(&d,l));
 assert(x4_video_pipeline_display_closed(&d)==X4_PIPELINE_STATE);assert(!x4_video_pipeline_retire_quarantine(&d,l));assert(!x4_video_pipeline_display_closed(&d));
 assert(!x4_video_pipeline_display_init(&d,2));assert(!x4_video_pipeline_acquire_scanout(&d,&l));assert(x4_video_pipeline_acquire_scanout(&d,&writer)==X4_PIPELINE_WAIT);
 assert(!x4_video_pipeline_abort_scanout(&d,l));assert(!x4_video_pipeline_display_closed(&d));
}
static void auto_mailbox_drain(void)
{
 X4StreamPipeline p={.display_handle=7,.diagnostics=x4_stream_diagnostics_create(1000000)};assert(p.diagnostics);
 x4_stream_diagnostics_vsync_init(p.diagnostics,X4_VSYNC_AUTO_T27);
 x4_video_pipeline_mailbox_clock_init(&p.mailbox_clock);
 assert(!x4_video_pipeline_display_init_mode(&p.display,77,4,true));
 bucket(p.diagnostics,0,30);bucket(p.diagnostics,1,30);bucket(p.diagnostics,2,30);bucket(p.diagnostics,3,60);
 assert(x4_stream_diagnostics_vsync_effective(p.diagnostics));
 assert(!x4_video_pipeline_display_mailbox_admission(&p.display,true));now=4001000;
 X4VideoPipelineLease initial=ready(&p,1),shown;
 assert(!submit_display(&p)&&native_mode==ORBIS_VIDEO_OUT_FLIP_VSYNC);
 assert(!x4_video_pipeline_observe_flip(&p.display,arg,++now,&shown));assert(shown.slot==initial.slot);
 X4VideoPipelineLease pending_lease=ready(&p,2);assert(!submit_display(&p));uint64_t pending_arg=arg;
 assert(native_mode==ORBIS_VIDEO_OUT_FLIP_VSYNC&&p.display.slots[pending_lease.slot].presentation_mode==X4_PRESENT_VSYNC_MAILBOX);
 X4VideoPipelineLease old_ready=ready(&p,3),writer;
 assert(!x4_video_pipeline_acquire_scanout(&p.display,&writer));
 assert(writer.slot!=initial.slot&&writer.slot!=pending_lease.slot&&writer.slot!=old_ready.slot);
 X4VideoPipelineScanout before[X4_PIPELINE_SCANOUT_CAPACITY];memcpy(before,p.display.slots,sizeof(before));
 unsigned calls_before=calls;
 /* Closing the third near60 second changes Auto while ALL four owners exist. */
 bucket(p.diagnostics,4,60);bucket(p.diagnostics,5,60);bucket(p.diagnostics,6,40);
 assert(!x4_stream_diagnostics_vsync_effective(p.diagnostics));
 assert(!x4_video_pipeline_display_mailbox_admission(&p.display,false));
 assert(!memcmp(before,p.display.slots,sizeof(before))&&calls==calls_before);
 assert(p.display.pending==(int)pending_lease.slot&&p.display.screen==(int)initial.slot);
 assert(p.display.slots[pending_lease.slot].presentation_mode==X4_PRESENT_VSYNC_MAILBOX);
 X4VideoPipelineLease blocked;assert(x4_video_pipeline_acquire_scanout(&p.display,&blocked)==X4_PIPELINE_WAIT);
 assert(!submit_display(&p)&&calls==calls_before); /* already sent VSYNC is unchanged */
 /* Previously issued writer may finish: only the older, UNSENT READY retires. */
 assert(!x4_video_pipeline_finish_scanout(&p.display,writer,4,now-100,now));
 assert(p.display.slots[old_ready.slot].state==X4_SCANOUT_FREE&&p.display.mailbox_replaced==1);
 assert(p.display.slots[pending_lease.slot].state==X4_SCANOUT_PENDING&&p.display.slots[initial.slot].state==X4_SCANOUT_SCREEN);
 assert(!x4_video_pipeline_observe_flip(&p.display,pending_arg,++now,&shown));
 assert(shown.slot==pending_lease.slot&&p.display.slots[initial.slot].state==X4_SCANOUT_FREE);
 assert(!submit_display(&p)&&calls==calls_before+1&&native_mode==ORBIS_VIDEO_OUT_FLIP_HSYNC);
 assert(p.display.pending==(int)writer.slot&&p.display.slots[writer.slot].presentation_mode==X4_PRESENT_HSYNC);
 assert(x4_video_pipeline_acquire_scanout(&p.display,&blocked)==X4_PIPELINE_WAIT);
 assert(!x4_video_pipeline_observe_flip(&p.display,arg,++now,&shown));assert(shown.slot==writer.slot);
 /* Drained state obeys Cola admission although physical capacity remains four. */
 X4VideoPipelineLease next=ready(&p,5);assert(x4_video_pipeline_acquire_scanout(&p.display,&blocked)==X4_PIPELINE_WAIT);
 assert(!submit_display(&p)&&native_mode==ORBIS_VIDEO_OUT_FLIP_HSYNC);
 assert(x4_video_pipeline_acquire_scanout(&p.display,&blocked)==X4_PIPELINE_WAIT);
 assert(!x4_video_pipeline_observe_flip(&p.display,arg,++now,&shown)&&shown.slot==next.slot);
 assert(!x4_video_pipeline_display_closed(&p.display));x4_stream_diagnostics_free(p.diagnostics);
 puts("PASS Auto VSYNC->HSYNC with SCREEN+PENDING+READY+WRITING: transition mutates no lease, pending keeps VSYNC, next submit HSYNC, drains back to Cola");
}
static void submission(void)
{
 for(unsigned policy=X4_VSYNC_AUTO;policy<=X4_VSYNC_AUTO_T27;++policy){
  for(unsigned fps=30;fps<=60;fps+=30){
   for(unsigned mailbox=0;mailbox<2;++mailbox){
    now=1000000;
    X4StreamPipeline p={.display_handle=7,.diagnostics=x4_stream_diagnostics_create(now)};assert(p.diagnostics);
    x4_stream_diagnostics_vsync_init(p.diagnostics,policy);x4_video_pipeline_mailbox_clock_init(&p.mailbox_clock);
    for(unsigned i=0;i<5;++i)bucket(p.diagnostics,i,fps);
    now=6000000;
    bool vsync=policy!=X4_VSYNC_NEVER&&(policy!=X4_VSYNC_AUTO_T27||fps==30);
    assert(x4_stream_diagnostics_vsync_effective(p.diagnostics)==vsync);
    if(policy==X4_VSYNC_SYNCED)assert(!x4_video_pipeline_display_init_synced(&p.display,3));
    else assert(!x4_video_pipeline_display_init_mode(&p.display,3,mailbox?4:3,mailbox));
    assert(!x4_video_pipeline_display_mailbox_admission(&p.display,mailbox&&vsync&&policy!=X4_VSYNC_SYNCED));
    ready(&p,1);unsigned n=calls;
    assert(!submit_display(&p)&&calls==n+1);
    assert(native_mode==(vsync?ORBIS_VIDEO_OUT_FLIP_VSYNC:ORBIS_VIDEO_OUT_FLIP_HSYNC));
    assert(!submit_display(&p)&&calls==n+1);
    X4VideoPipelineLease shown;assert(!x4_video_pipeline_observe_flip(&p.display,arg,++now,&shown));
    ready(&p,2);flip_error=-55;assert(submit_display(&p)==-55);flip_error=0;
    assert(!x4_video_pipeline_display_closed(&p.display));x4_stream_diagnostics_free(p.diagnostics);
   }
  }
 }
 X4VideoPipelineMailboxClock c;x4_video_pipeline_mailbox_clock_init(&c);uint64_t target;
 assert(!x4_video_pipeline_mailbox_due(&c,1000,900,&target)&&!target);
 assert(!x4_video_pipeline_mailbox_anchor(&c,100000,99000,100100,true));
 assert(x4_video_pipeline_mailbox_anchor(&c,116667,115000,116800,true));
 assert(x4_video_pipeline_mailbox_due(&c,120000,119000,&target)==X4_PIPELINE_WAIT&&target==133334);
 assert(!x4_video_pipeline_mailbox_due(&c,132000,119000,&target)&&target==133334);
 assert(x4_video_pipeline_mailbox_due(&c,151000,119000,&target)==X4_PIPELINE_WAIT&&target==166668);
 assert(c.missed_opportunities==2);
 assert(!x4_video_pipeline_mailbox_anchor(&c,116668,120000,120001,true));assert(!c.valid);
 assert(!x4_video_pipeline_mailbox_due(&c,2000000,1900000,&target)&&!target);
}
int main(void){
 auto_policy();ownership();auto_mailbox_drain();submission();
 puts("PASS real 30/60 fps submission, forced modes, Auto T27 AU hysteresis, >1h cadence, Mailbox ownership and native submission errors");
 jitter_cases();
}
'''.replace('@SUBMIT@',submit).replace('@WAIT_METRICS@',wait_metrics)
with ExitStack() as stack:
 d=os.environ.get('X4_VSYNC_TEST_OUTPUT')
 out=Path(d) if d else Path(stack.enter_context(tempfile.TemporaryDirectory(prefix='x4-vsync46c-')))
 out.mkdir(parents=True,exist_ok=True)
 unit=out/'test.c';unit.write_text(fixture)
 command=shlex.split(os.environ.get('CC','clang'))+['-std=gnu2x','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-DX4_STREAM_PROFILE=1','-DX4_VIDEO_PROBE=0','-I'+str(root),'-I'+str(root/'tests/video_capacity_stubs'),'-I'+str(root/'tests/host_stubs'),str(unit),str(root/'src/video/video_pipeline.c'),'-Wl,--gc-sections']+shlex.split(os.environ.get('X4_TEST_CFLAGS',''))+['-o',str(out/'test')]
 subprocess.run(command,check=True,cwd=root);subprocess.run([str(out/'test')],check=True,cwd=root)
