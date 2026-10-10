# XCloud4 0.7.32 — reception boundary console report

## Verified result

On **2026-10-10**, the owner completed one game session from a fresh XCloud4 **0.7.32** launch, returned to the catalog and closed the application. The owner reports that pauses remain, the experience feels better, and audio, controls, menu behavior and closure worked correctly. These observations do not establish a controlled performance improvement.

The paired exports were retrieved twice with identical bytes. Their internal product version and runtime Build ID match the published package. Both export operations returned `0`; the monitor start and join results are `0`, and no pause remained open at stop. The bounded private Klog capture completed with one continuous connection.

**All 14 video sequence discontinuities, covering 202 absent sequence positions, are already observed at the positive UDP socket-read boundary, DATAGRAM.** The same ranges appear in the 12 instrumented RTP stages through APP_CALLBACK. Two audio discontinuities cover three positions and have the same boundary pattern. There are no retained or cumulative late, backward or duplicate observations, no range evictions, no half-wrap ambiguities, and no sequence-observer omissions in this pass.

This moves the observed boundary earlier than the 0.7.31 application callback. It **does not identify physical Internet loss or an Xbox server fault**. NIC/kernel/socket-queue behavior before the positive read, the network path and the sender remain indistinguishable. The datagram header is an unauthenticated candidate; downstream SRTP-valid observations corroborate the received source/sequence tuples. The missing positions themselves never supplied authenticated packets to inspect.

The critical ledger is complete: **643 attempted / 643 retained, zero gate omissions and zero ring overwrites**. Detailed per-packet/application windows remain incomplete. This distinction matters for every negative conclusion below.

## Binary, configuration and dependencies

| Item | Verified value |
|---|---|
| Product / SFO APP_VER | 0.7.32 / 01.02 |
| Runtime Build ID | `152d863f3e21a2111101fcccd6c9094c52eb57a4` |
| Runtime source checkpoint | `f3e14bd63488a0d551c72c1f744c06807adab085` |
| Package SHA-256 | `78798b772caf562bb9bd423cb4bd868188e7dd7d39e6a2b0a1e5d5fbbe52bb98` |
| Request | 960 × 540, maxFPS 30, 5000 kbps |
| Retained delivered geometry | 1280 × 720, pitch 1280; 210 observations, all equal |
| Ingress | NEW/MPSC, 256 entries |
| Copy readers | Four persistent readers |
| Worker | 16 ms budget, four Decode feeds per batch |
| Reorder | 128 entries, depth 32, 25 ms wait |
| libdatachannel | 0.24.6, `bdc5ff28e9d3b863144c94a677ecf5bf043aaf15` |
| libjuice | 1.7.4, `b89c792e3612faf2f12cf35bcc56857313a06be3` |
| libsrtp | 2.8.0, `d33b8ffb1491a0b4b58a206889f09800cf7310ab` |
| Mbed TLS | 3.6.7, `068ff080b369adfac81509f9b57b2afabaf82dc5` |

The remaining pinned dependencies, reproducible numeric hooks and receive-chain boundaries are documented in [the reception diagnostic plan](RECEPTION_DIAGNOSTICS_0.7.32.md). This report adds results; it does not alter the package, release source archives or functional settings. The decoder still supplies no output PTS. The observed 720p geometry does not prove a negotiated provider FPS value.

## Exposure and comparison

| Manually marked population | Active exposure, seconds | Closed NEW intervals >100 ms | Per minute |
|---|---:|---:|---:|
| loading | 65.299030 | 3 | 2.756549 |
| stable | 630.924914 | 10 | 0.950985 |
| transition | 392.120036 | 1 | 0.153014 |

The combined stable denominator is **630.924914 seconds / 10.515415 minutes**: the first stable epoch lasts **629.149994 seconds**, the second only **1.774920 seconds**. The trace records four manual toggles: stable at 01:05.283 from first NEW, transition at 11:34.433, stable at 17:59.700 and transition at 18:01.475. These are observed button marks, not inferred scene boundaries. The owner states that cinematics were not marked; any such cinematic inside a stable epoch remains in that denominator. The long transition cannot be relabeled as stable gameplay or assigned to specific cinematics from this record.

Entire first-to-last NEW exposure is **1088.325016 seconds / 18.138750 minutes**, with **14 intervals >100 ms / 0.771828 per minute**. The separately integrated manual active exposure is 1088.343980 s; its boundaries differ slightly from the first-to-last NEW endpoints. Four phase-crossing intervals are excluded from phase-specific populations. Cadence and phase ledgers are complete, with no clock/identity errors.

