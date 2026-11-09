/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_modal.h"
#include "font.h"
#include "canvas.h"
#include "../core/app_identity.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

enum {
    PANEL_RADIUS = 26, PANEL_MIN_H = 300, PANEL_MAX_H = 720, BODY_TOP = 112,
    BODY_PITCH = 36, ACTION_GAP = 24, BUTTON_H = 60,
};
#define BODY_W X4_UI2_MODAL_BODY_W

/* Keep only the dimmed area under the panel so a resize need not redraw
 * the page underneath. The original page is redrawn by the owner on close. */
static uint32_t panel_backdrop[X4_UI2_MODAL_W * PANEL_MAX_H];
static uint32_t *panel_canvas;
static X4UiRect panel_rect, panel_damage;
static char panel_title[256];

void x4_ui2_modal_scrim(uint32_t *p)
{
    if (!p) return;
    panel_canvas = NULL;
    panel_rect = (X4UiRect){0};
    panel_damage = (X4UiRect){0};
    panel_title[0] = 0;
    for (size_t i = 0; i < (size_t)X4_UI_WIDTH * X4_UI_HEIGHT; ++i)
        p[i] = 0xff000000u | ((p[i] & 0x00fcfcfcu) >> 2);
}

static bool horizontal_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static const char *skip_horizontal_space(const char *text)
{
    while (horizontal_space(*text)) ++text;
    return text;
}

static const char *next_word_end(const char *word)
{
    while (*word && *word != '\n' && !horizontal_space(*word)) ++word;
    return word;
}

static const char *previous_word_end(const char *start, const char *end)
{
    while (end > start && !horizontal_space(end[-1])) --end;
    while (end > start && horizontal_space(end[-1])) --end;
    return end;
}

static void draw_checked_line(uint32_t *p, int x, int y, int size, int weight,
    const char *line, uint32_t color)
{
    if (!p || !*line) return;
#ifdef X4_UI46_CAPTURE
    x4_ui46_capture_contrast(p, "Texto de interfaz", x, y,
        x4_text_width_px(size, weight, line), size, color);
#endif
    x4_text_px(p, x, y, size, weight, line, color);
}

int x4_ui2_text_bounded(uint32_t *p, int x, int y, int size, int weight,
    const char *text, int width, int line_height, int max_lines, uint32_t color)
{
    if (!text || width <= 0 || max_lines <= 0 || line_height <= 0) return 0;
    const char ellipsis[] = "…";
    int lines = 0;
    for (; lines < max_lines; ++lines) {
        const char *start = skip_horizontal_space(text);
        if (!*start) break;
        if (*start == '\n') {
            text = start + 1;
            continue;
        }
        char line[1024];
        const char *fit = start, *scan = start;
        while (*scan && *scan != '\n') {
            const char *end = next_word_end(scan);
            const size_t bytes = (size_t)(end - start);
            if (bytes >= sizeof(line)) break;
            memcpy(line, start, bytes);
            line[bytes] = 0;
            if (x4_text_width_px(size, weight, line) > width) break;
            fit = end;
            scan = skip_horizontal_space(end);
        }
        const bool omitted_word = fit == start;
        text = skip_horizontal_space(omitted_word ? next_word_end(start) : fit);
        if (*text == '\n') ++text;
        const bool truncate = omitted_word ||
            (lines + 1 == max_lines && *skip_horizontal_space(text));
        if (truncate) {
            for (;;) {
                size_t bytes = (size_t)(fit - start);
                if (bytes <= sizeof(line) - sizeof(ellipsis)) {
                    memcpy(line, start, bytes);
                    memcpy(line + bytes, ellipsis, sizeof(ellipsis));
                    if (x4_text_width_px(size, weight, line) <= width) {
                        draw_checked_line(p, x, y + lines * line_height, size, weight, line, color);
                        break;
                    }
                }
                if (fit == start) break;
                fit = previous_word_end(start, fit);
            }
        } else {
            size_t bytes = (size_t)(fit - start);
            memcpy(line, start, bytes);
            line[bytes] = 0;
            draw_checked_line(p, x, y + lines * line_height, size, weight, line, color);
        }
    }
    return lines;
}

int x4_ui2_modal_text(uint32_t *p, const char *text, int x, int y, int width, int lines)
{
    return x4_ui2_text_bounded(p, x, y, X4_UI2_LABEL, X4_FONT_REGULAR, text,
        width, BODY_PITCH, lines, X4_UI2_MUTED);
}

static int measured_height(const char *text, int size, int weight, int width,
    int pitch, int max_lines)
{
    const int lines = x4_ui2_text_bounded(NULL, 0, 0, size, weight, text,
        width, pitch, max_lines, X4_UI2_MUTED);
    return lines ? (lines - 1) * pitch + size : 0;
}

