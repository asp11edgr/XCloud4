/* SPDX-License-Identifier: GPL-3.0-only */
/* Build the real core/main.c separately with only its entry point renamed.
 * These fixtures replace firmware and background service boundaries. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <orbis/GnmDriver.h>
#include <orbis/SystemService.h>
#include "audio/demo_audio.h"
#include "audio/mic_capture.h"
#include "auth/device_auth.h"
#include "auth/release_check.h"
#if !X4_PUBLIC && HOST51_XSTS_DIAG
#include "auth/xsts_claims_diag.h"
#endif
#include "core/lifecycle.h"
#include "input/controller.h"
#include "input/script_engine.h"
#include "media/live_trace.h"
#include "ui/catalog_assets.h"
#include "ui/lang.h"
#include "ui/picture_settings.h"
#include "ui/play_history.h"
#include "ui/ui2_boot.h"
#include "ui/ui2_net.h"
#include "ui/ui_lang_pref.h"
#include "video/display.h"
#include "video/h264_demo.h"
#ifdef X4_UPDATE51_ENABLED
#include "update/update_job51.h"
#endif

enum { FRAME_US = 100000, FRAMES = 300, MAX_COPIES = 128, SECONDS = 30 };
static const size_t pixel_count = (size_t)X4_UI_WIDTH * X4_UI_HEIGHT;
static const char *const products[] = {"BT5P2X999VH2", "TEST12345678", "TEST87654321"};
static const char *const title_ids[] = {"fixture-fortnite", "fixture-racer", "fixture-platform"};
static const char *const title_names[] = {"Fortnite", "Fixture Racer", "Fixture Platform"};
typedef struct {
    const char *name;
    unsigned restore, gamertag, catalog, recent, sigl, art, preference;
    bool prepublished;
} Scenario;
static const Scenario cases[] = {
    {"cold", 0, 0, 0, 0, 0, 0, 0, true},
    {"recent-first", 4, 27, 58, 42, 112, 162, 0, false},
    {"catalog-first", 3, 90, 42, 78, 134, 166, 0, false},
    {"preference-mid", 5, 60, 76, 43, 97, 181, 131, false},
};
static const Scenario *scenario;
static const char *directory;
static uint32_t *last;
static uint32_t covers[3][384 * 216];
static unsigned cutoff = FRAMES, frame_tick, reads, presents, skipped, boot_resets, requests, sigl_requests;
static unsigned lang_writes, preference_reads, art_reads, preload_reads, reload_at = UINT_MAX;
static unsigned diag_opens, diag_renames, diag_failures;
#if HOST51_SPLASH
static unsigned present_attempts, first_present_failures, presented_frames, splash_calls;
static bool splash_api_failure;
#endif
#ifdef X4_UPDATE51_ENABLED
static unsigned update_boot, update_exit;
#endif
static unsigned redraw_at[MAX_COPIES], redraws;
static uint64_t art_generation;
static bool spanish, closing, saw_published, sigl_demanded, art_demanded, preload_counted;
static struct X4Auth { int unused; } account_context;
static struct X4CatalogAssets { int unused; } assets_context;

int x4_boot51_real_main(void);
FILE *__real_fopen(const char *, const char *);
int __real_open(const char *, int, ...);
int __real_rename(const char *, const char *);
int __real_remove(const char *);
int __real_unlink(const char *);
int __real_mkdir(const char *, mode_t);
void __real_x4_ui_lang_pref_init(const char *);
void __real_x4_ui2_boot_reset(void);

static void path_for(char out[1024], const char *name)
{
    const int len = snprintf(out, 1024, "%s/%s", directory, name);
    assert(len > 0 && len < 1024);
}

static const char *mapped(const char *path, char out[1024])
{
    static const char prefix[] = "/data/xcloud4/";
    if (strncmp(path, prefix, sizeof(prefix) - 1) == 0) {
        path_for(out, path + sizeof(prefix) - 1);
        return out;
    }
    if (!strncmp(path, "/data/", 6)) {
        assert(!"unexpected sandbox path");
    }
    return !strncmp(path, "/app0/", 6) ? path + 6 : path;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
    char buffer[1024];
    if (!strncmp(path, X4_UI_LANG_PREF_PATH, strlen(X4_UI_LANG_PREF_PATH))) {
        if (strcmp(mode, "rb")) ++lang_writes;
        else ++preference_reads;
    }
    if (!strcmp(path, "/data/xcloud4/ui-boot-diag.json.tmp")) {
        assert(!strcmp(mode, "wb") && frame_tick >= 600);
        ++diag_opens;
    }
    return __real_fopen(mapped(path, buffer), mode);
}

int __wrap_rename(const char *from, const char *to)
{
    char a[1024], b[1024];
    if (!strcmp(to, "/data/xcloud4/ui-boot-diag.json")) {
        const char prefix[] = "/data/xcloud4/ui-boot-diag.json.";
        assert(!strncmp(from, prefix, sizeof(prefix) - 1));
        if (getenv("HOST51_DIAG_RETRY") && !diag_failures) {
            ++diag_failures;
            errno = EIO;
            return -1;
        }
        ++diag_renames;
    }
    return __real_rename(mapped(from, a), mapped(to, b));
}

int __wrap_open(const char *path, int flags, ...)
{
    char buffer[1024];
    if (!(flags & O_CREAT)) return __real_open(mapped(path, buffer), flags);
    va_list args;
    va_start(args, flags);
    const int mode = va_arg(args, int);
    va_end(args);
    const char prefix[] = "/data/xcloud4/ui-boot-diag.json.";
    if (!strncmp(path, prefix, sizeof(prefix) - 1)) {
        assert(mode == 0666 && (flags & (O_EXCL | O_NOFOLLOW)) == (O_EXCL | O_NOFOLLOW));
        assert(frame_tick >= (getenv("HOST51_DIAGNOSTIC") ? 600u : cutoff));
        ++diag_opens;
    }
    return __real_open(mapped(path, buffer), flags, mode);
}

int __wrap_unlink(const char *path)
{
    char buffer[1024];
    return __real_unlink(mapped(path, buffer));
}

int __wrap_remove(const char *path)
{
    char buffer[1024];
    return __real_remove(mapped(path, buffer));
}

int __wrap_mkdir(const char *path, mode_t mode)
{
    char buffer[1024];
    if (!strcmp(path, "/data") || !strcmp(path, "/data/xcloud4")) path = directory;
    return __real_mkdir(mapped(path, buffer), mode);
}

time_t __wrap_time(time_t *output)
{
    const time_t fixed = 1700000000;
    if (output) *output = fixed;
    return fixed;
}

void __wrap_x4_ui_lang_pref_init(const char *path)
{
    if (!scenario->preference) {
        __real_x4_ui_lang_pref_init(path);
        reload_at = frame_tick;
    } else {
        assert(path == NULL);
        /* Deferred completion fixture. Firmware load is synchronous today.
         * Force a different provisional UI language to exercise invalidation. */
        x4_lang_set_ui(X4_LANG_FR);
    }
}

