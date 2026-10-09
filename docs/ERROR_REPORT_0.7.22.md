# XCloud4 0.7.22 — faster conversion, remaining choppiness and perceived delay

Product **0.7.22**, PS4 **APP_VER 00.92**, application identifier **XCLD00001**. The owner reports better video than 0.7.21, with continuing choppiness and greater delay between a control action and its visible result. **Input latency has not been measured**; the observation does not establish a controller transport, network or server cause.

## Evidence boundary

The preserved current-version snapshot records streaming-copy **mode 1**, SSE2 conversion **mode 2**, first Opus decoding with **960 samples**, and a **1280 × 720** H.264 image with pitch **1280**. The sustained selection excludes the first mixed startup/media interval. It contains **24 complete paired intervals**, totaling **120.486935 seconds**, **2350 copies/conversions** and **2350 new image draws**. Raw console/account logs remain private.

The [published 0.7.22 release](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.22) preserves the reviewed build and matching artifacts. Those source/binary integrity checks are separate from these later hardware observations.

## Measured local processing

| Operation | Observed mean |
|---|---:|
| Native Decode | 4.842 ms per call; 1.903 calls per media tick |
| Native NV12 copy into CPU storage | **29.337 ms per image** |
| SSE2 NV12-to-RGB conversion | **3.001 ms per image** |
| Copy plus conversion | 32.338 ms per image |
| Image draw | 5.528 ms per call |
| Presentation call | 3.270 ms per call |
| New image draws | **19.504 per second** |

The earlier frozen [0.7.21 selection](ERROR_REPORT_0.7.21.md) records **20.068 ms per conversion**. The resulting conversion-region ratio is about **6.686×**, while the copy mean remains near its earlier **29.354 ms**. These are different live sessions, not a controlled benchmark. Neither the conversion ratio nor the application new-draw rate represents panel FPS, server rendering FPS or a guarantee of smooth playback.

## Remaining bottlenecks and uncertainty

Copy occupies about **57.22%** of the selected elapsed intervals, compared with **5.85%** for conversion. The same selection records **8 full-queue events**, **10 dropped-packet counter increments**, **10 lost-packet counter increments** and **63 keyframe requests**. The **1824 `damaged` increments** count media-parser access-unit resets/discards; they are not a count of visibly damaged decoded frames. Local counters alone do not prove physical network loss or its cause.

Decode, copying, drawing and presentation still share the existing serial processing path. The between-operation decode budget cannot interrupt a single copy. The capture does not establish absolute video age, end-to-end input latency or wire-level acknowledgment of each control report. Full button/chord/cancellation validation and external closure remain separate work. Counter intervals spanning a decoder restart must be excluded when they contain the known wrapped delta.

## Next owner-selected scope

The owner selected [0.7.23's 960 × 540 request](RELEASE_NOTES_0.7.23.md) to prioritize fluency. The implemented change updates the two startup capability/dimension messages and adds a numeric request diagnostic; 30 FPS and 5000 kbps remain. Native memory type, decoder bounds/rings/staging, audio, input and the 1080p display path are preserved. Its native build, final compiled-profile audit, complete source inventory, fresh Claude Opus 5.5 PASS and VM/PC/retrieved-PS4 package integrity are confirmed. A request does not prove Xbox delivers 540p: the next attempt must record actual decoded dimensions and fresh processing measurements. No 0.7.23 FPS, latency or smoothness improvement is yet established.