int x4_ui2_modal_text_height(const char *text, int width, int lines)
{
    return measured_height(text, X4_UI2_LABEL, X4_FONT_REGULAR, width, BODY_PITCH, lines);
}

static void panel_backdrop_copy(uint32_t *p, X4UiRect rect, bool restore)
{
    for (int y = 0; y < rect.h; ++y) {
        uint32_t *screen_row = p + (size_t)(rect.y + y) * X4_UI_WIDTH + rect.x;
        uint32_t *backdrop_row = panel_backdrop + (size_t)y * X4_UI2_MODAL_W;
        memcpy(restore ? screen_row : backdrop_row, restore ? backdrop_row : screen_row,
            (size_t)rect.w * sizeof(*screen_row));
    }
}

X4UiRect x4_ui2_modal_damage(void)
{
    return panel_damage;
}

X4UiRect x4_ui2_modal_panel(uint32_t *p, const char *title, int body_height)
{
    if (!p) return (X4UiRect){0};
    const int max_body = PANEL_MAX_H - BODY_TOP - ACTION_GAP - BUTTON_H - X4_UI2_MODAL_MARGIN;
    if (body_height < 0) body_height = 0;
    if (body_height > max_body) body_height = max_body;
    int h = BODY_TOP + body_height + ACTION_GAP + BUTTON_H + X4_UI2_MODAL_MARGIN;
    if (h < PANEL_MIN_H) h = PANEL_MIN_H;
    const X4UiRect r = {
        (X4_UI_WIDTH - X4_UI2_MODAL_W) / 2, (X4_UI_HEIGHT - h) / 2, X4_UI2_MODAL_W, h,
    };
    const bool same_canvas = panel_canvas == p;
    const bool new_panel = !same_canvas || panel_rect.h != h;
    panel_damage = r;
    if (new_panel) {
        if (same_canvas) {
            panel_backdrop_copy(p, panel_rect, true);
            /* The panels are centered and share the same width. */
            if (panel_rect.h > h) panel_damage = panel_rect;
        }
        panel_backdrop_copy(p, r, false);
        x4_ui2_card(p, r.x, r.y, r.w, r.h, PANEL_RADIUS);
        panel_canvas = p;
        panel_rect = r;
    } else {
        /* Clear the content and focus glow without changing the shell. */
        x4_rect(p, r.x + 24, r.y + BODY_TOP - 12, r.w - 48,
            r.h - BODY_TOP - 10, X4_UI2_SURFACE);
    }
    if (!title) title = "";
    if (new_panel || strcmp(panel_title, title)) {
        if (!new_panel)
            x4_rect(p, r.x + 24, r.y + 24, r.w - 48, BODY_TOP - 36, X4_UI2_SURFACE);
        x4_ui2_text_bounded(p, r.x + X4_UI2_MODAL_MARGIN, r.y + X4_UI2_MODAL_MARGIN,
            X4_UI2_TITLE, X4_FONT_BOLD, title, BODY_W, 60, 1, X4_UI2_TEXT);
        x4_apple_fill_round_rect(p, r.x + X4_UI2_MODAL_MARGIN, r.y + 87,
            72, 3, 1, X4_UI2_ACCENT);
        snprintf(panel_title, sizeof(panel_title), "%s", title);
    }
    return (X4UiRect){r.x + X4_UI2_MODAL_MARGIN, r.y + BODY_TOP, BODY_W, body_height};
}

