# AJ GeForce NOW PS4 reference review

Reviewed on **2026-10-09** at immutable commit **`87568ca50a89e6f8dd65fe005d21521c27e65c26`** of [AJfiles/AJ-GeforceNow-PS4](https://github.com/AJfiles/AJ-GeforceNow-PS4/tree/87568ca50a89e6f8dd65fe005d21521c27e65c26). This is a scoped static review of application, build, media and selected libpeer sources. It does not establish execution on the owner's PS4 Fat/firmware 12.00. The author reports measurements on PS4 Pro/firmware 9.00; those results were not reproduced here.

## Relevant implementation

| Area | AJ implementation | Relevance to XCloud4 |
|---|---|---|
| Native build | OpenOrbis/freebsd12-elf; libpeer-orbis selects internal SCTP, while libusrsctp remains in the link group | Useful PS4 runtime reference; does not validate XCloud4's usrsctp integration |
| Signaling | NVIDIA WebSocket offer; client answer plus NVST SDP | Provider-specific protocol; XCloud4 submits a client offer through Xbox HTTP signaling |
| Video | FFmpeg H.264 software decoding and native VideoOut presentation | Potential software fallback and presentation reference; XCloud4's local native decoder already works |
| Audio | Opus, 48 kHz stereo S16, SceAudioOut MAIN, bounded PCM queue | Useful loss/concealment and worker-lifetime reference |
| Input | NVIDIA data-channel packets with XInput-style buttons/axes | Controller representation reference; wire messages differ from Xbox |
| Waiting queue | CloudMatch polling with cancel and a bounded wait | Useful UI pattern; states and requests remain specific to NVIDIA |

The active library selection is established by [build linkage](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/scripts/build-ps4.ps1#L116) and [native CMake sources](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/ps4/CMakeLists.txt). A libdatachannel build helper also exists; that helper does not make it the active application backend. Archive linkage alone does not identify the selected SCTP implementation.

## Concrete useful patterns

- [Audio worker](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/stream/audio/AudioPipeline_ps4.cpp#L114) explicitly avoids timed condition-variable waits because Orbis libc++ can throw a system error on a normal timeout. This independently matches the failure XCloud4 corrected in 0.7.16. Its polling workaround is not required to replace the confirmed native-runtime correction.
- [Native audio setup and decoding](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/stream/audio/AudioPipeline_ps4.cpp#L61) uses an initial user with SYSTEM fallback and Opus loss concealment. XCloud4 retains its confirmed SYSTEM output path.
- [VideoOut renderer](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/stream/PS4VideoOutRenderer.cpp#L745) bounds resolution and framebuffer changes; [FFmpeg integration](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/stream/ffmpeg/FFmpegVideoDecoder.cpp) supplies software decoding. Reported hardware-decoder failures on another console do not invalidate XCloud4's confirmed local decoding.
- [MbedTLS timer compatibility](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/mbedtls_timing_compat.c#L22) uses a monotonic clock; [network worker](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/webrtc/session.cpp#L334) separates peer processing from presentation.

## Native SCTP selection and connection state

The [PS4 toolchain](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/cmake/orbis-ps4-x64.cmake#L13) targets **x86_64-pc-freebsd12-elf**. For that target, [libpeer CMake](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/third_party/libpeer-orbis/src/CMakeLists.txt#L18) explicitly sets **CONFIG_USE_USRSCTP=0**, selecting libpeer's internal SCTP implementation. The same file lists `libusrsctp.a` in its link group at line 36, and the application build also links that archive. Its presence does not establish that the PS4 data-channel path uses usrsctp.

After a successful DTLS handshake, [peer_connection.c](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/third_party/libpeer-orbis/src/peer_connection.c#L651) calls `sctp_create_association` when data channels are configured. A nonzero result logs a creation failure at line 672, but execution still reaches **PEER_CONNECTION_COMPLETED** at line 678. Consequently, that state alone does not prove successful SCTP association creation or channel opening, and it cannot validate XCloud4's distinct usrsctp ABI integration. These are source-level observations, not results from running AJ on the owner's console.

## Provider-specific behavior

[Negotiation](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/webrtc/negotiation.cpp#L703) receives NVIDIA's offer, aligns the answer with its media/MID order and sends both SDP and NVST attributes. Its manually supplied candidates use actual CloudMatch endpoints and provider ports after a short delay. These values do not justify synthesizing equivalent Xbox endpoints or copying NVST attributes into Xbox SDP.

The selected [libpeer IPv6 setting](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/third_party/libpeer-orbis/src/config.h#L64) is **CONFIG_IPV6=0**. This implementation is not evidence of a working PS4 IPv6 or Teredo solution. The reviewed application paths preserve no Teredo conversion comparable to XCloud4's validated helper.

[Native queue handling](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/ps4/main.cpp#L4084) reports provider position/ETA and permits cancellation; [input handling](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/webrtc/input.cpp#L565) uses a reliable NVIDIA channel. Their control messages and heartbeat cadence do not establish the corresponding Xbox protocol.

## Scope, review and reuse

Actual Claude Code selected **claude-opus-5-5** and completed a read-only review in **3 turns** of selected reference excerpts and XCloud4's new sanitized connection evidence. Independent peer reviews examined provider negotiation and native integration. Reviewer suggestions were checked against actual source; missing context, enum differences and interleaved logs are not treated as proven defects.

The top-level [license](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/LICENSE) is MIT. Bundled dependencies retain their individual licenses; this review does not classify every bundled source as MIT. No external application implementation was copied into XCloud4 as part of this review.

The useful next step is comparing PS4 runtime and internal SCTP behavior while preserving known working media paths. AJ's selected internal backend does not independently validate XCloud4's usrsctp integration. A complete WebRTC backend replacement is not supported by the current evidence. This reference review is not a demonstrated fix for XCloud4's [0.7.17 failure](ERROR_REPORT_0.7.17.md).
