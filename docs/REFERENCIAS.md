# Project references

Primary sources were reviewed on October 8, 2026. These identifiers pin the material reviewed even as upstream projects change.

| Project | Reviewed revision | Intended use | Observed license |
|---|---|---|---|
| [GreenVita](https://github.com/Day-OS/green-vita) | `ae2625d295b4fba005a769b1309fd70dcd6cb63f` | Authentication, Xbox sessions and WebRTC protocol | MPL-2.0 |
| [Better xCloud](https://github.com/redphx/better-xcloud) | `f8397043f6d2148d2345d508902a38c69cf1ee20` | Session preferences and region selection | MIT |
| [Moonlight PS4](https://github.com/JaimeJimenezG/Moonlight-ps4) | `61427a214d4e632ee246816a98ee4f2374844a73` | Research into PS4 H.264, VideoOut, audio and input APIs | No root LICENSE/COPYING found in the reviewed tree; permission to adapt its own code remains unresolved |

## GreenVita

The client is written in Rust for PS Vita. Reviewed files `src/api_xbox/auth.rs` and `src/api/streaming/rtc/session.rs` contain authentication/token exchanges and WebRTC session abstractions for media reception and input transmission. Vita memory, paths and decoders require PS4-specific implementation.

XCloud4 uses the protocol as a reference and original native C implementation. GreenVita's Rust source and credential persistence have not been copied. The public OAuth client identifier also used by GreenVita is temporarily selected in 0.6.2 with the owner's authorization, following an observed own-client Passport refusal. It is public metadata, not a secret or an ownership claim. See [registration](REGISTRO_MICROSOFT.md) and [the investigation](INVESTIGACION_PASSPORT.md).

Any future source adaptation must retain applicable attribution and license notices.

## Better xCloud

A TypeScript project for the web client. `src/utils/region.ts` reads the preferred region and, where relevant, the service-returned default. `src/modules/stream/stream-settings-utils.ts` provides session-setting research. XCloud4 needs its own models and UI; the script does not provide a PS4-native browser integration.

## Moonlight PS4

Reviewed `src/video/decoder_orbis.c`, `src/audio/audio_orbis.c`, README, PLAN and console documentation. Its README reports validation on firmware 9.00, which does not establish compatibility with this project's firmware 12.00.

The project demonstrates research paths for `libSceVideodec2`, presentation and `sceAudioOut`. Its transport is Moonlight/Sunshine; Xbox Cloud Gaming requires its corresponding session protocol and WebRTC transport. Published 9.00 kernel patches are not incorporated into the 12.00 client. Its implementation has not been copied.

## Incorporated source through the confirmed baseline

These three applications are protocol/API research references. Version 0.1.0 adapts public OpenOrbis VideoOut/Pad initialization and adds an original UI, renderer and icon under GPL-3.0-only. Videodec2 declarations and the public AudioOut example are attributed in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Native network contracts researched for 0.3.x

OpenOrbis v0.5.4 Http/Net/Ssl declarations and public examples were consulted to complete missing signatures. Transport and OAuth code are original XCloud4 implementations.

- Integer returns for `sceSslTerm(ctx)` and `sceNetPoolDestroy(id)`: [flatz HTTP source](https://github.com/flatz/ps4_remote_pkg_installer/blob/master/http.c) and [network source](https://github.com/flatz/ps4_remote_pkg_installer/blob/master/net.c).
- `sceHttpSetRecvTimeOut(id, usec)`, `sceHttpSetAutoRedirect(id, enabled)` and TLS options: [shadPS4 HTTP implementation](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/network/http.cpp) and [declarations](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/network/http.h).

Declarations inform ABI work; they do not establish console execution. Actual results are recorded in [JORNADA.md](JORNADA.md).

For 0.3.1, the native `/<sandbox>/common/lib/<name>.sprx` path and `sceKernelGetFsSandboxRandomWord` were researched in [flatz's module source](https://github.com/flatz/ps4_remote_pkg_installer/blob/master/module.c). The function is declared in the local SDK's `orbis/libkernel.h`. XCloud4 implements a bounded module loader with export validation and diagnostics that omit complete paths; the reference implementation was not copied.

## Xbox credentials and catalog for 0.4.0

GreenVita at the pinned revision above supplies protocol references:

- [RPS, XSTS and cloud offerings](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/auth.rs).
- [Regional `/v2/titles` and request headers](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/api.rs).
- [Title fields and service-reported access](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/game_catalog.rs).
- [Public localized Store names](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/catalog.rs).

The catalog milestone used the own XCloud4 registration and original C code. Actual credential/catalog success is supported by the owner's photo and console logs, not inferred from upstream source.

## Session, authorization and transport

GreenVita `src/api_xbox/stream.rs` and session creation in `api.rs` describe `/v5/sessions/cloud/play`, readiness, `/connect`, SDP, ICE, keepalive and DELETE. The native implementation preserves these protocol references. Session preparation/deletion are confirmed in 0.5.0; Passport and `/connect` with the temporary profile are confirmed in 0.6.2.

The Passport comparison additionally reviewed [Stratix MicrosoftAuthService](https://github.com/nafields/stratix/blob/59d804185192f0c0f618836aace9229b77ce48e4/Packages/XCloudAPI/Sources/XCloudAPI/Auth/MicrosoftAuthService.swift), revision `59d804185192f0c0f618836aace9229b77ce48e4`. This is protocol research, not incorporated Swift source.

Transport library candidates, pins and local SDK evidence are in [WEBRTC_PS4.md](WEBRTC_PS4.md). Title search stays deferred in [MEJORAS_FUTURAS.md](MEJORAS_FUTURAS.md).

## AJ GeForce NOW PS4

The owner supplied [AJfiles/AJ-GeforceNow-PS4](https://github.com/AJfiles/AJ-GeforceNow-PS4/tree/87568ca50a89e6f8dd65fe005d21521c27e65c26), pinned at `87568ca50a89e6f8dd65fe005d21521c27e65c26`. The [scoped reference review](REFERENCE_REVIEW_AJ_GFN_PS4.md) compares the active libpeer backend, native audio/video and provider-specific negotiation. The author's reported PS4 Pro/9.00 performance was not reproduced on the owner's PS4. No application implementation was copied into XCloud4 in this review.
