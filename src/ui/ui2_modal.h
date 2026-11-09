/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "ui2_theme.h"
#include "ui_surface.h"
#include "../update/update_job51.h"

enum {
    X4_UI2_MODAL_W = 1120,
    X4_UI2_MODAL_MARGIN = 32,
    X4_UI2_MODAL_BODY_W = X4_UI2_MODAL_W - 2 * X4_UI2_MODAL_MARGIN,
};

typedef struct {
    enum X4PadGlyph glyph;
    const char *label;
    bool primary;
    bool focused;
    bool cross_on_focus;
} X4Ui2ModalAction;

/* The owner dims the retained UI once after drawing the underlying page and
 * shell. Subsequent draws update only the panel and restore any area it vacates. */
void x4_ui2_modal_scrim(uint32_t *pixels);
X4UiRect x4_ui2_modal_panel(uint32_t *pixels, const char *title, int body_height);
X4UiRect x4_ui2_modal_damage(void);
void x4_ui2_modal_actions(uint32_t *pixels, const X4Ui2ModalAction *actions, size_t count);
void x4_ui2_modal_message(uint32_t *pixels, const char *title, const char *body,
    const char *second, uint32_t second_color, const X4Ui2ModalAction *actions, size_t count);

/* Word wrapping with a whole-word ellipsis on the final truncated line.
 * A null canvas measures the number of occupied lines. */
int x4_ui2_text_bounded(uint32_t *pixels, int x, int y, int size, int weight,
    const char *text, int width, int line_height, int max_lines, uint32_t color);
int x4_ui2_modal_text(uint32_t *pixels, const char *text, int x, int y, int width, int lines);
int x4_ui2_modal_text_height(const char *text, int width, int lines);

void x4_ui2_modal_draw_access(uint32_t *pixels, const X4AccessCountryUi *ui,
    X4AccessCountryCardPhase phase);
void x4_ui2_update_notice_draw(const X4UpdateNotice *notice, uint32_t *pixels);
void x4_ui2_update_job_draw(const X4Update51View *job, uint32_t *pixels);
void x4_ui2_update_manual_draw(uint32_t *pixels, const char *version, const char *pkg_name, int32_t code);
void x4_ui2_update_applied_draw(uint32_t *pixels, const char *version, const char *pkg_name);
void x4_ui2_cloud_confirm_draw(uint32_t *pixels);
void x4_ui2_exit_error_draw(int error, uint32_t *pixels);
