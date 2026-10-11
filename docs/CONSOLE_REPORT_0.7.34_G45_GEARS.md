# XCloud4 0.7.34 - independent G45 Gears-labeled console pass

## Result and scope

The owner reports at least two perceptible stutters in a separate game labeled **Gears E-Day**, with audio, controller input, catalog return and exit working correctly. The official game/scene identity is not independently verified. This fresh pass used **G45: NACK ON / grace 45 ms / rumble OFF**, runtime source ID `54d5793d1b50a7ea24ba73e79b258577f51511c0`, suffix `-n1-g45-r0`.

The complete manual stable population contains **five local NEW-image intervals >100 ms in 612.982958 seconds (10.216383 minutes): 0.489410/min**. Loading contains two additional intervals. Four stable pauses overlap missing-sequence/recovery episodes; the 101.117 ms pause remains insufficiently explained. These are local NEW observations, not physical scanout or controller-to-image latency.

Unlike [the preceding G0/G45 pair](CONSOLE_REPORT_0.7.34_G0_G45.md), this pass exercised NACK and two grace holds. It does **not** establish that graphical complexity caused the missing positions or that grace reduced pauses: the game/exposure changed, and there is no matching grace-off pass under equivalent conditions.

## Preserved configuration and coverage

Requested settings remain **960 x 540 / maxFPS 30 / 5000 kbps**, four copy readers, NEW/MPSC256, 16 ms/four Decode feeds and reorder **128 entries / depth 32 / 25 ms**. All retained native geometry is **1280 x 720, pitch 1280**. Recovery, SDP and RTX remain unchanged; no rumble or worker redesign is part of this pass.

Both original numeric exports were read twice with identical bytes. Build identity comes from the progress header; the paired legacy export agrees on local session, configured readers and first/last NEW. Sampler start/join return 0 and lifecycle flags 15 are retained. These flags describe sampler/monitor/queue lifecycle, not a universal successful-close result. Successful numeric exports and process deletion are separately observed in Klog; the owner confirms a no-error exit.

| Population | Observation |
| --- | ---: |
| Stable non-crossing intervals | 18,305 |
| Stable interval p50 / p95 / max, ms | 33.053 / 49.929 / 416.999 |
| Marked loading exposure, seconds | 82.245974 |
| Loading non-crossing intervals >100 ms | 2 |
| Stable non-crossing intervals >100 ms | 5 |
| Retained whole-session cadence intervals | 21,154 |
| Independent sampler rows / missed periods | 7,675 / 0 |
| Maximum sampler lateness, ms | 1.700 |
| Queue admission rejects | 0 |

Cadence/phase ledgers have zero omissions or clock/identity errors, with no open pause at stop. One phase-crossing interval is outside the stable/loading populations. Transition exposure is 2 microseconds with no qualifying interval. Unmarked loading/cinematics remain in the owner's manual phase; phase labels do not establish identical scenes.

Critical retention is **2,276 / 2,284 attempted records**: eight omissions (six LATE_POSITION, one NACK_AUTH_RETURN and one NACK_GRACE). Sequence-observer gate/source omissions are zero. Critical retention therefore supports exact sequence-gap totals, but not complete per-position detail. It also does not establish complete functional control-inbox or socket/transport coverage.

General detail is incomplete: **14,857** progress-event admissions were omitted and **5,857,492** history records overwritten; 76 actor snapshots, one queue snapshot and 33 sampler-detail gate observations were unstable/omitted. The independent sampler missed no rows. Critical windows have no duplicate opens; 64,136 context records were omitted. None of those populations is interchangeable with packet loss.

Timing-valid stable samples comprise 6,130 rows; only **413** have a valid nonempty oldest-packet age. Those ages have p50/p95/max **0.866 / 3.704 / 31.639 ms**, with sampled maximum queue depth 53. These sparse sampled observations do not establish a continuous peak or the full age distribution during a hold.

## Stable-pause chronology

Times are relative to the recorded stable mark. They are not wall-clock timestamps. The first sampled lead is an investigation lead, not a causal classification; detailed timing remains incomplete.

