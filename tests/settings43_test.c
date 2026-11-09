/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/streaming/stream_profile.h"
#include "../src/ui/picture_settings.h"
#include "../src/ui/stream_settings.h"
#include "../src/ui/lang.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static void expect_remb(X4RembMode mode, unsigned fixed_mbps)
{
    const X4RembSettings remb=x4_stream_settings_remb();
    assert(remb.mode==mode&&remb.fixed_mbps==fixed_mbps);
}

static void requests(void)
{
    expect_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS);
    X4StreamRequestConfig before=x4_stream_settings_get();
    assert(x4_stream_request_recovery_mode(&before)==(X4_PUBLIC?2u:0u));
    assert(!x4_stream_request_loss_percent(&before));
    assert(x4_stream_request_nack_policy(&before)==X4_NACK_POLICY_T46B);
    assert(x4_stream_request_rr_mode(&before)==X4_RR_EDGE);
    for(unsigned policy=0;policy<=3;++policy)for(unsigned rr=0;rr<=2;++rr){
        X4StreamNackPolicy nack=(X4StreamNackPolicy)policy;
        X4StreamRrMode report=(X4StreamRrMode)rr;
#if X4_PUBLIC
        assert(x4_stream_settings_set_recovery_policy(2,0,nack,report)==
            (nack==X4_NACK_POLICY_T46B&&rr==X4_RR_EDGE));
        X4StreamRequestConfig request=before;
        request.recovery_options=(unsigned char)(15u|(policy<<4)|(rr<<6));
        request.interaction_modes=X4_INTERACTION_GPU_FORCE_CPU|X4_INTERACTION_DITHER_OFF;
        X4StreamRequestConfig fixed=x4_stream_request_capture(&request);
        assert(x4_stream_request_nack_policy(&fixed)==X4_NACK_POLICY_T46B);
        assert(x4_stream_request_rr_mode(&fixed)==X4_RR_EDGE&&!fixed.recovery_options);
        assert(!x4_stream_request_gpu_force_cpu(&fixed)&&x4_stream_request_dither_enabled(&fixed));
#else
        assert(x4_stream_settings_set_recovery_policy(0,0,nack,report));
        assert(x4_stream_settings_set_recovery(1,3));
        X4StreamRequestConfig request=x4_stream_request_capture(NULL);
        assert(x4_stream_request_recovery_mode(&request)==1&&x4_stream_request_loss_percent(&request)==3);
        assert(x4_stream_request_nack_policy(&request)==nack);
        assert(x4_stream_request_rr_mode(&request)==report);
#endif
    }
#if !X4_PUBLIC
    assert(x4_stream_settings_set_recovery_policy(0,0,X4_NACK_POLICY_T46B,X4_RR_EDGE));
