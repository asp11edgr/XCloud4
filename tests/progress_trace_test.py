#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic SYNTHETIC reader fixtures; never console measurements."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

PARSER = Path(__file__).resolve().parents[1]/'scripts/analyze_progress_trace.py'
spec = importlib.util.spec_from_file_location('progress_reader', PARSER)
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


def record(t, event, a=0, b=0, ordinal=1, flags=0):
    return {'t_us': t, 'a': a, 'b': b, 'ordinal': ordinal, 'event': event, 'flags': flags}


def fixture(*, history=(), cadence=(), samples=(), windows=(), header=None,
            exposures=None, rx=None, attempts=None, drops=None, histogram=None, phases=None):
    """Independent ABI fixture writer; all values are synthetic and deterministic."""
    h = [0]*128
    # Keep these constants independent of the reader so a reader ABI error fails.
    expected = {1: 1, 2: 1024, 8: 4, 9: 100000, 32: 7, 34: 12000, 35: 65536,
        36: 8192, 37: 8, 38: 4096, 39: 512, 40: 64, 41: 16, 42: 32,
        43: 640, 44: 32, 45: 32, 46: 8, 47: 3, 91: 32}
    for i, v in expected.items():
        h[i] = v
    h[0] = int.from_bytes(b'X4PROG1\0', 'little')
    h[3], h[4], h[5] = 7, 10000, 1000000
    h[10], h[12], h[14], h[16] = len(samples), len(cadence), len(history), len(windows)
    records = list(history) + [row for wh, rows in windows for row in rows]
    unique = {tuple(row.values()): row for row in records}.values()
    if attempts is None:
        attempts = [0]*512
        for row in unique:
            e = row['event']
            slot = (e >> 8)*64 + (e & 255) if e >> 8 < 8 and e & 255 < 64 else 63
            attempts[slot] += 1
    drops = drops or [0]*512
    histogram = histogram or [0]*32
    if not any(histogram):
        for _, _, interval, _ in cadence:
            histogram[min(31, max(0, interval.bit_length()-1))] += 1
    exposures = exposures if exposures is not None else [0]*8
    h[18], h[19], h[27], h[28] = sum(attempts)&p.U64, sum(drops)&p.U64, sum(histogram)&p.U64, sum(exposures[::2])&p.U64
    h[25] = sum(row[2] > 100000 for row in cadence)
    h[22] = len(cadence)+1 if cadence else 0
    h[23] = cadence[0][0]-cadence[0][2] if cadence else 0
    h[24] = cadence[-1][0] if cadence else 0
    h[68] = sum(row['event'] == 0x614 for row in unique)
    phases = phases if phases is not None else [(r['t_us'], r['b'], r['a'], 1) for r in unique if r['event'] == 0x614]
    h[89] = len(phases)
    h[70] = h[25]
    if header:
        for i, v in header.items():
            h[i] = v
    if not header or 87 not in header:
        h[87] = (len(samples)+h[11])&p.U64
    if not header or 68 not in header:
        h[68] = (len(phases)+h[90])&p.U64
    header_bytes = bytearray(struct.pack('<128Q', *h))
    header_bytes[768:768+10] = b'synthetic\0'
    header_bytes[832:832+7] = b'0.7.31\0'
    out = bytearray(header_bytes)
    config = [960, 540, 30, 5000, 4, 16000, 4, 256, 128, 32, 25000, 0x42e01f, 3600, 108000, 0, 0]
    def words(values):
        out.extend(struct.pack('<%dQ' % len(values), *values))
    def rows(records):
        for row in records:
            out.extend(struct.pack('<QQQIHH', *(row[k] for k in ('t_us', 'a', 'b', 'ordinal', 'event', 'flags'))))
    words(config)
    words(attempts); words(drops); words([0]*64); words(histogram); words(exposures)
    for row in phases:
        words(row)
    for _ in range(64-len(phases)):
        words([0]*4)
    for source in rx or [[0]*16 for _ in range(16)]:
        words(source)
    for sample in samples:
        words(sample)
    for item in cadence:
        words(item)
    rows(history)
    for wh, records in windows:
        words(wh); rows(records)
    return bytes(out)


def sample(target, actual, **values):
    row = [0]*80
    row[0:3] = target, actual, actual-target
    row[6] = 1|8
    row[30] = actual
    for key, value in values.items():
        row[p.SAMPLE.index(key)] = value
    return row


def fixture_v2(*, critical=(), omitted=None, stage_sources=(), stages=None,
               critical_header=None, au_filter=None, rejected=None, **kwargs):
    """Independent literal v2 ABI; no producer/reader constants imported."""
    base = bytearray(fixture(**kwargs))
    base[:8] = b'X4PROG2\0'
    struct.pack_into('<Q', base, 8, 2)
    attempts, drops = [0]*32, omitted or [0]*32
    for row in critical:
        attempts[row[2] & 65535] += 1
    attempts = [a+b for a, b in zip(attempts, drops)]
    h = [0]*32
    for i, value in {0: int.from_bytes(b'X4CRIT2\0', 'little'), 1: 2, 2: 256,
        3: 64, 4: 4096, 5: len(critical), 6: sum(attempts), 7: sum(drops),
        9: 16, 10: 16, 11: 24, 12: 32, 16: 32, 17: 16, 18: 512,
        19: 3072, 20: 1024, 23: len(critical), 24: 16, 25: 10000, 26: 8}.items():
        h[i] = value
    if critical_header:
        h = [critical_header.get(i, n) for i, n in enumerate(h)]
    def words(values):
        base.extend(struct.pack('<%dQ' % len(values), *values))
    words(h); words(attempts); words(drops)
    for row in stages or [[0]*16 for _ in range(16)]:
        words(row)
    words([n for row in rejected or [[0]*32 for _ in range(16)] for n in row])
    words([0]*16); words(au_filter or [0]*8)
    sources = [[0]*24 for _ in range(256)]
    for stage, slot, row in stage_sources:
        sources[stage*16+slot] = row
    for row in sources:
        words(row)
    for row in critical:
        words(row)
    return bytes(base)


def critical_record(event, stage=10, *, t=100000, identity=1, ssrc=42, pt=102,
                    a=0, b=0, c=0, flags=2, ordinal=1):
    return (t, identity, event | (stage << 16) | (flags << 32),
            ssrc | (pt << 32), a, b, c, ordinal)


def nack_critical_record(event, **kwargs):
    """Literal .733 sparse contract; does not import producer enum values."""
    kwargs.setdefault('stage', 65535)
    kwargs.setdefault('flags', 0)
    row = list(critical_record(event, **kwargs))
    row[3] |= 1 << 63
    return tuple(row)


