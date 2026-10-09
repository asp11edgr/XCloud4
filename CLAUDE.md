# XCloud4

Native PS4 client project using OpenOrbis v0.5.4, C23 and Clang/LLD 21. Firmware 12.00 with GoldHEN v2.4b18.7. Package 0.1.0 installs but fails at startup: kernel logs identify missing /app0/sce_module/libSceFios2.prx, PRX_SCE_MODULE_LOAD_ERROR 0xa0020102.

- Source of truth: this Git repository. Compilation runs in the existing Lubuntu VM at `~/Projects/XCloud4`, with `~/.config/xcloud4/env.sh`.
- Build: `X4_RUNTIME_MODULES=/external/local/runtime make -j2 package`. Host libraries used by the legacy packaging tool are isolated outside the repository. Packaging must require both libSceFios2.prx and libc.prx as SELF modules. Package 0.1.1 passed the initial Fios2 lookup but then the console reported missing libc.prx; 0.1.2 addresses that dependency.
- Package 0.1.2 started successfully according to the PS4 kernel log; the owner confirmed CONTROL and PROYECTO appear and work. Full per-button/axis verification, Xbox auth, media decode and audio remain pending. Preserve this working baseline.
- Current milestone 0.2.1: own UI, VideoOut, DualShock 4 and a local media demo. Owner reported moving H264 video working in 0.2.0; Klog confirms first640x368 image and sample completion. Audio0.2.0 failed0x809B0001 with sceMbusAddHandleByUserId. Audio0.2.1 opens MAIN as SYSTEM0xFF and waits for consumed PCM before reuse, following official OpenOrbis audio-wav sample. Actual sound remains pending. Xbox auth, WebRTC and Opus remain pending.
- Use `docs/REFERENCIAS.md` for pinned upstream references. No code from GreenVita, Better xCloud or Moonlight PS4 has been incorporated yet.
- Videodec2 ABI types are adapted from GPL OpenOrbis PR213 (Backporter commit 9b9e82a2ec4e8cd3c34a086ca82339032cf69da0). Preserve source pin and attribution. Do not confuse research on numeric API constants with copying Moonlight implementation.
- Preserve source attribution and GPL-3.0-only licensing. Do not incorporate unlicensed source or proprietary Sony SDK binaries. The required libc/Fios2 modules are open auxiliary modules built from OpenOrbis v0.5.4 src/modules; preserve their attribution. Keep their compiled binaries outside Git.
- The owner requested Claude Opus 5.5. Use it explicitly for Claude Code review. Treat review hypotheses as unconfirmed until supported by actual artifacts or console logs.
- Do not add or run tests unless the owner explicitly requests them. Do not claim hardware behavior from a successful build.
- Keep credentials, tokens, private keys, SDK files and generated packages outside Git.
- GitHub publication is requested for the end of the workday, with a private repository. Do not publish earlier.
