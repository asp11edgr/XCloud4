/* Basado en PsBox (C) 2026 RafaelNGP, GPL-3.0-or-later */
/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#if !X4_PUBLIC && !X4_VIDEO_PROBE
#include "../update/boot_diag51.h"
#endif
#include "../ui/apple_ui.h"
#include "../ui/net_hud.h"
#include "../ui/loading_screen.h"
#include "../ui/ui_perf.h"
#include "../ui/ui_draw_metrics.h"
#include "../ui/ui2_boot.h"
#include "../ui/ui2_home.h"
#include "../ui/ui2_net.h"
#include "../ui/ui2_shell.h"
#include <string.h>
#include <stdatomic.h>
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#if !X4_VIDEO_PROBE
#include <orbis/SystemService.h>
#endif
#include "../video/display.h"
#include "../input/controller.h"
#include "../ui/screen.h"
#include "../ui/lang.h"
#include "../ui/catalog_gui.h"
#include "../ui/game_page.h"
#include "../ui/favorites.h"
#include "../ui/play_history.h"
#include "../ui/ui2_account.h"
#include "../ui/ui2_info.h"
#include "../ui/ui2_modal.h"
#include "../ui/ui2_fortnite.h"
#include "../ui/catalog_presentation.h"
#include "../ui/font.h"
#include "../auth/auth_profile.h"
#include "../auth/xbox_live.h"
#include "../media/live_media.h"
#include "lifecycle.h"
#include "app_identity.h"
#include "probe_controller.h"
#if X4_VIDEO_PROBE && X4_PROBE_T27
#include "../video/probe_t27_controller.h"
#endif
#include "../streaming/stream_profile.h"
#if !X4_PUBLIC
#include "../input/script_engine.h"
#endif
#include "../streaming/stream_startup.h"
#include "../ui/stream_video_status.h"
#include "../ui/stream_settings.h"
#include "../auth/release_check.h"
#include "../ui/picture_settings.h"
#include "../ui/ui_lang_pref.h"
#include "../ui/server_settings.h"
#include "../ui/access_country.h"
#include "../ui/cloud_time_ui.h"
#include "../ui/canvas.h"
#include "../audio/mic_capture.h"

#if X4_STREAM_PROFILE && !X4_VIDEO_PROBE
/* Main is the sole writer and exporter. No pad read or local action is
 * changed by these measurements. Durations use upper 50us histogram bins. */
enum { INTERACTION_HIST_BINS=256, INTERACTION_PHASES=17, INTERACTION_GAPS=128 };
typedef struct { uint64_t bins[INTERACTION_HIST_BINS],count,total,max; } InteractionTimes;
typedef struct { uint64_t at_us,elapsed_us; unsigned phase; } InteractionGap;
static struct {
    bool available;
    unsigned modes;
    uint64_t begin_us,ended_us,calls,ok,errors,rate_second,second_calls,max_second;
    int last_result;
    struct { int result; uint64_t count; } results[16];
    unsigned result_count;
    uint64_t result_omitted;
    InteractionTimes call_times;
    uint64_t loop_previous,phase_at,phase_us[INTERACTION_PHASES],phase_gaps[INTERACTION_PHASES],gaps,gap_omitted;
    unsigned phase;
    InteractionGap gap[INTERACTION_GAPS];
} interaction_main;
typedef struct {
    OrbisPadVibeParam applied;
    int handle;
    uint64_t last_call_us;
    bool called,stop_pending;
    X4ControllerRoster *roster;
    unsigned index;
    uint64_t generation;
} InteractionRumble;
static void interaction_time(InteractionTimes *h,uint64_t value)
{
    unsigned b=value/50u<INTERACTION_HIST_BINS?(unsigned)(value/50u):INTERACTION_HIST_BINS-1;
    ++h->bins[b];++h->count;h->total+=value;if(value>h->max)h->max=value;
}
static uint64_t interaction_percentile(const InteractionTimes *h,unsigned percent)
{
    uint64_t target=(h->count*percent+99u)/100u,n=0;
    if(!target)return 0;
    for(unsigned i=0;i<INTERACTION_HIST_BINS;++i)if((n+=h->bins[i])>=target)
        return i==INTERACTION_HIST_BINS-1?h->max:(uint64_t)(i+1)*50u;
    return h->max;
}
static X4UiPerf menu_copy_times,menu_prepare_times;
static uint64_t menu_log_at,menu_interval_frames,menu_interval_open_frames;
static void interaction_main_begin(unsigned modes,uint64_t now)
{
    memset(&interaction_main,0,sizeof(interaction_main));
    interaction_main.available=true;interaction_main.modes=modes;interaction_main.begin_us=now;
    memset(&menu_copy_times,0,sizeof(menu_copy_times));memset(&menu_prepare_times,0,sizeof(menu_prepare_times));
    menu_log_at=menu_interval_frames=menu_interval_open_frames=0;
    x4_touch_menu_cache_reset();
}
static void interaction_phase(unsigned phase,uint64_t now)
{
    if(!interaction_main.available)return;
    if(interaction_main.phase_at&&now>=interaction_main.phase_at&&interaction_main.phase<INTERACTION_PHASES)
        interaction_main.phase_us[interaction_main.phase]+=now-interaction_main.phase_at;
    interaction_main.phase=phase;interaction_main.phase_at=now;
}
static void interaction_main_read(uint64_t now)
{
    if(!interaction_main.available)return;
    interaction_phase(X4_MON_PHASE_MAIN_POLL,now);
    if(interaction_main.loop_previous&&now>=interaction_main.loop_previous&&now-interaction_main.loop_previous>20000u){
        unsigned longest=0;
        for(unsigned i=1;i<INTERACTION_PHASES;++i)if(interaction_main.phase_us[i]>interaction_main.phase_us[longest])longest=i;
        ++interaction_main.phase_gaps[longest];
        uint64_t index=interaction_main.gaps++;
        if(index<INTERACTION_GAPS)interaction_main.gap[index]=(InteractionGap){now,now-interaction_main.loop_previous,longest};
        else ++interaction_main.gap_omitted;
    }
    memset(interaction_main.phase_us,0,sizeof(interaction_main.phase_us));interaction_main.loop_previous=now;
}
static void interaction_rumble_result(int result,uint64_t began,uint64_t ended)
{
    if(!interaction_main.available)return;
    uint64_t second=began>=interaction_main.begin_us?(began-interaction_main.begin_us)/1000000u:0;
    if(second!=interaction_main.rate_second){interaction_main.rate_second=second;interaction_main.second_calls=0;}
    ++interaction_main.second_calls;if(interaction_main.second_calls>interaction_main.max_second)interaction_main.max_second=interaction_main.second_calls;
    ++interaction_main.calls;if(result>=0)++interaction_main.ok;else ++interaction_main.errors;
    interaction_main.last_result=result;
    unsigned i=0;while(i<interaction_main.result_count&&interaction_main.results[i].result!=result)++i;
    if(i<16){if(i==interaction_main.result_count){interaction_main.results[i].result=result;++interaction_main.result_count;}++interaction_main.results[i].count;}
    else ++interaction_main.result_omitted;
    interaction_time(&interaction_main.call_times,ended>=began?ended-began:0);
}
/* A stop observed while the 16ms cap is active remains latched, even if a
 * newer positive intensity arrives before the next permitted native call. */
static void interaction_rumble_step(InteractionRumble *s,int handle,OrbisPadVibeParam latest,uint64_t now)
{
    if(handle<0){s->handle=-1;return;}
    if(!latest.lgMotor&&!latest.smMotor&&(s->applied.lgMotor||s->applied.smMotor))s->stop_pending=true;
    OrbisPadVibeParam wanted=s->stop_pending?(OrbisPadVibeParam){0}:latest;
    if(s->handle==handle&&s->applied.lgMotor==wanted.lgMotor&&s->applied.smMotor==wanted.smMotor){
        if(!wanted.lgMotor&&!wanted.smMotor)s->stop_pending=false;
        return;
    }
    if(s->called&&(now<s->last_call_us||now-s->last_call_us<16000u))return;
    uint64_t began=sceKernelGetProcessTime();
    int result=s->roster?x4_controller_roster_vibrate(s->roster,s->index,s->generation,
        wanted.lgMotor,wanted.smMotor):scePadSetVibration(handle,&wanted);
    uint64_t ended=sceKernelGetProcessTime();
    s->last_call_us=began;s->called=true;
    interaction_rumble_result(result,began,ended);
    if(result>=0){s->applied=wanted;s->handle=handle;if(!wanted.lgMotor&&!wanted.smMotor)s->stop_pending=false;}
}
static void interaction_rumble_stop(InteractionRumble *s,int handle)
{
    if(handle<0)return;
    if(s->handle==handle&&!s->applied.lgMotor&&!s->applied.smMotor&&!s->stop_pending)return;
    for(unsigned attempt=0;attempt<3;++attempt){
        uint64_t now=sceKernelGetProcessTime();
        if(s->called&&now>=s->last_call_us&&now-s->last_call_us<16000u){
            sceKernelUsleep((unsigned)(16000u-(now-s->last_call_us)));now=sceKernelGetProcessTime();
        }
        interaction_rumble_step(s,handle,(OrbisPadVibeParam){0},now);
        if(s->handle==handle&&!s->applied.lgMotor&&!s->applied.smMotor&&!s->stop_pending)return;
    }
    printf("XCloud4: parada de vibracion no confirmada tras 3 intentos; retorno=%d\n",interaction_main.last_result);
}
void x4_main_interaction_metrics_write(FILE *f)
{
    if(!interaction_main.ended_us)interaction_main.ended_us=sceKernelGetProcessTime();
    uint64_t ended=interaction_main.ended_us;
    uint64_t elapsed=ended>=interaction_main.begin_us?ended-interaction_main.begin_us:0;
    fprintf(f,"{\"available\":%s,\"rumble_enabled\":%s,\"native_calls\":%llu,\"successful\":%llu,\"errors\":%llu,\"elapsed_us\":%llu,\"calls_per_second\":%.6f,\"maximum_calls_in_one_second\":%llu,\"last_result\":%d,\"call_duration_us\":{\"n\":%llu,\"p50_upper\":%llu,\"p95_upper\":%llu,\"max\":%llu,\"bin_us\":50},\"native_results\":[",
        interaction_main.available?"true":"false",(interaction_main.modes&X4_INTERACTION_RUMBLE_OFF)?"false":"true",
        (unsigned long long)interaction_main.calls,(unsigned long long)interaction_main.ok,(unsigned long long)interaction_main.errors,
        (unsigned long long)elapsed,elapsed?(double)interaction_main.calls*1000000.0/(double)elapsed:0.0,(unsigned long long)interaction_main.max_second,interaction_main.last_result,
        (unsigned long long)interaction_main.call_times.count,(unsigned long long)interaction_percentile(&interaction_main.call_times,50),
        (unsigned long long)interaction_percentile(&interaction_main.call_times,95),(unsigned long long)interaction_main.call_times.max);
    for(unsigned i=0;i<interaction_main.result_count;++i){if(i)fputc(',',f);fprintf(f,"{\"result\":%d,\"count\":%llu}",interaction_main.results[i].result,(unsigned long long)interaction_main.results[i].count);}
    fprintf(f,"],\"native_results_omitted\":%llu,\"main_loop_gaps_over_20ms\":{\"count\":%llu,\"omitted\":%llu,\"phase_scope\":\"phase with most measured time in the pad-read interval; numeric phases use existing monitor enum; session startup and close included\",\"records\":[",
        (unsigned long long)interaction_main.result_omitted,(unsigned long long)interaction_main.gaps,(unsigned long long)interaction_main.gap_omitted);
    uint64_t n=interaction_main.gaps<INTERACTION_GAPS?interaction_main.gaps:INTERACTION_GAPS;
    for(uint64_t i=0;i<n;++i){if(i)fputc(',',f);fprintf(f,"{\"at_us\":%llu,\"elapsed_us\":%llu,\"phase\":%u}",(unsigned long long)interaction_main.gap[i].at_us,(unsigned long long)interaction_main.gap[i].elapsed_us,interaction_main.gap[i].phase);}
    fputs("],\"counts_by_phase\":[",f);
    for(unsigned i=0;i<INTERACTION_PHASES;++i){if(i)fputc(',',f);fprintf(f,"%llu",(unsigned long long)interaction_main.phase_gaps[i]);}
    fputs("]},\"menu_ui\":{\"steady_overlay_us\":",f);x4_ui_perf_write(f,&menu_copy_times);
    fputs(",\"preparation_us\":",f);x4_ui_perf_write(f,&menu_prepare_times);
    X4TouchMenuCacheStats menu_stats;x4_touch_menu_cache_snapshot(&menu_stats);
    fprintf(f,",\"regenerations\":%llu,\"composed_frames\":%llu,\"copied_bytes\":%llu,\"cache_bytes\":%zu,\"scope\":\"prepare outside presentation; steady_overlay includes the menu copy and any ordinary live status overlay; opaque write-only destination\"},\"loading_ui\":",
        (unsigned long long)menu_stats.regenerations,(unsigned long long)menu_stats.frames,
        (unsigned long long)menu_stats.copied_bytes,menu_stats.cache_bytes);
    x4_loading_screen_metrics_write(f);fputc('}',f);
}
#endif

#if !X4_VIDEO_PROBE
static void trace_inactive(X4LiveMedia *live)
{
    if (live) {
        X4Trace *trace = x4_live_media_trace(live);
        x4_trace_monitor_epoch(trace, X4_MON_ACTOR_SESSION, X4_MON_EPOCH_TRANSITION);
        x4_trace_set_active(trace, false);
    }
}
static bool trace_session_active(enum X4SessionState state)
{
    return state != X4_SESSION_STOPPING && state != X4_SESSION_CLOSED &&
        state != X4_SESSION_CANCELLED && state != X4_SESSION_ERROR;
}

static void mic_end_match(X4TouchMenu *menu)
{
    if (!menu->mic_enabled) return;
    menu->mic_enabled = false;
    x4_mic_session_end();
}

static void mic_begin_match(X4TouchMenu *menu)
{
    mic_end_match(menu);
    if (!x4_picture_settings_mic_enabled()) return;
    int32_t initial_user = -1;
    if (sceUserServiceGetInitialUser(&initial_user) < 0) initial_user = -1;
    menu->mic_enabled = true;
    (void)x4_mic_session_begin(true,initial_user,x4_picture_settings_mic_ms());
}

static void mic_draw(uint32_t *pixels,const X4TouchMenu *menu,
    const X4MicSnapshot *mic,const char *notice,bool show_level)
{
    if (!mic) return;
    if (menu->is_open)
        x4_apple_draw_footer_hint(pixels,112,282,X4_PAD_SQUARE,
            x4_tr(mic->muted?X4_TR_MIC_UNMUTE:X4_TR_MIC_MUTE));
    if (!mic->active && !notice) return;
    int level = 0;
#if !X4_PUBLIC
    if (show_level && mic->active)
        level=mic->level<=100u?(int)mic->level+1:101;
#else
    (void)show_level;
#endif
    x4_rect(pixels,1548,162,340,level?48:34,X4_RGB(11,16,29));
    x4_text_ellipsis_px(pixels,1560,168,19,X4_FONT_SEMIBOLD,
        notice?notice:x4_tr(X4_TR_MIC_ACTIVE),316,
        notice?X4_RGB(255,190,62):X4_RGB(238,244,252));
    if (level) {
        x4_rect(pixels,1560,198,316,5,X4_RGB(65,76,90));
        x4_rect(pixels,1560,198,(level-1)*316/100,5,X4_RGB(80,207,125));
    }
}
#if X4_STREAM_PROFILE
typedef struct {
    const X4LiveMediaSnapshot *media;
    const X4SessionSnapshot *session;
    int muted;
    const X4TouchMenu *touch_menu;
    bool show_banner;
    bool received_banner;
    bool net_hud_enabled;
    const char *pad_notice;
    X4CloudTime cloud_time;
    unsigned cloud_alert;
    const X4MicSnapshot *mic;
    const char *mic_notice;
    bool mic_show_level;
} StreamOverlayContext;
static void stream_overlay(uint32_t *pixels,void *context)
{
    const StreamOverlayContext *c=context;
    uint64_t begin=sceKernelGetProcessTime();
    if(c->net_hud_enabled&&x4_net_hud_draw(pixels))
        x4_net_hud_note_copy_us(sceKernelGetProcessTime()-begin);
    uint64_t overlay_begin=sceKernelGetProcessTime();
    x4_live_overlay(c->media,c->session,c->muted,c->touch_menu,c->show_banner,pixels);
    x4_cloud_time_game_draw(pixels,c->cloud_time,c->cloud_alert,c->touch_menu&&c->touch_menu->is_open);
    if(c->mic)mic_draw(pixels,c->touch_menu,c->mic,c->mic_notice,
        c->net_hud_enabled&&c->mic_show_level);
    if(c->pad_notice){
        x4_apple_fill_round_rect(pixels,1430,92,440,56,16,X4_RGBA(16,21,31,235));
        x4_text_ellipsis_px(pixels,1450,106,25,X4_FONT_SEMIBOLD,c->pad_notice,398,0xfff7f8ffu);
    }
#if !X4_PUBLIC
    x4_script_engine_draw_notification(pixels);
#endif
    uint64_t end=sceKernelGetProcessTime();
    ++menu_interval_frames;
    if(c->touch_menu&&c->touch_menu->is_open){
        ++menu_interval_open_frames;x4_ui_perf_record(&menu_copy_times,end-overlay_begin);
    }
#if !X4_PUBLIC
    if(!menu_log_at)menu_log_at=begin;
    if(end-menu_log_at>=5000000u){
        X4TouchMenuCacheStats stats;x4_touch_menu_cache_snapshot(&stats);
        printf("XCloud4 UI menu: copy_n=%llu p50_us=%llu p95_us=%llu max_us=%llu prepare_n=%llu prepare_max_us=%llu regenerations=%llu frames=%llu menu_frames=%llu cache_bytes=%zu\n",
            (unsigned long long)menu_copy_times.n,(unsigned long long)x4_ui_perf_percentile(&menu_copy_times,50),
            (unsigned long long)x4_ui_perf_percentile(&menu_copy_times,95),(unsigned long long)menu_copy_times.max,
            (unsigned long long)menu_prepare_times.n,(unsigned long long)menu_prepare_times.max,
            (unsigned long long)stats.regenerations,(unsigned long long)menu_interval_frames,
            (unsigned long long)menu_interval_open_frames,stats.cache_bytes);
        menu_log_at=end;menu_interval_frames=menu_interval_open_frames=0;
    }
#endif
    if(c->received_banner) x4_stream_received_draw(pixels,c->media->width,c->media->height,
        c->media->video_matrix_received,c->media->video_matrix_selected,c->media->video_full_range);
}
static void loading_begin_cached(X4CatalogAssets *assets,const X4CatalogTitle *title,uint64_t now)
{
    x4_picture_settings_ui_cache_invalidate();x4_catalog_gui_keyboard_cache_invalidate();
    static X4CatalogAssetView view;
    memset(&view,0,sizeof(view));
    int acquired=assets&&title&&title->product_id[0]&&x4_catalog_assets_acquire(assets,title->product_id,&view);
    const X4CoverRaster *image=acquired&&view.surface[X4_COVER_DETAIL].rgba?&view.surface[X4_COVER_DETAIL]:NULL;
    bool ready=x4_loading_screen_begin(title?title->name:"XCloud4",image,now);
    if(acquired)x4_catalog_assets_release(assets,&view);
    if(!ready)printf("XCloud4 UI loading: cache allocation unavailable; existing session screen retained\n");
}

typedef struct {
    bool open, accepted;
    int page;
    char title[96];
} CloudLaunchNotice;

static bool cloud_launch_allowed(CloudLaunchNotice *notice, X4CloudTime time, int page, const char *id)
{
    if (notice->open) return false;
    if (time.source == X4_CLOUD_TIME_NONE || time.unlimited || time.remaining_s ||
        (notice->accepted && notice->page == page && !strcmp(notice->title, id))) return true;
    *notice = (CloudLaunchNotice){.open=true,.page=page};
    snprintf(notice->title,sizeof(notice->title),"%s",id);
#if !X4_PUBLIC
    printf("XCloud4: tiempo nube aviso=sin_tiempo\n");
#endif
    return false;
}
/* Main is the only display owner. Stop only video once, preserve the RTC
 * and audio owners, and let the explicit detached observation authorize UI
 * reopening. A native cleanup error may retain the media context. */
static void stream_halt_video_error(X4LiveMedia *live, X4LiveMediaSnapshot *status,
    int error, bool *handled)
{
    if (*handled) return;
    *handled = true;
    int halt_rc = x4_live_media_stream_halt_video(live);
    x4_live_media_snapshot(live, status);
    char message[256];
    (void)x4_stream_video_error_text(message, sizeof(message),
        status->video_rejected_width, status->video_rejected_height, error);
    printf("XCloud4: %s video_error=%d halt_rc=%d display_detached=%d; audio/RTC no cancelados\n",
        message, error, halt_rc, status->video_display_detached);
    if (!status->video_display_detached) {
#if X4_PUBLIC
        printf("XCloud4: VideoOut sigue retenido; no se puede dibujar el aviso ni reabrir UI con seguridad. PARA SALIR: BOTON PS > CERRAR APLICACION.\n");
#else
        printf("XCloud4: VideoOut sigue retenido; no se puede dibujar el aviso ni reabrir UI con seguridad. usa el botón PS para cerrar la aplicación.\n");
#endif
    }
}
#endif
static void main_submit_done(void)
{
    static bool gnm_idle_error_logged;
    int rc = sceGnmSubmitDone();
    if (rc < 0 && !gnm_idle_error_logged) {
        printf("XCloud4: Gnm submitDone idle result=0x%08x\n", (unsigned)rc);
        gnm_idle_error_logged = true;
    }
}
#endif

/* Decode the existing packaged icon before the single image worker starts.
 * No disk read/decoder allocation occurs in any screen draw. */
#if !X4_VIDEO_PROBE
static X4CoverRasters ui_app_icon;
static void load_ui_app_icon(void)
{
    if (ui_app_icon.surface[X4_COVER_GRID].rgba) return;
    FILE *f=fopen("/app0/assets/icon0.png","rb");
    if (!f) return;
    if (fseek(f,0,SEEK_END)) { fclose(f); return; }
    long size=ftell(f);
    if (size<=0 || size>2*1024*1024 || fseek(f,0,SEEK_SET)) { fclose(f); return; }
    unsigned char *bytes=malloc((size_t)size);
    if (bytes && fread(bytes,1,(size_t)size,f)==(size_t)size)
        (void)x4_cover_decode(bytes,(size_t)size,&ui_app_icon);
    free(bytes);fclose(f);
}

#endif

#if !X4_VIDEO_PROBE
static void add_art(X4CatalogArtDemand demand[X4_CATALOG_PRELOAD_PRODUCTS],size_t *count,
    const char *id,uint32_t mask)
{
    if(!mask||!x4_product_id_valid(id))return;
    for(size_t i=0;i<*count;++i){
        if(!strcmp(demand[i].id,id)){
            demand[i].mask|=mask;
            return;
        }
    }
    if(*count==X4_CATALOG_PRELOAD_PRODUCTS)return;
    X4CatalogArtDemand *entry=&demand[(*count)++];
    memcpy(entry->id,id,strlen(id)+1u);
    entry->mask=mask;
}
static void menu_art(X4CatalogArtDemand demand[X4_CATALOG_PRELOAD_PRODUCTS],size_t *count,
    const X4CatalogBrowser *browser,const X4CatalogSnapshot *catalog)
{
    const char *ids[2];
    uint32_t masks[2];
    const char *featured=NULL;
    if(catalog->state==X4_CATALOG_READY&&browser->featured_count&&
        browser->featured_sources[0]<catalog->count)
        featured=catalog->titles[browser->featured_sources[0]].product_id;
    size_t n=x4_screen_cover_demand(0,featured,ids,masks);
    for(size_t i=0;i<n;++i)add_art(demand,count,ids[i],masks[i]);
}
static void discover_art(X4CatalogArtDemand demand[X4_CATALOG_PRELOAD_PRODUCTS],size_t *count,
    const X4CatalogBrowser *browser,const X4CatalogSnapshot *catalog,unsigned first,
    bool initial,const char *background)
{
    if(catalog->state!=X4_CATALOG_READY||catalog->count>X4_CATALOG_MAX)return;
    add_art(demand,count,background,1u<<X4_COVER_BACKGROUND);
    for(unsigned r=first;r<first+2u&&r<browser->home_rows.count;++r){
        const X4HomeRow *row=&browser->home_rows.rows[r];
        unsigned start=initial?0:x4_catalog_browser_home_row_start(browser,r);
        for(unsigned i=start;i<start+X4_CATALOG_UI2_DISCOVER_VISIBLE&&i<row->count;++i)
            if(row->indices[i]<catalog->count)
                add_art(demand,count,catalog->titles[row->indices[i]].product_id,1u<<X4_COVER_ROW);
    }
}
static const char *discover_opening_background(const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog,unsigned row_index,unsigned column)
{
    unsigned source=0;
    if(row_index<browser->home_rows.count){
        const X4HomeRow *row=&browser->home_rows.rows[row_index];
        if(column<row->count)source=row->indices[column];
    }
    return source<catalog->count?catalog->titles[source].product_id:NULL;
}
#if !X4_PUBLIC
enum CoverScreen { COVER_SCREEN_NONE, COVER_SCREEN_MENU, COVER_SCREEN_DISCOVER, COVER_SCREEN_CATALOG };
static bool cover_screen_ready(X4CatalogAssets *assets,enum CoverScreen screen,uint64_t shown_at,
    const X4CatalogBrowser *browser,const X4CatalogSnapshot *catalog,const X4RecentGames *recent,
    X4CatalogArtReady *ready)
{
    X4CatalogArtDemand art[X4_CATALOG_PRELOAD_PRODUCTS]={0};
    size_t count=0;
    if(screen==COVER_SCREEN_MENU)menu_art(art,&count,browser,catalog);
    else if(catalog->state!=X4_CATALOG_READY)return false;
    else if(screen==COVER_SCREEN_DISCOVER){
        discover_art(art,&count,browser,catalog,browser->home_first_row,false,
            discover_opening_background(browser,catalog,browser->home_row,browser->discover_row_selected));
    }else if(screen==COVER_SCREEN_CATALOG){
        const X4CatalogTitle *title=x4_catalog_browser_selected_title(browser,catalog,recent);
        add_art(art,&count,title?title->product_id:NULL,1u<<X4_COVER_BACKGROUND);
        unsigned first=browser->selected/X4_CATALOG_TAB_PAGE_SIZE*X4_CATALOG_TAB_PAGE_SIZE;
        for(unsigned i=first;i<first+X4_CATALOG_TAB_PAGE_SIZE&&i<browser->filtered_count;++i){
            unsigned source=browser->indices[i];
            if(source<catalog->count)add_art(art,&count,catalog->titles[source].product_id,1u<<X4_COVER_ROW);
        }
    }else return false;
    return x4_catalog_assets_art_ready(assets,art,count,shown_at,ready)&&!ready->pending;
}
#endif
#endif
#if !X4_VIDEO_PROBE
typedef struct {
    X4GamePage page;
    X4GameDetails details;
    uint64_t detail_revision, redraw;
    unsigned source;
    int origin;
    X4GamePageSimilar similar[X4_GAME_SIMILAR_MAX];
    unsigned similar_count;
    bool similar_dirty;
    struct {
        X4GamePage page;
        unsigned source;
        bool free_in_store;
        char id[96], product[X4_PRODUCT_ID_CAP], name[128];
    } history[4];
    unsigned history_count;
    enum X4CatalogAssetState background_state;
    bool open, free_in_store, has_details;
    char id[96], product[X4_PRODUCT_ID_CAP], name[128];
} GamePageUi;

