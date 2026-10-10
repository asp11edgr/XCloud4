# XCloud4 0.7.31 — first long console diagnostic run

## Result and interpretation

On **2026-10-10**, the exact 0.7.31 package was exercised on the owner's PS4. Both console exports were retrieved twice, preserved privately and hash-verified. The owner confirms returning to the catalog and closing with OPTIONS, and reports almost no pauses with longer periods between them. Audio and controller behavior were not separately characterized in that final answer; a no-error screen was not independently inspected.

The complete cadence ledger contains **30 closed intervals greater than 100 ms during 27.110969 minutes of manually marked stable gameplay: 1.106563/min**. Those are intervals between local matches of *new* presented RGB generations, not total latency, provider FPS, control-to-image latency or physical scanout times. This was longer than the requested 10–15 minutes and contains two stable epochs separated by a manual transition.

The strongest sampled investigation lead is **before Decode, around reordering/AU assembly/recovery**. For 16 stable pauses, the first retained no-progress span has new admitted packets but no newly valid complete AU. One first span has no callback progress; 13 are indeterminate at that first-stage resolution. Positive receipt of some packets does not establish receipt of every packet required for a complete picture. A causal repair is not yet established.

**The functional NEW/MPSC 0.7.30 queue, four copy readers, reorder settings and recovery decisions remain preserved. No performance optimization was implemented in this diagnostic run.**

## Binary and actual settings

- Product: **0.7.31**; SFO APP_VER/VERSION **01.01**.
- Runtime input identity: `0caa54f34a994de1ffed125d27e47767062384b2`. This is an input identity, not the Git commit or package hash.
- Source checkpoint: `c1bacf372a139cdc8d4f136d7c609761be532856`.
- Package: **8,978,432 bytes**, SHA-256 `ba4a3ba4c0d29cdcfccc47338a3eb1cab08e06d7b1e98e19f673d55dc0182b4d`.
- Requested video: **960 × 540, maxFPS 30, 5000 kbps**.
- All **140 retained native-output geometry observations are 1280 × 720, pitch 1280**. This is the delivered resolution observed in the retained records, not a proven provider frame-rate setting.
- Four copy readers; worker budget **16 ms / four Decode calls**; ingress **MPSC / 256**; reorder **128 slots / depth 32 / 25 ms**.

The exact package, source archives and dependency archives remain unchanged in the [0.7.31 release](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.31). This later report does not replace those assets.

## Exposure and presentation cadence

| Population | Exposure | Closed intervals >100 ms | Per minute | Denominator |
|---|---:|---:|---:|---|
| Loading, active | 40.890005 s | 1 | 1.467351 | Manual active epoch |
| Stable, combined | 1626.658140 s | 30 | 1.106563 | Manual active epoch |
| Transition, active | 79.999874 s | 0 | 0 | Manual active epoch |
| Entire first-to-last NEW | 1747.520055 s | 31 | 1.064365 | NEW endpoints |

The first stable epoch is **187.589041 s / 2 long intervals**. The corrected stable mark 3 epoch is **1439.069099 s / 28 long intervals**. Three phase-crossing cadence intervals are excluded from phase-specific populations. The full cadence ledger and manual phase ledger have no omitted entries or identity errors.

| Interval population | Count | p50 | p95 | Maximum |
|---|---:|---:|---:|---:|
| Stable, non-crossing NEW intervals | 49,554 | 33.036 ms | 49.952 ms | 1250.940 ms |
| All NEW intervals | 53,550 | 33.033 ms | 49.958 ms | 1300.034 ms |

Quantiles describe intervals between images; they are not quantiles of end-to-end delay. The 1300.034 ms maximum belongs to loading. Marking stable is a manual observation and does not prove the absence of an unmarked in-game loading screen.

## Chronology of every closed interval greater than 100 ms

Times below are relative to the **first NEW match**, including loading and transition. A lead is the first *retained sampled* no-progress observation, not proof of the first physical cause. Detailed records are incomplete; an earlier unrecorded stage may differ. With a 100 ms sampler, one interior row alone usually cannot establish a before/after progression.

