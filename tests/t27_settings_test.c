/* SPDX-License-Identifier: GPL-3.0-only */
/* Real T27 settings/migration/UI; files are isolated in the supplied host temp directory. */
#define _POSIX_C_SOURCE 200809L
#include "../src/ui/picture_settings.h"
#include "../src/ui/stream_settings.h"
#include "../src/ui/screen.h"
#include "../src/ui/apple_ui.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool rename_fail;
int __real_rename(const char *,const char *);
int __wrap_rename(const char *from,const char *to)
{ return rename_fail?-1:__real_rename(from,to); }
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"T27 settings line %d: %s\n",__LINE__,#x);abort();}}while(0)
static char legacy[256],current[256];
static unsigned char legacy_bytes[16];
static uint32_t v1_sum(uint32_t quality)
{ return 0x31533458u^(1u<<16)^(quality*0x9e3779b9u)^0xa5a55a5au; }
static void write_bytes(const char *path,const void *bytes,size_t size)
{ FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(bytes,1,size,f)==size);CHECK(!fclose(f)); }
static void read_exact(const char *path,void *bytes,size_t size)
{ FILE *f=fopen(path,"rb");CHECK(f);CHECK(fread(bytes,1,size,f)==size);CHECK(fgetc(f)==EOF);CHECK(!fclose(f)); }
static void legacy_unchanged(void)
{ unsigned char bytes[16];read_exact(legacy,bytes,sizeof(bytes));CHECK(!memcmp(bytes,legacy_bytes,sizeof(bytes))); }
static void config_equal(const X4PictureSettingsConfig *a,const X4PictureSettingsConfig *b)
{ CHECK(a->quality==b->quality&&a->conversion_backend==b->conversion_backend&&a->vsync==b->vsync&&a->dither==b->dither); }
static void migration(const char *dir)
{
    CHECK(snprintf(legacy,sizeof(legacy),"%s/settings-public-v1.bin",dir)>0);
    CHECK(snprintf(current,sizeof(current),"%s/settings-public-v2.bin",dir)>0);
    (void)remove(current);
    CHECK(x4_picture_settings_set_paths(legacy,current));
    CHECK(!x4_picture_settings_set_paths(legacy,legacy));
    uint32_t old[4]={0x31533458u,1u,2u,v1_sum(2u)};
    memcpy(legacy_bytes,old,sizeof(old));write_bytes(legacy,old,sizeof(old));
    x4_picture_settings_init();
    X4PictureSettingsConfig c=x4_picture_settings_config();
    CHECK(c.quality==X4_PIC_QUALITY_720P&&c.conversion_backend==1u&&c.vsync==X4_VSYNC_AUTO&&c.dither);
    CHECK(access(current,F_OK));legacy_unchanged(); /* Migration is read-only until explicit Save. */
    for(unsigned q=0;q<4u;++q)for(unsigned gpu=0;gpu<2u;++gpu)
        for(unsigned vsync=0;vsync<3u;++vsync)for(unsigned dither=0;dither<2u;++dither){
            X4PictureSettingsConfig desired={(X4PictureQualityOption)q,gpu,(X4StreamVsyncPolicy)vsync,dither!=0};
            CHECK(x4_picture_settings_set_config(&desired));x4_picture_settings_init();
            c=x4_picture_settings_config();config_equal(&c,&desired);legacy_unchanged();
#if X4_PUBLIC
            X4StreamRequestConfig req=x4_stream_settings_get();
            CHECK(req.profile==x4_picture_settings_to_profile(desired.quality));
            CHECK(x4_stream_request_vsync_policy(&req)==desired.vsync);
            CHECK(x4_stream_request_dither_enabled(&req)==desired.dither);
            CHECK(x4_stream_request_gpu_force_cpu(&req)==!desired.conversion_backend);
            CHECK(!x4_stream_request_mailbox(&req));
#else
            CHECK(x4_picture_settings_get()==(q==3u?X4_PIC_QUALITY_1080P_ALTA:(X4PictureQualityOption)q));
#endif
        }
    /* A valid v2 wins over v1; the Lab view never silently erases public Maximum. */
    c=(X4PictureSettingsConfig){X4_PIC_QUALITY_1080P_MAX,1u,X4_VSYNC_ALWAYS,true};
    CHECK(x4_picture_settings_set_config(&c));x4_picture_settings_init();
    CHECK(x4_picture_settings_config().quality==X4_PIC_QUALITY_1080P_MAX);legacy_unchanged();
    unsigned char good[28],after[28];read_exact(current,good,sizeof(good));
    for(unsigned byte=0;byte<sizeof(good);++byte){
        unsigned char bad[28];memcpy(bad,good,sizeof(bad));bad[byte]^=0x80;
        write_bytes(current,bad,sizeof(bad));x4_picture_settings_init();
        c=x4_picture_settings_config();CHECK(c.quality==X4_PIC_QUALITY_1080P_ALTA&&c.conversion_backend==1u&&c.vsync==X4_VSYNC_AUTO&&c.dither);
        legacy_unchanged(); /* Corrupt v2 must NOT fall back to the legacy 720p setting. */
    }
    write_bytes(current,good,sizeof(good));x4_picture_settings_init();
    c=x4_picture_settings_config();X4PictureSettingsConfig before=c;
    c.conversion_backend=0u;c.dither=false;c.vsync=X4_VSYNC_NEVER;
    rename_fail=true;CHECK(!x4_picture_settings_set_config(&c));rename_fail=false;
    c=x4_picture_settings_config();config_equal(&c,&before);read_exact(current,after,sizeof(after));CHECK(!memcmp(after,good,sizeof(good)));legacy_unchanged();
    c.vsync=(X4StreamVsyncPolicy)3;CHECK(!x4_picture_settings_set_config(&c));
    c=before;c.conversion_backend=2;CHECK(!x4_picture_settings_set_config(&c));
    c=before;c.quality=(X4PictureQualityOption)4;CHECK(!x4_picture_settings_set_config(&c));
    CHECK(!x4_picture_settings_set_config(NULL));
    puts("PASS v2: 48 combinations, strict corruption, absent-only v1 migration, legacy bytes unchanged, failed Save atomic publication");
}
static void packed_capture(void)
{
    for(unsigned modes=0;modes<=255u;++modes){
        for(unsigned conversion=0;conversion<2u;++conversion)
            for(unsigned sharp=0;sharp<3u;++sharp)for(unsigned request=0;request<3u;++request){
#if !X4_PUBLIC
                CHECK(x4_stream_settings_set_lab(X4_REQUEST_1080P_G8,X4_COLOR_FORCE_601,X4_CPU_2,X4_DEPTH_2,modes,conversion,sharp,request));
                X4StreamRequestConfig c=x4_stream_settings_get();
                CHECK(c.interaction_modes==modes&&c.convert_mode==conversion&&c.sharpness==sharp&&c.resolution_request_mode==request);
                CHECK(x4_stream_settings_set_cpu(X4_REQUEST_720P_HQ,X4_COLOR_AUTO_709,X4_CPU_1));
                X4StreamRequestConfig next=x4_stream_settings_get();
                CHECK(next.interaction_modes==modes&&next.convert_mode==conversion&&next.sharpness==sharp&&next.resolution_request_mode==request&&next.decoder_depth==X4_DEPTH_2);
#else
                X4StreamRequestConfig c=x4_stream_request_config(X4_REQUEST_1080P_G8,X4_COLOR_FORCE_601);
                c.interaction_modes=(unsigned char)modes;c.convert_mode=(unsigned char)conversion;
                c.sharpness=(unsigned char)sharp;c.resolution_request_mode=(unsigned char)request;
#endif
                X4StreamRequestConfig captured=x4_stream_request_capture(&c);
                CHECK(captured.interaction_modes==modes);
#if X4_PUBLIC
                CHECK(captured.convert_mode==X4_CONVERT_SMOOTH_DEFAULT&&captured.sharpness==X4_SHARPNESS_PUBLIC_DEFAULT&&captured.resolution_request_mode==0u);
#else
                CHECK(captured.convert_mode==conversion&&captured.sharpness==sharp&&captured.resolution_request_mode==request);
#endif
                unsigned raw=modes&X4_INTERACTION_VSYNC_MASK;
                X4StreamVsyncPolicy expected=raw==X4_INTERACTION_VSYNC_ALWAYS?X4_VSYNC_ALWAYS:raw==X4_INTERACTION_VSYNC_OFF?X4_VSYNC_NEVER:X4_VSYNC_AUTO;
                CHECK(x4_stream_request_vsync_policy(&captured)==expected);
#if X4_PUBLIC
                CHECK(!x4_stream_request_mailbox(&captured));
#else
                CHECK(x4_stream_request_mailbox(&captured)==!!(modes&X4_INTERACTION_MAILBOX));
#endif
                for(unsigned policy=0;policy<3u;++policy){
                    unsigned replaced=x4_stream_interaction_with_vsync(modes,(X4StreamVsyncPolicy)policy);
                    CHECK((replaced&~X4_INTERACTION_VSYNC_MASK)==(modes&~X4_INTERACTION_VSYNC_MASK));
                }
                c.interaction_modes^=255u;
                CHECK(captured.interaction_modes==modes); /* Immutable per-session capture. */
            }
    }
    puts("PASS all 256 interaction bytes, legacy low bits, packed filter/sharpness/request independence and by-value capture");
}
static void menu_model(void)
{
    X4PictureSettingsConfig c={X4_PIC_QUALITY_1080P_ALTA,1u,X4_VSYNC_AUTO,true};
    CHECK(x4_picture_settings_set_config(&c));X4PictureSettingsUi ui={0};
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
#if X4_PUBLIC
    CHECK(ui.selected==0u);
    for(unsigned i=0;i<3u;++i)CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    CHECK(x4_picture_settings_config().quality==X4_PIC_QUALITY_1080P_ALTA);
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true));
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true));
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_DOWN,true));
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CANCEL,true));
    X4PictureSettingsConfig after=x4_picture_settings_config();config_equal(&after,&c);
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));
    for(unsigned i=0;i<3u;++i)CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_RIGHT,true));
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CONFIRM,true));
    CHECK(!ui.open&&x4_picture_settings_config().quality==X4_PIC_QUALITY_1080P_MAX);