static void game_page_show(GamePageUi *ui, int origin, unsigned source,
    const X4CatalogTitle *title, const X4RecentGame *recent)
{
    x4_game_page_init(&ui->page);
    ui->origin = origin;
    ui->source = source;
    ui->background_state = X4_ASSET_PENDING;
    ui->open = true;
    ui->has_details = false;
    ui->similar_dirty = true;
    ui->detail_revision = UINT64_MAX;
    ++ui->redraw;
    ui->free_in_store = title && (title->is_free_in_store || title->f2p_only);
    snprintf(ui->id, sizeof(ui->id), "%s", title ? title->id : recent->id);
    snprintf(ui->product, sizeof(ui->product), "%s", title ? title->product_id : recent->product_id);
    snprintf(ui->name, sizeof(ui->name), "%s", title ? title->name : recent->name);
}

static void game_page_open(GamePageUi *ui, int origin, unsigned source,
    const X4CatalogTitle *title, const X4RecentGame *recent)
{
    ui->history_count = 0;
    game_page_show(ui, origin, source, title, recent);
}

static void game_page_follow(GamePageUi *ui, unsigned source, const X4CatalogTitle *title)
{
    if (ui->history_count == sizeof(ui->history) / sizeof(*ui->history)) {
        --ui->history_count;
        memmove(ui->history, ui->history + 1, ui->history_count * sizeof(*ui->history));
    }
    unsigned slot = ui->history_count++;
    ui->history[slot].page = ui->page;
    ui->history[slot].source = ui->source;
    ui->history[slot].free_in_store = ui->free_in_store;
    memcpy(ui->history[slot].id, ui->id, sizeof(ui->id));
    memcpy(ui->history[slot].product, ui->product, sizeof(ui->product));
    memcpy(ui->history[slot].name, ui->name, sizeof(ui->name));
    game_page_show(ui, ui->origin, source, title, NULL);
    x4_game_page_cache_invalidate();
}

static bool game_page_parent(GamePageUi *ui)
{
    if (!ui->history_count)
        return false;
    unsigned slot = --ui->history_count;
    ui->page = ui->history[slot].page;
    ui->source = ui->history[slot].source;
    ui->free_in_store = ui->history[slot].free_in_store;
    memcpy(ui->id, ui->history[slot].id, sizeof(ui->id));
    memcpy(ui->product, ui->history[slot].product, sizeof(ui->product));
    memcpy(ui->name, ui->history[slot].name, sizeof(ui->name));
    ui->background_state = X4_ASSET_PENDING;
    ui->detail_revision = UINT64_MAX;
    ui->has_details = false;
    ui->similar_dirty = true;
    x4_game_page_cache_invalidate();
    return true;
}

static void game_page_close(GamePageUi *ui, X4CatalogAssets *assets)
{
    ui->open = false;
    ui->history_count = 0;
    x4_game_page_cache_dispose();
    ++ui->redraw;
    x4_catalog_assets_game_request(assets, NULL, -1);
}

static const X4CatalogTitle *game_page_selected(const GamePageUi *ui,
    const X4CatalogSnapshot *catalog)
{
    if (!ui->open || catalog->state != X4_CATALOG_READY || ui->source >= catalog->count)
        return NULL;
    const X4CatalogTitle *title = &catalog->titles[ui->source];
    return !strcmp(title->id, ui->id) && !strcmp(title->product_id, ui->product) ? title : NULL;
}

static void game_page_rebind(GamePageUi *ui, const X4CatalogSnapshot *catalog)
{
    if (!ui->open || catalog->state != X4_CATALOG_READY || game_page_selected(ui,catalog))
        return;
    ui->source = X4_CATALOG_MAX;
    for (unsigned i = 0; i < catalog->count; ++i) {
        if (ui->product[0] && !strcmp(catalog->titles[i].product_id, ui->product) &&
            (!ui->id[0] || !strcmp(catalog->titles[i].id, ui->id))) {
            ui->source = i;
            snprintf(ui->id, sizeof(ui->id), "%s", catalog->titles[i].id);
            ui->similar_dirty = true;
            break;
        }
    }
}

static void game_page_rebuild_similar(GamePageUi *ui, const X4CatalogBrowser *browser,
    const X4CatalogSnapshot *catalog)
{
    unsigned sources[X4_GAME_SIMILAR_MAX];
    ui->similar_count = x4_catalog_browser_ui2_similar(browser, catalog, ui->source, sources);
    for (unsigned i = 0; i < ui->similar_count; ++i)
        ui->similar[i] = (X4GamePageSimilar){
            .source_index = sources[i], .title = catalog->titles[sources[i]].name,
        };
    ui->similar_dirty = false;
}

static X4GamePageModel game_page_model(const GamePageUi *ui, const X4CatalogSnapshot *catalog,
    const X4Favorites *favorites, bool busy)
{
    const X4CatalogTitle *title = game_page_selected(ui, catalog);
    return (X4GamePageModel){
        .product_id=ui->product, .title=title ? title->name : ui->name,
        .available=title != NULL, .entitled=title && (title->entitled || title->f2p_only),
        .free_in_store=x4_ui2_fortnite_is_product(ui->product) ||
            (title ? title->is_free_in_store || title->f2p_only : ui->free_in_store),
        .busy=busy, .details=ui->has_details ? &ui->details : NULL,
        .favorite=x4_favorites_contains(favorites, ui->product),
        .similar=ui->similar, .similar_count=ui->similar_count,
    };
}

static void ui_toggle_favorite(X4Favorites *favorites, X4CatalogBrowser *browser,
    const char *product, const char *title, char *notice, size_t notice_size)
{
    int result = x4_favorites_toggle(favorites, product, title, X4_FAVORITES_PATH);
    x4_catalog_browser_ui2_favorites(browser, favorites);
    if (result == X4_FAVORITES_FULL || result == X4_FAVORITES_ERROR)
        snprintf(notice, notice_size, "%s", result == X4_FAVORITES_FULL ?
            x4_tr(X4_TR_50_FAVORITES_FULL) :
            x4_tr(X4_TR_50_FAVORITES_SAVE_FAILED));
    else notice[0] = 0;
}

typedef struct {
    char product[X4_PRODUCT_ID_CAP];
    uint64_t started_us;
    bool active, has_image;
} UiLocalSession;

static void ui_history_skip(const char *reason, int error)
{
#if !X4_PUBLIC
    printf("XCloud4: historial omitido motivo=%s errno=%d\n", reason, error);
#else
    (void)reason;
    (void)error;
#endif
}

static void ui_history_record(X4History *history, const char *product, uint64_t seconds)
{
#if !X4_PUBLIC
    const uint64_t start = sceKernelGetProcessTime();
#endif
    const bool saved = x4_history_record(history, product, x4_history_now(), seconds, X4_HISTORY_PATH);
    if (!saved) {
        const int error = errno;
        ui_history_skip("escritura", error);
        return;
    }
#if !X4_PUBLIC
    const uint64_t us = sceKernelGetProcessTime() - start;
    printf("XCloud4: historial escritura=%llu.%03llu ms guardado=1\n",
        (unsigned long long)(us / 1000), (unsigned long long)(us % 1000));
#endif
}

static void ui_history_begin(UiLocalSession *local, const char *product)
{
    *local = (UiLocalSession){.active = true};
    if (x4_product_id_valid(product))
        snprintf(local->product, sizeof(local->product), "%s", product);
}

static void ui_history_first_image(X4History *history, UiLocalSession *local, uint64_t now_us)
{
    if (!local->active || local->has_image) return;
    local->has_image = true;
    local->started_us = now_us;
    if (local->product[0]) ui_history_record(history, local->product, 0);
}

static void ui_history_finish(X4History *history, UiLocalSession *local, uint64_t now_us)
{
    if (!local->active) return;
    if (!local->product[0]) ui_history_skip("sin_producto", 0);
    else if (!local->has_image) ui_history_skip("sin_imagen", 0);
    else {
        const uint64_t seconds = now_us >= local->started_us ?
            (now_us - local->started_us) / 1000000u : 0;
        ui_history_record(history, local->product, seconds);
    }
    *local = (UiLocalSession){0};
}

static const char *ui_status_quality(X4StreamRequestProfile profile)
{
    switch (profile) {
    case X4_REQUEST_1080P_G: return x4_tr(X4_TR_50_QUALITY_1080P_HIGH);
    case X4_REQUEST_1080P_HQ: return x4_tr(X4_TR_50_QUALITY_1080P_MAX);
    case X4_REQUEST_1080P_H:
    case X4_REQUEST_1080P_G8: return "1080p";
    default: return "720p";
    }
}

static unsigned game_page_input(uint32_t pressed)
{
    unsigned actions = 0;
    if (pressed & ORBIS_PAD_BUTTON_UP) actions |= X4_GAME_INPUT_UP;
    if (pressed & ORBIS_PAD_BUTTON_DOWN) actions |= X4_GAME_INPUT_DOWN;
    if (pressed & ORBIS_PAD_BUTTON_LEFT) actions |= X4_GAME_INPUT_LEFT;
    if (pressed & ORBIS_PAD_BUTTON_RIGHT) actions |= X4_GAME_INPUT_RIGHT;
    if (pressed & ORBIS_PAD_BUTTON_CROSS) actions |= X4_GAME_INPUT_SELECT;
    if (pressed & ORBIS_PAD_BUTTON_CIRCLE) actions |= X4_GAME_INPUT_BACK;
    if (pressed & ORBIS_PAD_BUTTON_L1) actions |= X4_GAME_INPUT_PREVIOUS_TAB;
    if (pressed & ORBIS_PAD_BUTTON_R1) actions |= X4_GAME_INPUT_NEXT_TAB;
    return actions;
}

static size_t game_page_draw(GamePageUi *ui, X4GamePageModel model,
    const X4CatalogSnapshot *catalog, X4CatalogAssets *assets, uint32_t *pixels, bool persistent,
    X4UiRect damage[X4_GAME_PAGE_DAMAGE_MAX])
{
    X4CatalogBackgroundView bg = {0};
    X4CatalogAssetView cover = {0};
    bool got_cover = !model.details && x4_catalog_assets_acquire(assets,ui->product,&cover) > 0;
    if (got_cover) {
        model.category = cover.metadata.category;
        model.studio = cover.metadata.developer[0] ? cover.metadata.developer : cover.metadata.publisher;
        model.description = cover.metadata.description;
    }
    X4GameScreenshotView shots[X4_GAME_SCREENSHOT_MAX] = {0}, full = {0};
    bool got[X4_GAME_SCREENSHOT_MAX] = {0};
    bool background = x4_catalog_assets_acquire_background(assets, &bg) > 0;
    if (background && !strcmp(bg.product_id, ui->product)) model.background = &bg.raster;
    unsigned count = model.details ? model.details->screenshot_count : 0;
    if (count > X4_GAME_SCREENSHOT_MAX) count = X4_GAME_SCREENSHOT_MAX;
    int full_index = x4_game_page_fullscreen_index(&ui->page);
    bool got_full = full_index >= 0 && (unsigned)full_index < count &&
        x4_catalog_assets_game_acquire(assets, ui->product, (unsigned)full_index, true, &full) > 0;
    if (got_full) model.fullscreen = &full.raster;
    for (unsigned i = 0; i < count; ++i) {
        got[i] = x4_catalog_assets_game_acquire(assets, ui->product, i, false, &shots[i]) > 0;
        if (got[i]) model.screenshots[i] = &shots[i].raster;
    }
    X4GamePageSimilar similar[X4_GAME_SIMILAR_MAX];
    static X4CatalogAssetView similar_covers[X4_GAME_SIMILAR_MAX];
    bool got_similar[X4_GAME_SIMILAR_MAX] = {0};
    if (ui->page.tab == X4_GAME_TAB_SIMILAR) {
        for (unsigned i = 0; i < model.similar_count; ++i) {
            similar[i] = model.similar[i];
            const char *id = catalog->titles[similar[i].source_index].product_id;
            got_similar[i] = x4_catalog_assets_acquire(assets, id, &similar_covers[i]) > 0;
            if (got_similar[i] && similar_covers[i].surface[X4_COVER_ROW].rgba)
                similar[i].image = &similar_covers[i].surface[X4_COVER_ROW];
        }
        model.similar = similar;
    }
    const size_t count_damage = x4_game_page_draw_cached(&ui->page, &model,
        model.background ? bg.generation : 0, pixels, persistent, damage);
    for (unsigned i = 0; i < model.similar_count; ++i)
        if (got_similar[i]) x4_catalog_assets_release(assets, &similar_covers[i]);
    for (unsigned i = 0; i < count; ++i)
        if (got[i]) x4_catalog_assets_game_release(assets, &shots[i]);
    if (got_full) x4_catalog_assets_game_release(assets, &full);
    if (background) x4_catalog_assets_release_background(assets, &bg);
    if (got_cover) x4_catalog_assets_release(assets, &cover);
    return count_damage;
}

static X4GamepadRoster gamepad_snapshot(const X4ControllerRosterView *view,
    const X4GamepadFrame *p1, bool multiplayer, bool game_input, uint64_t sampled_us)
{
    X4GamepadRoster roster={.multiplayer=multiplayer};
    roster.pads[0]=*p1;
    if(view->pad[0].data.connected)roster.connected_mask=1u;
    roster.changed_us[0]=view->changed_us[0];
    roster.generation[0]=view->generation[0];
    if(multiplayer)for(unsigned i=1;i<X4_GAMEPAD_COUNT;++i){
        if(view->pad[i].data.connected)roster.connected_mask|=(uint8_t)(1u<<i);
        if(game_input)x4_controller_gamepad(&view->pad[i],&roster.pads[i]);
        roster.pads[i].sampled_us=sampled_us;
        roster.changed_us[i]=view->changed_us[i];
        roster.generation[i]=view->generation[i];
    }
    return roster;
}
#endif

#if !X4_VIDEO_PROBE
static X4Ui2Section ui2_section_for(const X4Screen *screen, const X4CatalogBrowser *browser)
{
    switch (screen->page) {
    case 2: return X4_UI2_ABOUT;
    case 7: return X4_UI2_SUPPORT;
    case 5:
        if (browser->active_tab == X4_CAT_TAB_SEARCH) return X4_UI2_SEARCH;
        if (browser->active_tab == X4_CAT_TAB_DISCOVER) return X4_UI2_DISCOVER;
        return browser->active_tab == X4_CAT_TAB_RECENTS ? X4_UI2_LIBRARY : X4_UI2_CATALOG;
    default: return X4_UI2_HOME;
    }
}

typedef struct {
    bool called;
#if !X4_PUBLIC
    UiBootDiag *diag;
#endif
} Ui2Splash;

static void ui2_splash_presented(Ui2Splash *splash)
{
    if (splash->called) return;
    splash->called = true;
    const int32_t code = sceSystemServiceHideSplashScreen();
#if !X4_PUBLIC
    splash->diag->splash_called = true;
    splash->diag->splash_code = code;
    if (code >= 0) ui_boot_event(splash->diag, BOOT_SPLASH_HIDDEN, sceKernelGetProcessTime());
#else
    (void)code;
#endif
}

static void ui2_boot_present(X4Display *display, enum X4Ui2BootStep step, Ui2Splash *splash)
{
    uint64_t begin = sceKernelGetProcessTime();
    uint32_t *pixels = x4_display_pixels(display);
    if (!x4_ui2_boot_draw(pixels, step)) return;
    X4UiRect damage;
    if (x4_ui2_boot_damage(&damage) != SIZE_MAX)
        x4_display_set_damage(display, &damage, 1);
    uint64_t drawn = sceKernelGetProcessTime();
    if (x4_display_present(display) < 0) return;
    uint64_t ended = sceKernelGetProcessTime();
    ui2_splash_presented(splash);
    X4UiCopyStats copied;
    x4_display_copy_stats(display, &copied);
    x4_ui_draw_metrics_record(X4_UI_BOOT, false, true, drawn-begin, copied.elapsed_us,
        ended-drawn, copied.bytes, ended, 0);
}

#endif

