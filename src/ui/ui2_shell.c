/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_shell.h"
#include "../core/app_identity.h"
#include "canvas.h"
#include "font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { NAV_Y = 23, NAV_H = 61, PROFILE_X = 1408 };

const X4UiRect x4_ui2_header_rect = {0, 0, X4_UI_WIDTH, X4_UI2_HEADER_H};

static bool valid_section(X4Ui2Section section)
{
    return (unsigned)section < X4_UI2_SECTION_COUNT;
}

X4Ui2NavResult x4_ui2_shell_handle(X4Ui2Shell *shell, unsigned flags, bool at_top_edge)
{
    X4Ui2NavResult result = {0};
    if (!shell) return result;
    if (flags & X4_UI2_NAV_CIRCLE) {
        if (!shell->focused) return result;
        shell->focused = false;
        return (X4Ui2NavResult){.consumed = true, .changed = true};
    }
    if (flags & X4_UI2_NAV_OPTIONS)
        return (X4Ui2NavResult){.consumed = true, .activate = true, .target = X4_UI2_SETTINGS};
    if (flags & (X4_UI2_NAV_PREVIOUS | X4_UI2_NAV_NEXT)) {
        const bool next = (flags & X4_UI2_NAV_NEXT) != 0;
        const unsigned count = X4_UI2_SETTINGS + 1u;
        X4Ui2Section target = (unsigned)shell->section < count ?
            (X4Ui2Section)((shell->section + (next ? 1u : count - 1u)) % count) :
            (next ? X4_UI2_HOME : X4_UI2_SETTINGS);
        return (X4Ui2NavResult){.consumed = true, .activate = true, .target = target};
    }
    if (!shell->focused) {
        if ((flags & X4_UI2_NAV_UP) && at_top_edge) {
            shell->selected = valid_section(shell->section) ? shell->section : X4_UI2_HOME;
            shell->focused = true;
            result.consumed = result.changed = true;
        }
        return result;
    }
    if (flags & X4_UI2_NAV_DOWN) {
        shell->focused = false;
        return (X4Ui2NavResult){.consumed = true, .changed = true};
    }
    if (flags & (X4_UI2_NAV_LEFT | X4_UI2_NAV_RIGHT)) {
        unsigned delta = flags & X4_UI2_NAV_RIGHT ? 1u : X4_UI2_SECTION_COUNT - 1u;
        unsigned selected = valid_section(shell->selected) ? (unsigned)shell->selected : X4_UI2_HOME;
        shell->selected = (X4Ui2Section)((selected + delta) % X4_UI2_SECTION_COUNT);
        return (X4Ui2NavResult){.consumed = true, .changed = true};
    }
    if ((flags & X4_UI2_NAV_SELECT) && valid_section(shell->selected))
        return (X4Ui2NavResult){.consumed = true, .activate = true, .target = shell->selected};
    result.consumed = (flags & X4_UI2_NAV_UP) != 0;
    return result;
}

void x4_ui2_shell_set_section(X4Ui2Shell *shell, X4Ui2Section section)
{
    if (!shell || !valid_section(section)) return;
    shell->section = shell->selected = section;
    shell->focused = false;
}

typedef struct {
    int x, w;
    enum X4TextId label;
    enum X4TablerIconId icon;
    bool icon_only;
} NavTab;

static const NavTab nav_tabs[X4_UI2_ACCOUNT] = {
    {314, 128, X4_TR_50_NAV_HOME, X4_ICON_NONE, false},
    {451, 144, X4_TR_APPLE_TAB_DISCOVER, X4_ICON_NONE, false},
    {604, 140, X4_TR_APPLE_TAB_CATALOG, X4_ICON_NONE, false},
    {753, 156, X4_TR_U49_MY_GAMES, X4_ICON_NONE, false},
    {918, 139, X4_TR_U49_SEARCH_LABEL, X4_ICON_SEARCH, false},
    {1066, 199, X4_TR_U49_SETTINGS, X4_ICON_SETTINGS, false},
    {1274, 55, X4_TR_U49_SUPPORT_TITLE, X4_ICON_HEART, true},
    {1338, 55, X4_TR_U49_ABOUT_TITLE, X4_ICON_INFO, true},
};

static struct {
    uint32_t backdrop[X4_UI_WIDTH * X4_UI2_HEADER_H];
    uint32_t *canvas;
} header;

