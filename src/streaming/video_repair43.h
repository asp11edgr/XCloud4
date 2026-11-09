/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
enum { X4_REPAIR43_NORMAL=0, X4_REPAIR43_RTX=1, X4_REPAIR43_FEC=2,
       X4_REPAIR43_MEDIA_SLOTS=512, X4_REPAIR43_FEC_SLOTS=64,
       X4_REPAIR43_PACKET_CAP=2048, X4_REPAIR43_MAX_HEADERS=3,
       X4_REPAIR43_HEADER_CAP=32, X4_REPAIR43_CLOSE_BINS=256 };
typedef struct X4VideoRepair43 X4VideoRepair43;
typedef struct {
    unsigned mode,loss_percent;
    uint32_t seed,media_ssrc,rtx_ssrc,fec_ssrc;
    uint8_t media_pt,rtx_pt,fec_pt;
    bool rtx_accepted,fec_accepted,mutable_extensions;
} X4VideoRepair43Config;
/* Emitted storage is borrowed for this callback only; copy before retaining. */
typedef void (*X4VideoRepair43Emit)(void *context,const uint8_t *packet,
    size_t bytes,unsigned method,uint64_t now_us);
typedef struct {
    uint64_t packets[3],emitted[3],format_errors[3],lost_simulated[3];
    uint64_t gaps_detected,gaps_closed[3],gaps_expired;
    uint64_t duplicate[3],late[3],rtx_empty,foreign,unsupported;
    uint64_t fec_no_missing,fec_multiple_missing,fec_cache_expired;
    uint64_t closure_samples[3],closure_p50_us[3],closure_p95_us[3],closure_max_us[3];
    bool closure_p50_finite[3],closure_p95_finite[3];
    uint64_t closure_histogram[3][X4_REPAIR43_CLOSE_BINS];
    unsigned header_count,header_bytes[X4_REPAIR43_MAX_HEADERS];
    uint8_t headers[X4_REPAIR43_MAX_HEADERS][X4_REPAIR43_HEADER_CAP];
    size_t allocated_bytes;
    uint32_t seed;
    unsigned mode,loss_percent;
} X4VideoRepair43Snapshot;
/* Single reception owner calls process/tick. snapshot is safe concurrently;
 * destroy requires that owner and all observers have been stopped/joined. */
X4VideoRepair43 *x4_video_repair43_create(const X4VideoRepair43Config *config);
void x4_video_repair43_free(X4VideoRepair43 *repair);
int x4_video_repair43_process(X4VideoRepair43 *repair,const uint8_t *packet,
    size_t bytes,uint64_t now_us,X4VideoRepair43Emit emit,void *context);
void x4_video_repair43_tick(X4VideoRepair43 *repair,uint64_t now_us);
void x4_video_repair43_snapshot(const X4VideoRepair43 *repair,
    X4VideoRepair43Snapshot *snapshot);
#ifdef __cplusplus
}
#endif