| Stable elapsed | NEW interval, ms | Retained evidence |
| --- | ---: | --- |
| 01:24.187 | 416.999 | R2; admitted grace ends on capacity, followed by recovery. Earliest sampled lead has no callback progress; this does not distinguish server, network or earlier delivery delays. |
| 03:52.635 | 101.117 | No corresponding retained NACK range or recovery episode. Operation detail is insufficient to assign the remaining interval to Decode, copying or presentation. |
| 04:48.926 | 334.021 | R3; grace offer rejected as stale source. Later authenticated requested positions are behind worker expected. Sampled lead shows queue admission without valid-AU progress. |
| 07:47.939 | 416.953 | R4; stale-source offer and late worker returns. Sampled lead shows queue admission without valid-AU progress. |
| 09:28.723 | 349.963 | R5-R7; stale-source offers, three ordinary depth-32 skips and recovery. One matched recovery lasts 202.463 ms, ending at AU-feed acceptance, not displayed IDR. |

Loading intervals are **416.997 ms and 1301.979 ms**. R1 occurs before the first loading NEW interval; its accepted packet/AU observations are startup evidence. The other loading pause has a sampled no-callback-progress lead and no matching retained NACK request. Neither loading interval belongs in the stable pause-rate numerator.

## Missing positions and NACK outcomes

The video's seven sequence gaps cover **102 forward-skipped positions**, already observed at DATAGRAM after positive UDP reads. This does not identify loss before that read or distinguish server, network and OS queues. Upstream counters record 112 backward observations before SRTP and 102 after successful SRTP validation, alongside ten replay failures. These aggregate observations do not establish individual replay-packet identity or count those failures as authenticated returns. No authentication/decryption failure is observed in the instrumented failure counters.

Seven selected NACK ranges cover 102 positions and traverse the existing SRTCP route with successful local-send results. The watch tracks **88 first authenticated arrivals**, with 87 individual AUTH_RETURN records retained. An observer state-gate omission retires the other **14 watched positions**. All fourteen are independently observed later at DATAGRAM, SRTP_VALID and APP_CALLBACK before R2's capacity release and deadline; their missing NACK tickets prevent the same request/result correlation. A later arrival does not establish causal retransmission, server receipt or RTT.

| Range | Selected positions | Retained AUTH positions | Worker outcome | Grace outcome |
| --- | ---: | ---: | --- | --- |
| R1, loading | 10 | 10 | 1 STORED; 9 behind expected | Held; deadline release |
| R2, stable | 14 | 14 independent SRTP_VALID; no retained NACK tickets | Worker correlation unavailable after watch retirement | Held; capacity release |
| R3, stable | 21 | 21 | All behind expected | Stale-source offer |
| R4, stable | 18 | 18 | All behind expected | Stale-source offer |
| R5, stable | 9 | 9 | All behind expected | Stale-source offer |
| R6, stable | 6 | 6 | All behind expected | Stale-source offer |
| R7, stable | 24 | 23 | All 24 behind expected; one AUTH omission inferred by accounting | Stale-source offer |

The retained selected first-arrival ranges after successful LOCAL_SEND_RESULT are approximately **0.807-1.294 ms (R1), 30.966-32.526 ms (R3), 33.291-34.177 ms (R4), 44.013-44.405 ms (R5), 43.504-43.737 ms (R6), and 42.766-43.877 ms (R7)**. These are recorded result-to-arrival observations, not RTT or confirmed NACK-response times. R2 has no valid correlated return-time distribution after watch retirement. R7's stated distribution covers only 23 retained NACK AUTH_RETURN positions; the unretained position could widen it. Assigning the single overall NACK AUTH_RETURN omission to R7 is inferred from range/counter accounting, not an observed missing event. NACK_REORDER_RESULT is separately complete at 88/88; LATE_POSITION is a different, incompletely retained stream. Some later R5-R7 AUTH observations are at or after detection +45 ms, so source rebinding alone would not guarantee acceptance within the unchanged deadline.

**R1:** all ten callbacks arrive before detection +45 ms. Only one position is STORED. The nine others wait approximately 72 ms between callback and worker extraction, then fall behind expected. The recorded HOLD-to-release span is 73.227 ms; the absolute deadline was not renewed, but enforcement occurred when the worker next evaluated it, 28.586 ms after that deadline. This is not evidence of a continuous timer-driven wait or a guaranteed scheduling bound. Three AU-stage observations represent **one** startup AU: packet contribution, structural completion and recovery acceptance. They do not represent three repaired pictures, a displayed repaired image or stable-game utility.

The initial worker operation contains Decode **23.305 ms**, selected COPY_BEGIN-to-COPY_END **10.448 ms** (the separate copy counter reports 10.056 ms), an additional **34.290 ms** between COPY_END and BYTECHECK_END, and conversion **3.006 ms**. Current code performs a serial reference copy and byte comparison after the selected copy, outside its copy counter. That extra startup span helps account for delayed worker evaluation; it is not the complete callback-to-worker delay and does not explain later stable pauses automatically.

