/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_result.h"
#include "probe_t27_plan.h"
#include "stream_diagnostics.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26 && X4_PROBE_T27
/* Same measured/native structures as T26, new 11-cell metadata and one raw
 * maximum-speed reference mode. No native logic is implemented here. */
int x4_t27_native_result_encode(const X4T26CellResult *,char *,size_t);
int x4_t27_native_result_write(FILE *,const X4T26CellResult *);
int x4_t27_native_result_read(const char *,size_t,const char *,const char *,X4T26RecoveredResult *);
int x4_t27_result_write(FILE *,const X4T26CellResult *,const X4PipelinePresentationSummary *);
int x4_t27_result_read(const char *,size_t,const char *,const char *,X4T26RecoveredResult *,X4PipelinePresentationSummary *);
#endif
