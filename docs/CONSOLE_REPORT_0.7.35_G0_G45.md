# XCloud4 0.7.35: G0 / G45 console evidence

## Result and scope

The console exercised the intended observer-contention/rebase case in **G45**. The worker validated a new binding for the same original video source and admitted all six subsequent range offers. `stale_source_offers` remained zero. Grace resumed after the proven transition in this session.

Returned positions contributed to structurally complete H264 access units (AU), including units accepted by recovery for Decode. This establishes local reception/reorder/AU utility. It does not associate a repaired AU with a specific decoded or presented picture: the native decoder supplies no output PTS.

The pair does **not** establish improved fluency. The owner labeled the game “Gears E day en modo historia” in both passes, explicitly confirmed **different zones**, and perceived slightly more pauses in G45. This is the owner's label, not a verified official title. Both profiles also had occasional sound described as interference. That new audio report remains unmeasured acoustically.

No playback code, queue size, timeout, reader count, recovery, SDP, RTX, rumble or UI changed while collecting/analyzing this pair. The prepared implementation/build/host-test evidence is separate: [0.7.35 preparation report](NACK_SEQUENCE_RESYNC_0.7.35.md).

## Verified original pair

Runtime content identity: `8425f615c09af4a4d796157922968635fbcad89b`. G0 is `-n1-g0-r0`; G45 is `-n1-g45-r0`. Both are product 0.7.35, NACK ON and rumble OFF. G0's initial NACK configuration records are no longer retained after critical-ring overwrite; its verified embedded profile and 13 reserved/dispatched requests establish active NACK, not complete retention of initial settings.

Each closed numeric original was independently read twice from the console with identical bytes, then matched by internal build/session identity, lifecycle, first/last NEW and both current readers. Two earlier G0 exports lacked STABLE marks; they are preserved but excluded from the principal comparison.

| Original | Bytes | SHA-256 |
| --- | ---: | --- |
| G0 numeric `.bin` | 2,570,112 | `3a203967d2a4a047acfd7f275fff3712a1de75ef112900a419b1a08b9d8d3650` |
| G0 `.progress.bin` | 6,856,608 | `b57c54bd05f0b58bf392e0b59d9a55e8b136dc540624f2f63aa678457f8355a9` |
| G45 numeric `.bin` | 2,503,776 | `4bcd3f357722096712fe8dcee4739a1e5e9ae89fca7111e44cbedc27b3cd58ea` |
| G45 `.progress.bin` | 8,256,480 | `868cd57ba8f7c4f8b335d165901165f0660328a7fffb055c564f0761cf01e63e` |

Raw source/sequence identifiers, numeric exports and kernel logs remain private. Published aggregate findings contain no media payload, key, credential, token or full SDP.

## Marked stable cadence

The denominator is **manual active STABLE exposure**, excluding loading/transitions and intervals crossing phase boundaries. These are local monotonic NEW presentation observations, not provider FPS, physical scanout or total control-to-image latency. The threshold is strictly greater than 100 ms. Both cadence/phase ledgers are complete and clock identities valid.

| Stable-only measure | G0 | G45 |
| --- | ---: | ---: |
| Exposure | 616.667044 s / 10.277784 min | 848.943110 s / 14.149052 min |
| Eligible noncrossing NEW intervals | 18,370 | 25,334 |
| Closed intervals >100 ms | 7 | 10 |
| Intervals >100 ms per minute | 0.681081 | 0.706761 |
| All eligible intervals p50 | 33.051 ms | 33.049 ms |
| All eligible intervals p95 | 49.998 ms | 49.978 ms |
| Stable intervals >100 ms p50 | 383.990 ms | 199.983 ms |
| Maximum eligible interval | 452.018 ms | 451.973 ms |
| Whole-session closed intervals >100 ms | 11 | 12 |

The rates are similar in this ordered pair. Different durations and zones prevent a causal same-scene A/B conclusion. The owner's perceived increase is retained separately from the >100 ms cadence count; this metric does not capture every visible disturbance.

Both traces request 960×540, maxFPS 30 and 5,000 kbps, while every retained native geometry observation is **1280×720, pitch 1280** (213 G0 / 214 G45 observations). Four readers, MPSC256, 16 ms/four-Decode budget and reorder 128/depth 32/25 ms remain configured identically. A request does not establish delivered resolution or provider frame rate.

## Observer continuity actually exercised

G45 records one functional observation-gate omission and one consumed observer reset, with zero SSRC changes. The new sequence epoch is 2; the transport/session identity remains current and original PT is 102.

