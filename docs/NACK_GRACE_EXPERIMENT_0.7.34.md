# XCloud4 0.7.34 â€” bounded 45 ms NACK grace experiment

## Status and purpose

This is a preparation report. **Native builds, packaged-profile verification and the 0.7.34 console comparison are pending.** Preliminary portable checks cover the contracts listed below; they do not establish useful repair or fewer pauses on PS4.

The [0.7.33 console report](CONSOLE_REPORT_0.7.33.md) observed authenticated positions returning after the worker had abandoned their holes. This motivates a bounded wait for eligible requested positions. It does not prove that NACK caused those returns, or that extending the wait will improve playback.

This intermediate source change contains only the bounded grace experiment. Its index tree has not separately been compiled or executed; verification of the complete later candidate must not be attributed automatically to this intermediate tree.

## Profiles and preserved settings

| Profile | `X4_RECEIVER_NACK` | `X4_NACK_GRACE_MS` | Purpose |
|---|---:|---:|---|
| Baseline | 1 | 0 | Current NACK behavior without added grace |
| Grace | 1 | 45 | Isolate the bounded worker wait |

The Makefile default remains grace 0; enabling 45 ms requires NACK 1. Build fresh application objects when switching flags, and verify each package's build identity, configuration and hash before installation. Package names and checksums are not verified by this document.

Both profiles retain MPSC ingress, four copy readers, the existing 16 ms/four-feed worker budget, the 128-slot reorder and existing H.264/Opus/recovery behavior. The stream request remains 960 Ã— 540, maxFPS 30 and 5,000 kbps. Actual delivered dimensions must be measured again. There is no new RTX negotiation, SDP change, resolution experiment, decoder allocation or recovery architecture.

## Worker policy and bounds

The video worker exclusively owns the pure grace state. A request must match the current session, accepted original-video source and sequence epoch. An unsolicited control cannot change that binding.

- An admitted position has an absolute **detection time + 45,000 Âµs** deadline. Worker delay consumes this budget; it is not a fresh 45 ms after control delivery or transport dispatch.
- HOLD applies only when the current expected position belongs to an admitted pending range. It temporarily supersedes both ordinary depth-32 and 25 ms hole expiry for that position. Unrelated holes retain their ordinary policy.
- Duplicate requests, overlapping requests, advancement of expected, dispatch and authenticated-return observations never renew the deadline.
- **Authenticated reception alone does not clear pending grace.** Only actual STORED admission to the current worker reorder does. A delayed control reconciles matching slots already STORED; authentication completion can retire a send watch without claiming worker admission.
- At most **16 ranges are active**, with **64 request histories** retained for the fresh session. Completed history remains available to prevent overlapping requests from rearming old positions. No unlimited retries or refunded attempt budget is introduced.
- The reorder still has **128 slots**. A prospective or buffered forward offset reaching 128, a full reorder, or unknown headroom releases matching grace before aliasing/window jump. **Capacity can end the hold before 45 ms**, including after only a few milliseconds at the actual packet rate.
- Deadline and capacity releases force the existing hole advance and AU reset/recovery path. Other cancellations disable the special hold and return to ordinary depth/time decisions without accelerating them. No arbitrary compressed P-frame discard or enlarged queue is added.
- Advancement stops before a position still protected by a different active range, including an ordinary ready gap. An abandoned prefix can therefore end without emitting a packet; the next range retains its original deadline.
- Queries also run when no new packet arrives. Clock regression retires active ranges; deadline arithmetic overflow rejects admission.

The current integration binds the accepted worker sequence epoch 1. Transport requests after a sequence rebase to epoch 2 or later fail source matching and cannot silently rebind from control. This is a deliberate fail-closed limitation: **grace utility after that rebase is not implemented or claimed**.

## Functional control inbox and lifetime

The control inbox is independent of optional trace retention: **256 fixed cells**, at most **16 CAS attempts** per producer, one video-worker consumer. A callback publishes numeric state without allocation, sleep, hardware operations or waiting. It cannot bypass an unpublished FIFO head.

The RTC owner binds the borrowed inbox before callback dispatch. Shutdown retires and joins producers before unbinding and freeing storage; failed joins retain the dependent state. An omitted control permanently disables grace for that session, while NACK observation/dispatch continues. Only a deadline/capacity release that covered the unresolved expected position forces its existing fallback; an omission must not accelerate an unrelated baseline hole.

## Evidence to retain

Keep detection, reserved/requested mask, selected mask, transport-attempt start, protection result, local return, authenticated arrival, reorder result, grace release and AU contribution as separate stages. The attempt clock is captured before packet construction/transport work. A successful local return is **not server receipt, RTT or proof of retransmission causality**. Later traffic may be delayed original traffic.

Report accepted positions, deadline/capacity/source/transport releases, active/history exhaustion, inbox omissions, hold duration and remaining headroom. Separate worker STORED acceptance from AU structural completion, recovery acceptance and the subsequent Decode feed. Native output, RGB publication and actual NEW presentation need their own observations: the decoder supplies no output PTS that would justify assigning the last input AU to an image.

Retain bounded detail/omission counters alongside cumulative totals. Do not label every pause as a NACK hole or compute complete-population percentiles from capped records. The [receiver NACK evidence contract](RECEIVER_NACK_EXPERIMENT_0.7.33.md) continues to distinguish local feedback outcomes and authenticated returns.

## Controlled console procedure

1. Close the current application. Install the verified **Baseline** package, launch freshly, confirm its identity/profile and open one game. Preserve the current working installer and previous exports.
2. After loading, hold **L1 + R1** and press **TRIANGLE** once to mark stable gameplay. Record the game, scene, network arrangement and planned actions; play for **10â€“15 stable minutes**.
3. Use the same chord to mark each transition/loading interval and then stable play again. Preserve the marks and accompanying notes. Do not retrospectively infer an equivalent scene or uninterrupted stable exposure.
4. Return with **L1 + R1 + CIRCLE**, then exit from the catalog with **OPTIONS**. Retain both numeric exports, hashes, close/export results and coverage counters. Use only one game session per fresh application process.
5. Repeat with **Grace**, using the same game/scene, marking, duration and network arrangement. A fresh Baseline repeat can follow if the pair warrants confirmation.

Compare closed actual NEW-image intervals strictly >100 ms per stable minute, cadence distribution, recovery episodes/duration, AU discards and positively observed AU utility. Report separate exposure/coverage for each pass. Collect distinct owner observations for video, audio, controls and clean return/exit. Changing service/server/network conditions prevent one pair from establishing causality.

If grace worsens stable playback or introduces an audio/input/closure regression, stop and retain the evidence. Roll back to **Baseline: NACK 1, grace 0** with a fresh launch. This rollback requires no change to SDP, readers or recovery.

## Verification boundary

| Check | Current evidence |
|---|---|
| Pure grace state | 23 portable cases prepared; preliminary GCC plain and ASan/UBSan passed; Clang plain passed |
| Functional inbox | 5 groups prepared; preliminary GCC plain and ASan/UBSan passed |
| Clang sanitizers | Unavailable because required runtime libraries are missing; no sanitizer pass claimed |
| Final combined suite and reorder integration | Pending completion and final source-correspondence receipts |
| Native application/profile inspection | Pending |
| Fresh Baseline/Grace console utility and pause comparison | Pending |

The preliminary checks do not validate PS4 scheduling, callback contention, server behavior or performance. Final reviewed-source and build receipts must supersede preparation-time claims before delivery.
