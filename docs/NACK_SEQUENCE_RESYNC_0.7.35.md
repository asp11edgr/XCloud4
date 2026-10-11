# XCloud4 0.7.35 - validated NACK sequence resynchronization

## Status and purpose

At preparation time, the implementation, requested host campaign, native rebuild, selected artifact inspection and transfer/readback were **verified**, and the fresh console comparison was pending. The subsequent [G0/G45 console report](CONSOLE_REPORT_0.7.35_G0_G45.md) records an exercised rebase with resumed grace/AU utility and similar pause rates in different zones; it does not establish a fluency benefit. Local host fixtures establish resumed grace and AU assembly for a validated transition; they do not establish real retransmission or a displayed repaired picture. The [machine-readable preparation receipt](BUILD_VERIFICATION_0.7.35.json) remains the frozen build-time record.

The baseline is commit `6d26bce`. The [separate 0.7.34 G45 pass](CONSOLE_REPORT_0.7.34_G45_GEARS.md) recorded one local observation-gate omission, an observer rebase and five later `stale_source` grace offers. Their sequence origins differed by 65,536 from the independent upstream observer. That supports a local continuity problem; it does not identify network loss, server retransmission or a new SSRC.

This change validates the hand-off between the authenticated NACK observer and the video worker after such an omission. It preserves the original grace policy and stream/worker configuration.

## Omission, rebase and worker binding

1. **A contended receive try-gate omits an observation.** The callback marks the observer dirty and leaves that packet without usable NACK sequence provenance. It does not wait, invent a tag or inspect mutable observer state outside the gate. Packet delivery and NACK observation are distinct paths.
2. **The next successful incoming or poll gate holder retires the old watches.** The next observation starts a new sequence epoch with an immutable anchor: current owner, accepted source, previous epoch/high position, new raw anchor, RTP timestamp and local clock. A sequence announcement is separate from a retransmission request.
3. **The worker validates two independent inputs.** The announcement must match a real original-video PT102 anchor packet that passed Track admission and was actually `STORED` without a reorder `JUMP`. Exact raw sequence, RTP timestamp, owner/source, immutable tag and authenticated clock must agree. Announcement and packet proof can arrive in either order. An announcement, AUTH observation or arbitrary REQUEST alone cannot bind the worker.
4. **Only a proven same-source omission rebase can commit.** Initial binding is epoch 1. A later binding must be the immediate successor of the committed epoch, with coherent previous/consumed high positions and forward modular distance below 32,768. Foreign owners/sources, backward or half-span ambiguity, overflow, missing/LATE/JUMP anchors and contradictory proofs fail closed. General source-change or ambiguity tags do not grant an omission rebase.

The new raw sequence origin maps into one canonical extended domain with an offset that must be an exact multiple of **65,536**. For example, a validated raw anchor 6 after previous high 65,538 maps to canonical 65,542, with offset 65,536. Matching packet and request tickets use that same mapping; an old ticket with the same 16-bit wire value cannot alias the new epoch.

## Preserved safety limits

- Old active grace holds retire during the validated hand-off. Pending masks are not transplanted into the new domain, and retirement does not force an unrelated baseline skip or restart recovery.
- Each admitted range retains **detection time + 45,000 microseconds** as its absolute deadline. Rebase, dispatch, duplicates and arrivals cannot renew it. The existing worker evaluates eligibility; scheduling or a nonpreemptible Decode can delay evaluation past the deadline.
- The existing **64-request session budget** and consumed request identities survive rebases. Overlapping canonical positions cannot gain another grace interval through a new sequence epoch. A consumed unmapped request stays consumed after a later anchor; duplicate controls cannot rearm it.
- A valid REQUEST waiting for its matching candidate proof may remain at the inbox head only within its original deadline. Media processing continues. Unknown provenance does not become permission to hold.
- Delayed controls can reconcile only retained `STORED` slots with matching immutable packet provenance. AUTH and ENQUEUE do not count as worker acceptance. A stale OUTCOME cannot cancel a separately proven current range.
- Functional bridge omissions still disable grace for the same session. NACK observation/dispatch continues; complete diagnostic detail does not prove complete inbox delivery.

