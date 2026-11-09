/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_account.h"
#include "canvas.h"
#include "cloud_time_ui.h"
#include "net_hint.h"
#include "screen.h"
#include "ui2_modal.h"
#include "../../third_party/qrcodegen/qrcodegen.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

enum {
    W = X4_UI_WIDTH, H = X4_UI_HEIGHT,
    LEFT = X4_UI2_CONTENT_X, RIGHT = X4_UI_SAFE_RIGHT,
    ACTION_W = 728, ACTION_Y = 365, ACTION_PITCH = 83, ACTION_H = 68,
    HISTORY_X = 1030, HISTORY_Y = ACTION_Y, HISTORY_W = RIGHT - HISTORY_X,
    ENTRY_X = HISTORY_X + 24, ENTRY_Y = HISTORY_Y + 107, ENTRY_W = HISTORY_W - 48,
    ENTRY_H = 60, ENTRY_PITCH = 76, NO_FOCUS = UINT_MAX,
};

_Static_assert(ENTRY_Y + (X4_UI2_ACCOUNT_TOP_GAMES - 1) * ENTRY_PITCH +
    ENTRY_H + X4_UI2_FOCUS_SPREAD < X4_UI_FOOTER_Y,
    "Account history and focus are clear of the footer");
_Static_assert(LEFT + ACTION_W + 2 * X4_UI2_FOCUS_SPREAD < HISTORY_X,
    "Account columns have room for focus on both sides");

static bool history_precedes(const X4HistoryEntry *a, const X4HistoryEntry *b)
{
    if (a->played_s != b->played_s) return a->played_s > b->played_s;
    return a->last_session_s > b->last_session_s;
}

void x4_ui2_account_summary(const X4History *history, X4Ui2AccountSummary *out)
{
    if (!out) return;
    *out = (X4Ui2AccountSummary){0};
    if (!history || history->count > X4_HISTORY_MAX) return;
    for (unsigned i = 0; i < history->count; ++i) {
        const X4HistoryEntry *entry = &history->entries[i];
        if (!entry->product_id[0]) continue;
        ++out->games;
        const uint64_t available = UINT64_MAX - out->total_seconds;
        out->total_seconds += entry->played_s < available ? entry->played_s : available;

        unsigned place = 0;
        while (place < out->count && !history_precedes(entry, &out->most_played[place]))
            ++place;
        if (place == X4_UI2_ACCOUNT_TOP_GAMES) continue;
        if (out->count < X4_UI2_ACCOUNT_TOP_GAMES) ++out->count;
        for (unsigned pos = out->count - 1; pos > place; --pos)
            out->most_played[pos] = out->most_played[pos - 1];
        out->most_played[place] = *entry;
    }
}

static unsigned history_count(const X4Ui2AccountModel *model)
{
    return model->history && model->history->count <= X4_UI2_ACCOUNT_TOP_GAMES ?
        model->history->count : 0;
}

static unsigned account_row(const X4Ui2Account *account, const X4Ui2AccountModel *model)
{
    if (account->row >= X4_UI2_ACCOUNT_HISTORY_FIRST + history_count(model)) return 0;
    return account->row;
}

bool x4_ui2_account_move(X4Ui2Account *account, const X4Ui2AccountModel *model,
    int dx, int dy)
{
    if (!account || !model || !model->auth || model->busy || model->closing ||
        model->auth->state != X4_AUTH_AUTHORIZED) return false;
    unsigned row = account_row(account, model);
    const bool in_history = row >= X4_UI2_ACCOUNT_HISTORY_FIRST;
    if (dx > 0 && !in_history && history_count(model)) {
        account->left_row = row;
        account->row = X4_UI2_ACCOUNT_HISTORY_FIRST;
    } else if (dx < 0 && in_history) {
        account->row = account->left_row < X4_UI2_ACCOUNT_HISTORY_FIRST ? account->left_row : 0;
    } else if (!dx && dy) {
        const unsigned first = in_history ? X4_UI2_ACCOUNT_HISTORY_FIRST : 0;
        const unsigned end = X4_UI2_ACCOUNT_HISTORY_FIRST + (in_history ? history_count(model) : 0);
        if (dy > 0 && row + 1 < end) account->row = row + 1;
        else if (dy < 0 && row > first) account->row = row - 1;
    }
    return account->row != row;
}

