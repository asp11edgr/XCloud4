/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/access_country.h"
#include "../src/ui/apple_ui.h"
#include "../src/ui/catalog_gui.h"
#include "../src/ui/picture_settings.h"
#include "../src/ui/server_settings.h"
#include "../src/ui/screen.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int x4_catalog_assets_acquire(X4CatalogAssets *a,const char *id,X4CatalogAssetView *v)
{ (void)a;(void)id;(void)v;return 0; }
void x4_catalog_assets_release(X4CatalogAssets *a,X4CatalogAssetView *v)
{ (void)a;(void)v; }

enum { W=1920,H=1080,PIXELS=W*H };
static void save(const char *dir,const char *name,const char *language,const uint32_t *p)
{
    char path[1024];assert(snprintf(path,sizeof(path),"%s/%s-%s.ppm",dir,name,language)<(int)sizeof(path));
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n1920 1080\n255\n");
    for(size_t i=0;i<PIXELS;++i){unsigned char rgb[3]={(unsigned char)p[i],(unsigned char)(p[i]>>8),(unsigned char)(p[i]>>16)};
        assert(fwrite(rgb,1,3,f)==3);}
    assert(!fclose(f));
}
static void settings_capture(const char *dir,const char *language,uint32_t *p)
{
    X4PictureSettingsUi settings={.open=true,.selected=1,.server_selected=0};
    for(unsigned tab=0;tab<2;++tab){
        settings.tab=tab;x4_apple_bg_draw(p);x4_picture_settings_ui_draw(&settings,p);
        save(dir,tab?"settings-server":"settings-picture",language,p);
    }
    settings.first_time_mode=true;settings.tab=1;
    x4_apple_bg_draw(p);x4_picture_settings_ui_draw(&settings,p);save(dir,"settings-first-server",language,p);
#ifndef X4_T22_BASELINE_CAPTURE
    settings.first_time_mode=false;settings.tab=2;x4_access_country_ui_open(&settings.access_ui);
    settings.access_ui.selected=X4_ACCESS_US;
    x4_apple_bg_draw(p);x4_picture_settings_ui_draw(&settings,p);save(dir,"settings-access",language,p);
    settings.access_ui.confirming=true;
    x4_apple_bg_draw(p);x4_picture_settings_ui_draw(&settings,p);save(dir,"settings-access-confirm",language,p);
#endif
}
#ifndef X4_T22_BASELINE_CAPTURE
static void rectangle_equal(const uint32_t *a,const uint32_t *b,int x,int y,int w,int h)
{
    for(int row=y;row<y+h;++row)for(int col=x;col<x+w;++col)
        assert(a[(size_t)row*W+col]==b[(size_t)row*W+col]);
}
static void only_toast_changed(const uint32_t *actual,const uint32_t *before,int x,int y,int w,int h)
{
    unsigned changed=0;
    assert(x>=0&&y>=0&&x+w<=W&&y+h<=H);
    for(int row=0;row<H;++row)for(int col=0;col<W;++col){
        size_t offset=(size_t)row*W+col;
        if(actual[offset]==before[offset])continue;
        ++changed;
        assert(col>=x&&col<x+w&&row>=y&&row<y+h);
    }
    assert(changed>0);
}
static void cached_access(void)
{
    uint32_t *base=malloc(PIXELS*sizeof(*base)),*actual=malloc(PIXELS*sizeof(*actual)),*expected=malloc(PIXELS*sizeof(*expected));
    assert(base&&actual&&expected);x4_apple_bg_draw(base);memcpy(actual,base,PIXELS*sizeof(*actual));
    X4PictureSettingsUi settings={.open=true,.tab=0};
    const unsigned tabs[]={0,1,2,2,2,1,0};
    for(unsigned frame=0;frame<sizeof(tabs)/sizeof(tabs[0]);++frame){
        settings.tab=tabs[frame];settings.access_ui.selected=X4_ACCESS_US;
        settings.access_ui.confirming=frame==4;
        memcpy(expected,base,PIXELS*sizeof(*expected));
        x4_picture_settings_ui_draw(&settings,expected);
        x4_picture_settings_ui_draw_cached(&settings,actual);
        if(memcmp(actual,expected,PIXELS*sizeof(*actual))){
            for(size_t i=0;i<PIXELS;++i)if(actual[i]!=expected[i]){
                fprintf(stderr,"access cached/full mismatch frame=%u at %zu,%zu\n",frame,i%W,i/W);break;
            }
            abort();
        }
    }
    x4_picture_settings_ui_cache_invalidate();free(base);free(actual);free(expected);
    puts("PASS access cached/full equality: enter, selection, confirmation, leave to Server/Picture");
}
#endif
int main(int argc,char **argv)
{
    assert(argc>1);assert(!x4_font_init("assets/fonts/inter.x4font"));
    char path[256];assert(snprintf(path,sizeof(path),"/tmp/x4-access-capture-%ld.bin",(long)getpid())<(int)sizeof(path));
    (void)remove(path);assert(x4_access_country_set_path(path));x4_access_country_init();
    uint32_t *owned=malloc((PIXELS+2u)*sizeof(*owned));assert(owned);
    owned[0]=owned[PIXELS+1u]=0xcafebabeu;uint32_t *p=owned+1;
    X4ServerRegion region={0};
    snprintf(region.name,sizeof(region.name),"SouthCentralUS");
    snprintf(region.api_host,sizeof(region.api_host),"southcentralus.gssv-play-prod.xboxlive.com");
    snprintf(region.host,sizeof(region.host),"%s",region.api_host);region.is_default=true;region.last_game_ms=52;
    char server_path[256];assert(snprintf(server_path,sizeof(server_path),"/tmp/x4-access-server-%ld.bin",(long)getpid())<(int)sizeof(server_path));
    x4_server_settings_set_path(server_path);(void)remove(server_path);
    assert(x4_server_settings_update_regions(&region,1));
#ifndef X4_T22_BASELINE_CAPTURE
    cached_access();
#endif
    for(unsigned lang=0;lang<2;++lang){
        const char *language=lang?"en":"es";
        x4_lang_set((enum X4Lang)lang,lang?"en-US":"es-MX",lang?"US":"MX");
        settings_capture(argv[1],language,p);
#ifndef X4_T22_BASELINE_CAPTURE
        X4CatalogSnapshot *cat=calloc(1,sizeof(*cat));assert(cat);
        uint32_t *reference=malloc(PIXELS*sizeof(*reference));assert(reference);
        X4CatalogBrowser browser;X4RecentGames recent;X4CatalogGuiStatus status={.connected=1};
        x4_catalog_browser_init(&browser);x4_recent_init(&recent);
        cat->state=X4_CATALOG_ERROR;cat->error=X4_AUTH_E_XBOX;cat->http_status=403;
        snprintf(cat->offering,sizeof(cat->offering),"xgpuwebf2p");
        snprintf(cat->stage,sizeof(cat->stage),"Xbox Cloud Gaming rechazo la cuenta");
        const char *names[]={"card-auto","card-country","card-confirm","card-trying","card-failed","card-help"};
        X4Screen screen={.page=8};
        for(unsigned state=0;state<6;++state){
            X4AccessCountryUi ui={.selected=state?X4_ACCESS_US:X4_ACCESS_AUTO};
            ui.confirming=state==2;ui.help=state==5;
            X4AccessCountryCardPhase phase=state==3?X4_ACCESS_CARD_TRYING:X4_ACCESS_CARD_SELECT;
            cat->state=state==3?X4_CATALOG_LOADING:X4_CATALOG_ERROR;
            x4_access_country_card_set(&ui,phase);
            x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,p);save(argv[1],names[state],language,p);
            x4_apple_bg_draw(reference);
            rectangle_equal(p,reference,84,946,400,42); /* No selection/search shortcuts; status at right remains. */
            x4_fortnite_summary_draw(&screen,cat,state==3,p);
            char name[80];snprintf(name,sizeof(name),"fortnite-%s",names[state]);save(argv[1],name,language,p);
        }
        cat->state=X4_CATALOG_ERROR;
        X4AccessCountryUi failed={.selected=X4_ACCESS_US};
        x4_access_country_card_set(&failed,X4_ACCESS_CARD_SELECT);
        x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,reference);
        status.notice=lang?"The query could not start (0x00001234).":"No se pudo iniciar la consulta (0x00001234).";
        x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,p);
        only_toast_changed(p,reference,84,946,1752,42); /* Coordinator failure remains visible. */
        save(argv[1],"card-query-dispatch-failed",language,p);
        status.notice=NULL;
        cat->state=X4_CATALOG_READY;
        X4AccessCountryUi applied={.selected=X4_ACCESS_US};
        assert(x4_access_country_apply(X4_ACCESS_US,true,"tarjeta"));
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_SELECT);
        x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,reference);
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_SUCCESS);
        x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,p);save(argv[1],"card-success",language,p);
        only_toast_changed(p,reference,550,988,820,52);
        rectangle_equal(p,reference,84,946,1752,42); /* Existing footer stays intact. */
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_SELECT);
        x4_fortnite_summary_draw(&screen,cat,0,reference);
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_SUCCESS);
        x4_fortnite_summary_draw(&screen,cat,0,p);save(argv[1],"fortnite-country-success",language,p);
        only_toast_changed(p,reference,84,952,550,52);
        rectangle_equal(p,reference,704,264,1132,44); /* Quality subtitle stays intact. */
        rectangle_equal(p,reference,704,1004,1132,44); /* Right-column controls stay intact. */
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_SELECT);
        X4SessionSnapshot session={.access_country=X4_ACCESS_US};
        x4_apple_bg_draw(p);x4_live_overlay(NULL,&session,0,NULL,true,p);save(argv[1],"video-banner-country",language,p);
        /* XSTS/network errors retain the ordinary card and no country selector. */
        cat->state=X4_CATALOG_ERROR;cat->xerr=2148916235u;
        x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,p);save(argv[1],"card-account-country-xsts",language,p);
        memcpy(reference,p,PIXELS*sizeof(*reference));
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_TRYING);
        x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,p);
        assert(!memcmp(p,reference,PIXELS*sizeof(*p))); /* Phase alone cannot hide XSTS footer. */
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_SELECT);
        cat->xerr=0;cat->http_status=0;cat->error=-1005;
        snprintf(cat->stage,sizeof(cat->stage),"red");
        x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,p);save(argv[1],"card-network-error",language,p);
        memcpy(reference,p,PIXELS*sizeof(*reference));
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_TRYING);
        x4_catalog_gui_draw(cat,&browser,&recent,NULL,&status,p);
        assert(!memcmp(p,reference,PIXELS*sizeof(*p))); /* Network footer is also unchanged. */
        x4_access_country_card_set(&applied,X4_ACCESS_CARD_SELECT);
        free(reference);
        free(cat);
        puts("PASS access geometry: toast-only dirty rectangles; protected subtitles/footers identical; contextual shortcuts only");
#endif
        assert(owned[0]==0xcafebabeu&&owned[PIXELS+1u]==0xcafebabeu);
    }
    x4_picture_settings_ui_cache_invalidate();x4_catalog_gui_keyboard_cache_invalidate();
    x4_font_close();free(owned);remove(path);remove(server_path);
    printf("PASS access captures public=%d: real ES/EN renderers, framebuffer canaries\n",X4_PUBLIC);
    return 0;
}
