/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "stream_settings.h"
#if !X4_PUBLIC
#include "../input/script_engine.h"
#endif
#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
#include "apple_ui.h"
#include "canvas.h"
#include "font.h"
#include "lang.h"
#include "ui2_settings.h"
#include "ui2_modal.h"
#include <stdio.h>
#if !X4_PUBLIC
#include "../video/gpu_quality_ref.h"
#ifdef X4_UPDATE51_ENABLED
#include "../update/update_job51.h"
#include "../update/update_log51.h"
#endif
#include <stdatomic.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifndef X4_LAB_GPU_SETTINGS_PATH
#define X4_LAB_GPU_SETTINGS_PATH "/data/xcloud4/lab-gpu-v1.bin"
#endif
#ifndef X4_LAB_RECOVERY_SETTINGS_PATH
#define X4_LAB_RECOVERY_SETTINGS_PATH "/data/xcloud4/lab-recovery-v1.bin"
#endif

static atomic_uint conversion_backend;
static atomic_uint quality_settings;
/* The catalog owns one main-thread panel. New choices are a local draft until
 * its separate Apply action succeeds; no quality preference is written. */
static const X4StreamSettingsUi *quality_draft_owner;
static unsigned quality_draft;
static const X4StreamSettingsUi *recovery_draft_owner;
static unsigned recovery_draft_mode,recovery_draft_loss;
static X4StreamNackPolicy recovery_draft_policy;
static X4StreamRrMode recovery_draft_rr;
static X4RembSettings recovery_draft_remb;
enum { RECOVERY_FILE_LEGACY=4u, RECOVERY_FILE_RR_ZERO=8u, RECOVERY_FILE_VERSION=4u };
static const unsigned remb_fixed_rates[]={10u,15u,X4_REMB_DEFAULT_FIXED_MBPS,30u};
enum { REMB_RATE_COUNT=sizeof(remb_fixed_rates)/sizeof(remb_fixed_rates[0]) };
enum { CONVERSION_BIT=32u, CONVERSION_SAVE_ERROR=64u, VSYNC_T27_DRAFT=128u };
enum { QUALITY_DITHER=1u, QUALITY_DEBAND_MASK=6u, QUALITY_CAS=8u,
    QUALITY_PAUSE_REPEAT=16u, QUALITY_MASK=31u };

static bool quality_valid(unsigned word)
{ return !(word&~QUALITY_MASK)&&((word>>1)&3u)<=2u; }

unsigned x4_gpu_quality_settings_get(void)
{ return atomic_load_explicit(&quality_settings,memory_order_acquire); }

bool x4_gpu_quality_settings_set(unsigned word)
{
    if(!quality_valid(word))return false;
    atomic_store_explicit(&quality_settings,word,memory_order_release);
    return true;
}

static unsigned draft_quality(const X4StreamSettingsUi *s)
{ return quality_draft_owner==s?quality_draft:x4_gpu_quality_settings_get(); }

static unsigned char conversion_checksum(const unsigned char *record)
{
    unsigned char value=0xa7u;
    for(unsigned i=0;i<7u;++i)value=(unsigned char)((value*33u)^record[i]);
    return value;
}
static bool recovery_values_valid(unsigned mode,unsigned loss)
{return mode<=2u&&(loss==0u||loss==1u||loss==3u);}
static unsigned remb_fixed_index(unsigned mbps)
{
    for(unsigned i=0;i<REMB_RATE_COUNT;++i)
        if(remb_fixed_rates[i]==mbps)return i;
    return REMB_RATE_COUNT;
}
static void recovery_draft_capture(const X4StreamSettingsUi *owner,const X4StreamRequestConfig *request)
{
    recovery_draft_owner=owner;
    recovery_draft_mode=x4_stream_request_recovery_mode(request);
    recovery_draft_loss=x4_stream_request_loss_percent(request);
    recovery_draft_policy=x4_stream_request_nack_policy(request);
    recovery_draft_rr=x4_stream_request_rr_mode(request);
    recovery_draft_remb=x4_stream_settings_remb();
}
static int recovery_load(void)
{
    (void)x4_stream_settings_set_recovery_policy(0,0,X4_NACK_POLICY_T46B,X4_RR_EDGE);
    X4RembSettings remb={X4_REMB_ADAPTIVE,X4_REMB_DEFAULT_FIXED_MBPS};
    (void)x4_stream_settings_set_remb(remb.mode,remb.fixed_mbps);
    recovery_draft_owner=NULL;
    FILE *file=fopen(X4_LAB_RECOVERY_SETTINGS_PATH,"rb");
    if(!file)return 0;
    unsigned char record[8];size_t bytes=fread(record,1,sizeof(record),file);int tail=fgetc(file);
    bool good=!ferror(file);if(fclose(file))good=false;
    if(!good||bytes!=sizeof(record)||tail!=EOF||memcmp(record,"X4RC",4)||
        record[7]!=conversion_checksum(record))return -1;
    unsigned version=record[4],flags=record[5],mode=flags&3u,loss=record[6];
    X4StreamNackPolicy policy=X4_NACK_POLICY_T46B;
    X4StreamRrMode rr=X4_RR_EDGE;
    if(version==1u){
        if(flags&~3u)return -1;
    }else if(version==2u){
        if(flags&~15u)return -1;
        if(flags&RECOVERY_FILE_LEGACY)policy=X4_NACK_POLICY_T43;
        if(flags&RECOVERY_FILE_RR_ZERO)rr=X4_RR_ZERO;
    }else if(version==3u||version==RECOVERY_FILE_VERSION){
        if(version==3u&&(flags&~63u))return -1;
        policy=(X4StreamNackPolicy)((flags>>2)&3u);
        rr=(X4StreamRrMode)((flags>>4)&3u);
        if(version==RECOVERY_FILE_VERSION){
            if(loss&~15u)return -1;
            remb.mode=(X4RembMode)(flags>>6);
            if((unsigned)remb.mode>X4_REMB_ADAPTIVE)return -1;
            remb.fixed_mbps=remb_fixed_rates[loss>>2];
            loss&=3u;
        }
    }else return -1;
    if(!recovery_values_valid(mode,loss)||
        !x4_stream_settings_set_recovery_policy(mode,loss,policy,rr))return -1;
    return x4_stream_settings_set_remb(remb.mode,remb.fixed_mbps)?0:-1;
}
static bool recovery_save(unsigned mode,unsigned loss,X4StreamNackPolicy policy,X4StreamRrMode rr,
    X4RembSettings remb)
{
    unsigned rate_index=remb_fixed_index(remb.fixed_mbps);
    if(!recovery_values_valid(mode,loss)||(unsigned)policy>(unsigned)X4_NACK_POLICY_T46B||
        (unsigned)rr>X4_RR_EDGE||(unsigned)remb.mode>X4_REMB_ADAPTIVE||rate_index>=REMB_RATE_COUNT)return false;
    const char *path=X4_LAB_RECOVERY_SETTINGS_PATH;
    if(!strcmp(path,"/data/xcloud4/lab-recovery-v1.bin")){
#if defined(_WIN32)
        (void)_mkdir("/data");(void)_mkdir("/data/xcloud4");
#else
        (void)mkdir("/data",0777);(void)mkdir("/data/xcloud4",0777);
#endif
    }
    char temporary[264];int n=snprintf(temporary,sizeof(temporary),"%s.tmp",path);
    if(n<0||(size_t)n>=sizeof(temporary))return false;
    /* V4 uses reserved bits. V1–V3 keep their existing interpretation. */
    unsigned flags=mode|((unsigned)policy<<2)|((unsigned)rr<<4)|((unsigned)remb.mode<<6);
    unsigned char record[8]={'X','4','R','C',RECOVERY_FILE_VERSION,(unsigned char)flags,
        (unsigned char)(loss|(rate_index<<2)),0};
    record[7]=conversion_checksum(record);
    FILE *file=fopen(temporary,"wb");if(!file)return false;
    bool good=fwrite(record,1,sizeof(record),file)==sizeof(record);
    if(fflush(file))good=false;
#if !defined(_WIN32)
    if(good&&fsync(fileno(file)))good=false;
#endif
    if(fclose(file))good=false;
    if(!good){(void)remove(temporary);return false;}
    if(rename(temporary,path)){(void)remove(temporary);return false;}
    return true;
}

