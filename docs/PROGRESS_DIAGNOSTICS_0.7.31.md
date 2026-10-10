# Independent progress diagnostics — 0.7.31

## Scope and current state

This checkpoint instruments residual pauses in the **NEW/MPSC** path preserved from the [completed 0.7.30 comparison](CONSOLE_COMPARISON_0.7.30.md). Instrumentation and its secondary reader are implemented. Synthetic host checks, final C-writer/reader interoperability, selected source review, native build and package transfer are verified separately below. **Console installation/launch, overhead and the 10–15 minute gameplay capture remain pending.** This checkpoint establishes no performance repair.

The runtime experiment preserves **four copy readers**, **16 ms / four Decode** checks, reorder **128 slots / depth 32 / 25 ms**, existing recovery, audio/input and **960 × 540 / maxFPS 30 / 5000 kbps** requests. The prior first native pictures were **1280 × 720**. Requested settings are not proof of delivered format. No new video optimization or speculative recovery correction belongs to this checkpoint.

The existing pause-trace reader and original 0.7.30 evidence remain unchanged. A fresh read of the original B binary reproduces the saved analysis exactly: **1.886038 s**, **100.006 ms** and **366.998 ms** closed NEW-match intervals. The first remains indeterminate; the second contains a **49.065 ms** Decode wall span, including scheduling; the third positively records a depth-triggered skip of nine positions and **240.194 ms** recovery-feed wait. Those facts do not prove network loss, server RTT or the cause of every microsecond.

## Secondary capture contract

The bounded monitor format is **X4PROG1, version 1, little endian**. It samples approximately every **100 ms** on its own thread. Target, actual and snapshot-end times expose delayed sampler wakeups and the duration of each observation. Counters are read within that range, not simultaneously at its starting timestamp. Independent cumulative stage counters and last-progress state precede the legacy diagnostic admission gate, so a missing legacy detail does not erase the corresponding cumulative count. Secondary sample/event omissions, history overwrite, capacity, torn/unstable state and open operations must still be reported; independent does not mean lossless or cost-free.

The reader separates receive callbacks and bytes by local session and kind/SSRC; queue acceptance/rejection and safe depth/oldest-age observations; reorder expectation, trigger and skipped positions; complete valid AUs versus discard reasons; Decode, valid/no-output and pending-surface state; native copy/conversion; RGB publication/consumption; flip submission/completion; and recovery begin/end. Source-change detail observations remain distinct: an SSRC table entry alone does not identify every source epoch. It reports attempted PLI dispatch and its local result separately from a consumed local request. Neither establishes remote delivery or acknowledgment.

A stalled stage is an investigation lead supported by progression on each side. No RX can reflect network, server or pre-callback scheduling; some RX does not prove all required packets arrived. A valid AU without output is not automatically a decoder fault. Native output has no returned PTS: the reader must not assign it to the latest compressed input. Local receipt, queue, operation and flip observations do not measure provider capture age, physical scanout or control-to-image latency.

## Phases, denominators and distributions

The session starts in **loading**. After actual loading, the owner holds **L1 + R1** and taps **TRIANGLE** to mark **stable gameplay**. Repeat the chord to mark a subsequent **transition**, then repeat after loading to mark stable again. Each manual mark has a unique local ordinal. **This combination is reserved locally:** while held, the forwarded gamepad frame is neutral, including during loading when visibility prevents a phase mark. Ordinary button mappings outside the combination remain unchanged. Active exposure follows actual application visibility. Session/source observations and manual phase boundaries remain distinguishable.

Report loading, transition and stable exposure separately. Stable rates use recorded manual active-epoch durations, not the first-to-last NEW endpoint span of the old comparison. A gap crossing a phase/epoch boundary is marked and excluded from the stable-only closed-gap distribution. Missing boundaries or incomplete epochs cannot silently supply a denominator. Open gaps and recovery waits have no invented completed duration.

Retained fresh-NEW cadence supports nearest-rank quantiles only for its stated population. The exact interval ledger is a bounded prefix; omitted cadence makes its quantiles a retained-subset result. The cumulative floor-log2 histogram continues and supplies quantile bounds, not exact bucket-edge percentiles; its last bucket has no finite upper bound. A mixed-epoch bit excludes every interval spanning a phase change, including stable → transition → stable with equal endpoint enums. A separate main-owned 64-entry phase ledger survives event admission drops. Stable-only rates require complete recorded manual boundaries, a complete valid cadence ledger and nonzero recorded active exposure. Report sample target/actual delay, event omissions and capture coverage beside every absence-based classification; missing observations do not prove missing packets or idle stages.

## Private reader and bounded layout

Run the read-only reader with an explicit private output path:

```text
python scripts/analyze_progress_trace.py capture.progress.bin --out private-analysis.json
```

The reader accepts at most **12 MiB**, validates declared capacities before decoding, rejects unsupported/truncated/trailing layouts and creates output exclusively. It never overwrites the original capture or an existing analysis. JSON includes local SSRCs and source keys: keep it outside public Git history.

