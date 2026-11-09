/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>

#define X4_UI_LANG_PREF_PATH "/data/xcloud4/ui-lang-v1.bin"

typedef enum {
    X4_UI_LANG_AUTO,
    X4_UI_LANG_ES,
    X4_UI_LANG_EN,
    X4_UI_LANG_PT_BR,
    X4_UI_LANG_FR,
    X4_UI_LANG_IT,
    X4_UI_LANG_DE,
    X4_UI_LANG_COUNT,
} X4UiLangPreference;

/* Main/UI thread only. NULL selects the production record. */
void x4_ui_lang_pref_init(const char *path);
X4UiLangPreference x4_ui_lang_pref_get(void);
typedef enum {
    X4_UI_LANG_SAVE_IDLE,
    X4_UI_LANG_SAVE_PENDING,
    X4_UI_LANG_SAVE_DONE,
    X4_UI_LANG_SAVE_FAILED,
} X4UiLangSave;

void x4_ui_lang_pref_start(void);
void x4_ui_lang_pref_stop(void);
/* Request and poll are nonblocking. Poll applies a successful write on the UI thread. */
X4UiLangSave x4_ui_lang_pref_request(X4UiLangPreference preference);
X4UiLangSave x4_ui_lang_pref_poll(void);
const char *x4_ui_lang_pref_label(X4UiLangPreference preference);

