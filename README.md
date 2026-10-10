# XCloud4

Experimental native Xbox Cloud Gaming client for a PS4 Fat running firmware **12.00** and **GoldHEN v2.4b18.7**, built with OpenOrbis.

## Preserved authorization milestone: 0.6.2

The owner confirmed that connection authorization completes without an error. The PS4 kernel log confirms Microsoft token renewal **HTTP 200**, Passport **HTTP 200**, `/connect` **HTTP 202**, and automatic session deletion **HTTP 200**, with no cleanup error. This milestone is preserved as **`v0.6.2`**.

Version 0.6.2 uses, with the owner's authorization, the temporary public OAuth client identifier also used by GreenVita. The original XCloud4 registration is preserved. All authentication steps use the same selected client; refresh tokens are never reused across clients. The own registration worked for account access and the catalog but returned `invalid_scope` for Passport. See [connection authorization](docs/AUTORIZACION_CONEXION.md) and [the Passport investigation](docs/INVESTIGACION_PASSPORT.md).

**Actual game video, game audio and game input are not present in this confirmed version.** Development has reached WebRTC negotiation and native media integration. Successful connection authorization alone does not establish a media connection.

The PS4 interface remains **Spanish**. Repository documentation and GitHub content are **English**. The owner authorized **public GitHub publication** on 2026-10-09. Older release notes retain the publication policy in effect at their checkpoints.

## Latest diagnostic checkpoint: 0.7.26

Product **0.7.26**, PS4 **APP_VER 00.96**, adds bounded numeric capture around intervals greater than **100 ms between actual NEW-picture flip matches**. It keeps the existing two readers, copy routines, recovery and playback settings. Independent AU and native-output identities preserve the decoder ABI's lack of returned PTS. The capture records exact reset reasons and keyframe-wait transitions, with explicit limits when events are missing or windows fill. It measures local observations, not end-to-end latency.

The [diagnostic specification and offline analyzer](docs/PAUSE_TRACE_0.7.26.md) and [release evidence](docs/RELEASE_NOTES_0.7.26.md) describe the next owner capture. Native compilation, matching retrieved artifacts and the 70 frozen build inputs are verified. Actual Claude Opus 5.5 completed the integrated source review and a fresh review of its corrections without finding a material defect. Console installation, exclusive trace export and the owner capture remain pending. First capture the two-reader reference; the four-reader comparison is deferred to 0.7.27. No new fluency improvement or pause cause is claimed.

## Latest tested experimental checkpoint: 0.7.25

Product **0.7.25**, PS4 **APP_VER 00.95**, adds one persistent CPU copy helper alongside the existing video owner. Two bounded, disjoint spans copy the same leased NV12 picture into cached staging. The first valid picture is compared once against the serial routine; mismatch or unavailable checking keeps that decoder start serial. The **16 ms / four Decode** policy, conversion, completed-image mailbox, presentation, audio, input and provider protocol remain unchanged. No Better xCloud option is integrated. See the [isolated experiment](docs/PARALLEL_COPY_EXPERIMENT_0.7.25.md) and [delivery evidence](docs/RELEASE_NOTES_0.7.25.md).

Fresh actual **Claude Opus 5.5** final source review returned PASS (**9 turns / 8 Read calls**). An independent selected-source and compiled-byte inspection found no material defect. Antigravity completed the plan review with **Gemini 3.1 Pro High**, but its final review attempts reached the account quota and read no final code. Native compilation, frozen runtime correspondence and matching VM/PC/retrieved-PS4 package hashes are verified. The owner has installed this checkpoint and reports fewer pauses, with more time between them. The first-copy byte check passed for one **1,382,400-byte** leased picture; it is not a check of every picture. A frozen **60.111-second / 12-interval** selection records **16.033 ms mean copy**, **27.749 new image draws/s** and a **500.040 ms maximum completed-picture gap**. Delivered video remains **1280×720**. These are separate, uncontrolled observations from the 0.7.24 baseline, not an A/B benchmark, source FPS or end-to-end latency. Pauses and recovery events remain; closure is unverified. See the [initial console report](docs/VIDEO_REPORT_0.7.25.md). The complete dependency-source archive was recovered on the PC from verified preserved payloads and the build VM's exact 0.7.25 manifest. Every source payload matches that manifest; the compressed archive hash differs from the interrupted VM export because of repackaging.

## Previous tested experimental checkpoint: 0.7.24

