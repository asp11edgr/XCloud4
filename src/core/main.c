/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include <orbis/libkernel.h>
#include "../video/display.h"
#include "../input/controller.h"
#include "../ui/screen.h"

int main(void)
{
    X4Display display;
    X4Controller controller;
    X4Screen screen = {0};
    X4DemoVideo video = {0};
    X4DemoAudio audio = {.handle = -1};
    int media_active = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    int rc = x4_display_open(&display);
    if (rc < 0) {
        printf("XCloud4: no se pudo iniciar VideoOut: 0x%08x\n", (unsigned)rc);
        sceKernelUsleep(3000000);
        return 1;
    }
    x4_controller_init(&controller);
    printf("XCloud4 0.2.1: inicio de interfaz, control y muestra multimedia\n");
    for (unsigned frame = 0; !screen.exit_requested; ++frame) {
        x4_controller_read(&controller, frame);
        x4_screen_update(&screen, controller.pressed, controller.data.buttons);
        if (screen.exit_requested) break;
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
        rc = x4_display_present(&display);
        if (rc < 0) {
            printf("XCloud4: error de presentacion: 0x%08x\n", (unsigned)rc);
            break;
        }
    }
    x4_audio_stop(&audio);
    x4_video_stop(&video);
    x4_controller_close(&controller);
    x4_display_close(&display);
    return rc < 0 ? 1 : 0;
}