void x4_ui2_modal_actions(uint32_t *p, const X4Ui2ModalAction *actions, size_t count)
{
    enum { MIN_BUTTON_W = 312, GAP = 16, INSET = 18, GLYPH_GAP = 6, TEXT_GAP = 14 };
    if (!p || p != panel_canvas || !actions || !count || count > 3) return;
    struct { int button, cross, shortcut, glyph; } widths[3] = {0};
    const int gap_w = (int)(count - 1) * GAP;
    const int max_button_w = (BODY_W - gap_w) / (int)count;
    int group_w = gap_w;
    for (size_t i = 0; i < count; ++i) {
        const X4Ui2ModalAction action = actions[i];
        if (action.glyph == X4_PAD_CROSS || action.cross_on_focus)
            widths[i].cross = x4_ui2_pad_glyph(NULL, 0, 0, X4_PAD_CROSS);
        if (action.glyph != X4_PAD_CROSS)
            widths[i].shortcut = x4_ui2_pad_glyph(NULL, 0, 0, action.glyph);
        widths[i].glyph = widths[i].cross + widths[i].shortcut +
            (widths[i].cross && widths[i].shortcut ? GLYPH_GAP : 0);
        int w = 2 * INSET + (widths[i].glyph ? widths[i].glyph + TEXT_GAP : 0) +
            x4_text_width_px(X4_UI2_BODY, X4_FONT_BOLD, action.label);
        if (w < MIN_BUTTON_W) w = MIN_BUTTON_W;
        if (w > max_button_w) w = max_button_w;
        widths[i].button = w;
        group_w += w;
    }

    int x = panel_rect.x + panel_rect.w - X4_UI2_MODAL_MARGIN - group_w;
    const int y = panel_rect.y + panel_rect.h - X4_UI2_MODAL_MARGIN - BUTTON_H;
    for (size_t i = 0; i < count; ++i) {
        const X4Ui2ModalAction action = actions[i];
        const int w = widths[i].button, glyph_w = widths[i].glyph;
        if (action.primary)
            x4_apple_fill_round_rect(p, x, y, w, BUTTON_H, BUTTON_H / 2, X4_UI2_ACCENT);
        else x4_ui2_card(p, x, y, w, BUTTON_H, BUTTON_H / 2);
        if (action.focused) x4_ui2_focus(p, x, y, w, BUTTON_H, BUTTON_H / 2);
        const bool cross = action.focused && widths[i].cross;
        if (action.primary && (cross || widths[i].shortcut))
            x4_apple_fill_round_rect(p, x + INSET - 4, y + 11, glyph_w + 10, 38, 19, X4_UI2_INK);
        if (cross)
            (void)x4_ui2_pad_glyph(p, x + INSET, y + 16, X4_PAD_CROSS);
        if (widths[i].shortcut)
            (void)x4_ui2_pad_glyph(p, x + INSET + glyph_w - widths[i].shortcut, y + 16, action.glyph);
        const int text_inset = glyph_w ? INSET + glyph_w + TEXT_GAP : INSET;
        x4_ui2_text_bounded(p, x + text_inset, y + 18, X4_UI2_BODY, X4_FONT_BOLD,
            action.label, w - text_inset - INSET, 28, 1,
            action.primary ? X4_UI2_INK : X4_UI2_TEXT);
        x += w + GAP;
    }
}

void x4_ui2_modal_message(uint32_t *p, const char *title, const char *body,
    const char *second, uint32_t second_color, const X4Ui2ModalAction *actions, size_t count)
{
    enum { PARAGRAPH_GAP = 12, BODY_LINES = 8, SECOND_LINES = 3 };
    const int body_height = x4_ui2_modal_text_height(body, BODY_W, BODY_LINES);
    const int second_height = x4_ui2_modal_text_height(second, BODY_W, SECOND_LINES);
    const int second_offset = body_height + (body_height && second_height ? PARAGRAPH_GAP : 0);
    const X4UiRect r = x4_ui2_modal_panel(p, title, second_offset + second_height);
    x4_ui2_modal_text(p, body, r.x, r.y, r.w, BODY_LINES);
    x4_ui2_text_bounded(p, r.x, r.y + second_offset, X4_UI2_LABEL, X4_FONT_REGULAR,
        second, r.w, BODY_PITCH, SECOND_LINES, second_color);
    x4_ui2_modal_actions(p, actions, count);
}

void x4_ui2_update_notice_draw(const X4UpdateNotice *notice, uint32_t *p)
{
    if (!notice || !notice->open || !p) return;
    const X4Ui2ModalAction actions[] = {
        {X4_PAD_CIRCLE, x4_tr(X4_TR_51_UPDATE_LATER), false, !notice->install_focused, true},
        {X4_PAD_CROSS, x4_tr(X4_PUBLIC ? X4_TR_51_UPDATE_NOW : X4_TR_51F_PREPARE),
            notice->can_install, notice->install_focused, false},
    };
    if (notice->local) {
        x4_ui2_modal_message(p, x4_tr(X4_TR_51_UPDATE_LOCAL), x4_tr(X4_TR_51_UPDATE_LOCAL_CONFIRM),
            NULL, X4_UI2_MUTED, actions, 2);
        return;
    }
    enum { LEFT_W = 638, QR_EDGE = 280, GAP = 12 };
    char version[256];
    snprintf(version, sizeof(version), x4_tr(X4_TR_UPDATE_NOTICE_VERSION_FMT),
        notice->version, X4_PRODUCT_VERSION);
    const char *description = x4_tr(notice->can_install ? X4_TR_51_UPDATE_QR_HINT : X4_TR_51_UPDATE_NO_ASSETS);
    const int heading_h = measured_height(version, X4_UI2_HEADING, X4_FONT_SEMIBOLD,
        LEFT_W, 42, 3);
    const int description_h = x4_ui2_modal_text_height(description, LEFT_W, 5);
    const int link_offset = heading_h + description_h + 2 * GAP;
    const int text_h = link_offset + X4_UI2_CAPTION;
    const X4UiRect r = x4_ui2_modal_panel(p, x4_tr(X4_TR_UPDATE_NOTICE_TITLE),
        text_h > QR_EDGE ? text_h : QR_EDGE);
    x4_ui2_text_bounded(p, r.x, r.y, X4_UI2_HEADING, X4_FONT_SEMIBOLD,
        version, LEFT_W, 42, 3, X4_UI2_TEXT);
    x4_ui2_modal_text(p, description, r.x, r.y + heading_h + GAP, LEFT_W, 5);
    x4_ui2_text_bounded(p, r.x, r.y + link_offset, X4_UI2_CAPTION, X4_FONT_SEMIBOLD,
        x4_tr(X4_TR_UI2_MODAL_UPDATE_LINK), LEFT_W, 28, 1, X4_UI2_ACCENT);
    (void)x4_apple_draw_public_qr(p, r.x + r.w - QR_EDGE / 2, r.y + QR_EDGE / 2,
        QR_EDGE, notice->url);
    x4_ui2_modal_actions(p, actions, 2);
}

