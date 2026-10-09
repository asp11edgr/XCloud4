# XCloud4 0.7.14 — nested SDP diagnostics

This checkpoint adds numeric diagnostics for a parsed nested exchange before the existing direct SDP validation. Source scope, focused read-only review, full package build, matching source and transfer integrity are confirmed. **Console evidence is pending. No valid remote answer, corrected negotiation or game video/audio is established.**

## Previous confirmed evidence

The owner made two attempts with installed 0.7.13. The first returns **HTTP 200 / 2458 bytes**, then fails direct response validation with **X4_AUTH_E_RESPONSE / 0xFFFFF82A** before remote SDP is applied, without public-code/keyword output. The second returns a logical refusal with **ConnectionExchangeFailed**, mask **0x00010400** identifying only `command` and `PerformSdpExchangeV1Command`. That message references an operation without establishing its underlying cause. Cleanup succeeds and neither attempt receives game media. See [both 0.7.13 captures](RELEASE_NOTES_0.7.13.md).

## Frozen diagnostic scope

The original `signal_remote_sdp` helper in `src/auth/xbox_live.c` is invoked when the outer exchange has been decoded and parsed (`signal_exchange` result **1**). A parsed exchange is not itself proof of a valid SDP answer. The helper reports only:

- Nested exchange JSON type and byte length.
- Direct `sdp` member result, JSON type and raw span length.
- Success of the existing decoder, decoded length, nonempty result and fixed header class.
- `status` and `debugInfo` member results, JSON types and raw span lengths.
- `messageType` member result, JSON type and fixed class.

The original rule remains: an object must contain a direct, uniquely found SDP string accepted by the existing decoder, and decoded SDP must be nonempty. Header/messageType classifications are diagnostic; they do not add acceptance conditions or normalize response content. The same remote-description call and failure behavior remain.

### Header classes

| Class | Observed prefix category |
|---|---|
| 0 | Not decoded |
| 1 | Decoded empty string |
| 2 | `v=0` followed by CR/LF |
| 3 | `v=0` followed by LF |
| 4 | `v=0` followed by literal backslash-r/backslash-n characters |
| 5 | Other prefix beginning with `v=0` |
| 6 | Other decoded prefix |

### messageType classes

| Class | Category |
|---|---|
| 0 | Absent or null |
| 1 | Unknown or invalid |
| 2 | Fixed token `offer` |
| 3 | Fixed token `answer` |

A **32-byte temporary buffer** is used for messageType classification and securely cleared. `debugInfo` is not decoded or interpreted. No raw field text, SDP, arbitrary debug information, response body or credentials are logged. Existing service-code/keyword diagnostics, protocol/RTC/media decisions, heartbeat cadence and cleanup remain unchanged.

## Metadata and evidence status

Product version is **0.7.14**, with PS4 package **APP_VER 00.84**. Claude Opus 5.5's focused static review completed successfully in **3 turns / 2 reads**, without a material finding in the visible helper scope. Its excerpt did not show the complete `x4_json_member` implementation; independent inspection of that full function confirmed failed/duplicate member lookups clear the output span. No runtime result is inferred from the review.

## Package and matching source

The full VM package build succeeded:

- Package: `XCloud4-0.7.14.pkg`, identifier `XCLD00001`.
- Size: **8912896 bytes**.
- SHA-256: `891d1077c4d949534353023eafe27f0d4c7ec68839dba8fdb567fd6ac0602dc8`.
- VM, PC and PS4 FTP retrieval hashes match.

Application checkpoint `285ed7edff1e26fe524a0ee8368b8cb133fce78c` supplies the authentication diagnostic separately from the corresponding dependency/native-adapter archive, closed from the frozen mirror:

- Filename: `XCloud4-0.7.14-dependency-sources.tar.gz`.
- Size: **83561616 bytes**.
- SHA-256: `55592c70d91bb259aed346273a2eb0cc23afb153e315538daea33dd2a8dc2c4d`.
- All **9367** manifest source hashes verified locally, covering **150843700 source bytes**; the PC archive hash matches the guest archive.
- The archived third-party notices match the frozen repository file exactly.

Pinned vendors, RTC adapters and configuration are unchanged, with matching notices. Generated artifacts, Git metadata, local credentials and logs are excluded; original public upstream fixtures remain. Both application source and the dependency/native-adapter archive, with retained licenses, accompany the package checkpoint. Every earlier snapshot stays immutable.

## Console status

The verified package is available on the PS4. No 0.7.14 console result is recorded yet. Build/review/integrity evidence does not establish nested-field classifications, an accepted remote answer or received game media.

No automated tests were added or run. The PS4 UI stays Spanish, repository documentation stays English and GitHub stays private. See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
