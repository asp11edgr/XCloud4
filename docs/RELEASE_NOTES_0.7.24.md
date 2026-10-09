# XCloud4 0.7.24 — dedicated video worker and completed-image presentation

Product **0.7.24**, PS4 **APP_VER 00.94**, application identifier **XCLD00001**. The source change targets the serialized native video pipeline and repeated framebuffer rendering observed in 0.7.23. The PS4 interface stays Spanish; repository and release content stay English. **Console playback, fluency, input cadence and clean closure have not yet been observed for this version.**

## Reason and primary-source comparison

The [bounded 0.7.23 attempt](ERROR_REPORT_0.7.23.md) received a first **1280 × 720** picture despite requesting **960 × 540**. Native copy averaged **29.349 ms per image**, conversion **3.003 ms**, and new image draws **13.775/second**. The same selection counted **1765 repeated draws**, overflow and keyframe recovery. The owner reports intermittent stutters and much smoother controls. These are separate local measurements and subjective observations, not a controlled comparison or end-to-end input-latency measurement.

The [native-client comparison](CLIENT_VIDEO_COMPARISON_0.7.24.md) inspected pinned GreenVita, PSBox, ProsperoLight, Moonlight PS4 and AJ GeForce NOW PS4 implementations. Dedicated decoder ownership, bounded completed-image exchange, prediction-preserving presentation omissions and explicit worker joins inform original XCloud4 changes. No upstream application implementation was copied. PS5 GPU paths and Vita decoder-to-texture formats do not establish native PS4 compatibility.

## Implemented source boundary

- **Video ownership:** one worker initializes and tears down Videodec2 and owns RTP ordering, complete-AU assembly, Decode, staging copy and RGB conversion. The main thread maintains UI, samples the controller and presents completed images.
- **Processing:** the worker checks **16 ms / four Decode calls between operations**, then converts the newest pending valid output. A native operation or the final copy/conversion can exceed that budget. The worker yields while idle; this is not a guaranteed 16 ms frame deadline.
- **RGB mailbox:** three heap-backed image slots transfer completed RGB data. Publication, dimensions and generation are committed under a short gate. Drawing leases a slot and reads pixels outside the gate; the worker cannot overwrite a consumer-held image. A newer completed publication can replace an unconsumed publication.
- **Presentation:** the main loop skips repeat scaling and flips while the last live image remains current. A new publication, relevant UI change or **500 ms** overlay refresh triggers a full inactive-framebuffer update. Only a successful display call containing a live image confirms its generation as presented. VideoOut retirement and `sceGnmSubmitDone` remain in the presentation path.
- **Closure:** close first stops and joins the video owner. Its native teardown failure is retained with the context and allocations; the main thread does not retry native video APIs after that owner exits. The existing transport-callback/auth lifetime contract still applies.
- **Diagnostics:** numeric reports add completed publications, superseded pending images, AU submissions, worker idle yields, ingress packet residence and oldest queued-packet age. Main-loop diagnostics measure local pad sampling intervals and skipped idle flips.

Ingress residence measures time spent in the local packet ring. Picture age begins after local conversion. Pad intervals measure how often this client samples the controller. None is Xbox RTT, capture-to-display delay, a wire-level controller acknowledgment or end-to-end audio/video latency. Worker maxima reset after acknowledgment of a report boundary and are not exactly synchronized with independently sampled ingress counters.

## Preserved paths and remaining limits

The native decoder remains bounded at **1280 × 720**, with **type 3 output**, **four native output slots**, existing input reservations, validated picture geometry and staging capacity. Admitted predictive AUs continue through Decode; no new compressed-AU catch-up discard is introduced. IDR recovery, Opus/AudioOut, gamepad report format/sender, **1920 × 1080** display buffers and the exit path remain. The **960 × 540 / 30 FPS / 5000 kbps** request is retained; this release does not establish what format Xbox will deliver.

Moving work out of the main loop does not remove the observed approximately **29 ms** native copy. Triple buffering adds approximately **7 MiB** of RGB capacity compared with the previous single RGB allocation. Native thread stack capacity, startup/teardown timing, queue behavior, delivered dimensions and sustained performance require console evidence. No 30/60 FPS result, fixed stutter or measured latency reduction is claimed.

## Preparation and review status

