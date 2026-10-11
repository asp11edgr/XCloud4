# XCloud4 0.7.34 - bounded NACK grace and optional rumble

## Subsequent console evidence

The [closed G0/G45 report](CONSOLE_REPORT_0.7.34_G0_G45.md) now records both rumble-off passes. Neither had a sequence gap/NACK request or a stable NEW-image interval over 100 ms, so useful grace repair remains unexercised. Physical rumble remains pending. The preparation evidence below is preserved; packages and their source identities are unchanged.

## Verified preparation checkpoint

At the preparation checkpoint, three native profiles were built, inspected and copied to PS4 with **two exact FTP readbacks each**. Final runtime source identity is `54d5793d1b50a7ea24ba73e79b258577f51511c0`. **At that checkpoint, no 0.7.34 playback trial, useful grace repair, physical vibration or instrumentation-overhead result had yet been measured.** The [dev-0.7.34 release page](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.34) and its artifact manifest identify publication status and downloadable assets.

| Profile | NACK / grace ms / rumble | Build suffix | Package |
| --- | --- | --- | --- |
| G0 | 1 / 0 / 0 | `n1-g0-r0` | XCloud4-0.7.34-grace-off.pkg |
| G45 | 1 / 45 / 0 | `n1-g45-r0` | XCloud4-0.7.34-grace-45.pkg |
| G45VIB | 1 / 45 / 1 | `n1-g45-r1` | XCloud4-0.7.34-grace-45-rumble.pkg |

Each embedded Build ID is the source identity followed by its suffix. The grace-only checkpoint was committed separately at `4d4a1a4`; the final rumble/evidence checkpoint is the source tree published with this release. Runtime identity excludes analyzer, test and documentation changes, so a later evidence-label update is not a new runtime build.

## Functional scope and limits

The worker's original [grace policy](NACK_GRACE_EXPERIMENT_0.7.34.md) protects only an eligible requested pending position, using **detection time + 45,000 microseconds**. AUTH arrival alone does not complete it; actual STORED worker admission does. Deadlines are never renewed. Expiry is evaluated in the existing worker loop even without ingress; scheduling or Decode/copy can delay evaluation, so no independent timer guarantees execution exactly at 45 ms. Only deadline/capacity releases force the existing hole advance; cancellation returns to ordinary depth/time behavior. An advance stops before another range's protected position, including a skip that emits no packet.

The reorder remains **128 slots / depth 32 / 25 ms**. Capacity can end a hold **before 45 ms**, and a later out-of-window burst can still use the existing JUMP/reset fallback. Sixteen active ranges and 64 request histories bound the experiment. Unknown omitted control/bridge delivery disables grace for that session, while NACK stays enabled. Current original-video **PT 102 / worker sequence epoch 1** limitations remain; no utility after a later sequence rebase is claimed.

MPSC256, four copy readers, the **16 ms / four Decode** budget, native allocation, H.264/Opus, recovery architecture and **960 x 540 / maxFPS 30 / 5000 kbps request** remain unchanged. Earlier measured delivery was **1280 x 720**; the new trial must record actual dimensions again. No SDP/RTX/resolution or interface redesign is included.

[Optional rumble](RUMBLE_0.7.34.md) uses an original bounded binary parser and latest-event mailbox, with Pad calls owned by main. It accepts modern/legacy layouts, maps the two main motors, caps a receipt-anchored pulse at 5 seconds and treats duration 0 as stop. Trigger motors, repeat and delay are decoded without actuation. A pending event newer than the poll clock is deferred without renewing its deadline. This is not full Xbox scheduling fidelity or proof of physical vibration.

## Verification actually completed

- **Host campaign:** ten harnesses exercised with GCC and Clang. Grace state: **23 cases / 531,181 checks**; shared portable reorder plus grace: **19 cases / 420 checks**; inbox: **seven groups**; rumble: **13 cases**. GCC ASan/UBSan passed for the new harnesses. Clang sanitizer runtimes were unavailable, not passing. There is no complete native-worker end-to-end fixture.
- **Reader timeline:** the native-build campaign passed **127** checks. A later analyzer-only labeling correction passed **128**: shared release code 5 means `source_or_reorder_reset`, not proof of an SSRC change. Original 0.7.33 evidence uses distinct attempt-start and local-result clocks; neither measures RTT or retransmission causality.
- **Source review:** two actual Claude Opus 5.5 immutable read-only reviews. The second read **28 selected files**, confirmed four corrections and found no new runtime-safety issue in that scope. The corrections cover release-specific forcing, fresh rumble poll time, stale inbox cursor handling and preservation of independent pending ranges. A wrong-root `candidate.diff` read failed even though the artifact was present; no complete diff review is claimed. Claude did not execute the builds or tests.
- **Native build:** the complete dependency stack was rebuilt and all three application profiles were force-rebuilt. Selected native identity/import checks passed, including **libScePad / scePadSetVibration**. The 221-file frozen build checkpoint matched before/after. Native import/static checks do not establish controller actuation, worker scheduling or performance.
- **Artifacts:** VM/PC hashes and two PS4 readbacks match for every package. All **9,385 dependency-source payload hashes** verify, with no unsafe paths, duplicate members or symlinks. Known patched vendor/recipe/native-adapter correspondence and generator reconstruction passed. The archive has no Git metadata; declared pins are checked against the export recipe, and the Opus tar was not redownloaded in this verification. Full application source is preserved in the repository checkpoint, separately from this dependency archive.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| XCloud4-0.7.34-grace-off.pkg | 9,043,968 | `41fa0c83ed43469fe919af3ec460306c8c0659b79bfbee8057645b841b4386e4` |
| XCloud4-0.7.34-grace-45.pkg | 9,043,968 | `4598a9670b8e4e35f3876f9766878fd196ccdcb2d3ba05a2e67ec1c19bf104f4` |
| XCloud4-0.7.34-grace-45-rumble.pkg | 9,043,968 | `2195f57e11767bfe959b520e11b8b49b6b34b90e91c3db3f87daf08fd96ea9c0` |
| XCloud4-0.7.34-dependency-sources.tar.gz | 83,630,492 | `8bd10b4eac1b83eaa03ec57222a241abf5760068a713c30d681b53ed172027af` |

## Fresh console procedure

1. Close the app, install **G0**, launch freshly and open one game. After loading, hold **L1 + R1** and tap **TRIANGLE** to mark stable play. This locally reserved combination neutralizes the forwarded gamepad frame.
2. Play **10-15 manually stable minutes**; mark transitions separately and return to stable after loading. Preserve game/scene/network notes and original exports. Return with **L1 + R1 + CIRCLE**, then exit the catalog using **OPTIONS**.
3. Repeat freshly with **G45**, rumble still off, for the same planned activity and stable duration. Compare exposure-qualified NEW-image intervals over 100 ms, recovery, AU utility, grace releases/capacity and coverage. Collect separate video/audio/control/return/exit feedback; scene/server/network equivalence is not assumed.
4. Only after this pair, try **G45VIB** separately for owner-observed physical vibration and reliable stop/cancel/disconnect behavior. A rumble regression returns to G45; a grace regression returns to G0, with a fresh launch.

Worker-cost timing begins at admitted OFFER consumption, not the entire detection-to-release window. HOLD, RELEASE and worker ABANDON are separate clocks. Missing capped details never establish absence; successful local protected feedback never establishes server receipt. No decoder-output PTS supports assigning the last input AU to a native picture. PS4 UI remains Spanish; repository documentation is English. Raw captures, SSRCs/local source keys and credentials remain private.
