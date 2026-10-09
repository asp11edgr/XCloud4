/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    X4_GAMEPAD_NEXUS = 1u << 1,
    X4_GAMEPAD_MENU = 1u << 2,
    X4_GAMEPAD_VIEW = 1u << 3,
    X4_GAMEPAD_A = 1u << 4,
    X4_GAMEPAD_B = 1u << 5,
    X4_GAMEPAD_X = 1u << 6,
    X4_GAMEPAD_Y = 1u << 7,
    X4_GAMEPAD_UP = 1u << 8,
    X4_GAMEPAD_DOWN = 1u << 9,
    X4_GAMEPAD_LEFT = 1u << 10,
    X4_GAMEPAD_RIGHT = 1u << 11,
    X4_GAMEPAD_LB = 1u << 12,
    X4_GAMEPAD_RB = 1u << 13,
    X4_GAMEPAD_L3 = 1u << 14,
    X4_GAMEPAD_R3 = 1u << 15,
};

/* Portable state, not a serialized C struct. Axes use [-32767,32767],
 * positive Y up; triggers use [0,65535]. Neutralize local reserved chords
 * in the source. connected is physical state, not an Xbox acknowledgement. */
typedef struct {
    bool connected;
    uint16_t buttons;
    int16_t left_x, left_y, right_x, right_y;
    uint16_t left_trigger, right_trigger;
} X4GamepadFrame;

/* Copy under the source's short gate. Called outside the RTC gate by its
 * sender; do not access RTC, block, allocate or retain out. False produces
 * a neutral report. Source/context outlive successful RTC close/join. */
typedef bool (*X4GamepadSource)(void *user, X4GamepadFrame *out);
