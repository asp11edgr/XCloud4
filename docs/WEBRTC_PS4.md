# Native WebRTC transport on PS4

## Existing application references

GreenVita is the main Xbox protocol reference: session creation, readiness, connection authorization, SDP, ICE, keepalive and closure. Better xCloud supplies settings/region research. Moonlight PS4 supplies native API research; its Sunshine transport does not itself connect to Xbox.

XCloud4's original C implementation of Microsoft, RPS, XSTS, cloud credentials and catalog was confirmed on PS4 in 0.4.0. Session preparation/deletion followed in 0.5.0 and Passport plus `/connect` in 0.6.2. Reusing protocol research retains that work while adapting console operations to OpenOrbis. GreenVita's Rust and Moonlight PS4's implementations have not been copied.

**The confirmed 0.6.2 milestone does not receive game video/audio.** Native WebRTC and actual media reception are the current development stage.

## Libraries reviewed on October 8, 2026

| Library | Pinned revision | Observed components | Status at the confirmed baseline |
|---|---|---|---|
| libdatachannel | `bdc5ff28e9d3b863144c94a677ecf5bf043aaf15` | C++17, C API, libjuice ICE, DTLS, SRTP, SCTP; optional Mbed TLS | Configured for OpenOrbis; first build stopped on incompatible headers; not included in 0.6.2 |
| libpeer | `5b849de378545c31d34759a145413846953e1366` | C, BSD sockets, Mbed TLS, libsrtp, usrsctp, cJSON | Alternative requiring comparison against Xbox media and channel requirements |