int main(void)
{
#if X4_VIDEO_PROBE
    /* Local source only: enter before any auth/catalog/network allocation. */
#if X4_PROBE_T27
    return x4_t27_controller_run();
#else
    return x4_probe_controller_run();
#endif
#else
#if !X4_PUBLIC
    UiBootDiag boot_diag = {.started_us = sceKernelGetProcessTime()};
    Ui2Splash splash = {.diag = &boot_diag};
#else
    Ui2Splash splash = {0};
#endif
    X4Display display={.handle=-1};
    X4Controller controller={0},primary={0};
    X4ControllerRoster *pads=NULL;
    X4ControllerRosterView pad_view={0};
    X4GamepadMatchOption pad_option={0};
    unsigned menu_pad=0;
    bool menu_pad_rearm=false;
#if X4_STREAM_PROFILE
    uint8_t last_match_pads=0;
    uint64_t pad_notice_until=0;
    int pad_notice_serial=0;
    char pad_notice[64]={0};
#endif
    X4Screen screen = {0};
    X4Ui2Shell ui2_shell = {0};
    X4Ui2Status ui2_status = {0};
    X4Ui2Shell shown_shell = {0};
    uint64_t status_revision = 0, shown_status_revision = 0;
    X4Ui2Account account_ui = {0};
    X4Ui2AccountSummary account_history = {0};
    uint64_t account_history_revision = UINT64_MAX;
    int account_return_page = 0;
    bool account_return_settings = false;
    int about_return_page = 0;
    bool about_return_settings = false;
    bool logout_notice = false;
    bool logout_confirm_focused = false;
    X4Ui2Shell account_return_shell = {0};
    X4Ui2Shell about_return_shell = {0};
    X4Ui2Shell settings_return_shell = {0};
    bool settings_return_valid = false;
    bool modal_latched = false;
    int modal_underlay = 0;
    bool account_canvas_valid = false;
    bool settings_canvas_valid = false;
    bool shell_presented = false;
    int home_choice = X4_UI2_HOME_CONTINUE;
    enum X4CatalogTab login_tab = X4_CAT_TAB_DISCOVER;
    X4DemoVideo video = {0};
    X4DemoAudio audio = {.handle = -1};
    X4Auth *auth = x4_auth_create();
    X4AuthSnapshot account = {0};
    x4_auth_snapshot(auth, &account);
    /* At creation, only restoration can already be connecting or authorized. */
    bool home_boot_catalog_pending = account.state == X4_AUTH_CONNECTING ||
        account.state == X4_AUTH_AUTHORIZED;
#if !X4_PUBLIC
    const bool boot_restore_started = home_boot_catalog_pending;
#endif
    X4AuthRememberSnapshot remembered = {0};
    bool remember_notice = false;
    static X4CatalogSnapshot catalog;
    static X4CatalogBrowser catalog_browser;
    X4AuthCatalogCursor catalog_cursor = {0};
    X4AuthRecentCursor recent_cursor = {0};
    X4RecentSnapshot recent_snapshot = {0};
    X4RecentGames recent;
    static X4CatalogPresentation presentation;
    static X4SiglList home_sigls[X4_SIGL_COUNT];
    uint64_t home_sigl_revision = 0, home_sigl_epoch = 0;
    bool home_canvas_valid = false, menu_canvas_valid = false;
    bool home_background_requested = false;
    bool cover_preload_active = false;
    enum X4CatalogTab background_request_tab = X4_CAT_TAB_COUNT;
#if !X4_PUBLIC
    bool home_first_frame_pending = true;
    bool menu_first_frame_pending = true;
    enum CoverScreen cover_screen = COVER_SCREEN_NONE;
    uint64_t cover_screen_shown = 0;
    bool cover_screen_reported = false;
    enum { TAB_H, TAB_V, TAB_BACKGROUND, TAB_SWITCH, TAB_TIMINGS };
    static const char *const tab_log_names[X4_CAT_TAB_COUNT] = {
        [X4_CAT_TAB_DISCOVER] = "descubrir",
        [X4_CAT_TAB_CATALOG] = "catalogo",
        [X4_CAT_TAB_RECENTS] = "recientes",
        [X4_CAT_TAB_SEARCH] = "buscar"
    };
    struct {
        uint64_t since_us;
        uint64_t count[TAB_TIMINGS], max_us[TAB_TIMINGS];
        uint64_t bytes_max;
        size_t thumbnail_peak_bytes, background_peak_bytes;
    } home_log[X4_CAT_TAB_COUNT] = {0};
#endif
    X4CatalogAssets *catalog_assets = NULL;
    uint64_t ui_generation = 1, metadata_revision = 0;
    int pending_auth = -1, pending_launch = 0;
    uint64_t pending_generation = 0;
    static GamePageUi game_page_ui;
    static X4Favorites favorites;
    static X4History play_history;
    UiLocalSession local_session = {0};
    int fortnite_return_page = 0, fortnite_return_selection = 0;
    X4Ui2Fortnite fortnite = {0};
    uint64_t game_scroll_after = 0;
    char ui_notice[160] = {0};
    X4SessionSnapshot session = {0};
    X4CloudTimeState cloud_clock = {0};
    X4CloudTime cloud_time = {0};
    CloudLaunchNotice cloud_launch = {0};
    uint64_t cloud_account = 0, cloud_revision = 0, cloud_alert_until = 0;
    unsigned cloud_alert = 0, cloud_alert_queue = 0;
    bool cloud_session = false, cloud_force = false;
    unsigned catalog_selected = 0;
    int session_back = 0;
    bool is_fortnite_session = false;
    bool fortnite_login_flow = false;
    X4AccessCountryUi access_card_ui = {0};
    bool access_card_visible = false, access_query = false;
    bool access_query_started = false, access_query_cancelled = false;
    int access_query_page = 0;
    uint64_t access_notice_until = 0;
    X4UpdateNotice update_notice = {0};
#ifdef X4_UPDATE51_ENABLED
    bool update_progress_open = false, update_manual = false, update_rejected = false;
#if !X4_PUBLIC
    uint64_t update_close_at = 0;
#endif
    X4Update51View update_view = {0};
#endif
#if X4_STREAM_PROFILE && !X4_PUBLIC
    bool update_test_requested = false;
#endif
    int game_controls = 0;
    int media_active = 0;
    int closing = 0;
    X4LiveMedia *live = NULL;
    X4LiveMediaSnapshot live_status = {0};
    int live_error = 0, live_muted = 0;
#if X4_STREAM_PROFILE
    bool video_start_error = false;
#endif
    int live_retained = 0;
    bool live_presented = false;
#if X4_STREAM_PROFILE
    bool stream_display_active=false;
    X4StreamSettingsUi stream_settings_ui={0};
#if !X4_PUBLIC
    uint64_t stream_received_until=0;
    /* Restore the Lab R3 conversion choice before any session worker exists. */
    (void)x4_stream_settings_conversion_init();
#endif
#endif
    x4_server_settings_init();
    x4_picture_settings_init();
    X4PictureSettingsUi picture_settings_ui={0};
    bool diagnostic_stable = false;
    uint64_t diagnostic_mark = 0;
    X4TouchMenu touch_menu;
    x4_touch_menu_init(&touch_menu, X4_PUBLIC != 0);
    X4MicStatus previous_mic_status=X4_MIC_OFF;
    uint64_t mic_notice_until=0;
#if !X4_PUBLIC
    bool mic_meter_requested=false;
#endif
    uint64_t video_banner_until_us = 0;
    int overlay_key[11] = {0};
#if X4_STREAM_PROFILE
    int reconnect_notice_key[9]={0};
#endif
    uint64_t overlay_at = 0;
    uint64_t input_previous = 0, input_report_at = 0;
#if !X4_PUBLIC
    uint64_t script_previous_us = 0;
#endif
    uint64_t input_intervals = 0, input_interval_us = 0, input_interval_max_us = 0;
    uint64_t idle_present_skips = 0;
    uint32_t ui_state_sig = 0;
    unsigned ui_dirty_buffers = 2;
    uint64_t ui_dirty_skips = 0;
    enum X4UiScreen ui_previous_metric=X4_UI_OTHER;
    bool ui_previous_navigation=false;
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    InteractionRumble rumble_state={.handle=-1},rumble_others[3]={0};
#else
    OrbisPadVibeParam rumble_applied={0};
    int rumble_handle=-1;
    uint64_t rumble_pad_calls=0,rumble_pad_ok=0,rumble_pad_errors=0;
#endif
    bool rumble_was_connected=false;
#endif
    x4_catalog_browser_init(&catalog_browser);
    x4_catalog_browser_ui2_select(&catalog_browser, X4_CAT_TAB_DISCOVER);
    x4_favorites_load(&favorites, X4_FAVORITES_PATH);
    (void)x4_history_load(&play_history, X4_HISTORY_PATH);
    x4_ui2_home_history(&play_history);
    x4_apple_access_card_use_overlay(true);
    x4_catalog_browser_ui2_favorites(&catalog_browser, &favorites);
    x4_recent_init(&recent);
    x4_catalog_presentation_init(&presentation);
    x4_catalog_presentation_set_generation(&presentation, ui_generation);
    setvbuf(stdout, NULL, _IONBF, 0);
    (void)x4_lang_init();
    x4_ui_lang_pref_init(NULL);
#if !X4_PUBLIC
    ui_boot_event(&boot_diag, BOOT_LANGUAGE, sceKernelGetProcessTime());
#endif
    uint64_t language_revision = x4_lang_ui_revision();
#ifdef X4_UPDATE51_ENABLED
    x4_update51_boot_cleanup();
#endif
    x4_release_check_start();
reopen_interface:;
    x4_ui_lang_pref_start();
    modal_latched = account_canvas_valid = settings_canvas_valid = false;
    int rc = x4_display_open(&display);
    if (rc < 0) {
        printf("XCloud4: no se pudo iniciar VideoOut: 0x%08x\n", (unsigned)rc);
#if !X4_PUBLIC
        (void)ui_boot_diag_save(&boot_diag, sceKernelGetProcessTime(), ui_dirty_skips, true);
#endif
        sceKernelUsleep(3000000);
        /* Never enter the known failing libc _exit path on this firmware. */
        if (x4_exit_prepare() >= 0) x4_exit_request();
        for (;;) sceKernelUsleep(100000);
    }
    x4_ui_draw_metrics_reset();
    shell_presented = false;
    x4_ui2_boot_reset();
    x4_ui2_home_invalidate();
    x4_ui2_status_update(&ui2_status, X4_UI2_NETWORK_UNKNOWN, 0, 0, -1, NULL);
    ui2_boot_present(&display, X4_UI2_BOOT_FONTS, &splash);
    if (x4_font_init("/app0/assets/fonts/inter.x4font") < 0 &&
        x4_font_init("/app0/assets/fonts/noto_sans.x4font") < 0)
        printf("XCloud4: fuente externa no disponible; respaldo suavizado activo\n");
    ui2_boot_present(&display, X4_UI2_BOOT_CACHE, &splash);
    load_ui_app_icon();
    catalog_assets = x4_catalog_assets_create();
    x4_catalog_assets_set_generation(catalog_assets, ui_generation);
    ui2_boot_present(&display, X4_UI2_BOOT_ACCOUNT, &splash);
    x4_auth_snapshot(auth, &account);
    x4_picture_settings_ui_cache_invalidate();
    x4_catalog_gui_keyboard_cache_invalidate();
    x4_catalog_gui_home_invalidate();
    x4_screen_home_invalidate();
    home_canvas_valid = menu_canvas_valid = false;
    home_sigl_revision = 0;
    memset(home_sigls, 0, sizeof(home_sigls));
#if !X4_PUBLIC
    home_first_frame_pending = true;
    menu_first_frame_pending = true;
    cover_screen = COVER_SCREEN_NONE;
    cover_screen_shown = 0;
    cover_screen_reported = false;
    uint64_t home_warm_begin = sceKernelGetProcessTime();
#endif
    /* Prepare the local catalog canvas before the interactive first frame.
     * Catalog and Recents keep their existing asynchronous loading paths. */
    ui2_boot_present(&display, X4_UI2_BOOT_CATALOG, &splash);
    x4_catalog_gui_home_prewarm(x4_display_pixels(&display));
#if !X4_PUBLIC
    printf("XCloud4: descubrir fuente precalentada us=%llu\n",
        (unsigned long long)(sceKernelGetProcessTime() - home_warm_begin));
#endif
    memset(&pad_view,0,sizeof(pad_view));
    for(unsigned i=0;i<X4_GAMEPAD_COUNT;++i)pad_view.pad[i].handle=-1;
    pad_option=(X4GamepadMatchOption){0};
    menu_pad=0;menu_pad_rearm=false;
    pads=x4_controller_roster_start(x4_picture_settings_multiplayer());
    if(!pads)x4_controller_init(&primary);
#if X4_STREAM_PROFILE
    last_match_pads=0;pad_notice_until=0;pad_notice_serial=0;
#endif
#if X4_STREAM_PROFILE
    stream_display_active=false;
    if(!live){live_retained=0;live_error=0;
        video_start_error=false;
    }
#endif
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    rumble_state=(InteractionRumble){.handle=-1,.roster=pads};rumble_was_connected=false;
    for(unsigned i=1;i<X4_GAMEPAD_COUNT;++i)
        rumble_others[i-1]=(InteractionRumble){.handle=-1,.roster=pads,.index=i};
#else
    rumble_applied=(OrbisPadVibeParam){0}; rumble_handle=-1; rumble_was_connected=false;
#endif
#endif
#ifdef X4_INGRESS_BASELINE
    printf("XCloud4 %s BASE: solo para referencia, no es la prueba autorizada\n",X4_PRODUCT_VERSION);
#else
    printf("XCloud4 %s: progreso independiente, base MPSC\n", X4_PRODUCT_VERSION);
#endif
    printf("XCloud4: %s\n", X4_AUTH_PROFILE_NOTE);
    printf("XCloud4: diagnostic build=%s\n", X4_BUILD_ID);
#if !X4_PUBLIC
    x4_script_engine_init();
    script_previous_us = 0;
    bool warning_acc = false;
    char saved_s[256] = {0};
    if (x4_script_engine_load_persistence(&warning_acc, saved_s, sizeof(saved_s))) {
        if (saved_s[0] != '\0') {
            x4_script_engine_load(saved_s);
            if (x4_script_engine_needs_gyro()) {
                if(pads)x4_controller_roster_set_motion_sensor(pads,true);
                else x4_controller_set_motion_sensor(&primary,true);
            }
        }
    }
#endif
    ui2_boot_present(&display, X4_UI2_BOOT_READY, &splash);
    for (unsigned frame = 0;; ++frame) {
#if X4_STREAM_PROFILE
        bool stream_settings_consumed=false;
#endif
        uint64_t input_at = sceKernelGetProcessTime();
#if !X4_PUBLIC
        (void)ui_boot_diag_save(&boot_diag, input_at, ui_dirty_skips, false);
        for (unsigned tab = 0; tab < X4_CAT_TAB_COUNT; ++tab) {
            if (!home_log[tab].since_us || input_at - home_log[tab].since_us < 5000000u)
                continue;
            printf("XCloud4: descubrir resumen pestana=%s h=%llu h_max_us=%llu v=%llu v_max_us=%llu "
                "fondo=%llu fondo_max_us=%llu cambio_tab=%llu cambio_tab_max_us=%llu bytes_max=%llu "
                "thumbnail_peak_bytes=%zu background_peak_bytes=%zu\n", tab_log_names[tab],
                (unsigned long long)home_log[tab].count[TAB_H], (unsigned long long)home_log[tab].max_us[TAB_H],
                (unsigned long long)home_log[tab].count[TAB_V], (unsigned long long)home_log[tab].max_us[TAB_V],
                (unsigned long long)home_log[tab].count[TAB_BACKGROUND],
                (unsigned long long)home_log[tab].max_us[TAB_BACKGROUND],
                (unsigned long long)home_log[tab].count[TAB_SWITCH], (unsigned long long)home_log[tab].max_us[TAB_SWITCH],
                (unsigned long long)home_log[tab].bytes_max, home_log[tab].thumbnail_peak_bytes,
                home_log[tab].background_peak_bytes);
            memset(&home_log[tab], 0, sizeof(home_log[tab]));
        }
#endif
        x4_trace_monitor_state(live ? x4_live_media_trace(live) : NULL,
            X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAIN_POLL, input_at);
#if X4_STREAM_PROFILE
        uint64_t controller_read_at=sceKernelGetProcessTime();
        if(live&&screen.page==6&&x4_auth_busy(auth))interaction_main_read(controller_read_at);
#endif
        bool pad_match=screen.page==6&&x4_auth_busy(auth);
        bool several=x4_gamepad_match_option(&pad_option,x4_picture_settings_multiplayer(),pad_match);
        if(pads){
            x4_controller_roster_configure(pads,several,pad_match);
            x4_controller_roster_read(pads,&pad_view);
            primary=pad_view.pad[0];
        }else{
            x4_controller_read(&primary,frame);
            pad_view.pad[0]=primary;
            pad_view.assigned[0]=primary.handle>=0;
        }
        controller=primary;
#if X4_STREAM_PROFILE
        if(live&&screen.page==6&&x4_auth_busy(auth))
            x4_live_media_stream_note_controller_read(live,controller_read_at);
#endif
        /* Local pad sampling cadence, not Xbox RTT or end-to-end latency. */
        if (screen.page == 6 && x4_auth_busy(auth)) {
            if (input_previous) {
                uint64_t elapsed = input_at - input_previous;
                ++input_intervals;
                input_interval_us += elapsed;
                if (elapsed > input_interval_max_us) input_interval_max_us = elapsed;
            }
            input_previous = input_at;
            if (!input_report_at) input_report_at = input_at;
            if (input_at - input_report_at >= 5000000) {
                printf("XCloud4 input: samples=%llu mean_us=%llu max_us=%llu idle_flips_skipped=%llu\n",
                    (unsigned long long)input_intervals,
                    (unsigned long long)(input_intervals ? input_interval_us / input_intervals : 0),
                    (unsigned long long)input_interval_max_us,
                    (unsigned long long)idle_present_skips);
#if !X4_PUBLIC
                x4_script_engine_log_metrics_5s(input_at);
                x4_controller_log_motion_5s(&controller, input_at);
#endif
                input_report_at = input_at;
                input_intervals = input_interval_us = input_interval_max_us = idle_present_skips = 0;
            }
        } else {
            input_previous = input_report_at = 0;
            input_intervals = input_interval_us = input_interval_max_us = idle_present_skips = 0;
        }
        int previous_page = screen.page;
        bool game_input_owned = false, game_play = false, reopen_catalog = false;
        uint32_t game_page_pressed = 0;
        if (touch_menu.mic_enabled && (screen.page!=6 || !x4_auth_busy(auth) ||
            closing || session_back || live_error || !trace_session_active(session.state)))
            mic_end_match(&touch_menu);
        /* Keep Xbox button ownership across transient RTC disconnects. A
         * momentary status change must never turn B/Menu into local exit. */
        if (!x4_auth_busy(auth) || screen.page != 6) game_controls = 0;
        else if (session.rtc_connected) game_controls = 1;
#if X4_STREAM_PROFILE
        /* Loading owns Cancel until its transition hands over the display,
         * even if RTC already connected. The in-game pad protocol is unchanged. */
        X4LoadingView loading_input;x4_loading_screen_snapshot(&loading_input);
        if(screen.page==6&&loading_input.active&&
            !session_back&&!closing&&(controller.pressed&ORBIS_PAD_BUTTON_CIRCLE)){
            mic_end_match(&touch_menu);
            trace_inactive(live);x4_auth_cancel(auth);session_back=1;
        }
#endif
        bool game_input = screen.page == 6 && game_controls && x4_auth_busy(auth) &&
            !closing && !session_back && !live_error;
#if !X4_PUBLIC
        if (!game_input) script_previous_us = 0;
#endif
#if X4_PUBLIC
        (void)diagnostic_stable;
        (void)diagnostic_mark;
#endif
        if (game_input && (!several || controller.data.connected)) {
            x4_touch_menu_feed_touch(&touch_menu, game_input, controller.data.touch.fingers, controller_read_at);
        } else {
            x4_touch_menu_close(&touch_menu);
        }
        X4GamepadFrame gamepad = {0};
        if (game_input && (!several || controller.data.connected)) {
            if (x4_touch_menu_is_open(&touch_menu)) {
                gamepad.connected = controller.data.connected;
            } else {
                X4Controller filtered = controller;
                filtered.data.buttons = x4_touch_menu_filter_buttons(&touch_menu, controller.data.buttons);
                const uint32_t hud_chord=ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3;
                /* Solo P1 can use the local chord. Keep the T37 mapping when
                 * there is no other physical controller. */
                bool extra_pad=pad_option.multiplayer&&(pad_view.pad[1].data.connected||
                    pad_view.pad[2].data.connected||pad_view.pad[3].data.connected);
                if(extra_pad&&(controller.data.buttons&hud_chord)==hud_chord){
                    if(controller.pressed&hud_chord)(void)x4_net_hud_cycle_mode();
                    filtered.data.buttons&=~hud_chord;
                }
                x4_controller_gamepad(&filtered, &gamepad);
                if (x4_touch_menu_is_nexus_active(&touch_menu, controller_read_at)) {
                    gamepad.buttons |= X4_GAMEPAD_NEXUS;
                }
#if !X4_PUBLIC
                if (x4_script_engine_is_active()) {
                    uint16_t saved_nexus = gamepad.buttons & X4_GAMEPAD_NEXUS;
                    const uint64_t script_at = sceKernelGetProcessTime();
                    float dt = (script_previous_us && script_at > script_previous_us) ?
                        (float)(script_at - script_previous_us) / 1000000.0f : 0.0f;
                    if (dt > 0.1f) dt = 0.1f;
                    script_previous_us = script_at;
                    x4_script_engine_process_input(&filtered, &gamepad, dt, controller_read_at);
                    gamepad.buttons |= saved_nexus;
                }
#endif
            }
        }
#if X4_STREAM_PROFILE
        gamepad.sampled_us=controller_read_at;
#endif
        X4GamepadRoster gamepads=gamepad_snapshot(&pad_view,&gamepad,several,game_input,controller_read_at);
        x4_auth_set_gamepad(auth,&gamepads);
#if X4_STREAM_PROFILE
        if(game_input&&several){
            uint8_t changes=(uint8_t)((last_match_pads^gamepads.connected_mask)&0x0eu);
            for(unsigned i=1;i<X4_GAMEPAD_COUNT;++i)if(changes&(1u<<i)){
                snprintf(pad_notice,sizeof(pad_notice),x4_tr((gamepads.connected_mask&(1u<<i))?
                    X4_TR_PAD_TOAST_CONNECTED:X4_TR_PAD_TOAST_DISCONNECTED),i+1u);
                pad_notice_until=input_at+3000000u;
                pad_notice_serial=pad_notice_serial==INT32_MAX?1:pad_notice_serial+1;
                break;
            }
            last_match_pads=gamepads.connected_mask;
        }else{
            last_match_pads=0;
            pad_notice_until=0;
        }
#endif
#if X4_RUMBLE
        X4RumbleOutput desired={0};
        /* A report can arrive after the frame-start input sample. Poll with
         * a fresh time so that earlier sample is not treated as clock reversal. */
        x4_auth_poll_rumble(auth,sceKernelGetProcessTime(),&desired);
#if X4_STREAM_PROFILE
        if((!controller.data.connected&&rumble_was_connected)||controller.handle<0)rumble_state.handle=-1;
#else
        if((!controller.data.connected&&rumble_was_connected)||controller.handle<0)rumble_handle=-1;
#endif
        rumble_was_connected=controller.data.connected;
        if(!game_input||!session.rtc_connected||session.reconnecting||!controller.data.connected)
            desired.lgMotor=desired.smMotor=0;
#if X4_STREAM_PROFILE
        if(interaction_main.modes&X4_INTERACTION_RUMBLE_OFF)desired.lgMotor=desired.smMotor=0;
        if(rumble_state.generation!=pad_view.generation[0])rumble_state.handle=-1;
        rumble_state.generation=pad_view.generation[0];
        if(desired.generation&&desired.generation!=rumble_state.generation)
            desired.lgMotor=desired.smMotor=0;
        interaction_rumble_step(&rumble_state,controller.handle,
            (OrbisPadVibeParam){desired.lgMotor,desired.smMotor},sceKernelGetProcessTime());
        if(pads&&several)for(unsigned i=1;i<X4_GAMEPAD_COUNT;++i){
            InteractionRumble *state=&rumble_others[i-1];
            X4RumbleMotorOutput motor=desired.others[i-1];
            if(state->generation!=pad_view.generation[i])state->handle=-1;
            state->generation=pad_view.generation[i];
            int handle=pad_view.pad[i].handle;
            if(!pad_view.pad[i].data.connected||!game_input||!session.rtc_connected||session.reconnecting||
                (interaction_main.modes&X4_INTERACTION_RUMBLE_OFF)||
                motor.generation!=state->generation)motor.lgMotor=motor.smMotor=0;
            interaction_rumble_step(state,handle,(OrbisPadVibeParam){motor.lgMotor,motor.smMotor},
                sceKernelGetProcessTime());
        }
#else
        if(controller.handle>=0&&(rumble_handle!=controller.handle||
            rumble_applied.lgMotor!=desired.lgMotor||rumble_applied.smMotor!=desired.smMotor)) {
            OrbisPadVibeParam next={desired.lgMotor,desired.smMotor};
            int vibration_rc=scePadSetVibration(controller.handle,&next);
            ++rumble_pad_calls;
            if(vibration_rc>=0) ++rumble_pad_ok; else ++rumble_pad_errors;
            if(vibration_rc>=0) { rumble_applied=next; rumble_handle=controller.handle; }
        }
#endif
#endif
        unsigned next_menu_pad=x4_gamepad_menu_index(gamepads.connected_mask,
            pads&&several&&!game_input);
        if(next_menu_pad!=menu_pad){menu_pad=next_menu_pad;menu_pad_rearm=true;}
        if(menu_pad)controller=pad_view.pad[menu_pad];
        if(menu_pad_rearm){
            controller.pressed=0;
            if(!controller.data.buttons)menu_pad_rearm=false;
            controller.data.buttons=0;
        }
        if (!closing) {
            if (screen.exit_error) {
                if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) screen.exit_error = 0;
                controller.pressed = 0;
            }
            bool confirm_logout = false;
            if (logout_notice) {
                if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) logout_notice = false;
                else if (controller.pressed & ORBIS_PAD_BUTTON_LEFT) logout_confirm_focused = true;
                else if (controller.pressed & ORBIS_PAD_BUTTON_RIGHT) logout_confirm_focused = false;
                else if (controller.pressed & ORBIS_PAD_BUTTON_CROSS) {
                    logout_notice = false;
                    confirm_logout = logout_confirm_focused;
                }
                controller.pressed = 0;
            }
            if (confirm_logout && screen.page == 4 && !x4_auth_busy(auth) && pending_auth < 0) {
                x4_catalog_assets_suspend(catalog_assets);
                pending_auth = -1;
                x4_auth_forget(auth);
                x4_xbox_gamertag(false, catalog.gamertag);
                x4_recent_init(&recent);
            }
            if (cloud_launch.open) {
                bool accepted = screen.page == cloud_launch.page &&
                    !(controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) &&
                    (controller.pressed & ORBIS_PAD_BUTTON_CROSS);
                if (accepted) {
                    cloud_launch.open = false;
                    cloud_launch.accepted = true;
                } else if (screen.page != cloud_launch.page ||
                           (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE)) {
                    cloud_launch = (CloudLaunchNotice){0};
                }
                controller.pressed = accepted ? ORBIS_PAD_BUTTON_CROSS : 0;
            }
            /* An update modal owns all UI edges, including Cancel/Options. */
#ifdef X4_UPDATE51_ENABLED
            if (update_progress_open) {
                if (!update_rejected) update_view = x4_update51_view();
                const uint32_t pressed = controller.pressed;
#if !X4_PUBLIC
                if (update_view.stage != X4_UPDATE51_ACCEPTED) update_close_at = 0;
                if (update_view.stage == X4_UPDATE51_ACCEPTED) {
                    if (!update_close_at) update_close_at = input_at + UINT64_C(5000000);
                    if (input_at >= update_close_at || (pressed & ORBIS_PAD_BUTTON_CROSS)) {
                        screen.exit_requested = 1;
                        update_progress_open = false;
                        update_close_at = 0;
                    }
                } else
#endif
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                    if (update_view.stage == X4_UPDATE51_FAILED ||
                        update_view.stage == X4_UPDATE51_CANCELLED ||
                        update_view.stage == X4_UPDATE51_MANUAL ||
                        update_view.stage == X4_UPDATE51_UPDATED) update_progress_open = false;
                    else x4_update51_cancel();
                }
#if !X4_PUBLIC
                else if (update_view.stage == X4_UPDATE51_CHOOSE) {
                    if (pressed & ORBIS_PAD_BUTTON_LEFT) x4_update51_select_install(false);
                    if (pressed & ORBIS_PAD_BUTTON_RIGHT) x4_update51_select_install(true);
                    if (pressed & ORBIS_PAD_BUTTON_CROSS) x4_update51_confirm_install();
                }
#endif
                else if (!update_view.busy && (pressed & ORBIS_PAD_BUTTON_CROSS)) update_progress_open = false;
                controller.pressed = 0;
            }
#endif
            if (update_notice.open) {
#ifdef X4_UPDATE51_ENABLED
                const uint32_t pressed = controller.pressed;
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) update_notice.open = false;
                else if (pressed & ORBIS_PAD_BUTTON_LEFT) update_notice.install_focused = false;
                else if (pressed & ORBIS_PAD_BUTTON_RIGHT)
                    update_notice.install_focused = update_notice.can_install;
                else if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                    if (!update_notice.install_focused) update_notice.open = false;
                    else {
                        const bool allowed = update_notice.can_install && !game_input && !pending_launch &&
                            screen.page != 6 && !live_retained && !session_back &&
                            !closing && !screen.exit_requested && pending_auth < 0 && !x4_auth_busy(auth) &&
                            !(pressed & ORBIS_PAD_BUTTON_OPTIONS);
                        update_rejected = !x4_update51_start_checked(update_notice.tag,
                            update_notice.local, allowed, &update_view);
                        update_notice.open = false;
                        update_progress_open = true;
                    }
                }
#else
                (void)x4_apple_update_notice_handle(&update_notice,
                    (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0);
#endif
                controller.pressed = 0;
            }
#ifdef X4_UPDATE51_ENABLED
            if (screen.page == 2 && !ui2_shell.focused && !update_progress_open && !update_notice.open &&
                !picture_settings_ui.open && !closing &&
                (controller.pressed & ORBIS_PAD_BUTTON_CROSS)) {
                if (x4_release_check_restart()) {
                    update_manual = true;
                    update_notice.shown = false;
                }
                controller.pressed = 0;
            }
#endif
            /* Account consent owns every edge; X accepts, Circle cancels.
             * L1 never changes the in-game controller or its protocol. */
            if (remember_notice) {
                if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) remember_notice = false;
                else if (controller.pressed & ORBIS_PAD_BUTTON_CROSS) {
                    (void)x4_auth_set_remember(auth, true);
                    remember_notice = false;
                }
                controller.pressed = 0;
            } else if (screen.page == 4 && !picture_settings_ui.open &&
                       !x4_auth_busy(auth) && pending_auth < 0 &&
                       (controller.pressed & ORBIS_PAD_BUTTON_L1)) {
                if (remembered.enabled) (void)x4_auth_set_remember(auth, false);
                else if (remembered.available) remember_notice = true;
                controller.pressed = 0;
            }
            /* Access-card input belongs only to catalog/Fortnite. Reuse the
             * existing pending-auth coordinator; never sign Microsoft out. */
            if (screen.page == 5 || screen.page == 8) {
                if (access_query && !access_query_started && pending_auth < 0 &&
                           !x4_auth_busy(auth)) {
                    /* The existing coordinator may refuse before dispatch
                     * (for example, Store cleanup failed). Allow a new edge. */
                    access_query = false;
                    access_card_visible = false;
                }
                if (access_notice_until && input_at >= access_notice_until)
                    access_notice_until = 0;
            } else {
                access_card_visible = false;
                access_notice_until = 0;
            }
            static bool first_choice_modal_opened = false;
            bool awaiting_first = x4_server_settings_awaiting_first_choice();
            if (awaiting_first) {
                if (!first_choice_modal_opened) {
                    x4_picture_settings_open_server_first_time(&picture_settings_ui);
                    first_choice_modal_opened = true;
                }
            } else {
                first_choice_modal_opened = false;
            }
            uint32_t ui_pressed = controller.pressed;
            if (game_input) ui_pressed &= ~ORBIS_PAD_BUTTON_OPTIONS;
            /* CIRCLE finishes keyboard editing before account navigation.
             * OPTIONS still follows the global cleanup/exit path. */
            if (screen.page == 5 && catalog_browser.editing)
                ui_pressed &= ~ORBIS_PAD_BUTTON_CIRCLE;
#if X4_STREAM_PROFILE && !X4_PUBLIC
            /* R3 remains a local UI action on Inicio and the catalog. */
            if (screen.page == 0 || screen.page == 5) {
                unsigned actions=0;
                if (!menu_pad && !picture_settings_ui.open && (controller.pressed & ORBIS_PAD_BUTTON_R3))
                    actions |= X4_SETTINGS_OPEN;
                if(controller.pressed&ORBIS_PAD_BUTTON_UP) actions|=X4_SETTINGS_UP;
                if(controller.pressed&ORBIS_PAD_BUTTON_DOWN) actions|=X4_SETTINGS_DOWN;
                if(controller.pressed&ORBIS_PAD_BUTTON_LEFT) actions|=X4_SETTINGS_LEFT;
                if(controller.pressed&ORBIS_PAD_BUTTON_RIGHT) actions|=X4_SETTINGS_RIGHT;
                if(controller.pressed&ORBIS_PAD_BUTTON_CROSS) actions|=X4_SETTINGS_CONFIRM;
                if(controller.pressed&ORBIS_PAD_BUTTON_CIRCLE) actions|=X4_SETTINGS_CANCEL;
                bool can_edit=!picture_settings_ui.open&&!catalog_browser.editing&&
                    !x4_auth_busy(auth)&&pending_auth<0&&!pending_launch&&!session_back&&
                    !live_retained&&!access_query&&!access_card_visible&&!game_page_ui.open&&
                    !update_notice.open&&!cloud_launch.open&&!screen.exit_error;
#ifdef X4_UPDATE51_ENABLED
                can_edit = can_edit && !update_progress_open && !game_input && !closing && !screen.exit_requested;
                if (stream_settings_ui.open && can_edit && !x4_stream_settings_ui_warning_open() &&
                    stream_settings_ui.row == X4_UPDATE51_LAB_ROW && actions == X4_SETTINGS_CONFIRM &&
                    !(controller.pressed & ORBIS_PAD_BUTTON_OPTIONS)) {
                    update_notice.open = update_notice.local = update_notice.can_install = true;
                    update_notice.install_focused = false;
                    update_notice.tag[0] = 0;
                    stream_settings_ui.open = false;
                    stream_settings_consumed = true;
                } else
#endif
                if (stream_settings_ui.open && can_edit && !x4_stream_settings_ui_warning_open() &&
                    (controller.pressed & ORBIS_PAD_BUTTON_TRIANGLE) &&
                    !(controller.pressed & (ORBIS_PAD_BUTTON_CIRCLE | ORBIS_PAD_BUTTON_OPTIONS))) {
                    /* Lab-only UI action: discard the draft and show the
                     * sample on Home; it never changes the real check. */
                    update_test_requested = true;
                    stream_settings_ui.open = false;
                    stream_settings_consumed = true;
                    screen.page = 0;
                    ui_pressed = 0;
                    controller.pressed = 0;
                } else stream_settings_consumed=x4_stream_settings_ui_handle(&stream_settings_ui,actions,can_edit);
#if !X4_PUBLIC
                if (stream_settings_ui.applied) {
                    if(pads)x4_controller_roster_set_motion_sensor(pads,x4_script_engine_needs_gyro());
                    else x4_controller_set_motion_sensor(&primary,x4_script_engine_needs_gyro());
                }
#endif
                if (stream_settings_consumed) {
#ifdef X4_UPDATE51_ENABLED
                    ui_pressed = 0;
#else
                    ui_pressed &= ORBIS_PAD_BUTTON_OPTIONS;
#endif
                }
                if(stream_settings_consumed || stream_settings_ui.open) controller.pressed = 0;
            } else stream_settings_ui.open=false;
