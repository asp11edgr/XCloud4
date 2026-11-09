/* SPDX-License-Identifier: GPL-3.0-only */
/* Real UI renderers over an original synthetic frozen game frame. */
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
#include "../src/input/touch_menu.h"

uint64_t sceKernelGetProcessTime(void) {return 10000000;}
const char *x4_server_settings_active_display_name(void) {return "";}
const char *x4_picture_settings_active_quality_label(void) {return "1080p";}
const char *x4_access_country_label(X4AccessCountry country) {(void)country;return "";}

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
static void save(const char *directory,const char *state,const char *language,const uint32_t *p)
{
    char path[1024];int n=snprintf(path,sizeof(path),"%s/%s_%s.ppm",directory,state,language);
    assert(n>0&&(size_t)n<sizeof(path));FILE *f=fopen(path,"wb");assert(f);
    assert(fprintf(f,"P6\n1920 1080\n255\n")>0);
    for(size_t i=0;i<(size_t)W*H;++i) {
        unsigned char rgb[3]={(unsigned char)p[i],(unsigned char)(p[i]>>8),(unsigned char)(p[i]>>16)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}
int main(int argc,char **argv)
{
    assert(argc==4);bool es=!strcmp(argv[3],"es");
    assert(es||!strcmp(argv[3],"en"));bool menu=!strcmp(argv[2],"menu");
    assert(menu||!strcmp(argv[2],"reconnect"));
    x4_lang_set(es?X4_LANG_ES:X4_LANG_EN,es?"es-MX":"en-US",es?"MX":"US");
    assert(!x4_font_init("assets/fonts/inter.x4font"));x4_apple_set_animation_phase(0);
    uint32_t *raw=malloc(((size_t)W*H+32)*sizeof(*raw));assert(raw);
    for(size_t i=0;i<(size_t)W*H+32;++i)raw[i]=0xac512943;
    uint32_t *p=raw+16;frame(p);
    X4TouchMenu touch;x4_touch_menu_init(&touch,X4_PUBLIC!=0);
    touch.is_open=menu;touch.selected_index=3;
    X4SessionSnapshot session={.state=X4_SESSION_STREAMING,.reconnecting=!menu,.reconnect_attempt=3};
    X4LiveMediaSnapshot media={0};
    x4_live_overlay(&media,&session,0,&touch,false,p);
    for(unsigned i=0;i<16;++i)assert(raw[i]==0xac512943&&raw[(size_t)W*H+16+i]==0xac512943);
    save(argv[1],argv[2],argv[3],p);
    printf("PASS real UI %s/%s: frozen synthetic frame, guarded bounds, localized text, no network or PS4 execution\n",argv[2],argv[3]);
    free(raw);x4_touch_menu_cache_reset();x4_font_close();return 0;
}