Product **0.7.24**, PS4 **APP_VER 00.94**, moves native video processing to one owning worker and transfers completed RGB images through a triple-buffer mailbox. UI/input polling and presentation stay on the main thread. Repeated scaling/flips are skipped until a new completed picture, relevant UI change or **500 ms** overlay refresh. The worker checks **16 ms / four Decode calls between operations**; native calls and copy/conversion may exceed that budget. Prediction references, native output type/bounds, audio and gamepad reporting remain.

The [pinned native-client comparison](docs/CLIENT_VIDEO_COMPARISON_0.7.24.md) and independent selected-source review support the ownership design. **Fresh actual Claude Opus 5.5 source PASS, Antigravity Gemini 3.1 Pro High selected-source/primary-client comparison, native build and matching-source/VM/PC/retrieved-PS4 package checks are confirmed. The owner now reports more fluid playback and responsive controls with recurring brief pauses.** The first picture remains **1280×720**. A bounded **670.472978-second** selection records **17.886 new image draws/s**, **29.382 ms copy** and **3.003 ms conversion**, with continuing queue/recovery pressure. This is a separate uncontrolled live sample from 0.7.23, not an A/B benchmark, server FPS or input-latency measurement. Later session deletion returns HTTP 200 with zero reported cleanup error and resource closure; the PS4 home screen and absence of a CE notice were not independently confirmed. See the [0.7.24 console report and counter definitions](docs/ERROR_REPORT_0.7.24.md). The original [release notes](docs/RELEASE_NOTES_0.7.24.md), tag and assets retain their preparation-time status. The isolated two-reader copy experiment is prepared as **0.7.25** above; its console result remains unverified.

## Previous tested development checkpoint: 0.7.23

The owner selected a **960 × 540** video request for **0.7.23**, PS4 **APP_VER 00.93**, to prioritize fluency. The implementation updates the two fixed RTC startup capability/dimension messages and adds a numeric request diagnostic; **30 FPS / 5000 kbps** and the existing native media/input pipeline remain unchanged. The native build, final compiled-profile audit, complete matching-source inventory, fresh actual Claude **Opus 5.5** PASS and VM/PC/retrieved-PS4 package hashes are verified. **The console's first decoded picture remains 1280 × 720, pitch 1280, despite the 960 × 540 request.** A bounded second attempt records **13.775 new image draws/s**, **29.349 ms copy** and **3.003 ms conversion**, with queue overflow and keyframe recovery. This uncontrolled live sample does not establish actual 540p delivery, improved fluency or measured input latency. See the [0.7.23 measured result and limits](docs/ERROR_REPORT_0.7.23.md) and [request, review and artifact evidence](docs/RELEASE_NOTES_0.7.23.md).

The owner subsequently reports worse video with intermittent stutters and much smoother controls; end-to-end latency remains unmeasured. Actual Claude Opus 5.5 completed a [selected-source comparison with Better xCloud](docs/BETTER_XCLOUD_COMPARISON_0.7.23.md), identifying profile/bitrate concepts and browser-specific options. No Better xCloud option is integrated.

## Previous tested development checkpoint: 0.7.22

Product **0.7.22**, PS4 **APP_VER 00.92**, converts eight pixels at a time with original SSE2 code. The owner reports better video but continuing choppiness and greater control-to-image delay; that delay has not been measured. A sustained **120.487-second** sample records **19.504 new image draws per second**, **3.001 ms conversion** and **29.337 ms copy** per image. Conversion is about **6.686×** faster than the separate 0.7.21 sample's conversion region; this is not a controlled benchmark or an FPS multiplier. Copy remains the largest measured local stage.

The native build, final source inventory, compiled-code audit, fresh actual Claude **Opus 5.5** PASS and VM/PC/retrieved-PS4 package hashes are verified. The [published 0.7.22 development release](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.22) preserves its artifacts. See the [0.7.22 measured result and limits](docs/ERROR_REPORT_0.7.22.md) and [release evidence](docs/RELEASE_NOTES_0.7.22.md). Panel FPS, absolute video age, wire-level input acknowledgment and the complete control matrix are unverified.

## Previous tested development checkpoint: 0.7.21

