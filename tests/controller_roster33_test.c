/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>
#include "../src/input/controller.c"
#include "../src/input/rumble.h"

enum { PRIMARY = 20, GUEST = 23, PAD_COUNT = 32, HANDLE_BASE = 100 };
static OrbisUserServiceLoginUserIdList logged = {{PRIMARY, NO_USER, NO_USER, NO_USER}};
static OrbisUserServiceEvent events[8];
static unsigned events_read, events_count;
static atomic_bool physical[PAD_COUNT], opened[PAD_COUNT], faulty[PAD_COUNT];
static atomic_uint active_sdk_read[PAD_COUNT], opens[PAD_COUNT], closes[PAD_COUNT], vibration[PAD_COUNT];
static atomic_uint_fast64_t clock_us;
static bool start_worker, list_failed;
static unsigned discovery_calls;

static unsigned user_from_handle(int handle)
{
    assert(handle >= HANDLE_BASE && handle < HANDLE_BASE + PAD_COUNT);
    return (unsigned)(handle - HANDLE_BASE);
}

int scePadInit(void) { return 0; }
int scePadOpen(int32_t user, int type, int index, void *param)
{
    assert(user >= 0 && user < PAD_COUNT && type == ORBIS_PAD_PORT_TYPE_STANDARD && !index && !param);
    assert(!atomic_exchange(&opened[user], true));
    atomic_fetch_add(&opens[user], 1);
    return HANDLE_BASE + user;
}
int scePadClose(int handle)
{
    unsigned user = user_from_handle(handle);
    assert(!atomic_load(&active_sdk_read[user]));
    assert(atomic_exchange(&opened[user], false));
    atomic_fetch_add(&closes[user], 1);
    return 0;
}
int scePadReadState(int handle, OrbisPadData *data)
{
    unsigned user = user_from_handle(handle);
    atomic_fetch_add(&active_sdk_read[user], 1);
    assert(atomic_load(&opened[user]));
    bool error = atomic_load(&faulty[user]);
    data->connected = atomic_load(&physical[user]);
    data->buttons = ORBIS_PAD_BUTTON_CROSS;
    assert(atomic_load(&opened[user]));
    atomic_fetch_sub(&active_sdk_read[user], 1);
    return error ? -66 : 0;
}
int scePadSetVibration(int handle, const OrbisPadVibeParam *param)
{
    unsigned user = user_from_handle(handle);
    assert(atomic_load(&opened[user]));
    atomic_store(&vibration[user], (unsigned)param->lgMotor * 256 + param->smMotor);
    return 0;
}
int scePadSetMotionSensorState(int handle, int enabled)
{
    assert(user_from_handle(handle) == PRIMARY && (enabled == 0 || enabled == 1));
    return 0;
}
int sceUserServiceInitialize(void *param)
{
    assert(param);
    return 0;
}
int sceUserServiceGetInitialUser(int32_t *user)
{
    *user = PRIMARY;
    return 0;
}
int sceUserServiceGetUserName(int32_t user, char *out, size_t capacity)
{
    assert(user >= 0 && user < PAD_COUNT);
    int n = user == GUEST ? snprintf(out, capacity, "Guest") : snprintf(out, capacity, "User %d", user);
    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}
int sceUserServiceGetLoginUserIdList(OrbisUserServiceLoginUserIdList *out)
{
    ++discovery_calls;
    if (list_failed) return -1;
    *out = logged;
    return 0;
}
int sceUserServiceGetEvent(OrbisUserServiceEvent *out)
{
    if (events_read == events_count) return -1;
    *out = events[events_read++];
    return 0;
}
uint64_t sceKernelGetProcessTime(void)
{
    return atomic_load(&clock_us);
}
int sceKernelUsleep(unsigned us)
{
    atomic_fetch_add(&clock_us, us);
    sched_yield();
    return 0;
}
int scePthreadCreate(OrbisPthread *thread, const pthread_attr_t *attr,
    void *(*entry)(void *), void *arg, const char *name)
{
    assert(strcmp(name, "x4-pads") == 0);
    return start_worker ? pthread_create(thread, attr, entry, arg) : 0;
}
int32_t scePthreadAttrInit(OrbisPthreadAttr *attr) { return pthread_attr_init(attr); }
int32_t scePthreadAttrSetstacksize(OrbisPthreadAttr *attr, size_t bytes)
{
    assert(bytes == ROSTER_STACK_BYTES);
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    bytes = 1024u * 1024u; /* TSan host runtime requires a larger real stack. */
#endif
#endif
    return pthread_attr_setstacksize(attr, bytes);
}
int32_t scePthreadAttrDestroy(OrbisPthreadAttr *attr) { return pthread_attr_destroy(attr); }
int scePthreadJoin(OrbisPthread thread, void **result)
{
    return start_worker ? pthread_join(thread, result) : 0;
}

