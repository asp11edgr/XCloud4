# XCloud4 0.7.33 — receiver NACK OFF/ON console report

## Verified result

On **2026-10-10**, two fresh, closed console sessions exercised the published **0.7.33 OFF and ON** packages. Both paired numeric exports were read back twice with identical bytes and the expected internal Build ID. The capture is now stopped after both closed-session exports were saved. No additional playback pass is requested for this report.

**ON requested six missing video ranges, covering 55 selected positions. All 55 positions subsequently appeared as first authenticated original-video observations after the matched successful local-send result observation. All arrived after their corresponding local reorder hole had already been abandoned, were rejected behind the worker's expected sequence, and made no observed contribution to an AU.** This experiment observes return viability but demonstrates **no useful repair or reproducible reduction of pauses** under the unchanged reorder policy.

The six protection operations succeeded locally and the propagated `transportSend` boolean was true six times. Neither result acknowledges server receipt. A later authenticated original sequence remains compatible with delayed original traffic as well as retransmission; this capture does not uniquely prove NACK causality. RTX was not enabled, negotiated anew or observed.

OFF retained **3 stable NEW-image intervals >100 ms in 15.050083 minutes (0.199334/min)**. ON retained **9 in 15.032901 minutes (0.598687/min)**. ON also encountered **six video gaps / 55 positions**, versus **three / 29** in OFF. This single ordered pair has unequal upstream gap exposure and unverified scene/server equivalence. It establishes neither improvement nor causal worsening from NACK.

The owner subsequently reports that **OFF and ON felt the same**, with **audio, controls, catalog return and application exit working correctly**. This is the owner's perceptual and functional feedback, separate from the numerical pause counts; it does not establish equivalent measured performance or a NACK benefit.

## Binary and unchanged configuration

| Item | OFF | ON |
|---|---|---|
| Product / PS4 APP_VER | 0.7.33 / 01.03 | 0.7.33 / 01.03 |
| Runtime Build ID | `e9a0b21e02d059559c4a57bed8f80db183f86021-nack0` | `e9a0b21e02d059559c4a57bed8f80db183f86021-nack1` |
| Package SHA-256 | `cff808534bca6c58c1449fd7cf36e1ae5aae443afd9abf951796e874001d9687` | `0c9c5e659e537f3e2c4d6cf8a3b78ece7e7ee20a000a41f13db2861412ae1dc2` |
| Retained actual geometry | 1280 × 720, pitch 1280; 140 observations | 1280 × 720, pitch 1280; 292 observations |
| Actual remote Answer NACK check | accepted; requests disabled | accepted; requests enabled |

Implementation checkpoint: `dbcc9c4bf427afabbf1c0c7eb969f1d58b1885ae`. The pinned libdatachannel 0.24.6 / libjuice 1.7.4 / libsrtp 2.8.0 / Mbed TLS 3.6.7 revisions and the single generator's authenticated RTP/protected RTCP route are recorded in [the experiment contract](RECEIVER_NACK_EXPERIMENT_0.7.33.md). There is no parallel receiver generator or raw unprotected socket feedback.

Both trace headers retain **NEW/MPSC256, four persistent copy readers, 960 × 540 / maxFPS 30 / 5000 kbps request, 16 ms worker budget / four Decode feeds, reorder 128 entries / depth 32 / 25,000 µs**, and the existing recovery policy. The 720p geometry is measured; negotiated provider FPS and end-to-end age are not established. This report changes documentation only. Packages, source archives, existing release assets and runtime settings remain preserved.

## Exposure, cadence and guardrails

The denominator below is **manual active phase exposure**, with only non-crossing, closed NEW-image intervals assigned to that phase. NEW is the existing local consumption/presentation match of a new RGB generation; it is not physical scanout or a sender timestamp.

