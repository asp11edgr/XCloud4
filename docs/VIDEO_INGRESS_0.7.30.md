# Video ingress contention experiment — 0.7.30

## Confirmed historical evidence

An independent read of the original 0.7.26 and 0.7.27 numeric binaries, with repeated window records deduplicated, reproduces the supplied findings. In 0.7.27, **17 retained REORDER_HOLE decisions cover 17 sequence positions**. Eleven have an earlier `QUEUE_DROP / PUSH_CONTENDED`; six have an earlier `RX_VALID` with the queued bit clear but no retained exact rejection reason. These are not 17 pauses and do not explain the session's **83** long presentation intervals.

For sequence 5704, the rejected receipt is at 206754727 µs and the contention event at 206754729 µs. An ingress reset follows at 206767593 µs; the last previous new presentation is at 206783537 µs; reorder abandons sequence 5704 at 206794531 µs. Recovery ends at 206910214 µs, RGB is published at 206921633 µs and the next new presentation is at 206934573 µs. This is positive evidence of a packet arriving and being rejected locally before the retained hole. It does not prove the cause of the entire 151036 µs interval. Conversely, sequence 39763 is rejected **61225 µs after pause 3136 starts** and its hole is declared after that pause closes; it cannot explain that pause's onset.

The historical `QUEUE_DROP` flags 1024 mean reason 4 (`PUSH_CONTENDED`) shifted eight bits; 768 mean queue full. The sampled depth is not the reason. Omitted diagnostic events are not missing network packets. Raw private traces and logs remain outside Git.

## Callback contract and chosen queue

The actual dependency pin, libdatachannel `bdc5ff28e9d3b863144c94a677ecf5bf043aaf15`, serializes the message callback of each Track with its `synchronized_callback` recursive mutex. XCloud4 has one video Track per media context, no receive-path reentry into RTC, and one video consumer. Four RTC workers do **not** demonstrate four simultaneous video producers. A dedicated SPSC queue would be viable with this provider.

The selected queue nevertheless supports **multiple producers and one consumer**, preserving the public receive API's any-transport-thread contract and avoiding reliance on a future provider maintaining this serialization. It is video-only; audio retains the existing guarded ring.

- Keep **256 slots**, each holding at most **2048 bytes**, allocated once before callbacks start.
- Reserve a slot with a strong CAS on the enqueue ticket; at most **16 attempts**. A failed reservation due to simultaneous producers is a separately counted bounded failure, not silently retried forever.
- Publish payload, size and arrival metadata with `ready` release; the sole consumer acquires it, copies the packet, clears ready, then releases the dequeue ticket.
- A slot cannot be reused while the consumer is reading it. A reserved unpublished head cannot be bypassed. The owner returns without consuming it and yields 50 µs rather than spinning.
- No consumer mutex, sleep or allocation exists in the candidate receive path. Reservation elapsed time is monotonic wall time, including scheduling, not CPU time.
- Stop rejects new reservations; a reservation already obtained finishes publication. Free still requires transport shutdown and producer/consumer joins. The active-operation check is a retention guard, not permission to race free with new callers.

Unsigned ticket arithmetic is tested across UINT64 wrap with the unchanged power-of-two capacity. Full counts include reserved unpublished slots. Depth/oldest-age snapshots are approximate independent samples. Candidate contention can still occur between producers after 16 failed reservation attempts; the specific consumer-lock drop branch is eliminated, not every possible packet loss.

## Shared diagnostics and instrumented baseline

Both **XCloud4-0.7.30-baseline.pkg** (A) and **XCloud4-0.7.30.pkg** (B) use the same added callback diagnostics, four copy readers and APP_VER **01.00**. A shows `VERSION 0.7.30 BASE`. A preserves the previous try-only, copy-under-gate mechanism, using an atomic owner value in the gate so a failed CAS identifies owner 1 (producer) or 2 (consumer) at that operation. It is an **instrumented baseline**, not the exact 0.7.29 binary or identical timing overhead. B uses the new MPSC queue. Build with `X4_INGRESS_MODE=baseline` and package suffix `-baseline`, or default `mpsc` and no suffix; rebuild all objects when changing modes.

Periodic reports and close totals separate callback count, overlap count, current/peak simultaneous callbacks, elapsed sum/maximum/histogram, reservation elapsed/retries, full/contended/other/invalid-RTP failures, ingress resets, recovery starts/completions and completed recovery elapsed time. The 12 disjoint callback histogram buckets are <=2, <=4, ... <=2048 µs and >2048 µs; labels mean bucket upper bounds, not cumulative counts. Maxima are lifetime best observed values with bounded update attempts. Startup's initial wait is excluded from recovery episode totals. An open recovery at stop is not a completed duration.

