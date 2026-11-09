/* SPDX-License-Identifier: GPL-3.0-only */
/* Real R3 renderer and real script engine/scanner. Each state gets a fresh
 * process, an owned persistence fixture, and real packaged Lua source. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/ui/stream_settings.h"
#include "../src/ui/lang.h"
#include "../src/ui/font.h"
#include "../src/input/script_engine.h"
enum { W=1920,H=1080,PIXELS=W*H };

static void save_ppm(const char *path,const uint32_t *p)
{
    FILE *f=fopen(path,"wb");assert(f);
    assert(fprintf(f,"P6\n1920 1080\n255\n")>0);
    for(size_t i=0;i<PIXELS;++i){
        unsigned char rgb[3]={(unsigned char)p[i],(unsigned char)(p[i]>>8),(unsigned char)(p[i]>>16)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}

int main(int argc,char **argv)
{
    assert(argc==4);
    const char *state=argv[2],*language=argv[3];
    assert(!strcmp(state,"none")||!strcmp(state,"selected")||!strcmp(state,"warning"));
    assert(!strcmp(language,"es")||!strcmp(language,"en"));
    x4_lang_set(!strcmp(language,"es")?X4_LANG_ES:X4_LANG_EN,
        !strcmp(language,"es")?"es-MX":"en-US",!strcmp(language,"es")?"MX":"US");
    assert(!x4_font_init("assets/fonts/inter.x4font"));
    x4_script_engine_init();
    X4ScriptEntry entries[X4_MAX_SCRIPTS];
    int count=x4_script_engine_scan(entries,X4_MAX_SCRIPTS);
    assert(count==4); /* Exactly the unchanged four packaged Lua sources. */
    const char *chosen=NULL;
    for(int i=0;i<count;++i)if(!strcmp(entries[i].name,"giroscopio.lua"))chosen=entries[i].path;
    assert(chosen);
    const bool selected=!strcmp(state,"selected");
    assert(x4_script_engine_save_persistence(selected,selected?chosen:""));
    if(selected){
        assert(x4_script_engine_load(chosen));
        assert(x4_script_engine_is_active());
        assert(!strcmp(x4_script_engine_get_name(),"giroscopio.lua"));
    }
    X4StreamSettingsUi ui={0};
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    ui.row=17;
    if(!strcmp(state,"warning")){
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
        assert(ui.open&&!ui.applied&&!x4_script_engine_is_active());
        bool accepted=true;char saved[256];
        assert(x4_script_engine_load_persistence(&accepted,saved,sizeof(saved))&&!accepted);
    }
    uint32_t *guarded=malloc((PIXELS+32u)*sizeof(*guarded));assert(guarded);
    for(size_t i=0;i<PIXELS+32u;++i)guarded[i]=0x5a5a5a5a;
    uint32_t *pixels=guarded+16;
    for(size_t i=0;i<PIXELS;++i)pixels[i]=0xff140e0a;
    x4_stream_settings_ui_draw(&ui,pixels);
    for(unsigned i=0;i<16;++i)assert(guarded[i]==0x5a5a5a5a&&guarded[PIXELS+16u+i]==0x5a5a5a5a);
    char path[1024];int n=snprintf(path,sizeof(path),"%s/r3_script_%s_%s.ppm",argv[1],state,language);
    assert(n>0&&(size_t)n<sizeof(path));save_ppm(path,pixels);
    printf("capture state=%s language=%s scanned=%d selected=%s renderer=real engine=real\n",
        state,language,count,selected?"giroscopio.lua":"none");
    free(guarded);x4_script_engine_shutdown();x4_font_close();return 0;
}
