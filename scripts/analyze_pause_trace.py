#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read-only analysis of bounded XCloud4 two/four-reader numeric traces.

All times are local monotonic observations. This tool neither starts XCloud4
nor connects to a console. Negative evidence is disabled for incomplete
captures. Native output IDs never identify the AU passed to the last Decode.
"""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
import sys

MAX_BYTES = 8 * 1024 * 1024
HEADER = struct.Struct('<32Q')
RECORD = struct.Struct('<QQQIHH')
MAGIC = 0x3145434152543458
COUNTER_LAYOUT = (512 << 32) | 64
HEADER_NAMES = (
    'magic', 'schema', 'endian', 'header_bytes', 'record_bytes',
    'window_header_bytes', 'local_session', 'readers', 'window_capacity',
    'window_count', 'history_capacity', 'history_count', 'admitted_records',
    'history_overwrites', 'exact_closed_long_intervals', 'observed_ongoing_intervals',
    'retained_closed_details', 'late_closed_detections', 'missing_gap_details',
    'storage_exhaustions', 'admission_drops', 'monitor_gate_skips',
    'unstable_snapshots', 'event_capped_windows', 'time_capped_windows',
    'open_at_stop', 'first_new_us', 'last_new_us', 'max_detection_excess_us',
    'coverage_bits', 'stage_count', 'counter_layout',
)
WINDOW_NAMES = (
    'ordinal', 'record_count', 'coverage_bits', 'requested_pre_start_us',
    'min_stored_us', 'max_stored_us', 'first_gap_id', 'last_gap_id',
    'first_gap_start_us', 'opened_us', 'last_closed_us', 'post_deadline_us',
    'frozen_us', 'freeze_reason', 'history_overwrites_at_open', 'reserved',
    'omitted_records', 'gap_begins', 'gap_closes', 'drops_at_open',
    'drops_at_freeze', 'closed_at_open', 'closed_at_freeze',
    'ongoing_at_open', 'ongoing_at_freeze', 'gap_open_at_freeze',
    'first_admitted_ordinal', 'last_admitted_ordinal', 'pre_records',
    'appended_records', 'required_pre_us', 'required_post_us',
)
EVENTS = {}
for base, names in (
    (0x100, 'AU_BEGIN AU_COMPLETE AU_RESET WAIT_IDR IDR_ACCEPT PLI_REQUEST '
     'PLI_DISPATCH QUEUE_POP QUEUE_ACCEPT QUEUE_DROP REORDER_INSERT REORDER_EMIT '
     'REORDER_HOLE REORDER_DROP WORKER_YIELD MEDIA_START MEDIA_STOP SOURCE_EPOCH'),
    (0x200, 'DECODE_BEGIN DECODE_END OUTPUT_VALID OUTPUT_NONE OUTPUT_REJECT '
     'OUTPUT_SUPERSEDED COPY_BEGIN COPY_END COPY_OWNER_ELAPSED COPY_HELPER_ELAPSED '
     'COPY_WAIT_ELAPSED COPY_CHECK CONVERT_BEGIN CONVERT_END RGB_PUBLICATION RGB_SUPERSEDED'),
    (0x300, 'DRAW_BEGIN DRAW_END DRAW_SKIP FLIP_SUBMIT GNM_DONE FLIP_MATCH '
     'PRESENT_NEW PRESENT_REPEAT FLIP_FAIL'),
    (0x400, 'RX_VALID RX_REJECT RX_CALLBACK'),
    (0x500, 'SESSION_START ACTIVE_CHANGE GAP_BEGIN GAP_CLOSED WINDOW_OPEN WINDOW_FROZEN SESSION_END'),
    (0x120, 'DAMAGE_FIRST IDR_SEEN SUBMIT_BEGIN SUBMIT_END RESET_TOTAL DAMAGE_TOTAL '
     'WAIT_BEGIN WAIT_END POP_CONTENDED'),
    (0x220, 'OWNER_SPAN_BEGIN OWNER_SPAN_END HELPER_SPAN_BEGIN HELPER_SPAN_END '
     'HELPER_WAIT_BEGIN HELPER_WAIT_END BYTECHECK_BEGIN BYTECHECK_END OUTPUT_GEOMETRY '
     'VIDEO_ERROR VIDEO_START VIDEO_STOP FEED_REJECT PRESERVE_BEGIN PRESERVE_END COPY_CONFIG'),
):
    EVENTS.update((base + i, name) for i, name in enumerate(names.split()))
RESET_NAMES = (
    'normal_submitted', 'timestamp_change', 'marker_incomplete',
    'waiting_recovery', 'parameter_prefix_capacity', 'feed_error',
    'coalesced_ingress_gap', 'new_track', 'reorder_jump', 'reorder_hole',
)
DAMAGE_NAMES = (
    'forbidden_nal', 'invalid_nal_type', 'single_or_stap_during_fu',
    'short_stap_length', 'invalid_stap_length', 'stap_trailing_bytes',
    'bad_fu_header', 'nested_fu_start', 'orphan_fu_continuation',
    'fu_header_mismatch', 'annex_b_capacity', 'discarded_timestamp',
    'unsupported_or_short_packetization',
)
COVERAGE = {
    1: 'trace_admission_drops', 2: 'short_prehistory', 4: 'event_cap',
    8: 'time_cap', 16: 'saved_window_storage_cap', 32: 'unstable_cadence_snapshot',
    64: 'missing_gap_detail', 128: 'interrupted_open_gap',
    256: 'record_ordinal_wrap', 512: 'identity_or_clock_error',
}


def event_name(event):
    return EVENTS.get(event, 'UNKNOWN_%04x' % event)


def reset_name(reason):
    return RESET_NAMES[reason] if reason < len(RESET_NAMES) else 'unknown_%d' % reason


def coverage(bits):
    names = [name for bit, name in COVERAGE.items() if bits & bit]
    if bits & ~1023:
        names.append('unknown_coverage_bits')
    return names


def distribution(values):
    """Nearest-rank quantiles over only explicitly matched local observations."""
    values = sorted(values)
    if not values:
        return {'count': 0, 'p50_us': None, 'p95_us': None, 'max_us': None}
    return {'count': len(values), 'p50_us': values[math.ceil(.50 * len(values)) - 1],
            'p95_us': values[math.ceil(.95 * len(values)) - 1], 'max_us': values[-1]}


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def unpack(self, layout):
        if self.at + layout.size > len(self.data):
            raise ValueError('Truncated trace at byte %d' % self.at)
        result = layout.unpack_from(self.data, self.at)
        self.at += layout.size
        return result

    def records(self, count):
        if count > 16384:
            raise ValueError('Record count exceeds the fixed capture bound')
        result = []
        for _ in range(count):
            t, a, b, ordinal, event, flags = self.unpack(RECORD)
            result.append({'t': t, 'a': a, 'b': b, 'ordinal': ordinal,
                           'event': event, 'flags': flags})
        return result


def parse(data):
    reader = Reader(data)
    h = dict(zip(HEADER_NAMES, reader.unpack(HEADER)))
    fixed = {'magic': MAGIC, 'schema': 1, 'endian': 0x0102030405060708,
             'header_bytes': 256, 'record_bytes': 32, 'window_header_bytes': 256,
             'window_capacity': 12, 'history_capacity': 8192,
             'stage_count': 8, 'counter_layout': COUNTER_LAYOUT}
    for name, expected in fixed.items():
        if h[name] != expected:
            raise ValueError('Unsupported or damaged %s' % name)
    if h['readers'] not in (2, 4):
        raise ValueError('Unsupported configured reader count')
    if h['window_count'] > 12 or h['history_count'] > 8192:
        raise ValueError('Header count exceeds a fixed capacity')
    stage_rows = [reader.unpack(struct.Struct('<4Q')) for _ in range(8)]
    event_rows = [reader.unpack(struct.Struct('<3Q')) for _ in range(512)]
    reset_rows = reader.unpack(struct.Struct('<64Q'))
    for attempted, admitted, dropped, reserved in stage_rows:
        if reserved or attempted != admitted + dropped:
            raise ValueError('Inconsistent quiescent stage counters')
    if any(a != b + c for a, b, c in event_rows):
        raise ValueError('Inconsistent quiescent event counters')
    if sum(row[1] for row in stage_rows) != h['admitted_records']:
        raise ValueError('Admitted-record total does not match stage counters')
    if h['history_count'] + h['history_overwrites'] != h['admitted_records']:
        raise ValueError('Rolling-history count does not match admitted records')
    if sum(row[2] for row in stage_rows) != h['admission_drops']:
        raise ValueError('Admission-drop total does not match stage counters')
    if tuple(sum(row[i] for row in event_rows) for i in range(3)) != tuple(
            sum(row[i] for row in stage_rows) for i in range(3)):
        raise ValueError('Event and stage counter totals disagree')
    if sum(reset_rows) != event_rows[64 + 2][0]:
        raise ValueError('Reset attempts do not match their dedicated event counter')
    windows = []
    for index in range(h['window_count']):
        w = dict(zip(WINDOW_NAMES, reader.unpack(HEADER)))
        if (w['ordinal'] != index + 1 or w['reserved'] or w['record_count'] > 16384
                or w['pre_records'] > 8192
                or w['pre_records'] + w['appended_records'] != w['record_count']
                or w['required_pre_us'] != 500000 or w['required_post_us'] != 500000):
            raise ValueError('Invalid window header %d' % (index + 1))
        windows.append((w, reader.records(w['record_count'])))
    history = reader.records(h['history_count'])
    if reader.at != len(data):
        raise ValueError('Unexpected trailing trace data')
    for records in [r for _, r in windows] + [history]:
        for r in records:
            if r['event'] in (0x222, 0x223):
                low = r['flags'] & 255
                if (r['flags'] >> 8 >= h['readers'] - 1
                        or low not in ((0,) if r['event'] == 0x222 else (0, 1))):
                    raise ValueError('Invalid helper identity/result flags')
    # Ordinals are admission identities, NOT physical ordering across producers.
    if not (h['coverage_bits'] & 256):
        identities = {}
        for records in [r for _, r in windows] + [history]:
            for r in records:
                old = identities.setdefault(r['ordinal'], r)
                if not r['ordinal'] or old != r:
                    raise ValueError('Conflicting admitted-record identity')
    return h, stage_rows, event_rows, reset_rows, windows, history


def unique_records(groups, wrapped=False):
    # With ordinal wrap, deduplicate the entire record; identity attribution is
    # explicitly unavailable. Do not collapse different records at one ordinal.
    records = {}
    for group in groups:
        for r in group:
            key = tuple(r.values()) if wrapped else r['ordinal']
            records[key] = r
    return sorted(records.values(), key=lambda r: (r['t'], r['ordinal']))


def paired_durations(records, begin_event, end_event, begin_key='a', end_key='a'):
    starts, values, incomplete, ambiguous = {}, [], 0, set()
    def identity(r, field):
        # Older two-reader traces use helper index zero. Four-reader records
        # carry the immutable helper index in the flag high byte. Low flags
        # describe result and cannot form the BEGIN/END identity.
        return (r['a'], r['flags'] >> 8) if field == 'copy_helper' else r[field]
    for r in records:
        if r['event'] == begin_event:
            key = identity(r, begin_key)
            if key in starts:
                ambiguous.add(key)
            starts[key] = r['t']
        elif r['event'] == end_event:
            key = identity(r, end_key)
            start = starts.pop(key, None)
            if start is None or key in ambiguous or r['t'] < start:
                incomplete += 1
            else:
                values.append(r['t'] - start)
    result = distribution(values)
    result['unpaired_or_ambiguous'] = incomplete + len(starts)
    return result


def timings(records, wrapped):
    pairs = {
        'au_begin_to_marker_observation': (0x100, 0x101),
        'native_decode_call': (0x200, 0x201), 'selected_copy_observation': (0x206, 0x207),
        'owner_span_observation': (0x220, 0x221),
        'helper_span_observation': (0x222, 0x223, 'copy_helper', 'copy_helper'),
        'helper_wait_observation': (0x224, 0x225),
        'conversion_observation': (0x20c, 0x20d), 'draw_observation': (0x300, 0x301),
        'submitted_flip_to_match': (0x303, 0x305, 'b', 'a'),
    }
    result = {name: paired_durations(records, *pair) for name, pair in pairs.items()}
    helper_indices = sorted({r['flags'] >> 8 for r in records if r['event'] in (0x222, 0x223)})
    result['helper_span_observation_by_index'] = {
        str(index): paired_durations([r for r in records if r['event'] in (0x222, 0x223)
                                      and r['flags'] >> 8 == index],
                                     0x222, 0x223, 'copy_helper', 'copy_helper')
        for index in helper_indices}
    result['queue_arrival_to_pop'] = distribution([r['b'] for r in records if r['event'] == 0x107])
    callbacks = [r for r in records if r['event'] == 0x402]
    if callbacks:
        result['callback_elapsed_excludes_final_record'] = distribution([r['b'] >> 32 for r in callbacks])
        result['ingress_reservation_monotonic_elapsed'] = distribution([r['b'] & 0xffffffff for r in callbacks])
        result['callback_concurrency_retained_max'] = max(r['a'] >> 32 for r in callbacks)
        result['reservation_retries_retained'] = sum(r['flags'] & 31 for r in callbacks)
    result['helper_wait_reported'] = distribution([r['b'] for r in records if r['event'] == 0x225])
    result['copy_wall_reported_excludes_reference_check'] = distribution(
        [r['b'] for r in records if r['event'] == 0x207])
    result['conversion_wall_reported'] = distribution([r['b'] for r in records if r['event'] == 0x20d])
    # AU -> Decode is an explicit submitted-input association. It does not imply
    # that any native output belongs to that input, even when returned that call.
    marker, outputs, publications = {}, {}, {}
    before_decode, output_to_rgb, rgb_to_present = [], [], []
    for r in records:
        event, t, a, b = r['event'], r['t'], r['a'], r['b']
        if event == 0x101 and not (r['flags'] & 7):
            marker[a] = t
        elif event == 0x200 and b in marker and t >= marker[b]:
            before_decode.append(t - marker[b])
        elif event == 0x202:
            outputs[a] = t
        elif event == 0x20e:
            publications[a] = t
            if b in outputs and t >= outputs[b]:
                output_to_rgb.append(t - outputs[b])
        elif event == 0x306 and a in publications and t >= publications[a]:
            rgb_to_present.append(t - publications[a])
    result['valid_au_marker_to_decode_entry'] = distribution(before_decode)
    result['leased_native_output_to_rgb_publication'] = distribution(output_to_rgb)
    result['rgb_publication_to_new_flip_match'] = distribution(rgb_to_present)
    result['record_ordinal_identity_valid'] = not wrapped
    result['limits'] = [
        'Capture omissions can bias every distribution; these are retained observations only.',
        'No time is assigned from an input AU through the decoder to an output.',
        'Helper END is recorded after release of completion; observed ordering can trail conversion.',
        'Owner, helper and wait durations overlap; do not add them as CPU or wall time.',
        'No provider send time, physical scanout time or control-to-image latency is measured.',
    ]
    return result


def reset_observation(r):
    reason, detail, damage = r['b'] >> 48, (r['b'] >> 32) & 65535, r['b'] & 0xffffffff
    return {'t_us': r['t'], 'au_id': r['a'], 'reason': reset_name(reason),
            'reason_number': reason, 'detail_bits': detail,
            'damage_bits': damage, 'damage': [n for i, n in enumerate(DAMAGE_NAMES) if damage & (1 << i)],
            'gap_reset': bool(r['flags'] & 1), 'au_open': bool(r['flags'] & 2),
            'waiting_before': bool(r['flags'] & 64), 'waiting_after': bool(r['flags'] & 128)}


def exit_totals(records, wrapped):
    # These are exact owner counters emitted at worker exit, unlike window
    # events. A missing row is unknown, never an invented zero.
    calls = {name: None for name in RESET_NAMES}
    discarded = {name: None for name in RESET_NAMES}
    damage = {name: None for name in DAMAGE_NAMES}
    conflicts = set()
    for r in records:
        if r['event'] == 0x124 and r['a'] < len(RESET_NAMES) and r['flags'] in (0, 1):
            table = discarded if r['flags'] else calls
            name = reset_name(r['a'])
            label = ('discarded:' if r['flags'] else 'calls:') + name
        elif r['event'] == 0x125 and r['a'] < len(DAMAGE_NAMES):
            table, name = damage, DAMAGE_NAMES[r['a']]
            label = 'damage:' + name
        else:
            continue
        if table[name] is not None and table[name] != r['b']:
            conflicts.add(label)
            table[name] = None
        elif label not in conflicts:
            table[name] = r['b']
    complete = (not wrapped and not conflicts and
                all(n is not None for table in (calls, discarded, damage) for n in table.values()))
    return {'all_reset_calls_by_reason': calls, 'discarded_open_au_by_reason': discarded,
            'discarded_open_au_by_overlapping_damage_bit': damage,
            'complete_retained_exit_totals': complete, 'conflicting_rows': sorted(conflicts),
            'limits': 'Missing rows are unknown. Damage bits overlap. Reset calls can discard no AU.'}


def classify(records, start, end, safe_negative):
    during = [r for r in records if start < r['t'] < end]
    counts = Counter(event_name(r['event']) for r in during)
    valid_au = sum(r['event'] == 0x101 and not (r['flags'] & 7) for r in during)
    rx, native, rgb = counts['RX_VALID'], counts['OUTPUT_VALID'], counts['RGB_PUBLICATION']
    candidates = []
    if safe_negative:
        if not rx:
            candidates.append('no_valid_rtp_observed_network_server_callback_parse_not_distinguished')
        if rx and not valid_au:
            candidates.append('valid_rtp_without_valid_complete_au_review_reorder_loss_assembly')
        if valid_au and not native:
            candidates.append('valid_au_without_native_output_review_recovery_idr_decode')
        if native and not rgb:
            candidates.append('native_output_without_rgb_review_copy_wait_conversion')
        if rgb:
            candidates.append('rgb_during_new_presentation_gap_review_main_draw_videoout')
    elif rgb:
        candidates.append('positive_rgb_progress_during_gap_absence_claims_disabled')
    if not candidates:
        candidates.append('unclassified_incomplete_or_mixed_progress')
    resets = [reset_observation(r) for r in during if r['event'] == 0x102]
    last_progress = {}
    for r in during:
        if r['event'] in (0x400, 0x101, 0x200, 0x201, 0x202, 0x206, 0x207,
                          0x20c, 0x20d, 0x20e, 0x300, 0x301, 0x303, 0x305):
            last_progress[event_name(r['event'])] = r['t']
    return {'candidates': candidates, 'root_cause_proven': False,
            'absence_claims_enabled': safe_negative, 'event_counts': dict(counts),
            'valid_complete_au_observations': valid_au, 'last_progress_us': last_progress,
            'reset_reason_counts': dict(Counter(r['reason'] for r in resets)),
            'reset_observations': resets,
            'wait_begin_observations': counts['WAIT_BEGIN'], 'wait_end_observations': counts['WAIT_END'],
            'limits': 'Upstream/downstream counts are independent; progress may use older buffered data.'}


def analyze(data):
    h, stages, events, resets, windows, history = parse(data)
    wrapped = bool(h['coverage_bits'] & 256)
    all_records = unique_records([r for _, r in windows] + [history], wrapped)
    reports, closed_ids = [], set()
    for w, records in windows:
        ordered = unique_records([records], wrapped)
        reasons = coverage(w['coverage_bits'] | h['coverage_bits'])
        if w['omitted_records']:
            reasons.append('window_omitted_records')
        # Even a drop outside this window conservatively disables negative claims.
        safe_negative = not reasons and not h['admission_drops'] and not wrapped
        gaps = []
        closed = {r['a']: r for r in ordered if r['event'] == 0x503}
        begins = {r['a']: r for r in ordered if r['event'] == 0x502}
        context_gap_ids = []
        for gap_id in sorted(set(closed) | set(begins)):
            # Other windows' delayed BEGIN metadata can enter prehistory
            # without their earlier CLOSED record. Keep it as context only.
            if not (w['first_gap_id'] <= gap_id <= w['last_gap_id']):
                context_gap_ids.append(gap_id)
                continue
            close, begin = closed.get(gap_id), begins.get(gap_id)
            if close:
                if close['b'] <= 100000 or close['b'] > close['t']:
                    raise ValueError('Invalid closed-gap duration')
                start, end = close['t'] - close['b'], close['t']
                if begin and begin['b'] != start:
                    raise ValueError('Closed gap disagrees with its start metadata')
                closed_ids.add(gap_id)
            else:
                start, end = begin['b'], w['frozen_us']
                if end < start:
                    raise ValueError('Invalid open-gap bounds')
            complete_bounds = w['min_stored_us'] <= start and w['max_stored_us'] >= end
            gap = {'id': gap_id, 'closed': close is not None, 'start_us': start,
                   'end_us': end if close else None,
                   'closed_interval_us': close['b'] if close else None,
                   'late_detection': bool((close or begin)['flags'] & 32),
                   'coverage_limits': reasons + ([] if complete_bounds else ['gap_bounds_not_fully_retained'])}
            gap.update(classify(ordered, start, end,
                                safe_negative and complete_bounds and close is not None))
            gaps.append(gap)
        reports.append({'header': w, 'coverage_limits': reasons,
                        'prehistory_context_gap_ids': context_gap_ids,
                        'gaps': gaps, 'timings': timings(ordered, wrapped)})
    event_totals = []
    for index, row in enumerate(events):
        if any(row):
            event = ((index // 64) << 8) | (index % 64)
            event_totals.append({'event_hex': '%04x' % event, 'event': event_name(event),
                                 'attempted': row[0], 'admitted': row[1], 'admission_dropped': row[2]})
    return {'format': 'XCloud4 numeric pause analysis schema 1 (two/four readers)',
            'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
            'threshold_us': 100000, 'header': h,
            'coverage_limits': coverage(h['coverage_bits']),
            'configured_reader_count_is_not_actual_parallel_selection': True,
            'stage_counters': [{'stage': i, 'attempted': r[0], 'admitted': r[1],
                                'admission_dropped': r[2]} for i, r in enumerate(stages)],
            'event_attempt_totals': event_totals,
            'reset_primary_attempt_totals': {reset_name(i): n for i, n in enumerate(resets) if n},
            'authoritative_totals_from_retained_exit_records': exit_totals(all_records, wrapped),
            'retained_distinct_closed_gap_ids': len(closed_ids),
            'windows': reports, 'all_retained_observation_timings': timings(all_records, wrapped),
            'limits': [
                'A gap measures NEW local flip-match observations; 100ms is not end-to-end latency.',
                'A classification is an investigation candidate, never a proven cause.',
                'Exact closed interval totals can exceed retained individual gap details.',
                'The decoder does not return PTS. No output is assigned to the latest input.',
                'RTP seq16 observation skips are not confirmed loss; no cross-source inference is made.',
                'AU marker observation includes rejected/incomplete AUs; valid counts filter flags1/2/4.',
                'Primary reset attempts include normal resets with no discarded AU.',
                'Damage-bit totals overlap. PLI_REQUEST is local demand, not RTCP transmission.',
                'Counts, observations and local timing do not establish source FPS or visible behavior.',
                'Diagnostic admission and capture work can affect playback timings.',
                'Header readers is configured count. COPY_CONFIG and COPY_END describe availability/selection.',
            ]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace', type=Path)
    parser.add_argument('--out', type=Path, help='Create a new English JSON report (never overwrite)')
    args = parser.parse_args()
    try:
        with args.trace.open('rb') as source:
            data = source.read(MAX_BYTES + 1)
        if len(data) > MAX_BYTES:
            raise ValueError('Trace exceeds the fixed 8 MiB read bound')
        report = json.dumps(analyze(data), indent=2, ensure_ascii=True) + '\n'
        if args.out:
            with args.out.open('x', encoding='utf-8', newline='\n') as target:
                target.write(report)
        else:
            sys.stdout.write(report)
    except (OSError, ValueError, struct.error) as error:
        print('Trace analysis failed: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
