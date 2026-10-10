/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <orbis/libkernel.h>
#include "../video/display.h"
#include "../input/controller.h"
#include "../ui/screen.h"
#include "../auth/auth_profile.h"
#include "../media/live_media.h"
#include "lifecycle.h"

static void trace_inactive(X4LiveMedia *live)
{
    if (live) x4_trace_set_active(x4_live_media_trace(live), false);
}
static bool trace_session_active(enum X4SessionState state)
{
    return state != X4_SESSION_STOPPING && state != X4_SESSION_CLOSED &&
        state != X4_SESSION_CANCELLED && state != X4_SESSION_ERROR;
}

int main(void)
{
    X4Display display;
    X4Controller controller;
    X4Screen screen = {0};
    X4DemoVideo video = {0};
    X4DemoAudio audio = {.handle = -1};
    X4Auth *auth = x4_auth_create();
    X4AuthSnapshot account = {0};
    static X4CatalogSnapshot catalog;
    X4SessionSnapshot session = {0};
    unsigned catalog_selected = 0;
    int session_back = 0;
    int game_controls = 0;
    int media_active = 0;
    int closing = 0;
    X4LiveMedia *live = NULL;
    X4LiveMediaSnapshot live_status = {0};
    int live_error = 0, live_muted = 0;
    int live_retained = 0;
    bool live_presented = false;
    int overlay_key[10] = {0};
    uint64_t overlay_at = 0;
    uint64_t input_previous = 0, input_report_at = 0;
    uint64_t input_intervals = 0, input_interval_us = 0, input_interval_max_us = 0;
    uint64_t idle_present_skips = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
reopen_interface:;
    int rc = x4_display_open(&display);
    if (rc < 0) {
        printf("XCloud4: no se pudo iniciar VideoOut: 0x%08x\n", (unsigned)rc);
        sceKernelUsleep(3000000);
        /* Never enter the known failing libc _exit path on this firmware. */
        if (x4_exit_prepare() >= 0) x4_exit_request();
        for (;;) sceKernelUsleep(100000);
    }
    x4_controller_init(&controller);
    printf("XCloud4 0.7.26: diagnostico de pausas con dos lectores NV12\n");
    printf("XCloud4: %s\n", X4_AUTH_PROFILE_NOTE);
    for (unsigned frame = 0;; ++frame) {
        uint64_t input_at = sceKernelGetProcessTime();
        x4_controller_read(&controller, frame);
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
        bool game_input = screen.page == 6 && game_controls && x4_auth_busy(auth) &&
            !closing && !session_back && !live_error;
        bool chord = (controller.data.buttons & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1)) ==
            (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1);
        bool local_action = game_input && chord && (controller.data.buttons &
            (ORBIS_PAD_BUTTON_OPTIONS | ORBIS_PAD_BUTTON_CIRCLE | ORBIS_PAD_BUTTON_SQUARE));
        bool guide_action = game_input && chord && (controller.data.buttons & ORBIS_PAD_BUTTON_TOUCH_PAD);
        X4GamepadFrame gamepad = {0};
        if (game_input) x4_controller_gamepad(&controller, &gamepad);
        if (local_action || guide_action) {
            memset(&gamepad, 0, sizeof(gamepad));
            gamepad.connected = controller.data.connected;
            if (guide_action && !local_action) gamepad.buttons = X4_GAMEPAD_NEXUS;
        }
        x4_auth_set_gamepad(auth, &gamepad);
        if (!closing) {
            uint32_t ui_pressed = controller.pressed;
            if (game_input) ui_pressed &= ~ORBIS_PAD_BUTTON_OPTIONS;
            x4_screen_update(&screen, ui_pressed, controller.data.buttons);
            if (game_input && chord && (controller.pressed & ORBIS_PAD_BUTTON_OPTIONS))
                screen.exit_requested = 1;
        }
        if ((previous_page == 4 || previous_page == 5 || previous_page == 6) && screen.page != previous_page) {
            trace_inactive(live);
            x4_auth_cancel(auth);
        }
        if (screen.exit_requested) {
            trace_inactive(live);
            if (x4_auth_busy(auth)) {
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
            if (controller.pressed & ORBIS_PAD_BUTTON_CROSS) x4_auth_start(auth, X4_AUTH_SIGN_IN);
            else if (controller.pressed & ORBIS_PAD_BUTTON_SQUARE) x4_auth_start(auth, X4_AUTH_CHECK_CONNECTION);
            else if (controller.pressed & ORBIS_PAD_BUTTON_TRIANGLE) x4_auth_forget(auth);
            else if ((controller.pressed & ORBIS_PAD_BUTTON_R1) &&
                     account.state == X4_AUTH_AUTHORIZED && !x4_auth_busy(auth)) {
                screen.page = 5;
                if (catalog.state != X4_CATALOG_READY) {
                    catalog_selected = 0;
                    x4_auth_start(auth, X4_AUTH_XBOX_CATALOG);
                }
            }
        }
        x4_auth_snapshot(auth, &account);
        x4_auth_catalog_snapshot(auth, &catalog);
        x4_auth_session_snapshot(auth, &session);
        if (!closing && screen.page == 6 && previous_page == 6) {
            if ((controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) && (!game_input || chord)) {
                trace_inactive(live);
                x4_auth_cancel(auth);
                session_back = 1;
            }
            /* Keep the session page visible until remote cleanup finishes. */
            if (session_back && !x4_auth_busy(auth)) {
                session_back = 0;
                screen.page = 5;
            }
        }
        if (!closing && screen.page == 5 && previous_page == 5) {
            if ((controller.pressed & ORBIS_PAD_BUTTON_SQUARE) && !x4_auth_busy(auth)) {
                catalog_selected = 0;
                x4_auth_start(auth, X4_AUTH_XBOX_CATALOG);
            }
            if (catalog.count) {
                if (controller.pressed & ORBIS_PAD_BUTTON_UP)
                    catalog_selected = catalog_selected ? catalog_selected - 1 : catalog.count - 1;
                if (controller.pressed & ORBIS_PAD_BUTTON_DOWN)
                    catalog_selected = (catalog_selected + 1) % catalog.count;
                if (controller.pressed & ORBIS_PAD_BUTTON_L1)
                    catalog_selected = catalog_selected >= 8 ? catalog_selected - 8 : 0;
                if (controller.pressed & ORBIS_PAD_BUTTON_R1)
                    catalog_selected = catalog_selected + 8 < catalog.count ? catalog_selected + 8 : catalog.count - 1;
                if (catalog_selected >= catalog.count) catalog_selected = catalog.count - 1;
                if ((controller.pressed & ORBIS_PAD_BUTTON_CROSS) &&
                    catalog.state == X4_CATALOG_READY && !x4_auth_busy(auth)) {
                    live_presented = false;
                    live_error = 0;
                    if (live) {
                        trace_inactive(live);
                        live_error = x4_auth_set_media_callback(auth, NULL, NULL);
                        if (!live_error) live_error = x4_live_media_close(live);
                        if (!live_error) live = NULL;
                    }
                    if (!live_error) {
                        live_retained = 0;
                        live = x4_live_media_create(&live_error);
                        if (live) {
                            live_error = x4_live_media_set_payload_type(live, X4_LIVE_KIND_VIDEO, 102);
                            if (!live_error) live_error = x4_live_media_set_payload_type(live, X4_LIVE_KIND_AUDIO, 111);
                            if (!live_error) live_error = x4_live_media_start(live);
                        }
                    }
                    memset(&live_status, 0, sizeof(live_status));
                    live_muted = 0;
                    rc = live_error;
                    if (!rc) rc = x4_auth_set_media_callback(auth, x4_live_media_receive, live);
                    if (!rc) { game_controls = 0; rc = x4_auth_start_session(auth, catalog_selected); }
                    if (rc && !live_error) live_error = rc;
                    x4_auth_session_snapshot(auth, &session);
                    if (rc == 0 || session.state == X4_SESSION_ERROR || live_error) {
                        session_back = 0;
                        screen.page = 6;
                    }
                    if (rc == X4_AUTH_E_SIGNED_OUT) screen.page = 4;
                    if (rc) printf("XCloud4: solicitar sesion fallo 0x%08x\n", (unsigned)rc);
                }
            } else catalog_selected = 0;
        }
        if (live && !live_retained) {
            x4_live_media_tick(live);
            x4_live_media_snapshot(live, &live_status);
            if (x4_auth_busy(auth)) {
                if (x4_live_media_take_keyframe_request(live)) x4_auth_request_keyframe(auth);
                if (live_status.video_error) { trace_inactive(live); x4_auth_cancel(auth); }
                if (screen.page == 6 && (controller.pressed & ORBIS_PAD_BUTTON_SQUARE) && (!game_input || chord)) {
                    live_muted = !live_muted;
                    x4_live_media_set_muted(live, live_muted != 0);
                }
            } else {
                /* The worker closes RTC before publishing completion. */
                trace_inactive(live);
                int stop_rc = x4_auth_set_media_callback(auth, NULL, NULL);
                if (!stop_rc) stop_rc = x4_live_media_close(live);
                if (!stop_rc) live = NULL;
                else { live_error = stop_rc; live_retained = 1; printf("XCloud4: medios retienen recursos, cierre 0x%08x\n", (unsigned)stop_rc); }
            }
        }
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
        X4Trace *live_trace = live ? x4_live_media_trace(live) : NULL;
        bool trace_active = live_visible && !live_retained && trace_session_active(session.state);
        x4_trace_set_active(live_trace, trace_active);
        /* Runs before the idle continue too: a repeated/status-only refresh
         * must not conceal a gap in actual NEW live completions. */
        x4_trace_poll(live_trace);
        int next_overlay_key[10] = {screen.page, live_muted, live_status.audio_error,
            live_status.audio_playing, session.input_error, session.input_ready,
            session.rtc_connected, session.state, screen.exit_error, controller.data.connected};
        uint64_t draw_at = sceKernelGetProcessTime();
        bool overlay_changed = memcmp(overlay_key, next_overlay_key, sizeof(overlay_key)) != 0;
        if (live_visible && live_presented && !overlay_changed && draw_at - overlay_at < 500000 &&
            !x4_live_media_has_new_picture(live)) {
            ++idle_present_skips;
            sceKernelUsleep(2000);
            continue;
        }
        bool drew_live = live_visible && x4_live_media_draw(live, x4_display_pixels(&display));
        uint64_t drawn_generation = 0, drawn_output = 0;
        bool drawn_fresh = false;
        if (drew_live && !live_retained)
            x4_live_media_drawn(live, &drawn_generation, &drawn_output, &drawn_fresh);
        /* Media draw records the independent native-output identity. Display
         * uses the actual drawn publication tag, never a later snapshot. */
        (void)drawn_output;
        if (!drew_live) x4_screen_draw(&screen, &controller, x4_display_pixels(&display));
        if (screen.page == 3) x4_media_draw(&video, &audio, x4_display_pixels(&display));
        if (screen.page == 6) {
            if (drew_live)
                x4_live_overlay(&live_status, &session, live_muted, x4_display_pixels(&display));
            else {
                X4SessionSnapshot shown = session;
                if (live_error && !x4_auth_busy(auth)) {
                    shown.state = X4_SESSION_ERROR;
                    shown.error = live_error;
                    shown.http_status = 0;
                    shown.ready_seen = shown.connection_authorized = 0;
                    shown.rtc_connected = 0;
                    snprintf(shown.stage, sizeof(shown.stage), "no se pudieron preparar o cerrar los medios nativos");
                    if (catalog_selected < catalog.count)
                        snprintf(shown.title_name, sizeof(shown.title_name), "%s", catalog.titles[catalog_selected].name);
                }
                x4_session_draw(&shown, x4_auth_busy(auth), closing, x4_display_pixels(&display));
                x4_live_status_draw(&live_status, live_error, x4_display_pixels(&display));
            }
        }
        else if (screen.page == 4 || closing)
            x4_auth_draw(&account, x4_auth_busy(auth), closing, x4_display_pixels(&display));
        else if (screen.page == 5)
            x4_catalog_draw(&catalog, catalog_selected, x4_auth_busy(auth), x4_display_pixels(&display));
        x4_exit_error_draw(screen.exit_error, x4_display_pixels(&display));
        uint64_t present_begin = sceKernelGetProcessTime();
        rc = drew_live && trace_active ?
            x4_display_present_trace(&display, live_trace, drawn_generation, drawn_fresh) :
            x4_display_present(&display);
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
    x4_auth_cancel(auth);
    while (x4_auth_busy(auth)) sceKernelUsleep(100000);
    if (live) {
        int live_rc = x4_auth_set_media_callback(auth, NULL, NULL);
        if (!live_rc) live_rc = x4_live_media_close(live);
        if (!live_rc) live = NULL;
        else live_error = live_rc;
    }
    printf("XCloud4: cerrar acceso Microsoft\n");
    int auth_rc = x4_auth_close(auth);
    if (auth_rc >= 0) auth = NULL;
    printf("XCloud4: cerrar audio\n");
    x4_audio_stop(&audio);
    printf("XCloud4: cerrar decoder\n");
    x4_video_stop(&video);
    printf("XCloud4: cerrar control\n");
    x4_controller_close(&controller);
    printf("XCloud4: cerrar pantalla\n");
    x4_display_close(&display);
    printf("XCloud4: recursos cerrados\n");
    int exit_rc = x4_exit_prepare();
    if (exit_rc >= 0) exit_rc = x4_exit_request();
    if (exit_rc >= 0) {
        /* LoadExec is asynchronous; permit recovery if the shell never exits. */
        for (unsigned wait = 0; wait < 100; ++wait) sceKernelUsleep(100000);
        exit_rc = -10;
        printf("XCloud4: tiempo de salida agotado\n");
    }
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
}
