/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdlib.h>
#include "../ui/apple_ui.h"
#include "../ui/loading_screen.h"
#include "../ui/ui_perf.h"
#include "../ui/ui_draw_metrics.h"
#include <string.h>
#include <stdatomic.h>
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>
#include "../video/display.h"
#include "../input/controller.h"
#include "../ui/screen.h"
#include "../ui/lang.h"
#include "../ui/catalog_gui.h"
#include "../ui/catalog_presentation.h"
#include "../ui/font.h"
#include "../auth/auth_profile.h"
#include "../media/live_media.h"
#include "lifecycle.h"
#include "build_identity.h"
#include "probe_controller.h"
#include "../streaming/stream_profile.h"
#include "../streaming/stream_startup.h"
#include "../ui/stream_video_status.h"
#include "../ui/stream_settings.h"
#include "../auth/release_check.h"
#include "../ui/picture_settings.h"
#include "../ui/server_settings.h"
#include "../ui/access_country.h"

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
    int result=scePadSetVibration(handle,&wanted);
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
#if X4_STREAM_PROFILE
typedef struct {
    const X4LiveMediaSnapshot *media;
    const X4SessionSnapshot *session;
    int muted;
    const X4TouchMenu *touch_menu;
    bool show_banner;
    bool received_banner;
} StreamOverlayContext;
static void stream_overlay(uint32_t *pixels,void *context)
{
    const StreamOverlayContext *c=context;
    uint64_t begin=sceKernelGetProcessTime();
    uint64_t overlay_begin=begin;
    x4_live_overlay(c->media,c->session,c->muted,c->touch_menu,c->show_banner,pixels);
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

int main(void)
{
#if X4_VIDEO_PROBE
    /* Local source only: enter before any auth/catalog/network allocation. */
    return x4_probe_controller_run();
#else
    X4Display display={.handle=-1};
    X4Controller controller;
    X4Screen screen = {0};
    X4DemoVideo video = {0};
    X4DemoAudio audio = {.handle = -1};
    X4Auth *auth = x4_auth_create();
    X4AuthSnapshot account = {0};
    static X4CatalogSnapshot catalog;
    static X4CatalogBrowser catalog_browser;
    X4AuthCatalogCursor catalog_cursor = {0};
    X4AuthRecentCursor recent_cursor = {0};
    X4RecentSnapshot recent_snapshot = {0};
    X4RecentGames recent;
    static X4CatalogPresentation presentation;
    X4CatalogAssets *catalog_assets = NULL;
    uint64_t ui_generation = 1, metadata_revision = 0;
    int pending_auth = -1, pending_launch = 0;
    char pending_title_id[96] = {0};
    uint64_t pending_generation = 0;
    char ui_notice[160] = {0};
    X4SessionSnapshot session = {0};
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
#endif
#endif
    x4_server_settings_init();
    x4_picture_settings_init();
    X4PictureSettingsUi picture_settings_ui={0};
    bool diagnostic_stable = false;
    uint64_t diagnostic_mark = 0;
    X4TouchMenu touch_menu;
    x4_touch_menu_init(&touch_menu, X4_PUBLIC != 0);
    uint64_t video_banner_until_us = 0;
    int overlay_key[11] = {0};
    uint64_t overlay_at = 0;
    uint64_t input_previous = 0, input_report_at = 0;
    uint64_t input_intervals = 0, input_interval_us = 0, input_interval_max_us = 0;
    uint64_t idle_present_skips = 0;
    uint32_t ui_state_sig = 0;
    unsigned ui_dirty_buffers = 2;
    uint64_t ui_dirty_skips = 0;
    enum X4UiScreen ui_previous_metric=X4_UI_OTHER;
    bool ui_previous_navigation=false;
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    InteractionRumble rumble_state={.handle=-1};
#else
    OrbisPadVibeParam rumble_applied={0};
    int rumble_handle=-1;
    uint64_t rumble_pad_calls=0,rumble_pad_ok=0,rumble_pad_errors=0;
#endif
    bool rumble_was_connected=false;
#endif
    x4_catalog_browser_init(&catalog_browser);
    catalog_browser.apple_mode = true;
    x4_recent_init(&recent);
    x4_catalog_presentation_init(&presentation);
    x4_catalog_presentation_set_generation(&presentation, ui_generation);
    setvbuf(stdout, NULL, _IONBF, 0);
    (void)x4_lang_init();
    x4_release_check_start();
reopen_interface:;
    if (x4_font_init("/app0/assets/fonts/inter.x4font") < 0 &&
        x4_font_init("/app0/assets/fonts/noto_sans.x4font") < 0)
        printf("XCloud4: fuente externa no disponible; respaldo suavizado activo\n");
    load_ui_app_icon();
    catalog_assets = x4_catalog_assets_create();
    x4_catalog_assets_set_generation(catalog_assets, ui_generation);
    int rc = x4_display_open(&display);
    if (rc < 0) {
        printf("XCloud4: no se pudo iniciar VideoOut: 0x%08x\n", (unsigned)rc);
        sceKernelUsleep(3000000);
        /* Never enter the known failing libc _exit path on this firmware. */
        if (x4_exit_prepare() >= 0) x4_exit_request();
        for (;;) sceKernelUsleep(100000);
    }
    x4_ui_draw_metrics_reset();
    x4_picture_settings_ui_cache_invalidate();
    x4_catalog_gui_keyboard_cache_invalidate();
    x4_controller_init(&controller);
#if X4_STREAM_PROFILE
    stream_display_active=false;
    if(!live){live_retained=0;live_error=0;
        video_start_error=false;
    }
#endif
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    rumble_state=(InteractionRumble){.handle=-1};rumble_was_connected=false;
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
    for (unsigned frame = 0;; ++frame) {
#if X4_STREAM_PROFILE
        bool stream_settings_consumed=false;
#endif
        uint64_t input_at = sceKernelGetProcessTime();
        x4_trace_monitor_state(live ? x4_live_media_trace(live) : NULL,
            X4_MON_ACTOR_MAIN, X4_MON_PHASE_MAIN_POLL, input_at);
#if X4_STREAM_PROFILE
        uint64_t controller_read_at=sceKernelGetProcessTime();
        if(live&&screen.page==6&&x4_auth_busy(auth))interaction_main_read(controller_read_at);
#endif
        x4_controller_read(&controller, frame);
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
                input_report_at = input_at;
                input_intervals = input_interval_us = input_interval_max_us = idle_present_skips = 0;
            }
        } else {
            input_previous = input_report_at = 0;
            input_intervals = input_interval_us = input_interval_max_us = idle_present_skips = 0;
        }
        int previous_page = screen.page;
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
            trace_inactive(live);x4_auth_cancel(auth);session_back=1;
        }
