# XCloud4 0.7.23 — 540p requested, first decoded picture still 720p

Product **0.7.23**, PS4 **APP_VER 00.93**, application identifier **XCLD00001**. The bounded second game attempt records the startup request **960 × 540 / 30 FPS / 5000 kbps**, but its first valid H.264 picture is **1280 × 720**, pitch **1280**. The request therefore did not produce a 540p first picture. This does not establish whether Xbox ignored, clamped or renegotiated the request.

The [release's compiled-profile and artifact checks](RELEASE_NOTES_0.7.23.md) remain valid evidence of what the application sends. They do not establish the format supplied by Xbox.

## Owner observation

After further 0.7.23 attempts, the owner reports that video worsened, with periods of fluid motion interrupted by stutters, while the controls feel much smoother than before. This is subjective feedback across live attempts, not a measured input-latency result or a controlled comparison. Input implementation was not changed in 0.7.23; the reason for the perceived improvement is not established.

## Bounded measurement

A private diagnostic snapshot separates this game attempt from the preceding attempt under the same application launch. It includes the recorded closing/canceled states and final numeric summary. Authentication responses and tokens are excluded; the ongoing capture remains active.

The sustained selection excludes **three warmup pairs** and the **first mixed startup/media interval**. It contains **27 complete paired intervals**, totaling **135.752188 seconds**, **1870 copies/conversions** and **1870 new image draws**. No wrapped-counter interval was found in this selection.

| Operation | Observed mean |
|---|---:|
| Native Decode | 8.538 ms per call |
| Native NV12 copy into CPU storage | **29.349 ms per image** |
| SSE2 NV12-to-RGB conversion | **3.003 ms per image** |
| Copy plus conversion | 32.352 ms per image |
| Image draw | 5.519 ms per call |
| Presentation call | 8.951 ms per call |
| New image draws | **13.775 per second** |

Five-second interval update rates range from **9.191 to 19.525 new draws per second**. These are application image updates, not panel or server FPS. This live attempt is not a controlled benchmark against 0.7.22; its delivered first picture remains 720p, so it cannot demonstrate the benefit of actual 540p delivery.

## Queue and recovery observations

The selection records **1932 full-queue events**, **2003 dropped-packet counter increments**, **1985 lost-packet counter increments** and **1698 keyframe requests**. It also records **1765 repeated image draws**. The **5579 `damaged` increments** count media-parser access-unit resets/discards, not visibly damaged physical frames.

Copying remains near **29 ms per image** and conversion near **3 ms**. Queue/recovery pressure and repeated images remain present. These counters do not prove a physical network-loss cause, absolute video age, wire-level controller acknowledgment or end-to-end input latency.

## Recorded closure and limits

The attempt records **closing state 4**, then **canceled state 6**, with final **HTTP 200 / error 0**. These observations are not evidence of a crash or an explanation of what initiated closure. No capture control, additional binary run, automated test or hardware probe was performed for this analysis.

The request change has not demonstrated lower delivered resolution or improved fluency. Native allocation type, bounds, rings, staging, input, audio and display behavior remain as the reviewed checkpoint. The original package, matching source and release assets are preserved; any further change requires its own source/build and console evidence.

The [Better xCloud source comparison](BETTER_XCLOUD_COMPARISON_0.7.23.md) identifies profile/bitrate concepts for further investigation and distinguishes browser rendering/audio options from the native pipeline. No option has yet been integrated.
