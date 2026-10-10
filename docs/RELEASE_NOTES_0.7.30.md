# XCloud4 0.7.30 — isolated video ingress experiment

Product **0.7.30**, PS4 **APP_VER 01.00**, title ID **XCLD00001**. PS4 UI remains Spanish; GitHub documentation is English.

The previous **0.7.29** search/filter/selected-title/return trial is confirmed by the owner's response: it worked correctly. This is not an exhaustive catalog naming or video-performance benchmark.

## Changes

- Replace only video's shared try-gate ring with a bounded, preallocated MPSC reservation/publication queue; retain capacity256 and packet limit2048.
- Preserve audio's original queue, four copy readers, reorder limits, AU recovery, codec/stream settings and gamepad mappings.
- Add bounded numeric callback concurrency/timing/reservation diagnostics, branch-specific failures and recovery episode totals.
- Prepare an instrumented try-gate baseline with identical profiling for the requested A/B/A comparison; its header explicitly says BASE.
- Add the explicitly requested host concurrency/integrity/lifetime harness and sanitizer runner; no PS4 application is automatically launched.

See the [source-backed experiment and comparison protocol](VIDEO_INGRESS_0.7.30.md). Historical 17 locally rejected hole positions do not explain all 83 pauses. The callback contract supports SPSC under the current vendor; defensive MPSC does not imply multiple console producers were measured.

## Evidence status

Historical binary findings were independently reproduced. Both baseline and candidate host harnesses passed normal GCC15.2 execution, AddressSanitizer plus UndefinedBehaviorSanitizer with leak detection, and ThreadSanitizer in Lubuntu. Each threaded run preserved 4000 packets with ordering checked per producer; rejection/retry counts are scheduling-dependent test observations, not console performance. The earlier Clang host run passed normal checks but could not link its missing sanitizer runtime libraries; GCC sanitizer results are separate successful runs.

Actual **Claude Opus 5.5** selected-source review completed with no material issue: **36 turns / 35 Read calls / 31 distinct files**, complete returned-line coverage, zero failed or outside-scope reads, and all 170 frozen paths matching before/during/after review. Independent source checks preserve the experiment constants and unchanged audio/H264 source. The selected host harness is not a full live-media integration test; allocation failures, partial initialization and failed joins were reviewed in source, not fault-injected.

Both native variants were rebuilt completely with explicit modes and suffixes, preserving distinct banners. Native import/lifecycle checks, 75 frozen build inputs before/after, original dependency export bytes and retrieved package/ELF/OELF/eboot correspondence are verified. An independent linked check matched **28,132 machine-code bytes across 26 selected functions** from the two ELF files. Both installers were copied to PS4 and read back byte-for-byte; no PS4 application was launched automatically. These are selected static/artifact checks, not complete machine semantics or hardware behavior.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| XCloud4-0.7.30-baseline.pkg | 8,912,896 | `0553b2adc908cb5afd6cdf4268b6db4b04f8c5dca4375615a96ed33f489f2c5b` |
| XCloud4-0.7.30.pkg | 8,912,896 | `bd2691228964a1dc52d207bb2feb86f80232a02c6e5fea156e875ac07fa5e01d` |
| XCloud4-0.7.30-dependency-sources.tar.gz | 83,582,905 | `4ca24841720a4a01d0b09c836827a05626d2d03f11eb9c7bb43c34e3991d69c0` |

Console **A/B/A remains pending**. No solved-pause, stable-FPS or network root-cause claim is made. Raw private logs/traces, credentials, SDK copies and generated build objects stay outside Git; existing stable installers remain available for rollback. Post-review changes only update public evidence status and ignore generated Python caches; the compiled runtime and host-test source are unchanged from the reviewed freeze.
