# XCloud4

Experimental native Xbox Cloud Gaming client for a PS4 Fat running firmware **12.00** and **GoldHEN v2.4b18.7**, built with OpenOrbis.

## Current confirmed milestone: 0.6.2

The owner confirmed that connection authorization completes without an error. The PS4 kernel log confirms Microsoft token renewal **HTTP 200**, Passport **HTTP 200**, `/connect` **HTTP 202**, and automatic session deletion **HTTP 200**, with no cleanup error. This milestone is preserved as **`v0.6.2`**.

Version 0.6.2 uses, with the owner's authorization, the temporary public OAuth client identifier also used by GreenVita. The original XCloud4 registration is preserved. All authentication steps use the same selected client; refresh tokens are never reused across clients. The own registration worked for account access and the catalog but returned `invalid_scope` for Passport. See [connection authorization](docs/AUTORIZACION_CONEXION.md) and [the Passport investigation](docs/INVESTIGACION_PASSPORT.md).

**Actual game video, game audio and game input are not present in this confirmed version.** Work has resumed on WebRTC negotiation and native media reception. Successful connection authorization alone does not establish a media connection.

The PS4 interface remains **Spanish**. Repository documentation and GitHub content are **English**. The requested GitHub repository is **private**.

## Development package: 0.7.10 restricted service error name

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

The development package integrates pinned libdatachannel/libjuice, DTLS-SRTP, native DNS/entropy/thread adapters, bounded H.264 RTP reconstruction, Videodec2 output and Opus/AudioOut. Game controller messages remain pending. See [0.7.2 hardware evidence](docs/RELEASE_NOTES_0.7.2.md), [WebRTC status](docs/WEBRTC_PS4.md) and [live media limits](docs/MULTIMEDIA_EN_VIVO.md).

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
4. Implement a cloud session, WebRTC transport, game video/audio and controller messages — session authorization confirmed; actual streaming in progress.
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

The 0.7.10 development checkpoint produces `build/xcloud4.elf`, `build/eboot.bin` and `dist/XCloud4-0.7.10.pkg`. The preserved `v0.6.2` source produces its corresponding 0.6.2 package without the new WebRTC dependencies. Dependency builds and SDK binaries remain outside Git.

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
