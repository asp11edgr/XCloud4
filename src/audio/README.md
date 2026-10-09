# Audio

`demo_audio.c` generates soft S16 PCM tones at 48 kHz, alternating left and right for eight seconds. A native thread feeds AudioOut in blocks of 1024 samples; mute state persists when the sample repeats.

Version 0.2.0 failed with `0x809B0001` when associating the user with the output. Version 0.2.1 opens MAIN with SYSTEM (`0xFF`), following the public OpenOrbis example, and waits before reusing PCM data. The owner confirmed sound; Klog records 384000 samples without an error. Version 0.2.2 preserves this implementation.

Live Opus decoding, WebRTC audio reception and audiovisual synchronization are subsequent work. See [native media](../../docs/MULTIMEDIA.md).

The current development implementation, `live_audio.c`, decodes queued Opus RTP with an external static Opus 1.5.2 library and sends stereo PCM to native AudioOut. Submitted PCM stays context-owned until consumption/closure succeeds. This live path has not yet been confirmed on PS4; see [live media design](../../docs/MULTIMEDIA_EN_VIVO.md) and [third-party notices](../../THIRD_PARTY_NOTICES.md).
