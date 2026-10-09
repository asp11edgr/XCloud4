# Display and video

`display.c` allocates two 1920 × 1080 VideoOut buffers and waits for presentation before reusing them. This UI output worked on PS4 in version 0.1.2.

`h264_demo.c` adds a local 640 × 368 H.264 sample in version 0.2.0. It resolves Videodec2 when opening the sample, queries memory requirements, retains compressed data and converts NV12 frames to RGB for the UI. `videodec2_abi.h` adapts public OpenOrbis types, with attribution in [third-party notices](../../THIRD_PARTY_NOTICES.md).

The owner reported working video; the photo shows 217 / 240 frames, and Klog confirms the first frame and sample completion. Versions 0.2.1 and 0.2.2 preserve this video implementation.

The confirmed sample does not decode an Xbox session. WebRTC reception, frame loss handling, synchronization and 720p performance remain subsequent work. See [native media](../../docs/MULTIMEDIA.md).

The current development implementation, `live_h264.c`, accepts complete Annex B access units reconstructed from RTP, rotates persistent ONION inputs at a fixed decode pipeline depth of one, validates native NV12 output and converts it for display. The maximum configured size is 1280 × 720. Native live playback, decoder parameters and input-consumption behavior still require PS4 evidence; see [live media design](../../docs/MULTIMEDIA_EN_VIVO.md).
