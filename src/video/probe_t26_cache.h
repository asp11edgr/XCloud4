/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_t26_plan.h"
#if X4_VIDEO_PROBE && X4_PROBE_T26
#include "probe_t26_samples.h"
#include <stddef.h>
#include <stdatomic.h>
enum { X4_T26_WARMUP_AUS=180, X4_T26_MEASURED_IMAGES=600,
    X4_T26_SOURCE_AUS=781 };
/* Immutable compressed data only. Decode still copies into its existing
 * native WB_ONION input ledger; no output image is cached here. */
typedef struct {
    uint8_t *data;
    size_t bytes,allocated_bytes,v1_input_arena_bytes;
    uint64_t offset[X4_T26_SOURCE_AUS],pts_us[X4_T26_SOURCE_AUS];
    uint32_t size[X4_T26_SOURCE_AUS];
    bool verified;
} X4T26ClipData;
/* Zero initialize before the first load; close returns this empty state. */
int x4_t26_clip_load(X4T26ClipData *,const X4T26GeneratedSample *,const atomic_bool *);
void x4_t26_clip_close(X4T26ClipData *);
const uint8_t *x4_t26_clip_au(const X4T26ClipData *,unsigned ordinal,size_t *,uint64_t *);
#endif
