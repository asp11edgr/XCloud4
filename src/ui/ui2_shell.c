/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_shell.h"
#include "../core/app_identity.h"
#include "canvas.h"
#include "font.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    SIDEBAR_W = X4_UI2_SIDEBAR_W,
    ROW_X = 73, ROW_W = 151, ROW_H = 48, ROW_RADIUS = X4_UI2_RADIUS_CONTROL,
    STATUS_X = 1452, STATUS_Y = 56, STATUS_H = 58,
#if !X4_PUBLIC && defined(X4_UPDATE_TEST_MARKER)
    STATUS_MARKER_W = 128,
#else
    STATUS_MARKER_W = 0,
#endif
    CAPSULE_W = 304, AVATAR_W = 78, STATUS_W = CAPSULE_W + AVATAR_W + STATUS_MARKER_W,
};
#define NAV_BACK X4_RGB(10, 19, 30)

const X4UiRect x4_ui2_sidebar_rect = {0, 0, SIDEBAR_W, X4_UI_HEIGHT};
const X4UiRect x4_ui2_status_rect = {STATUS_X - STATUS_MARKER_W, STATUS_Y, STATUS_W, STATUS_H};

static bool valid_section(X4Ui2Section section)
{
    return (unsigned)section < X4_UI2_SECTION_COUNT;
}

X4Ui2NavResult x4_ui2_shell_handle(X4Ui2Shell *shell, unsigned flags, bool at_left_edge)
{
    X4Ui2NavResult result = {0};
    if (!shell || flags & X4_UI2_NAV_CIRCLE) return result;
    if (flags & X4_UI2_NAV_OPTIONS)
        return (X4Ui2NavResult){.consumed = true, .activate = true, .target = X4_UI2_SETTINGS};
    if (flags & (X4_UI2_NAV_PREVIOUS | X4_UI2_NAV_NEXT)) {
        if ((unsigned)shell->section > X4_UI2_LIBRARY) return result;
        unsigned delta = flags & X4_UI2_NAV_NEXT ? 1u : X4_UI2_LIBRARY;
        X4Ui2Section target = (X4Ui2Section)((shell->section + delta) % (X4_UI2_LIBRARY + 1u));
        return (X4Ui2NavResult){.consumed = true, .activate = true, .target = target};
    }
    if (!shell->focused) {
        if ((flags & X4_UI2_NAV_LEFT) && at_left_edge) {
            shell->selected = valid_section(shell->section) ? shell->section : X4_UI2_HOME;
            shell->focused = true;
            result.consumed = result.changed = true;
        }
        return result;
    }
    if (flags & X4_UI2_NAV_RIGHT) {
        shell->focused = false;
        return (X4Ui2NavResult){.consumed = true, .changed = true};
    }
    if (flags & (X4_UI2_NAV_UP | X4_UI2_NAV_DOWN)) {
        unsigned delta = flags & X4_UI2_NAV_DOWN ? 1u : X4_UI2_SECTION_COUNT - 1u;
        unsigned selected = valid_section(shell->selected) ? (unsigned)shell->selected : X4_UI2_HOME;
        shell->selected = (X4Ui2Section)((selected + delta) % X4_UI2_SECTION_COUNT);
        return (X4Ui2NavResult){.consumed = true, .changed = true};
    }
    if ((flags & X4_UI2_NAV_SELECT) && valid_section(shell->selected))
        return (X4Ui2NavResult){.consumed = true, .activate = true, .target = shell->selected};
    result.consumed = (flags & X4_UI2_NAV_LEFT) != 0;
    return result;
}

void x4_ui2_shell_set_section(X4Ui2Shell *shell, X4Ui2Section section)
{
    if (!shell || !valid_section(section)) return;
    shell->section = shell->selected = section;
    shell->focused = false;
}

static int row_y(unsigned row)
{
    if (row == X4_UI2_SUPPORT) return 803;
    if (row == X4_UI2_ABOUT) return 865;
    return 150 + (int)row * 68;
}

