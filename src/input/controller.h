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

#if !X4_PUBLIC
void x4_controller_set_motion_sensor(X4Controller *controller, bool enable);
bool x4_controller_is_motion_sensor_enabled(const X4Controller *controller);
void x4_controller_log_motion_5s(const X4Controller *controller, uint64_t now_us);
#endif

/* The worker owns discovery/open/close. Main owns the sampled view and
 * tries each handle lease once; contention is not a physical disconnect. */
typedef struct X4ControllerRoster X4ControllerRoster;
typedef struct {
    X4Controller pad[X4_GAMEPAD_COUNT];
    bool assigned[X4_GAMEPAD_COUNT];
    int32_t user_id[X4_GAMEPAD_COUNT];
    char user_name[X4_GAMEPAD_COUNT][32];
    uint64_t generation[X4_GAMEPAD_COUNT];
    uint64_t changed_us[X4_GAMEPAD_COUNT];
} X4ControllerRosterView;

X4ControllerRoster *x4_controller_roster_start(bool several);
void x4_controller_roster_configure(X4ControllerRoster *roster, bool several, bool in_match);
void x4_controller_roster_read(X4ControllerRoster *roster, X4ControllerRosterView *out);
void x4_controller_roster_stop(X4ControllerRoster *roster);
int x4_controller_roster_vibrate(X4ControllerRoster *roster, unsigned index,
    uint64_t generation, uint8_t large, uint8_t small);
#if !X4_PUBLIC
void x4_controller_roster_set_motion_sensor(X4ControllerRoster *roster, bool enable);
#endif
