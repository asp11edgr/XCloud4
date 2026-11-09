/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "video_repair43.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t epoch;
    unsigned requested_mode, requested_loss;
    bool configured, closing;
    uint64_t receive_calls, unauthenticated, receive_contention, engine_errors;
    uint64_t incomplete_au, discarded_au, pli_requested, freezes;
    /* Actual worker admission outcomes; different from the engine cache TTL. */
    uint64_t delivery_late[3], delivery_duplicate[3], delivery_unknown;
    X4VideoRepair43Config accepted;
    X4VideoRepair43Snapshot repair;
} X4Recovery43Snapshot;
/* Session-owner setup only. No registry lookup/lock on packet reception. */
/* Bridge-only receive hook: reports validated original media after simulated
 * loss (Lab) and before duplicate/late admission. No packet bytes are copied. */
int x4_video_repair43_process_observed(X4VideoRepair43 *repair,const uint8_t *packet,
    size_t bytes,uint64_t now_us,X4VideoRepair43Emit emit,void (*observe_original)(void *),
    void *context);
int x4_recovery43_begin(uint32_t epoch, unsigned mode, unsigned loss, uint32_t seed);
int x4_recovery43_answer(uint32_t epoch, const X4VideoRepair43Config *accepted);
bool x4_recovery43_snapshot(uint32_t epoch, X4Recovery43Snapshot *snapshot);
/* Existing NACK owner calls this; logging is outside registry/receiver locks. */
void x4_recovery43_poll(uint32_t epoch, uint64_t now_us, bool final);
/* All four values are incremental DELTAS (atomic fetch-add), never absolute
 * snapshots. Zero does not reset a counter. PLI counts attempted native RTCP. */
void x4_recovery43_note_media(uint32_t epoch, uint64_t incomplete,
    uint64_t discarded, uint64_t pli, uint64_t freezes);
enum { X4_RECOVERY43_DELIVERY_LATE=1, X4_RECOVERY43_DELIVERY_DUPLICATE=2 };
/* Read-only exact sequence+RTP timestamp lookup; no cache mutation. */
void x4_recovery43_delivery(uint32_t epoch, uint16_t sequence,
    uint32_t rtp_timestamp, unsigned reason);
void x4_recovery43_end(uint32_t epoch);
#ifdef __cplusplus
}
#endif
