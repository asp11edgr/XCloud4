/* SPDX-License-Identifier: GPL-3.0-only */
/* The SDK exposes musl clock IDs; libc++ was compiled with MONOTONIC=1.
 * Native PS4 uses FreeBSD MONOTONIC=4. Translate at the symbol boundary so
 * both precompiled libc++ and new C/C++ objects use actual native clocks. */
#include <time.h>
#include <errno.h>
#include <stdint.h>
extern int32_t sceKernelClockGettime(int32_t, struct timespec *);
extern int32_t sceKernelClockGetres(int32_t, struct timespec *);
_Static_assert(sizeof(struct timespec)==16,
               "Native PS4 clock calls require the x86_64 timespec ABI");

static int native_clock(clockid_t clock)
{
    switch (clock) {
    case CLOCK_REALTIME: return 0;
    case CLOCK_MONOTONIC: return 4;
    case CLOCK_THREAD_CPUTIME_ID: return 14;
    default: return -1;
    }
}
int clock_gettime(clockid_t clock, struct timespec *time)
{
    int native = native_clock(clock);
    if(native < 0 || !time){errno=EINVAL;return -1;}
    int32_t rc=sceKernelClockGettime(native,time);
    if(rc<0){errno=(int)((uint32_t)rc&0xffffu);return -1;}
    return rc;
}
int clock_getres(clockid_t clock, struct timespec *time)
{
    int native = native_clock(clock);
    if(native < 0){errno=EINVAL;return -1;}
    int32_t rc=sceKernelClockGetres(native,time);
    if(rc<0){errno=(int)((uint32_t)rc&0xffffu);return -1;}
    return rc;
}
