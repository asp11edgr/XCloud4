#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Deterministic SYNTHETIC reader fixtures; never console measurements."""
import hashlib
import importlib.util
import json
from pathlib import Path
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


if __name__ == '__main__':
    unittest.main(verbosity=2)