| Retained transition fact | Observation |
| --- | --- |
| Sequence announcement | 862,444,891 µs |
| Validated worker COMMIT | 862,445,180 µs |
| Exact raw/canonical anchors | Verified in private originals; omitted from public report |
| Proven offset | 393,216 = 6 × 65,536 |
| Binding announcements / proofs / commits, whole session | 2 / 2 / 2 |
| Later request ordinals | R10–R15 |
| Later offers admitted / HOLD / accepted RELEASE | 6 / 6 / 6 |
| `stale_source_offers` | 0 |
| Functional sequence-control inbox omissions / stale / pending at close | 0 / 0 / 0 |

The transition used matching immutable original-source provenance. No standalone STORED-proof event is retained: proof is supported by the proof counter and the inspected COMMIT contract requiring a valid worker proof. An arbitrary request did not establish the epoch. Retained HOLD deadlines for R2/R4/R5/R6 and R10–R15 match detection +45,000 µs. The 64-request session budget and deadline preservation are code/host-test results; this 15-request console pass does not exercise budget exhaustion. Missing or contradictory exact anchors remain a separate safe liveness limit described in the preparation report.

G45 also counts 10 `binding_unproven` and 21 `binding_unmapped_provenance` operations (G0: 0 and 1). These may repeat and are not packet-loss counts. A retained `BIND_OR_CONTROL_REJECT` at 862,446,755 µs has unclassified mixed-layout fields and helper result 4; it does not establish a packet discard. Safe rejection and successful later binding are separate observations.

The observer retired four watches covering 49 pending positions at rebase. Their absence from subsequent watched AUTH records does not prove those positions never reached transport/application. A functional observation omission, retired watch, control rejection and diagnostic omission are different facts.

## Returned positions and AU utility

The complete private range audit records 15 reserved G45 ranges: 14 have successful protected/local-send result paths; R9 has no dispatch path. Two gated dispatch operations and one suppression record are separate counts, not two proven lost requests. A successful protection/send API result is local evidence, not server receipt or a causal retransmission/RTT proof. The observer correlated 150 authenticated returned positions; the worker accepted 102 positions. All 15 retained RELEASE records comprise eight completed, two deadline, two bounded-capacity and three source/reorder-reset retirements (R7–R9). Counter populations and retired watches must not be combined into a universal loss count.

| Retained NACK-associated AU stage | Whole G45 | After validated rebase, R10–R15 |
| --- | ---: | ---: |
| Positions contributing while assembling an AU | 102 | 78 |
| Positions in structurally complete AUs | 95 in 8 AUs | 78 in 6 AUs |
| Positions in AUs accepted by recovery before Decode | 92 in 7 AUs | 75 in 5 AUs |
| Positions in complete AUs rejected by recovery | 3 in 1 AU | 3 in 1 AU |
| Positions in discarded AUs | 10 in 2 AUs | 3 in 1 AU |

STORED, contribution, structural completion, recovery acceptance, Decode output, RGB and NEW remain separate. R6's seven accepted positions entered an AU that was discarded without structural completion; they are not a useful repair. The full range audit attributes the three structurally complete/recovery-rejected positions to R10, whose AU was then discarded. Stage populations overlap: the 10 discarded positions include those three, so they cannot be added as independent losses. The table supports resumed grace and pre-Decode utility, not seven demonstrated repaired pictures or a fluency benefit. Retained reorder results omit one record, and grace detail omits two records; cumulative counters and retained AU-stage populations have their own coverage.

## Capacity/deadline cases preserved

| Range | Release reason / time after detection | Subsequent observed arrival |
| --- | --- | --- |
| R2 | Capacity at +6.539 ms | All 16 correlated AUTH arrivals occur after release; first is 5.301 ms later. AUTH→POP p50/p95/max: 0.618/1.359/1.359 ms. |
| R4 | Capacity at +7.146 ms | All 21 correlated AUTH arrivals occur after release; first is 22.595 ms later. AUTH→POP p50/p95/max: 0.706/1.815/1.887 ms. |
| R5 | Absolute deadline | Nine correlated AUTH arrivals occur after release. |
| R6 | Absolute deadline after partial acceptance | Seven positions reach AUTH/POP/STORED before release but their AU is discarded; two later positions are rejected; nine positions have no later watched AUTH correlation after retirement. |

These new R2/R4 cases are **different from historical 0.7.34 R2**, where all 14 returns were already authenticated/enqueued before capacity release and were popped too late. That admission-order limitation still exists and was retained in deterministic tests. No time/capacity increase or worker redesign is justified by conflating the cases.

