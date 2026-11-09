# SPDX-License-Identifier: GPL-3.0-only
"""Small source oracle for unchanged legacy audio and real JSON smoke output."""
from pathlib import Path
import hashlib
import json
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/audio/live_audio.c").read_text(encoding="utf-8")

def function(name):
    match = re.search(r"^(?:static )?[^\n]*\b" + re.escape(name) + r"\([^\n]*\)\n\{", SOURCE, re.M)
    if not match:
        raise AssertionError(f"missing {name}")
    start = match.start()
    end = SOURCE.index("{", start) + 1
    depth = 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]

class AudioSourceTest(unittest.TestCase):
    def test_previous_worker_exact_t40b(self):
        # Pin the verified base. It already services the RTP receive epoch in
        # play(), which is the difference from the original 0.7.47 digest.
        expected = {
            "append_pcm": "d7906e7807066b825b83fabd386a488fc8c39796b2a1a4b2822eb62d59156f7b",
            "play": "0a2afce6dead13864c7412463646e199105f4fff7a29ba7754272f0d89661786",
        }
        for name, digest in expected.items():
            self.assertEqual(hashlib.sha256(function(name).encode()).hexdigest(), digest)
        self.assertIn("x4_live_audio_start_mode(error, payload_type, true)", function("x4_live_audio_start"))
        self.assertIn("previous ? play : play_new", function("x4_live_audio_start_mode"))

    def test_reset_only_at_track_change_new_mode(self):
        worker = function("play_new")
        self.assertEqual(worker.count("OPUS_RESET_STATE"), 1)
        reset = worker.index("OPUS_RESET_STATE")
        self.assertIn("X4_LIVE_TRACK_NEW", worker[:reset])
        self.assertNotIn("OPUS_RESET_STATE", function("new_conceal"))
        self.assertNotIn("OPUS_RESET_STATE", function("new_decode_packet"))

    def test_snapshot_does_not_read_worker_queue(self):
        snapshot = function("x4_live_audio_snapshot")
        self.assertNotIn("pcm_count", snapshot)
        self.assertNotIn("pcm_head", snapshot)
        self.assertIn("atomic_load", snapshot)

    def test_valley_only_after_consumption(self):
        self.assertNotIn("new_level(", function("play_new"))
        render = function("new_render")
        self.assertEqual(render.count("new_level(a, now)"), 1)
        self.assertLess(render.index("a->pcm_count -= real"), render.index("new_level(a, now)"))
        self.assertNotIn("high_pending", SOURCE)
        self.assertIn("new_valley_reset(a)", function("play_new"))

    def test_real_json_when_supplied(self):
        if len(sys.argv) < 2 or not Path(sys.argv[1]).is_file():
            self.skipTest("runtime output supplied by the focused C runner")
        records = [json.loads(line.split("=", 1)[1]) for line in Path(sys.argv[1]).read_text().splitlines()
                   if line.startswith("AUDIO_METRICS_JSON=")]
        self.assertEqual(len(records), 1)
        record = records[0]
        self.assertEqual(record["mode"], "new")
        self.assertTrue(record["fec_implemented"])
        self.assertEqual(record["fec_packets"], 0)
        self.assertEqual(record["pcm_overflows"], 1)
        self.assertEqual(record["underflow_intervals_ms"]["mean"], 20)
        self.assertEqual(record["buffer_ms"]["max"], 100)
        self.assertEqual(record["buffer_ms"]["scope"], "post_render_valley_before_trim_including_PLC")
        self.assertEqual(record["valley_ms"]["window_ms"], 1000)
        self.assertEqual(record["target_ms"]["p50"], 50)
        self.assertEqual(record["target_ms"]["max"], 60)
        self.assertEqual(record["target_ms"]["adjustments"], 1)
        self.assertEqual(record["output_block_samples"], 512)
        self.assertEqual(record["audio_out_open"]["rc_512"], 7)
        self.assertFalse(record["audio_out_open"]["attempted_1024"])

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
