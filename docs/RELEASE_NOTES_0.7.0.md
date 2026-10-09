# XCloud4 0.7.0 — native streaming development package

This development package was built and tried on the owner's PS4 firmware 12.00 with GoldHEN v2.4b18.7. The first attempt failed during local RTC creation before media negotiation. **Actual live video/audio has not yet been confirmed.** The last completed console milestone remains 0.6.2 connection authorization.

## Changes

- Integrate pinned libdatachannel, libjuice, usrsctp, libsrtp and Mbed TLS for native WebRTC negotiation, ICE, DTLS-SRTP and data channels.
- Add native entropy, DNS/interface discovery, thread, timing and BSD socket ABI adapters without modifying the installed SDK.
- Request H.264 baseline 720p/30 FPS at 5 Mbps and stereo Opus at 48 kHz; use negotiated video/audio payload types.
- Add bounded RTP queues/reordering, H.264 single-NAL/STAP-A/FU-A reconstruction, native Videodec2 output validation and Opus/AudioOut integration.
- Retain output/input buffers until native consumption or successful teardown, preserve ownership on cleanup failure and request replacement keyframes after video loss.
- Include exact dependency licenses and prepare matching patched source for distribution.

The PS4 UI remains Spanish. GitHub content is English and the repository remains private. Title search remains future work.

## Evidence available

The pinned dependency archives and full application compiled, linked, converted to SELF and packaged successfully in Lubuntu. Static ELF inspection recorded 104 imports, 34 constructor entries and an 8-byte TLS section. The package transferred to the PS4 and its FTP retrieval matched the original SHA-256.

These results establish build structure and file integrity. No automated tests were added or run.

### First console result

Xbox `/connect` returned HTTP 202 and the session reached Provisioned with HTTP 200. Native entropy and NetCtl initialization returned zero. Local RTC opening then returned **-2 before SDP, ICE or RTP**. Remote cleanup returned HTTP 200. The local RTC creation path needs repair; this error is distinct from the earlier Passport `invalid_scope` refusal. Actual transport/media behavior and shutdown during live streaming remain unconfirmed.

## Package

- Filename: `XCloud4-0.7.0.pkg`.
- Application identifier: `XCLD00001`.
- Size: 8847360 bytes.
- SHA-256: `80860ced1f18ef794fc628736864d5b4c894c9bea344759d4bbfcfdc84c6cf19`.

## Matching dependency source

- Filename: `XCloud4-0.7.0-dependency-sources.tar.gz`.
- Size: 83532544 bytes.
- SHA-256: `0e48073b43a94e68c109cf76ca08fdfb7c3e967f0908f2f72e732fb843624d68`.
- Manifest: 9367 captured source files; every recorded source hash matched the archive.

The source archive includes exact patched pinned dependency trees/submodules, Opus 1.5.2, ABI overlays, port/build configuration, native streaming adapters and original notices. Git metadata, build caches, SDK binaries and local credentials/runtime logs are excluded. Original public upstream test fixtures remain included. Publish this source alongside the matching package when distributing it; regenerate the snapshot if captured source changes.

## Current limits

Native playback remains unconfirmed. The port currently supports IPv4. Game input/control messages, reconnection, sustained playback, latency behavior and timestamp-based audio/video synchronization remain incomplete or unverified. The existing local H.264/PCM sample is distinct from live game reception.

No confirmed `v0.7.0` tag or published 0.7 release is claimed by this document. See [WebRTC status](WEBRTC_PS4.md), [live media design](MULTIMEDIA_EN_VIVO.md), [installation](INSTALACION_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