| Evidence | Current state |
| --- | --- |
| Runtime source and UI integration | Frozen and built; **67** runtime/build/recipe paths match PC and VM before and after compilation |
| Independent selected-source review | Completed without a material finding under the documented ownership/lifetime contract; no runtime execution |
| Actual Claude Opus 5.5 review | **PASS for the source actually read**, no material defect; 16 turns / 15 Read calls / 12 paths, exit 0 and empty stderr |
| Actual Antigravity review | **Gemini 3.1 Pro High** observed; selected application review and separate primary-client comparison completed without a material defect reported; exit 0 and empty stderr |
| Native application/package build and compiled checks | **Succeeded**; APP_VER/VERSION **00.94**, XCLD00001; native thread imports, flip completion and heap-backed SCTP route inspected |
| Matching-source inventory and package hashes | VM/PC artifacts match; **9369** manifest hashes verified, **9781** entries, no forbidden entries |
| PC/VM/retrieved-PS4 package correspondence | Identical package bytes and SHA-256; verified transfer leaves only the 0.7.24 installer |
| Delivered dimensions, video/audio, controls and exit | Pending owner console attempt |

Claude read the full main/media/video implementations, display, controller and audio, plus selected Auth cancellation/callback ranges and the runtime diff. It did not reread the full unchanged RTC implementation or Xbox session closure; the Xbox source diff changes only the version. The media header was partly read directly and otherwise represented by the diff. Its PASS is scoped to those reads.

Antigravity used its official headless CLI with the observed model **gemini-3.1-pro-high**. Its first final consultation read six application/document paths. A separate comparison then made **17 view_file calls across ten paths**, including eight pinned primary-source files from GreenVita, PSBox, ProsperoLight, Moonlight PS4, AJ GeForce NOW PS4 and Better xCloud. These reads support comparable ownership/presentation concepts, not literal code integration, PS4 GPU API compatibility or verified playback. The ingress ring uses a short try-lock gate; queue residence measures RTP packets, not AU or end-to-end latency.

Both reviewers used read-only tools, accessed no path outside their allowlists and left the frozen source unchanged. All frozen/current review hashes match. Native initialization, teardown/join, default thread capacity and decoder operation on the new owner still require runtime evidence. Private source-review receipts preserve the inspected file hashes. The client comparison's pinned upstream files were verified against Git-tree blob identifiers and SHA-256 fingerprints. Source reviews do not establish binary behavior or hardware performance. No automated tests were added or run for this checkpoint.


## Immutable build artifacts

- **XCloud4-0.7.24.pkg:** 8,912,896 bytes; SHA-256 `8c8b40f778e6fc3d899dd2fcdd69b64b6ae9232b2b9c39ada77a29a70fbeb5ef`.
- **XCloud4-0.7.24-dependency-sources.tar.gz:** 83,583,097 bytes; SHA-256 `3160a42c46369167b613426fa4bfe2c6ec58346a6917af75ed1c99af2a6db8aa`.
- **Native ELF:** 6,552,000 bytes; SHA-256 `733b5b66a05bafbcb1b1b7cfbb6540eba96a423a055cfea12175ba5f66281653`.
- **OELF:** 6,766,960 bytes; SHA-256 `cade599d6c742bd61b23fd0898d2e538e39dac5a034678f01197c4145e943bef`.
- **eboot.bin:** 4,539,712 bytes; SHA-256 `87d183c699f5f3ff6c8a9d59cb6b96a61db690728310766b2bea2cb6a38bc399`.

The complete source manifest checks 150,890,220 bytes. Sixteen selected project/recipe paths match exactly, and the exported applied SCTP source matches the compiled dependency snapshot. The archive contains patched dependencies, original licenses, recipes and native transport adapters; full application source is preserved in Git at the release checkpoint. SDK files, runtime binaries, credentials and raw console logs are excluded from Git/source export.

Compiled inspection confirms `scePthreadCreate` and `scePthreadJoin` resolve to libkernel, and `sceGnmSubmitDone` resolves to libSceGnmDriver and remains before flip waiting. The SCTP receive function keeps its heap-backed buffer and **552-byte** local stack reservation. The video batch is inlined into its worker: observed local reservations are **2168 bytes** for that worker and **2232 bytes** for main, excluding saved registers and deeper callees. These values do not establish the default thread stack capacity or decoder ABI behavior on the new thread.

The package has been read back from PS4 and matched against the VM/PC hash. The previous installer was removed only after this correspondence passed. **Installation, actual video/audio, sustained fluency, control cadence and clean closure remain pending the owner's console attempt.** The existing 0.7.23 release/tag and artifacts are preserved. No automated tests or application execution were performed by the build or source reviewers.
