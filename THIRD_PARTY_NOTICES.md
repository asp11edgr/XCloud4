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

### Optional access-country login table (1.2.1-beta / lab 0.7.58)

The fixed country/IP values in `src/ui/access_country.h` are adapted from
Better xCloud, copyright (c) redphx, MIT license: `redphx/better-xcloud`,
commit `f8397043f6d2148d2345d508902a38c69cf1ee20`,
[`src/enums/bypass-servers.ts`](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/enums/bypass-servers.ts).
The retained MIT notice is `docs/licenses/Better-xCloud-MIT-LICENSE.txt` and
is included in both packages. The original XCloud4 integration adds
`X-Forwarded-For` only to validated GSSV login requests when explicitly
selected by the user; Automatic omits it. These addresses are header values,
not proxies or video destinations. No account credential or third-party
service implementation is included. Host tests establish request isolation,
not acceptance in an unsupported country.

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

The 0.7.14 original `signal_remote_sdp` helper adds numeric diagnostics for a parsed nested exchange: JSON type/length, direct SDP member result/type/span, unchanged decoder success/length/nonempty result and a fixed header class. It records status and debugInfo member results/types/spans, plus messageType member result/type and a fixed absent/unknown/offer/answer class. The 32-byte temporary messageType buffer is securely cleared; debugInfo is not decoded or interpreted. No remote text, SDP contents, credentials or arbitrary debug information are printed. These classifications do not change the original direct nonempty SDP acceptance rule, protocol flow, RTC or media settings. This is original diagnostic code with no copied implementation; exact application source plus corresponding dependency/native-adapter source and licenses accompany distribution. Earlier snapshots remain immutable, and no repaired negotiation or live playback is established at preparation time.

The 0.7.15 original port patch omits `Description::Media::addSSRC` in libdatachannel's C API only when a receiving-only track supplies zero SSRC and no name, msid or trackId. This avoids emitting the incomplete `a=ssrc:0` declaration for an undefined local sending source. Explicit source metadata and other track directions retain the original behavior. The modified MPL-2.0 `src/capi.cpp`, exact pinned dependency trees, original patch helper and native adapters accompany this checkpoint in its matching dependency-source archive. [RFC 5576 source-attribute grammar](https://www.rfc-editor.org/rfc/rfc5576.html#section-4.1) is a protocol reference; no RFC implementation is copied. The owner-supplied Chromium parser results are external evidence, not a reproduced Xbox or PS4 result.

