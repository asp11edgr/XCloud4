# Native local media sample

## Scope and confirmed results

`IMAGEN Y SONIDO` exercises the initial native playback path without connecting to Xbox. In 0.2.0, the owner reported working video and the photo shows 217 / 240 frames. Klog records the first H.264 frame, 640 × 368 with pitch 640, and sample completion. That version's audio failed with `0x809B0001`.

Version 0.2.1 fixed local sound and the owner confirmed it. Version 0.2.2 fixed application exit and the owner confirmed clean return to the PS4 menu. These results establish the local sample, not 720p performance or WebRTC reception.

## Media and memory layout

- Synthetic H.264 Annex B clip, baseline 3.0, 640 × 368, 30 FPS, eight seconds, no B-frames, one AUD delimiter per frame. `scripts/generar-muestra.sh` creates it with FFmpeg/libx264 on the PC.
- Included clip SHA-256: `fc920497b038586e8611b65accc0cd9bb81eb0f768332e37e95462119c473a31`. The file is included so builds do not require FFmpeg. Regenerate with `bash scripts/generar-muestra.sh`; `X4_FFMPEG` selects a local executable.
- The entire compressed file stays in ONION direct memory while the decoder exists; compressed units in flight are not overwritten. Frames use four GARLIC direct-memory buffers and one RGB copy for the UI. Decode depth is one.
- Videodec2 and AudioOut are resolved on entering the sample. Sysmodule requests dependencies; the loader checks existing modules and names with/without extensions. These libraries are not added to startup dependencies.
- Expected decoder output is linear NV12, 640 × 368. Dimensions, pitch, size and buffer ownership are validated before conversion. Unexpected output becomes an error without reading foreign memory.
- Soft triangular 440 Hz tones: stereo S16 PCM, 48 kHz, blocks of 1024 samples. Left/right alternate with pauses; a native thread feeds AudioOut. These tones are not a video soundtrack and do not establish cloud audiovisual synchronization.
- X restarts the sample; Square retains mute state even across repeats; Circle returns; `OPTIONS` exits. Closure joins audio and removes the decoder before freeing memory. If decoder or queue removal fails, buffers are retained until the process ends.

## Sources and limits

Videodec2 types adapt OpenOrbis PR #213, pinned in [third-party notices](../THIRD_PARTY_NOTICES.md). OpenOrbis v0.5.4 supplies AudioOut, kernel, Pad and VideoOut. Playback/conversion code is original. Moonlight PS4 code and kernel patches were not incorporated.

The display format is `A8B8G8R8_SRGB`, as in the confirmed UI baseline. ABI sizes are checked during compilation. Compilation does not establish decoder, parameter or sound behavior on the console.

## Static review

Claude Code Pro 2.1.292 reviewed the source using `claude-opus-5-5` (firstParty provider). Adjustments covered module-list capacity, extension-bearing names, accepted decoder buffers, addresses after mapping failure and audio state. Entry-count capacity was compared against [flatz's primary module source](https://github.com/flatz/ps4_remote_pkg_installer/blob/master/module.c).

Proposals without a demonstrated defect were not applied: swapping red/blue (the display uses ABGR), removing packaging files (GP4 explicitly lists files), or removing the first presentation (each flip waits for completion). Profile/level were not changed to zero because that was an unconfirmed hardware hypothesis. Console evidence guides later parameter changes.

No automated tests were added or run. The earlier `v0.1.2` milestone is retained.

## Audio fix in 0.2.1

Actual 0.2.0 Klog contains `[AudioOut] Error:sceMbusAddHandleByUserId 0x20000007` on each sample start. The UI/photo show `0x809B0001`; MAIN was given the UserService user. OpenOrbis v0.5.4's public example opens MAIN with `ORBIS_USER_SERVICE_USER_ID_SYSTEM` (`0xFF`). Version 0.2.1 adopts that association and removes the audio user lookup.

It also waits via `sceAudioOutOutput(handle, NULL)` before reusing PCM and at completion, and logs init/open/thread results. [Primary source](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/v0.5.4/samples/audio-wav/audio-wav/main.cpp). Microsoft credentials are unrelated. Video remains unchanged from 0.2.0.

The owner later confirmed that 0.2.1 sound works. Klog shows `AudioOutOpen MAIN SYSTEM(0xff) -> 0x20000007`, thread creation and `PCM terminado, muestras=384000 error=0x00000000` for two plays. This confirms local PCM only; Opus, WebRTC and synchronization remain subsequent work.

## Exit fix in 0.2.2

`OPTIONS` caused `CE-34878-0` in 0.2.0 and 0.2.1. For 0.2.1 process 84, Klog shows SIGSYS on the main thread: RIP `libkernel + 0x28bc`, last branch from `0x409330` to `libkernel + 0x28b0`. The exact version's ELF has `_exit@plt` at offset `0x9330`, and C startup returns from `main` through `exit` → `_Exit` → `_exit`. This locates the failure at final process exit.

Version 0.2.2 resolves `sceSystemServiceLoadExec` when exit is requested, releases resources and requests `"exit"`. It waits for the system to remove the application instead of returning from `main`. Preparation failure leaves the UI open; a rejected request after closure reopens it for another attempt. Shutdown stages are logged. Audio/video remain as in 0.2.1.

Removal waiting is limited to ten seconds; timeout reopens the UI with an error. A pause before reopening avoids a fast presentation-failure/exit-refusal loop. Claude Opus 5.5 reviewed the change and informed those measures. The last-branch comparison used the exact 0.2.1 executable; static review alone did not establish LoadExec behavior.

Function contract: OpenOrbis v0.5.4 `include/orbis/SystemService.h`. Usage research, without copying its implementation: [PS4CheatsManager `terminate`](https://github.com/bucanero/PS4CheatsManager/blob/main/source/main.c), requesting LoadExec with `"exit"`. No automated tests were run.

**Subsequent confirmed result:** the owner reported correct 0.2.2 exit. Klog shows resources released, exit requested and `Kill for LoadExec(0x5a) => 0`, with no new SIGSYS for that closure. `v0.2.2` is preserved as the local media and clean-exit baseline.