The two comparison profiles remain **G0: `-n1-g0-r0`** and **G45: `-n1-g45-r0`**: NACK ON, rumble OFF, with grace 0 or 45 ms. MPSC256, four copy readers, the 16 ms/four-Decode budget, reorder 128 entries/depth 32/25 ms, recovery, SDP and RTX remain unchanged. The request remains 960 x 540/maxFPS 30/5000 kbps; earlier native output was 1280 x 720. A request does not establish delivered geometry.

Sequence provenance adds a fixed 48-byte tag to existing packet metadata: the ingress ticket grows from 40 to 88 bytes. No queue/window capacity grows, no per-packet allocation or blocking diagnostic write is added, and this metadata does not provide extra buffering to conceal pauses. Binding counters such as `mapping_checks` and `unmapped_provenance` count operations, which may repeat for a packet; they are not universal packet-loss counts. Rejection kind 20 preserves unclassified numeric fields when its mixed layouts cannot identify an epoch or request ordinal safely.

## Host fixture and verification boundary

The new fixture uses the production NACK handler, bounded inbox, application receive/ingress path, `video_batch`, reorder, grace and H264 AU assembly. Its separate baseline mode compiles the corresponding units from an independently extracted `6d26bce` source tree. The patched pinned Message supplies zero-default additional metadata to that baseline; the old handler/worker policy is not replaced by the new binding helper. The verified historical archive SHA-256 is `f8202fcfcd7112fab8e01cf30fef5a74b86d61a238921c4d5eb46897aa52d7ca`.

Executed scenarios include wrap plus forced gate contention, dirty retirement through incoming or poll, G0/G45 processing on the same monotonic synthetic calendar, exact anchor proof, stale owner/foreign source/duplicate controls, queued arrivals at deadline or capacity, and consumed unmapped or mapped-stale request replay. Both historical cases reproduce the epoch-1 worker rejecting five later epoch-2 offers. The current cases resume canonical binding after the validated rebase: raw 6 maps to 65,542 and requested raw 7 maps to 65,543. For the timely return, G45 reaches STORED, structural AU completion and recovery acceptance; G0 abandons by its ordinary depth policy and rejects that return. Exact assembled bytes are checked at a stub Decode feed.

The shared-clock boundary cases also preserve a result unfavorable to G45: with a return left in ingress until the absolute deadline, G45 abandons before admission while G0 can still store it through its existing ingress-aware ordinary policy. AUTH-before-deadline is therefore not worker utility. The capacity case retains all fourteen returns already enqueued, a 114-packet/span-127 snapshot and the next offset-128 insertion forcing release; the subsequent returns are rejected behind expected. Neither policy is changed to improve this fixture.

## Preparation results and artifacts

| Check | Result and scope |
| --- | --- |
| Eleven host harnesses | GCC and Clang plain runs PASS; 22 completed harness logs. |
| Integrated current pipeline | 17 cases / 29,741 checks per run; GCC plain and ASan/UBSan, Clang plain PASS. |
| Exact historical production units | Two cases / 1,555 checks; GCC and Clang PASS reproducing the failure. Not an old native binary. |
| Pure binding helper | Six cases / 145 checks; GCC plain/ASan/UBSan and Clang plain PASS. |
| Python trace reader | 134 tests PASS, including final generic rejection-field labels checked after the native freeze. |
| Sanitizers | GCC configured ASan/UBSan checks PASS; existing ingress, progress and receive-bridge TSan checks PASS. Clang sanitizer libraries are unavailable, explicitly not a pass. |
| Native build | Dependency Message ABI rebuilt; G0 and G45 PASS. Rumble OFF in both. |
| Correspondence | 85 runtime/build-identity file hashes checked; embedded ELF/OELF profile identities and selected native imports/direct call bodies PASS. |
| Sources and delivery | 9,387 dependency-source manifest entries verified; both PS4 installers independently read back twice with exact bytes. |
| Independent review | Actual Claude Code Opus 5.5 selected-source and targeted-delta reviews completed with qualified PASS. Coverage is partial; no reviewer console/performance test. |
| Fresh console G0/G45 | Pending at preparation time. Subsequently completed: see the separate console report for measured cadence, continuity/AU evidence, different-zone and retention limits. Instrumentation overhead and a causal fluency benefit remain unestablished. |

Runtime fingerprint: `8425f615c09af4a4d796157922968635fbcad89b`, a content identity rather than a Git commit. Both profiles use PS4 APP_VER **01.03** and each installer is **9,043,968 bytes**.

