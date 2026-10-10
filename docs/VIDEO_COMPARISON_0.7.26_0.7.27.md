# Video comparison: 0.7.26 and 0.7.27

This report compares two completed sessions on the owner's PS4. Version 0.7.26 uses two NV12 copy readers: the video owner and one persistent CPU helper. Version 0.7.27 uses four: the same owner and three persistent helpers. The decoder, buffer ownership, copy instructions, conversion, queues, recovery policy, audio, input and worker budget remain the same.

These are **observational results from different sessions**, not a controlled A/B/A experiment. They do not isolate the reader count from game activity, service behavior, scheduling or connection conditions.

## Owner observations and verified scope

The owner reports a substantial improvement with 0.7.27, uses Ethernet, and confirms closing the application without an error. The selected console log independently records successful remote cleanup, all three helper joins and trace export. Returning to the PS4 home screen was not independently observed by this analysis.

The first native picture in both selections is **1280 × 720, pitch 1280**. Although the application requests 960 × 540, these observations do not establish delivery of 540p. Each version's first valid copy passed the serial byte comparison for 1,382,400 bytes. That checks one picture, not every subsequent copy.

| Initial check | 0.7.26 | 0.7.27 |
| --- | ---: | ---: |
| Configured copy readers | 2 | 4 |
| Successfully created CPU helpers | 1 | 3 |
| First-picture serial comparison | PASS | PASS |
| Compared bytes | 1,382,400 | 1,382,400 |
| One-off comparison duration | 34.264 ms | 35.599 ms |

The report excludes the entire performance window marked as containing this check. The initial reference comparison is also excluded from the copy wall counter by the implementation.

## Complete post-check performance windows

Each selected interval contains all four numeric rows: `perf`, `render`, `worker` and `copy`. Both use a 16,000 µs worker budget and a four-Decode limit. The budget is checked between operations; it does not interrupt a native Decode or copy.

Weighted means below use **summed microseconds divided by the matching operation count**. They are not averages of interval averages. The final unreported partial interval is omitted.

| Observation | 0.7.26: two readers | 0.7.27: four readers |
| --- | ---: | ---: |
| Complete windows | 82 | 125 |
| Total selected duration | 410.310017 s | 625.577964 s |
| Native Decode calls / valid picture outputs | 22,155 / 22,155 | 36,090 / 36,090 |
| Parallel / serial copies | 10,708 / 0 | 22,422 / 0 |
| RGB publications | 10,708 | 22,422 |
| New / repeated draws | 10,707 / 7 | 22,422 / 9 |
| New draws per second of selected telemetry | 26.094903 | 35.842055 |
| Decode mean / maximum call | 8.319253 / 52.689 ms | 8.902911 / 30.472 ms |
| Copy wall mean / maximum | 16.015894 / 17.796 ms | 9.251069 / 11.233 ms |
| RGB conversion mean / maximum | 3.028287 / 3.215 ms | 3.044237 / 3.282 ms |
| Owner copy span mean | 14.760984 ms | 7.455171 ms |
| Helper wait mean / maximum | 1.247294 / 3.066 ms | 1.787645 / 3.752 ms |
| Draw mean | 5.588718 ms | 5.590233 ms |
| Present-call mean | 9.490124 ms | 8.462151 ms |
| Forced pending-output preservation copies | 0 | 0 |

The selected copy wall mean is **42.238198% lower** in 0.7.27. This describes one local processing stage. It is not a percentage improvement in overall gameplay, a causal estimate, or a controller-to-screen latency measurement.

New draws per second are **not server FPS or physical panel scanout rate**. Decode outputs can be coalesced before conversion. A successful present call can repeat an image. Present-call duration is different from the interval between new-image flip matches.

Copy wall includes dispatch and synchronization. Owner, helper and wait durations overlap and must not be added to it. In 0.7.26 `helper_us` measures one helper span; in 0.7.27 it is the sum of three overlapping spans. Their per-copy sums, 14.711941 and 22.266531 ms respectively, are neither elapsed copy wall nor CPU time and are not directly comparable as a performance measure.

## Queue and recovery counters in the same windows

