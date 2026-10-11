# XCloud4 0.7.34 — bounded 45 ms NACK grace experiment

## Status and purpose

This is a verified preparation report. Final host campaigns, native dependency/application rebuilds, selected linked inspection, source/archive correspondence and two exact PS4 readbacks of each of the three profiles passed. The [first closed G0/G45 pair](CONSOLE_REPORT_0.7.34_G0_G45.md) is recorded: neither session had a non-crossing stable/loading NEW-image interval over 100 ms or a NACK request. The configured grace policy was not exercised; useful repair or a fluency benefit remains unverified. [Release notes](RELEASE_NOTES_0.7.34.md) record exact artifact hashes and review limits.

The [0.7.33 console report](CONSOLE_REPORT_0.7.33.md) observed authenticated positions returning after the worker had abandoned their holes. This motivates a bounded wait for eligible requested positions. It does not prove that NACK caused those returns, or that extending the wait will improve playback.

The grace change and [rumble experiment](RUMBLE_0.7.34.md) are separate. Keep rumble disabled throughout the grace comparison.

## Profiles and preserved settings

| Profile | `X4_RECEIVER_NACK` | `X4_NACK_GRACE_MS` | `X4_RUMBLE` | Purpose |
|---|---:|---:|---:|---|
| Baseline | 1 | 0 | 0 | Current NACK behavior without added grace |
| Grace | 1 | 45 | 0 | Isolate the bounded worker wait |
| Rumble follow-up | 1 | 45 | 1 | Separate controller-feedback trial after the pair |

The Makefile defaults remain grace 0 and rumble 0; enabling 45 ms requires NACK 1. Command-line flag changes alone do not invalidate existing objects. The delivered profiles used a full native dependency rebuild and forced fresh application builds for each configuration. Their source identity is `54d5793d1b50a7ea24ba73e79b258577f51511c0`, with suffixes `n1-g0-r0`, `n1-g45-r0` and `n1-g45-r1`. Verified package names and checksums are in the release notes.

All three profiles retain MPSC ingress, four copy readers, the existing 16 ms/four-feed worker budget, the 128-slot reorder and existing H.264/Opus/recovery behavior. The stream request remains 960 × 540, maxFPS 30 and 5,000 kbps. Actual delivered dimensions must be measured again. There is no new RTX negotiation, SDP change, resolution experiment, decoder allocation or recovery architecture.

## Worker policy and bounds

The video worker exclusively owns the pure grace state. A request must match the current session, accepted original-video source and sequence epoch. An unsolicited control cannot change that binding.

- An admitted position has an absolute **detection time + 45,000 µs** deadline. Worker delay consumes this budget; it is not a fresh 45 ms after control delivery or transport dispatch.
- HOLD applies only when the current expected position belongs to an admitted pending range. It temporarily supersedes both ordinary depth-32 and 25 ms hole expiry for that position. Unrelated holes retain their ordinary policy.
- Duplicate requests, overlapping requests, advancement of expected, dispatch and authenticated-return observations never renew the deadline.
- **Authenticated reception alone does not clear pending grace.** Only actual STORED admission to the current worker reorder does. A delayed control reconciles matching slots already STORED; authentication completion can retire a send watch without claiming worker admission.
- At most **16 ranges are active**, with **64 request histories** retained for the fresh session. Completed history remains available to prevent overlapping requests from rearming old positions. No unlimited retries or refunded attempt budget is introduced.
- The reorder still has **128 slots**. A prospective or buffered forward offset reaching 128, a full reorder, or unknown headroom releases matching grace before aliasing/window jump. **Capacity can end the hold before 45 ms**, including after only a few milliseconds at the actual packet rate.
- Deadline and capacity releases force the existing hole advance and AU reset/recovery path. Other cancellations disable the special hold and return to ordinary depth/time decisions without accelerating them. No arbitrary compressed P-frame discard or enlarged queue is added.
- Advancement stops before a position still protected by a different active range, including an ordinary ready gap. An abandoned prefix can therefore end without emitting a packet; the next range retains its original deadline.
- Queries also run in the existing worker loop when no new packet arrives. Scheduling and native Decode/copy work can delay evaluation: the absolute deadline governs eligibility, not a new independent timer or guaranteed wall-clock execution exactly at 45 ms. Clock regression retires active ranges; deadline arithmetic overflow rejects admission.

The current integration binds the accepted worker sequence epoch 1. Transport requests after a sequence rebase to epoch 2 or later fail source matching and cannot silently rebind from control. This is a deliberate fail-closed limitation: **grace utility after that rebase is not implemented or claimed**.

The current generator/correlation path is limited to original video PT **102**. A different payload type is not silently treated as supported. A capacity-limited advance can stop at another protected range yet leave an incoming packet outside the unchanged 128-position window; the existing JUMP/reset path can still retire buffered state. Bounded grace does not guarantee that every requested position remains recoverable.

## Functional control inbox and lifetime

The control inbox is independent of optional trace retention: **256 fixed cells**, at most **16 CAS attempts** per producer, one video-worker consumer. A callback publishes numeric state without allocation, sleep, hardware operations or waiting. It cannot bypass an unpublished FIFO head.

The RTC owner binds the borrowed inbox before callback dispatch. Shutdown retires and joins producers before unbinding and freeing storage; failed joins retain the dependent state. An omitted control or unknown bridge delivery permanently disables grace for that session, while NACK observation/dispatch continues. Complete critical-detail retention does not prove complete control-inbox delivery. Only a deadline/capacity release that covered the unresolved expected position forces its existing fallback; an omission must not accelerate an unrelated baseline hole.

## Evidence to retain

