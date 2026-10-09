# XCloud4 development instructions

Native PS4 client using OpenOrbis v0.5.4, C23 and Clang/LLD 21. Target hardware: firmware 12.00 with GoldHEN v2.4b18.7.

- Source of truth: this Git repository. Compile in the existing Lubuntu VM at `~/Projects/XCloud4`, loading `~/.config/xcloud4/env.sh`.
- Build: `X4_RUNTIME_MODULES=/external/local/runtime make -j2 package`. Legacy packaging libraries are isolated outside the repository. Require `libSceFios2.prx` and `libc.prx` as SELF modules. Versions 0.1.0 and 0.1.1 failed before `main` because these dependencies were missing; 0.1.2 fixed startup.
- Preserve confirmed milestones: `v0.1.2` (startup and views), `v0.2.2` (local H.264/PCM and clean exit), `v0.3.1` (Microsoft), `v0.4.0` (catalog), `v0.5.0` (session preparation/deletion), and `v0.6.2` (connection authorization/deletion).
- Version 0.6.2's Passport HTTP 200, `/connect` HTTP 202 and DELETE HTTP 200 are confirmed by the owner and Klog. It uses the authorized temporary public client also used by GreenVita. The original registration is preserved. Never mix refresh tokens between clients.
- Actual game media and input were not received in the confirmed 0.6.2 milestone. The owner has authorized continued work toward real video and audio. Do not describe authorization acceptance as a connected WebRTC stream.
- Preserve the confirmed native media and exit paths. PCM uses SYSTEM (`0xFF`) for MAIN and waits before buffer reuse. `OPTIONS` resolves SystemServiceLoadExec, releases resources and requests `"exit"`; do not return through `_exit`, which triggered SIGSYS in 0.2.1. Keep the ten-second exit timeout and recoverable UI on rejection.
- Use `docs/REFERENCIAS.md` for pinned references. GreenVita's Rust and Moonlight PS4's implementations have not been copied into the confirmed baseline.
- Videodec2 ABI types are adapted from OpenOrbis PR #213, Backporter commit `9b9e82a2ec4e8cd3c34a086ca82339032cf69da0`, under GPL-3.0. Preserve attribution and distinguish API research from source copying.
- Preserve applicable notices and GPL-3.0-only licensing. Do not incorporate unlicensed code or proprietary Sony SDK binaries. Required libc/Fios2 auxiliary modules have source in OpenOrbis v0.5.4 `src/modules`; keep their compiled binaries outside Git.
- The owner requested **Claude Opus 5.5**. Select `claude-opus-5-5` explicitly for Claude Code work. Review hypotheses remain unconfirmed until supported by source, generated artifacts or console evidence.
- Do not add or run tests unless the owner explicitly requests them. Do not claim hardware behavior from successful compilation.
- Keep credentials, device codes, tokens, private keys, SDK files, generated packages and private logs outside Git.
- The owner authorized **private GitHub publication**. Repository documentation and GitHub content must be English; **the PS4 UI stays Spanish**.
- Title search is recorded in `docs/MEJORAS_FUTURAS.md` for a later update. Do not implement it in the current streaming stage.
