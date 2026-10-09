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
