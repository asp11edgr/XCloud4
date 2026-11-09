/* SPDX-License-Identifier: GPL-3.0-only */
/* Initialization follows the public OpenOrbis input sample. */
#include "controller.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#if !X4_PUBLIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wcast-function-type-mismatch"
#endif
static inline void x4_pad_set_motion_sensor(int handle, int enable)
{
    ((int (*)(int, int))(void *)scePadSetMotionSensorState)(handle, enable);
}
#pragma GCC diagnostic pop
#endif

#if !X4_PUBLIC
#include <stdio.h>
static bool s_motion_enabled = false;
static uint64_t s_last_motion_log_us = 0;
static uint32_t s_motion_samples = 0;
static float s_gyro_min[3], s_gyro_max[3], s_gyro_sum[3];
static float s_accel_min[3], s_accel_max[3], s_accel_sum[3];
#endif

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
#if !X4_PUBLIC
    if (s_motion_enabled && c->handle >= 0) x4_pad_set_motion_sensor(c->handle, 1);
#endif
}
void x4_controller_init(X4Controller *c)
{
    memset(c, 0, sizeof(*c)); c->handle = -1; open_controller(c);
}
static void sample_controller(X4Controller *c, bool close_on_error, bool motion)
{
#if X4_PUBLIC
    (void)motion;
#endif
    OrbisPadData data = {0};
    data.leftStick.x = data.leftStick.y = 128;
    data.rightStick.x = data.rightStick.y = 128;
    int rc = c->handle < 0 ? c->error : scePadReadState(c->handle, &data);
    if (rc < 0) {
        c->error = rc; data.connected = 0; data.buttons = 0;
        if (close_on_error && c->handle >= 0) scePadClose(c->handle);
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
#if !X4_PUBLIC
    if (motion && s_motion_enabled && c->handle >= 0 && data.connected) {
        const float gx = data.vel.x, gy = data.vel.y, gz = data.vel.z;
        const float ax = data.acell.x, ay = data.acell.y, az = data.acell.z;
        if (s_motion_samples == 0) {
            s_gyro_min[0] = s_gyro_max[0] = s_gyro_sum[0] = gx;
            s_gyro_min[1] = s_gyro_max[1] = s_gyro_sum[1] = gy;
            s_gyro_min[2] = s_gyro_max[2] = s_gyro_sum[2] = gz;
            s_accel_min[0] = s_accel_max[0] = s_accel_sum[0] = ax;
            s_accel_min[1] = s_accel_max[1] = s_accel_sum[1] = ay;
            s_accel_min[2] = s_accel_max[2] = s_accel_sum[2] = az;
        } else {
            if (gx < s_gyro_min[0]) { s_gyro_min[0] = gx; }
            if (gx > s_gyro_max[0]) { s_gyro_max[0] = gx; }
            s_gyro_sum[0] += gx;
            if (gy < s_gyro_min[1]) { s_gyro_min[1] = gy; }
            if (gy > s_gyro_max[1]) { s_gyro_max[1] = gy; }
            s_gyro_sum[1] += gy;
            if (gz < s_gyro_min[2]) { s_gyro_min[2] = gz; }
            if (gz > s_gyro_max[2]) { s_gyro_max[2] = gz; }
            s_gyro_sum[2] += gz;
            if (ax < s_accel_min[0]) { s_accel_min[0] = ax; }
            if (ax > s_accel_max[0]) { s_accel_max[0] = ax; }
            s_accel_sum[0] += ax;
            if (ay < s_accel_min[1]) { s_accel_min[1] = ay; }
            if (ay > s_accel_max[1]) { s_accel_max[1] = ay; }
            s_accel_sum[1] += ay;
            if (az < s_accel_min[2]) { s_accel_min[2] = az; }
            if (az > s_accel_max[2]) { s_accel_max[2] = az; }
            s_accel_sum[2] += az;
        }
        s_motion_samples++;
    }
#endif
}
void x4_controller_read(X4Controller *c, unsigned frame)
{
    if (c->handle < 0 && frame % 120 == 0) open_controller(c);
    sample_controller(c, true, true);
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

#if !X4_PUBLIC

void x4_controller_set_motion_sensor(X4Controller *c, bool enable)
{
    s_motion_enabled = enable;
    if (c && c->handle >= 0) {
        x4_pad_set_motion_sensor(c->handle, enable ? 1 : 0);
    }
    if (!enable) {
        s_motion_samples = 0;
    }
}

bool x4_controller_is_motion_sensor_enabled(const X4Controller *c)
{
    (void)c;
    return s_motion_enabled;
}

void x4_controller_log_motion_5s(const X4Controller *c, uint64_t now_us)
{
    if (!s_motion_enabled || !c || c->handle < 0) return;
    if (s_last_motion_log_us == 0) s_last_motion_log_us = now_us;
    if (now_us - s_last_motion_log_us < 5000000ULL) return;

    if (s_motion_samples > 0) {
        printf("XCloud4 pad motion: samples=%u gyro=[min:%.2f,%.2f,%.2f max:%.2f,%.2f,%.2f mean:%.2f,%.2f,%.2f] "
               "accel=[min:%.2f,%.2f,%.2f max:%.2f,%.2f,%.2f mean:%.2f,%.2f,%.2f]\n",
               s_motion_samples,
               s_gyro_min[0], s_gyro_min[1], s_gyro_min[2],
               s_gyro_max[0], s_gyro_max[1], s_gyro_max[2],
               s_gyro_sum[0] / s_motion_samples, s_gyro_sum[1] / s_motion_samples, s_gyro_sum[2] / s_motion_samples,
               s_accel_min[0], s_accel_min[1], s_accel_min[2],
               s_accel_max[0], s_accel_max[1], s_accel_max[2],
               s_accel_sum[0] / s_motion_samples, s_accel_sum[1] / s_motion_samples, s_accel_sum[2] / s_motion_samples);
    }
    s_motion_samples = 0;
    s_last_motion_log_us = now_us;
}
#endif

enum { ROSTER_SEVERAL = 1, ROSTER_IN_MATCH = 2, ROSTER_EVENTS_PER_TICK = 8 };
#define ROSTER_REFRESH_US UINT64_C(250000)
#define ROSTER_WORKER_US 10000u
#define ROSTER_STACK_BYTES (128u * 1024u)
#define NO_USER ORBIS_USER_SERVICE_USER_ID_INVALID

typedef struct {
    atomic_flag gate;
    int32_t user;
    char name[32];
    int handle;
    atomic_bool failed;
    bool connected;
#if !X4_PUBLIC
    bool motion;
#endif
    uint64_t generation, changed_us, retry_us;
} X4ControllerSlot;

typedef struct {
    int32_t user;
    char name[32];
} X4ControllerUser;

struct X4ControllerRoster {
    OrbisPthread thread;
    atomic_bool stop;
    atomic_uint configuration, assigned_mask;
    X4ControllerSlot slot[X4_GAMEPAD_COUNT];
    /* Worker only. Keep observed login order separately from reservations. */
    X4ControllerUser initial, ordered[X4_GAMEPAD_COUNT];
    unsigned ordered_count;
    int32_t reservation[X4_GAMEPAD_COUNT];
    bool initial_logged, library_ready, service_ready, was_several;
    uint64_t refresh_us;
    /* Main only. In a contended turn preserve the previous physical state. */
    X4ControllerRosterView view;
};

static bool slot_try(X4ControllerSlot *s)
{
    return !atomic_flag_test_and_set_explicit(&s->gate, memory_order_acquire);
}

static void slot_release(X4ControllerSlot *s)
{
    atomic_flag_clear_explicit(&s->gate, memory_order_release);
}

static bool valid_user(int32_t user)
{
    return user != NO_USER && user != ORBIS_USER_SERVICE_USER_ID_SYSTEM;
}

static void read_name(X4ControllerUser *u)
{
    memset(u->name, 0, sizeof(u->name));
    if (sceUserServiceGetUserName(u->user, u->name, sizeof(u->name)) < 0)
        u->name[0] = 0;
    u->name[sizeof(u->name) - 1] = 0;
    for (unsigned i = 0; u->name[i]; ++i) {
        unsigned byte = (unsigned char)u->name[i];
        if (byte < 0x20 || byte == 0x7f) u->name[i] = ' ';
    }
}

static void roster_login(X4ControllerRoster *r, int32_t user, bool logged)
{
    if (!valid_user(user)) return;
    if (user == r->initial.user) {
        r->initial_logged = logged;
        return;
    }
    unsigned i = 0;
    while (i < r->ordered_count && r->ordered[i].user != user) ++i;
    if (logged && i == r->ordered_count && i < X4_GAMEPAD_COUNT) {
        r->ordered[i].user = user;
        read_name(&r->ordered[i]);
        ++r->ordered_count;
    } else if (!logged && i < r->ordered_count) {
        --r->ordered_count;
        memmove(&r->ordered[i], &r->ordered[i + 1],
            (r->ordered_count - i) * sizeof(r->ordered[0]));
    }
}

static bool listed(const OrbisUserServiceLoginUserIdList *list, int32_t user)
{
    for (unsigned i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; ++i)
        if (list->userId[i] == user) return true;
    return false;
}

static void roster_discover(X4ControllerRoster *r, uint64_t now)
{
    for (unsigned i = 0; i < ROSTER_EVENTS_PER_TICK; ++i) {
        OrbisUserServiceEvent event;
        if (sceUserServiceGetEvent(&event) < 0) break;
        if (event.event == SCE_USER_SERVICE_EVENT_TYPE_LOGIN)
            roster_login(r, event.userId, true);
        else if (event.event == SCE_USER_SERVICE_EVENT_TYPE_LOGOUT)
            roster_login(r, event.userId, false);
    }
    if (now < r->refresh_us) return;
    r->refresh_us = now + ROSTER_REFRESH_US;
    OrbisUserServiceLoginUserIdList list;
    if (sceUserServiceGetLoginUserIdList(&list) < 0) return;
    r->initial_logged = listed(&list, r->initial.user);
    for (unsigned i = 0; i < r->ordered_count; )
        if (listed(&list, r->ordered[i].user)) ++i;
        else roster_login(r, r->ordered[i].user, false);
    for (unsigned i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; ++i)
        roster_login(r, list.userId[i], true);
}

static void set_physical(X4ControllerSlot *s, bool connected)
{
    if (s->connected == connected) return;
    s->connected = connected;
    if (++s->generation == 0) ++s->generation;
    s->changed_us = sceKernelGetProcessTime();
}

static void close_pad(int handle)
{
    if (handle < 0) return;
#if X4_RUMBLE
    OrbisPadVibeParam zero = {0};
    (void)scePadSetVibration(handle, &zero);
#endif
    (void)scePadClose(handle);
}

static void service_slot(X4ControllerSlot *s, int32_t user, const char *name, bool wanted, uint64_t now)
{
    /* user/handle are written only by this worker. A healthy unchanged
     * slot needs no lease, so it cannot contend with normal P1 sampling. */
    if (s->user == user && ((!wanted && s->handle < 0) ||
        (wanted && s->handle >= 0 && !atomic_load_explicit(&s->failed, memory_order_acquire)))) return;
    if (!slot_try(s)) return;
    int retired = -1;
    bool failed = atomic_load_explicit(&s->failed, memory_order_relaxed);
    if (s->user != user || !wanted || failed) {
        retired = s->handle;
        s->handle = -1;
        set_physical(s, false);
        if (s->user != user) {
            s->user = user;
            s->retry_us = 0;
            memset(s->name, 0, sizeof(s->name));
        }
        if (failed) s->retry_us = now + ROSTER_REFRESH_US;
        atomic_store_explicit(&s->failed, false, memory_order_relaxed);
    }
    if (name) memcpy(s->name, name, sizeof(s->name));
    bool open = wanted && s->handle < 0 && now >= s->retry_us;
    if (open) s->retry_us = now + ROSTER_REFRESH_US;
    slot_release(s);

    /* Opening or closing an OS handle never holds the main-thread lease. */
    close_pad(retired);
    if (!open) return;
    int handle = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    if (handle < 0) return;
    if (!slot_try(s)) {
        close_pad(handle);
        return;
    }
    s->handle = handle;
#if !X4_PUBLIC
    s->motion = false;
#endif
    slot_release(s);
}

static void service_roster(X4ControllerRoster *r, uint64_t now)
{
    if (!r->library_ready) {
        if (scePadInit() < 0) return;
        r->library_ready = true;
    }
    if (!r->service_ready) {
        OrbisUserServiceInitializeParams params = {.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST};
        /* Another application subsystem may already have initialized it. */
        (void)sceUserServiceInitialize(&params);
        r->service_ready = true;
    }
    if (!valid_user(r->initial.user)) {
        int32_t initial = NO_USER;
        if (sceUserServiceGetInitialUser(&initial) < 0 || !valid_user(initial)) return;
        r->initial.user = initial;
        r->initial_logged = true;
        read_name(&r->initial);
    }
    unsigned configuration = atomic_load_explicit(&r->configuration, memory_order_acquire);
    bool several = (configuration & ROSTER_SEVERAL) != 0;
    bool in_match = (configuration & ROSTER_IN_MATCH) != 0;
    if (several) {
        if (!r->was_several) r->refresh_us = 0;
        roster_discover(r, now);
    } else {
        r->ordered_count = 0;
        r->initial_logged = true; /* Preserve the single-pad retry behavior. */
    }
    r->was_several = several;

    unsigned assigned_mask = 1;

    r->reservation[0] = r->initial.user;
    if (!several || !in_match)
        for (unsigned i = 1; i < X4_GAMEPAD_COUNT; ++i) r->reservation[i] = NO_USER;
    for (unsigned u = 0; u < r->ordered_count; ++u) {
        int32_t user = r->ordered[u].user;
        unsigned free_index = X4_GAMEPAD_COUNT;
        bool present = false;
        for (unsigned i = 1; i < X4_GAMEPAD_COUNT; ++i) {
            if (r->reservation[i] == user) present = true;
            if (r->reservation[i] == NO_USER && free_index == X4_GAMEPAD_COUNT) free_index = i;
        }
        if (!present && free_index < X4_GAMEPAD_COUNT) r->reservation[free_index] = user;
    }

    service_slot(&r->slot[0], r->initial.user, r->initial.name, r->initial_logged, now);
    for (unsigned i = 1; i < X4_GAMEPAD_COUNT; ++i) {
        int32_t user = r->reservation[i];
        if (user != NO_USER) assigned_mask |= 1u << i;
        const char *name = NULL;
        for (unsigned u = 0; u < r->ordered_count; ++u)
            if (r->ordered[u].user == user) name = r->ordered[u].name;
        service_slot(&r->slot[i], user, name, name != NULL, now);
    }
    if (atomic_load_explicit(&r->assigned_mask, memory_order_relaxed) != assigned_mask)
        atomic_store_explicit(&r->assigned_mask, assigned_mask, memory_order_release);
}

static void *roster_worker(void *opaque)
{
    X4ControllerRoster *r = opaque;
    while (!atomic_load_explicit(&r->stop, memory_order_acquire)) {
        service_roster(r, sceKernelGetProcessTime());
        sceKernelUsleep(ROSTER_WORKER_US);
    }
    /* The caller stopped sampling before joining. No new leases can start. */
    for (unsigned i = 0; i < X4_GAMEPAD_COUNT; ++i) close_pad(r->slot[i].handle);
    return NULL;
}

X4ControllerRoster *x4_controller_roster_start(bool several)
{
    X4ControllerRoster *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    atomic_init(&r->stop, false);
    atomic_init(&r->configuration, several ? ROSTER_SEVERAL : 0);
    atomic_init(&r->assigned_mask, 1);
    r->initial.user = NO_USER;
    for (unsigned i = 0; i < X4_GAMEPAD_COUNT; ++i) {
        r->slot[i].gate = (atomic_flag)ATOMIC_FLAG_INIT;
        atomic_init(&r->slot[i].failed, false);
        r->slot[i].handle = r->view.pad[i].handle = -1;
        r->slot[i].user = r->view.user_id[i] = r->reservation[i] = NO_USER;
    }
    OrbisPthreadAttr attr;
    int rc = scePthreadAttrInit(&attr);
    if (!rc) {
        rc = scePthreadAttrSetstacksize(&attr, ROSTER_STACK_BYTES);
        if (!rc) rc = scePthreadCreate(&r->thread, &attr, roster_worker, r, "x4-pads");
        (void)scePthreadAttrDestroy(&attr);
    }
    if (rc) {
        free(r);
        return NULL;
    }
    return r;
}

void x4_controller_roster_configure(X4ControllerRoster *r, bool several, bool in_match)
{
    if (!r) return;
    unsigned configuration = (several ? ROSTER_SEVERAL : 0) | (in_match ? ROSTER_IN_MATCH : 0);
    if (atomic_load_explicit(&r->configuration, memory_order_relaxed) != configuration)
        atomic_store_explicit(&r->configuration, configuration, memory_order_release);
}

void x4_controller_roster_read(X4ControllerRoster *r, X4ControllerRosterView *out)
{
    if (!r || !out) return;
    unsigned assigned = atomic_load_explicit(&r->assigned_mask, memory_order_acquire);
    for (unsigned i = 0; i < X4_GAMEPAD_COUNT; ++i) {
        X4ControllerSlot *s = &r->slot[i];
        X4Controller *pad = &r->view.pad[i];
        pad->pressed = 0;
        if (i && !(assigned & (1u << i)) && !r->view.assigned[i]) continue;
        if (!slot_try(s)) continue;
        if (r->view.user_id[i] != s->user || r->view.generation[i] != s->generation ||
            pad->handle != s->handle) pad->previous = 0;
        pad->handle = s->handle;
#if !X4_PUBLIC
        if (i == 0 && s->handle >= 0 && s->motion != s_motion_enabled) {
            x4_pad_set_motion_sensor(s->handle, s_motion_enabled ? 1 : 0);
            s->motion = s_motion_enabled;
        }
#endif
        if (atomic_load_explicit(&s->failed, memory_order_relaxed)) pad->handle = -1;
        sample_controller(pad, false, i == 0);
        if (s->handle >= 0 && pad->error < 0)
            atomic_store_explicit(&s->failed, true, memory_order_release);
        set_physical(s, pad->data.connected);
        r->view.assigned[i] = valid_user(s->user);
        r->view.user_id[i] = s->user;
        memcpy(r->view.user_name[i], s->name, sizeof(s->name));
        r->view.generation[i] = s->generation;
        r->view.changed_us[i] = s->changed_us;
        slot_release(s);
    }
    *out = r->view;
}

int x4_controller_roster_vibrate(X4ControllerRoster *r, unsigned index, uint64_t generation,
    uint8_t large, uint8_t small)
{
#if X4_RUMBLE
    if (!r || index >= X4_GAMEPAD_COUNT || (!generation && index != 0)) return -1;
    X4ControllerSlot *s = &r->slot[index];
    if (!slot_try(s)) return -1;
    int result = -1;
    if (s->handle >= 0 && !atomic_load_explicit(&s->failed, memory_order_relaxed) &&
        (!generation || generation == s->generation) && (s->connected || (!large && !small))) {
        OrbisPadVibeParam vibe = {.lgMotor = large, .smMotor = small};
        result = scePadSetVibration(s->handle, &vibe);
    }
    slot_release(s);
    return result;
#else
    (void)r; (void)index; (void)generation; (void)large; (void)small;
    return -1;
#endif
}

#if !X4_PUBLIC
void x4_controller_roster_set_motion_sensor(X4ControllerRoster *r, bool enable)
{
    (void)r;
    /* The next P1 read applies the new setting under its handle lease. */
    x4_controller_set_motion_sensor(NULL, enable);
}
#endif

void x4_controller_roster_stop(X4ControllerRoster *r)
{
    if (!r) return;
    atomic_store_explicit(&r->stop, true, memory_order_release);
    if (!scePthreadJoin(r->thread, NULL)) free(r);
}
