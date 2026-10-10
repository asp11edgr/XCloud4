# Parallel native-copy experiment for 0.7.25

## Status and question

This is the authorized **two-reader** experiment: one existing video owner plus one persistent CPU copy helper. The implementation is frozen in the four video/media files. Scoped final Claude and independent static reviews, native build and artifact inspections are complete. The package's VM, PC and PS4 readback hashes match and 0.7.25 has been observed running. The [initial console report](VIDEO_REPORT_0.7.25.md) confirms the two-reader helper, a passing first-picture byte comparison and the first 12 complete reporting intervals. Pauses remain; longer observation and safe closure are pending. Dependency-source content was completely recovered on the PC in a reconstructed archive. Four readers are a separate future experiment, conditional on evidence from this one.

The question is whether overlapping two bounded reads reduces the elapsed native-to-cached NV12 copy without breaking picture ownership or making queue/recovery behavior worse. Shared bandwidth, dispatch and cache traffic may prevent improvement. No twofold speedup or 30/60 FPS target is treated as a result.

## Measured 0.7.24 baseline

The frozen completed attempt excludes the first mixed interval. Its first native picture was **1280 × 720, pitch 1280**, despite the retained **960 × 540** request. The full selection has **134 intervals / 670.473 seconds**, **11,992** completed publications and the same number of new draws.

| Local metric | First 3 full intervals | Full selection | Last 12 full intervals |
| --- | ---: | ---: | ---: |
| Duration | 15.017 s | 670.473 s | 60.033 s |
| Copy per copied image | 29.389 ms | 29.382 ms | 29.387 ms |
| RGB conversion per converted image | 3.003 ms | 3.003 ms | 3.003 ms |
| Decode per native call | 6.337 ms | 8.377 ms | 8.610 ms |
| Completed publications/second | 20.044 | 17.886 | 17.624 |
| New image draws/second | 20.044 | 17.886 | 17.640 |
| Mean ingress-packet residence | 124.142 ms | 198.154 ms | 196.804 ms |

The full selection counted **1,944 ring-overflow notifications**, **2,193 dropped / 2,200 lost packet events**, **12,953 damaged/rejected-AU events** and **3,490 local PLI request notifications**. Those events do not count physical damaged images, prove network packet loss or establish how often feedback reached Xbox. Publication and draw counters can differ at report boundaries. The owner reported closing the app, and resource-close markers were recorded; that alone is not a separately captured PS4-menu or CE-dialog check.

## Preserved ownership and bounds

- The existing video worker remains the sole owner of decoder initialization, AU assembly, Decode, pending pictures, conversion and native teardown. The helper only copies bytes; it never calls Videodec2 or accesses RGB/mailbox state.
- Preserve the **16 ms / four Decode** checks between operations. Native Decode or copy/conversion may exceed that budget. Native type 3 mappings, four output reservations, two inputs, accepted geometry/pitch, staging capacity and output-base validation remain.
- Allocate cached staging with **63 extra bytes**, retain its original allocation pointer and align the usable area to **64 bytes**. Usable capacity remains bounded by the existing native frame reservation, at most **16 MiB**; padding does not authorize extra source reads.
- For a validated aligned source, split the exact NV12 byte length at `floor(length / 2)` rounded down to 64 bytes. Both spans must contain at least 64 bytes. Owner and helper write disjoint staging spans with no gap; the existing bounded remainder handles the final tail and does not introduce an even-pitch requirement.
- Publish at most one helper job using release/acquire sequence ownership. The owner waits for matching completion before reading staging, reusing the pending native reservation or releasing storage. CPU fences order CPU accesses; they do not establish a new GPU-completion guarantee.
- If forced preservation returns an error **or leaves the picture pending**, return before Decode/reuse. Do not start a serial overwrite while a dispatched helper may still write. Helper-job errors must block unsafe continuation.
- Stop and join the helper before native deletion, unmapping or freeing staging. A failed join is latched and retains the complete context and storage. Native thread stack capacity and scheduling/join duration remain unmeasured.