| NEW interval population | Count | p50 | p95 | Maximum |
|---|---:|---:|---:|---:|
| Stable, non-crossing | 18,788 | 33.044 ms | 49.952 ms | 500.984 ms |
| All | 32,914 | 33.036 ms | 49.992 ms | 1251.063 ms |

These are intervals between local matches of new RGB generations, not end-to-end video age, control latency, provider FPS or physical scanout times. The longest interval is in loading, outside the stable denominator.

The [0.7.31 report](CONSOLE_REPORT_0.7.31.md) recorded 30 stable pauses in 27.110969 minutes, **1.106563/min**, with stable p50/p95 **33.036/49.952 ms**. This run recorded **0.950985/min** with nearly identical p50/p95 and retained copy costs. The exposures, scene boundaries and upstream gap populations differ; the small rate difference and the owner's impression do not prove that instrumentation improved playback or that its overhead is zero. The controlled 0.7.30 NEW queue result remains the functional baseline.

## Reception populations and scheduling

Only two RTP source identities are observed: **V0**, original H.264 PT102, and **A0**, original Opus  PT111. Raw SSRCs remain in the private evidence. There are no RTX-normalized observations, TCP-framed observations or TURN decapsulation observations in this run.

| Stage | V0 packet observations | A0 packet observations | V0 skipped positions | A0 skipped positions | Maximum observed previous-stage delay, ms |
|---|---:|---:|---:|---:|---:|
| DATAGRAM | 600541 | 54449 | 202 | 3 | not observed |
| ICE_DELIVER | 600541 | 54449 | 202 | 3 | 0.178 |
| DTLS_QUEUE_ACCEPT | 600541 | 54449 | 202 | 3 | 0.169 |
| DTLS_QUEUE_POP | 600541 | 54449 | 202 | 3 | 3.969 |
| SRTP_INPUT | 600541 | 54449 | 202 | 3 | 0.141 |
| SRTP_VALID | 600541 | 54449 | 202 | 3 | 0.231 |
| PEER_DISPATCH | 600541 | 54449 | 202 | 3 | 0.159 |
| TRACK_INCOMING | 600541 | 54449 | 202 | 3 | 0.158 |
| TRACK_QUEUE_ATTEMPT | 600541 | 54449 | 202 | 3 | 0.177 |
| TRACK_DELIVERED | 600541 | 54449 | 202 | 3 | 0.168 |
| APP_CALLBACK | 600534 | 54448 | 202 | 3 | 0.193 |
| RTX_NORMALIZED | 0 | 0 | 0 | 0 | not observed |
| TCP_FRAMED | 0 | 0 | 0 | 0 | not observed |
| PEER_HANDLER_OUT | 600541 | 54449 | 202 | 3 | 0.260 |
| RTCP_VALID | 0 | 0 | 0 | 0 | 0.196 |
| TURN_DECAP | 0 | 0 | 0 | 0 | not observed |

Socket totals also contain non-RTP/control traffic: 705,478 datagram observations versus 654,990 candidate RTP header observations. ICE totals are 704,822; RTCP-valid totals are 15,797. These different populations are not media-loss counters. No transport-delay event exceeded the 10 ms critical threshold; the largest measured DTLS queue delay was **3.969 ms**, with zero max-delay CAS omissions. Stage timing follows individually timestamped observations and is not one atomic global snapshot.

The final APP_CALLBACK source totals are **seven video positions and one audio position shorter** than TRACK_DELIVERED; their high-water sequences also end seven and one positions earlier. This is an end-of-capture tail difference, separate from the 14/2 interior gap ranges. The application observer runs after the callback ownership guard, which can return before observation during close. That branch is a plausible explanation, but it has no per-position export here; the exact disposition of those final eight positions remains unproven. Do not add them to the 202/3 interior skipped-position totals or call them physical loss.

All **600,534** admitted video callbacks are extracted, inserted into reorder and emitted. Application rejection counters are zero, and the final ingress and reorder snapshots are empty. No reorder window jump or foreign-source/PT REORDER_DROP is recorded. REORDER_DROP covers the existing source/PT filter; it is not a universal counter of late or otherwise missing packets.

The only nonzero instrumented transport branch counter is **126,478 SOCKET_WOULD_BLOCK** observations. An empty nonblocking read is not a lost-packet report. Instrumented socket-error, DTLS-queue-full-or-stopping, SRTP authentication/decryption/replay, RTP parse, routing and Track-queue rejection branches are zero. This applies to the covered branches and source populations; unhooked conditions remain unknown.