Primary sources: [libdatachannel](https://github.com/paullouisageneau/libdatachannel/tree/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15), [libpeer](https://github.com/sepfy/libpeer/tree/5b849de378545c31d34759a145413846953e1366). Observed licenses are MPL-2.0 and MIT, respectively. Dependencies retain separate licenses and must be pinned/attributed before distribution.

## Local toolchain evidence

The Lubuntu OpenOrbis v0.5.4 installation contains `include/c++/v1`, `libc++.a`, `libc++abi.a`, `pthread.h`, `poll.h`, `sys/socket.h` and import library `libScePosix.so`. Symbol inspection finds `pthread_create`, `pthread_join`, `socket`, `sendto`, `recvfrom`, `select` and `clock_gettime` in Posix; `poll` also appears in libkernel.

Available declarations/symbols support porting research. They do not establish that calls, ABI structures or WebRTC libraries work on firmware 12.00. No transport runtime result is claimed from this inspection.

## Initial external build work

Mbed TLS **3.6.7**, revision `068ff080b369adfac81509f9b57b2afabaf82dc5`, built static `libmbedcrypto.a`, `libmbedx509.a` and `libmbedtls.a` in Lubuntu for OpenOrbis. External source and archives stay outside Git/PKG at the confirmed 0.6.2 baseline. `scripts/webrtc/openorbis.cmake` and `mbedtls-user-config.h` record configuration. No Mbed TLS programs or tests were run.

The profile enables DTLS-SRTP, disables Mbed TLS's own network/file layers and requires `mbedtls_hardware_poll` for native cryptographic entropy. Initial configuration did not yet include that adapter. A fixed seed or non-cryptographic substitute is not acceptable.

libdatachannel configuration with Mbed TLS succeeded. Its first `datachannel-static` build stopped on missing BSD `u_int`/`u_long` types in usrsctp and missing `pthread_np.h` in libjuice. Before console use, the port must also review SDK `sockaddr_storage`, DNS/interface discovery, thread ABI and libjuice entropy. The current stage addresses those items; successful archive compilation must be distinguished from a working native session.

Pinned submodules from that review:

- libjuice: `b89c792e3612faf2f12cf35bcc56857313a06be3`.
- libsrtp: `d33b8ffb1491a0b4b58a206889f09800cf7310ab`.
- usrsctp: `fec583d54493f879d2ae44a743423bf8a04371ab`.

Each retains its own license; distribution requires applicable notices and modification/source references. Primary sources: [Mbed TLS 3.6.7](https://github.com/Mbed-TLS/mbedtls/tree/068ff080b369adfac81509f9b57b2afabaf82dc5) and [libdatachannel build documentation](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/BUILDING.md).

## Implementation requirements

The development offer currently selects H.264 payload type **102**, `profile-level-id=42e01f`, packetization mode 1, and limits `max-fs=3600` / `max-mbps=108000`; audio uses Opus payload type **111**, 48 kHz, two channels. The requested starting stream is 720p/30 FPS at 5 Mbps. These are implementation settings, not confirmed service or console performance.

Port work has continued beyond the initial header failure. The pinned libdatachannel build completed at 100% with SDK/C++ ABI adaptation and consistent Mbed TLS configuration. Final vendor patches are stable and all static archives are installed. The complete 0.7.0 application compiled, linked, converted to SELF and packaged successfully. Its uploaded/retrieved package hash matched. The first console attempt accepted Xbox connection authorization, but local RTC opening returned -2 before SDP, ICE or media reception; actual game video/audio remains unconfirmed.

The native authentication/media source objects compiled without warnings. The external prefix contains Mbed TLS crypto/X.509/TLS archives, its Everest/p256-m support archives, and Opus. Static ELF inspection records 104 imports, 34 constructor entries and an 8-byte TLS section; these are binary structure observations, not native ABI execution evidence. Exact dependency license texts and bundled attribution notices are retained in [docs/licenses](licenses/README.md), with modification/source-availability details in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

`scripts/webrtc/export_source.sh` exports the actual patched working trees without building or changing them. It checks pinned revisions, records file hashes and rejects source edits during copying. It includes submodules, Opus, overlays, port configuration, native adapters and license texts, while excluding Git metadata, generated builds, SDK binaries and private runtime data. Export waits for stable vendor sources; its manifest describes source bytes rather than claiming native compatibility.

The matching source snapshot is prepared locally as `XCloud4-0.7.0-dependency-sources.tar.gz`: **83532544 bytes**, SHA-256 `0e48073b43a94e68c109cf76ca08fdfb7c3e967f0908f2f72e732fb843624d68`. Its manifest covers **9367 source files** from the patched dependencies and native port, plus generated inventory metadata. Every recorded source hash matched the archive. Original public upstream test fixtures remain included; build caches, Git metadata, local credentials and runtime logs are excluded. This snapshot must accompany distribution of its matching 0.7.0 package and must be regenerated if the captured source changes.

### First 0.7.0 console attempt

The console log records native entropy and NetCtl initialization returning zero, Xbox `/connect` HTTP 202 and Provisioned state HTTP 200. Local RTC opening then returned -2. No SDP, ICE or RTP was reached, so this result does not diagnose service negotiation or media decoding. Remote deletion returned HTTP 200. Instrumentation of the local RTC creation path is the next repair step; this observed failure is distinct from the earlier Passport `invalid_scope` refusal.

### 0.7.1 diagnostic checkpoint

Version 0.7.1 adds fixed initialization-stage labels and numeric native/Mbed TLS error diagnostics in `src/capi.cpp`, `src/impl/init.cpp`, `src/impl/tls.cpp` and the native adapters. No account credentials, SDP or ICE values are logged. Its full package compiled and its upload/retrieval hash matched. The actual console log reached thread-pool creation with count **200112**, then reported numeric **system error 1** before the pool-ready checkpoint. No SDP, ICE or media was reached. The immutable 0.7.0 source snapshot is preserved separately. See [0.7.1 notes](RELEASE_NOTES_0.7.1.md).

Matching source `XCloud4-0.7.1-dependency-sources.tar.gz` is **83536524 bytes**, SHA-256 `46501c565b2bf1782c8dbf971c13f474b32f4ccf76a86083f343207ecbdf9891`. All **9367** manifest source hashes match. It captures the additional diagnostic vendor files and updated native adapters/port script. The exporter now selects the package version, supports `X4_SOURCE_VERSION` explicitly and refuses to replace an existing snapshot.

### 0.7.2 explicit worker limit

ABI inspection found that the SDK's prebuilt libc++ CPU-count probe calls `sysconf(84)`. In the native FreeBSD selector mapping, 84 is `_SC_THREAD_CPUTIME`, whose `_POSIX_THREAD_CPUTIME` value is **200112L**; CPU-count selectors are 57/58. This explains the implausible count observed by 0.7.1. Version 0.7.2 calls `rtcSetThreadPoolSize(4)` before `rtcCreatePeerConnection`, checks its result and records actual created-worker counts and creation failure. It retains the native process-ID syscall 20 implementation; the correction bounds RTC policy rather than changing that syscall.

Claude Opus 5.5 completed a read-only review in **29 turns**, without an error, and recommended bounding the pool before initialization. The resulting 0.7.2 package compiled and its FTP round-trip hash matched. Its console result and real video/audio remain pending. Matching patched sources are captured separately as `XCloud4-0.7.2-dependency-sources.tar.gz`, including `src/impl/threadpool.cpp` diagnostics and the exact native adapters. Previous snapshots remain intact. See [0.7.2 notes](RELEASE_NOTES_0.7.2.md).

1. Pin dependencies and configure static OpenOrbis builds with examples/tests disabled.
2. Review sockets, threads, timing, DNS and cryptographic entropy ABI. Resolve missing declarations/exports while retaining bounds and cancellation.
3. Generate a real SDP offer and DTLS fingerprint on PS4 and exchange them with Xbox using the researched session protocol.
4. Receive SRTP, reconstruct H.264 frames for native decoding, decode Opus and feed AudioOut.
5. Implement Xbox control/input channels and connect DualShock 4 state.

Session preparation or accepted `/connect` is not evidence of functioning WebRTC or a complete game.

## Protocol sources

- [Live native media design and current limits](MULTIMEDIA_EN_VIVO.md).
- [RFC 6184: H.264 RTP payload format](https://www.rfc-editor.org/rfc/rfc6184).
- [RFC 7587: Opus RTP payload format](https://www.rfc-editor.org/rfc/rfc7587).

- [Session creation/settings](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/api.rs).
- [State, connection, SDP, ICE and closure](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/stream.rs).
- [GreenVita RTC session](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api/streaming/rtc/session.rs).
