# XCloud4 0.7.18 client comparison and proposed receive fix

The current blocking failure is a native worker crash in `SctpTransport::doRecv()` after successful Xbox signaling, ICE completion and DTLS connection. The smallest proposed repair moves its 65,536-byte receive buffer from automatic storage to an owned heap allocation. The worker's actual stack bounds remain unmeasured, and live game video/audio remains unconfirmed.

This comparison covers discoverable public Xbox clients, their protocol libraries and relevant native console implementations. It identifies the specific inspected paths and source revisions; it does not claim every existing client or reproduce another author's playback results. All proposals below were unimplemented when this 0.7.18 review was completed.

Subsequent development: [0.7.19](RELEASE_NOTES_0.7.19.md) implements the target-only owned receive buffer, with a verified smaller linked frame. Its [first console attempt](ERROR_REPORT_0.7.19.md) reaches live game audio and video, with slow video, input and external-close limitations. The native stack default remains unmeasured. The remaining stack-diagnostic and worker-sizing proposals are still unimplemented; the observations below preserve the 0.7.18 investigation.

## Current console evidence

The [0.7.18 error report](ERROR_REPORT_0.7.18.md) preserves the owner's reported **CE-34878-8**, the matching package/source hashes and the captured **SIGSEGV**. SCTP construction and bind return success; nonblocking connect returns **-1 / EINPROGRESS (36)** and its start path completes. No SCTP association, channel opening or game-media callback is confirmed before the fatal signal.

The matched ELF reserves **66,072 bytes** in `doRecv`, whose source contains a **65,536-byte automatic buffer**. The faulting call's return-address push writes at **RSP minus 8** into an unmapped page. This strongly supports exhaustion of available worker stack space. Native stack size, mapped bounds and possible preceding corruption remain unresolved. The fault occurs before entry into the mutex implementation, so it does not establish a bad mutex object.