Still unobserved: kernel/NIC queues before UDP read, fatal poll/POLLNVAL, raw TCP reads/errors before framing, weak-Track expiry, ICE receive-callback exception slot 24 and socket fairness-continuation slot 26. The latter two raw zeros are explicitly **unavailable measurements**, not measured absence. The process-global diagnostic sink reports one binding and no thread-slot omissions; asynchronous transport ownership across multiple successive sessions remains outside this first-session scope.

## Chronology of every retained sequence discontinuity

Time is relative to the first NEW presentation match. Ranges are inclusive 16-bit wire positions, interpreted using the retained stage-local extended sequence epochs. Both sources start with matching first sequence epochs across these stages, and no resynchronization or ambiguity is observed. Every row has the same source/PT/extended expected/received tuple at all 12 stages, closely spaced observation times, and a matching downstream authenticated received header. This is strong correlation of the **gap observations**, not proof of the path of each absent packet.

For **every row**, the first observed boundary is DATAGRAM; no later missing-position appearance is recorded at any instrumented stage. Watched ranges never evict, observers never omit, and the full critical timeline is retained. This bounds the absence within this capture; it does not prove that the sender never transmitted those positions or that they could not appear after observation ended.

| Gap ID | Source | Socket observation time | Absent wire range | Received wire seq | Positions | Socket-to-app observation, µs | Overlapping presentation pause IDs |
|---:|---|---:|---|---:|---:|---:|---|
| 1 | V0 | 02:55.483 | 26910–26916 | 26917 | 7 | 119 | 5 |
| 2 | V0 | 03:15.403 | 38285–38300 | 38301 | 16 | 129 | 6 |
| 3 | V0 | 03:33.623 | 48499–48516 | 48517 | 18 | 173 | 7 |
| 4 | A0 | 03:33.623 | 11570–11570 | 11571 | 1 | 721 | 7 |
| 5 | V0 | 04:32.854 | 16290–16329 | 16330 | 40 | 351 | 8 |
| 6 | V0 | 04:33.154 | 16480–16487 | 16488 | 8 | 78 | 8 |
| 7 | V0 | 04:58.734 | 30401–30421 | 30422 | 21 | 166 | 9 |
| 8 | V0 | 06:54.046 | 29982–29989 | 29990 | 8 | 135 | 10 |
| 9 | V0 | 06:54.046 | 29992–30001 | 30002 | 10 | 375 | 10 |
| 10 | V0 | 07:11.826 | 40012–40022 | 40023 | 11 | 132 | 11 |
| 11 | A0 | 07:11.826 | 22478–22479 | 22480 | 2 | 349 | 11 |
| 12 | V0 | 07:11.826 | 40028–40055 | 40056 | 28 | 440 | 11 |
| 13 | V0 | 07:11.826 | 40064–40072 | 40073 | 9 | 672 | 11 |
| 14 | V0 | 07:11.888 | 40074–40079 | 40080 | 6 | 76 | 11 |
| 15 | V0 | 11:20.259 | 50262–50269 | 50270 | 8 | 116 | 13 |
| 16 | V0 | 13:18.211 | 50217–50228 | 50229 | 12 | 365 | 14 |

No individual range crosses the 16-bit wrap; the extended high-water progresses through wraps during the session. The 192 per-stage sequence-gap events are **16 source episodes × 12 observations**, not 192 distinct gaps or 2460 lost packets. Video totals are **14 episodes / 202 positions**; audio totals are **2 / 3**.

## Presentation pauses and recovery chronology

