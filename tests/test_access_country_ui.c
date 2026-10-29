/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/access_country.h"
#include "../src/ui/picture_settings.h"
#include "../src/ui/server_settings.h"
#include "../src/ui/lang.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool fail_rename;
int __real_rename(const char *, const char *);
int __wrap_rename(const char *old_path,const char *new_path)
{
    return fail_rename ? -1 : __real_rename(old_path,new_path);
}
static void fresh(const char *path)
{
    (void)remove(path);
    assert(x4_access_country_set_path(path));
    x4_access_country_init();
    assert(x4_access_country_get()==X4_ACCESS_AUTO);
    assert(!x4_access_country_warning_accepted());
}
static void persistence(const char *path)
{
    fresh(path);
    assert(!x4_access_country_ip(X4_ACCESS_AUTO));
    assert(!x4_access_country_valid_ip("1.2.3.4"));
    assert(!x4_access_country_valid_ip("143.244.47.65\r\nfoo: bar"));
    assert(!x4_access_country_valid_ip(NULL));
    assert(!x4_access_country_apply(X4_ACCESS_US,false,"tarjeta"));
    assert(!x4_access_country_apply((X4AccessCountry)99,true,"tarjeta"));
    assert(!x4_access_country_apply(X4_ACCESS_US,true,"unknown"));
    for(unsigned i=1;i<X4_ACCESS_COUNTRY_COUNT;++i){
        X4AccessCountry country=(X4AccessCountry)i;
        assert(x4_access_country_valid_ip(x4_access_country_ip(country)));
        assert(x4_access_country_apply(country,true,"tarjeta"));
        x4_access_country_init();
        assert(x4_access_country_get()==country&&x4_access_country_warning_accepted());
    }
    assert(x4_access_country_apply(X4_ACCESS_AUTO,false,"ajustes"));
    x4_access_country_init();
    assert(x4_access_country_get()==X4_ACCESS_AUTO&&x4_access_country_warning_accepted());
    assert(x4_access_country_apply(X4_ACCESS_US,false,"tarjeta"));
    unsigned char before[20],after[20];
    FILE *f=fopen(path,"rb");assert(f&&fread(before,1,sizeof(before),f)==sizeof(before));assert(!fclose(f));
    fail_rename=true;
    assert(!x4_access_country_apply(X4_ACCESS_BR,false,"ajustes"));
    fail_rename=false;
    assert(x4_access_country_get()==X4_ACCESS_US);
    f=fopen(path,"rb");assert(f&&fread(after,1,sizeof(after),f)==sizeof(after));assert(!fclose(f));
    assert(!memcmp(before,after,sizeof(before)));
    /* Truncation, trailing data and unknown version all reset to Auto. */
    for(unsigned mode=0;mode<3;++mode){
        unsigned char invalid[21];memcpy(invalid,before,20);invalid[20]=0x33;
        size_t n=mode==0?7:mode==1?21:20;
        if(mode==2)invalid[4]=99;
        f=fopen(path,"wb");assert(f&&fwrite(invalid,1,n,f)==n);assert(!fclose(f));
        x4_access_country_init();
        assert(x4_access_country_get()==X4_ACCESS_AUTO&&!x4_access_country_warning_accepted());
    }
    char invalid_path[270];memset(invalid_path,'x',sizeof(invalid_path)-1);invalid_path[sizeof(invalid_path)-1]=0;
    assert(!x4_access_country_set_path(invalid_path));
    puts("PASS access persistence: fixed table, consent, replacement failure keeps old bytes, restart, invalid records");
}
static void ui_model(const char *path)
{
    fresh(path);
    X4AccessCountryUi ui;
    x4_access_country_ui_open(&ui);
    assert(x4_access_country_ui_handle(&ui,X4_ACCESS_UI_RIGHT,false,"tarjeta")==X4_ACCESS_UI_NONE);
    assert(ui.selected==X4_ACCESS_US&&x4_access_country_get()==X4_ACCESS_AUTO);
    assert(x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CANCEL,false,"tarjeta")==X4_ACCESS_UI_BACK);
    assert(x4_access_country_get()==X4_ACCESS_AUTO&&access(path,F_OK));
    x4_access_country_ui_open(&ui);ui.selected=X4_ACCESS_US;
    assert(x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CONFIRM,false,"tarjeta")==X4_ACCESS_UI_NONE&&ui.confirming);
    assert(x4_access_country_get()==X4_ACCESS_AUTO&&access(path,F_OK));
    assert(x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CANCEL,false,"tarjeta")==X4_ACCESS_UI_BACK);
    assert(!x4_access_country_warning_accepted());
    x4_access_country_ui_open(&ui);ui.selected=X4_ACCESS_US;
    (void)x4_access_country_ui_handle(&ui,X4_ACCESS_UI_HELP,false,"tarjeta");assert(ui.help);
    (void)x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CONFIRM,false,"tarjeta");assert(ui.help);
    (void)x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CANCEL,false,"tarjeta");assert(!ui.help);
    (void)x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CONFIRM,false,"tarjeta");assert(ui.confirming);
    assert(x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CONFIRM,false,"tarjeta")==X4_ACCESS_UI_APPLIED);
    assert(!ui.confirming&&x4_access_country_get()==X4_ACCESS_US&&x4_access_country_warning_accepted());
    /* A queued/running query ignores duplicate X and selection changes. */
    assert(x4_access_country_ui_handle(&ui,X4_ACCESS_UI_RIGHT|X4_ACCESS_UI_CONFIRM,true,"tarjeta")==X4_ACCESS_UI_NONE);
    assert(ui.selected==X4_ACCESS_US);
    assert(x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CANCEL,true,"tarjeta")==X4_ACCESS_UI_CANCEL_QUERY);
    x4_access_country_ui_open(&ui);ui.selected=X4_ACCESS_BR;
    assert(x4_access_country_ui_handle(&ui,X4_ACCESS_UI_CONFIRM,false,"tarjeta")==X4_ACCESS_UI_APPLIED);
    assert(!ui.confirming); /* Acceptance survives later countries. */
    x4_access_country_card_set(&ui,X4_ACCESS_CARD_TRYING);
    X4AccessCountryUi copy;X4AccessCountryCardPhase phase;
    x4_access_country_card_get(&copy,&phase);assert(copy.selected==ui.selected&&phase==X4_ACCESS_CARD_TRYING);
    puts("PASS access UI model: provisional/cancel, first consent, help, one apply edge, busy/cancel, copied render state");
}
static void options_model(const char *path)
{
    fresh(path);
    X4PictureSettingsUi s={0};
    assert(x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_OPEN,true)&&s.open&&s.tab==0);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_NEXT_TAB,true);assert(s.tab==1);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_NEXT_TAB,true);assert(s.tab==2);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_RIGHT,true);assert(s.access_ui.selected==X4_ACCESS_US);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_CONFIRM,true);assert(s.open&&s.access_ui.confirming);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_PREV_TAB,true);assert(s.tab==2);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_CANCEL,true);assert(!s.open);
    assert(x4_access_country_get()==X4_ACCESS_AUTO&&!x4_access_country_warning_accepted());
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_OPEN,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_NEXT_TAB,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_NEXT_TAB,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_RIGHT,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_CONFIRM,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_CONFIRM,true);
    assert(!s.open&&s.access_changed&&x4_access_country_get()==X4_ACCESS_US);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_OPEN,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_NEXT_TAB,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_NEXT_TAB,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_LEFT,true);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_CONFIRM,true);
    assert(x4_access_country_get()==X4_ACCESS_AUTO);
    x4_picture_settings_open_server_first_time(&s);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_NEXT_TAB|X4_PIC_SETTINGS_RIGHT,true);
    assert(s.first_time_mode&&s.tab==1&&!s.access_ui.confirming);
    (void)x4_picture_settings_ui_handle(&s,X4_PIC_SETTINGS_CANCEL,true);
    puts("PASS access OPTIONS: third section only, consent/cancel, restore Auto, first server chooser excludes Access");
}
int main(void)
{
    char dir[]="/tmp/x4-access-ui-XXXXXX";assert(mkdtemp(dir));
    char access_path[256],picture_path[256],server_path[256];
    assert(snprintf(access_path,sizeof(access_path),"%s/access.bin",dir)<(int)sizeof(access_path));
    assert(snprintf(picture_path,sizeof(picture_path),"%s/picture.bin",dir)<(int)sizeof(picture_path));
    assert(snprintf(server_path,sizeof(server_path),"%s/server.bin",dir)<(int)sizeof(server_path));
    x4_picture_settings_set_path(picture_path);x4_server_settings_set_path(server_path);
    persistence(access_path);ui_model(access_path);options_model(access_path);
    for(unsigned lang=0;lang<2;++lang){
        x4_lang_set((enum X4Lang)lang,lang?"en-US":"es-MX",lang?"US":"MX");
        for(unsigned i=0;i<X4_ACCESS_COUNTRY_COUNT;++i)assert(strlen(x4_access_country_label((X4AccessCountry)i)));
        assert(strlen(x4_tr(X4_TR_ACCESS_WARNING))&&strlen(x4_tr(X4_TR_ACCESS_DESCRIPTION)));
    }
    x4_picture_settings_ui_cache_invalidate();
    remove(access_path);remove(picture_path);remove(server_path);assert(!rmdir(dir));
    printf("PASS access UI/persistence public=%d\n",X4_PUBLIC);
    return 0;
}