These counters use the performance-window populations above, not the complete binary-trace population.

| Local observation | 0.7.26 | 0.7.27 |
| --- | ---: | ---: |
| Dequeued video packets | 218,429 | 344,216 |
| Ingress-to-dequeue dwell mean / maximum | 117.258891 / 484.049 ms | 7.877611 / 131.310 ms |
| Highest queue depth at a report | 248 | 15 |
| Queue high-water mark | 256 | 126 |
| Full-queue events | 154 | 0 |
| Full-queue events per selected second | 0.375326 | 0 |
| Producer gate contention | 104 | 126 |
| Consumer gate contention | 413 | 434 |
| Locally dropped packets | 258 | 126 |
| Local lost/reorder counter | 320 | 126 |
| Damaged/discarded AU counter | 2,413 | 1,077 |
| Damaged/discarded AU counter per selected second | 5.880919 | 1.721608 |
| Local keyframe requests | 1,130 | 1,019 |
| Local keyframe requests per selected second | 2.754015 | 1.628894 |

Lower queue dwell and faster copy coincide with the owner's reported improvement. These sessions do not establish that all of the difference was caused by the reader change.

`lost`, reorder decisions and AU discard counters do not independently prove physical network loss or visible corruption. Damage-bit categories overlap. Normal submitted-AU resets are not damaged AUs. A local PLI request is demand for recovery; requests can coalesce and pass through the existing dispatch gate, so request count is not RTCP transmission count. Queue dwell measures local residence only, not the age of a picture sent by the provider.

## New-image presentation gaps

The trace counts an interval longer than **100 ms between actual local new-image flip-match observations**. This is a diagnostic pause threshold, not end-to-end latency or a measurement of physical scanout.

Whole-session gap counts use the span from the first to the last new-image observation. That span includes portions omitted from the complete post-check telemetry windows.

| Complete trace accounting | 0.7.26 | 0.7.27 |
| --- | ---: | ---: |
| First-to-last new-image span | 414.534041 s | 633.468897 s |
| Exact closed intervals over 100 ms | 96 | 83 |
| Descriptive count per minute of that span | 13.895119 | 7.861475 |
| Saved windows / fixed capacity | 12 / 12 | 12 / 12 |
| Window-qualified closed gaps with details | 13 | 14 |
| Additional closed-gap metadata in final rolling history | 1 | 0 |
| Diagnostic records not admitted | 5,009 | 11,423 |
| Attempts without space for another window | 83 | 69 |

The smaller count must be interpreted with the different session durations. These rates are descriptive and do not turn separate sessions into a controlled comparison. Diagnostic admission drops are **not multimedia packet losses**.

The bounded trace stores a rolling history and at most 12 detailed windows. It does not store a list or histogram of every presentation interval or every long gap. Therefore **whole-session gap p50, p95 and maximum are unavailable**.

The following durations are exact for the retained, window-qualified completed gaps only. Percentiles use nearest rank. This subset is capped and can be unrepresentative; it cannot estimate the corresponding percentiles of all 96 or 83 gaps.

| Retained detailed-gap population | 0.7.26 | 0.7.27 |
| --- | ---: | ---: |
| Number of completed gaps | 13 | 14 |
| p50 duration | 181.975 ms | 166.037 ms |
| p95 duration | 732.995 ms | 549.016 ms |
| Largest retained duration | 732.995 ms | 549.016 ms |

## Positive observations during the detailed gaps

Every entry here means that at least one matching event was retained inside that gap. Event absence cannot be used to conclude that a stage stopped: both traces have admission drops, exhausted window capacity and unstable cadence snapshots.

| Positive event or observation | 0.7.26: 13 detailed gaps | 0.7.27: 14 detailed gaps |
| --- | ---: | ---: |
| Valid RTP reception | 13 | 14 |
| Valid complete-AU observations | 13 | 14 |
| Independent valid native output | 13 | 14 |
| RGB publication | 13 | 14 |
| Waiting-for-recovery reset | 13 | 14 |
| Local keyframe request | 13 | 14 |
| Local reorder-hole decision | 11 | 12 |
| Local queue drop | 3 | 2 |
| Keyframe-wait end | 12 | 13 |

