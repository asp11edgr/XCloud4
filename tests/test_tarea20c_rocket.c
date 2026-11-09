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
#include "../src/ui/ui2_theme.h"

enum { W=1920,H=1080,AX=800,AY=300,AW=320,AH=180 };
#define PIXELS ((size_t)W*H)
static uint32_t *owned,*pixels;
static void capture(const char *dir,unsigned phase,const char *language)
{
    if(!dir)return;
    char path[1024];snprintf(path,sizeof(path),"%s/47_cloud_ring_phase_%02u_%s.ppm",dir,phase,language);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",W,H);
    unsigned char *row=malloc(W*3);assert(row);
    for(unsigned y=0;y<H;++y){
        for(unsigned x=0;x<W;++x){uint32_t v=pixels[(size_t)y*W+x];row[x*3]=(unsigned char)v;row[x*3+1]=(unsigned char)(v>>8);row[x*3+2]=(unsigned char)(v>>16);}
        assert(fwrite(row,3,W,f)==W);
    }
    free(row);assert(!fclose(f));
}
static unsigned count_color(uint32_t color)
{
    unsigned count=0;
    for(unsigned y=AY;y<AY+AH;++y)for(unsigned x=AX;x<AX+AW;++x)
        if(pixels[(size_t)y*W+x]==color)++count;
    return count;
}
static void step_mapping(void)
{
    assert(x4_loading_screen_step(X4_LOADING_REQUEST)==X4_LOADING_STEP_SESSION);
    assert(x4_loading_screen_step(X4_LOADING_RESOURCES)==X4_LOADING_STEP_SERVER);
    assert(x4_loading_screen_step(X4_LOADING_AUTHORIZE)==X4_LOADING_STEP_SERVER);
    assert(x4_loading_screen_step(X4_LOADING_CONNECT)==X4_LOADING_STEP_OPTIMIZE);
    assert(x4_loading_screen_step(X4_LOADING_CONNECTED)==X4_LOADING_STEP_OPTIMIZE);
    assert(x4_loading_screen_step(X4_LOADING_FIRST_PICTURE)==X4_LOADING_STEP_READY);
    assert(x4_loading_screen_step(X4_LOADING_COMPLETE)==X4_LOADING_STEPS);
    assert(x4_loading_screen_step(X4_LOADING_ERROR)==X4_LOADING_STEP_INVALID);
}

static void cloud_frames(const char *dir,unsigned language)
{
    x4_lang_set(language?X4_LANG_EN:X4_LANG_ES,language?"en-US":"es-MX",language?"US":"MX");
    const char *suffix=language?"en":"es";
    const uint64_t begin=1000000;
    assert(x4_loading_screen_begin(language?"Sample adventure":"Aventura de prueba",NULL,begin));
    assert(x4_loading_screen_draw(pixels,begin));
    assert(!x4_loading_screen_draw(pixels,begin));
    uint32_t *first=malloc((size_t)AW*AH*4);assert(first);
    for(unsigned y=0;y<AH;++y)memcpy(first+(size_t)y*AW,pixels+(size_t)(AY+y)*W+AX,AW*4);
    unsigned lit=count_color(X4_UI2_ACCENT);
    assert(lit>900&&lit<1450);
    capture(dir,0,suffix);
    /* The animated ring must write only to the existing bounded zone. */
    for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)
        if(x<AX||x>=AX+AW||y<AY||y>=AY+AH)pixels[(size_t)y*W+x]=0xff273849u;
    for(unsigned phase=1;phase<=32;++phase){
        uint64_t now=begin+(uint64_t)phase*33334;
        assert(x4_loading_screen_draw(pixels,now));
        assert(owned[0]==0xabcdef12u&&owned[PIXELS+1]==0xabcdef12u);
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x)
            if(x<AX||x>=AX+AW||y<AY||y>=AY+AH)assert(pixels[(size_t)y*W+x]==0xff273849u);
        if(phase==16||phase==32){
            bool equal=true;
            for(unsigned y=0;y<AH;++y)
                equal&=!memcmp(first+(size_t)y*AW,pixels+(size_t)(AY+y)*W+AX,AW*4);
            assert(equal==(phase==32));
        }
    }
    free(first);x4_loading_screen_end();
    /* Fresh frames for review also show the fixed cloud, title and stage rail. */
    const unsigned phases[3]={8,16,24};
    for(unsigned i=0;i<3;++i){
        assert(x4_loading_screen_begin(language?"Sample adventure":"Aventura de prueba",NULL,begin));
        assert(x4_loading_screen_draw(pixels,begin+(uint64_t)phases[i]*33334));
        capture(dir,phases[i],suffix);x4_loading_screen_end();
    }
    assert(x4_loading_screen_begin("XCloud4",NULL,begin));
    x4_loading_screen_first_picture(begin);
    assert(x4_loading_screen_draw(pixels,begin));
    assert(count_color(X4_UI2_ACCENT)>3000);
    assert(!x4_loading_screen_transition_ready(begin+159999));
    assert(x4_loading_screen_transition_ready(begin+160000));
    x4_loading_screen_end();
}

int main(int argc,char **argv)
{
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    owned=malloc((PIXELS+2)*4);assert(owned);owned[0]=owned[PIXELS+1]=0xabcdef12u;pixels=owned+1;
    step_mapping();
    for(unsigned language=0;language<2;++language)cloud_frames(argc>1?argv[1]:NULL,language);
    free(owned);x4_font_close();
    puts("PASS T47 cloud ring: exact32-frame cycle, 320x180-only writes, four stage steps, ready ring and160ms transition, ES/EN");return 0;
}
