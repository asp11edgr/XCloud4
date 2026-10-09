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

ABI inspection found that the SDK's prebuilt libc++ CPU-count probe calls `sysconf(84)`. In the native FreeBSD selector mapping, 84 is `_SC_THREAD_CPUTIME`, whose `_POSIX_THREAD_CPUTIME` value is **200112L**; CPU-count selectors are 57/58. This explains the implausible count observed by 0.7.1. Version 0.7.2 calls `rtcSetThreadPoolSize(4)` before `rtcCreatePeerConnection`, checks its result and records actual created-worker counts and creation failure. The existing SDK thread-ID adapter is retained.

Claude Opus 5.5 completed a read-only review in **29 turns**, without an error, and recommended bounding the pool before initialization. The resulting 0.7.2 package compiled and its FTP round-trip hash matched. Its console capture records pool-size result zero, workers 1/2/3/4 created, and completed thread-pool/PSA/SCTP/DTLS/SRTP/ICE library initialization. Peer-connection creation returns handle 1, and local track/data-channel creation succeeds. `rtcSetLocalDescription(pc, "offer")` then reports a runtime-error category and returns **-2 before the SDP callback**. These results establish local initialization, not ICE connectivity or received media.

Matching patched sources are captured separately as `XCloud4-0.7.2-dependency-sources.tar.gz`: **83536843 bytes**, SHA-256 `3932e6593bf76eb26851c4db8ee0448d9b76e9f95a68163022e975558251c3c0`; all **9367** manifest source hashes matched. The snapshot includes `src/impl/threadpool.cpp` diagnostics and exact native adapters. Previous snapshots remain intact. Real game video/audio is still unconfirmed. See [0.7.2 notes](RELEASE_NOTES_0.7.2.md).

### 0.7.3 offer and native network diagnostics

Safe fixed checkpoints now cover offer stages, ICE agent creation/local description/gathering results, underlying exception categories, ephemeral certificate generation/formatting and native UDP/socket/poll/resolver/interface results (events 50–63). libjuice `agent.c`, `conn_poll.c` and `udp.c` add numeric result/errno/count evidence; native logging preserves `errno`. No credentials, SDP, ICE values or certificate contents are logged.

The final dependency build and full 0.7.3 application/package build completed successfully. The uploaded/retrieved package hash matched. Claude Opus 5.5 completed its read-only offer review in **9 turns**, successfully, without code changes or a demonstrated fix. Queued callback delivery means the missing SDP callback alone does not identify the internal failure stage; offer/ICE/certificate/network checkpoints separate those paths.

The actual 0.7.3 capture records certificate DER **362 bytes** and readiness, ICE-agent/local-description results zero, offer media count **3** and commit. Gathering begins; poll-pipe/poll-thread and bind-address resolution return zero. UDP `F_SETFL` then fails with **errno 13 (`EACCES`)**, connection returns -1, gathering -2, and local-description request -2. Remote cleanup returns HTTP 200 and no RTP is received. The failure is isolated to native UDP flag setting; the exact reason for denial and its repair are still under investigation. Real game video/audio remains unconfirmed.

The immutable matching source snapshot is **83543499 bytes**, SHA-256 `5c39a0ed1d50c39cc8bff12550f6e86ff7e6f13a55a4abab029e4ec086f857e9`; all **9367** manifest source hashes matched. It preserves the exact 0.7.3 provider changes before later repairs. See [0.7.3 notes](RELEASE_NOTES_0.7.3.md).

### 0.7.4 direct socket nonblocking request

libjuice UDP/TCP now use direct BSD `FIONBIO`, defined through the existing SDK `_IOW('f', 126, int)` encoding, **`0x8004667e`**, with a four-byte integer argument. Numeric event 64 records zero or actual socket errno; a rejected setup closes the socket and fails. Pipe `F_SETFL` handling and the four-worker policy remain in place. The [FreeBSD descriptor source](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/sys/kern/kern_descrip.c) shows `F_SETFL` also requesting asynchronous mode, but attributing the PS4 denial specifically to that extra request remains a hypothesis. Existing `fcntl` constants were verified.

