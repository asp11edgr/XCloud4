# XCloud4 0.7.20 — gamepad sending, video work and graphics completion

Product **0.7.20**, PS4 **APP_VER 00.90**, application identifier **XCLD00001**. The PS4 interface remains Spanish; repository and release content remain English. **The native build, package/source verification and focused review of resolved findings succeeded. Game controls, measured playback performance and external closure are awaiting a console attempt.**

## Previous confirmed result

The owner confirmed live game video and audio in [0.7.19](RELEASE_NOTES_0.7.19.md). Audio worked well, video was very slow and no gamepad-report sender existed. Its first external close failed with **0xa0d0c00a / CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC**, after the kernel reported that `submitDone()` had not been called. There was no faulting CPU-thread information for that asynchronous graphics/system exception.

A separate ARK attempt reached live media, continued for approximately five minutes, then reported RTC/ICE failure and successful Xbox deletion with HTTP 200. That later disconnect does not establish why the remote connection ended. Both outcomes remain preserved in the [0.7.19 report](ERROR_REPORT_0.7.19.md).

## Xbox gamepad reports

The original sender serializes one controller into the provider's **38-byte input 1.0 report**. The portable `gamepad.h` state is not sent as a C struct. The report contains the button mask, four stick axes, two triggers, sequence/timestamp and protocol trailers.

- Send at no more than **60 Hz**, without catch-up bursts, after channel metadata and Xbox startup are complete, RTC is connected and the input channel is open.
- Send only when the channel's buffered amount is zero. A busy channel or send error increments a drop counter; it does not queue historical controller states.
- Read the latest state from the authentication mailbox. A source older than **250 ms**, a disconnected controller or cancellation produces neutral input.
- Stop and join the sender before releasing its RTC/source context. Numeric packet/drop/error counters are included in session diagnostics.

These are implemented source behaviors. Successful Xbox acknowledgement, actual button/axis response, neutral release and shutdown remain pending on the console.

Physical controller sampling remains in the UI thread. The independent sender prevents synchronous session HTTP calls from delaying transmission, but it cannot make a slow native decode call sample the controller faster. The stale-state policy releases inputs rather than replaying an old held state indefinitely.

## Controller mapping

| DualShock 4 | Xbox / local action during a connected game |
|---|---|
| CROSS / CIRCLE / SQUARE / TRIANGLE | A / B / X / Y |
| Directional pad | Xbox directional pad |
| L1 / R1, L2 / R2 | LB / RB, left / right trigger |
| L3 / R3, both sticks | Stick clicks and stick axes |
| OPTIONS | Xbox Menu |
| Touchpad click | Xbox View |
| Hold L1 + R1, then OPTIONS | Exit XCloud4 locally |
| Hold L1 + R1, then CIRCLE | Close the session and return to the catalog |
| Hold L1 + R1, then SQUARE | Toggle local sound |
| Hold L1 + R1 with the touchpad | Xbox Guide |
| PS | PS4 system button |
| SHARE | No mapping through the current OpenOrbis Pad interface |

Reserved local chords neutralize the game report; the Guide chord sends the Guide bit without passing the chord's shoulder/View buttons. This is the prepared mapping, not a record that each control has already worked in a game.

## Video drawing and bounded processing

Scaling caches source-coordinate lookups when image/output dimensions change, removing division from the per-pixel copy loop. Live playback skips the full menu clear/redraw behind the image. Decoded output can remain pending so the latest output is converted after processing; output storage is converted before reuse when required by its lifetime.

Processing checks a **6 ms or two native Decode-call limit between operations**. It is not a hard bound on one native decoder call, conversion or rendering work. No predictive frame dropping is introduced. Existing bounded RTP queues and loss/keyframe recovery remain in place.

Numeric summaries at approximately **five-second intervals** record counts and durations for media processing, decoded/converted images and drawing. They allow later rate calculations from actual elapsed time. No achieved FPS, speedup, queue-loss reduction or ARK improvement is claimed before the console measurements.

## Graphics completion and external close

