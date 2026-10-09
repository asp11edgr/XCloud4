# XCloud4 0.7.16 — current RTC connection failure

Recorded on **2026-10-09** from the owner's console attempt and the preserved private capture. Target: PS4 firmware **12.00**, **GoldHEN v2.4b18.7**, OpenOrbis **v0.5.4**. Runtime source: `2f2bbf8444fc7c6bfb9496ca351b6ca0cb5cc33e`; product **0.7.16**, PS4 **APP_VER 00.86**.

## Summary

The application now stays open when the connection fails. Xbox provides a valid SDP answer and remote ICE data, but the RTC connection fails before any game video or audio is received. The session is deleted successfully. **Live game video, audio and game input remain unresolved.**

## Reproduction on the owner's console

1. Install `XCloud4-0.7.16.pkg`, replacing the previous XCloud4 application.
2. Authorize the Microsoft account in the Spanish account view.
3. Open the catalog and select a title marked `CON ACCESO`.
4. Wait for the connection result. The application reports a session error and remains open.

The private capture identifies the 0.7.16 startup banner. These steps describe the observed attempt; a single attempt does not establish stability for every session or failure path.

## Sanitized observed sequence

| Stage | Observed result |
|---|---|
| Local SDP offer | 1266 bytes; audio MID 0, video MID 1; no undefined receiving SSRC declarations |
| Xbox SDP response | HTTP 200; 1866 response bytes; 1504 decoded SDP bytes |
| Remote SDP | Valid answer applied |
| Local ICE submission | HTTP 202 |
| Remote ICE retrieval | HTTP 200; 449 response bytes |
| IPv6 filtering | Class 1, reason 12: valid hexadecimal IPv6 outside the Teredo prefix |
| RTC | Failed, then disconnected and closed |
| Application detail | `0xFFFFF824` |
| Game media | No video/audio received; zero RTP counters |
| Session cleanup | DELETE HTTP 200, no cleanup error |
| Application closure | Owner confirms no CE-34878-0 in this attempt; no timed-wait exception in the capture |

HTTP success during signaling does not prove that ICE, DTLS or the media connection succeeds. Candidate filtering reason 12 establishes why one IPv6 candidate was omitted, without proving that this omission caused the connection failure. The present capture does not isolate the failing RTC phase or show how many usable IPv4 candidates reached the ICE implementation.

## Corrected preceding crash

Two 0.7.15 attempts failed RTC and then terminated with `condition_variable timed_wait failed: Operation timed out`, producing CE-34878-0. The exact linked timed-wait method compared its return value against Linux-style **110**, while the native SDK defines **ETIMEDOUT as 60**.

Version 0.7.16 compiles the complete, unmodified LLVM 11 condition-variable source with native headers. Its object is selected before the existing libc++ archive, and the final ELF comparison is **60**. SDK archives remain unchanged. The owner's console result confirms the closure is corrected for the tested path. This crash fix does not establish the cause of the preceding RTC failure.

## Evidence and artifact integrity

- Package: `XCloud4-0.7.16.pkg`, **8912896 bytes**, SHA-256 `57f434bf98469b7ae38cff1739376fb958d77eb10e4151272c99f258a5fd9489`. VM, PC and PS4 FTP-retrieval hashes match.
- Exact dependency/native-adapter source: `XCloud4-0.7.16-dependency-sources.tar.gz`, **83566130 bytes**, SHA-256 `f274e91489dde87e948616411fbde8129b6091d0cbffae16b5ef774b5682f3b3`. All **9368** manifest source hashes verify; the inventory contains no forbidden generated/private paths.
- Preserved private capture extract: **282 selected lines**, SHA-256 `a4dcd805790c5bc59d208962d830f43d1162d09afc3664921728426752238848`. Only the sanitized sequence above is public; the raw capture is kept outside Git.
- Actual Claude Code selected **claude-opus-5-5** for the focused source review and reported no material issue in the reviewed runtime fix and diagnostics. The review did not establish hardware behavior. No automated tests were added or run.

See the [release notes](RELEASE_NOTES_0.7.16.md), [third-party notices](../THIRD_PARTY_NOTICES.md) and [development prerelease](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.16).

## Next investigation

Record distinct ICE, DTLS and SCTP states, bounded failure categories and remote-candidate submission counts. Separately check the native clock used for ICE timing. Any following correction must be tied to source/build evidence and a new console attempt before attributing the failure to a particular cause.