class ReceiverNackReaderTests(unittest.TestCase):
    def analyze(self, rows, omitted=None):
        numbered = [tuple(row[:7])+(i+1,) for i, row in enumerate(rows)]
        return p.analyze(fixture_v2(critical=numbered, omitted=omitted))['upstream_diagnostics']['receiver_NACK']

    def state(self, enabled=1, epoch=7):
        return nack_critical_record(27, identity=epoch << 32, a=enabled, b=64, c=(16 << 32)|64)

    def request(self, epoch=7, ordinal=1, ssrc=42, first=65535, last=65536):
        return nack_critical_record(19, t=110010, identity=(epoch << 32)|ordinal,
            ssrc=ssrc, a=first, b=last, c=110000)

    def test_older_v2_without_state_is_unavailable_not_disabled(self):
        out = self.analyze([])
        self.assertFalse(out['available'])
        self.assertIsNone(out['experiment_enabled'])

    def test_explicit_disabled_state_is_available_observation(self):
        out = self.analyze([self.state(0)])
        self.assertTrue(out['available'])
        self.assertFalse(out['experiment_enabled'])
        self.assertEqual(out['retained_state_observations'][0]['watch_slots'], 16)
        self.assertEqual(out['retained_state_observations'][0]['range_position_cap'], 64)

    def test_attach_or_media_state_does_not_prove_remote_feedback_result(self):
        for flags in (0, 4, 6):
            with self.subTest(flags=flags):
                out = self.analyze([nack_critical_record(27, identity=7 << 32,
                    a=1, b=64, c=(16 << 32)|64, flags=flags)])
                state = out['retained_state_observations'][0]
                for item in (state, out):
                    self.assertIsNone(item['answer_checked'])
                    self.assertIsNone(item['remote_feedback_accepted'])
                    self.assertIsNone(item['effective_requests_enabled'])
                self.assertFalse(out['coverage']['answer_check_result_retained'])

    def test_checked_answer_state_records_acceptance_and_effective_mode(self):
        for enabled in (0, 1):
            for accepted in (0, 1):
                with self.subTest(enabled=enabled, accepted=accepted):
                    out = self.analyze([self.state(enabled), nack_critical_record(27,
                        t=100010, identity=7 << 32, a=enabled, b=64,
                        c=(16 << 32)|64, flags=16 | (2 if accepted else 0))])
                    state = out['retained_state_observations'][-1]
                    for item in (state, out):
                        self.assertTrue(item['answer_checked'])
                        self.assertEqual(item['remote_feedback_accepted'], bool(accepted))
                        self.assertEqual(item['effective_requests_enabled'], bool(enabled and accepted))
                    self.assertTrue(out['coverage']['answer_check_result_retained'])
                    self.assertFalse(out['feedback_state_at_stop_established'])

    def test_omitted_answer_state_does_not_infer_rejection_or_activation(self):
        omitted = [0]*32; omitted[27] = 1
        out = self.analyze([self.state()], omitted)
        self.assertIsNone(out['answer_checked'])
        self.assertIsNone(out['remote_feedback_accepted'])
        self.assertIsNone(out['effective_requests_enabled'])
        self.assertFalse(out['coverage']['NACK_detail_complete'])

    def test_checked_answer_does_not_cross_configuration_epoch(self):
        out = self.analyze([nack_critical_record(27, identity=6 << 32,
            a=1, flags=18), self.state(epoch=7)])
        self.assertIsNone(out['answer_checked'])
        self.assertIsNone(out['remote_feedback_accepted'])
        self.assertIsNone(out['effective_requests_enabled'])
        self.assertTrue(out['coverage']['answer_check_result_retained'])

    def test_later_media_and_close_states_do_not_replace_checked_answer(self):
        out = self.analyze([self.state(), nack_critical_record(27, t=100010,
            identity=7 << 32, a=1, flags=18), nack_critical_record(27, t=100020,
            identity=7 << 32, a=1, flags=4), nack_critical_record(27, t=100030,
            identity=7 << 32, a=0, flags=8|16)])
        self.assertTrue(out['remote_feedback_accepted'])
        self.assertTrue(out['effective_requests_enabled'])
        self.assertIsNone(out['retained_state_observations'][-1]['answer_checked'])
        self.assertEqual(out['latest_retained_answer_check_result']['t_us'], 100010)

    def test_feedback_check_local_error_is_not_a_compatibility_result(self):
        result = p.analyze(fixture_v2(critical=[self.state(),
            critical_record(17, 65535, identity=24, a=(1 << 64)-1, ordinal=2)]))['upstream_diagnostics']
        setting = result['retained_negotiation_and_RTX_settings'][0]
        self.assertEqual(setting['local_feedback_check_error'], -1)
        self.assertIsNone(setting['remote_feedback_accepted'])
        self.assertFalse(setting['error_establishes_feedback_not_supported'])
        self.assertFalse(setting['media_session_failure_established'])
        self.assertIsNone(result['receiver_NACK']['effective_requests_enabled'])

    def test_request_dispatch_auth_reorder_and_AU_are_distinct(self):
        key = (7 << 32)|1
        rows = [self.state(), nack_critical_record(18, t=110000, identity=(7 << 32)|9,
            a=65535, b=65536, c=65537, flags=1), self.request(),
            nack_critical_record(20, t=110020, identity=key, a=65535, b=65536, c=110010),
            nack_critical_record(30, t=110021, identity=key, a=65535, b=3, c=110020),
            nack_critical_record(21, t=110030, identity=key, a=0, b=20, c=110020),
            nack_critical_record(22, t=110040, identity=key, a=0, b=30, c=110020),
            nack_critical_record(23, t=110050, identity=key, a=1, b=30, c=110020),
            nack_critical_record(24, t=110100, identity=key, a=65535, b=42, c=110010, flags=1),
            nack_critical_record(24, t=110200, identity=key, a=65536, b=42, c=110010, flags=5),
            nack_critical_record(25, t=110201, identity=key, a=65536, b=0, c=1),
            nack_critical_record(26, t=110202, identity=key, a=4, b=1, c=65536),
            nack_critical_record(26, t=110210, identity=key, a=4, b=2, c=65536)]
        out = self.analyze(rows)
        request = out['request_correlations'][0]
        self.assertTrue(request['association_available'])
        self.assertTrue(request['detection_identity_unique'])
        self.assertEqual(request['timings_us']['detection_to_request_record'], 10)
        self.assertEqual(request['timings_us']['SRTCP_local_call_span'], 10)
        self.assertEqual(request['timings_us']['request_record_to_first_authenticated_return'], 90)
        self.assertEqual(request['timings_us']['dispatch_record_to_first_authenticated_return'], 80)
        self.assertTrue(request['all_selected_positions_have_first_authenticated_arrival_after_dispatch'])
        self.assertNotIn('all_requested_positions_observed_in_retained_authenticated_returns', request)
        self.assertEqual(request['retained_authenticated_positions'], [65535, 65536])
        self.assertEqual(request['actually_dispatched_positions'], [65535, 65536])
        self.assertEqual(request['AU_observation_counts_by_stage']['accepted_by_recovery_for_Decode'], 1)
        self.assertFalse(request['accepted_AU_observation_is_presented_image'])
        self.assertFalse(request['later_return_proves_NACK_delivery_or_retransmission'])

    def test_signed_status_and_bool_send_have_separate_meanings(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(22, t=110020, identity=key, a=(1 << 64)-1, flags=1),
            nack_critical_record(23, t=110030, identity=key, a=0)])
        stages = out['request_correlations'][0]['retained_stage_records']
        self.assertEqual(stages['NACK_SRTCP_RESULT'][0]['local_status'], -1)
        self.assertFalse(stages['NACK_SRTCP_RESULT'][0]['SRTCP_local_success'])
        self.assertFalse(stages['NACK_LOCAL_SEND_RESULT'][0]['local_send_success'])

    def test_protected_and_local_send_bytes_have_distinct_scopes(self):
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(21, t=110020, identity=(7 << 32)|1, b=32),
            nack_critical_record(22, t=110030, identity=(7 << 32)|1, b=42),
            nack_critical_record(23, t=110040, identity=(7 << 32)|1, a=1, b=32)])
        stages = out['request_correlations'][0]['retained_stage_records']
        self.assertEqual(stages['NACK_SRTCP_BEGIN'][0]['byte_count_scope'], 'plaintext_original_message')
        self.assertEqual(stages['NACK_SRTCP_RESULT'][0]['byte_count_scope'], 'protected_copy')
        self.assertEqual(stages['NACK_LOCAL_SEND_RESULT'][0]['byte_count_scope'], 'plaintext_original_message')
        self.assertEqual(stages['NACK_SRTCP_RESULT'][0]['bytes'], 42)
        self.assertEqual(stages['NACK_LOCAL_SEND_RESULT'][0]['bytes'], 32)

    def test_recovery_acceptance_then_feed_failure_preserves_both_AU_facts(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(26, t=110100, identity=key, a=7, b=2, c=65535),
            nack_critical_record(26, t=110110, identity=key, a=7, b=4, c=65535)])
        request = out['request_correlations'][0]
        self.assertEqual(request['AU_observation_counts_by_stage'],
            {'accepted_by_recovery_for_Decode': 1, 'discarded': 1})
        rows = request['retained_stage_records']['NACK_AU_CONTRIBUTION']
        self.assertEqual([r['AU_ordinal'] for r in rows], [7, 7])
        self.assertFalse(rows[0]['successful_Decode_feed_established'])
        self.assertFalse(rows[0]['native_Decode_output_established'])
        self.assertFalse(request['accepted_AU_observation_establishes_successful_Decode_feed'])
        self.assertFalse(request['accepted_AU_observation_establishes_native_Decode_output'])
        self.assertFalse(request['accepted_AU_observation_is_presented_image'])

    def test_local_send_success_does_not_require_or_prove_return(self):
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(23, t=110020, identity=(7 << 32)|1, a=1)])
        request = out['request_correlations'][0]
        self.assertIsNone(request['first_authenticated_return'])
        self.assertIsNone(request['timings_us']['request_record_to_first_authenticated_return'])
        self.assertFalse(request['no_return_record_proves_no_remote_return'])

    def test_sources_and_epochs_never_correlate_by_sequence_alone(self):
        rows = [self.state(), self.request(), self.request(ssrc=43), self.request(epoch=8),
            nack_critical_record(24, t=110100, identity=(8 << 32)|1, a=65535, c=110010, flags=1)]
        requests = self.analyze(rows)['request_correlations']
        self.assertEqual(len(requests), 3)
        self.assertEqual([r['first_authenticated_return'] is not None for r in requests], [False, False, True])

    def test_duplicate_flags_and_positions_do_not_count_as_new_returns(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(24, t=110100, identity=key, a=65535, c=110010, flags=1),
            nack_critical_record(24, t=110101, identity=key, a=65535, c=110010, flags=2)])
        request = out['request_correlations'][0]
        self.assertEqual(request['retained_authenticated_positions'], [65535])
        self.assertEqual(request['duplicate_flag_observations'], 1)
        self.assertFalse(request['all_selected_positions_have_first_authenticated_arrival_after_dispatch'])

    def test_returns_outside_range_or_before_request_are_not_latency(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(24, t=110100, identity=key, a=65537, c=110010),
            nack_critical_record(24, t=110000, identity=key, a=65535, c=110010)])
        request = out['request_correlations'][0]
        self.assertEqual(request['return_outside_requested_range_observations'], 1)
        self.assertEqual(request['return_before_request_observations'], 1)
        self.assertIsNone(request['first_authenticated_return'])

    def test_request_cap_and_reversed_range_are_not_correlated(self):
        for first, last in ((1, 65), (8, 7)):
            with self.subTest(first=first, last=last):
                out = self.analyze([self.state(), self.request(first=first, last=last)])
                self.assertFalse(out['request_correlations'][0]['association_available'])

    def test_repeated_request_identity_is_explicitly_ambiguous(self):
        out = self.analyze([self.state(), self.request(), self.request(first=3, last=4)])
        self.assertFalse(out['request_correlations'][0]['association_available'])
        self.assertEqual(len(out['orphan_or_ambiguous_request_stage_records']), 2)

    def test_omitted_state_does_not_invent_enabled_mode(self):
        omitted = [0]*32; omitted[27] = 1
        out = self.analyze([self.request()], omitted)
        self.assertTrue(out['available'])
        self.assertIsNone(out['experiment_enabled'])
        self.assertFalse(out['coverage']['NACK_detail_complete'])
        self.assertFalse(out['coverage']['configuration_details_retained'])

    def test_non_NACK_omission_does_not_hide_NACK_specific_coverage(self):
        omitted = [0]*32; omitted[1] = 2
        out = self.analyze([self.state()], omitted)
        self.assertTrue(out['coverage']['NACK_detail_complete'])

    def test_missing_request_keeps_return_as_orphan(self):
        out = self.analyze([self.state(), nack_critical_record(24, identity=(7 << 32)|1, a=9)])
        self.assertEqual(len(out['orphan_or_ambiguous_request_stage_records']), 1)
        self.assertEqual(out['request_correlations'], [])

    def test_counters_are_latest_per_epoch_not_summed(self):
        out = self.analyze([self.state(),
            nack_critical_record(29, t=110000, a=14, b=1, c=7),
            nack_critical_record(29, t=110100, a=14, b=2, c=7),
            nack_critical_record(29, t=110200, a=14, b=1, c=8),
            nack_critical_record(29, t=110300, a=100, b=5, c=7)])
        self.assertEqual(out['counter_snapshot_duplicates_not_summed'], 1)
        items = out['latest_retained_counter_snapshots']
        self.assertEqual([(r['counter_epoch'], r['counter_id'], r['value']) for r in items],
                         [(7, 14, 2), (7, 100, 5), (8, 14, 1)])
        self.assertEqual(items[0]['counter_name'], 'requests_reserved')
        self.assertEqual(items[1]['counter_name'], 'state_gate_omissions')

    def test_unknown_AU_and_reorder_codes_stay_unknown(self):
        key = (7 << 32)|1
        request = self.analyze([self.state(), self.request(),
            nack_critical_record(25, identity=key, a=65535, b=99),
            nack_critical_record(26, identity=key, a=1, b=99, c=65535)])['request_correlations'][0]
        self.assertEqual(request['reorder_observation_counts_by_decision'], {'unknown': 1})
        self.assertEqual(request['AU_observation_counts_by_stage'], {'unknown': 1})

    def test_reorder_rejection_flags_distinguish_behind_from_occupied(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(25, t=110100, identity=key, a=65535, b=1, c=1, flags=1),
            nack_critical_record(25, t=110110, identity=key, a=65536, b=1, c=1, flags=2)])
        rows = out['request_correlations'][0]['retained_stage_records']['NACK_REORDER_RESULT']
        self.assertEqual([r['decision'] for r in rows], ['late_or_occupied_slot']*2)
        self.assertTrue(rows[0]['behind_worker_expected_before_insert'])
        self.assertIsNone(rows[0]['occupied_slot_at_nonnegative_difference_before_insert'])
        self.assertEqual(rows[0]['rejection_subreason'], 'behind_worker_expected')
        self.assertFalse(rows[1]['behind_worker_expected_before_insert'])
        self.assertTrue(rows[1]['occupied_slot_at_nonnegative_difference_before_insert'])
        self.assertEqual(rows[1]['rejection_subreason'], 'occupied_slot')

    def test_unflagged_or_conflicting_reorder_rejection_is_not_confirmed_late(self):
        key = (7 << 32)|1
        for flags in (0, 3, 4, 5, 7):
            with self.subTest(flags=flags):
                out = self.analyze([self.state(), self.request(),
                    nack_critical_record(25, t=110100, identity=key, a=65535, b=1, c=1, flags=flags)])
                row = out['request_correlations'][0]['retained_stage_records']['NACK_REORDER_RESULT'][0]
                self.assertIsNone(row['behind_worker_expected_before_insert'])
                self.assertIsNone(row['occupied_slot_at_nonnegative_difference_before_insert'])
                self.assertEqual(row['admission_outcome'], 'rejected_without_confirmed_behind_expected')
                self.assertFalse(row['rejection_code_alone_proves_late_after_worker_advance'])

    def test_reorder_window_jump_is_admission_not_confirmed_behind(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(25, t=110100, identity=key, a=65535, b=2, c=1, flags=1)])
        row = out['request_correlations'][0]['retained_stage_records']['NACK_REORDER_RESULT'][0]
        self.assertEqual(row['decision'], 'window_jump')
        self.assertEqual(row['admission_outcome'], 'admitted')
        self.assertIsNone(row['behind_worker_expected_before_insert'])
        self.assertIsNone(row['rejection_subreason'])

    def test_reorder_admission_counts_do_not_count_all_code1_as_late(self):
        key = (7 << 32)|1
        rows = [self.state(), self.request()]
        for i, (code, flags) in enumerate(((0, 0), (2, 0), (1, 1), (1, 2), (1, 4), (1, 0))):
            rows.append(nack_critical_record(25, t=110100+i, identity=key,
                a=65535, b=code, c=1, flags=flags))
        request = self.analyze(rows)['request_correlations'][0]
        self.assertEqual(request['reorder_observation_counts_by_admission_outcome'],
            {'admitted': 2, 'confirmed_behind_worker_expected': 1,
             'rejected_without_confirmed_behind_expected': 3})
        self.assertEqual(request['reorder_rejection_observation_counts_by_subreason'],
            {'behind_worker_expected': 1, 'occupied_slot': 1, 'other_or_unclassified': 1, 'unknown': 1})

    def test_counter_names_match_current_state_declaration_order(self):
        header = PARSER.parents[1]/'src/streaming/receiver_nack_state.h'
        text = header.read_text(encoding='utf-8')
        block = text[text.index('uint64_t observations;'):text.index('} X4ReceiverNackCounters;')]
        self.assertEqual(p.NACK_COUNTERS, re.findall(r'uint64_t (\w+);', block))
        self.assertEqual(p.NACK_COUNTERS[35:39], ['observer_discontinuities',
            'omission_retired_ranges', 'omission_retired_positions', 'clock_regressions'])

    def test_global_ticket_omissions_with_unknown_source_are_coverage(self):
        row = list(nack_critical_record(29, t=110100, identity=0, a=107, b=3, c=0))
        row[3] = 0
        out = self.analyze([self.state(), tuple(row)])
        coverage = out['coverage']
        self.assertTrue(coverage['functional_tracking_limit_observed'])
        self.assertFalse(coverage['authenticated_return_absence_claims_permitted'])
        self.assertEqual(coverage['retained_functional_tracking_limit_snapshots'][0]['counter_name'], 'AU_ticket_omissions')
        self.assertEqual(coverage['retained_functional_tracking_limit_snapshots'][0]['source'], 0)

    def test_provenance_and_ticket_operation_counters_are_not_packet_loss(self):
        out = self.analyze([self.state(),
            nack_critical_record(29, t=110100, a=102, b=2, c=7),
            nack_critical_record(29, t=110110, a=109, b=2, c=7)])
        counters = out['latest_retained_counter_snapshots']
        self.assertEqual(counters[0]['counter_name'], 'non_authenticated_or_RTX_flagged')
        self.assertIn('RTX-normalized', counters[0]['counter_scope'])
        self.assertEqual(counters[1]['counter_name'], 'delivery_ticket_set_get_operation_omissions')
        self.assertFalse(counters[1]['counter_is_lost_packet_count'])
        self.assertTrue(out['coverage']['functional_tracking_limit_observed'])

    def test_suppression_gap_id_collision_does_not_create_request_association(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(28, t=110100, identity=key, a=1, b=2, c=1)])
        suppression = out['retained_suppression_observations'][0]
        self.assertFalse(suppression['suppression_request_association_available'])
        self.assertEqual(out['request_correlations'][0]['retained_stage_records']['NACK_AUTH_RETURN'], [])

    def test_feedback_not_confirmed_suppression_does_not_infer_rejection(self):
        for checked_flags in (None, 16):
            with self.subTest(checked_flags=checked_flags):
                rows = [self.state()]
                if checked_flags is not None:
                    rows.append(nack_critical_record(27, t=100010,
                        identity=7 << 32, a=1, flags=checked_flags))
                rows.append(nack_critical_record(28, t=110100,
                    identity=(7 << 32)|1, a=6, b=2, c=1))
                out = self.analyze(rows)
                self.assertEqual(out['retained_suppression_observations'][0]['reason'], 'feedback_not_confirmed')
                if checked_flags is None:
                    self.assertIsNone(out['remote_feedback_accepted'])
                else:
                    self.assertFalse(out['remote_feedback_accepted'])

    def test_dispatch_mask_restricts_selected_positions_and_predispatch_return(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(first=100, last=102),
            nack_critical_record(24, t=110015, identity=key, a=100, c=110010, flags=1),
            nack_critical_record(20, t=110021, identity=key, a=100, b=102, c=110010),
            nack_critical_record(30, t=110022, identity=key, a=100, b=6, c=110020),
            nack_critical_record(23, t=110025, identity=key, a=1, b=30, c=110020),
            nack_critical_record(24, t=110030, identity=key, a=101, c=110010, flags=1),
            nack_critical_record(24, t=110040, identity=key, a=102, c=110010, flags=5)])
        request = out['request_correlations'][0]
        self.assertTrue(request['dispatch_mask_association_available'])
        self.assertEqual(request['actually_dispatched_positions'], [101, 102])
        self.assertEqual(request['retained_authenticated_positions'], [100, 101, 102])
        self.assertEqual(request['retained_authenticated_dispatched_positions_after_dispatch'], [101, 102])
        self.assertEqual(request['returns_before_dispatch_observations'], 1)
        self.assertEqual(request['returns_outside_dispatch_mask_observations'], 1)
        self.assertEqual(request['first_authenticated_return']['sequence_extended'], 100)
        self.assertEqual(request['first_authenticated_return_of_dispatched_position_after_dispatch']['sequence_extended'], 101)
        self.assertEqual(request['timings_us']['dispatch_record_to_first_authenticated_return'], 9)
        self.assertEqual(request['timings_us']['dispatch_timestamp_to_first_authenticated_dispatched_position'], 10)
        self.assertTrue(request['all_selected_positions_have_first_authenticated_arrival_after_dispatch'])
        self.assertTrue(request['return_classification_does_not_infer_state_cancellation'])
        self.assertFalse(request['pre_dispatch_arrival_is_retransmission_response'])

    def test_missing_dispatch_mask_does_not_claim_original_range_was_sent(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(20, t=110020, identity=key, a=65535, b=65536, c=110010),
            nack_critical_record(24, t=110100, identity=key, a=65535, c=110010)])
        request = out['request_correlations'][0]
        self.assertFalse(request['dispatch_mask_association_available'])
        self.assertIsNone(request['actually_dispatched_positions'])
        self.assertIsNone(request['all_selected_positions_have_first_authenticated_arrival_after_dispatch'])
        self.assertIsNone(request['timings_us']['dispatch_record_to_first_authenticated_return'])

    def test_invalid_or_ambiguous_dispatch_masks_are_not_correlated(self):
        key = (7 << 32)|1
        for first, bits in ((65534, 1), (65535, 4), (65535, 0)):
            with self.subTest(first=first, bits=bits):
                out = self.analyze([self.state(), self.request(),
                    nack_critical_record(20, t=110020, identity=key, a=65535, b=65536, c=110010),
                    nack_critical_record(30, t=110021, identity=key, a=first, b=bits, c=110020)])
                self.assertFalse(out['request_correlations'][0]['dispatch_mask_association_available'])
        mask = nack_critical_record(30, t=110021, identity=key, a=65535, b=3, c=110020)
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(20, t=110020, identity=key, a=65535, b=65536, c=110010), mask, mask])
        self.assertFalse(out['request_correlations'][0]['dispatch_mask_association_available'])

    def test_highest_dispatch_mask_bit_and_wrap_are_not_truncated(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(first=65520, last=65583),
            nack_critical_record(20, t=110020, identity=key, a=65520, b=65583, c=110010),
            nack_critical_record(30, t=110021, identity=key, a=65520, b=1 << 63, c=110020),
            nack_critical_record(24, t=110100, identity=key, a=65583, c=110010, flags=1)])
        request = out['request_correlations'][0]
        self.assertEqual(request['actually_dispatched_positions'], [65583])
        self.assertIsNone(request['all_selected_positions_have_first_authenticated_arrival_after_dispatch'])
        self.assertIsNone(request['successful_local_send_result_observed_us'])
        self.assertFalse(request['all_original_range_positions_observed_in_retained_authenticated_returns'])

    def test_close_state_does_not_change_experiment_configuration(self):
        out = self.analyze([self.state(), nack_critical_record(27, t=110100,
            identity=7 << 32, flags=8, a=0, b=0, c=0)])
        self.assertTrue(out['experiment_enabled'])
        self.assertTrue(out['closing_observation_retained'])
        self.assertTrue(out['coverage']['configuration_details_retained'])

    def test_AU_latency_does_not_pair_distinct_positions_in_one_range(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(24, t=110100, identity=key, a=65535, c=110010),
            nack_critical_record(26, t=110110, identity=key, a=4, b=2, c=65536)])
        request = out['request_correlations'][0]
        self.assertIsNone(request['timings_us']['first_authenticated_return_to_first_accepted_AU_observation'])

    def test_omitted_dispatch_mask_is_NACK_coverage_loss(self):
        omitted = [0]*32; omitted[30] = 1
        out = self.analyze([self.state(), self.request()], omitted)
        self.assertFalse(out['coverage']['NACK_detail_complete'])
        self.assertEqual(out['coverage']['event_counts']['NACK_DISPATCH_MASK']['omitted'], 1)

    def test_embedded_times_are_distinct_from_later_critical_record_clock(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(),
            nack_critical_record(18, t=110002, identity=(7 << 32)|9, a=65535, b=65536, c=65537),
            self.request(),
            nack_critical_record(30, t=110023, identity=key, a=65535, b=3, c=110020),
            nack_critical_record(20, t=110025, identity=key, a=65535, b=65536, c=110008),
            nack_critical_record(24, t=110100, identity=key, a=65535, c=110008)])
        request = out['request_correlations'][0]
        self.assertTrue(request['dispatch_mask_association_available'])
        self.assertTrue(request['detection_identity_unique'])
        self.assertFalse(request['embedded_request_reference_matches_retained_request_time'])
        self.assertTrue(request['embedded_request_reference_consistent_across_retained_stages'])
        self.assertEqual(request['timings_us']['request_record_to_dispatch_record'], 15)
        self.assertEqual(request['timings_us']['embedded_request_to_first_authenticated_return'], 92)

    def test_mask_selection_precedes_actual_transport_dispatch(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(30, t=110022, identity=key, a=65535, b=3, c=110020),
            nack_critical_record(24, t=110030, identity=key, a=65535, c=110010),
            nack_critical_record(20, t=110045, identity=key, a=65535, b=65536, c=110010),
            nack_critical_record(23, t=110047, identity=key, a=1, b=30, c=110040),
            nack_critical_record(24, t=110050, identity=key, a=65536, c=110010, flags=1)])
        request = out['request_correlations'][0]
        self.assertEqual(request['mask_selection_us'], 110020)
        self.assertEqual(request['actual_transport_dispatch_reference_us'], 110040)
        self.assertEqual(request['returns_before_dispatch_observations'], 1)
        self.assertEqual(request['retained_authenticated_dispatched_positions_after_dispatch'], [65536])
        self.assertEqual(request['timings_us']['serialized_selection_to_dispatch_reference'], 20)
        self.assertEqual(request['timings_us']['dispatch_timestamp_to_first_authenticated_dispatched_position'], 10)
        self.assertFalse(request['all_selected_positions_have_first_authenticated_arrival_after_dispatch'])

    def test_mask_alone_does_not_establish_transport_dispatch_reference(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(30, t=110022, identity=key, a=65535, b=3, c=110020),
            nack_critical_record(20, t=110045, identity=key, a=65535, b=65536, c=110010)])
        request = out['request_correlations'][0]
        self.assertIsNone(request['actual_transport_dispatch_reference_us'])
        self.assertIsNone(request['returns_before_dispatch_observations'])
        self.assertEqual(request['return_dispatch_boundary_kind'], 'unavailable')

    def test_shutdown_AU_discard_is_separate_from_recovery_filter(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(),
            nack_critical_record(26, t=110100, identity=key, a=7, b=4, c=65535, flags=1)])
        request = out['request_correlations'][0]
        row = request['retained_stage_records']['NACK_AU_CONTRIBUTION'][0]
        self.assertTrue(row['shutdown_discard_observation'])
        self.assertEqual(row['AU_stage'], 'discarded')
        self.assertNotIn('recovery_filter_rejected', request['AU_observation_counts_by_stage'])

    def test_duplicate_after_dispatch_is_not_a_first_arrival_repair_candidate(self):
        key = (7 << 32)|1
        out = self.analyze([self.state(), self.request(first=100, last=100),
            nack_critical_record(30, t=110022, identity=key, a=100, b=1, c=110020),
            nack_critical_record(24, t=110030, identity=key, a=100, c=110010, flags=5),
            nack_critical_record(20, t=110045, identity=key, a=100, b=100, c=110010),
            nack_critical_record(23, t=110047, identity=key, a=1, b=30, c=110040),
            nack_critical_record(24, t=110050, identity=key, a=100, c=110010, flags=2)])
        request = out['request_correlations'][0]
        self.assertEqual(request['retained_authenticated_dispatched_positions_after_dispatch'], [100])
        self.assertEqual(request['post_dispatch_duplicate_observations'], 1)
        self.assertEqual(request['first_post_dispatch_authenticated_observation']['t_us'], 110050)
        self.assertEqual(request['timings_us']['dispatch_record_to_first_post_dispatch_authenticated_observation'], 5)
        self.assertEqual(request['first_authenticated_arrival_positions_after_dispatch'], [])
        self.assertIsNone(request['first_authenticated_return_of_dispatched_position_after_dispatch'])
        self.assertIsNone(request['timings_us']['dispatch_record_to_first_authenticated_return'])
        self.assertFalse(request['all_selected_positions_have_first_authenticated_arrival_after_dispatch'])
        self.assertEqual(request['returns_before_dispatch_observations'], 1)
        self.assertFalse(request['later_return_proves_NACK_delivery_or_retransmission'])

    def test_duplicate_without_retained_first_arrival_does_not_invent_completion(self):
        key = (7 << 32)|1
        omitted = [0]*32; omitted[24] = 1
        out = self.analyze([self.state(), self.request(first=100, last=100),
            nack_critical_record(30, t=110022, identity=key, a=100, b=1, c=110020),
            nack_critical_record(20, t=110045, identity=key, a=100, b=100, c=110010),
            nack_critical_record(23, t=110047, identity=key, a=1, b=30, c=110040),
            nack_critical_record(24, t=110050, identity=key, a=100, c=110010, flags=2)], omitted)
        request = out['request_correlations'][0]
        self.assertEqual(request['retained_authenticated_dispatched_positions_after_dispatch'], [100])
        self.assertEqual(request['first_authenticated_arrival_positions_after_dispatch'], [])
        self.assertFalse(request['all_selected_positions_have_first_authenticated_arrival_after_dispatch'])
        self.assertFalse(out['coverage']['NACK_detail_complete'])
        self.assertFalse(out['coverage']['authenticated_return_absence_claims_permitted'])

    def test_unknown_or_conflicting_flags_remain_observations(self):
        key = (7 << 32)|1
        for flags in (0, 3):
            with self.subTest(flags=flags):
                out = self.analyze([self.state(), self.request(first=100, last=100),
                    nack_critical_record(30, t=110022, identity=key, a=100, b=1, c=110020),
                    nack_critical_record(20, t=110045, identity=key, a=100, b=100, c=110010),
                    nack_critical_record(23, t=110047, identity=key, a=1, b=30, c=110040),
                    nack_critical_record(24, t=110050, identity=key, a=100, c=110010, flags=flags)])
                request = out['request_correlations'][0]
                self.assertEqual(request['retained_authenticated_dispatched_positions_after_dispatch'], [100])
                self.assertEqual(request['first_authenticated_arrival_positions_after_dispatch'], [])
                self.assertEqual(request['post_dispatch_unknown_or_conflicting_first_flags'], 1)