static enum X4TextId update_error_text(X4Update51Error error)
{
    switch (error) {
    case X4_UPDATE51_BAD_SIGNATURE: return X4_TR_51_UPDATE_ERR_SIGNATURE;
    case X4_UPDATE51_BAD_HASH: return X4_TR_51_UPDATE_ERR_HASH;
    case X4_UPDATE51_NO_SPACE: return X4_TR_51_UPDATE_ERR_SPACE;
    case X4_UPDATE51_OLD_VERSION: return X4_TR_51_UPDATE_ERR_VERSION_FMT;
    case X4_UPDATE51_READ: return X4_TR_51C_UPDATE_ERR_READ_FMT;
    case X4_UPDATE51_BAD_MANIFEST: return X4_TR_51C_UPDATE_ERR_MANIFEST;
    case X4_UPDATE51_BAD_PACKAGE: return X4_TR_51C_UPDATE_ERR_PACKAGE;
    case X4_UPDATE51_BUSY: return X4_TR_51C_UPDATE_ERR_BUSY;
    case X4_UPDATE51_NETWORK: return X4_TR_51C_UPDATE_ERR_NETWORK;
    case X4_UPDATE51_INSTALL_ERROR: return X4_TR_51C_UPDATE_ERR_INSTALL;
    case X4_UPDATE51_START_ERROR:
    case X4_UPDATE51_GENERAL:
    default: return X4_TR_51C_UPDATE_ERR_START;
    }
}

static void update_mb(char *out, size_t cap, uint32_t bytes)
{
    const unsigned tenths = bytes / 100000u;
    const char separator = x4_lang_current() == X4_LANG_EN ? '.' : ',';
    (void)snprintf(out, cap, "%u%c%u", tenths / 10u, separator, tenths % 10u);
}

/* A signed PKG name is a bounded ASCII basename. Wrap it without eliding
 * bytes so the exact name stays visible even for the longest valid name. */
static int draw_update_pkg_name(uint32_t *p, int x, int y, const char *name)
{
    const size_t length = strlen(name);
    int height = 0;
    for (size_t offset = 0; offset < length;) {
        char line[129];
        size_t count = length - offset;
        if (count >= sizeof(line)) count = sizeof(line) - 1;
        memcpy(line, name + offset, count);
        line[count] = '\0';
        while (count > 1 && x4_text_width_px(X4_UI2_CAPTION, X4_FONT_SEMIBOLD, line) > BODY_W)
            line[--count] = '\0';
        draw_checked_line(p, x, y + height, X4_UI2_CAPTION, X4_FONT_SEMIBOLD, line, X4_UI2_ACCENT);
        height += 28;
        offset += count;
    }
    return height;
}