#else
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_UP,true));CHECK(ui.selected==2u);
    CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CANCEL,true));
    CHECK(x4_stream_settings_set_lab(X4_REQUEST_1080P_G8,X4_COLOR_AUTO_709,X4_CPU_0,X4_DEPTH_AUTO,0u,1u,2u,0u));
    for(unsigned policy=0;policy<3u;++policy)for(unsigned mailbox=0;mailbox<2u;++mailbox){
        X4StreamSettingsUi lab={0};CHECK(x4_stream_settings_ui_handle(&lab,X4_SETTINGS_OPEN,true));
        lab.row=7u;
        for(unsigned i=0;i<policy;++i)CHECK(x4_stream_settings_ui_handle(&lab,X4_SETTINGS_RIGHT,true));
        lab.row=16u;if(mailbox)CHECK(x4_stream_settings_ui_handle(&lab,X4_SETTINGS_RIGHT,true));
        CHECK(x4_stream_settings_get().interaction_modes==0u);
        CHECK(x4_stream_settings_ui_handle(&lab,X4_SETTINGS_CONFIRM,true));
        X4StreamRequestConfig captured=x4_stream_request_capture(NULL);
        CHECK(x4_stream_request_vsync_policy(&captured)==(X4StreamVsyncPolicy)policy&&x4_stream_request_mailbox(&captured)==(mailbox!=0));
        CHECK(x4_stream_settings_set_lab(X4_REQUEST_1080P_G8,X4_COLOR_AUTO_709,X4_CPU_0,X4_DEPTH_AUTO,0u,1u,2u,0u));
    }
