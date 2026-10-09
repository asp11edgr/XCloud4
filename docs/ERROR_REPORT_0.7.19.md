# XCloud4 0.7.19 — first live media, input pending and external-close failure

The owner confirms real game video and audio on the PS4. Audio works well; video is choppy or very slow, with a later ARK attempt reportedly taking several seconds to update. Game controls do not work. Closing from the PS4 menu produces the owner's reported **CE-34878-0**.

## Matched build and preserved evidence

Product **0.7.19**, PS4 **APP_VER 00.89**, title **XCLD00001**.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| PKG | 8,912,896 | `91761dfb772f763c7924118d5e38778ade56e933061bece57afc2558367faf8a` |
| Corresponding dependency source | 83,580,007 | `e1d252ee00a22b95645b5e8b7d4c9f272f3a6f85ae6ec5d7476b94817fe76ac6` |
| Private first-media/closure extract | 84,554 | `76d7bbe2a2271c7694323dd4b10828160739cf58378b20022f114ea48e4fca2e` |

The **1,210-line** first-attempt extract is preserved privately and ends before the next 0.7.19 application banner. Raw account, system and console logs are not published. The [release notes](RELEASE_NOTES_0.7.19.md) record the actual build, review, source manifest and VM/PC/PS4 package correspondence.

## First live transport/media observations

The captured attempt passes SCTP initialization and nonblocking start, followed by **SCTP transport state 2**, **RTC state 2** and **Xbox channels prepared**. It then records:

- First Opus packet decoded: **960 samples**.
- First H.264 image: **1280 × 720**, pitch **1280**.
- Successful keepalive responses while the stream remains active, including a pulse at approximately **239 seconds** elapsed.

These decoder observations and the owner's listening/viewing report establish the first live-media milestone. The previous 0.7.18 receive-worker crash does not recur before media in this attempt. The smaller linked receive reservation is confirmed, but the actual native default worker-stack size remains unmeasured. No measured playback FPS or complete final packet/drop summary is available.

## Why the game controls do not work

The current transport creates the Xbox input channel and sends client metadata, but it has **no gamepad-report sender**. Local DualShock input is read for application navigation; it is not yet transmitted as Xbox gamepad frames. Existing local OPTIONS/CIRCLE/SQUARE actions also need a distinct in-game control scheme before those buttons can serve Xbox Menu/B/X.

This is an incomplete implementation boundary, not evidence that Xbox rejects correctly encoded gamepad reports. Encoding, periodic sending, connection gating, neutral/release behavior and safe sender shutdown are the next input work.

## External-close exception

The shell requests suspension before closing XCloud4. The kernel reports that `submitDone()` has not been called, then raises asynchronous exception **0xa0d0c00a**, named **CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC**. It explicitly reports **no thread information** for this GPU/system exception. The coredump and application-kill sequence follows.

This identifies the immediate failure as a graphics-submission/suspension timeout. It is distinct from the 0.7.18 SIGSEGV at a `doRecv` call instruction; there is no faulting CPU thread/address here to map to that function. The correct submission-completion and suspend/close handling must be checked in the native presentation path. A repaired external-close result is not yet established. No normal XCloud4 cleanup summary was captured for this forced closure.

## Video delay and next observations

The owner reports slow updates in both the first game and ARK. Current source processes video and draws in the UI thread, converts decoded output to RGB, and scales it to the display using CPU work; the main loop also paints the menu behind live video. Bounded ingress queues can discard packets when full or contended and request a new keyframe.

Those are concrete implementation costs and recovery paths to inspect. The current log does not quantify their durations, queue pressure, packet loss or achieved FPS, so it does not establish which dominates the delay. The next version should reduce redundant drawing/scaling work and record bounded numeric timing/queue counters while preserving the working audio and validated transport.

## Second attempt: ARK

The second 0.7.19 launch also reaches connected WebRTC, the first 960-sample Opus decode and a 1280 × 720 H.264 image. The last successful keepalive shown is approximately 300 seconds elapsed. RTC later reaches state 5 and ICE state 6; the application closes Xbox successfully with HTTP 200 and reports a WebRTC connection failure. Its final received counts are **144,269 video packets** and **14,933 audio packets**. These are packets, not decoded or displayed frame counts. This attempt has an application cleanup summary; it is separate from the first attempt's forced external closure. The recorded states do not establish why the remote connection ended.