void x4_ui2_update_manual_draw(uint32_t *p, const char *version, const char *pkg_name, int32_t code)
{
    if (!p || !version || !pkg_name) return;
    const X4Ui2ModalAction close = {X4_PAD_CIRCLE, x4_tr(X4_TR_UPDATE_NOTICE_CLOSE), false, true, true};
    const char *title = x4_tr(X4_TR_51F_MANUAL_TITLE);
    if (code && code != -EEXIST) {
        char technical[16];
        (void)snprintf(technical, sizeof(technical), "0x%08x", (unsigned)code);
        x4_ui2_modal_message(p, title, x4_tr(X4_TR_51F_MANUAL_ERROR), technical,
            X4_UI2_MUTED, &close, 1);
        return;
    }

    enum { GAP = 12, TEXT_LINES = 4 };
    char instructions[512];
    const char *body = x4_tr(X4_TR_51F_MANUAL_CONFLICT);
    const char *after = code ? NULL : x4_tr(X4_TR_51F_MANUAL_REOPEN);
    if (!code) {
        char verified[192];
        (void)snprintf(verified, sizeof(verified), x4_tr(X4_TR_51F_MANUAL_VERIFIED_FMT), version);
        (void)snprintf(instructions, sizeof(instructions), "%s\n%s", verified, x4_tr(X4_TR_51F_MANUAL_STEPS));
        body = instructions;
    }
    const int file_y = x4_ui2_modal_text_height(body, BODY_W, TEXT_LINES) + GAP;
    const int after_y = file_y + draw_update_pkg_name(NULL, 0, 0, pkg_name) + GAP;
    const int after_h = x4_ui2_modal_text_height(after, BODY_W, 2);
    const X4UiRect r = x4_ui2_modal_panel(p, title, after_h ? after_y + after_h : after_y - GAP);
    x4_ui2_modal_text(p, body, r.x, r.y, r.w, TEXT_LINES);
    (void)draw_update_pkg_name(p, r.x, r.y + file_y, pkg_name);
    if (after_h) x4_ui2_modal_text(p, after, r.x, r.y + after_y, r.w, 2);
    x4_ui2_modal_actions(p, &close, 1);
}

void x4_ui2_update_applied_draw(uint32_t *p, const char *version, const char *pkg_name)
{
    if (!p || !version) return;
    const X4Ui2ModalAction close = {X4_PAD_CIRCLE, x4_tr(X4_TR_UPDATE_NOTICE_CLOSE), false, true, true};
    const char *title = x4_tr(X4_TR_51F_UPDATED_TITLE);
    char body[128];
    (void)snprintf(body, sizeof(body), x4_tr(X4_TR_51F_UPDATED_FMT), version);
    if (!pkg_name || !*pkg_name) {
        x4_ui2_modal_message(p, title, body, NULL, X4_UI2_MUTED, &close, 1);
        return;
    }

    enum { GAP = 12, LINES = 3 };
    const char *cleanup = x4_tr(X4_TR_51G_REMOVE_PKG);
    const int cleanup_y = x4_ui2_modal_text_height(body, BODY_W, LINES) + GAP;
    const int file_y = cleanup_y + x4_ui2_modal_text_height(cleanup, BODY_W, LINES) + GAP;
    const int file_h = draw_update_pkg_name(NULL, 0, 0, pkg_name);
    const X4UiRect r = x4_ui2_modal_panel(p, title, file_y + file_h);
    x4_ui2_modal_text(p, body, r.x, r.y, r.w, LINES);
    x4_ui2_modal_text(p, cleanup, r.x, r.y + cleanup_y, r.w, LINES);
    (void)draw_update_pkg_name(p, r.x, r.y + file_y, pkg_name);
    x4_ui2_modal_actions(p, &close, 1);
}