typedef struct {
    int size, weight;
} SidebarFace;

static const enum X4TextId sidebar_labels[X4_UI2_SECTION_COUNT] = {
    X4_TR_50_NAV_HOME, X4_TR_APPLE_TAB_DISCOVER, X4_TR_APPLE_TAB_CATALOG,
    X4_TR_U49_MY_GAMES, X4_TR_U49_SEARCH_LABEL, X4_TR_U49_SETTINGS,
    X4_TR_U49_SUPPORT_TITLE, X4_TR_U49_ABOUT_TITLE,
};

enum { ROW_TEXT_INSET = 46, ROW_TEXT_W = ROW_W - ROW_TEXT_INSET - 3 };
#if !X4_PUBLIC
static const char lab_label[] = "Lab (R3)";
#endif

static SidebarFace sidebar_face(void)
{
    static const SidebarFace choices[] = {
        {20, X4_FONT_SEMIBOLD}, {18, X4_FONT_REGULAR},
        {16, X4_FONT_REGULAR}, {14, X4_FONT_REGULAR},
    };
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i) {
        const SidebarFace face = choices[i];
        int width = 0;
        for (unsigned row = 0; row < X4_UI2_SECTION_COUNT; ++row) {
            const int label_width = x4_text_width_px(face.size, face.weight, x4_tr(sidebar_labels[row]));
            if (label_width > width) width = label_width;
        }
#if !X4_PUBLIC
        const int lab_width = x4_text_width_px(face.size, face.weight, lab_label);
        if (lab_width > width) width = lab_width;
#endif
        if (width <= ROW_TEXT_W) return face;
    }
    return choices[sizeof(choices) / sizeof(choices[0]) - 1];
}

static void row_label(uint32_t *pixels, unsigned row, SidebarFace face, uint32_t color)
{
    static const enum X4TablerIconId icons[X4_UI2_SECTION_COUNT] = {
        X4_ICON_HOME, X4_ICON_WORLD, X4_ICON_DEVICE_DESKTOP, X4_ICON_DEVICE_GAMEPAD,
        X4_ICON_SEARCH, X4_ICON_SETTINGS, X4_ICON_HEART, X4_ICON_INFO,
    };
    const int y = row_y(row);
    x4_apple_draw_icon(pixels, ROW_X + 13, y + (ROW_H - 24) / 2, icons[row], color);
    x4_text_px(pixels, ROW_X + ROW_TEXT_INSET, y + (ROW_H - face.size) / 2,
        face.size, face.weight, x4_tr(sidebar_labels[row]), color);
}

static void capture_rect(uint32_t *packed, const uint32_t *pixels, X4UiRect rect)
{
    size_t bytes = (size_t)rect.w * sizeof(*pixels);
    for (int y = 0; y < rect.h; ++y)
        memcpy(packed + (size_t)y * rect.w, pixels + (size_t)(rect.y + y) * X4_UI_WIDTH + rect.x, bytes);
}

static void paste_rect(uint32_t *pixels, const uint32_t *packed, X4UiRect rect)
{
    size_t bytes = (size_t)rect.w * sizeof(*pixels);
    for (int y = 0; y < rect.h; ++y)
        memcpy(pixels + (size_t)(rect.y + y) * X4_UI_WIDTH + rect.x, packed + (size_t)y * rect.w, bytes);
}

static struct {
    uint32_t backdrop[SIDEBAR_W * X4_UI_HEIGHT];
    uint32_t base[SIDEBAR_W * X4_UI_HEIGHT];
    enum X4Lang language;
    SidebarFace face;
    bool has_backdrop, ready;
} sidebar_cache;

enum { NAV_ACTIVE = 1, NAV_FOCUSED = 2 };

static unsigned nav_style(const X4Ui2Shell *shell, unsigned row)
{
    unsigned active = shell && valid_section(shell->section) ? (unsigned)shell->section : X4_UI2_HOME;
    return (row == active ? NAV_ACTIVE : 0) |
        (shell && shell->focused && (unsigned)shell->selected == row ? NAV_FOCUSED : 0);
}