Product **0.7.21**, PS4 **APP_VER 00.91**, copies validated native NV12 output into persistent CPU storage before RGB conversion, with runtime-selected SSE4.1 streaming reads and a baseline SSE2 fallback. Native type 3 output and existing buffer bounds remain. The owner confirms video is much better than 0.7.20 but still choppy, and confirms the D-pad and sticks work; the earlier D-pad concern was retracted. Other buttons, local chords and release/cancellation paths have not been fully checked.

Sixteen sustained intervals cover **80.513 seconds** and report **12.979 new image draws per second**, with interval rates of **10.96–14.00**; later observations fall roughly within **8–13**. Mean copy plus conversion is **49.422 ms**, compared with **499.5965 ms** for the prior direct conversion sample, a ratio of about **10.109×** for that preparation region. These are different live intervals, not a controlled benchmark; the draw rate is not panel or server FPS. Queue overflow, damaged reconstruction and keyframe waits remain.

The native build, package/source hashes, **9369** source-manifest entries and compiled dispatch are verified. Fresh actual Claude Code **Opus 5.5** source review returned PASS, and VM/PC/retrieved-PS4 package hashes match. Exclude the single counter interval spanning a decoder restart, which can contain a wrapped delta. See the [0.7.21 measured result and remaining limits](docs/ERROR_REPORT_0.7.21.md), [artifact evidence](docs/RELEASE_NOTES_0.7.21.md) and [0.7.20 baseline](docs/ERROR_REPORT_0.7.20.md).

## Previous tested development checkpoint: 0.7.20

Product **0.7.20**, PS4 **APP_VER 00.90**, adds real Xbox gamepad-report sending, reduces repeated CPU video drawing work and records numeric performance timings. Input uses a **38-byte report**, sent at no more than **60 Hz** when the Xbox channel is ready and has no queued data. The sender reads the latest controller state; a source older than **250 ms**, disconnection or cancellation produces neutral input. OPTIONS maps to Xbox Menu and the touchpad click maps to Xbox View. Hold **L1 + R1** with OPTIONS to exit, CIRCLE to return to the catalog, SQUARE to mute, or the touchpad to send Xbox Guide. PS remains a system button; SHARE has no mapping through the current SDK Pad interface.

Video scaling caches coordinate mappings, and live playback skips the full menu repaint. Decoding checks a **6 ms / two Decode-call limit between operations** and converts the latest pending output; an individual native call can exceed that budget. No predictive frame dropping is introduced. The console's five-second summaries now establish the conversion bottleneck described above. The presentation path imports `sceGnmSubmitDone()` after flip submission and before the scanout wait; its first actual result is **0**. Successful external suspension/closure still requires a separate observed attempt.

The native build, matching source and VM/PC/PS4 package hashes are verified. Actual Claude Code **Opus 5.5** identified two source issues; both were corrected and a fresh focused review returned PASS. The console again decodes live audio and 1280×720 video and starts the input sender. The owner tentatively reports controller response, but individual buttons, axes and reserved chords remain unverified. Video remains slow; external closure is not yet confirmed. The matching-source exporter includes the portable gamepad header. See [0.7.20 scope, review and verified assets](docs/RELEASE_NOTES_0.7.20.md) and the [measured result](docs/ERROR_REPORT_0.7.20.md). The PS4 interface remains Spanish.

## Previous tested development checkpoint: 0.7.19

Version **0.7.19**, PS4 **APP_VER 00.89**, moves the native SCTP receive scratch buffer to owned heap storage, allocated once per receive invocation inside the existing exception handler. Its **65,536-byte capacity**, receive lock, pending counter, notification/message copies and end-of-record handling are preserved. Other targets retain the upstream automatic array; no new worker-stack attributes are introduced.

The affected dependency unit and full application package built successfully. The final linked `SctpTransport::doRecv()` reserves **552 local stack bytes**, compared with **66,072 bytes** in 0.7.18. A fresh actual Claude Code **Opus 5.5** review reported no material defects; source inventory and VM/PC/PS4 package hashes are verified. **The owner now confirms live game audio and video.** The log records SCTP/RTC connection, Xbox channels prepared, first Opus decoding and a 1280×720 H.264 image. Audio works well; video updates slowly, input reports are not yet implemented, and external closure raises CE-34878-0 with a graphics-suspension timeout. Native stack bounds and measured FPS remain unknown. See the [0.7.19 result and limitations](docs/ERROR_REPORT_0.7.19.md), [release evidence](docs/RELEASE_NOTES_0.7.19.md) and [development prerelease](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.19).