X4Ui2AccountAction x4_ui2_account_select(const X4Ui2Account *account,
    const X4Ui2AccountModel *model, const char **product_id)
{
    if (product_id) *product_id = NULL;
    if (!account || !model || !model->auth || model->busy || model->closing)
        return X4_UI2_ACCOUNT_NONE;
    if (model->auth->state != X4_AUTH_AUTHORIZED)
        return model->auth->state == X4_AUTH_WAITING ? X4_UI2_ACCOUNT_NONE : X4_UI2_ACCOUNT_SIGN_IN;
    const unsigned row = account_row(account, model);
    switch (row) {
    case 0: return model->remember.available || model->remember.enabled ?
        X4_UI2_ACCOUNT_REMEMBER : X4_UI2_ACCOUNT_NONE;
    case 1: return X4_UI2_ACCOUNT_SERVER;
    case 2: return X4_UI2_ACCOUNT_COUNTRY;
    case 3: return X4_UI2_ACCOUNT_SIGN_OUT;
    default:
        if (product_id) *product_id = model->history->most_played[row - X4_UI2_ACCOUNT_HISTORY_FIRST].product_id;
        return X4_UI2_ACCOUNT_OPEN_GAME;
    }
}

static void line(uint32_t *p, const char *role, int x, int y, int w,
    int size, int weight, const char *label, uint32_t color)
{
#ifdef X4_UI46_CAPTURE
    x4_ui46_capture_contrast(p, role, x, y, w, size, color);
#else
    (void)role;
#endif
    x4_ui2_text_bounded(p, x, y, size, weight, label, w, size + 8, 1, color);
}

static bool cloud_visible(X4CloudTime time)
{
    return time.source != X4_CLOUD_TIME_NONE &&
        (time.unlimited || (time.max_s && time.remaining_s <= time.max_s));
}

static const char *catalog_name(const X4CatalogSnapshot *catalog, const char *id)
{
    if (!catalog || catalog->state != X4_CATALOG_READY || catalog->count > X4_CATALOG_MAX)
        return NULL;
    for (unsigned i = 0; i < catalog->count; ++i) {
        const X4CatalogTitle *title = &catalog->titles[i];
        if (title->name[0] && !strcmp(title->product_id, id)) return title->name;
    }
    return NULL;
}

static X4UiRect row_rect(unsigned row)
{
    if (row >= X4_UI2_ACCOUNT_HISTORY_FIRST)
        return (X4UiRect){ENTRY_X, ENTRY_Y + (int)(row - X4_UI2_ACCOUNT_HISTORY_FIRST) * ENTRY_PITCH,
            ENTRY_W, ENTRY_H};
    int y = ACTION_Y + (int)row * ACTION_PITCH;
    if (row == 3) y += ACTION_PITCH;
    return (X4UiRect){LEFT, y, ACTION_W, ACTION_H};
}

static X4UiRect row_damage(X4UiRect r)
{
    const int pad = X4_UI2_FOCUS_SPREAD;
    return (X4UiRect){r.x - pad, r.y - pad, r.w + 2 * pad, r.h + 2 * pad};
}

static void draw_toggle(uint32_t *p, int x, int y, bool enabled)
{
    const uint32_t fill = enabled ? X4_UI2_ACCENT : X4_UI2_BORDER;
    const uint32_t knob = enabled ? X4_UI2_INK : X4_UI2_TEXT;
    x4_apple_fill_round_rect(p, x, y, 54, 30, 15, fill);
    x4_apple_fill_round_rect(p, x + (enabled ? 28 : 4), y + 4, 22, 22, 11, knob);
}

static void draw_lock_icon(uint32_t *p, int x, int y, uint32_t color)
{
    x4_apple_stroke_round_rect(p, x + 6, y + 1, 12, 15, 6, 2.0f, color);
    x4_apple_fill_round_rect(p, x + 3, y + 9, 18, 14, 3, X4_UI2_SURFACE);
    x4_apple_stroke_round_rect(p, x + 3, y + 9, 18, 14, 3, 2.0f, color);
    x4_apple_fill_round_rect(p, x + 10, y + 13, 4, 5, 2, color);
}

static void draw_sign_out_icon(uint32_t *p, int x, int y, uint32_t color)
{
    x4_rect(p, x + 3, y + 3, 2, 18, color);
    x4_rect(p, x + 5, y + 3, 7, 2, color);
    x4_rect(p, x + 5, y + 19, 7, 2, color);
    x4_rect(p, x + 8, y + 11, 12, 2, color);
    for (int i = 0; i < 5; ++i) {
        x4_rect(p, x + 16 + i, y + 6 + i, 2, 2, color);
        x4_rect(p, x + 16 + i, y + 16 - i, 2, 2, color);
    }
}