The final dependency/application/package build succeeded, and the transferred/retrieved package hash matched. Static ELF/OELF inspection verifies the ioctl request, pointer to integer one and native `_ioctl` import mapping. Claude Opus 5.5 completed a read-only socket review in **5 turns**, without identifying a defect within that review. Neither static inspection nor review establishes hardware success.

The actual 0.7.4 capture records certificate DER **359 bytes**, local-offer media count **3** and commit, then direct `FIONBIO` **errno 13 (`EACCES`)**. Connection/gathering/local-description results are -1/-2/-2, remote cleanup returns HTTP 200, and no RTP is received. The direct request does not resolve the socket denial; its exact native cause remains unconfirmed. Native network API/descriptor compatibility is being investigated. Live game video/audio remains unconfirmed.

Matching 0.7.4 source is preserved separately: **83543344 bytes**, SHA-256 `6bf845b53bdeab0898b33d3d1e07876661c24324f7a4ad6a06c4bfa18d311c31`; all **9367** manifest hashes matched. It captures this failed attempt before subsequent repairs, with every previous archive unchanged. See [0.7.4 notes and primary ABI references](RELEASE_NOTES_0.7.4.md).

### 0.7.5 original native SO_NBIO adapter

An original adapter sets `SO_NBIO=0x1200` through POSIX `setsockopt` on the existing kernel descriptor, with `SOL_SOCKET=0xffff` and a four-byte integer one. It then queries the same option through `getsockopt` and requires enabled mode plus a returned size of four bytes. Events 65–68 record set/get errno, mode and size. Any operation/state/size failure rejects socket setup. UDP/TCP use the adapter, preserving the four-worker policy and pipe handling.

Pinned Sony/WebKit and WoWPS sources support the interface/constant research; their implementations were not copied. WoWPS documents a similar OpenOrbis descriptor/nonblocking limitation, while XCloud4's EACCES evidence is its own console capture. No SDK implementation or exact internal denial cause is established by that comment. Claude Opus 5.5 completed a read-only review in **5 turns**, with no concrete defect identified within that scope. Dependency/application/package builds and static ELF/OELF option/import checks succeeded.

The actual console set/query results are errno **0**, enabled mode **256 (`0x100`)** and size **4**. Nonblocking socket setup, connection and interface query succeed; one interface and one host candidate are recorded. Gathering, offer readiness and the local-description request return zero. Xbox accepts `POST /sdp` with **HTTP 202**, but repeated answer requests remain **HTTP 204** without a remote SDP answer until keepalive returns **HTTP 410**. Remote deletion returns **HTTP 200**. This confirms local socket/gathering progress, not completed ICE connectivity: no RTP or live game video/audio is received. The reason for the missing answer/session expiry remains unconfirmed.

Matching 0.7.5 source is preserved separately: **83543722 bytes**, SHA-256 `8efc459212d3bc99304442e3f98f1c970af3907fb225600933493001eb9d841b`; all **9367** manifest source hashes matched and the PC copy matches. Previous snapshots remain unchanged. See [0.7.5 notes and source pins](RELEASE_NOTES_0.7.5.md).

### 0.7.6 structural SDP and HTTP diagnostics

Original bounded diagnostics describe media/codec/feedback/BUNDLE/setup/trickle/candidate structure through enums and numbers, without logging raw SDP or private values. HTTP diagnostics retain fixed allowlist categories and numeric timing/status/object summaries before clearing response data. Negotiation behavior and the 30-second keepalive interval are unchanged. Claude Opus 5.5 reviewed SDP/protocol configuration in seven turns and the scanner separately in two turns. The scanner review found a diagnostic-only setup/trickle labeling limitation above eight media sections; the actual three-section offer is unaffected.