void __wrap_x4_ui2_boot_reset(void)
{
    ++boot_resets;
    __real_x4_ui2_boot_reset();
}

uint64_t sceKernelGetProcessTime(void)
{
    return UINT64_C(1000000) + (uint64_t)frame_tick * FRAME_US;
}

int sceKernelUsleep(unsigned us)
{
    if (us == 4000) ++skipped;
    return 0;
}

int32_t sceGnmSubmitDone(void) { return 0; }

#if HOST51_SPLASH
int32_t sceSystemServiceHideSplashScreen(void)
{
    assert(presented_frames == 1);
    ++splash_calls;
    assert(splash_calls == 1);
    return splash_api_failure ? -EIO : 0;
}
#endif

X4ControllerRoster *x4_controller_roster_start(bool several)
{
    (void)several;
    return NULL;
}

void x4_controller_init(X4Controller *pad) { pad->handle = -1; }
void x4_controller_close(X4Controller *pad) { assert(pad->handle == -1); }

void x4_controller_read(X4Controller *pad, unsigned frame)
{
    assert(frame == reads && frame <= cutoff + 1);
    ++reads;
    frame_tick = frame;
    pad->pressed = 0;
    pad->data.buttons = 0;
    pad->data.connected = true;
    pad->data.leftStick.x = pad->data.leftStick.y = 128;
    pad->data.rightStick.x = pad->data.rightStick.y = 128;
    if (scenario->preference && frame == scenario->preference) {
        __real_x4_ui_lang_pref_init(NULL);
        reload_at = frame;
    }
}