static void draw_action(uint32_t *p, const X4Ui2AccountModel *model,
    X4UiRect r, unsigned row, bool focused)
{
    const enum X4TextId labels[] = {X4_TR_AUTH_REMEMBER, X4_TR_U49_ACCOUNT_SERVER,
        X4_TR_ACCESS_COUNTRY, X4_TR_U49_ACCOUNT_SIGN_OUT};
    const bool available = model->remember.available || model->remember.enabled;
    x4_ui2_card(p, r.x, r.y, r.w, r.h, X4_UI2_RADIUS_CONTROL);
    const int icon_x = r.x + 20, icon_y = r.y + 22;
    if (row == 0) draw_lock_icon(p, icon_x, icon_y, X4_UI2_MUTED);
    else if (row == 3) draw_sign_out_icon(p, icon_x, icon_y, X4_UI2_MUTED);
    else x4_apple_draw_icon(p, icon_x, icon_y, row == 1 ? X4_ICON_SERVER : X4_ICON_WORLD, X4_UI2_MUTED);
    const int value_x = r.x + 330;
    line(p, "account action", r.x + 56, r.y + 21,
        value_x - r.x - 76, X4_UI2_BODY, X4_FONT_SEMIBOLD, x4_tr(labels[row]), X4_UI2_TEXT);
    if (row == 0 && available) {
        const char *value = x4_tr(model->remember.enabled ? X4_TR_AUTH_REMEMBER_ON : X4_TR_AUTH_REMEMBER_OFF);
        const int toggle_x = r.x + r.w - 76;
        const int value_w = x4_text_width_px(X4_UI2_CAPTION, X4_FONT_SEMIBOLD, value);
        int x = toggle_x - value_w - 12;
        if (x < value_x) x = value_x;
        line(p, "account remember value", x, r.y + 23, toggle_x - x - 10,
            X4_UI2_CAPTION, X4_FONT_SEMIBOLD, value, X4_UI2_MUTED);
        draw_toggle(p, toggle_x, r.y + 19, model->remember.enabled);
    } else if (row < 3) {
        const char *value = row == 0 ? x4_tr(X4_TR_AUTH_REMEMBER_UNAVAILABLE) :
            row == 1 ? model->server : model->country;
        if (value && value[0]) {
            int end = r.x + r.w - (row == 0 ? 22 : 52);
            const int value_w = x4_text_width_px(X4_UI2_BODY, X4_FONT_REGULAR, value);
            int x = end - value_w;
            if (x < value_x) x = value_x;
            line(p, "account action value", x, r.y + 21, end - x,
                X4_UI2_BODY, X4_FONT_REGULAR, value, X4_UI2_MUTED);
        }
    }
    if (row) x4_apple_draw_icon(p, r.x + r.w - 40, r.y + 22,
        X4_ICON_CHEVRON_RIGHT, X4_UI2_MUTED);
    if (focused) x4_ui2_focus(p, r.x, r.y, r.w, r.h, X4_UI2_RADIUS_CONTROL);
}

static void draw_entry(uint32_t *p, const X4Ui2AccountModel *model,
    X4UiRect r, unsigned index, bool focused)
{
    const X4HistoryEntry *entry = &model->history->most_played[index];
    const char *name = catalog_name(model->catalog, entry->product_id);
    X4CatalogAssetView art = {0};
    const bool acquired = model->assets && x4_catalog_assets_acquire(model->assets, entry->product_id, &art);
    const X4CoverRaster *cover = NULL;
    if (acquired) {
        if (!name && art.metadata.name[0]) name = art.metadata.name;
        if (art.surface[X4_COVER_ROW].rgba) cover = &art.surface[X4_COVER_ROW];
    }
    x4_ui2_card(p, r.x, r.y, r.w, r.h, X4_UI2_RADIUS_CONTROL);
    if (cover)
        x4_apple_blit_image_rounded(p, r.x + 10, r.y + 3, 96, 54, 8, cover, X4_GRAD_NONE);
    else {
        x4_apple_fill_round_rect(p, r.x + 10, r.y + 3, 96, 54, 8, X4_UI2_BACKGROUND);
        x4_apple_draw_icon(p, r.x + 46, r.y + 18, X4_ICON_DEVICE_GAMEPAD, X4_UI2_MUTED);
    }
    char duration[64];
    x4_cloud_time_duration(duration, sizeof(duration), entry->played_s);
    const int time_w = x4_text_width_px(X4_UI2_CAPTION, X4_FONT_SEMIBOLD, duration);
    const int end = r.x + r.w - 56;
    const int time_x = end - time_w;
    line(p, "account game title", r.x + 124, r.y + 19, time_x - r.x - 146,
        X4_UI2_BODY, X4_FONT_SEMIBOLD, name ? name : x4_tr(X4_TR_U49_ACCOUNT_GAME), X4_UI2_TEXT);
    line(p, "account play time", time_x, r.y + 21, time_w + 4,
        X4_UI2_CAPTION, X4_FONT_SEMIBOLD, duration, X4_UI2_MUTED);
    x4_apple_draw_icon(p, r.x + r.w - 40, r.y + 18,
        X4_ICON_CHEVRON_RIGHT, X4_UI2_MUTED);
    if (focused) x4_ui2_focus(p, r.x, r.y, r.w, r.h, X4_UI2_RADIUS_CONTROL);
    if (acquired) x4_catalog_assets_release(model->assets, &art);
}