The console capture again accepts SDP submission with HTTP 202 and receives HTTP 204 through **33** polls. Keepalive returns **HTTP 410**, allowlist category **2 (`SessionNotActive`)**, **118 bytes**, after **30317 ms**; remote deletion succeeds with HTTP 200. No remote SDP, completed ICE, RTP or game media is received. The callback header is reconstructed from interleaved fragments: **1093 bytes**, three sections, zero candidates, three unique/matched BUNDLE entries. Individual sections show video/audio/application order, H.264 102/profile42E01F, Opus111 and SCTP5000. These are structural evidence, not a successful exchange.

Source inspection identifies a retained initial callback offer despite completed gathering. Obtaining the current local description after gathering is the next repair target; the missing answer's precise cause and repair result remain unconfirmed. The full package built without warnings/errors, and PC/VM/PS4 hashes match. Matching source is **83544715 bytes**, SHA-256 `14f8578e49d8bef7497b68b8803d6ed71c0fc5d720f72687d7ba05b515246f2d`; all **9367** source hashes and the copied archive matched. See [0.7.6 package, runtime and review evidence](RELEASE_NOTES_0.7.6.md).

### 0.7.7 current description after gathering

The original transport waits for actual gathering completion and reads the current local offer through the provider C API. It verifies offer type, queried/read length, capacity and termination within 32768 bytes, retaining pending availability and native errors under existing deadlines. Candidate addresses are not synthesized or merged; no extra candidate prerequisite replaces the provider's completion state. The structural summary now describes this current offer, and the scanner's >8-media setup/trickle labeling is repaired. H.264 profile, media order and HTTP signaling are unchanged.

The full application/package build succeeded without warnings/errors, and static peer review completed successfully. Claude Opus 5.5 completed its focused read-only review in **2 turns / 1 read**, with no proven defect found within that scope. VM/PC/PS4 package retrieval hashes match.

The actual console reports gathering complete (enum2) and a **1201-byte** current offer with **one candidate**, three unique/matched BUNDLE entries and media ports **61345**. H.264102/profile42E01F/four feedback entries, Opus111 and SCTP5000 remain. The refreshed-description source issue is resolved in this capture, but Xbox accepts POST with HTTP202 then returns HTTP204 through **33** polls without an answer. Keepalive returns **HTTP410 / SessionNotActive**, **118 bytes**, at **30492 ms**. Remote cleanup returns HTTP200 with no RTP; OPTIONS closes resources and returns without CE-34878-0. The owner reports the same session error. Remote negotiation and game video/audio remain unconfirmed.

Matching sources closed separately: **83544519 bytes**, SHA-256 `33e30efb0c20cf45e976e950b8d320147f46bad60f4224bf685ce680c14949b8`, with all **9367** manifest source hashes verified and the PC copy matching. Previous snapshots remain unchanged. See [0.7.7 notes](RELEASE_NOTES_0.7.7.md).

### 0.7.8 native resolver timeout

The original resolver's native timeout argument changes from `2` to **`2000000` microseconds (two seconds)**, with numeric-only return/timing diagnostics. The pinned [ioQuake3-PS4 call](https://github.com/Mayo1970/ioQuake3-PS4/blob/d4c7912af73c3b0eaa57195391ad76f21e79a910/code/qcommon/net_ip.c#L447) uses `3 * 1000 * 1000` and supports the unit research. No reference implementation is copied; XCloud4 retains its original resolver and selects its own bound. Hostnames, addresses, ICE values and credentials are omitted.

The pinned project's technical DNS note explicitly identifies microsecond units. Events 69–73 record resolver creation result, timeout, lookup result, bounded elapsed microseconds and a nonzero-address boolean, with no actual address output. Only original resolver/diagnostic-label files change; vendors are unchanged.

This repairs a timeout argument in source; DNS is not established as the sole cause of the earlier missing Xbox answer. The full application/package built without warnings/errors, SDK elapsed-time return width is confirmed, and VM/PC/PS4 retrieval hashes match. Current-offer/H.264 settings are unchanged by this patch.

