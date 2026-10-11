/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    X4_NACK_SEQUENCE_INITIAL = 1,
    X4_NACK_SEQUENCE_OMISSION,
    X4_NACK_SEQUENCE_SOURCE_CHANGE,
    X4_NACK_SEQUENCE_AMBIGUOUS,
    X4_NACK_SEQUENCE_OVERFLOW
};

/* Original authenticated PT102 media only. This immutable epoch anchor is
 * published with the packet bytes; it is NOT a request or a rebind permission.
 * Packet source/raw extended seq/RTP timestamp/auth time remain separate.
 * The worker confirms the matching anchor only after track acceptance and
 * STORED without JUMP. transport_epoch comes from the transport owner, never
 * an arbitrary request. Zero means that no usable provenance was observed.
 * Only INITIAL(0->1) and validated same-source OMISSION transitions may bind.
 * No packet payload, credentials or server-delivery claim is part of this tag.
 */
typedef struct {
    uint64_t anchor_extended, previous_high_extended, anchor_us;
    uint32_t transport_epoch, sequence_epoch, previous_epoch;
    uint32_t anchor_rtp_timestamp, reason;
} X4NackSequenceTag;