The compiled source is the immutable 0.7.18 corresponding-source archive, SHA-256 `8caec278e2f0bd8e337a347c016ad3bc5ad1a352749b8a05e2697baeb8fb6acc`, associated with source checkpoint `9c573a74346b97a143f4e575301b3bdcf54c8cb5`. Upstream libdatachannel is [0.24.6 at bdc5ff28](https://github.com/paullouisageneau/libdatachannel/tree/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15), with XCloud4's target patches described in [the notices](../THIRD_PARTY_NOTICES.md). Upstream alone does not describe the patched binary.

## Public client implementations inspected

| Client or library and immutable revision | Inspected implementation and relevance | License evidence and limits |
|---|---|---|
| [GreenVita ae2625d2](https://github.com/Day-OS/green-vita/tree/ae2625d295b4fba005a769b1309fd70dcd6cb63f) | Rust Xbox authentication, session, RTC worker and protocol paths; a useful Xbox reference on a different console. | MPL-2.0. Native Vita decoding/thread behavior is platform specific. |
| [PSBox ef22c57a](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/tree/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e) | [Peer setup](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/stream/stream_session.cpp#L388) and Xbox signaling use libdatachannel; audio is added at line 448 before video at 463. | File SPDX is GPL-3.0-or-later; root LICENSE is GPL-3.0. A source review is not a reproduced PS5 or PS4 result. |
| [GreenOvercast 2e97ea66](https://github.com/Producdevity/GreenOvercast/tree/2e97ea66a7e917c3f158452877dd324aa852554c) | ARM Linux Zig application, libdatachannel C API, pinned vendor and selected patch/build paths inspected. | MPL-2.0; dependencies have their own notices. |
| [Theseus 5fc7dddd](https://github.com/MrMilenko/Theseus/tree/5fc7ddddd03b4b5a7e6233769ad6a8fe46009965) | Conditional desktop Xbox streaming path and pinned libdatachannel inspected. Its original Xbox dashboard is a separate target. | Root GPL-3.0 text and third-party notices. Desktop link conditions matter. |
| [LunarNX 949858e2](https://github.com/thinkzhou/LunarNX/tree/949858e2ff1f1b4aac56e5a1bfd86b8adab87fc3) | Switch Xbox wrappers, network helper, candidate processing and build selection inspected. Uses libpeer; PlayStation uses Chiaki. | Original code excluding `src/ps` is MIT; PlayStation integration/combined distribution has AGPL terms. |
| [XStreaming 9e6a2125](https://github.com/Geocld/XStreaming/tree/9e6a21251c379c861cc48f105530895dab9180c3) | React Native WebRTC route plus separate Rust Nano RTC route; selected backend is not established for every user. | Current root LICENSE is **AGPL-3.0**, even though an older downstream attribution calls it MIT. |
| [XStreamingDesktop 0e4b6612](https://github.com/Geocld/XStreamingDesktop/tree/0e4b661254cdacba2afcecad5fd9ff3068f91beb) | Electron renderer imports `xstreaming-player` 0.3.4 and performs offer/answer/ICE exchange. | MIT. Application and player are one integrated path. |
| [Geocld xstreaming-player 8e178aef](https://github.com/Geocld/xstreaming-player/tree/8e178aef067ec31625311865e5ee51a6924823e2) | Actual package family used by XStreamingDesktop; browser RTCPeerConnection and media components inspected. | Package metadata declares MIT; no root LICENSE found. npm release-to-commit identity was not verified. |
| [unknownskl xbox-xcloud-player a6511a64](https://github.com/unknownskl/xbox-xcloud-player/tree/a6511a645736f4988f77323284c6f0f36bf93aef) | Browser peer creation, offer/answer, Xbox stream and channel helpers inspected. | Package metadata declares MIT; no root LICENSE found. Browser SCTP internals are outside this repository. |
| [AJ GeForce NOW PS4 87568ca5](https://github.com/AJfiles/AJ-GeforceNow-PS4/tree/87568ca50a89e6f8dd65fe005d21521c27e65c26) | Native PS4 decoder/audio and active libpeer path reviewed in the [scoped comparison](REFERENCE_REVIEW_AJ_GFN_PS4.md). Its active native build selects internal SCTP. | Root LICENSE is MIT; dependencies retain separate terms. Provider signaling is NVIDIA-specific. No application implementation was copied. |
| [Moonlight PS4 61427a21](https://github.com/JaimeJimenezG/Moonlight-ps4/tree/61427a214d4e632ee246816a98ee4f2374844a73) | Native decoder/audio API research; transport is Moonlight/Sunshine rather than Xbox WebRTC. | No root LICENSE/COPYING found for its own code at this pin; adaptation permission remains unresolved. |

### Additional references with a narrower review

The existing [reference notes](REFERENCIAS.md) and auth investigation cover [Greenlight 07a3f01c](https://github.com/unknownskl/greenlight/tree/07a3f01cba527c9c8287f90ef326c5e89c64687b), [Better xCloud f8397043](https://github.com/redphx/better-xcloud/tree/f8397043f6d2148d2345d508902a38c69cf1ee20), [OpenXbox xcloud-rs cd94b22f](https://github.com/OpenXbox/xcloud-rs/tree/cd94b22f611bece91c5963c5647588b06adc705f), [CloudNow 6627c042](https://github.com/owenselles/CloudNow/tree/6627c0423474ebe6647e86a2db953ce55dabaae1) and [Stratix 59d80418](https://github.com/nafields/stratix/tree/59d804185192f0c0f618836aace9229b77ce48e4). Their reviewed auth, Passport, API-schema, region or session-setting paths do not establish native SCTP worker-stack behavior. This comparison does not present them as complete source audits.

### Documentation-only discoveries and exclusions

| Discovery | Why it cannot establish this native receive fix |
|---|---|
| [libnxbox faee3c4b](https://github.com/ursusworks/libnxbox/tree/faee3c4b50b40cc701b938b7b0c20578e8b1904a) | The seven-entry public tree has README, license and images. [README line 149](https://github.com/ursusworks/libnxbox/blob/faee3c4b50b40cc701b938b7b0c20578e8b1904a/README.md#L149) says implementation publication is planned; no transport source is available at this pin. |
| [Cirrus](https://github.com/verdjs/Cirrus), [RayxCloud](https://github.com/RayLabsStudio/RayxCloud) | README identities and CloudNow/Stratix lineage found; implementation pins and active paths were not independently inspected here. |
| [CloudX full project](https://github.com/Pgeniebox/cloudxsolution-android-tv) and [Android example](https://github.com/Pgeniebox/xbox-cloud-Solution-Android-tv) | Public README describes Java/native handover from WebView. Code and backend selection need pinned inspection before substantive reuse. |
| [Kite](https://github.com/DRHATL95/kite) | README identifies Xbox Home/Remote Play of the user's own console. Its cloud console discovery does not make it an Xbox cloud-game client. Implementation was not compared here. |
| XStreamingDesktop forks, downstream packaging, generic WebRTC remote desktops | Duplicate lineages or different providers; discovery does not supply independent Xbox/PS4 execution evidence. |

Searches included Xbox cloud clients for PS5, Vita, Switch, Linux and mobile; GreenOvercast, Theseus, XStreaming and xbox-xcloud-player; native C++/Rust/Go clients; and libdatachannel/OpenOrbis stack and receive-buffer terms. Public GitHub repository/commit/tree endpoints and pinned source files supplied implementation evidence. Private/proprietary implementations and unpublished sources cannot support a source comparison.

## Receive buffers and worker stacks

| Implementation | Verified source behavior | Consequence for XCloud4 |
|---|---|---|
| XCloud4 0.7.18 | Patched `doRecv` retains an automatic 65,536-byte buffer; linked frame reserves 66,072 bytes. SDK header and ELF show NULL attributes at worker creation. | Earliest observed fatal boundary. Native available stack remains unknown. |
| GreenOvercast vendor [c6696d15](https://github.com/paullouisageneau/libdatachannel/blob/c6696d157b5612df2a741d9a03b192b47ab6cefb/src/impl/sctptransport.cpp#L477) | Same automatic buffer. [Thread pool](https://github.com/paullouisageneau/libdatachannel/blob/c6696d157b5612df2a741d9a03b192b47ab6cefb/src/impl/threadpool.cpp#L28) uses standard threads without explicit stack size; its selected patch changes RTCP receiving only. | Linux playback assertions do not establish PS4 stack capacity. |
| Theseus vendor [d5d31c7d](https://github.com/paullouisageneau/libdatachannel/blob/d5d31c7d794345d7aa536c074a9e17cbec95089d/src/impl/sctptransport.cpp#L479) | Same automatic buffer and standard thread pool in the conditional desktop path. | Its original Xbox executable stack linker flag does not size desktop RTC workers. |
| GreenVita RTC crate | Cargo lock selects `rtc` 0.20.0-rc.2. Exact crate source [34d06c3b](https://github.com/webrtc-rs/rtc/blob/34d06c3b6f5d153f15342b2dee9f391da1fe1148/rtc/src/peer_connection/transport/sctp/mod.rs#L38) stores and resizes a heap-backed Vec; [handler](https://github.com/webrtc-rs/rtc/blob/34d06c3b6f5d153f15342b2dee9f391da1fe1148/rtc/src/peer_connection/handler/sctp.rs#L150) reads into it. | Supports a heap-storage strategy, using a different Rust RTC engine. Its [worker](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api/streaming/rtc/worker.rs#L76) does not explicitly set stack size. |
| LunarNX network helper | [Header](https://github.com/thinkzhou/LunarNX/blob/949858e2ff1f1b4aac56e5a1bfd86b8adab87fc3/src/platform/network_worker.h#L8) defaults to 2 MiB; [implementation](https://github.com/thinkzhou/LunarNX/blob/949858e2ff1f1b4aac56e5a1bfd86b8adab87fc3/src/platform/network_worker.cpp#L106) initializes, sets and destroys pthread attributes. | A concrete explicit-stack approach on Switch, not PS4 ABI proof or a measurement of all its transport threads. |
| orbis-compat [6922a7b6](https://github.com/orbis-ports/orbis-compat/tree/6922a7b6f4291a28edd239f1a4db50ed7aa00359) | [Thread header](https://github.com/orbis-ports/orbis-compat/blob/6922a7b6f4291a28edd239f1a4db50ed7aa00359/include/orbis_thread.h#L5) reports its author's 65,536-byte PS4 worker default; [wrapper](https://github.com/orbis-ports/orbis-compat/blob/6922a7b6f4291a28edd239f1a4db50ed7aa00359/src/orbis_thread.cpp#L351) adjusts stacks, and `stackOfSelf` queries attributes. | Relevant external evidence, not a measurement on our console. Whole-wrapper adoption would introduce global thread policy. Selected thread files are MIT; repository is archived/superseded. |

LunarNX's [Switch build](https://github.com/thinkzhou/LunarNX/blob/949858e2ff1f1b4aac56e5a1bfd86b8adab87fc3/Makefile.switch#L10) selects legacy libpeer with `CONFIG_USE_USRSCTP=1`; its desktop CMake selects zero. AJ's active PS4 native path selects zero. Those are different SCTP implementations/builds and cannot be described as an underlying libdatachannel path.

## Protocol and media comparison

| Phase | XCloud4 current implementation and observation | Reference use and next boundary |
|---|---|---|
| Microsoft and Xbox authorization | Device authorization, Xbox token, XSTS and cloud credentials succeeded in the latest attempt. | GreenVita/Stratix/CloudNow inform API research. Earlier `invalid_scope` belongs to a different failure. |
| Provisioning and queues | Session polling, pending/provisioned states, cancellation and deadlines are in `src/auth/xbox_live.c`. This attempt reached ready. | Compare queue timing separately when Xbox reports a queue; this crash occurred after readiness. |
| Session and keepalive | Creation, `/connect`, keepalive and deletion are implemented. Current connection authorization succeeds. | GreenVita and browser clients provide protocol references; keepalive does not establish media reception. |
| SDP | Valid 1,504-byte answer was applied. Current offer has audio MID 0, video MID 1, receiving tracks and the SSRC guard. | Track order and provider compatibility are source specific. A new offer rewrite would not address the observed stack write. |
| ICE and native networking | Native socket, entropy, clock and candidate adapters; ICE completed. | Candidate counts do not prove every route usable. Keep validated Teredo/monotonic-clock work while addressing the current crash. |
| DTLS and SRTP | DTLS connected and SRTP key derivation completed. | Authentication/key setup and received RTP are separate milestones. |
| SCTP and Xbox channels | Corrected native address ABI, successful bind and accepted nonblocking start, followed by fatal receive task. Four channel definitions exist. | Association, channel opens and handshake acknowledgments are the next observations after stability is restored. |
| Video | Original bounded RTP ring, reordering, H.264 depacketization, SPS/PPS/IDR handling and native decoder integration exist. | GreenVita/PSBox/native PS4 clients inform platform API choices. First received packet and decoded frame remain unconfirmed. |
| Audio | Opus decoder, bounded PCM queue and native AudioOut worker exist; earlier synthetic audio worked. | Live audio requires actual received/decoded Opus and audible console output. Synthetic success does not establish live playback. |
| Input | Local DualShock input and Xbox channel/client metadata exist; actual remote gamepad-frame transmission remains a later step. | Inspect GreenVita/player report layouts before integration; an input channel definition is not working remote control. |
| Closure | Callback gating, channel/peer deletion, worker cancellation and native output draining exist. The crash produced no normal final summary. | Validate stable close/reopen after transport survives; do not infer final packet counters from the missing cleanup. |

## Ranked proposed changes

### 1 Move the receive scratch buffer to owned heap storage

Confine the first change to the target-patched `SctpTransport::doRecv`. Keep its mutex acquisition and pending-receive decrement unchanged. Move the existing buffer-size constant above the loop, inside the existing try block, and allocate one RAII-owned 65,536-byte array per invocation. Use a pointer from that owner in the unchanged receive and vector-insert expressions.

An uninitialized `unique_ptr<byte[]>` allocation preserves the old array's initialization behavior; `make_unique` would zero-fill the array. Place allocation after the pending decrement so allocation failure cannot leave the scheduling count stuck. Existing vector inserts copy the bytes, so no scratch pointer needs to outlive the invocation. Preserve notification/message fragmentation, end-of-record handling and the exception boundary.

Expected effect: remove the identified 64 KiB automatic array from the frame. Costs: one heap allocation per receive task; allocation failure and deeper callee stack usage remain possible. A member-owned reusable buffer is a broader alternative if later evidence shows allocation cost matters. Shared global or per-iteration allocation adds unnecessary lifetime/concurrency or allocation work.

Record the actual new linked frame size after implementation, then observe the console receive path. No repaired executable or console outcome exists yet.

### 2 Measure native worker stack bounds

After verifying the native attribute ABI, capture numeric return codes, stack size, guard size and available margin once per worker or in its caller. Place this before entry into the old large-frame function; diagnostics inside `doRecv` cannot run before its prologue reserves that frame. Destroy any initialized attribute and avoid raw addresses or arbitrary dependency log text.

This supplies the currently missing native bound. It does not itself repair the crash and must account for attribute-pool and ABI behavior.

### 3 Configure explicit RTC worker stack sizes when justified

Use measured requirements to size the four RTC workers with a verified native wrapper that preserves ownership, join and cleanup semantics. `std::thread` has no stack-attribute argument. A process-wide pthread interposer affects more threads and carries greater ABI/lifetime scope than the receive-buffer change. LunarNX and orbis-compat supply specific strategies, not a verified PS4 implementation to transplant wholesale.

### Subsequent streaming observations

Once receive stability is established, record SCTP association, Xbox channel opening/handshake, first RTP callbacks, first decoded video and audible audio as separate milestones. The source review does not establish the exact ordering of future provider/media events. Localize any subsequent failure using its new evidence. Input encoding and catalog search remain later work.

## Source reuse requirements

The proposed receive change modifies XCloud4's existing MPL-2.0 dependency and must retain its notices and an exact matching source snapshot with any distributed package. The comparison adds no external implementation. A future adaptation must use the selected file's actual license and backend, including XStreaming's current AGPL terms and LunarNX's separate Xbox/PlayStation components. Moonlight's unresolved own-code license and package-only license declarations remain limits on source reuse.

## Research and review provenance

Actual Antigravity Opus 4.6 Thinking performed public searches and source retrievals, but its quota stopped the main investigation before a complete report. A fresh focused Claude Code review completed successfully using **claude-opus-5-5**, eight turns and seven scoped file reads. It ranked heap allocation first, stack diagnostics second and an explicit stack bridge third, and identified the size-constant/placement details above.

Actual Antigravity then completed the follow-up using **Gemini 3.1 Pro Low**, the available Pro option in its signed-in model menu. Its direct external activity consisted of two pinned source retrievals (PSBox and GreenVita session files) and four searches covering CloudNow, PSBox, RayxCloud and Cirrus. It read the SCTP receive source and supplied comparison material, corrected the heap-owner/raw-pointer proposal, and retained the unknown native stack-size limit. Its broader comparison partly relied on the separately inspected sources supplied by the coordinating reviewers; those sources are identified above and are not attributed to independent Antigravity retrievals.

The coordinating reviews checked the current native/media source, immutable dependency source, public client pins, selected license files and linked crash evidence. They corrected unsupported model claims about native stack defaults, guaranteed freedom from stack exhaustion, active backends and license scope. No Antigravity inspection of authentication/media implementation files was observed in the follow-up trace, so its report's claim of such inspection is not used as evidence. This consolidated report includes only conclusions supported by the inspected sources and recorded attempt. No runtime changes, new package, tests or console deployment were performed during this comparison.
