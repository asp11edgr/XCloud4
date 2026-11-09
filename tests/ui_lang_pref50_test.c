/* SPDX-License-Identifier: GPL-3.0-only */
#include "../src/ui/lang.h"
#include "../src/ui/picture_settings.h"
#include "../src/ui/ui_lang_pref.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

static atomic_bool delay_next;
static atomic_uint_fast64_t rename_elapsed;
static uint64_t monotonic_us(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

int __real_rename(const char *from, const char *to);
int __wrap_rename(const char *from, const char *to)
{
    if (!atomic_exchange(&delay_next, false)) return __real_rename(from, to);
    const uint64_t start = monotonic_us();
    struct timespec remaining = {1, 0};
    while (nanosleep(&remaining, &remaining)) {}
    const int rc = __real_rename(from, to);
    atomic_store(&rename_elapsed, monotonic_us() - start);
    return rc;
}

static void next_frame(void)
{
    const struct timespec interval = {0, 16000000};
    (void)nanosleep(&interval, NULL);
}

static bool select_and_wait(X4UiLangPreference preference)
{
    const uint64_t start = monotonic_us();
    X4UiLangSave status = x4_ui_lang_pref_request(preference);
    while (status == X4_UI_LANG_SAVE_PENDING) {
        assert(monotonic_us() - start < 5000000u);
        next_frame();
        status = x4_ui_lang_pref_poll();
    }
    return status == X4_UI_LANG_SAVE_DONE;
}

static void step(X4PictureSettingsUi *settings, unsigned action)
{
    assert(x4_picture_settings_ui_handle(settings, action, true));
}

static void prepare_german(X4PictureSettingsUi *settings)
{
    x4_picture_settings_ui_open_section(settings, X4_PIC_SECTION_INTERFACE);
    step(settings, X4_PIC_SETTINGS_CONFIRM);
    assert(settings->dropdown_open && settings->dropdown_choice == X4_UI_LANG_AUTO);
    step(settings, X4_PIC_SETTINGS_UP);
    assert(settings->dropdown_choice == X4_UI_LANG_DE);
    step(settings, X4_PIC_SETTINGS_CONFIRM);
    assert(!settings->dropdown_open && x4_picture_settings_ui_dirty(settings));
    assert(x4_ui_lang_pref_get() == X4_UI_LANG_AUTO && x4_lang_current() == X4_LANG_FR);
}

static void check_market(void)
{
    assert(!strcmp(x4_lang_locale(), "fr-CA") && !strcmp(x4_lang_market(), "CA"));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *path = argv[1];
    FILE *file = fopen(path, "rb");
    assert(!file); /* Caller supplies a fresh test path. */
    x4_lang_apply_system_id(22); /* Canadian French */
    x4_ui_lang_pref_init(path);
    assert(x4_ui_lang_pref_get() == X4_UI_LANG_AUTO);
    assert(x4_lang_system_ui() == X4_LANG_FR && x4_lang_current() == X4_LANG_FR);
    check_market();

    X4PictureSettingsUi settings = {0};
    prepare_german(&settings);
    step(&settings, X4_PIC_SETTINGS_CANCEL);
    assert(settings.leave_pending);
    step(&settings, X4_PIC_SETTINGS_RIGHT);
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    assert(!settings.open && x4_ui_lang_pref_get() == X4_UI_LANG_AUTO);
    assert(!fopen(path, "rb"));

    prepare_german(&settings);
    const uint64_t revision = x4_lang_ui_revision();
    step(&settings, X4_PIC_SETTINGS_DOWN);
    assert(settings.save_focused);
    uint32_t *pixels = calloc(1920u * 1080u, sizeof(*pixels));
    assert(pixels);
    x4_picture_settings_ui_draw_page(&settings, pixels);
    atomic_store(&delay_next, true);
    const uint64_t start = monotonic_us();
    step(&settings, X4_PIC_SETTINGS_CONFIRM);
    x4_picture_settings_ui_draw_page(&settings, pixels);
    uint64_t max_frame_us = monotonic_us() - start;
    assert(settings.language_saving && x4_lang_current() == X4_LANG_FR);
    unsigned pending_frames = 0;
    while (settings.language_saving) {
        assert(monotonic_us() - start < 5000000u);
        next_frame();
        const uint64_t before = monotonic_us();
        step(&settings, 0);
        (void)x4_picture_settings_ui_draw_page_cached(&settings, pixels, NULL);
        const uint64_t frame_us = monotonic_us() - before;
        if (frame_us > max_frame_us) max_frame_us = frame_us;
        ++pending_frames;
    }
    free(pixels);
    printf("LANG50B_SAVE injected_rename_us=%llu total_us=%llu frames=%u max_frame_us=%llu\n",
        (unsigned long long)atomic_load(&rename_elapsed),
        (unsigned long long)(monotonic_us() - start), pending_frames,
        (unsigned long long)max_frame_us);
    assert(atomic_load(&rename_elapsed) >= 1000000u && pending_frames >= 2);
    assert(max_frame_us <= 100000u);
    assert(!x4_picture_settings_ui_dirty(&settings));
    assert(x4_ui_lang_pref_get() == X4_UI_LANG_DE && x4_lang_current() == X4_LANG_DE);
    assert(x4_lang_ui_revision() != revision);
    check_market();

    x4_lang_set_ui(X4_LANG_EN);
    x4_ui_lang_pref_init(path);
    assert(x4_ui_lang_pref_get() == X4_UI_LANG_DE && x4_lang_current() == X4_LANG_DE);

    static const enum X4Lang expected[] = {
        X4_LANG_FR, X4_LANG_ES, X4_LANG_EN, X4_LANG_PT_BR, X4_LANG_FR, X4_LANG_IT, X4_LANG_DE,
    };
    for (unsigned i = 0; i < X4_UI_LANG_COUNT; ++i) {
        assert(select_and_wait((X4UiLangPreference)i));
        assert(x4_lang_current() == expected[i]);
        x4_ui_lang_pref_init(path);
        assert(x4_ui_lang_pref_get() == (X4UiLangPreference)i);
        assert(x4_lang_current() == expected[i]);
        check_market();
    }

    assert(!select_and_wait((X4UiLangPreference)-1));
    assert(x4_ui_lang_pref_get() == X4_UI_LANG_DE);
    char impossible[512];
    const int n = snprintf(impossible, sizeof(impossible), "%s/child.bin", path);
    assert(n > 0 && (size_t)n < sizeof(impossible));
    x4_ui_lang_pref_init(impossible);
    assert(!select_and_wait(X4_UI_LANG_ES));
    assert(x4_ui_lang_pref_get() == X4_UI_LANG_AUTO && x4_lang_current() == X4_LANG_FR);
    x4_ui_lang_pref_init(path);
    assert(x4_ui_lang_pref_get() == X4_UI_LANG_DE);

    file = fopen(path, "r+b");
    assert(file && !fseek(file, 8, SEEK_SET));
    assert(fputc(X4_UI_LANG_IT, file) == X4_UI_LANG_IT); /* Keep the checksum for DE. */
    assert(!fclose(file));
    x4_ui_lang_pref_init(path);
    assert(x4_ui_lang_pref_get() == X4_UI_LANG_AUTO && x4_lang_current() == X4_LANG_FR);
    check_market();
    x4_ui_lang_pref_stop();
    assert(!remove(path));
    puts("PASS T50 language preference: draft, discard, live save, reload, all choices, corrupt/missing, write failure, locale and market");
    return 0;
}