static void draw_tab(uint32_t *pixels, const NavTab *tab, bool active, bool focused)
{
    if (focused) {
        x4_apple_fill_round_rect(pixels, tab->x, NAV_Y, tab->w, NAV_H,
            X4_UI2_RADIUS_CONTROL, X4_UI2_ACTIVE);
        x4_ui2_focus(pixels, tab->x, NAV_Y, tab->w, NAV_H, X4_UI2_RADIUS_CONTROL);
    }
    const char *label = x4_tr(tab->label);
    const int icon_space = tab->icon == X4_ICON_NONE ? 0 : 36;
    const int available = tab->w - 16 - icon_space;
    int weight = active || focused ? X4_FONT_BOLD : X4_FONT_SEMIBOLD;
    int size = 26;
    int width = x4_text_width_px(size, weight, label);
    if (width > available) {
        size = 22;
        width = x4_text_width_px(size, weight, label);
    }
    if (width > available) width = available;
    const int left = tab->x + (tab->w - icon_space - width) / 2;
    if (tab->icon != X4_ICON_NONE)
        x4_apple_draw_icon(pixels, tab->icon_only ? tab->x + (tab->w - 24) / 2 : left,
            NAV_Y + (NAV_H - 24) / 2, tab->icon, X4_UI2_TEXT);
    if (!tab->icon_only)
        x4_text_ellipsis_px(pixels, left + icon_space, NAV_Y + (NAV_H - size) / 2,
            size, weight, label, available, X4_UI2_TEXT);
    if (active)
        x4_rect(pixels, tab->x + 10, NAV_Y + NAV_H + 3, tab->w - 20, 3, X4_UI2_ACCENT);
    if (tab->icon_only && focused) {
        const int tooltip = x4_text_width_px(16, X4_FONT_SEMIBOLD, label);
        x4_text_px(pixels, tab->x + (tab->w - tooltip) / 2, 86, 16,
            X4_FONT_SEMIBOLD, label, X4_UI2_TEXT);
    }
}