class ReceiverNackResultBoundaryTests(unittest.TestCase):
    analyze = ReceiverNackReaderTests.analyze
    state = ReceiverNackReaderTests.state
    request = ReceiverNackReaderTests.request

    def rows(self, first=100, last=107, local_status=1, local_flags=0):
        key = (7 << 32)|1
        return [self.state(), self.request(first=first, last=last),
            nack_critical_record(30, t=110032, identity=key, a=first,
                b=(1 << (last-first+1))-1, c=110020),
            nack_critical_record(20, t=110110, identity=key, a=first, b=last, c=110010),
            nack_critical_record(21, t=110045, identity=key, c=110040),
            nack_critical_record(22, t=110070, identity=key, a=0, c=110040),
            nack_critical_record(23, t=110100, identity=key,
                a=local_status & p.U64, c=110040, flags=local_flags)]

    def test_attempt_start_and_two_results_are_distinct_boundaries(self):
        key = (7 << 32)|1
        rows = self.rows()
        times = [110030, 110050, 110070, 110080, 110100, 110101]
        rows += [nack_critical_record(24, t=t, identity=key, a=100+i, c=110010, flags=1)
            for i, t in enumerate(times)]
        request = self.analyze(rows)['request_correlations'][0]
        self.assertEqual(request['transport_attempt_start_reference_us'], 110040)
        self.assertEqual(request['successful_SRTCP_result_observed_us'], 110070)
        self.assertEqual(request['successful_local_send_result_observed_us'], 110100)
        self.assertTrue(request['protected_local_send_route_success_observed'])
        self.assertEqual([r['classification'] for r in
            request['authenticated_return_result_boundary_classification']], [
            'before_attempt_start', 'before_successful_protection_result',
            'same_clock_as_protection_result_ambiguous',
            'between_successful_protection_and_local_result_ambiguous',
            'same_clock_as_local_result_ambiguous', 'after_successful_local_send_result'])
        self.assertEqual(request['first_authenticated_arrival_positions_after_successful_local_send_result'], [105])
        self.assertEqual(request['timings_us']['attempt_start_to_first_authenticated_selected_arrival'], 10)
        self.assertEqual(request['timings_us']['successful_local_send_result_to_first_authenticated_selected_arrival'], 1)
        self.assertFalse(request['all_selected_positions_have_first_arrival_after_successful_local_send_result'])
        self.assertTrue(all(not r['retransmission_causality_established'] for r in
            request['authenticated_return_result_boundary_classification']))

    def test_only_boolean_one_without_exception_is_local_success(self):
        key = (7 << 32)|1
        for status, exception in ((0, 0), (-1, 0), (1, 1), (2, 0)):
            with self.subTest(status=status, exception=exception):
                request = self.analyze(self.rows(local_status=status, local_flags=exception)+[
                    nack_critical_record(24, t=110120, identity=key, a=100, c=110010, flags=1)
                ])['request_correlations'][0]
                self.assertIsNone(request['successful_local_send_result_observed_us'])
                self.assertEqual(request['local_send_result_observed_us'], 110100)
                self.assertEqual(request['authenticated_return_result_boundary_classification'][0]['classification'],
                    'after_unsuccessful_local_result_not_post_success')
                self.assertIsNone(request['timings_us']['successful_local_send_result_to_first_authenticated_selected_arrival'])
                self.assertFalse(request['protected_local_send_route_success_observed'])

    def test_missing_local_result_never_uses_dispatch_record_clock(self):
        key = (7 << 32)|1
        rows = [r for r in self.rows() if r[2] & 65535 != 23]
        rows += [nack_critical_record(24, t=110120, identity=key, a=100, c=110010, flags=1)]
        request = self.analyze(rows)['request_correlations'][0]
        self.assertEqual(request['transport_attempt_start_reference_us'], 110040)
        self.assertIsNone(request['successful_local_send_result_observed_us'])
        self.assertEqual(request['authenticated_return_result_boundary_classification'][0]['classification'],
            'after_protection_result_local_result_unavailable')
        self.assertEqual(request['first_authenticated_arrival_positions_after_successful_local_send_result'], [])

    def test_duplicate_or_conflicting_result_references_do_not_supply_boundary(self):
        key = (7 << 32)|1
        for duplicate in (True, False):
            with self.subTest(duplicate=duplicate):
                rows = self.rows()
                rows.append(nack_critical_record(23, t=110115, identity=key, a=1,
                    c=110040 if duplicate else 110041))
                rows.append(nack_critical_record(24, t=110120, identity=key, a=100, c=110010, flags=1))
                request = self.analyze(rows)['request_correlations'][0]
                self.assertIsNone(request['successful_local_send_result_observed_us'])
                self.assertEqual(request['first_authenticated_arrival_positions_after_successful_local_send_result'], [])
                self.assertFalse(request['protected_local_send_route_success_observed'])

    def test_invalid_ordering_or_zero_attempt_reference_is_unknown(self):
        key = (7 << 32)|1
        for mutate in ('zero', 'before_request', 'local_before_protection', 'protection_before_begin'):
            with self.subTest(mutate=mutate):
                rows = []
                for row in self.rows():
                    changed = list(row)
                    kind = row[2] & 65535
                    if kind in (21, 22, 23) and mutate in ('zero', 'before_request'):
                        changed[6] = 0 if mutate == 'zero' else 110009
                    if kind == 23 and mutate == 'local_before_protection':
                        changed[0] = 110060
                    if kind == 22 and mutate == 'protection_before_begin':
                        changed[0] = 110042
                    rows.append(tuple(changed))
                rows.append(nack_critical_record(24, t=110120, identity=key, a=100, c=110010, flags=1))
                request = self.analyze(rows)['request_correlations'][0]
                self.assertFalse(request['protected_local_send_route_success_observed'])
                if mutate != 'protection_before_begin':
                    self.assertIsNone(request['successful_local_send_result_observed_us'])
                else:
                    self.assertIsNone(request['successful_SRTCP_result_observed_us'])

    def test_first_before_local_result_then_duplicate_is_not_new_repair(self):
        key = (7 << 32)|1
        request = self.analyze(self.rows(first=100, last=100)+[
            nack_critical_record(24, t=110080, identity=key, a=100, c=110010, flags=5),
            nack_critical_record(24, t=110120, identity=key, a=100, c=110010, flags=2)
        ])['request_correlations'][0]
        self.assertEqual(request['first_authenticated_arrival_positions_after_successful_local_send_result'], [])
        self.assertFalse(request['all_selected_positions_have_first_arrival_after_successful_local_send_result'])
        self.assertEqual(request['authenticated_return_result_boundary_classification'][1]['classification'],
            'after_successful_local_send_result')
        self.assertTrue(request['authenticated_return_result_boundary_classification'][1]['duplicate_observation'])

    def test_success_result_cannot_create_selected_positions_without_mask(self):
        key = (7 << 32)|1
        rows = [r for r in self.rows() if r[2] & 65535 != 30]
        request = self.analyze(rows+[
            nack_critical_record(24, t=110120, identity=key, a=100, c=110010, flags=1)
        ])['request_correlations'][0]
        self.assertEqual(request['successful_local_send_result_observed_us'], 110100)
        self.assertIsNone(request['transport_attempt_start_reference_us'])
        self.assertIsNone(request['dispatch_mask_selected_positions'])
        self.assertEqual(request['first_authenticated_arrival_positions_after_successful_local_send_result'], [])


