# XCloud4 0.7.23 — owner-selected 540p video request

Product **0.7.23**, PS4 **APP_VER 00.93**, application identifier **XCLD00001**, has frozen request source and a successful native build. The owner selected a smaller video request to prioritize fluency. The PS4 interface remains Spanish; repository and release content remain English. **Fresh Claude source review, compiled-profile audit, matching-source verification and VM/PC/retrieved-PS4 package integrity are confirmed. The console result remains pending.**

## Reason

[0.7.22 measurements](ERROR_REPORT_0.7.22.md) confirm conversion falls to **3.001 ms per image**, while copying remains **29.337 ms**. The selected live sample records **19.504 new image draws per second**. The owner reports better video but continuing choppiness and greater control-to-image delay. That delay is an observation, not a measured latency or a demonstrated network/input defect.

## Implemented request boundary

Two fixed RTC startup JSON messages change:

- **Client device capabilities:** set the top-level and nested video maximum dimensions to **960 × 540**, along with the nested video's actual width and height.
- **Dimensions changed:** request horizontal/vertical and preferred dimensions of **960 × 540**, with the safe area running from **0, 0** to **960, 540**.

**30 FPS** and the **5000 kbps** bitrate ceiling remain unchanged. Custom-resolution support, codec selection and other startup properties are preserved. The native type 3 output allocation, **1280 × 720** decoder bounds, staging storage, fixed rings, processing budget, gamepad sender, audio and **1920 × 1080** display path are unchanged.

A numeric startup diagnostic records **960 × 540 / 30 FPS / 5000 kbps**. The request does not establish the delivered resolution. The console must report actual decoded width, height and pitch; copy/conversion duration, image-update rate, queue/recovery behavior and owner-observed fluency remain to be measured.

## Review, build and artifact status

| Evidence | Current state |
|---|---|
| Final request source and fresh actual Claude Opus 5.5 review | Frozen; **PASS**, 8 turns / 7 reads / 7 files, without material findings |
| Full native application/package build | Succeeded; APP_VER and VERSION **00.93** |
| `XCloud4-0.7.23.pkg` | **8,912,896 bytes**; SHA-256 `5a0534f677248280165177e3b7c77bc0b42df48d72020b99255dc81959dfe9e3` |
| VM / PC / retrieved PS4 package correspondence | Identical size and SHA-256; only the 0.7.23 installer remains after verified transfer and removal of the earlier installer |
| `XCloud4-0.7.23-dependency-sources.tar.gz` | VM/PC hash verified: **83,583,568 bytes**; SHA-256 `eed50d430c31ea50ba981d90a67ce66bca613e7c1ede0b99fabed631a615d00c` |
| Matching-source inventory | All **9369** manifest hashes verified; **9781** archive entries / **150,890,220** source bytes; no forbidden entries or privacy findings |
| Delivered dimensions and console performance | Pending console attempt |

The native ELF has SHA-256 `947dc7fc33ad02a3dbc55d90ca2b898678c835d0dca5b44276954c5dfd2b4ad2`. The OELF is **6,736,336 bytes**, SHA-256 `6a5ee328fcfb41c20a66a5b0ccb08366282a60e2caebee27e380705238b24d69`; `eboot.bin` has SHA-256 `47e0860cac93eaa66cc4fd79c4803423ae848e92f5849d53cecdbb6342a5ebe6`.

The final read-only compiled-profile audit returned **PASS**, confirming the exact two JSON payloads and diagnostic at their active startup references. Native video-object bytes are identical to 0.7.22, the heap-backed SCTP receiver retains its **552-byte** stack reservation, and flip submission still precedes Gnm completion. Existing picture validation accommodates 960 × 540 with padded pitch inside the retained 1280 × 720 capacity. These checks do not establish the resolution delivered by Xbox or console performance.

The actual Claude review completed with exit code 0, no stderr and no access outside its allowlist. Independent checks confirm all **nine** frozen/current review-path hashes match. RTC reading covered lines **400–499**; video/media/ABI implementations were read, while `main.c` was represented by its version-only diff. Unchanged headers and notices were checked against the baseline rather than reread in full. This source-only review did not measure delivered dimensions or input latency.

The compiled VM source matches **67** frozen PC runtime/build/script/notice/license files, and **16** selected export files match exactly. The dependency archive's PC copy passed all manifest, exclusion and privacy checks. It contains patched dependency sources, recipes and native adapters. Full application source, including media/main, is preserved separately in Git at the corresponding release checkpoint.

The published 0.7.22 checkpoint and source/package artifacts remain unchanged. This preparation does not establish a 30 FPS result, lower input latency or fixed choppiness. No automated tests have been added or run.
