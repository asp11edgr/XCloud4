# XCloud4 0.7.17 — RTC fails after ICE completion and DTLS connection

Recorded on **2026-10-09** from the owner's console attempt. Product **0.7.17**, PS4 **APP_VER 00.87**, firmware **12.00**, **GoldHEN v2.4b18.7**. Runtime source: `587c8999d47dcab8b612a65d81ca3e28a8f4deb6`.

## Observed result

The owner reports a session error. The capture now confirms **ICE completed**, **DTLS connected**, and **SRTP key derivation completed**. The peer connection nevertheless enters failed state, then disconnects and closes without receiving any game video/audio. Session deletion succeeds. No timed-wait exception appears in this captured attempt.

The error remains **0xFFFFF824**, but the new diagnostics establish which connection stages occurred. This application detail is a general RTC failure result, not a precise SCTP or media error code.

## Sanitized sequence

| Stage | Observed result |
|---|---|
| Xbox SDP | HTTP 200; 1866 response bytes; 1504 decoded SDP bytes; answer applied |
| Local ICE submission | HTTP 202 |
| Remote ICE response | HTTP 200; 451 bytes; 3 items counted |
| Candidate delivery | 1 native-wrapper call, return 0; 1 new cached candidate |
| IPv6 filtering | Class 1, reason 12, outside the Teredo prefix |
| DTLS construction | Stages 0 and 1 |
| ICE state | 3, completed |
| DTLS state | 2, connected |
| Peer state | 4, failed |
| SRTP key derivation | Stages 0 and 1, begin and complete |
| Shutdown | DTLS 0 disconnected; peer 3 disconnected; ICE 6 closed; peer 5 closed |
| Game media | Zero RTP/video/audio counters |
| Cleanup | DELETE HTTP 200; no cleanup error |

The count of 3 received items does not mean 3 usable ICE routes: an item may be omitted or contain an empty end marker. A native-wrapper return of zero does not by itself prove internal ICE acceptance. Here the independent ICE completed-state event confirms that ICE progressed despite the omitted non-Teredo IPv6 candidate.

Log messages from concurrent callbacks can interleave. The table records the captured events; it does not establish a total order across all threads. No SCTP state event 87 or new ICE/DTLS exception-class event appears in this extract.

## Current localization and limits

In the pinned libdatachannel source, the DTLS-connected callback initializes SCTP when an application section is present. `PeerConnection::initSctpTransport()` can catch an exception, directly set the peer state to failed and rethrow without emitting event 87. The enclosing transport state callback catches standard exceptions, which permits the DTLS-SRTP path to continue to key derivation. This is a plausible explanation for the captured sequence, **not a confirmed root cause**.

The current capture does not distinguish a missing or incompatible application description, SCTP construction/socket options, or bind/connect failure. The next correction records bounded construction steps and numeric return/error values and checks the native SCTP address ABI. No arbitrary exception message, raw SDP or candidate value needs to be published.

## Preserved evidence

- Private bounded extract: **305 selected lines**, SHA-256 `e539a9defa827e4a0dde60ee2aef1a2d462af36737e7db0c20fc6138efe614c5`, stored outside Git.
- Package SHA-256: `cf7dd37c8ae677274fa834e8b671248abca9b0ccec715a71b3587c166e75f772`; VM, PC, console retrieval and GitHub asset digests match.
- Dependency/native source SHA-256: `b8eca2b0027dcb388558cf93dd613ec477c94d00a83c44baaae65c4e0d1e2618`; all 9368 manifest source hashes verify.
- [0.7.17 release notes](RELEASE_NOTES_0.7.17.md), [published prerelease](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.17), and [preceding 0.7.16 report](ERROR_REPORT_0.7.16.md).

The new diagnostics show real connection-stage progress. **Actual game video, audio and game input remain unresolved.** No automated tests were added or run.
