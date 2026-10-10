# XCloud4 0.7.31 — independent progress instrumentation

## Verified pre-console checkpoint

This version implements bounded diagnostics for residual pauses in the **NEW/MPSC** path established by the [0.7.30 console comparison](CONSOLE_COMPARISON_0.7.30.md). Synthetic host/parser checks, final-writer interoperability, selected source review, native build, retrieved artifacts and package transfer are verified. **Console installation/launch, overhead and the proposed 10–15 minute gameplay capture remain pending.** This note does not claim a performance repair or console PASS.

Functional settings remain unchanged: **four copy readers**, **16 ms / four Decode** checks, reorder **128 slots / depth 32 / 25 ms**, recovery, video conversion, audio/input and the requested **960 × 540 / maxFPS 30 / 5000 kbps**. Prior first native pictures were **1280 × 720**; the request does not establish delivered format.

## Diagnostic changes

- An independent sampler targets **100 ms** intervals using the common local monotonic clock. Target/actual/snapshot-end times and missed periods expose delayed sampling. A circular buffer retains the latest **12000** samples, approximately twenty minutes at the target period.
- Cumulative RX, stage, reset and presentation counters precede capped detail admission. SSRC sequence observations, queue/actor coherence, omissions, overwrites and open waits remain explicit.
- A bounded NEW cadence ledger, mixed-phase flag and cumulative histogram describe local presentation matches. A main-owned phase ledger is independent of detail admission. PLI attempts and local results are distinct from local demand and unmeasured remote acknowledgment.
- The session begins in loading. Hold **L1 + R1** and tap **TRIANGLE** after loading to mark stable gameplay; repeat to mark transition and again for stable. The combination is reserved locally: its forwarded gamepad frame is neutral while held, including during loading when no visible phase mark is recorded. Ordinary buttons outside the combination retain their mappings.
- A new read-only secondary reader and explicitly synthetic deterministic fixtures validate the supported file contract. The existing 0.7.30 reader and original evidence remain unchanged.

See [the diagnostic contract and run procedure](PROGRESS_DIAGNOSTICS_0.7.31.md). Stable rates use recorded manual active-phase exposure. Phase-crossing or incompletely bounded gaps cannot silently count as stable gameplay. Retained operation observations do not associate the latest compressed AU with decoder output; native output has no returned PTS.

## Verification and artifacts

- **Parser:** 40 deterministic synthetic checks and 18 schema/layout checks passed with original-input preservation. That earlier execution observed writer hash prefix `57362075`. Separate final-writer interoperability then parsed **40 C-emitted synthetic files / 292 semantic checks**, bound to final writer `0a33eb1912194c183893bef7115dfd07fc72e8e15a09850be376c63f27d605d3` and seven matching host inputs. It supplies the final-writer evidence absent from the earlier receipt.
- **Host harness:** 12 cases passed in each GCC plain, ASan+UBSan and TSan mode, and Clang plain. Clang sanitizer libraries were unavailable. TSan does not model standalone fences; host scheduling is not native PS4 proof, and actor/cadence/RX instability branches are not all exercised.
- **Selected source review:** actual Claude Opus 5.5, **50 turns / 49 Read calls / 41 fully returned files**, zero failed/outside-scope operations and matching frozen hashes. No material runtime/parser defect was found. Two documentation findings concerned the locally reserved chord and earlier-writer evidence; this update addresses both with the correct mapping scope and separate final-writer receipt. It does not claim a fresh code review or that Claude executed tests/builds.
- **Native/artifact checks:** native build and selected linked import/lifecycle inspection passed. All **80 native build inputs / 180 frozen repository paths** matched at their checkpoints. Build identity `0caa54f34a994de1ffed125d27e47767062384b2` is embedded; SFO APP_VER/VERSION is **01.01**, title ID **XCLD00001**. VM/PC package copies and **two PS4 FTP readbacks** matched exactly. Copy verification does not establish installation or launch.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| XCloud4-0.7.31.pkg | 8,978,432 | `ba4a3ba4c0d29cdcfccc47338a3eb1cab08e06d7b1e98e19f673d55dc0182b4d` |
| XCloud4-0.7.31-project-source.tar.gz | 1,307,404 | `7cbac1620769722f6dadbcab3dbd4e752e5dba13488fc4d7971a91372e25cf7d` |
| XCloud4-0.7.31-dependency-sources.tar.gz | 83,585,834 | `b306e040c696e4334cfee2cfd48f8569bd9267a77f5fe1aebc52698c71cdacda` |

The project archive preserves the build source checkpoint; the separate dependency archive contains pinned, patched dependency sources, recipes and native adapters. These artifacts remain immutable. This evidence/documentation update followed the frozen build without changing runtime, parser or tests.

## Evidence limits

The original B trace reproduces its saved analysis exactly: closed NEW intervals of **1.886038 s**, **100.006 ms** and **366.998 ms**. Their prior qualified interpretations remain unchanged. Diagnostic omission is not network packet loss; Decode wall time includes scheduling; a local PLI result is not delivery. All new runtime measurements and performance conclusions await the owner-operated console capture.

Queue `CLOCK` can mean an arrival newer than the sampler's start reference, rather than an actual clock failure. Stage observations span actual/snapshot-end times. Detail floods and caps shorten retained history, so missing details do not establish absence. Native scheduling, heap headroom, optional diagnostic export, instrumentation overhead, audio/controls and clean close still require console evidence. The PS4 interface remains Spanish and repository documentation English.
