/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/picture_settings.h"
#include "../src/ui/server_settings.h"
#include "../src/ui/apple_ui.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"

typedef struct { int x, y, w, h; char text[256]; } TextBox;
static TextBox boxes[256];
static unsigned box_count;
static bool recording;

/* Intercept the real renderer's text calls, not copied layout constants.
 * The conservative line box includes six pixels below nominal text size. */
static void record(int x, int y, int size, int weight, const char *text)
{
    if (!recording || !text || !text[0]) return;
    assert(box_count < sizeof(boxes)/sizeof(boxes[0]));
    TextBox *b = &boxes[box_count++];
    *b = (TextBox){.x=x,.y=y,.w=x4_text_width_px(size,weight,text),.h=size+6};
    snprintf(b->text,sizeof(b->text),"%s",text);
    if (b->x < 100 || b->y < 126 || b->x+b->w > 1820 || b->y+b->h > 1048) {
        fprintf(stderr,"text outside safe area: (%d,%d %d,%d) %s\n",b->x,b->y,b->w,b->h,text);
        abort();
    }
    for (unsigned i=0;i+1<box_count;++i) {
        TextBox *a=&boxes[i];
        if (a->x < b->x+b->w && b->x < a->x+a->w &&
            a->y < b->y+b->h && b->y < a->y+a->h) {
            fprintf(stderr,"text overlap: %s (%d,%d,%d,%d) / %s (%d,%d,%d,%d)\n",
                    a->text,a->x,a->y,a->w,a->h,b->text,b->x,b->y,b->w,b->h);
            abort();
        }
    }
}
void __real_x4_text_px(uint32_t *,int,int,int,int,const char *,uint32_t);
void __real_x4_text_ellipsis_px(uint32_t *,int,int,int,int,const char *,int,uint32_t);
void __wrap_x4_text_px(uint32_t *p,int x,int y,int size,int weight,const char *s,uint32_t c)
{
    record(x,y,size,weight,s);
    __real_x4_text_px(p,x,y,size,weight,s,c);
}
void __wrap_x4_text_ellipsis_px(uint32_t *p,int x,int y,int size,int weight,const char *s,int width,uint32_t c)
{
    char fitted[2048];
    x4_text_fit_utf8(fitted,sizeof(fitted),size,weight,s,width);
    record(x,y,size,weight,fitted);
    __real_x4_text_ellipsis_px(p,x,y,size,weight,s,width,c);
}

static void render_checked(X4PictureSettingsUi *ui,uint32_t *pixels)
{
    memset(pixels,0x70,1920u*1080u*sizeof(*pixels));
    box_count=0;
    recording=true;
    x4_picture_settings_ui_draw(ui,pixels);
    recording=false;
    assert(box_count > 8);
    assert(x4_server_settings_is_ui_drawn());
}

static void save_ppm(const char *dir,const char *name,uint32_t *pixels)
{
    if (!dir) return;
    char path[1024];
    assert(snprintf(path,sizeof(path),"%s/%s.ppm",dir,name) < (int)sizeof(path));
    FILE *f=fopen(path,"wb");
    assert(f);
    fprintf(f,"P6\n1920 1080\n255\n");
    for (unsigned i=0;i<1920u*1080u;++i) {
        unsigned char rgb[3]={(unsigned char)pixels[i],(unsigned char)(pixels[i]>>8),(unsigned char)(pixels[i]>>16)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}

static void fill_regions(void)
{
    X4ServerRegion regions[12]={0};
    for (unsigned i=0;i<12;++i) {
        snprintf(regions[i].name,sizeof(regions[i].name),
                 "Region_with_a_very_long_display_name_for_layout_%02u",i);
        snprintf(regions[i].api_host,sizeof(regions[i].api_host),
                 "uksouth.gssv-play-prod.xboxlive.com");
        snprintf(regions[i].host,sizeof(regions[i].host),"%s",regions[i].api_host);
        regions[i].is_default=i==0;
        regions[i].last_game_ms=i<4 ? 73+(int)i*60 : 0;
    }
    assert(x4_server_settings_update_regions(regions,12));
}

static void navigation(void)
{
    X4PictureSettingsUi ui={0};
    assert(!x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,false));
    assert(!ui.open);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    assert(ui.open && ui.tab==0 && ui.selected==0);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true));
    assert(ui.selected==1);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true));
    assert(ui.selected==2);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_NEXT_TAB,true));
    assert(ui.tab==1);
    for (unsigned i=0;i<8;++i)
        assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true));
    assert(ui.server_selected==8);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_PREV_TAB,true));
    assert(ui.tab==0 && ui.selected==2);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CANCEL,true));
    assert(!ui.open && x4_picture_settings_get()==X4_PIC_QUALITY_1080P_ALTA);
    x4_picture_settings_open_server_first_time(&ui);
    assert(ui.first_time_mode && ui.tab==1);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_PREV_TAB,false));
    assert(ui.tab==1 && ui.open);
    assert(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CANCEL,false));
    assert(!ui.open && !ui.first_time_mode);
    puts("PASS T20 settings: single-step/tab navigation, cancel and modal guard preserved");
}

