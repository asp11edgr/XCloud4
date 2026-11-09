/* SPDX-License-Identifier: GPL-3.0-only */
#define X4_BENCH48_MAIN t48_bench_main
#include "bench_tarea48a2.c"
#undef X4_BENCH48_MAIN
#include "picture_settings.h"
#include "cloud_time_ui.h"
#if X4_BENCH_49
#include "ui2_account.h"
#include "ui2_info.h"
#include "ui2_modal.h"
#endif

static void home49_bench(uint32_t *pixels)
{
    X4Ui2HomeModel model;
    X4UiRect damage[2];
    x4_catalog_browser_home_rebuild(&browser, &catalog, &recent, sigls);
    x4_ui2_home_model(&model, true, &catalog, &recent, &browser.home_rows);
    DrawSamples samples = {0};
    uint64_t start = clock_us();
    size_t changed = x4_ui2_home_draw(&model, &catalog, &assets, 1,
        X4_UI2_HOME_CONTINUE, false, pixels, damage);
    samples.first_us = clock_us() - start;
    samples.first_full = changed == SIZE_MAX;
    x4_ui_draw_metrics_reset();
    for (unsigned frame = 0; frame < FRAMES; ++frame) {
        start = clock_us();
        changed = x4_ui2_home_draw(&model, &catalog, &assets, 1,
            frame & 1u ? X4_UI2_HOME_CONTINUE : X4_UI2_HOME_EXPLORE, true, pixels, damage);
        measure(&samples, X4_UI_MENU, frame, start, clock_us(), changed);
    }
    report(&samples, X4_UI_MENU, "inicio");
    x4_ui2_home_close();
}

static size_t secondary49_draw(unsigned scene, bool first, bool next, uint32_t *pixels,
    uint32_t *legacy, X4PictureSettingsUi *settings)
{
    const X4AuthSnapshot auth = {.state=X4_AUTH_AUTHORIZED};
#if X4_BENCH_49
    (void)legacy;
    switch (scene) {
    case 0: {
        X4UiRect damage[2];
        return x4_picture_settings_ui_draw_page_cached(settings, pixels, damage);
    }
    case 1: {
        X4UiRect damage[2];
        const X4Ui2Account account={.row=next};
        const X4Ui2AccountModel model={.auth=&auth,
            .remember={.available=true},.focus_owned=true,.gamertag="Jugador",.initial="J",
            .server=x4_tr(X4_TR_SERVER_OPT_AUTO),.country=x4_access_country_label(X4_ACCESS_AUTO),
            .cloud_time={.remaining_s=7200,.max_s=36000,.source=X4_CLOUD_TIME_ACCOUNT},
            .catalog=&catalog,.assets=&assets};
        return x4_ui2_account_draw(&account, &model, pixels, !first, damage);
    }
    case 2: x4_ui2_about_draw(pixels, NULL, X4_UPDATE_LATEST, "1.3.0"); break;
    case 3: x4_ui2_support_draw(pixels); break;
    default:
        if (first) x4_ui2_modal_scrim(pixels);
        x4_ui2_cloud_confirm_draw(pixels);
        return first ? SIZE_MAX : 1;
    }
#else
    (void)next;
    (void)first;
    switch (scene) {
    case 0: x4_picture_settings_ui_draw_cached(settings, legacy); break;
    case 1: x4_auth_draw(&auth, false, false, legacy); break;
    case 2: case 3: {
        const X4Screen screen={.page=scene==2?2:7};
        const X4Controller controller={0};
        x4_screen_draw(&screen, &controller, legacy);
        break;
    }
    default: x4_cloud_time_confirm_draw(legacy); break;
    }
    x4_ui2_shell_fit_legacy(pixels, legacy);
#endif
    return SIZE_MAX;
}

static void secondary49_bench(uint32_t *pixels, uint32_t *legacy)
{
    static const char *const names[]={"ajustes","cuenta","acerca","apoyar","modal"};
    static const enum X4UiScreen metrics[]={X4_UI_OPTIONS_PICTURE,X4_UI_ACCOUNT,
        X4_UI_ABOUT,X4_UI_SUPPORT,
#if X4_BENCH_49
        X4_UI_MODAL
#else
        X4_UI_OTHER
#endif
    };
    for (unsigned scene=0; scene<sizeof(names)/sizeof(*names); ++scene) {
        X4PictureSettingsUi settings={0};
        x4_picture_settings_ui_handle(&settings,X4_PIC_SETTINGS_OPEN,true);
        x4_rect(pixels,0,0,WIDTH,HEIGHT,X4_UI2_BACKGROUND);
        x4_rect(legacy,0,0,WIDTH,HEIGHT,X4_UI2_BACKGROUND);
        DrawSamples samples={0};
        uint64_t start=clock_us();
        size_t changed=secondary49_draw(scene,true,false,pixels,legacy,&settings);
        samples.first_us=clock_us()-start;
        samples.first_full=changed==SIZE_MAX;
        x4_ui_draw_metrics_reset();
        for (unsigned frame=0; frame<FRAMES; ++frame) {
            bool next=(frame&1u)==0;
            if (!scene)
                x4_picture_settings_ui_handle(&settings,next?X4_PIC_SETTINGS_DOWN:X4_PIC_SETTINGS_UP,true);
            start=clock_us();
            changed=secondary49_draw(scene,false,next,pixels,legacy,&settings);
            measure(&samples,metrics[scene],frame,start,clock_us(),changed);
        }
        report(&samples,metrics[scene],names[scene]);
    }
}

int main(int argc, char **argv)
{
    assert(argc==4);
    const bool en=!strcmp(argv[3],"en");
    x4_lang_set(en?X4_LANG_EN:X4_LANG_ES,en?"en-US":"es-MX",en?"US":"MX");
    assert(!x4_font_init("assets/fonts/inter.x4font") && !x4_font_is_fallback());
    load_fixtures(argv[1]);
    uint32_t *pixels=guarded47(), *legacy=guarded47();
    x4_catalog_gui_home_prewarm(pixels);
    publish_background(argv[1],forza);
    home49_bench(pixels);
    browse(X4_CAT_TAB_DISCOVER,false,pixels);
    browse(X4_CAT_TAB_CATALOG,false,pixels);
    browse(X4_CAT_TAB_RECENTS,false,pixels);
    browse(X4_CAT_TAB_CATALOG,true,pixels);
    game(argv[2],pixels);
    secondary49_bench(pixels,legacy);
    check_canvas(pixels);
    check_canvas(legacy);
    free(pixels-GUARDED_WORDS);
    free(legacy-GUARDED_WORDS);
    x4_catalog_browser_ui2_library_close(&browser);
    x4_font_close();
    return 0;
}