| # | Start from first NEW | Interval ms | Epoch | Interior sampler rows | First retained lead |
|---:|---:|---:|---|---:|---|
| 1 | 00:29.781 | 1300.034 | loading | 0 | U: insufficient retained evidence for a first-stage lead |
| 2 | 01:37.482 | 116.949 | stable | 0 | U: insufficient retained evidence for a first-stage lead |
| 3 | 03:43.123 | 102.042 | stable | 0 | U: insufficient retained evidence for a first-stage lead |
| 4 | 06:27.456 | 450.015 | stable | 0 | U: insufficient retained evidence for a first-stage lead |
| 5 | 06:47.525 | 401.051 | stable | 0 | U: insufficient retained evidence for a first-stage lead |
| 6 | 07:02.407 | 199.962 | stable | 0 | U: insufficient retained evidence for a first-stage lead |
| 7 | 07:03.508 | 234.026 | stable | 0 | U: insufficient retained evidence for a first-stage lead |
| 8 | 09:27.270 | 334.040 | stable | 3 | A: RX/admission continue; no valid AU progress |
| 9 | 10:08.711 | 299.991 | stable | 3 | A: RX/admission continue; no valid AU progress |
| 10 | 10:36.222 | 100.008 | stable | 1 | U: insufficient retained evidence for a first-stage lead |
| 11 | 11:09.572 | 100.011 | stable | 1 | U: insufficient retained evidence for a first-stage lead |
| 12 | 14:16.760 | 316.026 | stable | 3 | A: RX/admission continue; no valid AU progress |
| 13 | 15:07.561 | 182.994 | stable | 1 | U: insufficient retained evidence for a first-stage lead |
| 14 | 15:10.247 | 1250.940 | stable | 13 | A: RX/admission continue; no valid AU progress |
| 15 | 15:11.549 | 933.043 | stable | 9 | A: RX/admission continue; no valid AU progress |
| 16 | 15:12.617 | 366.021 | stable | 4 | A: RX/admission continue; no valid AU progress |
| 17 | 15:13.401 | 600.008 | stable | 6 | A: RX/admission continue; no valid AU progress |
| 18 | 15:14.085 | 433.011 | stable | 4 | A: RX/admission continue; no valid AU progress |
| 19 | 15:14.935 | 584.981 | stable | 6 | A: RX/admission continue; no valid AU progress |
| 20 | 16:52.867 | 349.984 | stable | 3 | A: RX/admission continue; no valid AU progress |
| 21 | 16:53.951 | 300.956 | stable | 3 | A: RX/admission continue; no valid AU progress |
| 22 | 17:52.893 | 516.982 | stable | 5 | R: no RX callback progress in first retained sample span |
| 23 | 20:48.821 | 232.045 | stable | 3 | A: RX/admission continue; no valid AU progress |
| 24 | 20:49.253 | 350.934 | stable | 3 | A: RX/admission continue; no valid AU progress |
| 25 | 21:04.586 | 367.994 | stable | 4 | A: RX/admission continue; no valid AU progress |
| 26 | 23:34.086 | 250.027 | stable | 2 | A: RX/admission continue; no valid AU progress |
| 27 | 25:46.335 | 101.016 | stable | 1 | U: insufficient retained evidence for a first-stage lead |
| 28 | 26:57.056 | 116.969 | stable | 1 | U: insufficient retained evidence for a first-stage lead |
| 29 | 26:57.189 | 100.114 | stable | 1 | U: insufficient retained evidence for a first-stage lead |
| 30 | 27:49.976 | 368.015 | stable | 3 | A: RX/admission continue; no valid AU progress |
| 31 | 27:50.810 | 118.010 | stable | 1 | U: insufficient retained evidence for a first-stage lead |

### Long stable pauses and missing evidence

