/* SPDX-License-Identifier: GPL-3.0-only */
/* Four requested states through the real account and opt-in renderers. No
 * authentication, session-store mount or network operation is performed. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/screen.h"
#include "../src/ui/apple_ui.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"

enum { W=1920,H=1080,PIXELS=W*H };
static bool in_warning;
static char shown_warning[1024];
static size_t shown_bytes;

/* Observe real text geometry without replacing font drawing or wrapping. */
void x4_text_px_impl(uint32_t *,int,int,int,int,const char *,uint32_t);
void x4_text_ellipsis_px_impl(uint32_t *,int,int,int,int,const char *,int,uint32_t);
static void observe(int x,int y,int size,int weight,const char *text)
{
    assert(text);
    int width=x4_text_width_px(size,weight,text);
    assert(x>=0 && y>=0 && x+width<=W && y+size<=H);
    if(in_warning) {
        assert(x>=430 && x+width<=1490 && y>=280 && y+size<=810);
        if(size==27 && y>=416 && y<710) {
            size_t n=strlen(text);
            assert(shown_bytes+n+2<sizeof(shown_warning));
            if(shown_bytes)shown_warning[shown_bytes++]=' ';
            memcpy(shown_warning+shown_bytes,text,n+1);shown_bytes+=n;
        }
    }
}
void x4_text_px(uint32_t *p,int x,int y,int size,int weight,const char *text,uint32_t color)
{observe(x,y,size,weight,text);x4_text_px_impl(p,x,y,size,weight,text,color);}
void x4_text_ellipsis_px(uint32_t *p,int x,int y,int size,int weight,const char *text,int max_width,uint32_t color)
{
    char fit[1024];x4_text_fit_utf8(fit,sizeof(fit),size,weight,text,max_width);
    observe(x,y,size,weight,fit);
    x4_text_ellipsis_px_impl(p,x,y,size,weight,text,max_width,color);
}

static void save_ppm(const char *path,const uint32_t *pixels)
{
    FILE *f=fopen(path,"wb");assert(f);
    assert(fprintf(f,"P6\n1920 1080\n255\n")>0);
    for(size_t i=0;i<PIXELS;++i) {
        unsigned char rgb[3]={(unsigned char)pixels[i],(unsigned char)(pixels[i]>>8),
                              (unsigned char)(pixels[i]>>16)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}

int main(int argc,char **argv)
{
    assert(argc==4);
    const char *state=argv[2],*language=argv[3];
    assert(!strcmp(state,"option") || !strcmp(state,"warning"));
    assert(!strcmp(language,"es") || !strcmp(language,"en"));
    bool warning=!strcmp(state,"warning"),spanish=!strcmp(language,"es");
    x4_lang_set(spanish?X4_LANG_ES:X4_LANG_EN,spanish?"es-MX":"en-US",spanish?"MX":"US");
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    x4_apple_set_animation_phase(0);
    X4HomeDrawContext home={0};x4_screen_set_home_context(&home);
    X4AuthSnapshot account={.state=X4_AUTH_AUTHORIZED};
    snprintf(account.stage,sizeof(account.stage),"cuenta Microsoft autorizada");
    uint32_t *guarded=malloc((PIXELS+32u)*sizeof(*guarded));assert(guarded);
    for(size_t i=0;i<PIXELS+32u;++i)guarded[i]=0xa5c37f11;
    uint32_t *pixels=guarded+16;
    x4_auth_draw(&account,0,0,pixels);
    in_warning=warning;
    x4_auth_remember_draw(false,true,warning,pixels);
    if(warning) assert(!strcmp(shown_warning,x4_tr(X4_TR_AUTH_REMEMBER_WARNING_BODY)));
    for(unsigned i=0;i<16;++i)
        assert(guarded[i]==0xa5c37f11 && guarded[PIXELS+16u+i]==0xa5c37f11);
    char path[1024];int n=snprintf(path,sizeof(path),"%s/account_%s_%s.ppm",argv[1],state,language);
    assert(n>0 && (size_t)n<sizeof(path));save_ppm(path,pixels);
    printf("PASS capture account %s/%s: real render, default-off, bounds%s; no network or saved credentials\n",
        state,language,warning?", complete warning inside card":"");
    free(guarded);x4_font_close();return 0;
}
