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

Pinned library candidates and external compilation status are documented in [WEBRTC_PS4.md](docs/WEBRTC_PS4.md). They were not included in the confirmed 0.6.2 package. Any later integration must add the corresponding license texts, source references and modification notices before distribution.
