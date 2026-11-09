/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/loading_screen.h"
#include "../src/ui/apple_ui.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#define W 1920
#define H 1080
#define PIXELS ((size_t)W*H)
static uint32_t *pixels,*fresh;
static void capture(const char *dir,const char *stage,const char *lang,const char *background)
{
    if(!dir)return;
    char path[1024];snprintf(path,sizeof(path),"%s/20c_queue_%s_%s_%s.ppm",dir,stage,background,lang);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",W,H);
    unsigned char row[W*3];
    for(unsigned y=0;y<H;++y){
        for(unsigned x=0;x<W;++x){uint32_t v=pixels[(size_t)y*W+x];row[x*3]=(unsigned char)v;row[x*3+1]=(unsigned char)(v>>8);row[x*3+2]=(unsigned char)(v>>16);}
        assert(fwrite(row,3,W,f)==W);
    }
    assert(!fclose(f));
}
static void queue_ui(const char *dir,const X4CoverRaster *game,const char *background,unsigned lang)
{
    x4_lang_set(lang?X4_LANG_EN:X4_LANG_ES,lang?"en-US":"es-MX",lang?"US":"MX");
    const char *language=lang?"en":"es";
    assert(x4_loading_screen_begin("Original neutral queue fixture",game,1000000));
    X4SessionSnapshot s={.state=X4_SESSION_WAITING,.waiting_for_resources=1};
    x4_loading_screen_update(&s,1000000);assert(x4_loading_screen_draw(pixels,1000000));
    X4LoadingView view;x4_loading_screen_snapshot(&view);
    assert(view.waiting_for_resources&&!view.queue_eta_available&&view.percent==20&&!view.first_picture);
    capture(dir,"elapsed_0_00",language,background);
    s.queue_elapsed_seconds=181;s.queue_eta_available=1;s.queue_eta_seconds=119;
    x4_loading_screen_update(&s,182000000);assert(x4_loading_screen_draw(pixels,182000000));
    x4_loading_screen_snapshot(&view);assert(view.queue_elapsed_seconds==181&&view.queue_eta_seconds==119&&view.percent==20);
    capture(dir,"elapsed_3_01_eta_1_59",language,background);
    /* While the elapsed estimate changes, the rail and progress regions
     * keep their cached generations. Reuse the same animation tick too. */
    const size_t rail=(size_t)746*W+700;
    const size_t bar=(size_t)842*W+700;
    uint32_t rail_before=pixels[rail],bar_before=pixels[bar];
    pixels[rail]=pixels[bar]=0xff273849u;
    ++s.queue_elapsed_seconds;
    x4_loading_screen_update(&s,182000001);
    assert(x4_loading_screen_draw(pixels,182000001));
    assert(pixels[rail]==0xff273849u&&pixels[bar]==0xff273849u);
    pixels[rail]=rail_before;pixels[bar]=bar_before;
    s.queue_elapsed_seconds=500;s.queue_eta_available=0;
    x4_loading_screen_update(&s,501000000);assert(x4_loading_screen_draw(pixels,501000000));
    x4_loading_screen_snapshot(&view);assert(!view.queue_eta_available&&view.percent==20);
    capture(dir,"elapsed_8_20_no_eta",language,background);
    /* A fresh destination must match the incrementally maintained canvas. */
    assert(x4_loading_screen_draw(fresh,501000000));assert(!memcmp(pixels,fresh,PIXELS*4));
    s.state=X4_SESSION_READY;s.waiting_for_resources=0;
    x4_loading_screen_update(&s,502000000);assert(x4_loading_screen_draw(pixels,502000000));
    x4_loading_screen_snapshot(&view);assert(!view.waiting_for_resources&&view.percent==35);
    capture(dir,"queue_exit_ready",language,background);
    assert(x4_loading_screen_draw(fresh,502000000));assert(!memcmp(pixels,fresh,PIXELS*4));
    s.state=X4_SESSION_ERROR;s.error=-2013;s.queue_limit_reached=1;
    x4_loading_screen_update(&s,901000000);assert(!x4_loading_screen_draw(pixels,901000000));
    x4_loading_screen_snapshot(&view);assert(view.stage==X4_LOADING_ERROR);
    x4_loading_screen_end();
}
int main(int argc,char **argv)
{
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    pixels=malloc(PIXELS*4);fresh=malloc(PIXELS*4);assert(pixels&&fresh);
    X4CoverRaster neutral={W,H,malloc(PIXELS*4)};assert(neutral.rgba);
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)neutral.rgba[(size_t)y*W+x]=X4_RGB(25+x*40/W,40+y*50/H,90);
    for(unsigned lang=0;lang<2;++lang){queue_ui(argc>1?argv[1]:NULL,NULL,"own",lang);queue_ui(argc>1?argv[1]:NULL,&neutral,"neutral",lang);}
    free(neutral.rgba);free(fresh);free(pixels);x4_font_close();
    puts("PASS TAREA20c queue loading ES/EN: elapsed/optional ETA, no fake progress, queue exit clears text, error hands off, incremental=fresh");return 0;
}