SIMD RGB conversion, the completed-image triple mailbox, display pacing, audio, input reports, HTTP/SDP/ICE behavior and current bad-gap/reset policy are outside this change. No Better xCloud option is adapted. Delivered 720p is a separate observation from the requested 540p profile.

## First-valid-copy check and serial fallback

Each decoder start may check its **first valid native picture once**. If the picture is eligible, allocate a temporary cached reference buffer of exactly the validated NV12 length before dispatch. Complete the parallel copy, then run the existing fenced serial routine on the same leased native source into the reference buffer and compare exactly that length. Neither another Decode nor native reservation reuse may occur between those operations.

Only a byte-for-byte match can report a check pass. A mismatch copies the completed serial reference into staging and disables parallel copying for the remainder of that start. An ineligible first picture, reference-allocation failure or helper-creation failure records a distinct not-checked reason and keeps that start serial. A later decoder start may attempt its own check. The reference buffer is freed after comparison/fallback; a current 720p frame occupies **1,382,400 bytes**, while the general bound remains validated rather than hardcoded to 720p.

A pass validates one leased buffer against the existing serial implementation; it is not an all-buffer, GPU-coherence or image-correctness proof. A mismatch may also reflect source visibility/change and does not identify the helper as its sole cause. Worker elapsed time includes the one-time cost. Separate numeric check counters and timing must mark its reporting window, which is excluded from steady comparisons along with startup/transition intervals.

The implemented one-time numeric byte-check uses result **0 = not checked**, **1 = pass**, **2 = mismatch** and reason **0 = none**, **1 = helper unavailable**, **2 = unsafe/tiny spans**, **3 = temporary allocation failed**, **4 = job failure**. Job failure is a fatal native-video error with the picture still pending; it prevents the next Decode rather than silently continuing with serial output. The first three not-checked causes select serial copying for that decoder start.

## Measurements and A/B/A procedure

The existing `video perf` report retains `copy`, `copy_bytes`, `copy_us` and `copy_max_us`. Successful selected-copy wall time covers dispatch, both spans and completion wait, including forced copies. Extra reference allocation, serial reference, comparison and mismatch fallback are counted in `check_us` rather than folded into `copy_us`; worker elapsed time still includes their cost. Helper results are accumulated by the owner only after completion.

The additional `video copy` report contains:

| Report fields | Meaning |
| --- | --- |
| `parallel`, `serial` | Successful selected-copy calls by actual path |
| `owner_us`, `helper_us` | Elapsed span-copy time; owner time also covers whole serial copies |
| `wait_us`, `wait_max_us` | Owner wait after its span until matching helper completion; total and report maximum |
| `forced_preserve` | Pending-picture preservation before native reservation reuse |
| `check_attempts`, `check_pass`, `check_mismatch`, `check_not_checked` | First-valid-copy diagnostic outcomes, including attempts that cannot be checked |
| `check_bytes`, `check_us` | Bytes compared against a serial reference and the separate check cost |
| `check_window` | **1** when the report contains a new check attempt; exclude that window from steady comparison |

Cumulative snapshot fields are `video_copy_parallel_calls`, `video_copy_serial_calls`, `video_copy_owner_us`, `video_copy_helper_us`, `video_copy_wait_us`, `video_copy_wait_max_us`, `video_forced_preserve_calls`, and `video_copy_check_attempts/pass/mismatch/not_checked/bytes/us`. Retired decoder totals are included across restarts; the owner resets the report maximum after acknowledgment. Reader elapsed times overlap, so adding `owner_us`, `helper_us` and `wait_us` does not reconstruct wall time. Numeric reports reveal no image data, addresses or native handles.

1. **A:** retain the original [0.7.24 checkpoint](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.24) and installer on the PC. Record delivered dimensions and matched complete intervals from serial copying.
2. **B:** after code/build/integrity review, use 0.7.25 and record actual helper mode, first-copy check, selected-copy wall/span/wait times, forced copies, publications/new draws, queue residence/overflow, recovery and closure. Exclude the marked check window and mixed startup intervals from the steady selection; retain their cost separately.
3. **A again:** repeat 0.7.24 with comparable game activity and selection length when authorized. Preserve its existing tag/assets; make no tag move or asset replacement. The PS4 installer folder follows the current-version-only policy; rollback installers remain on the PC.