#endif
    assert(!x4_stream_settings_set_recovery(3,0)&&!x4_stream_settings_set_recovery(0,2));
    const unsigned losses[]={0,1,3};
    for(unsigned mode=0;mode<3;++mode)for(unsigned l=0;l<3;++l){
#if X4_PUBLIC
        assert(x4_stream_settings_set_recovery(mode,losses[l])==(mode==2&&l==0));
        X4StreamRequestConfig fake=before;fake.recovery_options=(unsigned char)(mode|(l<<2));
        assert(x4_stream_request_recovery_mode(&fake)==2&&!x4_stream_request_loss_percent(&fake));
        assert(!x4_stream_request_capture(&fake).recovery_options);
#else
        assert(x4_stream_settings_set_recovery(mode,losses[l]));
        X4StreamRequestConfig current=x4_stream_settings_get(),snapshot=x4_stream_request_capture(&current);
        assert(x4_stream_request_recovery_mode(&current)==mode&&x4_stream_request_loss_percent(&current)==losses[l]);
        current.recovery_options=0;
        assert(x4_stream_request_recovery_mode(&snapshot)==mode&&x4_stream_request_loss_percent(&snapshot)==losses[l]);
        assert(x4_stream_settings_set(before.profile,before.color_preference));
        assert(x4_stream_settings_set_cpu(before.profile,before.color_preference,X4_CPU_0));
        assert(x4_stream_settings_set_all(before.profile,before.color_preference,X4_CPU_0,X4_DEPTH_AUTO));
        assert(x4_stream_settings_set_lab(before.profile,before.color_preference,X4_CPU_0,X4_DEPTH_AUTO,
            before.interaction_modes,before.convert_mode,before.sharpness,before.resolution_request_mode));
        X4StreamRequestConfig preserved=x4_stream_settings_get();
        assert(preserved.recovery_options==snapshot.recovery_options);
        assert(preserved.interaction_modes==before.interaction_modes&&preserved.convert_mode==before.convert_mode&&
            preserved.sharpness==before.sharpness&&preserved.resolution_request_mode==before.resolution_request_mode);
#endif
    }
    assert(x4_stream_settings_set_recovery(X4_PUBLIC?2u:0u,0));
    X4StreamRequestConfig injected=before;injected.interaction_modes=X4_INTERACTION_VSYNC_SYNCED;
    X4StreamRequestConfig captured=x4_stream_request_capture(&injected);
    assert(x4_stream_request_vsync_policy(&injected)==(X4_PUBLIC?X4_VSYNC_AUTO:X4_VSYNC_SYNCED));
    assert(x4_stream_request_vsync_policy(&captured)==(X4_PUBLIC?X4_VSYNC_AUTO:X4_VSYNC_SYNCED));
}
#if !X4_PUBLIC
static atomic_bool updates_done;
static const unsigned concurrent_remb_rates[]={10u,30u,X4_REMB_DEFAULT_FIXED_MBPS};
static void *update_recovery(void *ignored)
{
    (void)ignored;const unsigned losses[]={0,1,3};
    for(unsigned i=0;i<200000u;++i){
        unsigned option=i%3u;
        assert(x4_stream_settings_set_recovery(option,losses[option]));
        assert(x4_stream_settings_set_remb((X4RembMode)option,concurrent_remb_rates[option]));
    }
    atomic_store_explicit(&updates_done,true,memory_order_release);return NULL;
}
static void concurrent_settings(void)
{
    X4StreamRequestConfig before=x4_stream_settings_get();
    atomic_init(&updates_done,false);pthread_t producer;
    assert(!pthread_create(&producer,NULL,update_recovery,NULL));
    unsigned iteration=0;
    do {
        assert(x4_stream_settings_set_cpu(before.profile,before.color_preference,(X4StreamCpuVariant)(iteration++%4u)));
        X4StreamRequestConfig snapshot=x4_stream_settings_get();
        unsigned mode=x4_stream_request_recovery_mode(&snapshot),loss=x4_stream_request_loss_percent(&snapshot);
        assert(mode<=2u&&loss==(mode==2u?3u:mode));
        const X4RembSettings remb=x4_stream_settings_remb();
        assert((unsigned)remb.mode<=X4_REMB_ADAPTIVE&&remb.fixed_mbps==concurrent_remb_rates[remb.mode]);
    } while(!atomic_load_explicit(&updates_done,memory_order_acquire));
    assert(!pthread_join(producer,NULL));
    assert(x4_stream_settings_set_recovery(0,0));
    assert(x4_stream_settings_set_cpu(before.profile,before.color_preference,X4_CPU_0));
    assert(x4_stream_settings_set_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS));
}
static void menu(const char *path)
{
    remove(path);assert(!x4_stream_settings_conversion_init());
    assert(!x4_stream_settings_set_remb((X4RembMode)3,X4_REMB_DEFAULT_FIXED_MBPS));
    assert(!x4_stream_settings_set_remb(X4_REMB_FIXED,12u));
    expect_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS);
    assert(!x4_stream_request_recovery_mode(NULL)&&!x4_stream_request_loss_percent(NULL));
    assert(x4_stream_request_nack_policy(NULL)==X4_NACK_POLICY_T46B);
    X4StreamSettingsUi ui={0};
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));ui.row=18;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));ui.row=19;
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_LEFT,true));
    ui.row=22;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
    ui.row=23;assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
    assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CANCEL,true));
    assert(!x4_stream_request_recovery_mode(NULL)&&!x4_stream_request_loss_percent(NULL));
    expect_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS);
    assert(!fopen(path,"rb"));
    const unsigned losses[]={0,1,3};
    for(unsigned mode=0;mode<3;++mode)for(unsigned loss=0;loss<3;++loss){
        assert(x4_stream_settings_set_recovery(0,0));
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));ui.row=18;
        for(unsigned n=0;n<mode;++n)assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
        ui.row=19;for(unsigned n=0;n<loss;++n)assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
        assert(!x4_stream_request_recovery_mode(NULL)&&!x4_stream_request_loss_percent(NULL));
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true)&&!ui.open&&ui.applied);
        assert(x4_stream_request_recovery_mode(NULL)==mode&&x4_stream_request_loss_percent(NULL)==losses[loss]);
        FILE *file=fopen(path,"rb");assert(file);unsigned char record[8];
        assert(fread(record,1,sizeof(record),file)==sizeof(record)&&fgetc(file)==EOF&&!fclose(file));
        assert(!memcmp(record,"X4RC",4)&&record[4]==4&&
            record[5]==(mode|((unsigned)X4_NACK_POLICY_T46B<<2)|(X4_RR_EDGE<<4)|(X4_REMB_ADAPTIVE<<6))&&
            record[6]==(losses[loss]|(2u<<2)));
        assert(x4_stream_settings_set_recovery(0,0));assert(!x4_stream_settings_conversion_init());
        assert(x4_stream_request_recovery_mode(NULL)==mode&&x4_stream_request_loss_percent(NULL)==losses[loss]);
    }
    for(unsigned policy=0;policy<=3;++policy)for(unsigned rr=0;rr<=2;++rr){
        assert(x4_stream_settings_set_recovery_policy(0,0,X4_NACK_POLICY_T43C,X4_RR_EDGE));
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));ui.row=20;
        for(unsigned i=0;i<policy;++i)assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
        ui.row=21;
        for(unsigned i=0;i<(rr+1u)%3u;++i)assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
        assert(x4_stream_request_nack_policy(NULL)==X4_NACK_POLICY_T43C&&x4_stream_request_rr_mode(NULL)==X4_RR_EDGE);
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true)&&!ui.open&&ui.applied);
        assert(x4_stream_request_nack_policy(NULL)==(X4StreamNackPolicy)policy);
        assert(x4_stream_request_rr_mode(NULL)==(X4StreamRrMode)rr);
        assert(x4_stream_settings_set_recovery_policy(0,0,X4_NACK_POLICY_T43C,X4_RR_EDGE));
        assert(!x4_stream_settings_conversion_init());
        assert(x4_stream_request_nack_policy(NULL)==(X4StreamNackPolicy)policy);
        assert(x4_stream_request_rr_mode(NULL)==(X4StreamRrMode)rr);
    }

    const unsigned remb_rates[]={10u,15u,20u,30u};
    for(unsigned mode=0;mode<=X4_REMB_ADAPTIVE;++mode)for(unsigned rate=0;rate<4;++rate){
        assert(x4_stream_settings_set_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS));
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_OPEN,true));
        ui.row=22;
        for(unsigned i=0;i<(mode+1u)%3u;++i)assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
        ui.row=23;
        for(unsigned i=0;i<(rate+2u)%4u;++i)assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_RIGHT,true));
        expect_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS);
        assert(x4_stream_settings_ui_handle(&ui,X4_SETTINGS_CONFIRM,true)&&!ui.open&&ui.applied);
        expect_remb((X4RembMode)mode,remb_rates[rate]);
        assert(x4_stream_settings_set_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS));
        assert(!x4_stream_settings_conversion_init());
        expect_remb((X4RembMode)mode,remb_rates[rate]);
    }

    FILE *file;
    for(unsigned version=1;version<=2;++version)for(unsigned zero=0;zero<version;++zero){
        unsigned char old[8]={'X','4','R','C',(unsigned char)version,
            (unsigned char)(2u|(version==2?4u:0u)|(zero?8u:0u)),3,0xa7};
        for(unsigned i=0;i<7;++i)old[7]=(unsigned char)((old[7]*33u)^old[i]);
        file=fopen(path,"wb");assert(file);
        assert(fwrite(old,1,sizeof(old),file)==sizeof(old)&&!fclose(file));
        assert(!x4_stream_settings_conversion_init());
        assert(x4_stream_request_recovery_mode(NULL)==2&&x4_stream_request_loss_percent(NULL)==3);
        assert(x4_stream_request_nack_policy(NULL)==(version==2?X4_NACK_POLICY_T43:X4_NACK_POLICY_T46B));
        assert(x4_stream_request_rr_mode(NULL)==(zero?X4_RR_ZERO:X4_RR_EDGE));
        expect_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS);
    }

    unsigned char v3[8]={'X','4','R','C',3,1u|(X4_NACK_POLICY_T43B<<2)|(X4_RR_REPAIRED<<4),3,0xa7};
    for(unsigned i=0;i<7;++i)v3[7]=(unsigned char)((v3[7]*33u)^v3[i]);
    file=fopen(path,"wb");assert(file);
    assert(fwrite(v3,1,sizeof(v3),file)==sizeof(v3)&&!fclose(file));
    assert(x4_stream_settings_set_remb(X4_REMB_OFF,10u));
    assert(!x4_stream_settings_conversion_init());
    assert(x4_stream_request_recovery_mode(NULL)==1&&x4_stream_request_loss_percent(NULL)==3);
    assert(x4_stream_request_nack_policy(NULL)==X4_NACK_POLICY_T43B);
    assert(x4_stream_request_rr_mode(NULL)==X4_RR_REPAIRED);
    expect_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS);

    file=fopen(path,"wb");assert(file);assert(fputs("old-or-corrupt",file)>=0&&!fclose(file));
    assert(x4_stream_settings_conversion_init()==-1);
    assert(!x4_stream_request_recovery_mode(NULL)&&!x4_stream_request_loss_percent(NULL));
    expect_remb(X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS);
    remove(path);
    x4_lang_set(X4_LANG_ES,"es-MX","MX");assert(!strcmp(x4_tr(X4_TR_LAB_RECOVERY_TITLE),"Recuperación de vídeo"));
    assert(!strcmp(x4_lang_lab_remb(X4_LAB_REMB_TITLE),"Aviso de ancho de banda (REMB)"));
    x4_lang_set(X4_LANG_EN,"en-US","US");assert(!strcmp(x4_tr(X4_TR_LAB_LOSS_TITLE),"Simulated loss"));
    assert(!strcmp(x4_lang_lab_remb(X4_LAB_REMB_TITLE),"Bandwidth feedback (REMB)"));
}
#endif
int main(int argc,char **argv)
{
    assert(argc==2);requests();
#if !X4_PUBLIC
    concurrent_settings();
    menu(argv[1]);
#else
    (void)argv;
    x4_lang_set(X4_LANG_ES,"es-MX","MX");
    assert(!*x4_tr(X4_TR_LAB_RECOVERY_TITLE)&&!*x4_tr(X4_TR_LAB_LOSS_TITLE));
    x4_lang_set(X4_LANG_EN,"en-US","US");
    assert(!*x4_tr(X4_TR_LAB_RECOVERY_TITLE)&&!*x4_tr(X4_TR_LAB_LOSS_TITLE));
#endif
    puts("PASS 44a REMB settings, 3 modes/4 rates, v1-v3 migration, v4 persistence, public Off");
    return 0;
}