The extra numeric `RX_CALLBACK` event is `0x402`: `a` high32 is simultaneous video callbacks, low16 is sequence (zero if invalid); `b` high32 is callback elapsed and low32 reservation elapsed, saturated at UINT32_MAX. Flags low5 are reservation retries; bit6 means queued and bit7 invalid RTP. The callback timer includes parse, queue work and existing trace/poll work; it excludes the final record/statistic updates and the outer provider callback wrapper. `QUEUE_DROP.b` high32 is baseline gate owner at failed CAS, low32 sampled depth. No tokens, SDP, SSRC, payload, addresses or pointers are recorded. Tracing remains capped and try-only; extra events can increase diagnostic omissions, which must be reported.

Queue age now starts at entry to the XCloud4 video receive function, rather than just after the old queue copy. Both experiment variants share this definition. Reorder arrival still starts at insertion; its **128-slot / depth32 / 25000 µs** policy, **16000 µs / four Decode** worker budget and AU recovery decisions are unchanged. The native decoder still supplies no output PTS, so no output is attributed to the last compressed input. Local `PLI_REQUEST` remains a consumed request, not a network dispatch or a server-response timestamp.

## Preparation-time host checks and comparison protocol

The explicitly requested POSIX harness tests argument validation, true fullness, maximum-size byte integrity, deterministic consumer/push overlap with free capacity, held-slot ownership, unpublished-head FIFO, four persistent concurrent producers with per-producer ordering, bounded CAS exhaustion, unsigned wrap, and stop/free/join. The runner builds both variants and optionally ASan/UBSan/leak and TSan. Host results are reported separately from console behavior; successful host checks cannot establish PS4 timing or fluent playback.

Compare **A → B → A**, using the same title, scene, actual stream dimensions, duration, Ethernet connection and settings. Prefer three minutes of active gameplay per run after loading/queueing; retain the two baselines instead of pooling them. Return with **L1 + R1 + CIRCLE** to stop and dump the trace. Record audio, controls, catalog return and application exit. Trace filenames encode variant 0 (A) or 1 (B).

Report per run: rejection counts by exact branch, callbacks and peak overlap, callback/reservation distributions, queue-age distributions, ingress resets and recovery episodes per active minute, new-presentation gaps >100 ms per minute and their p50/p95/maximum. Separate active gameplay exposure from loading, intentional inactive time and shutdown. Trace-window distributions describe retained observations only; final report counters and cadence header totals have separate coverage. Do not substitute a diagnostic-window sample for a whole-session distribution.

A favorable result was defined as fewer consumer-contention rejections and associated recovery without worsening callback time, queue age, audio, controls or closure. The protocol above preserves the preparation-time target. Active gameplay exposure and scene equivalence were not independently established in the completed attempts; the observed report instead uses explicit first-to-last NEW endpoint spans and separate whole-session counters.

## Post-build console checkpoint

The [completed A1 → B → A2 comparison](CONSOLE_COMPARISON_0.7.30.md) records **115 / 3 / 102** exact closed NEW-match intervals above 100 ms over **219.354041 / 222.923990 / 201.552959 seconds**: **31.456 / 0.807 / 30.364** per exposure minute. Whole-session consumer-lock rejections are **231 / 0 / 202**, ingress resets **207 / 0 / 186**, and queue-full rejections zero in all three. Callback overlap is zero, peak one; no general concurrent-provider claim follows. A2 completes 120 of 121 recovery episodes, leaving one duration unknown.

Coherent post-check reporting windows show approximately **9.25 ms** native copying in every run; the observed change is not a copy-speed improvement. All first native pictures are **1280 × 720, pitch 1280**, despite the shared **960 × 540 / maxFPS 30** request. Both baseline captures exhaust saved-detail capacity; B has diagnostic omissions, unstable snapshots and one time-capped detail window. The report distinguishes these limits and the three remaining B gaps without deriving network loss, RTT or physical display latency from missing records.

The owner reports markedly fewer stutters in NEW, confirms good/comparable audio and controls in both variants, and confirms the A2 menu return. Those reports are subjective; scene/server/network equivalence and individual controls were not independently verified. **The candidate is restored**, confirmed by two identical installed-package reads matching the release candidate's bytes and SHA-256. No solved-all-pauses or stable-FPS result is claimed. This documentation checkpoint changes no runtime or existing release asset.