Different cloud sessions, scenes, cadence, congestion and recovery make an uncontrolled comparison provisional. Ingress residence is local RTP-ring time, and mailbox/picture age begins in the client. Neither measures media PTS age, source capture-to-display delay, Xbox RTT or end-to-end controller latency. Decode counts do not independently establish source cadence. A lower copy time alone is insufficient if publication, stalls or closure regress.

## Review and primary-source provenance

Actual **Claude Opus 5.5** plan review completed in **11 turns / 10 Read calls** and required the post-forced-conversion error/pending guard. Actual Antigravity retry used **gemini-3.1-pro-high**, **1 turn / 4 view_file calls**, and reported no material application flaw in the plan. Its first command was denied before execution and is not review evidence. These plan reviews concern the proposal and frozen baseline.

The final read-only **claude-opus-5-5** review completed in **9 turns / 8 Read calls** with a **scoped source PASS** and no concrete material defect in the frozen patch. The independent review found no material defect in the **eight-file** source scope and byte-matched **eight selected linked-symbol ranges / 8,401 instruction bytes** against the retrieved ELF. It inspected helper calls, single-job ordering, checked spans, first-copy fallback, the pending-picture guard and join-before-release paths. The original serial copy and SIMD conversion function bodies remain unchanged. This is bounded static evidence, not runtime equality or a reproduction of the compiler build.

Antigravity's final **gemini-3.1-pro-high** invocation and **claude-opus-4-6-thinking** retry reached individual quota (**HTTP 429**) with **zero file reads**. They produced no valid final source review or PASS; the completed Antigravity plan review remains separate.

The native build and artifact inspections passed. The **67-file** source/build inventory matches the frozen PC inputs before and after the native build; retrieved ELF/OELF/SELF/SFO hashes match the producer receipts, and selected linked paths plus thread create/join imports were inspected. The **8,912,896-byte** package has matching VM/PC/PS4 readback SHA-256 **`42ac3bf40c8d32fe7abbe2d26615b2940c53b748c45c267755de92d708a26630`**.

The complete PC dependency archive was reconstructed after the original VM compressed transfer was interrupted. Its **9,369 source payloads / 150,890,220 source bytes** and recovered VM manifest match; it contains **9,781 archive entries**. The reconstructed archive is **83,475,236 bytes**, SHA-256 **`bcade0b5d4b5afc3141d8cd39b8164b5f2d0fb8eb558198190645411f4199f4b`**. Different repacking changes the compressed checksum; source-content recovery does not establish compressed-byte identity with the original VM archive. Full application source is separate in the repository. Independent full-archive verification passed for every payload, the recovered metadata, selected project sources and licenses; see the [release notes](RELEASE_NOTES_0.7.25.md) for artifact state.

Reviews and artifact inspections alone do not establish console behavior. The separate [initial console observations](VIDEO_REPORT_0.7.25.md) confirm a byte match for **one** first picture and selected-copy wall time averaging **16.033 ms** over **12 complete intervals / 60.111 s**. They do not establish native stack capacity, stable source FPS, end-to-end latency or safe closure; the owner reports fewer pauses with longer stretches between them, and pauses remain.

The [pinned client comparison](CLIENT_VIDEO_COMPARISON_0.7.24.md) establishes ownership concepts and platform limits. [Moonlight PS4's persistent WC-copy pool](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c#L85-L93) and [parallel chunk operation](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c#L198-L224) are research evidence. Its author's scaling comment is not a measurement of this console. Its application-license evidence remains unresolved at that pin, so its pool, synchronization and implementation are not copied. XCloud4's single-job protocol, existing copy routines and checked spans are original **GPL-3.0-only** project code; foreign cache aliases and PS5/Vita surface APIs are not introduced.
