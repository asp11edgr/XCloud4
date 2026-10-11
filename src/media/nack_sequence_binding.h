/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "../streaming/nack_sequence.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum X4NackBindingResult {
    X4_BINDING_NONE, X4_BINDING_CANDIDATE, X4_BINDING_COMMITTED,
    X4_BINDING_DUPLICATE, X4_BINDING_STALE, X4_BINDING_INVALID,
    X4_BINDING_PROOF, X4_BINDING_UNPROVEN
};
typedef struct {
    uint64_t announcements, candidates, commits, duplicates, stale, invalid;
    uint64_t proofs, unproven, mapping_checks, unmapped_provenance, mapped_offers;
} X4NackBindingCounters;
typedef struct {
    X4NackSequenceTag committed, candidate, proof;
    uint64_t source, canonical_offset, consumed_raw_high, proof_authenticated_us;
    uint32_t owner_epoch;
    bool bound, candidate_valid, proof_valid;
    X4NackBindingCounters counters;
} X4NackSequenceBinding;

/* Worker-only, fixed storage: one announced candidate and one earlier STORED
 * anchor proof. The owner epoch comes from the already owner-bound inbox and
 * source from the accepted Track. No request can initialize either value.
 * The original PT102 tag is an announcement, not permission to bind.
 * Only an exact anchor packet (raw sequence, RTP timestamp and immutable tag)
 * actually STORED without JUMP can prove a new epoch. Missing/LATE/JUMP anchors
 * fail closed. A proof may precede its announcement; neither alone commits.
 *
 * OMISSION preserves the canonical position domain using the old epoch's
 * known origin and previous high. A modular halfspan, backward anchor, or old
 * consumed media ahead of that previous high cannot prove a mapping. Offset
 * arithmetic is checked and remains a multiple of65536. No deadline or budget
 * is restarted. All clocks are the common monotonic process clock.
 */
void x4_nack_binding_init(X4NackSequenceBinding *, uint32_t owner_epoch,
    uint64_t accepted_track_source);
int x4_nack_binding_announce(X4NackSequenceBinding *, const X4NackSequenceTag *,
    uint64_t source, uint64_t now_us);
int x4_nack_binding_stored(X4NackSequenceBinding *, const X4NackSequenceTag *,
    uint64_t source, uint64_t raw_extended, uint16_t wire_sequence,
    uint32_t rtp_timestamp, uint64_t authenticated_us, uint64_t now_us,
    uint64_t *canonical_extended);
bool x4_nack_binding_map(X4NackSequenceBinding *, const X4NackSequenceTag *,
    uint64_t source, uint64_t raw_extended, uint16_t wire_sequence,
    uint32_t rtp_timestamp, uint64_t authenticated_us, uint64_t now_us,
    uint64_t *canonical_extended);
bool x4_nack_binding_offer_map(X4NackSequenceBinding *, uint64_t source,
    uint32_t transport_epoch, uint32_t sequence_epoch, uint64_t first_raw,
    uint64_t missing_mask, uint64_t *first_canonical);
bool x4_nack_binding_waiting(const X4NackSequenceBinding *, uint64_t source,
    uint32_t transport_epoch, uint32_t sequence_epoch);

#ifdef __cplusplus
}
#endif
