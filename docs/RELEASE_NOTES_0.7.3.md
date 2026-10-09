# XCloud4 0.7.3 — offer and native network diagnostics

The diagnostic dependencies and full application compiled and packaged successfully. The package transferred to the PS4 and its FTP retrieval matched the original hash. The owner's console attempt isolates the remaining failure to setting UDP socket flags during gathering. **No RTP was received and actual game video/audio remains unconfirmed.**

## Failure being investigated

Version 0.7.2 successfully initializes four RTC workers and the PSA/SCTP/DTLS/SRTP/ICE libraries, then creates a local peer connection, tracks and data channels. Calling `rtcSetLocalDescription(pc, "offer")` reports a runtime-error category and returns **-2**, with no SDP callback captured at that point. Callback delivery is queued, so its absence alone does not identify the internal failure stage. Game video/audio remains unconfirmed.

## Diagnostic changes

The reproducible port script and native adapters add fixed event labels and numeric results in the offer, ICE, certificate and native network paths:

| Events | Recorded stage/result |
|---|---|
| 20–26 | Offer preparation, local ICE description, media population, commit and gathering stages. |
| 30–33 | ICE agent creation, local-description result and gathering result. |
| 35 | Underlying ICE exception category. |
| 40–45 | Ephemeral certificate generation from key creation through DER result and completion. |
| 48–49 | UTC-conversion result and formatted-time length. |
| 50–54 | UDP socket, flag, bind and bind-address resolution errors/results. |
| 55–61 | Connection result, host-candidate count, resolver/poll thread results and poll-pipe errors. |
| 62–63 | Native interface-query result and result count. |

Affected libdatachannel sources are `src/peerconnection.cpp`, `src/impl/peerconnection.cpp`, `src/impl/icetransport.cpp`, `src/impl/certificate.cpp` and `src/impl/tls.cpp`; libjuice changes include `src/agent.c`, `src/conn_poll.c` and `src/udp.c`. `scripts/webrtc/prepare_port.py` reproduces the modifications with complete replacement checks. Native adapters map fixed events and add numeric interface-query results. The diagnostic logger preserves `errno`. No remote exception text, credentials, SDP, ICE values or certificate contents are logged. The four-worker policy remains in place.

Claude Opus 5.5 completed its read-only offer review in **9 turns**, successfully, without code changes. The review identified that generalized initialization errors and queued certificate/callback/gathering work can hide the underlying stage; it did not demonstrate a fix. These checkpoints distinguish those paths through numeric results.

## Actual console result

- Ephemeral certificate DER result: **362 bytes**, then certificate-ready status.
- ICE agent creation and local description: zero results; offer media count **3**, then offer committed.
- Gathering begins; poll-pipe and poll-thread results are zero. Bind-address resolution also returns zero.
- Setting UDP flags with `F_SETFL` fails with **errno 13 (`EACCES`)**.
- Connection result is **-1**, gathering result is **-2**, and `rtcSetLocalDescription` returns **-2**.
- Remote cleanup returns **HTTP 200**. No RTP is received.

This isolates a native UDP flag-setting failure after certificate/local-offer preparation. It does not establish why that native operation was denied or demonstrate a repair. Bind-address resolution success alone does not prove successful socket binding. The next repair stage investigates the native socket operation.

## Package evidence

- Filename: `XCloud4-0.7.3.pkg`.
- Application identifier: `XCLD00001`.
- Size: 8847360 bytes.
- SHA-256: `3290f527aa67d94f17eb345cba055b3f96718e67f147dec807b9a8caa4f5172a`.

Dependency compilation and the full application/package build completed successfully. Upload and FTP retrieval matched this hash. No automated tests were added or run. The console diagnostics establish certificate/local-offer preparation and the failed native socket operation; ICE connectivity and game media remain unconfirmed. The last completed release milestone remains 0.6.2 connection authorization, with local RTC initialization additionally confirmed in 0.7.2.

## Matching dependency source

`XCloud4-0.7.3-dependency-sources.tar.gz` captures the exact stable patched dependencies and pinned submodules, Opus, ABI overlays, configuration, native streaming adapters and retained licenses. Matching source must accompany any distributed package. Earlier 0.7.0/0.7.1/0.7.2 snapshots remain unchanged.

- Size: 83543499 bytes.
- SHA-256: `5c39a0ed1d50c39cc8bff12550f6e86ff7e6f13a55a4abab029e4ec086f857e9`.
- Manifest: 9367 captured source files; every recorded source hash matched the archive.

The PS4 UI stays Spanish, GitHub content stays English and the repository stays private. See [0.7.2 hardware evidence](RELEASE_NOTES_0.7.2.md), [WebRTC status](WEBRTC_PS4.md), [live media limits](MULTIMEDIA_EN_VIVO.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
