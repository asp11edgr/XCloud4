/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../auth/release_check.h"
#include "product_metadata.h"

void x4_ui2_about_draw(uint32_t *pixels, const X4CoverRaster *app_icon,
    enum X4UpdateStatus status, const char *version);
void x4_ui2_support_draw(uint32_t *pixels);
