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
    setvbuf(stdout, NULL, _IONBF, 0);
    int rc = x4_display_open(&display);
    if (rc < 0) {
        printf("XCloud4: no se pudo iniciar VideoOut: 0x%08x\n", (unsigned)rc);
        sceKernelUsleep(3000000);
        return 1;
    }
    x4_controller_init(&controller);
    printf("XCloud4 0.1.0: inicio de interfaz y control\n");
    for (unsigned frame = 0; !screen.exit_requested; ++frame) {
        x4_controller_read(&controller, frame);
        x4_screen_update(&screen, controller.pressed, controller.data.buttons);
        x4_screen_draw(&screen, &controller, x4_display_pixels(&display));
        rc = x4_display_present(&display);
        if (rc < 0) {
            printf("XCloud4: error de presentacion: 0x%08x\n", (unsigned)rc);
            break;
        }
    }
    x4_controller_close(&controller);
    x4_display_close(&display);
    return rc < 0 ? 1 : 0;
}