#endif
    X4Screen screen={.page=8,.selected=0};
    for(unsigned i=0;i<10u;++i)x4_screen_update(&screen,ORBIS_PAD_BUTTON_DOWN,0);
    CHECK(screen.selected==(X4_PUBLIC?3:2));
    puts("PASS UI: Save/Cancel, four public qualities, three Lab qualities, Lab VSync and Queue/Mailbox choices");
}
static void save_capture(const char *dir,const char *name,const uint32_t *pixels)
{
    if(!dir)return;char path[1024];CHECK(snprintf(path,sizeof(path),"%s/%s.ppm",dir,name)>0);
    FILE *f=fopen(path,"wb");CHECK(f);CHECK(fprintf(f,"P6\n1920 1080\n255\n")>0);
    for(size_t i=0;i<1920u*1080u;++i){unsigned char rgb[3]={(unsigned char)pixels[i],(unsigned char)(pixels[i]>>8),(unsigned char)(pixels[i]>>16)};CHECK(fwrite(rgb,1,3,f)==3);}
    CHECK(!fclose(f));
}
static void captures(const char *dir)
{
    const size_t n=1920u*1080u;uint32_t *owned=malloc((n+32u)*sizeof(*owned));CHECK(owned);
    for(size_t i=0;i<n+32u;++i)owned[i]=0x13579bdfu;uint32_t *p=owned+16u;
    for(unsigned language=0;language<2u;++language){
        x4_lang_set(language?X4_LANG_EN:X4_LANG_ES,language?"en-US":"es-MX",language?"US":"MX");
        char name[128];
#if X4_PUBLIC
        for(unsigned policy=0;policy<3u;++policy){
            X4PictureSettingsConfig c={X4_PIC_QUALITY_1080P_MAX,1u,(X4StreamVsyncPolicy)policy,true};
            CHECK(x4_picture_settings_set_config(&c));X4PictureSettingsUi ui={0};
            CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_OPEN,true));ui.selected=2u;
            x4_apple_bg_draw(p);x4_picture_settings_ui_draw(&ui,p);
            snprintf(name,sizeof(name),"public-options-vsync-%u-%s",policy,language?"en":"es");save_capture(dir,name,p);
            CHECK(x4_picture_settings_ui_handle(&ui,X4_PIC_SETTINGS_CANCEL,true));
        }
        X4CatalogSnapshot *catalog=calloc(1,sizeof(*catalog));CHECK(catalog);catalog->state=X4_CATALOG_READY;catalog->count=1;
        snprintf(catalog->titles[0].name,sizeof(catalog->titles[0].name),"Fortnite");
        for(unsigned quality=0;quality<4u;++quality){
            X4Screen screen={.page=8,.selected=(int)quality};x4_fortnite_summary_draw(&screen,catalog,0,p);
            snprintf(name,sizeof(name),"public-fortnite-quality-%u-%s",quality,language?"en":"es");save_capture(dir,name,p);
        }
        free(catalog);