static void draw_highlight(uint32_t *pixels, unsigned row, unsigned style)
{
    if (!style) return;
    int y = row_y(row);
    bool active = (style & NAV_ACTIVE) != 0;
    x4_apple_fill_round_rect(pixels, ROW_X, y, ROW_W, ROW_H, ROW_RADIUS,
        active ? X4_UI2_ACTIVE : X4_UI2_SURFACE);
    if (active)
        x4_apple_fill_round_rect(pixels, ROW_X, y + 6, 4, ROW_H - 12, 2, X4_UI2_ACCENT);
    row_label(pixels, row, sidebar_cache.face, X4_UI2_TEXT);
    if (style & NAV_FOCUSED) x4_ui2_focus(pixels, ROW_X, y, ROW_W, ROW_H, ROW_RADIUS);
}

#if !X4_PUBLIC
static void draw_lab(uint32_t *pixels, SidebarFace face)
{
    const int x = ROW_X + 13, y = 939;
    x4_rect(pixels, x + 8, y, 9, 2, X4_UI2_MUTED);
    x4_rect(pixels, x + 10, y + 2, 2, 8, X4_UI2_MUTED);
    x4_rect(pixels, x + 14, y + 2, 2, 8, X4_UI2_MUTED);
    for (int i = 0; i < 9; ++i) {
        x4_rect(pixels, x + 10 - i, y + 9 + i, 2, 2, X4_UI2_MUTED);
        x4_rect(pixels, x + 14 + i, y + 9 + i, 2, 2, X4_UI2_MUTED);
    }
    x4_rect(pixels, x + 3, y + 18, 19, 2, X4_UI2_MUTED);
    x4_rect(pixels, x + 7, y + 14, 11, 1, X4_UI2_MUTED);
    x4_text_px(pixels, ROW_X + ROW_TEXT_INSET, y + (20 - face.size) / 2,
        face.size, face.weight, lab_label, X4_UI2_MUTED);
}
#endif

static bool remember_sidebar_backdrop(const uint32_t *pixels, int fresh_height)
{
    bool changed = false;
    if (!sidebar_cache.has_backdrop) {
        for (int i = 0; i < SIDEBAR_W * X4_UI_HEIGHT; ++i)
            sidebar_cache.backdrop[i] = X4_UI2_BACKGROUND;
        sidebar_cache.has_backdrop = true;
    }
    const size_t row_bytes = (size_t)SIDEBAR_W * sizeof(*pixels);
    for (int y = 0; y < fresh_height; ++y) {
        const uint32_t *source = pixels + (size_t)y * X4_UI_WIDTH;
        uint32_t *target = sidebar_cache.backdrop + (size_t)y * SIDEBAR_W;
        if (memcmp(source, target, row_bytes)) {
            memcpy(target, source, row_bytes);
            changed = true;
        }
    }
    return changed;
}

static void paint_sidebar_backdrop(uint32_t *pixels)
{
    enum { SOFT_EDGE = 74, MAX_SHADE = 224 };
    static uint32_t tint[SIDEBAR_W];
    static bool tint_ready;
    if (!tint_ready) {
        for (unsigned x = 0; x < SIDEBAR_W; ++x) {
            unsigned edge = SIDEBAR_W - 1u - x;
            if (edge > SOFT_EDGE) edge = SOFT_EDGE;
            const unsigned alpha = MAX_SHADE * edge * edge * (3u * SOFT_EDGE - 2u * edge) /
                (SOFT_EDGE * SOFT_EDGE * SOFT_EDGE);
            tint[x] = X4_RGBA(5, 11, 20, alpha);
        }
        tint_ready = true;
    }
    paste_rect(pixels, sidebar_cache.backdrop, x4_ui2_sidebar_rect);
    for (int y = 0; y < X4_UI_HEIGHT; ++y) {
        uint32_t *row = pixels + (size_t)y * X4_UI_WIDTH;
        for (int x = 0; x < SIDEBAR_W - 1; ++x)
            x4_apple_blend_pixel(row + x, tint[x]);
    }
}

