/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint8_t media_pt,rtx_pt,fec_pt;
} X4Recovery43Offer;
typedef struct {
    uint32_t media_ssrc,rtx_ssrc,fec_ssrc;
    uint8_t media_pt,rtx_pt,fec_pt;
    bool video_found,rtx_accepted,fec_accepted,mutable_extensions;
} X4Recovery43Negotiation;
/* Mode0 is a strict no-op, including invalid/non-SDP buffers. New lines only
 * enter the unique active video section. No transport-cc is introduced. */
int x4_recovery43_sdp_offer(char *sdp,size_t capacity,unsigned mode,
    X4Recovery43Offer *offered);
/* Acceptance requires real codec mappings and group members declared by
 * a=ssrc, since the pinned peer dispatcher routes on those declarations.
 * Missing or ambiguous mappings are a nonfatal decline, never a guess. */
int x4_recovery43_sdp_answer(const char *sdp,size_t bytes,unsigned mode,
    X4Recovery43Negotiation *answer);
#ifdef __cplusplus
}
#endif
