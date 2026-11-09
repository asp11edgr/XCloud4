/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_plan.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include <stddef.h>

enum { X4_T26_PHASE_NONE, X4_T26_PHASE_QUERY_COMPUTE,
    X4_T26_PHASE_ALLOCATE_QUEUE, X4_T26_PHASE_QUERY_DECODER,
    X4_T26_PHASE_CREATE, X4_T26_PHASE_WARMUP, X4_T26_PHASE_DECODE,
    X4_T26_PHASE_FLUSH, X4_T26_PHASE_DELETE, X4_T26_PHASE_ARBITRATION,
    X4_T26_PHASE_COUNT };
enum { X4_T26_TERMINAL_NONE, X4_T26_TERMINAL_COMPLETE,
    X4_T26_TERMINAL_BUDGET, X4_T26_TERMINAL_V1_CRASH,
    X4_T26_TERMINAL_V1_FAILED_15 };
enum { X4_T26_RESUME_IDLE=0, X4_T26_RESUME_COMMITTED=1,
    X4_T26_RESUME_SKIPPED=2, X4_T26_RESUME_V1_TERMINATED=3 };
enum { X4_T26_STATE_ID_MAX=64, X4_T26_STATE_TEXT_MAX=1536 };
typedef struct {
    char build_id[X4_T26_STATE_ID_MAX+1],manifest_sha256[65];
    uint64_t generation,started_us,deadline_us,budget_used_us;
    uint64_t completed_mask,skipped_mask,closed_app_mask;
    unsigned next_cell,active_cell,active_mode,phase,terminal;
    bool active,v1_passed_15,best_ready;
    X4T26Config best;
} X4T26State;

int x4_t26_state_init(X4T26State *,const char *build_id,
    const char *manifest_sha256,uint64_t now_us);
bool x4_t26_state_valid(const X4T26State *);
bool x4_t26_state_deadline_reached(const X4T26State *,uint64_t now_us);
/* Fresh-process clock anchor; used_us is cumulative durable experiment time,
 * including the caller's conservative charge for an interrupted cell. Never
 * pass only the new process elapsed time. A smaller cumulative charge fails. */
int x4_t26_state_rebase(X4T26State *,uint64_t now_us,uint64_t used_us);
int x4_t26_state_begin(X4T26State *,unsigned cell,unsigned mode,unsigned phase);
int x4_t26_state_mark(X4T26State *,unsigned mode,unsigned phase);
int x4_t26_state_set_best(X4T26State *,const X4T26Config *);
/* Call ONLY after native readers/cleanup have safely finished. passed is
 * required to expand v1 after BOTH15Mbps modes; errors simply finish v2 cells. */
int x4_t26_state_commit_cell(X4T26State *,unsigned cell,bool passed);
/* Only between cells after cleanup; marks remaining cells skipped by budget. */
int x4_t26_state_finish_budget(X4T26State *);
/* New-process recovery. Durable result cells take precedence over an older
 * active marker. Other active v2 is skipped; active v1 TERMINATES the bank. */
int x4_t26_state_resume(X4T26State *,uint64_t durable_completed_mask,
    bool durable_v1_15_passed);
/* Bounded pure JSON, no file/network/native calls. Identity and checksum must
 * match; malformed input never partly changes *out. Success serialize returns
 * byte length excluding NUL; parse returns0. All failures return negative. */
int x4_t26_state_serialize(const X4T26State *,char *,size_t capacity);
int x4_t26_state_parse(const char *,size_t bytes,const char *expected_build,
    const char *expected_manifest,X4T26State *out);
#endif
