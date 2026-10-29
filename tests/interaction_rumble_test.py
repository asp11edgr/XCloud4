#!/usr/bin/env python3
"""Compile the actual main-thread rumble/measurement helpers with a fake pad."""
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/core/main.c').read_text(encoding='utf-8')
screen_header = (root / 'src/ui/screen.h').read_text(encoding='utf-8')
# This production typedef is flat. Keep its exact fields rather than creating
# a stale substitute for the menu owner's snapshot contract.
cache_type = re.search(r'typedef struct \{[^{}]+\} X4TouchMenuCacheStats;', screen_header)
assert cache_type, 'production X4TouchMenuCacheStats typedef'
start = source.index('/* Main is the sole writer and exporter.')
end = source.index('\n#endif', start)
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "@UI_PERF_HEADER@"
@MENU_CACHE_TYPE@
/* These owners are outside the rumble harness. Their doubles expose an
 * explicit empty state while the real main JSON writer still executes. */
static unsigned menu_cache_resets,menu_cache_snapshots,loading_metrics_writes;
void x4_touch_menu_cache_reset(void){++menu_cache_resets;}
void x4_touch_menu_cache_snapshot(X4TouchMenuCacheStats *out){
 assert(out);++menu_cache_snapshots;memset(out,0,sizeof(*out));
}
void x4_loading_screen_metrics_write(FILE *file){
 ++loading_metrics_writes;fputs("{\"fixture_no_loading_owner\":true}",file);
}
enum {X4_INTERACTION_RUMBLE_OFF=2, X4_MON_PHASE_MAIN_POLL=14,
 X4_MON_PHASE_MAIN_STATUS=15, X4_MON_PHASE_MAIN_IDLE=16};
typedef struct {uint8_t lgMotor,smMotor;} OrbisPadVibeParam;
static uint64_t clock_us=1000, calls_at[1024];
static OrbisPadVibeParam calls_value[1024];
static unsigned calls;
static int native_result;
static uint64_t sceKernelGetProcessTime(void){return clock_us;}
static int sceKernelUsleep(unsigned delay){clock_us+=delay;return 0;}
static int scePadSetVibration(int handle,OrbisPadVibeParam *value){
 assert(handle==1);assert(calls<1024);
 if(calls)assert(clock_us-calls_at[calls-1]>=16000);
 calls_at[calls]=clock_us;calls_value[calls++]=*value;clock_us+=25;
 return native_result;
}
'''
prefix = prefix.replace('@UI_PERF_HEADER@', (root / 'src/ui/ui_perf.h').as_posix())
prefix = prefix.replace('@MENU_CACHE_TYPE@', cache_type.group(0))
suffix = r'''
int main(void){
 menu_copy_times.n=7;menu_prepare_times.n=8;
 menu_log_at=menu_interval_frames=menu_interval_open_frames=9;
 interaction_main_begin(0,clock_us);
 assert(menu_cache_resets==1&&!menu_copy_times.n&&!menu_prepare_times.n&&
     !menu_log_at&&!menu_interval_frames&&!menu_interval_open_frames);
 InteractionRumble s={.handle=-1};
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){0},clock_us);
 assert(calls==1);
 for(unsigned i=0;i<100;++i){
   clock_us=2000+i*1000;
   interaction_rumble_step(&s,1,(OrbisPadVibeParam){(uint8_t)(i+1),10},clock_us);
 }
 assert(calls<=8);assert(s.applied.lgMotor);
 uint64_t last=s.last_call_us;
 clock_us=last+6000;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){0},clock_us);
 assert(s.stop_pending);
 clock_us=last+8000;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){222,111},clock_us);
 assert(s.stop_pending);
 clock_us=last+16000;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){222,111},clock_us);
 assert(!s.stop_pending&&!s.applied.lgMotor&&!s.applied.smMotor);
 assert(!calls_value[calls-1].lgMotor&&!calls_value[calls-1].smMotor);
 clock_us=last+32000;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){222,111},clock_us);
 assert(s.applied.lgMotor==222);
 unsigned before=calls;
 clock_us+=16000;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){222,111},clock_us);
 assert(calls==before);
 native_result=-77;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){0},clock_us);
 assert(s.stop_pending&&s.applied.lgMotor==222);
 before=calls;clock_us+=2000;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){9,8},clock_us);
 assert(calls==before&&s.stop_pending);
 native_result=0;clock_us=s.last_call_us+16000;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){9,8},clock_us);
 assert(!s.stop_pending&&!s.applied.lgMotor);
 clock_us+=16000;
 interaction_rumble_step(&s,1,(OrbisPadVibeParam){9,8},clock_us);
 assert(s.applied.lgMotor==9);
 interaction_rumble_stop(&s,1);
 assert(!s.applied.lgMotor&&!s.applied.smMotor);
 before=calls;interaction_rumble_stop(&s,1);assert(calls==before);
 interaction_main_read(clock_us);
 clock_us+=1000;interaction_phase(X4_MON_PHASE_MAIN_STATUS,clock_us);
 clock_us+=30000;interaction_main_read(clock_us);
 assert(interaction_main.gaps==1);
 assert(interaction_main.gap[0].phase==X4_MON_PHASE_MAIN_STATUS);
 assert(interaction_main.errors==1);
 x4_main_interaction_metrics_write(stdout);puts("");
 assert(menu_cache_snapshots==1&&loading_metrics_writes==1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='x4-rumble-main-') as tmp:
    test = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    test.write_text(prefix + source[start:end] + suffix, encoding='utf-8')
    subprocess.run(shlex.split(os.environ.get('CC', 'cc')) +
                   ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Werror=vla',
                    '-Werror=alloca'] + shlex.split(os.environ.get('X4_TEST_CFLAGS', '')) +
                   [str(test), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
    report = json.loads(result.stdout)
    assert report['available'] and report['rumble_enabled']
    assert report['errors'] == 1
    assert report['native_calls'] == sum(x['count'] for x in report['native_results'])
    assert report['call_duration_us']['p50_upper'] == 50
    assert report['call_duration_us']['max'] == 25
    assert report['main_loop_gaps_over_20ms']['count'] == 1
    assert report['menu_ui']['steady_overlay_us']['n'] == 0
    assert report['menu_ui']['preparation_us']['n'] == 0
    assert report['menu_ui']['regenerations'] == 0
    assert report['menu_ui']['composed_frames'] == 0
    assert report['menu_ui']['copied_bytes'] == 0
    assert report['menu_ui']['cache_bytes'] == 0
    assert report['loading_ui'] == {'fixture_no_loading_owner': True}
print('PASS real main rumble: 16ms cap, latest intensities, latched stop, failed-call retry, final stop and metric JSON')