| Mode | Marked population | Exposure, seconds | Closed intervals >100 ms | Per minute |
|---|---|---:|---:|---:|
| OFF | loading | 67.407957 | 0 | 0.000000 |
| OFF | stable | 903.005009 | 3 | 0.199334 |
| OFF | transition | 0.000002 | 0 | 0.000000 |
| ON | loading | 62.878025 | 0 | 0.000000 |
| ON | stable | 901.974035 | 9 | 0.598687 |
| ON | transition | 1.166027 | 0 | 0.000000 |

| Stable NEW-interval population | Count | p50, ms | p95, ms | Maximum, ms |
|---|---:|---:|---:|---:|
| OFF | 32,849 | 32.962 | 34.955 | 350.044 |
| ON | 31,918 | 32.972 | 34.985 | 384.038 |

Manual phase ledgers and all cadence intervals are retained, without clock/identity errors or an open pause at stop. OFF has one stable epoch; ON has two separated by a marked **1.166027-second** transition. Unmarked cinematics or loading remain in their manual phase; scene equivalence is not inferred from marks alone. The application was closed after each pass; neither session has been relabeled as an installation-only checkpoint.

Both captures retain monitor start/join `0`, media/video/sampler stop events and NACK close state. Private Klog separately records session DELETE HTTP 200 / error 0 and both export return codes 0 for each mode. These support successful cleanup/export; they do not independently prove absence of a visible PS4 error dialog or individual audio/controller behavior. **The owner confirms good audio, controls, catalog return and exit, and perceives the pauses as the same in both modes.**

Retained copy-wall p50/p95 is **9.319/9.936 ms (OFF, n=157)** and **9.356/9.921 ms (ON, n=307)**; conversion p50 is **3.037/3.045 ms**. These are bounded windows and final history, mixing phases and favoring pauses/tail observations, not a complete stable-frame A/B. They do not establish zero diagnostic overhead. No compressed input is associated with a decoder output: the native decoder still supplies no output PTS.

The preserved [0.7.32 result](CONSOLE_REPORT_0.7.32.md), **0.950985 stable pauses/min, 14 video gaps / 202 positions**, is a different session population. Its original report, traces and assets remain intact; it is not a new OFF control for this pair.

## Reception and AU populations

| Whole-session observation | OFF | ON |
|---|---:|---:|
| Valid/admitted video callbacks | 549,522 | 548,697 |
| Application queue rejections, all covered reasons | 0 | 0 |
| Video gap ranges / absent positions first seen at DATAGRAM | 3 / 29 | 6 / 55 |
| Audio gap ranges / absent positions first seen at DATAGRAM | 2 / 2 | 1 / 1 |
| Playback recovery episodes after startup | 3 | 4 |
| Reorder-hole reset calls | 3 | 6 |
| AU structurally complete before recovery filter | 58,083 | 56,463 |
| AU rejected by recovery filter | 50 | 72 |
| AU accepted for Decode feed | 58,033 | 56,391 |
| Open AU discarded for marker-incomplete / orphan FU continuation | 1 | 3 |

The recovery filter rejects **49/71** AUs for missing IDR and **one in each mode** for missing IDR/SPS/PPS together. These are completed AUs rejected while recovering, not 50/72 failures of structural assembly. Reset calls can discard no open AU; damage bits can overlap. Accepted-for-feed is neither a successful Decode call nor native output/RGB/presentation.

ON retains **548,697 original video observations at every covered RTP boundary from DATAGRAM through authenticated SRTP, Track and APP_CALLBACK**, including PEER_HANDLER_OUT. All six gap ranges recur through these stages and all 55 positions later reappear. The callback observer totals and state-engine totals differ by eight because their covered lifecycle boundaries differ; that difference is not classified as packet loss.

No covered authentication, decryption, replay, parsing, routing or Track rejection counter is nonzero. The only nonzero instrumented rejection/drain counter is nonblocking `SOCKET_WOULD_BLOCK` (132,134 in ON); an empty drain is not a loss observation. Unhooked conditions from [the reception plan](RECEPTION_DIAGNOSTICS_0.7.32.md) remain unavailable. The first observed gap boundary is after a positive UDP read; sender, network and earlier NIC/kernel/socket-queue causes remain indistinguishable.

