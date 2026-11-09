/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#ifndef X4_VIDEO_PROBE
#define X4_VIDEO_PROBE 0
#endif
#ifndef X4_PROBE_T26
#define X4_PROBE_T26 0
#endif
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <stddef.h>
#include <stdint.h>
#include "probe_t26_plan.h"

/* The owner durably records the active cell before a risky native call.
 * A failed marker prevents the call; it never authorizes an untracked test. */
typedef int (*X4T26BeforeNative)(void *context, const char *operation);
typedef struct {
    X4T26Config config;
    X4T26BeforeNative before;
    void *context;
} X4T26NativeOptions;

typedef struct {
    int64_t physical_offset;
    uint64_t bytes;
    int memory_type;
} X4T26Allocation;
enum { X4_T26_ALLOCATION_MAX = 16 };
#endif