#endif
        bool game_input = screen.page == 6 && game_controls && x4_auth_busy(auth) &&
            !closing && !session_back && !live_error;
#if X4_PUBLIC
        (void)diagnostic_stable;
        (void)diagnostic_mark;
#endif
        if (game_input) {
            x4_touch_menu_feed_touch(&touch_menu, game_input, controller.data.touch.fingers, controller_read_at);
        } else {
            x4_touch_menu_close(&touch_menu);
        }
        X4GamepadFrame gamepad = {0};
        if (game_input) {
            if (x4_touch_menu_is_open(&touch_menu)) {
                gamepad.connected = controller.data.connected;
            } else {
                X4Controller filtered = controller;
                filtered.data.buttons = x4_touch_menu_filter_buttons(&touch_menu, controller.data.buttons);
                x4_controller_gamepad(&filtered, &gamepad);
                if (x4_touch_menu_is_nexus_active(&touch_menu, controller_read_at)) {
                    gamepad.buttons |= X4_GAMEPAD_NEXUS;
                }
            }
        }
#if X4_STREAM_PROFILE
        gamepad.sampled_us=controller_read_at;
#endif
        x4_auth_set_gamepad(auth, &gamepad);
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
        if(!game_input||!session.rtc_connected||!controller.data.connected)
            desired.lgMotor=desired.smMotor=0;