void x4_ui2_shell_draw_sidebar_backdrop(uint32_t *pixels, const X4Ui2Shell *shell, int fresh_height)
{
    if (!pixels) return;
    if (fresh_height > X4_UI_HEIGHT) fresh_height = X4_UI_HEIGHT;
    const bool changed = remember_sidebar_backdrop(pixels, fresh_height);
    enum X4Lang language = x4_lang_current();
    if (!changed && sidebar_cache.ready && sidebar_cache.language == language)
        paste_rect(pixels, sidebar_cache.base, x4_ui2_sidebar_rect);
    else {
        if (!sidebar_cache.ready || sidebar_cache.language != language)
            sidebar_cache.face = sidebar_face();
        paint_sidebar_backdrop(pixels);
        x4_ui2_brand(pixels, X4_UI_SAFE_LEFT, 66, 23);
        for (unsigned i = 0; i < X4_UI2_SECTION_COUNT; ++i)
            row_label(pixels, i, sidebar_cache.face, X4_UI2_MUTED);
        x4_rect(pixels, ROW_X, 780, ROW_W, 1, X4_UI2_BORDER);
#if !X4_PUBLIC
        draw_lab(pixels, sidebar_cache.face);
#endif
        capture_rect(sidebar_cache.base, pixels, x4_ui2_sidebar_rect);
        sidebar_cache.language = language;
        sidebar_cache.ready = true;
    }
    for (unsigned i = 0; i < X4_UI2_SECTION_COUNT; ++i)
        draw_highlight(pixels, i, nav_style(shell, i));
}

void x4_ui2_shell_draw_sidebar(uint32_t *pixels, const X4Ui2Shell *shell)
{
    x4_ui2_shell_draw_sidebar_backdrop(pixels, shell, 0);
}