static void draw_focus_row(uint32_t *p, const X4Ui2AccountModel *model,
    unsigned row, bool focused)
{
    const X4UiRect r = row_rect(row);
    if (row < X4_UI2_ACCOUNT_HISTORY_FIRST) draw_action(p, model, r, row, focused);
    else draw_entry(p, model, r, row - X4_UI2_ACCOUNT_HISTORY_FIRST, focused);
}

static void draw_authorized(uint32_t *p, const X4Ui2AccountModel *model, unsigned focus)
{
    const int profile_y = 151;
    x4_ui2_card(p, LEFT, profile_y, RIGHT - LEFT, 178, X4_UI2_RADIUS_CARD);
    x4_apple_fill_round_rect(p, LEFT + 32, profile_y + 37, 104, 104, 52, X4_UI2_ACTIVE);
    const char *initial = model->initial;
    if (initial && initial[0]) {
        const int width = x4_text_width_px(X4_UI2_TITLE, X4_FONT_BOLD, initial);
        const int x = LEFT + 84 - width / 2;
        line(p, "account initial", x, profile_y + 63, width + 4,
            X4_UI2_TITLE, X4_FONT_BOLD, initial, X4_UI2_ACCENT);
    } else {
        x4_apple_draw_icon(p, LEFT + 72, profile_y + 77, X4_ICON_USER, X4_UI2_ACCENT);
    }
    const char *name = model->gamertag && model->gamertag[0] ?
        model->gamertag : x4_tr(X4_TR_U49_ACCOUNT_TITLE);
    line(p, "account name", LEFT + 170, profile_y + 40, 1160, X4_UI2_TITLE,
        X4_FONT_BOLD, name, X4_UI2_TEXT);
    x4_ui2_chip(p, LEFT + 170, profile_y + 108, 240, 38,
        x4_tr(X4_TR_U49_ACCOUNT_ACTIVE), true);

    const unsigned count = history_count(model);
    x4_ui2_card(p, HISTORY_X, HISTORY_Y, HISTORY_W, 502, X4_UI2_RADIUS_CARD);
    line(p, "account history title", ENTRY_X, HISTORY_Y + 23, ENTRY_W,
        X4_UI2_HEADING, X4_FONT_BOLD, x4_tr(X4_TR_U49_ACCOUNT_LOCAL), X4_UI2_TEXT);
    if (count) {
        char duration[64], summary[192];
        x4_cloud_time_duration(duration, sizeof(duration), model->history->total_seconds);
        const unsigned games = model->history->games;
        snprintf(summary, sizeof(summary), x4_tr_count(games,
            X4_TR_50_ACCOUNT_LOCAL_ONE_FMT, X4_TR_U49_ACCOUNT_LOCAL_FMT), games, duration);
        line(p, "account history summary", ENTRY_X, HISTORY_Y + 67, ENTRY_W,
            X4_UI2_BODY, X4_FONT_REGULAR, summary, X4_UI2_MUTED);
    } else {
        x4_ui2_text_bounded(p, ENTRY_X, HISTORY_Y + 82, X4_UI2_BODY, X4_FONT_REGULAR,
            x4_tr(X4_TR_U49_ACCOUNT_LOCAL_EMPTY), ENTRY_W, 34, 2, X4_UI2_MUTED);
    }
    for (unsigned row = 0; row < X4_UI2_ACCOUNT_HISTORY_FIRST + count; ++row)
        draw_focus_row(p, model, row, row == focus);

    if (cloud_visible(model->cloud_time)) {
        const int y = ACTION_Y + 3 * ACTION_PITCH;
        char value[120];
        x4_cloud_time_label(model->cloud_time, value, sizeof(value));
        x4_ui2_card(p, LEFT, y, ACTION_W, ACTION_H, X4_UI2_RADIUS_CONTROL);
        x4_apple_draw_icon(p, LEFT + 20, y + 22, X4_ICON_DEVICE_GAMEPAD, X4_UI2_MUTED);
        line(p, "account cloud label", LEFT + 56, y + 21, 270,
            X4_UI2_BODY, X4_FONT_SEMIBOLD, x4_tr(X4_TR_U49_ACCOUNT_CLOUD_TIME), X4_UI2_TEXT);
        const int end = LEFT + ACTION_W - 52;
        int value_x = end - x4_text_width_px(X4_UI2_BODY, X4_FONT_REGULAR, value);
        const int min_value_x = LEFT + 334;
        if (value_x < min_value_x) value_x = min_value_x;
        line(p, "account cloud value", value_x, y + 21, end - value_x,
            X4_UI2_BODY, X4_FONT_REGULAR, value, X4_UI2_MUTED);
    }
    const X4Ui2Hint hints[] = {
        {X4_PAD_CROSS, x4_tr(X4_TR_APPLE_HINT_SELECT)},
        {X4_PAD_R1, x4_tr(X4_TR_U49_ACCOUNT_CATALOG)},
#if X4_PUBLIC
        {X4_PAD_TRIANGLE, x4_tr(X4_TR_U49_ACCOUNT_SIGN_OUT)},
#else
        {X4_PAD_TRIANGLE, x4_tr(X4_TR_U49_ACCOUNT_FORGET)},
#endif
        {X4_PAD_OPTIONS, x4_tr(X4_TR_APPLE_HINT_SETTINGS)},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_APPLE_HINT_BACK)},
    };
    x4_ui2_footer(p, hints, sizeof(hints) / sizeof(*hints));
}

