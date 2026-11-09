/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#ifndef X4_VIDEO_PROBE
#define X4_VIDEO_PROBE 0
#endif
#ifndef X4_PROBE_T26
#define X4_PROBE_T26 0
#endif
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <stdbool.h>
#include <stdint.h>

enum { X4_T26_API_V1=1, X4_T26_API_V2=2 };
enum { X4_T26_CLIP_1080_8, X4_T26_CLIP_1080_15, X4_T26_CLIP_1080_20,
    X4_T26_CLIP_1080_27, X4_T26_CLIP_720_8, X4_T26_CLIP_COUNT };
enum { X4_T26_MODE_MAXIMUM, X4_T26_MODE_PACED, X4_T26_MODE_COUNT };
enum { X4_T26_GROUP_FACTORS, X4_T26_GROUP_BASE_EXTRA, X4_T26_GROUP_BEST,
    X4_T26_GROUP_REALISTIC, X4_T26_GROUP_V1 };
enum { X4_T26_FAMILY_BASE, X4_T26_FAMILY_DEPTH, X4_T26_FAMILY_MEMORY,
    X4_T26_FAMILY_AFFINITY, X4_T26_FAMILY_PRIORITY, X4_T26_FAMILY_PIPE,
    X4_T26_FAMILY_RESOURCE, X4_T26_FAMILY_COUNT };
enum { X4_T26_FACTOR_COUNT=13, X4_T26_FACTOR_CELLS=26,
    X4_T26_BASE_EXTRA_FIRST=26, X4_T26_BEST_FIRST=29,
    X4_T26_REALISTIC_FIRST=34, X4_T26_V1_FIRST=38,
    X4_T26_CELL_COUNT=43, X4_T26_TOTAL_MODES=82 };
#define X4_T26_BUDGET_US UINT64_C(1200000000)
#define X4_T26_NOMINAL_MODE_US UINT64_C(13000000)

typedef struct {
    unsigned api,resource_type,depth;
    int output_memory_type;
    uint64_t affinity_mask;
    int priority;
    unsigned pipe_id,queue_id;
    bool realistic;
} X4T26Config;
typedef struct {
    unsigned index,group,clip,factor;
    X4T26Config config;
} X4T26Cell;
/* One committed logical cell after BOTH modes. p95 is the larger of its two
 * Decode p95 values. A rejected/crashed cell is completed but not accepted.
 * Incomplete, zero-sample, erroneous or hash-mismatching cells cannot win. */
typedef struct {
    bool completed,accepted,hashes_ok;
    uint64_t errors,decode_p95_us;
} X4T26Outcome;
typedef struct {
    X4T26Cell cells[X4_T26_CELL_COUNT];
    X4T26Outcome factors[X4_T26_FACTOR_CELLS];
    X4T26Config best;
    uint32_t selected_factors;
    bool best_ready;
} X4T26Plan;

void x4_t26_config_base(X4T26Config *);
bool x4_t26_config_valid(const X4T26Config *);
bool x4_t26_config_equal(const X4T26Config *,const X4T26Config *);
int x4_t26_factor_config(unsigned factor,X4T26Config *);
unsigned x4_t26_factor_family(unsigned factor);
const char *x4_t26_clip_id(unsigned clip);
void x4_t26_plan_init(X4T26Plan *);
const X4T26Cell *x4_t26_plan_cell(const X4T26Plan *,unsigned index);
unsigned x4_t26_cell_first_mode(const X4T26Cell *);
unsigned x4_t26_cell_mode_count(const X4T26Cell *);
int x4_t26_plan_note(X4T26Plan *,unsigned cell,const X4T26Outcome *);
/* Requires all26 factor cells committed. Winner improves strictly at BOTH
 * bitrates, with no errors/hash differences; ties retain base/earlier factor.
 * Each family is chosen independently; combined acceptance is measured later. */
int x4_t26_plan_select_best(X4T26Plan *);
int x4_t26_plan_restore_best(X4T26Plan *,const X4T26Config *);
uint64_t x4_t26_plan_nominal_remaining_us(const X4T26Plan *,unsigned next_cell);
#endif