/* A synthetic status-clock change after the measurement ends forces a final
 * VideoOut boundary, whose expected error takes main's normal cleanup route. */
int x4_ui2_local_minute(uint64_t now)
{
    (void)now;
    return frame_tick >= cutoff ? 721 : 720;
}

X4Ui2Network x4_ui2_network_type(uint64_t now)
{
    (void)now;
    return X4_UI2_NETWORK_CABLE;
}

int x4_display_open(X4Display *display)
{
    display->ui = x4_ui_surface_create();
    display->handle = 1;
    for (unsigned i = 0; i < X4_UI_SCANOUTS; ++i)
        display->buffers[i] = calloc(pixel_count, sizeof(uint32_t));
    assert(display->ui && display->buffers[0] && display->buffers[1]);
    return 0;
}

uint32_t *x4_display_pixels(X4Display *display)
{
    return x4_ui_surface_pixels(display->ui);
}

void x4_display_set_damage(X4Display *display, const X4UiRect *rects, size_t count)
{
    x4_ui_surface_set_damage(display->ui, rects, count);
}

void x4_display_copy_stats(const X4Display *display, X4UiCopyStats *out)
{
    *out = display->copy_stats;
}

int x4_display_present(X4Display *display)
{
#if HOST51_SPLASH
    if (presented_frames) assert(splash_calls == 1);
    if (++present_attempts <= first_present_failures) {
        assert(splash_calls == 0);
        return -EIO;
    }
#endif
    if (frame_tick >= cutoff) {
        closing = true;
        return -EINTR;
    }
    const unsigned index = (unsigned)display->index;
    uint32_t *pixels = display->buffers[index];
    assert(x4_ui_surface_publish(display->ui, pixels, index, &display->copy_stats) == 0);
#if HOST51_SPLASH
    ++presented_frames;
#endif
    if (reads) {
        if (!presents || memcmp(last, pixels, pixel_count * sizeof(*last))) {
            assert(redraws < MAX_COPIES);
            redraw_at[redraws++] = frame_tick;
            memcpy(last, pixels, pixel_count * sizeof(*last));
        }
        ++presents;
    }
    display->index ^= 1;
    return 0;
}

int x4_display_close_checked(X4Display *display)
{
    x4_ui_surface_destroy(display->ui);
    for (unsigned i = 0; i < X4_UI_SCANOUTS; ++i) free(display->buffers[i]);
    *display = (X4Display){.handle = -1};
    return 0;
}

int x4_exit_prepare(void) { return 0; }

static void verify_save_other_settings(void)
{
    char before[1024], after[1024];
    path_for(before, "unused-legacy.bin");
    path_for(after, "picture-test-v2.bin");
    assert(x4_picture_settings_set_paths(before, after));
    assert(x4_picture_settings_set(X4_PIC_QUALITY_1080P_ALTA));
    X4PictureSettingsUi settings = {0};
    x4_picture_settings_ui_open_section(&settings, X4_PIC_SECTION_TRANSMISSION);
    assert(x4_picture_settings_ui_handle(&settings, X4_PIC_SETTINGS_RIGHT, true));
    assert(x4_picture_settings_ui_dirty(&settings));
    settings.save_focused = true;
    assert(x4_picture_settings_ui_handle(&settings, X4_PIC_SETTINGS_CONFIRM, true));
    assert(!settings.save_failed && !settings.language_saving);
    assert(x4_picture_settings_get() == X4_PIC_QUALITY_1080P);
}

