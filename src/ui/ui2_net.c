/* SPDX-License-Identifier: GPL-3.0-only */
#include "ui2_net.h"

#if defined(X4_OPENORBIS)
#include <orbis/NetCtl.h>
#include <orbis/Rtc.h>
#include <orbis/libkernel.h>

/* Reuse the service initialized by the existing application. Enumerating
 * exports avoids loading or changing NetCtl from the UI. Fail closed when
 * multiple distinct modules report the same API name. */
static void *loaded_symbol(const char *name)
{
    OrbisKernelModule modules[256];
    size_t count = 0;
    if (sceKernelGetModuleList(modules, sizeof(modules) / sizeof(*modules), &count) < 0)
        return NULL;
    if (count > sizeof(modules) / sizeof(*modules))
        count = sizeof(modules) / sizeof(*modules);
    void *found = NULL;
    for (size_t i = 0; i < count; ++i) {
        void *candidate = NULL;
        if (sceKernelDlsym(modules[i], name, &candidate) < 0 || !candidate) continue;
        if (found && candidate != found) return NULL;
        found = candidate;
    }
    return found;
}
#else
#include <time.h>
#endif

static bool due(uint64_t now, uint64_t last, uint64_t interval)
{
    return now < last || now - last >= interval;
}

X4Ui2Network x4_ui2_network_type(uint64_t now_us)
{
#if defined(X4_OPENORBIS)
    static int32_t (*get_info)(int32_t, OrbisNetCtlInfo *);
    static X4Ui2Network network;
    static uint64_t last;
    static bool sampled;
    if (sampled && !due(now_us, last, 5000000u)) return network;
    sampled = true;
    last = now_us;
    if (!get_info)
        get_info = (int32_t (*)(int32_t, OrbisNetCtlInfo *))loaded_symbol("sceNetCtlGetInfo");
    network = X4_UI2_NETWORK_UNKNOWN;
    OrbisNetCtlInfo info = {.device = UINT32_MAX};
    if (get_info && get_info(ORBIS_NET_CTL_INFO_DEVICE, &info) == 0) {
        /* OpenOrbis declares the DEVICE query. Values 0 and 1 are the
         * wired and wireless ABI values documented by shadPS4. */
        if (info.device == 0) network = X4_UI2_NETWORK_CABLE;
        else if (info.device == 1) network = X4_UI2_NETWORK_WIFI;
    }
    return network;
#else
    (void)now_us;
    return X4_UI2_NETWORK_UNKNOWN;
#endif
}

int x4_ui2_local_minute(uint64_t now_us)
{
    static uint64_t last;
    static bool sampled;
    static int minute = -1;
    if (sampled && !due(now_us, last, 1000000u)) return minute;
    sampled = true;
    last = now_us;
#if defined(X4_OPENORBIS)
    static int32_t (*get_clock)(TimeTable *);
    static bool rtc_resolved;
    if (!rtc_resolved) {
        rtc_resolved = true;
        get_clock = (int32_t (*)(TimeTable *))loaded_symbol("sceRtcGetCurrentClockLocalTime");
        if (!get_clock) {
            int32_t handle = (int32_t)sceKernelLoadStartModule("/system/common/lib/libSceRtc.sprx",
                0, NULL, 0, NULL, NULL);
            void *address = NULL;
            if (handle >= 0 && sceKernelDlsym(handle, "sceRtcGetCurrentClockLocalTime", &address) >= 0)
                get_clock = (int32_t (*)(TimeTable *))address;
        }
    }
    /* Reserve aligned trailing room for the RTC timestamp tail. The UI
     * consumes only the documented hour/minute members. */
    union { TimeTable table; uint64_t storage[4]; } time = {0};
    minute = get_clock && get_clock(&time.table) == 0 &&
        time.table.hour < 24 && time.table.minute < 60
        ? time.table.hour * 60 + time.table.minute : -1;
#else
    time_t now = time(NULL);
    struct tm *local = now != (time_t)-1 ? localtime(&now) : NULL;
    minute = local ? local->tm_hour * 60 + local->tm_min : -1;
#endif
    return minute;
}