static void queue_event(int type, int32_t user)
{
    assert(events_count < sizeof(events) / sizeof(events[0]));
    events[events_count++] = (OrbisUserServiceEvent){type, user};
}

static void at(X4ControllerRoster *r, X4ControllerRosterView *view, uint64_t now)
{
    atomic_store(&clock_us, now);
    service_roster(r, now);
    x4_controller_roster_read(r, view);
}

static void stop_manual(X4ControllerRoster *r)
{
    for (unsigned i = 0; i < X4_GAMEPAD_COUNT; ++i) close_pad(r->slot[i].handle);
    x4_controller_roster_stop(r);
}

static void test_lifecycle(void)
{
    X4ControllerRoster *r = x4_controller_roster_start(false);
    assert(r);
    X4ControllerRosterView view;
    logged = (OrbisUserServiceLoginUserIdList){{GUEST, PRIMARY, NO_USER, NO_USER}};
    atomic_store(&physical[PRIMARY], true);
    atomic_store(&physical[GUEST], true);
    at(r, &view, 1);
    assert(!discovery_calls && !atomic_load(&opens[GUEST]));
    assert(view.user_id[0] == PRIMARY && view.pad[0].data.connected && !view.assigned[1]);

    x4_controller_roster_configure(r, true, true);
    at(r, &view, 10);
    assert(view.user_id[0] == PRIMARY && view.user_id[1] == GUEST);
    assert(strcmp(view.user_name[1], "Guest") == 0 && view.pad[1].data.connected);
    uint64_t guest_generation = view.generation[1];
    assert(guest_generation && view.changed_us[1] == 10);

    assert(slot_try(&r->slot[1]));
    atomic_store(&physical[GUEST], false);
    at(r, &view, 100);
    assert(view.pad[1].data.connected && view.generation[1] == guest_generation);
    slot_release(&r->slot[1]);
    at(r, &view, 200);
    assert(!view.pad[1].data.connected && view.generation[1] != guest_generation);
    assert(view.changed_us[1] == 200 && atomic_load(&opened[GUEST]));
    atomic_store(&physical[GUEST], true);
    at(r, &view, 1000200);
    assert(view.pad[1].data.connected && view.user_id[1] == GUEST);
    assert(view.generation[1] != guest_generation && view.changed_us[1] == 1000200);

    atomic_store(&faulty[GUEST], true);
    at(r, &view, 1000300);
    assert(view.pad[1].error == -66 && !view.pad[1].data.connected);
    at(r, &view, 1000400);
    assert(!atomic_load(&opened[GUEST]) && atomic_load(&closes[GUEST]) == 1);
    atomic_store(&faulty[GUEST], false);
    at(r, &view, 1250400);
    assert(atomic_load(&opened[GUEST]) && view.pad[1].data.connected && view.user_id[1] == GUEST);

    queue_event(SCE_USER_SERVICE_EVENT_TYPE_LOGOUT, GUEST);
    logged = (OrbisUserServiceLoginUserIdList){{PRIMARY, NO_USER, NO_USER, NO_USER}};
    at(r, &view, 1250500);
    assert(!view.pad[1].data.connected && !atomic_load(&opened[GUEST]));
    assert(view.assigned[1] && view.user_id[1] == GUEST);

    queue_event(SCE_USER_SERVICE_EVENT_TYPE_LOGIN, 24);
    queue_event(SCE_USER_SERVICE_EVENT_TYPE_LOGIN, 21);
    logged = (OrbisUserServiceLoginUserIdList){{PRIMARY, 21, 24, NO_USER}};
    atomic_store(&physical[21], true);
    atomic_store(&physical[24], true);
    at(r, &view, 1600000);
    assert(view.user_id[2] == 24 && view.user_id[3] == 21); /* Event order beats list order. */
    assert(view.pad[2].data.connected && view.pad[3].data.connected);

    queue_event(SCE_USER_SERVICE_EVENT_TYPE_LOGIN, 22);
    logged = (OrbisUserServiceLoginUserIdList){{PRIMARY, 21, 24, 22}};
    at(r, &view, 2000000);
    assert(!atomic_load(&opens[22])); /* A fifth identity cannot steal the guest slot. */
    queue_event(SCE_USER_SERVICE_EVENT_TYPE_LOGOUT, 22);
    queue_event(SCE_USER_SERVICE_EVENT_TYPE_LOGIN, GUEST);
    logged.userId[3] = GUEST;
    at(r, &view, 2400000);
    assert(view.pad[1].data.connected && view.user_id[1] == GUEST);

    list_failed = true;
    at(r, &view, 2800000);
    list_failed = false;
    assert(view.pad[0].data.connected && view.pad[1].data.connected);

    x4_controller_roster_configure(r, false, false);
    at(r, &view, 2800100);
    assert(view.pad[0].data.connected && !view.assigned[1] && !view.assigned[2] && !view.assigned[3]);
    assert(!atomic_load(&opened[GUEST]) && !atomic_load(&opened[21]) && !atomic_load(&opened[24]));
    stop_manual(r);
    puts("PASS pad lifecycle: disabled P1, guest, try lease, error, stable reservation, event order and fifth user");
}