#if X4_STREAM_PROFILE
        if(interaction_main.modes&X4_INTERACTION_RUMBLE_OFF)desired.lgMotor=desired.smMotor=0;
        interaction_rumble_step(&rumble_state,controller.handle,
            (OrbisPadVibeParam){desired.lgMotor,desired.smMotor},sceKernelGetProcessTime());
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
        if (!closing) {
            /* The Home update card owns all UI edges while open. A close
             * edge cannot also navigate, open OPTIONS or start sign-in. */
            if (update_notice.open) {
                (void)x4_apple_update_notice_handle(&update_notice,
                    (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0);
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
            /* R3 belongs to local video settings only while the catalog is
             * idle. Consume the opening/closing frame, including any X edge. */
            if (screen.page == 5) {
                unsigned actions=0;
                if(controller.pressed&ORBIS_PAD_BUTTON_R3) actions|=X4_SETTINGS_OPEN;
                if(controller.pressed&ORBIS_PAD_BUTTON_UP) actions|=X4_SETTINGS_UP;
                if(controller.pressed&ORBIS_PAD_BUTTON_DOWN) actions|=X4_SETTINGS_DOWN;
                if(controller.pressed&ORBIS_PAD_BUTTON_LEFT) actions|=X4_SETTINGS_LEFT;
                if(controller.pressed&ORBIS_PAD_BUTTON_RIGHT) actions|=X4_SETTINGS_RIGHT;
                if(controller.pressed&ORBIS_PAD_BUTTON_CROSS) actions|=X4_SETTINGS_CONFIRM;
                if(controller.pressed&ORBIS_PAD_BUTTON_CIRCLE) actions|=X4_SETTINGS_CANCEL;
                bool can_edit=!catalog_browser.editing&&!x4_auth_busy(auth)&&pending_auth<0&&
                    !pending_launch&&!session_back&&!live_retained&&!access_query;
                if (stream_settings_ui.open && can_edit &&
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
                if(stream_settings_consumed) ui_pressed&=ORBIS_PAD_BUTTON_OPTIONS;
                if(stream_settings_consumed || stream_settings_ui.open) controller.pressed = 0;
            } else stream_settings_ui.open=false;
#elif X4_STREAM_PROFILE
            (void)stream_settings_ui;
#endif
            bool can_edit_pic = (picture_settings_ui.first_time_mode ||
                ((screen.page == 0 || screen.page == 4 || screen.page == 5 || screen.page == 8) && !x4_auth_busy(auth) && pending_auth < 0)) &&
                !game_input && !catalog_browser.editing && !pending_launch && !session_back && !live_retained && !closing && !access_query;
            unsigned pic_actions = 0;
            if ((controller.pressed & ORBIS_PAD_BUTTON_OPTIONS) && screen.page == 8 && !picture_settings_ui.open) {
                picture_settings_ui.tab = 1;
            }
            if (controller.pressed & ORBIS_PAD_BUTTON_OPTIONS) pic_actions |= X4_PIC_SETTINGS_OPEN;
            if (controller.pressed & ORBIS_PAD_BUTTON_UP) pic_actions |= X4_PIC_SETTINGS_UP;
            if (controller.pressed & ORBIS_PAD_BUTTON_DOWN) pic_actions |= X4_PIC_SETTINGS_DOWN;
            if (controller.pressed & ORBIS_PAD_BUTTON_LEFT) pic_actions |= X4_PIC_SETTINGS_LEFT;
            if (controller.pressed & ORBIS_PAD_BUTTON_RIGHT) pic_actions |= X4_PIC_SETTINGS_RIGHT;
            if (controller.pressed & ORBIS_PAD_BUTTON_CROSS) pic_actions |= X4_PIC_SETTINGS_CONFIRM;
            if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) pic_actions |= X4_PIC_SETTINGS_CANCEL;
            if (controller.pressed & ORBIS_PAD_BUTTON_L1) pic_actions |= X4_PIC_SETTINGS_PREV_TAB;
            if (controller.pressed & ORBIS_PAD_BUTTON_R1) pic_actions |= X4_PIC_SETTINGS_NEXT_TAB;
            bool pic_consumed = x4_picture_settings_ui_handle(&picture_settings_ui, pic_actions, can_edit_pic);
            if (pic_consumed || picture_settings_ui.open) {
                ui_pressed = 0;
                controller.pressed = 0;
            }
            if (picture_settings_ui.server_changed) {
                picture_settings_ui.server_changed = false;
                if (screen.page == 8 && !x4_auth_busy(auth)) {
                    pending_auth = X4_AUTH_FORTNITE_QUERY;
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
                bool eligible = catalog.state == X4_CATALOG_ERROR &&
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
                        pending_auth = access_query_page == 8 ?
                            X4_AUTH_FORTNITE_QUERY : X4_AUTH_XBOX_CATALOG;
                    } else if (action == X4_ACCESS_UI_CANCEL_QUERY) {
                        pending_auth = -1;
                        x4_auth_cancel(auth);
                        access_query_cancelled = true;
                        if (!access_query_started) access_query = false;
                    } else if (action == X4_ACCESS_UI_BACK) {
                        access_card_visible = false;
                        if (screen.page == 8 || screen.catalog_from_home) {
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
                    trace_inactive(live);
                    x4_auth_cancel(auth);
                    session_back = 1;
                    break;
                case X4_TOUCH_MENU_ACTION_TOGGLE_MUTE:
                    live_muted = !live_muted;
                    if (live) x4_live_media_set_muted(live, live_muted != 0);
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
            x4_screen_update(&screen, ui_pressed, controller.data.buttons);
            if (screen.open_settings_requested) {
                screen.open_settings_requested = 0;
                if (can_edit_pic) {
                    (void)x4_picture_settings_ui_handle(&picture_settings_ui, X4_PIC_SETTINGS_OPEN, true);
                }
            }
            if (screen.explore_catalog_requested) {
                screen.explore_catalog_requested = 0;
                catalog_browser.apple_mode = true;
                catalog_browser.active_tab = X4_CAT_TAB_DISCOVER;
                catalog_browser.top_bar_focused = false;
                catalog_browser.hero_focused = true;
                if (screen.signed_in) {
                    screen.page = 5;
                    screen.catalog_from_home = 1;
                    if (!x4_auth_busy(auth) && pending_auth < 0) {
                        if (catalog.state != X4_CATALOG_READY) {
                            catalog_selected = 0;
                            pending_auth = X4_AUTH_XBOX_CATALOG;
                        } else {
                            pending_auth = X4_AUTH_XBOX_RECENTS;
                        }
                    }
                } else {
                    screen.auto_catalog_after_login = 1;
                    screen.page = 4;
                    if (!x4_auth_busy(auth) && pending_auth < 0) {
                        pending_auth = X4_AUTH_SIGN_IN;
                    }
                }
            }
        }
        if ((previous_page == 0 || previous_page == 4 || previous_page == 5 || previous_page == 6 || previous_page == 8) && screen.page != previous_page) {
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
        if (!closing && screen.page == 4 && previous_page == 4) {
            if (fortnite_login_flow) {
                if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                    fortnite_login_flow = false;
                    trace_inactive(live);
                    x4_auth_cancel(auth);
                    screen.page = 0;
                } else if (account.state == X4_AUTH_AUTHORIZED && !x4_auth_busy(auth)) {
                    fortnite_login_flow = false;
                    screen.page = 8;
                    screen.selected = (int)x4_picture_settings_get();
                    pending_auth = X4_AUTH_FORTNITE_QUERY;
                }
            } else if (screen.auto_catalog_after_login && account.state == X4_AUTH_AUTHORIZED && !x4_auth_busy(auth)) {
                screen.auto_catalog_after_login = 0;
                screen.catalog_from_home = 1;
                screen.page = 5;
                catalog_browser.apple_mode = true;
                catalog_browser.active_tab = X4_CAT_TAB_DISCOVER;
                catalog_browser.top_bar_focused = false;
                catalog_browser.hero_focused = true;
                if (pending_auth < 0) {
                    if (catalog.state != X4_CATALOG_READY) {
                        catalog_selected = 0;
                        pending_auth = X4_AUTH_XBOX_CATALOG;
                    } else {
                        pending_auth = X4_AUTH_XBOX_RECENTS;
                    }
                }
            } else if (!x4_auth_busy(auth) && pending_auth < 0) {
                if (controller.pressed & ORBIS_PAD_BUTTON_CROSS) pending_auth = X4_AUTH_SIGN_IN;
#if !X4_PUBLIC
                else if (controller.pressed & ORBIS_PAD_BUTTON_SQUARE) pending_auth = X4_AUTH_CHECK_CONNECTION;
#endif
                else if (controller.pressed & ORBIS_PAD_BUTTON_TRIANGLE) {
                    x4_catalog_assets_suspend(catalog_assets);
                    pending_auth = -1;
                    if (!x4_auth_busy(auth)) x4_auth_forget(auth);
                    x4_recent_init(&recent);
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
        if (pending_auth >= 0) {
            x4_catalog_assets_suspend(catalog_assets);
            if (x4_catalog_assets_fault(catalog_assets) == X4_ASSET_FAULT_CLEANUP) {
                pending_auth = -1;
                pending_launch = 0;
                snprintf(ui_notice, sizeof(ui_notice), "%s", x4_tr(X4_TR_NOTICE_COVER_CLEANUP));
            } else if (!x4_auth_busy(auth) && x4_catalog_assets_idle(catalog_assets)) {
                enum X4AuthAction action = (enum X4AuthAction)pending_auth;
                pending_auth = -1;
                rc = x4_auth_start(auth, action);
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
        bool catalog_copied = x4_auth_catalog_snapshot_cached(auth, &catalog, &catalog_cursor) != 0;
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
            ++ui_generation;
            x4_catalog_presentation_set_generation(&presentation, ui_generation);
            x4_catalog_assets_set_generation(catalog_assets, ui_generation);
            x4_catalog_presentation_apply_names(&presentation, &catalog);
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
        bool metadata_changed = false;
        if ((screen.page == 0 || screen.page == 5) && !x4_auth_busy(auth)) {
            X4ProductMetadata completed[X4_CATALOG_ASSET_SLOTS];
            uint64_t revision = 0;
            size_t n = x4_catalog_assets_copy_metadata(catalog_assets, completed, X4_CATALOG_ASSET_SLOTS, &revision);
            if (revision != metadata_revision) {
                metadata_revision = revision;
                x4_catalog_presentation_ingest(&presentation, ui_generation, revision, completed, n);
                metadata_changed = x4_catalog_presentation_apply_names(&presentation, &catalog);
                if (metadata_changed) {
                    x4_catalog_browser_rebuild(&catalog_browser, &catalog);
                    x4_recent_refresh_catalog(&recent, &catalog);
                    x4_catalog_browser_rebuild_featured(&catalog_browser, &catalog, &recent);
                }
            }
        }
        x4_auth_session_snapshot(auth, &session);
        if (!closing && screen.page == 6 && previous_page == 6) {
            if ((controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) && !game_input && !session_back) {
                trace_inactive(live);
                x4_auth_cancel(auth);
                session_back = 1;
            }
            /* Keep the session page visible until remote cleanup finishes. */
            if (session_back && !x4_auth_busy(auth)) {
                session_back = 0;
                if (is_fortnite_session) {
                    screen.page = 8;
                    screen.selected = (int)x4_picture_settings_get();
                    is_fortnite_session = false;
                } else {
                    screen.page = 5;
                    pending_auth = X4_AUTH_XBOX_RECENTS;
                }
            }
        }
        if (!closing && screen.page == 5 && previous_page == 5) {
            static uint64_t search_debounce_deadline = 0;
            uint32_t pressed = controller.pressed;
            bool queued_launch_ready = false;
            if (pending_launch) {
                const X4CatalogTitle *chosen = x4_catalog_browser_selected_title(&catalog_browser, &catalog, &recent);
                if (x4_catalog_assets_fault(catalog_assets) == X4_ASSET_FAULT_CLEANUP) {
                    pending_launch = 0;
                    snprintf(ui_notice, sizeof(ui_notice), "%s", x4_tr(X4_TR_NOTICE_COVER_CLEANUP));
                } else if (pressed || catalog_copied || recent_copied || pending_generation != ui_generation ||
                    !chosen || strcmp(chosen->id, pending_title_id)) pending_launch = 0;
                else if (!metadata_changed && !x4_auth_busy(auth) && x4_catalog_assets_idle(catalog_assets)) {
                    queued_launch_ready = true;
                    pending_launch = 0;
                }
            }
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
            } else if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) &&
                       catalog.state == X4_CATALOG_READY && !x4_auth_busy(auth)) {
                catalog_browser.editing = true;
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
                    x4_catalog_browser_cycle_filter(&catalog_browser, -1);
                if (pressed & ORBIS_PAD_BUTTON_R2)
                    x4_catalog_browser_cycle_filter(&catalog_browser, 1);
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
                if ((pressed & ORBIS_PAD_BUTTON_CROSS) && catalog_browser.top_bar_focused) {
                    (void)x4_catalog_browser_activate_top_bar(&catalog_browser);
                    pressed &= ~ORBIS_PAD_BUTTON_CROSS;
                }
                if (catalog_browser.open_fortnite_requested) {
                    catalog_browser.open_fortnite_requested = false;
                    catalog_browser.active_tab = X4_CAT_TAB_DISCOVER;
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
                bool launch_edge = queued_launch_ready || (pressed & ORBIS_PAD_BUTTON_CROSS);
                if (launch_edge && !catalog_copied && !recent_copied && !metadata_changed && !selection_changing &&
                    pending_auth < 0 && !x4_auth_busy(auth) &&
                    x4_catalog_browser_resolved_index(&catalog_browser, &catalog, &recent, &source_index)) {
                    x4_catalog_assets_suspend(catalog_assets);
                    if (x4_catalog_assets_fault(catalog_assets) == X4_ASSET_FAULT_CLEANUP) {
                        snprintf(ui_notice, sizeof(ui_notice), "%s", x4_tr(X4_TR_NOTICE_COVER_CLEANUP));
                    } else if (!x4_catalog_assets_idle(catalog_assets)) {
                        pending_launch = 1;
                        pending_generation = ui_generation;
                        snprintf(pending_title_id, sizeof(pending_title_id), "%s", catalog.titles[source_index].id);
                    } else {
                    /* The visible filtered ordinal is never a session index. */
                    catalog_selected = source_index;
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
                        if(!live_error)interaction_rumble_stop(&rumble_state,controller.handle);
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
#if X4_STREAM_PROFILE
                        rc = x4_auth_start_session_request(auth,catalog_selected,&request_cfg);
#else
                        rc = x4_auth_start_session(auth, catalog_selected);
#endif
                    }
                    if (rc && !live_error) live_error = rc;
#if X4_STREAM_PROFILE
                    loading_begin_cached(catalog_assets,catalog_selected<catalog.count?&catalog.titles[catalog_selected]:NULL,sceKernelGetProcessTime());
#endif
                    x4_auth_session_snapshot(auth, &session);
                    if (rc == 0 || session.state == X4_SESSION_ERROR || live_error) {
                        session_back = 0;
                        screen.page = 6;
                    }
                    if (rc == X4_AUTH_E_SIGNED_OUT) screen.page = 4;
                    if (rc) printf("XCloud4: solicitar sesion fallo 0x%08x\n", (unsigned)rc);
                    }
                }
            }
        }
        if (!closing && screen.page == 8 && previous_page != 8) {
            if (account.state != X4_AUTH_AUTHORIZED) {
                fortnite_login_flow = true;
                screen.page = 4;
                pending_auth = X4_AUTH_SIGN_IN;
            } else {
                screen.selected = (int)x4_picture_settings_get();
                if (!x4_auth_busy(auth) && pending_auth < 0) {
                    pending_auth = X4_AUTH_FORTNITE_QUERY;
                }
            }
        }
        if (!closing && screen.page == 8 && previous_page == 8 && !picture_settings_ui.open) {
            uint32_t pressed = controller.pressed;
            if ((pressed & ORBIS_PAD_BUTTON_CROSS) && catalog.state == X4_CATALOG_READY &&
                catalog.count > 0 && !x4_auth_busy(auth) && pending_auth < 0) {
                x4_picture_settings_set((X4PictureQualityOption)screen.selected);
#if !X4_PUBLIC
                X4StreamRequestProfile prof = x4_picture_settings_to_profile((X4PictureQualityOption)screen.selected);
                x4_stream_settings_set_lab(prof, X4_COLOR_AUTO_709, X4_CPU_0, X4_DEPTH_AUTO,
                    X4_INTERACTION_VSYNC_OFF, X4_CONVERT_SMOOTH_DEFAULT, X4_SHARPNESS_PUBLIC_DEFAULT, 0);
#endif
                catalog_selected = 0;
                live_presented = false;
                live_error = 0;
#if X4_STREAM_PROFILE
                video_start_error = false;
#if !X4_PUBLIC
                stream_received_until = 0;
#endif
                X4StreamRequestConfig request_cfg = x4_stream_settings_get();
#endif
                if (live) {
                    trace_inactive(live);
                    live_error = x4_auth_set_media_callback(auth, NULL, NULL, NULL);
#if X4_STREAM_PROFILE && X4_RUMBLE
                    if (!live_error) interaction_rumble_stop(&rumble_state, controller.handle);
#endif
                    if (!live_error) live_error = x4_live_media_close(live);
                    if (!live_error) live = NULL;
                }
                if (!live_error) {
                    live_retained = 0;
#if X4_STREAM_PROFILE
                    interaction_main_begin(request_cfg.interaction_modes, sceKernelGetProcessTime());
                    x4_rtc_input_metrics_reset();
                    live = x4_live_media_create_request(&live_error, &request_cfg);
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
                if (!rc) rc = x4_auth_set_grace_inbox(auth, x4_live_media_grace_inbox(live));
#if X4_STREAM_PROFILE
                if (!rc) rc = x4_auth_set_stream_startup(auth, x4_live_media_stream_startup(live));
                if (!rc) rc = x4_auth_set_keyframe_limiter(auth, x4_live_media_keyframe_limiter(live));
#endif
                if (!rc) {
                    game_controls = 0;
#if X4_STREAM_PROFILE
                    rc = x4_auth_start_session_request(auth, 0, &request_cfg);
#else
                    rc = x4_auth_start_session(auth, 0);
#endif
                }
                if (rc && !live_error) live_error = rc;
#if X4_STREAM_PROFILE
                loading_begin_cached(catalog_assets,catalog.count?&catalog.titles[0]:NULL,sceKernelGetProcessTime());
#endif
                x4_auth_session_snapshot(auth, &session);
                if (rc == 0 || session.state == X4_SESSION_ERROR || live_error) {
                    session_back = 0;
                    screen.page = 6;
                    is_fortnite_session = true;
                }
                if (rc == X4_AUTH_E_SIGNED_OUT) screen.page = 4;
                if (rc) printf("XCloud4: iniciar sesion Fortnite fallo 0x%08x\n", (unsigned)rc);
            }
        }
        if (!closing && screen.page == 5 && catalog.state == X4_CATALOG_READY && !x4_auth_busy(auth) &&
            pending_auth < 0 && !pending_launch) {
            char wanted[X4_CATALOG_ASSET_SLOTS][X4_PRODUCT_ID_CAP];
            const char *ids[X4_CATALOG_ASSET_SLOTS];
            size_t wanted_count = x4_catalog_presentation_demand(&presentation, &catalog_browser, &catalog, &recent, wanted);
            for (unsigned fi = 0; fi < catalog_browser.featured_count && wanted_count < X4_CATALOG_ASSET_SLOTS; ++fi) {
                unsigned fsrc = catalog_browser.featured_sources[fi];
                if (fsrc < catalog.count && catalog.titles[fsrc].product_id[0]) {
                    bool present = false;
                    for (size_t k = 0; k < wanted_count; ++k) {
                        if (!strcmp(wanted[k], catalog.titles[fsrc].product_id)) { present = true; break; }
                    }
                    if (!present) {
                        snprintf(wanted[wanted_count], X4_PRODUCT_ID_CAP, "%s", catalog.titles[fsrc].product_id);
                        ++wanted_count;
                    }
                }
            }
            for (size_t i = 0; i < wanted_count; ++i) ids[i] = wanted[i];
            x4_catalog_assets_request(catalog_assets, ids, wanted_count);
            x4_catalog_assets_resume(catalog_assets);
        } else if (!closing && (screen.page == 0 || screen.page == 8) && !x4_auth_busy(auth) && pending_auth < 0 && !pending_launch) {
            const char *home_ids[2];
            size_t home_count = 0;
            home_ids[home_count++] = "BT5P2X999VH2";
            if (catalog.state == X4_CATALOG_READY && catalog_browser.featured_count > 0) {
                unsigned fsrc = catalog_browser.featured_sources[0];
                if (fsrc < catalog.count && catalog.titles[fsrc].product_id[0] &&
                    strcmp(catalog.titles[fsrc].product_id, "BT5P2X999VH2") != 0) {
                    home_ids[home_count++] = catalog.titles[fsrc].product_id;
                }
            }
            x4_catalog_assets_request(catalog_assets, home_ids, home_count);
            x4_catalog_assets_resume(catalog_assets);
        } else x4_catalog_assets_suspend(catalog_assets);
        if (live && !live_retained) {
            x4_live_media_tick(live);
            x4_live_media_snapshot(live, &live_status);
            if (x4_auth_busy(auth)) {
                if (x4_live_media_take_keyframe_request(live)) x4_auth_request_keyframe(auth);
                if (live_status.video_error) {
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
                if(!stop_rc)interaction_rumble_stop(&rumble_state,controller.handle);
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
                live_error=rc;trace_inactive(live);x4_auth_cancel(auth);
                printf("XCloud4: cerrar pantalla UI fallo 0x%08x; recursos conservados, no se puede dibujar el aviso, cierre seguro\n",(unsigned)rc);
                break;
            }
            rc=x4_live_media_stream_display_open(live);
            if(rc){
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
                };
#if X4_STREAM_PROFILE
                interaction_phase(X4_MON_PHASE_DRAW,sceKernelGetProcessTime());
#endif
                int stream_rc=x4_live_media_stream_present(live,stream_overlay,&overlay);
                if(stream_rc<0&&!live_error){
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
            ((screen.page == 8 || screen.page == 5) && (x4_auth_busy(auth) || catalog.state == X4_CATALOG_LOADING)) ||
            (screen.page == 6 && x4_auth_busy(auth)));
        x4_apple_set_animation_phase(ui_animated ? (unsigned)(draw_at / 100000u) : 0);
        /* Offer only after this frame's navigation/panel/auth actions. An
         * existing release result supplies the version and validated tag;
         * this UI path performs no HTTP request. */
        bool update_home_idle = !closing && !screen.exit_requested && screen.page == 0 &&
            !picture_settings_ui.open && !x4_server_settings_awaiting_first_choice() &&
            !screen.open_settings_requested && !screen.explore_catalog_requested &&
            !fortnite_login_flow && pending_auth < 0 && !pending_launch &&
            !x4_auth_busy(auth) && !live_retained && !session_back;
#if X4_STREAM_PROFILE && !X4_PUBLIC
        update_home_idle = update_home_idle && !stream_settings_ui.open;
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
            (void)x4_apple_update_notice_offer(&update_notice,version,
                valid_tag ? tag : NULL,true,false);
        }
        bool ui_reuse_work=false;
        if (!live_visible && screen.page != 3 && screen.page != 6) {
            uint32_t next_ui_sig = 2166136261u;
#define UI_HASH(v) do { next_ui_sig ^= (uint32_t)(v); next_ui_sig *= 16777619u; } while (0)
            UI_HASH(screen.page);
            if (screen.page == 2) UI_HASH(x4_release_check_status(NULL, 0));
            UI_HASH(screen.selected);
            UI_HASH(screen.signed_in);
            UI_HASH(screen.exit_error);
            UI_HASH(update_notice.open);
            UI_HASH(update_notice.shown);
            UI_HASH(x4_apple_get_animation_phase());
            UI_HASH(account.state);
            UI_HASH(account.error);
            UI_HASH(account.seconds_left);
            UI_HASH(account.http_status);
            UI_HASH(catalog.state);
            UI_HASH(catalog.count);
            UI_HASH(catalog_browser.active_tab);
            UI_HASH(catalog_browser.top_bar_focused);
            UI_HASH(catalog_browser.top_bar_index);
            UI_HASH(catalog_browser.hero_focused);
            UI_HASH(catalog_browser.featured_index);
            UI_HASH(catalog_browser.discover_row_selected);
            UI_HASH(catalog_browser.selected);
            UI_HASH(catalog_browser.recent_selected);
            UI_HASH(catalog_browser.filter);
            UI_HASH(catalog_browser.editing);
            UI_HASH(catalog_browser.keyboard_cursor);
            UI_HASH(catalog_browser.detail_scroll_offset);
            UI_HASH(catalog_browser.filtered_count);
            for (size_t qi = 0; qi < sizeof(catalog_browser.query) && catalog_browser.query[qi]; ++qi) {
                UI_HASH(catalog_browser.query[qi]);
            }
            UI_HASH(metadata_revision);
            UI_HASH(x4_catalog_assets_idle(catalog_assets));
            UI_HASH(x4_auth_busy(auth));
            UI_HASH(picture_settings_ui.open);
            UI_HASH(picture_settings_ui.tab);
            UI_HASH(picture_settings_ui.selected);
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
        bool loading_visible=false,loading_full_damage=false;
        bool options_ready=x4_picture_settings_ui_cache_ready(&picture_settings_ui);
        bool keyboard_ready=x4_catalog_gui_keyboard_cache_ready(&catalog_browser);
        if(screen.page!=5||!catalog_browser.editing)x4_catalog_gui_keyboard_cache_invalidate();
        bool drew_live=false,drawn_fresh=false;
        uint64_t drawn_generation=0,drawn_output=0;
        enum X4UiScreen ui_metric=X4_UI_OTHER;bool ui_navigation=false;
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
        /* A modal owns its frozen WB backdrop until close. Do not redraw the
         * underlying page, blur or dim it again for focus navigation. */
        if(!options_ready){
        if (!drew_live && screen.page != 5 && !loading_visible) {
            if (screen.page == 0 || screen.page == 8 || screen.page == 2 || screen.page == 4) {
                static X4HomeDrawContext hctx;
                static X4CatalogAssetView fn_view, feat_view;
                memset(&hctx, 0, sizeof(hctx));
                memset(&fn_view, 0, sizeof(fn_view));
                memset(&feat_view, 0, sizeof(feat_view));
                hctx.app_icon = &ui_app_icon.surface[X4_COVER_GRID];
                hctx.signed_in = (account.state == X4_AUTH_AUTHORIZED);
                snprintf(hctx.gamertag, sizeof(hctx.gamertag), "%s", catalog.gamertag);
                int fn_acq = (catalog_assets && x4_catalog_assets_acquire(catalog_assets, "BT5P2X999VH2", &fn_view));
                int feat_acq = 0;
                if (catalog_assets && catalog.state == X4_CATALOG_READY && catalog_browser.featured_count > 0) {
                    unsigned fsrc = catalog_browser.featured_sources[0];
                    if (fsrc < catalog.count && catalog.titles[fsrc].product_id[0]) {
                        feat_acq = x4_catalog_assets_acquire(catalog_assets, catalog.titles[fsrc].product_id, &feat_view);
                    }
                }
                if (fn_acq) {
                    hctx.fortnite_cover = fn_view.surface[X4_COVER_GRID].rgba
                        ? &fn_view.surface[X4_COVER_GRID]
                        : (fn_view.surface[X4_COVER_DETAIL].rgba ? &fn_view.surface[X4_COVER_DETAIL] : NULL);
                }
                if (feat_acq) {
                    hctx.featured_hero = feat_view.surface[X4_COVER_DETAIL].rgba
                        ? &feat_view.surface[X4_COVER_DETAIL]
                        : (feat_view.surface[X4_COVER_GRID].rgba ? &feat_view.surface[X4_COVER_GRID] : NULL);
                } else if (fn_acq && fn_view.surface[X4_COVER_DETAIL].rgba) {
                    hctx.featured_hero = &fn_view.surface[X4_COVER_DETAIL];
                }
                x4_screen_set_home_context(&hctx);
                x4_screen_draw(&screen, &controller, x4_display_pixels(&display));
                if (screen.page == 8) x4_fortnite_summary_draw(&screen, &catalog, x4_auth_busy(auth), x4_display_pixels(&display));
                if (feat_acq) x4_catalog_assets_release(catalog_assets, &feat_view);
                if (fn_acq) x4_catalog_assets_release(catalog_assets, &fn_view);
            } else {
                x4_screen_draw(&screen, &controller, x4_display_pixels(&display));
            }
        }
        if (screen.page == 3) x4_media_draw(&video, &audio, x4_display_pixels(&display));
        if (screen.page == 6) {
            if (drew_live)
                x4_live_overlay(&live_status, &session, live_muted, &touch_menu, show_banner, x4_display_pixels(&display));
            else if(!loading_visible) {
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
                x4_session_draw(&shown, x4_auth_busy(auth), closing, x4_display_pixels(&display));
                x4_live_status_draw(&live_status, live_error, x4_display_pixels(&display));
            }
        }
        else if (screen.page == 4 || closing)
            x4_auth_draw(&account, x4_auth_busy(auth), closing, x4_display_pixels(&display));
        else if (screen.page == 5) {
            X4CatalogGuiStatus gui = {controller.data.connected, x4_auth_busy(auth),
                pending_launch || pending_auth >= 0, ui_notice};
            x4_catalog_gui_draw_cached(&catalog, &catalog_browser, &recent, catalog_assets, &gui, x4_display_pixels(&display));
#if X4_STREAM_PROFILE && !X4_PUBLIC
            x4_stream_settings_ui_draw(&stream_settings_ui,x4_display_pixels(&display));
            if (stream_settings_ui.open)
                x4_apple_draw_update_test_action(x4_display_pixels(&display),310,924);
#endif
        }
        }
        if (picture_settings_ui.open) {
            x4_picture_settings_ui_draw_cached(&picture_settings_ui, x4_display_pixels(&display));
        }
        if (!ui_reuse_work && screen.page == 0 && update_notice.open)
            x4_apple_update_notice_draw(&update_notice,x4_display_pixels(&display));
        x4_exit_error_draw(screen.exit_error, x4_display_pixels(&display));
        if(screen.exit_error||live_error||(screen.page==6&&session.state==X4_SESSION_ERROR))ui_metric=X4_UI_ERRORS;
        else if(picture_settings_ui.open){
            ui_metric=picture_settings_ui.first_time_mode?X4_UI_FIRST_TIME:
                picture_settings_ui.tab?X4_UI_OPTIONS_SERVER:X4_UI_OPTIONS_PICTURE;
            ui_navigation=options_ready;
        }else if(screen.page==5&&catalog_browser.editing){ui_metric=X4_UI_KEYBOARD;ui_navigation=keyboard_ready;}
#if X4_STREAM_PROFILE && !X4_PUBLIC
        else if(stream_settings_ui.open)ui_metric=X4_UI_LAB_SETTINGS;
#endif
        else switch(screen.page){
            case 0:ui_metric=X4_UI_HOME;break;case 2:ui_metric=X4_UI_ABOUT;break;
            case 3:ui_metric=X4_UI_DEMO;break;case 4:ui_metric=X4_UI_ACCOUNT;break;
            case 5:ui_metric=X4_UI_CATALOG;break;
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
        else if(!screen.exit_error&&!drew_live){
            if(loading_visible&&!loading_full_damage){const X4UiRect r={510,230,900,680};x4_display_set_damage(&display,&r,1);}
            else if(picture_settings_ui.open){
                X4PictureSettingsCacheStats cache;x4_picture_settings_ui_cache_snapshot(&cache);
                if(!cache.last_draw_full){const X4UiRect r={100,126,1720,836};x4_display_set_damage(&display,&r,1);}
            }else if(screen.page==5&&catalog_browser.editing){
                X4CatalogKeyboardCacheStats cache;x4_catalog_gui_keyboard_cache_snapshot(&cache);
                if(!cache.last_draw_full){const X4UiRect r={420,200,1080,680};x4_display_set_damage(&display,&r,1);}
            }
        }
        uint64_t ui_draw_end=sceKernelGetProcessTime();
        uint64_t present_begin = sceKernelGetProcessTime();
        rc = drew_live && trace_active ?
            x4_display_present_trace(&display, live_trace, drawn_generation, drawn_fresh) :
            x4_display_present(&display);
        uint64_t present_end=sceKernelGetProcessTime();
        if(!drew_live){
            X4UiCopyStats copied;x4_display_copy_stats(&display,&copied);
            x4_ui_draw_metrics_record(ui_metric,ui_navigation,!ui_reuse_work,
                ui_reuse_work?0:ui_draw_end-ui_draw_begin,copied.elapsed_us,
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
    /* A display failure can break the loop early: cancel then join before
     * cleaning the UI. Ordinary OPTIONS keeps rendering while it cancels. */
    trace_inactive(live);
    x4_catalog_assets_suspend(catalog_assets);
    x4_catalog_assets_destroy(catalog_assets);
    catalog_assets = NULL;
    x4_auth_cancel(auth);
#if X4_RUMBLE
#if X4_STREAM_PROFILE
    interaction_rumble_stop(&rumble_state,controller.handle);
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
        auth = NULL;
        catalog_cursor = (X4AuthCatalogCursor){0};
        recent_cursor = (X4AuthRecentCursor){0};
        x4_recent_init(&recent);
        x4_catalog_browser_init(&catalog_browser);
    }
    printf("XCloud4: cerrar audio\n");
    x4_audio_stop(&audio);
    printf("XCloud4: cerrar decoder\n");
    x4_video_stop(&video);
    printf("XCloud4: cerrar control\n");
    x4_controller_close(&controller);
    printf("XCloud4: cerrar pantalla\n");
#if X4_STREAM_PROFILE
    int display_close_rc=x4_display_close_checked(&display);
    if(display_close_rc<0)live_error=display_close_rc;
#else
    x4_display_close(&display);
#endif
    x4_cover_rasters_free(&ui_app_icon);
    x4_loading_screen_end();
    x4_touch_menu_cache_reset();
    x4_ui_draw_metrics_flush(sceKernelGetProcessTime());
    x4_picture_settings_ui_cache_invalidate();x4_catalog_gui_keyboard_cache_invalidate();
    x4_font_close();
    printf("XCloud4: recursos cerrados\n");
    x4_release_check_stop();
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