static void code_qr(uint32_t *p, const X4AuthSnapshot *auth, int x, int y, int edge)
{
    static uint8_t modules[qrcodegen_BUFFER_LEN_MAX], temp[qrcodegen_BUFFER_LEN_MAX];
    static char last_uri[256], last_code[32];
    static bool valid;
    if (strcmp(last_uri, auth->verification_uri) || strcmp(last_code, auth->user_code)) {
        memcpy(last_uri, auth->verification_uri, sizeof(last_uri));
        memcpy(last_code, auth->user_code, sizeof(last_code));
        char payload[512];
        valid = x4_auth_qr_payload(auth->verification_uri, auth->user_code, payload, sizeof(payload)) &&
            qrcodegen_encodeText(payload, temp, modules, qrcodegen_Ecc_MEDIUM,
                qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true);
    }
    if (!valid) return;
    const int modules_w = qrcodegen_getSize(modules);
    const int scale = edge / (modules_w + 8);
    const int inset = (edge - scale * modules_w) / 2;
    x4_apple_fill_round_rect(p, x, y, edge, edge, 14, X4_UI2_TEXT);
    for (int row = 0; row < modules_w; ++row) {
        for (int col = 0; col < modules_w; ++col)
            if (qrcodegen_getModule(modules, col, row))
                x4_rect(p, x + inset + col * scale, y + inset + row * scale,
                    scale, scale, X4_RGB(7, 11, 18));
    }
}

static void step_number(uint32_t *p, int x, int y, const char *number)
{
    x4_apple_fill_round_rect(p, x, y, 40, 40, 20, X4_UI2_ACTIVE);
    x4_text_px(p, x + 13, y + 7, X4_UI2_BODY, X4_FONT_BOLD, number, X4_UI2_ACCENT);
}