## Previous tested development checkpoint: 0.7.18

Version **0.7.18** corrects a verified native SCTP address ABI mismatch: the public C++ declaration placed the family at byte 0, while the actual C implementation expects length at byte 0 and family at byte 1. The target patch adds compile-time layout checks and bounded numeric initialization diagnostics. Actual Claude Code Opus 5.5, Antigravity Opus 4.6 Thinking and independent source reviews, native packaging, linked caller/library byte-layout evidence, matching source inventory and VM/PC/PS4 package hashes are confirmed. **The first console attempt reaches SCTP construction, successful bind and accepted nonblocking start, then crashes with SIGSEGV in `SctpTransport::doRecv()` before confirmed media.** Its large automatic receive buffer and fault at RSP - 8 strongly suggest worker-stack exhaustion; the actual worker stack size is not yet established. See the [current 0.7.18 error report](docs/ERROR_REPORT_0.7.18.md). Live game video/audio remains unconfirmed. Read the [0.7.18 evidence](docs/RELEASE_NOTES_0.7.18.md) and download the [public development prerelease](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.18).

The completed [public-client comparison and proposed receive fix](docs/CLIENT_COMPARISON_0.7.18.md) records the inspected sources and actual Antigravity/Claude review scope. Its receive-buffer proposal is implemented in 0.7.19, whose first attempt now reaches live media. The remaining native stack-measurement proposals are unimplemented; video performance, input and external closure remain separate work.

## Previous tested development checkpoint: 0.7.17

Version **0.7.17** corrects a concrete local timing incompatibility: libjuice selected `CLOCK_BOOTTIME=7`, rejected by the native clock adapter, and returned timestamp zero. The PS4 target now selects `CLOCK_MONOTONIC=1`, translated to native clock 4. The linked ELF confirms the new selection. Bounded numeric diagnostics separate ICE, DTLS, SRTP and SCTP phases and summarize remote candidate submissions. Focused actual Claude Opus 5.5 reviews, native packaging, exact matching source inventory and VM/PC/PS4 package hashes are verified. **The owner tested this build: ICE completes, DTLS connects and SRTP key derivation completes, then the peer fails with no game media.** See the [current 0.7.17 error report](docs/ERROR_REPORT_0.7.17.md) and [release evidence](docs/RELEASE_NOTES_0.7.17.md).

## Previous development checkpoint: 0.7.16

**0.7.16 console result:** the application stays open after connection failure, a valid Xbox SDP answer is applied, and session cleanup succeeds. The RTC connection still fails with **0xFFFFF824**, with no game video/audio received. Read the [current error report](docs/ERROR_REPORT_0.7.16.md) and download the [0.7.16 development prerelease](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.16). This is a development build, with live game media and game input still unresolved.

The owner resumed development after the 0.7.14 pause and authorized three changes together: omit undefined receiving-track SSRC declarations, create audio MID 0 before video MID 1 as PSBox does, and expand valid Teredo candidates into IPv4 UDP routes. The full 0.7.15 package, focused Claude review and matching source integrity are confirmed. **Two console attempts now apply a valid Xbox SDP answer**, then RTC fails with no media and cleanup succeeds. An uncaught `condition_variable timed_wait failed` exception subsequently closes the application with CE-34878-0. See [0.7.15 scope and evidence](docs/RELEASE_NOTES_0.7.15.md). The preserved 0.7.14 refusal remains historical evidence; combining the new changes limits attribution.

The 0.7.16 checkpoint rebuilds the complete LLVM 11 condition-variable object with native headers, correcting the linked timeout comparison from 110 to 60. The full build, focused Claude review, explicit object selection and native constant are confirmed; the package has been copied and hash-verified on PS4 for the owner to install. It also records a fixed numeric IPv6 rejection reason. **The owner confirms the application now stays open after connection failure.** The console applies a valid SDP answer, omits a non-Teredo IPv6 candidate (reason 12), then fails RTC and cleans up with no media; no timed-wait exception recurs in this attempt. ICE/RTC failure and game media remain unresolved. See [0.7.16 source and evidence](docs/RELEASE_NOTES_0.7.16.md).

The native WebRTC/media application compiled, linked, converted to SELF and packaged in 0.7.0. On its first console attempt, Xbox preparation and connection authorization succeeded, but local RTC opening returned **-2 before SDP, ICE or media reception**. Remote cleanup returned HTTP 200.

