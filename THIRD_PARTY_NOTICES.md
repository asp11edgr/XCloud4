# Dependencies and third-party references

## OpenOrbis PS4 Toolchain

Pinned preparation version: **v0.5.4**.

- [Source](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
- Archive: `toolchain-llvm-18.tar.gz`.
- GitHub-published SHA-256: `3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526`.
- Repository license: GPL-3.0. Components distributed inside the SDK retain their respective licenses.
- The Makefile and packaging adapt parameters from the hello_world/input examples. VideoOut initialization in `src/video/display.c` and controller initialization in `src/input/controller.c` adapt public SDK examples. The original license is retained in `docs/licenses/OpenOrbis-GPL-3.0.txt`.

The SDK is installed outside this repository at `~/.local/share/xcloud4/OpenOrbis/PS4Toolchain`.

### Videodec2 declarations

`src/video/videodec2_abi.h` adapts public types from OpenOrbis proposal #213 by Backporter under the repository's GPL-3.0 license. Pinned source: `9b9e82a2ec4e8cd3c34a086ca82339032cf69da0`, `include/orbis/_types/Videodec2.h`.

- [Proposal](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/pull/213).
- [Corresponding source](https://github.com/Backporter/OpenOrbis-PS4-Toolchain/blob/9b9e82a2ec4e8cd3c34a086ca82339032cf69da0/include/orbis/_types/Videodec2.h).
- Retained license: `docs/licenses/OpenOrbis-GPL-3.0.txt`.

SDK v0.5.4 has incomplete declarations for this API. The proposal's 48-byte output structure is retained and functions are resolved when the sample opens. Module loading, memory ownership, sample reading, NV12 conversion and PCM playback are original XCloud4 implementations. Moonlight PS4's implementation has not been copied.

### Local sample and AudioOut

`assets/sample.h264` is a synthetic pattern generated in Lubuntu with FFmpeg 8.0.1 and libx264 using `scripts/generar-muestra.sh`. It contains no game footage, music or third-party imagery. FFmpeg and libx264 run only on the development PC; their executables and libraries are not included in the PKG. XCloud4 generates its PCM tones.

Version 0.2.1 adapts SYSTEM-user selection for MAIN and buffer-consumption waiting via `sceAudioOutOutput(handle, NULL)` from OpenOrbis v0.5.4's public `samples/audio-wav/audio-wav/main.cpp`, under GPL-3.0. [Corresponding source](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/v0.5.4/samples/audio-wav/audio-wav/main.cpp). The license is retained in `docs/licenses/OpenOrbis-GPL-3.0.txt`. That example's music and WAV decoder are not included.

## Protocol and API research

GreenVita (MPL-2.0), Better xCloud (MIT), and Moonlight PS4 (permission to adapt its own code not yet clarified) are research references. Their implementations have not been incorporated into the confirmed 0.6.2 baseline. Reviewed versions and relevant files are listed in [REFERENCIAS.md](docs/REFERENCIAS.md).

The public OAuth reference identifier also used by GreenVita is used temporarily with the owner's authorization in 0.6.2. This is not a claim of ownership of that registration or incorporation of GreenVita's Rust implementation.

## PC-only packaging libraries

OpenOrbis's LibOrbisPkg packager uses .NET Core 3.0. To run it on Ubuntu 26.04, the official Ubuntu package `libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb` is extracted into an isolated local directory without installing it system-wide. SHA-256: `7cf39d70a639017d1dd7c8d36daa2258063608688e449fddf40ffdd46f992a78`. [Source package download](https://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb).

These libraries are used only for local packaging. They are not included in the PKG or XCloud4 repository.

XCloud4 source is GPL-3.0-only; see [LICENSE](LICENSE). The pixel alphabet, icon and UI were created for this project.

## OpenOrbis auxiliary modules

Actual console logs established that the loader requires `sce_module/libSceFios2.prx` and `sce_module/libc.prx` before entering `main`: 0.1.0 failed because Fios2 was missing; 0.1.1 passed that lookup and failed because libc was missing. Package 0.1.2 and subsequent packages include both modules from the OpenOrbis v0.5.4 example, retained externally without modifying their content.

These are open auxiliary OpenOrbis modules, not proprietary Sony SDK libraries. Corresponding v0.5.4 source is `src/modules/libSceFios2/libSceFios2/lib.c` and `src/modules/libc/libc/lib.c`. `src/modules/build-and-copy.sh` builds and copies them to examples. They retain OpenOrbis's GPL-3.0 license, included at `docs/licenses/OpenOrbis-GPL-3.0.txt`.

[Corresponding source](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/tree/v0.5.4/src/modules).

Generated module binaries stay outside Git. A native system module obtained over FTP from the console was not used or distributed. Application behavior is established from console results, not merely module inclusion.

## WebRTC dependencies under development

Version 0.7 development integrates the following pinned sources. They were not included in the confirmed 0.6.2 package. Exact upstream license/attribution files are retained under `docs/licenses` and must accompany the later package. Archive compilation and native source-object builds do not establish a working live game stream.

| Component | Pinned source | License/notice retained |
|---|---|---|
| libdatachannel 0.24.6 | [bdc5ff28e9d3b863144c94a677ecf5bf043aaf15](https://github.com/paullouisageneau/libdatachannel/tree/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15) | [MPL-2.0](docs/licenses/libdatachannel-MPL-2.0.txt) |
| libjuice 1.7.4 | [b89c792e3612faf2f12cf35bcc56857313a06be3](https://github.com/paullouisageneau/libjuice/tree/b89c792e3612faf2f12cf35bcc56857313a06be3) | [MPL-2.0](docs/licenses/libjuice-MPL-2.0.txt) |
| usrsctp, libdatachannel fork | [fec583d54493f879d2ae44a743423bf8a04371ab](https://github.com/paullouisageneau/usrsctp/tree/fec583d54493f879d2ae44a743423bf8a04371ab) | [BSD 3-Clause](docs/licenses/usrsctp-LICENSE.md) |
| libsrtp | [d33b8ffb1491a0b4b58a206889f09800cf7310ab](https://github.com/cisco/libsrtp/tree/d33b8ffb1491a0b4b58a206889f09800cf7310ab) | [BSD 3-Clause](docs/licenses/libsrtp-LICENSE.txt) |
| nlohmann/json | [55f93686c01528224f448c19128836e7df245f72](https://github.com/nlohmann/json/tree/55f93686c01528224f448c19128836e7df245f72) | [MIT](docs/licenses/nlohmann-json-MIT.txt) |
| plog | [94899e0b926ac1b0f4750bfbd495167b4a6ae9ef](https://github.com/SergiusTheBest/plog/tree/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef) | [MIT](docs/licenses/plog-MIT.txt) |
| Mbed TLS 3.6.7 | [068ff080b369adfac81509f9b57b2afabaf82dc5](https://github.com/Mbed-TLS/mbedtls/tree/068ff080b369adfac81509f9b57b2afabaf82dc5) | [Apache-2.0 OR GPL-2.0-or-later, both original texts](docs/licenses/MbedTLS-dual-license.txt) |
| Mbed TLS framework, build helper | [dde0c4a0e448a0552f18817dcea633bb851fd288](https://github.com/Mbed-TLS/mbedtls-framework/tree/dde0c4a0e448a0552f18817dcea633bb851fd288) | [Exact dual-license text](docs/licenses/MbedTLS-framework-LICENSE.txt) |

Mbed TLS's bundled Everest code is Apache-2.0; its exact [upstream README](docs/licenses/MbedTLS-everest-README.md) and Apache text in the Mbed TLS license are retained. The inspected interface credits INRIA and Microsoft Corporation. Bundled p256-m is Apache-2.0 OR GPL-2.0-or-later and authored by Manuel Pégourié-Gonnard; both its [Mbed TLS attribution README](docs/licenses/MbedTLS-p256-m-README.md) and [upstream README](docs/licenses/p256-m-upstream-README.md) are retained. These components are pinned by the Mbed TLS revision, rather than a guessed independent upstream revision.

libdatachannel/libjuice source headers retain Paul-Louis Ageneau's copyright notices. usrsctp's license retains Randall Stewart/Michael Tuexen attribution; libsrtp's retains Cisco Systems attribution. Other file-specific notices remain in the corresponding source. Copied notice hashes and locations are recorded in [the license inventory](docs/licenses/README.md).

### OpenOrbis port modifications and corresponding source

`scripts/webrtc/prepare_port.py` describes reproducible source changes and SDK header overlays for the pinned checkout: native BSD socket/attribute ABI, optional thread naming, native entropy, nonblocking sockets and portable SCTP support. The port uses `openorbis.cmake`, `mbedtls-user-config.h` and the original XCloud4 `rtc_net.c` adapter. No installed SDK is modified. Source headers and applicable licenses are retained.

When distributing a 0.7 PKG containing these libraries, provide the complete corresponding dependency-source archive alongside it: patched pinned trees including submodules, their original notices, generated overlays, configuration and the exact port script. Original public source links alone do not describe modified files. `scripts/webrtc/export_source.sh` produces `XCloud4-VERSION-dependency-sources.tar.gz` from the actual patched build trees and records their pins, modified paths and file hashes. It selects the package version (or explicit `X4_SOURCE_VERSION`) and refuses to replace existing snapshots. Each checkpoint has separate corresponding source: 0.7.1 adds initialization diagnostics; 0.7.2 records actual worker creation and bounds the application RTC pool; 0.7.3 adds offer/ICE/certificate diagnostics in libdatachannel and numeric native network results in libjuice `agent.c`, `conn_poll.c` and `udp.c`, with matching native adapters and reproducible complete replacement checks. Publication of matching source must accompany any distributed package. Recipients must have access to that source. Generated build caches, `.git`, SDK binaries, locally generated credentials, account tokens and runtime logs are excluded; original public upstream test fixtures remain included. No completed live playback result is claimed here.

The 0.7.4 port restores libjuice's upstream nonblocking socket approach in `src/socket.h`, `src/udp.c` and `src/tcp.c`: use the BSD `FIONBIO` ioctl for UDP/TCP sockets, with the existing SDK ioctl encoding. The reproducible modifications remain in `scripts/webrtc/prepare_port.py` and retain libjuice's original MPL-2.0 notices. A fixed numeric diagnostic records the ioctl result/errno; failed setup closes the socket and reports failure. Pipe flag handling remains separate. The matching 0.7.4 source snapshot must capture these changes and the native adapter; previous snapshots remain unchanged. Console repair and live playback have not been confirmed.

The 0.7.5 original native adapter uses POSIX `setsockopt` with PS4 `SO_NBIO` (`0x1200`), then verifies the returned mode and four-byte size through `getsockopt`. Interface/constant research used Sony's [PlayStation WebKit additions](https://github.com/WebKit/WebKit/blob/23af623a3a7e6b6458b1f51cf2af9c2fe77b30e8/Source/WTF/wtf/playstation/UniStdExtrasPlayStation.cpp), pinned at `23af623a3a7e6b6458b1f51cf2af9c2fe77b30e8`, and [WoWPS socket control](https://github.com/01cedric/WoWPS/blob/7649815066dd4d008107c691cbde758b8a29d8bf/include/platform/ps4/socket_control.hpp), pinned at `7649815066dd4d008107c691cbde758b8a29d8bf`. Their implementations are not copied into XCloud4. Reproducible libjuice changes call the original adapter and retain MPL-2.0 notices. The matching source snapshot must include these changes and native diagnostics. WoWPS documents an OpenOrbis descriptor/nonblocking limitation; XCloud4's actual `fcntl`/`ioctl` EACCES evidence comes from its console capture, not a verified SDK implementation or confirmed internal cause. Runtime success is not yet established.

The 0.7.6 checkpoint adds original structural SDP diagnostics and bounded HTTP failure categorization before the response buffer is cleared. Only fixed enum/numeric summaries are retained; raw SDP, ICE values, credentials and arbitrary remote text are not exposed. The negotiation behavior is unchanged, and these diagnostics do not establish a repaired cause or working media. The matching source snapshot must preserve the exact native adapters, unchanged pinned dependency trees and port configuration; previous version archives remain immutable. GreenVita continues to be a protocol research reference, with no copied implementation in these diagnostics.

The 0.7.7 original transport change obtains the current local SDP through libdatachannel's public C API after gathering completes, rather than submitting the initial description callback's retained snapshot. It checks actual offer type, returned lengths, bounds and termination; provider availability remains pending until ready. It uses actual provider state; no candidate addresses are synthesized or merged. Structural diagnostics describe that current offer and repair overflow setup/trickle labeling above eight media sections. The same pinned dependencies, licenses and port configuration apply, with the exact updated native adapter required in a separate matching source snapshot. This change does not establish Xbox acceptance of a remote negotiation or live playback. Previous package/source snapshots remain unchanged.

The 0.7.8 original resolver adapter corrects the native hostname-resolution timeout from `2` to `2000000` microseconds (two seconds) and adds numeric-only return/timing diagnostics. API-unit research uses [ioQuake3-PS4's resolver call](https://github.com/Mayo1970/ioQuake3-PS4/blob/d4c7912af73c3b0eaa57195391ad76f21e79a910/code/qcommon/net_ip.c#L447), pinned at `d4c7912af73c3b0eaa57195391ad76f21e79a910`, which passes `3 * 1000 * 1000`; its [technical DNS note](https://github.com/Mayo1970/ioQuake3-PS4/blob/d4c7912af73c3b0eaa57195391ad76f21e79a910/AGENTS.md#L193) explicitly identifies microsecond units. No external implementation is copied; XCloud4 retains its original resolver and chooses its own two-second bound. Events 69–73 describe resolver creation result, timeout, lookup result, bounded elapsed microseconds and a nonzero-address boolean. Hostnames, addresses, ICE values and credentials are not logged. The matching 0.7.8 snapshot must retain the exact updated adapter and unchanged dependency configuration/notices. The timeout correction has no confirmed console/server result at preparation time, and DNS has not been established as the cause of the missing Xbox SDP answer. Earlier snapshots remain unchanged.

The 0.7.9 original authentication diagnostics in `src/auth/xbox_live.c` also classify logical failures carried by successful HTTP responses during SDP/ICE exchange, acknowledgement and keepalive, before clearing private response data. Existing non-2xx logging and error-code classes 0–14 are retained. Bounded node summaries describe root/errorDetails/error plus one nested details-object level using JSON types, fixed classes, validated uint32/signed32 numeric code data, uint32 status and message/detail types only. No recursion, allocation, arbitrary provider text, raw SDP, ICE values or credentials are introduced by the diagnostics. This diagnostic checkpoint does not change negotiation decisions, media settings or receive/output behavior. The matching source must preserve the exact application revision plus unchanged pinned dependency/native-adapter trees and notices, separately from every prior immutable checkpoint. A diagnosed refusal is not a completed remote negotiation or live playback result.

The 0.7.10 original authentication helper adds a restricted local diagnostic for the direct `errorDetails` object's string `code` only. It accepts at most 64 ASCII characters: an initial letter followed by letters, digits or underscore; invalid/absent/duplicated fields remain silent, and the temporary 65-byte buffer is cleared on every path. Message text, other string fields, response bodies, SDP, ICE values and credentials remain hidden. Interface research uses [CloudNow's diagnostic filter](https://github.com/owenselles/CloudNow/blob/6627c0423474ebe6647e86a2db953ce55dabaae1/CloudNow/Xbox/XboxCloudSignalingAPI.swift#L820), pinned at `6627c0423474ebe6647e86a2db953ce55dabaae1`, and [OpenXbox's separate error code/message fields](https://github.com/OpenXbox/xcloud-rs/blob/cd94b22f611bece91c5963c5647588b06adc705f/gamestreaming_webrtc/src/api.rs#L573), pinned at `cd94b22f611bece91c5963c5647588b06adc705f`. No reference implementation is copied; XCloud4's original helper applies its own narrower field/ASCII/length bounds. Numeric diagnostics and negotiation/media decisions are unchanged. The exact application revision plus corresponding dependency/native-adapter archive and licenses must accompany distribution; earlier snapshots remain immutable. No server correction or completed stream is established at preparation time.

The 0.7.11 original authentication change processes a pending SDP GET and its logical result before servicing the heartbeat, so an available refusal can be diagnosed before a heartbeat failure ends negotiation. The 30-second heartbeat interval remains, with its next deadline calculated from the captured dispatch-time timestamp instead of HTTP completion. [GreenVita's backend](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/streaming/backend.rs#L193), pinned at `ae2625d295b4fba005a769b1309fd70dcd6cb63f`, schedules the next 30-second deadline before launching its asynchronous request. That scheduling is a protocol reference; no Rust implementation is copied. XCloud4's GET remains synchronous and can delay a due heartbeat by that request. Existing cancellation/deadline checks, initial heartbeat, media/parser configuration, queue handling and cleanup remain in place. The exact application revision plus corresponding dependency/native-adapter archive/licenses must accompany distribution. No server lifetime or successful negotiation is established at preparation time; earlier snapshots remain immutable.

The 0.7.12 original authentication change adds one diagnostic-only `GET /sdp` when the normal SDP exchange is still pending and keepalive has failed with HTTP 410, an `X4_AUTH_E_STATUS` session error and a strictly parsed root-object code of `SessionNotActive`. The private flag is reset on every keepalive call and consumed by the diagnostic helper. It requires no cancellation and at least 30 seconds remaining within the existing negotiation deadline; cancellation/deadline are checked again after the request. A direct session HTTP call preserves the original outcome, public snapshot and poll counters. Numeric/class diagnostics precede clearing the response; any late answer is discarded without applying SDP or retrying submission. Response, exchange, SDP and request buffers are cleared on all eligible paths, including a skipped request. No external implementation is copied. Heartbeat cadence, codec/media behavior, queue limits and cleanup remain unchanged. Distribution requires the exact application revision together with the corresponding dependency/native-adapter archive and retained licenses. No repaired server outcome, server lifetime or live playback is established at preparation time; earlier snapshots remain immutable.

The 0.7.13 original authentication diagnostic inspects only a direct `errorDetails.message` string. Strict JSON member/type checks reject duplicate or inappropriate fields. A 1025-byte temporary buffer admits 1–1024 decoded ASCII bytes, allowing CR/LF/TAB and rejecting NUL, other controls and non-ASCII; decoding is skipped when the raw JSON span exceeds 6146 bytes. Local output contains only validity, raw span length, decoded length and an 18-bit fixed keyword mask. Case folding and word boundaries use explicit ASCII rules; phrase spaces admit ordinary spaces/CR/LF/TAB, and ICE does not match inside service/device. The buffer is securely cleared on every path. No message contents, arbitrary response text, SDP or credentials are printed. This is original diagnostic code with no copied reference implementation; existing public-code/numeric diagnostics, protocol decisions, heartbeat cadence and media behavior remain unchanged. Keyword presence does not demonstrate a failure cause. Exact application source plus corresponding dependency/native-adapter source and licenses accompany distribution; earlier snapshots remain immutable, with no live playback result established at preparation time.

## SDK C++ runtime

Version 0.7 links the OpenOrbis SDK's `libc++.a` and `libc++abi.a`. The installed libc++ header reports `_LIBCPP_VERSION 11000` and identifies **Apache-2.0 WITH LLVM-exception**. Official LLVM 11.0.0 [libc++ license](docs/licenses/LLVM-libcxx-11-LICENSE.TXT) and [libc++abi license](docs/licenses/LLVM-libcxxabi-11-LICENSE.TXT), including legacy notices and LLVM exceptions, are retained. [libc++ source license](https://github.com/llvm/llvm-project/blob/llvmorg-11.0.0/libcxx/LICENSE.TXT), [libc++abi source license](https://github.com/llvm/llvm-project/blob/llvmorg-11.0.0/libcxxabi/LICENSE.TXT).

The exact upstream build revision of the prebuilt SDK archives was not established; the version observation does not prove an exact LLVM 11.0.0 source match. The toolchain distribution itself is fixed at OpenOrbis v0.5.4. These runtime notices do not change XCloud4's GPL-3.0-only license.

## Opus 1.5.2 — live audio integration under development

The live audio implementation links against an externally built static Opus decoder from the official **1.5.2** source release. The archive is `opus-1.5.2.tar.gz`, verified SHA-256 `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`. [Official release/source download](https://opus-codec.org/release/stable/2024/04/12/libopus-1_5_2.html).

Opus uses a BSD 3-Clause license. The exact release `COPYING`, including its copyright and patent-license references, is retained as [Opus-BSD-3-Clause.txt](docs/licenses/Opus-BSD-3-Clause.txt). The external library source and generated archive stay outside this Git repository; any source/configuration modifications must be recorded when distributing the library. Opus was not included in the confirmed 0.6.2 package. Native live audio has not yet been confirmed on PS4.

`src/media/live_media.c` and `src/audio/live_audio.c` implement original bounded RTP reception and decoding integration. Protocol references are [RFC 6184: H.264 RTP payloads](https://www.rfc-editor.org/rfc/rfc6184) and [RFC 7587: Opus RTP payloads](https://www.rfc-editor.org/rfc/rfc7587). No RFC source code was copied.
