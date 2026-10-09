# XCloud4 0.7.2 — bounded RTC worker initialization

This development checkpoint corrects the worker-pool initialization problem located by 0.7.1. The console capture confirms four workers and local RTC initialization now succeed. **Local SDP offer generation still fails before media; actual game video/audio remains unconfirmed.** The last completed streaming milestone is still 0.6.2 connection authorization.

## Observed cause and changes

The 0.7.1 console log requested **200112 workers**, then reported numeric **system error 1** before the pool-ready checkpoint. ABI inspection traced this count to the prebuilt SDK libc++ CPU-count probe using `sysconf(84)`: native FreeBSD selector 84 denotes `_SC_THREAD_CPUTIME`, with `_POSIX_THREAD_CPUTIME=200112L`, while native CPU-count selectors are 57/58. See the primary [selector definitions](https://github.com/freebsd/freebsd-src/blob/releng/9.0/include/unistd.h) and [sysconf implementation](https://github.com/freebsd/freebsd-src/blob/releng/9.0/lib/libc/gen/sysconf.c).

- Call `rtcSetThreadPoolSize(4)` before peer-connection initialization and stop on a negative result.
- Record actual created-worker counts and the count reached if worker creation fails.
- Preserve fixed initialization labels and numeric diagnostics without logging account credentials, SDP or ICE values.
- Retain the existing SDK thread-ID adapter and create a separate matching source snapshot.

Claude Opus 5.5 completed a read-only review in **29 turns**, without an error, and recommended the explicit worker limit before initialization. Hardware evidence now confirms that initialization correction; it has not yet demonstrated working native transport or playback.

## Actual console result

- `rtcSetThreadPoolSize(4)` returned zero; creation counters reached 1, 2, 3 and 4.
- Thread-pool, PSA, SCTP, DTLS, SRTP and ICE library initialization completed with zero status.
- Peer-connection creation returned handle 1; local track and data-channel creation succeeded.
- `rtcSetLocalDescription(pc, "offer")` reported a runtime-error category and returned **-2**, before the local SDP callback.

These results confirm local initialization and object creation. They do not establish ICE connectivity, a remote media connection or playback. The [0.7.3 diagnostic package](RELEASE_NOTES_0.7.3.md) adds bounded offer-generation, ICE-agent, certificate and native network checkpoints. Claude Opus 5.5 completed its read-only offer review in nine turns without changing code. SDP callback delivery is queued, so absence of that callback alone does not locate the internal failure stage.

## Build and package evidence

The full application compiled and packaged successfully. Its upload to the PS4 and FTP retrieval matched the original SHA-256. No automated tests were added or run. The PS4 UI remains Spanish; GitHub content is English and the repository is private. Title search and game-input messages remain pending.

- Filename: `XCloud4-0.7.2.pkg`.
- Application identifier: `XCLD00001`.
- Size: 8847360 bytes.
- SHA-256: `f2d8cf6e606435927f98ddcd2ae274fed795787468e2bbe8bc7d2bfb9dfbc66a`.

## Matching dependency source

`XCloud4-0.7.2-dependency-sources.tar.gz` contains the exact patched dependency trees and submodules, Opus, ABI overlays, port/build configuration, native streaming adapters and retained licenses. It includes the added `src/impl/threadpool.cpp` diagnostics. Its manifest and accompanying SHA-256 file record the captured bytes. Previous 0.7.0/0.7.1 snapshots remain unchanged. Matching source must accompany any distributed package.

- Size: 83536843 bytes.
- SHA-256: `3932e6593bf76eb26851c4db8ee0448d9b76e9f95a68163022e975558251c3c0`.
- Manifest: 9367 source files; every recorded source hash matched the archive.

See [WebRTC evidence](WEBRTC_PS4.md), [0.7.1 diagnostics](RELEASE_NOTES_0.7.1.md), [live media limits](MULTIMEDIA_EN_VIVO.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