#elif X4_STREAM_PROFILE
            (void)stream_settings_ui;
#endif
            bool can_edit_pic = (picture_settings_ui.first_time_mode ||
                ((screen.page == 0 || screen.page == 2 || screen.page == 4 || screen.page == 5 ||
                  screen.page == 7 || screen.page == 8) && !x4_auth_busy(auth) && pending_auth < 0)) &&
                !game_input && !catalog_browser.editing && !pending_launch && !session_back && !live_retained &&
                !closing && !access_query && !game_page_ui.open;
            unsigned pic_actions = 0;
            if (controller.pressed & ORBIS_PAD_BUTTON_OPTIONS) pic_actions |= X4_PIC_SETTINGS_OPEN;
            if (controller.pressed & ORBIS_PAD_BUTTON_UP) pic_actions |= X4_PIC_SETTINGS_UP;
            if (controller.pressed & ORBIS_PAD_BUTTON_DOWN) pic_actions |= X4_PIC_SETTINGS_DOWN;
            if (controller.pressed & ORBIS_PAD_BUTTON_LEFT) pic_actions |= X4_PIC_SETTINGS_LEFT;
            if (controller.pressed & ORBIS_PAD_BUTTON_RIGHT) pic_actions |= X4_PIC_SETTINGS_RIGHT;
            if (controller.pressed & ORBIS_PAD_BUTTON_CROSS) pic_actions |= X4_PIC_SETTINGS_CONFIRM;
            if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) pic_actions |= X4_PIC_SETTINGS_CANCEL;
            if (controller.pressed & ORBIS_PAD_BUTTON_L1) pic_actions |= X4_PIC_SETTINGS_PREV_TAB;
            if (controller.pressed & ORBIS_PAD_BUTTON_R1) pic_actions |= X4_PIC_SETTINGS_NEXT_TAB;
            const bool settings_was_open = picture_settings_ui.open && !picture_settings_ui.first_time_mode;
            const X4Ui2Shell before_settings = ui2_shell;
            bool pic_consumed;
            if (!picture_settings_ui.open && can_edit_pic && screen.page == 8 &&
                (pic_actions & X4_PIC_SETTINGS_OPEN)) {
                x4_picture_settings_ui_open_section(&picture_settings_ui, X4_PIC_SECTION_SERVER);
                pic_consumed = true;
            } else {
                pic_consumed = x4_picture_settings_ui_handle(&picture_settings_ui, pic_actions, can_edit_pic);
            }
            if (language_revision != x4_lang_ui_revision()) {
                language_revision = x4_lang_ui_revision();
                ui_notice[0] = 0;
                /* A language revision invalidates cached pixels, not boot or account state. */
                x4_ui2_home_invalidate();
                x4_screen_home_invalidate();
                x4_catalog_gui_home_invalidate();
                x4_catalog_gui_keyboard_cache_invalidate();
                x4_game_page_cache_invalidate();
                x4_picture_settings_ui_cache_invalidate();
                x4_touch_menu_cache_reset();
                home_canvas_valid = menu_canvas_valid = false;
                account_canvas_valid = settings_canvas_valid = false;
                modal_latched = shell_presented = false;
                ++status_revision;
                ui_dirty_buffers = 2;
            }
            if (!settings_was_open && picture_settings_ui.open && !picture_settings_ui.first_time_mode) {
                settings_return_shell = before_settings;
                settings_return_valid = true;
                ui2_shell.focused = false;
            }
            if (settings_was_open && !picture_settings_ui.open && settings_return_valid) {
                ui2_shell = settings_return_shell;
                settings_return_valid = false;
            }
            if (picture_settings_ui.route != X4_PIC_ROUTE_NONE) {
                const int target_page = picture_settings_ui.route == X4_PIC_ROUTE_ACCOUNT ? 4 : 2;
                if (target_page != screen.page) {
                    if (target_page == 4) {
                        if (screen.page == 2 && about_return_settings && about_return_page == 4) {
                            ui2_shell = about_return_shell;
                            about_return_settings = false;
                        } else {
                            account_return_page = screen.page;
                            account_return_shell = ui2_shell;
                            account_return_settings = true;
                        }
                    } else {
                        if (screen.page == 4 && account_return_settings && account_return_page == 2) {
                            ui2_shell = account_return_shell;
                            account_return_settings = false;
                        } else {
                            about_return_page = screen.page;
                            about_return_shell = ui2_shell;
                            about_return_settings = true;
                        }
                    }
                    screen.page = target_page;
                }
                picture_settings_ui.route = X4_PIC_ROUTE_NONE;
                ui2_shell.focused = false;
            }
            if(picture_settings_ui.open&&picture_settings_ui.tab==3)
                for(unsigned i=0;i<X4_GAMEPAD_COUNT;++i){
                    X4PictureSettingsPad *row=&picture_settings_ui.pads[i];
                    row->assigned=pad_view.assigned[i];
                    row->connected=pad_view.pad[i].data.connected;
                    memcpy(row->name,pad_view.user_name[i],sizeof(row->name));
                    row->name[sizeof(row->name)-1]='\0';
                }
            if (pic_consumed || picture_settings_ui.open) {
                ui_pressed = 0;
                controller.pressed = 0;
            }
            if (picture_settings_ui.server_changed) {
                picture_settings_ui.server_changed = false;
                if (screen.page == 8) {
                    if (game_page_ui.open) game_page_close(&game_page_ui, catalog_assets);
                    x4_ui2_fortnite_begin(&fortnite);
                } else if (screen.page == 5 && catalog.state == X4_CATALOG_READY && !x4_auth_busy(auth)) {
                    catalog_selected = 0;
                    pending_auth = X4_AUTH_XBOX_CATALOG;
                }
            }
            if (picture_settings_ui.access_changed) {
                picture_settings_ui.access_changed = false;
                if (access_card_visible) x4_access_country_ui_open(&access_card_ui);
            }
            if (!picture_settings_ui.open && !pic_consumed &&
                (screen.page == 5 || screen.page == 8) && !pending_launch &&
                !live_retained && !session_back) {
                bool eligible = screen.page == 8 ? fortnite.phase == X4_UI2_FORTNITE_REGION :
                    catalog.state == X4_CATALOG_ERROR &&
                    x4_lang_is_cloud_unavailable(catalog.error, catalog.http_status,
                        catalog.xerr, catalog.offering, catalog.stage);
                if (eligible && !access_card_visible && !access_query &&
                    pending_auth < 0 && !x4_auth_busy(auth)) {
                    x4_access_country_ui_open(&access_card_ui);
                    access_card_visible = true;
                }
                if ((access_card_visible && eligible) || access_query) {
                    unsigned actions = 0;
                    if (controller.pressed & ORBIS_PAD_BUTTON_LEFT) actions |= X4_ACCESS_UI_LEFT;
                    if (controller.pressed & ORBIS_PAD_BUTTON_RIGHT) actions |= X4_ACCESS_UI_RIGHT;
                    if (controller.pressed & ORBIS_PAD_BUTTON_CROSS) actions |= X4_ACCESS_UI_CONFIRM;
                    if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) actions |= X4_ACCESS_UI_CANCEL;
                    if (controller.pressed & ORBIS_PAD_BUTTON_TRIANGLE) actions |= X4_ACCESS_UI_HELP;
                    bool busy = access_query || pending_auth >= 0 || x4_auth_busy(auth);
                    X4AccessCountryUiResult action = x4_access_country_ui_handle(
                        &access_card_ui, actions, busy, "tarjeta");
                    if (action == X4_ACCESS_UI_APPLIED && !busy) {
                        access_query_page = screen.page;
                        access_query = true;
                        access_query_started = access_query_cancelled = false;
                        access_notice_until = 0;
                        catalog_selected = 0;
                        if (access_query_page == 8) x4_ui2_fortnite_begin(&fortnite);
                        else pending_auth = X4_AUTH_XBOX_CATALOG;
                    } else if (action == X4_ACCESS_UI_CANCEL_QUERY) {
                        pending_auth = -1;
                        x4_auth_cancel(auth);
                        access_query_cancelled = true;
                        if (!access_query_started) access_query = false;
                    }
                    if (screen.page == 8 && (action == X4_ACCESS_UI_BACK ||
                        action == X4_ACCESS_UI_CANCEL_QUERY)) {
                        access_query = access_query_started = access_card_visible = false;
                        x4_ui2_fortnite_back(&fortnite);
                        screen.page = fortnite_return_page;
                        if (screen.page == 0) screen.selected = fortnite_return_selection;
                        reopen_catalog = true;
                    } else if (action == X4_ACCESS_UI_BACK) {
                        access_card_visible = false;
                        if (screen.catalog_from_home) {
                            screen.catalog_from_home = 0;
                            screen.page = 0;
                        } else screen.page = 4;
                    }
                    /* Consume edges before catalog navigation, search,
                     * account navigation or Fortnite's ordinary Play action. */
                    ui_pressed = 0;
                    controller.pressed = 0;
                }
            }
            if (screen.page == 5 || screen.page == 8) {
                x4_access_country_card_set(&access_card_ui, access_query ?
                    X4_ACCESS_CARD_TRYING : (access_notice_until ?
                    X4_ACCESS_CARD_SUCCESS : X4_ACCESS_CARD_SELECT));
            } else x4_access_country_card_set(NULL, X4_ACCESS_CARD_SELECT);
            if (game_input && x4_touch_menu_is_open(&touch_menu)) {
                X4TouchMenuAction menu_action = x4_touch_menu_update(&touch_menu, controller.pressed, controller.data.buttons, controller_read_at);
                switch (menu_action) {
                case X4_TOUCH_MENU_ACTION_BACK_TO_CATALOG:
                    mic_end_match(&touch_menu);
                    trace_inactive(live);
                    x4_auth_cancel(auth);
                    session_back = 1;
                    break;
                case X4_TOUCH_MENU_ACTION_TOGGLE_MUTE:
                    live_muted = !live_muted;
                    if (live) x4_live_media_set_muted(live, live_muted != 0);
                    break;
                case X4_TOUCH_MENU_ACTION_TOGGLE_MIC:
                    x4_mic_toggle_mute();
                    break;
                case X4_TOUCH_MENU_ACTION_RELOAD_STREAM:
                    x4_auth_request_reload(auth);
                    break;
                case X4_TOUCH_MENU_ACTION_NET_HUD:
                    (void)x4_net_hud_cycle_mode();
                    x4_touch_menu_cache_reset();
                    break;
                case X4_TOUCH_MENU_ACTION_XBOX_BUTTON:
                    x4_touch_menu_trigger_nexus(&touch_menu, controller_read_at);
                    break;
#if !X4_PUBLIC
                case X4_TOUCH_MENU_ACTION_MARK_TEST: {
                    X4Trace *live_trace = live ? x4_live_media_trace(live) : NULL;
                    diagnostic_stable = !diagnostic_stable;
                    unsigned epoch = diagnostic_stable ? X4_MON_EPOCH_STABLE : X4_MON_EPOCH_TRANSITION;
                    x4_trace_monitor_epoch(live_trace, X4_MON_ACTOR_SESSION, epoch);
                    x4_trace_record(live_trace, X4_TRACE_SESSION_PHASE, 0, epoch, ++diagnostic_mark);
                    printf("XCloud4: diagnostic phase=%s mark=%llu\n",
                        diagnostic_stable ? "stable" : "transition", (unsigned long long)diagnostic_mark);
                    break;
                }
                case X4_TOUCH_MENU_ACTION_EXIT_APP:
                    screen.exit_requested = 1;
                    break;
#endif
                default:
                    break;
                }
                ui_pressed = 0;
            }
            if (screen.page == 5 && catalog_browser.editing) {
                ui_pressed &= ~ORBIS_PAD_BUTTON_CIRCLE;
            }
            screen.signed_in = (account.state == X4_AUTH_AUTHORIZED);
            if (game_page_ui.open && screen.page == game_page_ui.origin && !picture_settings_ui.open) {
                game_input_owned = true;
                game_page_pressed = ui_pressed;
                controller.pressed = ui_pressed = 0;
            }
            if (!game_input_owned && screen.page == 5 && !picture_settings_ui.open &&
                !catalog_browser.editing && !ui2_shell.focused &&
                (ui_pressed & ORBIS_PAD_BUTTON_CIRCLE) &&
                x4_catalog_browser_ui2_close_selector(&catalog_browser)) {
                controller.pressed &= ~ORBIS_PAD_BUTTON_CIRCLE;
                ui_pressed &= ~ORBIS_PAD_BUTTON_CIRCLE;
            }
            bool shell_input = !game_input_owned && !picture_settings_ui.open && !cloud_launch.open &&
                !update_notice.open && !catalog_browser.editing && !closing &&
                (screen.page == 0 || screen.page == 2 || screen.page == 5 || screen.page == 7 ||
                 (screen.page == 4 && !x4_auth_busy(auth)));
#if X4_STREAM_PROFILE && !X4_PUBLIC
            shell_input = shell_input && !stream_settings_consumed && !stream_settings_ui.open;
#endif
            if (shell_input && ui_pressed) {
                const X4Ui2Shell before_nav = ui2_shell;
                X4Ui2HomeModel model;
                if (screen.page == 0) {
                    x4_ui2_home_model(&model, screen.signed_in, &catalog, &recent, &catalog_browser.home_rows);
                    home_choice = x4_ui2_home_normalize(&model, home_choice);
                }
                unsigned nav_flags = 0;
                if (ui_pressed & ORBIS_PAD_BUTTON_LEFT) nav_flags |= X4_UI2_NAV_LEFT;
                if (ui_pressed & ORBIS_PAD_BUTTON_RIGHT) nav_flags |= X4_UI2_NAV_RIGHT;
                if (ui_pressed & ORBIS_PAD_BUTTON_UP) nav_flags |= X4_UI2_NAV_UP;
                if (ui_pressed & ORBIS_PAD_BUTTON_DOWN) nav_flags |= X4_UI2_NAV_DOWN;
                if (ui_pressed & ORBIS_PAD_BUTTON_CROSS) nav_flags |= X4_UI2_NAV_SELECT;
                if (ui_pressed & ORBIS_PAD_BUTTON_L1) nav_flags |= X4_UI2_NAV_PREVIOUS;
                if (ui_pressed & ORBIS_PAD_BUTTON_R1) nav_flags |= X4_UI2_NAV_NEXT;
                if (ui_pressed & ORBIS_PAD_BUTTON_CIRCLE) nav_flags |= X4_UI2_NAV_CIRCLE;
                /* Circle follows the existing page/modal return path. */
                bool at_edge = screen.page == 0 ?
                    x4_ui2_home_move(&model, home_choice, X4_UI2_HOME_LEFT) == home_choice :
                    screen.page == 5 ? x4_catalog_browser_ui2_left_edge(&catalog_browser) :
                    screen.page == 4 ? account_ui.row < X4_UI2_ACCOUNT_HISTORY_FIRST : true;
                X4Ui2NavResult nav = x4_ui2_shell_handle(&ui2_shell, nav_flags, at_edge);
                X4Ui2HomeRoute route = {0};
                if (!nav.consumed && screen.page == 0 && !ui2_shell.focused &&
                    !(nav_flags & X4_UI2_NAV_CIRCLE)) {
                    if (ui_pressed & ORBIS_PAD_BUTTON_TRIANGLE) route.action = X4_UI2_HOME_LOGIN;
                    else if (nav_flags == X4_UI2_NAV_SELECT)
                        route = x4_ui2_home_activate(&model, home_choice);
                    else if (nav_flags & X4_UI2_NAV_LEFT)
                        home_choice = x4_ui2_home_move(&model, home_choice, X4_UI2_HOME_LEFT);
                    else if (nav_flags & X4_UI2_NAV_RIGHT)
                        home_choice = x4_ui2_home_move(&model, home_choice, X4_UI2_HOME_RIGHT);
                    else if (nav_flags & X4_UI2_NAV_UP)
                        home_choice = x4_ui2_home_move(&model, home_choice, X4_UI2_HOME_UP);
                    else if (nav_flags & X4_UI2_NAV_DOWN)
                        home_choice = x4_ui2_home_move(&model, home_choice, X4_UI2_HOME_DOWN);
                }
                bool enter_tab = false;
                if (nav.activate) {
                    switch (nav.target) {
                    case X4_UI2_HOME: screen.page = 0; break;
                    case X4_UI2_DISCOVER: login_tab = X4_CAT_TAB_DISCOVER; enter_tab = true; break;
                    case X4_UI2_CATALOG: login_tab = X4_CAT_TAB_CATALOG; enter_tab = true; break;
                    case X4_UI2_SEARCH: login_tab = X4_CAT_TAB_SEARCH; enter_tab = true; break;
                    case X4_UI2_LIBRARY: login_tab = X4_CAT_TAB_RECENTS; enter_tab = true; break;
                    case X4_UI2_SETTINGS:
                        if (can_edit_pic) {
                            settings_return_shell = before_nav;
                            settings_return_valid = true;
                            (void)x4_picture_settings_ui_handle(&picture_settings_ui, X4_PIC_SETTINGS_OPEN, true);
                        }
                        break;
                    case X4_UI2_SUPPORT: screen.page = 7; break;
                    case X4_UI2_ABOUT: screen.page = 2; break;
                    default: break;
                    }
                    x4_ui2_shell_set_section(&ui2_shell, nav.target);
                }
                switch (route.action) {
                case X4_UI2_HOME_LOGIN:
                    account_return_page = screen.page;
                    account_return_shell = before_nav;
                    account_return_settings = false;
                    screen.auto_catalog_after_login = 0;
                    screen.page = 4;
                    break;
                case X4_UI2_HOME_CATALOG: case X4_UI2_HOME_RECENTS:
                    login_tab = route.action == X4_UI2_HOME_CATALOG ? X4_CAT_TAB_CATALOG : X4_CAT_TAB_RECENTS;
                    enter_tab = true;
                    break;
                case X4_UI2_HOME_GAME:
                    if (route.source < catalog.count && !x4_auth_busy(auth)) {
                        ui_notice[0] = 0;
                        game_page_open(&game_page_ui, 0, route.source, &catalog.titles[route.source], NULL);
                    }
                    break;
                case X4_UI2_HOME_FORTNITE: screen.page = 8; break;
                default: break;
                }
                if (enter_tab && previous_page == 5 && screen.signed_in)
                    x4_catalog_browser_ui2_select(&catalog_browser, login_tab);
                else if (enter_tab) screen.explore_catalog_requested = 1;
                if (!(ui_pressed & ORBIS_PAD_BUTTON_CIRCLE) &&
                    (nav.consumed || ui2_shell.focused || route.action != X4_UI2_HOME_NONE ||
                     (previous_page == 0 && (nav_flags || (ui_pressed & ORBIS_PAD_BUTTON_TRIANGLE))))) {
                    controller.pressed = ui_pressed = 0;
                }
            }
            if (ui_pressed & ORBIS_PAD_BUTTON_CIRCLE)
                ui2_shell.focused = false;
            const bool returning_about = screen.page == 2 && about_return_settings;
            if ((ui_pressed & ORBIS_PAD_BUTTON_CIRCLE) && !game_input_owned &&
                ((screen.page == 4 && !fortnite_login_flow) || returning_about)) {
                const bool return_to_settings = returning_about || account_return_settings;
                screen.auto_catalog_after_login = 0;
                screen.page = returning_about ? about_return_page : account_return_page;
                ui2_shell = returning_about ? about_return_shell : account_return_shell;
                if (return_to_settings) {
                    x4_picture_settings_ui_open_section(&picture_settings_ui,
                        returning_about ? X4_PIC_SECTION_ABOUT : X4_PIC_SECTION_ACCOUNT);
                    picture_settings_ui.section_focused = true;
                    settings_return_shell = ui2_shell;
                    settings_return_valid = true;
                    ui2_shell.focused = false;
                }
                if (returning_about) about_return_settings = false;
                else account_return_settings = false;
                controller.pressed = ui_pressed = 0;
            }
            if (screen.page == 8) {
                /* While resolving the ficha, Circle is the only page action. */
                if (previous_page == 8 && !game_page_ui.open &&
                    (ui_pressed & ORBIS_PAD_BUTTON_CIRCLE)) {
                    x4_ui2_fortnite_back(&fortnite);
                    screen.page = fortnite_return_page;
                    if (screen.page == 0) screen.selected = fortnite_return_selection;
                    reopen_catalog = true;
                }
            } else if (screen.page != 0 || (ui_pressed & ORBIS_PAD_BUTTON_CIRCLE)) {
                x4_screen_update(&screen, ui_pressed, controller.data.buttons);
            }
            if (previous_page == 0 && screen.page == 8) {
                fortnite_return_page = 0;
                fortnite_return_selection = screen.selected;
                x4_ui2_fortnite_begin(&fortnite);
            }
            if (screen.open_recent_requested) {
                screen.open_recent_requested = 0;
                unsigned source;
                if (x4_home_continue_index(&catalog,&recent,&source)) {
                    ui_notice[0] = 0;
                    game_page_open(&game_page_ui, 0, source, &catalog.titles[source], NULL);
                } else {
                    x4_catalog_browser_ui2_select(&catalog_browser, X4_CAT_TAB_RECENTS);
                    screen.page = 5;
                    screen.catalog_from_home = 1;
                    if (!x4_auth_busy(auth) && pending_auth < 0) {
                        if (catalog.state != X4_CATALOG_READY) pending_auth = X4_AUTH_XBOX_CATALOG;
                        else if (recent.state == X4_RECENT_IDLE) pending_auth = X4_AUTH_XBOX_RECENTS;
                    }
                }
                controller.pressed = 0;
            }
            if (screen.open_settings_requested) {
                screen.open_settings_requested = 0;
                if (can_edit_pic) {
                    (void)x4_picture_settings_ui_handle(&picture_settings_ui, X4_PIC_SETTINGS_OPEN, true);
                }
            }
            if (screen.explore_catalog_requested) {
                screen.explore_catalog_requested = 0;
                x4_catalog_browser_ui2_select(&catalog_browser, login_tab);
                if (screen.signed_in) {
                    screen.page = 5;
                    screen.catalog_from_home = 1;
                    if (!x4_auth_busy(auth) && pending_auth < 0) {
                        if (catalog.state != X4_CATALOG_READY) {
                            catalog_selected = 0;
                            pending_auth = X4_AUTH_XBOX_CATALOG;
                        } else if (recent.state == X4_RECENT_IDLE) {
                            pending_auth = X4_AUTH_XBOX_RECENTS;
                        }
                    }
                } else {
                    screen.auto_catalog_after_login = 1;
                    account_return_page = screen.page;
                    account_return_shell = ui2_shell;
                    account_return_settings = false;
                    screen.page = 4;
                    if (!x4_auth_busy(auth) && pending_auth < 0) {
                        pending_auth = X4_AUTH_SIGN_IN;
                    }
                }
            }
        }
        if ((previous_page == 0 || previous_page == 4 || previous_page == 5 || previous_page == 6 || previous_page == 8) && screen.page != previous_page) {
            if (previous_page==6) mic_end_match(&touch_menu);
            x4_catalog_assets_suspend(catalog_assets);
            pending_launch = 0;
            if (!(previous_page == 4 && (screen.page == 8 || screen.page == 5)) &&
                !(previous_page == 0 && (screen.page == 4 || screen.page == 5 || screen.page == 8))) {
                pending_auth = -1;
                trace_inactive(live);
                x4_auth_cancel(auth);
            }
        }
        if (screen.exit_requested) {
            mic_end_match(&touch_menu);
            x4_catalog_assets_suspend(catalog_assets);
            pending_auth = -1;
            pending_launch = 0;
            trace_inactive(live);
            if (x4_auth_busy(auth) || (!x4_catalog_assets_idle(catalog_assets) &&
                x4_catalog_assets_fault(catalog_assets) != X4_ASSET_FAULT_CLEANUP)) {
                x4_auth_cancel(auth);
                closing = 1;
            } else {
                closing = 0;
                screen.exit_error = x4_exit_prepare();
                if (screen.exit_error < 0) {
                    printf("XCloud4: preparar salida fallo 0x%08x\n", (unsigned)screen.exit_error);
                    screen.exit_requested = 0;
                } else break;
            }
        }
        if (screen.page == 4 && play_history.revision != account_history_revision) {
            x4_ui2_account_summary(&play_history, &account_history);
            account_history_revision = play_history.revision;
        }
        if (!closing && screen.page == 4 && previous_page == 4 && !remember_notice && !logout_notice) {
            if (fortnite_login_flow) {
                if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                    fortnite_login_flow = false;
                    trace_inactive(live);
                    x4_auth_cancel(auth);
                    x4_ui2_fortnite_back(&fortnite);
                    screen.page = fortnite_return_page;
                    if (screen.page == 0) screen.selected = fortnite_return_selection;
                } else if (account.state == X4_AUTH_AUTHORIZED && !x4_auth_busy(auth)) {
                    fortnite_login_flow = false;
                    screen.page = 8;
                    x4_ui2_fortnite_begin(&fortnite);
                }
            } else if (screen.auto_catalog_after_login && account.state == X4_AUTH_AUTHORIZED && !x4_auth_busy(auth)) {
                screen.auto_catalog_after_login = 0;
                screen.catalog_from_home = 1;
                screen.page = 5;
                x4_catalog_browser_ui2_select(&catalog_browser, login_tab);
                if (pending_auth < 0) {
                    if (catalog.state != X4_CATALOG_READY) {
                        catalog_selected = 0;
                        pending_auth = X4_AUTH_XBOX_CATALOG;
                    } else {
                        pending_auth = X4_AUTH_XBOX_RECENTS;
                    }
                }
            } else if (!x4_auth_busy(auth) && pending_auth < 0) {
                const X4Ui2AccountModel model = {.auth = &account, .remember = remembered,
                    .cloud_time = cloud_time, .history = &account_history};
                const uint32_t buttons = controller.pressed;
                const int dx = (buttons & ORBIS_PAD_BUTTON_RIGHT) != 0 ?
                    1 : (buttons & ORBIS_PAD_BUTTON_LEFT) ? -1 : 0;
                const int dy = (buttons & ORBIS_PAD_BUTTON_DOWN) != 0 ?
                    1 : (buttons & ORBIS_PAD_BUTTON_UP) ? -1 : 0;
                if (dx || dy) x4_ui2_account_move(&account_ui, &model, dx, dy);
                const char *product = NULL;
                const X4Ui2AccountAction action = (buttons & ORBIS_PAD_BUTTON_CROSS) ?
                    x4_ui2_account_select(&account_ui, &model, &product) : X4_UI2_ACCOUNT_NONE;
                if (action == X4_UI2_ACCOUNT_SIGN_IN) pending_auth = X4_AUTH_SIGN_IN;
#if !X4_PUBLIC
                else if (buttons & ORBIS_PAD_BUTTON_SQUARE) pending_auth = X4_AUTH_CHECK_CONNECTION;
#endif
                else if (action == X4_UI2_ACCOUNT_SIGN_OUT || (buttons & ORBIS_PAD_BUTTON_TRIANGLE)) {
                    logout_notice = true;
                    logout_confirm_focused = false;
                } else if (action == X4_UI2_ACCOUNT_REMEMBER) {
                    if (remembered.enabled) (void)x4_auth_set_remember(auth, false);
                    else if (remembered.available) remember_notice = true;
                } else if (action == X4_UI2_ACCOUNT_SERVER || action == X4_UI2_ACCOUNT_COUNTRY) {
                    settings_return_shell = ui2_shell;
                    settings_return_valid = true;
                    x4_picture_settings_ui_open_section(&picture_settings_ui, action == X4_UI2_ACCOUNT_SERVER ?
                        X4_PIC_SECTION_SERVER : X4_PIC_SECTION_ACCESS);
                } else if (action == X4_UI2_ACCOUNT_OPEN_GAME && product) {
                    unsigned source = 0;
                    while (source < catalog.count && strcmp(product, catalog.titles[source].product_id))
                        ++source;
                    ui_notice[0] = 0;
                    if (source < catalog.count) {
                        game_page_open(&game_page_ui, 4, source, &catalog.titles[source], NULL);
                    } else {
                        X4RecentGame item = {0};
                        snprintf(item.id, sizeof(item.id), "%s", product);
                        snprintf(item.product_id, sizeof(item.product_id), "%s", product);
                        snprintf(item.name, sizeof(item.name), "%s", x4_tr(X4_TR_U49_ACCOUNT_GAME));
                        game_page_open(&game_page_ui, 4, X4_CATALOG_MAX, NULL, &item);
                    }
                }
                else if ((controller.pressed & ORBIS_PAD_BUTTON_R1) &&
                         account.state == X4_AUTH_AUTHORIZED && !x4_auth_busy(auth)) {
                    screen.page = 5;
                    catalog_browser.apple_mode = true;
                    catalog_browser.active_tab = X4_CAT_TAB_DISCOVER;
                    catalog_browser.top_bar_focused = false;
                    catalog_browser.hero_focused = true;
                    if (catalog.state != X4_CATALOG_READY) {
                        catalog_selected = 0;
                        pending_auth = X4_AUTH_XBOX_CATALOG;
                    } else pending_auth = X4_AUTH_XBOX_RECENTS;
                }
            }
        }
        /* Public Store work must release native HTTP/Net before auth uses
         * it. Cancellation is nonblocking; keep drawing/navigation alive.
         * No credential or arbitrary URL crosses this coordinator. */
        if (screen.page == 8 && account.state == X4_AUTH_AUTHORIZED &&
            fortnite.phase == X4_UI2_FORTNITE_QUEUED && pending_auth < 0)
            pending_auth = X4_AUTH_FORTNITE_QUERY;
        if (pending_auth >= 0) {
            x4_catalog_assets_suspend(catalog_assets);
            if (x4_catalog_assets_fault(catalog_assets) == X4_ASSET_FAULT_CLEANUP) {
                if (pending_auth == X4_AUTH_FORTNITE_QUERY) {
                    x4_ui2_fortnite_started(&fortnite, X4_AUTH_E_BUSY);
                    if (access_query_page == 8) access_query = access_card_visible = false;
                }
                pending_auth = -1;
                pending_launch = 0;
                snprintf(ui_notice, sizeof(ui_notice), "%s", x4_tr(X4_TR_NOTICE_COVER_CLEANUP));
            } else if (!x4_auth_busy(auth) && x4_catalog_assets_idle(catalog_assets)) {
                enum X4AuthAction action = (enum X4AuthAction)pending_auth;
                pending_auth = -1;
                rc = x4_auth_start(auth, action);
#if !X4_PUBLIC
                if (!rc && action == X4_AUTH_XBOX_CATALOG)
                    ui_boot_event(&boot_diag, BOOT_CATALOG_REQUESTED, input_at);
                if (!rc && (action == X4_AUTH_XBOX_CATALOG || action == X4_AUTH_XBOX_RECENTS))
                    ui_boot_event(&boot_diag, BOOT_RECENTS_REQUESTED, input_at);
#endif
                if (action == X4_AUTH_FORTNITE_QUERY) {
                    x4_ui2_fortnite_started(&fortnite, rc);
                    if (rc && access_query_page == 8) access_card_visible = false;
                }
                if (access_query && !access_query_started &&
                    action == (access_query_page == 8 ? X4_AUTH_FORTNITE_QUERY : X4_AUTH_XBOX_CATALOG)) {
                    access_query_started = rc == 0;
                    if (rc) access_query = false;
                }
                if (rc) snprintf(ui_notice, sizeof(ui_notice), x4_tr(X4_TR_NOTICE_QUERY_ERR_FMT), rc);
                else ui_notice[0] = 0;
            }
        }
        x4_trace_monitor_state(live ? x4_live_media_trace(live) : NULL,
            X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAIN_STATUS, 0);
