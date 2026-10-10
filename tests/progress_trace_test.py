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