void x4_ui2_update_job_draw(const X4Update51View *job, uint32_t *p)
{
    if (!p || !job) return;
    const X4Ui2ModalAction cancel = {X4_PAD_CIRCLE, x4_tr(X4_TR_ACCESS_CANCEL), false, true, false};
    const X4Ui2ModalAction close = {X4_PAD_CIRCLE, x4_tr(X4_TR_UPDATE_NOTICE_CLOSE), false, true, true};
#if !X4_PUBLIC
    const X4Ui2ModalAction exit = {X4_PAD_CROSS, x4_tr(X4_TR_51F_CLOSE_NOW), true, true, false};
#endif
    const char *title = x4_tr(X4_TR_51_UPDATE_NOW);
    if (job->stage == X4_UPDATE51_DOWNLOADING) {
        char got[16], total[16], label[64];
        update_mb(got, sizeof(got), job->received);
        update_mb(total, sizeof(total), job->total);
        (void)snprintf(label, sizeof(label), x4_tr(X4_TR_51_UPDATE_MB_FMT), got, total);
        const X4UiRect rect = x4_ui2_modal_panel(p, x4_tr(X4_TR_51_UPDATE_DOWNLOAD), 86);
        x4_ui2_modal_text(p, label, rect.x, rect.y, rect.w, 1);
        x4_apple_fill_round_rect(p, rect.x, rect.y + 54, rect.w, 14, 7, X4_UI2_BORDER);
        const uint32_t received = job->received < job->total ? job->received : job->total;
        const int fill = job->total ? (int)((uint64_t)received * (unsigned)rect.w / job->total) : 0;
        if (fill) x4_apple_fill_round_rect(p, rect.x, rect.y + 54, fill, 14, 7, X4_UI2_ACCENT);
        x4_ui2_modal_actions(p, &cancel, 1);
        return;
    }
    char code[16];
    (void)snprintf(code, sizeof(code), "0x%08x", (unsigned)job->code);
    switch (job->stage) {
    case X4_UPDATE51_FETCHING:
        x4_ui2_modal_message(p, title, x4_tr(X4_TR_51_UPDATE_MANIFEST), NULL, X4_UI2_MUTED, &cancel, 1);
        break;
    case X4_UPDATE51_VERIFYING:
        x4_ui2_modal_message(p, title, x4_tr(X4_TR_51_UPDATE_VERIFY), NULL, X4_UI2_MUTED, &cancel, 1);
        break;
    case X4_UPDATE51_INSTALLING:
#if X4_PUBLIC
        x4_ui2_modal_message(p, x4_tr(X4_TR_51F_MANUAL_TITLE), x4_tr(X4_TR_51G_PREPARE_MANUAL),
            NULL, X4_UI2_MUTED, NULL, 0);
#else
        x4_ui2_modal_message(p, x4_tr(X4_TR_51B_INSTALL_TITLE), x4_tr(X4_TR_51_UPDATE_INSTALL),
            NULL, X4_UI2_MUTED, NULL, 0);
#endif
        break;
#if !X4_PUBLIC
    case X4_UPDATE51_ACCEPTED: {
        char body[256];
        (void)snprintf(body, sizeof(body), x4_tr(X4_TR_51F_CLOSE_IN_FMT), job->close_seconds);
        x4_ui2_modal_message(p, x4_tr(X4_TR_51B_INSTALL_TITLE), body,
            NULL, X4_UI2_MUTED, &exit, 1);
        break;
    }
    case X4_UPDATE51_CHOOSE: {
        const X4Ui2ModalAction actions[] = {
            {X4_PAD_CIRCLE, x4_tr(X4_TR_ACCESS_CANCEL), false, false, false},
            {X4_PAD_CROSS, x4_tr(X4_TR_51F_TRY_BGFT),
                !job->manual_selected, !job->manual_selected, false},
            {X4_PAD_CROSS, x4_tr(X4_TR_51F_PICK_MANUAL), job->manual_selected, job->manual_selected, false},
        };
        x4_ui2_modal_message(p, x4_tr(X4_TR_51F_PREPARE), x4_tr(job->resuming ? X4_TR_51F_RESUME : X4_TR_51F_CHOOSE),
            x4_tr(X4_TR_51G_EXPERIMENTAL), X4_UI2_MUTED, actions, sizeof(actions) / sizeof(actions[0]));
        break;
    }
#endif
#ifdef X4_UPDATE51_ENABLED
    case X4_UPDATE51_MANUAL: {
        X4Update51ManualView manual;
        if (x4_update51_manual_view(&manual))
            x4_ui2_update_manual_draw(p, manual.version, manual.pkg_name, manual.code);
        else
            x4_ui2_update_manual_draw(p, "", "", -ENOENT);
        break;
    }
#endif
    case X4_UPDATE51_UPDATED: {
#ifdef X4_UPDATE51_ENABLED
        X4Update51ManualView applied;
        if (x4_update51_applied_view(&applied))
            x4_ui2_update_applied_draw(p, applied.version, applied.pkg_name);
        else
#endif
            x4_ui2_update_applied_draw(p, X4_PRODUCT_VERSION, NULL);
        break;
    }
    case X4_UPDATE51_CANCELLED:
        x4_ui2_modal_message(p, title, x4_tr(X4_TR_51_UPDATE_CANCELLED), NULL, X4_UI2_MUTED, &close, 1);
        break;
    case X4_UPDATE51_FAILED: {
        const enum X4TextId id = update_error_text(job->error);
        const char *body = x4_tr(id);
        const char *technical_code = code;
        char formatted[160];
        if (id == X4_TR_51_UPDATE_ERR_VERSION_FMT) {
            (void)snprintf(formatted, sizeof(formatted), body, X4_PRODUCT_VERSION);
            body = formatted;
        } else if (id == X4_TR_51C_UPDATE_ERR_READ_FMT) {
            (void)snprintf(formatted, sizeof(formatted), body, code);
            body = formatted;
            technical_code = NULL;
        }
        x4_ui2_modal_message(p, title, body, technical_code, X4_UI2_MUTED, &close, 1);
        break;
    }
    default: break;
    }
}

void x4_ui2_cloud_confirm_draw(uint32_t *p)
{
    const X4Ui2ModalAction actions[] = {
        {X4_PAD_CROSS, x4_tr(X4_TR_CLOUD_TIME_TRY), true, true, false},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_CANCEL_HINT), false, false, false},
    };
    x4_ui2_modal_message(p, x4_tr(X4_TR_UI2_MODAL_CLOUD_TITLE), x4_tr(X4_TR_CLOUD_TIME_EMPTY),
        NULL, X4_UI2_MUTED, actions, sizeof(actions) / sizeof(actions[0]));
}

