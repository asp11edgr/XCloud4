/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/apple_ui.h"
#include "../src/ui/lang.h"
#include "../src/ui/font.h"
#include "../src/ui/stream_settings.h"
#include "../src/core/build_identity.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELEASES "https://github.com/asp11edgr/XCloud4/releases"
static bool record_text;
static bool title_rendered,url_rendered;
static char body_rendered[512],description_rendered[512];
void __real_x4_text_px(uint32_t *,int,int,int,int,const char *,uint32_t);
void __wrap_x4_text_px(uint32_t *pixels,int x,int y,int size,int weight,const char *text,uint32_t color)
{
    char *out=NULL;
    if(record_text){
        int width=x4_text_width_px(size,weight,text);
        assert(x>=300&&y>=218);
        assert(x+width<=1620&&y+size<=862);
    }
    if(record_text&&x==362&&y>=362&&y<530){
        assert(x4_text_width_px(size,weight,text)<=700);assert(y+size<=530);out=body_rendered;
    }
    if(record_text&&x==1138&&y>=728&&y<=784){
        assert(x4_text_width_px(size,weight,text)<=420);assert(y+size<=812);out=description_rendered;
    }
    if(out){
        size_t used=strlen(out),n=strlen(text);assert(used+n+2<512);
        if(used)out[used++]=' ';
        memcpy(out+used,text,n+1);
    }
    __real_x4_text_px(pixels,x,y,size,weight,text,color);
}
void __real_x4_text_ellipsis_px(uint32_t *,int,int,int,int,const char *,int,uint32_t);
void __wrap_x4_text_ellipsis_px(uint32_t *pixels,int x,int y,int size,int weight,
    const char *text,int max_width,uint32_t color)
{
    if(record_text){
        int width=x4_text_width_px(size,weight,text);
        assert(width<=max_width); /* Fixtures must fit without truncation. */
        assert(x>=300&&y>=218&&x+width<=1620&&y+size<=862);
        if(x==362&&y==274){
            assert(!strcmp(text,x4_tr(X4_TR_UPDATE_NOTICE_TITLE)));title_rendered=true;
        }
        if(x==1138&&y==824){
            assert(!strcmp(text,"github.com/asp11edgr/XCloud4/releases"));url_rendered=true;
        }
    }
    __real_x4_text_ellipsis_px(pixels,x,y,size,weight,text,max_width,color);
}
static void model(void)
{
    X4UpdateNotice notice={0};
    assert(!x4_apple_update_notice_offer(&notice,"1.2.0-beta","v1.2.0-beta",false,false));
    assert(!notice.open&&!notice.shown);
    assert(x4_apple_update_notice_offer(&notice,"1.2.0-beta","v1.2.0-beta",true,false));
    assert(notice.open&&notice.shown&&!strcmp(notice.url,RELEASES "/tag/v1.2.0-beta"));
    assert(x4_apple_update_notice_handle(&notice,false)&&notice.open);
    assert(!x4_apple_update_notice_offer(&notice,"9.9.9",NULL,true,false));
    assert(!strcmp(notice.version,"1.2.0-beta"));
    assert(x4_apple_update_notice_handle(&notice,true)&&!notice.open);
    assert(!x4_apple_update_notice_handle(&notice,true));
    assert(!x4_apple_update_notice_offer(&notice,"1.2.0-beta","v1.2.0-beta",true,false));
    X4UpdateNotice testing={0};
    assert(x4_apple_update_notice_offer(&testing,"9.9.9",NULL,true,true));
    assert(!testing.shown&&!strcmp(testing.url,RELEASES));
    assert(x4_apple_update_notice_handle(&testing,true));
    assert(x4_apple_update_notice_offer(&testing,"1.2.0-beta","v1.2.0-beta",true,false));
    assert(testing.shown);
    const char *invalid[]={NULL,"","https://untrusted.invalid/x","../other","v1.2.0?x=y","v1.2.0\r\nX: y",".",".."};
    for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){
        X4UpdateNotice bad={0};
        assert(x4_apple_update_notice_offer(&bad,"9.9.9",invalid[i],true,true));
        assert(!strcmp(bad.url,RELEASES));
    }
    char long_tag[66];memset(long_tag,'a',sizeof(long_tag)-1);long_tag[sizeof(long_tag)-1]=0;
    X4UpdateNotice bad={0};
    assert(x4_apple_update_notice_offer(&bad,"9.9.9",long_tag,true,true));assert(!strcmp(bad.url,RELEASES));
    X4UpdateNotice version={0};
    assert(!x4_apple_update_notice_offer(&version,"\n",NULL,true,false));
    assert(!version.shown);
    puts("PASS update notice model: Home eligibility, once/startup, Circle-only, test independence, bounded controlled URLs");
}
static void draw_checked(const X4UpdateNotice *notice,uint32_t *p)
{
    body_rendered[0]=description_rendered[0]=0;title_rendered=url_rendered=false;record_text=true;
    x4_apple_update_notice_draw(notice,p);record_text=false;
    char expected[192];
    snprintf(expected,sizeof(expected),x4_tr(X4_TR_UPDATE_NOTICE_VERSION_FMT),notice->version,X4_PRODUCT_VERSION);
    assert(strstr(expected,notice->version)&&strstr(expected,X4_PRODUCT_VERSION));
    assert(!strcmp(body_rendered,expected));
    assert(!strcmp(description_rendered,x4_tr(X4_TR_UPDATE_NOTICE_DESCRIPTION)));
    assert(strstr(description_rendered,"GoldHEN > Package Installer."));
    assert(title_rendered&&url_rendered);
}
static void save(const char *dir,const char *name,const uint32_t *p)
{
    char path[1024];assert(snprintf(path,sizeof(path),"%s/%s.ppm",dir,name)<(int)sizeof(path));
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n1920 1080\n255\n");
    for(size_t i=0;i<1920u*1080u;++i){unsigned char rgb[3]={(unsigned char)p[i],(unsigned char)(p[i]>>8),(unsigned char)(p[i]>>16)};
        assert(fwrite(rgb,1,3,f)==3);}
    assert(!fclose(f));
}
static void captures(const char *dir)
{
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    size_t count=1920u*1080u;
    uint32_t *owned=malloc((count+2u)*sizeof(*owned)),*before=malloc(count*sizeof(*before));
    assert(owned&&before);owned[0]=owned[count+1u]=0x4a2b8c01u;uint32_t *p=owned+1;
    for(unsigned lang=0;lang<2;++lang){
        const char *locale=lang?"en":"es";x4_lang_set((enum X4Lang)lang,lang?"en-US":"es-MX",lang?"US":"MX");
        assert(!strcmp(x4_tr(X4_TR_UPDATE_NOTICE_TITLE),lang?"New version available":"Nueva versión disponible"));
        assert(!strcmp(x4_tr(X4_TR_UPDATE_NOTICE_VERSION_FMT),lang?
            "XCloud4 %s is available. You have %s.":"XCloud4 %s ya está disponible. Tienes la %s."));
        assert(!strcmp(x4_tr(X4_TR_UPDATE_NOTICE_DESCRIPTION),lang?
            "Scan it to download it from GitHub. Install it with GoldHEN > Package Installer.":
            "Escanéalo para descargarla desde GitHub. Instálala con GoldHEN > Package Installer."));
        /* Synthetic future release fixture. No HTTP or existing release is
         * claimed; the capture manifest labels this URL as an encoding test. */
        X4UpdateNotice synthetic={0};
#if X4_PUBLIC
        assert(x4_apple_update_notice_offer(&synthetic,"9.9.9","v9.9.9",true,false));
#else
        assert(x4_apple_update_notice_offer(&synthetic,"9.9.9","lab-9.9.9",true,false));
#endif
        x4_apple_bg_draw(p);draw_checked(&synthetic,p);
        char name[96];snprintf(name,sizeof(name),"update-notice-synthetic-valid-tag-%s",locale);save(dir,name,p);
        X4UpdateNotice fallback={0};assert(x4_apple_update_notice_offer(&fallback,"9.9.9",NULL,true,true));
        x4_apple_bg_draw(p);draw_checked(&fallback,p);
        snprintf(name,sizeof(name),"update-notice-test-generic-%s",locale);save(dir,name,p);
        x4_apple_bg_draw(p);memcpy(before,p,count*sizeof(*p));
        x4_apple_draw_update_test_action(p,200,950);
#if X4_PUBLIC
        assert(!memcmp(before,p,count*sizeof(*p)));
#else
        assert(memcmp(before,p,count*sizeof(*p)));
        X4StreamSettingsUi draft={.open=true,.profile=X4_REQUEST_1080P_G8,
            .cpu=X4_CPU_0,.depth=X4_DEPTH_AUTO};
        x4_apple_bg_draw(p);x4_stream_settings_ui_draw(&draft,p);
        x4_apple_draw_update_test_action(p,310,924);
        snprintf(name,sizeof(name),"update-notice-lab-r3-action-%s",locale);save(dir,name,p);
#endif
        assert(owned[0]==0x4a2b8c01u&&owned[count+1u]==0x4a2b8c01u);
    }
    free(before);free(owned);x4_font_close();
    printf("PASS update notice captures public=%d: ES/EN, valid-tag/generic QR, framebuffer bounds, Lab-only action\n",X4_PUBLIC);
    puts("PASS update text: exact ES/EN title/body/instructions, offered and installed versions, complete wrapping inside card");
}
int main(int argc,char **argv)
{
    model();if(argc>1)captures(argv[1]);return 0;
}