#if X4_STREAM_PROFILE
        interaction_phase(X4_MON_PHASE_MAIN_STATUS,sceKernelGetProcessTime());
#endif
        x4_auth_snapshot(auth, &account);
        x4_auth_remember_snapshot(auth, &remembered);
        screen.signed_in = (account.state == X4_AUTH_AUTHORIZED);
        bool catalog_copied = x4_auth_catalog_snapshot_cached(auth, &catalog, &catalog_cursor) != 0;
        x4_xbox_gamertag(screen.signed_in, catalog.gamertag);
        if (screen.page == 8)
            x4_ui2_fortnite_complete(&fortnite, &catalog, x4_auth_busy(auth));
        /* Evaluate completion only after copying the new worker's catalog,
         * never against the previous frame's refusal/READY snapshot. */
        if (access_query && access_query_started && !x4_auth_busy(auth) &&
            catalog.state != X4_CATALOG_LOADING) {
            access_query = access_query_started = false;
            access_card_visible = false;
            if (!access_query_cancelled && catalog.state == X4_CATALOG_READY) {
                access_card_ui.selected = x4_auth_access_country(auth);
                access_notice_until = input_at + 3000000u;
            }
            x4_access_country_card_set(&access_card_ui, access_notice_until ?
                X4_ACCESS_CARD_SUCCESS : X4_ACCESS_CARD_SELECT);
        }
        if (catalog_copied) {
            x4_ui2_home_catalog_changed();
            ++ui_generation;
            x4_catalog_presentation_set_generation(&presentation, ui_generation);
            x4_catalog_assets_set_generation(catalog_assets, ui_generation);
            x4_catalog_presentation_apply_names(&presentation, &catalog);
            (void)x4_catalog_presentation_apply_genres(&presentation, &catalog_browser, &catalog);
            x4_catalog_browser_rebuild(&catalog_browser, &catalog);
            x4_recent_refresh_catalog(&recent, &catalog);
            x4_catalog_browser_rebuild_featured(&catalog_browser, &catalog, &recent);
            if (catalog.state != X4_CATALOG_READY) catalog_browser.editing = false;
        }
        bool recent_copied = x4_auth_recent_snapshot_cached(auth, &recent_snapshot, &recent_cursor) != 0;
        if (recent_copied) {
            x4_recent_begin(&recent, recent_snapshot.account_epoch, recent_snapshot.request_generation);
            if (recent_snapshot.state == X4_CATALOG_READY || recent_snapshot.state == X4_CATALOG_ERROR ||
                recent_snapshot.state == X4_CATALOG_CANCELLED)
                x4_recent_publish(&recent, recent_snapshot.account_epoch, recent_snapshot.request_generation,
                    recent_snapshot.title_ids, recent_snapshot.count,
                    recent_snapshot.state == X4_CATALOG_READY ? 0 :
                        recent_snapshot.error ? recent_snapshot.error : X4_AUTH_E_RESPONSE,
                    recent_snapshot.http_status, &catalog);
            else if (recent_snapshot.state == X4_CATALOG_IDLE) x4_recent_init(&recent);
            x4_catalog_browser_sync_recents(&catalog_browser, &recent);
            x4_catalog_browser_rebuild_featured(&catalog_browser, &catalog, &recent);
        }
#if !X4_PUBLIC
        if (boot_restore_started && screen.signed_in)
            ui_boot_event(&boot_diag, BOOT_RESTORED, input_at);
        if (screen.signed_in && catalog.gamertag[0])
            ui_boot_event(&boot_diag, BOOT_GAMERTAG, input_at);
        if (catalog_copied && catalog.state != X4_CATALOG_IDLE && catalog.state != X4_CATALOG_LOADING)
            ui_boot_event(&boot_diag, BOOT_CATALOG_RECEIVED, input_at);
        if (recent_copied && recent_snapshot.state != X4_CATALOG_IDLE && recent_snapshot.state != X4_CATALOG_LOADING)
            ui_boot_event(&boot_diag, BOOT_RECENTS_RECEIVED, input_at);