void x4_ui2_exit_error_draw(int error, uint32_t *p)
{
    if (!error) return;
    char code[96];
    snprintf(code, sizeof(code), x4_tr(X4_TR_UI2_MODAL_EXIT_CODE_FMT), (unsigned)error);
    const X4Ui2ModalAction close = {X4_PAD_CIRCLE, x4_tr(X4_TR_UPDATE_NOTICE_CLOSE), false, true, false};
    x4_ui2_modal_message(p, x4_tr(X4_TR_UI2_MODAL_EXIT_TITLE), x4_tr(X4_TR_UI2_MODAL_EXIT_BODY),
        code, X4_UI2_TEXT, &close, 1);
}

static void access_trying(uint32_t *p, const X4AccessCountryUi *ui)
{
    enum { SPINNER_W = 54, ROW_MIN_H = 36, GAP = 12 };
    char message[192];
    snprintf(message, sizeof(message), x4_tr(X4_TR_ACCESS_TRYING_FMT),
        x4_access_country_label(ui->selected));
    int row_h = x4_ui2_modal_text_height(message, BODY_W - SPINNER_W, 3);
    if (row_h < ROW_MIN_H) row_h = ROW_MIN_H;
    const char *warning = ui->selected != X4_ACCESS_AUTO ? x4_tr(X4_TR_ACCESS_WARNING) : NULL;
    const int warning_h = x4_ui2_modal_text_height(warning, BODY_W, 4);
    const X4UiRect r = x4_ui2_modal_panel(p, x4_tr(X4_TR_ACCESS_TITLE),
        row_h + (warning_h ? GAP + warning_h : 0));
    x4_apple_spinner(p, r.x, r.y, x4_apple_get_animation_phase());
    x4_ui2_modal_text(p, message, r.x + SPINNER_W, r.y, r.w - SPINNER_W, 3);
    x4_ui2_text_bounded(p, r.x, r.y + row_h + GAP, X4_UI2_LABEL, X4_FONT_REGULAR,
        warning, r.w, BODY_PITCH, 4, X4_RGB(255, 211, 151));
    const X4Ui2ModalAction cancel = {X4_PAD_CIRCLE, x4_tr(X4_TR_ACCESS_CANCEL), false, true, false};
    x4_ui2_modal_actions(p, &cancel, 1);
}

static void access_help(uint32_t *p, const X4AccessCountryUi *ui)
{
    enum { QR_EDGE = 252, QR_COLUMN = 292, GAP = 12 };
    const enum X4Lang lang = x4_lang_current();
    const char *caption = x4_region_qr_caption(lang);
    const char *link = x4_region_qr_short_label(lang);
    const int link_offset = x4_ui2_modal_text_height(caption, BODY_W - QR_COLUMN, 5) + GAP;
    const int link_h = measured_height(link, X4_UI2_CAPTION, X4_FONT_SEMIBOLD,
        BODY_W - QR_COLUMN, 28, 2);
    int row_h = link_offset + link_h;
    if (row_h < QR_EDGE) row_h = QR_EDGE;
    const char *warning = ui->selected != X4_ACCESS_AUTO ? x4_tr(X4_TR_ACCESS_WARNING) : NULL;
    const int warning_h = x4_ui2_modal_text_height(warning, BODY_W, 4);
    const X4UiRect r = x4_ui2_modal_panel(p, x4_tr(X4_TR_ACCESS_HELP),
        row_h + (warning_h ? GAP + warning_h : 0));
    (void)x4_apple_draw_public_qr(p, r.x + QR_EDGE / 2, r.y + QR_EDGE / 2,
        QR_EDGE, x4_region_qr_url(lang));
    x4_ui2_modal_text(p, caption, r.x + QR_COLUMN, r.y, r.w - QR_COLUMN, 5);
    x4_ui2_text_bounded(p, r.x + QR_COLUMN, r.y + link_offset, X4_UI2_CAPTION,
        X4_FONT_SEMIBOLD, link, r.w - QR_COLUMN, 28, 2, X4_UI2_ACCENT);
    x4_ui2_text_bounded(p, r.x, r.y + row_h + GAP, X4_UI2_LABEL, X4_FONT_REGULAR,
        warning, r.w, BODY_PITCH, 4, X4_RGB(255, 211, 151));
    const X4Ui2ModalAction close = {X4_PAD_CIRCLE, x4_tr(X4_TR_ACCESS_BACK), false, true, false};
    x4_ui2_modal_actions(p, &close, 1);
}