The 1024-byte header is followed by numeric configuration, independent attempted/dropped event counters, reset counters, histogram, manual epoch exposures, the fixed phase ledger and 16 RX-source slots. Bounded variable sections hold the **latest 12000 samples**, a prefix of **65536 NEW intervals**, **8192 history records** and **eight windows of 4096 detail records**. Samples are 640 bytes; cadence, phase and detail records are 32 bytes. At a 100 ms target the circular sample capacity is approximately **20 minutes**, including loading; earlier overwritten samples are counted explicitly, and late sampling changes the elapsed span. The embedded build identity is an input identity, not a circular assertion of the package's own hash.

The report separates cumulative totals from retained event details and deduplicates overlapping history/windows by complete record identity. A reused/wrapped admission ordinal never silently merges different records. Queue oldest age, actor phase age and the primary source/sequence tuple require their corresponding coherent witnesses; counters remain individually atomic rather than one globally synchronized sample. The primary sequence needs coherence bit 1, a nonzero source key and its valid flag. History overwrite is unknown unless coherence bit 3 is present and its value is not the unknown sentinel; skipped sampler history reads remain explicit. Counter wrap is flagged, with no rate or causal classification inferred for that sample interval. Reset-call and discarded-open-AU reason totals are separate overlapping populations; do not add them as distinct resets. A retained completed recovery span means accepted recovery AU feed, not a displayed IDR.

Queue `CLOCK` means the witnessed arrival is newer than the sampler's start reference. A legitimate packet can arrive during the observation range; this flag alone does not establish a clock failure. Stage and age observations must be interpreted within target/actual/snapshot-end timing. Frequent RX and phase records can fill history and windows quickly; cumulative counters continue, while coverage flags limit detail-based absence claims.

## Authorized long-run procedure

1. Retain the restored NEW 0.7.30 package and its existing evidence. Record the instrumented version/build identity and actual requested/delivered settings.
2. Let loading finish, then mark stable gameplay. Play for **10–15 minutes** with comparable activity; mark transitions separately. The owner starts the application and the play session.
3. Record subjective audio, controls, catalog return and app exit. Retrieve the bounded secondary capture and its matching legacy trace; preserve original bytes and hashes privately.
4. Compare with NEW 0.7.30 to look for instrumentation overhead, keeping exposure definitions and scene/network limitations explicit. Do not repeat the earlier queue investigation or treat separate sessions as a controlled benchmark.
5. Deliver a qualified chronology, stable gap rate/distribution, recovery/discard reasons, sample delay, omissions and open waits. Propose one minimal runtime repair only if that new evidence supports it.

## Verified evidence and remaining limits

Deterministic reader fixtures are explicitly **synthetic, not console observations**. All **40** checks and **18** schema/layout checks passed, preserving original inputs. That earlier checkpoint observed writer hash prefix `57362075`; it must not be described as execution against the later final writer. A subsequent, separate interoperability check used **40 actual C-writer synthetic files** from final writer SHA-256 `0a33eb1912194c183893bef7115dfd07fc72e8e15a09850be376c63f27d605d3`. All files parsed and **292 semantic checks passed**, including gated-history UNKNOWN, coherent RX tuples, circular samples, mixed phases, caps and an open second gap. The seven final host input hashes and all original inputs matched before/after. The legacy reader and original B binary remain unchanged.

```text
python tests/progress_trace_test.py
```

The host harness passed **12 cases in each GCC plain, ASan+UBSan and TSan mode**, plus **Clang plain**. Clang sanitizer libraries were unavailable, which is not a sanitizer pass. TSan's warning that it does not model standalone fences remains a limit. Host actor, cadence and RX instability branches are not all exercised; these fixtures do not establish PS4 scheduling, native atomic/fence behavior or heap headroom.

Actual **Claude Opus 5.5** review returned all **41 selected files** in full through **49 Read calls / 50 turns**, with no failed or outside-scope operations and matching immutable source hashes. It found no material runtime/parser defect, but identified two documentation issues: the reserved chord and evidence predating the final writer. This factual update corrects the chord description and records the separate final-writer interoperability receipt. Claude did not run tests, build native artifacts or assess console performance; this later documentation update is not a new code review.

Native build and selected linked import/lifecycle inspection passed. At their checkpoints, **80 native build inputs and 180 frozen repository paths** matched; build identity is `0caa54f34a994de1ffed125d27e47767062384b2`. The **8,978,432-byte** package matched VM and PC copies and **two exact FTP readbacks** from PS4. Package and immutable project/dependency archive hashes are listed in [the release notes](RELEASE_NOTES_0.7.31.md). The archives preserve the build checkpoint; this documentation update followed it without changing runtime, parser or test code. No console installation, launch, export, clean exit, overhead or fluency result is established yet.

Raw traces, SSRCs and numeric source identities remain private. Public summaries contain bounded aggregates and interpretation limits; no packet payload, provider response, address, credential or arbitrary remote text belongs in this diagnostic.