The 0.7.1 diagnostics located failure during creation of a thread pool incorrectly requesting **200112 workers**, followed by numeric system error 1 before initialization completed. ABI inspection traced that count to the SDK C++ runtime's CPU-count probe using the wrong native `sysconf` selector.

`XCloud4-0.7.2.pkg` explicitly requests **four RTC workers before initialization**. The console capture confirms all four workers were created; the thread pool and PSA/SCTP/DTLS/SRTP/ICE library initialization completed, and local peer-connection, track and data-channel creation succeeded. Requesting the local SDP offer then returned **-2 with a runtime-error category**, with no SDP callback captured at that point. Callback delivery is queued, so its absence does not identify the exact internal failure stage.

`XCloud4-0.7.3.pkg` adds fixed offer, certificate, ICE-agent, UDP/socket and interface-query diagnostics. The console capture confirms certificate generation, local ICE description and offer commit. Gathering then fails when setting UDP socket flags with `F_SETFL`: **errno 13 (`EACCES`)**, followed by connection/gathering errors and return -2. Remote cleanup succeeds with HTTP 200; no RTP is received. **Actual live video/audio remains unconfirmed.** Claude Opus 5.5 completed a read-only offer review in nine turns. See [0.7.3 notes](docs/RELEASE_NOTES_0.7.3.md).

`XCloud4-0.7.4.pkg` restores direct BSD `FIONBIO` nonblocking setup for UDP/TCP, with numeric result diagnostics. Dependency/application builds, package integrity and static request/import inspection are confirmed. The console attempt also rejects direct `FIONBIO` with **errno 13 (`EACCES`)**; certificate/local-offer preparation succeeds, remote cleanup returns HTTP 200 and no RTP is received. **The socket failure remains unresolved and live video/audio is unconfirmed.** Claude Opus 5.5 completed a read-only socket review in five turns. See [0.7.4 notes](docs/RELEASE_NOTES_0.7.4.md).

`XCloud4-0.7.5.pkg` uses the PlayStation `SO_NBIO` socket option through POSIX set/query calls on the existing descriptor and verifies enabled mode/four-byte size. The console confirms successful socket setup, host-candidate gathering and local offer submission: **Xbox accepts `POST /sdp` with HTTP 202, but returns HTTP 204 without a remote answer until keepalive returns HTTP 410**. Remote cleanup returns HTTP 200. No RTP or game video/audio is received. Claude Opus 5.5 completed a read-only review in five turns. API research, package/source hashes and hardware evidence are in [0.7.5 notes](docs/RELEASE_NOTES_0.7.5.md); the reason for the missing remote answer remains under investigation.

`XCloud4-0.7.6.pkg` adds structural SDP and allowlisted HTTP diagnostics without changing negotiation. The console again receives no remote answer; keepalive HTTP 410 is now identified as **`SessionNotActive` after 33 pending SDP polls**. A callback summary reconstructed from interleaved log fragments records three media sections and zero candidates. Source inspection identifies the retained initial offer snapshot as the next repair target; no corrected server result or media is confirmed. See [0.7.6 evidence and review limitations](docs/RELEASE_NOTES_0.7.6.md).

`XCloud4-0.7.7.pkg` obtains the current provider SDP after gathering through libdatachannel's C API, replacing submission of the initial callback snapshot. The console confirms a **1201-byte offer with one real candidate**, fixing that source-state issue. Xbox accepts submission with HTTP 202, but 33 HTTP 204 polls still return no answer before keepalive reports HTTP 410 / `SessionNotActive`. Cleanup and OPTIONS exit succeed. **No successful remote negotiation or game media is confirmed.** See [0.7.7 scope and hardware evidence](docs/RELEASE_NOTES_0.7.7.md).

`XCloud4-0.7.8.pkg` corrects the original native DNS adapter's timeout to `2000000` microseconds (two seconds). The console confirms successful lookup and a **1286-byte offer with two candidates**. After accepted submission, 37 pending polls and a successful keepalive, Xbox returns **HTTP 200 with non-null `errorDetails`**, which the parser identifies as a logical SDP refusal. Cleanup succeeds; **no RTP or game video/audio is received**. DNS is not established as the sole cause of the previous failure. See [0.7.8 runtime, package evidence and primary reference](docs/RELEASE_NOTES_0.7.8.md).

