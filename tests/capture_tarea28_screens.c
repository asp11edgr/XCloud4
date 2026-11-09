/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "../src/ui/stream_settings.h"
#include "../src/ui/lang.h"
#include "../src/ui/font.h"
#include "../src/ui/apple_ui.h"
#include "../src/input/script_engine.h"

enum { W = 1920, H = 1080, PIXELS = W * H };

static void save_ppm(const char *path, const uint32_t *pixels)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    fprintf(f, "P6\n1920 1080\n255\n");
    for (size_t i = 0; i < PIXELS; ++i) {
        uint32_t c = pixels[i];
        unsigned char rgb[3] = {
            (unsigned char)((c >> 16) & 0xff),
            (unsigned char)((c >> 8) & 0xff),
            (unsigned char)(c & 0xff)
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *out_dir = argc > 1 ? argv[1] : ".";
    uint32_t *p = malloc(PIXELS * sizeof(uint32_t));
    assert(p != NULL);

    x4_font_init("assets/fonts");
    x4_lang_init();
    x4_script_engine_init();

    struct {
        enum X4Lang lang;
        const char *locale;
        const char *market;
        const char *suffix;
    } langs[] = {
        {X4_LANG_ES, "es-ES", "es", "es"},
        {X4_LANG_EN, "en-US", "en", "en"}
    };

    char path[512];

    for (int l = 0; l < 2; ++l) {
        x4_lang_set(langs[l].lang, langs[l].locale, langs[l].market);
        const char *sfx = langs[l].suffix;

        /* 1. Panel R3 con foco en Fila 17 mostrando "Ninguno" / "None" */
        X4StreamSettingsUi ui = {0};
        ui.open = true;
        ui.row = 17;
        /* Simulate Ninguno (default) */
        x4_stream_settings_ui_handle(&ui, X4_SETTINGS_OPEN, true);
        ui.row = 17;

        x4_apple_bg_draw(p);
        x4_stream_settings_ui_draw(&ui, p);
        snprintf(path, sizeof(path), "%s/r3_script_none_%s.ppm", out_dir, sfx);
        save_ppm(path, p);

        /* 2. Panel R3 mostrando script seleccionado ("reasignar.lua") */
        /* Navigate right to select first script with warning accepted */
        x4_script_engine_save_persistence(true, "assets/scripts/reasignar.lua");
        x4_stream_settings_ui_handle(&ui, X4_SETTINGS_RIGHT, true);
        ui.row = 17;

        x4_apple_bg_draw(p);
        x4_stream_settings_ui_draw(&ui, p);
        snprintf(path, sizeof(path), "%s/r3_script_selected_%s.ppm", out_dir, sfx);
        save_ppm(path, p);

        /* 3. Diálogo modal de advertencia */
        /* Reset persistence to unaccepted and trigger selection to open modal */
        x4_script_engine_save_persistence(false, "");
        /* Reload persistence inside settings by re-opening */
        ui.open = false;
        x4_stream_settings_ui_handle(&ui, X4_SETTINGS_CANCEL, true);
        ui.open = true;
        ui.row = 17;
        /* Toggle right without accepted warning triggers modal */
        x4_stream_settings_ui_handle(&ui, X4_SETTINGS_RIGHT, true);

        x4_apple_bg_draw(p);
        x4_stream_settings_ui_draw(&ui, p);
        snprintf(path, sizeof(path), "%s/r3_script_warning_%s.ppm", out_dir, sfx);
        save_ppm(path, p);

        /* Close modal for next iteration */
        x4_stream_settings_ui_handle(&ui, X4_SETTINGS_CANCEL, true);
        ui.open = false;
    }

    free(p);
    printf("Captures completed successfully.\n");
    return 0;
}