**#14, 1250.940 ms, starting at 15:10.247.** Twelve adjacent retained sample comparisons span roughly **+2.019 to +1201.946 ms** of the gap. In those comparisons there are **531 new video callbacks/admissions**, **535 queue pops**, **23 reorder-hole observations**, **50 reset calls** and **48 discarded open AUs**. No valid AU or Decode entry advances during the first eleven comparisons; one AU/Decode entry appears in the last comparison. No valid native output, copy completion or RGB publication advances within those comparisons. Old-image flip activity can continue. The earliest retained span points to missing valid AU progress, with recovery/discard activity, rather than a continuing copy of a native picture. The exact skipped sequence positions, per-reset reasons, recovery start/end and completion-to-NEW chain are unavailable for this late gap; admitted packets alone cannot prove network completeness.

**#15, 933.043 ms, starting at 15:11.549.** Eight retained sample comparisons cover approximately **+99.955 to +899.985 ms**. They record **394 callbacks/admissions**, **394 queue pops**, **17 reorder-hole observations**, **37 reset calls**, **36 discarded AUs** and **four local PLI dispatch attempts**. Again, valid AU/Decode entry does not advance until the last comparison, without native/RGB progress inside the sampled spans. The onset before the first comparison and the final image chain remain unobserved. This is consistent with a period of AU/recovery blockage; it does not locate the source of the sequence gaps or prove the cause of every millisecond.

**#22, 516.982 ms, starting at 17:52.893.** Two adjacent comparisons covering approximately **+56.000 to +255.933 ms** have no new video callback, queue, AU or Decode progress. The next comparisons record reception returning (**217 callbacks over the four retained comparisons**) plus **11 resets / 10 AU discards**, still without a valid AU in those comparisons. The sampler continues. This distinguishes an initial no-callback span from the later AU/recovery lead. Network, server, transport scheduling or a delay before the reception callback remain possible; this is not an Internet-loss diagnosis.

**#1, 1300.034 ms, loading.** The first sample ring rows were overwritten. The legacy detail window positively records RGB/render activity near the end, but has admission omissions. It cannot establish uninterrupted RX silence or an exact first stalled stage.

For the other gaps, the table supplies the complete observed duration and retained stage lead. Precise lengths of individual internal waits are unknown unless matched detail records survived. A gap duration must not be relabeled as a recovery duration or network round trip.

The legacy companion preserves the following **seven matched recovery-feed waits**, all initiated with primary reason `reorder_hole`. These are positive observations, with incomplete surrounding detail coverage; they do not account for every gap or the 15 other recovery episodes.

| Gap # | NEW interval | Matched recovery-feed wait |
|---:|---:|---:|
| 4 | 450.015 ms | 223.334 ms |
| 5 | 401.051 ms | 197.297 ms |
| 6 | 199.962 ms | 151.079 ms |
| 7 | 234.026 ms | 163.817 ms |
| 8 | 334.040 ms | 150.552 ms |
| 9 | 299.991 ms | 138.237 ms |
| 12 | 316.026 ms | 154.745 ms |

Their accepted-feed endpoints are not displayed-IDR endpoints. Their starting reset reason does not establish where required packets became unavailable.

## Cumulative reception, AU and recovery evidence

| Measure | Observed total | Interpretation |
|---|---:|---|
| Video callbacks / admitted ingress packets / queue pops | 927,075 each | Local counters; sequence completeness remains a separate question |
| Queue rejection, full, producer contention and consumer-lock rejection | 0 | MPSC admission rejection is not the observed source of these pauses |
| Valid complete AUs / Decode entries / valid native outputs | 57,132 each | Equal totals do not associate each input AU with an output PTS |
| RGB publications | 53,554 | Some native outputs were superseded before copy |
| New presentation matches | 53,551 | Different stage population from decoder outputs |
| Reorder-hole observations / reset calls with that primary reason | 120 | A sequence position declared absent is not proof of loss on the Internet |
| Recovery episodes / completed recovery episodes | 22 / 22 | Final ingress totals; completion means recovery feed accepted, not necessarily an IDR already presented |
| Summed recovery wait | 6,588,641 µs | Across episodes; individual late-pause allocation unavailable |
| Raw WAIT_BEGIN / WAIT_END counters | 23 / 23 | Includes one startup wait, separate from the 22 recovery episodes |
| Local consumed PLI requests | 205 | Demand, not dispatched messages |
| Actual local PLI dispatch attempts / results | 89 / 89 | Zero local result errors in final sampled cumulative count; no delivery acknowledgment measured |

