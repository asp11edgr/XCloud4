# XCloud4 0.7.21 — stage native NV12 before CPU conversion

Product **0.7.21**, PS4 **APP_VER 00.91**, application identifier **XCLD00001**. The PS4 interface remains Spanish; repository and release content remain English. **At the original packaging checkpoint, native build, package/source integrity, compiled instruction inspection, fresh Claude source review and PS4 delivery were confirmed; the console result was pending.** The later console result is recorded in the dated addendum below.

## Reason

The actual [0.7.20 capture](ERROR_REPORT_0.7.20.md) reaches live audio/video and the input sender, but one interval spends **4,995,965 µs on ten RGB conversions**, compared with **95,930 µs on ten Decode calls** and **57,716 µs on ten image draws**. New draws average about **1.90 per second** in that interval. Repeated conversion measurements remain near **500 ms per picture**, while the 256-packet input queue fills and drops packets.

The native output is `WC_GARLIC` direct memory. Reading its Y/UV bytes repeatedly in the scalar conversion loop is a concrete access pattern to improve. The memory-access explanation is a hypothesis supported by primary references; it is not yet a measured 0.7.21 correction.

## Implemented copy boundary

- Preserves the decoder's native output ring, type 3 allocation, queried frame-size/alignment limits and existing ownership validation. Requires nonzero even width/height, pitch at least the width and no greater than 8192, and validated picture/owned-buffer capacities; no even-pitch restriction is introduced.
- Allocates persistent bounded CPU heap storage for the NV12 picture during setup, rather than allocating it for each frame.
- Copies only the validated picture extent, **pitch × height × 3 / 2**, once into that storage before RGB conversion.
- Selects the original aligned SSE4.1 streaming-load path only when runtime **CPUID leaf 1, ECX bit 19** reports support; otherwise uses baseline SSE2 copying. Both paths preserve safe copy bounds/remainder handling. Conversion reads CPU storage with the existing color arithmetic. No SSE-path speed is assumed before measurement.
- Records copy bytes/calls/duration separately from RGB conversion and retains end-to-end media timing, so an apparent conversion reduction cannot hide the added copy cost.
- Preserves decoder/output lifetime and retains native buffers when teardown fails. A copied picture must be obtained before its native slot can be reused.

The frozen source has been compiled and independently inspected in the linked native instructions. No framebuffer is changed to ONION or write-back mapping. **`MFENCE` orders CPU memory accesses; it is not evidence of GPU/decoder completion and does not replace native output-lifetime rules.** This checkpoint adds no rendering worker or decoder pipeline-depth change.

## Research references and limits

[Moonlight PS4, pinned source](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c#L111), commit **`61427a214d4e632ee246816a98ee4f2374844a73`**, uses `MOVNTDQA` streaming reads into CPU bounce storage. Its commentary about generic-copy costs is the author's observation, not a measurement reproduced by XCloud4. No LICENSE/COPYING was found in the inspected tree, so it remains a research reference; its implementation is not copied.

[Intel SSE4.1 documentation](https://www.intel.com/content/dam/develop/external/us/en/documents/d9156103-705230.pdf), section **2.2.3**, describes streaming loads from WC memory, required **16-byte source alignment** and grouping accesses to the same streaming line. The speed of that hint on this PS4 must be measured. The actual SDK-linked `memcpy` inspected during preparation uses ordinary loads, so a generic copy is not evidence that the streaming instruction is emitted.

[AJ GeForce NOW PS4](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/stream/PS4VideoOutRenderer.cpp), commit **`87568ca50a89e6f8dd65fe005d21521c27e65c26`**, provides separate SIMD color-conversion/native-size VideoOut and Piglet rendering references. Those are larger alternative paths; they are not integrated or hardware-verified in this change.

The installed OpenOrbis `Videodec2.h` contains untyped placeholder declarations and does not document a cacheable output-memory contract. Public client experiments with cacheable aliases therefore do not establish an official requirement or justify changing the working native output allocation here.

## Review, build and artifact status

| Evidence | Current state |
|---|---|
| Frozen source and actual Claude Code Opus 5.5 review | PASS; fresh read-only source review completed in 13 turns, without a material finding |
| Full native application/package build | Succeeded |
| `XCloud4-0.7.21.pkg` | **8,912,896 bytes**; SHA-256 `9d4177d81e0f9b4c252d2d6a7de1ccc181434f8c781e754019b887a0fd4b7427` |
| VM / PC / retrieved PS4 package correspondence | Identical size and SHA-256; only the 0.7.21 installer remains after verified transfer and removal of the earlier installer |
| `XCloud4-0.7.21-dependency-sources.tar.gz` | **83,582,572 bytes**; SHA-256 `12eeeddfd73448e34ab1e9bdd66380818e2a58227d84900a2e974f7d7ee73f6e` |
| Matching-source inventory | **9369** manifest entries verified; **9781** archive entries; **150,889,328** source bytes; no forbidden entries |
| Copy/conversion timing, queue pressure and game behavior | Pending console attempt |

The compiled VM source matches **67** frozen PC runtime/build/script/notice/license files. The archive contains the patched dependency sources, recipes and native adapters. It does not contain all application media/main source; the complete application source is preserved in Git at the corresponding checkpoint.

The compiled native ELF has SHA-256 `f1a47412225373c7f44a18156a5d8ff59d6f95aa7892d18d023e237230aa6d56`; the OELF has SHA-256 `fc654df6ee4f94874e0e97b3f4dc0ece45ec0ca0a27dfb3f9ceccafef8b252db`. Read-only instruction inspection confirms CPUID leaf 1/ECX bit 19 gates `MOVNTDQA`, retains the baseline SSE2 path and bounded tails, emits `MFENCE`, and makes conversion read the staging buffer. The Gnm import still binds the correct module, and the SCTP receive implementation retains its heap buffer and **552-byte** local stack reservation. These are compiled-code checks, not a console benchmark or closure proof.

The earlier actual Claude Opus 5.5 performance analysis completed in **15 turns / 14 reads / 14 files**. A separate fresh review of the 0.7.21 source returned **PASS** in **13 turns / 12 reads / 12 files**, with no access outside its allowlist and matching source hashes after review. It read the media/video implementation and application/display/ABI/build context, plus the SDK memory-type definition; notice reading was limited to lines 120–139. It did not execute the application or a hardware benchmark. It identified a pre-existing measurement limitation: decoder restart resets video counters while the previous report snapshot survives, so one interval can show a wrapped delta. Exclude that interval from timing comparisons. This checkpoint does not alter the unrelated reset behavior.

The 0.7.20 input mapping, bounded processing and graphics-completion changes are retained. The owner's controller observation is tentative; individual input paths and external closure remain to be checked. No achieved FPS or performance gain is promised for 0.7.21, and no automated tests have been added or run. Earlier artifacts remain preserved.

## Hardware addendum — 2026-10-09

The owner confirms much better video than 0.7.20, with remaining choppiness, and explicitly confirms the **D-pad and sticks work**, retracting the earlier D-pad concern. The preserved early sustained capture records **49.422 ms per copy + conversion** and **12.979 new image draws per second** across **80.513 seconds**. The preparation-region ratio against the earlier direct-conversion sample is about **10.109×**, from different live intervals rather than a controlled benchmark. Queue overflow and keyframe recovery remain; the complete control matrix and external closure are unverified.

See the [0.7.21 measured result and limitations](ERROR_REPORT_0.7.21.md). This later hardware observation does not turn the original source-only Claude review into an executed hardware test. Original package/source hashes and review metadata above remain unchanged.