## Timing-boundary correction for the 0.7.34 preparation

The original `actual_transport_dispatch_reference_us` is **attempt start**, sampled before building/protecting/sending feedback. It is not a send-completion timestamp. The historic timing table below now labels that boundary correctly without changing its numeric observations or the preserved raw captures.

Re-reading the same ON originals against the separate `SRTCP_RESULT` and `LOCAL_SEND_RESULT` record clocks places all 55 first AUTH observations after their corresponding successful local-send result. Zero retained observations fall in the ambiguous interval between those two results. Missing results, failed results or overlapping clocks remain indeterminate in the corrected reader.

| Request | Positions | SRTCP result→first/last AUTH, ms | Local send result→first/last AUTH, ms |
|---|---:|---:|---:|
| R1 | 9 | 200.311–200.690 | 200.236–200.615 |
| R2 | 2 | 31.944–32.148 | 31.919–32.123 |
| R3 | 11 | 29.163–30.394 | 29.134–30.365 |
| R4 | 10 | 34.637–35.229 | 34.607–35.199 |
| R5 | 9 | 30.217–32.437 | 30.194–32.414 |
| R6 | 14 | 38.181–39.379 | 38.134–39.332 |

For the 46 faster positions, local-send-result→AUTH spans **29.134–39.332 ms**, with p50/p95 **34.607/39.238 ms**. The nine R1 positions span **200.236–200.615 ms**. Across all 55 positions, p50/p95/max is **34.837/200.523/200.615 ms**. These are local observation intervals, not server RTT, server receipt or proof that NACK caused retransmission. All matched hole-abandonment and behind-expected/AU conclusions remain unchanged.

The new [bounded grace experiment](NACK_GRACE_EXPERIMENT_0.7.34.md) is prepared separately; its console utility is still unmeasured. Original 0.7.33 exports and prior release assets are preserved.

## Every ON request and authenticated return

The table uses request labels rather than publishing SSRC/source identities. Exact extended ranges, selected masks and common monotonic clocks remain in the private originals. Each request selected its whole range; no pre-dispatch first arrival cancelled a selected position. One request per new range, no retries, and **6/64** reserved attempts were used. Suppression, tracking eviction, gate/ticket omission and close-undispatched counters are zero in ON. OFF records three disabled ranges / 29 positions and no dispatch/protection/local-send attempt.

| Request | Positions | Detect→request, ms | Request→transport attempt start, ms | Detect→hole skip, ms | Transport attempt start→first/last AUTH, ms | Hole skip→first/last AUTH, ms | AUTH→worker INSERT min/max, ms | Behind expected / AU contribution |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| R1 | 9 | 0.004 | 0.034 | 2.399 | 200.363–200.742 | 198.002–198.381 | 0.439–0.732 | 9 / 0 |
| R2 | 2 | 0.002 | 0.100 | 3.301 | 31.961–32.165 | 28.762–28.966 | 119.854–120.046 | 2 / 0 |
| R3 | 11 | 0.002 | 0.934 | 2.910 | 29.207–30.438 | 27.233–28.464 | 118.673–119.802 | 11 / 0 |
| R4 | 10 | 0.004 | 0.565 | 1.937 | 34.663–35.255 | 33.295–33.887 | 1.025–1.530 | 10 / 0 |
| R5 | 9 | 0.005 | 0.938 | 3.058 | 30.241–32.461 | 28.126–30.346 | 0.629–0.852 | 9 / 0 |
| R6 | 14 | 0.004 | 0.201 | 2.448 | 38.225–39.423 | 35.982–37.180 | 0.548–1.873 | 14 / 0 |

