# XCloud4 0.7.25 — initial console video observations

The first console attempt confirms that the two-reader copy helper runs and its first-picture byte comparison passes. In the first **12 complete reporting intervals / 60.111 seconds**, selected-copy wall time averaged **16.033 ms**, with **27.749 new image draws per second**. Pauses remain: the owner reports longer playback stretches between them, and the local counters still contain recovery events and an image-completion gap of **500.040 ms**. The session remains open; closure has not been checked.

## Evidence selection

The capture identifies version **0.7.25**, successful helper creation with **two readers**, and the existing video owner with **16,000 µs / four Decode** checks. The first native image is **1280 × 720, pitch 1280**; the retained request for 960 × 540 does not establish that delivered resolution.

The immutable initial selection pairs each `video perf`, `video render`, `video worker` and `video copy` report. It includes the first 12 complete intervals after the first image, excluding the transition interval containing **`check_window=1`**. Initial connection/empty intervals are also excluded. A private numeric snapshot preserves the original line numbers and prefix/snapshot hashes; raw authentication and session data are not published. Later capture data is outside this fixed report.

The first-picture check reports **result 1, reason 0**, comparing **1,382,400 bytes** against the existing serial copy while the same native picture remains leased. Its recorded check cost is **35.281 ms**. This confirms equality for that one buffer; it does not validate every subsequent picture or establish GPU coherence. Its reporting window is excluded from the steady timing selection.

## First sustained selection

| Local observation | Result |
| --- | ---: |
| Matched complete intervals / duration | 12 / 60.111 s |
| Selected copies / completed publications | 1,669 / 1,669 |
| New image draws / repeated draws | 1,668 / 0 |
| Completed publications per second | 27.765 |
| New image draws per second | 27.749 |
| Copy path: parallel / serial | 1,669 / 0 |
| Selected-copy wall time: mean / maximum | 16.033 / 17.960 ms |
| Owner span / helper span: mean per parallel copy | 14.764 / 14.720 ms |
| Owner completion wait: mean / maximum | 1.267 / 3.088 ms |
| RGB conversion: mean per converted image | 3.030 ms |
| Native Decode: mean per call | 7.700 ms |
| Native Decode calls | 3,451 |
| Local ingress-packet residence: mean / maximum | 93.335 / 390.246 ms |
| Maximum local image-completion gap | 500.040 ms |
| Ring high-water / depth at last report | 194 / 9 packets |
| Forced pending-picture preservation calls | 0 |

Copy wall time covers dispatch, both spans and completion wait. Owner/helper elapsed times overlap and must not be added to reconstruct wall time. The one-time serial-reference comparison is reported separately. Publication and new-draw counts can differ by one at reporting boundaries.

The selection records **0 ring-overflow notifications**, **15 dropped-packet events**, **21 lost-packet events**, **164 damaged/rejected-AU events** and **92 local PLI request notifications**. These are client counters: `damaged` counts AU rejection/reset events rather than visibly damaged images, and `pli` counts consumed local requests rather than actual feedback dispatches. The counters alone cannot assign the remaining pauses to network loss, reorder resets, source cadence or any one processing stage.

## Descriptive comparison with 0.7.24

| Local metric | Completed 0.7.24 attempt | Initial 0.7.25 selection |
| --- | ---: | ---: |
| Complete intervals / duration | 134 / 670.473 s | 12 / 60.111 s |
| Copy wall time: mean | 29.382 ms | 16.033 ms |
| RGB conversion: mean | 3.003 ms | 3.030 ms |
| New image draws per second | 17.886 | 27.749 |
| Local ingress-packet residence: mean | 198.154 ms | 93.335 ms |

The [0.7.24 report](ERROR_REPORT_0.7.24.md) and this selection come from **different cloud attempts with unequal duration and uncontrolled activity**. This is descriptive evidence, not a controlled A/B or A/B/A result. A causal speedup ratio, stable 30/60 FPS result or pause elimination cannot be concluded from these samples.

The owner reports improvement with more time between pauses and continued play. That subjective feedback is recorded separately from the numeric prefix. Source FPS, media PTS age, capture-to-display delay, Xbox RTT and end-to-end controller latency remain unmeasured. Aggregate intervals do not provide p50/p95 timing distributions. No restart or close marker occurs within the frozen initial prefix; safe closure and CE-dialog absence remain unverified.

## Review and artifact state

The [release notes](RELEASE_NOTES_0.7.25.md) separate the actual final **Claude Opus 5.5 scoped PASS**, independent static/compiled inspection, native build and package-transfer correspondence from these console observations. Antigravity completed the plan review; its final source-review attempts were quota-blocked with zero reads and provide no final PASS.

The package matches the VM, PC and PS4 readback and was observed running as 0.7.25. The complete PC dependency archive was reconstructed after the original VM compressed transfer was interrupted. All **9,369 source payloads / 150,890,220 bytes** and the recovered VM manifest match; the archive contains **9,781 entries**. Repacking produces a different compressed checksum from the original VM archive. The reconstructed public archive is **83,475,236 bytes**, SHA-256 **`bcade0b5d4b5afc3141d8cd39b8164b5f2d0fb8eb558198190645411f4199f4b`**. It contains dependency sources; the full application source remains in the repository. Separate independent full-archive verification passed for every source payload, the recovered VM metadata, selected project sources and licenses.

Longer observation, comparable repeated sessions and safe closure remain the next evidence to collect. Runtime parameters and source are unchanged during this capture.