Non-normal reset calls are **27 marker-incomplete**, **180 waiting-recovery**, **120 reorder-hole**, plus **one new-track** reset. The **57,132 normal-submitted resets** are ordinary AU lifecycle operations, not faults.

Discarded open AUs are a separate population: **27 marker-incomplete + 180 waiting-recovery + 98 reorder-hole = 305**. Some reset calls discard no AU, so these populations must not be added. Retained exit totals also report **80 orphan FU continuation** and **63 discarded-timestamp** damage bits; bits overlap and are not another count of distinct discarded images.

An IDR observation alone does not establish a complete valid AU, accepted decoder output, RGB publication or a NEW presentation. Native output has no returned PTS; timings before and after Decode remain separate.

## Retained operation timings

These are the matching observations in the legacy detail windows, **not distributions of all operations**. Admission/retention omissions may bias them. Wall spans include scheduling; helper and owner spans overlap and must not be added.

| Retained span | Count | p50 | p95 | Maximum |
|---|---:|---:|---:|---:|
| Decode call | 484 | 10.674 ms | 16.508 ms | 53.389 ms |
| Native copy | 432 | 9.357 ms | 9.930 ms | 10.975 ms |
| Conversion | 436 | 3.034 ms | 3.108 ms | 3.180 ms |
| Helper completion wait | 440 | 1.853 ms | 2.448 ms | 3.614 ms |
| Draw | 430 | 5.601 ms | 6.226 ms | 6.416 ms |
| Submitted flip to local matching status | 432 | 9.418 ms | 16.444 ms | 18.150 ms |
| RGB publication to NEW match | 427 | 17.006 ms | 23.998 ms | 34.922 ms |

The legacy reader retains **1192 helper spans**, split **401 / 388 / 403** over three helpers. The secondary reader's helper-span result of zero is a known pairing limitation: the producer encodes the helper index in the upper flag byte while the secondary duration key uses the lower byte. It must not be used to claim the helpers did not run. The frozen parser and all original outputs remain preserved; no parser correction was mixed into this result.

## Real recording coverage

- **Cadence and phase:** all **53,550** intervals and all five ledger boundaries survived; no cadence/clock identity errors and no open gap at stop.
- **Independent samples:** **18,091 executed**, latest **12,000 retained**; the earliest **6091 rows were overwritten** by the circular 20-minute capacity. This is not 6091 missed executions. Cumulative missed target periods: **zero**. Retained actual cadence p50 **100.000 ms**, p95 **100.057 ms**, max **101.020 ms**; maximum target lateness **1.983 ms**. Retained snapshot spans p50 **7 µs**, p95 **9 µs**, max **117 µs**. These numbers are not the total instrumentation cost.
- **Detailed monitor:** **14,240,981 event attempts**, **39,702 admission omissions**, **14,193,109 history overwrites**. All eight detail windows hit their event cap; **45 omitted window capture/allocation attempts** are not 45 distinct pauses. The retained detailed record set is incomplete.
- **Coherence:** cumulative actor snapshot instability **224**, queue snapshot instability **6**, sampler history-detail gate omissions **118**. RX source-cap and sequence-observation omissions are zero. Individual atomic counters do not form one globally simultaneous snapshot.
- **Legacy companion:** **12 retained closed-gap details of 31**, **19 storage exhaustions**, **112,361 admission omissions**, **22,373 monitor gate skips**, **118 unstable snapshots**. Its independent cumulative exit reset totals survive.
- **External console log:** its first bounded socket capture expired at 30 minutes and was reconnected after an approximately eight-minute administrative gap. Both successful export messages were observed after reconnection and both original binary files were verified. Socket logs can replay a retained buffer; their administrative wall-clock times are not assumed to be media timestamps or continuous coverage.
- Sampler thread start and join return codes are **zero**. A responding sampler while video stalls is positive evidence of independent sampling, not proof that every actor snapshot was coherent or that instrumentation had zero overhead.

