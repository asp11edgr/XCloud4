/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t27_plan.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
#include <stddef.h>
enum { X4_T27_PHASE_NONE,X4_T27_PHASE_QUERY,X4_T27_PHASE_CREATE,X4_T27_PHASE_DECODE,X4_T27_PHASE_DELETE,X4_T27_PHASE_COUNT };
enum { X4_T27_TERMINAL_NONE,X4_T27_TERMINAL_COMPLETE,X4_T27_TERMINAL_STOPPED,X4_T27_TERMINAL_INTERRUPTED };
enum { X4_T27_STATE_TEXT_MAX=1024 };
typedef struct {
 char build_id[65],manifest_sha256[65];
 uint64_t generation,started_us,deadline_us,budget_used_us,completed_mask,skipped_mask,closed_app_mask;
 unsigned next_cell,active_cell,active_mode,phase,terminal;bool active;
} X4T27State;
int x4_t27_state_init(X4T27State *,const char *,const char *,uint64_t);
bool x4_t27_state_valid(const X4T27State *);
int x4_t27_state_rebase(X4T27State *,uint64_t,uint64_t);
int x4_t27_state_begin(X4T27State *,unsigned,unsigned,unsigned);
int x4_t27_state_mark(X4T27State *,unsigned,unsigned);
int x4_t27_state_commit_cell(X4T27State *,unsigned,bool);
int x4_t27_state_finish_budget(X4T27State *);
int x4_t27_state_resume(X4T27State *,uint64_t);
int x4_t27_state_serialize(const X4T27State *,char *,size_t);
int x4_t27_state_parse(const char *,size_t,const char *,const char *,X4T27State *);
#endif
