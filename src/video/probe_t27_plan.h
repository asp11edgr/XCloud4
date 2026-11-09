/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_plan.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
#include "probe_t26_realistic.h"
enum { X4_T27_BASE_COUNT=2, X4_T27_REALISTIC_COUNT=9, X4_T27_CELL_COUNT=11 };
#define X4_T27_BUDGET_US UINT64_C(600000000)
typedef struct {
 unsigned index,clip;bool reference;
 X4T26Config config;X4T26RealisticOptions presentation;
} X4T27Cell;
bool x4_t27_plan_cell(unsigned index,X4T27Cell *);
uint64_t x4_t27_plan_remaining_us(unsigned next);
const char *x4_t27_plan_mode(const X4T27Cell *);
#endif
