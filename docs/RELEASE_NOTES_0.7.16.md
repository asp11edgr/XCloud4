# XCloud4 0.7.16 — native C++ timed wait and ICE diagnostics

Product **0.7.16**, PS4 **APP_VER 00.86**, application identifier **XCLD00001**. This checkpoint retains the SSRC omission, audio-first track order and Teredo support of 0.7.15. **Game video/audio remains unconfirmed.**

## Observed previous failure

Two 0.7.15 console attempts apply valid Xbox SDP answers, receive remote ICE, fail RTC and delete the session successfully. Each then terminates with an uncaught C++ `condition_variable timed_wait failed: Operation timed out` exception and CE-34878-0. The stack maps through `condition_variable::__do_timed_wait`, `ThreadPool::dequeue`, `ThreadPool::run` and the C++ thread proxy to terminate/abort. See [0.7.15 evidence](RELEASE_NOTES_0.7.15.md).

Static inspection of the exact built ELF shows that the precompiled timed-wait method compares its result with **110 (`0x6e`)**. The installed SDK's `bits/errno.h` defines native **ETIMEDOUT as 60**. This ABI mismatch treats a normal native timeout as an error and explains the observed exception path. It does not explain the preceding failed ICE connection.

## Narrow runtime replacement

The complete official LLVM 11 `condition_variable.cpp` is included unmodified and compiled as an explicit object with the existing native headers. It supplies the five strong methods belonging to the old archive member before `-lc++`, leaving that member unselected under normal static linking. Installed SDK archives and other runtime objects remain unchanged. This handles the native timeout through the normal condition-variable contract and retains actual error reporting.

- Source: `src/streaming/rtc_condition_variable.cpp`.
- Upstream commit: `176249bd6732a8044d457092ed932768724a6f06` (`llvmorg-11.0.0`).
- Source SHA-256: `39d39fc245662b760c7c74a29218eeb8c66805996dc00e25e1e77693e53a77a5`.
- License: Apache-2.0 WITH LLVM-exception, retained source header and full `docs/licenses/LLVM-libcxx-11-LICENSE.TXT`.

The observed headers report libc++ version 11000 and ABI namespace `std::__1`. The precise upstream revision of the other precompiled SDK objects remains unknown; this replacement does not assert that they all match that source revision.

## Candidate rejection categories

The previous numeric IPv6 class 1 could not distinguish ordinary IPv6 from a rejected Teredo candidate. It now includes a fixed reason, without logging any address, port, MID, candidate string or credentials. Acceptance/conversion rules and validation order remain.

| Reason | Category |
|---|---|
| 0 | Accepted Teredo candidate |
| 1 | Missing basic fields |
| 2 | Invalid foundation |
| 3 | Invalid component number |
| 4 | Component other than 1 |
| 5 | Transport other than UDP |
| 6 | Invalid priority |
| 7 | Invalid outer port |
| 8 | Invalid `typ` marker |
| 9 | Unsupported candidate type |
| 10 | Unpaired extension |
| 11 | Unsupported/invalid IPv6 grammar |
| 12 | IPv6 outside the Teredo prefix |
| 13 | Zero decoded Teredo port |
| 14 | Excluded decoded IPv4 range |

These categories describe validation decisions and do not themselves establish the cause of ICE/RTC failure. Build, review, matching source integrity and console results will be recorded when available. No automated implementation tests are added or run.

## Build and focused review

The C++ object compiled with all five expected strong `T` methods. Actual Claude Code selected **claude-opus-5-5** and completed the focused source-only review successfully in **6 turns**, with no material issue in the inspected source, Makefile and diagnostic diff. Review did not run builds/tests, inspect metadata changes or prove hardware behavior. Independent inspection verified the native SDK header, complete upstream source hash and archive-member symbol coverage.

The full application compiled, linked, converted to SELF and packaged. The link map attributes all five methods to `build/streaming/rtc_condition_variable.o`; the old `libc++.a(condition_variable.cpp.o)` member is absent. The existing separate destructor archive member remains. In the final ELF, `__do_timed_wait` at relative address **0x232d0** compares against **0x3c (60)** at **0x2332f**, confirming the native timeout value is in the linked application. Source matches the exact upstream SHA-256. No SDK archives were rewritten.

## Package integrity

- `XCloud4-0.7.16.pkg`: **8912896 bytes**.
- SHA-256: `57f434bf98469b7ae38cff1739376fb958d77eb10e4151272c99f258a5fd9489`.
- VM, PC and PS4 FTP retrieval hashes match.
- Only the 0.7.16 installer remains in `/data/pkg`, after verification and removal of the previous installer. Earlier local packages and source archives remain immutable.

## Console status

The 0.7.16 package has been delivered for the owner to install. The log capture is connected. A corrected linked constant and successful build do not confirm that the console stays open, that ICE connects or that media is received. The previous RTC failure remains under investigation.

## Matching dependency/native-adapter source

- Archive: `XCloud4-0.7.16-dependency-sources.tar.gz`, **83566130 bytes**.
- SHA-256: `f274e91489dde87e948616411fbde8129b6091d0cbffae16b5ef774b5682f3b3`.
- Guest/PC hashes match. All **9368** manifest source hashes verify locally, covering **150852465 source bytes**, with **9779 archive entries** and no forbidden generated/private paths.
- Archived notices and the complete C++ runtime source match the frozen repository bytes. The full application revision separately supplies the Makefile and candidate diagnostics. Earlier package/source snapshots stay immutable.
