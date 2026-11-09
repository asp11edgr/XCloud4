/* SPDX-License-Identifier: GPL-3.0-only */
/* The include is extracted verbatim from main.c by the runner. Production
 * overlay and production toast drawing run over a guarded synthetic frame. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/screen.h"
#include "../src/ui/font.h"
#include "../src/ui/canvas.h"
#include "../src/ui/apple_ui.h"
#include "../src/ui/lang.h"
#include "../src/ui/ui_perf.h"
#include "../src/ui/stream_settings.h"
#include "../src/input/script_engine.h"

static uint64_t clock_us=10000000;
uint64_t sceKernelGetProcessTime(void){return clock_us;}
const char *x4_server_settings_active_display_name(void){return "";}
const char *x4_picture_settings_active_quality_label(void){return "1080p";}
const char *x4_access_country_label(X4AccessCountry country){(void)country;return "";}
/* This branch is not exercised; the real notification/overlay are linked. */
void x4_stream_received_draw(uint32_t *p,unsigned w,unsigned h,unsigned mr,unsigned ms,bool fr)
{(void)p;(void)w;(void)h;(void)mr;(void)ms;(void)fr;assert(false);}
static X4UiPerf menu_copy_times;
static uint64_t menu_interval_frames,menu_interval_open_frames;
#if !X4_PUBLIC
static X4UiPerf menu_prepare_times;
static uint64_t menu_log_at;
#endif
#include "main_stream_overlay_actual.inc"

enum{W=1920,H=1080,GUARD=16};
static void synthetic_frame(uint32_t *p)
{
    for(int y=0;y<H;++y)for(int x=0;x<W;++x)
        p[(size_t)y*W+x]=X4_RGB(10+(unsigned)y/40,24+(unsigned)x/70,42+(unsigned)y/20);
    x4_rect(p,960,515,2,50,X4_RGB(210,230,225));
    x4_rect(p,936,539,50,2,X4_RGB(210,230,225));
    x4_rect(p,45,970,330,5,X4_RGB(85,198,155));
    x4_rect(p,1450,970,425,5,X4_RGB(140,180,200));
}
static void check_guards(const uint32_t *raw)
{for(unsigned i=0;i<GUARD;++i)assert(raw[i]==0xa145ba20&&raw[(size_t)W*H+GUARD+i]==0xa145ba20);}
static void save(const char *path,const uint32_t *p)
{
    FILE *f=fopen(path,"wb");assert(f);assert(fprintf(f,"P6\n1920 1080\n255\n")>0);
    for(size_t i=0;i<(size_t)W*H;++i){unsigned char rgb[3]={(unsigned char)p[i],(unsigned char)(p[i]>>8),(unsigned char)(p[i]>>16)};assert(fwrite(rgb,1,3,f)==3);}
    assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==2);x4_lang_set(X4_LANG_ES,"es-MX","MX");
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    const size_t pixels=(size_t)W*H;
    uint32_t *raw=malloc((pixels+2*GUARD)*sizeof(*raw)),*baseline=malloc(pixels*sizeof(*baseline));assert(raw&&baseline);
    for(size_t i=0;i<pixels+2*GUARD;++i)raw[i]=0xa145ba20;
    uint32_t *p=raw+GUARD;synthetic_frame(baseline);
    X4LiveMediaSnapshot media={0};X4SessionSnapshot session={.state=X4_SESSION_STREAMING,.rtc_connected=1};
    X4TouchMenu touch;x4_touch_menu_init(&touch,X4_PUBLIC!=0);
    StreamOverlayContext context={&media,&session,0,&touch,false,false};
#if !X4_PUBLIC
    x4_script_engine_test_clock(sceKernelGetProcessTime,2000);
    x4_script_engine_set_toast("Turbo activado");
    for(unsigned frame=0;frame<2;++frame){
        clock_us=10000000+(frame?2999000:0);
        memcpy(p,baseline,pixels*sizeof(*p));stream_overlay(p,&context);
        assert(memcmp(p,baseline,pixels*sizeof(*p))!=0);check_guards(raw);
        unsigned changed=0;
        for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(p[(size_t)y*W+x]!=baseline[(size_t)y*W+x]){
            assert(y>=36&&y<80&&x>=0&&x<W);++changed;
        }
        assert(changed>500);
        if(!frame)save(argv[1],p);
    }
    clock_us=13000000;memcpy(p,baseline,pixels*sizeof(*p));stream_overlay(p,&context);
    assert(!memcmp(p,baseline,pixels*sizeof(*p)));check_guards(raw);
    puts("PASS Lab actual main stream_overlay + actual toast: visible at0/2999ms over synthetic streaming frame; absent at3000ms; guarded bounds; capture es");
#else
    (void)argv;
    memcpy(p,baseline,pixels*sizeof(*p));stream_overlay(p,&context);
    assert(!memcmp(p,baseline,pixels*sizeof(*p)));check_guards(raw);
    (void)save;
    puts("PASS Public actual main stream_overlay: no script notification call or framebuffer change");
#endif
    free(raw);free(baseline);x4_touch_menu_cache_reset();x4_font_close();return 0;
}