All six local SRTCP protection statuses are `0`, and all six propagated local send booleans are true. Protection call spans are **7–23 µs**; detection→transport attempt start is **0.038–0.943 ms**. Those are local spans, not wire RTT, receipt acknowledgments or video age.

Each matched legacy hole records **buffered depth 32**; none records the time-expiry reason. R4 permits expiry in its call flags but does not report expiry as the decision. Thus the configured 25 ms was **not a minimum repair opportunity**: the local range was abandoned after **1.937–3.301 ms** from generator detection.

The independent join matches each hole's exact first wire sequence and length to one request range, in this session's sole original-video source and continuous sequence epoch, with no SSRC change, half-wrap ambiguity or observer omission. Legacy hole records themselves lack SSRC/request identity, so this is a qualified session/range/time association. AUTH clocks were compared directly to skip clocks; a behind-expected INSERT flag alone was not used to infer arrival timing. All 55 AUTH observations occur **27.233–198.381 ms after the matched skip**.

Per selected position, attempt start→first AUTH p50/p95/max is **34.893/200.650/200.742 ms**. Five ranges, 46 positions, span **29.207–39.423 ms**, with p50/p95 **34.663/39.329 ms**. R1's nine positions appear around **200 ms**. Do not infer a sender RTT or unique retransmission cause from these local spans.

**Authentication is not worker admission.** R2/R3 then take **118.673–120.046 ms** from AUTH to the directly associated worker reorder observation; the other four ranges take **0.439–1.873 ms**. These are measured local post-authentication spans. Their exact scheduling/queue cause is not established by the bounded retained details. A repair deadline must be evaluated at worker admission as well as AUTH; even the faster authenticated returns do not guarantee useful admission within a 45 ms candidate grace.

| Requested-position outcome under current policy | Positions |
|---|---:|
| No later first authenticated observation during covered session | 0 |
| First authenticated observation after corresponding hole abandonment | 55 |
| Returned before abandonment and contributed to a usable AU | 0 |

All 55 have direct `NACK_REORDER_RESULT rc=1 / flag=1` behind-expected evidence and **zero NACK AU-contribution events**. No native output, RGB or NEW presentation is attributed to those positions. R1–R3 occupy **one** recovery episode, not three independent recoveries; R4, R5 and R6 each overlap another.

## Stable pause chronology

Times are relative to each mode's first NEW observation. Recovery duration ends when local recovery/feed acceptance clears the waiting state, not when an IDR is presented. First sampled leads describe progress over a retained approximately 100 ms span; they are not established causes.

| Pause | Start | NEW interval, ms | Video socket gaps inside | ON request labels | Overlapping recovery, ms | First retained sampled lead |
|---|---|---:|---:|---|---|---|
| OFF-1 | 07:28.065 | 350.044 | 1 | none | 180.049 | Admission continues; no accepted AU progress |
| OFF-2 | 08:26.157 | 283.044 | 1 | none | 131.761 | Admission continues; no accepted AU progress |
| OFF-3 | 14:24.267 | 316.942 | 1 | none | 100.359 | No callback progress in the sampled span |
| ON-1 | 01:55.414 | 384.038 | 3 | R1, R2, R3 | 232.252 | Admission continues; no accepted AU progress |
| ON-2 | 03:41.254 | 299.997 | 1 | R4 | 205.841 | Admission continues; no accepted AU progress |
| ON-3 | 07:00.287 | 150.007 | 0 | none | none | Indeterminate in retained coverage |
| ON-4 | 07:24.862 | 183.091 | 0 | none | none | No callback progress in the sampled span |
| ON-5 | 08:53.817 | 116.907 | 0 | none | none | Indeterminate in retained coverage |
| ON-6 | 09:07.865 | 168.028 | 0 | none | none | No callback progress in the sampled span |
| ON-7 | 09:21.679 | 384.005 | 1 | R5 | 205.805 | Admission continues; no accepted AU progress |
| ON-8 | 10:16.702 | 381.958 | 1 | R6 | 202.027 | Admission continues; no accepted AU progress |
| ON-9 | 13:08.106 | 101.074 | 0 | none | none | Indeterminate in retained coverage |

