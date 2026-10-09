/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "../video/display.h"
uint32_t x4_rgb(unsigned r, unsigned g, unsigned b);
void x4_rect(uint32_t *pixels, int x, int y, int w, int h, uint32_t color);
void x4_text(uint32_t *pixels, int x, int y, int scale, const char *text, uint32_t color);
