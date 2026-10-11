#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only X4PROG1/v1 and X4PROG2/v2 monitor analysis; no playback I/O.

JSON output is PRIVATE evidence: it contains local numeric source identities.
Each counter is an observation, not provider frame age or a packet-loss claim.
"""
import argparse
from bisect import bisect_left, bisect_right
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

MAX_BYTES = 12 * 1024 * 1024
U64 = (1 << 64) - 1
HEADER = struct.Struct('<128Q')
RECORD = struct.Struct('<QQQIHH')
MAGIC = b'X4PROG1\0'
MAGIC2 = b'X4PROG2\0'
CAPS = {10: 12000, 12: 65536, 14: 8192, 16: 8, 89: 64}
FIXED = {1: 1, 2: 1024, 9: 100000, 34: 12000, 35: 65536,
         36: 8192, 37: 8, 38: 4096, 39: 512, 40: 64, 41: 16,
         42: 32, 43: 640, 44: 32, 45: 32, 46: 8, 47: 3, 91: 32}
H_NAMES = {1: 'version', 3: 'local_session', 4: 'created_us', 5: 'ended_us',
    6: 'start_rc', 7: 'join_rc', 8: 'configured_readers', 9: 'sampler_period_us',
    10: 'sample_count', 11: 'sample_omitted', 12: 'cadence_count',
    13: 'cadence_omitted', 14: 'history_count', 15: 'history_overwrites',
    16: 'window_count', 17: 'window_omitted', 18: 'event_attempts',
    19: 'event_drops', 20: 'missed_periods', 21: 'max_sampler_lateness_us',
    22: 'new_count', 23: 'first_new_us', 24: 'last_new_us',
    25: 'closed_gaps_gt_100ms', 26: 'cadence_identity_errors',
    27: 'interval_count', 28: 'active_duration_us', 29: 'clock_errors',
    30: 'rx_source_cap_omitted', 31: 'rx_sequence_observation_omitted',
    32: 'lifecycle_flags', 33: 'coverage_flags',
    64: 'actor_snapshot_unstable', 65: 'queue_snapshot_unstable',
    66: 'cadence_snapshot_unstable', 67: 'supplied_queue_snapshot_used',
    68: 'manual_epoch_marks', 69: 'open_gap_at_stop', 70: 'retained_gap_details',
    71: 'queue_accepted_callbacks', 86: 'sampler_detail_gate_omitted',
    87: 'sample_total', 88: 'sampler_clock_errors',
    89: 'phase_ledger_count', 90: 'phase_ledger_omitted', 91: 'phase_ledger_record_bytes',
    92: 'primary_RX_sample_unstable'}
CONFIG = ('width height max_fps bitrate_kbps readers budget_us decode_limit '
    'queue_capacity reorder_capacity reorder_depth reorder_wait_us h264_profile '
    'max_fs max_mbps reserved0 reserved1').split()
SAMPLE = ('target_us actual_us lateness_us missed_periods event_drops '
    'history_overwrites coherence_bits queue_depth oldest_arrival_us queue_flags '
    'video_callbacks video_valid_callbacks video_bytes last_video_callback_us '
    'last_valid_video_us primary_video_sequence_encoded queue_accepted_callbacks queue_drop_attempts '
    'valid_AU_attempts Decode_end_attempts native_valid_output_attempts '
    'RGB_publication_attempts flip_submit_success_attempts NEW_count last_NEW_us '
    'last_NEW_generation closed_gt100ms_gaps AU_reset_attempts PLI_attempts '
    'PLI_result_errors observation_end_us primary_video_source_key').split()
SAMPLE_EXTRA = ('queue_pop_attempts reorder_state_encoded reorder_emit_attempts reorder_hole_attempts '
    'Decode_begin_attempts copy_begin_attempts copy_end_attempts convert_begin_attempts '
    'convert_end_attempts last_valid_AU_us last_native_output_us last_RGB_publication_us '
    'last_Decode_end_us last_copy_end_us last_convert_end_us discarded_open_AU_attempts').split()
WINDOW = ('window_id record_count coverage_flags gap_id gap_start_us opened_us '
    'closed_gap_end_us frozen_us freeze_reason event_drops_at_open '
    'event_drops_at_freeze pre_records last_gap_id closed_gap_count '
    'reserved14 reserved15').split()
ACTORS = 'worker main display helper0 helper1 helper2 rtc session'.split()
PHASES = ('UNKNOWN WAIT_DATA ASSEMBLE DECODE COPY CONVERT WAIT_SURFACE COPY_WAIT '
    'DRAW FLIP_SUBMIT FLIP_WAIT MAILBOX STARTUP STOPPING MAIN_POLL MAIN_STATUS MAIN_IDLE').split()
EPOCHS = ('unknown', 'loading', 'stable', 'transition')
COVERAGE = ('sample_cap cadence_cap history_overwrite window_storage_cap '
    'event_admission_drop snapshot_unstable window_time_cap start_disabled '
    'join_failed actor_unstable rx_sequence_unknown clock window_event_cap '
    'open_at_stop short_prehistory epoch_ledger_cap').split()
EVENTS = {}
for base, words in (
    (0x100, 'AU_BEGIN AU_COMPLETE AU_RESET WAIT_IDR IDR_ACCEPT PLI_REQUEST PLI_DISPATCH QUEUE_POP QUEUE_ACCEPT QUEUE_DROP REORDER_INSERT REORDER_EMIT REORDER_HOLE REORDER_DROP WORKER_YIELD MEDIA_START MEDIA_STOP SOURCE_EPOCH'),
    (0x120, 'DAMAGE_FIRST IDR_SEEN SUBMIT_BEGIN SUBMIT_END RESET_TOTAL DAMAGE_TOTAL WAIT_BEGIN WAIT_END POP_CONTENDED'),
    (0x200, 'DECODE_BEGIN DECODE_END OUTPUT_VALID OUTPUT_NONE OUTPUT_REJECT OUTPUT_SUPERSEDED COPY_BEGIN COPY_END COPY_OWNER_ELAPSED COPY_HELPER_ELAPSED COPY_WAIT_ELAPSED COPY_CHECK CONVERT_BEGIN CONVERT_END RGB_PUBLICATION RGB_SUPERSEDED'),
    (0x220, 'OWNER_SPAN_BEGIN OWNER_SPAN_END HELPER_SPAN_BEGIN HELPER_SPAN_END HELPER_WAIT_BEGIN HELPER_WAIT_END BYTECHECK_BEGIN BYTECHECK_END OUTPUT_GEOMETRY VIDEO_ERROR VIDEO_START VIDEO_STOP FEED_REJECT PRESERVE_BEGIN PRESERVE_END COPY_CONFIG'),
    (0x300, 'DRAW_BEGIN DRAW_END DRAW_SKIP FLIP_SUBMIT GNM_DONE FLIP_MATCH PRESENT_NEW PRESENT_REPEAT FLIP_FAIL'),
    (0x400, 'RX_VALID RX_REJECT RX_CALLBACK'),
    (0x500, 'SESSION_START ACTIVE_CHANGE GAP_BEGIN GAP_CLOSED WINDOW_OPEN WINDOW_FROZEN SESSION_END'),
    (0x600, 'AU_VALID REORDER_STATE REORDER_WINDOW_JUMP RGB_CONSUME FLIP_CALL_BEGIN PLI_ATTEMPT PLI_RESULT SESSION_PHASE'),
    (0x610, 'SAMPLER_START SAMPLER_SAMPLE MON_GAP_BEGIN MON_GAP_END EPOCH ACTIVE SAMPLER_STOP WINDOW_CLOSE DISABLED PHASE_CHANGE RX_OBSERVATION'),
):
    EVENTS.update((base + i, name) for i, name in enumerate(words.split()))
EVENTS.update({0x608: 'AU_STRUCTURAL_COMPLETE', 0x609: 'AU_FILTER_REJECT'})
TRANSPORT = ('DATAGRAM ICE_DELIVER DTLS_QUEUE_ACCEPT DTLS_QUEUE_POP SRTP_INPUT '
    'SRTP_VALID PEER_DISPATCH TRACK_INCOMING TRACK_QUEUE_ATTEMPT TRACK_DELIVERED '
    'APP_CALLBACK RTX_NORMALIZED TCP_FRAMED PEER_HANDLER_OUT RTCP_VALID TURN_DECAP').split()
CRITICAL = ('UNKNOWN SEQUENCE_GAP LATE_POSITION REJECT RECOVERY_BEGIN RECOVERY_END '
    'PLI_LOCAL PLI_ATTEMPT PLI_RESULT AU_FILTER IDR_SEEN AU_PARAMETERS '
    'PRESENT_GAP_BEGIN PRESENT_GAP_END SEQUENCE_AMBIGUOUS AU_DISCARD '
    'TRANSPORT_DELAY RTC_SETTING NACK_DETECT NACK_REQUEST NACK_DISPATCH '
    'NACK_SRTCP_BEGIN NACK_SRTCP_RESULT NACK_LOCAL_SEND_RESULT NACK_AUTH_RETURN '
    'NACK_REORDER_RESULT NACK_AU_CONTRIBUTION NACK_STATE NACK_SUPPRESS NACK_COUNTER '
    'NACK_DISPATCH_MASK NACK_GRACE').split()
NACK_COUNTERS = ('observations wrong_payload_type observations_after_close sources_started '
    'ssrc_changes forward_packets backward_packets duplicate_packets backward_untracked '
    'before_epoch ambiguous_halfspan extended_overflow gap_ranges missing_positions '
    'requests_reserved disabled_ranges disabled_positions oversized_ranges oversized_positions '
    'budget_suppressed_ranges budget_suppressed_positions watched_ranges_evicted '
    'pending_ranges_evicted pending_positions_evicted matched_arrivals first_arrivals '
    'duplicate_arrivals completed_ranges pending_positions ssrc_retired_ranges '
    'ssrc_retired_positions ambiguity_retired_ranges ambiguity_retired_positions '
    'close_retired_ranges close_retired_positions observer_discontinuities '
    'omission_retired_ranges omission_retired_positions clock_regressions').split()
NACK_EXTRA_COUNTERS = {100: 'state_gate_omissions', 101: 'invalid_RTP',
    102: 'non_authenticated_or_RTX_flagged', 103: 'dispatch_queue_full', 104: 'dispatch_gated',
    105: 'handler_duplicates', 106: 'undispatched_on_close',
    107: 'AU_ticket_omissions', 108: 'reorder_ticket_omissions',
    109: 'delivery_ticket_set_get_operation_omissions',
    110: 'sequence_control_inbox_omissions', 111: 'observer_resets_consumed'}
NACK_AU_STAGES = ('in_progress_packet_contribution', 'structurally_complete',
    'accepted_by_recovery_for_Decode', 'recovery_filter_rejected', 'discarded')
NACK_SUPPRESS = {1: 'experiment_disabled', 2: 'range_cap', 3: 'session_request_cap',
    4: 'range_no_longer_pending', 5: 'watch_range_not_retained',
    6: 'feedback_not_confirmed', 7: 'closing', 8: 'dispatch_slot_already_queued'}
NACK_REORDER = {0: 'stored', 1: 'late_or_occupied_slot', 2: 'window_jump'}
NACK_GRACE_SUBKINDS = {1: 'CONTROL_REQUEST', 2: 'CONTROL_OUTCOME', 3: 'OFFER',
    4: 'RELEASE', 5: 'CALLBACK', 6: 'QUEUE_POP', 7: 'WORKER_ACCEPT',
    8: 'WORK_COST', 9: 'QUEUE_AGE', 10: 'COUNTER', 11: 'HOLD_BEGIN', 12: 'ABANDON',
    13: 'SEQUENCE_ANNOUNCE', 14: 'SEQUENCE_ANCHOR_CLOCK',
    15: 'OBSERVER_OMISSION', 16: 'OBSERVER_RESET',
    17: 'BIND_CANDIDATE', 18: 'BIND_PROOF', 19: 'BIND_COMMIT',
    20: 'BIND_OR_CONTROL_REJECT', 21: 'REQUEST_CANONICAL_MAP'}
NACK_GRACE_OFFER = ('admitted disabled stale_session closed invalid duplicate '
    'stale_source expired active_cap overlap clock deadline_overflow').split()
NACK_GRACE_RELEASE = ('none accepted deadline capacity passed source_or_reorder_reset close '
    'transport clock explicit').split()
NACK_GRACE_COUNTERS = {100: 'control_inbox_admitted', 101: 'control_inbox_omitted',
    102: 'control_inbox_stale', 103: 'worker_controls_consumed', 104: 'offers',
    105: 'ranges_admitted', 106: 'accepted_worker_positions', 107: 'deadline_ranges',
    108: 'capacity_ranges', 109: 'completed_ranges', 110: 'stale_source_offers',
    111: 'expired_offers', 112: 'overlap_positions', 113: 'active_cap_offers',
    114: 'configured_grace_ms', 115: 'configured_rumble',
    116: 'binding_announcements', 117: 'binding_candidates', 118: 'binding_commits',
    119: 'binding_duplicates', 120: 'binding_stale', 121: 'binding_invalid',
    122: 'binding_proofs', 123: 'binding_unproven', 124: 'binding_mapping_checks',
    125: 'binding_unmapped_provenance', 126: 'binding_offer_maps',
    127: 'binding_repeated_detail_suppressed', 128: 'control_fifo_pending_at_close'}
T_COUNTER = ('packet_observations bytes last_us RTP_header_metadata_observations '
    'other_or_invalid_metadata_observations gap_events forward_skipped_positions '
    'backward_observations duplicate_observations sequence_gate_omitted '
    'source_slot_omitted range_evictions later_missing_position_observations '
    'half_wrap_ambiguities known_previous_clock_observations max_observed_delay_us').split()
T_SOURCE = ('key packet_observations bytes last_us first_us high_water_sequence_encoded '
    'sequence_us forward_skipped_positions gap_events backward_observations '
    'duplicate_observations sequence_gate_omitted later_missing_position_observations '
    'range_evictions half_wrap_ambiguities dirty_after_omission next_range_slot '
    'retained_range_count extended_high_water_sequence').split()
REJECT_REASON = ('UNKNOWN SOCKET_ERROR EMPTY_DATAGRAM SOCKET_POLL_ERROR ICE_INACTIVE '
    'STUN_PARSE ICE_UNKNOWN_SOURCE ICE_UNEXPECTED DTLS_QUEUE_FULL_OR_STOPPING MEDIA_SHORT '
    'SRTP_ERROR SRTCP_ERROR DEMUX_UNKNOWN PEER_HANDLER_EXCEPTION NO_TRACK '
    'TRACK_DIRECTION TRACK_HANDLER_EXCEPTION TRACK_QUEUE_FULL RTP_SHORT RTP_VERSION '
    'RTP_PAYLOAD RTX_UNWRAP_UNAVAILABLE APP_INVALID APP_NO_SINK ICE_CALLBACK_EXCEPTION '
    'SOCKET_WOULD_BLOCK SOCKET_FAIRNESS TURN_INVALID SRTP_AUTH_FAIL '
    'SRTP_REPLAY_FAIL SRTP_REPLAY_OLD').split()
# These enum slots have no emitting hook in the pinned 0.7.32 patch. The
# allocated counter cells still exist; a raw zero is not a measured zero.
UNINSTRUMENTED_REJECT = {
    24: 'No hook in the existing IceTransport receive-callback exception branch.',
    26: 'No hook in the existing UDP/TCP fairness-limit continuation branches.',
}
RESET = ('normal_submitted timestamp_change marker_incomplete waiting_recovery '
    'parameter_prefix_capacity feed_error coalesced_ingress_gap new_track '
    'reorder_jump reorder_hole').split()


def signed(n, bits=64):
    return n - (1 << bits) if n & (1 << (bits - 1)) else n


def flags(n):
    result = [name for i, name in enumerate(COVERAGE) if n & (1 << i)]
    if n >> len(COVERAGE):
        result.append('unknown_bits')
    return result


def distribution(values):
    values = sorted(values)
    return {'count': len(values), 'p50_us': values[math.ceil(.5 * len(values))-1] if values else None,
            'p95_us': values[(95 * len(values)+99)//100-1] if values else None,
            'max_us': values[-1] if values else None}


def histogram_bounds(buckets):
    total, result = sum(buckets), {'count': sum(buckets), 'exact': False}
    for name, numerator, denominator in (('p50', 1, 2), ('p95', 95, 100), ('max', 1, 1)):
        goal, acc, found = (total*numerator+denominator-1)//denominator, 0, None
        if total:
            for i, count in enumerate(buckets):
                acc += count
                if acc >= goal:
                    found = [0 if i == 0 else 1 << i,
                             None if i == 31 else (1 << (i+1))-1]
                    break
        result[name + '_us_bounds'] = found
    return result


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def words(self, count):
        return self.unpack(struct.Struct('<%dQ' % count))

    def unpack(self, layout):
        if self.at + layout.size > len(self.data):
            raise ValueError('Truncated progress trace at byte %d' % self.at)
        result = layout.unpack_from(self.data, self.at)
        self.at += layout.size
        return result

    def records(self, count):
        if count > 8192:
            raise ValueError('Record count exceeds capacity')
        return [dict(zip(('t_us', 'a', 'b', 'ordinal', 'event', 'flags'),
                         self.unpack(RECORD))) for _ in range(count)]


def parse_critical(r):
    ch = r.words(32)
    fixed = {0: int.from_bytes(b'X4CRIT2\0', 'little'), 1: 2, 2: 256, 3: 64,
        4: 4096, 9: 16, 10: 16, 11: 24, 12: 32, 16: 32, 17: 16,
        18: 512, 19: 3072, 20: 1024, 24: 16, 25: 10000, 26: 8}
    if any(ch[i] != value for i, value in fixed.items()) or any(ch[27:]):
        raise ValueError('Unsupported critical appendix layout')
    if ch[5] > 4096:
        raise ValueError('Critical count exceeds capacity')
    attempted, omitted = r.words(32), r.words(32)
    stages = [r.words(16) for _ in range(16)]
    rejected = [r.words(32) for _ in range(16)]
    delay_omitted, au_filter_reasons = r.words(16), r.words(8)
    sources = [[r.words(24) for _ in range(16)] for _ in range(16)]
    records = []
    for _ in range(ch[5]):
        t, identity, encoded, source, a, b, c, ordinal = r.words(8)
        event, stage, provenance = encoded & 65535, (encoded >> 16) & 65535, encoded >> 32
        # critical31 stage is a bounded grace/binding subkind, not a transport
        # location. The existing v2 record/counter capacities stay unchanged.
        stage_limit = 22 if event == 31 else 16
        if event >= 32 or (stage >= stage_limit and stage != 65535):
            raise ValueError('Invalid critical type/stage')
        records.append(dict(t_us=t, identity=identity, event=event, stage=stage,
            flags=provenance, source=source, a=a, b=b, c=c, ordinal=ordinal))
    if sum(attempted) & U64 != ch[6] or sum(omitted) & U64 != (ch[7]+ch[8]) & U64:
        raise ValueError('Critical independent counter sums disagree')
    if (ch[5]+ch[7]+ch[8]) & U64 != ch[6] or (ch[5]+ch[8]) & U64 != ch[23]:
        raise ValueError('Critical retained/omitted ordinals disagree')
    if any(b > a for a, b in zip(attempted, omitted)):
        raise ValueError('Critical omissions exceed attempts')
    first_ordinal = ch[23]-ch[5]+1
    if any(row['ordinal'] != first_ordinal+i for i, row in enumerate(records)):
        raise ValueError('Noncontiguous critical ring ordinals')
    retained_by_type = Counter(row['event'] for row in records)
    if any(retained_by_type[i] != a-b for i, (a, b) in enumerate(zip(attempted, omitted))):
        raise ValueError('Critical per-event retained counters disagree')
    for stage_sources in sources:
        keys = []
        for row in stage_sources:
            if any(row[19:]):
                raise ValueError('Nonzero transport source reserved words')
            if not row[0]:
                if any(row):
                    raise ValueError('Nonzero unused transport source')
                continue
            if not row[0] & (1 << 63) or row[0] & ~((1 << 63) | (127 << 32) | 0xffffffff) or row[5] > 0x1ffff or row[15] > 1 or row[17] > 16:
                raise ValueError('Invalid transport source identity/state')
            keys.append(row[0])
        if len(keys) != len(set(keys)):
            raise ValueError('Duplicate transport source key')
    for row in records:
        if row['event'] == 1 and (not 0 < row['c'] < 32768 or
                row['b'] < row['a'] or row['b']-row['a'] != row['c']):
            raise ValueError('Invalid modular missing range')
    return dict(header=ch, attempted=attempted, omitted=omitted, stages=stages,
        rejected=rejected, delay_omitted=delay_omitted, sources=sources,
        au_filter_reasons=au_filter_reasons, records=records)


def parse(data):
    """Accept only the bounded versioned ABI. Never repair input bytes."""
    if len(data) > MAX_BYTES:
        raise ValueError('Progress trace exceeds 12 MiB read bound')
    r = Reader(data)
    h = r.words(128)
    version = 2 if data[:8] == MAGIC2 else 1
    if data[:8] not in (MAGIC, MAGIC2):
        raise ValueError('Unsupported progress magic')
    for index, value in FIXED.items():
        if h[index] != (version if index == 1 else value):
            raise ValueError('Unsupported layout at header word %d' % index)
    if any(h[i] > cap for i, cap in CAPS.items()):
        raise ValueError('Header count exceeds capacity')
    if h[8] not in (1, 2, 4):
        raise ValueError('Invalid configured reader count')
    if not h[32] & 4 or signed(h[7]) < 0 or (h[32] & 1 and not h[32] & 2):
        raise ValueError('Trace not ended and joined/quiescent')
    if h[32] & ~15:
        raise ValueError('Unknown lifecycle bits')
    # Build/product occupy words96..111. Reserved words must not hide data.
    reserved = [63] + list(range(93, 96)) + list(range(112, 128))
    if any(h[i] for i in reserved):
        raise ValueError('Nonzero reserved header words')
    strings = {}
    for key, offset in (('build_id', 768), ('product_version', 832)):
        field = data[offset:offset+64]
        nul = field.find(b'\0')
        if nul < 0 or any(field[nul:]):
            raise ValueError('Unterminated or nonzero-padded identity')
        text = field[:nul]
        if any(c < 32 or c > 126 for c in text):
            raise ValueError('Non-ASCII identity')
        strings[key] = text.decode('ascii')
    config = dict(zip(CONFIG, r.words(16)))
    if config['reserved0'] or config['reserved1']:
        raise ValueError('Nonzero config reserved words')
    if config['readers'] != h[8]:
        raise ValueError('Header/config reader counts disagree')
    attempted, dropped = r.words(512), r.words(512)
    resets, histogram, exposure = r.words(64), r.words(32), r.words(8)
    phase_rows = [r.words(4) for _ in range(64)]
    if any(any(row) for row in phase_rows[h[89]:]):
        raise ValueError('Nonzero unused phase ledger rows')
    phases = phase_rows[:h[89]]
    if any(row[2] > 3 or row[3] > 1 for row in phases):
        raise ValueError('Invalid phase ledger epoch/active')
    rx = [r.words(16) for _ in range(16)]
    samples = [r.words(80) for _ in range(h[10])]
    cadence = [r.words(4) for _ in range(h[12])]
    history, windows = r.records(h[14]), []
    for index in range(h[16]):
        w = dict(zip(WINDOW, r.words(16)))
        if w['record_count'] > 4096 or w['pre_records'] > w['record_count']:
            raise ValueError('Invalid window record count')
        if (version == 1 and w['reserved14']) or w['reserved15']:
            raise ValueError('Nonzero reserved window words')
        if w['window_id'] != index+1:
            raise ValueError('Invalid window identity')
        if w['freeze_reason'] not in (1, 2, 3, 4):
            raise ValueError('Unknown window freeze reason')
        windows.append({'header': w, 'records': r.records(w['record_count'])})
    critical = parse_critical(r) if version == 2 else None
    if r.at != len(data):
        raise ValueError('Unexpected trailing bytes')
    if any(any(row[10:]) for row in rx):
        raise ValueError('Nonzero RX reserved words')
    if any(row[0] and not 1 <= row[0] >> 32 <= 3 for row in rx):
        raise ValueError('Invalid RX kind/source key')
    if any(((e >> 32) & 0x7fffffff) > 3 or (e & 0xffffffff) > 3 for _, _, _, e in cadence):
        raise ValueError('Unsupported cadence epoch')
    if (sum(attempted) & U64) != h[18] or (sum(dropped) & U64) != h[19]:
        raise ValueError('Independent event counter sums disagree')
    if (sum(histogram) & U64) != h[27] or (sum(exposure[::2]) & U64) != h[28]:
        raise ValueError('Histogram/exposure totals disagree')
    if (h[12] + h[13]) & U64 != h[27]:
        raise ValueError('Cadence retained/omitted counts disagree')
    if (h[10] + h[11]) & U64 != h[87]:
        raise ValueError('Circular sample counts disagree')
    if (h[89] + h[90]) & U64 != h[68]:
        raise ValueError('Manual phase ledger counts disagree')
    return {'header_words': h, 'header': {name: signed(h[i]) if i in (6, 7) else h[i]
            for i, name in H_NAMES.items()}, 'identity': strings, 'config': config,
            'event_attempts': attempted, 'event_drops': dropped, 'reset_primary': resets,
            'histogram': histogram, 'exposure': exposure, 'phase_ledger': phases, 'rx': rx,
            'samples': samples, 'cadence': cadence, 'history': history, 'windows': windows,
            'critical': critical}


def event_slot(event):
    return (event >> 8)*64 + (event & 255) if event >> 8 < 8 and event & 255 < 64 else 63


def deduplicate(records):
    """Admission ordinals can wrap; full tuple is the overlap identity."""
    seen, by_ordinal, out, conflicts = set(), {}, [], 0
    for record in records:
        key = tuple(record[name] for name in ('t_us', 'a', 'b', 'ordinal', 'event', 'flags'))
        if key in seen:
            continue
        if record['ordinal'] in by_ordinal and by_ordinal[record['ordinal']] != key:
            conflicts += 1
        seen.add(key)
        by_ordinal[record['ordinal']] = key
        out.append(record)
    # Ordinal never establishes temporal ordering across concurrent producers.
    out.sort(key=lambda row: (row['t_us'], row['ordinal']))
    return out, len(records)-len(out), conflicts


def durations(records, begin, end, actor_flags=False):
    starts, ambiguous, values, unmatched = {}, set(), [], 0
    for row in records:
        if row['event'] in (0x22a, 0x22b, 0x111):
            unmatched += len(starts)
            starts.clear()
            ambiguous.clear()
        if row['event'] not in (begin, end):
            continue
        # The producer stores the helper index in the upper flag byte; the
        # lower byte is the operation/result flag, not an actor identity.
        key = (row['a'], (row['flags'] >> 8) & 255) if actor_flags else row['a']
        if row['event'] == begin:
            if key in starts:
                ambiguous.add(key)
            starts[key] = row['t_us']
        else:
            start = starts.pop(key, None)
            if start is None or key in ambiguous or row['t_us'] < start:
                unmatched += 1
            else:
                values.append(row['t_us']-start)
    return dict(distribution(values), unmatched_or_ambiguous=unmatched+len(starts),
                open_begin_observations=len(starts), population='retained matched operation spans')


def recovery(records):
    # WAIT_END uses the later IDR's AU ordinal, not WAIT_BEGIN's AU ordinal.
    start, ambiguous, complete, unmatched, interrupted = None, False, [], 0, []
    for row in records:
        if row['event'] in (0x22a, 0x22b, 0x111) and start is not None:
            unmatched += 1
            interrupted.append({'begin': start, 'boundary_event': row,
                                'completed_duration_us': None})
            start, ambiguous = None, False
        if row['event'] == 0x126 and not row['flags'] & 0x8000:
            if start is not None:
                unmatched += 1
                ambiguous = True
            start = row
        elif row['event'] == 0x127:
            if start is None or ambiguous or row['t_us'] < start['t_us']:
                unmatched += 1
            else:
                complete.append({'begin_us': start['t_us'], 'end_us': row['t_us'],
                    'duration_us': row['t_us']-start['t_us'], 'begin_reason': start['flags'],
                    'recovery_end_means': 'AU feed accepted, not displayed IDR'})
            start, ambiguous = None, False
    return {'retained_completed': complete, 'unmatched_or_ambiguous': unmatched,
            'open_at_capture_end': start, 'open_duration_us': None,
            'interrupted_by_source_or_decoder_boundary': interrupted}


def sample_analysis(rows):
    samples, intervals = [], []
    counters = (SAMPLE[10:13] + [SAMPLE[i] for i in range(16, 24)] +
        [SAMPLE[i] for i in (26, 27, 28, 29)] +
        [name for i, name in enumerate(SAMPLE_EXTRA) if i in (0, 2, 3, 4, 5, 6, 7, 8, 15)])
    previous = None
    for raw in rows:
        s = dict(zip(SAMPLE, raw))
        s.update(zip(SAMPLE_EXTRA, raw[64:80]))
        s['history_overwrites_raw'] = raw[5]
        s['history_overwrites_coherent'] = bool(raw[6] & 8) and raw[5] != U64
        s['history_overwrites'] = raw[5] if s['history_overwrites_coherent'] else None
        s['actors'] = []
        for actor, name in enumerate(ACTORS):
            phase, epoch, begin, version = raw[32+actor*4:36+actor*4]
            coherent = bool(raw[6] & (1 << (8+actor))) and version != U64 and not version & 1
            s['actors'].append({'actor': name, 'phase': PHASES[phase] if phase < len(PHASES) else 'UNSUPPORTED',
                'epoch': EPOCHS[epoch] if epoch < 4 else 'UNSUPPORTED', 'begin_us': begin,
                'version': version, 'coherent': coherent,
                'observed_phase_age_us': raw[1]-begin if coherent and begin and raw[1] >= begin else None,
                'phase_age_us_bounds_in_observation': [max(0, raw[1]-begin), raw[30]-begin]
                    if coherent and begin and raw[30] >= max(begin, raw[1]) else None})
        queue_ok = bool(raw[6] & 4) and bool(raw[9] & 1) and not raw[9] & (4 | 8 | 16 | 32 | 64)
        s['queue_oldest_age_us'] = raw[1]-raw[8] if queue_ok and raw[8] and raw[1] >= raw[8] else None
        s['last_valid_video_callback_age_us'] = raw[1]-raw[14] if raw[14] and raw[1] >= raw[14] else None
        s['session_epoch'] = s['actors'][7]['epoch'] if s['actors'][7]['coherent'] else 'unknown'
        seq = s['primary_video_sequence_encoded']
        sequence_valid = (bool(raw[6] & 2) and seq != U64 and bool(seq & 65536)
                          and bool(s['primary_video_source_key']))
        s['primary_video_sequence'] = {'coherent_matched_source': sequence_valid,
            'last_sequence': seq & 65535 if sequence_valid else None,
            'source_key': s['primary_video_source_key'], 'unknown': not sequence_valid}
        s['reorder_state'] = {'expected': (s['reorder_state_encoded'] >> 32) & 0x7fffffff,
            'buffered': s['reorder_state_encoded'] & 0xffffffff,
            'started': bool(s['reorder_state_encoded'] & (1 << 63))}
        s['last_progress_ages_us'] = {name: raw[1]-s[name] if s[name] and raw[1] >= s[name] else None
            for name in SAMPLE_EXTRA[9:15]}
        s['last_progress_age_us_bounds'] = {name: [max(0, raw[1]-s[name]), raw[30]-s[name]]
            if s[name] and raw[30] >= max(raw[1], s[name]) else None for name in SAMPLE_EXTRA[9:15]}
        s['observation_span_us'] = raw[30]-raw[1] if raw[30] >= raw[1] else None
        s['timing_valid'] = raw[1] >= raw[0] and raw[2] == raw[1]-raw[0] and raw[30] >= raw[1]
        if previous is not None:
            elapsed = s['actual_us']-previous['actual_us']
            values, wrapped = {}, []
            for name in counters:
                if s[name] < previous[name]:
                    wrapped.append(name)
                values[name] = (s[name]-previous[name]) & U64
            cadence_coherent = bool(s['coherence_bits'] & previous['coherence_bits'] & 1)
            valid = elapsed > 0 and not wrapped and s['timing_valid'] and previous['timing_valid'] and cadence_coherent
            stage = 'indeterminate'
            if valid:
                chain = [('video_callbacks', 'no_callback_progress'),
                    ('queue_accepted_callbacks', 'callbacks_without_queue_accept_progress'),
                    ('valid_AU_attempts', 'queue_accept_without_valid_AU_progress'),
                    ('native_valid_output_attempts', 'valid_AU_without_native_output_progress'),
                    ('RGB_publication_attempts', 'native_output_without_RGB_progress'),
                    ('NEW_count', 'RGB_without_NEW_progress')]
                for name, label in chain:
                    if not values[name]:
                        stage = label
                        break
                else:
                    stage = 'all_selected_progress_counters_advanced'
            intervals.append({'begin_us': previous['actual_us'], 'end_us': s['observation_end_us'],
                'sample_start_end_us': s['actual_us'],
                'elapsed_us': elapsed if elapsed >= 0 else None, 'counter_deltas': values,
                'wrapped_or_reset_counters': wrapped, 'rates_valid': valid,
                'cadence_endpoints_coherent': cadence_coherent,
                'begin_session_epoch': previous['session_epoch'], 'end_session_epoch': s['session_epoch'],
                'investigation_lead': stage,
                'causal_classification': False, 'globally_coherent_snapshot': False,
                'counter_read_time_is_exact': False})
        samples.append(s)
        previous = s
    return {'rows': samples, 'successive_sample_progress': intervals,
        'lateness_retained': distribution([s['lateness_us'] for s in samples]),
        'invalid_timing_rows': sum(not s['timing_valid'] for s in samples),
        'population': 'retained sampler rows; 100ms target is not actual wakeup cadence'}


def pipeline_record(row):
    out = dict(row, name=EVENTS.get(row['event'], 'UNKNOWN_%04x' % row['event']))
    if row['event'] == 0x10c:
        expected, lost = row['a'] >> 32, row['a'] & 0xffffffff
        out.update(pre_expected_wire_sequence=expected, skipped_prefix_positions=lost,
            expected_after_skipped_prefix_wire=(expected+lost) & 65535,
            pre_buffered_packets=row['b'], reason_depth32=bool(row['flags'] & 1),
            reason_ordinary_time_expiry=bool(row['flags'] & 2),
            ordinary_time_expiry_permitted_for_call=bool(row['flags'] & 4),
            reason_forced_grace_release=bool(row['flags'] & 8),
            reason_advance_limited_before_active_pending=bool(row['flags'] & 16),
            unknown_reason_flag_bits=row['flags'] & ~31,
            skipped_prefix_does_not_complete_protected_range=True,
            packet_emit_established=False, Decode_feed_established=False,
            output_PTS_association_established=False)
    return out


def gap_chronology(cadence, records, sampled, incomplete):
    """Positive retained evidence and sampler slices, never a fabricated cause."""
    times = [row['t_us'] for row in records]
    rows = sampled['rows']
    sample_times = [s['actual_us'] for s in rows]
    sample_time_valid = all(a <= b for a, b in zip(sample_times, sample_times[1:]))
    progress = sampled['successive_sample_progress']
    out = []
    for interval in cadence:
        if not interval['closed_gt100ms'] or interval['begin_us'] is None:
            continue
        begin, end = interval['begin_us'], interval['end_us']
        details = records[bisect_left(times, begin):bisect_right(times, end)]
        first = bisect_left(sample_times, begin) if sample_time_valid else 0
        last = bisect_right(sample_times, end) if sample_time_valid else 0
        inside = rows[first:last]
        contained = [item for item in progress[max(0, first-1):last]
                     if item['begin_us'] >= begin and item['end_us'] <= end]
        leads = [item for item in contained if item['investigation_lead'] not in
                 ('indeterminate', 'all_selected_progress_counters_advanced')]
        out.append({'begin_us': begin, 'end_us': end, 'interval_us': interval['interval_us'],
            'phase_crossing_observed': interval['phase_crossing_observed'],
            'stable_only_eligible': interval['stable_only_eligible'],
            'retained_detail_event_counts': dict(Counter(EVENTS.get(r['event'], 'UNKNOWN_%04x' % r['event']) for r in details)),
            'interior_sampler_rows': len(inside), 'interior_sample_progress': contained,
            'sample_time_order_valid': sample_time_valid,
            'first_observed_no_progress_lead': leads[0] if leads else None,
            'unsampled_or_unretained_lead_possible': True,
            'detail_coverage_incomplete': incomplete,
            'retained_reorder_jump_observations': [r for r in details if r['event'] == 0x602],
            'retained_reorder_hole_observations': [pipeline_record(r) for r in details if r['event'] == 0x10c],
            'retained_recovery': recovery(details),
            'retained_operation_spans': {name: durations(details, a, b) for name, a, b in
                (('Decode', 0x200, 0x201), ('copy', 0x206, 0x207), ('conversion', 0x20c, 0x20d))},
            'cause_established': False})
    return out


def nack_record(row):
    """Decode local observations without equating a send and remote delivery."""
    event = row['event']
    out = dict(row, request_epoch=row['identity'] >> 32,
               request_ordinal=row['identity'] & 0xffffffff)
    out['source_metadata_available'] = bool(row['source'] & (1 << 63))
    out['network_delivery_established'] = False
    if event in (18, 19, 20):
        count = row['b']-row['a']+1 if row['b'] >= row['a'] else None
        out.update(first_missing_extended=row['a'], last_missing_extended=row['b'],
            missing_positions=count, missing_range_inclusive=True,
            range_valid=count is not None and 0 < count < 32768,
            request_range_within_64=count is not None and 0 < count <= 64)
        if event == 18:
            out.update(gap_id=row['identity'], experiment_enabled=bool(row['flags'] & 1),
                       received_extended=row['c'])
        elif event == 19:
            out['detected_us'] = row['c']
        else:
            out['requested_us'] = row['c']
    elif event in (21, 22, 23):
        status = signed(row['a'])
        out.update(local_status=status, bytes=row['b'], attempt_start_us=row['c'],
                   dispatch_us=row['c'],
                   dispatch_us_compatibility_alias='attempt_start_us; before construction/send, not successful local result',
                   exception_observed=bool(row['flags'] & 1))
        out['byte_count_scope'] = 'protected_copy' if event == 22 else 'plaintext_original_message'
        if event == 22:
            out['SRTCP_local_success'] = status == 0 and not out['exception_observed']
        elif event == 23:
            out.update(local_send_success=status == 1 and not out['exception_observed'],
                       local_send_result_observed_us=row['t_us'],
                       status_semantics='0 failed, 1 success, -1 unavailable; local API only')
    elif event == 24:
        out.update(sequence_extended=row['a'], RTP_timestamp=row['b'], requested_us=row['c'],
            first_observation=bool(row['flags'] & 1), duplicate_observation=bool(row['flags'] & 2),
            range_complete_observed=bool(row['flags'] & 4), authenticated_original_RTP_observation=True,
            retransmission_causality_established=False)
    elif event == 25:
        rejection_flags = row['flags'] & 7
        rejected = row['b'] == 1
        reason = ({1: 'behind_worker_expected', 2: 'occupied_slot',
            4: 'other_or_unclassified'}.get(rejection_flags, 'unknown') if rejected else None)
        out.update(sequence_extended=row['a'], decision=NACK_REORDER.get(row['b'], 'unknown'),
            decision_code=row['b'], request_ordinal_observed=row['c'],
            rejection_subreason=reason,
            behind_worker_expected_before_insert=True if rejected and rejection_flags == 1
                else False if rejected and rejection_flags == 2 else None,
            occupied_slot_at_nonnegative_difference_before_insert=True
                if rejected and rejection_flags == 2 else None,
            admission_outcome='admitted' if row['b'] in (0, 2) else
                'confirmed_behind_worker_expected' if rejected and rejection_flags == 1 else
                'rejected_without_confirmed_behind_expected' if rejected else 'unknown',
            rejection_code_alone_proves_late_after_worker_advance=False)
    elif event == 26:
        out.update(AU_ordinal=row['a'], AU_stage=NACK_AU_STAGES[row['b']]
            if row['b'] < len(NACK_AU_STAGES) else 'unknown', AU_stage_code=row['b'],
            sequence_extended=row['c'], presented_image_established=False,
            successful_Decode_feed_established=False, native_Decode_output_established=False,
            shutdown_discard_observation=row['b'] == 4 and bool(row['flags'] & 1))
    elif event == 27:
        checked = bool(row['flags'] & 16) and not bool(row['flags'] & 8)
        out.update(experiment_enabled=bool(row['a']), session_request_cap=row['b'],
            watch_slots=row['c'] >> 32, range_position_cap=row['c'] & 0xffffffff,
            configuration_bits=row['flags'], closing_observation=bool(row['flags'] & 8),
            answer_checked=True if checked else None,
            remote_feedback_accepted=bool(row['flags'] & 2) if checked else None,
            effective_requests_enabled=bool(row['a']) and bool(row['flags'] & 2) if checked else None)
    elif event == 28:
        out.update(reason=NACK_SUPPRESS.get(row['a'], 'unknown_%d' % row['a']),
                   reason_code=row['a'], suppressed_positions=row['b'], gap_id=row['c'],
                   identity_semantics='epoch plus request-or-gap ID; not a request association',
                   suppression_request_association_available=False)
    elif event == 29:
        out.update(counter_id=row['a'], counter_name=NACK_COUNTERS[row['a']]
            if row['a'] < len(NACK_COUNTERS) else NACK_EXTRA_COUNTERS.get(row['a'], 'unknown_%d' % row['a']),
            value=row['b'], counter_epoch=row['c'], cumulative_snapshot_not_delta=True)
        if row['a'] == 102:
            out['counter_scope'] = 'Non-authenticated OR retransmission/RTX-normalized provenance; not authentication failures alone'
        elif row['a'] == 109:
            out.update(counter_scope='Ticket setter/getter omitted operations; can count twice for one packet',
                       counter_is_lost_packet_count=False)
    elif event == 30:
        out.update(first_missing_extended=row['a'], pending_bitmap=row['b'],
            selection_us=row['c'], dispatched_positions=[row['a']+bit for bit in range(64)
                if row['b'] & (1 << bit)], bitmap_positions_are_actually_selected=True,
            selection_is_atomic_transport_send=False)
    return out


def nack_analysis(cr, timeline):
    """Bounded request/source/epoch correlation; later arrivals are observations."""
    rows = sorted((r for r in timeline if 18 <= r['event'] <= 30),
                  key=lambda r: (r['t_us'], r['ordinal']))
    counts = {CRITICAL[i]: {'attempts': cr['attempted'][i], 'omitted': cr['omitted'][i],
        'retained': cr['attempted'][i]-cr['omitted'][i]} for i in range(18, 31)}
    states = [r for r in rows if r['event'] == 27]
    coverage = {'event_counts': counts, 'retained_records': len(rows),
        'NACK_detail_complete': not any(cr['omitted'][18:31]),
        'configuration_details_retained': any(not r['closing_observation'] for r in states),
        'answer_check_result_retained': any(r['answer_checked'] is True for r in states),
        'local_status_absence_establishes_no_send': False,
        'bounded_by_existing_4096_critical_records': True}
    # Older v2 captures allocate these counter slots but have no emitter. A
    # zero STATE is unavailability, not an experiment proven to be disabled.
    if not cr['attempted'][27]:
        return {'available': False, 'reason': 'No NACK_STATE observation; older v2 NACK hooks unavailable',
            'coverage': coverage, 'orphan_new_event_records': rows,
            'experiment_enabled': None, 'answer_checked': None,
            'remote_feedback_accepted': None, 'effective_requests_enabled': None}

    def elapsed(end, begin):
        return end-begin if begin is not None and begin > 0 and end >= begin else None

    def key(row):
        return row['source'], row['identity']

    def key_valid(row):
        return (row['source_metadata_available'] and row['payload_type'] == 102 and
                row['ssrc'] != 0 and 0 < row['request_ordinal'] <= 64)

    request_rows, grouped, detections = {}, {}, {}
    for row in rows:
        if row['event'] == 18:
            detections.setdefault((row['source'], row['identity'] >> 32, row['a'], row['b']), []).append(row)
        elif 19 <= row['event'] <= 26 or row['event'] == 30:
            grouped.setdefault(key(row), []).append(row)
            if row['event'] == 19:
                request_rows.setdefault(key(row), []).append(row)
    requests, correlated_keys = [], set()
    for request_key, candidates in request_rows.items():
        if len(candidates) != 1:
            requests.append({'source': request_key[0], 'identity': request_key[1],
                'association_available': False, 'reason': 'Repeated request identity in retained records',
                'request_observations': candidates})
            continue
        request = candidates[0]
        if not key_valid(request) or not request['request_range_within_64']:
            requests.append({'source': request_key[0], 'identity': request_key[1],
                'association_available': False, 'reason': 'Invalid/unknown source, request ordinal or range',
                'request_observations': candidates})
            continue
        correlated_keys.add(request_key)
        members = grouped[request_key]
        stages = {CRITICAL[i]: [r for r in members if r['event'] == i] for i in range(20, 27)}
        stages['NACK_DISPATCH_MASK'] = [r for r in members if r['event'] == 30]
        dispatches = stages['NACK_DISPATCH']
        dispatch = dispatches[0] if (len(dispatches) == 1 and
            dispatches[0]['a'] == request['a'] and dispatches[0]['b'] == request['b'] and
            request['detected_us'] <= dispatches[0]['requested_us'] <= request['t_us'] and
            dispatches[0]['t_us'] >= request['t_us']) else None
        masks = stages['NACK_DISPATCH_MASK']
        mask = masks[0] if (len(masks) == 1 and dispatch and
            masks[0]['a'] == request['a'] and masks[0]['b'] != 0 and
            masks[0]['b'] >> request['missing_positions'] == 0 and
            request['t_us'] <= masks[0]['selection_us'] <= masks[0]['t_us']) else None
        dispatched_positions = mask['dispatched_positions'] if mask else None
        dispatch_references = {r['attempt_start_us'] for i in range(21, 24) for r in stages[CRITICAL[i]]}
        dispatch_reference = next(iter(dispatch_references)) if (len(dispatch_references) == 1 and
            mask and next(iter(dispatch_references)) > 0 and
            mask['selection_us'] <= next(iter(dispatch_references)) <= dispatch['t_us']) else None
        # This embedded clock is taken BEFORE packet construction/send. It
        # supplies an attempt-start boundary only, never an after-send result.
        # A DISPATCH record alone is not an alternative send-result boundary.
        return_dispatch_boundary = dispatch_reference
        returns = stages['NACK_AUTH_RETURN']
        in_range = [r for r in returns if request['a'] <= r['sequence_extended'] <= request['b']]
        valid_returns = [r for r in in_range if r['t_us'] >= request['t_us']]
        first = min(valid_returns, key=lambda r: r['t_us'], default=None)
        returned_positions = sorted({r['sequence_extended'] for r in valid_returns})
        dispatched_returns = [r for r in valid_returns if mask and
            r['sequence_extended'] in dispatched_positions and return_dispatch_boundary is not None and
            r['t_us'] >= return_dispatch_boundary]
        first_postdispatch_observation = min(dispatched_returns, key=lambda r: r['t_us'], default=None)
        returned_dispatched_positions = sorted({r['sequence_extended'] for r in dispatched_returns})
        # A duplicate after dispatch cannot become a newly repaired position
        # when its first authenticated arrival was already before dispatch.
        # Unknown/conflicting flags remain observations, never first arrivals.
        first_arrivals = [r for r in dispatched_returns if r['first_observation'] and
            not r['duplicate_observation'] and not any(prior['first_observation'] and
                prior['sequence_extended'] == r['sequence_extended'] and prior['t_us'] < r['t_us']
                for prior in valid_returns)]
        dispatched_first = min(first_arrivals, key=lambda r: r['t_us'], default=None)
        first_arrival_positions = sorted({r['sequence_extended'] for r in first_arrivals})
        predispatch_returns = [r for r in valid_returns if dispatch_reference is not None and
            r['t_us'] < dispatch_reference]
        au = stages['NACK_AU_CONTRIBUTION']
        accepted = [r for r in au if r['AU_stage_code'] == 2 and
            request['a'] <= r['sequence_extended'] <= request['b']]
        # Compare only the same observed packet sequence, never two different
        # positions in one requested range or a compressed input to decoder output.
        accepted_same_first = min((r for r in accepted if first and
            r['sequence_extended'] == first['sequence_extended'] and r['t_us'] >= first['t_us']),
            key=lambda r: r['t_us'], default=None)
        sr_begin, sr_result = stages['NACK_SRTCP_BEGIN'], stages['NACK_SRTCP_RESULT']
        sr_span = elapsed(sr_result[0]['t_us'], sr_begin[0]['t_us']) if (
            len(sr_begin) == len(sr_result) == 1 and
            sr_begin[0]['dispatch_us'] == sr_result[0]['dispatch_us']) else None
        # Observe the successful results themselves. A return between successful
        # protection and local send-result observations can race transportSend;
        # it cannot be called a post-send response. Equal clocks are ambiguous.
        def transport_clock_valid(row):
            return (len(dispatch_references) == 1 and row['attempt_start_us'] > 0 and
                row['attempt_start_us'] == next(iter(dispatch_references)) and
                request['t_us'] <= row['attempt_start_us'] <= row['t_us'] and
                (mask is None or row['attempt_start_us'] >= mask['selection_us']) and
                (dispatch is None or row['attempt_start_us'] <= dispatch['t_us']))

        protection_result = sr_result[0] if (len(sr_result) == 1 and
            sr_result[0]['SRTCP_local_success'] and transport_clock_valid(sr_result[0]) and
            (not sr_begin or len(sr_begin) == 1 and transport_clock_valid(sr_begin[0]) and
             sr_begin[0]['t_us'] <= sr_result[0]['t_us'])) else None
        local_results = stages['NACK_LOCAL_SEND_RESULT']
        local_result = local_results[0] if (len(local_results) == 1 and
            transport_clock_valid(local_results[0]) and
            (protection_result is None or local_results[0]['t_us'] >= protection_result['t_us'])) else None
        local_success = local_result if local_result and local_result['local_send_success'] else None
        selected_returns = [r for r in valid_returns if mask and r['sequence_extended'] in dispatched_positions]
        selected_first = [r for r in selected_returns if r['first_observation'] and not r['duplicate_observation']
            and not any(prior['first_observation'] and prior['sequence_extended'] == r['sequence_extended']
                and prior['t_us'] < r['t_us'] for prior in valid_returns)]
        first_after_success = [r for r in selected_first if local_success and r['t_us'] > local_success['t_us']]
        first_after_success_row = min(first_after_success, key=lambda r: r['t_us'], default=None)
        arrival_classification = []
        for r in valid_returns:
            t = r['t_us']
            if dispatch_reference is not None and t < dispatch_reference:
                category = 'before_attempt_start'
            elif local_success and t > local_success['t_us']:
                category = 'after_successful_local_send_result'
            elif protection_result and t > protection_result['t_us'] and local_result and t < local_result['t_us']:
                category = 'between_successful_protection_and_local_result_ambiguous'
            elif local_result and t == local_result['t_us']:
                category = 'same_clock_as_local_result_ambiguous'
            elif protection_result and t == protection_result['t_us']:
                category = 'same_clock_as_protection_result_ambiguous'
            elif local_result and not local_result['local_send_success'] and t > local_result['t_us']:
                category = 'after_unsuccessful_local_result_not_post_success'
            elif protection_result and t < protection_result['t_us']:
                category = 'before_successful_protection_result'
            elif protection_result and t > protection_result['t_us']:
                category = 'after_protection_result_local_result_unavailable'
            else:
                category = 'local_result_boundary_unavailable'
            arrival_classification.append({'sequence_extended': r['sequence_extended'], 'AUTH_us': t,
                'selected': r in selected_returns, 'first_observation': r['first_observation'],
                'duplicate_observation': r['duplicate_observation'], 'classification': category,
                'attempt_start_to_AUTH_us': elapsed(t, dispatch_reference),
                'protection_result_to_AUTH_us': elapsed(t, protection_result['t_us']) if protection_result else None,
                'successful_local_send_result_to_AUTH_us': elapsed(t, local_success['t_us']) if local_success and
                    t > local_success['t_us'] else None,
                'retransmission_causality_established': False})
        matched_detection = [r for r in detections.get((request['source'], request['request_epoch'],
            request['a'], request['b']), []) if request['detected_us'] <= r['t_us'] <= request['t_us']]
        embedded_request_times = {r['requested_us'] for r in dispatches+returns}
        requests.append({'source': request['source'], 'identity': request['identity'],
            'request_epoch': request['request_epoch'], 'request_ordinal': request['request_ordinal'],
            'ssrc': request['ssrc'], 'payload_type': request['payload_type'],
            'association_available': True, 'association_scope': 'source plus full epoch/request identity',
            'request': request, 'exact_detection_candidates': matched_detection,
            'detection_association_uses_source_epoch_range_and_bounded_record_time': True,
            'detection_identity_unique': len(matched_detection) == 1,
            'retained_stage_records': stages, 'first_authenticated_return': first,
            'retained_authenticated_positions': returned_positions,
            'dispatch_mask_association_available': mask is not None,
            'actually_dispatched_positions': dispatched_positions,
            'dispatch_mask_selected_positions': dispatched_positions,
            'actually_dispatched_positions_compatibility_alias': 'selected bitmap only; does not prove transport send or server receipt',
            'mask_selection_us': mask['selection_us'] if mask else None,
            'transport_attempt_start_reference_us': dispatch_reference,
            'actual_transport_dispatch_reference_us': dispatch_reference,
            'actual_transport_dispatch_reference_compatibility_alias': 'transport_attempt_start_reference_us; before construction/send',
            'return_dispatch_boundary_kind': 'embedded_attempt_start_reference' if dispatch_reference is not None else 'unavailable',
            'legacy_after_dispatch_fields_semantics': 'After attempt START only, not after successful local send result; no DISPATCH clock fallback',
            'successful_SRTCP_result_observed_us': protection_result['t_us'] if protection_result else None,
            'local_send_result_observed_us': local_result['t_us'] if local_result else None,
            'successful_local_send_result_observed_us': local_success['t_us'] if local_success else None,
            'protected_local_send_route_success_observed': bool(protection_result and local_success),
            'authenticated_return_result_boundary_classification': arrival_classification,
            'result_boundary_observation_counts': dict(Counter(r['classification'] for r in arrival_classification)),
            'first_authenticated_arrival_positions_after_successful_local_send_result':
                sorted({r['sequence_extended'] for r in first_after_success}),
            'first_selected_authenticated_arrival_after_successful_local_send_result': first_after_success_row,
            'all_selected_positions_have_first_arrival_after_successful_local_send_result':
                set(r['sequence_extended'] for r in first_after_success) == set(dispatched_positions)
                if mask and local_success else None,
            'first_authenticated_return_of_dispatched_position_after_dispatch': dispatched_first,
            'first_post_dispatch_authenticated_observation': first_postdispatch_observation,
            'retained_authenticated_dispatched_positions_after_dispatch': returned_dispatched_positions,
            'first_authenticated_arrival_positions_after_dispatch': first_arrival_positions,
            'post_dispatch_duplicate_observations': sum(r['duplicate_observation'] for r in dispatched_returns),
            'post_dispatch_unknown_or_conflicting_first_flags':
                sum(r['first_observation'] == r['duplicate_observation'] for r in dispatched_returns),
            'post_dispatch_completion_requires_first_arrival_flags': True,
            'all_selected_positions_have_first_authenticated_arrival_after_dispatch':
                set(first_arrival_positions) == set(dispatched_positions) if mask and dispatch_reference is not None else None,
            'all_original_range_positions_observed_in_retained_authenticated_returns':
                len(returned_positions) == request['missing_positions'],
            'returns_before_dispatch_observations': len(predispatch_returns) if dispatch_reference is not None else None,
            'returns_before_dispatch_record_observations':
                sum(r['t_us'] < dispatch['t_us'] for r in valid_returns) if dispatch else None,
            'returns_outside_dispatch_mask_observations':
                sum(r['sequence_extended'] not in dispatched_positions for r in valid_returns) if mask else None,
            'pre_dispatch_arrival_is_retransmission_response': False,
            'return_classification_does_not_infer_state_cancellation': True,
            'return_outside_requested_range_observations': len(returns)-len(in_range),
            'return_before_request_observations': len(in_range)-len(valid_returns),
            'first_flag_observations': sum(r['first_observation'] for r in valid_returns),
            'duplicate_flag_observations': sum(r['duplicate_observation'] for r in valid_returns),
            'range_complete_flag_observations': sum(r['range_complete_observed'] for r in valid_returns),
            'embedded_request_reference_matches_retained_request_time':
                all(r['requested_us'] == request['t_us'] for r in dispatches+returns),
            'embedded_request_reference_consistent_across_retained_stages':
                len(embedded_request_times) == 1 and
                all(request['detected_us'] <= time <= request['t_us'] for time in embedded_request_times),
            'request_record_and_embedded_request_time_are_separate_observations': True,
            'timings_us': {'detection_to_request_record': elapsed(request['t_us'], request['c']),
                'request_record_to_dispatch_record': elapsed(dispatch['t_us'], request['t_us']) if dispatch else None,
                'SRTCP_local_call_span': sr_span,
                'request_record_to_first_authenticated_return': elapsed(first['t_us'], request['t_us']) if first else None,
                'dispatch_record_to_first_authenticated_return': elapsed(dispatched_first['t_us'], dispatch['t_us'])
                    if dispatched_first and dispatch else None,
                'dispatch_record_to_first_post_dispatch_authenticated_observation':
                    elapsed(first_postdispatch_observation['t_us'], dispatch['t_us'])
                    if first_postdispatch_observation and dispatch else None,
                'dispatch_timestamp_to_first_authenticated_dispatched_position':
                    elapsed(dispatched_first['t_us'], dispatch_reference) if dispatched_first else None,
                'attempt_start_to_first_authenticated_selected_arrival':
                    elapsed(dispatched_first['t_us'], dispatch_reference) if dispatched_first else None,
                'successful_local_send_result_to_first_authenticated_selected_arrival':
                    elapsed(first_after_success_row['t_us'], local_success['t_us']) if first_after_success_row else None,
                'successful_local_send_result_to_selected_first_arrivals_distribution': distribution([
                    r['t_us']-local_success['t_us'] for r in first_after_success]),
                'serialized_selection_to_dispatch_reference':
                    elapsed(dispatch_reference, mask['selection_us']) if dispatch_reference is not None and mask else None,
                'embedded_request_to_first_authenticated_return': elapsed(first['t_us'], first['requested_us']) if first else None,
                'first_authenticated_return_to_first_accepted_AU_observation':
                    elapsed(accepted_same_first['t_us'], first['t_us']) if accepted_same_first and first else None},
            'AU_observation_counts_by_stage': dict(Counter(r['AU_stage'] for r in au)),
            'reorder_observation_counts_by_decision': dict(Counter(r['decision'] for r in stages['NACK_REORDER_RESULT'])),
            'reorder_observation_counts_by_admission_outcome':
                dict(Counter(r['admission_outcome'] for r in stages['NACK_REORDER_RESULT'])),
            'reorder_rejection_observation_counts_by_subreason':
                dict(Counter(r['rejection_subreason'] for r in stages['NACK_REORDER_RESULT']
                    if r['decision_code'] == 1)),
            'accepted_AU_observation_is_presented_image': False,
            'accepted_AU_observation_establishes_successful_Decode_feed': False,
            'accepted_AU_observation_establishes_native_Decode_output': False,
            'later_return_proves_NACK_delivery_or_retransmission': False,
            'no_return_record_proves_no_remote_return': False})
    counter_rows = [r for r in rows if r['event'] == 29]
    latest = {}
    for row in counter_rows:
        latest[(row['source'], row['counter_epoch'], row['counter_id'])] = row
    tracking_ids = {21, 22, 23, 35, 36, 37, 100, 103, 104, 106, 107, 108, 109}
    tracking_rows = [r for r in latest.values() if r['counter_id'] in tracking_ids]
    coverage.update(retained_functional_tracking_limit_snapshots=tracking_rows,
        functional_tracking_limit_observed=any(r['value'] for r in tracking_rows),
        global_ticket_counters_include_unknown_source=True,
        absent_tracking_counter_is_zero=False,
        missing_tracking_counter_ids=sorted(tracking_ids-{r['counter_id'] for r in tracking_rows}),
        authenticated_return_absence_claims_permitted=False)
    configured_states = [r for r in states if not r['closing_observation']]
    configuration = configured_states[-1] if configured_states else None
    checked_states = [r for r in configured_states if r['answer_checked'] is True and
        configuration and r['request_epoch'] == configuration['request_epoch']]
    feedback_result = checked_states[-1] if checked_states else None
    return {'available': True, 'experiment_enabled': configuration['experiment_enabled'] if configuration else None,
        'answer_checked': True if feedback_result else None,
        'remote_feedback_accepted': feedback_result['remote_feedback_accepted'] if feedback_result else None,
        'effective_requests_enabled': configuration['experiment_enabled'] and feedback_result['remote_feedback_accepted']
            if configuration and feedback_result else None,
        'feedback_result_scope': 'latest retained checked result in current configuration epoch; not proof of final state',
        'latest_retained_answer_check_result': feedback_result,
        'feedback_state_at_stop_established': False,
        'closing_observation_retained': any(r['closing_observation'] for r in states),
        'retained_state_observations': states, 'coverage': coverage,
        'request_correlations': requests,
        'orphan_or_ambiguous_request_stage_records': [r for r in rows if (19 <= r['event'] <= 26 or r['event'] == 30)
            and key(r) not in correlated_keys],
        'retained_detection_observations': [r for r in rows if r['event'] == 18],
        'retained_suppression_observations': [r for r in rows if r['event'] == 28],
        'latest_retained_counter_snapshots': sorted(latest.values(),
            key=lambda r: (r['counter_epoch'], r['source'], r['counter_id'])),
        'counter_snapshot_duplicates_not_summed': len(counter_rows)-len(latest),
        'limitations': ['NACK request, dispatch, SRTCP result and local send are distinct local observations.',
            'The embedded c clock is the attempt start before packet construction/send, not transport completion.',
            'Legacy dispatch-named fields are compatibility aliases for attempt-start observations only; never after-send evidence.',
            'Post-success candidates compare AUTH strictly after a unique successful local send-result observation; equal clocks and between-stage returns remain ambiguous.',
            'A DISPATCH record is never a fallback successful local-send boundary when result observations are absent.',
            'A local successful send is not acknowledgment or proof of delivery to the server.',
            'A later authenticated original packet may be delayed/reordered rather than retransmitted.',
            'Only retained validated dispatch-mask bits describe actually selected positions; the original gap can be larger.',
            'The mask clock is serialized selection; a later arrival can race packet construction before transport dispatch.',
            'Returns before dispatch are distinct observations, not evidence of a response or automatic state cancellation.',
            'Post-dispatch duplicate observations do not qualify as first-arrival repair candidates or post-dispatch completion.',
            'First-arrival candidates use explicit producer flags; missing first-arrival records remain unknown, not no delivery.',
            'Only a retained non-closing STATE with flag16 observes a completed remote Answer check; attach/media STATE is unknown.',
            'Feedback summaries use retained checked results in the current configuration epoch; omitted later observations can hide a change.',
            'Extended sequence identity is source and request epoch local; SSRC changes and resync epochs remain separate.',
            'Missing critical records and evicted watch ranges prevent absence claims.',
            'Request-to-return times are local observed spans, not server RTT or capture age.',
            'AU stage2 means acceptance by the recovery filter before Decode feed, not a successful feed, native output or presented image.',
            'Reorder code1 also covers occupied slots/invalid input; only its explicit flag1 proves behind worker expected before insertion.',
            'The decoder has no returned PTS; no compressed input is assigned to the latest native output.',
            'Counter snapshots are cumulative per epoch and must not be added as independent deltas.']}


def nack_grace_record(row):
    """Critical31 uses stage as subkind, not a transport-stage location."""
    kind = row['stage']
    out = dict(row, grace_subkind=NACK_GRACE_SUBKINDS.get(kind, 'UNKNOWN_%d' % kind),
        grace_subkind_id=kind, stage_is_transport_location=False,
        request_epoch=row['identity'] >> 32, request_ordinal=row['identity'] & 0xffffffff,
        source_metadata_available=bool(row['source'] & (1 << 63)),
        network_delivery_established=False, output_PTS_association_established=False)
    if kind == 1:
        out.update(first_extended=row['a'], original_missing_mask=row['b'], detected_us=row['c'],
            original_missing_positions=[row['a']+bit for bit in range(64) if row['b'] & (1 << bit)],
            computed_absolute_deadline_us=row['c']+45000 if row['c'] <= U64-45000 else None,
            control_is_worker_receipt_not_network_send=True)
    elif kind == 2:
        status, reason = (row['flags'] & 255)-2, row['flags'] >> 8
        out.update(attempt_start_us=row['a'], local_return_observed_us=row['b'],
            selected_mask=row['c'], local_result=status, outcome_reason=reason,
            local_boolean_success_observed=status == 1,
            AUTH_complete_does_not_complete_worker_grace=reason == 4)
    elif kind == 3:
        result = row['c'] & 0xffffffff
        out.update(deadline_us=row['a'], granted_mask=row['b'], occupancy=row['c'] >> 32,
            offer_result_code=result,
            offer_result=NACK_GRACE_OFFER[result] if result < len(NACK_GRACE_OFFER) else 'unknown')
    elif kind == 4:
        reason = row['flags']
        out.update(deadline_us=row['a'], accepted_worker_mask=row['b'], occupancy=row['c'],
            release_reason_code=reason,
            release_reason=NACK_GRACE_RELEASE[reason] if reason < len(NACK_GRACE_RELEASE) else 'unknown',
            state_ended_observed=True, actual_worker_hole_advance_established=False,
            release_establishes_AU_reconstruction=False)
        if reason == 5:
            out.update(reason5_is_shared_source_or_reorder_reset=True,
                SSRC_change_established_by_reason5=False)
    elif kind == 5:
        out.update(sequence_extended=row['a'], authenticated_observed_us=row['b'],
            callback_begin_us=row['c'], admission_reason=row['flags'],
            callback_establishes_worker_acceptance=False)
    elif kind == 6:
        out.update(sequence_extended=row['a'], callback_begin_us=row['b'], authenticated_observed_us=row['c'],
            queue_pop_us=row['t_us'], callback_to_queue_pop_us=row['t_us']-row['b']
                if row['b'] and row['t_us'] >= row['b'] else None,
            AUTH_to_queue_pop_us=row['t_us']-row['c'] if row['c'] and row['t_us'] >= row['c'] else None)
    elif kind == 7:
        out.update(sequence_extended=row['a'], expected_wire_sequence=row['b'] >> 32,
            buffered_packets=row['b'] & 0xffffffff, deadline_us=row['c'],
            reorder_result=row['flags'], stored_without_jump=row['flags'] == 0,
            worker_accept_establishes_AU_or_picture=False)
    elif kind == 8:
        out.update(Decode_elapsed_cumulative_us=row['a'], copy_elapsed_cumulative_us=row['b'],
            conversion_elapsed_cumulative_us=row['c'], measured_scope='aggregate work during retained range',
            values_are_independent_stage_aggregates=True, sums_establish_input_output_association=False)
    elif kind == 9:
        out.update(oldest_queue_age_us=row['a'], queue_depth=row['b'], max_returned_packet_dwell_us=row['c'])
    elif kind == 10:
        out.update(counter_id=row['a'], value=row['b'], cumulative_snapshot_not_delta=True,
            counter_name=NACK_GRACE_COUNTERS.get(row['a'], 'unknown_%d' % row['a']))
    elif kind == 11:
        out.update(expected_wire_sequence=row['a'] >> 32, buffered_packets=row['a'] & 0xffffffff,
            deadline_us=row['b'], furthest_forward_offset=row['c'],
            unchanged_window_capacity=128, held_observed=True,
            HOLD_establishes_continuous_hold_duration=False, HOLD_establishes_return_utility=False)
    elif kind == 12:
        expected, lost = row['a'] >> 32, row['a'] & 0xffffffff
        reason = row['flags'] & 255
        valid = expected <= 65535 and 0 < lost < 128 and 0 < row['b'] <= 128 and lost <= row['c'] < 128
        out.update(pre_expected_wire_sequence=expected, lost_positions=lost,
            post_expected_wire_sequence=(expected+lost) & 65535,
            pre_buffered_packets=row['b'], pre_furthest_forward_offset=row['c'],
            advance_observation_us=row['t_us'], advance_metadata_valid=valid,
            grace_end_reason_code=reason,
            grace_end_reason=NACK_GRACE_RELEASE[reason] if reason < len(NACK_GRACE_RELEASE) else 'unknown',
            protected_boundary_before_active_pending=bool(row['flags'] & 256),
            unknown_ABANDON_flag_bits=row['flags'] & ~511,
            ordinary_worker_advance_observation=row['identity'] == 0,
            ordinary_advance_has_request_association=False if row['identity'] == 0 else None,
            actual_worker_hole_advance_established=valid,
            state_release_is_not_this_advance=True, advance_establishes_AU_or_picture=False,
            protected_boundary_does_not_complete_pending_range=True, packet_emit_established=False,
            record_clock_is_advance_observation_not_exact_function_entry=True)
        if reason == 5:
            out.update(reason5_is_shared_source_or_reorder_reset=True,
                SSRC_change_established_by_reason5=False)
    elif 13 <= kind <= 20:
        out.update(transport_epoch=row['identity'] >> 32,
            sequence_epoch=row['identity'] & 0xffffffff,
            identity_is_request=False, request_ordinal=None,
            observer_record_not_worker_binding=kind <= 16)
        if kind == 13:
            out.update(anchor_raw_extended=row['a'], previous_raw_high=row['b'],
                previous_sequence_epoch=row['c'], sequence_reason=row['flags'],
                transition_is_announcement_only=True)
        elif kind == 14:
            out.update(anchor_observed_us=row['a'], anchor_RTP_timestamp=row['b'])
        elif kind == 15:
            out.update(omitted_observation_wire_sequence=row['a'], RTP_timestamp=row['b'],
                omitted_observations=row['c'], functional_gate_contention=True,
                diagnostic_retention_omission=False, packet_discard_established=False)
        elif kind == 16:
            out.update(previous_raw_high=row['a'], previous_sequence_epoch=row['b'],
                dirty_consumed_by={1: 'incoming', 2: 'poll'}.get(row['flags'], 'unknown'))
        elif kind == 17:
            out.update(anchor_raw_extended=row['a'], previous_raw_high=row['b'],
                previous_sequence_epoch=row['c'], sequence_reason=row['flags'],
                transition_is_announcement_only=True)
        elif kind == 18:
            out.update(anchor_raw_extended=row['a'], anchor_RTP_timestamp=row['b'],
                authenticated_observed_us=row['c'], exact_anchor_STORED_observed=True,
                announcement_still_required=True)
        elif kind == 19:
            out.update(anchor_raw_extended=row['a'], anchor_canonical_extended=row['b'],
                canonical_offset=row['c'], sequence_reason=row['flags'],
                worker_binding_commit_observed=True,
                new_epoch_does_not_renew_request_deadline=True)
        else:
            out.update(sequence_epoch=None, identity_is_request=None,
                identity_low32_unclassified=row['identity'] & 0xffffffff,
                binding_result=row['flags'] & 255, sequence_reason=row['flags'] >> 8,
                anchor_or_control_raw_extended=row['a'], rejection_payload_b=row['b'],
                rejection_payload_c=row['c'], rejection_payload_layout_unclassified=True,
                identity_role='transition_or_rejected_control',
                packet_discard_established=False)
    elif kind == 21:
        out.update(first_raw_extended=row['a'], first_canonical_extended=row['b'],
            sequence_epoch=row['c'], map_is_request_not_binding_permission=True)
    return out


def nack_grace_analysis(cr, timeline):
    rows = sorted((r for r in timeline if r['event'] == 31), key=lambda r: (r['t_us'], r['ordinal']))
    attempted, omitted = cr['attempted'][31], cr['omitted'][31]
    if not attempted:
        return {'available': False, 'reason': 'No critical31 NACK_GRACE hooks observed; older captures unavailable',
            'experiment_enabled': None, 'coverage': {'attempts': 0, 'omitted': 0, 'retained': 0}}
    grouped = {}
    for row in rows:
        if (row['grace_subkind_id'] in tuple(range(1, 10)) + (11, 12, 21) and
                row['request_ordinal'] is not None and 0 < row['request_ordinal'] <= 64 and
                row['source_metadata_available'] and row['payload_type'] == 102 and row['ssrc'] != 0):
            grouped.setdefault((row['source'], row['identity']), []).append(row)
    ranges = []
    for (source, identity), members in grouped.items():
        requests = [r for r in members if r['grace_subkind_id'] == 1]
        offers = [r for r in members if r['grace_subkind_id'] == 3]
        releases = [r for r in members if r['grace_subkind_id'] == 4]
        holds = [r for r in members if r['grace_subkind_id'] == 11]
        abandons = [r for r in members if r['grace_subkind_id'] == 12]
        canonical_maps = [r for r in members if r['grace_subkind_id'] == 21]
        request = requests[0] if len(requests) == 1 else None
        offer = offers[0] if len(offers) == 1 else None
        sequence_rows = [r for r in members if r['grace_subkind_id'] in (5, 6, 7)]
        per_position = []
        for position in sorted({r['sequence_extended'] for r in sequence_rows}):
            observations = [r for r in sequence_rows if r['sequence_extended'] == position]
            callbacks = [r for r in observations if r['grace_subkind_id'] == 5]
            popped = [r for r in observations if r['grace_subkind_id'] == 6]
            stored = [r for r in observations if r['grace_subkind_id'] == 7 and r['stored_without_jump']]
            per_position.append({'sequence_extended': position,
                'within_retained_original_mask': position in request['original_missing_positions'] if request else None,
                'within_retained_granted_mask': bool(0 <= position-request['first_extended'] < 64 and
                    offer['granted_mask'] & (1 << (position-request['first_extended']))) if request and offer else None,
                'retained_AUTH_clocks_us': sorted({r['authenticated_observed_us'] for r in callbacks+popped
                    if r['authenticated_observed_us']}),
                'retained_callback_begin_clocks_us': sorted({r['callback_begin_us'] for r in callbacks+popped
                    if r['callback_begin_us']}),
                'retained_queue_pop_clocks_us': [r['t_us'] for r in popped],
                'retained_worker_STORED_record_clocks_us': [r['t_us'] for r in stored],
                'STORED_record_clock_relative_to_deadline': [
                    'before' if r['t_us'] < r['deadline_us'] else 'at' if r['t_us'] == r['deadline_us'] else 'after'
                    for r in stored],
                'retained_observations': observations,
                'no_record_does_not_establish_no_arrival': True,
                'AUTH_is_not_worker_acceptance': True,
                'record_clock_is_not_proven_actual_insert_instant': True})
        ranges.append({'source': source, 'identity': identity, 'request_ordinal': identity & 0xffffffff,
            'request_epoch': identity >> 32, 'request': request, 'offer': offer,
            'offer_deadline_matches_detect_plus_45ms': offer['deadline_us'] == request['computed_absolute_deadline_us']
                if request and offer and request['computed_absolute_deadline_us'] is not None else None,
            'retained_release_observations': releases,
            'retained_state_end_observations': releases,
            'retained_hold_observations': holds,
            'grace_held_observation_retained': bool(holds),
            'state_end_observation_retained': bool(releases),
            'retained_worker_ABANDON_observations': abandons,
            'actual_worker_hole_advance_observation_retained': any(r['advance_metadata_valid'] for r in abandons),
            'retained_worker_advance_reason_counts': dict(Counter(r['grace_end_reason'] for r in abandons)),
            'state_end_and_worker_advance_are_distinct': True,
            'hold_to_state_end_observation_us': releases[0]['t_us']-holds[0]['t_us']
                if len(holds) == len(releases) == 1 and releases[0]['t_us'] >= holds[0]['t_us'] else None,
            'state_end_to_worker_advance_observation_us': abandons[0]['t_us']-releases[0]['t_us']
                if len(releases) == len(abandons) == 1 and abandons[0]['advance_metadata_valid'] and
                    abandons[0]['grace_end_reason_code'] == releases[0]['release_reason_code'] and
                    abandons[0]['t_us'] >= releases[0]['t_us'] else None,
            'retained_release_reason_counts': dict(Counter(r['release_reason'] for r in releases)),
            'release_mask_checks': [{'t_us': r['t_us'],
                'accepted_mask_is_subset_of_granted': not bool(r['accepted_worker_mask'] & ~offer['granted_mask'])
                    if offer else None,
                'accepted_release_equals_granted_mask': r['accepted_worker_mask'] == offer['granted_mask']
                    if offer and r['release_reason_code'] == 1 else None} for r in releases],
            'retained_position_chronology': per_position,
            'retained_worker_STORED_observations': [r for r in members if r['grace_subkind_id'] == 7 and r['stored_without_jump']],
            'retained_sequence_and_work_chronology': members,
            'retained_canonical_request_maps': canonical_maps,
            'AUTH_does_not_complete_grace': True,
            'AU_reconstruction_native_picture_and_presentation_established': False})
    counters, latest = [r for r in rows if r['grace_subkind_id'] == 10], {}
    for row in counters:
        latest[(row['source'], row['request_epoch'], row['counter_id'])] = row
    modes = [r for r in counters if r['counter_id'] == 114]
    mode_values = {r['value'] for r in modes}
    inbox_omissions = [r for r in latest.values() if r['counter_id'] == 101]
    return {'available': True, 'experiment_enabled': bool(next(iter(mode_values)))
            if len(mode_values) == 1 and mode_values <= {0, 45} else None,
        'retained_configured_grace_ms_values': sorted(mode_values),
        'configured_mode_observation_does_not_prove_runtime_hold': True,
        'release_reason_code5_scope': 'Production SOURCE is shared by source retirement and reorder window JUMP/reset; code5 alone cannot establish an SSRC change.',
        'coverage': {'attempts': attempted, 'omitted': omitted, 'retained': attempted-omitted,
            'details_complete': not omitted, 'per_subkind_attempts_available': False,
            'absent_retained_subkind_establishes_absence': False,
            'absent_subkind_establishes_no_record_attempt': not omitted,
            'retained_control_inbox_omissions': sum(r['value'] for r in inbox_omissions)
                if len(inbox_omissions) == 1 else None,
            'control_inbox_coverage_scope': 'latest cumulative snapshot; distinct from critical record admission',
            'source_or_control_inbox_completeness_not_established_by_critical_coverage': True},
        'retained_subkind_counts': dict(Counter(r['grace_subkind'] for r in rows)),
        'retained_sequence_binding_chronology': [r for r in rows if 13 <= r['grace_subkind_id'] <= 20],
        'range_correlations': ranges, 'retained_timeline': rows,
        'retained_ordinary_worker_ABANDON_observations': [r for r in rows if r['grace_subkind_id'] == 12 and
            r['ordinary_worker_advance_observation']],
        'latest_retained_counter_snapshots': sorted(latest.values(), key=lambda r: (r['request_epoch'], r['source'], r['counter_id'])),
        'counter_snapshot_duplicates_not_summed': len(counters)-len(latest),
        'limitations': ['Worker-owned original missing masks and accepted masks differ from callback AUTH cancellation.',
            '45ms is an absolute per-request deadline, never a new deadline for every pending position.',
            'Capacity128 remains a hard fallback; retained HOLD does not prove capacity was sufficient.',
            'CONTROL_OUTCOME attempt start and local return are separate clocks; successful local boolean is not remote receipt.',
            'WORK_COST counters are aggregates over the range; no decoder PTS or per-input native-output association exists.',
            'Release/worker acceptance does not establish complete valid AU, Decode feed, native output, RGB or presentation.',
            'RELEASE observes a state end; only valid critical ABANDON observes the actual worker hole advance, independently of detailed pause windows.',
            'HOLD, RELEASE and ABANDON clocks are separate observations, not a continuous measured hold or exact function-entry timeline.',
            'ABANDON flag0x100 and REORDER_HOLE reason16 observe a skipped prefix stopped before another pending position; neither proves an emitted packet or completion of the protected range.',
            'Ordinary limited ABANDON identity0 has no receiver-NACK request association.',
            'Release reason5 retains the numeric SOURCE code but means source_or_reorder_reset; the production window-JUMP path also uses it, so it does not prove SSRC change.',
            'Only latest per-source/epoch counters are used; cumulative snapshots are not summed.']}


def critical_analysis(p):
    cr = p['critical']
    if cr is None:
        return {'available': False, 'reason': 'v1 contains no upstream critical ledger'}
    h = cr['header']
    timeline = []
    for row in cr['records']:
        item = dict(row, name=CRITICAL[row['event']] if row['event'] < len(CRITICAL)
                    else 'UNKNOWN_%d' % row['event'],
                    stage_name=TRANSPORT[row['stage']] if row['stage'] < 16 else 'PIPELINE',
                    ssrc=row['source'] & 0xffffffff, payload_type=(row['source'] >> 32) & 127)
        if row['event'] == 1:
            item.update(expected=row['a'] & 65535, received=row['b'] & 65535,
                expected_extended=row['a'], received_extended=row['b'], skipped_positions=row['c'],
                range_end=(row['b']-1) & 65535, wraps=(row['a'] & 65535) > ((row['b']-1) & 65535),
                header_authenticated=bool(row['flags'] & 2))
        elif row['event'] == 2:
            item.update(sequence=row['a'] & 65535, expected=row['b'] & 65535,
                sequence_extended=row['a'], expected_extended=row['b'], gap_created_us=row['c'],
                repeated_later_observation=bool(row['flags'] & (1 << 30)),
                uniqueness_unavailable=bool(row['flags'] & (1 << 31)))
        elif row['event'] == 3:
            item.update(reason=REJECT_REASON[row['a']] if row['a'] < len(REJECT_REASON)
                        else 'UNKNOWN_%d' % row['a'], local_result=signed(row['b'], 32),
                        metadata_available=bool(row['flags'] & 1),
                        sequence=row['c'] if row['c'] <= 65535 else None,
                        stage_is_branch_location=True,
                        packet_discard_not_established_by_record_name=True)
            if row['a'] in UNINSTRUMENTED_REJECT:
                item.update(reason_hook_available=False,
                            measurement_interpretation=UNINSTRUMENTED_REJECT[row['a']])
            elif row['a'] == 8:
                item['queue_full_vs_stopping_distinguished'] = False
            elif row['a'] == 21:
                item.update(existing_branch_continues=True,
                            same_packet_PEER_HANDLER_OUT_observations_may_repeat=True)
            elif row['a'] == 12:
                item['preceding_SRTP_INPUT_observation_established'] = False
        elif row['event'] == 16:
            item.update(previous_stage_us=row['b'], measured_delay_us=row['c'])
        if 18 <= row['event'] <= 30:
            item = nack_record(item)
        elif row['event'] == 31:
            item['stage_name'] = 'NACK_GRACE'
            item = nack_grace_record(item)
        timeline.append(item)
    # Exact modular range identities only. Overlapping but nonidentical ranges
    # are not silently equated; RTX PT/SSRC remain separate unless a recorded
    # normalization explicitly relates them. Ordinals are retention order,
    # while monotonic times order observations from concurrent producers.
    gaps = [r for r in timeline if r['event'] == 1]
    by_identity = {}
    for row in gaps:
        by_identity.setdefault((row['ssrc'], row['payload_type'], row['a'], row['b'], row['c']), []).append(row)
    groups, group_ids = [], {}
    for key, matches in by_identity.items():
        group_ids[key] = len(groups)+1
        groups.append({'candidate_group_id': len(groups)+1,
            'ssrc': key[0], 'payload_type': key[1], 'expected_extended': key[2],
            'received_extended': key[3], 'skipped_positions': key[4],
            'stage_local_extension_epochs_aligned': False,
            'observations': [{key: r[key] for key in ('stage_name', 'stage', 't_us', 'identity', 'flags')}
                             for r in sorted(matches, key=lambda r: r['t_us'])]})
    late = {}
    for row in timeline:
        if row['event'] == 2:
            late.setdefault((row['stage'], row['identity']), []).append(row)
    rejects = [r for r in timeline if r['event'] == 3 and r['metadata_available']]
    chronology = []
    for row in gaps:
        direct_rejects = [r for r in rejects if r['ssrc'] == row['ssrc'] and
            r['sequence'] is not None and ((r['sequence']-row['a']) & 65535) < row['c'] and
            # Bound the time search to the observed gap episode. Different
            # sequence wraps over a long run must not match by sequence alone.
            row['t_us']-2000000 <= r['t_us'] <= row['t_us']+2000000]
        chronology.append(dict(row,
            exact_range_candidate_group_id=group_ids[(row['ssrc'], row['payload_type'], row['a'], row['b'], row['c'])],
            later_positions=late.get((row['stage'], row['identity']), [])[:16],
            later_position_detail_expansion_omitted=max(0, len(late.get((row['stage'], row['identity']), []))-16),
            nearby_explicit_reject_candidates=direct_rejects[:16],
            reject_candidate_detail_expansion_omitted=max(0, len(direct_rejects)-16),
            first_missing_stage_established=False,
            bounded_interpretation=('Discontinuity is observed at the datagram boundary; absence before socket '
                'delivery is unobserved, and a forward gap may be reordered later.' if row['stage'] == 0 else
                'Discontinuity observed at this stage; missing packet-specific earlier delivery evidence '
                'cannot establish the first stage that discarded it.'),
            packet_identity_correspondence_established=False,
            stage_local_extension_epochs_aligned=False,
            physical_network_loss_established=False))
    stages = []
    for stage, counts in enumerate(cr['stages']):
        sources = []
        for row in cr['sources'][stage]:
            if row[0]:
                sources.append(dict(zip(T_SOURCE, row[:19]), ssrc=row[0] & 0xffffffff,
                    payload_type=(row[0] >> 32) & 127, high_water_sequence=row[5] & 65535,
                    sequence_initialized=bool(row[5] & 65536),
                    gap_positions_are_physical_loss=False))
        stages.append(dict(zip(T_COUNTER, counts), stage=stage, name=TRANSPORT[stage],
            source_rows=sources, max_delay_CAS_omitted=cr['delay_omitted'][stage],
            maximum_is_lower_bound=bool(cr['delay_omitted'][stage]),
            branch_counters={REJECT_REASON[i] if i < len(REJECT_REASON) else 'UNKNOWN_%d' % i: n
                             for i, n in enumerate(cr['rejected'][stage]) if n},
            branch_counters_are_branch_observations=True,
            unavailable_branch_counter_slots={REJECT_REASON[i]: {
                'reason_id': i, 'raw_count': cr['rejected'][stage][i],
                'measurement_available': False, 'measured_outcomes': None,
                'interpretation': meaning}
                for i, meaning in UNINSTRUMENTED_REJECT.items()}))
    au = {name: p['event_attempts'][event_slot(event)] for name, event in
          (('structurally_complete_before_recovery_filter', 0x608),
           ('rejected_by_recovery_filter', 0x609), ('accepted_for_Decode_feed', 0x600))}
    au['filter_flag_histogram'] = {str(i): {'count': n,
        'missing': [name for bit, name in ((1, 'IDR'), (2, 'SPS'), (4, 'PPS')) if i & bit]}
        for i, n in enumerate(cr['au_filter_reasons']) if n}
    settings = []
    for r in timeline:
        if r['event'] != 17:
            continue
        item = dict(t_us=r['t_us'], kind=r['identity'], a=r['a'], b=r['b'], c=r['c'])
        if r['identity'] in (10, 11, 12, 13):
            item.update(scope='local_offer' if r['a'] >> 32 == 0 else
                        'accepted_answer' if r['a'] >> 32 == 1 else 'unknown',
                        media_index=(r['a'] >> 16) & 65535, payload_type=r['a'] & 65535)
        elif r['identity'] == 19:
            item.update(scope='local_offer' if r['a'] == 0 else
                        'accepted_answer' if r['a'] == 1 else 'unknown',
                        summary_records=r['b'], summary_capped=bool(r['c']))
        if r['identity'] == 10:
            item['codec'] = {1: 'H264', 2: 'Opus', 3: 'RTX'}.get(r['b'], 'other')
        elif r['identity'] == 11:
            item['primary_apt'] = r['b']
        elif r['identity'] == 13:
            item['feedback_announced'] = [name for bit, name in
                ((1, 'nack'), (2, 'nack_pli'), (4, 'ccm_fir'), (8, 'goog_remb')) if r['b'] & bit]
        elif r['identity'] == 3:
            item['wire_RTX'] = {'ssrc': r['a'] >> 32, 'pt': (r['a'] >> 16) & 127, 'seq': r['a'] & 65535}
            item['normalized_original'] = {'ssrc': r['b'] >> 32, 'pt': (r['b'] >> 16) & 127, 'seq': r['b'] & 65535}
        elif r['identity'] == 4:
            item.update(track_handle=r['a'], handler_RTX_enabled=bool(r['b'] & 1),
                        handler_FIR_enabled=bool(r['b'] & 2), primary_ssrc=r['c'])
        elif r['identity'] == 20:
            item.update(receiving_handler_installed=bool(r['a']),
                        receiver_NACK_generator_installed=bool(r['b']),
                        sender_NACK_responder_installed=bool(r['c']))
        elif r['identity'] == 22:
            item.update(thread_slot_omissions_process_total=r['a'],
                        thread_slots_used=r['b'],
                        snapshot_phase='attach' if r['c'] == 0 else 'detach' if r['c'] == 1 else 'unknown')
        elif r['identity'] == 23:
            item.update(process_trace_bindings=r['a'], process_global_sink=bool(r['b']),
                        transport_owner_epoch_association_available=bool(r['c']))
        elif r['identity'] == 24:
            item.update(local_feedback_check_error=signed(r['a']),
                        answer_checked=None, remote_feedback_accepted=None,
                        error_establishes_feedback_not_supported=False,
                        media_session_failure_established=False)
        settings.append(item)
    return {'available': True, 'schema': 'X4CRIT2/v2', 'stages': stages,
        'coverage': {'retained_records': h[5], 'critical_attempts': h[6],
            'gate_omitted': h[7], 'ring_overwrites': h[8], 'source_slot_omitted': h[13],
            'sequence_gate_omitted': h[14], 'gap_id_counter': h[15],
            'first_retained_ordinal': timeline[0]['ordinal'] if timeline else None,
            'last_retained_ordinal': timeline[-1]['ordinal'] if timeline else None,
            'retained_time_min_us': min((r['t_us'] for r in timeline), default=None),
            'retained_time_max_us': max((r['t_us'] for r in timeline), default=None),
            'detail_complete': not (h[7] or h[8]),
            'sequence_observation_complete': not (h[13] or h[14]),
            'event_counts': {CRITICAL[i] if i < len(CRITICAL) else 'UNKNOWN_%d' % i:
                {'attempts': a, 'omitted': b, 'retained': a-b}
                for i, (a, b) in enumerate(zip(cr['attempted'], cr['omitted'])) if a or b},
            'packet_payloads_retained': False, 'per_packet_history_complete': False},
        'retention': {'prehistory_cap': h[18], 'body_cap_including_prehistory': h[19],
            'reserved_end_capacity': h[20], 'duplicate_window_open_attempts': h[21],
            'context_retention_omissions': h[22]},
        'AU_stage_totals': au,
        'retained_negotiation_and_RTX_settings': settings,
        'receiver_NACK': nack_analysis(cr, timeline),
        'receiver_NACK_grace': nack_grace_analysis(cr, timeline),
        'exact_range_candidate_groups': groups,
        'retained_gap_chronology': sorted(chronology, key=lambda r: (r['t_us'], r['ordinal'])),
        'retained_critical_timeline': timeline,
        'limitations': ['Unauthenticated clear RTP headers are candidate identities, not trusted media.',
            'Datagram, direct ICE and TURN-decapsulated observations have different populations.',
            'REJECT and branch counters describe branch observations, not universal packet-discard counts.',
            'DTLS_QUEUE_FULL_OR_STOPPING does not distinguish a full queue from a stopping queue.',
            'ICE_CALLBACK_EXCEPTION and SOCKET_FAIRNESS have no hooks; their raw zero counters are unmeasured.',
            'RTX_UNWRAP_UNAVAILABLE preserves the continuing upstream branch and can repeat the same wire RTX at PEER_HANDLER_OUT.',
            'DEMUX_UNKNOWN uses a branch-location label; it does not establish a preceding SRTP_INPUT packet observation.',
            'A later position can be reordered traffic or retransmission; flags/negotiated mapping are needed.',
            'Sparse gap records and cumulative totals do not retain every individual packet at every stage.',
            'Nearby explicit rejects are candidates, not proven payload identities across wraps or PT mappings.',
            'Successful PLI API results establish only the local API outcome.',
            'There is no native-output PTS association with the last Decode input.']}


def analyze(data):
    p = parse(data)
    h, raw = p['header'], p['header_words']
    counter_wraps = [name for name, values in (('event_attempts', p['event_attempts']),
        ('event_drops', p['event_drops']), ('histogram', p['histogram']),
        ('epoch_exposure', p['exposure'][::2])) if sum(values) > U64]
    all_records = p['history'] + [r for w in p['windows'] for r in w['records']]
    records, duplicates, ordinal_conflicts = deduplicate(all_records)
    marks = [dict(zip(('t_us', 'ordinal', 'epoch', 'active'), row)) for row in p['phase_ledger']]
    marks_complete = (not h['phase_ledger_omitted'] and len(marks) == h['manual_epoch_marks'] and
        all(mark['ordinal'] == i+1 for i, mark in enumerate(marks)) and
        all(a['t_us'] <= b['t_us'] for a, b in zip(marks, marks[1:])))
    # An intervening manual phase change matters even if both endpoints are stable.
    cadence = []
    for end, generation, interval, epochs in p['cadence']:
        begin = end-interval if end >= interval else None
        begin_epoch, end_epoch = (epochs >> 32) & 0x7fffffff, epochs & 0xffffffff
        encoded_mixed = bool(epochs & (1 << 63))
        crossed = encoded_mixed or begin_epoch != end_epoch or any(begin is not None and begin < m['t_us'] <= end for m in marks)
        cadence.append({'end_us': end, 'begin_us': begin, 'generation': generation,
            'interval_us': interval, 'begin_epoch': EPOCHS[begin_epoch], 'end_epoch': EPOCHS[end_epoch],
            'mixed_epoch_flag': encoded_mixed,
            'phase_crossing_observed': crossed, 'manual_boundary_records_complete': marks_complete,
            'stable_only_eligible': begin is not None and begin_epoch == end_epoch == 2 and not crossed and marks_complete,
            'closed_gt100ms': interval > 100000})
    ledger_complete = not h['cadence_omitted'] and h['cadence_count'] == h['interval_count']
    cadence_valid = (not h['clock_errors'] and not h['sampler_clock_errors'] and not h['cadence_identity_errors'] and
                    all(c['begin_us'] is not None for c in cadence) and
                    all(a['end_us'] <= b['end_us'] for a, b in zip(cadence, cadence[1:])))
    epoch_results = {}
    for epoch, name in enumerate(EPOCHS):
        duration, n = p['exposure'][epoch*2:epoch*2+2]
        selected = [c for c in cadence if c['begin_epoch'] == c['end_epoch'] == name and not c['phase_crossing_observed']]
        complete = ledger_complete and cadence_valid and marks_complete and not counter_wraps
        long = [c['interval_us'] for c in selected if c['closed_gt100ms']]
        epoch_results[name] = {'active_duration_us': duration, 'endpoint_same_epoch_interval_count': n,
            'retained_non_crossing_intervals': len(selected), 'retained_closed_gt100ms': len(long),
            'closed_gap_rate_per_min': len(long)*60000000/duration if duration and complete else None,
            'rate_population_complete': complete, 'retained_gap_distribution': distribution(long),
            'denominator': 'manual active epoch exposure, not first-to-last NEW',
            'boundary_limitation': None if marks_complete else 'manual phase boundaries missing; endpoint epoch equality insufficient'}
    counters = {}
    used_slots = {event_slot(event) for event in EVENTS}
    for event, name in EVENTS.items():
        slot = event_slot(event)
        if p['event_attempts'][slot] or p['event_drops'][slot]:
            counters[name] = {'event': event, 'attempts': p['event_attempts'][slot], 'detail_drops': p['event_drops'][slot]}
    rx = []
    for row in p['rx']:
        if not row[0]:
            continue
        kind, ssrc = (row[0] >> 32)-1, row[0] & 0xffffffff
        rx.append(dict(zip(('key', 'valid_callbacks', 'bytes', 'last_us', 'sequence_version',
            'last_sequence_encoded', 'forward_skipped_positions', 'backward_or_duplicate_observations',
            'sequence_unknown_observations', 'first_us'), row[:10]), kind=kind, ssrc=ssrc,
            last_sequence=row[5] & 65535, last_sequence_present=bool(row[5] & 65536),
            sequence_snapshot_coherent=not bool(row[4] & 1),
            skipped_positions_are_network_loss=False, source_epoch_association='local session + kind/SSRC only'))
    pli = []
    for row in records:
        if row['event'] == 0x606:
            pli.append({'attempt_id': row['a'], 't_us': row['t_us'],
                'local_result': signed(row['b'], 32) if row['b'] <= 0xffffffff else None,
                'remote_delivery_or_acknowledgment_measured': False})
    spans = {name: durations(records, begin, end, helpers) for name, begin, end, helpers in (
        ('Decode', 0x200, 0x201, False), ('native_copy', 0x206, 0x207, False),
        ('conversion', 0x20c, 0x20d, False), ('draw', 0x300, 0x301, False),
        ('helper_span', 0x222, 0x223, True), ('helper_wait', 0x224, 0x225, False))}
    record_counts = Counter(EVENTS.get(r['event'], 'UNKNOWN_%04x' % r['event']) for r in records)
    context_omissions = bool(p['critical'] and (p['critical']['header'][22] or
        any(w['header']['reserved14'] for w in p['windows'])))
    negative_complete = (not context_omissions and not h['coverage_flags'] and not h['event_drops'] and
                         not h['sample_omitted'] and not h['history_overwrites'] and
                         not h['sampler_detail_gate_omitted'] and
                         not ordinal_conflicts and not counter_wraps)
    sampled = sample_analysis(p['samples'])
    return {'schema': 'X4PROG%d/v%d analysis' % (raw[1], raw[1]), 'input_sha256': hashlib.sha256(data).hexdigest(),
        'input_bytes': len(data), 'private_numeric_evidence': True,
        'identity': p['identity'], 'header': h, 'requested_config': p['config'],
        'delivered_geometry_observations': [{'t_us': r['t_us'], 'native_output_id': r['a'],
            'width': r['b'] & 65535, 'height': (r['b'] >> 16) & 65535, 'pitch': r['b'] >> 32}
            for r in records if r['event'] == 0x228],
        'coverage': {'flags': flags(h['coverage_flags']), 'window_flags': [flags(w['header']['coverage_flags']) for w in p['windows']],
            'negative_detail_evidence_complete': negative_complete, 'deduplicated_records': len(records),
            'overlap_duplicates_removed': duplicates, 'ordinal_conflicts_or_wraps': ordinal_conflicts,
            'detected_cumulative_wrap_indicators': counter_wraps,
            'cadence_ledger_complete': ledger_complete, 'cadence_clock_identity_valid': cadence_valid,
            'manual_phase_records_complete': marks_complete, 'open_gap_at_stop': bool(h['open_gap_at_stop'])},
        'presentation': {'retained_cadence': cadence, 'retained_interval_distribution': distribution([c['interval_us'] for c in cadence]),
            'distribution_is_every_interval': ledger_complete and cadence_valid,
            'all_interval_histogram_bounds': histogram_bounds(p['histogram']),
            'closed_gaps_gt100ms_total': h['closed_gaps_gt_100ms'],
            'open_gap_duration_us': None, 'epochs': epoch_results},
        'independent_event_counters': counters,
        'unmapped_event_counter_slots': {str(i): {'attempts': a, 'detail_drops': b}
            for i, (a, b) in enumerate(zip(p['event_attempts'], p['event_drops'])) if i not in used_slots and (a or b)},
        'reset_primary_attempts': {RESET[i] if i < len(RESET) else 'unknown_%d' % i: n for i, n in enumerate(p['reset_primary'][:32]) if n},
        'discarded_open_AU_primary_attempts': {RESET[i] if i < len(RESET) else 'unknown_%d' % i: n for i, n in enumerate(p['reset_primary'][32:]) if n},
        'rx_kind_totals': [dict(zip(('callbacks', 'valid_callbacks', 'bytes', 'last_callback_us', 'last_valid_us'), raw[48+k*5:53+k*5])) for k in range(3)],
        'queue_rejected_callbacks': dict(zip(('RTP_invalid', 'size', 'full', 'producer_contention', 'stopped', 'other'), raw[72:78])),
        'RTP_rejected_reason_indices': {str(i): n for i, n in enumerate(raw[78:86])},
        'manual_phase_ledger': marks, 'rx_sources': rx, 'sampler': sampled, 'retained_operation_spans': spans,
        'closed_gap_chronology': gap_chronology(cadence, records, sampled, not negative_complete),
        'retained_recovery': recovery(records), 'retained_PLI_local_results': pli,
        'upstream_diagnostics': critical_analysis(p),
        'retained_event_counts': dict(record_counts), 'retained_timeline': [pipeline_record(r) for r in records],
        'retained_RX_observations': [{'t_us': r['t_us'], 'kind': r['flags'] & 255,
            'valid': bool(r['flags'] & 256), 'ssrc': r['a'] >> 32 if r['flags'] & 256 else None,
            'sequence': r['a'] & 65535 if r['flags'] & 256 else None, 'bytes': r['b']}
            for r in records if r['event'] == 0x61a],
        'windows': [w['header'] for w in p['windows']],
        'limits': ['Counters are individually atomic, not one global sample snapshot.',
            'Zero retained details never establishes absence when admission, overwrite, caps or unstable reads occur.',
            'No RX progress may reflect network/server/pre-callback scheduling; some RX does not prove all needed packets arrived.',
            'Source sequence skips are observations, not Internet packet loss.',
            'Local PLI attempt/result never establishes remote delivery or acknowledgment.',
            'AU, Decode, native output, RGB and flip IDs are distinct; there is no returned native-output PTS association.',
            'Operation wall spans include scheduling; local NEW matches are not provider FPS or physical scanout.',
            'Unknown/wrapped counters or absent phase boundaries do not supply stable-only rates.']}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('trace', type=Path)
    ap.add_argument('--out', type=Path, help='Create a new private JSON file; never overwrite')
    args = ap.parse_args(argv)
    try:
        with args.trace.open('rb') as file:
            data = file.read(MAX_BYTES+1)
        result = analyze(data)
        encoded = json.dumps(result, indent=2, ensure_ascii=True, allow_nan=False)+'\n'
        if args.out:
            with args.out.open('x', encoding='utf-8', newline='\n') as file:
                file.write(encoded)
        else:
            sys.stdout.write(encoded)
    except (OSError, ValueError, struct.error) as exc:
        print('Progress trace rejected: %s' % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