`XCloud4-0.7.9.pkg` classifies logical signaling failures before response data is cleared, preserving negotiation/media behavior. The console identifies an **`errorDetails` object with an unrecognized string code**, carried by HTTP 200 after pending SDP polls and successful keepalive. The exact server refusal category remains unknown; cleanup succeeds and **no RTP or game media is received**. Build/transfer integrity and Claude's eight-turn read-only review are recorded separately. See [0.7.9 diagnostic and runtime evidence](docs/RELEASE_NOTES_0.7.9.md).

`XCloud4-0.7.10.pkg` prints only a validated direct service-code identifier, preserving negotiation/media behavior. Product 0.7.10 uses **APP_VER 00.80**, with build/review/integrity and matching source confirmed. **Two attempts end with keepalive HTTP410 / SessionNotActive, no errorDetails and no RTP/media**; the new helper is not entered, and the underlying logical SDP refusal remains uncaptured. Both reach ready/provisioned status before SDP. See [0.7.10 source, attempts and queue limit](docs/RELEASE_NOTES_0.7.10.md).

`XCloud4-0.7.11.pkg` processes pending SDP responses before heartbeat servicing and schedules the next 30-second heartbeat from dispatch time. Build, matching source and VM/PC/PS4 retrieval hashes are confirmed. The console submits a **1286-byte offer with two candidates**, then records **38 pending SDP polls and keepalive HTTP 410 / `SessionNotActive`, with no errorDetails or RTP/media**. The underlying logical SDP refusal remains uncaptured. A synchronous GET can still delay a due pulse; no server lifetime or correction is established. Product 0.7.11 uses **APP_VER 00.81**. Claude Opus 5.5 completed the focused read-only review in three turns / two reads. See [0.7.11 package, source and console evidence](docs/RELEASE_NOTES_0.7.11.md).

`XCloud4-0.7.12.pkg` adds one bounded, diagnostic-only SDP request after a specific inactive-session keepalive failure, discarding the response and preserving the original failure. Full build, matching source and VM/PC/PS4 retrieval hashes are confirmed. The console returns a logical SDP refusal with the exact public code **`ConnectionExchangeFailed`** after a successful keepalive. The existing restricted-name helper captures it; the new terminal GET is **not entered**, because keepalive succeeds. Cleanup succeeds, with **no RTP/game media**. The public category does not establish the underlying cause. Claude Opus 5.5 completed the focused read-only review in three turns / two reads. Product 0.7.12 uses **APP_VER 00.82**. See [0.7.12 scope, package, source and console evidence](docs/RELEASE_NOTES_0.7.12.md).

`XCloud4-0.7.13.pkg` summarizes a direct errorDetails message using fixed keyword bits and lengths, while keeping message contents private. Full build, matching source and VM/PC/PS4 retrieval hashes are confirmed. **Two attempts have distinct outcomes:** a 2458-byte HTTP 200 response fails validation before remote SDP application, without keyword output; a later 243-byte response reports **`ConnectionExchangeFailed`**, with mask **`0x00010400`** identifying the terms `command` and `PerformSdpExchangeV1Command`. Those terms identify the referenced operation, not its underlying failure cause. Cleanup succeeds in both attempts, with **no RTP/game media**. Peer inspection and Claude Opus 5.5's focused read-only review completed without a material finding. Product 0.7.13 uses **APP_VER 00.83**. See [0.7.13 both attempts and exact evidence](docs/RELEASE_NOTES_0.7.13.md).

`XCloud4-0.7.14.pkg` classifies a parsed nested exchange's direct SDP and other field shapes using numeric diagnostics, without changing the accepted SDP rule or printing remote text. Full build, focused Claude review, matching source and transfer integrity are confirmed. The console again reports **`ConnectionExchangeFailed`**, message mask **`0x00010400`** (`command` / `PerformSdpExchangeV1Command`), after successful keepalive. The new nested-field helper is **not entered**, because the logical outer error occurs first. Cleanup succeeds, with ICE/RTC/video/audio counters at zero. **The root cause remains unknown; no valid remote answer or game media is established.** Product 0.7.14 uses **APP_VER 00.84**. See [0.7.14 actual capture and paused status](docs/RELEASE_NOTES_0.7.14.md).

