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
    /* Local pad-read time; never serialized in the 38-byte report. */
    uint64_t sampled_us;
} X4GamepadFrame;

#define X4_GAMEPAD_COUNT 4u

typedef struct {
    X4GamepadFrame pads[X4_GAMEPAD_COUNT];
    uint64_t changed_us[X4_GAMEPAD_COUNT];
    uint64_t generation[X4_GAMEPAD_COUNT];
    uint8_t connected_mask;
    bool multiplayer; /* Captured for the life of the match. */
} X4GamepadRoster;

typedef struct {
    bool active, multiplayer;
} X4GamepadMatchOption;

static inline bool x4_gamepad_match_option(X4GamepadMatchOption *option,
    bool enabled, bool in_match)
{
    if (!option->active || !in_match) option->multiplayer = enabled;
    option->active = in_match;
    return option->multiplayer;
}

static inline unsigned x4_gamepad_menu_index(uint8_t mask, bool allow_fallback)
{
    if (!allow_fallback || (mask & 1u)) return 0;
    for (unsigned i = 1; i < X4_GAMEPAD_COUNT; ++i)
        if (mask & (1u << i)) return i;
    return 0;
}

/* A busy or expired input mailbox returns false. It is not evidence of a
 * physical removal. The RTC sender owns any neutral/announcement fallback. */
typedef bool (*X4GamepadSource)(void *user, X4GamepadRoster *out);