#endif
        if (home_boot_catalog_pending && !x4_auth_busy(auth)) {
            if (!screen.signed_in && account.state != X4_AUTH_CONNECTING)
                home_boot_catalog_pending = false;
            else if (screen.signed_in && screen.page == 0 && !closing && !screen.exit_requested &&
                !picture_settings_ui.open && !game_page_ui.open && pending_auth < 0 && !pending_launch) {
                /* The existing catalog action also obtains Recents. Route it
                 * through the ordinary cleanup coordinator once per startup. */
                home_boot_catalog_pending = false;
                if (catalog.state == X4_CATALOG_IDLE) pending_auth = X4_AUTH_XBOX_CATALOG;
                else if (catalog.state == X4_CATALOG_READY && recent_snapshot.state == X4_CATALOG_IDLE)
                    pending_auth = X4_AUTH_XBOX_RECENTS;
            }
        }
        bool home_rows_changed = catalog_copied || recent_copied;
        if (recent_snapshot.account_epoch != home_sigl_epoch) {
            home_sigl_epoch = recent_snapshot.account_epoch;
            home_sigl_revision = 0;
            memset(home_sigls, 0, sizeof(home_sigls));
            x4_catalog_assets_request_sigls(catalog_assets, 0);
            home_rows_changed = true;
        }
        if ((screen.page == 0 || screen.page == 5) && catalog.state == X4_CATALOG_READY) {
            x4_catalog_assets_request_sigls(catalog_assets, home_sigl_epoch);
#if !X4_PUBLIC
            if (catalog_assets && home_sigl_epoch)
                ui_boot_event(&boot_diag, BOOT_SIGLS_REQUESTED, input_at);
#endif
            if (x4_catalog_assets_copy_sigls(catalog_assets, home_sigls, &home_sigl_revision) > 0) {
                home_rows_changed = true;
#if !X4_PUBLIC
                bool sigls_pending = false;
                for (unsigned i = 0; i < X4_SIGL_COUNT; ++i) sigls_pending |= home_sigls[i].pending;
                if (home_sigl_epoch && !sigls_pending)
                    ui_boot_event(&boot_diag, BOOT_SIGLS_RECEIVED, input_at);
#endif
            }
        }
        if (home_rows_changed)
            x4_catalog_browser_home_rebuild(&catalog_browser, &catalog, &recent, home_sigls);
        bool metadata_changed = false;
        if ((screen.page == 0 || screen.page == 5) && !x4_auth_busy(auth)) {
            static X4ProductMetadata completed[X4_CATALOG_ASSET_SLOTS];
            uint64_t revision = metadata_revision; /* A busy worker leaves it unchanged. */
            size_t n = x4_catalog_assets_copy_metadata(catalog_assets, completed, X4_CATALOG_ASSET_SLOTS, &revision);
            if (revision != metadata_revision) {
                metadata_revision = revision;
                x4_catalog_presentation_ingest(&presentation, ui_generation, revision, completed, n);
                metadata_changed = x4_catalog_presentation_apply_names(&presentation, &catalog);
                if (x4_catalog_presentation_apply_genres(&presentation, &catalog_browser, &catalog))
                    metadata_changed = true;
                if (metadata_changed) {
                    x4_catalog_browser_rebuild(&catalog_browser, &catalog);
                    x4_recent_refresh_catalog(&recent, &catalog);
                    x4_catalog_browser_rebuild_featured(&catalog_browser, &catalog, &recent);
                }
            }
        }
        x4_auth_session_snapshot(auth, &session);
        bool cloud_active = screen.page == 6 && x4_auth_busy(auth);
        bool cloud_playing = cloud_active && session.ever_connected;
        X4CloudTime measured = {0};
        uint64_t measured_account = cloud_account, measured_revision = cloud_revision;
        if (!closing)
            x4_auth_cloud_time(auth,x4_lang_market(),cloud_force || cloud_session != cloud_active,
                &measured,&measured_account,&measured_revision);
        cloud_force = false;
        cloud_session = cloud_active;
        if (cloud_account != measured_account) {
            cloud_clock = (X4CloudTimeState){0};
            cloud_account = measured_account;
            cloud_revision = cloud_alert = cloud_alert_queue = 0;
            cloud_alert_until = 0;
            cloud_launch = (CloudLaunchNotice){0};
        }
        x4_cloud_time_set_playing(&cloud_clock,cloud_playing,input_at);
        if (measured_revision != cloud_revision) {
            cloud_revision = measured_revision;
            x4_cloud_time_apply(&cloud_clock,measured,input_at);
            if (measured.source!=X4_CLOUD_TIME_NONE && cloud_clock.value.source==X4_CLOUD_TIME_NONE) {
                cloud_alert = cloud_alert_queue = 0;
                cloud_alert_until = 0;
                cloud_launch = (CloudLaunchNotice){0};
            }
        }
        cloud_time = x4_cloud_time_current(&cloud_clock,input_at);
        x4_cloud_time_home_set(cloud_time);
        if (cloud_playing) {
            cloud_alert_queue |= x4_cloud_time_alerts(&cloud_clock,input_at);
            if (input_at >= cloud_alert_until) {
                cloud_alert = cloud_alert_queue & X4_CLOUD_TIME_ALERT_15 ? X4_CLOUD_TIME_ALERT_15 :
                    cloud_alert_queue & X4_CLOUD_TIME_ALERT_5;
                cloud_alert_queue &= ~cloud_alert;
                if (cloud_alert) {
                    cloud_alert_until = input_at + 4000000u;
#if !X4_PUBLIC
                    printf("XCloud4: tiempo nube aviso_min=%u\n",cloud_alert == X4_CLOUD_TIME_ALERT_15 ? 15u : 5u);
#endif
                }
            }
        } else {
            cloud_alert = cloud_alert_queue = 0;
            cloud_alert_until = 0;
        }
        if (!closing && screen.page == 6 && previous_page == 6) {
            if ((controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) && !game_input && !session_back) {
                mic_end_match(&touch_menu);
                trace_inactive(live);
                x4_auth_cancel(auth);
                session_back = 1;
            }
            /* Keep the session page visible until remote cleanup finishes. */
            if (session_back && !x4_auth_busy(auth)) {
                session_back = 0;
                if (is_fortnite_session) {
                    screen.page = 8;
                    is_fortnite_session = false;
                } else {
                    screen.page = 5;
                    pending_auth = X4_AUTH_XBOX_RECENTS;
                }
            }
        }
        if (game_page_ui.open && (closing || screen.exit_requested ||
            screen.page != game_page_ui.origin || account.state != X4_AUTH_AUTHORIZED || access_card_visible))
            game_page_close(&game_page_ui, catalog_assets);
        if (game_page_ui.open) {
            game_page_rebind(&game_page_ui, &catalog);
            enum X4CatalogAssetState background;
            if (x4_catalog_assets_background_status(catalog_assets,game_page_ui.product,&background))
                game_page_ui.background_state = background;
            uint64_t revision = x4_catalog_assets_game_revision(catalog_assets);
            if (game_page_ui.detail_revision != revision) {
                enum X4CatalogAssetState state;
                if (x4_catalog_assets_game_details(catalog_assets, game_page_ui.product,
                        &game_page_ui.details, &state) > 0) {
                    game_page_ui.has_details = state == X4_ASSET_READY;
                    game_page_ui.detail_revision = revision;
                    ++game_page_ui.redraw;
                    if (game_page_ui.has_details && x4_catalog_presentation_ingest_detail(&presentation,
                            game_page_ui.product, &game_page_ui.details)) {
                        if (x4_catalog_presentation_apply_genres(&presentation, &catalog_browser, &catalog))
                            x4_catalog_browser_rebuild(&catalog_browser, &catalog);
                        game_page_ui.similar_dirty = true;
                    }
                }
            }
            if (game_page_ui.similar_dirty || catalog_copied || metadata_changed)
                game_page_rebuild_similar(&game_page_ui, &catalog_browser, &catalog);
        }
        if (game_input_owned && game_page_ui.open && !cloud_launch.open) {
            unsigned actions = game_page_input(game_page_pressed);
            if (controller.data.connected && input_at >= game_scroll_after) {
                if (controller.data.rightStick.y < 80) actions |= X4_GAME_INPUT_SCROLL_UP;
                else if (controller.data.rightStick.y > 175) actions |= X4_GAME_INPUT_SCROLL_DOWN;
                if (actions & (X4_GAME_INPUT_SCROLL_UP | X4_GAME_INPUT_SCROLL_DOWN))
                    game_scroll_after = input_at + 120000u;
            }
            if (actions) {
                X4GamePageModel model = game_page_model(&game_page_ui, &catalog, &favorites,
                    x4_auth_busy(auth) || pending_auth >= 0 || pending_launch);
                enum X4GamePageAction action = x4_game_page_update(&game_page_ui.page, &model, actions);
                ++game_page_ui.redraw;
                if (action == X4_GAME_ACTION_BACK) {
                    pending_launch = 0;
                    if (!game_page_parent(&game_page_ui))
                        game_page_close(&game_page_ui, catalog_assets);
                    if (!game_page_ui.open && screen.page == 8) {
                        x4_ui2_fortnite_back(&fortnite);
                        screen.page = fortnite_return_page;
                        if (screen.page == 0) screen.selected = fortnite_return_selection;
                        reopen_catalog = true;
                    }
                } else if (action == X4_GAME_ACTION_REFRESH && pending_auth < 0 && !x4_auth_busy(auth)) {
                    if (screen.page == 8) {
                        game_page_close(&game_page_ui, catalog_assets);
                        x4_ui2_fortnite_begin(&fortnite);
                    } else pending_auth = X4_AUTH_XBOX_CATALOG;
                } else if (action == X4_GAME_ACTION_FAVORITE) {
                    ui_toggle_favorite(&favorites, &catalog_browser, model.product_id,
                        model.title, ui_notice, sizeof(ui_notice));
                } else if (action == X4_GAME_ACTION_SIMILAR && game_page_ui.page.similar_index < model.similar_count) {
                    unsigned index = model.similar[game_page_ui.page.similar_index].source_index;
                    if (catalog.state == X4_CATALOG_READY && index < catalog.count) {
                        game_page_follow(&game_page_ui, index, &catalog.titles[index]);
                        ui_notice[0] = 0;
                    }
                } else if (action == X4_GAME_ACTION_PLAY) game_play = true;
                if (game_page_ui.open && game_page_ui.similar_dirty)
                    game_page_rebuild_similar(&game_page_ui, &catalog_browser, &catalog);
            }
        }
        if (reopen_catalog) pending_auth = X4_AUTH_XBOX_CATALOG;

        bool launch_game_page = false;
        unsigned game_source = 0;
        const X4CatalogTitle *game_title = NULL;
        if (pending_launch && (!game_page_ui.open || game_page_pressed ||
                catalog_copied || pending_generation != ui_generation))
            pending_launch = 0;
        if (game_page_ui.open && (game_play || pending_launch) &&
            !catalog_copied && !recent_copied && !metadata_changed &&
            pending_auth < 0 && !x4_auth_busy(auth)) {
            const X4CatalogTitle *chosen = game_page_selected(&game_page_ui, &catalog);
            if (chosen && (screen.page != 8 || game_page_ui.source == 0) &&
                cloud_launch_allowed(&cloud_launch,cloud_time,screen.page,chosen->id)) {
                x4_catalog_assets_suspend(catalog_assets);
                if (x4_catalog_assets_fault(catalog_assets) == X4_ASSET_FAULT_CLEANUP) {
                    pending_launch = 0;
                    snprintf(ui_notice, sizeof(ui_notice), "%s", x4_tr(X4_TR_NOTICE_COVER_CLEANUP));
                } else if (!x4_catalog_assets_idle(catalog_assets)) {
                    pending_launch = 1;
                    pending_generation = ui_generation;
                } else {
                    pending_launch = 0;
                    launch_game_page = true;
                    game_source = game_page_ui.source;
                    game_title = chosen;
                }
            } else pending_launch = 0;
        }

        if (!closing && screen.page == 5 && previous_page == 5 &&
            !game_page_ui.open && !game_input_owned) {
            static uint64_t search_debounce_deadline = 0;
            uint32_t pressed = menu_pad&&catalog_browser.editing?0u:controller.pressed;
            if (!catalog_browser.editing && catalog_browser.criteria_dirty)
                x4_catalog_browser_rebuild(&catalog_browser, &catalog);
            x4_catalog_browser_ui2_library_sync(&catalog_browser, &catalog, &recent, &play_history);
            const X4Ui2LibraryCard library_card = !ui2_shell.focused && !catalog_browser.editing &&
                catalog_browser.active_tab == X4_CAT_TAB_RECENTS ?
                x4_catalog_browser_ui2_library_selection(&catalog_browser) : (X4Ui2LibraryCard){0};
#if X4_STREAM_PROFILE
            if(stream_settings_consumed) {
                pending_launch=0;
            } else
#endif
            if (catalog_browser.editing) {
                if (pressed & (ORBIS_PAD_BUTTON_R2 | ORBIS_PAD_BUTTON_CIRCLE)) {
                    catalog_browser.editing = false;
                    if (catalog_browser.criteria_dirty) {
                        x4_catalog_browser_rebuild(&catalog_browser, &catalog);
                        search_debounce_deadline = 0;
                    }
                } else {
                    if (pressed & ORBIS_PAD_BUTTON_UP)
                        x4_catalog_browser_keyboard_move(&catalog_browser, 0, -1);
                    if (pressed & ORBIS_PAD_BUTTON_DOWN)
                        x4_catalog_browser_keyboard_move(&catalog_browser, 0, 1);
                    if (pressed & ORBIS_PAD_BUTTON_LEFT)
                        x4_catalog_browser_keyboard_move(&catalog_browser, -1, 0);
                    if (pressed & ORBIS_PAD_BUTTON_RIGHT)
                        x4_catalog_browser_keyboard_move(&catalog_browser, 1, 0);
                    if (pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                        x4_catalog_browser_query_clear(&catalog_browser);
                        search_debounce_deadline = sceKernelGetProcessTime() + 150000;
                    } else if (pressed & ORBIS_PAD_BUTTON_SQUARE) {
                        x4_catalog_browser_query_delete(&catalog_browser);
                        search_debounce_deadline = sceKernelGetProcessTime() + 150000;
                    } else if (pressed & ORBIS_PAD_BUTTON_R1) {
                        x4_catalog_browser_query_append(&catalog_browser, ' ');
                        search_debounce_deadline = sceKernelGetProcessTime() + 150000;
                    } else if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                        x4_catalog_browser_query_append(&catalog_browser,
                            x4_catalog_browser_keyboard_char(&catalog_browser));
                        search_debounce_deadline = sceKernelGetProcessTime() + 150000;
                    }
                }
                if (catalog_browser.criteria_dirty &&
                    (search_debounce_deadline == 0 || sceKernelGetProcessTime() >= search_debounce_deadline)) {
                    x4_catalog_browser_rebuild(&catalog_browser, &catalog);
                    search_debounce_deadline = 0;
                }
            } else if (catalog_browser.active_tab == X4_CAT_TAB_RECENTS &&
                (catalog_browser.library.popup || catalog_browser.library.manage) &&
                (pressed & (ORBIS_PAD_BUTTON_TRIANGLE | ORBIS_PAD_BUTTON_SQUARE))) {
                /* These quick actions apply to cards, not popup choices. */
            } else if (!menu_pad && (pressed & ORBIS_PAD_BUTTON_TRIANGLE) &&
                library_card.product_id && library_card.product_id[0]) {
                ui_toggle_favorite(&favorites, &catalog_browser, library_card.product_id,
                    library_card.title, ui_notice, sizeof(ui_notice));
            } else if (!menu_pad&&(pressed & ORBIS_PAD_BUTTON_TRIANGLE) &&
                       catalog.state == X4_CATALOG_READY && !x4_auth_busy(auth)) {
                (void)x4_catalog_browser_ui2_close_selector(&catalog_browser);
                if (catalog_browser.active_tab != X4_CAT_TAB_CATALOG)
                    x4_catalog_browser_ui2_select(&catalog_browser, X4_CAT_TAB_SEARCH);
                catalog_browser.editing = true;
            } else if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && library_card.product_id &&
                library_card.product_id[0] && x4_catalog_browser_ui2_library_manage(&catalog_browser)) {
                /* The selection stays beneath the management popup. */
            } else if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && !x4_auth_busy(auth)) {
                x4_catalog_assets_retry(catalog_assets);
                pending_auth = catalog_browser.section == X4_CATALOG_SECTION_RECENT && recent.state == X4_RECENT_ERROR ?
                    X4_AUTH_XBOX_RECENTS : X4_AUTH_XBOX_CATALOG;
            } else {
                uint8_t rstick_y = controller.data.rightStick.y;
                static uint64_t next_rstick_scroll_us = 0;
                uint64_t now_us = sceKernelGetProcessTime();
                if (now_us >= next_rstick_scroll_us) {
                    if (rstick_y < 80) {
                        if (x4_catalog_browser_scroll_detail(&catalog_browser, -1)) {
                            next_rstick_scroll_us = now_us + 120000;
                        }
                    } else if (rstick_y > 175) {
                        if (x4_catalog_browser_scroll_detail(&catalog_browser, 1)) {
                            next_rstick_scroll_us = now_us + 120000;
                        }
                    }
                }
                if (pressed & ORBIS_PAD_BUTTON_L2)
                    x4_catalog_browser_ui2_cycle_chip(&catalog_browser, &catalog, -1);
                if (pressed & ORBIS_PAD_BUTTON_R2)
                    x4_catalog_browser_ui2_cycle_chip(&catalog_browser, &catalog, 1);
                if (catalog_browser.criteria_dirty)
                    x4_catalog_browser_rebuild(&catalog_browser, &catalog);
                if (pressed & ORBIS_PAD_BUTTON_UP)
                    x4_catalog_browser_apple_nav(&catalog_browser, &catalog, &recent, 0, -1, 0);
                if (pressed & ORBIS_PAD_BUTTON_DOWN)
                    x4_catalog_browser_apple_nav(&catalog_browser, &catalog, &recent, 0, 1, 0);
                if (pressed & ORBIS_PAD_BUTTON_LEFT)
                    x4_catalog_browser_apple_nav(&catalog_browser, &catalog, &recent, -1, 0, 0);
                if (pressed & ORBIS_PAD_BUTTON_RIGHT)
                    x4_catalog_browser_apple_nav(&catalog_browser, &catalog, &recent, 1, 0, 0);
                if (pressed & ORBIS_PAD_BUTTON_L1)
                    x4_catalog_browser_apple_nav(&catalog_browser, &catalog, &recent, 0, 0, -1);
                if (pressed & ORBIS_PAD_BUTTON_R1)
                    x4_catalog_browser_apple_nav(&catalog_browser, &catalog, &recent, 0, 0, 1);
                /* The old header has no interactive targets in UI 2.0. */
                catalog_browser.top_bar_focused = false;
                if (catalog_browser.open_fortnite_requested) {
                    catalog_browser.open_fortnite_requested = false;
                    fortnite_return_page = 5;
                    x4_ui2_fortnite_begin(&fortnite);
                    screen.page = 8;
                    pressed &= ~ORBIS_PAD_BUTTON_CROSS;
                }
                if (catalog_browser.open_settings_requested) {
                    catalog_browser.open_settings_requested = false;
                    if (!x4_auth_busy(auth) && pending_auth < 0 && !catalog_browser.editing &&
                        !pending_launch && !session_back && !live_retained) {
                        (void)x4_picture_settings_ui_handle(&picture_settings_ui, X4_PIC_SETTINGS_OPEN, true);
                    }
                    pressed &= ~ORBIS_PAD_BUTTON_CROSS;
                }
                if (catalog_browser.open_account_requested) {
                    catalog_browser.open_account_requested = false;
                    screen.page = 4;
                    pressed &= ~ORBIS_PAD_BUTTON_CROSS;
                }
                unsigned source_index;
                /* A fresh response or a navigation/filter press can select a
                 * row that has not been shown yet. Require a later X edge. */
                bool selection_changing = (pressed & (ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN |
                    ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT |
                    ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2)) != 0;
                if ((pressed & ORBIS_PAD_BUTTON_CROSS) && !catalog_copied && !recent_copied &&
                    !metadata_changed && !selection_changing && !picture_settings_ui.open &&
                    screen.page == 5 && pending_auth < 0 && !x4_auth_busy(auth)) {
                    ui_notice[0] = 0;
                    X4Ui2CatalogAction action = x4_catalog_browser_ui2_activate(&catalog_browser, &catalog);
                    if (action == X4_UI2_CATALOG_ACT_KEYBOARD)
                        catalog_browser.editing = true;
                    else if (action == X4_UI2_CATALOG_ACT_FAVORITE) {
                        const X4CatalogTitle *hero = x4_catalog_browser_ui2_hero(&catalog_browser, &catalog);
                        if (hero)
                            ui_toggle_favorite(&favorites, &catalog_browser, hero->product_id,
                                hero->name, ui_notice, sizeof(ui_notice));
                    }
                    else if (action == X4_UI2_CATALOG_ACT_REMOVE_FAVORITE) {
                        if (library_card.favorite && library_card.product_id)
                            ui_toggle_favorite(&favorites, &catalog_browser, library_card.product_id,
                                library_card.title, ui_notice, sizeof(ui_notice));
                    }
                    else if (action == X4_UI2_CATALOG_ACT_NONE) {
                        if (x4_catalog_browser_resolved_index(&catalog_browser, &catalog, &recent, &source_index)) {
                            game_page_open(&game_page_ui, 5, source_index, &catalog.titles[source_index], NULL);
                        } else if (catalog_browser.active_tab == X4_CAT_TAB_RECENTS) {
                            if ((library_card.product_id && library_card.product_id[0]) ||
                                (library_card.title_id && library_card.title_id[0])) {
                                X4RecentGame item = {0};
                                if (library_card.product_id)
                                    snprintf(item.product_id, sizeof(item.product_id), "%s", library_card.product_id);
                                if (library_card.title)
                                    snprintf(item.name, sizeof(item.name), "%s", library_card.title);
                                if (library_card.title_id)
                                    snprintf(item.id, sizeof(item.id), "%s", library_card.title_id);
                                game_page_open(&game_page_ui, 5, X4_CATALOG_MAX, NULL, &item);
                            }
                        }
                    }
                }
            }
        }
        if (launch_game_page && (screen.page == 5 || screen.page == 0 || screen.page == 4 ||
            (screen.page == 8 && previous_page == 8 && !picture_settings_ui.open))) {
                    const bool launching_fortnite = screen.page == 8;
                    /* The visible filtered ordinal is never a session index. */
                    catalog_selected = game_source;
                    ui_history_begin(&local_session, game_title->product_id);
                    live_presented = false;
                    live_error = 0;
#if X4_STREAM_PROFILE
                    video_start_error = false;
#if !X4_PUBLIC
                    stream_received_until=0;
#endif
                    /* One immutable main-thread copy binds media and /play
                     * to the same confirmed menu selection for this session. */
                    X4StreamRequestConfig request_cfg=x4_stream_settings_get();
#endif
                    if (live) {
                        trace_inactive(live);
                        live_error = x4_auth_set_media_callback(auth, NULL, NULL, NULL);
#if X4_STREAM_PROFILE && X4_RUMBLE
                        if(!live_error)interaction_rumble_stop(&rumble_state,primary.handle);
#endif
                        if (!live_error) live_error = x4_live_media_close(live);
                        if (!live_error) live = NULL;
                    }
                    if (!live_error) {
                        live_retained = 0;
#if X4_STREAM_PROFILE
                        interaction_main_begin(request_cfg.interaction_modes,sceKernelGetProcessTime());
                        x4_rtc_input_metrics_reset();
                        live = x4_live_media_create_request(&live_error,&request_cfg);
                        memset(reconnect_notice_key,0,sizeof(reconnect_notice_key));
#else
                        live = x4_live_media_create(&live_error);
#endif
                        diagnostic_stable = false;
                        diagnostic_mark = 0;
                        if (live) {
                            live_error = x4_live_media_set_payload_type(live, X4_LIVE_KIND_VIDEO, 102);
                            if (!live_error) live_error = x4_live_media_set_payload_type(live, X4_LIVE_KIND_AUDIO, 111);
                            if (!live_error) live_error = x4_live_media_start(live);
                        }
                    }
                    memset(&live_status, 0, sizeof(live_status));
                    live_muted = 0;
                    rc = live_error;
                    if (!rc) rc = x4_auth_set_media_callback(auth, x4_live_media_receive, live, x4_live_media_trace(live));
                    if (!rc) rc = x4_auth_set_grace_inbox(auth,x4_live_media_grace_inbox(live));
#if X4_STREAM_PROFILE
                    if (!rc) rc = x4_auth_set_stream_startup(auth,x4_live_media_stream_startup(live));
                    if (!rc) rc = x4_auth_set_keyframe_limiter(auth,x4_live_media_keyframe_limiter(live));
#endif
                    if (!rc) { game_controls = 0;
                        bool multi=x4_gamepad_match_option(&pad_option,x4_picture_settings_multiplayer(),true);
                        if(pads)x4_controller_roster_configure(pads,multi,true);
                        X4GamepadRoster initial=gamepad_snapshot(&pad_view,&(X4GamepadFrame){0},
                            multi,false,controller_read_at);
                        x4_auth_set_gamepad(auth,&initial);
                        mic_begin_match(&touch_menu);
#if X4_STREAM_PROFILE
                        rc = x4_auth_start_session_request(auth,catalog_selected,&request_cfg);
#else
                        rc = x4_auth_start_session(auth, catalog_selected);
#endif
                    }
                    if (!rc) cloud_force = true;
                    else {
                        mic_end_match(&touch_menu);
                        ui_history_finish(&play_history, &local_session, input_at);
                    }
                    if (rc && !live_error) live_error = rc;
#if X4_STREAM_PROFILE
                    loading_begin_cached(catalog_assets,catalog_selected<catalog.count?&catalog.titles[catalog_selected]:NULL,sceKernelGetProcessTime());
#endif
                    x4_auth_session_snapshot(auth, &session);
                    if (rc == 0 || session.state == X4_SESSION_ERROR || live_error) {
                        session_back = 0;
                        screen.page = 6;
                        is_fortnite_session = launching_fortnite;
                    }
                    if (rc == X4_AUTH_E_SIGNED_OUT) screen.page = 4;
                    if (rc) printf("XCloud4: solicitar sesion fallo 0x%08x\n", (unsigned)rc);
        }
        if (!closing && screen.page == 8 && previous_page != 8 &&
            account.state != X4_AUTH_AUTHORIZED) {
            fortnite_login_flow = true;
            screen.page = 4;
            pending_auth = X4_AUTH_SIGN_IN;
        }
        if (!closing && screen.page == 8 && !picture_settings_ui.open &&
            !game_page_ui.open && !game_input_owned && !access_card_visible && !access_query &&
            fortnite.phase == X4_UI2_FORTNITE_READY &&
            catalog.state == X4_CATALOG_READY && catalog.count > 0 &&
            x4_ui2_fortnite_is_product(catalog.titles[0].product_id) &&
            !x4_auth_busy(auth) && pending_auth < 0) {
            ui_notice[0] = 0;
            game_page_open(&game_page_ui, 8, 0, &catalog.titles[0], NULL);
        }
        if (game_page_ui.open && screen.page != game_page_ui.origin)
            game_page_close(&game_page_ui, catalog_assets);
        if (screen.page != cloud_launch.page || (!cloud_launch.open && !pending_launch))
            cloud_launch = (CloudLaunchNotice){0};
        if (screen.page == 5 && !catalog_browser.editing && catalog_browser.criteria_dirty)
            x4_catalog_browser_rebuild(&catalog_browser, &catalog);
        enum X4CatalogTab browse_tab = catalog_browser.active_tab;
        if (screen.page == 5)
            x4_catalog_browser_ui2_library_sync(&catalog_browser, &catalog, &recent, &play_history);
        bool secondary_tab = browse_tab != X4_CAT_TAB_DISCOVER;
        bool asset_tab_ready = browse_tab == X4_CAT_TAB_RECENTS ?
            recent.state == X4_RECENT_READY || catalog_browser.library.members != 0 :
            catalog.state == X4_CATALOG_READY;
        bool asset_menu_ready = !closing && screen.page==0 && !x4_auth_busy(auth) &&
            pending_auth<0 && !pending_launch && !game_page_ui.open;
        if (asset_menu_ready) {
            X4CatalogArtDemand art[X4_CATALOG_PRELOAD_PRODUCTS]={0};
            size_t count=0;
            menu_art(art,&count,&catalog_browser,&catalog);
            X4Ui2HomeModel visible_home;
            x4_ui2_home_model(&visible_home, screen.signed_in, &catalog, &recent, &catalog_browser.home_rows);
            if (visible_home.continuation < catalog.count)
                add_art(art, &count, catalog.titles[visible_home.continuation].product_id, 1u<<X4_COVER_ROW);
            for (unsigned i = 0; i < visible_home.recent_count; ++i)
                if (visible_home.recent[i] < catalog.count)
                    add_art(art, &count, catalog.titles[visible_home.recent[i]].product_id, 1u<<X4_COVER_ROW);
            for (unsigned i = 0; i < visible_home.popular_count; ++i)
                if (visible_home.popular[i] < catalog.count)
                    add_art(art, &count, catalog.titles[visible_home.popular[i]].product_id, 1u<<X4_COVER_ROW);
            if(catalog.state==X4_CATALOG_READY)
                discover_art(art,&count,&catalog_browser,&catalog,catalog_browser.home_first_row,false,
                    discover_opening_background(&catalog_browser,&catalog,catalog_browser.home_row,
                        catalog_browser.discover_row_selected));
            if(x4_catalog_assets_preload(catalog_assets,art,count))cover_preload_active=count!=0;
        } else if(cover_preload_active&&x4_catalog_assets_preload(catalog_assets,NULL,0)) {
            cover_preload_active=false;
        }
        if ((asset_menu_ready||screen.page==5)&&catalog.state==X4_CATALOG_READY) {
            X4CatalogArtDemand art[X4_CATALOG_PRELOAD_PRODUCTS]={0};
            size_t count=0;
            menu_art(art,&count,&catalog_browser,&catalog);
            unsigned last;
            if (screen.page == 0 && x4_home_continue_index(&catalog,&recent,&last))
                add_art(art,&count,catalog.titles[last].product_id,1u<<X4_COVER_ROW);
            discover_art(art,&count,&catalog_browser,&catalog,0,true,
                discover_opening_background(&catalog_browser,&catalog,0,0));
            x4_catalog_assets_protect(catalog_assets,art,count);
        }
        if (screen.page == 5 && browse_tab == X4_CAT_TAB_DISCOVER) {
            bool idle_discover = !closing && !game_page_ui.open && !catalog_browser.editing &&
                !picture_settings_ui.open && !cloud_launch.open && !update_notice.open &&
                !x4_auth_busy(auth) && pending_auth < 0 && !pending_launch && !ui2_shell.focused;
#if X4_STREAM_PROFILE && !X4_PUBLIC
            idle_discover = idle_discover && !stream_settings_ui.open;
#endif
            (void)x4_catalog_browser_ui2_carousel_tick(&catalog_browser, &catalog,
                sceKernelGetProcessTime(), !idle_discover || controller.data.buttons != 0);
        } else if (previous_page == 5) catalog_browser.ui2_carousel_since = 0;
        if (game_page_ui.open && !closing && !x4_auth_busy(auth) && pending_auth < 0 && !pending_launch) {
            const char *product = game_page_ui.id[0] && game_page_ui.product[0] ? game_page_ui.product : NULL;
            x4_catalog_assets_request_surfaces(catalog_assets, NULL, NULL, 0);
            x4_catalog_assets_game_request(catalog_assets, product,
                x4_game_page_fullscreen_index(&game_page_ui.page));
            x4_catalog_assets_request_background(catalog_assets, product);
            x4_catalog_assets_resume(catalog_assets);
            home_background_requested = false;
        } else if (!closing && screen.page == 5 && asset_tab_ready && !x4_auth_busy(auth) &&
            pending_auth < 0 && !pending_launch) {
            char wanted[X4_CATALOG_ASSET_SLOTS][X4_PRODUCT_ID_CAP];
            const char *ids[X4_CATALOG_ASSET_SLOTS];
            uint32_t masks[X4_CATALOG_ASSET_SLOTS];
            size_t wanted_count = x4_catalog_presentation_demand_surfaces(&presentation,
                &catalog_browser, &catalog, &recent, wanted, masks);
            for (size_t i = 0; i < wanted_count; ++i) ids[i] = wanted[i];
            x4_catalog_assets_request_surfaces(catalog_assets, ids, masks, wanted_count);
            if (!catalog_browser.editing) {
                uint64_t now = sceKernelGetProcessTime();
                const char *background = NULL;
                if (!secondary_tab) {
                    (void)x4_catalog_browser_home_tick(&catalog_browser, &catalog, now);
                    background = catalog_browser.home_background_id[0] ? catalog_browser.home_background_id : NULL;
                }
                /* The worker retains an active slot across request changes.
                 * Clear the preceding tab's active slot before a new request. */
                if (background && home_background_requested && background_request_tab != browse_tab)
                    x4_catalog_assets_request_background(catalog_assets, NULL);
                x4_catalog_assets_request_background(catalog_assets, background);
                home_background_requested = background != NULL;
                background_request_tab = browse_tab;
            }
            x4_catalog_assets_resume(catalog_assets);
        } else if (!closing && (screen.page == 0 || screen.page == 8) && !x4_auth_busy(auth) && pending_auth < 0 && !pending_launch) {
            const char *home_ids[2];
            uint32_t home_masks[2];
            const char *featured_id = NULL;
            if (screen.page == 0 && catalog.state == X4_CATALOG_READY && catalog_browser.featured_count > 0) {
                unsigned fsrc = catalog_browser.featured_sources[0];
                if (fsrc < catalog.count) featured_id = catalog.titles[fsrc].product_id;
            }
            size_t home_count = x4_screen_cover_demand(screen.page, featured_id, home_ids, home_masks);
            x4_catalog_assets_request_surfaces(catalog_assets, home_ids, home_masks, home_count);
            x4_catalog_assets_resume(catalog_assets);
        } else x4_catalog_assets_suspend(catalog_assets);
        if (home_background_requested && (screen.page != 5 || !asset_tab_ready ||
            background_request_tab != browse_tab || (!secondary_tab && catalog_browser.editing))) {
            x4_catalog_assets_request_background(catalog_assets, NULL);
            home_background_requested = false;
        }
        if (home_canvas_valid && (screen.page != 5 || (secondary_tab && catalog_copied))) {
            x4_catalog_gui_home_invalidate();
            home_canvas_valid = false;
        }
        if (live && !live_retained) {
            x4_live_media_tick(live);
            x4_live_media_snapshot(live, &live_status);
            /* The profile path presents directly through VideoOut and bypasses
             * the menu canvas. Count only a confirmed game presentation. */
            if (live_status.video_present_calls)
                ui_history_first_image(&play_history, &local_session, input_at);
            if (x4_auth_busy(auth)) {
                if (x4_live_media_take_keyframe_request(live)) x4_auth_request_keyframe(auth);
                if (live_status.video_error) {
                    mic_end_match(&touch_menu);
#if X4_STREAM_PROFILE
                    if(!live_error)live_error=live_status.video_error;
                    stream_halt_video_error(live, &live_status, live_error, &video_start_error);
#else
                    trace_inactive(live); x4_auth_cancel(auth);
#endif
                }
                if (screen.page == 6 && (controller.pressed & ORBIS_PAD_BUTTON_SQUARE) && !game_input) {
                    live_muted = !live_muted;
                    x4_live_media_set_muted(live, live_muted != 0);
                }
            } else {
                /* The worker closes RTC before publishing completion. */
                trace_inactive(live);
                int stop_rc = x4_auth_set_media_callback(auth, NULL, NULL, NULL);
#if X4_STREAM_PROFILE && X4_RUMBLE
                if(!stop_rc)interaction_rumble_stop(&rumble_state,primary.handle);
#endif
                if (!stop_rc) stop_rc = x4_live_media_close(live);
                if (!stop_rc) live = NULL;
                else { live_error = stop_rc; live_retained = 1; printf("XCloud4: medios retienen recursos, cierre 0x%08x\n", (unsigned)stop_rc); }
#if X4_STREAM_PROFILE
                /* Successful full close is also proof that stream VideoOut
                 * released its ownership before the context was freed. */
                if (!stop_rc) live_status.video_display_detached = true;
#endif
            }
        }
        if (local_session.active && (screen.page != 6 || !x4_auth_busy(auth)))
            ui_history_finish(&play_history, &local_session, input_at);
#if X4_STREAM_PROFILE
        if(!live&&screen.page!=6&&x4_loading_screen_active())x4_loading_screen_end();
        /* The stream close joins CPU readers and closes its VideoOut before
         * UI ownership can be restored. Never reopen over retained resources. */
        if(stream_display_active && live_status.video_display_detached){
            rc=x4_display_open(&display);
            if(rc<0){printf("XCloud4: reabrir pantalla UI fallo 0x%08x; no se puede dibujar el aviso, cierre seguro\n",(unsigned)rc);break;}
            stream_display_active=false;ui_state_sig=0;ui_dirty_buffers=2;
            x4_picture_settings_ui_cache_invalidate();x4_catalog_gui_keyboard_cache_invalidate();
        }
#endif
        if (screen.page != 3 && media_active) {
            x4_audio_stop(&audio);
            x4_video_stop(&video);
            media_active = 0;
        }
        if (screen.page == 3) {
            if (!media_active || (controller.pressed & ORBIS_PAD_BUTTON_CROSS)) {
                /* Present the menu before entering potentially slow system APIs. */
                x4_screen_draw(&screen, &controller, x4_display_pixels(&display));
                x4_media_draw(&video, &audio, x4_display_pixels(&display));
                rc = x4_display_present(&display);
                if (rc < 0) break;
                ui2_splash_presented(&splash);
                x4_audio_stop(&audio);
                x4_video_start(&video);
                x4_audio_start(&audio);
                media_active = 1;
            }
            if (controller.pressed & ORBIS_PAD_BUTTON_SQUARE)
                atomic_store(&audio.muted, !atomic_load(&audio.muted));
            x4_video_tick(&video);
        }
        /* Keep scanout unchanged between completed pictures. A real UI
         * transition or periodic status refresh still redraws the entire
         * inactive framebuffer, so two-buffer reuse never mixes old pixels. */
        bool live_visible = screen.page == 6 && live && x4_auth_busy(auth) && !closing && !session_back &&
            !live_error && !live_status.video_error && session.state != X4_SESSION_STOPPING &&
            live_status.video_ready;
        bool loading_transition=false;
        X4Trace *live_trace = live ? x4_live_media_trace(live) : NULL;
        if (!closing && screen.page == 6 && live && x4_auth_busy(auth)) {
            if (live_status.video_ready && !video_banner_until_us) {
                video_banner_until_us = controller_read_at + 6000000ULL;
            }
        } else {
            video_banner_until_us = 0;
        }
        bool show_banner = (video_banner_until_us != 0 && controller_read_at < video_banner_until_us);
        X4MicSnapshot mic_snapshot;
        const X4MicSnapshot *mic=NULL;
        const char *mic_notice=NULL;
        bool mic_show_level=false;
        if (touch_menu.mic_enabled) {
            mic_snapshot=x4_mic_snapshot();
            mic=&mic_snapshot;
            bool unavailable=mic->status==X4_MIC_UNAVAILABLE||mic->status==X4_MIC_REJECTED;
            if (live_visible && mic->status!=previous_mic_status) {
                previous_mic_status=mic->status;
                mic_notice_until=unavailable?input_at+3000000u:0;
            }
            if (unavailable && input_at<mic_notice_until)
                mic_notice=x4_tr(mic->status==X4_MIC_REJECTED?X4_TR_MIC_REJECTED:X4_TR_MIC_UNAVAILABLE);
#if !X4_PUBLIC
            /* Single-pad sessions retain T41's Xbox button delivery. The Lab
             * only observes this chord to toggle a local microphone meter. */
            const uint32_t mic_chord=ORBIS_PAD_BUTTON_L3|ORBIS_PAD_BUTTON_R3;
            if (live_visible && game_input && !touch_menu.is_open && (controller.data.buttons&mic_chord)==mic_chord &&
                (controller.pressed&mic_chord)) mic_meter_requested=!mic_meter_requested;
            mic_show_level=mic->active && (mic_meter_requested||x4_net_hud_mode()>=X4_NET_HUD_BASIC);
#endif
        } else if (previous_mic_status!=X4_MIC_OFF) {
            previous_mic_status=X4_MIC_OFF;
            mic_notice_until=0;
#if !X4_PUBLIC
            mic_meter_requested=false;
#endif
        }
        bool trace_active = live_visible && !live_retained && trace_session_active(session.state);
        x4_trace_set_active(live_trace, trace_active);
        /* Runs before the idle continue too: a repeated/status-only refresh
         * must not conceal a gap in actual NEW live completions. */
        x4_trace_poll(live_trace);
