/* SPDX-License-Identifier: GPL-3.0-only */
/* Include in the host capture harness for both T49 and T50. */
#include "screen.h"

static void capture50_game_screens(uint32_t *p,
    void (*begin_scene)(const char *, uint32_t *, bool),
    void (*end_scene)(const char *, uint32_t *))
{
    const char *name = "menu-partida";
    begin_scene(name, p, true);
    /* Synthetic video frame with visible landmarks outside the opaque menu. */
    for (int y = 0; y < HEIGHT; ++y)
        for (int x = 0; x < WIDTH; ++x)
            p[(size_t)y * WIDTH + x] = X4_RGB(12 + y / 40, 25 + x / 65, 40 + y / 16);
    x4_apple_fill_round_rect(p, 1080, 250, 450, 530, 30, X4_RGB(25, 65, 72));
    x4_apple_fill_round_rect(p, 1230, 370, 120, 340, 24, X4_RGB(95, 140, 122));
    x4_rect(p, 40, 965, 1840, 3, X4_RGB(55, 98, 110));

    X4TouchMenu menu;
    x4_touch_menu_init(&menu, X4_PUBLIC != 0);
    assert(!x4_touch_menu_feed_touch(&menu, true, 2, 1000000));
    assert(x4_touch_menu_feed_touch(&menu, true, 2, 2000000));
    menu.selected_index = 3;
    X4TouchMenu activation = menu;
    assert(x4_touch_menu_update(&activation, ORBIS_PAD_BUTTON_CROSS, ORBIS_PAD_BUTTON_CROSS,
                               2100000) == X4_TOUCH_MENU_ACTION_RELOAD_STREAM);
    x4_touch_menu_cache_reset();
    assert(x4_touch_menu_prepare(&menu, 0));
    x4_touch_menu_draw(&menu, 0, p);
    end_scene(name, p);

    static const struct {
        const char *name;
        enum X4SessionState state;
        const char *stage;
        int busy, closing, error, http;
    } scenes[] = {
        {"partida-conectando", X4_SESSION_CONNECTING, "negociando ruta ICE con Xbox", 1, 0, 0, 200},
        {"partida-error", X4_SESSION_ERROR, "fallo la conexion WebRTC", 0, 0, X4_AUTH_E_SESSION, 0},
        {"partida-cierre", X4_SESSION_STOPPING, "cerrando sesion en Xbox", 1, 1, 0, 0}
    };
    x4_apple_set_animation_phase(0);
    for (size_t i = 0; i < sizeof(scenes) / sizeof(scenes[0]); ++i) {
        X4SessionSnapshot session = {
            .state = scenes[i].state,
            .error = scenes[i].error,
            .http_status = scenes[i].http,
            .elapsed_seconds = 7
        };
        snprintf(session.title_name, sizeof(session.title_name), "%s", "Forza Horizon 5");
        snprintf(session.stage, sizeof(session.stage), "%s", scenes[i].stage);
        begin_scene(scenes[i].name, p, true);
        x4_session_draw(&session, scenes[i].busy, scenes[i].closing, p);
        end_scene(scenes[i].name, p);
    }
}