static void draw_code(uint32_t *p, const X4AuthSnapshot *auth)
{
    x4_ui2_card(p, LEFT, 156, RIGHT - LEFT, 708, X4_UI2_RADIUS_CARD);
    const int x = LEFT + 48;
    line(p, "account sign-in header", x, 212, 920, X4_UI2_HEADING,
        X4_FONT_BOLD, x4_tr(X4_TR_U49_ACCOUNT_SIGN_IN), X4_UI2_TEXT);
    step_number(p, x, 292, "1");
    x4_ui2_text_bounded(p, x + 60, 295, X4_UI2_LABEL, X4_FONT_SEMIBOLD,
        x4_tr(X4_TR_U49_ACCOUNT_STEP_SCAN), 830, 35, 2, X4_UI2_TEXT);
    line(p, "account Microsoft address", x + 60, 374, 830, X4_UI2_BODY,
        X4_FONT_SEMIBOLD, auth->verification_uri, X4_UI2_ACCENT);
    step_number(p, x, 449, "2");
    x4_ui2_text_bounded(p, x + 60, 452, X4_UI2_LABEL, X4_FONT_SEMIBOLD,
        x4_tr(X4_TR_U49_ACCOUNT_STEP_CODE), 830, 35, 2, X4_UI2_TEXT);

    x4_ui2_card(p, x + 60, 530, 790, 108, X4_UI2_RADIUS_CONTROL);
    int code_size = X4_UI2_TITLE;
    if (x4_text_width_px(code_size, X4_FONT_BOLD, auth->user_code) > 700)
        code_size = X4_UI2_HEADING;
    line(p, "account device code", x + 88, 583 - code_size / 2, 710,
        code_size, X4_FONT_BOLD, auth->user_code, X4_UI2_TEXT);
    char countdown[96];
    snprintf(countdown, sizeof(countdown), x4_tr(X4_TR_AUTH_EXPIRES_FMT),
        auth->seconds_left / 60, auth->seconds_left % 60);
    line(p, "account code countdown", x + 60, 669, 700, X4_UI2_BODY,
        X4_FONT_SEMIBOLD, countdown, X4_UI2_MUTED);
    line(p, "account awaiting authorization", x, 755, 880, X4_UI2_BODY,
        X4_FONT_SEMIBOLD, x4_tr(X4_TR_U49_ACCOUNT_WAIT), X4_UI2_TEXT);
    line(p, "account sign-in privacy", x, 796, 880, X4_UI2_CAPTION,
        X4_FONT_REGULAR, x4_tr(X4_TR_U49_ACCOUNT_PRIVACY), X4_UI2_MUTED);
    code_qr(p, auth, 1346, 304, 360);
    const X4Ui2Hint hints[] = {{X4_PAD_CIRCLE, x4_tr(X4_TR_AUTH_REMEMBER_WARNING_CANCEL)}};
    x4_ui2_footer(p, hints, 1);
}

static const char *status_title(enum X4AuthState state, bool closing)
{
    if (closing) return x4_tr(X4_TR_AUTH_CLOSING_TITLE);
    switch (state) {
    case X4_AUTH_CONNECTING: return x4_tr(X4_TR_AUTH_CONNECTING);
    case X4_AUTH_CONNECTED: return x4_tr(X4_TR_AUTH_CONNECTED);
    case X4_AUTH_CANCELLED: return x4_tr(X4_TR_AUTH_CANCELLED);
    case X4_AUTH_EXPIRED: return x4_tr(X4_TR_AUTH_EXPIRED);
    case X4_AUTH_DENIED: return x4_tr(X4_TR_AUTH_DENIED);
    case X4_AUTH_ERROR: return x4_tr(X4_TR_AUTH_ERROR);
    default: return x4_tr(X4_TR_AUTH_READY);
    }
}