#if X4_STREAM_PROFILE
        /* video_ready is published only after a valid native output, not RTP,
         * SPS, Query or Create. Fade the UI by its own clock before handing
         * VideoOut to streaming: a single picture is enough, even if the next
         * AU never arrives. The decoder and normal latest-ready ring continue. */
        if(live_visible&&!stream_display_active&&x4_loading_screen_active()){
            uint64_t transition_at=sceKernelGetProcessTime();
            x4_loading_screen_first_picture(transition_at);
            loading_transition=!x4_loading_screen_transition_ready(transition_at);
            if(!loading_transition)x4_loading_screen_end();
        }
        if(live_visible&&!stream_display_active&&!loading_transition){
            x4_ui_draw_metrics_flush(sceKernelGetProcessTime());
            x4_picture_settings_ui_cache_invalidate();x4_catalog_gui_keyboard_cache_invalidate();
            rc=x4_display_close_checked(&display);
            if(rc){
                mic_end_match(&touch_menu);
                live_error=rc;trace_inactive(live);x4_auth_cancel(auth);
                printf("XCloud4: cerrar pantalla UI fallo 0x%08x; recursos conservados, no se puede dibujar el aviso, cierre seguro\n",(unsigned)rc);
                break;
            }
            rc=x4_live_media_stream_display_open(live);
            if(rc){
                mic_end_match(&touch_menu);
                live_error=rc;
                printf("XCloud4: transicion VideoOut fallo 0x%08x\n",(unsigned)rc);
                /* UI may have relinquished ownership even if the new open
                 * failed. Only confirmed video detach permits UI reopen. */
                stream_display_active=true;
                stream_halt_video_error(live, &live_status, live_error, &video_start_error);
            } else stream_display_active=true;
        }
        if(screen.page==6&&x4_loading_screen_active())x4_loading_screen_update(&session,input_at);
        if(stream_display_active){
            if(live&&!live_retained&&!live_error){
                uint64_t menu_prepare_begin=sceKernelGetProcessTime();
                X4TouchMenuCacheStats before_menu,after_menu;
                x4_touch_menu_cache_snapshot(&before_menu);
                (void)x4_touch_menu_prepare(&touch_menu,live_muted);
                x4_touch_menu_cache_snapshot(&after_menu);
                if(after_menu.regenerations!=before_menu.regenerations)
                    x4_ui_perf_record(&menu_prepare_times,sceKernelGetProcessTime()-menu_prepare_begin);
                x4_live_media_stream_note_menu_open(live,touch_menu.is_open);
                uint64_t hud_prepare_begin=sceKernelGetProcessTime();
                if(x4_net_hud_prepare(live,&live_status,&session,live_muted,input_at))
                    x4_net_hud_note_compose_us(sceKernelGetProcessTime()-hud_prepare_begin);
#if !X4_PUBLIC
                if(!stream_received_until&&live_status.video_color_ready)
                    stream_received_until=input_at+6000000;
#endif
                StreamOverlayContext overlay={&live_status,&session,live_muted,&touch_menu,show_banner,
#if X4_PUBLIC
                    false
#else
                    live_status.video_color_ready&&input_at<stream_received_until
#endif
                    ,true,input_at<pad_notice_until?pad_notice:NULL,cloud_time,cloud_alert,mic,mic_notice,mic_show_level
                };
#if X4_STREAM_PROFILE
                interaction_phase(X4_MON_PHASE_DRAW,sceKernelGetProcessTime());
#endif
                int stream_rc=x4_live_media_stream_present(live,stream_overlay,&overlay);
                /* Refresh notices on a frozen frame only when their visible
                 * state changes. A busy scanout retries next main iteration. */
                unsigned idle_seconds=session.inactivity_until_us>input_at?
                    (unsigned)((session.inactivity_until_us-input_at+999999u)/1000000u):0;
                int notice_key[9]={session.reconnecting,(int)session.reconnect_attempt,
                    session.recovered_until_us>input_at,(int)idle_seconds,
                    touch_menu.is_open,touch_menu.selected_index,input_at<pad_notice_until?pad_notice_serial:0,
                    (int)cloud_alert,mic?((mic->active?1:0)|(mic_notice?2:0)|
                    (touch_menu.is_open?(mic->muted?4:8):0)):0};
                bool notice_active=notice_key[0]||notice_key[2]||notice_key[3]||notice_key[6]||notice_key[7]||notice_key[8];
                bool previous_notice=reconnect_notice_key[0]||reconnect_notice_key[2]||
                    reconnect_notice_key[3]||reconnect_notice_key[6]||reconnect_notice_key[7]||reconnect_notice_key[8];
                if(stream_rc>=0&&(notice_active||previous_notice)&&
                    memcmp(notice_key,reconnect_notice_key,sizeof(notice_key))) {
                    StreamOverlayContext frozen_overlay=overlay;
                    frozen_overlay.net_hud_enabled=false;
                    int redraw_rc=x4_live_media_stream_redraw_notice(live,stream_overlay,&frozen_overlay);
                    if(redraw_rc==0)memcpy(reconnect_notice_key,notice_key,sizeof(notice_key));
                    else if(redraw_rc==-ENOMEM) {
                        printf("XCloud4: aviso congelado sin memoria rc=%d; reconexion continua\n",redraw_rc);
                        memcpy(reconnect_notice_key,notice_key,sizeof(notice_key));
                    } else if(redraw_rc<0)stream_rc=redraw_rc;
                }
                if(stream_rc<0&&!live_error){
                    mic_end_match(&touch_menu);
                    live_error=stream_rc;
                    printf("XCloud4: presentacion stream fallo 0x%08x\n",(unsigned)stream_rc);
                    stream_halt_video_error(live, &live_status, live_error, &video_start_error);
                }
            }
            /* Same-owner state needs no new lock. Flip completion has no wake
             * event here: observe it and read the pad at 1 ms while anything
             * is in flight; only fully idle presentation uses 5 ms. */
            unsigned main_in_flight=x4_live_media_stream_main_state(live);
            main_submit_done();
            interaction_phase(X4_MON_PHASE_FLIP_WAIT,sceKernelGetProcessTime());
            int wait_rc=x4_live_media_stream_wait(live,main_in_flight?1000u:5000u);
            if(wait_rc<0){
                if(!live_error){
                    mic_end_match(&touch_menu);
                    live_error=wait_rc;
                    printf("XCloud4: espera principal stream fallo 0x%08x\n",(unsigned)wait_rc);
                    stream_halt_video_error(live,&live_status,live_error,&video_start_error);
                }
                /* A broken wait endpoint cannot service an error screen.
                 * Enter the existing safe close; never poll a failed wait. */
                trace_inactive(live);x4_auth_cancel(auth);break;
            }
            continue;
        }
#endif
        int next_overlay_key[11] = {screen.page, live_muted, live_status.audio_error,
            live_status.audio_playing, session.input_error, session.input_ready,
            session.rtc_connected, session.state, screen.exit_error, controller.data.connected,
            (touch_menu.is_open ? (10 + touch_menu.selected_index) : 0) + (show_banner ? 100 : 0)};
        uint64_t draw_at = sceKernelGetProcessTime();
        x4_ui_draw_metrics_poll(draw_at,ui_dirty_skips);
        bool overlay_changed = memcmp(overlay_key, next_overlay_key, sizeof(overlay_key)) != 0;
        if (live_visible && live_presented && !overlay_changed && draw_at - overlay_at < 500000 &&
            !x4_live_media_has_new_picture(live)) {
            ++idle_present_skips;
            x4_trace_monitor_state(live_trace, X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAIN_IDLE, 0);
#if X4_STREAM_PROFILE
            interaction_phase(X4_MON_PHASE_MAIN_IDLE,sceKernelGetProcessTime());
#endif
            main_submit_done();
            sceKernelUsleep(2000);
            continue;
        }
        x4_trace_monitor_state(live_trace, X4_MON_ACTOR_MAIN, X4_MON_PHASE_DRAW, 0);
#if X4_STREAM_PROFILE
        interaction_phase(X4_MON_PHASE_DRAW,sceKernelGetProcessTime());
#endif
        /* Spinner advances at 10 Hz only on waiting pages. Dirty-buffer
         * hashing still skips unchanged static screens. */
        bool ui_animated = !live_visible && (closing ||
            (screen.page == 4 && (x4_auth_busy(auth) || account.state == X4_AUTH_WAITING || account.state == X4_AUTH_CONNECTING)) ||
            (screen.page == 8 && x4_ui2_fortnite_waiting(&fortnite)) ||
            (screen.page == 5 && (x4_auth_busy(auth) || catalog.state == X4_CATALOG_LOADING)) ||
            (screen.page == 6 && x4_auth_busy(auth)));
        x4_apple_set_animation_phase(ui_animated ? (unsigned)(draw_at / 100000u) : 0);
        /* Offer only after this frame's navigation/panel/auth actions. An
         * existing release result supplies the version and validated tag;
         * this UI path performs no HTTP request. */
        const bool update_page = screen.page == 0 ||
#ifdef X4_UPDATE51_ENABLED
            (update_manual && screen.page == 2) ||
#endif
            (screen.page == 5 &&
            catalog_browser.active_tab == X4_CAT_TAB_DISCOVER && !catalog_browser.editing &&
            !access_card_visible && !access_query);
        bool update_home_idle = !closing && !screen.exit_requested && update_page &&
            !picture_settings_ui.open && !game_page_ui.open && !x4_server_settings_awaiting_first_choice() &&
            !screen.open_settings_requested && !screen.explore_catalog_requested &&
            !fortnite_login_flow && pending_auth < 0 && !pending_launch &&
            !x4_auth_busy(auth) && !live_retained && !session_back;
#ifdef X4_UPDATE51_ENABLED
        update_home_idle = update_home_idle && !update_progress_open;
#endif
#if X4_STREAM_PROFILE && !X4_PUBLIC
        update_home_idle = update_home_idle && !stream_settings_ui.open;
#endif
#ifdef X4_UPDATE51_ENABLED
        if (update_home_idle && !update_notice.open && !screen.exit_error &&
            !cloud_launch.open && !remember_notice && !logout_notice && x4_update51_take_manual_offer()) {
            update_rejected = false;
            update_progress_open = true;
            update_home_idle = false;
        }
#endif
#if X4_STREAM_PROFILE && !X4_PUBLIC
        if (update_test_requested && update_home_idle && !update_notice.open) {
            if (x4_apple_update_notice_offer(&update_notice,"9.9.9",NULL,true,true))
                update_test_requested = false;
        } else
#endif
        if (update_home_idle && !update_notice.open && !update_notice.shown &&
            x4_release_check_status(NULL,0) == X4_UPDATE_AVAILABLE) {
            char version[32], tag[64];
            (void)x4_release_check_status(version,sizeof(version));
            bool valid_tag = x4_release_check_tag(tag,sizeof(tag));
            if (x4_apple_update_notice_offer(&update_notice,version,valid_tag ? tag : NULL,true,false)) {
#ifdef X4_UPDATE51_ENABLED
                update_notice.can_install = valid_tag && x4_release_check_asset(X4_UPDATE51_MANIFEST) &&
                    x4_release_check_asset(X4_UPDATE51_SIGNATURE);
                update_notice.install_focused = update_notice.can_install;
                update_manual = false;
#endif
            }
        }
        const bool settings_page = picture_settings_ui.open && !picture_settings_ui.first_time_mode;
        bool lab_page = false, lab_modal = false;
#if X4_STREAM_PROFILE && !X4_PUBLIC
        lab_page = stream_settings_ui.open;
        lab_modal = lab_page && x4_stream_settings_ui_warning_open();
#endif
        const bool access_modal = !picture_settings_ui.open && !lab_page &&
            (screen.page == 5 || screen.page == 8) && (access_card_visible || access_query);
        const bool settings_modal = x4_picture_settings_ui_has_modal(&picture_settings_ui);
        const bool ui_modal = !live_visible && (screen.exit_error || update_notice.open ||
#ifdef X4_UPDATE51_ENABLED
            update_progress_open ||
#endif
            cloud_launch.open || remember_notice || logout_notice || access_modal || settings_modal || lab_modal);
        const int underlay = settings_page ? 10 : lab_page ? 11 :
            game_page_ui.open ? 12 + screen.page : screen.page;
        const bool reuse_modal_background = ui_modal && modal_latched && modal_underlay == underlay;
        if (modal_latched && !reuse_modal_background) {
            x4_catalog_gui_home_invalidate();
            x4_ui2_home_invalidate();
            x4_game_page_cache_invalidate();
            x4_picture_settings_ui_cache_invalidate();
            home_canvas_valid = menu_canvas_valid = false;
            account_canvas_valid = settings_canvas_valid = false;
            shell_presented = false;
        }
        if (!settings_page) settings_canvas_valid = false;
        if (screen.page != 4 || game_page_ui.open || settings_page || lab_page)
            account_canvas_valid = false;
        bool tabs_canvas = !closing && screen.page == 5 && !live_visible && !game_page_ui.open &&
            (catalog.state == X4_CATALOG_READY || browse_tab == X4_CAT_TAB_RECENTS) &&
            (!catalog_browser.editing || secondary_tab) && !settings_page && !lab_page;
        bool menu_frame = !closing && screen.page == 0 && !live_visible && !game_page_ui.open &&
            !settings_page && !lab_page;
        bool game_page_frame = !closing && !live_visible && game_page_ui.open &&
            !settings_page && !lab_page;
#if !X4_PUBLIC
        char home_toast[128];
        bool home_has_toast = (tabs_canvas || menu_frame || game_page_frame) &&
            x4_script_engine_get_notification(home_toast, sizeof(home_toast));
        if (home_has_toast) tabs_canvas = menu_frame = game_page_frame = false;
        bool keyboard_toast = home_has_toast && secondary_tab && catalog_browser.editing;
        if (keyboard_toast) x4_catalog_gui_keyboard_cache_invalidate();
#if X4_STREAM_PROFILE
        if (stream_settings_ui.open) tabs_canvas = menu_frame = game_page_frame = false;
#endif
#endif
        bool tabs_frame = tabs_canvas && !catalog_browser.editing;
        const unsigned discover_fade = x4_catalog_gui_discover_clock(
            tabs_frame && browse_tab == X4_CAT_TAB_DISCOVER ? draw_at : 0);
#if !X4_PUBLIC
        enum CoverScreen next_cover_screen = screen.page==0?COVER_SCREEN_MENU:
            screen.page==5&&browse_tab==X4_CAT_TAB_DISCOVER?COVER_SCREEN_DISCOVER:
            screen.page==5&&browse_tab==X4_CAT_TAB_CATALOG?COVER_SCREEN_CATALOG:COVER_SCREEN_NONE;
        if(next_cover_screen!=cover_screen){
            cover_screen=next_cover_screen;
            cover_screen_shown=0;
            cover_screen_reported=false;
        }
        bool cover_screen_visible=cover_screen==COVER_SCREEN_MENU?menu_frame:
            cover_screen!=COVER_SCREEN_NONE&&!closing&&!live_visible&&!picture_settings_ui.open&&
            !catalog_browser.editing&&!screen.exit_error;
#endif
        if (!tabs_canvas && home_canvas_valid) {
            x4_catalog_gui_home_invalidate();
            home_canvas_valid = false;
        }
        if (!menu_frame && menu_canvas_valid) {
            x4_screen_home_invalidate();
            menu_canvas_valid = false;
        }
        if (!game_page_frame) x4_game_page_cache_invalidate();
        if (!live_visible) {
            X4Ui2Section section = settings_page ? X4_UI2_SETTINGS :
                ui2_section_for(&screen, &catalog_browser);
            ui2_shell.section = section;
            if (!ui2_shell.focused) ui2_shell.selected = section;
            X4StreamRequestConfig request = x4_stream_settings_get();
#if X4_PUBLIC
            request = x4_stream_request_config(x4_picture_settings_current_profile(), X4_COLOR_AUTO_709);
#endif
            if (x4_ui2_status_set(&ui2_status, x4_ui2_network_type(draw_at),
                    ui_status_quality(request.profile), x4_ui2_local_minute(draw_at),
                    screen.signed_in ? catalog.gamertag : NULL)) ++status_revision;
        }
        bool ui_reuse_work=false;
        if (!live_visible && screen.page != 3 && screen.page != 6) {
            uint32_t next_ui_sig = 2166136261u;
#define UI_HASH(v) do { next_ui_sig ^= (uint32_t)(v); next_ui_sig *= 16777619u; } while (0)
            UI_HASH(screen.page);
            UI_HASH(ui2_shell.section);
            UI_HASH(ui2_shell.selected);
            UI_HASH(ui2_shell.focused);
            UI_HASH(status_revision);
            UI_HASH(home_choice);
            if (screen.page == 0) {
                UI_HASH(catalog_cursor.revision);
                UI_HASH(recent_cursor.revision);
                UI_HASH(home_sigl_revision);
                UI_HASH(play_history.revision);
                /* The status avatar tracks only the initial; the greeting uses the whole name. */
                if (screen.signed_in)
                    for (size_t i = 0; i < sizeof(catalog.gamertag) && catalog.gamertag[i]; ++i)
                        UI_HASH((unsigned char)catalog.gamertag[i]);
            }
            UI_HASH(recent.state);
            UI_HASH(recent.generation);
            if (screen.page == 0 || screen.page == 2) UI_HASH(x4_release_check_status(NULL, 0));
            if (screen.page == 0 || screen.page == 5) UI_HASH(x4_lang_current());
            UI_HASH(screen.selected);
            if (screen.page == 8) {
                UI_HASH(fortnite.phase);
                UI_HASH(fortnite.error);
                UI_HASH(x4_lang_current());
            }
            UI_HASH(screen.signed_in);
            UI_HASH(screen.exit_error);
            UI_HASH(cloud_launch.open);
            UI_HASH(ui_modal);
            UI_HASH(access_modal);
            UI_HASH(game_page_ui.open);
            if (game_page_ui.open) {
                UI_HASH(game_page_ui.redraw);
                UI_HASH(game_page_ui.background_state);
                UI_HASH(x4_catalog_assets_game_revision(catalog_assets));
                UI_HASH(pending_auth >= 0);
                UI_HASH(pending_launch);
                for (const unsigned char *c=(const unsigned char *)ui_notice; *c; ++c) UI_HASH(*c);
            }
            if (screen.page == 5 && browse_tab == X4_CAT_TAB_DISCOVER) {
                UI_HASH(cloud_time.source);
                UI_HASH(cloud_time.unlimited);
                UI_HASH(cloud_time.remaining_s);
                UI_HASH(cloud_time.max_s);
            }
            UI_HASH(update_notice.open);
            UI_HASH(update_notice.shown);
#ifdef X4_UPDATE51_ENABLED
            UI_HASH(update_notice.install_focused);
            UI_HASH(update_progress_open);
            if (update_progress_open) {
                if (!update_rejected) update_view = x4_update51_view();
#if !X4_PUBLIC
                if (update_view.stage == X4_UPDATE51_ACCEPTED) {
                    update_view.close_seconds = !update_close_at ? 5u : draw_at >= update_close_at ? 0u :
                        (unsigned)((update_close_at - draw_at + UINT64_C(999999)) / UINT64_C(1000000));
                }
                UI_HASH(update_view.manual_selected);
                UI_HASH(update_view.close_seconds);
#endif
                UI_HASH(update_view.stage);
                UI_HASH(update_view.received / 100000u);
                UI_HASH(update_view.total);
                UI_HASH(update_view.code);
                UI_HASH(update_view.error);
            }
#endif
            UI_HASH(x4_apple_get_animation_phase());
            UI_HASH(account.state);
            UI_HASH(account.error);
            UI_HASH(account.seconds_left);
            UI_HASH(account.http_status);
            UI_HASH(remembered.enabled);
            UI_HASH(remembered.available);
            UI_HASH(remembered.error);
            UI_HASH(remember_notice);
            UI_HASH(logout_notice);
            if (logout_notice) UI_HASH(logout_confirm_focused);
            UI_HASH(account_ui.row);
            UI_HASH(account_history_revision);
            if (screen.page == 4) {
                UI_HASH(cloud_time.source);
                UI_HASH(cloud_time.remaining_s);
                UI_HASH(cloud_time.max_s);
                UI_HASH(cloud_time.unlimited);
            }
            UI_HASH(catalog.state);
            UI_HASH(catalog.count);
            UI_HASH(catalog_browser.active_tab);
            UI_HASH(catalog_browser.top_bar_focused);
            UI_HASH(catalog_browser.top_bar_index);
            UI_HASH(catalog_browser.hero_focused);
            UI_HASH(catalog_browser.featured_index);
            UI_HASH(catalog_browser.discover_row_selected);
            /* Row changes can arrive while both idle UI buffers are already clean. */
            UI_HASH(catalog_browser.home_revision);
            UI_HASH(catalog_browser.home_first_row);
            UI_HASH(catalog_browser.home_row);
            UI_HASH(discover_fade);
            UI_HASH(tabs_frame);
            UI_HASH(menu_frame);
            if (tabs_frame) {
                const char *background = secondary_tab ? catalog_browser.tab_background_id :
                    catalog_browser.home_background_id;
                for (const unsigned char *ch=(const unsigned char *)background; *ch; ++ch) UI_HASH(*ch);
            }
#if !X4_PUBLIC
            UI_HASH(home_has_toast);
            if (home_has_toast)
                for (const unsigned char *ch=(const unsigned char *)home_toast; *ch; ++ch) UI_HASH(*ch);
#endif
            UI_HASH(catalog_browser.selected);
            UI_HASH(catalog_browser.recent_selected);
            UI_HASH(catalog_browser.filter);
            UI_HASH(catalog_browser.ui2_revision);
            UI_HASH(catalog_browser.editing);
            UI_HASH(catalog_browser.keyboard_cursor);
            UI_HASH(catalog_browser.detail_scroll_offset);
            UI_HASH(catalog_browser.filtered_count);
            if (screen.page == 5) {
                UI_HASH(pending_launch);
                UI_HASH(pending_auth >= 0);
                for (const unsigned char *ch=(const unsigned char *)ui_notice; *ch; ++ch) UI_HASH(*ch);
            }
            for (size_t qi = 0; qi < sizeof(catalog_browser.query) && catalog_browser.query[qi]; ++qi) {
                UI_HASH(catalog_browser.query[qi]);
            }
            /* The asset mailbox revision also advances when a cover is published. */
            UI_HASH(metadata_revision);
            UI_HASH(x4_catalog_assets_idle(catalog_assets));
            UI_HASH(x4_auth_busy(auth));
            UI_HASH(picture_settings_ui.open);
            UI_HASH(picture_settings_ui.revision);
            UI_HASH(picture_settings_ui.first_time_mode);
            UI_HASH(picture_settings_ui.tab);
            UI_HASH(picture_settings_ui.selected);
            UI_HASH(picture_settings_ui.draft_settings);
            if(picture_settings_ui.open&&picture_settings_ui.tab==3){
                UI_HASH(picture_settings_ui.draft_multiplayer);
                for(unsigned i=0;i<X4_GAMEPAD_COUNT;++i){
                    const X4PictureSettingsPad *row=&picture_settings_ui.pads[i];
                    UI_HASH(row->assigned);UI_HASH(row->connected);
                    for(const unsigned char *c=(const unsigned char *)row->name;*c;++c)UI_HASH(*c);
                }
            }
            if(picture_settings_ui.open&&picture_settings_ui.tab==4){
                UI_HASH(picture_settings_ui.draft_mic_enabled);
                UI_HASH(picture_settings_ui.draft_mic_ms);
                UI_HASH(picture_settings_ui.mic_selected);
            }
            UI_HASH(picture_settings_ui.save_failed);
            UI_HASH(picture_settings_ui.language_saving);
            UI_HASH(picture_settings_ui.server_selected);
            UI_HASH(picture_settings_ui.server_scroll_offset);
            UI_HASH(picture_settings_ui.access_ui.selected);
            UI_HASH(picture_settings_ui.access_ui.confirming);
            UI_HASH(picture_settings_ui.access_ui.help);
            UI_HASH(picture_settings_ui.access_ui.save_error);
            UI_HASH(access_card_ui.selected);
            UI_HASH(access_card_ui.confirming);
            UI_HASH(access_card_ui.help);
            UI_HASH(access_card_ui.save_error);
            UI_HASH(access_query);
            UI_HASH(access_notice_until != 0);
            UI_HASH(controller.data.connected);
            UI_HASH(controller.data.buttons);
#if X4_STREAM_PROFILE && !X4_PUBLIC
            UI_HASH(stream_settings_ui.open);
            UI_HASH(stream_settings_ui.row);
            UI_HASH(lab_modal);
#endif
#undef UI_HASH
            if (next_ui_sig != ui_state_sig || catalog_copied || recent_copied || metadata_changed) {
                ui_state_sig = next_ui_sig;
                ui_dirty_buffers = 2;
            }
            if (ui_dirty_buffers == 0) {
                ++ui_dirty_skips;
                main_submit_done();
                sceKernelUsleep(4000);
                continue;
            }
            ui_reuse_work=ui_dirty_buffers==1;
            --ui_dirty_buffers;
        } else {
            ui_dirty_buffers = 2;
        }
        uint64_t ui_draw_begin = sceKernelGetProcessTime();
#if !X4_PUBLIC
        X4CatalogArtReady screen_art;
        bool screen_art_complete=cover_screen_visible&&!cover_screen_reported&&!ui_reuse_work&&
            cover_screen_ready(catalog_assets,cover_screen,cover_screen_shown?cover_screen_shown:ui_draw_begin,
                &catalog_browser,&catalog,&recent,&screen_art);
#endif
        bool loading_visible=false,loading_full_damage=false;
        bool keyboard_ready=x4_catalog_gui_keyboard_cache_ready(&catalog_browser);
        if(screen.page!=5||!catalog_browser.editing)x4_catalog_gui_keyboard_cache_invalidate();
        bool drew_live=false,drawn_fresh=false;
        uint64_t drawn_generation=0,drawn_output=0;
        enum X4UiScreen ui_metric=X4_UI_OTHER;bool ui_navigation=false;
        X4UiRect home_damage[12];
        X4UiRect game_damage[X4_GAME_PAGE_DAMAGE_MAX + 5];
        X4UiRect shell_rects[4];
        size_t shell_count = 0;
        bool status_damage = false, ui2_full_reset = false;
        bool drew_own_page = false;
        X4UiRect own_damage[8];
        size_t own_damage_count = SIZE_MAX;
        X4HomeDamageKind home_damage_kind = X4_HOME_DAMAGE_OTHER;
        size_t home_damage_count = SIZE_MAX;
        size_t game_damage_count = SIZE_MAX;
        bool drew_game_page = false;
        bool drew_tabs = false, drew_menu = false;
        unsigned rendered_fade_step = 0;
        uint64_t fade_draw_begin = 0;
        size_t menu_damage_count = SIZE_MAX;
#if !X4_PUBLIC
        bool menu_drawn = false;