The 16 ms budget cannot interrupt an in-progress Decode/copy/conversion. The historical additional startup reference-copy/byte comparison remains distinct from stable pauses. Short unexplained intervals and incomplete detailed histories remain open investigation items.

The callback event is written after queue admission but carries callback-begin time. It is not an exact MPSC publication timestamp. The measured AUTH→POP spans and source/sequence association do not supply an exact queue-residence interval from that callback timestamp alone.

## Coverage and interpretation limits

| Diagnostic population | G0 | G45 |
| --- | ---: | ---: |
| Critical attempts / retained records | 4,204 / 4,096 | 3,916 / 3,905 |
| Critical gate omissions / overwritten records | 27 / 81 | 11 / 0 |
| Source-slot / sequence-observation omissions | 0 / 0 | 0 / 0 |
| Cadence / manual-mark omissions | 0 / 0 | 0 / 0 |
| Progress detailed-window omissions | 3 | 4 |

Complete cumulative source observations do not establish complete per-packet history. Complete cadence supports the stable rates despite critical/general-detail omissions; it cannot fill missing causal records. General event admission drops/history overwrites persist. PLI_REQUEST is local demand; an attempt/API result is not a remote acknowledgment. No absent retained record establishes a negative fact in an incomplete population.

## Audio finding in both profiles

The owner clarified that occasional sound resembling interference occurred **in both G0 and G45**. No acoustic/PCM recording, noise timestamp or confirmed coincidence with video pauses exists. This report therefore records a newly reported audio problem, not a measured regression attributable to grace.

| Whole-session audio observation | G0 | G45 |
| --- | ---: | ---: |
| Valid app callbacks | 34,510 | 46,265 |
| Forward sequence-gap events at DATAGRAM | 5 | 3 |
| Forward skipped positions | 7 | 5 |
| Later missing / backward / duplicate observations, audio source | 0 / 0 / 0 | 0 / 0 / 0 |
| Source sequence-observation omissions | 0 | 0 |

For each original PT111 audio source, cumulative packet/gap/forward-position observations agree from positive UDP-read DATAGRAM through authenticated delivery and APP_CALLBACK. The gaps already exist at that observation boundary. This does not identify Internet, server, OS queues or the audible artifact's cause. G45's three global replay-failure observations are not audio-specific.

The current export lacks audio admission/reorder/Opus-result/PCM/output counters and durations. Klog records first 960-sample Opus output and clean audio-close messages, which do not prove glitch-free sound. The live-audio UI snapshot's `bad_frames` combines decoder errors and PCM trim; its `underflows` counts local short PCM blocks, not measured device underruns. Sequence-loss reset, no PLC/FEC and zero-filled partial blocks are source facts and possible mechanisms, not demonstrated causes of the reported noise.

## Next minimum investigation

The continuity correction is supported by this exercised transition and remains the base. The evidence does not select a further functional video optimization: bounded-capacity release, queued-return admission and the unexplained short pauses remain separate.

The next proposed change is **audio-only diagnostic instrumentation**, preserving playback policy/buffers: export existing admission, loss/reset and output counters; separate negative Opus results from PCM trims; retain bounded monotonic events for PCM-short blocks and AudioOut wait/result. Add a noise marker and independent omission counters. Keep NACK/grace/reorder/readers/resolution/recovery and rumble unchanged. No audio NACK, PLC/FEC, buffer/time enlargement or worker redesign is included in that proposal.

A later fluency comparison needs fresh G0/G45 sessions in the same intended zone/scene with marked loading/transitions. This different-zone pair is retained and must not be relabeled as a controlled causal A/B test. Numerical exports and reproducible readers remain available privately.

## Analysis and review provenance

The author independently parsed the closed originals with both current readers, verified hashes against double-read FTP receipts, and checked cadence/phase pairing. Separate private NACK/AU and audio audits supply the full-range outcomes, arrival deltas, window omissions and Klog observations; historical deterministic fixtures belong to the preparation campaign. These are distinct evidence sources.

Actual Claude Code `claude-opus-5-5` completed a read-only review of an immutable draft, selected derived facts, reader semantics and three binding source/header extracts. It inspected all supplied lines and raised encoding, discarded-AU, coverage and scope corrections incorporated here. It did not reparse originals, verify their bytes, run host/native tests or review all ranges. The final edited report was checked by the author; the reviewer did not reread it. No fresh runtime tests, build or functional changes were performed for this documentation-only console analysis.
