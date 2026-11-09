/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "ui2_theme.h"

typedef enum X4Ui2BootStep {
    X4_UI2_BOOT_FONTS, X4_UI2_BOOT_CACHE, X4_UI2_BOOT_ACCOUNT,
    X4_UI2_BOOT_CATALOG, X4_UI2_BOOT_READY,
} X4Ui2BootStep;

/* An actual startup boundary supplies the active step. No elapsed-time
 * interpolation and no renderer-owned wait. */
bool x4_ui2_boot_draw(uint32_t *pixels, X4Ui2BootStep step);
void x4_ui2_boot_reset(void);
/* SIZE_MAX = full backing, 1 = progress and any changed text, 0 = unchanged. */
size_t x4_ui2_boot_damage(X4UiRect *rect);