class ReceiverNackGraceReaderTests(unittest.TestCase):
    def analyze(self, rows, omitted=None):
        numbered = [tuple(row[:7])+(i+1,) for i, row in enumerate(rows)]
        return p.analyze(fixture_v2(critical=numbered, omitted=omitted))['upstream_diagnostics']['receiver_NACK_grace']

    def row(self, kind, **kwargs):
        kwargs.setdefault('identity', (7 << 32)|1)
        kwargs.setdefault('t', 120000+kind)
        return nack_critical_record(31, stage=kind, **kwargs)

    def test_sequence_identity_collision_is_not_a_NACK_request(self):
        identity = (7 << 32)|2
        rows = [self.row(kind, identity=identity, a=6, b=65538, c=1)
                for kind in range(13, 21)]
        out = self.analyze(rows)
        self.assertEqual(out['range_correlations'], [])
        self.assertEqual(len(out['retained_sequence_binding_chronology']), 8)
        for row in out['retained_timeline']:
            self.assertIsNone(row['request_ordinal'])
            self.assertEqual(row['transport_epoch'], 7)
            if row['grace_subkind_id'] == 20:
                self.assertIsNone(row['identity_is_request'])
                self.assertIsNone(row['sequence_epoch'])
                self.assertEqual(row['identity_low32_unclassified'], 2)
                self.assertTrue(row['rejection_payload_layout_unclassified'])
                self.assertIn('rejection_payload_b', row)
                self.assertIn('rejection_payload_c', row)
                self.assertNotIn('previous_high_or_control_epoch', row)
                self.assertNotIn('committed_epoch_or_owner_transport_epoch', row)
            else:
                self.assertFalse(row['identity_is_request'])
                self.assertEqual(row['sequence_epoch'], 2)
            self.assertEqual(row['observer_record_not_worker_binding'], row['grace_subkind_id'] <= 16)

    def test_zero_and_large_sequence_ordinals_never_break_range_grouping(self):
        for identity in (0, (7 << 32), (7 << 32)|65536, (7 << 32)|0xffffffff):
            with self.subTest(identity=identity):
                out = self.analyze([self.row(13, identity=identity),
                                    self.row(19, identity=identity)])
                self.assertEqual(out['range_correlations'], [])
                self.assertEqual(len(out['retained_sequence_binding_chronology']), 2)
                self.assertTrue(all(row['request_ordinal'] is None
                                    for row in out['retained_timeline']))

    def test_binding_proof_commit_and_request_map_preserve_distinct_facts(self):
        identity = (7 << 32)|2
        rows = [self.row(17, t=120000, identity=identity, a=6, b=65538, c=1, flags=2),
                self.row(18, t=121000, identity=identity, a=6, b=70000, c=120500),
                self.row(19, t=121001, identity=identity, a=6, b=65542, c=65536, flags=2),
                self.row(1, t=130010, identity=identity, a=7, b=1, c=130000),
                self.row(21, t=130011, identity=identity, a=7, b=65543, c=2),
                self.row(3, t=130012, identity=identity, a=175000, b=1, c=0)]
        out = self.analyze(rows)
        self.assertEqual(len(out['range_correlations']), 1)
        result = out['range_correlations'][0]
        self.assertEqual(result['request']['original_missing_positions'], [7])
        self.assertTrue(result['offer_deadline_matches_detect_plus_45ms'])
        mapping = result['retained_canonical_request_maps'][0]
        self.assertEqual((mapping['first_raw_extended'], mapping['first_canonical_extended'], mapping['sequence_epoch']),
                         (7, 65543, 2))
        self.assertTrue(mapping['map_is_request_not_binding_permission'])
        proof, commit = out['retained_sequence_binding_chronology'][1:]
        self.assertTrue(proof['exact_anchor_STORED_observed'])
        self.assertTrue(proof['announcement_still_required'])
        self.assertTrue(commit['worker_binding_commit_observed'])
        self.assertFalse(commit['observer_record_not_worker_binding'])
        self.assertEqual(commit['canonical_offset'], 65536)
        self.assertFalse(result['AU_reconstruction_native_picture_and_presentation_established'])

    def test_gate_omission_is_functional_not_diagnostic_loss_or_packet_drop(self):
        rows = [self.row(15, a=4, b=90000, c=1),
                self.row(16, a=65538, b=1, flags=2),
                self.row(20, flags=(2 << 8)|7, a=6, b=65538, c=1)]
        omitted = [0]*32; omitted[31] = 1
        out = self.analyze(rows, omitted)
        gap, reset, rejected = out['retained_timeline']
        self.assertTrue(gap['functional_gate_contention'])
        self.assertFalse(gap['diagnostic_retention_omission'])
        self.assertFalse(gap['packet_discard_established'])
        self.assertEqual(reset['dirty_consumed_by'], 'poll')
        self.assertEqual((rejected['binding_result'], rejected['sequence_reason']), (7, 2))
        self.assertIsNone(rejected['identity_is_request'])
        self.assertIsNone(rejected['sequence_epoch'])
        self.assertEqual(rejected['identity_low32_unclassified'], 1)
        self.assertFalse(rejected['observer_record_not_worker_binding'])
        self.assertFalse(rejected['packet_discard_established'])
        self.assertFalse(out['coverage']['details_complete'])
        self.assertFalse(out['coverage']['absent_subkind_establishes_no_record_attempt'])

    def test_new_binding_counters_are_cumulative_and_separate_from_observer(self):
        rows = [self.row(10, identity=0, a=index, b=index-110) for index in range(116,129)]
        rows += [self.row(10, identity=0, a=118, b=9, t=140000)]
        out = self.analyze(rows)
        latest = {row['counter_id']:row for row in out['latest_retained_counter_snapshots']}
        self.assertEqual(len(latest), 13)
        self.assertEqual(latest[118]['value'], 9)
        self.assertEqual(latest[118]['counter_name'], 'binding_commits')
        self.assertEqual(latest[127]['counter_name'], 'binding_repeated_detail_suppressed')
        self.assertEqual(latest[128]['counter_name'], 'control_fifo_pending_at_close')
        self.assertEqual(out['counter_snapshot_duplicates_not_summed'], 1)
        raw = [nack_critical_record(27, flags=7), nack_critical_record(29, a=110, b=1),
               nack_critical_record(29, a=111, b=2)]
        raw = [tuple(row[:7])+(i+1,) for i,row in enumerate(raw)]
        counters = p.analyze(fixture_v2(critical=raw))['upstream_diagnostics']['receiver_NACK']['latest_retained_counter_snapshots']
        self.assertEqual([row['counter_name'] for row in counters],
                         ['sequence_control_inbox_omissions', 'observer_resets_consumed'])

    def test_older_grace_capture_keeps_raw_request_without_fabricated_binding(self):
        out = self.analyze([self.row(1, a=65535, b=3, c=110000),
                            self.row(3, a=155000, b=3, c=0)])
        self.assertEqual(out['retained_sequence_binding_chronology'], [])
        self.assertEqual(len(out['range_correlations']), 1)
        result = out['range_correlations'][0]
        self.assertEqual(result['request']['original_missing_positions'], [65535,65536])
        self.assertEqual(result['retained_canonical_request_maps'], [])
        self.assertTrue(result['offer_deadline_matches_detect_plus_45ms'])
        self.assertFalse(result['AU_reconstruction_native_picture_and_presentation_established'])

    def test_older_capture_is_unavailable_not_OFF(self):
        out = self.analyze([])
        self.assertFalse(out['available'])
        self.assertIsNone(out['experiment_enabled'])

    def test_all_subkinds_use_stage_as_grace_kind_not_transport_location(self):
        rows = [self.row(1, a=65535, b=3, c=110000),
            self.row(2, a=110040, b=110100, c=3, flags=3),
            self.row(3, a=155000, b=3, c=(32 << 32)),
            self.row(4, a=155000, b=3, c=44, flags=1),
            self.row(5, a=65535, b=119990, c=120000, flags=2),
            self.row(6, a=65535, b=120000, c=119990),
            self.row(7, a=65535, b=(65535 << 32)|32, c=155000),
            self.row(8, a=49, b=9, c=3), self.row(9, a=20, b=32, c=16),
            self.row(10, identity=0, a=101, b=0),
            self.row(11, a=(65535 << 32)|32, b=155000, c=42),
            self.row(12, a=(65535 << 32)|2, b=32, c=42, flags=2)]
        out = self.analyze(rows)
        self.assertEqual(out['retained_subkind_counts'], {name: 1 for name in (
            'CONTROL_REQUEST', 'CONTROL_OUTCOME', 'OFFER', 'RELEASE', 'CALLBACK',
            'QUEUE_POP', 'WORKER_ACCEPT', 'WORK_COST', 'QUEUE_AGE', 'COUNTER', 'HOLD_BEGIN', 'ABANDON')})
        self.assertTrue(all(r['stage_name'] == 'NACK_GRACE' and not r['stage_is_transport_location']
            for r in out['retained_timeline']))
        details = out['range_correlations'][0]
        self.assertTrue(details['offer_deadline_matches_detect_plus_45ms'])
        self.assertEqual(details['request']['original_missing_positions'], [65535, 65536])
        self.assertEqual(details['retained_release_reason_counts'], {'accepted': 1})
        self.assertFalse(details['AU_reconstruction_native_picture_and_presentation_established'])
        self.assertTrue(all(not r['output_PTS_association_established'] for r in out['retained_timeline']))

    def test_AUTH_cancellation_outcome_is_not_worker_completion(self):
        out = self.analyze([self.row(2, a=0, b=0, c=0, flags=(4 << 8))])
        row = out['retained_timeline'][0]
        self.assertEqual(row['local_result'], -2)
        self.assertTrue(row['AUTH_complete_does_not_complete_worker_grace'])
        self.assertEqual(out['range_correlations'][0]['retained_worker_STORED_observations'], [])

    def test_worker_record_clock_is_separate_from_AUTH_and_queue_pop(self):
        out = self.analyze([self.row(5, t=140000, a=100, b=139980, c=139990),
            self.row(6, t=140010, a=100, b=139990, c=139980),
            self.row(7, t=140020, a=100, b=(100 << 32)|10, c=145000)])
        position = out['range_correlations'][0]['retained_position_chronology'][0]
        self.assertEqual(position['retained_AUTH_clocks_us'], [139980])
        self.assertEqual(position['retained_callback_begin_clocks_us'], [139990])
        self.assertEqual(position['retained_queue_pop_clocks_us'], [140010])
        self.assertEqual(position['retained_worker_STORED_record_clocks_us'], [140020])
        self.assertEqual(position['STORED_record_clock_relative_to_deadline'], ['before'])
        self.assertTrue(position['record_clock_is_not_proven_actual_insert_instant'])
        self.assertEqual(out['retained_timeline'][1]['AUTH_to_queue_pop_us'], 30)

    def test_reorder_jump_or_rejection_does_not_complete_worker_mask(self):
        out = self.analyze([self.row(7, a=100, b=(100 << 32)|32, c=145000, flags=1),
            self.row(7, a=101, b=(101 << 32)|32, c=145000, flags=2)])
        self.assertEqual(out['range_correlations'][0]['retained_worker_STORED_observations'], [])

    def test_cumulative_counters_latest_only_and_critical_omissions_explicit(self):
        omitted = [0]*32; omitted[31] = 2
        out = self.analyze([self.row(10, t=120000, identity=0, a=101, b=2),
            self.row(10, t=120001, identity=0, a=101, b=3)], omitted)
        self.assertEqual(out['latest_retained_counter_snapshots'][0]['value'], 3)
        self.assertEqual(out['latest_retained_counter_snapshots'][0]['counter_name'], 'control_inbox_omitted')
        self.assertEqual(out['counter_snapshot_duplicates_not_summed'], 1)
        self.assertEqual(out['coverage']['omitted'], 2)
        self.assertFalse(out['coverage']['details_complete'])
        self.assertFalse(out['coverage']['absent_retained_subkind_establishes_absence'])
        self.assertEqual(out['coverage']['retained_control_inbox_omissions'], 3)

    def test_explicit_configuration_snapshot_observes_mode_not_held_range(self):
        for mode, expected in ((0, False), (45, True), (46, None)):
            with self.subTest(mode=mode):
                out = self.analyze([self.row(10, identity=0, a=114, b=mode),
                    self.row(10, identity=0, a=115, b=1)])
                self.assertEqual(out['experiment_enabled'], expected)
                self.assertEqual(out['range_correlations'], [])
                self.assertTrue(out['configured_mode_observation_does_not_prove_runtime_hold'])
                self.assertEqual(out['latest_retained_counter_snapshots'][1]['counter_name'], 'configured_rumble')
        out = self.analyze([self.row(10, identity=0, a=114, b=0),
            self.row(10, identity=0, a=114, b=45)])
        self.assertIsNone(out['experiment_enabled'])

    def test_deadline_overflow_duplicate_controls_and_source_mismatch_are_unknown(self):
        request = self.row(1, a=100, b=3, c=p.U64-10)
        out = self.analyze([request, self.row(3, a=0, b=3, c=0)])
        self.assertIsNone(out['range_correlations'][0]['offer_deadline_matches_detect_plus_45ms'])
        out = self.analyze([self.row(1, a=100, b=3, c=110000)]*2)
        self.assertIsNone(out['range_correlations'][0]['request'])
        out = self.analyze([self.row(1, a=100, b=3, c=110000),
            self.row(3, a=155000, b=3, c=0, ssrc=43)])
        self.assertEqual(len(out['range_correlations']), 2)
        self.assertTrue(all(r['offer_deadline_matches_detect_plus_45ms'] is None for r in out['range_correlations']))

    def test_state_release_is_separate_from_actual_worker_advance(self):
        rows = [self.row(11, t=120000, a=(65535 << 32)|32, b=155000, c=42),
            self.row(4, t=155005, a=155000, b=0, c=32, flags=2)]
        out = self.analyze(rows)
        state = out['range_correlations'][0]
        self.assertTrue(state['grace_held_observation_retained'])
        self.assertTrue(state['state_end_observation_retained'])
        self.assertFalse(state['actual_worker_hole_advance_observation_retained'])
        self.assertIsNone(state['state_end_to_worker_advance_observation_us'])
        self.assertEqual(state['hold_to_state_end_observation_us'], 35005)
        self.assertFalse(state['retained_state_end_observations'][0]['actual_worker_hole_advance_established'])
        out = self.analyze(rows+[self.row(12, t=155010, a=(65535 << 32)|2, b=32, c=42, flags=2)])
        state = out['range_correlations'][0]
        self.assertTrue(state['actual_worker_hole_advance_observation_retained'])
        self.assertEqual(state['state_end_to_worker_advance_observation_us'], 5)
        self.assertEqual(state['retained_worker_advance_reason_counts'], {'deadline': 1})
        advance = state['retained_worker_ABANDON_observations'][0]
        self.assertEqual(advance['pre_expected_wire_sequence'], 65535)
        self.assertEqual(advance['lost_positions'], 2)
        self.assertEqual(advance['post_expected_wire_sequence'], 1)
        self.assertEqual(advance['pre_buffered_packets'], 32)
        self.assertEqual(advance['pre_furthest_forward_offset'], 42)
        self.assertFalse(advance['advance_establishes_AU_or_picture'])
        self.assertFalse(advance['output_PTS_association_established'])

    def test_ABANDON_critical_fact_does_not_require_pause_window_or_release(self):
        out = self.analyze([self.row(12, t=160000, a=(100 << 32)|3, b=12, c=40, flags=3)])
        state = out['range_correlations'][0]
        self.assertFalse(state['state_end_observation_retained'])
        self.assertFalse(state['grace_held_observation_retained'])
        self.assertTrue(state['actual_worker_hole_advance_observation_retained'])
        self.assertEqual(state['retained_worker_advance_reason_counts'], {'capacity': 1})
        self.assertIsNone(state['state_end_to_worker_advance_observation_us'])

    def test_omitted_ABANDON_or_reason_conflict_does_not_invent_timeline(self):
        omitted = [0]*32; omitted[31] = 1
        release = self.row(4, t=155005, a=155000, b=0, c=32, flags=2)
        out = self.analyze([release], omitted)
        self.assertFalse(out['range_correlations'][0]['actual_worker_hole_advance_observation_retained'])
        self.assertFalse(out['coverage']['details_complete'])
        out = self.analyze([release,self.row(12, t=155010, a=(100 << 32)|1, b=32, c=42, flags=3)])
        state = out['range_correlations'][0]
        self.assertEqual(state['retained_release_reason_counts'], {'deadline': 1})
        self.assertEqual(state['retained_worker_advance_reason_counts'], {'capacity': 1})
        self.assertIsNone(state['state_end_to_worker_advance_observation_us'])

    def test_invalid_advance_metadata_and_unknown_reason_remain_raw_observations(self):
        for expected,lost,buffered,furthest in ((65536,1,1,1),(100,0,1,1),
            (100,128,128,128),(100,2,0,2),(100,2,129,2),(100,2,1,1)):
            with self.subTest(expected=expected,lost=lost,buffered=buffered,furthest=furthest):
                out = self.analyze([self.row(12,a=(expected << 32)|lost,b=buffered,c=furthest,flags=2)])
                self.assertFalse(out['range_correlations'][0]['actual_worker_hole_advance_observation_retained'])
                self.assertFalse(out['retained_timeline'][0]['advance_metadata_valid'])
        out = self.analyze([self.row(12,a=(100 << 32)|1,b=1,c=1,flags=99)])
        self.assertEqual(out['retained_timeline'][0]['grace_end_reason'], 'unknown')
        self.assertTrue(out['range_correlations'][0]['actual_worker_hole_advance_observation_retained'])

    def test_protected_ABANDON_flag_is_not_release_reason_or_complete_hole(self):
        out = self.analyze([self.row(4,t=150000,a=145000,b=0,c=32,flags=2),
            self.row(12,t=150005,a=(100 << 32)|3,b=32,c=8,flags=2|256)])
        state = out['range_correlations'][0]
        advance = state['retained_worker_ABANDON_observations'][0]
        self.assertEqual(advance['grace_end_reason_code'], 2)
        self.assertEqual(advance['grace_end_reason'], 'deadline')
        self.assertTrue(advance['protected_boundary_before_active_pending'])
        self.assertEqual(advance['post_expected_wire_sequence'], 103)
        self.assertEqual(state['state_end_to_worker_advance_observation_us'], 5)
        self.assertFalse(advance['packet_emit_established'])
        self.assertTrue(advance['protected_boundary_does_not_complete_pending_range'])

    def test_ordinary_limited_ABANDON_identity_zero_has_no_request_association(self):
        out = self.analyze([self.row(12,identity=0,a=(65535 << 32)|1,b=32,c=10,flags=256)])
        self.assertEqual(out['range_correlations'], [])
        self.assertEqual(len(out['retained_ordinary_worker_ABANDON_observations']), 1)
        advance = out['retained_ordinary_worker_ABANDON_observations'][0]
        self.assertTrue(advance['ordinary_worker_advance_observation'])
        self.assertFalse(advance['ordinary_advance_has_request_association'])
        self.assertEqual(advance['grace_end_reason'], 'none')
        self.assertTrue(advance['protected_boundary_before_active_pending'])
        self.assertEqual(advance['post_expected_wire_sequence'], 0)

    def test_SOURCE_code5_is_shared_with_reorder_reset_not_proof_of_SSRC_change(self):
        out = self.analyze([self.row(4,t=150000,a=145000,b=0,c=32,flags=5),
            self.row(12,t=150005,a=(100 << 32)|3,b=32,c=8,flags=5|256)])
        state = out['range_correlations'][0]
        release = state['retained_state_end_observations'][0]
        advance = state['retained_worker_ABANDON_observations'][0]
        self.assertEqual(release['release_reason_code'],5)
        self.assertEqual(advance['grace_end_reason_code'],5)
        self.assertEqual(release['release_reason'],'source_or_reorder_reset')
        self.assertEqual(advance['grace_end_reason'],'source_or_reorder_reset')
        self.assertEqual(release['ssrc'],advance['ssrc'])
        for row in (release,advance):
            self.assertTrue(row['reason5_is_shared_source_or_reorder_reset'])
            self.assertFalse(row['SSRC_change_established_by_reason5'])
        self.assertIn('window JUMP/reset',out['release_reason_code5_scope'])
        self.assertEqual(state['state_end_to_worker_advance_observation_us'],5)

    def test_REORDER_HOLE_protected_prefix_preserves_each_existing_reason_bit(self):
        row = record(150000,0x10c,a=(65535 << 32)|2,b=32,flags=1|2|4|8|16)
        result = p.analyze(fixture(history=[row]))
        hole = result['retained_timeline'][0]
        for name in ('reason_depth32','reason_ordinary_time_expiry',
            'ordinary_time_expiry_permitted_for_call','reason_forced_grace_release',
            'reason_advance_limited_before_active_pending'):
            self.assertTrue(hole[name])
        self.assertEqual(hole['expected_after_skipped_prefix_wire'], 1)
        self.assertFalse(hole['packet_emit_established'])
        self.assertFalse(hole['Decode_feed_established'])
        self.assertTrue(hole['skipped_prefix_does_not_complete_protected_range'])
        baseline = p.analyze(fixture(history=[record(150000,0x10c,a=(100 << 32)|1,b=32,flags=1|4)]))
        hole = baseline['retained_timeline'][0]
        self.assertTrue(hole['reason_depth32'])
        self.assertTrue(hole['ordinary_time_expiry_permitted_for_call'])
        self.assertFalse(hole['reason_ordinary_time_expiry'])
        self.assertFalse(hole['reason_advance_limited_before_active_pending'])


