# Better xCloud comparison with XCloud4 0.7.23

## Scope and observed result

Better xCloud was inspected at immutable commit `f8397043f6d2148d2345d508902a38c69cf1ee20`, under its [MIT license](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/LICENSE#L1-L23). XCloud4 was read at `afeb9cd36c3096584df7cb8169ae40afb2f891f0`. This is a primary-source comparison of selected files, not a browser-to-PS4 integration or a performance validation.

The observed XCloud4 0.7.23 session requested **960×540** but decoded its first picture at **1280×720, pitch 1280**. Copy remained approximately **29 ms per image**, conversion approximately **3 ms**, and the packet queue repeatedly filled. This establishes that the observed picture did not follow the 540p request; it does not explain why or prove that another provider profile supports 540p.

The preceding 0.7.22 sample measured 19.504 new draws/second over 120.487 seconds. Copy averaged 29.337 ms and conversion 3.001 ms. These local timings do not measure input RTT, queue residence, end-to-end audio/video latency or CPU utilization.

## Options and native relevance

| Mechanism | Verified Better xCloud behavior | Relevance to XCloud4 |
| --- | --- | --- |
| Resolution profile | Offers auto/720p/1080p/1080p-HQ. Updates both `settings.osName` and device-info: android/windows/tizen respectively. | Profile selection is an adaptable signaling idea. These paths show no 540p option or guaranteed delivered dimensions. |
| Bitrate/codec | Reorders codec preference and inserts/replaces media SDP `b=AS` through a JavaScript hook. | SDP preferences are portable concepts; the JavaScript hook is browser-specific. Lower compressed bitrate does not shrink an unchanged 720p NV12 picture. |
| Maximum FPS | Skips local canvas updates according to the configured interval. | This caps local rendering, not demonstrated server frame rate. |
| WebGL2/WebGPU rendering | Uses `HTMLVideoElement` texture upload or external texture import. | Browser APIs cannot be activated directly in OpenOrbis. They do not prove zero-copy or a native speedup. |
| Audio options | Conditional `AudioContext` latency-hint/volume patch; combined media playback is experimental and off by default. | WebAudio/media-element changes are not native Opus/AudioOut tuning. The current good native audio remains unchanged. |

Primary pinned sources: [resolution enum](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/enums/pref-values.ts#L32-L37), [profile/device-info](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/network.ts#L312-L355), [play interception](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/xcloud-interceptor.ts#L129-L174), [bitrate/codec hook](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/monkey-patches.ts#L91-L115), [SDP bitrate](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/sdp.ts#L55-L115), [local FPS scheduler](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/modules/player/base-canvas-player.ts#L80-L111), [WebGL2](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/modules/player/webgl2/webgl2-player.ts#L35-L50), [WebGPU import](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/modules/player/webgpu/webgpu-player.ts#L112-L113), [conditional audio installation](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/index.ts#L429-L437), [AudioContext wrapper](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/monkey-patches.ts#L130-L153), [combine default](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/settings-storages/global-settings-storage.ts#L226-L233).

## Differences and limits

Better xCloud supplies browser/SDK/display metadata. XCloud4 identifies its native Sony/PS4/Orbis device and requests dimensions through later data-channel capabilities; its decoder and offer still permit 720p. This difference does not establish a causal explanation for the delivered 720p picture. See [native play/device metadata](https://github.com/asp11edgr/XCloud4/blob/afeb9cd36c3096584df7cb8169ae40afb2f891f0/src/auth/xbox_live.c#L29-L60) and [startup capability messages](https://github.com/asp11edgr/XCloud4/blob/afeb9cd36c3096584df7cb8169ae40afb2f891f0/src/streaming/rtc_transport.c#L474-L477).

Better xCloud's optional [prevent-resolution-drop patch](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/modules/patcher/patcher.ts#L1252-L1278) rewrites reported browser frame timings. It does not reduce actual copy or conversion cost. No `playoutDelayHint` or `jitterBufferTarget` was found in the 24-file fetched corpus's inspected active paths; this is not a whole-repository or underlying-SDK absence claim.

The useful next focus is measured native video backlog/copy cost. Queue age is unmeasured; any future policy must preserve complete H264 units, prediction dependencies, keyframe recovery and owned buffers. No arbitrary predictive-frame dropping, decoder memory-type change, unverified cached-memory API, GPU implementation or worker redesign is proposed as an established fix. Provider-profile/bitrate options require actual delivered dimensions and quality evidence. Audio has no supported reason for a rewrite here.

## Actual Claude participation

Claude Code completed this comparison with **claude-opus-5-5 requested and observed**, exit 0, empty stderr, 30 reported turns and 29 Read calls over 24 allowlisted files. MCP was disabled; no tools other than Read were called. The local audit found no outside-path accesses and verified frozen hashes against the manifests and native commit. Several external files and `xbox_live.c` were read in explicit partial ranges; this was not a repository-wide audit.

Private source/review artifacts retain the raw model result. Its CPU-load and zero-copy statements were calibrated: summed serialized intervals are not CPU-utilization measurements, and browser texture calls do not establish zero-copy. **No Better xCloud option has been adapted to XCloud4.** This comparison changed no application code, audio, SDP, queue, build or runtime behavior and ran no tests or hardware probes.