#else
        for(unsigned policy=0;policy<3u;++policy)for(unsigned mailbox=0;mailbox<2u;++mailbox){
            X4StreamSettingsUi ui={.open=true,.row=mailbox?16u:7u,.profile=X4_REQUEST_1080P_G8,
                .cpu=X4_CPU_0,.depth=X4_DEPTH_AUTO,.color=X4_COLOR_AUTO_709,.convert_sharp=5u};
            ui.interaction_modes=(unsigned char)x4_stream_interaction_with_vsync(mailbox?X4_INTERACTION_MAILBOX:0u,(X4StreamVsyncPolicy)policy);
            x4_apple_bg_draw(p);x4_stream_settings_ui_draw(&ui,p);
            snprintf(name,sizeof(name),"lab-vsync-%u-mailbox-%u-%s",policy,mailbox,language?"en":"es");save_capture(dir,name,p);
        }
#endif
        for(unsigned i=0;i<16u;++i)CHECK(owned[i]==0x13579bdfu&&owned[n+16u+i]==0x13579bdfu);
    }
    x4_picture_settings_ui_cache_invalidate();free(owned);
}
int main(int argc,char **argv)
{
    CHECK(argc==2||argc==3);CHECK(!x4_font_init("assets/fonts/inter.x4font"));
    migration(argv[1]);packed_capture();menu_model();captures(argc==3?argv[2]:NULL);
    x4_font_close();printf("PASS T27 settings (%s): %u checks; host evidence only\n",X4_PUBLIC?"Public":"Lab",checks);return 0;
}