int main(int argc,char **argv)
{
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    x4_picture_settings_set_path("/tmp/x4-tarea20-picture-test.bin");
    x4_server_settings_set_path("/tmp/x4-tarea20-server-test.bin");
    remove("/tmp/x4-tarea20-picture-test.bin");
    remove("/tmp/x4-tarea20-server-test.bin");
    x4_picture_settings_init();
    x4_server_settings_init();
    fill_regions();
    navigation();
    uint32_t *owned=calloc(1920u*1080u+2,sizeof(*owned));
    assert(owned);
    owned[0]=owned[1920u*1080u+1]=0xcafebabeu;
    uint32_t *pixels=owned+1;
    for (unsigned lang=0;lang<2;++lang) {
        x4_lang_set(lang ? X4_LANG_EN : X4_LANG_ES,lang ? "en-US" : "es-MX",lang ? "US" : "MX");
        X4PictureSettingsUi ui={.open=true};
        for (unsigned quality=0;quality<4;++quality) {
            ui.selected=quality;
            render_checked(&ui,pixels);
        }
        char name[128];
        snprintf(name,sizeof(name),"settings_picture_%s",lang ? "en" : "es");
        save_ppm(argc>1 ? argv[1] : NULL,name,pixels);
        ui.tab=1;
        for (unsigned selected=0;selected<=12;++selected) {
            ui.server_selected=selected;
            render_checked(&ui,pixels);
            assert(selected>=ui.server_scroll_offset && selected<ui.server_scroll_offset+6);
        }
        snprintf(name,sizeof(name),"settings_server_scrolled_%s",lang ? "en" : "es");
        save_ppm(argc>1 ? argv[1] : NULL,name,pixels);
        ui.server_selected=0;
        render_checked(&ui,pixels);
        snprintf(name,sizeof(name),"settings_server_auto_%s",lang ? "en" : "es");
        save_ppm(argc>1 ? argv[1] : NULL,name,pixels);
        ui.first_time_mode=true;
        render_checked(&ui,pixels);
        snprintf(name,sizeof(name),"settings_first_time_%s",lang ? "en" : "es");
        save_ppm(argc>1 ? argv[1] : NULL,name,pixels);
        assert(x4_server_settings_set_selected("RemovedRegion",true));
        fill_regions();
        assert(x4_server_settings_server_not_available());
        ui.first_time_mode=false;
        ui.server_selected=12;
        render_checked(&ui,pixels);
        snprintf(name,sizeof(name),"settings_server_unavailable_%s",lang ? "en" : "es");
        save_ppm(argc>1 ? argv[1] : NULL,name,pixels);
        x4_server_settings_clear_not_available();
        assert(owned[0]==0xcafebabeu && owned[1920u*1080u+1]==0xcafebabeu);
    }
    free(owned);
    remove("/tmp/x4-tarea20-picture-test.bin");
    remove("/tmp/x4-tarea20-server-test.bin");
    x4_font_close();
    puts("PASS T20 settings: ES/EN real text calls have no overlap; long names, scroll, first-time, canaries");
    return 0;
}