static void access_selector(uint32_t *p, const X4AccessCountryUi *ui, const char *save_error)
{
    enum { SELECT_H = 60, GAP = 12 };
    const char *description = x4_tr(X4_TR_ACCESS_DESCRIPTION);
    const char *step = x4_tr(X4_TR_UI2_MODAL_ACCESS_STEP);
    const char *warning = ui->selected != X4_ACCESS_AUTO ? x4_tr(X4_TR_ACCESS_WARNING) : NULL;
    const int label_y = x4_ui2_modal_text_height(description, BODY_W, 3) + GAP;
    const int select_y = label_y + X4_UI2_CAPTION + GAP;
    const int step_y = select_y + SELECT_H + GAP;
    const int step_h = measured_height(step, X4_UI2_CAPTION, X4_FONT_REGULAR, BODY_W, 28, 2);
    const int warning_y = step_y + step_h + (warning ? GAP : 0);
    const int error_y = warning_y + x4_ui2_modal_text_height(warning, BODY_W, 3) + (save_error ? GAP : 0);
    const X4UiRect r = x4_ui2_modal_panel(p, x4_tr(X4_TR_ACCESS_TITLE),
        error_y + x4_ui2_modal_text_height(save_error, BODY_W, 2));

    x4_ui2_modal_text(p, description, r.x, r.y, r.w, 3);
    x4_ui2_text_bounded(p, r.x, r.y + label_y, X4_UI2_CAPTION,
        X4_FONT_SEMIBOLD, x4_tr(X4_TR_ACCESS_COUNTRY), r.w, 28, 1, X4_UI2_MUTED);
    x4_ui2_card(p, r.x, r.y + select_y, r.w, SELECT_H, X4_UI2_RADIUS_CONTROL);
    x4_ui2_focus(p, r.x, r.y + select_y, r.w, SELECT_H, X4_UI2_RADIUS_CONTROL);
    x4_text_px(p, r.x + 18, r.y + select_y + 15, X4_UI2_LABEL,
        X4_FONT_SEMIBOLD, "‹", X4_UI2_ACCENT);
    x4_ui2_text_bounded(p, r.x + 55, r.y + select_y + 15, X4_UI2_LABEL,
        X4_FONT_SEMIBOLD, x4_access_country_label(ui->selected), r.w - 110, 32, 1, X4_UI2_TEXT);
    x4_text_px(p, r.x + r.w - 34, r.y + select_y + 15, X4_UI2_LABEL,
        X4_FONT_SEMIBOLD, "›", X4_UI2_ACCENT);
    x4_ui2_text_bounded(p, r.x, r.y + step_y, X4_UI2_CAPTION,
        X4_FONT_REGULAR, step, r.w, 28, 2, X4_UI2_MUTED);
    x4_ui2_text_bounded(p, r.x, r.y + warning_y, X4_UI2_LABEL,
        X4_FONT_REGULAR, warning, r.w, BODY_PITCH, 3, X4_RGB(255, 211, 151));
    x4_ui2_text_bounded(p, r.x, r.y + error_y, X4_UI2_LABEL,
        X4_FONT_REGULAR, save_error, r.w, BODY_PITCH, 2, X4_RGB(254, 165, 154));

    const X4Ui2ModalAction actions[] = {
        {X4_PAD_CROSS, x4_tr(X4_TR_ACCESS_TRY), true, true, false},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_ACCESS_BACK), false, false, false},
        {X4_PAD_TRIANGLE, x4_tr(X4_TR_ACCESS_HELP), false, false, false},
    };
    x4_ui2_modal_actions(p, actions, sizeof(actions) / sizeof(actions[0]));
}

void x4_ui2_modal_draw_access(uint32_t *p, const X4AccessCountryUi *ui,
    X4AccessCountryCardPhase phase)
{
    if (!p || !ui) return;
    if (phase == X4_ACCESS_CARD_TRYING) {
        access_trying(p, ui);
        return;
    }
    if (ui->help) {
        access_help(p, ui);
        return;
    }
    const char *save_error = ui->save_error ? x4_tr(X4_TR_ACCESS_SAVE_ERROR) : NULL;
    if (!ui->confirming) {
        access_selector(p, ui, save_error);
        return;
    }
    const X4Ui2ModalAction actions[] = {
        {X4_PAD_CROSS, x4_tr(X4_TR_ACCESS_CONTINUE), true, true, false},
        {X4_PAD_CIRCLE, x4_tr(X4_TR_ACCESS_CANCEL), false, false, false},
    };
    x4_ui2_modal_message(p, x4_tr(X4_TR_ACCESS_CONFIRM_TITLE), x4_tr(X4_TR_ACCESS_WARNING),
        save_error, X4_RGB(254, 165, 154), actions, sizeof(actions) / sizeof(actions[0]));
}