int x4_stream_settings_conversion_init(void)
{
    unsigned backend=0;int result=0;
    FILE *file=fopen(X4_LAB_GPU_SETTINGS_PATH,"rb");
    if(file){
        unsigned char record[8];size_t bytes=fread(record,1,sizeof(record),file);
        int tail=fgetc(file);bool good=!ferror(file);
        if(fclose(file))good=false;
        if(good&&bytes==sizeof(record)&&tail==EOF&&!memcmp(record,"X4GC",4)&&
            record[4]==1u&&record[5]<=1u&&!record[6]&&record[7]==conversion_checksum(record))backend=record[5];
        else result=-1;
    }
    atomic_store_explicit(&conversion_backend,backend,memory_order_release);
    atomic_store_explicit(&quality_settings,0u,memory_order_release);
    quality_draft_owner=NULL;
    int recovery_result=recovery_load();
    return result?result:recovery_result;
}

unsigned x4_stream_settings_conversion_backend(void)
{ return atomic_load_explicit(&conversion_backend,memory_order_acquire); }

bool x4_stream_settings_conversion_set(unsigned backend)
{
    if(backend>1u)return false;
    if(backend==x4_stream_settings_conversion_backend())return true;
    const char *path=X4_LAB_GPU_SETTINGS_PATH;
    char temp[264];int size=snprintf(temp,sizeof(temp),"%s.tmp",path);
    if(size<0||(size_t)size>=sizeof(temp))return false;
    if(!strcmp(path,"/data/xcloud4/lab-gpu-v1.bin")){
#if defined(_WIN32)
        (void)_mkdir("/data");(void)_mkdir("/data/xcloud4");
#else
        (void)mkdir("/data",0777);(void)mkdir("/data/xcloud4",0777);
#endif
    }
    unsigned char record[8]={'X','4','G','C',1u,(unsigned char)backend,0,0};
    record[7]=conversion_checksum(record);
    FILE *file=fopen(temp,"wb");if(!file)return false;
    bool good=fwrite(record,1,sizeof(record),file)==sizeof(record);
    if(fflush(file))good=false;
#if !defined(_WIN32)
    if(good&&fsync(fileno(file)))good=false;
#endif
    if(fclose(file))good=false;
    if(!good){(void)remove(temp);return false;}
    if(rename(temp,path)){(void)remove(temp);return false;}
    atomic_store_explicit(&conversion_backend,backend,memory_order_release);
    return true;
}


#if !X4_PUBLIC
static X4ScriptEntry s_scanned_scripts[X4_MAX_SCRIPTS];
static int s_scanned_count = -1;
static int s_selected_script_idx = 0;
static int s_pending_script_idx = 0;
static bool s_warning_dialog_open = false;
static bool s_warning_accepted = false;
static bool s_scripts_loaded_init = false;

static void script_settings_rescan(void)
{
    char selected_path[256] = {0};
    if (!s_scripts_loaded_init) {
        s_scripts_loaded_init = true;
        (void)x4_script_engine_load_persistence(&s_warning_accepted,
            selected_path, sizeof(selected_path));
    } else if (s_selected_script_idx > 0 && s_selected_script_idx <= s_scanned_count) {
        (void)snprintf(selected_path, sizeof(selected_path), "%s",
            s_scanned_scripts[s_selected_script_idx - 1].path);
    }
    s_scanned_count = x4_script_engine_scan(s_scanned_scripts, X4_MAX_SCRIPTS);
    if (s_scanned_count < 0) s_scanned_count = 0;
    s_selected_script_idx = 0;
    if (selected_path[0]) {
        for (int i = 0; i < s_scanned_count; ++i) {
            if (!strcmp(s_scanned_scripts[i].path, selected_path)) {
                s_selected_script_idx = i + 1;
                break;
            }
        }
    }
}
#endif

/* convert_sharp's last bit keeps the fifth policy in the existing UI draft
 * without changing the panel layout used by its owner. */
static X4StreamVsyncPolicy draft_vsync_policy(const X4StreamSettingsUi *s)
{
    if(s->convert_sharp&VSYNC_T27_DRAFT)return X4_VSYNC_AUTO_T27;
    X4StreamRequestConfig draft={.interaction_modes=s->interaction_modes};
    return x4_stream_request_vsync_policy(&draft);
}

static bool draft_valid(const X4StreamSettingsUi *s)
{
    return (unsigned)s->profile<=X4_REQUEST_PROFILE_MAX&&(unsigned)s->color<=X4_COLOR_FORCE_601&&
        (unsigned)s->cpu<=X4_CPU_3&&(unsigned)s->depth<=X4_DEPTH_2&&
        ((s->convert_sharp>>1)&3u)<=2u&&
        ((s->convert_sharp>>3)&3u)<=2u&&quality_valid(draft_quality(s))&&
        recovery_draft_owner==s&&recovery_values_valid(recovery_draft_mode,recovery_draft_loss);
}
enum { REMB_MODE_ROW=22u, REMB_FIXED_ROW=23u };
#ifdef X4_UPDATE51_ENABLED
#define SETTINGS_ROWS (X4_UPDATE51_LAB_ROW + 2u)
static enum { APPINST_DIALOG_CLOSED, APPINST_DIALOG_CONFIRM, APPINST_DIALOG_START_FAILED } appinst_dialog;
static bool appinst_confirm_focused;
static X4UpdateLog51 last_update;
static bool last_update_known;
static char last_interrupted_call[80];
#else
#define SETTINGS_ROWS 24u
#endif
static unsigned lab_settings_rows(void)
{
#ifdef X4_UPDATE51_ENABLED
    return SETTINGS_ROWS + (x4_update51_pending() ? 1u : 0u);
#else
    return SETTINGS_ROWS;
#endif
}
#define LAB_VISIBLE_ROWS 10u
#define ROW_Y(row,original) (175+(row)*52)
#define ROW_H 50
#define PANEL_TITLE_Y 90
#define PANEL_NOTE_Y 145
#define LAB_TEXT(id,original) x4_tr(id)
#else
#line 237
#define SETTINGS_ROWS 11u
#define ROW_Y(row,original) (original)
#define ROW_H 54
#define PANEL_TITLE_Y 105
#define PANEL_NOTE_Y 142
#define LAB_TEXT(id,original) (original)
#endif

static void text(uint32_t *p, int x, int y, int size, int bold, const char *value, int width, uint32_t color)
{ x4_text_ellipsis_px(p, x, y, size, bold, value, width, color); }

