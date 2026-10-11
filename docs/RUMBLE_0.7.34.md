# XCloud4 0.7.34 — optional DualShock 4 rumble

## Status and isolation

The original portable parser, bounded mailbox and main-thread integration are implemented. Final host checks, the native rumble profile, selected Pad import inspection and two exact installer readbacks passed. **Actual negotiated input version, Xbox report receipt and physical controller vibration remain unmeasured.** A synthetic parser pass or successful local Pad call cannot establish those later stages.

`X4_RUMBLE` defaults to **0**. The [45 ms grace comparison](NACK_GRACE_EXPERIMENT_0.7.34.md) uses rumble 0 in both profiles. The separate follow-up enables rumble 1 while retaining NACK 1 and grace 45; do not attribute its results to the grace-only pair.

## Primary protocol evidence

| Reference and pinned revision | Inspected evidence | License |
|---|---|---|
| [PSBox input parser](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/e8a05232d41eb92f7ca04878e012f682bbd22d87/src/stream/input_packet.cpp) | Modern 13-byte input-channel report, four motor percentages and little-endian duration/delay | GPL-3.0 |
| [Better xCloud vibration manager](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/modules/device-vibration-manager.ts) | Modern/legacy type-width distinction and FourMotorRumble subtype | MIT |
| [xbox-xcloud-player input channel](https://github.com/unknownskl/xbox-xcloud-player/blob/a6511a645736f4988f77323284c6f0f36bf93aef/src/channel/input.ts) | 13-byte fields and ordered input protocol `1.0` | Package declares MIT |
| [Greenlight gamepad implementation](https://github.com/unknownskl/greenlight/blob/07a3f01cba527c9c8287f90ef326c5e89c64687b/packages/player/src/client/lib/input/gamepad.ts) | Main left/right correspond to strong/weak browser motors; client-specific pulse scheduling | Player package declares MIT |

These are public third-party clients, not a claimed official Microsoft GameStreaming-SDK. XCloud4's GPL-3.0-only C parser and mailbox are original implementations based on the observed fields; no client implementation was copied. Client scheduling behavior is not treated as a universal wire guarantee.

## Accepted layout and local v1 policy

Only exact **13-byte modern** and **12-byte legacy** binary reports are accepted. The type is exactly 128, subtype is 0 and controller index is 0. Unknown/combined reports, other controllers and percentages above 100 are rejected or classified unsupported.

| Field | Modern byte offset | Legacy byte offset |
|---|---:|---:|
| Type | 0–1, little-endian | 0 |
| Subtype / controller index | 2 / 3 | 1 / 2 |
| Main left / right | 4 / 5 | 3 / 4 |
| Trigger left / right | 6 / 7 | 5 / 6 |
| Duration, little-endian ms | 8–9 | 7–8 |
| Delay, little-endian ms | 10–11 | 9–10 |
| Repeat | 12 | 11 |

Main left maps to the DualShock 4 large motor and main right to the small motor, using rounded `percent × 255 / 100`. The verified OpenOrbis interface is `scePadSetVibration(handle, OrbisPadVibeParam*)`, whose fields are `lgMotor` and `smMotor`. PS5 vibration-mode/adaptive-trigger APIs are not used.

The first local policy applies **one main-motor pulse**, anchored to report receipt and capped at **5,000 ms**. **Duration 0 stops.** Delay, repeat and trigger values are decoded but deliberately ignored for actuation. DualShock 4 has no independent Xbox trigger motors, and this policy does not claim full Xbox rumble scheduling fidelity. In particular, duration 0 is not an indefinite pulse.

## Transport, mailbox and controller ownership

Binary messages on the existing input data channel enter a fixed latest-event mailbox. Text messages are not interpreted as rumble. The callback uses one nonblocking try-gate, drops only the optional event on contention, and performs no allocation, printing, sleep, spin or Pad call. A newer complete event can supersede an older pending one; both contention and supersession are counted.

The main thread polls desired motor bytes and owns pulse timing. An event already expired before polling cannot begin a delayed pulse. Disable, close, no active gameplay, disconnection and invalid clock cases request zero; main performs physical zeroing before shutdown waits and invalidates its applied-handle cache on disconnect/reopen. Hardware calls and their local results stay outside RTC callbacks.

Main takes a fresh monotonic time at the poll call. A callback can still publish between that clock read and slot acquisition, so a report with receipt time later than the poll argument remains pending for the next poll. It is neither activated prematurely nor consumed and discarded as a clock reversal. An already active pulse retains its existing age rules; a backward time relative to that active pulse or expiry overflow produces zero. Deferral never renews the receipt-anchored deadline, and disable/session reset prevents a deferred old event from becoming actionable.

The mailbox has a fresh session epoch and must outlive every producer. Beginning another session or freeing it requires previous producers/poller to be externally joined. Optional allocation failure disables vibration without blocking playback. A failed transport join retains borrowed state rather than freeing it beneath a callback.

No extra handshake flag has been invented. The inspected [player control startup](https://github.com/unknownskl/xbox-xcloud-player/blob/a6511a645736f4988f77323284c6f0f36bf93aef/src/channel/control.ts) uses ordinary gamepad-presence messaging; the inspected PSBox startup likewise supplies no additional verified rumble-enable flag. This supports leaving the existing handshake intact; it does **not** establish that this account/session will negotiate a compatible version or deliver reports.

## Separate console trial and rollback

After completing the rumble-off G0/G45 comparison, close the app and launch verified **G45VIB / `XCloud4-0.7.34-grace-45-rumble.pkg`**, with **NACK 1 / grace 45 / rumble 1**, freshly. Use a game/action known by the owner to provide controller feedback. Preserve game/scene notes and stable/transition marks; do not use this pass as the rumble-off performance baseline. Hold L1 + R1 and tap TRIANGLE for a phase mark, return with L1 + R1 + CIRCLE, then exit the catalog with OPTIONS.

Record these stages independently:

1. Actual input-channel/protocol negotiation evidence, if available.
2. Incoming binary messages and recognized, valid, malformed/unsupported reports.
3. Accepted, superseded, contended/inactive events and desired motor output.
4. Local `scePadSetVibration` calls, successes and errors.
5. Owner-confirmed physical vibration, correct stop on cancel/end, disconnect/reconnect behavior, and separate audio/control observations.

A local SDK success is not proof that the controller physically vibrated. No received reports leaves delivery/support unconfirmed, rather than proving a Pad failure. Return to **NACK 1 / grace 45 / rumble 0** with a fresh launch if feedback or lifecycle regresses. The broader grace rollback is **NACK 1 / grace 0 / rumble 0**.

## Verified preparation and remaining limits

The corrected **13-case** portable harness covers both layouts, malformed boundaries, motor scaling, receipt-time expiry, latest-event/epoch reset, duration cap/zero stop, ignored fields, clock errors, NULL/lifecycle contracts, a deterministic blocked producer and concurrent producers/main polling. Added module fixtures cover receipt after frame start and receipt after the poll clock sample, including later activation, deferred zero-stop, overflow and disable/rearm. They do not execute the real main loop or controller.

The final corrected **13-case** harness passed in the GCC and Clang host campaigns, with GCC ASan/UBSan passes. Clang sanitizer libraries were unavailable; no Clang sanitizer pass is claimed. These final results supersede the historical preliminary 11-case checks, without turning a module fixture into a test of the real main loop, Xbox input channel or physical controller.

The native dependency stack was rebuilt, followed by fresh G0, G45 and G45VIB application builds. The frozen runtime source ID is `54d5793d1b50a7ea24ba73e79b258577f51511c0`; G45VIB's embedded suffix is `n1-g45-r1`. Selected OELF inspection verifies `scePadSetVibration` imported from **libScePad**, separately from the host fixtures. Its **9,043,968-byte** package SHA-256 is `2195f57e11767bfe959b520e11b8b49b6b34b90e91c3db3f87daf08fd96ea9c0`, matching PC/VM bytes and both PS4 FTP readbacks. Full profile/source hashes are in [the release notes](RELEASE_NOTES_0.7.34.md).

Two actual Claude Opus 5.5 read-only reviews support the selected source scope; the second confirmed fresh poll timing and pending-event deferral along with the grace/inbox corrections. It found no new runtime-safety issue in that delta. Partial source coverage and a failed wrong-root diff read are preserved limits; this is not a complete diff or hardware review. Failed Pad calls can still be retried on the main thread without a new backoff policy; they do not run inside RTC callbacks.

The separate grace harness has final 23 state cases / 531,181 checks, shared reorder coverage has 19 cases / 420 checks, and the inbox has seven groups. None establishes real Xbox feedback reception, physical vibration, native scheduling or fluency. **No 0.7.34 console trial or physical rumble result is confirmed.** Runtime packages remain frozen while these evidence notes are updated.
