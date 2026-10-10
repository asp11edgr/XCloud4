# XCloud4 0.7.26 — pause diagnostics on the two-reader baseline

Product **0.7.26**, PS4 **APP_VER 00.96**, application identifier **XCLD00001**. Native compilation and retrieved artifact/source correspondence are verified. Source reviews are complete; PS4 installation, trace export and the owner diagnostic capture remain pending. The PS4 interface stays Spanish; repository reports stay English.

## Purpose and preserved behavior

The owner continues to observe alternating smooth playback and brief pauses on 0.7.25. Its aggregate `damaged` counter can increase even without queue overflow because it counts discarded open AUs during several recovery paths. The [pause-trace specification](PAUSE_TRACE_0.7.26.md) adds exact numeric observations before deciding which optimization to make next.

The release preserves the **video owner plus one copy helper**, their disjoint NV12 spans, first-valid-picture serial byte check/fallback, SSE2/SSE4.1 copy bodies and fences, native type 3/four output reservations, SIMD conversion, triple RGB mailbox, **16 ms / four Decode** checks, queue/reorder bounds and H264 recovery. Audio, controls, provider protocol and requested stream profile are unchanged. The proposed four-reader experiment is deferred to **0.7.27**.

## Diagnostic changes

- Local numeric events observe RTP framing/admission, ring dwell, track/reorder decisions, AU assembly, exact reset/damage and waiting-for-IDR transitions.
- Independent AU, Decode-call, native-output, copy and RGB publication IDs describe each stage. The native output ABI has no returned PTS; an output is never attributed to the latest input AU. Only an explicitly leased output connects to its copy, conversion and RGB publication.
- Actual drawn-generation tags reach the existing display wait. Only a successful matching flip completion resets NEW-image cadence; repeat/UI refreshes do not hide a pause. Ongoing monitors detect intervals **greater than 100 ms**, including idle/wait periods.
- A trace-only try-once gate records bounded RAM history and at most **12 windows**, with requested 500 ms pre/post capture, 16,384 records per window and a two-second window cap. Coverage/drop/cap flags and fixed counts survive detail exhaustion; absence-based claims remain conservative.
- The optional trace allocation is bounded below **8 MiB**. Failure disables diagnostics rather than failing playback. Trace overhead is real; this version makes no performance-improvement claim.
- Fixed closed-session rows report reset calls, discarded-open-AU increments and overlapping damage-bit counts, separately from event attempts and saved records.
- Exclusive numeric `/data/xcloud4-trace-0726-<local_number>.bin` export occurs only after transport/auth quiescence, successful worker joins and native teardown. Failed-close retention also retains the trace; export errors do not alter playback closure. Existing files are not overwritten.

No trace record contains payload bytes, NAL bytes, credentials, SDP, IP addresses, SSRC, pointers or raw RTP/PTS/DTS timestamps. Numeric local times, seq16, sizes, geometry, local ordinals and enum reasons are the permitted metadata.

## Status and limits

| Evidence | Current state |
| --- | --- |
| Two-reader diagnostic source and payload schema | Complete; 70 frozen build inputs match host/VM before and after compilation |
| Independent source review | Complete; schema/parser/source and compiled-byte correspondence inspected separately |
| Actual Claude Opus 5.5 source review | Integrated: 29 turns / 28 reads; correction review: 22 turns / 21 reads; no material defect found in reviewed scope |
| Native build/package and compiled inspection | Build complete; package/ELF/OELF/eboot retrieval hashes match VM; allocation, imports and selected compiled flows inspected |
| Matching dependency/source inventory | 9,369 payload files checked; original VM compressed archive fully retrieved with matching hash |
| Package transfer/readback and owner install | Delivery checked separately; owner installation pending |
| Actual trace allocation/export and pause capture | Pending |
| Offline parser | [`scripts/analyze_pause_trace.py`](../scripts/analyze_pause_trace.py) prepared; static review and syntax parsing, no trace run |

Source inspection does not establish binary behavior, native memory/cache guarantees, physical scanout time or end-to-end input/media latency. Sequence skips are observations, not proof of network packet loss. Missing trace records or capped windows cannot prove a stage did no work. The next capture must report coverage and distinguish startup/bytecheck/intentional stop from an active playback pause.

The first source review's nonblocking recommendations were applied: strengthen the cadence writer's release ordering, keep each window's own gap IDs separate from prehistory context, check the history accounting invariant, and retain authoritative exit totals in the analyzer. A fresh focused Claude review inspected those corrections and the bounded callback-close contexts. Review coverage is source-level evidence; it is not hardware verification. Antigravity's previously observed final-review quota limitation remains; no new Antigravity approval is claimed.

## Verified artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `XCloud4-0.7.26.pkg` | 8,912,896 | `4a55d62573f73371f3a97f59bdb30d3a383d2b736a93d3a8f363c6ca2f6303f2` |
| `XCloud4-0.7.26-dependency-sources.tar.gz` | 83,583,261 | `f9a4a0a72ebebe6388528af7909b478339c3a12336f209c6528b0bd5b986f035` |

The optional trace allocation is **6,570,008 bytes** in the inspected ELF. The actual linked libc implements `wbx` through exclusive creation; on-console file-system success remains unverified. A capped reconstructed window can contain a gap begin without its closing record; the analyzer preserves incomplete coverage instead of inferring inactivity.

The published 0.7.25 baseline and its artifacts remain preserved. No new playback benchmark, automatic test, fluency result or causal diagnosis is claimed by this checkpoint.