The actual console resolver results are creation **0**, timeout **2000000 microseconds**, lookup **0**, elapsed **11519 microseconds** and nonzero-address boolean **1**. The current SDP grows to **1286 bytes / two candidates**, with three media sections and BUNDLE entries. Xbox accepts submission with HTTP202, returns HTTP204 through **37** polls, then keepalive succeeds with HTTP200/37 bytes. The next SDP request returns **HTTP200/243 bytes**, with exchange result **-2** from non-null `errorDetails`. The exact logical-error category remains unknown. Cleanup returns HTTP200 with no RTP/video/audio. The owner reports a new error; successful DNS and more candidates do not establish a complete negotiation.

Matching source closed from the frozen mirror: **83545142 bytes**, SHA-256 `9aac8351b1b93b0f4e619e81a3291f74567ccaf0dd4da516f0e61fecf654a0e0`. All **9367** source hashes matched, with an identical PC copy and every prior snapshot unchanged. See [0.7.8 package evidence and exact sources](RELEASE_NOTES_0.7.8.md).

### 0.7.9 logical signaling diagnostics

The original authentication diagnostics extend to logical failures in successful HTTP responses for SDP/ICE exchange, acknowledgement and keepalive. Fixed classes0–14 remain; bounded nodes0–4 describe root/errorDetails/error and one details-object level with JSON types, validated uint32/signed32 code data, uint32 status and message types only. Diagnostics use no recursion/allocation and run before clearing private data. No arbitrary provider text or credential/SDP/ICE values are logged. Negotiation decisions and media behavior remain unchanged. The purpose is to classify the non-null errorDetails refusal seen in 0.7.8, whose exact server category is still unknown.

Claude Opus 5.5 completed a focused read-only review in **8 turns**, without a material finding in the reviewed diagnostic/JSON scope or source edits/tests. The full application/package built without warnings/errors, and VM/PC/PS4 retrieval hashes match.

Actual DNS succeeds (lookup0/elapsed11086 microseconds), with current SDP1286 bytes/two candidates and POST HTTP202. After **37** HTTP204 polls, keepalive returns HTTP200/37 bytes, then SDP HTTP200/243 bytes at **30251 ms / poll count38**. Route1 classification reports errorDetails object(type1), unrecognized string code(type3/class1), and string message type3; no root-code/status/nested-details value is reported. This identifies structure, not a specific refusal category. Remote cleanup returns HTTP200 with no RTP/video/audio.

The exact dependency/adapter source archive is **83545064 bytes**, SHA-256 `b2ab42ea193254aa4aa71c891a3e53396850685418cc6c366783e3abfdd94235`; all **9367** hashes match, with identical guest/PC copies. Application checkpoint `0e328bf` contains the authentication diagnostics separately from the unchanged RTC/vendor trees. Earlier snapshots remain immutable. See [0.7.9 source and runtime evidence](RELEASE_NOTES_0.7.9.md).

### 0.7.10 restricted service-code name

The original authentication helper admits only the direct errorDetails object's string code after checking 1–64 ASCII characters: first a letter, then letters/digits/underscore. Invalid/absent/duplicated values stay silent and the 65-byte temporary buffer is always cleared. Message/body/top-level/nested/other strings remain hidden. Numeric diagnostics/classes 0–14 and all negotiation/media decisions are unchanged.

Pinned CloudNow/OpenXbox references support identifier filtering and separate code/message fields; no implementation is copied. The helper is original with narrower bounds, and no exhaustive server enum or timing-based refusal cause is established. Product 0.7.10 uses PS4 APP_VER 00.80. Claude Opus 5.5 completed a focused read-only review successfully in 3 turns / 2 reads, without a material finding within the helper/JSON scope. The full application/package built without warnings/errors, and VM/PC/PS4 retrieval hashes match.