OFF's socket gaps occur **136.416, 125.213 and 180.017 ms after** their presentation intervals began; recovery then starts **2.844, 2.120 and 3.093 ms** after the socket gap. The gap/recovery overlap does not explain those earlier portions.

ON's four recovery-overlapping pauses likewise begin **114.786, 56.232, 145.910 and 143.625 ms before** the first corresponding socket gap. They include recovery waits of **202.027–232.252 ms**. The other **five stable pauses** have no retained socket-gap/NACK/recovery overlap. Missing detailed coverage and sampler granularity leave their origins insufficiently explained; they are not automatically assigned to NACK, Internet, Decode or display.

## Coverage and omissions

| Evidence population | OFF | ON |
|---|---|---|
| Critical attempted / retained | 358 / 358 | 1275 / 1273 |
| Critical omitted / overwritten | 0 / 0 | 2 / 0 |
| NACK detail and final counters | complete, 49 counter IDs | complete, 49 counter IDs |
| Sequence-observer omissions / source-cap omissions | 0 / 0 | 0 / 0 |
| Sampler retained / omitted | 9903 / 0 | 9871 / 0 |
| Sampler contextual try-gate omissions | 41 | 45 |
| Primary RX sample instability | 1 | 0 |
| Maximum observed sampler lateness | 1.759 ms | 1.792 ms |
| Cadence retained / omitted | 35572 / 0 | 34450 / 0 |
| Detailed windows retained / omitted | 3 / 0 | 8 / 1 |
| High-volume history overwrites | 8,345,234 | 8,326,765 |
| High-volume event admission drops | 13,564 | 13,326 |

ON's two critical omissions are **LATE_POSITION** rows in the cross-stage ledger, not NACK return, reorder, recovery or phase/cadence records. All 55 NACK first-return records and the cumulative per-stage 55-return totals are retained; do not describe the entire critical ledger as complete. The original legacy export separately retains **6/6 exact REORDER_HOLE records**, while the bounded progress details retain only three. This discrepancy is resolved by preserving both original formats, not by inventing missing progress rows.

High-volume history is circular and detailed windows have event caps/short prehistory. They are not complete per-packet histories. Snapshot instability is explicitly reported (OFF actor/queue/cadence 51/1/1; ON 61/3/0). No missing detail is treated as a measured absence. PLI_LOCAL, PLI_ATTEMPT and PLI_RESULT remain separate; a local result does not establish delivery to the server. IDR_SEEN, structural completion, recovery acceptance, native output, RGB and presentation remain distinct stages.

The sampler's contextual try-gate omissions do not omit the independently retained periodic sample itself, but limit context/opening observations for those ticks. ON retains six critical PRESENT_GAP_BEGIN rows and nine END rows; the complete cadence/phase population, not the opening-event count, supplies the nine stable intervals >100 ms. This asymmetry alone is not treated as a runtime fault or as three lost cadence intervals.

## One next controlled change, not implemented

**Propose one NACK-aware reorder grace policy for a pending requested original-video range: an absolute deadline of detection +45 ms, with no extension.** Only while that exact range remains pending, that deadline would govern both depth-triggered early abandonment and its usual 25 ms expiry. Ranges without such a request retain the existing policy. Completion, source/session change, close, an unsafe window span or capacity pressure must terminate the grace and fall back safely before a slot collision. The 128-entry capacity, readers, ingress, media request, work budget and recovery conditions remain unchanged.

The chosen 45 ms covers the observed **AUTH arrival clocks of 46 positions in five faster ranges**, including local dispatch delay; it deliberately does not wait for the roughly 200 ms outlier. It does **not** cover R2/R3's later worker-admission clocks. The grace is a candidate bound, not a verified optimum or promised repair. Raising only the timeout while keeping depth32 as an early trigger would not address the observed decision.

