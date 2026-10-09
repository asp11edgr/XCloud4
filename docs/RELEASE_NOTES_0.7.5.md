# XCloud4 0.7.5 — native SO_NBIO adapter

The dependency port and full application compiled and packaged successfully. **The owner's console confirms native nonblocking setup, candidate gathering and local SDP offer submission. Xbox did not return a remote SDP answer before keepalive returned HTTP 410. No live game video/audio was received.**

## Evidence and changes

The 0.7.3 console capture rejects UDP `F_SETFL`, and 0.7.4 also rejects direct `FIONBIO`, both with errno 13 (`EACCES`). Certificate/local-offer preparation and remote cleanup succeed; no RTP is received. The exact internal cause of those denials remains unconfirmed.

Version 0.7.5 adds an original bounded adapter for the existing POSIX descriptor:

- Set PS4 socket option **`SO_NBIO=0x1200`** through POSIX `setsockopt`, using `SOL_SOCKET=0xffff` and a four-byte integer set to one.
- Query that option through `getsockopt` and require an enabled result and returned size of four bytes.
- Record only numeric set/get errno, returned mode and size in events **65–68**.
- Fail socket setup when setting/querying the option fails or the returned state/size is invalid.

libjuice UDP/TCP call the original adapter; the four-worker RTC policy and existing pipe handling remain in place. Native diagnostics preserve `errno` and omit credentials, SDP and ICE values.

## API research and attribution

Sony's [PlayStation WebKit additions](https://github.com/WebKit/WebKit/blob/23af623a3a7e6b6458b1f51cf2af9c2fe77b30e8/Source/WTF/wtf/playstation/UniStdExtrasPlayStation.cpp), pinned at `23af623a3a7e6b6458b1f51cf2af9c2fe77b30e8`, use POSIX socket options for nonblocking mode. [WoWPS socket control](https://github.com/01cedric/WoWPS/blob/7649815066dd4d008107c691cbde758b8a29d8bf/include/platform/ps4/socket_control.hpp), pinned at `7649815066dd4d008107c691cbde758b8a29d8bf`, supplies the PS4 option constants and describes an OpenOrbis descriptor/nonblocking limitation.

XCloud4's adapter is original; implementations from those references were not copied. WoWPS's comment is supporting interface research, while XCloud4's actual EACCES evidence comes from its own console logs. No SDK implementation or precise native cause is claimed from that comment. Libjuice modifications retain their MPL-2.0 notices.

## Review and remaining evidence

Claude Opus 5.5 completed a read-only review in **5 turns**, successfully, with four grep inspections and no concrete defect identified within that review. It examined argument width/lifetime, UDP/TCP use and errno handling. No automated tests were added or run.

Static ELF/OELF inspection confirms the setter uses `SOL_SOCKET=0xffff`, `SO_NBIO=0x1200`, a pointer to integer one and length four. The getter uses the same descriptor, an initialized result and four-byte size, followed by error/size/enabled checks. Native `setsockopt` and `getsockopt` resolve to libkernel imports. These static checks are separate from the actual console evidence below.

## Actual console result

The set/query calls both report errno **0**; the returned mode is **256 (`0x100`)** with size **4**, and the adapter accepts that enabled result. Socket nonblocking setup, connection creation and interface query report zero; one interface and one host candidate are recorded. Gathering, offer readiness and `rtcSetLocalDescription` all return zero. The prior nonblocking operation failure is resolved in this capture.

Xbox accepts `POST /sdp` with **HTTP 202**, but repeated answer requests return **HTTP 204** without a remote SDP answer. Keepalive subsequently returns **HTTP 410**, and the application closes the session; remote deletion returns **HTTP 200**. The final state records SDP HTTP 204 and zero completed ICE, video and audio. The owner reports a new error view. The log does not establish why Xbox stopped maintaining the session, and no RTP or live game media was received.

## Package evidence

The full application/package build completed successfully in Lubuntu:

- Filename: `XCloud4-0.7.5.pkg`.
- Application identifier: `XCLD00001`.
- Size: 8847360 bytes.
- SHA-256: `077a7c2a04e8caad38cc57eb955a200c339c67116eb6bb4ef881cd4ec0eb97fb`.

The VM/PC package and PS4 FTP retrieval match that hash. After retiring the previous installer, the console installer directory contains only 0.7.5. Integrity verification does not establish media playback.

## Matching dependency source

`XCloud4-0.7.5-dependency-sources.tar.gz` captures the exact final patched dependencies and submodules, Opus, ABI overlays, configuration, original native adapters and retained licenses. It includes the SO_NBIO adapter and libjuice UDP/TCP use, with no copied implementation from the API references. Matching source must accompany any distributed package. The 0.7.4 and earlier snapshots remain unchanged.

- Archive size: **83543722 bytes**.
- SHA-256: `8efc459212d3bc99304442e3f98f1c970af3907fb225600933493001eb9d841b`.
- Manifest: **9367 source files**, all recorded hashes verified; the PC copy matches the guest archive.
- Generated build/cache/Git paths and local credentials/logs are excluded; original public upstream fixtures are retained.

The PS4 UI stays Spanish, GitHub content stays English and the repository stays private. See [0.7.4 evidence](RELEASE_NOTES_0.7.4.md), [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