static void draw_signed_out(uint32_t *p, const X4Ui2AccountModel *model)
{
    const X4AuthSnapshot *auth = model->auth;
    if (auth->state == X4_AUTH_WAITING && !model->closing) {
        draw_code(p, auth);
        return;
    }
    enum { INTRO_Y = 309, INTRO_W = 1340, BUTTON_H = 62 };
    const int x = LEFT + 48;
    const bool failed = auth->state == X4_AUTH_ERROR || auth->state == X4_AUTH_DENIED;
    const bool ready = auth->state == X4_AUTH_IDLE && !model->busy && !model->closing;
    const char *intro = x4_tr(model->closing ? X4_TR_AUTH_CLOSING_SUB : X4_TR_AUTH_PROMPT_PUBLIC);
    const char *next = ready ? x4_tr(X4_TR_U49_ACCOUNT_CODE_NEXT) : NULL;
    const int next_y = INTRO_Y + x4_ui2_modal_text_height(intro, INTRO_W, 2) + 12;
    const int button_y = ready ? next_y + x4_ui2_modal_text_height(next, INTRO_W, 2) + 32 : 662;
    x4_ui2_card(p, LEFT, 156, RIGHT - LEFT, ready ? button_y + BUTTON_H + 48 - 156 : 620,
        X4_UI2_RADIUS_CARD);
    x4_apple_fill_round_rect(p, x, 215, 56, 56, 28, failed ? X4_RGB(58, 41, 22) : X4_UI2_ACTIVE);
    x4_apple_draw_icon(p, x + 16, 231, failed ? X4_ICON_INFO : X4_ICON_USER,
        failed ? X4_RGB(255, 211, 151) : X4_UI2_ACCENT);
    line(p, "account sign-in state", x + 80, 220, 1320, X4_UI2_HEADING,
        X4_FONT_BOLD, status_title(auth->state, model->closing), X4_UI2_TEXT);
    x4_ui2_text_bounded(p, x, INTRO_Y, X4_UI2_LABEL, X4_FONT_REGULAR, intro,
        INTRO_W, 36, 2, X4_UI2_TEXT);
    if (ready) x4_ui2_modal_text(p, next, x, next_y, INTRO_W, 2);
    const char *stage = NULL;
    if (auth->stage[0])
        stage = !strcmp(auth->stage, "Xbox rechazo la autorizacion XSTS") ?
            x4_tr(X4_TR_U49_AUTH_XSTS) : x4_tr_stage(auth->stage);
    if (stage && stage[0]) x4_ui2_text_bounded(p, x, 400, X4_UI2_BODY, X4_FONT_REGULAR,
        stage, INTRO_W, 33, 3, X4_UI2_MUTED);
    if (failed) {
        char detail[80], message[256];
        const X4CatalogSnapshot *catalog = model->catalog;
        const char *hint = x4_net_hint(auth->error);
        if (catalog && catalog->xerr && catalog->state == X4_CATALOG_ERROR)
            hint = x4_lang_catalog_error_msg(catalog->error, catalog->http_status, catalog->xerr,
                catalog->offering, catalog->stage, message, sizeof(message));
        if (hint && hint[0]) x4_ui2_text_bounded(p, x, 520, X4_UI2_BODY, X4_FONT_REGULAR,
            hint, 1340, 33, 2, X4_UI2_MUTED);
        if (auth->error || auth->http_status >= 400) {
            snprintf(detail, sizeof(detail), "0x%08x · HTTP %d", (unsigned)auth->error, auth->http_status);
            line(p, "account sign-in technical detail", x, 603, 1020, X4_UI2_CAPTION,
                X4_FONT_REGULAR, detail, X4_UI2_MUTED);
        }
    }
    if (!model->busy && !model->closing)
        x4_ui2_button(p, x, button_y, 360, BUTTON_H, x4_tr(X4_TR_U49_ACCOUNT_SIGN_IN),
            true, model->focus_owned, X4_ICON_USER);
    const X4Ui2Hint hints[] = {
        {X4_PAD_CROSS, x4_tr(X4_TR_U49_ACCOUNT_SIGN_IN)},
#if !X4_PUBLIC
        {X4_PAD_SQUARE, x4_tr(X4_TR_U49_ACCOUNT_CHECK)},
#endif
        {X4_PAD_OPTIONS, x4_tr(X4_TR_APPLE_HINT_SETTINGS)},
        {X4_PAD_CIRCLE, x4_tr(model->busy || model->closing ?
            X4_TR_AUTH_REMEMBER_WARNING_CANCEL : X4_TR_APPLE_HINT_BACK)},
    };
    if (model->busy || model->closing)
        x4_ui2_footer(p, &hints[sizeof(hints) / sizeof(*hints) - 1], 1);
    else x4_ui2_footer(p, hints, sizeof(hints) / sizeof(*hints));
}

static uint64_t hash_number(uint64_t hash, uint64_t value)
{
    return (hash ^ value) * UINT64_C(1099511628211);
}

static uint64_t hash_text(uint64_t hash, const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; p && *p; ++p)
        hash = hash_number(hash, *p);
    return hash_number(hash, 0);
}

