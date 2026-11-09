#!/usr/bin/env python3
"""Check the production diagnostic JSON, without claiming physical scanout."""
import json
import sys
from pathlib import Path

d = json.loads(Path(sys.argv[1]).read_text())
p = d["presentation_pacing"]
assert d["vsync_policy"]["requested"] == 3
assert d["vsync_policy"]["initial_vsync"] is True
assert d["vsync_policy"]["effective_vsync_at_end"] is True
assert d["vsync_policy"]["transitions_total"] == 0
assert set(d["presentation_modes"]) == {"HSYNC", "VSYNC_Cola", "VSYNC_Mailbox", "VSYNC_Synced"}
assert p["ready_to_flip_us"]["count"] == 5
assert p["ready_to_flip_us"]["p50_upper_us"] == 2100
assert p["ready_to_flip_us"]["p95_upper_us"] == 40100
assert p["ready_histogram_quantum_us"] == 1000
assert p["ready_histogram_overflow_lower_us"] == 31000
assert len(p["ready_histogram_counts"]) == 32
assert sum(p["ready_histogram_counts"]) == 5
assert p["ready_histogram_counts"][31] == 1
assert p["ready_invalid"] == 1
assert p["shown_images"] == 6 and p["received_complete_AUs"] == 480
assert p["shown_per_received_AU"] == 0.0125
assert p["shown_by_mode"] == [1, 1, 0, 4]
assert p["intervals_over_20ms"] == 1
assert p["native_ptime_samples"] == 5 and p["observation_fallback_samples"] == 1
assert p["vblank_repeats"]["estimated"] == 1
assert p["vblank_repeats"]["validated_intervals"] == 3
assert "not an exact hardware vblank counter" in p["vblank_repeats"]["scope"]
q = p["queue_depths"]
assert q["samples"] == 4 and q["invalid_driver_samples"] == 1
assert q["own_ready_max"] == q["driver_pending_max"] == q["driver_gpu_pending_max"] == 99
assert q["own_ready_counts"] == [1, 1, 1, 0, 1]
assert sum(q["driver_pending_counts"]) == sum(q["driver_gpu_pending_counts"]) == 3
assert p["drops"] == dict(recovery=1, mailbox=2, ring_full=3, ring_latest=4)
assert p["cola_vsync_wait_drop"] == 2
assert "does not infer sole cause" in p["cola_vsync_wait_scope"]
print("PASS production JSON: finite percentiles, 1ms overflow, separate queue depths, actual frames/AUs and explicit repeat estimate")