| Pause ID | Start from first NEW | Interval, ms | Marked epoch | Sequence-gap IDs inside interval | Overlapping recovery wait, ms | First retained sampled lead |
|---:|---:|---:|---|---|---|---|
| 1 | 00:26.594 | 1251.063 | loading | none | none | No callback progress in first retained sample span |
| 2 | 00:47.998 | 132.963 | loading | none | none | Indeterminate at 100 ms sampling |
| 3 | 01:00.027 | 100.198 | loading | none | none | Indeterminate at 100 ms sampling |
| 4 | 02:39.693 | 100.154 | stable | none | none | Indeterminate at 100 ms sampling |
| 5 | 02:55.342 | 417.988 | stable | 1 | 244.709 | No callback progress in first retained sample span |
| 6 | 03:15.196 | 400.986 | stable | 2 | 152.690 | No callback progress in first retained sample span |
| 7 | 03:33.515 | 267.047 | stable | 3, 4 | 131.484 | Admission continues; no accepted AU progress |
| 8 | 04:32.707 | 500.984 | stable | 5, 6 | 316.433 | No callback progress in first retained sample span |
| 9 | 04:58.650 | 284.022 | stable | 7 | 165.209 | Admission continues; no accepted AU progress |
| 10 | 06:53.948 | 385.051 | stable | 8, 9 | 248.582 | Admission continues; no accepted AU progress |
| 11 | 07:11.700 | 416.947 | stable | 10, 11, 12, 13, 14 | 227.254 | Admission continues; no accepted AU progress |
| 12 | 08:13.229 | 100.175 | stable | none | none | Indeterminate at 100 ms sampling |
| 13 | 11:20.148 | 300.964 | stable | 15 | 149.232 | Admission continues; no accepted AU progress |
| 14 | 13:18.017 | 468.028 | transition | 16 | 244.632 | Admission continues; no accepted AU progress |

Eight stable pauses and one transition pause overlap reorder-hole recovery. Fourteen reorder-hole observations cover the fourteen video sequence discontinuities; several occur during one recovery episode. Each of the **nine playback recovery waits** has reason 9, reorder hole. Their durations are **131.484–316.433 ms**, total **1.880225 s**. The end event means a recovery AU was accepted for feeding, not that its IDR had already reached the display. Pauses can begin before the first gap is recognized and can finish after accepted feed; those portions are not automatically decoder or network latency.

For example, pause8 lasts **500.984 ms**. V0 gaps of 40 and 8 positions are observed at 04:32.854 and 04:33.154. Recovery spans **491211138–491527571 µs**, **316.433 ms**. The last NEW before this episode is491061828 µs, and the next is491562812 µs: about149.310 ms before recovery begins and35.241 ms after feed acceptance. These are distinct local observations; there is no output PTS association to assert that the closing picture is the recovery AU.

The initial recovery flag 0x8000 lasts **61.288020 s**, from the initial waiting sentinel until initial AU acceptance. It is startup preparation/first-media wait and is excluded from the nine playback waits. It must not be counted as a 61-second in-game recovery.

Pause 1, **1251.063 ms in loading**, has no overlapping retained source sequence gap or recovery. The interior monitor spans show no video callback progress for approximately 1.10 s. That first observed stall is at the callback or earlier; the absence of a sequence jump does not establish the absence of UDP reads during the pause, nor distinguish legitimate sender inactivity from local transport scheduling. There is no per-stage periodic socket counter history in this export. Pauses 2, 3, 4 and 12 remain indeterminate at the 100 ms sampling resolution. Their short intervals are not explained by a retained sequence gap or recovery, and incomplete detailed windows cannot exclude a draw/flip/decoder scheduling contribution.

## AU, decoder, RGB and presentation remain separate

| Observation | Total |
|---|---:|
| Structurally complete AU, before recovery filter | 34,167 |
| Structurally complete AU rejected by recovery filter | 73 |
| Accepted AU feed / Decode begin / Decode end / valid native-output observations | 34,094 each |
| Native outputs superseded before copy | 1,177 |
| Copy begin/end, conversion begin/end, RGB publication observations | 32,917 each |
| New presented RGB generations | 32,915 |

The identity **34,167 − 73 = 34,094** separates structural completion from recovery acceptance. Seventy-two filter rejects lack IDR; one lacks IDR/SPS/PPS. They are not 73 AU assembly failures. **82 nonempty AU discards** comprise73 recovery-filter discards,3 incomplete-marker discards and6 reorder-hole discards. AU reset reasons also include normal accepted submission and one new-track reset; normal submission is not a damaged-AU event.

Accepted feeds and native outputs have equal aggregate counts in this run, but no decoder output PTS establishes per-input correspondence. Native output supersession is an existing decoded-output selection; it does not justify discarding arbitrary compressed P-frames.

| Retained matched operation population | Spans | p50, ms | p95, ms | Maximum, ms | Unmatched/ambiguous |
|---|---:|---:|---:|---:|---:|
| Decode | 189 | 9.605 | 17.724 | 27.664 | 42 |
| native_copy | 162 | 9.439 | 9.970 | 10.906 | 32 |
| conversion | 163 | 3.031 | 3.117 | 3.216 | 37 |
| draw | 163 | 5.563 | 5.692 | 5.754 | 30 |