The original application adapter creates audio MID 0 before video MID 1, using [PSBox's track order](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/stream/stream_session.cpp#L413) as an interface reference. Both XCloud4 tracks remain receiving-only with the existing codecs and profiles. Original candidate handling recognizes strictly validated Teredo addresses and derives IPv4 UDP routes using the decoded port and fixed port 9002, preserving the supplied MID. [PSBox's candidate handling](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/stream/stream_session.cpp) and [RFC 4380 address format](https://www.rfc-editor.org/rfc/rfc4380.html#section-4) are protocol references; no reference implementation is copied. Sanitized structural diagnostics retain no raw SDP, candidate addresses or credentials. Teredo handling takes place after a remote SDP answer and does not establish the cause of the earlier exchange refusal. Combining these changes limits attribution if console behavior changes. No repaired negotiation or live media is established at preparation time; earlier package and source snapshots remain immutable.

The 0.7.17 original port patch selects `CLOCK_MONOTONIC` only for `X4_OPENORBIS` in the pinned libjuice `src/timestamp.c`. The SDK declares `CLOCK_BOOTTIME=7`, selected by the original upstream implementation, but XCloud4's native clock adapter rejects that ID. Its failed call makes libjuice return timestamp zero. The existing adapter translates `CLOCK_MONOTONIC=1` to native clock 4; other targets retain upstream clock selection. Sparse numeric clock/ICE/DTLS/SRTP/SCTP diagnostics are original additions to the pinned MPL-2.0 libjuice and libdatachannel sources and native application callbacks. Candidate submission counters retain existing conversion and cache behavior and do not assert ICE reachability. The exact modified trees, patch helper, native adapters and retained licenses must accompany this checkpoint's distribution. No upstream implementation is copied for these additions. The local clock incompatibility is source/build evidence; it has not been established as the cause of the console's RTC failure. Earlier package/source snapshots remain immutable.

The 0.7.18 original target patch makes the public usrsctp `sockaddr_conn` declaration use its BSD length/family layout for `X4_OPENORBIS` and makes libdatachannel write the length byte. The installed libc++ `__config` undefines `__FreeBSD__`, so the preceding C++ declaration used a 16-bit family at offset 0, while the actual userspace SCTP C library was configured with `HAVE_SCONN_LEN`/`HAVE_SA_LEN` and used a length byte at offset 0 plus an 8-bit family at offset 1. Static layout assertions and numeric construction/bind/connect diagnostics are original additions. No global platform macro or socket option is redefined. The modified usrsctp header retains its BSD notices and full [usrsctp license](docs/licenses/usrsctp-LICENSE.md); modified libdatachannel sources retain MPL-2.0 notices. Exact modified trees, native adapters, preparation scripts and licenses accompany this checkpoint. No AJ GeForce NOW implementation is copied; its native client is a research reference. The layout defect is verified locally, but its role in the preceding console failure requires a new console result. Earlier snapshots remain immutable.

The 0.7.19 original target patch replaces libdatachannel's automatic 65,536-byte `SctpTransport::doRecv` scratch array with one uninitialized RAII-owned heap array per invocation for `X4_OPENORBIS`. The existing mutex, pending-receive decrement, byte capacity, fragment/notification processing, vector copies and exception boundary are preserved. Other targets retain the upstream automatic array. The modified file retains MPL-2.0 notices; its exact source, preparation script and retained licenses accompany this checkpoint. This change addresses the large receive frame identified in the 0.7.18 console crash; it does not establish the native worker stack bounds or a repaired console result. No implementation from another client is copied. Earlier source/package snapshots remain immutable.

## SDK C++ runtime

Version 0.7 links the OpenOrbis SDK's `libc++.a` and `libc++abi.a`. The installed libc++ header reports `_LIBCPP_VERSION 11000` and identifies **Apache-2.0 WITH LLVM-exception**. Official LLVM 11.0.0 [libc++ license](docs/licenses/LLVM-libcxx-11-LICENSE.TXT) and [libc++abi license](docs/licenses/LLVM-libcxxabi-11-LICENSE.TXT), including legacy notices and LLVM exceptions, are retained. [libc++ source license](https://github.com/llvm/llvm-project/blob/llvmorg-11.0.0/libcxx/LICENSE.TXT), [libc++abi source license](https://github.com/llvm/llvm-project/blob/llvmorg-11.0.0/libcxxabi/LICENSE.TXT).

The exact upstream build revision of the prebuilt SDK archives was not established; the version observation does not prove an exact LLVM 11.0.0 source match. The toolchain distribution itself is fixed at OpenOrbis v0.5.4. These runtime notices do not change XCloud4's GPL-3.0-only license.

Version 0.7.16 incorporates the complete, unmodified LLVM 11 `condition_variable.cpp` as `src/streaming/rtc_condition_variable.cpp`, compiled explicitly with the installed native headers before the SDK's libc++ archive. [Official source](https://github.com/llvm/llvm-project/blob/176249bd6732a8044d457092ed932768724a6f06/libcxx/src/condition_variable.cpp), tag `llvmorg-11.0.0`, commit `176249bd6732a8044d457092ed932768724a6f06`, exact SHA-256 `39d39fc245662b760c7c74a29218eeb8c66805996dc00e25e1e77693e53a77a5`. The source retains its Apache-2.0 WITH LLVM-exception notice; the full LLVM libc++ license is distributed in `docs/licenses/LLVM-libcxx-11-LICENSE.TXT`. All five strong methods from the corresponding SDK archive member are supplied by this explicit object, so normal static linking leaves that old member unselected. The SDK archives themselves are not modified. The replacement uses the observed native `ETIMEDOUT=60`; the former prebuilt timed-wait method compares with 110 and treats native timeout as an exception. This narrow source replacement does not establish compatibility of every other prebuilt runtime component. The matching source export includes the exact C++ file and license; the application revision includes its Makefile build rule.

The 0.7.16 original Teredo helper records only a fixed numeric rejection reason (0 acceptance, 1–14 validation categories), preserving validation order, candidate conversion and buffers. No remote address, port, MID or text is logged. The categories identify a discarded-candidate rule, not a cause of RTC failure. Earlier application/dependency snapshots remain immutable; no repaired console behavior is established at preparation time.

## Opus 1.5.2 — live audio integration under development

The live audio implementation links against an externally built static Opus decoder from the official **1.5.2** source release. The archive is `opus-1.5.2.tar.gz`, verified SHA-256 `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1`. [Official release/source download](https://opus-codec.org/release/stable/2024/04/12/libopus-1_5_2.html).

Opus uses a BSD 3-Clause license. The exact release `COPYING`, including its copyright and patent-license references, is retained as [Opus-BSD-3-Clause.txt](docs/licenses/Opus-BSD-3-Clause.txt). The external library source and generated archive stay outside this Git repository; any source/configuration modifications must be recorded when distributing the library. Opus was not included in the confirmed 0.6.2 package. Native live audio was confirmed by the owner and the decoder log in 0.7.19.

`src/media/live_media.c` and `src/audio/live_audio.c` implement original bounded RTP reception and decoding integration. Protocol references are [RFC 6184: H.264 RTP payloads](https://www.rfc-editor.org/rfc/rfc6184) and [RFC 7587: Opus RTP payloads](https://www.rfc-editor.org/rfc/rfc7587). No RFC source code was copied.

## Xbox gamepad reports — 0.7.20

The original XCloud4 C encoder and portable state header implement the 38-byte single-controller wire format studied in [GreenVita input.rs](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/streaming/control/input.rs) (MPL-2.0) and [PSBox input_packet.cpp](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/stream/input_packet.cpp) (GPL-3.0-or-later). Those implementations were used as protocol references; their Rust/C++ source was not copied into this C sender. Xbox button positions, packet offsets and endianness are protocol constants. The periodic sender, source mailbox, stale-state policy, native lifecycle and local button combinations are XCloud4 implementation choices. A successful native send does not establish that Xbox consumed an input report; console validation is pending.

## CPU video staging — 0.7.21

XCloud4's original video staging and bounded copy implementation follows the instruction requirements in the [Intel SSE4 instruction reference, section 2.2.3](https://www.intel.com/content/dam/develop/external/us/en/documents/d9156103-705230.pdf): MOVNTDQA reads aligned 128-bit data from write-combining memory. The implementation checks CPU support before selecting the local SSE4.1 helper, copies only the validated NV12 image into a persistent CPU heap buffer, and converts that cached copy. The SSE2 fallback does not require SSE4.1. Native decoder output allocation and buffer ownership are preserved.

[Moonlight PS4's decoder](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c) was consulted as evidence about write-combining output reads and the cost of ordinary memcpy. No source implementation was copied or adapted from that repository; no LICENSE or COPYING file was found in its pinned source tree. Instruction facts and the research link do not confer a license to that code. XCloud4's new implementation remains GPL-3.0-only.

## CPU color conversion — 0.7.22

XCloud4's original SSE2 converter processes eight staged NV12 pixels at a time with signed 32-bit multiply/add results, arithmetic shifts and saturating packing. It preserves the existing integer BT.601 coefficients, rounding, chroma sampling and RGBA output order; remaining pixels use the existing scalar formula. SSE2 is part of the target's x86-64 instruction baseline. This implementation does not copy code from another streaming client. Native output ownership, the guarded SSE4.1 staging copy and its CPU fences remain unchanged. Instruction behavior is documented in the [AMD64 architecture programmer reference](https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/24592.pdf). XCloud4's converter remains GPL-3.0-only.

## Reception diagnostics — 0.7.32

The receive observations in `scripts/webrtc/prepare_receive_diagnostics.py` modify the pinned MPL-2.0 libdatachannel and libjuice files. Their existing license notices remain in the corresponding source export. The matching dependency archive includes the exact modified files, the numeric bridge interface and the reproducible patch scripts. The project bridge and bounded monitor are original GPL-3.0-only application code. No implementation from another streaming client is copied for these diagnostics.

The observations preserve existing transport and crypto branches, queue capacities and receiver feedback behavior. An internal Message extension carries numeric stage timestamps and provenance flags; it does not carry decoder PTS. Numeric SDP summaries and RTX correspondence omit credentials and complete SDP. The [reception test plan](docs/RECEPTION_DIAGNOSTICS_0.7.32.md) describes observations, blind boundaries and retention limits. A portable test or source inspection does not establish console scheduling cost or the cause of a missing packet.

## Public cover decoder — graphical UI

The unmodified `third_party/stb/stb_image.h` is retained from official upstream
commit `5736b15f7ea0ffb08dd38af21067c314d6a3aae9`, SHA-256
`38e08c1c5ab8869ae8d605ddaefa85ad3fea24a2964fd63a099c0c0f79c70bcc`.
[Pinned upstream source](https://github.com/nothings/stb/blob/5736b15f7ea0ffb08dd38af21067c314d6a3aae9/stb_image.h).
Upstream offers MIT or public-domain alternatives; XCloud4 chooses **MIT**.
The complete upstream dual notice stays beside the header, and the selected
[MIT copyright and permission notice](docs/licenses/stb_image-MIT.txt) is
included in packaged license files. The source receipt records exact hashes.
Only JPEG and PNG decoding are enabled. The bounded decoder allocator,
once-only cover resampling and public HTTP/cache integration are original
GPL-3.0-only XCloud4 code. No other streaming client's cover code was copied.

## Noto Sans — graphical UI

Unmodified Noto Sans Regular and SemiBold are retained from the official Noto
repository at commit `ffebf8c1ee449e544955a7e813c54f9b73848eac` under **SIL Open
Font License 1.1**. [Pinned upstream font directory](https://github.com/notofonts/noto-fonts/tree/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSans).
`assets/fonts/SOURCE.json` records source URLs and each font's SHA-256.
The original complete copyright/OFL notice is retained both beside the source
fonts and as [NotoSans-OFL.txt](docs/licenses/NotoSans-OFL.txt) for packaging.
The offline alpha atlas and compiled fallback are generated font assets under
the same OFL; generation uses PC-side Pillow/FreeType, with no runtime font
library added. Font source inputs, generator, atlas and notices accompany the
source export. These notices do not change the application code's license.

## Server recent-title protocol reference — graphical UI

[Greenlight's MRU route](https://github.com/unknownskl/greenlight/blob/07a3f01cba527c9c8287f90ef326c5e89c64687b/packages/desktop/main/helpers/xcloudapi.ts#L158)
and [result consumer](https://github.com/unknownskl/greenlight/blob/07a3f01cba527c9c8287f90ef326c5e89c64687b/packages/desktop/main/ipc/xcloud.ts#L73)
were inspected at commit `07a3f01cba527c9c8287f90ef326c5e89c64687b`.
That tree's [license](https://github.com/unknownskl/greenlight/blob/07a3f01cba527c9c8287f90ef326c5e89c64687b/LICENSE)
is MIT, copyright 2019 UnknownSKL (Jim Kroon). No TypeScript implementation
was copied; the GET path and `results[].titleId` response shape are protocol
references. XCloud4's bounded parser, auth-worker lifecycle and UI mapping
are original GPL-3.0-only code. The current account's authenticated MRU
response and PS4 UI remain unmeasured at preparation time; synthetic host
fixtures are identified as such in [UI_SERVER_RECENTS.md](docs/UI_SERVER_RECENTS.md).

## Better xCloud public /play device-info reference (0.7.42)

The original XCloud4 D/E/F serializers follow the public wire structure in
Better xCloud, branch `typescript`, pinned at
`f8397043f6d2148d2345d508902a38c69cf1ee20`:
[network.ts:316-337](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/network.ts#L316-L337)
and the bounded play-only override in
[xcloud-interceptor.ts:156-161](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/xcloud-interceptor.ts#L156-L161).
The host-derived upstream app ID is represented by `www.xbox.com` here.
No TypeScript interception implementation is copied. The upstream MIT notice
is retained in [Better-xCloud-MIT-LICENSE.txt](docs/licenses/Better-xCloud-MIT-LICENSE.txt).
The public request examples and differences are documented in
[STREAM_REQUEST_PROFILES.md](docs/STREAM_REQUEST_PROFILES.md).
This protocol experiment does not establish a delivered resolution or explain
the previous server selection of 1440p. XCloud4 remains GPL-3.0-only.

## Nayuki QR Code generator — support page QR rendering

Unmodified `qrcodegen.c` (`300eff07ee25baaa7578f20284411638154716379437391e7e689c0e6ce81403`) and `qrcodegen.h` (`e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589`) are retained in `third_party/qrcodegen/` from the official Project Nayuki repository at tag `v1.8.0` ([nayuki/QR-Code-generator](https://github.com/nayuki/QR-Code-generator/tree/v1.8.0/c)) under the **MIT License**.
The MIT notice is retained both in `third_party/qrcodegen/LICENSE` and as [qrcodegen-MIT.txt](docs/licenses/qrcodegen-MIT.txt).
The public build (`X4_PUBLIC=1`) calls `qrcodegen_encodeText` at runtime with static buffers and medium error correction to render the voluntary support QR code on the support page. XCloud4 remains GPL-3.0-only.

## Mozilla Root CA Store — bundled root certificates for older firmwares

Root CA certificates in `assets/certs/ca_bundle.pem` and `src/auth/ca_bundle.c` were extracted from Mozilla's CA store (pinned distribution via curl, `cacert-2024-09-24.pem`).
- Source URL: `https://curl.se/ca/cacert-2024-09-24.pem`
- Source SHA-256: `189d3cf6d103185fba06d76c1af915263c6d42225481a1759e853b33ac857540`
- License: Mozilla Public License 2.0 (MPL-2.0). The MPL-2.0 notice is retained in [docs/licenses/libdatachannel-MPL-2.0.txt](docs/licenses/libdatachannel-MPL-2.0.txt).
- Purpose: Provides trusted root certificates (DigiCert, Microsoft, USERTrust, Sectigo) to allow HTTPS verification on older PS4 system software (such as firmware 5.05) where built-in system roots are outdated (issue #3).

## libdatachannel RTT C API modification (1.2.0)

Version 1.2.0 includes an original modification to the pinned MPL-2.0 `src/capi.cpp` and `include/rtc/rtc.h` in `libdatachannel` to expose `rtcGetRtt(int pc)`, querying `PeerConnection::rtt()` from the active SCTP association. All original MPL-2.0 notices are preserved; the modified files retain their original MPL-2.0 license, included at `docs/licenses/libdatachannel-MPL-2.0.txt`. The reproducible preparation script `scripts/webrtc/prepare_rtt_api.py` accompanies this distribution and is invoked during the dependency build. Matching dependency source snapshots generated by `scripts/webrtc/export_source.sh` contain the modified files and reproducible scripts.

## Inter — graphical UI typography (1.2.0)

Unmodified Inter Regular, SemiBold, and Bold desktop TTF fonts are retained from the official Inter **v4.0** release (`Inter-4.0.zip`, SHA-256 `ff970a5d4561a04f102a7cb781adbd6ac4e9b6c460914c7a101f15acb7f7d1a4`) under the **SIL Open Font License 1.1**:
- [Official release](https://github.com/rsms/inter/releases/tag/v4.0).
- `Inter-Regular.ttf`: SHA-256 `64f8be6e55c37e32ef03da99714bf3aa58b8f2099bfe4f759a7578e3b8291123`.
- `Inter-SemiBold.ttf`: SHA-256 `0dc98e8aa59585394880f25ab89e6d915ad5134522e961b046ca51fad3a18255`.
- `Inter-Bold.ttf`: SHA-256 `0cb1bc1335372d9e3a0cf6f5311c7cce87af90d2a777fdeec18be605a2a70bc1`.
- Retained license: `assets/fonts/Inter-OFL.txt` and [Inter-OFL.txt](docs/licenses/Inter-OFL.txt) (SHA-256 `262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a`).
`assets/fonts/inter.x4font` is generated offline by `scripts/generate_font_assets.py` under the same OFL 1.1 license, while `assets/fonts/noto_sans.x4font` is retained as fallback.

## Tabler Icons — graphical UI icons (1.2.0)

UI icons for settings (gear), user/account, search (magnifying glass), info, support (heart), home, chevrons, display (`device-desktop`), and server are rasterized offline into alpha bitmaps (`src/ui/tabler_icons_atlas.h`) from the official **Tabler Icons** repository at tag `v3.24.0` ([tabler/tabler-icons](https://github.com/tabler/tabler-icons/tree/v3.24.0)) under the **MIT License**.
- Retained license: [Tabler-Icons-MIT.txt](docs/licenses/Tabler-Icons-MIT.txt) (SHA-256 `d5a3f948ae5cb95bfc4d2f767ef2bf6a778815b0ddb2d1c48037744cf636518e`).
- Reproducible generator: `scripts/generate_tabler_icons.py` using unmodified SVG paths in `assets/icons/tabler/`.



