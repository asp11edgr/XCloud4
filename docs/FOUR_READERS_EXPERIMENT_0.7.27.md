# Four-reader native-copy experiment for 0.7.27

## Status and question

The owner authorized a trial with **the existing video owner plus three persistent copy helpers**. Product **0.7.27**, PS4 **APP_VER 00.97**, is built: fresh selected-source reviews found no material defect, native build/linked inspection passed, and retrieved VM/PC artifacts and all 70 frozen build inputs match. Independent selected compiled inspection passed for **47 disassembly entries / 43,737 matching bytes**, without a blocking discrepancy in that scope. Console transfer, installation, actual four-reader mode, first-picture equality, playback performance and clean closure/export remain **pending**. Verified artifact sizes/checksums and the source-review caveat appear in the [release notes](RELEASE_NOTES_0.7.27.md); no measured improvement is claimed.

The question is whether four bounded reads of the same leased native picture reduce copy wall time without worsening queue residence, recovery, presentation gaps or closure. Shared memory bandwidth, cache traffic and scheduling may limit or reverse improvement. No twofold/fourfold speedup or stable source-FPS result is assumed.

## Two-reader reference

The completed [0.7.26 report](VIDEO_REPORT_0.7.26.md) selects **82 full intervals / 410.310 seconds**: **16.016 ms mean copy**, **26.095 new draws/s**, **484.049 ms maximum local queue residence** and continuing recovery activity. Delivered video is **1280 × 720**, despite the retained **960 × 540 / 30 FPS / 5000 kbps** request. Requested parameters do not establish actual source cadence or delivered resolution.

Its trace counts **96 exact NEW-match intervals over 100 ms**, with detail for only **13** in **12** saved windows. All 13 detailed intervals have retained recovery activity; coverage omissions prevent assigning a cause to every interval. This is the reference observation, not a controlled A/B benchmark.

## Bounded spans and ownership

- The video worker remains the sole owner of decoder start/feed/Decode, pending native pictures, RGB conversion/publication and native teardown. Copy helpers access only their immutable job descriptors and bounded source/destination spans; they never call Videodec2 or manipulate RGB/mailbox state.
- Three helper descriptors remain embedded and stable for their complete thread lifetime. Each has its own submitted/completed sequence, result, stop flag and elapsed-time accounting. At most one job per helper is outstanding.
- For exact validated NV12 length `length`, let `q = floor(floor(length / 64) / 4)`. The owner copies `[0, 64q)`; helpers copy `[64q, 128q)`, `[128q, 192q)` and `[192q, length)`. All spans are contiguous and disjoint; the last span retains the bounded tail. Every span must be safely eligible before dispatch.
- Native source-base/reservation validation, accepted dimensions/pitch and cached staging bounds remain. The existing SIMD copy routines and CPU fences remain; those fences do not introduce a new GPU-completion guarantee.
- Check that **all helpers are idle before publishing any job**. Publish immutable job fields before release submission. After dispatch the owner copies its span and waits for **all three matching completions**, even if one reports failure, before conversion, reference comparison, fallback, storage reuse or an error return that would otherwise release the source.
- Helper completion publishes result/time before release. Diagnostic helper-END observations use local job IDs; their record may appear after the owner has observed completion. Trace record order is not assumed to be completion order.
- A failed dispatched job is a fatal video error with the pending picture protected. Existing feed logic must return before Decode/reservation reuse when forced preservation fails or leaves a picture pending.

The intended mode is **four readers or serial copying**. No silent two-/three-reader mode is introduced.

## Partial creation and shutdown

If any helper cannot be created, publish stop to every successfully created helper and attempt to join **all** of them. Serial fallback is permitted only when every created helper has been successfully joined. Clear a helper's running state only after successful join. Any failed join is latched, retains the complete context/storage and aborts start; it cannot be converted into safe serial fallback.

Normal stop likewise signals **all helpers before joining**, attempts every needed join despite an earlier failure, and retains the context if any join fails. Native decoder deletion, output unmapping, staging release and diagnostic export/free occur only after the required producer/helper joins. Join duration, stack consumption and console scheduling remain unmeasured until the trial.

## First-valid-picture check

Preserve the one-time comparison against the existing fenced serial routine for the first valid native picture of each decoder start. Allocate a bounded reference before dispatch; finish every helper before serial reference/comparison. No Decode or native output reuse occurs during the check.

A byte match confirms only that one leased picture. A mismatch uses the completed serial reference and disables parallel copying for that start. An ineligible picture, unavailable reference allocation or safely completed partial-create cleanup keeps the start serial. A dispatched job failure or failed join remains an error with protected ownership, not an unchecked serial continuation.

