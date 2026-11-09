/* SPDX-License-Identifier: GPL-3.0-only */
/* Real Lab settings, persistence and renderer. No GPU or console execution. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "../src/ui/stream_settings.h"
#include "../src/ui/font.h"
#include "../src/ui/lang.h"
#if !X4_PUBLIC
#include "../src/video/gpu_quality_ref.h"
#endif
#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert(sizeof(X4StreamSettingsUi)==24&&offsetof(X4StreamSettingsUi,row)==4,
    "The new Lab choice uses existing draft padding; public ABI is unchanged");

#if X4_PUBLIC
#include "../src/ui/picture_settings.h"
X4PictureSettingsConfig x4_picture_settings_config(void)
{return (X4PictureSettingsConfig){X4_PIC_QUALITY_1080P_ALTA,1u,X4_VSYNC_AUTO,true};}
X4StreamRequestProfile x4_picture_settings_to_profile(X4PictureQualityOption quality)
{(void)quality;return X4_REQUEST_1080P_G;}
#endif

#if !X4_PUBLIC
static void capture(const char *dir,const char *name,const uint32_t *pixels)
{
    if(!dir)return;
    char path[1024];int n=snprintf(path,sizeof(path),"%s/%s.ppm",dir,name);
    assert(n>0&&(size_t)n<sizeof(path));FILE *file=fopen(path,"wb");assert(file);
    assert(fprintf(file,"P6\n1920 1080\n255\n")>0);
    for(unsigned i=0;i<1920u*1080u;++i){
        unsigned char rgb[3]={(unsigned char)pixels[i],(unsigned char)(pixels[i]>>8),(unsigned char)(pixels[i]>>16)};
        assert(fwrite(rgb,1,3,file)==3);
    }
    assert(!fclose(file));
}

#endif

#if !X4_PUBLIC
static unsigned char checksum(const unsigned char *record)
{
    unsigned char value=0xa7u;
    for(unsigned i=0;i<7u;++i)value=(unsigned char)((value*33u)^record[i]);
    return value;
}
static void bytes_write(const char *path,const void *data,size_t bytes)
{
    FILE *file=fopen(path,"wb");assert(file);assert(fwrite(data,1,bytes,file)==bytes);assert(!fclose(file));
}
static void record_write(const char *path,unsigned version,unsigned backend,unsigned reserved,bool tail)
{
    unsigned char record[9]={'X','4','G','C',(unsigned char)version,(unsigned char)backend,(unsigned char)reserved,0,0xff};
    record[7]=checksum(record);bytes_write(path,record,tail?9u:8u);
}
static void saved_bytes(const char *path,unsigned char out[8])
{
    FILE *file=fopen(path,"rb");assert(file);assert(fread(out,1,8,file)==8);assert(fgetc(file)==EOF);assert(!fclose(file));
}
static void unchanged_config(const X4StreamRequestConfig *a,const X4StreamRequestConfig *b)
{
    assert(a->profile==b->profile&&a->color_preference==b->color_preference&&a->cpu_variant==b->cpu_variant&&
        a->decoder_depth==b->decoder_depth&&a->interaction_modes==b->interaction_modes&&a->convert_mode==b->convert_mode&&
        a->sharpness==b->sharpness&&a->resolution_request_mode==b->resolution_request_mode);
}
static void persistence(const char *path)
{
    (void)unlink(path);
    assert(x4_stream_settings_conversion_init()==0&&x4_stream_settings_conversion_backend()==0);
    assert(x4_gpu_quality_settings_get()==0u);
    assert(!x4_gpu_quality_settings_set(6u)&&!x4_gpu_quality_settings_set(32u)&&!x4_gpu_quality_settings_set(~0u));
    assert(!x4_stream_settings_conversion_set(2)&&!x4_stream_settings_conversion_set(~0u));
    assert(access(path,F_OK)!=0);
    assert(x4_stream_settings_conversion_set(1));
    unsigned char original[8],after[8];saved_bytes(path,original);
    assert(!memcmp(original,"X4GC",4)&&original[4]==1&&original[5]==1&&original[7]==checksum(original));
    for(unsigned word=0;word<=31u;++word){
        unsigned previous=x4_gpu_quality_settings_get();
        if(((word>>1)&3u)==3u){assert(!x4_gpu_quality_settings_set(word));assert(x4_gpu_quality_settings_get()==previous);}
        else {assert(x4_gpu_quality_settings_set(word));assert(x4_gpu_quality_settings_get()==word);}
        saved_bytes(path,after);assert(!memcmp(original,after,8));
    }
    assert(x4_stream_settings_conversion_init()==0&&x4_stream_settings_conversion_backend()==1);
    assert(x4_gpu_quality_settings_get()==0u); /* App initialization never restores quality choices. */
    unsigned captured=x4_stream_settings_conversion_backend();
    assert(x4_stream_settings_conversion_set(0));
    assert(captured==1&&x4_stream_settings_conversion_backend()==0);
    assert(x4_stream_settings_conversion_init()==0&&x4_stream_settings_conversion_backend()==0);
    for(unsigned backend=2;backend<=255;backend+=253){
        record_write(path,1,backend,0,false);
        assert(x4_stream_settings_conversion_init()==-1&&x4_stream_settings_conversion_backend()==0);
    }
    record_write(path,0,1,0,false);assert(x4_stream_settings_conversion_init()==-1&&x4_stream_settings_conversion_backend()==0);
    record_write(path,2,1,0,false);assert(x4_stream_settings_conversion_init()==-1&&x4_stream_settings_conversion_backend()==0);
    record_write(path,1,1,1,false);assert(x4_stream_settings_conversion_init()==-1&&x4_stream_settings_conversion_backend()==0);
    record_write(path,1,1,0,true);assert(x4_stream_settings_conversion_init()==-1&&x4_stream_settings_conversion_backend()==0);
    original[7]^=1;bytes_write(path,original,sizeof(original));
    assert(x4_stream_settings_conversion_init()==-1&&x4_stream_settings_conversion_backend()==0);
    bytes_write(path,"X4GC",4);assert(x4_stream_settings_conversion_init()==-1&&x4_stream_settings_conversion_backend()==0);
    const unsigned char old_record[16]={'X','4','S','1',1,0,0,0,2,0,0,0,0,0,0,0};
    bytes_write(path,old_record,sizeof(old_record));
    assert(x4_stream_settings_conversion_init()==-1&&x4_stream_settings_conversion_backend()==0);
    unsigned char old_after[16];FILE *file=fopen(path,"rb");assert(file);
    assert(fread(old_after,1,sizeof(old_after),file)==sizeof(old_after));assert(!fclose(file));
    assert(!memcmp(old_record,old_after,sizeof(old_record))); /* Loading never rewrites an old/unknown record. */
    (void)after;
}
static void model(const char *path)
{
    (void)unlink(path);assert(!x4_stream_settings_conversion_init());
    X4StreamSettingsUi ui={0};
    X4StreamRequestConfig original=x4_stream_settings_get();
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN|X4_SETTINGS_CONFIRM,true));
    assert(ui.open&&!(ui.convert_sharp&32u));
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_UP,true)&&ui.row==16);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==0);
    ui.row=11;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT|X4_SETTINGS_CONFIRM,true));
    assert(ui.open&&(ui.convert_sharp&32u)&&x4_stream_settings_conversion_backend()==0&&access(path,F_OK)!=0);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL|X4_SETTINGS_CONFIRM,true));
    assert(!ui.open&&x4_stream_settings_conversion_backend()==0&&access(path,F_OK)!=0);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));ui.row=11;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    assert(!ui.open&&ui.applied&&x4_stream_settings_conversion_backend()==1);
    X4StreamRequestConfig now=x4_stream_settings_get();unchanged_config(&original,&now);
    unsigned captured=x4_stream_settings_conversion_backend();
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));ui.row=11;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,false));
    assert(!ui.open&&captured==1&&x4_stream_settings_conversion_backend()==1);
    unsigned char before[8],after[8];saved_bytes(path,before);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));ui.row=11;
    ui.profile=(X4StreamRequestProfile)-1;ui.convert_sharp^=32u;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    assert(ui.open&&x4_stream_settings_conversion_backend()==1);saved_bytes(path,after);assert(!memcmp(before,after,8));
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL,true));
    char temp[1024];int n=snprintf(temp,sizeof(temp),"%s.tmp",path);assert(n>0&&(size_t)n<sizeof(temp));
    assert(!mkdir(temp,0700));
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));ui.row=11;
    ui.profile=original.profile==X4_REQUEST_720P60?X4_REQUEST_1080P_G:X4_REQUEST_720P60;
    ui.convert_sharp^=32u;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    assert(ui.open&&!ui.applied&&(ui.convert_sharp&64u)&&x4_stream_settings_conversion_backend()==1);
    now=x4_stream_settings_get();unchanged_config(&original,&now);
    saved_bytes(path,after);assert(!memcmp(before,after,8));
    assert(!rmdir(temp));
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL,true));
    assert(x4_stream_settings_conversion_set(0));
    assert(captured==1&&x4_stream_settings_conversion_backend()==0);
    assert(!x4_stream_settings_conversion_init()&&x4_stream_settings_conversion_backend()==0);
}
static void independence(void)
{
    assert(x4_stream_settings_conversion_set(1));
    for(unsigned p=0;p<X4_REQUEST_PROFILE_COUNT;++p)for(unsigned color=0;color<2;++color)
        for(unsigned cpu=0;cpu<4;++cpu)for(unsigned depth=0;depth<3;++depth)
            for(unsigned modes=0;modes<=X4_INTERACTION_MASK;++modes){
                assert(x4_stream_settings_set_lab((X4StreamRequestProfile)p,(X4StreamColorPreference)color,
                    (X4StreamCpuVariant)cpu,(X4StreamDecoderDepth)depth,modes,1,2,0));
                assert(x4_stream_settings_conversion_backend()==1);
                X4StreamRequestConfig c=x4_stream_settings_get();
                assert((unsigned)c.profile==p&&(unsigned)c.color_preference==color&&c.cpu_variant==cpu&&
                    c.decoder_depth==depth&&c.interaction_modes==modes&&c.convert_mode==1&&c.sharpness==2&&
                    c.resolution_request_mode==0);
            }
    assert(x4_stream_settings_conversion_set(0));
}
static void quality_model(const char *path)
{
    (void)unlink(path);assert(!x4_stream_settings_conversion_init());
    X4StreamSettingsUi ui={0};
    assert(x4_gpu_quality_settings_get()==0u);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    for(unsigned row=1;row<17u;++row)
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==row);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_DOWN,true)&&ui.row==0u);
    ui.row=12;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT|X4_SETTINGS_CONFIRM,true));
    ui.row=13;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
    ui.row=14;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
    ui.row=15;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
    assert(ui.open&&x4_gpu_quality_settings_get()==0u&&access(path,F_OK)!=0);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL|X4_SETTINGS_CONFIRM,true));
    assert(!ui.open&&x4_gpu_quality_settings_get()==0u);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    for(unsigned row=12;row<16u;++row){ui.row=row;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));}
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    assert(!ui.open&&ui.applied&&x4_gpu_quality_settings_get()==27u&&access(path,F_OK)!=0);
    const unsigned captured=x4_gpu_quality_settings_get();
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    ui.row=13;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true)); /* Strong. */
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    assert(captured==27u&&x4_gpu_quality_settings_get()==29u);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    ui.row=13;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true)); /* Strong -> Off. */
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    assert(x4_gpu_quality_settings_get()==25u);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    ui.row=13;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true)); /* Off -> Strong. */
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,false));
    assert(!ui.open&&x4_gpu_quality_settings_get()==25u);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
    ui.row=12;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    ui.profile=(X4StreamRequestProfile)-1;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true));
    assert(ui.open&&x4_gpu_quality_settings_get()==25u&&x4_stream_settings_conversion_backend()==0u);
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL,true));
    assert(!x4_stream_settings_conversion_init()&&x4_gpu_quality_settings_get()==0u);
}
#endif

