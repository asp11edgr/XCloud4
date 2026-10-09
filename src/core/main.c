/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <stdatomic.h>
#include <orbis/libkernel.h>
#include "../video/display.h"
#include "../input/controller.h"
#include "../ui/screen.h"
#include "../auth/auth_profile.h"
#include "lifecycle.h"

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
    int media_active = 0;
    int closing = 0;
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
    printf("XCloud4 0.6.2: interfaz, multimedia, cuenta, catalogo y perfil temporal de acceso\n");
    printf("XCloud4: %s\n", X4_AUTH_PROFILE_NOTE);
    for (unsigned frame = 0;; ++frame) {
        x4_controller_read(&controller, frame);
        int previous_page = screen.page;
        if (!closing) x4_screen_update(&screen, controller.pressed, controller.data.buttons);
        if ((previous_page == 4 || previous_page == 5 || previous_page == 6) && screen.page != previous_page)
            x4_auth_cancel(auth);
        if (screen.exit_requested) {
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
            if (controller.pressed & ORBIS_PAD_BUTTON_CIRCLE) {
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
                    rc = x4_auth_start_session(auth, catalog_selected);
                    x4_auth_session_snapshot(auth, &session);
                    if (rc == 0 || session.state == X4_SESSION_ERROR) {
                        session_back = 0;
                        screen.page = 6;
                    }
                    if (rc == X4_AUTH_E_SIGNED_OUT) screen.page = 4;
                    if (rc) printf("XCloud4: solicitar sesion fallo 0x%08x\n", (unsigned)rc);
                }
            } else catalog_selected = 0;
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
        x4_screen_draw(&screen, &controller, x4_display_pixels(&display));
        if (screen.page == 3) x4_media_draw(&video, &audio, x4_display_pixels(&display));
        if (screen.page == 6)
            x4_session_draw(&session, x4_auth_busy(auth), closing, x4_display_pixels(&display));
        else if (screen.page == 4 || closing)
            x4_auth_draw(&account, x4_auth_busy(auth), closing, x4_display_pixels(&display));
        else if (screen.page == 5)
            x4_catalog_draw(&catalog, catalog_selected, x4_auth_busy(auth), x4_display_pixels(&display));
        x4_exit_error_draw(screen.exit_error, x4_display_pixels(&display));
        rc = x4_display_present(&display);
        if (rc < 0) {
            printf("XCloud4: error de presentacion: 0x%08x\n", (unsigned)rc);
            break;
        }
    }
    /* A display failure can break the loop early: cancel then join before
     * cleaning the UI. Ordinary OPTIONS keeps rendering while it cancels. */
    x4_auth_cancel(auth);
    while (x4_auth_busy(auth)) sceKernelUsleep(100000);
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
    closing = 0;
    session_back = 0;
    if (!auth) auth = x4_auth_create();
    sceKernelUsleep(250000);
    goto reopen_interface;
}