In each of the 14 detailed 0.7.27 gaps, exactly one RGB publication is retained near the end: its last publication precedes the closing flip match by **7.981–25.057 ms**. This documents recovery/progress near closure, **not continuous RGB production throughout the pause**. The reorder and recovery events are temporal co-occurrences, not a proven cause.

The native decoder does not return PTS. AU submission, Decode call and native-output identities remain separate. A valid output is never assigned to the most recent compressed input merely because it was returned during that Decode call. A marker observation also does not imply that the AU was submitted while waiting for an IDR.

## Selection, integrity and reproducibility

The 0.7.27 selection freezes exactly the first **382,144 bytes** of the private console log, ending on a complete line. The selected session begins after its 0.7.27 banner; the first check, remote close and trace-dump markers are unambiguous. Its 125 selected telemetry groups exclude earlier replay/backlog and subsequent games/reconnections in that prefix. The live capture was not stopped or modified.

The existing offline analyzer was used on the real binaries. Aggregate arithmetic was independently recomputed from the numeric rows and checked against the stored 0.7.26 aggregate. No synthetic traces, application execution, builds or console probes were used in this analysis.

| Evidence | Bytes | SHA-256 |
| --- | ---: | --- |
| 0.7.26 binary trace | 1,875,968 | `7476d54c61b05d2670aaaa0b94cf43b5632ad98b08581de95076e92709fafd2a` |
| 0.7.27 binary trace | 2,083,168 | `4804a6f27ec85cb813abf5d4869f10b07c4671e74e9b98dd6abb0067e926a61c` |
| Frozen 0.7.27 log prefix | 382,144 | `4b42c8f3d27608c4d2c9f8ed8b169d1072216d8e983047c020af219ac3511f00` |

Raw logs and binary traces remain private. This document contains derived numeric observations, not payloads, credentials, account identifiers, network addresses or RTP timestamps.

Public source checkpoints:

- [0.7.26 release](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.26), commit `cfcf6664752dbf065c67365123ed900b129809bd`.
- [0.7.27 release](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.27), commit `38c31ada7b77aaae165fb5106be5743743b651d1`.
- [0.7.26 video report](VIDEO_REPORT_0.7.26.md).

## Pending product work and useful feedback

The console catalog reported **2,734 validated received entries but retained only 128** because of its local cap. Received entries can repeat; that is not a claim of 2,734 distinct titles. The prepared 0.7.28 change raises retention to 4,096 distinct validated titles and adds a revision-based snapshot to avoid repeatedly copying the enlarged catalog during gameplay. Console confirmation of that checkpoint is pending for this comparison. Existing Store naming remains limited to the first 32 titles in batches of eight; other entries can display their identifiers.

The owner also requested hiding the persistent playback menu, with help shown while holding L1 + R1. Search by matching titles and grouping by service-confirmed access are now authorized for a later isolated checkpoint; cover art follows afterward. An unconfirmed-access label does not establish that purchasing a game is required. A previously reported purchase-restricted title is owner context and is separate from the transmitting sessions compared here.

A requested connection-quality check also remains investigation/planning work. HTTPS signaling reachability must be distinguished from the selected multimedia session's UDP quality. This report does not contain an implemented quality check or measurements from one.

Useful feedback should identify a small, reversible experiment and the evidence it needs:

1. How can arrival gaps, local reorder/AU recovery, IDR waiting and post-output stalls be distinguished without making claims from missing trace records?
2. What bounded sampling or summary would preserve a more representative gap-duration distribution without per-packet printing?
3. Which recovery change is justified by positive evidence, and how would it preserve H.264 references, FU handling, SPS/PPS, wrap behavior and clean teardown?
4. What matched A/B/A session and scene would separate copy cost from other effects, while recording actual dimensions, queue dwell and new-image gaps?
5. Which real xCloud endpoint/protocol can support a meaningful preflight check without presenting HTTPS response time as multimedia RTT, jitter or loss?

Further changes must preserve buffer leases, audio, input and failure-safe shutdown. These observations alone do not establish a residual-pause root cause or justify a recovery change.