The development package integrates pinned libdatachannel/libjuice, DTLS-SRTP, native DNS/entropy/thread adapters, bounded H.264 RTP reconstruction, Videodec2 output and Opus/AudioOut. Game controller reports are implemented, and the owner confirms the D-pad and sticks work in 0.7.21; other controls and release/cancellation behavior have not been fully validated. See the [0.7.21 result](docs/ERROR_REPORT_0.7.21.md), [WebRTC status](docs/WEBRTC_PS4.md) and [live media limits](docs/MULTIMEDIA_EN_VIVO.md).

## Confirmed progress

| Version | Result on the owner's PS4 |
|---|---|
| 0.1.2 | Application startup, `CONTROL` and `PROYECTO` views work. Fios2 and libc packaging dependencies are included. |
| 0.2.2 | Local H.264 sample, stereo PCM tones and clean `OPTIONS` exit to the PS4 menu work. |
| 0.3.1 | Native HTTPS and Microsoft device-code authorization work with XCloud4's own registration. |
| 0.4.0 | Real Xbox catalog: 2733 titles received, 128 retained locally, 21 of those marked with access by Xbox, and 32 Microsoft Store names obtained. |
| 0.5.0 | Remote session creation, readiness and automatic deletion work. |
| 0.6.2 | Passport authorization, `/connect` acceptance and automatic deletion work with the temporary reference client. |
| 0.7.2, partial | Four RTC workers, library initialization and local peer-connection/tracks/channels succeed; local SDP offer generation still fails before media. |
| 0.7.3, partial | Certificate, local ICE description and offer commit succeed; gathering fails on UDP `F_SETFL` with `EACCES`, cleanup succeeds, no RTP received. |
| 0.7.4, partial | Direct `FIONBIO` also fails with `EACCES`; certificate/local offer and cleanup succeed, no RTP received. |
| 0.7.5, partial | Nonblocking socket setup, gathering and local SDP submission succeed; Xbox returns no SDP answer before HTTP 410, cleanup succeeds, no media received. |
| 0.7.6, diagnostic | Keepalive HTTP 410 is identified as `SessionNotActive`; the initial callback summary has zero candidates, no remote SDP answer or media is received. |
| 0.7.7, partial | Current offer includes one real candidate; no Xbox answer before `SessionNotActive`, cleanup and OPTIONS exit succeed, no media received. |
| 0.7.8, partial | Native DNS succeeds and current offer has two candidates; Xbox returns a logical SDP refusal in HTTP 200, cleanup succeeds, no media received. |
| 0.7.9, diagnostic | Refusal contains an errorDetails object with an unrecognized string code; exact category unknown, cleanup succeeds, no media received. |
| 0.7.10, two attempts | Keepalive returns SessionNotActive without errorDetails; restricted-name helper not entered, cleanup succeeds, no media. |
| 0.7.11, scheduling | SDP is polled before heartbeat; keepalive still returns SessionNotActive without errorDetails, cleanup succeeds, no RTP/media. |
| 0.7.12, diagnostic | Logical SDP refusal is identified as ConnectionExchangeFailed after successful keepalive; terminal GET not entered, cleanup succeeds, no media. |
| 0.7.13, two attempts | A 2458-byte response fails validation; a later refusal identifies ConnectionExchangeFailed and the SDP command name, not its cause. Cleanup succeeds, no media. |
| 0.7.14, paused | ConnectionExchangeFailed repeats with the SDP command mask; nested helper not entered, cleanup succeeds, ICE/RTC/media zero. |
| 0.7.15, partial | Two valid Xbox SDP answers applied; RTC fails and cleanup succeeds with zero media, followed by an uncaught timed-wait exception/application crash. |
| 0.7.19, first live media | Connected SCTP/WebRTC, decoded Opus and a 1280×720 H.264 game image; owner confirms good audio and very slow video. Gamepad sending was absent. External closure caused a graphics-suspension timeout; a separate ARK attempt ended after roughly five minutes with RTC failure and successful remote cleanup. |

The owner reported these results and console logs support them. Not every button, axis, catalog navigation action or cancellation path has been checked separately. A successful build does not establish hardware behavior.

### Earlier failures and fixes

