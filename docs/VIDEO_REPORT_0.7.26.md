# XCloud4 0.7.26 — completed two-reader diagnostic capture

Version **0.7.26**, PS4 **APP_VER 00.96**, ran with the existing video owner and one copy helper. The console received a **1280 × 720, pitch 1280** native picture, passed the first-picture serial comparison and exported a numeric trace after successful session cleanup and helper join. The capture confirms recurring local intervals over 100 ms between matching NEW-picture flip observations. It does not establish their upstream cause.

This report supplements the original [release notes](RELEASE_NOTES_0.7.26.md), whose preparation-time status remains preserved. The PS4 interface remains Spanish.

## Complete telemetry-window selection

The selection pairs all four numeric reports (`video perf`, `video render`, `video worker`, `video copy`) for **82 complete intervals / 410.310017 seconds** after the first image. Startup/empty intervals and the transition interval with `check_window=1` are excluded. The final unreported partial interval is not included. Exit reason totals and the binary trace are analyzed separately; they do not have the same population as the interval selection.

| Local observation | Selected result |
| --- | ---: |
| Copies / completed RGB publications | 10,708 / 10,708 |
| New image draws / repeated draws | 10,707 / 7 |
| New image draws per second | 26.095 |
| Copy path: parallel / serial | 10,708 / 0 |
| Copy wall time: mean / maximum | 16.016 / 17.796 ms |
| RGB conversion: mean | 3.028 ms |
| Owner wait for helper: mean / maximum | 1.247 / 3.066 ms |
| Local ingress-to-dequeue residence: mean / maximum | 117.259 / 484.049 ms |
| Maximum interval between conversion completions | 820.968 ms |
| Successful present calls / maximum call duration | 10,714 / 18.325 ms |
| Ring high-water | 256 packets |
| Ring-full notifications / local packet drops | 154 / 258 |
| Local `lost` counter / discarded-open-AU counter | 320 / 2,413 |
| Consumed local keyframe-request notifications | 1,130 |

Publication/draw counts may differ at reporting boundaries. Copy wall time includes dispatch, span work and completion wait; owner/helper span times overlap. A present call's duration is not the interval between NEW presentations. The conversion-completion interval is measured before the mailbox and display, and must not be substituted for the binary trace's matching-flip interval.

The first-copy check reports **result 1, reason 0**, comparing **1,382,400 bytes** on the same leased native picture. Its separate check cost is **34.264 ms**. This verifies one buffer against the serial routine, not every picture or a new GPU-coherence guarantee.

## Actual trace export and coverage

The console's numeric export returned **0**. Two independent FTP retrievals produced identical **1,875,968-byte** files. An independent analysis checked binary length/hash, each window's event counts and duplicate ordinals against the retrieved bytes, with no conflicting duplicate records. Raw logs and the trace remain private.

| Trace accounting | Result |
| --- | ---: |
| Configured/observed copy readers | 2 |
| Exact closed NEW-picture intervals greater than 100 ms | 96 |
| Saved windows / closed intervals with retained detail | 12 / 13 |
| Storage-exhaustion observations | 83 |
| Trace-event admission drops | 5,009 |
| Monitor gate skips / unstable cadence snapshots | 2,220 / 18 |
| Event-capped / time-capped windows | 0 / 0 |
| Open interval at stop | 0 |

The exact closed-interval counter is maintained separately from trace admission. Multiple intervals can share a saved window, explaining 13 detailed intervals in 12 windows. The saved detail covers only part of the session. Admission drops are missing diagnostic records, not dropped RTP packets; exhausted window storage does not mean playback stopped. Rolling-history overwrites are expected bounded storage behavior.

## Positive recovery observations

All **13 retained detailed intervals** contain positive observations of valid RTP callbacks, local recovery state and resumed native-output/copy/RGB/draw activity. **12** contain an in-interval `WAIT_END`; the last retained wait-end observation precedes the matching NEW closure by approximately **30–42 ms**. This is temporal evidence of recovery in the saved sample. It neither classifies the other 83 intervals nor identifies a server, network, callback-scheduling or client-policy origin.

### First retained example: 732.995 ms