int x4_exit_request(void)
{
    assert(closing && frame_tick >= cutoff && presents && skipped && redraws);
    verify_save_other_settings();
    assert(lang_writes == 0 && preference_reads == 1);
    assert(x4_ui_lang_pref_get() == (spanish ? X4_UI_LANG_ES : X4_UI_LANG_AUTO));
    assert(x4_lang_current() == (spanish ? X4_LANG_ES : X4_LANG_EN));
    assert(reload_at == scenario->preference);
    assert(boot_resets == 1);
#if HOST51_SPLASH
    assert(splash_calls == 1 && present_attempts > first_present_failures + 1);
    printf("HOST51_SPLASH calls=%u failed_initial_presents=%u code=%d\n",
        splash_calls, first_present_failures, splash_api_failure ? -EIO : 0);
#endif
    if (!scenario->prepublished) assert(requests == 1);
    assert(sigl_requests == 1 && art_reads && preload_reads);
    assert(saw_published);
    if (!scenario->prepublished) {
        assert(redraw_at[0] == 0 && redraws >= 3);
        const unsigned expected[] = {scenario->restore, scenario->gamertag, scenario->catalog,
            scenario->recent, scenario->sigl, scenario->art, scenario->preference};
        for (size_t event = 0; event < sizeof(expected) / sizeof(expected[0]); ++event) {
            bool shown = false;
            for (unsigned i = 0; i < redraws; ++i) shown |= redraw_at[i] == expected[event];
            assert(shown);
        }
    }
#if HOST51_BOOT_DIAG
    const unsigned diagnostic_expected = !X4_PUBLIC;
#else
    const unsigned diagnostic_expected = !X4_PUBLIC && cutoff > 600 ? 1u : 0u;
#endif
    const unsigned failure_expected = diagnostic_expected && getenv("HOST51_DIAG_RETRY");
    const unsigned claims_refresh = HOST51_XSTS_DIAG && diagnostic_expected &&
        cutoff > 600 && !failure_expected;
    assert(diag_opens == diagnostic_expected + failure_expected + claims_refresh &&
        diag_renames == diagnostic_expected + claims_refresh && diag_failures == failure_expected);
#ifdef X4_UPDATE51_ENABLED
    assert(update_boot == 1 && update_exit == 1);
#endif
    char output[1024];
    path_for(output, "last.rgbx");
    FILE *file = __real_fopen(output, "wb");
    assert(file && fwrite(last, sizeof(*last), pixel_count, file) == pixel_count);
    assert(!fclose(file));
    printf("HOST51_LOOP scenario=%s language=%s inputs=0 simulated_s=%u polls=%u "
           "presents=%u changed=%u skipped=%u requests=%u sigl_requests=%u "
           "preference_reads=%u language_writes=%u other_settings_saved=1 boot_resets=%u "
           "diagnostic_writes=%u redraws=",
        scenario->name, spanish ? "es" : "auto", cutoff * SECONDS / FRAMES, reads, presents,
        redraws, skipped, requests, sigl_requests, preference_reads, lang_writes, boot_resets,
        diag_renames);
    for (unsigned i = 0; i < redraws; ++i) printf("%s%u", i ? "," : "", redraw_at[i]);
    puts("");
    free(last);
    exit(0);
}

X4Auth *x4_auth_create(void) { return &account_context; }
int x4_auth_close(X4Auth *auth) { assert(auth == &account_context); return 0; }
void x4_auth_cancel(X4Auth *auth) { assert(auth == &account_context && closing); }
int x4_auth_busy(X4Auth *auth)
{
    assert(auth == &account_context);
    return !closing && (frame_tick < scenario->restore ||
        (requests && (frame_tick < scenario->catalog || frame_tick < scenario->recent)));
}

int x4_auth_start(X4Auth *auth, enum X4AuthAction action)
{
    assert(auth == &account_context && !closing && frame_tick >= scenario->restore);
    assert(action == X4_AUTH_XBOX_CATALOG && !requests);
    ++requests;
    return 0;
}

void x4_auth_snapshot(X4Auth *auth, X4AuthSnapshot *out)
{
    assert(auth == &account_context);
    *out = (X4AuthSnapshot){.state = frame_tick >= scenario->restore ?
        X4_AUTH_AUTHORIZED : X4_AUTH_CONNECTING};
}

void x4_auth_remember_snapshot(X4Auth *auth, X4AuthRememberSnapshot *out)
{
    assert(auth == &account_context);
    *out = (X4AuthRememberSnapshot){.enabled = true, .available = true};
}

void x4_auth_session_snapshot(X4Auth *auth, X4SessionSnapshot *out)
{
    assert(auth == &account_context);
    *out = (X4SessionSnapshot){0};
}