#if !X4_PUBLIC
/* Display order is independent from stable captured profile IDs. */
static const X4StreamRequestProfile profile_order[]={X4_REQUEST_720P60,X4_REQUEST_720P_HQ,
    X4_REQUEST_1080P_G,X4_REQUEST_1080P_G8,X4_REQUEST_1080P_H,X4_REQUEST_1080P_HQ};
static unsigned profile_ordinal(X4StreamRequestProfile profile)
{
    for(unsigned i=0;i<X4_REQUEST_PROFILE_COUNT;++i)if(profile_order[i]==profile)return i;
    return 0;
}

bool x4_stream_settings_ui_handle(X4StreamSettingsUi *s, unsigned a, bool can_edit)
{
    if (!s) return false;
#if !X4_PUBLIC
#ifdef X4_UPDATE51_ENABLED
    if (appinst_dialog != APPINST_DIALOG_CLOSED) {
        if (!can_edit || (a & X4_SETTINGS_CANCEL))
            appinst_dialog = APPINST_DIALOG_CLOSED;
        else if (a == X4_SETTINGS_CONFIRM) {
            if (appinst_dialog == APPINST_DIALOG_CONFIRM && appinst_confirm_focused &&
                    !x4_update51_diagnose_appinst())
                appinst_dialog = APPINST_DIALOG_START_FAILED;
            else
                appinst_dialog = APPINST_DIALOG_CLOSED;
        } else if (a == X4_SETTINGS_LEFT || a == X4_SETTINGS_RIGHT)
            appinst_confirm_focused = a == X4_SETTINGS_RIGHT;
        return true;
    }
#endif
    if (s_warning_dialog_open) {
        if (a & X4_SETTINGS_CONFIRM) {
            s_warning_accepted = true;
            s_selected_script_idx = s_pending_script_idx;
            s_warning_dialog_open = false;
            const char *script_path = (s_selected_script_idx > 0 && s_selected_script_idx <= s_scanned_count) ?
                s_scanned_scripts[s_selected_script_idx - 1].path : "";
            (void)x4_script_engine_save_persistence(s_warning_accepted, script_path);
            return true;
        } else if (a & X4_SETTINGS_CANCEL) {
            s_warning_dialog_open = false;
            s_selected_script_idx = 0;
            return true;
        }
        return true;
    }
#endif
    if (!can_edit) { bool consumed = s->open || (a & X4_SETTINGS_OPEN); s->open = false;
#if !X4_PUBLIC
        if(quality_draft_owner==s)quality_draft_owner=NULL;
        if(recovery_draft_owner==s)recovery_draft_owner=NULL;
#endif
        return consumed; }
    if (!s->open) {
        if (!(a & X4_SETTINGS_OPEN)) return false;
#if !X4_PUBLIC
        script_settings_rescan(); /* Newly copied scripts appear on each R3 open. */
#ifdef X4_UPDATE51_ENABLED
        last_update_known = x4_update_log51_last(&last_update);
        if (!x4_update_log51_interrupted(last_interrupted_call, sizeof(last_interrupted_call), NULL))
            last_interrupted_call[0] = '\0';
#endif
#endif
        X4StreamRequestConfig current = x4_stream_settings_get();
        s->profile = current.profile; s->color = current.color_preference; s->cpu=(X4StreamCpuVariant)current.cpu_variant;
        s->depth=(X4StreamDecoderDepth)current.decoder_depth;
        s->interaction_modes=current.interaction_modes;
        unsigned req_mode=current.resolution_request_mode&X4_RESOLUTION_REQUEST_MODE_MASK;
        s->convert_sharp=(current.convert_mode&1u)|((current.sharpness&3u)<<1)|((req_mode<=2u?req_mode:0u)<<3);
#if !X4_PUBLIC
        s->convert_sharp|=(unsigned char)(x4_stream_settings_conversion_backend()<<5);
        if(x4_stream_request_vsync_policy(&current)==X4_VSYNC_AUTO_T27)s->convert_sharp|=VSYNC_T27_DRAFT;
        quality_draft=x4_gpu_quality_settings_get();quality_draft_owner=s;
        recovery_draft_capture(s,&current);
#endif
        s->row = 0; s->open = true; s->applied = false;
        return true; /* R3+X may open, but cannot confirm or launch. */
    }
    if (a & X4_SETTINGS_CANCEL) { s->open = false;
#if !X4_PUBLIC
        if(quality_draft_owner==s)quality_draft_owner=NULL;
        if(recovery_draft_owner==s)recovery_draft_owner=NULL;
#endif
        return true; }
#if !X4_PUBLIC
    if(quality_draft_owner!=s){quality_draft=x4_gpu_quality_settings_get();quality_draft_owner=s;}
    if(recovery_draft_owner!=s){X4StreamRequestConfig current=x4_stream_settings_get();recovery_draft_capture(s,&current);}
#endif
    unsigned navigation = a & (X4_SETTINGS_UP | X4_SETTINGS_DOWN | X4_SETTINGS_LEFT | X4_SETTINGS_RIGHT);
    unsigned rows = SETTINGS_ROWS;
#if !X4_PUBLIC
    if (navigation || (a & X4_SETTINGS_CONFIRM)) {
        rows = lab_settings_rows();
        if (s->row >= rows) {
            /* An expired pending row may disappear while it is selected. */
            s->row = rows - 1u;
            return true;
        }
    }
#endif
    if (navigation) {
        if (a & X4_SETTINGS_DOWN) s->row=(s->row+1u)%rows;
        else if(a & X4_SETTINGS_UP) s->row=(s->row+rows-1u)%rows;
#if !X4_PUBLIC
        s->convert_sharp&=(unsigned char)~CONVERSION_SAVE_ERROR;
#endif
        if (a & (X4_SETTINGS_LEFT | X4_SETTINGS_RIGHT)) {
            if (!s->row) s->profile = profile_order[(profile_ordinal(s->profile) +
                ((a & X4_SETTINGS_RIGHT) ? 1u : X4_REQUEST_PROFILE_COUNT-1u)) % X4_REQUEST_PROFILE_COUNT];
            else if(s->row==1) s->color = (X4StreamColorPreference)((unsigned)s->color ^ 1u);
            else if(s->row==2) s->cpu=(X4StreamCpuVariant)(((unsigned)s->cpu+((a&X4_SETTINGS_RIGHT)?1u:3u))%4u);
            else if(s->row==3) s->depth=(X4StreamDecoderDepth)(((unsigned)s->depth+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u);
            else if(s->row==4) s->interaction_modes^=X4_INTERACTION_INPUT_60HZ;
            else if(s->row==5) s->interaction_modes^=X4_INTERACTION_RUMBLE_OFF;
            else if(s->row==6) s->interaction_modes^=X4_INTERACTION_AUDIO_PREVIOUS;
            else if(s->row==7) {
#if X4_PUBLIC
                X4StreamRequestConfig draft={.interaction_modes=s->interaction_modes};
                X4StreamVsyncPolicy policy=x4_stream_request_vsync_policy(&draft)==X4_VSYNC_NEVER?
                    X4_VSYNC_ALWAYS:X4_VSYNC_NEVER;
                s->interaction_modes=(unsigned char)x4_stream_interaction_with_vsync(s->interaction_modes,policy);
#else
                const unsigned count=(unsigned)X4_VSYNC_AUTO_T27+1u;
                unsigned policy=((unsigned)draft_vsync_policy(s)+((a&X4_SETTINGS_RIGHT)?1u:count-1u))%count;
                unsigned modes=x4_stream_interaction_with_vsync(s->interaction_modes,(X4StreamVsyncPolicy)policy);
                s->interaction_modes=(unsigned char)modes;
                s->convert_sharp&=(unsigned char)~VSYNC_T27_DRAFT;
                if(modes&X4_INTERACTION_VSYNC_T27)s->convert_sharp|=VSYNC_T27_DRAFT;
#endif
            }
            else if(s->row==8) s->convert_sharp^=1u;
            else if(s->row==9) {
                unsigned cur_sharp=(s->convert_sharp>>1)&3u;
                unsigned new_sharp=(cur_sharp+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u;
                s->convert_sharp=(s->convert_sharp&~6u)|(new_sharp<<1);
            }
            else if(s->row==10) {
                unsigned cur_req=(s->convert_sharp>>3)&3u;
                unsigned new_req=(cur_req+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u;
                s->convert_sharp=(s->convert_sharp&~24u)|(new_req<<3);
            }
#if !X4_PUBLIC
            else if(s->row==11)s->convert_sharp^=CONVERSION_BIT;
            else if(s->row==12)quality_draft^=QUALITY_DITHER;
            else if(s->row==13){
                unsigned deband=(quality_draft>>1)&3u;
                deband=(deband+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u;
                quality_draft=(quality_draft&~QUALITY_DEBAND_MASK)|(deband<<1);
            }
            else if(s->row==14)quality_draft^=QUALITY_CAS;
            else if(s->row==15)quality_draft^=QUALITY_PAUSE_REPEAT;
            else if(s->row==16)s->interaction_modes^=X4_INTERACTION_MAILBOX;
            else if(s->row==17){
                int total = s_scanned_count + 1;
                if (total > 1) {
                    int next = s_selected_script_idx;
                    if (a & X4_SETTINGS_RIGHT) next = (next + 1) % total;
                    else next = (next + total - 1) % total;
                    if (next > 0 && !s_warning_accepted) {
                        s_pending_script_idx = next;
                        s_warning_dialog_open = true;
                    } else {
                        s_selected_script_idx = next;
                    }
                }
            }
            else if(s->row==18)recovery_draft_mode=(recovery_draft_mode+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u;
            else if(s->row==19){
                const unsigned loss_values[]={0u,1u,3u};
                unsigned index=recovery_draft_loss==3u?2u:recovery_draft_loss;
                recovery_draft_loss=loss_values[(index+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u];
            }
            else if(s->row==20)recovery_draft_policy=(X4StreamNackPolicy)(
                (recovery_draft_policy+((a&X4_SETTINGS_RIGHT)?1u:3u))%4u);
            else if(s->row==21)recovery_draft_rr=(X4StreamRrMode)(
                (recovery_draft_rr+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u);
            else if(s->row==REMB_MODE_ROW)
                recovery_draft_remb.mode=(X4RembMode)(
                    ((unsigned)recovery_draft_remb.mode+((a&X4_SETTINGS_RIGHT)?1u:2u))%3u);
            else if(s->row==REMB_FIXED_ROW){
                unsigned index=remb_fixed_index(recovery_draft_remb.fixed_mbps);
                unsigned step=(a&X4_SETTINGS_RIGHT)?1u:REMB_RATE_COUNT-1u;
                recovery_draft_remb.fixed_mbps=remb_fixed_rates[(index+step)%REMB_RATE_COUNT];
            }
#endif
        }
    } else if (a & X4_SETTINGS_CONFIRM) {
#if !X4_PUBLIC
#ifdef X4_UPDATE51_ENABLED
        if (s->row == X4_UPDATE51_LAB_ROW) return true; /* Main owns the update confirmation. */
        if (s->row == rows - 1u) {
            appinst_dialog = APPINST_DIALOG_CONFIRM;
            appinst_confirm_focused = false;
            return true;
        }
        if (s->row == X4_UPDATE51_LAB_PENDING_ROW) {
            if (x4_update51_cancel_pending()) s->row = X4_UPDATE51_LAB_ROW;
            return true;
        }
#endif
        if(!draft_valid(s))return true;
        if(!x4_stream_settings_conversion_set((s->convert_sharp>>5)&1u)){
            s->convert_sharp|=CONVERSION_SAVE_ERROR;return true;
        }
        if(!recovery_save(recovery_draft_mode,recovery_draft_loss,recovery_draft_policy,recovery_draft_rr,
            recovery_draft_remb)){
            s->convert_sharp|=CONVERSION_SAVE_ERROR;return true;
        }
#endif
        if(
#if !X4_PUBLIC
            x4_stream_settings_set_lab_recovery(s->profile,s->color,s->cpu,s->depth,
                s->interaction_modes|((s->convert_sharp&VSYNC_T27_DRAFT)?X4_INTERACTION_VSYNC_T27:0u),
                s->convert_sharp&1u,(s->convert_sharp>>1)&3u,(s->convert_sharp>>3)&3u,recovery_draft_mode,recovery_draft_loss,
                recovery_draft_policy,recovery_draft_rr)
#else
            x4_stream_settings_set_lab(s->profile,s->color,s->cpu,s->depth,s->interaction_modes,
                s->convert_sharp&1u,(s->convert_sharp>>1)&3u,(s->convert_sharp>>3)&3u)
#endif
            ){
#if !X4_PUBLIC
        (void)x4_gpu_quality_settings_set(quality_draft); /* Already validated; atomic publish cannot fail. */
        (void)x4_stream_settings_set_remb(recovery_draft_remb.mode,recovery_draft_remb.fixed_mbps);
        if (s_selected_script_idx == 0) {
            x4_script_engine_unload();
            (void)x4_script_engine_save_persistence(s_warning_accepted, "");
        } else if (s_selected_script_idx <= s_scanned_count) {
            const char *path = s_scanned_scripts[s_selected_script_idx - 1].path;
            if (x4_script_engine_load(path)) {
                (void)x4_script_engine_save_persistence(s_warning_accepted, path);
            } else {
                s_selected_script_idx = 0;
                (void)x4_script_engine_save_persistence(s_warning_accepted, "");
            }
        }
        quality_draft_owner=NULL;
        recovery_draft_owner=NULL;
#endif
        /* x4_stream_settings_set_extended(s->profile,s->color,s->cpu,s->depth,s->interaction_modes) */
        s->applied = true; s->open = false;
        }
    }
    return true;
}
void x4_stream_settings_summary(char *out, unsigned capacity)
{
    X4StreamRequestConfig current = x4_stream_settings_get();
    if (out && capacity) (void)snprintf(out, capacity, "Calidad: %s · CPU-%u · R3", current.label,current.cpu_variant);
}
int x4_stream_settings_applied_text(const X4StreamSettingsUi *s, char *out, unsigned capacity)
{
    if (!out || !capacity) return -1;
    out[0] = 0;
    if (!s || !s->applied || s->open) return -1;
    X4StreamRequestConfig applied = x4_stream_request_config(s->profile,s->color);
    int n=snprintf(out,capacity,"Aplicado: %s · CPU-%u",applied.label,(unsigned)s->cpu);
    return n>=0&&(unsigned)n<capacity?0:-1;
}
#if X4_PUBLIC
static void border(uint32_t *p, int x, int y, int w, int h, uint32_t c)
{
    x4_rect(p,x,y,w,3,c); x4_rect(p,x,y+h-3,w,3,c);
    x4_rect(p,x,y,3,h,c); x4_rect(p,x+w-3,y,3,h,c);
}
#endif
#if !X4_PUBLIC
bool x4_stream_settings_ui_warning_open(void)
{
#ifdef X4_UPDATE51_ENABLED
    if (appinst_dialog != APPINST_DIALOG_CLOSED) return true;
#endif
    return s_warning_dialog_open;
}

void x4_stream_settings_ui_draw_modal(uint32_t *p)
{
    if (!p) return;
#ifdef X4_UPDATE51_ENABLED
    if (appinst_dialog == APPINST_DIALOG_CONFIRM) {
        const X4Ui2ModalAction actions[] = {
            {X4_PAD_CIRCLE, x4_tr(X4_TR_ACCESS_CANCEL), false, !appinst_confirm_focused, true},
            {X4_PAD_CROSS, x4_tr(X4_TR_51F_APPINST_CONFIRM), true, appinst_confirm_focused, false},
        };
        x4_ui2_modal_message(p, x4_tr(X4_TR_51F_APPINST_ROW),
            x4_tr(X4_TR_51F_APPINST_WARNING), NULL, X4_UI2_MUTED,
            actions, sizeof(actions) / sizeof(actions[0]));
        return;
    }
    if (appinst_dialog == APPINST_DIALOG_START_FAILED) {
        const X4Ui2ModalAction close = {X4_PAD_CIRCLE, x4_tr(X4_TR_UPDATE_NOTICE_CLOSE), false, true, true};
        x4_ui2_modal_message(p, x4_tr(X4_TR_51F_APPINST_ROW),
            x4_tr(X4_TR_51F_APPINST_START_ERROR), NULL, X4_UI2_MUTED, &close, 1);
        return;
    }
#endif
    if (!s_warning_dialog_open) return;
    const X4Ui2ModalAction actions[] = {
        {X4_PAD_CROSS, x4_tr(X4_TR_LAB_SCRIPT_WARNING_ACCEPT), true, true, false},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_LAB_SCRIPT_WARNING_CANCEL), false, false, false},
    };
    x4_ui2_modal_message(p, x4_tr(X4_TR_LAB_SCRIPT_WARNING_TITLE),
        x4_tr(X4_TR_LAB_SCRIPT_WARNING_BODY), NULL, X4_UI2_MUTED,
        actions, sizeof(actions) / sizeof(actions[0]));
}

static void draw_lab_settings(const X4StreamSettingsUi *s,uint32_t *p)
{
    enum { x = X4_UI2_CONTENT_X, w = X4_UI_SAFE_RIGHT - x, top = 221, pitch = 55 };
    const unsigned quality=draft_quality(s);
    const unsigned rows = lab_settings_rows();
#ifdef X4_UPDATE51_ENABLED
    bool retry_local = false;
    const bool retry = x4_update51_retry_available(&retry_local);
#endif
    const unsigned selected=s->row<rows?s->row:rows-1u;
    const unsigned first=selected<LAB_VISIBLE_ROWS?0u:selected-LAB_VISIBLE_ROWS+1u;
    const enum X4TextId labels[REMB_MODE_ROW]={
        X4_TR_LAB_PROFILE,X4_TR_LAB_COLOR,X4_TR_LAB_CPU,X4_TR_LAB_DEPTH,
        X4_TR_LAB_INPUT,X4_TR_LAB_RUMBLE,X4_TR_LAB_AUDIO,X4_TR_LAB_VSYNC,
        X4_TR_LAB_FILTER,X4_TR_LAB_SHARPNESS,X4_TR_LAB_REQUEST,X4_TR_LAB_CONVERSION,
        X4_TR_LAB_DITHER,X4_TR_LAB_DEBAND,X4_TR_LAB_SHARPNESS_TYPE,X4_TR_LAB_GPU_PAUSE,
        X4_TR_LAB_PRESENTATION,X4_TR_LAB_CONTROLLER_SCRIPT,
        X4_TR_LAB_RECOVERY_TITLE,X4_TR_LAB_LOSS_TITLE,X4_TR_LAB_NACK_POLICY,X4_TR_LAB_RR_POLICY};
    const enum X4TextId profiles[]={X4_TR_LAB_PROFILE_720,X4_TR_LAB_PROFILE_720HQ,
        X4_TR_LAB_PROFILE_G,X4_TR_LAB_PROFILE_G8,X4_TR_LAB_PROFILE_H,X4_TR_LAB_PROFILE_HQ};
    const enum X4TextId cpus[]={X4_TR_LAB_CPU0,X4_TR_LAB_CPU1,X4_TR_LAB_CPU2,X4_TR_LAB_CPU3};
    const enum X4TextId depths[]={X4_TR_LAB_DEPTH_AUTO,X4_TR_LAB_DEPTH1,X4_TR_LAB_DEPTH2};
    const enum X4TextId sharpness[]={X4_TR_LAB_NO,X4_TR_LAB_SHARPNESS_LOW,X4_TR_LAB_SHARPNESS_MEDIUM};
    const enum X4TextId cas_sharpness[]={X4_TR_LAB_NO,X4_TR_LAB_CAS_LOW,X4_TR_LAB_CAS_MEDIUM};
    const enum X4TextId requests[]={X4_TR_LAB_REQUEST_REPEAT,X4_TR_LAB_REQUEST_ONCE,X4_TR_LAB_REQUEST_RETRY};
    const enum X4TextId deband[]={X4_TR_LAB_NO,X4_TR_LAB_DEBAND_SOFT,X4_TR_LAB_DEBAND_STRONG};
    const enum X4TextId vsync[]={X4_TR_VSYNC_AUTO,X4_TR_VSYNC_ALWAYS,X4_TR_VSYNC_NEVER,X4_TR_VSYNC_SYNCED};
    const enum X4TextId vsync_help[]={X4_TR_U49_VSYNC_AUTO_HELP,X4_TR_VSYNC_HELP_ALWAYS,X4_TR_U49_VSYNC_NEVER_HELP,X4_TR_VSYNC_HELP_SYNCED};
    const enum X4TextId recovery_values[]={X4_TR_LAB_RECOVERY_NACK,X4_TR_LAB_RECOVERY_RTX,X4_TR_LAB_RECOVERY_FEC};
    const enum X4TextId loss_values[]={X4_TR_U49_LOSS_0,X4_TR_U49_LOSS_1,X4_TR_U49_LOSS_3};
    X4StreamRequestConfig active_recovery=x4_stream_settings_get();
    const unsigned recover=recovery_draft_owner==s?recovery_draft_mode:x4_stream_request_recovery_mode(&active_recovery);
    const unsigned loss=recovery_draft_owner==s?recovery_draft_loss:x4_stream_request_loss_percent(&active_recovery);
    const X4StreamNackPolicy nack_policy=recovery_draft_owner==s?recovery_draft_policy:
        x4_stream_request_nack_policy(&active_recovery);
    const X4StreamRrMode rr=recovery_draft_owner==s?recovery_draft_rr:
        x4_stream_request_rr_mode(&active_recovery);
    const X4RembSettings remb=recovery_draft_owner==s?recovery_draft_remb:x4_stream_settings_remb();
    const enum X4TextId nack_values[]={X4_TR_LAB_NACK_T43C,X4_TR_LAB_NACK_T43,X4_TR_LAB_NACK_T43B};
    const enum X4TextId rr_values[]={X4_TR_LAB_RR_ZERO,X4_TR_LAB_RR_REPAIRED,X4_TR_LAB_RR_EDGE};
    const X4StreamVsyncPolicy policy=draft_vsync_policy(s);
    const bool legacy_vsync=policy==X4_VSYNC_AUTO_T27;
    const unsigned sharp=(s->convert_sharp>>1)&3u,request=(s->convert_sharp>>3)&3u,band=(quality>>1)&3u;
    const enum X4TextId values[REMB_MODE_ROW]={
        profiles[profile_ordinal(s->profile)],
        s->color==X4_COLOR_AUTO_709?X4_TR_LAB_COLOR_AUTO:X4_TR_LAB_COLOR_601,
        cpus[s->cpu<=X4_CPU_3?s->cpu:X4_CPU_0],depths[s->depth<=X4_DEPTH_2?s->depth:X4_DEPTH_AUTO],
        s->interaction_modes&X4_INTERACTION_INPUT_60HZ?X4_TR_LAB_INPUT_OLD:X4_TR_LAB_INPUT_NOW,
        s->interaction_modes&X4_INTERACTION_RUMBLE_OFF?X4_TR_LAB_NO:X4_TR_LAB_YES,
        s->interaction_modes&X4_INTERACTION_AUDIO_PREVIOUS?X4_TR_LAB_AUDIO_OLD:X4_TR_LAB_AUDIO_NEW,
        legacy_vsync?X4_TR_COUNT:vsync[policy],
        s->convert_sharp&1u?X4_TR_LAB_FILTER_SMOOTH:X4_TR_LAB_FILTER_ORIGINAL,
        (quality&QUALITY_CAS?cas_sharpness:sharpness)[sharp<=2u?sharp:0u],
        requests[request<=2u?request:1u],
        s->convert_sharp&CONVERSION_BIT?X4_TR_LAB_CONVERSION_GPU:X4_TR_LAB_CONVERSION_CPU,
        quality&QUALITY_DITHER?X4_TR_LAB_YES:X4_TR_LAB_NO,
        deband[band<=2u?band:0u],
        quality&QUALITY_CAS?X4_TR_LAB_SHARPNESS_CAS:X4_TR_LAB_SHARPNESS_CLASSIC,
        quality&QUALITY_PAUSE_REPEAT?X4_TR_LAB_GPU_PAUSE_REPEAT:X4_TR_LAB_GPU_PAUSE_CPU,
        s->interaction_modes&X4_INTERACTION_MAILBOX?X4_TR_LAB_PRESENTATION_MAILBOX:X4_TR_LAB_PRESENTATION_QUEUE,
        X4_TR_LAB_CONTROLLER_SCRIPT_NONE,recovery_values[recover<=2u?recover:0u],loss_values[loss==3u?2u:loss==1u?1u:0u],
        nack_policy==X4_NACK_POLICY_T46B?X4_TR_COUNT:nack_values[nack_policy],rr_values[rr]};
    x4_rect(p, 0, 0, X4_UI_WIDTH, X4_UI_HEIGHT, X4_UI2_BACKGROUND);
    x4_ui2_heading(p, x, 57, x4_tr(X4_TR_LAB_TITLE));
    x4_ui2_card(p, x, 150, w, 764, X4_UI2_RADIUS_CARD);
    char range[96];
    snprintf(range, sizeof(range), x4_tr(X4_TR_LAB_ROWS_FMT), first + 1u,
        first + LAB_VISIBLE_ROWS, rows);
    x4_ui2_settings_line(p, x + w - 360, 171, 20, X4_FONT_SEMIBOLD, range, 330, X4_UI2_MUTED);
    const char *note = x4_tr(X4_TR_LAB_NOTE);
#ifdef X4_UPDATE51_ENABLED
    char interrupted[192];
    if (last_interrupted_call[0]) {
        (void)snprintf(interrupted, sizeof(interrupted), x4_tr(X4_TR_51E_UPDATE_INTERRUPTED_FMT),
            last_interrupted_call);
        note = interrupted;
    }
#endif
    x4_ui2_settings_line(p, x + 24, 171, 22, X4_FONT_REGULAR, note, w - 410, X4_UI2_MUTED);
#ifdef X4_UPDATE51_ENABLED
    if (retry)
        x4_ui2_settings_line(p, x + 24, 197, X4_UI2_CAPTION, X4_FONT_SEMIBOLD,
            x4_tr(X4_TR_51E_UPDATE_RETRY_STATE), w - 48, X4_UI2_TEXT);
#endif
    for (unsigned visible = 0; visible < LAB_VISIBLE_ROWS; ++visible) {
        const unsigned row = first + visible;
        const int y = top + (int)visible * pitch;
        const char *label = row < REMB_MODE_ROW ? x4_tr(labels[row]) :
            x4_lang_lab_remb(row == REMB_MODE_ROW ? X4_LAB_REMB_TITLE : X4_LAB_REMB_FIXED_TITLE);
        const char *value;
        char formatted[32];
        if (row == REMB_MODE_ROW) {
            value = remb.mode == X4_REMB_OFF ? x4_tr(X4_TR_LAB_NO) :
                x4_lang_lab_remb(remb.mode == X4_REMB_FIXED ? X4_LAB_REMB_FIXED : X4_LAB_REMB_ADAPTIVE);
        } else if (row == REMB_FIXED_ROW) {
            snprintf(formatted, sizeof(formatted), "%u Mbps", remb.fixed_mbps);
            value = formatted;
#ifdef X4_UPDATE51_ENABLED
        } else if (row == X4_UPDATE51_LAB_ROW) {
            label = x4_tr(retry && retry_local ? X4_TR_51E_UPDATE_RETRY_LOCAL : X4_TR_51_UPDATE_LOCAL);
            value = "";
        } else if (row == rows - 1u) {
            label = x4_tr(X4_TR_51F_APPINST_ROW);
            value = "";
        } else if (row == X4_UPDATE51_LAB_PENDING_ROW) {
            label = x4_tr(X4_TR_51B_CANCEL_PENDING);
            value = "";
#endif
        } else if (row == 17u) {
            value = s_selected_script_idx > 0 && s_selected_script_idx <= s_scanned_count ?
                s_scanned_scripts[s_selected_script_idx - 1].name :
                x4_tr(X4_TR_LAB_CONTROLLER_SCRIPT_NONE);
        } else {
            value = row == 20u && nack_policy == X4_NACK_POLICY_T46B ? "T46b" :
                row == 7u && legacy_vsync ? x4_tr(X4_TR_U49_VSYNC_T27) : x4_tr(values[row]);
        }
        x4_ui2_settings_row(p, x + 23, y, w - 58, 47, label, value, selected == row, true);
    }
    const int scrollbar = (int)LAB_VISIBLE_ROWS * pitch - 8;
    const int thumb = scrollbar * (int)LAB_VISIBLE_ROWS / (int)rows;
    const int thumb_y = top + (int)first * (scrollbar - thumb) / (int)(rows - LAB_VISIBLE_ROWS);
    x4_apple_fill_round_rect(p, x + w - 21, top, 4, scrollbar, 2, X4_UI2_BORDER);
    x4_apple_fill_round_rect(p, x + w - 21, thumb_y, 4, thumb, 2, X4_UI2_MUTED);
    const char *help=selected>=REMB_MODE_ROW?
        x4_lang_lab_remb(selected==REMB_MODE_ROW?X4_LAB_REMB_HELP:X4_LAB_REMB_FIXED_HELP):
        selected==7u&&legacy_vsync?x4_tr(X4_TR_U49_VSYNC_T27_HELP):
        x4_tr(selected==7u?vsync_help[policy]:
        selected==16u?X4_TR_LAB_PRESENTATION_HELP:selected==17u?X4_TR_LAB_CONTROLLER_SCRIPT_HELP:
        selected==18u?X4_TR_LAB_RECOVERY_HELP:selected==19u?
        (!recover&&loss?X4_TR_LAB_LOSS_NACK_HELP:X4_TR_LAB_LOSS_HELP):X4_TR_LAB_QUALITY_SCOPE);
#ifdef X4_UPDATE51_ENABLED
    if (selected == X4_UPDATE51_LAB_ROW)
        help = x4_tr(retry && !retry_local ? X4_TR_51E_UPDATE_RETRY_ABOUT_HINT : X4_TR_51_UPDATE_LOCAL_HINT);
    if (selected == rows - 1u) help = x4_tr(X4_TR_51F_APPINST_WARNING);
    else if (selected == X4_UPDATE51_LAB_PENDING_ROW) help = x4_tr(X4_TR_51B_CANCEL_PENDING_HINT);
#endif
    x4_ui2_text_bounded(p, x + 24, 788, 22, X4_FONT_REGULAR, help,
        w - 48, 27, 2, X4_UI2_MUTED);
#ifdef X4_UPDATE51_ENABLED
    if (selected == X4_UPDATE51_LAB_ROW) {
        char summary[256];
        if (last_update_known) {
            const X4Install51Result *r = &last_update.installer;
            (void)snprintf(summary, sizeof(summary),
                "%s · %s · 0x%08x\nBGFT: %08x/%08x/%08x/%08x · AppInstUtil: %08x",
                x4_tr(X4_TR_51_UPDATE_LAST_RESULT), last_update.build, (unsigned)last_update.code,
                (unsigned)r->register_code, (unsigned)r->start_code,
                (unsigned)r->progress_query_code, (unsigned)r->progress_error, (unsigned)r->fallback_code);
        } else (void)snprintf(summary, sizeof(summary), "%s", x4_tr(X4_TR_51_UPDATE_NONE));
        x4_ui2_text_bounded(p, x + 24, 833, 22, X4_FONT_REGULAR, summary,
            w - 48, 26, 2, X4_UI2_MUTED);
    }
#endif
    if (s->convert_sharp & CONVERSION_SAVE_ERROR)
        x4_ui2_settings_line(p, x + 24, 855, 20, X4_FONT_SEMIBOLD,
            x4_tr(X4_TR_LAB_SAVE_ERROR), w - 48, X4_RGB(254, 165, 154));
    const X4Ui2Hint hints[] = {
#ifdef X4_UPDATE51_ENABLED
        {X4_PAD_CROSS, x4_tr(selected >= X4_UPDATE51_LAB_ROW ? X4_TR_APPLE_HINT_SELECT : X4_TR_UI2_SETTINGS_LAB_APPLY)},
#else
        {X4_PAD_CROSS, x4_tr(X4_TR_UI2_SETTINGS_LAB_APPLY)},
#endif
        {X4_PAD_CIRCLE, x4_tr(X4_TR_UI2_SETTINGS_LAB_CANCEL)},
    };
    x4_ui2_footer(p, hints, sizeof(hints) / sizeof(hints[0]));
}

void x4_stream_settings_ui_draw_page(const X4StreamSettingsUi *s, uint32_t *p)
{
    if (s && s->open && p) draw_lab_settings(s, p);
}
#endif
void x4_stream_settings_ui_draw(const X4StreamSettingsUi *s, uint32_t *p)
{
    if (!s || !p) return;
    const uint32_t bg=0xff211910,green=0xff64e78c;
#if X4_PUBLIC
    const uint32_t white=0xfff5f1eb,muted=0xffc4baae;
#endif
    if (!s->open) {
        char applied[96];
        if (!x4_stream_settings_applied_text(s,applied,sizeof(applied))) {
            x4_rect(p,650,109,580,34,bg);
            text(p,664,111,26,1,applied,552,green);
        }
        return;
    }
#if !X4_PUBLIC
    x4_stream_settings_ui_draw_page(s, p);
    if (x4_stream_settings_ui_warning_open()) {
        x4_ui2_modal_scrim(p);
        x4_stream_settings_ui_draw_modal(p);
    }
#else
    x4_rect(p,270,70,1380,940,bg); border(p,270,70,1380,940,green);
    text(p,310,PANEL_TITLE_Y,36,1,LAB_TEXT(X4_TR_LAB_TITLE,"Opciones de la próxima sesión"),1290,white);
    text(p,310,PANEL_NOTE_Y,22,0,LAB_TEXT(X4_TR_LAB_NOTE,"Se recuerda hasta cerrar XCloud4. Xbox decide qué señal entrega."),1290,muted);
    text(p,325,ROW_Y(0,175),25,1,LAB_TEXT(X4_TR_LAB_PROFILE,"Perfil de solicitud"),1250,white);
    const char *profiles[] = {LAB_TEXT(X4_TR_LAB_PROFILE_720,"720p60 · android"), LAB_TEXT(X4_TR_LAB_PROFILE_720HQ,"720p-HQ · web windows · alias 720HQ"), LAB_TEXT(X4_TR_LAB_PROFILE_G,"1080p-G · web windows · alias 1080"),
        LAB_TEXT(X4_TR_LAB_PROFILE_G8,"1080p-G8 · igual a G · solicitud 8 Mbps"),
        LAB_TEXT(X4_TR_LAB_PROFILE_H,"1080p-H · nativa windows · alias 1080"), LAB_TEXT(X4_TR_LAB_PROFILE_HQ,"1080p-HQ · web windows · alias 1080HQ")};
    text(p,345,ROW_Y(0,175)+26,23,1,profiles[profile_ordinal(s->profile)],1220,green);
    if (!s->row) x4_rect(p,300,ROW_Y(0,175)-6,4,ROW_H,green);
    text(p,325,ROW_Y(1,233),25,1,LAB_TEXT(X4_TR_LAB_COLOR,"Color si el SPS no especifica la matriz"),1240,white);
    text(p,345,ROW_Y(1,233)+26,23,0,s->color==X4_COLOR_AUTO_709?
        LAB_TEXT(X4_TR_LAB_COLOR_AUTO,"Automático (BT.709 en 720p o más)"):LAB_TEXT(X4_TR_LAB_COLOR_601,"BT.601"),1220,green);
    if (s->row==1) x4_rect(p,300,ROW_Y(1,233)-6,4,ROW_H,green);
    text(p,325,ROW_Y(2,291),25,1,LAB_TEXT(X4_TR_LAB_CPU,"CPU de la próxima sesión"),1240,white);
    const char *cpus[]={LAB_TEXT(X4_TR_LAB_CPU0,"CPU-0 · actual (700)"), LAB_TEXT(X4_TR_LAB_CPU1,"CPU-1 · decoder a prioridad 480"),
        LAB_TEXT(X4_TR_LAB_CPU2,"CPU-2 · decoder a 480 y núcleos separados"), LAB_TEXT(X4_TR_LAB_CPU3,"CPU-3 · decoder a 256 y núcleos separados")};
    text(p,345,ROW_Y(2,291)+26,23,0,cpus[s->cpu<=X4_CPU_3?s->cpu:X4_CPU_0],1220,green);
    if(s->row==2)x4_rect(p,300,ROW_Y(2,291)-6,4,ROW_H,green);
    text(p,325,ROW_Y(3,349),25,1,LAB_TEXT(X4_TR_LAB_DEPTH,"Profundidad del decoder"),1240,white);
    const char *depths[]={LAB_TEXT(X4_TR_LAB_DEPTH_AUTO,"Profundidad: Auto (720p: 1 · 1080p: 2)"),LAB_TEXT(X4_TR_LAB_DEPTH1,"Profundidad: 1"),LAB_TEXT(X4_TR_LAB_DEPTH2,"Profundidad: 2")};
    text(p,345,ROW_Y(3,349)+26,23,0,depths[s->depth<=X4_DEPTH_2?s->depth:X4_DEPTH_AUTO],1220,green);
    if(s->row==3)x4_rect(p,300,ROW_Y(3,349)-6,4,ROW_H,green);
    text(p,325,ROW_Y(4,407),25,1,LAB_TEXT(X4_TR_LAB_INPUT,"Envío del mando"),1240,white);
    text(p,345,ROW_Y(4,407)+26,23,0,s->interaction_modes&X4_INTERACTION_INPUT_60HZ?
        LAB_TEXT(X4_TR_LAB_INPUT_OLD,"60 Hz (anterior)"):LAB_TEXT(X4_TR_LAB_INPUT_NOW,"Inmediato"),1220,green);
    if(s->row==4)x4_rect(p,300,ROW_Y(4,407)-6,4,ROW_H,green);
    text(p,325,ROW_Y(5,465),25,1,LAB_TEXT(X4_TR_LAB_RUMBLE,"Vibración"),1240,white);
    text(p,345,ROW_Y(5,465)+26,23,0,s->interaction_modes&X4_INTERACTION_RUMBLE_OFF?LAB_TEXT(X4_TR_LAB_NO,"No"):LAB_TEXT(X4_TR_LAB_YES,"Sí"),1220,green);
    if(s->row==5)x4_rect(p,300,ROW_Y(5,465)-6,4,ROW_H,green);
    text(p,325,ROW_Y(6,523),25,1,LAB_TEXT(X4_TR_LAB_AUDIO,"Audio"),1240,white);
    text(p,345,ROW_Y(6,523)+26,23,0,s->interaction_modes&X4_INTERACTION_AUDIO_PREVIOUS?LAB_TEXT(X4_TR_LAB_AUDIO_OLD,"Anterior"):LAB_TEXT(X4_TR_LAB_AUDIO_NEW,"Nuevo"),1220,green);
    if(s->row==6)x4_rect(p,300,ROW_Y(6,523)-6,4,ROW_H,green);
    text(p,325,ROW_Y(7,581),25,1,LAB_TEXT(X4_TR_LAB_VSYNC,"Sincronía vertical"),1240,white);
    text(p,345,ROW_Y(7,581)+26,23,0,s->interaction_modes&X4_INTERACTION_VSYNC_OFF?
        LAB_TEXT(X4_TR_LAB_VSYNC_OFF,"No (menos retraso, puede verse un corte)"):LAB_TEXT(X4_TR_LAB_YES,"Sí"),1220,green);
    if(s->row==7)x4_rect(p,300,ROW_Y(7,581)-6,4,ROW_H,green);
    text(p,325,ROW_Y(8,639),25,1,LAB_TEXT(X4_TR_LAB_FILTER,"Conversión"),1240,white);
    text(p,345,ROW_Y(8,639)+26,23,0,(s->convert_sharp&1u)?
        LAB_TEXT(X4_TR_LAB_FILTER_SMOOTH,"Suave (bilineal H.264)"):LAB_TEXT(X4_TR_LAB_FILTER_ORIGINAL,"Original (vecino más próximo)"),1220,green);
    if(s->row==8)x4_rect(p,300,ROW_Y(8,639)-6,4,ROW_H,green);
    text(p,325,ROW_Y(9,697),25,1,LAB_TEXT(X4_TR_LAB_SHARPNESS,"Nitidez (solo luma)"),1240,white);
    const char *sharp_labels[]={LAB_TEXT(X4_TR_LAB_NO,"No"),LAB_TEXT(X4_TR_LAB_SHARPNESS_LOW,"Baja (k=0.25)"),LAB_TEXT(X4_TR_LAB_SHARPNESS_MEDIUM,"Media (k=0.5)")};
    text(p,345,ROW_Y(9,697)+26,23,0,sharp_labels[((s->convert_sharp>>1)&3u)<=2?((s->convert_sharp>>1)&3u):0],1220,green);
    if(s->row==9)x4_rect(p,300,ROW_Y(9,697)-6,4,ROW_H,green);
    text(p,325,ROW_Y(10,755),25,1,LAB_TEXT(X4_TR_LAB_REQUEST,"Petición de resolución"),1240,white);
    const char *req_labels[]={LAB_TEXT(X4_TR_LAB_REQUEST_REPEAT,"Cada 300 ms (anterior)"),LAB_TEXT(X4_TR_LAB_REQUEST_ONCE,"Una vez (como Edge)"),LAB_TEXT(X4_TR_LAB_REQUEST_RETRY,"Una vez y repetir cada 5 s sin vídeo")};
    unsigned req_sel = (s->convert_sharp >> 3) & 3u;
    text(p,345,ROW_Y(10,755)+26,23,0,req_labels[req_sel<=2?req_sel:1],1220,green);
    if(s->row==10)x4_rect(p,300,ROW_Y(10,755)-6,4,ROW_H,green);
#if !X4_PUBLIC
    text(p,325,ROW_Y(11,813),25,1,x4_tr(X4_TR_LAB_CONVERSION),1240,white);
    text(p,345,ROW_Y(11,813)+26,23,0,x4_tr((s->convert_sharp&CONVERSION_BIT)?
        X4_TR_LAB_CONVERSION_GPU:X4_TR_LAB_CONVERSION_CPU),1220,green);
    if(s->row==11)x4_rect(p,300,ROW_Y(11,813)-6,4,ROW_H,green);
    if(s->convert_sharp&CONVERSION_SAVE_ERROR)
        text(p,310,881,18,0,x4_tr(X4_TR_LAB_SAVE_ERROR),1290,0xffffb180);
#endif
    text(p,310,X4_UI_FOOTER_Y - 32,24,0,LAB_TEXT(X4_TR_LAB_NAV,"ARRIBA/ABAJO Campo   IZQUIERDA/DERECHA Cambiar"),1290,white);
    text(p,310,X4_UI_FOOTER_Y,24,0,LAB_TEXT(X4_TR_LAB_APPLY,"X Aplicar   CÍRCULO Cancelar"),1290,white);
#endif
}
#endif /* No technical R3 UI or its literals in the public product. */
#if X4_PUBLIC
#line 654
#endif
int x4_stream_received_text(char *out, unsigned capacity, unsigned width, unsigned height,
    unsigned received, unsigned selected, bool full_range)
{
    if (!out || !capacity) return -1;
    int n=snprintf(out,capacity,"Recibido %u×%u · matriz SPS %u%s · conversión %s · rango %s",
        width,height,received,received==2?" (sin especificar)":"",
        selected==1?"BT.709":"BT.601",full_range?"completo":"limitado");
    return n>=0&&(unsigned)n<capacity?0:-1;
}
void x4_stream_received_draw(uint32_t *p, unsigned width, unsigned height,
    unsigned received, unsigned selected, bool full_range)
{
    char line[224];
    if (x4_stream_received_text(line,sizeof(line),width,height,received,selected,full_range)) return;
    x4_rect(p,84,84,1752,64,0xff211910);
    text(p,104,97,30,1,line,1712,0xfff5f1eb);
}
#endif
