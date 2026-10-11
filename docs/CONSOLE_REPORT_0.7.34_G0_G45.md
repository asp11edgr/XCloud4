# XCloud4 0.7.34 - G0/G45 console comparison

## Result

Two fresh, closed console sessions exercised **G0: NACK ON / grace OFF / rumble OFF**, followed by **G45: NACK ON / grace 45 ms / rumble OFF**. Within the marked stable and loading non-crossing populations, both retained zero closed local NEW-image intervals strictly greater than 100 ms, with complete cadence and manual phase ledgers. The owner subjectively reports that G45 felt the same as the preceding pass.

**Neither session encountered an observed sequence gap that requested NACK.** There were zero requests, dispatches, grace offers, holds, requested-position returns or useful repair contributions. G45's configuration is verified, but its repair policy was not exercised. This pair establishes neither successful 45 ms repair nor a fluency benefit from grace. The lower descriptive G45 p95 cannot be attributed to a hold that never occurred.

The first-pass owner's game-name entry is preserved verbatim in the private notes. An official title/ID and exact scene equivalence are not independently established. The subsequent response (translated: "It felt the same as the previous one") reports similar experience; it does not separately confirm audio, controls, scene equivalence, visible catalog return or a no-error exit dialog. Numeric lifecycle/export evidence is reported separately below. These captures do not identify Internet or server causality.

## Binary and configuration

Both packages retain frozen runtime source ID **`54d5793d1b50a7ea24ba73e79b258577f51511c0`**, from the [prepared release](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.34), application checkpoint `f17861746640a1d2a9639609ec367eaf198372e0`. This report does not change code, packages or previous release assets.

| Item | G0 | G45 |
| --- | --- | --- |
| Product / APP_VER | 0.7.34 / 01.03 | 0.7.34 / 01.03 |
| Build suffix | `-n1-g0-r0` | `-n1-g45-r0` |
| Package SHA-256 | `41fa0c83ed43469fe919af3ec460306c8c0659b79bfbee8057645b841b4386e4` | `4598a9670b8e4e35f3876f9766878fd196ccdcb2d3ba05a2e67ec1c19bf104f4` |
| Retained native geometry | 1280 x 720, pitch 1280 | 1280 x 720, pitch 1280 |
| Numeric grace / rumble configuration | 0 / 0 | 45 / 0 |

Both retain **NEW/MPSC256, four copy readers, 960 x 540 / maxFPS 30 / 5000 kbps request, 16 ms / four Decode feeds and reorder 128 / depth 32 / 25 ms**. Recovery, SDP and RTX configuration remain unchanged. The actual remote Answer's latest retained NACK check is accepted in both; the reader does not claim that this observation independently establishes feedback state at stop. Requested maxFPS does not measure actual provider or panel FPS.

## Exposure and cadence

The stable denominator is the complete **manually marked active exposure**, not first-to-last NEW. Only closed intervals whose endpoints and observed boundaries remain in the stable phase enter the stable population. Unmarked cinematics or loading remain in the owner's manual phase. NEW is a local consumption/presentation match, not physical scanout or controller-to-image latency.

| Stable population | G0 | G45 |
| --- | ---: | ---: |
| Marked exposure, seconds | 617.714111 | 608.910981 |
| Marked exposure, minutes | 10.295235 | 10.148516 |
| Non-crossing closed intervals | 21,552 | 22,192 |
| Closed intervals >100 ms | 0 | 0 |
| Intervals >100 ms per minute | 0.000000 | 0.000000 |
| Interval p50, ms | 32.974 | 32.967 |
| Interval p95, ms | 48.953 | 34.947 |
| Interval maximum, ms | 82.041 | 68.015 |

Whole-session retained cadence contains **23,944 / 24,558** intervals, without cadence omissions, clock/identity errors or an open pause at stop. The marked loading exposure is **60.199884 / 58.526089 seconds**, with zero >100 ms non-crossing intervals; transition exposure is only **1 / 2 microseconds**, with no qualifying interval. Phase-crossing intervals and intervals without a begin time are excluded from the phase populations; this report does not establish their >100 ms status. These phase rates do not turn different service/scene conditions into a controlled causal benchmark. An interval under the diagnostic threshold can still be perceptible, and zero observed >100 ms intervals in these populations does not establish the absence of every pause.

There is no >100 ms pause in the covered non-crossing stable/loading populations or retained requested-range episode to enumerate. A detection-to-request-to-return/grace-acceptance chronology, return-time distribution or capacity-abandonment chronology is therefore **not applicable for this pair**, rather than a successful repair result.

## Recovery and AU stages

The complete critical ledger records **one startup recovery begin/end in each session**, one completed AU rejected for missing IDR/SPS/PPS and one startup AU discard. No additional covered recovery episode, reorder hole/jump or damage-bit discard is observed.