Application checkpoint `0fbe73a` contains the authentication helper separately from the matching dependency/native-adapter archive: **83558285 bytes**, SHA-256 `4dc79943434d23a05840665254769984d905e0e0e1bb9e4a186fa4ec03b39a77`. All **9367** source hashes and the PC copy match; vendors/RTC/configuration remain unchanged, with corresponding notices updated. Earlier snapshots stay immutable. See [0.7.10 package/source evidence, pins and metadata](RELEASE_NOTES_0.7.10.md).

The first console capture records DNS lookup0/12356 microseconds/nonzero1 and SDP1285 bytes/two candidates/three media and BUNDLE entries. POST returns HTTP202, followed by **38** pending HTTP204 polls. Keepalive starts at **30379 ms**, then returns **HTTP410/118 bytes** at **30670 ms**, root code class2/SessionNotActive with no errorDetails. The new name helper is not entered; cleanup returns HTTP200 and no RTP/video/audio is received.

A second attempt on the same installed version records SDP1285 bytes/two candidates and **37** pending HTTP204 polls. Keepalive starts at **30187 ms**, then returns HTTP410/118 bytes at **30480 ms**, root code class2/SessionNotActive without errorDetails. The name helper is again not entered; cleanup returns HTTP200 with no RTP/media. The underlying earlier logical SDP refusal remains uncaptured and no correction is established.

Both attempts pass ReadyToConnect, accepted connect202 and Provisioned before SDP; failure occurs during subsequent negotiation/polling. WaitingForResources is handled as waiting under the initial **180-second provisioning limit**. Extended queue duration/UI remain incomplete and are [recorded for improvement](MEJORAS_FUTURAS.md); no actual server wait-time estimate is established here.

### 0.7.11 pending SDP before heartbeat

The original authentication loop reads/processes pending SDP before servicing a due heartbeat. A still-pending result is cleared before heartbeat servicing and the existing 500-ms wait; an applied answer services heartbeat before returning toward ICE. The 30-second interval remains, with next deadline based on the captured dispatch timestamp rather than HTTP completion, avoiding addition of response duration to the scheduled deadline.

Pinned GreenVita backend scheduling supplies the protocol reference; no Rust code is copied. XCloud4's GET remains synchronous and may delay a pulse by one request, with existing cancellation/deadline guards intact. No server lifetime is confirmed; the aim is to capture an available logical SDP refusal before heartbeat HTTP 410. Initial heartbeat, SDP/parser/media settings, queue handling and cleanup remain. Product 0.7.11 uses APP_VER 00.81. Claude Opus 5.5 completed the focused read-only review in **3 turns / 2 reads**, with no concrete defect within that scope.

The full application/package built without warnings/errors: **8912896 bytes**, SHA-256 `13337dc56d3b1a84b8f2e083721c1348860e257d79e494eac20e45c54e558c9b`; VM, PC and PS4 retrieval hashes match. Application revision `27dc26112a4df0176c5c7b782d3a051bb4124b4a` supplies the authentication change separately from the closed dependency/native-adapter archive: **83560456 bytes**, SHA-256 `fbd5e7dd0213173bf52aab4a2a21489fd026bda66392e2a24004b831daf57778`. All **9367** manifest hashes matched and the PC archive hash matches the guest archive. Earlier snapshots remain immutable.

The console confirms the 0.7.11 banner and successful readiness/authorization/connect. Native DNS lookup returns **0** in **15248 microseconds**, with the nonzero-address boolean **1**. The current offer is **1286 bytes / two candidates**, with three media sections and three unique, matched BUNDLE entries. SDP submission returns HTTP 202, followed by **38** pending HTTP 204 polls. Keepalive is attempted at **29903 ms**, then returns **HTTP 410 / 118 bytes** at **30201 ms**, root-code class **2 (`SessionNotActive`)**, without errorDetails. The restricted-name helper is not entered; remote deletion returns HTTP 200, with no RTP/video/audio. The earlier logical SDP refusal remains uncaptured, and no corrected server negotiation or game media is confirmed. See [0.7.11 evidence, exact pin and limits](RELEASE_NOTES_0.7.11.md).