static void avatar_initial(char output[5], const char *gamertag)
{
    if (!gamertag) return;
    while (*gamertag == ' ' || *gamertag == '\t') ++gamertag;
    if (!*gamertag) return;
    uint32_t c = x4_utf8_next(&gamertag);
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) c -= 32;
    if (c < 32 || (c >= 127 && c <= 159)) return;
    if (c < 0x80) output[0] = (char)c;
    else if (c < 0x800) {
        output[0] = (char)(0xc0 | (c >> 6));
        output[1] = (char)(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        output[0] = (char)(0xe0 | (c >> 12));
        output[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        output[2] = (char)(0x80 | (c & 0x3f));
    } else {
        output[0] = (char)(0xf0 | (c >> 18));
        output[1] = (char)(0x80 | ((c >> 12) & 0x3f));
        output[2] = (char)(0x80 | ((c >> 6) & 0x3f));
        output[3] = (char)(0x80 | (c & 0x3f));
    }
}

bool x4_ui2_status_set(X4Ui2Status *status, X4Ui2Network network,
    const char *quality, int minute, const char *gamertag)
{
    if (!status) return false;
    X4Ui2Status next = {
        .network = (unsigned)network <= X4_UI2_NETWORK_WIFI ? network : X4_UI2_NETWORK_UNKNOWN,
        .minute = minute >= 0 && minute < 24 * 60 ? minute : -1,
        .valid = true,
    };
    if (quality) snprintf(next.quality, sizeof(next.quality), "%s", quality);
    if (gamertag) snprintf(next.gamertag, sizeof(next.gamertag), "%s", gamertag);
    avatar_initial(next.initial, gamertag);
    if (status->valid && status->network == next.network &&
        !strcmp(status->quality, next.quality) && !strcmp(status->gamertag, next.gamertag) && status->minute == next.minute &&
        !memcmp(status->initial, next.initial, sizeof(next.initial))) return false;
    *status = next;
    return true;
}

bool x4_ui2_status_update(X4Ui2Status *status, X4Ui2Network network,
    unsigned height, unsigned ignored_rate, int minute, const char *gamertag)
{
    (void)ignored_rate;
    char quality[16] = {0};
    if (height && height <= 4320) snprintf(quality, sizeof(quality), "%up", height);
    return x4_ui2_status_set(status, network, quality, minute, gamertag);
}

static void connection_icon(uint32_t *pixels, int x, int y, X4Ui2Network network)
{
    uint32_t accent = X4_UI2_ACCENT;
    if (network == X4_UI2_NETWORK_UNKNOWN) {
        x4_apple_draw_icon(pixels, x, y, X4_ICON_WORLD, X4_UI2_MUTED);
        return;
    }
    if (network == X4_UI2_NETWORK_CABLE) {
        x4_apple_stroke_round_rect(pixels, x + 1, y + 3, 22, 14, 3, 2.0f, accent);
        x4_rect(pixels, x + 8, y + 16, 8, 4, accent);
        for (int i = 0; i < 3; ++i)
            x4_rect(pixels, x + 7 + 4 * i, y + 7, 2, 5, accent);
        return;
    }
    /* Three compact arcs. This is composed only when status data changes. */
    for (int j = 0; j < 19; ++j) {
        for (int i = 0; i < 24; ++i) {
            int dx = i - 12, dy = 18 - j;
            if (dx < -dy || dx > dy) continue;
            float distance = sqrtf((float)(dx * dx + dy * dy));
            if (fabsf(distance - 15.0f) <= 1.0f || fabsf(distance - 10.0f) <= 1.0f ||
                fabsf(distance - 5.0f) <= 1.0f)
                pixels[(size_t)(y + j) * X4_UI_WIDTH + x + i] = accent;
        }
    }
    x4_apple_fill_round_rect(pixels, x + 10, y + 18, 5, 5, 2, accent);
}

static void draw_profile(uint32_t *pixels, const X4Ui2Status *status, bool active, bool focused)
{
    static const X4Ui2Status empty = {.minute = -1};
    if (!status || !status->valid) status = &empty;
    enum { AVATAR_X = PROFILE_X + 16, AVATAR_Y = NAV_Y + 8, AVATAR_EDGE = 46,
        TEXT_X = AVATAR_X + AVATAR_EDGE + 15 };
    if (focused) {
        x4_apple_fill_round_rect(pixels, PROFILE_X, NAV_Y, X4_UI_SAFE_RIGHT - PROFILE_X, NAV_H,
            X4_UI2_RADIUS_CONTROL, X4_UI2_ACTIVE);
        x4_ui2_focus(pixels, PROFILE_X, NAV_Y, X4_UI_SAFE_RIGHT - PROFILE_X, NAV_H,
            X4_UI2_RADIUS_CONTROL);
    } else {
        x4_rect(pixels, PROFILE_X, NAV_Y + 12, 1, 38, X4_UI2_BORDER);
    }
    x4_ui2_card(pixels, AVATAR_X, AVATAR_Y, AVATAR_EDGE, AVATAR_EDGE, AVATAR_EDGE / 2);
    if (status->initial[0]) {
        const int width = x4_text_width_px(24, X4_FONT_BOLD, status->initial);
        x4_text_px(pixels, AVATAR_X + (AVATAR_EDGE - width) / 2, AVATAR_Y + 10,
            24, X4_FONT_BOLD, status->initial, X4_UI2_TEXT);
    } else {
        x4_apple_draw_icon(pixels, AVATAR_X + 11, AVATAR_Y + 11, X4_ICON_USER, X4_UI2_TEXT);
    }
    x4_text_ellipsis_px(pixels, TEXT_X, NAV_Y + 4, 24, X4_FONT_SEMIBOLD,
        status->gamertag[0] ? status->gamertag : x4_tr(X4_TR_U49_ACCOUNT_TITLE),
        225, X4_UI2_TEXT);
    connection_icon(pixels, TEXT_X, NAV_Y + 34, status->network);
    char detail[80], time[8] = "--:--";
    if (status->minute >= 0 && status->minute < 24 * 60)
        snprintf(time, sizeof(time), "%02u:%02u", (unsigned)status->minute / 60u,
            (unsigned)status->minute % 60u);
    snprintf(detail, sizeof(detail), "%s  ·  %s", status->quality[0] ?
        status->quality : x4_tr(X4_TR_50_STATUS_AUTO_QUALITY), time);
    x4_text_ellipsis_px(pixels, TEXT_X + 32, NAV_Y + 37, 18, X4_FONT_SEMIBOLD,
        detail, 232, X4_UI2_MUTED);
    if (active) x4_rect(pixels, TEXT_X, NAV_Y + NAV_H + 3, 84, 3, X4_UI2_ACCENT);
#if !X4_PUBLIC
#if defined(X4_UPDATE_TEST_MARKER)
    const char *edition = X4_UPDATE_TEST_MARKER;
#else
    const char *edition = "Lab (R3)";
#endif
    x4_text_ellipsis_px(pixels, X4_UI_SAFE_RIGHT - 112, NAV_Y + 5, 18, X4_FONT_SEMIBOLD,
        edition, 108, X4_UI2_ACCENT);
#endif
}

void x4_ui2_shell_draw_header(uint32_t *pixels, const X4Ui2Shell *shell,
    const X4Ui2Status *status, bool fresh_background)
{
    if (!pixels) return;
    if (fresh_background || header.canvas != pixels) {
        memcpy(header.backdrop, pixels, sizeof(header.backdrop));
        header.canvas = pixels;
        for (int y = 0; y < X4_UI2_HEADER_H; ++y) {
            const unsigned opacity = y < 72 ? 187u :
                (unsigned)(X4_UI2_HEADER_H - 1 - y) * 187u / 36u;
            uint32_t *row = header.backdrop + (size_t)y * X4_UI_WIDTH;
            for (int x = 0; x < X4_UI_WIDTH; ++x)
                x4_apple_blend_pixel(row + x, X4_UI2_TINT(opacity));
        }
    }
    memcpy(pixels, header.backdrop, sizeof(header.backdrop));
    x4_ui2_brand(pixels, X4_UI_SAFE_LEFT, NAV_Y + 3, 36);
    const int glyph_x = X4_UI_SAFE_LEFT + 1;
    const int glyph_y = NAV_Y + 43;
    const int l1_width = x4_ui2_pad_glyph(pixels, glyph_x, glyph_y, X4_PAD_L1);
    x4_ui2_pad_glyph(pixels, glyph_x + l1_width + 8, glyph_y, X4_PAD_R1);
    const unsigned active = shell && valid_section(shell->section) ? (unsigned)shell->section : X4_UI2_HOME;
    const unsigned focus = shell && shell->focused ? (unsigned)shell->selected : X4_UI2_SECTION_COUNT;
    for (unsigned i = 0; i < X4_UI2_ACCOUNT; ++i)
        draw_tab(pixels, &nav_tabs[i], i == active, i == focus);
    draw_profile(pixels, status, active == X4_UI2_ACCOUNT, focus == X4_UI2_ACCOUNT);
}

void x4_ui2_shell_draw(uint32_t *pixels, const X4Ui2Shell *shell, const X4Ui2Status *status)
{
    x4_ui2_shell_draw_header(pixels, shell, status, true);
}

enum {
    LEGACY_X = X4_UI2_CONTENT_X, LEGACY_Y = 120,
    LEGACY_W = (X4_UI_SAFE_BOTTOM - LEGACY_Y) / 9 * 16, LEGACY_H = LEGACY_W * 9 / 16,
};
_Static_assert(LEGACY_W * 9 == LEGACY_H * 16 && LEGACY_Y + LEGACY_H <= X4_UI_SAFE_BOTTOM,
    "The legacy adapter must preserve aspect ratio and safe bounds");
static uint16_t legacy_columns[LEGACY_W];
static bool legacy_map_ready;

void x4_ui2_shell_fit_legacy(uint32_t *pixels, const uint32_t *source)
{
    if (!pixels || !source || pixels == source) return;
    if (!legacy_map_ready) {
        for (int x = 0; x < LEGACY_W; ++x)
            legacy_columns[x] = (uint16_t)(x * X4_UI_WIDTH / LEGACY_W);
        legacy_map_ready = true;
    }
    for (int y = 0; y < LEGACY_H; ++y) {
        int sy = y * X4_UI_HEIGHT / LEGACY_H;
        const uint32_t *row = source + (size_t)sy * X4_UI_WIDTH;
        uint32_t *destination = pixels + (size_t)(y + LEGACY_Y) * X4_UI_WIDTH + LEGACY_X;
        for (int x = 0; x < LEGACY_W; ++x)
            destination[x] = row[legacy_columns[x]];
    }
    x4_rect(pixels, 0, 0, X4_UI_WIDTH, LEGACY_Y, X4_UI2_BACKGROUND);
    x4_rect(pixels, 0, LEGACY_Y, LEGACY_X, LEGACY_H, X4_UI2_BACKGROUND);
    x4_rect(pixels, LEGACY_X + LEGACY_W, LEGACY_Y, X4_UI_WIDTH - LEGACY_X - LEGACY_W, LEGACY_H, X4_UI2_BACKGROUND);
    x4_rect(pixels, 0, LEGACY_Y + LEGACY_H, X4_UI_WIDTH, X4_UI_HEIGHT - LEGACY_Y - LEGACY_H, X4_UI2_BACKGROUND);
}