class V2ProgressReaderTests(unittest.TestCase):
    def test_empty_appendix_and_v1_compatibility(self):
        result = p.analyze(fixture_v2())
        self.assertEqual(result['schema'], 'X4PROG2/v2 analysis')
        self.assertTrue(result['upstream_diagnostics']['coverage']['detail_complete'])
        self.assertFalse(p.analyze(fixture())['upstream_diagnostics']['available'])

    def test_gap_wrap_later_position_and_stage_correlation(self):
        rows = [critical_record(1, 0, a=65534, b=65538, c=4, flags=1),
                critical_record(1, 10, t=100020, a=65534, b=65538, c=4, identity=2, ordinal=2),
                critical_record(2, 10, t=100030, a=65536, b=65539, c=100020, identity=2, ordinal=3)]
        out = p.analyze(fixture_v2(critical=rows))['upstream_diagnostics']
        gap = out['retained_gap_chronology'][1]
        self.assertTrue(gap['wraps'])
        self.assertEqual(gap['range_end'], 1)
        group = out['exact_range_candidate_groups'][gap['exact_range_candidate_group_id']-1]
        self.assertEqual(len(group['observations']), 2)
        self.assertEqual(gap['later_positions'][0]['sequence'], 0)
        self.assertFalse(gap['first_missing_stage_established'])
        self.assertFalse(gap['physical_network_loss_established'])

    def test_original_and_RTX_sources_are_not_equated(self):
        rows = [critical_record(1, a=9, b=11, c=2),
                critical_record(1, 11, a=9, b=11, c=2, pt=103, ordinal=2)]
        out = p.analyze(fixture_v2(critical=rows))['upstream_diagnostics']
        self.assertEqual([len(g['observations']) for g in out['exact_range_candidate_groups']], [1, 1])

    def test_omissions_survive_no_retained_gap(self):
        drops = [0]*32; drops[1] = 3
        out = p.analyze(fixture_v2(omitted=drops))['upstream_diagnostics']['coverage']
        self.assertEqual(out['event_counts']['SEQUENCE_GAP']['omitted'], 3)
        self.assertFalse(out['detail_complete'])

    def test_AU_stages_do_not_confuse_recovery_and_assembly(self):
        attempted = [0]*512
        attempted[6*64+8], attempted[6*64+9], attempted[6*64] = 10, 3, 7
        au = p.analyze(fixture_v2(attempts=attempted,
            au_filter=[0, 1, 0, 2, 0, 0, 0, 0]))['upstream_diagnostics']['AU_stage_totals']
        self.assertEqual(au['structurally_complete_before_recovery_filter'], 10)
        self.assertEqual(au['rejected_by_recovery_filter'], 3)
        self.assertEqual(au['accepted_for_Decode_feed'], 7)
        self.assertEqual(au['filter_flag_histogram']['3']['missing'], ['IDR', 'SPS'])

    def test_context_omissions_field_is_versioned(self):
        wh = [1, 0, 0, 7, 100000, 200000, 300000, 800000, 1, 0, 0, 0, 7, 1, 99, 0]
        self.assertEqual(p.analyze(fixture_v2(windows=[(wh, [])]))['upstream_diagnostics']['retention']['prehistory_cap'], 512)
        self.assertFalse(p.analyze(fixture_v2(windows=[(wh, [])]))['coverage']['negative_detail_evidence_complete'])
        with self.assertRaises(ValueError):
            p.parse(fixture(windows=[(wh, [])]))

    def test_bounded_reserved_and_counter_checks(self):
        for overrides in ({5: 4097}, {6: 2}, {27: 1}, {9: 17}, {26: 0}):
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                p.parse(fixture_v2(critical_header=overrides))
        with self.assertRaises(ValueError):
            p.parse(fixture_v2()[:-1])
        with self.assertRaises(ValueError):
            p.parse(fixture_v2()+b'\0')

    def test_bad_range_and_noncontiguous_ordinal_rejected(self):
        for row in (critical_record(1, a=4, b=6, c=1),
                    critical_record(1, a=4, b=6, c=2, ordinal=2)):
            with self.assertRaises(ValueError):
                p.parse(fixture_v2(critical=[row]))

    def test_extended_cycles_do_not_share_gap_identity(self):
        rows = [critical_record(1, a=9, b=11, c=2),
                critical_record(1, t=3000000, a=65545, b=65547, c=2, ordinal=2)]
        out = p.analyze(fixture_v2(critical=rows))['upstream_diagnostics']
        gaps = out['retained_gap_chronology']
        self.assertEqual([len(g['observations']) for g in out['exact_range_candidate_groups']], [1, 1])
        self.assertEqual(gaps[1]['expected'], 9)
        self.assertFalse(gaps[1]['stage_local_extension_epochs_aligned'])

    def test_source_extended_highwater_is_defined_not_reserved(self):
        row = [0]*24; row[0] = (1 << 63)|(102 << 32)|42
        row[5], row[18] = 65536|3, 65539
        source = p.analyze(fixture_v2(stage_sources=[(10, 0, row)]))['upstream_diagnostics']['stages'][10]['source_rows'][0]
        self.assertEqual(source['extended_high_water_sequence'], 65539)
        row[19] = 1
        with self.assertRaises(ValueError):
            p.parse(fixture_v2(stage_sources=[(10, 0, row)]))

    def test_per_event_totals_cannot_swap_record_types(self):
        data = bytearray(fixture_v2(critical=[critical_record(1, a=9, b=11, c=2)]))
        # Keep the global sum unchanged while falsifying the per-type summary.
        append_offset = len(fixture())
        attempts_offset = append_offset+256
        struct.pack_into('<Q', data, attempts_offset+8, 0)
        struct.pack_into('<Q', data, attempts_offset+16, 1)
        with self.assertRaises(ValueError):
            p.parse(data)

    def test_announced_NACK_and_observed_RTX_remain_separate(self):
        rows = [critical_record(17, 65535, identity=13, a=(1 << 32)|(2 << 16)|102, b=3),
                critical_record(17, 65535, identity=20, a=1, b=0, c=0, ordinal=2),
                critical_record(17, 65535, identity=3, a=(43 << 32)|(103 << 16)|5,
                                b=(42 << 32)|(102 << 16)|9, ordinal=3)]
        settings = p.analyze(fixture_v2(critical=rows))['upstream_diagnostics']['retained_negotiation_and_RTX_settings']
        self.assertEqual(settings[0]['scope'], 'accepted_answer')
        self.assertEqual(settings[0]['feedback_announced'], ['nack', 'nack_pli'])
        self.assertFalse(settings[1]['receiver_NACK_generator_installed'])
        self.assertEqual(settings[2]['wire_RTX']['seq'], 5)
        self.assertEqual(settings[2]['normalized_original']['seq'], 9)

    def test_SDP_summary_scope_uses_distinct_numeric_layout(self):
        rows = [critical_record(17, 65535, identity=19, a=1, b=24, c=0)]
        item = p.analyze(fixture_v2(critical=rows))['upstream_diagnostics']['retained_negotiation_and_RTX_settings'][0]
        self.assertEqual(item['scope'], 'accepted_answer')
        self.assertEqual(item['summary_records'], 24)
        self.assertFalse(item['summary_capped'])
        self.assertNotIn('payload_type', item)

    def test_full_critical_ring_uses_shared_candidate_groups(self):
        rows = [critical_record(1, a=9, b=11, c=2, ordinal=i+1) for i in range(4096)]
        out = p.analyze(fixture_v2(critical=rows))['upstream_diagnostics']
        self.assertEqual(len(out['exact_range_candidate_groups']), 1)
        self.assertEqual(len(out['exact_range_candidate_groups'][0]['observations']), 4096)
        self.assertEqual(len(out['retained_gap_chronology']), 4096)
        self.assertNotIn('exact_range_observations', out['retained_gap_chronology'][0])

    def test_auth_replay_and_binding_limits_have_distinct_fields(self):
        rows = [critical_record(3, 4, a=28, b=7, c=9, flags=1),
                critical_record(3, 4, a=29, b=9, c=10, flags=1, ordinal=2),
                critical_record(3, 4, a=30, b=10, c=11, flags=1, ordinal=3),
                critical_record(17, 65535, identity=22, a=3, b=32, c=1, ordinal=4),
                critical_record(17, 65535, identity=23, a=2, b=1, c=0, ordinal=5)]
        out = p.analyze(fixture_v2(critical=rows))['upstream_diagnostics']
        self.assertEqual([r['reason'] for r in out['retained_critical_timeline'][:3]],
                         ['SRTP_AUTH_FAIL', 'SRTP_REPLAY_FAIL', 'SRTP_REPLAY_OLD'])
        settings = out['retained_negotiation_and_RTX_settings']
        self.assertEqual(settings[0]['snapshot_phase'], 'detach')
        self.assertEqual(settings[0]['thread_slot_omissions_process_total'], 3)
        self.assertEqual(settings[1]['process_trace_bindings'], 2)
        self.assertFalse(settings[1]['transport_owner_epoch_association_available'])

    def test_uninstrumented_branch_slots_preserve_raw_zero_as_unknown(self):
        out = p.analyze(fixture_v2())['upstream_diagnostics']
        for stage in out['stages']:
            for name, reason in (('ICE_CALLBACK_EXCEPTION', 24), ('SOCKET_FAIRNESS', 26)):
                item = stage['unavailable_branch_counter_slots'][name]
                self.assertEqual(item['reason_id'], reason)
                self.assertEqual(item['raw_count'], 0)
                self.assertFalse(item['measurement_available'])
                self.assertIsNone(item['measured_outcomes'])
        rejected = [[0]*32 for _ in range(16)]
        rejected[1][24], rejected[0][26] = 7, 3
        stages = p.analyze(fixture_v2(rejected=rejected))['upstream_diagnostics']['stages']
        self.assertEqual(stages[1]['unavailable_branch_counter_slots']['ICE_CALLBACK_EXCEPTION']['raw_count'], 7)
        self.assertIsNone(stages[1]['unavailable_branch_counter_slots']['ICE_CALLBACK_EXCEPTION']['measured_outcomes'])
        self.assertEqual(stages[0]['branch_counters']['SOCKET_FAIRNESS'], 3)

    def test_branch_labels_do_not_invent_discard_full_queue_or_prior_SRTP_input(self):
        rows = [critical_record(3, 2, a=8, flags=1),
                critical_record(3, 13, a=21, flags=1, ordinal=2),
                critical_record(3, 4, a=12, flags=0, ordinal=3)]
        rejected = [[0]*32 for _ in range(16)]
        rejected[2][8] = rejected[13][21] = rejected[4][12] = 1
        out = p.analyze(fixture_v2(critical=rows, rejected=rejected))['upstream_diagnostics']
        queue, rtx, demux = out['retained_critical_timeline']
        self.assertEqual(queue['reason'], 'DTLS_QUEUE_FULL_OR_STOPPING')
        self.assertFalse(queue['queue_full_vs_stopping_distinguished'])
        self.assertEqual(out['stages'][2]['branch_counters']['DTLS_QUEUE_FULL_OR_STOPPING'], 1)
        self.assertTrue(rtx['existing_branch_continues'])
        self.assertTrue(rtx['same_packet_PEER_HANDLER_OUT_observations_may_repeat'])
        self.assertFalse(demux['preceding_SRTP_INPUT_observation_established'])
        self.assertEqual(out['stages'][4]['packet_observations'], 0)
        for row in (queue, rtx, demux):
            self.assertTrue(row['stage_is_branch_location'])
            self.assertTrue(row['packet_discard_not_established_by_record_name'])


