# XCloud4 0.7.25 — two-reader native-copy experiment

Product **0.7.25**, PS4 **APP_VER 00.95**, application identifier **XCLD00001**. The frozen implementation passed the scoped final Claude review, independent static review, native build and artifact inspections. The package matches its PS4 readback and has been observed running. Its **first-picture byte comparison passed**, and the [initial console report](VIDEO_REPORT_0.7.25.md) records 12 complete intervals; pauses remain. Longer performance observation and safe closure are pending. Dependency-source content has been recovered completely on the PC in a reconstructed archive. The PS4 interface remains Spanish.

## Reason and scope

The completed 0.7.24 selection contains **134 full reporting intervals / 670.473 seconds** after the initial mixed interval. Native NV12 copy averaged **29.382 ms**, RGB conversion **3.003 ms**, and completed publications/new image draws **17.886 per second**. Early and late intervals differ, and backlog/recovery persists. These are local observations from one uncontrolled attempt, not source FPS or end-to-end latency.

The [experiment design and comparison procedure](PARALLEL_COPY_EXPERIMENT_0.7.25.md) isolates copying: the existing video owner and one persistent helper copy disjoint spans of the same validated native picture into cached staging. The helper performs no Videodec2 calls. Native type 3 output, four output reservations, two inputs, the **16 ms / four Decode** policy, SIMD conversion, RGB mailbox, presentation, audio, controls and provider protocol remain the baseline.

The first valid copy of each decoder start has a one-time byte comparison against the existing serial routine while the native source lease remains held. A mismatch, ineligible first picture, optional allocation failure or helper-creation failure selects serial copying for that start. A match covers only that checked buffer. A helper job must finish before conversion, native output reuse or release; an error cannot bypass pending-picture preservation. Failed helper join retains its context and allocations.

## Review and delivery state

| Evidence | State |
| --- | --- |
| Claude Opus 5.5 plan review | Completed: **11 turns / 10 Read calls**; required the post-preservation error/pending guard before Decode or reuse |
| Antigravity plan review | Successful retry: observed **gemini-3.1-pro-high**, **1 turn / 4 view_file calls**; no material application flaw reported in that plan review |
| First Antigravity invocation | Command denied before execution; preserved separately and not counted as a review |
| Claude Opus 5.5 final source review | **Scoped PASS**: observed **claude-opus-5-5**, **9 turns / 8 Read calls**; no concrete material defect found in the frozen patch |
| Independent final static review | No material defect found in the **eight-file** scope; **eight selected linked-symbol ranges / 8,401 instruction bytes** match the retrieved ELF |
| Antigravity final source review | Unavailable: **gemini-3.1-pro-high** and the **claude-opus-4-6-thinking** retry reached individual quota (**HTTP 429**) with **zero file reads**; no final PASS |
| Runtime implementation | Frozen across four video/media files; the other four changed files contain version updates |
| Native build and artifact inspections | **PASS**: the **67-file** source/build inventory matches the frozen PC inputs before and after the native build; retrieved ELF/OELF/SELF/SFO hashes and selected compiled paths were inspected |
| Package transfer correspondence | **PASS**: package size and SHA-256 agree on the build VM, PC and PS4 readback; only the 0.7.25 installer remains in the PS4 installer folder |
| Dependency-source archive | Complete PC reconstruction: all **9,369 source payloads** match the recovered VM manifest; compressed bytes differ from the interrupted original transfer. Separate independent full-archive verification **PASS** |
| Initial console attempt | Running 0.7.25; two-reader helper enabled; **one 1,382,400-byte picture comparison passed**; first **12 complete intervals / 60.111 s** summarized in the [video report](VIDEO_REPORT_0.7.25.md) |
| Longer performance observation and safe closure | Pending; the owner reports more time between pauses, with pauses remaining |

The plan and final source reviews are separate. Static inspection and matching artifact bytes alone do not establish native scheduling, cache coherence, first-copy equality or console behavior; source correspondence uses frozen build inventories rather than an independent compiler reproduction. No speedup, 30/60 FPS result, stack-capacity verification or stutter/latency fix is claimed.

## Artifact checksums

| Artifact | Bytes | SHA-256 | Verified state |
| --- | ---: | --- | --- |
| `XCloud4-0.7.25.pkg` | 8,912,896 | `42ac3bf40c8d32fe7abbe2d26615b2940c53b748c45c267755de92d708a26630` | Complete VM/PC/PS4 copies match; console version and first-picture check observed |
| `XCloud4-0.7.25-dependency-sources.tar.gz` | 83,475,236 | `bcade0b5d4b5afc3141d8cd39b8164b5f2d0fb8eb558198190645411f4199f4b` | Complete PC reconstruction; payload hashes and recovered VM manifest match; separate independent full-archive verification **PASS** |

The dependency archive was reconstructed after the original VM compressed transfer was interrupted. Its **9,369 source files / 150,890,220 source bytes** and recovered manifest match, with **9,781 archive entries**. Different repacking produces a different compressed checksum; this is source-content recovery, not a claim that the original VM compressed bytes were retrieved completely. Full application source is provided separately in the repository.

The implementation is original project code under **GPL-3.0-only**; Moonlight PS4 is a research reference with unresolved application-license evidence at the inspected pin, and its code is not copied. The original 0.7.24 release, tag and assets remain intact for a planned A/B/A comparison.
