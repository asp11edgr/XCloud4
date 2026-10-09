/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../input/controller.h"
typedef struct { int page; int selected; int exit_requested; } X4Screen;
void x4_screen_update(X4Screen *screen, uint32_t pressed, uint32_t held);
void x4_screen_draw(const X4Screen *screen, const X4Controller *controller, uint32_t *pixels);
