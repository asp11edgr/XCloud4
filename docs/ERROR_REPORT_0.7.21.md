# XCloud4 0.7.21 — better video, remaining frame pacing and queue pressure

Product **0.7.21**, PS4 **APP_VER 00.91**, application identifier **XCLD00001**. The owner confirms video is much better than 0.7.20 but remains choppy. The owner explicitly corrected the earlier controller report: **the D-pad works and the sticks move**. This is not a complete validation of every button, axis, local chord or cancellation path.

## Observed console result

The current version's capture selects streaming-copy mode, decodes Opus with **960 samples**, and produces a **1280 × 720** H.264 picture with pitch **1280**. Numeric paired media/render summaries are preserved in a bounded snapshot; raw console/account data remains private. The native output allocation and decoder rings are unchanged. Build, source review and artifact evidence are preserved in the [0.7.21 release notes](RELEASE_NOTES_0.7.21.md).

## Measured processing costs

The early sustained selection excludes the first mixed startup/media interval and includes **16 complete intervals**, totaling **80.512969 seconds**, **1045 copies/conversions** and **1045 new image draws**.

| Operation | Observed mean |
|---|---:|
| Native Decode | 8.047 ms per call |
| Native NV12 copy into CPU storage | 29.354 ms per image |
| NV12-to-RGB conversion from CPU storage | 20.068 ms per image |
| Copy plus conversion | **49.422 ms per image** |
| Image draw | 5.517 ms per call |
| Presentation call | 8.311 ms per call |
| New image draws | **12.979 per second** |

New-draw rates across these sustained intervals range from **10.96 to 14.00 per second**. Later observations fall roughly within **8–13**, so the earlier mean is not a fixed or guaranteed playback rate. These counters measure application image updates; they do not measure the panel refresh rate or the server's rendering FPS.

The earlier [0.7.20 sample](ERROR_REPORT_0.7.20.md) spends **499.5965 ms per direct conversion**, including reads from native WC memory. Comparing it with 0.7.21's combined **copy + conversion** cost gives about **10.109×** for this image-preparation region. The captures use different live intervals and are not a controlled benchmark. The added copy cost is included; the ratio must not be described as a tenfold improvement in total game FPS.

## Remaining limits and next scope

Copy plus conversion occupies about **64.15%** of the selected elapsed intervals. The same selection still records **1138 full-queue events**, **1144 dropped packets**, **1141 lost packets** and **414 keyframe requests**. Reconstruction damage, waiting for a keyframe and repeated images remain visible in the counters. These observations show continuing processing and recovery pressure, without identifying every network-loss cause.

CPU conversion still takes about **20 ms per image**, while copying takes about **29 ms**. The [0.7.22 source change](RELEASE_NOTES_0.7.22.md) implements original SSE2 conversion in eight-pixel blocks while preserving native output memory and rings. Its native build, final source inventory, compiled instruction audit, fresh Claude Opus 5.5 PASS and VM/PC/retrieved-PS4 package integrity are confirmed; the 0.7.22 console result remains pending. Improving conversion alone does not establish smooth playback or remove the remaining copy, draw, presentation and recovery costs.

Timing intervals spanning a decoder restart can contain a pre-existing wrapped counter delta and must be excluded from comparisons. `MFENCE` orders CPU accesses; it does not establish GPU completion. External closure and the full controller matrix remain separate unverified behavior. No automated tests or additional hardware probes were run to produce this report.