static uint64_t account_content(const X4Ui2AccountModel *model)
{
    const X4AuthSnapshot *auth = model->auth;
    uint64_t hash = hash_number(UINT64_C(14695981039346656037), x4_lang_current());
    hash = hash_number(hash, auth->state);
    hash = hash_number(hash, model->busy);
    hash = hash_number(hash, model->closing);
    if (auth->state != X4_AUTH_AUTHORIZED || model->closing) {
        hash = hash_number(hash, model->focus_owned);
        hash = hash_number(hash, (unsigned)auth->error);
        hash = hash_number(hash, (unsigned)auth->http_status);
        hash = hash_number(hash, auth->seconds_left);
        hash = hash_text(hash, auth->stage);
        hash = hash_text(hash, auth->user_code);
        hash = hash_text(hash, auth->verification_uri);
        return hash_number(hash, model->catalog ? model->catalog->xerr : 0);
    }
    hash = hash_text(hash, model->gamertag);
    hash = hash_text(hash, model->initial);
    hash = hash_text(hash, model->server);
    hash = hash_text(hash, model->country);
    hash = hash_number(hash, model->remember.available);
    hash = hash_number(hash, model->remember.enabled);
    hash = hash_number(hash, model->cloud_time.source);
    hash = hash_number(hash, model->cloud_time.unlimited);
    hash = hash_number(hash, model->cloud_time.remaining_s / 60);
    hash = hash_number(hash, model->cloud_time.max_s / 60);
    hash = hash_number(hash, cloud_visible(model->cloud_time));
    hash = hash_number(hash, model->assets_revision);
    const unsigned count = history_count(model);
    hash = hash_number(hash, count);
    if (count) {
        hash = hash_number(hash, model->history->games);
        hash = hash_number(hash, model->history->total_seconds);
        for (unsigned i = 0; i < count; ++i) {
            const X4HistoryEntry *entry = &model->history->most_played[i];
            hash = hash_text(hash, entry->product_id);
            hash = hash_text(hash, catalog_name(model->catalog, entry->product_id));
            hash = hash_number(hash, entry->played_s);
        }
    }
    return hash;
}

size_t x4_ui2_account_draw(const X4Ui2Account *account, const X4Ui2AccountModel *model,
    uint32_t *pixels, bool retained, X4UiRect damage[2])
{
    static struct {
        uint32_t *canvas;
        uint64_t content;
        unsigned focus;
    } previous;
    if (!account || !model || !model->auth || !pixels) return 0;
    const bool authorized = model->auth->state == X4_AUTH_AUTHORIZED && !model->closing;
    const uint64_t content = account_content(model);
    const unsigned focus = authorized && model->focus_owned ? account_row(account, model) : NO_FOCUS;
    if (retained && pixels == previous.canvas && content == previous.content) {
        if (focus == previous.focus) return 0;
        size_t count = 0;
        const unsigned affected[] = {previous.focus, focus};
        for (size_t i = 0; i < sizeof(affected) / sizeof(*affected); ++i) {
            const unsigned row = affected[i];
            if (row == NO_FOCUS) continue;
            const X4UiRect r = row_damage(row_rect(row));
            const uint32_t base = row < X4_UI2_ACCOUNT_HISTORY_FIRST ? X4_UI2_BACKGROUND : X4_UI2_SURFACE;
            x4_rect(pixels, r.x, r.y, r.w, r.h, base);
            draw_focus_row(pixels, model, row, row == focus);
            if (damage) damage[count] = r;
            ++count;
        }
        previous.focus = focus;
        return count;
    }
    x4_rect(pixels, 0, 0, W, H, X4_UI2_BACKGROUND);
    x4_ui2_heading(pixels, LEFT, 56, x4_tr(X4_TR_U49_ACCOUNT_TITLE));
    if (authorized) draw_authorized(pixels, model, focus);
    else draw_signed_out(pixels, model);
    previous.canvas = pixels;
    previous.content = content;
    previous.focus = focus;
    return SIZE_MAX;
}

void x4_ui2_account_remember_modal(uint32_t *p)
{
    const X4Ui2ModalAction actions[] = {
        {X4_PAD_CROSS, x4_tr(X4_TR_AUTH_REMEMBER_WARNING_ACCEPT), true, true, false},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_AUTH_REMEMBER_WARNING_CANCEL), false, false, false},
    };
    x4_ui2_modal_message(p, x4_tr(X4_TR_AUTH_REMEMBER_WARNING_TITLE),
        x4_tr(X4_TR_AUTH_REMEMBER_WARNING_BODY), NULL, X4_UI2_MUTED,
        actions, sizeof(actions) / sizeof(*actions));
}

void x4_ui2_account_logout_modal(bool remembered, bool confirm_focused, uint32_t *p)
{
    const X4Ui2ModalAction actions[] = {
        {X4_PAD_CROSS, x4_tr(X4_TR_U49_ACCOUNT_SIGN_OUT), false, confirm_focused, false},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_AUTH_REMEMBER_WARNING_CANCEL), false, !confirm_focused, true},
    };
    x4_ui2_modal_message(p, x4_tr(X4_TR_U49_ACCOUNT_SIGN_OUT),
        x4_tr(X4_TR_U49_ACCOUNT_LOGOUT_BODY),
        remembered ? x4_tr(X4_TR_U49_ACCOUNT_LOGOUT_REMEMBER) : NULL, X4_UI2_MUTED,
        actions, sizeof(actions) / sizeof(*actions));
}
