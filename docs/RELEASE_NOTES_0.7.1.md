# XCloud4 0.7.1 — local RTC initialization diagnostics

This development checkpoint investigates 0.7.0's local RTC opening error **-2**, observed before SDP, ICE or RTP on PS4 firmware 12.00 with GoldHEN v2.4b18.7. Xbox connection authorization and remote cleanup succeeded in that earlier attempt. **This package adds diagnostics; it does not yet fix the cause or establish live playback.**

## Changes

- Add fixed initialization-stage labels and numeric native/Mbed TLS errors in libdatachannel's C API, initialization and TLS paths.
- Add corresponding bounded diagnostics in the native adapters.
- Keep account credentials, SDP and ICE values out of logs.
- Preserve 0.7.0's dependency-source snapshot and create a separate matching snapshot for this checkpoint.

The PS4 UI remains Spanish; GitHub content is English and the repository is private. No title-search feature or game-input support is added.

## Evidence and limits

The diagnostic code compiled and the full application packaged successfully. The package transferred to the PS4 and its FTP retrieval matched the original hash. Claude Opus 5.5 completed a read-only review in 29 turns without an error and recommended bounding the worker pool before initialization.

The actual console log recorded thread-pool initialization requesting **200112 workers**, then numeric **system error 1** before the pool-ready checkpoint. ABI inspection traced the count to an incompatible `sysconf` CPU-count selector in the prebuilt SDK C++ runtime. SDP, ICE and media were not reached. Version [0.7.2](RELEASE_NOTES_0.7.2.md) applies an explicit worker limit before initialization. Native transport/media behavior and actual game video/audio remain unconfirmed. The last completed console milestone remains 0.6.2 connection authorization. No automated tests were added or run; no completed 0.7.1 milestone is claimed here.

## Package

- Filename: `XCloud4-0.7.1.pkg`.
- Application identifier: `XCLD00001`.
- Size: 8847360 bytes.
- SHA-256: `6769f0b3a93f87143a1d4511b7f0a3657e8a60cfe393c4a5704fd1c17fdefe7d`.

## Matching dependency source

`XCloud4-0.7.1-dependency-sources.tar.gz` captures the exact patched dependency trees, all pinned submodules, Opus, ABI overlays, configuration, native streaming adapters and original notices. This includes the three additional diagnostic vendor files. Publish matching source alongside any distributed package. Keep the 0.7.0 snapshot unchanged.

- Size: 83536524 bytes.
- SHA-256: `46501c565b2bf1782c8dbf971c13f474b32f4ccf76a86083f343207ecbdf9891`.
- Manifest: 9367 captured source files; every recorded source hash matched the archive.

See [WebRTC status](WEBRTC_PS4.md), [live media limits](MULTIMEDIA_EN_VIVO.md), [0.7.0 evidence](RELEASE_NOTES_0.7.0.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