The first detailed interval includes **420 retained valid-RTP observations**, local reorder/marker/ingress resets and **34 waiting-recovery discards**. A retained `WAIT_BEGIN` occurs shortly before the interval. The following times are offsets from its prior successful NEW match:

| Retained observation | Offset |
| --- | ---: |
| First retained in-interval IDR observation | 674.068 ms |
| First retained in-interval native output | 696.537 ms |
| Retained wait-end transition | 696.543 ms |
| Explicitly linked tail RGB publication | 715.998 ms |
| Explicitly linked tail draw completion | 722.764 ms |
| Matching NEW-picture closure | 732.995 ms |

**First retained is not first actual.** Missing records can hide earlier activity. The explicitly linked tail RGB was published **16.997 ms before closure**; its presence late in the interval does not establish that a usable RGB picture was waiting throughout the pause, or implicate the main thread/VideoOut. The output, copy, RGB generation, draw and NEW match have explicit local links. An input AU is not attributed to that native output because the decoder ABI returns no output PTS.

## Retained paired timings

These distributions come from retained paired observations in saved windows plus final rolling history. They are **not whole-session distributions**, may be biased by missing detail, and must not be combined with the telemetry averages as if they shared a denominator.

| Retained local region | Samples | p50 | p95 | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Native Decode call | 808 | 7.935 ms | 12.605 ms | 36.973 ms |
| Selected-copy wall time, excluding reference check | 397 | 15.982 ms | 16.901 ms | 17.696 ms |
| RGB conversion | 398 | 3.027 ms | 3.084 ms | 3.184 ms |
| Draw operation | 374 | 5.591 ms | 5.693 ms | 6.292 ms |
| Submitted flip to matching observation | 389 | 10.092 ms | 17.119 ms | 18.272 ms |
| Local ingress arrival to queue pop | 10,416 | 23.926 ms | 410.159 ms | 484.049 ms |
| Valid AU marker to Decode entry | 816 | 0.006 ms | 0.011 ms | 0.061 ms |
| Leased native output to RGB publication | 393 | 19.061 ms | 19.982 ms | 20.821 ms |
| RGB publication to matching NEW observation | 387 | 16.997 ms | 24.967 ms | 26.805 ms |

Some begin/end pairs were missing or ambiguous: 20 Decode, 23 draw and 7 submitted-flip pairs were excluded. Region durations overlap and do not add up to end-to-end latency. Local matching observations are not physical scanout times.

## Exit totals and closure

The authoritative exit rows retain all reset reasons separately from selected-window counters. There are **2,414 discarded open AUs**: **2,163** while waiting for recovery, **96** at incomplete markers, **56** during coalesced ingress-gap resets and **99** during local reorder-hole decisions. There are also **22,420 normal submitted reset calls**, which discard no open AU. A reset can occur without discarding an AU. Overlapping damage-bit totals include **123 orphan FU continuations** and **109 discarded-timestamp observations**; adding damage bits would double-count some AUs.

The session finishes with **HTTP 200**, zero reported error and cleanup **0 / 200 / 0**. Helper join returns **0**, followed by successful numeric export. These markers verify the recorded session/helper/export paths; they are not independent evidence of the PS4 home screen or absence of a CE dialog.

## Interpretation and next experiment

Copy remains near the two-reader checkpoint's roughly 16 ms. Recovery activity and local queue residence remain visible. `lost` is a local/reorder counter, `damaged` counts discarded open AUs rather than visible damaged frames, and local PLI notifications coalesce before a **500 ms** dispatch gate. None independently counts physical wire loss or transmitted feedback.

Source FPS, provider capture time, output PTS age, Xbox RTT and controller-to-image latency remain unmeasured. Absence-based causal claims are disabled because trace coverage is incomplete. The retained data supports studying recovery and scheduling alongside copy throughput; it does not prove that four readers will remove the pauses.

The authorized next candidate is the [isolated four-reader experiment](FOUR_READERS_EXPERIMENT_0.7.27.md). It preserves this diagnostic baseline and recovery policy so its copy behavior can be compared with the same measurement definitions. Different cloud attempts still require comparable activity and repeated captures before attributing a speedup.