### 0.7.12 final SDP diagnostic

The frozen original authentication change adds one bounded, diagnostic-only GET/SDP when the normal SDP result is pending and keepalive fails with session ERROR/status error, actual/public HTTP 410 and strictly parsed root-object code 2 (`SessionNotActive`). The private flag resets on every keepalive call and is consumed by the helper. Cancellation must be absent and at least 30 seconds of budget remain inside the existing 90-second negotiation deadline; both are checked again after the request. The direct session call preserves the original failure, public snapshot and poll counters. It classifies the response before fully clearing buffers, discards any answer and makes no POST retry.

The heartbeat interval and dispatch scheduling remain as in 0.7.11, with no codec/media/queue changes. Product 0.7.12 uses APP_VER 00.82. Claude Opus 5.5 completed the focused read-only review in **3 turns / 2 reads**, without a concrete defect in that visible scope. Existing synchronous native HTTP timeout checkpoints remain a limitation.

The full application/package built without warnings/errors: **8912896 bytes**, SHA-256 `eafd597883970adb622bac06efe46b372516323c9e70de59a502ad24a31f040f`; VM, PC and PS4 retrieval hashes match. Application revision `3710c623b10a4fb30fe96ba5d91f2a3ef7bb144d` supplies the authentication change separately from the immutable dependency/native-adapter archive: **83559046 bytes**, SHA-256 `a8536d57a7ea626dadcc15a11a1d22ff3f2d931a48ade115c2d7ee3db5f54fa2`. All **9367** manifest hashes matched locally, with identical PC/guest archive hashes. Earlier snapshots remain preserved.

The console passes readiness/provisioning/authorization/connect. Native DNS lookup returns **0** in **14251 microseconds**, with the nonzero-address boolean **1**; the current offer is **1286 bytes / two candidates / three media sections / three BUNDLE entries**. After SDP submission returns **HTTP 202** and **37** pending HTTP 204 polls, keepalive at **29723 ms** returns **HTTP 200 / 37 bytes**. SDP poll **38** returns **HTTP 200 / 243 bytes** at **30840 ms**, containing an errorDetails object, string code outside the fixed allowlist (class 1) and string message. The existing 0.7.10 restricted-name helper captures **`ConnectionExchangeFailed`**; message text stays hidden.

The new terminal GET is **not entered**, because keepalive succeeds. The regular SDP response identifies a logical refusal, and no precise underlying SDP/codec cause is established. Remote deletion returns HTTP 200, with no RTP/video/audio received; the owner reports the session error. No corrected negotiation, server lifetime or game media is established. See [0.7.12 actual evidence and limits](RELEASE_NOTES_0.7.12.md).

### 0.7.13 fixed message categories

The frozen original diagnostic examines only a direct errorDetails object/message string, rejecting duplicate or inappropriate fields through strict JSON checks. Its 1025-byte buffer accepts 1–1024 decoded ASCII bytes with CR/LF/TAB allowed; decoding is skipped above a 6146-byte raw JSON span. Only validity/raw/decoded lengths and a fixed 18-bit mask are logged. The buffer is cleared on every path; message contents, response bodies, SDP and credentials remain hidden. Explicit ASCII case folding/word boundaries separate ICE from service/device substrings. Lexical presence is not a demonstrated failure cause; existing public-code/numeric diagnostics and all network/media/business-parser decisions remain.

Claude Opus 5.5's preceding read-only analysis completed in 7 turns / 5 reads without proving a cause. Separate RTC peer inspection and Claude's focused new-code review (**3 turns / 2 reads**) completed without a material finding in the helper/decoder scope. Product 0.7.13 uses APP_VER 00.83.