**R2:** the hold ends after 38.197 ms on capacity, before the absolute deadline, with 114 buffered entries and furthest distance 127; the next incoming position is outside the 128-entry window. All fourteen missing positions appear at SRTP_VALID from 355.639758 to 355.640367 seconds and APP_CALLBACK from 355.639772 to 355.640381 seconds, before capacity release at 355.641075 and deadline at 355.647743. These are local monotonic observations. They establish authenticated arrival despite watch retirement, not worker acceptance, useful AU contribution or causally identified retransmission. This demonstrates the existing bounded-capacity fallback and does not justify extending the timer.

## Why later offers did not hold

The NACK observer records one state-gate omission, one discontinuity, two source starts and **zero SSRC changes**. Five later offers are reported `stale_source`. Their extended sequence values are 65,536 below the independent upstream observer's values; the first two ranges agree with it.

Current code marks an omitted NACK observation dirty. The next successful state-gate holder, either incoming processing or dispatch polling, retires all pending watches and unbinds the observer. The next sequence observation starts its new sequence epoch; that rebase is separate from reorder arrival clocks and never renews the grace deadline. The grace worker explicitly accepts sequence epoch 1 only. Together, source code and recorded counters support an observer rebase followed by preventive rejection of later offers, rather than a new SSRC. The actual grace-control sequence-epoch value is not exported directly, so that value is not claimed as runtime introspection. Configuration/request epoch fields are distinct from that sequence epoch. Functional grace-inbox omissions remain zero.

This means the G45 label did not provide active 45 ms holding for all later stable gaps. The current scope fails closed after the observer rebase; a comparison that assumes uninterrupted grace would be misleading.

## AU stages and interpretation limits

Whole-session counters distinguish **22,239 structurally complete AUs**, **61 rejected by recovery** (60 missing IDR and one missing IDR/SPS/PPS), and **22,178 accepted for Decode feed**. The recovery-filter rejects are not 61 assembly failures. Separate discarded-open-AU reasons include marker-incomplete 1, waiting-recovery 61 and reorder-hole 4. Damage bits overlap and are not an additive loss count.

Local PLI requests, actual dispatch attempts/results, AUTH, callback, worker STORED, AU completion, recovery acceptance, native output, RGB and NEW remain distinct observations. The decoder returns no output PTS. No accepted input AU is automatically associated with a later native image or presentation. The single accepted requested position supplies no demonstrated stable-game fluency improvement.

## Proposed single next change

Before another grace effectiveness comparison, prepare a separate small change that **safely retires old grace state and rebinds the worker to a validated observer sequence rebase for the same current session/video source**, including validation of the extended-sequence origin. Keep the 45 ms deadline, 128-entry/depth-32/25-ms reorder, MPSC256, four readers, work budget, stream request, recovery, SDP and RTX unchanged. Preserve fail-closed behavior for unvalidated controls, old sessions and source changes; do not bind from an arbitrary offer, simply accept every epoch or transplant old pending masks.

That proposal is not implemented here. It needs wrap/rebase/stale-control/cancellation fixtures and a fresh comparable grace-off/grace-45 pair in the same intended game/scene. Capacity and worker extraction delay remain separate limitations; fixing the binding would not prove that 45 ms is sufficient or repair the unexplained 101 ms pause. No timer increase, extra queue memory, worker redesign or physical-rumble trial is mixed into this report.

Original captures and detailed numeric identifiers remain private. This checkpoint adds evidence only, preserving the [0.7.34 installers and original source assets](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.34). Independent numeric/source reviews and their actual scope are retained with the private verification receipts.

## Review scope

Actual **Claude Opus 5.5** read four frozen bundle files in full: derived facts, this report before review clarifications, selected source semantics and their manifest. Its review supports the measured cadence and distinctions between deadline, arrival, worker acceptance, AU stages and presentation. It identified the incoming-or-poll retirement nuance and the R7 distribution/accounting qualifications incorporated above. It did not inspect raw captures, execute tests, compile, operate the console or verify physical video/rumble.

The capacity occupancy/span, three depth-32 advances, original-file pairing, source-to-freeze correspondence, shared-clock/lifecycle semantics and the explicit owner question/reply are supported by separate source/numeric checks outside that compact Claude bundle. Runtime source ID is a content digest, not the later documentation Git commit. Numeric reparse and selected-source verification remain distinct from a full native end-to-end test or a controlled grace-performance comparison.