The application links the existing OpenOrbis **`SceGnmDriver`** import and calls **`sceGnmSubmitDone()` after a successful `sceVideoOutSubmitFlip()`, before the flip-status wait**. The first result and negative results are logged numerically; errors are propagated. The scanout wait and display-buffer ownership rules remain necessary.

This adds the per-frame completion notification missing from the 0.7.19 presentation path. The API denotes the end of a frame's command submission; the public [OpenOrbis declaration](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/v0.5.4/include/orbis/GnmDriver.h#L252) and [OpenGNM frame sequence](https://ps4-opengnm.github.io/opengnm/guides/rendering-pipeline/#submit-done) support the call and ordering. Applying that boundary to this CPU/VideoOut renderer is a proposed response to the observed suspend timeout. A successful external close is not yet established, and the graphics exception is not attributed to SCTP receive code.

## Review, build and artifact status

| Evidence | Current state |
|---|---|
| Actual Claude Code `claude-opus-5-5` reviews | Initial review found two issues; both corrected, fresh focused resolution review PASS |
| Full native application/package build | Dependency/application compilation, link, SELF conversion and packaging succeeded |
| `XCloud4-0.7.20.pkg` size and SHA-256 | 8,912,896 bytes; `c985a9c384691992860251106b49ae63ff607fc1df916d793d355e5789c0b74d` |
| VM / PC / retrieved PS4 package correspondence | Matching byte count and SHA-256; only the 0.7.20 installer remains on PS4 |
| `XCloud4-0.7.20-dependency-sources.tar.gz` inventory and SHA-256 | 83,583,284 bytes; `e11c40da81cff6a6cb79299da0cbd755ce46df45c8a80a887860f7a5a65e3498` |
| Game controls, measured video performance and external closure | Pending console attempt |

No automated tests were added or run for this checkpoint. Build/review evidence and measured hardware behavior will be recorded separately when available. The earlier package/source snapshots remain preserved.

### Findings resolved before the console attempt

The first actual **Claude Code `claude-opus-5-5`** review completed successfully as a review process, but its verdict was **not PASS**: one material issue could return B/X/Menu to local actions during a transient RTC disconnect; another could expire a reorder gap before a budget-delayed missing packet was ingested. It used **34 turns, 33 Read calls and 25 distinct files**. Reviewer access remained read-only, with no out-of-scope tool calls, edits or tests. This was a source review, not a console test or exhaustive repository audit.

The final source latches game-button ownership after the session first connects and preserves it across transient disconnects until the session ends or the page changes. Actual packet sending still requires a connected RTC channel. The first reorder drain suppresses time expiry; later drains allow time expiry only when the ingress queue appears empty. Depth/window limits remain active and the queue check is concurrent, so the change does not promise loss-free playback. A separate fresh focused **`claude-opus-5-5`** review of these three changed paths and their context returned **PASS**, with **9 turns, 8 Read calls and 8 distinct files**, zero out-of-scope calls and zero stderr bytes. The H.264 file was not reread in that focused review; its bytes remained identical to those read in the initial review. Frozen source hashes still matched after both reviews. The package was rebuilt and reverified after the changes; the initial pre-review artifacts remain private.

The corresponding dependency archive contains **9,369 manifested files**, **9,781 entries** and **150,888,183 source bytes**. Every manifest file hash was checked. Current port recipes, configuration, native streaming adapters, portable `src/input/gamepad.h` and notices match the exported bytes. The archived SCTP source matches the applied source retained from 0.7.19; native disassembly still reserves 552 local stack bytes. The source exporter refuses to replace an existing snapshot. Prior published assets remain recoverable from their versioned releases.

The exact linked presentation function calls `VideoOutSubmitFlip`, then `sceGnmSubmitDone`, then `GetFlipStatus`. The final OELF (**6,730,472 bytes**, SHA-256 `3cfecc9446dc95445c4e96b981e7db09d87fa822534ef1b77a0099802cff244d`) contains the expected NID `yvZ73uQUqrk#D#D`, library/module 3 named `libSceGnmDriver`, a `JUMP_SLOT` relocation for that import, and `DT_NEEDED` for `libSceGnmDriver.prx`. This verifies the packaged import rather than hardware execution or a repaired suspension result.
