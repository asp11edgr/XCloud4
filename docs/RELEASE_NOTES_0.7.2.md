# XCloud4 0.7.2 — bounded RTC worker initialization

This development checkpoint addresses the worker-pool initialization problem located by 0.7.1. **The owner's 0.7.2 console result is pending; actual game video/audio remains unconfirmed.** The last completed console milestone is still 0.6.2 connection authorization.

## Observed cause and changes

The 0.7.1 console log requested **200112 workers**, then reported numeric **system error 1** before the pool-ready checkpoint. ABI inspection traced this count to the prebuilt SDK libc++ CPU-count probe using `sysconf(84)`: native FreeBSD selector 84 denotes `_SC_THREAD_CPUTIME`, with `_POSIX_THREAD_CPUTIME=200112L`, while native CPU-count selectors are 57/58.

- Call `rtcSetThreadPoolSize(4)` before peer-connection initialization and stop on a negative result.
- Record actual created-worker counts and the count reached if worker creation fails.
- Preserve fixed initialization labels and numeric diagnostics without logging account credentials, SDP or ICE values.
- Retain native process-ID syscall 20 and create a separate matching source snapshot.

Claude Opus 5.5 completed a read-only review in **29 turns**, without an error, and recommended the explicit worker limit before initialization. This is a targeted correction based on the observed initialization failure; it has not yet demonstrated working native transport or playback.

## Build and package evidence

The full application compiled and packaged successfully. Its upload to the PS4 and FTP retrieval matched the original SHA-256. No automated tests were added or run. The PS4 UI remains Spanish; GitHub content is English and the repository is private. Title search and game-input messages remain pending.

- Filename: `XCloud4-0.7.2.pkg`.
- Application identifier: `XCLD00001`.
- Size: 8847360 bytes.
- SHA-256: `f2d8cf6e606435927f98ddcd2ae274fed795787468e2bbe8bc7d2bfb9dfbc66a`.

## Matching dependency source

`XCloud4-0.7.2-dependency-sources.tar.gz` contains the exact patched dependency trees and submodules, Opus, ABI overlays, port/build configuration, native streaming adapters and retained licenses. It includes the added `src/impl/threadpool.cpp` diagnostics. Its manifest and accompanying SHA-256 file record the captured bytes. Previous 0.7.0/0.7.1 snapshots remain unchanged. Matching source must accompany any distributed package.

See [WebRTC evidence](WEBRTC_PS4.md), [0.7.1 diagnostics](RELEASE_NOTES_0.7.1.md), [live media limits](MULTIMEDIA_EN_VIVO.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
