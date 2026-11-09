/* SPDX-License-Identifier: GPL-3.0-only */
/* Host-only controller fixture shapes, not a native ABI. */
#pragma once
#include <stdint.h>
enum {
    ORBIS_PAD_PORT_TYPE_STANDARD = 0,
    ORBIS_PAD_BUTTON_CROSS = 1u << 0, ORBIS_PAD_BUTTON_CIRCLE = 1u << 1,
    ORBIS_PAD_BUTTON_SQUARE = 1u << 2, ORBIS_PAD_BUTTON_TRIANGLE = 1u << 3,
    ORBIS_PAD_BUTTON_OPTIONS = 1u << 4, ORBIS_PAD_BUTTON_TOUCH_PAD = 1u << 5,
    ORBIS_PAD_BUTTON_UP = 1u << 6, ORBIS_PAD_BUTTON_DOWN = 1u << 7,
    ORBIS_PAD_BUTTON_LEFT = 1u << 8, ORBIS_PAD_BUTTON_RIGHT = 1u << 9,
    ORBIS_PAD_BUTTON_L1 = 1u << 10, ORBIS_PAD_BUTTON_R1 = 1u << 11,
    ORBIS_PAD_BUTTON_L3 = 1u << 12, ORBIS_PAD_BUTTON_R3 = 1u << 13
};
typedef struct {
    uint32_t buttons;
    uint8_t connected;
    struct { uint8_t x, y; } leftStick, rightStick;
    struct { uint8_t l2, r2; } analogButtons;
    struct { float x, y, z; } vel, acell;
} OrbisPadData;
typedef struct { uint8_t lgMotor, smMotor; } OrbisPadVibeParam;
int scePadInit(void);
int scePadOpen(int32_t user, int type, int index, void *param);
int scePadClose(int handle);
int scePadReadState(int handle, OrbisPadData *data);
int scePadSetVibration(int handle, const OrbisPadVibeParam *param);
int scePadSetMotionSensorState(int handle, int enabled);
