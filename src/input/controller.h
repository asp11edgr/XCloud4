/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <orbis/Pad.h>
#include "gamepad.h"
typedef struct {
    int handle;
    int library_ready;
    int error;
    uint32_t previous;
    uint32_t pressed;
    OrbisPadData data;
} X4Controller;
void x4_controller_init(X4Controller *controller);
void x4_controller_read(X4Controller *controller, unsigned frame);
void x4_controller_close(X4Controller *controller);
/* Xbox axes use positive Y upwards; no private SDK button masks are guessed. */
void x4_controller_gamepad(const X4Controller *controller, X4GamepadFrame *out);