#if !X4_PUBLIC
/* Measure actual glyph ink, not just nominal line-box coordinates. This
 * fails the former nearly touching title/note in either language. */
static void heading_spacing(const uint32_t *pixels)
{
    const uint32_t background=0xff211910u;
    int title_last=-1,note_first=175;
    for(int y=80;y<175;++y){
        bool ink=false;
        for(int x=310;x<1600;++x)
            if(pixels[(size_t)y*1920u+(unsigned)x]!=background){ink=true;break;}
        if(!ink)continue;
        if(y<145)title_last=y;
        else if(note_first==175)note_first=y;
    }
    assert(title_last>=90&&title_last<145&&note_first>=145&&note_first<175);
    assert(note_first-title_last-1>=12); /* At least twelve blank raster rows. */
}
static void quality_text_widths(void)
{
    const enum X4TextId labels[]={X4_TR_LAB_DITHER,X4_TR_LAB_DEBAND,X4_TR_LAB_SHARPNESS_TYPE,X4_TR_LAB_GPU_PAUSE};
    const enum X4TextId values[]={X4_TR_LAB_NO,X4_TR_LAB_YES,X4_TR_LAB_DEBAND_SOFT,
        X4_TR_LAB_DEBAND_STRONG,X4_TR_LAB_SHARPNESS_CLASSIC,X4_TR_LAB_SHARPNESS_CAS,
        X4_TR_LAB_CAS_LOW,X4_TR_LAB_CAS_MEDIUM,X4_TR_LAB_GPU_PAUSE_CPU,X4_TR_LAB_GPU_PAUSE_REPEAT};
    for(unsigned i=0;i<sizeof(labels)/sizeof(*labels);++i)
        assert(*x4_tr(labels[i])&&x4_text_width_px(25,1,x4_tr(labels[i]))<=1240);
    for(unsigned i=0;i<sizeof(values)/sizeof(*values);++i)
        assert(*x4_tr(values[i])&&x4_text_width_px(23,0,x4_tr(values[i]))<=1220);
    assert(x4_text_width_px(20,0,x4_tr(X4_TR_LAB_QUALITY_SCOPE))<=1290);
    char range[64];snprintf(range,sizeof(range),x4_tr(X4_TR_LAB_ROWS_FMT),7u,16u,16u);
    assert(x4_text_width_px(20,0,range)<=220);
}
static void viewport_spacing(const uint32_t *pixels)
{
    for(unsigned y=777;y<790;++y)for(unsigned x=310;x<1600;++x)
        assert(pixels[(size_t)y*1920u+x]==0xff211910u);
}
#endif