static void test_rumble_target(void)
{
    X4ControllerRoster *r = x4_controller_roster_start(true);
    assert(r);
    X4ControllerRosterView view;
    logged = (OrbisUserServiceLoginUserIdList){{PRIMARY, GUEST, NO_USER, NO_USER}};
    atomic_store(&physical[PRIMARY], true);
    atomic_store(&physical[GUEST], true);
    at(r, &view, 1);
    X4Rumble *rumble = x4_rumble_new();
    assert(rumble && x4_rumble_begin(rumble));
    assert(x4_rumble_set_targets(rumble, 3, view.generation));
    uint8_t report[13] = {128, 0, 0, 1, 100, 50, 0, 0, 100};
    assert(x4_rumble_receive(rumble, report, sizeof(report), 1) == X4_RUMBLE_VALID);
    X4RumbleOutput out;
    assert(x4_rumble_poll(rumble, 1, &out));
    assert(!out.lgMotor && out.others[0].lgMotor == 255 && out.others[0].smMotor == 128);
    assert(!x4_controller_roster_vibrate(r, 1, out.others[0].generation,
        out.others[0].lgMotor, out.others[0].smMotor));
    assert(!atomic_load(&vibration[PRIMARY]) && atomic_load(&vibration[GUEST]) == 255 * 256 + 128);

    report[3] = 3;
    assert(x4_rumble_receive(rumble, report, sizeof(report), 2) == X4_RUMBLE_INACTIVE);
    assert(!x4_rumble_poll(rumble, 2, &out) && !out.others[2].lgMotor);
    assert(x4_controller_roster_vibrate(r, 3, view.generation[3], 255, 0) < 0);

    uint64_t generation = view.generation[1];
    atomic_store(&physical[GUEST], false);
    at(r, &view, 3);
    atomic_store(&physical[GUEST], true);
    at(r, &view, 4);
    assert(view.generation[1] != generation);
    assert(x4_rumble_set_targets(rumble, 3, view.generation));
    assert(x4_rumble_poll(rumble, 4, &out) && !out.others[0].lgMotor);
    assert(x4_controller_roster_vibrate(r, 1, generation, 255, 255) < 0);

    X4RumbleSnapshot stats;
    x4_rumble_snapshot(rumble, &stats);
    assert(stats.received_by_index[1] == 1 && stats.received_by_index[3] == 1);
    x4_rumble_disable(rumble);
    assert(x4_rumble_free(rumble));
    stop_manual(r);
    puts("PASS vibration: slot 1 only, absent slot 3 ignored, obsolete generation rejected");
}

static void test_worker_race(void)
{
    logged = (OrbisUserServiceLoginUserIdList){{PRIMARY, GUEST, 21, 24}};
    start_worker = true;
    X4ControllerRoster *r = x4_controller_roster_start(true);
    assert(r);
    x4_controller_roster_configure(r, true, true);
    X4ControllerRosterView view;
    for (unsigned i = 0; i < 20000; ++i) {
        atomic_store(&physical[GUEST], (i % 37) < 23);
        atomic_store(&faulty[GUEST], (i % 127) == 0);
        x4_controller_roster_read(r, &view);
        if (view.pad[1].data.connected)
            (void)x4_controller_roster_vibrate(r, 1, view.generation[1], 1, 2);
        if (!(i % 13)) sched_yield();
    }
    x4_controller_roster_stop(r);
    assert(!atomic_load(&opened[PRIMARY]) && !atomic_load(&opened[GUEST]));
    puts("PASS real worker/main leases under concurrent reads, retries and vibration");
}

int main(void)
{
    test_lifecycle();
    test_rumble_target();
    test_worker_race();
    return 0;
}
