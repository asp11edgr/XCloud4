/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/picture_settings.h"
#include "../src/ui/server_settings.h"
#include <assert.h>
#include <stdio.h>

static void step(X4PictureSettingsUi *settings, unsigned action)
{
    assert(x4_picture_settings_ui_handle(settings, action, true));
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    x4_picture_settings_set_path(argv[1]);
    x4_server_settings_set_path(argv[2]);
    assert(x4_access_country_set_path(argv[3]));
    x4_picture_settings_init();
    x4_server_settings_init();

    X4PictureSettingsUi settings = {0};
    const X4PictureQualityOption original = x4_picture_settings_get();
    step(&settings, X4_PIC_SETTINGS_OPEN);
    assert(settings.open && !x4_picture_settings_ui_dirty(&settings));
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(settings.dropdown_open);
    step(&settings, X4_PIC_SETTINGS_DOWN);
    assert(x4_picture_settings_get() == original);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(!settings.dropdown_open && x4_picture_settings_ui_dirty(&settings));
    assert(x4_picture_settings_get() == original);
    step(&settings, X4_PIC_SETTINGS_CANCEL);
    assert(settings.open && settings.leave_pending && x4_picture_settings_ui_has_modal(&settings));
    step(&settings, X4_PIC_SETTINGS_CANCEL);
    assert(settings.open && !settings.leave_pending && !settings.dropdown_open);
    step(&settings, X4_PIC_SETTINGS_CANCEL);
    step(&settings, X4_PIC_SETTINGS_RIGHT);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(!settings.open && x4_picture_settings_get() == original);

    step(&settings, X4_PIC_SETTINGS_OPEN);
    step(&settings, X4_PIC_SETTINGS_RIGHT);
    for (unsigned i = 0; i < 2 && !settings.save_focused; ++i) step(&settings, X4_PIC_SETTINGS_DOWN);
    assert(settings.save_focused);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(settings.open && !x4_picture_settings_ui_dirty(&settings));
    assert(x4_picture_settings_get() != original);
    step(&settings, X4_PIC_SETTINGS_CANCEL);
    assert(!settings.open);

    const bool original_pad = x4_picture_settings_multiplayer();
    x4_picture_settings_ui_open_section(&settings, X4_PIC_SECTION_PADS);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(settings.draft_multiplayer != original_pad && x4_picture_settings_multiplayer() == original_pad);
    step(&settings, X4_PIC_SETTINGS_NEXT_TAB);
    step(&settings, X4_PIC_SETTINGS_DOWN);
    assert(settings.tab == X4_PIC_SECTION_ACCOUNT);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(settings.leave_pending && settings.route == X4_PIC_ROUTE_NONE);
    step(&settings, X4_PIC_SETTINGS_RIGHT);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(!settings.open && settings.route == X4_PIC_ROUTE_ACCOUNT &&
        x4_picture_settings_multiplayer() == original_pad);

    x4_picture_settings_ui_open_section(&settings, X4_PIC_SECTION_ACCESS);
    step(&settings, X4_PIC_SETTINGS_RIGHT);
    assert(settings.access_ui.selected != x4_access_country_get());
    step(&settings, X4_PIC_SETTINGS_DOWN);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(settings.access_ui.confirming && !settings.access_changed);
    step(&settings, X4_PIC_SETTINGS_CANCEL);
    assert(!settings.access_ui.confirming && !x4_access_country_warning_accepted());
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(settings.access_changed && x4_access_country_warning_accepted());

    x4_server_settings_set_awaiting_first_choice(true);
    x4_picture_settings_open_server_first_time(&settings);
    assert(settings.first_time_mode && x4_picture_settings_ui_has_modal(&settings));
    step(&settings, X4_PIC_SETTINGS_CANCEL);
    assert(!settings.open && x4_server_settings_first_choice_completed());

    puts("PASS T49 Ajustes: borrador, lista, Guardar, Descartar, rutas, aviso de acceso y primera elección");
    return 0;
}