Retain separate check result/reason/bytes/time counters and exclude the marked `check_window=1` from sustained comparisons. Equality and check cost for **0.7.27 are pending**.

## Preserved baseline

Keep the **16 ms / four Decode checks between operations**, native type 3/four output reservations, two inputs, SIMD conversion, triple RGB mailbox, display pacing, ring/reorder limits, AU/reset/keyframe policy, audio, controls, provider protocol and requested profile. The diagnostic design remains the same except for reader metadata and helper-index identity. The PS4 UI remains Spanish. No new queue growth, compressed-AU discard policy, GPU-copy API or Better xCloud option is part of this experiment.

The 16 ms budget is checked between operations; a native Decode, copy or conversion can exceed it. More readers do not change that contract.

## Measurement semantics and comparison

| Observation | Meaning |
| --- | --- |
| Configured reader count | The trace header's **4** describes the requested/configured pool, not proof that every picture used four readers |
| `COPY_CONFIG` (`0x22f`) | `a` requests four; `b` reports available/selected four or serial one. Flags: **0** full pool available, initially check pending or later eligibility restored after a successful check; **1** safely joined partial-creation fallback; **2** ineligible spans/reference unavailable; **3** mismatch or job failure. Flag 0 alone does not identify a new check or prove the per-picture copy path |
| Per-picture `COPY_END` path flag | Identifies the actual selected parallel or serial copy; a configured pool alone does not establish this |
| `copy_us` / `copy_max_us` | Selected-copy wall time covering dispatch, owner work and waiting for all completions |
| `owner_us` | Owner span time, or complete serial-copy time |
| `helper_us` | **Sum** of helper span durations; the three helper durations overlap each other and owner/wall time |
| Per-helper retained BEGIN/END observations | Inspect each helper independently through existing numeric trace events for imbalance and scheduling delay; public timing fields retain their names |
| `wait_us` / `wait_max_us` | Owner wait after its span until every matching completion; not queue or end-to-end latency |
| Check counters/time | Separate one-time serial-reference comparison, excluded from steady windows |
| Trace helper index | An immutable index occupies the flags' high byte in helper BEGIN/END; the low failure bit remains. It distinguishes helpers sharing a copy identity, not input-AU/output PTS correlation |

The analyzer must accept two- and four-reader traces and distinguish helper indexes without merging separate helper pairs. `COPY_CONFIG` is emitted on transitions rather than printed for every picture. A job-failure transition is not safe serial fallback; pending ownership and fatal-error rules still apply.

Do not add `owner_us`, the helper-duration sum and wait to reconstruct wall time. Record actual mode, geometry, check outcome, copy wall/spans/wait, new draws, queue residence/overflow, reset/recovery totals, exact NEW-match gaps and coverage, plus clean closure/export.

1. Preserve **0.7.26** as the instrumented two-reader reference and keep its installer on the PC.
2. After actual source/build/artifact checks, capture **0.7.27** with comparable game activity and duration. Keep warmup/check/partial-tail observations separate from coherent sustained windows.
3. When a repeat is authorized, return to the preserved **0.7.26** for an A/B/A comparison using the same definitions. Do not move its historical tag or replace its assets.

Cloud sessions, activity, delivered cadence and congestion can differ. Lower copy time alone is insufficient if recovery, gaps, queueing or closure worsen. Retained trace distributions are conditional samples, not whole-session or physical-scanout statistics.

## Research provenance and pending evidence

The [pinned client comparison](CLIENT_VIDEO_COMPARISON_0.7.24.md) and [Moonlight PS4 parallel-copy source](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c#L198-L224) support the concept of bounded persistent copy workers. Moonlight's author's scaling comment is not a result for this console. Its application-license evidence remains unresolved at that pin; its pool and synchronization implementation are not copied. XCloud4 retains its original project code and GPL-3.0-only notices.

Fresh actual **Claude Opus 5.5** selected-source review completed **21 turns / 20 attempted Read calls / 19 successful files** without finding a material defect. One mistyped evidence-path Read failed without returning contents; the correct scoped copy was subsequently read. The review retains that explicit caveat. Independent selected-source and selected compiled inspection passed in their documented scope. Native build/linked inspection and matching retrieved artifacts/source inventory are confirmed; all console outcomes remain pending. The existing one-helper media-header comment is a deferred comment followup, not the implemented three-helper contract. Earlier reviews and 0.7.26 observations do not verify this candidate's runtime result. See the [0.7.27 release evidence and limitations](RELEASE_NOTES_0.7.27.md).