## Comparison with preserved NEW 0.7.30

The original NEW trace still yields **3 gaps / 222.923990 s = 0.807450/min** using first-to-last NEW exposure. This long 0.7.31 run gives **31 / 1747.520055 s = 1.064365/min** with the same endpoint denominator, or **30 / 1626.658140 s = 1.106563/min** for manually stable exposure. Those populations, scene coverage and durations differ. The owner reports improvement, but this descriptive comparison proves neither a performance improvement nor an instrumentation regression. Stable manual exposure must not be substituted into the old unmarked denominator.

The original queue A/B/A finding remains preserved: BASE **31.46 / 30.36** versus NEW **0.81** gaps/min, and consumer-lock rejections **231 → 0 → 202**, with copy near **9.25 ms**. This run again has zero queue rejection and copy near 9–10 ms. It does not justify revisiting that queue correction or changing the four readers.

The three old NEW residual intervals retain their previous qualifications: **1886.038 ms** indeterminate; **100.006 ms** with a **49.065 ms Decode** wall span that does not explain its entirety; **366.998 ms** with a depth-32 skip of nine positions and **240.194 ms recovery feed wait**. Their observed sequence skips are not proof of Internet packet loss.

## One proposed next change

**Add a dedicated bounded diagnostic ledger for sequence-hole decisions and recovery milestones, independent of the saturated general history/windows.** Record monotonic decision time, trigger, skipped range, depth/age, later observed arrivals for those positions, recovery start/reason, IDR observation, valid complete AU, native output, RGB and NEW milestones as separate observations. Preserve counters and the 100 ms sampler; count the ledger's own omissions and require correspondence with that run's cumulative counters. A native output still must not be attributed to the latest input AU without returned PTS.

The actual general recorder attempts **3,784,555 PHASE_CHANGE** and **2,922,893 REORDER_STATE** records; all eight detail windows saturate and later contexts are omitted. Giving critical events separate bounded retention addresses the demonstrated evidence bottleneck. This is one diagnostic experiment, not a claimed video repair or a proposal to enlarge the gameplay queue.

Then run one clearly marked **10–15 minute** comparable stable scene, rather than extend beyond the sample ring. Require intact reset/recovery/sequence-jump context around the long gaps and recheck cadence, audio, input, catalog return and close. If practical, compare matched diagnostics-on/off sessions with the same functional settings to evaluate overhead. **Do not change reorder depth/timing, recovery, compressed-frame dropping, copy readers or resolution yet.** No single gameplay change is supported strongly enough by these incomplete causal chains.

## Evidence integrity

Raw console traces and JSON analyses contain local source identifiers and remain private. SHA-256 of the exact console originals:

- Legacy binary: `a07b07c77fa4c19eba7a016a8ca53c259a54e8922ab08c11f2fd5d6e8b9d63c0` (**2,570,400 bytes**).
- Independent progress binary: `7a8a381f83326523c0cbd859db5fb9e565b6a55caf6bda77411f44b6dd95281f` (**10,719,616 bytes**).

Both repeated FTP reads, PC originals and parser input hashes match. Runtime/parser/tests, the package and release assets are unchanged by this report. Two independent arithmetic/chronology reviews agree on the complete cadence, phase exposure and interpretation limits. A fresh actual **Claude Opus 5.5** review completed in **109 seconds**, with **seven Read calls covering five sanitized evidence/source files**, matching immutable inputs and no errors or out-of-scope tools. It independently supports the dedicated gap/recovery diagnostic experiment. This review did not run the app, tests or builds and its partial source excerpts do not independently prove every preservation claim. Exploratory pause grouping and distribution-confidence suggestions from that review are not promoted to verified perception metrics here.