void x4_auth_set_gamepad(X4Auth *auth, const X4GamepadRoster *input)
{
    assert(auth == &account_context && !input->pads[0].buttons);
}

bool x4_auth_poll_rumble(X4Auth *auth, uint64_t now, X4RumbleOutput *out)
{
    assert(auth == &account_context);
    (void)now;
    *out = (X4RumbleOutput){0};
    return false;
}

void x4_auth_cloud_time(X4Auth *auth, const char *market, bool force, X4CloudTime *out,
    uint64_t *epoch, uint64_t *revision)
{
    assert(auth == &account_context);
    (void)market; (void)force; (void)epoch; (void)revision;
    *out = (X4CloudTime){0};
}

int x4_auth_catalog_snapshot_cached(X4Auth *auth, X4CatalogSnapshot *out, X4AuthCatalogCursor *cursor)
{
    assert(auth == &account_context);
    const bool started = scenario->prepublished || requests;
    const bool ready = started && frame_tick >= scenario->catalog;
    const bool gamer = started && frame_tick >= scenario->gamertag;
    const uint64_t revision = 1u + (unsigned)started + 2u * ready + 4u * gamer;
    if (cursor->revision == revision) return 0;
    cursor->revision = revision;
    memset(out, 0, sizeof(*out));
    out->state = ready ? X4_CATALOG_READY : started ? X4_CATALOG_LOADING : X4_CATALOG_IDLE;
    if (gamer) snprintf(out->gamertag, sizeof(out->gamertag), "%s", "Jugador de prueba");
    if (ready) {
        out->count = out->total = 3;
        for (unsigned i = 0; i < out->count; ++i) {
            X4CatalogTitle *title = &out->titles[i];
            snprintf(title->id, sizeof(title->id), "%s", title_ids[i]);
            snprintf(title->product_id, sizeof(title->product_id), "%s", products[i]);
            snprintf(title->name, sizeof(title->name), "%s", title_names[i]);
            title->entitled = i != 0;
            title->f2p_only = title->is_free_in_store = i == 0;
        }
        saw_published = true;
    }
    return 1;
}

int x4_auth_recent_snapshot_cached(X4Auth *auth, X4RecentSnapshot *out, X4AuthRecentCursor *cursor)
{
    assert(auth == &account_context);
    const bool started = scenario->prepublished || requests;
    const bool ready = started && frame_tick >= scenario->recent;
    const uint64_t revision = 1u + (unsigned)started + (unsigned)ready;
    if (cursor->revision == revision) return 0;
    cursor->revision = revision;
    *out = (X4RecentSnapshot){.state = ready ? X4_CATALOG_READY :
        started ? X4_CATALOG_LOADING : X4_CATALOG_IDLE,
        .account_epoch = 1, .request_generation = started ? 1u : 0u};
    if (ready) {
        out->count = 2;
        snprintf(out->title_ids[0], sizeof(out->title_ids[0]), "%s", title_ids[1]);
        snprintf(out->title_ids[1], sizeof(out->title_ids[1]), "%s", title_ids[0]);
    }
    return 1;
}

X4CatalogAssets *x4_catalog_assets_create(void) { return &assets_context; }
void x4_catalog_assets_destroy(X4CatalogAssets *assets) { assert(assets == &assets_context && closing); }
void x4_catalog_assets_suspend(X4CatalogAssets *assets) { assert(assets == &assets_context); }
void x4_catalog_assets_resume(X4CatalogAssets *assets) { assert(assets == &assets_context); }
int x4_catalog_assets_idle(X4CatalogAssets *assets) { assert(assets == &assets_context); return 1; }
enum X4AssetFault x4_catalog_assets_fault(X4CatalogAssets *assets)
{
    assert(assets == &assets_context);
    return X4_ASSET_FAULT_NONE;
}
void x4_catalog_assets_set_generation(X4CatalogAssets *assets, uint64_t generation)
{
    assert(assets == &assets_context);
    art_generation = generation;
}

void x4_catalog_assets_request_sigls(X4CatalogAssets *assets, uint64_t session)
{
    assert(assets == &assets_context && session <= 1);
    if (session && !sigl_demanded) {
        sigl_demanded = true;
        ++sigl_requests;
    }
}