class SyntheticProgressReaderTests(unittest.TestCase):
    def test_valid_empty_bounded_file(self):
        data = fixture()
        out = p.analyze(data)
        self.assertEqual(out['input_sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(out['requested_config']['width'], 960)
        self.assertEqual(out['presentation']['retained_interval_distribution']['count'], 0)
        self.assertIsNone(out['presentation']['epochs']['stable']['closed_gap_rate_per_min'])
        self.assertTrue(out['private_numeric_evidence'])

    def test_exact_cadence_and_manual_stable_denominator(self):
        mark = record(100000, 0x614, 2, 1)
        cadence = [(400000, 1, 100001, (2 << 32)|2), (500000, 2, 100000, (2 << 32)|2)]
        out = p.analyze(fixture(history=[mark], cadence=cadence, exposures=[0, 0, 0, 0, 1000000, 2, 0, 0]))
        stable = out['presentation']['epochs']['stable']
        self.assertEqual(stable['retained_closed_gt100ms'], 1)  # strict > threshold
        self.assertEqual(stable['closed_gap_rate_per_min'], 60)
        self.assertTrue(out['presentation']['distribution_is_every_interval'])
        self.assertEqual(out['presentation']['retained_interval_distribution']['p95_us'], 100001)

    def test_same_endpoint_phase_with_intervening_boundary_excluded(self):
        marks = [record(100000, 0x614, 2, 1, 1), record(250000, 0x614, 3, 2, 2), record(350000, 0x614, 2, 3, 3)]
        out = p.analyze(fixture(history=marks, cadence=[(400000, 7, 200000, (2 << 32)|2)],
            exposures=[0, 0, 0, 0, 1000000, 1, 100000, 0]))
        c = out['presentation']['retained_cadence'][0]
        self.assertTrue(c['phase_crossing_observed'])
        self.assertFalse(c['stable_only_eligible'])
        self.assertEqual(out['presentation']['epochs']['stable']['retained_closed_gt100ms'], 0)

    def test_missing_manual_marks_disables_stable_rate(self):
        out = p.analyze(fixture(cadence=[(400000, 1, 200000, (2 << 32)|2)],
            exposures=[0, 0, 0, 0, 1000000, 1, 0, 0], header={68: 1, 90: 1}))
        self.assertIsNone(out['presentation']['epochs']['stable']['closed_gap_rate_per_min'])
        self.assertFalse(out['coverage']['manual_phase_records_complete'])

    def test_cadence_omission_histogram_bounds_not_exact_quantiles(self):
        buckets = [0]*32; buckets[16] = 2; buckets[17] = 1
        out = p.analyze(fixture(cadence=[(400000, 1, 100000, (2 << 32)|2)],
            histogram=buckets, header={13: 2, 33: 2}))
        self.assertFalse(out['presentation']['distribution_is_every_interval'])
        hist = out['presentation']['all_interval_histogram_bounds']
        self.assertEqual(hist['p95_us_bounds'], [131072, 262143])
        self.assertFalse(hist['exact'])
        self.assertEqual(out['presentation']['retained_interval_distribution']['count'], 1)

    def test_histogram_top_bucket_is_saturated(self):
        buckets = [0]*32; buckets[31] = 1
        self.assertEqual(p.histogram_bounds(buckets)['max_us_bounds'], [1 << 31, None])

    def test_own_sampler_lateness_and_noncausal_progress(self):
        rows = [sample(100000, 100003, video_callbacks=1, queue_accepted_callbacks=1),
                sample(200000, 260000, video_callbacks=2, queue_accepted_callbacks=2)]
        out = p.analyze(fixture(samples=rows, header={20: 1, 21: 60000}))
        interval = out['sampler']['successive_sample_progress'][0]
        self.assertEqual(out['sampler']['lateness_retained']['max_us'], 60000)
        self.assertEqual(interval['investigation_lead'], 'queue_accept_without_valid_AU_progress')
        self.assertFalse(interval['causal_classification'])
        self.assertFalse(interval['globally_coherent_snapshot'])

    def test_counter_wrap_reports_modular_delta_without_rate_or_absence(self):
        rows = [sample(100000, 100000, video_callbacks=p.U64-1), sample(200000, 200000, video_callbacks=2)]
        interval = p.analyze(fixture(samples=rows))['sampler']['successive_sample_progress'][0]
        self.assertEqual(interval['counter_deltas']['video_callbacks'], 4)
        self.assertFalse(interval['rates_valid'])
        self.assertIn('video_callbacks', interval['wrapped_or_reset_counters'])
        self.assertEqual(interval['investigation_lead'], 'indeterminate')

    def test_ordinal_overlap_dedup_wrap_and_conflicting_ordinals(self):
        rows = [record(100000, 0x600, 1, 20, 0xffffffff), record(200000, 0x600, 2, 21, 0)]
        wh = [1, 2, 0, 1, 100000, 200000, 300000, 800000, 1, 0, 0, 2, 1, 1, 0, 0]
        out = p.analyze(fixture(history=rows, windows=[(wh, rows)]))
        self.assertEqual(out['coverage']['overlap_duplicates_removed'], 2)
        self.assertEqual(out['coverage']['deduplicated_records'], 2)
        self.assertEqual(out['coverage']['ordinal_conflicts_or_wraps'], 0)
        conflict = rows + [record(300000, 0x600, 3, 22, 0)]
        out = p.analyze(fixture(history=conflict))
        self.assertEqual(out['coverage']['deduplicated_records'], 3)
        self.assertEqual(out['coverage']['ordinal_conflicts_or_wraps'], 1)
        self.assertFalse(out['coverage']['negative_detail_evidence_complete'])

    def test_pli_signed_local_result_is_never_ack(self):
        out = p.analyze(fixture(history=[record(100000, 0x605, 9), record(110000, 0x606, 9, 0xffffffff, 2)]))
        pli = out['retained_PLI_local_results'][0]
        self.assertEqual(pli['local_result'], -1)
        self.assertFalse(pli['remote_delivery_or_acknowledgment_measured'])

    def test_recovery_pairs_time_not_unrelated_au_id_and_open_unknown(self):
        rows = [record(100000, 0x126, 10, 0, 1, 8), record(340194, 0x127, 99, 0, 2),
                record(500000, 0x126, 102, 0, 3, 9)]
        result = p.analyze(fixture(history=rows, header={69: 1, 33: 8192}))['retained_recovery']
        self.assertEqual(result['retained_completed'][0]['duration_us'], 240194)
        self.assertIsNotNone(result['open_at_capture_end'])
        self.assertIsNone(result['open_duration_us'])

    def test_recovery_unmatched_end_not_completed_duration(self):
        result = p.analyze(fixture(history=[record(500000, 0x127, 102)]))['retained_recovery']
        self.assertEqual(result['unmatched_or_ambiguous'], 1)
        self.assertEqual(result['retained_completed'], [])

    def test_queue_unpublished_unsafe_oldest_age_and_torn_actor(self):
        row = sample(100000, 100010, coherence_bits=4, queue_flags=1|4, oldest_arrival_us=90000)
        row[32:36] = [3, 2, 80000, p.U64]
        result = p.analyze(fixture(samples=[row]))['sampler']['rows'][0]
        self.assertIsNone(result['queue_oldest_age_us'])
        self.assertFalse(result['actors'][0]['coherent'])
        self.assertIsNone(result['actors'][0]['observed_phase_age_us'])

    def test_valid_queue_age_and_actor_version(self):
        row = sample(100000, 100010, coherence_bits=4|256, queue_flags=1, oldest_arrival_us=90000)
        row[32:36] = [3, 2, 80000, 4]
        result = p.analyze(fixture(samples=[row]))['sampler']['rows'][0]
        self.assertEqual(result['queue_oldest_age_us'], 10010)
        self.assertTrue(result['actors'][0]['coherent'])
        self.assertEqual(result['actors'][0]['observed_phase_age_us'], 20010)

    def test_source_sequence_skips_not_network_loss(self):
        rows = [[0]*16 for _ in range(16)]
        rows[0][:10] = [(1 << 32)|123, 10, 2048, 100000, 4, 65535|65536, 9, 2, 1, 50000]
        source = p.analyze(fixture(rx=rows))['rx_sources'][0]
        self.assertEqual(source['forward_skipped_positions'], 9)
        self.assertFalse(source['skipped_positions_are_network_loss'])
        self.assertEqual(source['ssrc'], 123)
        self.assertEqual(source['last_sequence'], 65535)
        self.assertTrue(source['last_sequence_present'])

    def test_coverage_prevents_negative_detail_claim(self):
        out = p.analyze(fixture(header={11: 2, 15: 4, 33: 1|4|16|32, 64: 2}))
        self.assertFalse(out['coverage']['negative_detail_evidence_complete'])
        self.assertIn('sample_cap', out['coverage']['flags'])
        self.assertEqual(out['header']['sample_omitted'], 2)

    def test_binary_truncation_and_trailing_data_rejected(self):
        data = fixture(samples=[sample(100000, 100000)])
        for n in (0, 7, 100, 1023, 1024, len(data)-1):
            with self.subTest(n=n), self.assertRaises(ValueError):
                p.analyze(data[:n])
        with self.assertRaises(ValueError):
            p.parse(data+b'\0')

    def test_magic_version_layout_count_and_quiescence_rejected(self):
        for overrides in ({1: 2}, {2: 2048}, {43: 512}, {10: 12001}, {12: 65537}, {16: 9}, {89: 65}, {91: 16},
                          {32: 1}, {7: p.U64}, {8: 3}, {63: 1}, {127: 1}):
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                p.parse(fixture(header=overrides))
        bad = bytearray(fixture()); bad[0] = 0
        with self.assertRaises(ValueError):
            p.parse(bytes(bad))

    def test_reserved_sample_rx_window_and_invalid_epoch_rejected(self):
        source = [[0]*16 for _ in range(16)]; source[0][15] = 1
        with self.assertRaises(ValueError):
            p.parse(fixture(rx=source))
        wh = [1, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 1, 0]
        with self.assertRaises(ValueError):
            p.parse(fixture(windows=[(wh, [])]))
        with self.assertRaises(ValueError):
            p.parse(fixture(cadence=[(400000, 1, 100000, 5)]))
        with self.assertRaises(ValueError):
            p.parse(fixture(phases=[(100000, 1, 5, 1)]))

    def test_inconsistent_independent_totals_rejected(self):
        for overrides in ({18: 1}, {19: 1}, {27: 1}, {28: 1}):
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                p.parse(fixture(header=overrides))

    def test_independent_cumulative_sum_wrap_preserved(self):
        attempted = [0]*512; attempted[64] = p.U64; attempted[65] = 2
        out = p.analyze(fixture(attempts=attempted))
        self.assertEqual(out['header']['event_attempts'], 1)

    def test_read_size_cap(self):
        with self.assertRaises(ValueError):
            p.parse(b'\0'*(p.MAX_BYTES+1))

    def test_no_au_to_output_pts_association(self):
        out = p.analyze(fixture(history=[record(100000, 0x600, 10, 20), record(110000, 0x202, 99, 0, 2)]))
        self.assertTrue(any('no returned native-output PTS' in text for text in out['limits']))
        self.assertNotIn('input_output_association', out)

    def test_geometry_decodes_actual_hook_packing_without_input_association(self):
        result = p.analyze(fixture(history=[record(100000, 0x228, 99, (1280 << 32)|(720 << 16)|1280)]))
        geometry = result['delivered_geometry_observations'][0]
        self.assertEqual((geometry['width'], geometry['height'], geometry['pitch']), (1280, 720, 1280))
        self.assertEqual(geometry['native_output_id'], 99)

    def test_unstable_cadence_sample_does_not_invent_new_progress(self):
        rows = [sample(100000, 100000, NEW_count=50), sample(200000, 200000, coherence_bits=0)]
        progress = p.analyze(fixture(samples=rows))['sampler']['successive_sample_progress'][0]
        self.assertFalse(progress['rates_valid'])
        self.assertFalse(progress['cadence_endpoints_coherent'])
        self.assertEqual(progress['investigation_lead'], 'indeterminate')

    def test_phase_ledger_survives_missing_detail_events(self):
        out = p.analyze(fixture(phases=[(100000, 1, 2, 1)], header={68: 1, 15: 9000, 33: 4},
            cadence=[(400000, 7, 200000, (2 << 32)|2)], exposures=[0, 0, 0, 0, 1000000, 1, 0, 0]))
        self.assertTrue(out['coverage']['manual_phase_records_complete'])
        self.assertEqual(out['presentation']['epochs']['stable']['closed_gap_rate_per_min'], 60)
        self.assertFalse(out['coverage']['negative_detail_evidence_complete'])

    def test_phase_ledger_cap_disables_missing_boundary_rate(self):
        out = p.analyze(fixture(phases=[(100000, 1, 2, 1)], header={68: 2, 90: 1},
            cadence=[(400000, 7, 200000, (2 << 32)|2)], exposures=[0, 0, 0, 0, 1000000, 1, 0, 0]))
        self.assertFalse(out['coverage']['manual_phase_records_complete'])
        self.assertIsNone(out['presentation']['epochs']['stable']['closed_gap_rate_per_min'])

    def test_mixed_flag_excludes_same_epoch_when_boundary_ledger_capped(self):
        out = p.analyze(fixture(phases=[(100000, 1, 2, 1)], header={68: 3, 90: 2},
            cadence=[(400000, 7, 200000, (1 << 63)|(2 << 32)|2)],
            exposures=[0, 0, 0, 0, 1000000, 0, 100000, 0]))
        row = out['presentation']['retained_cadence'][0]
        self.assertTrue(row['mixed_epoch_flag'])
        self.assertTrue(row['phase_crossing_observed'])
        self.assertFalse(row['stable_only_eligible'])

    def test_snapshot_observation_range_is_not_instantaneous(self):
        rows = [sample(100000, 100000, observation_end_us=100020),
                sample(200000, 200000, observation_end_us=250000)]
        result = p.analyze(fixture(samples=rows))['sampler']
        self.assertEqual(result['rows'][1]['observation_span_us'], 50000)
        self.assertEqual(result['successive_sample_progress'][0]['end_us'], 250000)
        self.assertFalse(result['successive_sample_progress'][0]['counter_read_time_is_exact'])

    def test_circular_sample_overwrite_has_explicit_total_and_retained_population(self):
        result = p.analyze(fixture(samples=[sample(200000, 200000), sample(300000, 300000)],
            header={11: 12000, 33: 1}))
        self.assertEqual(result['header']['sample_total'], 12002)
        self.assertEqual(len(result['sampler']['rows']), 2)
        self.assertEqual(result['sampler']['lateness_retained']['count'], 2)
        self.assertFalse(result['coverage']['negative_detail_evidence_complete'])

    def test_gap_chronology_limits_sampled_lead_not_whole_cause(self):
        samples = [sample(200000, 200000, video_callbacks=10, queue_accepted_callbacks=10),
                   sample(300000, 300000, video_callbacks=11, queue_accepted_callbacks=11)]
        out = p.analyze(fixture(samples=samples, phases=[(100000, 1, 2, 1)], header={68: 1},
            cadence=[(400000, 7, 250000, (2 << 32)|2)], exposures=[0, 0, 0, 0, 1000000, 1, 0, 0]))
        gap = out['closed_gap_chronology'][0]
        self.assertEqual(gap['interior_sampler_rows'], 2)
        self.assertIsNotNone(gap['first_observed_no_progress_lead'])
        self.assertFalse(gap['cause_established'])
        self.assertTrue(gap['unsampled_or_unretained_lead_possible'])

    def test_decoder_restart_never_pairs_reused_operation_id(self):
        rows = [record(100000, 0x200, 1, 10, 1), record(200000, 0x22b, 1, 0, 2),
                record(250000, 0x22a, 2, 0, 3), record(300000, 0x201, 1, 11, 4)]
        result = p.analyze(fixture(history=rows))['retained_operation_spans']['Decode']
        self.assertEqual(result['count'], 0)
        self.assertEqual(result['unmatched_or_ambiguous'], 2)

    def test_recovery_source_boundary_remains_uncompleted(self):
        rows = [record(100000, 0x126, 10, 0, 1, 8), record(200000, 0x111, 2, 0, 2), record(300000, 0x127, 99, 0, 3)]
        result = p.analyze(fixture(history=rows))['retained_recovery']
        self.assertEqual(result['retained_completed'], [])
        self.assertEqual(len(result['interrupted_by_source_or_decoder_boundary']), 1)
        self.assertIsNone(result['interrupted_by_source_or_decoder_boundary'][0]['completed_duration_us'])

    def test_actor_phase_can_begin_inside_sample_read_range(self):
        row = sample(100000, 100000, observation_end_us=100100, coherence_bits=1|256)
        row[32:36] = [3, 2, 100050, 4]
        result = p.analyze(fixture(samples=[row]))['sampler']['rows'][0]['actors'][0]
        self.assertIsNone(result['observed_phase_age_us'])
        self.assertEqual(result['phase_age_us_bounds_in_observation'], [0, 50])

    def test_backwards_sampler_time_is_reported_not_interpreted_as_absence(self):
        rows = [sample(200000, 200000), sample(100000, 100000)]
        out = p.analyze(fixture(samples=rows, header={88: 1}, cadence=[(400000, 1, 250000, 0)]))
        self.assertFalse(out['sampler']['successive_sample_progress'][0]['rates_valid'])
        self.assertFalse(out['closed_gap_chronology'][0]['sample_time_order_valid'])
        self.assertEqual(out['closed_gap_chronology'][0]['interior_sampler_rows'], 0)

    def test_open_gap_window_has_no_completed_duration(self):
        wh = [1, 0, 8192, 9, 100000, 210000, 0, 300000, 4, 0, 0, 0, 9, 0, 0, 0]
        out = p.analyze(fixture(windows=[(wh, [])], header={69: 1, 33: 8192}))
        self.assertTrue(out['coverage']['open_gap_at_stop'])
        self.assertIsNone(out['presentation']['open_gap_duration_us'])
        self.assertEqual(out['closed_gap_chronology'], [])

    def test_primary_rx_sequence_unknown_and_coherent_matched_source(self):
        unknown = sample(100000, 100000, primary_video_sequence_encoded=p.U64, primary_video_source_key=(1 << 32)|123)
        valid = sample(200000, 200000, coherence_bits=1|2|8, primary_video_sequence_encoded=65536|65535, primary_video_source_key=(1 << 32)|123)
        rows = p.analyze(fixture(samples=[unknown, valid]))['sampler']['rows']
        self.assertTrue(rows[0]['primary_video_sequence']['unknown'])
        self.assertIsNone(rows[0]['primary_video_sequence']['last_sequence'])
        self.assertEqual(rows[1]['primary_video_sequence']['last_sequence'], 65535)
        self.assertTrue(rows[1]['primary_video_sequence']['coherent_matched_source'])

    def test_private_raw_rx_flags_do_not_fabricate_identity_for_invalid_packet(self):
        rows = [record(100000, 0x61a, (123 << 32)|10, 400, 1, 256), record(110000, 0x61a, 0, 10, 2, 0)]
        result = p.analyze(fixture(history=rows))['retained_RX_observations']
        self.assertEqual(result[0]['ssrc'], 123)
        self.assertEqual(result[0]['sequence'], 10)
        self.assertIsNone(result[1]['ssrc'])
        self.assertIsNone(result[1]['sequence'])

    def test_history_gate_unknown_never_looks_like_zero_or_counter_reset(self):
        rows = [sample(100000, 100000, history_overwrites=10),
                sample(200000, 200000, history_overwrites=p.U64, coherence_bits=1)]
        out = p.analyze(fixture(samples=rows, header={86: 1, 33: 32}))
        self.assertEqual(out['sampler']['rows'][0]['history_overwrites'], 10)
        self.assertIsNone(out['sampler']['rows'][1]['history_overwrites'])
        self.assertFalse(out['sampler']['rows'][1]['history_overwrites_coherent'])
        self.assertNotIn('history_overwrites', out['sampler']['successive_sample_progress'][0]['wrapped_or_reset_counters'])
        self.assertFalse(out['coverage']['negative_detail_evidence_complete'])

    def test_cli_never_overwrites_input_or_existing_output(self):
        with tempfile.TemporaryDirectory(prefix='x4-synthetic-reader-') as folder:
            path, out = Path(folder)/'synthetic.progress.bin', Path(folder)/'private.json'
            original = fixture()
            path.write_bytes(original)
            command = [sys.executable, str(PARSER), str(path), '--out']
            result = subprocess.run(command+[str(out)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(json.loads(out.read_text())['private_numeric_evidence'])
            written = out.read_bytes()
            result = subprocess.run(command+[str(out)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(out.read_bytes(), written)
            result = subprocess.run(command+[str(path)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(path.read_bytes(), original)
            malformed = Path(folder)/'bad.progress.bin'; malformed.write_bytes(b'bad')
            missing = Path(folder)/'never-created.json'
            result = subprocess.run([sys.executable, str(PARSER), str(malformed), '--out', str(missing)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(missing.exists())
            self.assertEqual(malformed.read_bytes(), b'bad')


class HelperIdentityTests(unittest.TestCase):
    def test_parallel_helpers_share_copy_id_but_not_identity(self):
        rows = [record(100+i, 0x222, a=19, ordinal=10+i, flags=i << 8)
                for i in range(3)]
        rows += [record(110+i, 0x223, a=19, ordinal=20+i,
                        flags=(i << 8) | 1) for i in range(3)]
        result = p.durations(rows, 0x222, 0x223, True)
        self.assertEqual(result['count'], 3)
        self.assertEqual(result['p50_us'], 10)
        self.assertEqual(result['unmatched_or_ambiguous'], 0)

    def test_missing_helper_start_is_not_reassigned_to_another_helper(self):
        rows = [record(100, 0x222, a=19, ordinal=1, flags=0),
                record(110, 0x223, a=19, ordinal=2, flags=1 << 8),
                record(120, 0x223, a=19, ordinal=3, flags=0)]
        result = p.durations(rows, 0x222, 0x223, True)
        self.assertEqual(result['count'], 1)
        self.assertEqual(result['max_us'], 20)
        self.assertEqual(result['unmatched_or_ambiguous'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
