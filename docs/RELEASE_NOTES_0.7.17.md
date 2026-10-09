# XCloud4 0.7.17 — native ICE clock and connection-phase diagnostics

Product **0.7.17**, PS4 **APP_VER 00.87**, application identifier **XCLD00001**. Retains the 0.7.15 SSRC omission, audio-first offer and validated Teredo derivation, and the 0.7.16 native C++ timed-wait replacement. **Game video/audio remains unconfirmed.** The latest console result is the [0.7.16 RTC failure](ERROR_REPORT_0.7.16.md).

## Concrete local clock incompatibility

The pinned libjuice `timestamp.c` selects `CLOCK_BOOTTIME` when that constant exists. The installed native headers declare this ID as **7**. Inspection of the actual preceding build's `timestamp.c.o` confirms that it passes **7** to `clock_gettime`. XCloud4's `rtc_clock.c` translates supported clock IDs but rejects 7 with `EINVAL`; libjuice then returns timestamp zero. ICE timers require a progressing clock.

For `X4_OPENORBIS` only, the checked port patch now selects **CLOCK_MONOTONIC (1)**, translated by the existing adapter to native **4**. Other targets retain upstream clock selection. This avoids claiming a global BOOTTIME-to-MONOTONIC equivalence. A clock failure emits only the return value and errno, once per process, preserving errno afterward.

This establishes a local timing incompatibility. It does **not** establish the cause of the preceding console failure: that capture did not identify which ICE/DTLS/SCTP phase failed, and a stuck timestamp would not itself explain a normally elapsed ICE timer. New build and console evidence are required.

## Bounded numeric diagnostics

- Register an ICE-state callback with the existing guarded callback lifetime and unregister it during cleanup.
- Identify libjuice failure sites: UDP poll error, fatal receive, lost connectivity or expired connectivity timer. At failed state, count candidate pairs, remote candidates and STUN entries.
- Record DTLS construction/state/exception category, numeric system errors, SRTP creation failure and key-derivation stage, and SCTP state. No new exception messages or packet contents are printed.
- Count remote candidate items, duplicates, native-wrapper submissions and actual cache additions per response. Numeric origin distinguishes unchanged non-IPv6 candidates from fixed-port and decoded-port Teredo derivations. The wrapper may reject an overlong item before reaching the C API; the new fields are named `wrapper_called` and `wrapper_result` accordingly. A C API return of zero confirms submission to that API; it does not establish that ICE internally accepted a usable route or connected. Duplicates are summarized per response without an individual log line for every repeated item.

Candidate validation, original MID, cache bounds/deduplication, IPv6 policy, signaling envelopes, codecs, queue behavior, resource cleanup and the Spanish UI remain in their existing paths. Diagnostics retain no addresses, ports, MID strings, SDP, fingerprint, account data or tokens.

## Diagnostic event map

| Event | Numeric meaning |
|---|---|
| 74 / 75 | First failed timestamp call: result / errno |
| 76 / 77 | ICE failure site / site-specific numeric detail |
| 78 / 79 | DTLS initialization exception class / system error code |
| 80 | DTLS transport state |
| 81 / 82 | DTLS receive exception class / system error code |
| 83 / 84 | SRTP inbound / outbound creation error |
| 85 | SRTP key derivation: 0 begin, 1 complete |
| 86 | DTLS construction: 0 begin, 1 constructed before insertion |
| 87 | SCTP transport state |
| 88 / 89 / 90 | Failed ICE candidate-pair / remote-candidate / STUN-entry counts |

ICE failure-site values are 1 poll error, 2 fatal receive, 3 lost connectivity and 4 connectivity timer expiry. Exception class is 2 system error, 3 runtime error, 4 another standard exception. The logged transport integers follow the exact pinned provider enums; lifecycle events are not a media-success result.

C API ICE states are 0 new, 1 checking, 2 connected, 3 completed, 4 failed, 5 disconnected, 6 closed. Native DTLS/SCTP transport states are 0 disconnected, 1 connecting, 2 connected, 3 completed, 4 failed; these integers differ from the C API peer-connection enum. For fatal receive, the pinned UDP helper returns `-sockerrno`, so event 77's `-ret` records the positive socket error code.

## Preparation status

Actual Claude Code selected **claude-opus-5-5** and completed the focused source review successfully in **7 turns**, reporting no material correctness issue in the inspected source. The initial review did not include the six pinned upstream files, so it did not verify their insertion anchors; independent inspection counted all **24** anchors exactly once in the prior VM source. Applying the checked preparation script and rebuilding the modified dependencies succeeded.

The reviewer identified two diagnostic clarifications: native-wrapper labels should not claim a C API call when the wrapper rejects before it, and duplicate items should be summarized to reduce log volume. Both are incorporated. Its initial suggestion that receive detail might be constant was checked against the actual pinned helper, which returns `-sockerrno`; the existing `-ret` is therefore preserved. The notice paragraph is located with the libjuice/libdatachannel modifications.

Inspection of the rebuilt `timestamp.c.o` shows **1** passed to `clock_gettime`, replacing **7**. The complete app compiled, linked, converted to SELF and packaged. The final ELF's `current_timestamp` at relative **0x1088c0** passes **1** to the existing `clock_gettime` adapter. The native condition-variable replacement remains selected, with the original archive member unselected. Earlier snapshots remain immutable. No automated tests are added or run; console behavior remains pending.

The narrow actual Claude Code follow-up completed successfully in **2 turns**, confirming the receive-return semantics and corrected wrapper labels/counters without a material issue in the inspected section. It did not inspect the relocated notice or prove hardware behavior.

## Package and source integrity

- `XCloud4-0.7.17.pkg`: **8912896 bytes**, SHA-256 `cf7dd37c8ae677274fa834e8b671248abca9b0ccec715a71b3587c166e75f772`.
- VM, PC and PS4 FTP-retrieval package hashes match. The package is copied for the owner to install; the 0.7.16 console result remains the latest tested checkpoint.
- `XCloud4-0.7.17-dependency-sources.tar.gz`: **83573065 bytes**, SHA-256 `b8eca2b0027dcb388558cf93dd613ec477c94d00a83c44baaae65c4e0d1e2618`.
- All **9368** manifest source hashes verify locally, covering **150864610 source bytes**, with no forbidden generated/private paths. Exact modified libjuice/libdatachannel trees, native adapters, preparation scripts and retained licenses are included. Application authentication code is supplied by the corresponding Git revision.

An initial, unpublished pre-follow-up package/source snapshot is preserved privately in the build guest under separate `initial-review` names. The release assets above contain the final wrapper diagnostic labels and relocated notice; earlier published checkpoints remain unchanged.

## Console status

**Awaiting the owner's 0.7.17 installation and connection attempt.** Correct clock selection, successful compilation and matching artifact hashes do not establish ICE connectivity, game media or controller transmission. The [0.7.16 error report](ERROR_REPORT_0.7.16.md) remains the current observed failure report until new console evidence is captured.
