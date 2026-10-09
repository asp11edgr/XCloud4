# XCloud4

Experimental native Xbox Cloud Gaming client for a PS4 Fat running firmware **12.00** and **GoldHEN v2.4b18.7**, built with OpenOrbis.

## Current confirmed milestone: 0.6.2

The owner confirmed that connection authorization completes without an error. The PS4 kernel log confirms Microsoft token renewal **HTTP 200**, Passport **HTTP 200**, `/connect` **HTTP 202**, and automatic session deletion **HTTP 200**, with no cleanup error. This milestone is preserved as **`v0.6.2`**.

Version 0.6.2 uses, with the owner's authorization, the temporary public OAuth client identifier also used by GreenVita. The original XCloud4 registration is preserved. All authentication steps use the same selected client; refresh tokens are never reused across clients. The own registration worked for account access and the catalog but returned `invalid_scope` for Passport. See [connection authorization](docs/AUTORIZACION_CONEXION.md) and [the Passport investigation](docs/INVESTIGACION_PASSPORT.md).

**Actual game video, game audio and game input are not present in this confirmed version.** Work has resumed on WebRTC negotiation and native media reception. Successful connection authorization alone does not establish a media connection.

The PS4 interface remains **Spanish**. Repository documentation and GitHub content are **English**. The requested GitHub repository is **private**.

## Confirmed progress

| Version | Result on the owner's PS4 |
|---|---|
| 0.1.2 | Application startup, `CONTROL` and `PROYECTO` views work. Fios2 and libc packaging dependencies are included. |
| 0.2.2 | Local H.264 sample, stereo PCM tones and clean `OPTIONS` exit to the PS4 menu work. |
| 0.3.1 | Native HTTPS and Microsoft device-code authorization work with XCloud4's own registration. |
| 0.4.0 | Real Xbox catalog: 2733 titles received, 128 retained locally, 21 of those marked with access by Xbox, and 32 Microsoft Store names obtained. |
| 0.5.0 | Remote session creation, readiness and automatic deletion work. |
| 0.6.2 | Passport authorization, `/connect` acceptance and automatic deletion work with the temporary reference client. |

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

## Build

Use the existing Lubuntu VM in VirtualBox, after [preparing the environment](docs/PREPARACION.md):

```bash
cd "$HOME/Projects/XCloud4"
source "$HOME/.config/xcloud4/env.sh"
make -j2
export X4_RUNTIME_MODULES="$HOME/.local/share/xcloud4/runtime/sdk-v0.5.4"
make package
```

The confirmed 0.6.2 source produces `build/xcloud4.elf`, `build/eboot.bin` and `dist/XCloud4-0.6.2.pkg`. Later development versions may change the package name.

`X4_RUNTIME_MODULES` must point to an external directory containing `libSceFios2.prx` and `libc.prx` in SELF format. These are open auxiliary OpenOrbis modules, available in its distribution with corresponding source under `src/modules`. Their compiled binaries stay outside Git. Packaging stops if either is missing or supplied as an unconverted ELF. See [third-party notices](THIRD_PARTY_NOTICES.md).

The legacy OpenOrbis packager needs isolated OpenSSL 1.1 libraries. Prepare them once with `bash scripts/preparar-empaquetador.sh`; this does not install them system-wide. See [PS4 installation](docs/INSTALACION_PS4.md).

## Source layout

- `src/core`: entry point and lifecycle.
- `src/auth`: native HTTPS, Microsoft account, Xbox catalog, session preparation and authorization.
- `src/streaming`: WebRTC integration under development.
- `src/video`: VideoOut and the local H.264/Videodec2 sample.
- `src/input`: DualShock 4 reading and reconnection.
- `src/audio`: AudioOut PCM sample; live Opus reception is under development.
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