Keep detection, reserved/requested mask, selected mask, transport-attempt start, protection result, local return, authenticated arrival, reorder result, grace release and AU contribution as separate stages. The attempt clock is captured before packet construction/transport work. A successful local return is **not server receipt, RTT or proof of retransmission causality**. Later traffic may be delayed original traffic.

Report accepted positions, deadline/capacity/source/transport releases, active/history exhaustion, inbox omissions, hold duration and remaining headroom. Separate worker STORED acceptance from AU structural completion, recovery acceptance and the subsequent Decode feed. Native output, RGB publication and actual NEW presentation need their own observations: the decoder supplies no output PTS that would justify assigning the last input AU to an image.

Retain bounded detail/omission counters alongside cumulative totals. Do not label every pause as a NACK hole or compute complete-population percentiles from capped records. The [receiver NACK evidence contract](RECEIVER_NACK_EXPERIMENT_0.7.33.md) continues to distinguish local feedback outcomes and authenticated returns.

Worker-cost accumulators start when the worker consumes an admitted **OFFER**. They do not cover the entire detection-to-release window or establish a per-packet native-output cost. HOLD, state RELEASE and actual worker ABANDON remain separate observations. Shared production reason code 5 is labeled **`source_or_reorder_reset`**: a reorder JUMP uses the same code, so it does not prove an SSRC change. The corrected 0.7.33 report retains separate attempt-start/local-result clocks; no return interval is a server RTT or causal retransmission proof.

## Controlled console procedure

1. Close the current application. Install verified **G0 / `XCloud4-0.7.34-grace-off.pkg`**, launch freshly, confirm its identity/profile and open one game. Preserve the current working installer and previous exports.
2. After loading, hold **L1 + R1** and press **TRIANGLE** once to mark stable gameplay. Record the game, scene, network arrangement and planned actions; play for **10–15 stable minutes**.
3. Use the same chord to mark each transition/loading interval and then stable play again. Preserve the marks and accompanying notes. Do not retrospectively infer an equivalent scene or uninterrupted stable exposure.
4. Return with **L1 + R1 + CIRCLE**, then exit from the catalog with **OPTIONS**. Retain both numeric exports, hashes, close/export results and coverage counters. Use only one game session per fresh application process.
5. Repeat with **G45 / `XCloud4-0.7.34-grace-45.pkg`**, rumble still 0, using the same game/scene, marking, duration and network arrangement. A fresh G0 repeat can follow if the pair warrants confirmation. Try **G45VIB / `XCloud4-0.7.34-grace-45-rumble.pkg`** only as a separate follow-up after this pair.

Compare closed actual NEW-image intervals strictly >100 ms per stable minute, cadence distribution, recovery episodes/duration, AU discards and positively observed AU utility. Report separate exposure/coverage for each pass. Collect distinct owner observations for video, audio, controls and clean return/exit. Changing service/server/network conditions prevent one pair from establishing causality.

If grace worsens stable playback or introduces an audio/input/closure regression, stop and retain the evidence. Roll back to **Baseline: NACK 1, grace 0, rumble 0** with a fresh launch. A rumble issue separately rolls back to rumble 0; neither rollback requires changing SDP, readers or recovery.

## Verification boundary

| Check | Current evidence |
|---|---|
| Pure grace state | Final 23 cases / 531,181 checks passed in GCC and Clang host campaigns |
| Shared portable reorder plus grace | Final 19 cases / 420 checks passed; these use shared reorder code, not the complete native worker |
| Functional inbox | Final seven groups passed, including stale cursor and full-width wrap |
| Rumble module | Final 13 cases passed, separate from grace |
| GCC sanitizer scope | ASan/UBSan passed for the new harnesses; host behavior is not PS4 proof |
| Clang sanitizers | Unavailable because required runtime libraries are missing; no sanitizer pass claimed |
| Overall host campaign | Ten harnesses exercised with both GCC and Clang |
| Reader | 127 checks passed in the native-build campaign; subsequent analyzer-only labeling correction passed 128 checks |
| Native application/profile inspection | Full dependency rebuild and three fresh profiles passed, including identities and Pad import |
| Package/source verification | Three package hashes and two PS4 readbacks each matched; all 9,385 dependency-archive payload hashes matched |
| Fresh Baseline/Grace console comparison | Closed G0/G45 pair: zero stable intervals over 100 ms in both; repair policy unexercised |

Two actual Claude Opus 5.5 immutable read-only reviews were completed. The second read 28 selected files and confirmed the four runtime corrections: release-specific forcing, fresh rumble poll time, stale inbox cursor handling and preservation of other pending ranges. It found no new runtime-safety issue in that selected scope. A wrong-root `candidate.diff` read failed despite the diff being present; neither review is described as a complete diff review. Tests/builds were executed separately from Claude.

There is no full native-worker end-to-end fixture or real server-acknowledgment test. Runtime source remains frozen; the later analyzer-only label/test update and this prose update do not alter the built packages. Host results do not validate native scheduling, callback contention, physical rumble or performance.

## Subsequent console evidence

The [first G0/G45 pair](CONSOLE_REPORT_0.7.34_G0_G45.md) encountered no observed sequence gaps, so repair was unexercised. A separate [G45 Gears-labeled pass](CONSOLE_REPORT_0.7.34_G45_GEARS.md) exercised seven requests and two holds: one startup range expired when the worker next evaluated its deadline; one stable range released on bounded capacity before 45 ms. The remaining five offers were stale after a local observer rebase. No stable useful repair or displayed repaired picture is demonstrated. The single next proposal is a validated same-session/source sequence-rebase hand-off with extended-origin verification, retiring old grace state and rejecting stale controls. No runtime adjustment accompanies that evidence report; all existing parameters and installers remain unchanged.
