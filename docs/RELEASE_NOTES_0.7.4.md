# XCloud4 0.7.4 — direct native nonblocking request

The dependency port and full application compiled and packaged successfully; package transfer/retrieval integrity is confirmed. The console attempt also rejects direct `FIONBIO` with **errno 13 (`EACCES`)**. **The proposed correction does not resolve the socket failure. No RTP was received and live game video/audio remains unconfirmed.**

## Evidence and proposed correction

Version 0.7.3 completes certificate/local-offer preparation, then fails during gathering at UDP `F_SETFL` with **errno 13 (`EACCES`)**. Connection/gathering return errors, remote cleanup succeeds with HTTP 200, and no RTP is received.

Version 0.7.4 restores libjuice's upstream direct nonblocking socket approach:

- Define `FIONBIO` through the existing SDK BSD encoding `_IOW('f', 126, int)`, yielding **`0x8004667e`** with a four-byte integer argument.
- Use that ioctl for UDP/TCP nonblocking setup and report numeric event **64**, zero on success or the actual socket errno on failure.
- Close the socket and report a failed setup when the ioctl is rejected.
- Retain pipe `F_SETFL` handling, the four-worker RTC limit, numeric diagnostics and credential privacy.

The modified libjuice files are `src/socket.h`, `src/udp.c` and `src/tcp.c`; the port script reproduces those changes and the native diagnostic adapter maps event 64. Original MPL-2.0 notices remain intact.

## Basis and limits of the diagnosis

The [FreeBSD 9 `filio.h`](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/sys/sys/filio.h) and [ioctl encoding](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/sys/sys/ioccom.h) define the BSD request. Its [descriptor implementation](https://raw.githubusercontent.com/freebsd/freebsd-src/releng/9.0/sys/kern/kern_descrip.c) shows `F_SETFL` requesting nonblocking mode and asynchronous mode through separate ioctls. This supports the narrower direct nonblocking request.

**There is no confirmation that the additional `FIOASYNC` request caused the original PS4 denial.** Existing `fcntl` constants were verified; the logs now show both denied `F_SETFL` in 0.7.3 and denied direct `FIONBIO` in 0.7.4. Their exact native internal cause remains unconfirmed. Native network nonblocking APIs and descriptor/API compatibility are the next investigation stage.

## Actual console result

- Certificate DER result: **359 bytes**; local offer media count **3** and offer commit succeed.
- Direct `FIONBIO` numeric event 64 reports **errno 13 (`EACCES`)**.
- Connection returns **-1**, gathering **-2**, and the local-description request **-2**.
- Remote cleanup returns **HTTP 200**; no RTP is received.

The owner and console capture confirm the failed attempt. Static request/import correctness and the earlier read-only review did not establish permission for the native socket operation.

Claude Opus 5.5 completed a read-only socket review in **5 turns**, successfully, without identifying a defect within that review. This does not prove runtime behavior. Static inspection of the ELF/converted OELF confirms the `0x8004667e` request, a pointer to a four-byte integer set to one, and the native `_ioctl` import mapping. These are binary/ABI observations, not a successful console call.

## Package evidence

- Filename: `XCloud4-0.7.4.pkg`.
- Application identifier: `XCLD00001`.
- Size: 8847360 bytes.
- SHA-256: `c36c1f0ad36827d8f102e893535446e9da0387d95f5cdc80e9ed6cf08f669955`.

Dependency/application compilation and packaging completed successfully. The package uploaded to the PS4 and its FTP retrieval matched this hash. No automated tests were added or run. The 0.7.3 installer was removed after the new transfer was verified; the console installer directory then contained only 0.7.4.

## Matching dependency source

`XCloud4-0.7.4-dependency-sources.tar.gz` captures the final patched dependencies and submodules, Opus, ABI overlays, port/build configuration, native streaming adapters and retained licenses. It includes libjuice's restored UDP/TCP ioctl setup and exact diagnostic adapter/patch script. Prior snapshots remain unchanged. Matching source must accompany any distributed package.

- Size: 83543344 bytes.
- SHA-256: `6bf845b53bdeab0898b33d3d1e07876661c24324f7a4ad6a06c4bfa18d311c31`.
- Manifest: 9367 source files; every recorded source hash matched the archive.

The PS4 UI remains Spanish, GitHub content remains English and the repository remains private.

See [0.7.3 console evidence](RELEASE_NOTES_0.7.3.md), [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