- **0.1.0–0.1.1:** startup stopped before `main` because the package lacked OpenOrbis Fios2, then libc. Both dependencies are present in 0.1.2.
- **0.2.0:** H.264 playback worked; audio failed with `0x809B0001`. Version 0.2.1 uses SYSTEM (`0xFF`) for the MAIN AudioOut port and waits before reusing PCM buffers. The owner confirmed the sound.
- **0.2.1:** exiting through `_exit` triggered `CE-34878-0`/SIGSYS. Version 0.2.2 releases resources and requests `sceSystemServiceLoadExec("exit", NULL)`. The owner confirmed clean exit.
- **0.3.0:** locating SSL failed before any HTTPS request. Version 0.3.1 resolves native module exports and sandbox library paths, with certificate validation retained.
- **0.6.0–0.6.1:** the own OAuth client reached session readiness but Passport returned HTTP 400, identified as `invalid_scope` in 0.6.1. Version 0.6.2's temporary reference client succeeded. The exact Microsoft registration policy has not been established.

Detailed evidence and package hashes are in the [development log](docs/JORNADA.md).

## Roadmap

1. Prepare OpenOrbis and produce a minimal native application — confirmed.
2. Establish local video, audio and DualShock 4 input — local media and UI confirmed.
3. Implement Microsoft account authorization and the Xbox catalog — confirmed.
4. Implement a cloud session, WebRTC transport, game video/audio and controller messages — session authorization confirmed; negotiation work resumed before actual streaming.
5. Improve reconnection, errors and performance — future work.

The proposed first beta target is **720p at 30 FPS**. Its feasibility depends on actual decoder and transport behavior on the console. Title search remains a [future request](docs/MEJORAS_FUTURAS.md), not part of the current stage.

`WaitingForResources` is handled as waiting, but initial provisioning is limited to **180 seconds**. Extended queue duration and UI remain a [future improvement](docs/MEJORAS_FUTURAS.md); no server wait-time estimate is claimed.

## Build

Use the existing Lubuntu VM in VirtualBox, after [preparing the environment](docs/PREPARACION.md):

```bash
cd "$HOME/Projects/XCloud4"
source "$HOME/.config/xcloud4/env.sh"
bash scripts/webrtc/build_native.sh  # first build of the pinned external dependencies
make -j2
export X4_RUNTIME_MODULES="$HOME/.local/share/xcloud4/runtime/sdk-v0.5.4"
make package
```

The current 0.7.23 development source successfully produces `build/xcloud4.elf`, `build/eboot.bin` and `dist/XCloud4-0.7.23.pkg`; the native build and VM/PC package integrity are verified. The preserved `v0.6.2` source produces its corresponding 0.6.2 package without the new WebRTC dependencies. Dependency builds and SDK binaries remain outside Git.

`X4_RUNTIME_MODULES` must point to an external directory containing `libSceFios2.prx` and `libc.prx` in SELF format. These are open auxiliary OpenOrbis modules, available in its distribution with corresponding source under `src/modules`. Their compiled binaries stay outside Git. Packaging stops if either is missing or supplied as an unconverted ELF. See [third-party notices](THIRD_PARTY_NOTICES.md).

The legacy OpenOrbis packager needs isolated OpenSSL 1.1 libraries. Prepare them once with `bash scripts/preparar-empaquetador.sh`; this does not install them system-wide. See [PS4 installation](docs/INSTALACION_PS4.md).

## Source layout

- `src/core`: entry point and lifecycle.
- `src/auth`: native HTTPS, Microsoft account, Xbox catalog, session preparation and authorization.
- `src/streaming`: WebRTC negotiation, native transport and SDK ABI adapters.
- `src/media`: bounded RTP queues, H.264 depacketization and live media state.
- `src/video`: VideoOut, local H.264 sample and native live Videodec2 integration.
- `src/input`: DualShock 4 reading and reconnection.
- `src/audio`: AudioOut PCM sample and live Opus decoding/output integration.
- `src/ui`: Spanish home, controller, project, local media, account, catalog and session views.
- `docs`: architecture, decisions, preparation, references and evidence.
- `scripts`: environment and local build tools.

Never commit passwords, device codes, access/refresh tokens, private keys, SDK contents, generated packages or private console logs.

## Upstream references and license

- [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain): native toolchain and public APIs.
- [GreenVita](https://github.com/Day-OS/green-vita): Xbox authentication, session and WebRTC protocol research.
- [Better xCloud](https://github.com/redphx/better-xcloud): settings and region research.
- [Moonlight PS4](https://github.com/JaimeJimenezG/Moonlight-ps4): native media API research.

Pinned versions and adaptation decisions are in [REFERENCIAS.md](docs/REFERENCIAS.md). XCloud4 is **GPL-3.0-only**; see [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