The full application/package built without warnings/errors: **8912896 bytes**, SHA-256 `4f2e4568435ae1e2e354d4655ad51db9b28eabe827c581a64d8c27afb9edd0b4`; VM, PC and PS4 retrieval hashes match. Application revision `1d3a3d13cc73aa1cfdd3b3e313a778cc4694fa49` supplies the authentication diagnostic separately from the immutable dependency/native-adapter archive: **83562288 bytes**, SHA-256 `9d54b7c68564b06ea5d2da8cd4654a5045e1669d2d5386cfe06140790c574037`. All **9367** manifest hashes matched locally, with identical PC/guest archive hashes; earlier snapshots remain preserved.

The first console attempt identifies 0.7.13. Native DNS lookup returns **0** in **15948 microseconds**, nonzero-address boolean **1**; current SDP is **1286 bytes / two candidates / three media sections / three BUNDLE entries**. After accepted submission and **33** pending HTTP 204 polls, keepalive at **29459 ms** succeeds with **HTTP 200 / 37 bytes**. The next SDP request returns **HTTP 200 / 2458 bytes**, with no public-code/errorDetails/keyword diagnostic emitted.

The application rejects the response as invalid, **X4_AUTH_E_RESPONSE / 0xFFFFF82A**, before applying remote SDP. No valid answer or ConnectionExchangeFailed category is established by this first attempt; schema/decoder validation needs investigation. The helper supplies no observed message mask. Remote deletion returns HTTP 200, with no RTP/video/audio; the owner reports the session error.

The second attempt uses the same installed version: DNS lookup **0 / 10484 microseconds**, current offer **1286 bytes / two candidates**, POST **202**, then **38** pending HTTP 204 polls. Keepalive dispatched at **29758 ms** succeeds with HTTP **200 / 37 bytes**. Normal SDP GET returns HTTP **200 / 243 bytes**, poll **39**, at **30872 ms**, with public code **ConnectionExchangeFailed**. Keyword output is **valid 1 / raw span 156 / decoded 154 / mask 0x00010400**: only `command` (bit 10) and `PerformSdpExchangeV1Command` (bit 16). This identifies the message's referenced operation, not its underlying cause or a successful answer. ICE/RTC/video/audio remain zero; cleanup returns HTTP 200. No game media is received in either attempt. Package/source/NOTICE remain unchanged. See [0.7.13 both attempts and limits](RELEASE_NOTES_0.7.13.md).

### 0.7.14 nested SDP diagnostics

The frozen original helper records a parsed nested exchange's type/length, direct SDP member/type/span, existing decoder success/length/nonempty result and a fixed header class. It records status/debugInfo member/type/span and messageType member/type/fixed class, with a securely cleared 32-byte temporary buffer. No debugInfo interpretation or remote text logging is added. The direct nonempty SDP acceptance rule, RTC/protocol/media decisions and cleanup remain unchanged.

Claude Opus 5.5's focused static review completed in **3 turns / 2 reads**, without a material finding in the visible helper scope. Independent full-function inspection confirms failed/duplicate JSON member lookups clear output spans; the review excerpt was incomplete for that function. Product 0.7.14 uses APP_VER 00.84.

The full VM package build succeeded: **8912896 bytes**, SHA-256 `891d1077c4d949534353023eafe27f0d4c7ec68839dba8fdb567fd6ac0602dc8`; VM, PC and PS4 retrieval hashes match. Application revision `285ed7edff1e26fe524a0ee8368b8cb133fce78c` supplies the authentication diagnostic separately from the immutable dependency/native-adapter archive: **83561616 bytes**, SHA-256 `55592c70d91bb259aed346273a2eb0cc23afb153e315538daea33dd2a8dc2c4d`. All **9367** manifest hashes matched locally, with identical PC/guest archive hashes and exact frozen NOTICE correspondence. Earlier snapshots remain preserved. **Console results remain pending**; no valid answer or game media is established. See [0.7.14 enum maps, build and source](RELEASE_NOTES_0.7.14.md).

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