| Installer | Embedded identity suffix | SHA-256 |
| --- | --- | --- |
| `XCloud4-0.7.35-grace-off.pkg` | `-n1-g0-r0` | `eaf1dbc939a569ea909b70c0c1bd929289e8a42ac607807ff2ca9669867d4cb9` |
| `XCloud4-0.7.35-grace-45.pkg` | `-n1-g45-r0` | `aafaed996db8e661caa91b900febf2a58af40f798081dbd8ae0c54c7ed9aac9a` |

The exact dependency source export is 83,636,667 bytes, SHA-256 `2e35a6f311d7ac41cf559eefb2304d49a6a883643d1be1b1553cfebeb6d0655e`. A private streaming wrapper avoided temporary build-host disk exhaustion while preserving original source selection, tar/gzip settings and source hashes. Product sources and compiled packages were not changed by that export workaround. Raw console logs/source identities remain private.

Authentication flags, time, kernel/video/audio and diagnostic bridges are **host doubles**. The fixture manually calls the worker batch; it does not run the native video thread, SRTP cryptography, SRTCP/network/server, native Decode, RGB conversion or presentation. AU-feed bytes do not prove a displayed repaired picture. Native compilation and selected linked inspection are separate checks; neither proves console scheduling or fluency.

## Remaining limitations and separate proposal

If the exact authenticated anchor is missing, LATE, JUMP or contradictory, the candidate cannot establish the new origin. It may remain unproven and leave grace unavailable for the remainder of that session. This is an explicit safe liveness limit, not a validated recovery path. A pending REQUEST still leaves the FIFO at its original deadline; neither holds nor memory grow indefinitely. The tested correction concerns a transition whose correspondence can be proven. It does not accept an arbitrary later epoch to work around a missing proof.

The previous R2 hold released on bounded capacity before 45 ms, even though all fourteen missing positions had reached authenticated delivery and the callback. The current worker still checks deadline/capacity before accepting later queued returns. Resynchronization does not solve that ordering or enlarge the reorder window.

The worker's 16 ms budget is checked between operations; it cannot preempt Decode or startup copying/verification. The previous startup COPY_END-to-BYTECHECK_END span was 34.290 ms, outside the selected-copy counter. That observation is historical and does not automatically explain a stable pause in a new run. No repeat timing investigation or startup optimization is included here. Native output provides no PTS association linking a returned packet or accepted AU to a later NEW image.

The current dispatcher selects a bounded pending snapshot under the observation gate, then releases that gate before packet construction/send. Arrivals after selection can still race dispatch. A **separate proposed change**, not implemented here, is a bounded dispatch queue with a single owner, explicit lifetime/cancellation and omission behavior, to separate observer work from dispatch selection. It needs its own review and fixtures; it would not solve worker capacity or establish server receipt.

## Fresh console comparison and rollback

1. Preserve baseline `6d26bce` and its verified original installers/source assets. Close the app, install the verified 0.7.35 **G0**, and start a fresh session after final build/delivery checks.
2. Use one intended game/scene and network arrangement. After loading, hold **L1 + R1** and tap **TRIANGLE** to mark STABLE. Record 10-15 minutes; use the same shortcut for TRANSITION before changing scenes or returning.
3. Return with **L1 + R1 + CIRCLE**, then exit from the menu with **OPTIONS**. Preserve the closed original numeric exports and record owner feedback on video, audio, controls and exit separately.
4. Repeat freshly with **G45**, rumble still OFF, using the same marking/duration. Verify actual output geometry and profile identity; do not assume scene/server equivalence from the requested settings.
5. Compare non-crossing STABLE NEW intervals with their exact exposure denominators, binding/retirement and inbox omissions, NACK dispatch/result, authenticated arrival, worker STORED, AU/recovery and native/RGB/NEW separately. Qualify overwritten/missing detail, queued-return age and open episodes. Local result-to-arrival times are not RTT or causal retransmission proof.

If regression occurs, close the app and reinstall the verified baseline artifact built from `6d26bce`; start a fresh session and preserve the candidate exports. A fresh G0 repeat is available if the pair warrants confirmation. Preserve historical tags/assets. No physical-rumble trial or broader architectural change belongs in this comparison.