size_t x4_ui2_shell_draw_sidebar_delta(uint32_t *pixels, const X4Ui2Shell *previous,
    const X4Ui2Shell *shell, X4UiRect damage[4])
{
    if (!pixels) return 0;
    if (!previous || !sidebar_cache.ready || sidebar_cache.language != x4_lang_current()) {
        x4_ui2_shell_draw_sidebar(pixels, shell);
        if (damage) damage[0] = x4_ui2_sidebar_rect;
        return 1;
    }

    enum { SPREAD = X4_UI2_FOCUS_SPREAD };
    size_t count = 0;
    for (unsigned i = 0; i < X4_UI2_SECTION_COUNT; ++i) {
        unsigned style = nav_style(shell, i);
        if (style == nav_style(previous, i)) continue;
        X4UiRect r = {ROW_X - SPREAD, row_y(i) - SPREAD, ROW_W + 2 * SPREAD, ROW_H + 2 * SPREAD};
        for (int y = r.y; y < r.y + r.h; ++y)
            memcpy(pixels + (size_t)y * X4_UI_WIDTH + r.x,
                sidebar_cache.base + (size_t)y * SIDEBAR_W + r.x, (size_t)r.w * sizeof(*pixels));
        draw_highlight(pixels, i, style);
        /* At most the two old and two new highlighted rows can differ. */
        if (damage) damage[count] = r;
        ++count;
    }
    return count;
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
    avatar_initial(next.initial, gamertag);
    if (status->valid && status->network == next.network &&
        !strcmp(status->quality, next.quality) && status->minute == next.minute &&
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

static struct {
    uint32_t backdrop[STATUS_W * STATUS_H];
    uint32_t *canvas;
    X4Ui2Status values;
    enum X4Lang language;
} status_cache;

void x4_ui2_shell_draw_status(uint32_t *pixels, const X4Ui2Status *status, bool fresh_background)
{
    if (!pixels) return;
    static const X4Ui2Status empty = {.minute = -1};
    if (!status || !status->valid) status = &empty;
    const bool fresh = fresh_background || pixels != status_cache.canvas;
    const X4Ui2Status *old = &status_cache.values;
    enum X4Lang language = x4_lang_current();
    if (!fresh && status_cache.language == language && old->network == status->network &&
        old->minute == status->minute && !strcmp(old->quality, status->quality) &&
        !memcmp(old->initial, status->initial, sizeof(old->initial))) return;
    if (fresh) capture_rect(status_cache.backdrop, pixels, x4_ui2_status_rect);
    else paste_rect(pixels, status_cache.backdrop, x4_ui2_status_rect);

    const int x = STATUS_X, y = STATUS_Y + 7;
#if !X4_PUBLIC && defined(X4_UPDATE_TEST_MARKER)
    x4_ui2_card(pixels, x - STATUS_MARKER_W, y, 116, 44, 22);
    x4_text_px(pixels, x - STATUS_MARKER_W + 12, y + 12,
        X4_UI2_CAPTION, X4_FONT_SEMIBOLD, X4_UPDATE_TEST_MARKER, X4_UI2_ACCENT);
#endif
    x4_ui2_card(pixels, x, y, CAPSULE_W, 44, 22);
    connection_icon(pixels, x + 16, y + 10, status->network);
    const char *quality = status->quality[0] ? status->quality :
        x4_tr(X4_TR_50_STATUS_AUTO_QUALITY);
    char time[8] = "--:--";
    if (status->minute >= 0 && status->minute < 24 * 60)
        snprintf(time, sizeof(time), "%02u:%02u", (unsigned)status->minute / 60u, (unsigned)status->minute % 60u);
    const int clock_x = x + 228;
#ifdef X4_UI48_CAPTURE
    x4_ui46_capture_contrast(pixels, "Calidad elegida", x + 49, y + 12, 159, X4_UI2_CAPTION, X4_UI2_TEXT);
    x4_ui46_capture_contrast(pixels, "Hora", clock_x, y + 12, 60, X4_UI2_CAPTION, X4_UI2_TEXT);
#endif
    x4_text_ellipsis_px(pixels, x + 49, y + 12, X4_UI2_CAPTION,
        X4_FONT_SEMIBOLD, quality, 159, X4_UI2_TEXT);
    x4_rect(pixels, x + 214, y + 11, 1, 22, X4_UI2_BORDER);
    x4_text_px(pixels, clock_x, y + 12, X4_UI2_CAPTION, X4_FONT_SEMIBOLD, time, X4_UI2_TEXT);

    const int center = STATUS_X + CAPSULE_W + 47;
    x4_ui2_card(pixels, center - 22, y, 44, 44, 22);
    if (status->initial[0]) {
        int width = x4_text_width_px(24, X4_FONT_BOLD, status->initial);
        x4_text_px(pixels, center - width / 2, y + 10,
            24, X4_FONT_BOLD, status->initial, X4_UI2_TEXT);
        x4_apple_fill_round_rect(pixels, center + 12, y + 32, 16, 16, 8, NAV_BACK);
        x4_apple_fill_round_rect(pixels, center + 15, y + 35, 10, 10, 5, X4_UI2_ACCENT);
    } else
        x4_apple_draw_icon(pixels, center - 12, y + 10, X4_ICON_USER, X4_UI2_MUTED);
    status_cache.values = *status;
    status_cache.canvas = pixels;
    status_cache.language = language;
}

void x4_ui2_shell_draw(uint32_t *pixels, const X4Ui2Shell *shell, const X4Ui2Status *status)
{
    x4_ui2_shell_draw_sidebar_backdrop(pixels, shell, X4_UI_HEIGHT);
    x4_ui2_shell_draw_status(pixels, status, true);
}

enum {
    LEGACY_X = X4_UI2_CONTENT_X, LEGACY_Y = 128,
    LEGACY_W = (X4_UI_SAFE_RIGHT - LEGACY_X) / 16 * 16, LEGACY_H = LEGACY_W * 9 / 16,
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