int x4_catalog_assets_copy_sigls(X4CatalogAssets *assets, X4SiglList lists[X4_SIGL_COUNT],
    uint64_t *revision)
{
    assert(assets == &assets_context);
    if (!sigl_demanded || frame_tick < scenario->sigl || *revision) return 0;
    X4SiglList *list = &lists[X4_SIGL_POPULAR];
    *list = (X4SiglList){.valid = true, .count = 3};
    for (unsigned i = 0; i < list->count; ++i)
        snprintf(list->ids[i], sizeof(list->ids[i]), "%s", products[2 - i]);
    *revision = 1;
    return 1;
}

int x4_catalog_assets_preload(X4CatalogAssets *assets, const X4CatalogArtDemand *demand, size_t count)
{
    assert(assets == &assets_context);
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(demand[i].id, products[1]) && (demand[i].mask & (1u << X4_COVER_ROW))) {
            art_demanded = true;
            if (!preload_counted) { ++preload_reads; preload_counted = true; }
        }
    }
    return 1;
}

void x4_catalog_assets_protect(X4CatalogAssets *assets, const X4CatalogArtDemand *demand, size_t count)
{
    assert(assets == &assets_context);
    (void)demand; (void)count;
}

void x4_catalog_assets_request_surfaces(X4CatalogAssets *assets, const char *const *ids,
    const uint32_t *masks, size_t count)
{
    assert(assets == &assets_context && count <= X4_CATALOG_ASSET_SLOTS);
    assert(count == 0 || (ids && masks));
}

static bool art_ready(void)
{
    return (scenario->prepublished || art_demanded) && frame_tick >= scenario->art;
}

size_t x4_catalog_assets_copy_metadata(X4CatalogAssets *assets, X4ProductMetadata *out,
    size_t capacity, uint64_t *revision)
{
    assert(assets == &assets_context);
    (void)out; (void)capacity;
    *revision = art_ready() ? 1 : 0;
    return 0;
}

void x4_catalog_assets_stats(X4CatalogAssets *assets, X4CatalogAssetStats *out)
{
    assert(assets == &assets_context);
    *out = (X4CatalogAssetStats){.revision = art_ready() ? 1u : 0u};
}

int x4_catalog_assets_acquire(X4CatalogAssets *assets, const char *product, X4CatalogAssetView *out)
{
    assert(assets == &assets_context);
    if (!art_ready()) return 0;
    for (unsigned i = 0; i < 3; ++i) {
        if (strcmp(products[i], product)) continue;
        *out = (X4CatalogAssetView){.generation = art_generation, .slot = i, .state = X4_ASSET_READY};
        out->surface[X4_COVER_ROW] = (X4CoverRaster){384, 216, covers[i]};
        ++art_reads;
        return 1;
    }
    return 0;
}

void x4_catalog_assets_release(X4CatalogAssets *assets, X4CatalogAssetView *view)
{
    assert(assets == &assets_context && view->generation == art_generation);
}

#if !X4_PUBLIC
int x4_catalog_assets_art_ready(X4CatalogAssets *assets, const X4CatalogArtDemand *demand,
    size_t count, uint64_t entered, X4CatalogArtReady *out)
{
    assert(assets == &assets_context);
    (void)demand; (void)entered;
    *out = (X4CatalogArtReady){.pending = art_ready() ? 0u : (unsigned)count};
    return 0;
}

void x4_script_engine_init(void) {}
void x4_script_engine_shutdown(void) {}
bool x4_script_engine_load_persistence(bool *accepted, char *saved, size_t capacity)
{
    (void)accepted; (void)saved; (void)capacity;
    return false;
}
bool x4_script_engine_export_session_json(const char *path) { (void)path; return true; }
bool x4_script_engine_get_notification(char *out, size_t capacity)
{
    (void)out; (void)capacity; return false;
}
void x4_script_engine_draw_notification(uint32_t *pixels) { (void)pixels; }
#endif