These progress-monitor durations cover retained windows, not every operation. Supplementary legacy windows retain a **48.978 ms Decode** inside stable pause4 (100.154 ms), and a maximum **14.350 ms Decode** inside stable pause12 (100.175 ms). Neither explains the complete interval; the progress-only 27.664 ms maximum is not the maximum across both evidence files. Source-side stage progress, Decode, native output, RGB publication and completed flip remain separate facts. No retained matched Decode/copy/draw span alone explains all pauses or establishes absence of other long operations outside coverage.

## NACK, RTX and PLI: observed versus implemented

The numeric local offer and accepted answer both contain Opus PT111 and H.264 PT102, and video feedback **NACK, NACK-PLI, CCM-FIR and GOOG-REMB**. Both summaries have six records and are uncapped. Neither includes an RTX codec/PT, apt or FID mapping. The numeric kind-20 configuration constant states that no NACK generator is installed; this is a source-configuration declaration, not runtime introspection. The actual chain and pinned source independently support it. The read-only vendor getter reports RTX disabled; the critical ledger contains zero RTX normalization and zero later missing-position observations.

The actual XCloud4 chain installs `RtcpReceivingSession`. The pinned implementation receives/control-processes media and can unwrap RTX when configured; it **does not generate receiver NACK requests**. XCloud4 does not install another receiver NACK generator. `RtcpNackResponder` is a sender-side response mechanism and is not a substitute for the missing receiver request path. See the [pinned receiving session](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/rtcpreceivingsession.cpp) and `src/streaming/rtc_transport.c`.

The historically named PLI events contain **59 local keyframe requests, 39 API dispatch attempts and 39 local results**, all with recorded local result zero. The critical ledger distinguishes **19 control-channel requests** from **20 `rtcRequestKeyframe` calls**. Because the receiving handler accepts FIR, its keyframe call selects FIR rather than PLI in this run. The pinned `requestKeyframe` returns true after dispatch; that return is not remote acknowledgment. This does not acknowledge delivery to the server. IDR_SEEN has 92 observations; seeing an IDR NAL is not structural completion, accepted feed, decoded output or presented recovery by itself.

## Coverage and retention

| Ledger or observer | Coverage |
|---|---|
| Critical sequence/reject/recovery/settings | 643/643 retained; gate omissions 0, ring overwrites 0 |
| Sequence observation at all instrumented RTP stages | Source-slot omissions 0, sequence-gate omissions 0, half-wrap ambiguities 0, range evictions 0 |
| Independent periodic sampler | 11,506 rows, none overwritten/omitted; missed periods 0; maximum lateness 1.920 ms |
| Cadence | All 32,914 intervals retained, identity/clock errors 0 |
| Manual phase ledger | Five entries including initial loading; no omitted entry |
| Progress detailed windows | Eight unique pause IDs, six subsequent windows not stored; zero overlap duplicates |
| Duplicate window requests | Five suppressed requests; no duplicate saved window |
| Window prehistory / body / reserved end | 512 / 3072 / 1024 records |
| Window context retention omissions | 70,566 |
| General progress-detail event admission | 25,270 omitted from 9,307,980 attempts |
| Rolling progress history | 9,274,528 overwritten |
| Sampler detailed-event gate | 72 omissions; numerical sampler rows still retained |
| Snapshot stability | Actor 160, queue 2, cadence 1, primary-RX 1 unstable reads counted |
| Legacy numeric pause trace | Twelve saved windows, two storage exhaustions; separate coverage limits |

Unlike 0.7.31, slots no longer duplicate a pause and no window opens already full of 4096 prehistory records. Storage remains finite: the progress detailed windows cover only the first eight of 14 pauses, with context selection and event-admission omissions. The independent critical ledger retains all sequence ranges and all recovery boundaries despite that saturation. General detailed history is not complete packet history and must not supply negative evidence outside its covered contexts.

## One proposed receiver-feedback experiment

