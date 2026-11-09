/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
/* Host capture: real UI over an original synthetic frame, no game resources. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/screen.h"
#include "../src/ui/apple_ui.h"
#include "../src/ui/font.h"
#include "../src/ui/canvas.h"
#include "../src/ui/lang.h"
#include "../src/ui/net_hud.h"
#include "../src/input/touch_menu.h"

static uint64_t fixture_time=1000000;
uint64_t sceKernelGetProcessTime(void) {return fixture_time;}
const char *x4_server_settings_active_display_name(void) {return "";}
const char *x4_picture_settings_active_quality_label(void) {return "1080p";}
const char *x4_access_country_label(X4AccessCountry c) {(void)c;return "";}

enum {W=1920,H=1080};
static void frame(uint32_t *p)
{
    for(int y=0;y<H;++y)for(int x=0;x<W;++x) {
        unsigned r=12+(unsigned)y/40,g=25+(unsigned)x/65,b=40+(unsigned)y/16;
        p[(size_t)y*W+x]=X4_RGB(r,g,b);
    }
    x4_apple_fill_round_rect(p,1080,250,450,530,30,X4_RGB(25,65,72));
    x4_apple_fill_round_rect(p,1230,370,120,340,24,X4_RGB(95,140,122));
    x4_rect(p,40,965,1840,3,X4_RGB(55,98,110));
}
static void set_mode(unsigned mode)
{
    for(unsigned i=0;i<4 && x4_net_hud_mode()!=mode;++i)(void)x4_net_hud_cycle_mode();
    assert(x4_net_hud_mode()==mode);
}
static void snapshots(X4LiveMediaSnapshot *media,X4SessionSnapshot *session,bool warn)
{
    *media=(X4LiveMediaSnapshot){.width=1920,.height=1080,.started=true,.video_ready=true,
        .video_frames=1,.video_present_calls=1};
    *session=(X4SessionSnapshot){.state=X4_SESSION_STREAMING,.rtt_ms=18,.ever_connected=1};
    (void)x4_net_hud_prepare(media,media,session,0,fixture_time);
    /* More than 60 genuinely distinct presentation intervals; no 0/>250 ms
     * values. The last 60 render the green, amber and red graph categories. */
    static const unsigned intervals[]={16667,16667,16667,25000,16667,36000,10000};
    for(unsigned i=0;i<80;++i) {
        fixture_time+=intervals[i%(sizeof(intervals)/sizeof(intervals[0]))];
        ++media->video_present_calls;++media->video_frames;
        media->video_packets+=12;media->audio_packets+=1;
        media->video_bytes+=25000;media->audio_bytes+=200;
        (void)x4_net_hud_prepare(media,media,session,0,fixture_time);
    }
    /* Explicit final one-second measurement keeps the fixture independent of
     * the graph's irregular frame intervals and activates a real drop cause. */
    fixture_time+=1000000;media->video_present_calls+=60;media->video_frames+=60;
    media->video_packets+=1000;media->audio_packets+=100;
    media->video_bytes+=1530000;media->audio_bytes+=20000;
    if(warn)++media->video_dropped_frames;
    (void)x4_net_hud_prepare(media,media,session,0,fixture_time);
    X4NetHudSnapshot hud;x4_net_hud_snapshot(&hud);
    assert(hud.have_stats && hud.warning==warn);
    assert(hud.width<=560 && hud.x>=0 && hud.y>=0);
}
static void save(const char *directory,const char *state,const char *language,const uint32_t *p)
{
    char path[1024];int n=snprintf(path,sizeof(path),"%s/%s_%s_%s.ppm",directory,
        X4_PUBLIC?"public":"lab",state,language);
    assert(n>0 && (size_t)n<sizeof(path));FILE *f=fopen(path,"wb");assert(f);
    assert(fprintf(f,"P6\n1920 1080\n255\n")>0);
    for(size_t i=0;i<(size_t)W*H;++i) {
        unsigned char rgb[3]={(unsigned char)p[i],(unsigned char)(p[i]>>8),(unsigned char)(p[i]>>16)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==5);bool es=!strcmp(argv[3],"es");assert(es || !strcmp(argv[3],"en"));
    bool menu=!strcmp(argv[2],"menu"),basic=!strcmp(argv[2],"basic");
    bool full=!strcmp(argv[2],"full_warning"),warning=!strcmp(argv[2],"warning");
    assert(menu || basic || full || warning);
    assert(X4_PUBLIC || (menu && es));
    x4_lang_set(es?X4_LANG_ES:X4_LANG_EN,es?"es-MX":"en-US",es?"MX":"US");
    assert(!x4_font_init("assets/fonts/inter.x4font"));x4_apple_set_animation_phase(0);
    x4_net_hud_test_reset(argv[4]);set_mode(full?X4_NET_HUD_FULL:warning?X4_NET_HUD_WARNING:X4_NET_HUD_BASIC);
    uint32_t *raw=malloc(((size_t)W*H+32)*sizeof(*raw));assert(raw);
    for(size_t i=0;i<(size_t)W*H+32;++i)raw[i]=0xac512943;
    uint32_t *p=raw+16;frame(p);
    X4TouchMenu touch;x4_touch_menu_init(&touch,X4_PUBLIC!=0);touch.is_open=menu;touch.selected_index=4;
    X4SessionSnapshot session={0};X4LiveMediaSnapshot media={0};
    if(menu)session=(X4SessionSnapshot){.state=X4_SESSION_STREAMING,.rtt_ms=18};
    else {snapshots(&media,&session,full||warning);assert(x4_net_hud_draw(p));}
    /* Same order as streaming: HUD first, existing notices/menu afterwards. */
    x4_live_overlay(&media,&session,0,&touch,false,p);
    for(unsigned i=0;i<16;++i)assert(raw[i]==0xac512943 && raw[(size_t)W*H+16+i]==0xac512943);
    save(argv[1],argv[2],argv[3],p);
    printf("PASS host capture %s/%s/%s: real HUD/menu, original frame, guarded 1920x1080, no network or console execution\n",X4_PUBLIC?"public":"lab",argv[2],argv[3]);
    free(raw);x4_touch_menu_cache_reset();x4_font_close();return 0;
}
