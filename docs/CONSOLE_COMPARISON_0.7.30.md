# XCloud4 0.7.30 — observed BASE → NEW → BASE comparison

**All three sequential runs completed: A1, B and A2.** BASE uses consumer-lock ingress; NEW uses the MPSC candidate. Both variants requested **960 × 540 / maxFPS 30**; their first native images were **1280 × 720, pitch 1280**. All used four copy readers and passed a one-time **1,382,400-byte** first-picture comparison, which does not validate subsequent pictures.

The owner reported BASE as more stuttered and NEW as much better, with almost no stutters. The owner subsequently confirmed that **audio and controls worked well and similarly in both variants**. Audio/control equivalence is subjective; input latency and individual buttons were not independently measured. Comparable activity was requested, but scene, server, cadence and network equivalence remain unverified. This is an uncontrolled comparison.

## Whole-session ingress and recovery

Final quiescent counters include startup and closing. Their lifetime differs from presentation exposure; do not divide them by that exposure for time rates.

| Whole-session counter | BASE A1 | NEW B | BASE A2 |
| --- | ---: | ---: | ---: |
| Video callbacks | 117,710 | 114,149 | 110,900 |
| Consumer-lock / contended rejections | 231 | 0 | 202 |
| Queue-full rejections | 0 | 0 | 0 |
| Ingress resets | 207 | 0 | 186 |
| Started / completed recovery episodes | 129 / 129 | 1 / 1 | 121 / 120 |
| Completed recovery duration | 31.627620 s | 0.240197 s | 27.922536 s |

Baseline contention rejections were attributed to the consumer lock. Callback peak was **1**, with zero overlap; arbitrary multi-producer behavior remains unverified. Recovery ends when a recovery AU is successfully fed, not when a fresh image reaches the display. Its cumulative duration is not visible-pause time or RTT. A2 had **one uncompleted episode with unknown duration**; initial waiting events can exist without a started recovery timer.

## Detected presentation gaps

NEW here means a successful fresh-image flip match. Exposure is the endpoint span from its first to last match, not reconstructed active-play time.

| Presentation measure | BASE A1 | NEW B | BASE A2 |
| --- | ---: | ---: | ---: |
| First-to-last NEW exposure | 219.354041 s | 222.923990 s | 201.552959 s |
| Exact recorded closed intervals >100 ms | 115 | 3 | 102 |
| Long intervals per exposure minute | 31.456 | 0.807 | 30.364 |
| Retained distinct gap details | 12 | 3 | 14 |
| Saved detail windows | 12 | 3 | 12 |

Rate is `interval count × 60 / exposure seconds`. Repeating BASE reproduced the high count, strengthening the observed association with the queue variant. It does not assign every pause to contention or establish a universal performance effect.

Coverage differs: A1/B/A2 record **8,131/8,642/7,920 diagnostic admission drops**, **2/16/9 unstable snapshots**, and **103/0/88 storage-cap exhaustions**. B has all three closed-gap durations, with one time-capped detail window; A2 has 14 distinct details across 12 saved windows. Baseline duration percentiles/totals are unavailable for all 115/102 gaps. Admission drops omit diagnostic records, not media packets. Rolling-history overwrites, monitor-gate skips and incomplete cadence rearm/disarm history prevent absence claims or reconstruction of every stage.

## Complete reporting-window measurements

These coherent five-row selections exclude first-picture byte-check transitions and malformed windows. They differ from whole-session counters and presentation exposure.

| Selected-window measure | BASE A1 | NEW B | BASE A2 |
| --- | ---: | ---: | ---: |
| Windows / duration | 42 / 210.129964 s | 42 / 210.175035 s | 39 / 195.199987 s |
| New image draws/second | 28.078 | 31.455 | 27.997 |
| Native-copy wall mean | 9.265 ms | 9.253 ms | 9.257 ms |
| RGB-conversion mean | 3.043 ms | 3.037 ms | 3.039 ms |
| Native Decode-call mean | 10.522 ms | 10.901 ms | 10.824 ms |
| Local ingress arrival-to-pop mean | 3.437 ms | 1.863 ms | 3.456 ms |

Means use actual call counts; rates use each selection's duration. Copy cost stayed nearly unchanged. Reader spans/wait overlap and cannot be added as wall time. Local new-draw cadence is not provider FPS or guaranteed smoothness.

Callback timing excludes its final diagnostic-record write; whole-session means are **5.606/5.685/5.624 µs**. Histogram p50 and p95 are each only bounded to **5–8 µs** in every run; those bucket limits are not exact percentiles. Retained-trace timing distributions have different, omission-biased populations. There is no demonstrated callback-speed improvement.

## The three remaining B gaps

- **1.886038 s:** indeterminate. Older-image flips continued; the closing RGB publication preceded its NEW match by **6.979 ms**. This does not attribute the preceding interval to rendering, network silence or the server.
- **100.006 ms:** mixed pipeline progress, only 6 µs above threshold. A **49.065 ms Decode wall span**, including possible scheduling, occurred; it does not explain the entire interval or isolate native decoder work.
- **366.998 ms:** a depth-32 reorder trigger skipped nine sequence positions, followed by a **240.194 ms** recovery-feed wait. This positively identifies a recovery subinterval, not network loss, wire-level PLI delivery or RTT.

Omissions prevent negative stage claims. A proposed next diagnostic uses independent bounded 100 ms arrival/last-RX gauges and owner/main operation state; it is **not implemented**. No PTS age, physical scanout or end-to-end control/video latency is measured.

## Evidence and closure

Trace hashes were independently rechecked. Ordinal deduplication retained **65,897/18,329/66,744** unique A1/B/A2 records; A2 removed **2,301** overlapping instances, with no conflicting ordinals. Coalesced reset totals reconcile with final counters and are not counted twice. Original evidence remains private.

All sessions report cleanup **HTTP 200 / error 0** and three successful helper joins. The owner confirmed A2's menu return; an independent no-CE screen check is unavailable. **NEW is restored:** two installed-package reads each matched the **8,912,896-byte** candidate and SHA-256 `bd2691228964a1dc52d207bb2feb86f80232a02c6e5fea156e875ac07fa5e01d`. The measurements and owner feedback support improved continuity in NEW, with remaining gaps and uncontrolled conditions. Runtime and existing release assets remain unchanged.
