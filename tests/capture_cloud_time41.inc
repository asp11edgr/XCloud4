/* SPDX-License-Identifier: GPL-3.0-only */
#include "cloud_time_ui.h"

static void assert_cloud_damage(const uint32_t *pixels, const uint32_t *baseline)
{
    X4UiRect damage;
    assert(x4_catalog_gui_home_damage(NULL, NULL) == 0);
    assert(x4_cloud_time_home_damage(&damage));
    assert(damage.x >= 0 && damage.y >= 0 && damage.x + damage.w <= WIDTH &&
        damage.y + damage.h <= HEIGHT);
    for (int y = 0; y < HEIGHT; ++y) {
        size_t offset = (size_t)y * WIDTH;
        if (y < damage.y || y >= damage.y + damage.h)
            assert(!memcmp(pixels + offset, baseline + offset, WIDTH * sizeof(*pixels)));
        else {
            assert(!memcmp(pixels + offset, baseline + offset, (size_t)damage.x * sizeof(*pixels)));
            size_t right = (size_t)(damage.x + damage.w);
            assert(!memcmp(pixels + offset + right, baseline + offset + right,
                (WIDTH - right) * sizeof(*pixels)));
        }
    }
}

static void capture_cloud_time(uint32_t *pixels)
{
    uint32_t *baseline = malloc(pixel_count * sizeof(*pixels));
    uint32_t *incremental = malloc(pixel_count * sizeof(*pixels));
    assert(baseline && incremental);
    memcpy(baseline, pixels, pixel_count * sizeof(*pixels));
    static const struct { const char *name; X4CloudTime value; } cases[] = {
        {"41-descubrir-4h10", {15000, 54000, false, X4_CLOUD_TIME_ACCOUNT}},
        {"41-descubrir-1h30", {5400, 54000, false, X4_CLOUD_TIME_ACCOUNT}},
        {"41-descubrir-0", {0, 54000, false, X4_CLOUD_TIME_ACCOUNT}},
        {"41-descubrir-ilimitado", {0, 0, true, X4_CLOUD_TIME_ACCOUNT}},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        x4_cloud_time_home_set(cases[i].value);
        draw_tab(pixels);
        assert_cloud_damage(pixels, baseline);
        memcpy(incremental, pixels, pixel_count * sizeof(*pixels));
        x4_catalog_gui_home_invalidate();
        draw_tab(pixels);
        assert(!memcmp(pixels, incremental, pixel_count * sizeof(*pixels)));
        assert(memcmp(pixels, baseline, pixel_count * sizeof(*pixels)));
        capture(cases[i].name, pixels);
    }
    x4_cloud_time_home_set((X4CloudTime){0});
    draw_tab(pixels);
    assert_cloud_damage(pixels, baseline);
    assert(!memcmp(pixels, baseline, pixel_count * sizeof(*pixels)));
    x4_apple_bg_draw(pixels);
    x4_cloud_time_game_draw(pixels, (X4CloudTime){300, 54000, false, X4_CLOUD_TIME_ACCOUNT},
        X4_CLOUD_TIME_ALERT_5, false);
    capture("41-partida-aviso-5min", pixels);
    memcpy(pixels, baseline, pixel_count * sizeof(*pixels));
    x4_cloud_time_confirm_draw(pixels);
    capture("41-lanzar-saldo-0", pixels);
    fprintf(stderr, "CLOUD_TIME_41_DAMAGE_PASS\n");
    free(incremental);
    free(baseline);
}
