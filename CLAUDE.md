# XCloud4

Native PS4 client project using OpenOrbis v0.5.4, C23 and Clang/LLD 21. The owner confirmed firmware 12.00 with GoldHEN; console execution is still pending.

- Source of truth: this Git repository. Compilation runs in the existing Lubuntu VM at `~/Projects/XCloud4`, with `~/.config/xcloud4/env.sh`.
- Build: `make -j2 package`. Host libraries used by the legacy packaging tool are isolated outside the repository.
- Current milestone: own UI, VideoOut and DualShock 4 input. Xbox auth, WebRTC, video decode and audio are pending.
- Use `docs/REFERENCIAS.md` for pinned upstream references. No code from GreenVita, Better xCloud or Moonlight PS4 has been incorporated yet.
- Preserve attribution and GPL-3.0-only licensing. Do not incorporate unlicensed source or proprietary SDK binaries.
- Do not add or run tests unless the owner explicitly requests them. Do not claim hardware behavior from a successful build.
- Keep credentials, tokens, private keys, SDK files and generated packages outside Git.
- GitHub publication is requested for the end of the workday, with a private repository. Do not publish earlier.
