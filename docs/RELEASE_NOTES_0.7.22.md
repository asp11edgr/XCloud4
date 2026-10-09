# XCloud4 0.7.22 — SSE2 color conversion

Product **0.7.22**, PS4 **APP_VER 00.92**, application identifier **XCLD00001**, has frozen conversion source and a successful native build. The PS4 interface remains Spanish; repository and release content remain English. **At packaging, fresh Claude source review, compiled instruction audit, matching-source verification and VM/PC/retrieved-PS4 package integrity were confirmed; the console result was pending.** The later hardware result is recorded in the dated addendum below.

## Reason

The owner confirms [0.7.21 video](ERROR_REPORT_0.7.21.md) is substantially better than 0.7.20 but still choppy. Its early sustained sample records **29.354 ms to copy** and **20.068 ms to convert** each image, with **12.979 new image draws per second** across **80.513 seconds**. Queue overflow, reconstruction recovery and later lower update rates remain. D-pad and stick operation is confirmed by the owner; a complete controller matrix has not been checked.

This bounded source change targets CPU color conversion. Its improvement has not been measured and no FPS or smoothness result is promised.

## Implemented conversion boundary

- Converts staged NV12 into the existing RGBA output in **eight-pixel SSE2 blocks**, loading exactly eight luma bytes and eight interleaved chroma bytes per block.
- Uses signed **32-bit** multiply/add results (`PMADDWD`), the previous **+128 / arithmetic shift by 8** rounding, then `PACKSSDW` followed by `PACKUSWB` for the existing 0–255 clamp. The prior BT.601 integer coefficients and output byte order are retained.
- Uses the matching scalar formula for a short row or its final **0–7 pixels**. Existing geometry, pitch and capacity checks remain; no extra row-alignment or even-pitch restriction is introduced.
- Preserves native output memory type, decoder input/output rings, allocations and picture ownership.
- Retains separate copy/conversion timing and reports conversion **mode 2**, block size 8 and the scalar-tail bound at startup.
- Leaves gamepad mapping and sender behavior unchanged.

The helper is an original XCloud4 implementation. [Intel's C++ Intrinsic Reference, document 312482-003US](https://www.intel.com/content/dam/develop/external/us/en/documents/18072-347603.pdf), printed pages **86, 94, 101 and 108**, supplies the `PMADDWD`, `PSRAD`, `MOVQ` and saturation contracts. These instruction contracts do not establish the performance of the compiled code on the owner's console. The working native memory allocation is retained. CPU memory fences do not provide GPU-completion evidence.

## Review, build and artifact status

| Evidence | Current state |
|---|---|
| Final frozen conversion source | Implemented; original SSE2 eight-pixel helper and matching bounded scalar tail |
| Fresh actual Claude Code Opus 5.5 review | **PASS**; 11 turns / 10 reads / 10 files; no material findings |
| Full native application/package build | Succeeded; APP_VER and VERSION **00.92**, title identifier **XCLD00001** |
| `XCloud4-0.7.22.pkg` | **8,912,896 bytes**; SHA-256 `47042d8a9afb603fe7f4df15b497cb2a9603a5aa8b7fa78ba6bd9fefcebc53aa` |
| VM / PC / retrieved PS4 package correspondence | Identical size and SHA-256; only the 0.7.22 installer remains after verified transfer and removal of the earlier installer |
| `XCloud4-0.7.22-dependency-sources.tar.gz` | VM/PC hash verified: **83,584,943 bytes**; SHA-256 `494440ddb0be8ebe591028d0faba0992850027ba6d9d2d44894b01f468c79fcc` |
| Matching-source archive inventory | All **9369** manifest hashes verified; **9781** archive entries / **150,890,132** source bytes; no forbidden entries or privacy findings |
| Copy/conversion timing, queue pressure and game behavior | Pending console attempt |

The native ELF has SHA-256 `d825b2695511748fd70d65d201c7eb6b1cdf71d8eda00f233357316bf7e269ae`. The OELF is **6,736,328 bytes**, SHA-256 `79eb063b6d487446aa673127514106f1eb28a4acf9b876e4fbcfca7422ef6fe1`; `eboot.bin` has SHA-256 `6ffcdffdf5555a7b0693782b6f318907a2df46415dd0260ea13223b43cd0110e`.

The final read-only compiled-code audit returned **PASS**. Actual object/ELF instructions confirm `MOVQ`, `PMADDWD`, `PSRAD` by 8, saturation packs and the bounded scalar tail. Linked coefficients, rounding, chroma duplication and RGBA byte order match the preceding arithmetic; intermediate bounds fit signed 32 bits. The conversion function uses only general x86/SSE2 instructions. The separate streaming-copy helper retains its runtime CPUID gate and CPU ordering fences. Neither this inspection nor the build establishes color correctness, speed or game smoothness on the console.

The fresh actual Claude Opus 5.5 review completed successfully with no material finding, no stderr and no reads outside its allowlist. Independent checks confirm all **nine** frozen/current review-path hashes match. Claude did not read `live_media.h`; its unchanged hash was verified independently. Its notice reading covered only lines **125–143**. The review was source-only: Claude did not build, execute, test or audit the binary. Its PASS does not establish a performance improvement or 30/60 FPS.

The compiled VM source matches **67** frozen PC runtime/build/script/notice/license files. Sixteen selected export files also match exactly. The dependency archive contains patched dependency sources, recipes and native adapters; its PC copy passed complete manifest, exclusion and privacy checks. The full application, including media/main source, is preserved separately in Git at the corresponding release checkpoint.

The verified 0.7.21 package/source evidence remains preserved. Its source-only review does not validate this conversion change. No automated tests have been added or run. Native copy, draw/presentation time, keyframe recovery, complete controls and external closure remain relevant limits beyond this checkpoint's conversion scope.

## Hardware addendum — 2026-10-09

The owner reports better video but continuing choppiness and greater control-to-image delay; input latency has not been measured. The preserved **24-interval / 120.487-second** sustained sample records **2350 new image draws**, averaging **19.504 per second**, with **3.001 ms conversion** and **29.337 ms copy** per image. The conversion-region comparison against the separate 0.7.21 sample is about **6.686×**; it is not a controlled benchmark or an FPS multiplier. The `damaged` counter records access-unit resets/discards, not physical damaged frames.

See the [0.7.22 measured result and limitations](ERROR_REPORT_0.7.22.md). Original review metadata, package/source hashes and published artifacts remain unchanged. The owner-selected next checkpoint requests 540p; its delivered dimensions and performance are unverified.