#endif
        if(ui_reuse_work){ui_metric=ui_previous_metric;ui_navigation=ui_previous_navigation;}
        if(!ui_reuse_work){
#if X4_STREAM_PROFILE
        loading_visible=screen.page==6&&(!live_visible||loading_transition)&&!live_error&&!closing&&!session_back&&
            x4_auth_busy(auth)&&session.state!=X4_SESSION_ERROR&&
            session.state!=X4_SESSION_STOPPING&&session.state!=X4_SESSION_CLOSED&&session.state!=X4_SESSION_CANCELLED&&
            !session.error&&session.http_status<400&&!session.cleanup_failed&&x4_loading_screen_active();
        if(loading_visible){
            X4LoadingView before_loading,after_loading;x4_loading_screen_snapshot(&before_loading);
            if(!x4_loading_screen_draw(x4_display_pixels(&display),ui_draw_begin)){
                main_submit_done();sceKernelUsleep(1000);continue;
            }
            x4_loading_screen_snapshot(&after_loading);
            loading_full_damage=after_loading.first_picture||
                after_loading.full_buffer_initializations!=before_loading.full_buffer_initializations;
        }
#endif
        drew_live = live_visible && !loading_transition && x4_live_media_draw(live, x4_display_pixels(&display));
        if (drew_live && !live_retained)
            x4_live_media_drawn(live, &drawn_generation, &drawn_output, &drawn_fresh);
        /* Media draw records the independent native-output identity. Display
         * uses the actual drawn publication tag, never a later snapshot. */
        (void)drawn_output;
        uint32_t *ui_pixels = x4_display_pixels(&display);
        if (!reuse_modal_background) {
        if (!drew_live && !loading_visible && (settings_page || lab_page)) {
            if (settings_page) {
                if (!settings_canvas_valid) {
                    x4_rect(ui_pixels, 0, 0, 1920, 1080, X4_UI2_BACKGROUND);
                    x4_picture_settings_ui_cache_invalidate();
                }
                own_damage_count = x4_picture_settings_ui_draw_page_cached(
                    &picture_settings_ui, ui_pixels, own_damage);
                settings_canvas_valid = true;
                drew_own_page = true;
            }
#if X4_STREAM_PROFILE && !X4_PUBLIC
            else {
                x4_rect(ui_pixels, 0, 0, 1920, 1080, X4_UI2_BACKGROUND);
                x4_stream_settings_ui_draw_page(&stream_settings_ui, ui_pixels);
            }
#endif
        } else {
        if (game_page_ui.open && !closing && !drew_live && !loading_visible) {
            X4GamePageModel model = game_page_model(&game_page_ui, &catalog, &favorites,
                x4_auth_busy(auth) || pending_auth >= 0 || pending_launch);
            char last[X4_HISTORY_LABEL], played[X4_HISTORY_LABEL];
            x4_history_text(x4_history_find(&play_history, model.product_id), x4_history_now(),
                x4_lang_current() == X4_LANG_EN, last, played);
            model.last_session = last;
            model.played_time = played;
            model.notice = ui_notice;
            game_damage_count = game_page_draw(&game_page_ui, model, &catalog, catalog_assets,
                ui_pixels, game_page_frame, game_damage);
            drew_game_page = true;
        }
        if (!game_page_ui.open && !drew_live && screen.page != 5 && !loading_visible) {
            if (screen.page == 0) {
                if (!menu_canvas_valid) {
                    x4_rect(ui_pixels, 0, 0, 1920, 1080, X4_UI2_BACKGROUND);
                    ui2_full_reset = true;
                }
                X4Ui2HomeModel model;
                x4_ui2_home_model(&model, screen.signed_in, &catalog, &recent, &catalog_browser.home_rows);
                home_choice = x4_ui2_home_normalize(&model, home_choice);
                menu_damage_count = x4_ui2_home_draw(&model, &catalog, catalog_assets, metadata_revision,
                    ui2_shell.focused ? -1 : home_choice, menu_canvas_valid && menu_frame,
                    ui_pixels, home_damage);
                menu_canvas_valid = drew_menu = menu_frame;
#if !X4_PUBLIC
                menu_drawn = true;
#endif
            } else if (screen.page == 2) {
                char version[32];
                const enum X4UpdateStatus status = x4_release_check_status(version, sizeof(version));
                x4_ui2_about_draw(ui_pixels, &ui_app_icon.surface[X4_COVER_GRID], status, version);
            } else if (screen.page == 7) {
                x4_ui2_support_draw(ui_pixels);
            } else if (screen.page == 8) {
                X4CatalogAssetView artwork = {0};
                const bool acquired = catalog_assets &&
                    x4_catalog_assets_acquire(catalog_assets, X4_FORTNITE_PRODUCT_ID, &artwork);
                x4_ui2_fortnite_draw(ui_pixels, &fortnite, &catalog, acquired ? &artwork : NULL);
                if (acquired) x4_catalog_assets_release(catalog_assets, &artwork);
            } else if (screen.page != 4 && !closing) {
                x4_screen_draw(&screen, &controller, ui_pixels);
            }
        }
        if (screen.page == 3) x4_media_draw(&video, &audio, x4_display_pixels(&display));
        if (screen.page == 6) {
            if (drew_live) {
                x4_live_overlay(&live_status, &session, live_muted, &touch_menu, show_banner, x4_display_pixels(&display));
                x4_cloud_time_game_draw(x4_display_pixels(&display),cloud_time,cloud_alert,touch_menu.is_open);
                if (mic) mic_draw(x4_display_pixels(&display),&touch_menu,mic,mic_notice,mic_show_level);
            } else if(!loading_visible) {
                X4SessionSnapshot shown = session;
#if X4_STREAM_PROFILE
                if(!live_error && live && !closing && !session_back &&
                    x4_auth_busy(auth) && x4_live_media_waiting_initial_resolution(live)) {
                    uint64_t sent_us = x4_stream_startup_alias_sent_us(x4_live_media_stream_startup(live));
                    uint64_t now_w = sceKernelGetProcessTime();
                    uint64_t wait_sec = (sent_us && now_w >= sent_us) ? (unsigned)((now_w - sent_us) / 1000000ULL) : 0;
                    if (wait_sec >= 5) {
                        snprintf(shown.stage, sizeof(shown.stage), "Xbox está preparando el juego… %u s", (unsigned)wait_sec);
                    } else {
                        snprintf(shown.stage, sizeof(shown.stage), "Esperando 1080p de Xbox…");
                    }
                }
#endif
#if X4_STREAM_PROFILE
                if (live_error && (!x4_auth_busy(auth) || video_start_error)) {
#else
                if (live_error && !x4_auth_busy(auth)) {
#endif
                    shown.state = X4_SESSION_ERROR;
                    shown.error = live_error;
                    shown.http_status = 0;
                    shown.ready_seen = shown.connection_authorized = 0;
                    shown.rtc_connected = 0;
                    snprintf(shown.stage, sizeof(shown.stage), "no se pudieron preparar o cerrar los medios nativos");
#if X4_STREAM_PROFILE
                    if(video_start_error)(void)x4_stream_video_error_caption(shown.stage,sizeof(shown.stage),live_error);
#endif
                    if (catalog_selected < catalog.count)
                        snprintf(shown.title_name, sizeof(shown.title_name), "%s", catalog.titles[catalog_selected].name);
                }
                x4_session_draw(&shown, x4_auth_busy(auth), closing, ui_pixels);
                x4_live_status_draw(&live_status, live_error, ui_pixels);
            }
        }
        else if ((screen.page == 4 && !game_page_ui.open) || closing) {
            const char *selected_server = x4_server_settings_get_selected();
            const X4Ui2AccountModel model = {
                .auth = &account, .remember = remembered,
                .busy = x4_auth_busy(auth), .closing = closing, .focus_owned = !ui2_shell.focused,
                .gamertag = catalog.gamertag, .initial = ui2_status.initial,
                .server = selected_server && selected_server[0] && strcmp(selected_server, "auto") ?
                    selected_server : x4_tr(X4_TR_SERVER_OPT_AUTO),
                .country = x4_access_country_label(x4_auth_access_country(auth)),
                .cloud_time = cloud_time, .history = &account_history, .catalog = &catalog,
                .assets = catalog_assets, .assets_revision = metadata_revision,
            };
            own_damage_count = x4_ui2_account_draw(&account_ui, &model, ui_pixels,
                account_canvas_valid, own_damage);
            account_canvas_valid = !closing;
            drew_own_page = true;
        }
        else if (screen.page == 5 && !game_page_ui.open) {
            x4_catalog_gui_sidebar_focused(ui2_shell.focused);
            X4CatalogGuiStatus gui = {controller.data.connected, x4_auth_busy(auth),
                pending_launch || pending_auth >= 0, ui_notice};
            if (browse_tab == X4_CAT_TAB_DISCOVER && !catalog_browser.editing)
                fade_draw_begin = sceKernelGetProcessTime();
            x4_catalog_gui_draw_cached(&catalog, &catalog_browser, &recent, catalog_assets, &gui, ui_pixels);
            if (fade_draw_begin) rendered_fade_step = x4_catalog_gui_discover_fade_step(NULL);
#if !X4_PUBLIC
            if (keyboard_toast) x4_catalog_gui_keyboard_cache_invalidate();
#endif
            if (tabs_frame) {
                home_damage_count = x4_catalog_gui_home_damage(home_damage, &home_damage_kind);
                if (home_damage_count != SIZE_MAX &&
                    x4_cloud_time_home_damage(&home_damage[home_damage_count])) ++home_damage_count;
                home_canvas_valid = drew_tabs = true;
            } else if (tabs_canvas) {
                /* A secondary tab retains its selected art across the keyboard. */
                home_canvas_valid = true;
            } else {
                /* A later modal or toast may overwrite the clean tab canvas. */
                x4_catalog_gui_home_invalidate();
            }
        }
        }
#if !X4_PUBLIC
        x4_script_engine_draw_notification(ui_pixels);
#endif
        if (!drew_live && screen.page != 3 && screen.page != 6 && !loading_visible && !loading_transition) {
            uint32_t *work = x4_display_pixels(&display);
            bool full_ui = ui2_full_reset || (!drew_menu && !drew_tabs && !drew_game_page && !drew_own_page) ||
                (drew_own_page && own_damage_count == SIZE_MAX) ||
                (drew_game_page && game_damage_count == SIZE_MAX) ||
                (drew_menu && menu_damage_count == SIZE_MAX && !shell_presented) ||
                (drew_tabs && home_damage_count == SIZE_MAX);
            int sidebar_fresh = 0;
            if (full_ui || (drew_game_page && game_damage_count == SIZE_MAX) ||
                (drew_tabs && home_damage_count == SIZE_MAX))
                sidebar_fresh = 1080;
            if (drew_tabs && home_damage_count != SIZE_MAX) {
                for (size_t i = 0; i < home_damage_count; ++i) {
                    const X4UiRect r = home_damage[i];
                    if (r.x == 0 && r.y == 0 && r.w >= x4_ui2_sidebar_rect.w && r.h > sidebar_fresh)
                        sidebar_fresh = r.h;
                }
            }
            if (sidebar_fresh) {
                x4_ui2_shell_draw_sidebar_backdrop(work, &ui2_shell, sidebar_fresh);
                if (sidebar_fresh < 1080 && (!shell_presented ||
                    shown_shell.section != ui2_shell.section || shown_shell.selected != ui2_shell.selected ||
                    shown_shell.focused != ui2_shell.focused))
                    shell_rects[shell_count++] = x4_ui2_sidebar_rect;
            } else if (full_ui || !shell_presented) {
                x4_ui2_shell_draw_sidebar(work, &ui2_shell);
                shell_rects[shell_count++] = x4_ui2_sidebar_rect;
            } else {
                shell_count = x4_ui2_shell_draw_sidebar_delta(work, &shown_shell, &ui2_shell, shell_rects);
            }
            bool status_backdrop_changed = full_ui || !shell_presented;
            if (drew_tabs && home_damage_count != SIZE_MAX) {
                const X4UiRect status = x4_ui2_status_rect;
                for (size_t i = 0; i < home_damage_count; ++i) {
                    const X4UiRect r = home_damage[i];
                    if (r.x <= status.x && r.y <= status.y &&
                        r.x + r.w >= status.x + status.w && r.y + r.h >= status.y + status.h)
                        status_backdrop_changed = true;
                }
            }
            status_damage = status_backdrop_changed || shown_status_revision != status_revision;
            if (status_damage) x4_ui2_shell_draw_status(work, &ui2_status, status_backdrop_changed);
            shown_shell = ui2_shell;
            shown_status_revision = status_revision;
            shell_presented = true;
        } else shell_presented = false;
        }
        if (ui_modal && !drew_live && !loading_visible && !loading_transition) {
            if (!reuse_modal_background) x4_ui2_modal_scrim(ui_pixels);
            if (screen.exit_error) x4_ui2_exit_error_draw(screen.exit_error, ui_pixels);
            else if (logout_notice) x4_ui2_account_logout_modal(remembered.enabled, logout_confirm_focused, ui_pixels);
            else if (remember_notice) x4_ui2_account_remember_modal(ui_pixels);
            else if (settings_modal) x4_picture_settings_ui_draw_modal(&picture_settings_ui, ui_pixels);
#if X4_STREAM_PROFILE && !X4_PUBLIC
            else if (lab_modal) x4_stream_settings_ui_draw_modal(ui_pixels);
#endif
#ifdef X4_UPDATE51_ENABLED
            else if (update_progress_open) x4_ui2_update_job_draw(&update_view, ui_pixels);
#endif
            else if (update_notice.open) x4_ui2_update_notice_draw(&update_notice, ui_pixels);
            else if (cloud_launch.open) x4_ui2_cloud_confirm_draw(ui_pixels);
            else if (access_modal)
                x4_ui2_modal_draw_access(ui_pixels, &access_card_ui,
                    access_query ? X4_ACCESS_CARD_TRYING : X4_ACCESS_CARD_SELECT);
            modal_underlay = underlay;
            modal_latched = true;
        } else modal_latched = false;
        if (modal_latched) {
            ui_metric = X4_UI_MODAL;
            ui_navigation = reuse_modal_background;
        } else if(screen.exit_error||live_error||(screen.page==6&&session.state==X4_SESSION_ERROR))ui_metric=X4_UI_ERRORS;
        else if(picture_settings_ui.open){
            ui_metric=picture_settings_ui.first_time_mode?X4_UI_FIRST_TIME:
                picture_settings_ui.tab?X4_UI_OPTIONS_SERVER:X4_UI_OPTIONS_PICTURE;
            ui_navigation=drew_own_page && own_damage_count != SIZE_MAX;
        }else if(screen.page==5&&catalog_browser.editing){ui_metric=X4_UI_KEYBOARD;ui_navigation=keyboard_ready;}
#if X4_STREAM_PROFILE && !X4_PUBLIC
        else if(stream_settings_ui.open)ui_metric=X4_UI_LAB_SETTINGS;
#endif
        else if(game_page_ui.open) {
            ui_metric=X4_UI_GAME_PAGE;
            ui_navigation=drew_game_page&&game_damage_count!=SIZE_MAX&&game_damage_count>0;
        }
        else switch(screen.page){
            case 0:
                ui_metric=X4_UI_MENU;
                ui_navigation=drew_menu&&menu_damage_count!=SIZE_MAX&&menu_damage_count>0;
                break;
            case 2:ui_metric=X4_UI_ABOUT;break;
            case 3:ui_metric=X4_UI_DEMO;break;
            case 4:ui_metric=X4_UI_ACCOUNT;ui_navigation=drew_own_page&&own_damage_count!=SIZE_MAX;break;
            case 5:
                ui_metric=browse_tab==X4_CAT_TAB_DISCOVER?(rendered_fade_step?X4_UI_DISCOVER_FADE:X4_UI_DISCOVER):
                    browse_tab==X4_CAT_TAB_RECENTS?X4_UI_RECENTS:X4_UI_CATALOG;
                ui_navigation=drew_tabs&&(home_damage_kind==X4_HOME_DAMAGE_HORIZONTAL||
                    home_damage_kind==X4_HOME_DAMAGE_VERTICAL);
                break;
            case 6:ui_metric=session.waiting_for_resources?X4_UI_QUEUE:X4_UI_LOADING;break;
            case 7:ui_metric=X4_UI_SUPPORT;break;case 8:ui_metric=X4_UI_FORTNITE;break;
            default:ui_metric=X4_UI_OTHER;break;
        }
        if(loading_visible)ui_navigation=!loading_full_damage;
        ui_previous_metric=ui_metric;ui_previous_navigation=ui_navigation;
        }
        /* Getter access defaults to full damage. Narrow it only after every
         * renderer/overlay has finished, never while a later draw can expand
         * its writes. Pending changes in the other scanout remain intact. */
        if(ui_reuse_work)x4_display_set_damage(&display,NULL,0);
        else if (modal_latched) {
            if (reuse_modal_background) {
                const X4UiRect rect = x4_ui2_modal_damage();
                x4_display_set_damage(&display, &rect, 1);
            }
        }
        else if(!drew_live&&!ui2_full_reset){
            if (drew_menu && menu_damage_count == SIZE_MAX) {
                home_damage[0] = x4_ui2_home_bounds();
                menu_damage_count = 1;
            }
            if (drew_menu) {
                for (size_t i = 0; i < shell_count; ++i) home_damage[menu_damage_count++] = shell_rects[i];
                if (status_damage) home_damage[menu_damage_count++] = x4_ui2_status_rect;
            }
            if (drew_tabs && home_damage_count != SIZE_MAX) {
                for (size_t i = 0; i < shell_count; ++i) home_damage[home_damage_count++] = shell_rects[i];
                if (status_damage) home_damage[home_damage_count++] = x4_ui2_status_rect;
            }
            if (drew_own_page && own_damage_count != SIZE_MAX) {
                for (size_t i = 0; i < shell_count; ++i) own_damage[own_damage_count++] = shell_rects[i];
                if (status_damage) own_damage[own_damage_count++] = x4_ui2_status_rect;
                x4_display_set_damage(&display, own_damage, own_damage_count);
            } else if (drew_game_page && game_page_frame && game_damage_count != SIZE_MAX) {
                for (size_t i = 0; i < shell_count; ++i) game_damage[game_damage_count++] = shell_rects[i];
                if (status_damage) game_damage[game_damage_count++] = x4_ui2_status_rect;
                x4_display_set_damage(&display, game_damage, game_damage_count);
            } else if(drew_tabs&&home_damage_count!=SIZE_MAX)
                x4_display_set_damage(&display,home_damage,home_damage_count);
            else if(drew_menu&&menu_damage_count!=SIZE_MAX)
                x4_display_set_damage(&display,home_damage,menu_damage_count);
            else if(loading_visible&&!loading_full_damage){
                const X4UiRect region = {510,230,900,680};
                x4_display_set_damage(&display,&region,1);
            }
            else if(screen.page==5&&catalog_browser.editing
#if !X4_PUBLIC
                &&!keyboard_toast
#endif
            ){
                X4CatalogKeyboardCacheStats cache;x4_catalog_gui_keyboard_cache_snapshot(&cache);
                if(!cache.last_draw_full){
                    X4UiRect rects[6]={{420,200,1080,680}};
                    size_t count = 1;
                    for (size_t i = 0; i < shell_count; ++i) rects[count++] = shell_rects[i];
                    if (status_damage) rects[count++] = x4_ui2_status_rect;
                    x4_display_set_damage(&display,rects,count);
                }
            }
        }
        uint64_t ui_draw_end=sceKernelGetProcessTime();
        if (rendered_fade_step == 1 && ui_draw_end - fade_draw_begin > 33000u) {
            x4_catalog_gui_discover_disable_fade();
#if !X4_PUBLIC
            const uint64_t fade_us = ui_draw_end - fade_draw_begin;
            printf("XCloud4: ui fundido desactivado paso=%llu.%03llu ms\n",
                (unsigned long long)(fade_us / 1000), (unsigned long long)(fade_us % 1000));
#endif
        }
        uint64_t present_begin = sceKernelGetProcessTime();
        rc = drew_live && trace_active ?
            x4_display_present_trace(&display, live_trace, drawn_generation, drawn_fresh) :
            x4_display_present(&display);
        uint64_t present_end=sceKernelGetProcessTime();
        if (rc == 0) ui2_splash_presented(&splash);
        if(!drew_live){
            X4UiCopyStats copied;x4_display_copy_stats(&display,&copied);
#if !X4_PUBLIC
            if (!rc && menu_drawn && !boot_diag.saved) {
                ui_boot_event(&boot_diag, BOOT_HOME_FIRST, ui_draw_end);
                ui_boot_event(&boot_diag, BOOT_HOME_LAST, ui_draw_end);
                ++boot_diag.home_draws;
            }
            if(!rc&&cover_screen_visible&&!cover_screen_shown)cover_screen_shown=present_end;
            if(!rc&&screen_art_complete){
                static const char *const names[]={NULL,"menu","descubrir","catalogo"};
                printf("XCloud4: COVER_SCREEN screen=%s ms=%llu ram=%u disk=%u network=%u fallback=%u\n",
                    names[cover_screen],(unsigned long long)((present_end-cover_screen_shown)/1000u),
                    screen_art.ram,screen_art.disk,screen_art.network,screen_art.fallback);
                cover_screen_reported=true;
            }
            if(menu_drawn&&menu_first_frame_pending){
                menu_first_frame_pending=false;
                printf("XCloud4: menu primer frame dibujo_us=%llu copia_us=%llu bytes=%llu\n",
                    (unsigned long long)(ui_draw_end-ui_draw_begin),
                    (unsigned long long)copied.elapsed_us,(unsigned long long)copied.bytes);
            }
            if(drew_tabs&&browse_tab==X4_CAT_TAB_DISCOVER&&home_first_frame_pending){
                home_first_frame_pending=false;
                printf("XCloud4: descubrir primer frame dibujo_us=%llu copia_us=%llu bytes=%llu\n",
                    (unsigned long long)(ui_draw_end-ui_draw_begin),
                    (unsigned long long)copied.elapsed_us,(unsigned long long)copied.bytes);
            }
            if(drew_tabs){
                unsigned tab = (unsigned)browse_tab;
                if (!home_log[tab].since_us) home_log[tab].since_us = present_end;
                if (home_damage_kind != X4_HOME_DAMAGE_OTHER) {
                    unsigned kind = home_damage_kind == X4_HOME_DAMAGE_HORIZONTAL ? TAB_H :
                        home_damage_kind == X4_HOME_DAMAGE_VERTICAL ? TAB_V :
                        home_damage_kind == X4_HOME_DAMAGE_TAB ? TAB_SWITCH : TAB_BACKGROUND;
                    ++home_log[tab].count[kind];
                    uint64_t draw_us = ui_draw_end - ui_draw_begin;
                    if (draw_us > home_log[tab].max_us[kind]) home_log[tab].max_us[kind] = draw_us;
                }
                if (copied.bytes > home_log[tab].bytes_max) home_log[tab].bytes_max = copied.bytes;
                X4CatalogAssetStats asset_stats;
                x4_catalog_assets_stats(catalog_assets, &asset_stats);
                if (asset_stats.thumbnail_peak_bytes > home_log[tab].thumbnail_peak_bytes)
                    home_log[tab].thumbnail_peak_bytes = asset_stats.thumbnail_peak_bytes;
                if (asset_stats.background_bytes > home_log[tab].background_peak_bytes)
                    home_log[tab].background_peak_bytes = asset_stats.background_bytes;
            }
#endif
            const bool ui_redrawn = !ui_reuse_work && (!drew_game_page || game_damage_count != 0);
            x4_ui_draw_metrics_record(ui_metric,ui_navigation,ui_redrawn,
                ui_redrawn?ui_draw_end-ui_draw_begin:0,copied.elapsed_us,
                present_end-present_begin,copied.bytes,present_end,ui_dirty_skips);
        }
        if (rc == 0 && drew_live && live && !live_retained) {
            x4_live_media_note_present(live, sceKernelGetProcessTime() - present_begin);
            memcpy(overlay_key, next_overlay_key, sizeof(overlay_key));
            overlay_at = draw_at;
            live_presented = true;
        } else live_presented = false;
        if (rc < 0) {
            printf("XCloud4: error de presentacion: 0x%08x\n", (unsigned)rc);
            break;
        }
    }
#if !X4_PUBLIC
    (void)ui_boot_diag_save(&boot_diag, sceKernelGetProcessTime(), ui_dirty_skips, true);
#endif
#ifdef X4_UPDATE51_ENABLED
    x4_update51_exit();
#endif
    /* A display failure can break the loop early: cancel then join before
     * cleaning the UI. Ordinary OPTIONS keeps rendering while it cancels. */
    mic_end_match(&touch_menu);
    trace_inactive(live);
    x4_catalog_gui_home_invalidate();
    x4_screen_home_invalidate();
    x4_game_page_cache_dispose();
    x4_catalog_assets_suspend(catalog_assets);
    x4_catalog_assets_destroy(catalog_assets);
    catalog_assets = NULL;
    x4_auth_cancel(auth);
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    interaction_rumble_stop(&rumble_state,primary.handle);
    printf("XCloud4: vibracion Pad calls=%llu success=%llu errors=%llu (resultado local)\n",
        (unsigned long long)interaction_main.calls,(unsigned long long)interaction_main.ok,
        (unsigned long long)interaction_main.errors);
#else
    if(controller.handle>=0) {
        OrbisPadVibeParam zero={0};
        (void)scePadSetVibration(controller.handle,&zero);
        rumble_applied=zero; rumble_handle=controller.handle;
    }
    printf("XCloud4: vibracion Pad calls=%llu success=%llu errors=%llu (resultado local)\n",
        (unsigned long long)rumble_pad_calls,(unsigned long long)rumble_pad_ok,
        (unsigned long long)rumble_pad_errors);
#endif
#endif
    ui_history_finish(&play_history, &local_session, sceKernelGetProcessTime());
    x4_catalog_browser_ui2_library_close(&catalog_browser);
    while (x4_auth_busy(auth)) sceKernelUsleep(100000);
    if (live) {
        int live_rc = x4_auth_set_media_callback(auth, NULL, NULL, NULL);
        if (!live_rc) live_rc = x4_live_media_close(live);
        if (!live_rc) live = NULL;
        else live_error = live_rc;
    }
    printf("XCloud4: cerrar acceso Microsoft\n");
    int auth_rc = x4_auth_close(auth);
    if (auth_rc >= 0) {
#if !X4_PUBLIC
        /* The auth and quota workers have both stopped. Flush any XSTS
         * response received since the earlier exit snapshot. */
        (void)ui_boot_diag_save(&boot_diag, sceKernelGetProcessTime(), ui_dirty_skips, true);
#endif
        auth = NULL;
        catalog_cursor = (X4AuthCatalogCursor){0};
        recent_cursor = (X4AuthRecentCursor){0};
        x4_recent_init(&recent);
        x4_catalog_browser_init(&catalog_browser);
        x4_catalog_browser_ui2_select(&catalog_browser, X4_CAT_TAB_DISCOVER);
        x4_catalog_browser_ui2_favorites(&catalog_browser, &favorites);
    }
    printf("XCloud4: cerrar audio\n");
    x4_audio_stop(&audio);
    printf("XCloud4: cerrar decoder\n");
    x4_video_stop(&video);
    printf("XCloud4: cerrar control\n");
    if(pads){x4_controller_roster_stop(pads);pads=NULL;}
    else x4_controller_close(&primary);
#if !X4_PUBLIC
    x4_script_engine_export_session_json("/data/xcloud4/session_scripts.json");
    x4_script_engine_shutdown();
#endif
    printf("XCloud4: cerrar pantalla\n");
#if X4_STREAM_PROFILE
    int display_close_rc=x4_display_close_checked(&display);
    if(display_close_rc<0)live_error=display_close_rc;
#else
    x4_display_close(&display);
#endif
    x4_cover_rasters_free(&ui_app_icon);
    x4_ui2_home_close();
    x4_loading_screen_end();
    x4_touch_menu_cache_reset();
    x4_ui_draw_metrics_flush(sceKernelGetProcessTime());
    x4_picture_settings_ui_cache_invalidate();x4_catalog_gui_keyboard_cache_invalidate();
    x4_font_close();
    printf("XCloud4: recursos cerrados\n");
    x4_release_check_stop();
    x4_ui_lang_pref_stop();
    int exit_rc = x4_exit_prepare();
    if (exit_rc >= 0) exit_rc = x4_exit_request();
    if (exit_rc >= 0) {
        /* LoadExec is asynchronous; permit recovery if the shell never exits. */
        for (unsigned wait = 0; wait < 100; ++wait) sceKernelUsleep(100000);
        exit_rc = -10;
        printf("XCloud4: tiempo de salida agotado\n");
    }
#if X4_STREAM_PROFILE
    if(live||display.handle>=0||display.memory||display.allocated){
        /* An unsuccessful shell exit cannot authorize a second owner over
         * unresolved resources. Keep all failed-close contexts alive. */
        printf("XCloud4: salida no completada con recursos retenidos; no se reabre VideoOut\n");
        for(;;)sceKernelUsleep(100000);
    }
#endif
    /* A rejected/timed-out request reopens the interface and permits a retry. */
    screen.exit_error = exit_rc;
    screen.exit_requested = 0;
    screen.page = 0;
    media_active = 0;
    live_presented = false;
    overlay_at = input_previous = input_report_at = 0;
    input_intervals = input_interval_us = input_interval_max_us = idle_present_skips = 0;
    closing = 0;
    session_back = 0;
    game_controls = 0;
    if (!auth) auth = x4_auth_create();
    sceKernelUsleep(250000);
    goto reopen_interface;
#endif
}