**Test one bounded receiver NACK generator on the existing authenticated video receive chain, using the existing secure RTCP feedback path and the already accepted NACK feedback.** This would close the observed mismatch between accepted feedback and the missing receiver request implementation. It is an evidence-directed repair experiment, not proof that missing NACK caused every discontinuity or that retransmission will remove every pause. Generic NACK identifies absent RTP sequence positions; its wire format and feedback scheduling are specified by [RFC 4585 sections 3 and 6.2.1](https://www.rfc-editor.org/rfc/rfc4585#section-6.2.1).

The experiment should add one bounded, rate-limited request owner, with wrap-safe deduplication and cancellation when a watched position arrives. Record requested ranges, actual dispatch attempts/local results, later sequence observations and whether reorder accepts them before its existing decision. Keep the NEW queue, four readers, resolution request, depth 32 / 25 ms reorder and recovery predicates unchanged. The accepted session has no RTX mapping; do not enable RTX, change SDP or add a second NACK system in this same test. Runtime support for constructing and sending receiver NACK must be implemented against the pinned handler; no nonexistent C API is assumed.

The source review of `reorder_next_reason` establishes that a hole can be skipped as soon as **32 packets are buffered**, even before 25 ms expires. Time expiry is measured from the oldest buffered slot timestamp assigned at **reorder insertion**, not the original socket or callback time. With a zero clock while ingress awaits insertion, the time condition is suppressed but the depth condition still applies. A packet behind `expected` is returned as `X4_LIVE_REORDER_LATE`. Four retained progress REORDER_HOLE observations have depth-only reason flags 1 and buffered 32. The legacy original extends that evidence to 12 of 14 decisions: nine depth-only decisions (buffered 32, flags 1, 134 positions) and three time-only decisions (buffered 14/9/1, flags 6 including allow-expire, 48 positions). Two later decisions lack retained flags; their trigger remains unknown. Recovery begins about 2.4–3.5 ms after the first DATAGRAM gap in eight of nine clusters, and about 27.2 ms in the remaining cluster. That is not an established available round-trip repair budget. Presentation also stops 84–207 ms before the first gap becomes visible. NACK cannot retrospectively remove that entire lead.

Treat this as a **bounded retransmission diagnostic**, not an established fluency correction under the unchanged reorder policy. Start with one request per newly observed missing range of at most 64 positions, no retry and a hard dispatch cap. The actual pinned SRTP inbound policy has a 1024-position replay window; the Claude review suggested 128 without that source file, which is not the configured value. Replayed or too-old retransmissions still require explicit branch/result observations.

An API return alone is not success. A useful result requires correlated missing positions to appear later and restore usable AU progress before recovery becomes necessary, with fewer recovery-associated pauses and no audio/input/closure regression. Returned positions after the existing deadline or depth jump should be counted as late repair, not hidden. With the fixed reorder decision, retransmission may arrive too late; such a result would not by itself prove that NACK or server support is absent. The sender may ignore requests or use another repair behavior even though feedback is accepted.

Use one comparable fresh-session 10–15 minute stable pass and the current binary as reference, marking actual loading boundaries. This diagnostic run does not contain the proposed implementation or a new optimized package. The loading stall and the short indeterminate pauses remain separate unresolved observations.

## Private evidence and verification

| Original export | Bytes | SHA-256 |
|---|---:|---|
| Legacy numeric trace | 2,412,448 | `94364fe1bc7c54119e5741e3fcf24d7e25b12ea41973de010eac34d579e5e4ef` |
| Progress/reception trace, X4PROG2/v2 | 9,229,440 | `4c4e16387e12ff6ae24f599969ae4f783e5d3df0f661b1f5d19f82ef15183106` |

The filenames still use the historical 0731 prefix; version attribution comes from the internal 0.7.32 product/Build ID. The exact analysis reader SHA-256 is `78c3f6cd4fb1d5811ba22078d4384f650be39a6f274a7a8277d4fe31ccff2913`. Original binaries and original saved analyses remain immutable. Private numeric files contain local source identifiers; payloads, keys, tokens and full SDP are not exported. Raw Klog stays private.

Pre-console verification remains separate from this pass: the final offline reader had 59 passing cases,52 C-writer interoperability fixtures passed 564 checks, and independent host suites completed 60 monitor, 21 receive-bridge and 39 ingress cases. The verified native package built successfully. Those tests establish covered host/binary contracts, not PS4 performance or physical packet-loss cause.

Final source checks also cover `RtcpNackResponder::incoming`, which answers received feedback rather than generating receiver NACK, and the inbound SRTP policy (1024). The exact critical results supply the zero return codes omitted from the smaller Claude summary.

Two independent reviewers checked the actual preserved console counters/chronology. Actual Claude Opus 5.5 reviews derived numerical evidence and scoped receive source; it does not independently decode the original binaries or run console tests. Review details are preserved privately. The owner-confirmed audio, controls, catalog and exit observations supplement the export/join facts; they are not a measured audio-jitter distribution or an independent inspection of the PS4 error screen.