void x4_trace_monitor_state(X4Trace *trace, unsigned actor, unsigned phase, uint64_t at)
{
    assert(!trace);
    (void)actor; (void)phase; (void)at;
}
void x4_trace_set_active(X4Trace *trace, bool active) { assert(!trace && !active); }
void x4_trace_poll(X4Trace *trace) { assert(!trace); }
void x4_mic_session_end(void) {}
void x4_audio_stop(X4DemoAudio *audio) { assert(audio->handle == -1); }
int x4_video_stop(X4DemoVideo *video) { (void)video; return 0; }
void x4_release_check_start(void) {}
void x4_release_check_stop(void) {}
enum X4UpdateStatus x4_release_check_status(char *version, size_t capacity)
{
    if (version && capacity) version[0] = 0;
    return X4_UPDATE_LATEST;
}

#if !X4_PUBLIC && HOST51_XSTS_DIAG
uint64_t x4_xsts_diag_revision(void)
{
    return frame_tick >= 604 ? 2 : 1;
}

void x4_xsts_diag_snapshot(X4XstsDiagSnapshot *out)
{
    out->revision = x4_xsts_diag_revision();
    out->count = (unsigned)out->revision;
    out->responses[0] = (X4XstsDiagResponse){
        .party = X4_XSTS_DIAG_GSSV, .status = 200, .count = 2, .names = {"uhs", "gtg"}
    };
    out->responses[1] = (X4XstsDiagResponse){
        .party = X4_XSTS_DIAG_MARKET, .status = 200, .count = 1, .names = {"xid"}
    };
}
#endif

#ifdef X4_UPDATE51_ENABLED
void x4_update51_boot_cleanup(void) { ++update_boot; }
void x4_update51_exit(void) { ++update_exit; }
X4Update51View x4_update51_view(void) { return (X4Update51View){0}; }
bool x4_update51_start(const char *tag, bool local)
{
    (void)tag;
    (void)local;
    assert(!"update started without input");
    return false;
}
bool x4_update51_start_checked(const char *tag, bool local, bool allowed, X4Update51View *rejected)
{
    (void)allowed;
    (void)rejected;
    return x4_update51_start(tag, local);
}
void x4_update51_cancel(void) { assert(!"update cancelled without input"); }
bool x4_update51_pending(void) { return false; }
bool x4_update51_cancel_pending(void) { return false; }
#endif

static void setup_inputs(void)
{
    char path[1024];
    path_for(path, "history-v1.bin");
    X4History history = {0};
    const time_t today = time(NULL);
    assert(today > 0 && x4_history_record(&history, products[0], (uint64_t)today, 4127, path));
    path_for(path, "ui-lang-v1.bin");
    if (spanish) {
        unsigned char record[] = {'X','4','L','A','N','G',1,0,X4_UI_LANG_ES,0,0,0,0};
        uint32_t sum = UINT32_C(2166136261);
        for (unsigned i = 0; i < 9; ++i) sum = (sum ^ record[i]) * UINT32_C(16777619);
        for (unsigned i = 0; i < 4; ++i) record[9 + i] = (unsigned char)(sum >> (i * 8));
        FILE *file = __real_fopen(path, "wb");
        assert(file && fwrite(record, sizeof(record), 1, file) == 1 && !fclose(file));
    } else {
        assert(!__real_fopen(path, "rb") && errno == ENOENT);
    }
    for (unsigned k = 0; k < 3; ++k) {
        for (unsigned y = 0; y < 216; ++y) {
            for (unsigned x = 0; x < 384; ++x)
                covers[k][y * 384 + x] = 0xff000000u | ((x / 2) << (k * 8)) |
                    (y << (((k + 1) % 3) * 8)) | (70u << (((k + 2) % 3) * 8));
        }
    }
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    directory = argv[1];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        if (!strcmp(argv[2], cases[i].name)) scenario = &cases[i];
    assert(scenario && (!strcmp(argv[3], "es") || !strcmp(argv[3], "auto")));
    spanish = !strcmp(argv[3], "es");
    if (getenv("HOST51_DIAGNOSTIC")) cutoff = 610;
#if HOST51_SPLASH
    if (getenv("HOST51_SPLASH_BOOT_FAIL")) first_present_failures = 5;
    splash_api_failure = getenv("HOST51_SPLASH_API_FAIL") != NULL;
#endif
    setup_inputs();
    last = malloc(pixel_count * sizeof(*last));
    assert(last);
    return x4_boot51_real_main();
}