**Capacity may preempt the grace.** Observed worker expected-sequence offsets at later INSERT reach 215, 187, 132 and 142 positions in R2/R3/R5/R6. Those offsets are not occupancy at AUTH and do not prove a counterfactual buffer requirement, but no complete retained packet history establishes that 128 slots can hold every range for 45 ms. The separate trial must record buffered depth/span, the exact hard-capacity fallback, completion/expiry timing and resulting AU utility. Do not silently increase capacity or extend a deadline to force a positive result.

Prepare that policy as a separately selectable change only after this report. Compare fresh 10–15-minute stable sessions against current ON with matching game/scene/marking, then return to the reference if needed. Success requires authenticated positions admitted before abandonment, structural AU completion and recovery acceptance, plus a reproducible pause reduction without added latency or audio/control/closure regressions. Feed/native/RGB/presentation still need their own observations. Late return counts or successful local sends alone are not success.

To reverse the current NACK experiment immediately, close the application, install the preserved **0.7.33 OFF** package and launch freshly. For the pre-experiment checkpoint, use preserved **0.7.32**. Never swap packages during an open game session. No next installer or reorder modification is included in this report.

## Original evidence and review scope

Both paired exports are private numeric evidence, preserved byte-for-byte. The legacy filename prefix `0731` is a format convention; the internal product version and Build ID identify these captures as **0.7.33**.

| Mode | Original filename | Bytes | SHA-256 |
|---|---|---:|---|
| OFF | `xcloud4-trace-0731-1-51594802.bin` | 961,824 | `5de46e698a8f540eafe7013affa5576fb20f1c856ea2d980878d470d14bf625f` |
| OFF | `xcloud4-trace-0731-1-51594802.bin.progress.bin` | 8,011,776 | `023ae49021c166ad7afa3eee5d46fb4b336f8a0ee5458f0d8fd1027a9e42970c` |
| ON | `xcloud4-trace-0731-1-83332700.bin` | 2,127,840 | `fe7a71a10e375a97565356ea12f04cc13c4cb42ee405a012dbaedc7c99defa82` |
| ON | `xcloud4-trace-0731-1-83332700.bin.progress.bin` | 8,288,032 | `9823e27f70e91ae9b42a9a72c0029433a50cfe3187d88288a46c335997a3d82e` |

The final reader SHA-256 is `6a80cd5fdb4fd0a64bd742bc0bcbb0cee8e7ba14c7e615a44a7bf81942a6bb1f`. Two independent analyses agree on the OFF/ON phase/cadence counts, six ON ranges, 55 selected first authenticated returns, exact hole joins and lack of AU contribution. Immutable private summaries retain the underlying identities and omissions. Raw exports and Klog are not committed or added to public release assets.

The existing preparation evidence remains in [the release notes](RELEASE_NOTES_0.7.33.md): requested state/wire/production-handler/reader/monitor checks, native compilation, linked inspection, source/package correspondence and actual Claude Opus 5.5 source review. These were preparation checks; they did not predict console fluency. No new runtime tests, build or code change was performed for this documentary result.

An **actual Claude Code Opus 5.5** read-only review completed successfully on the frozen, sanitized derived-evidence bundle: seven Read calls, six files read in full, about 122.5 seconds, no errors/permission denials, observed model `claude-opus-5-5`, and unchanged input/bundle hashes. Its verified review agrees on separating local sends, authenticated returns, unusable late admission and lack of a causal OFF/ON fluency result. Its AUTH→worker delay observation was independently checked against the 55 per-position records above. This was a review of selected source extracts and derived console evidence, not operation of the console or a new runtime test. Claims that actual post-skip offsets prove counterfactual buffer capacity, that DATAGRAM absence proves physical loss, or that NEW counts establish provider FPS are not adopted. The bounded grace remains a proposed separate trial.