| Whole-session AU observation | G0 | G45 |
| --- | ---: | ---: |
| Structurally complete before recovery filter | 38,051 | 39,925 |
| Rejected by recovery filter | 1 | 1 |
| Accepted for Decode feed | 38,050 | 39,924 |
| NACK ranges / selected positions | 0 / 0 | 0 / 0 |
| Grace worker-accepted requested positions | 0 | 0 |

Structural completion, recovery acceptance and Decode feed are separate facts. Native output, RGB and NEW presentation are not attributed to these input AUs: the decoder supplies no returned output PTS. There were no feedback local-send results or returned requested positions from which to infer delivery, causal retransmission or RTT.

## Queue sampling and coverage

The independent sampler retained **6,981 / 6,863 rows**, with zero sample omissions or missed periods. Maximum observed sampler lateness is **1.778 / 1.674 ms**. Sampling is not continuous queue residence and counters are individually atomic rather than one global snapshot.

| Timing-valid stable sampled queue observations | G0 | G45 |
| --- | ---: | ---: |
| Stable rows | 6,177 | 6,089 |
| Rows with a valid nonempty oldest-packet age | 2,515 | 2,656 |
| Nonempty oldest age p50, ms | 4.004 | 3.858 |
| Nonempty oldest age p95, ms | 12.894 | 11.206 |
| Nonempty oldest age maximum, ms | 116.061 | 76.686 |
| Maximum reported sampled queue depth | 104 | 91 |

Null/empty/unstable ages are excluded from that age distribution. Depth is a sampled observation, not a continuous peak. These values are descriptive and cannot establish queue behavior during grace because no hold occurred. They do not establish zero instrumentation overhead or an end-to-end latency improvement.

Critical record admission/retention is **129/129 (G0)** and **128/128 (G45)**, with zero critical gate/ring/source-slot/sequence-observation omissions. Critical admission completeness does not establish complete transport reception, correct functional control-inbox delivery or complete general tracing. All instrumented sequence stages retain zero gap events, skipped positions or later missing-position observations. The NACK/grace ledger consists of state and counter snapshots; grace control admissions, omissions, offers, accepted positions, deadline releases and capacity releases are zero. This is evidence that the covered trial was unexercised, not evidence that every unavailable transport/socket condition is absent.

General operation detail remains incomplete: progress event admissions omitted **9,797 / 9,311**, and history overwrote **5,816,945 / 5,705,571** records. Actor snapshots were unstable **48 / 38** times and sampler detail gates omitted **40 / 30** observations. These limits prevent complete per-operation timing or absence claims; they do not invalidate the independently complete critical, cadence or phase populations. No per-packet video payload was retained.

## Preservation, lifecycle and next step

Each legacy/progress pair was read twice by FTP with identical bytes. Build identity comes from the progress header; its paired legacy export agrees on local session, reader count and first/last NEW. Both retain sampler start/join return **0**, lifecycle flags **15**, session end and no open pause. G45's PC Klog also retains successful local numeric export results. The original one-hour PC Klog capture expired before continuous G0 coverage could be established; G0's saved numeric exports supply its evidence. Later backlog reception is not proof of uninterrupted PC capture or a shared administrative/media clock.

Raw exports, source/SSRC identities, detailed derived JSON and administrative logs remain private. Package/source hashes and the actual native/host preparation scope remain in [the release notes](RELEASE_NOTES_0.7.34.md); original 0.7.33 evidence stays preserved. Two independent parsing/pair/coverage checks support this report.

An actual **Claude Opus 5.5** read-only evidence review completed on an immutable four-file bundle: its manifest, selected derived facts, this draft and reader-semantics excerpts. The orchestration verified hashes and unchanged inputs. Claude identified the 1/2-microsecond transition correction and the need to scope zero >100 ms to non-crossing phase populations; these corrections and its coverage/rumble limits are incorporated here. Claude did not reparse raw traces, execute tests, validate native operation or observe physical vibration. Artifact/FTP/lifecycle claims outside that bundle rely on the separately retained verification receipts, not on this review.

No new worker, reorder or recovery change is justified by these unchallenged passes. Keep **G0** available as the reference and **G45** as the pending repair experiment. A future fresh pair needs naturally observed missing positions before useful acceptance/AU contribution or capacity/queue limitations can be evaluated; do not deliberately alter another variable or claim that the earlier 46 positions are repaired.

The next independently selectable test is one fresh **G45VIB / `-n1-g45-r1`** pass, following [the rumble guide](RUMBLE_0.7.34.md). Check report reception, desired motor output, local Pad results and owner-observed vibration/stop separately. Protocol reception and successful local Pad results do not prove motor movement; the owner's observation on the actual controller is required. Keep this independent pass out of the rumble-off G0/G45 performance pair. Physical vibration and separate owner audio/controller confirmation remain pending; no fixed next optimization is proposed from absent repair exposure.
