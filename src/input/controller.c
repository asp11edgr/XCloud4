/* SPDX-License-Identifier: GPL-3.0-only */
/* Initialization follows the public OpenOrbis input sample. */
#include "controller.h"
#include <string.h>
#include <orbis/UserService.h>
static void open_controller(X4Controller *c)
{
    int rc;
    if (!c->library_ready) {
        rc = scePadInit();
        if (rc < 0) { c->error = rc; return; }
        c->library_ready = 1;
    }
    OrbisUserServiceInitializeParams params = {.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST};
    sceUserServiceInitialize(&params);
    int32_t user = -1;
    rc = sceUserServiceGetInitialUser(&user);
    if (rc < 0) { c->error = rc; return; }
    rc = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    if (rc < 0) { c->error = rc; return; }
    c->handle = rc;
    c->error = 0;
}
void x4_controller_init(X4Controller *c)
{
    memset(c, 0, sizeof(*c)); c->handle = -1; open_controller(c);
}
void x4_controller_read(X4Controller *c, unsigned frame)
{
    c->pressed = 0;
    if (c->handle < 0 && frame % 120 == 0) open_controller(c);
    OrbisPadData data = {0};
    data.leftStick.x = data.leftStick.y = 128;
    data.rightStick.x = data.rightStick.y = 128;
    int rc = c->handle < 0 ? c->error : scePadReadState(c->handle, &data);
    if (rc < 0) {
        c->error = rc; data.connected = 0; data.buttons = 0;
        if (c->handle >= 0) scePadClose(c->handle);
        c->handle = -1;
    } else c->error = 0;
    if (!data.connected) {
        data.buttons = 0;
        data.leftStick.x = data.leftStick.y = 128;
        data.rightStick.x = data.rightStick.y = 128;
        data.analogButtons.l2 = data.analogButtons.r2 = 0;
    }
    c->pressed = data.buttons & ~c->previous;
    c->previous = data.buttons;
    c->data = data;
}
void x4_controller_close(X4Controller *c)
{
    if (c->handle >= 0) scePadClose(c->handle);
    c->handle = -1;
}

static int16_t axis(uint8_t value)
{
    int n = (int)value - 128;
    return (int16_t)(n >= 0 ? n * 32767 / 127 : n * 32767 / 128);
}

void x4_controller_gamepad(const X4Controller *c, X4GamepadFrame *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!c || !c->data.connected || c->error) return;
    out->connected = true;
    static const struct { uint32_t ps; uint16_t xbox; } map[] = {
        {ORBIS_PAD_BUTTON_CROSS, X4_GAMEPAD_A}, {ORBIS_PAD_BUTTON_CIRCLE, X4_GAMEPAD_B},
        {ORBIS_PAD_BUTTON_SQUARE, X4_GAMEPAD_X}, {ORBIS_PAD_BUTTON_TRIANGLE, X4_GAMEPAD_Y},
        {ORBIS_PAD_BUTTON_OPTIONS, X4_GAMEPAD_MENU}, {ORBIS_PAD_BUTTON_TOUCH_PAD, X4_GAMEPAD_VIEW},
        {ORBIS_PAD_BUTTON_UP, X4_GAMEPAD_UP}, {ORBIS_PAD_BUTTON_DOWN, X4_GAMEPAD_DOWN},
        {ORBIS_PAD_BUTTON_LEFT, X4_GAMEPAD_LEFT}, {ORBIS_PAD_BUTTON_RIGHT, X4_GAMEPAD_RIGHT},
        {ORBIS_PAD_BUTTON_L1, X4_GAMEPAD_LB}, {ORBIS_PAD_BUTTON_R1, X4_GAMEPAD_RB},
        {ORBIS_PAD_BUTTON_L3, X4_GAMEPAD_L3}, {ORBIS_PAD_BUTTON_R3, X4_GAMEPAD_R3},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if (c->data.buttons & map[i].ps) out->buttons |= map[i].xbox;
    out->left_x = axis(c->data.leftStick.x);
    out->left_y = (int16_t)-axis(c->data.leftStick.y);
    out->right_x = axis(c->data.rightStick.x);
    out->right_y = (int16_t)-axis(c->data.rightStick.y);
    out->left_trigger = (uint16_t)c->data.analogButtons.l2 * 257u;
    out->right_trigger = (uint16_t)c->data.analogButtons.r2 * 257u;
}