static void render(const char *dir)
{
    size_t count=1920u*1080u;uint32_t *owned=malloc((count+32u)*sizeof(*owned));assert(owned);
#if !X4_PUBLIC
    uint32_t *pixels=owned+16;
#endif
    for(size_t i=0;i<count+32u;++i)owned[i]=0x1234abcdu;
    for(unsigned english=0;english<2;++english){
        x4_lang_set(english?X4_LANG_EN:X4_LANG_ES,english?"en-US":"es-MX",english?"US":"MX");
#if !X4_PUBLIC
        quality_text_widths();
        assert(x4_gpu_quality_settings_set(0u));
        for(unsigned backend=0;backend<2;++backend){
            X4StreamSettingsUi ui={.open=true,.profile=X4_REQUEST_1080P_G8,.cpu=X4_CPU_0,
                .depth=X4_DEPTH_AUTO,.row=11,.color=X4_COLOR_AUTO_709,.convert_sharp=(unsigned char)(5u|(backend<<5))};
            memset(pixels,0,count*sizeof(*pixels));x4_stream_settings_ui_draw(&ui,pixels);
            heading_spacing(pixels);
            viewport_spacing(pixels);
            assert(x4_text_width_px(25,1,x4_tr(X4_TR_LAB_CONVERSION))<=1240);
            assert(x4_text_width_px(22,0,x4_tr(X4_TR_LAB_NOTE))<=1290);
            assert(x4_text_width_px(23,0,x4_tr(backend?X4_TR_LAB_CONVERSION_GPU:X4_TR_LAB_CONVERSION_CPU))<=1220);
            assert(!strcmp(x4_tr(X4_TR_LAB_CONVERSION),english?"Conversion: CPU / GPU":"Conversión: CPU / GPU"));
            char name[64];snprintf(name,sizeof(name),"lab-conversion-%s-%s",backend?"gpu":"cpu",english?"en":"es");
            capture(dir,name,pixels);
            ui.convert_sharp|=64u;memset(pixels,0,count*sizeof(*pixels));x4_stream_settings_ui_draw(&ui,pixels);
            heading_spacing(pixels);
            assert(x4_text_width_px(18,0,x4_tr(X4_TR_LAB_SAVE_ERROR))<=1290);
            snprintf(name,sizeof(name),"lab-conversion-%s-save-error-%s",backend?"gpu":"cpu",english?"en":"es");
            capture(dir,name,pixels);
        }
        for(unsigned variant=0;variant<3u;++variant){
            assert(x4_gpu_quality_settings_set(variant==2u?27u:0u));
            X4StreamSettingsUi ui={.open=true,.profile=X4_REQUEST_1080P_HQ,.cpu=X4_CPU_0,
                .depth=X4_DEPTH_AUTO,.row=variant?15u:0u,.color=X4_COLOR_AUTO_709,.convert_sharp=37u};
            memset(pixels,0,count*sizeof(*pixels));x4_stream_settings_ui_draw(&ui,pixels);
            heading_spacing(pixels);viewport_spacing(pixels);
            char name[64];snprintf(name,sizeof(name),"lab-quality-%s-%s",variant==0u?"top":variant==1u?"off":"cas-soft",english?"en":"es");
            capture(dir,name,pixels);
        }
        assert(x4_gpu_quality_settings_set(0u));
#else
        /* Public has no technical R3 renderer/handlers or Lab GPU settings. */
        (void)dir;
        X4StreamRequestConfig request=x4_stream_settings_get();
        assert(request.profile==X4_REQUEST_1080P_G);
        assert(x4_stream_request_vsync_policy(&request)==X4_VSYNC_AUTO);
        assert(x4_stream_request_dither_enabled(&request)&&!x4_stream_request_gpu_force_cpu(&request));
        assert(!x4_stream_request_mailbox(&request));
#endif
        for(unsigned i=0;i<16;++i)assert(owned[i]==0x1234abcdu&&owned[count+16+i]==0x1234abcdu);
    }
    free(owned);
}
int main(int argc,char **argv)
{
    assert(argc==3||argc==4);assert(!x4_font_init(argv[1]));
#if !X4_PUBLIC
    persistence(argv[2]);model(argv[2]);independence();quality_model(argv[2]);
#endif
    render(argc==4?argv[3]:NULL);x4_font_close();
    puts("PASS conversion settings: real persistence/model/render, captured selection, independent settings, public exclusion; host only");
    return 0;
}
