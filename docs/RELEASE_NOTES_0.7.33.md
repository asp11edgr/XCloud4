# XCloud4 0.7.33 — controlled receiver NACK trial

## Status

**Prepared and compiled; console comparison pending.** This release tests repair support and repair utility separately. It does not establish that the server received feedback, that later packets were caused by NACK, or that playback pauses have been fixed.

Preparation began from `d550518988abbb85eb9634be406dfd0ddcd36d04`. A live fetch found no newer local or remote application commit. The preserved [0.7.32 report](CONSOLE_REPORT_0.7.32.md), original traces and installer remain unchanged. Reanalysis with the final reader reproduces 14 video gaps / 202 positions, 600,534 valid application callbacks and ten stable pauses in 630.924914 seconds, with all 643 critical records retained.

## Experiment

One original-video receiver generator observes authenticated, validated PT102 RTP after the existing receiving handler and before application reorder. It sends compound RR + SDES + Generic NACK through the protected RTCP/SRTCP transport and observes the actual local transport boolean. Acceptance is checked against the actual remote Answer. No RTX or SDP change is included.

Each fresh session reserves at most **64 attempts**, each for a new range of at most **64 positions**, with **16 watched ranges**, cancellation and **no retries**. OFF keeps the observer/dispatcher and suppresses requests. Requests, selected positions, protection, local send, later authentication, reorder and AU acceptance have separate bounded numeric records and omission counters. No payload, credentials or complete SDP is exported.

NEW/MPSC256, four copy readers, 960×540 / 30 FPS / 5,000 kbps request, 16 ms / four Decode feeds, reorder **128 / depth 32 / 25 ms**, SRTP replay window 1024 and recovery remain unchanged. Actual delivered dimensions must be measured again. Depth can abandon a hole before 25 ms; late returns do not establish absent retransmission support. A recovery-accepted AU is not automatically a decoded or presented image because the decoder does not return output PTS.

See the [full experiment, evidence contract, test commands and comparison procedure](RECEIVER_NACK_EXPERIMENT_0.7.33.md).

## Artifact identity

Native source identity: `e9a0b21e02d059559c4a57bed8f80db183f86021`, derived from 74 named native source/build inputs. OFF and ON embed respectively `-nack0` and `-nack1`. Both use PS4 APP_VER **01.03**, TITLE_ID **XCLD00001** and replace the same installed application.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| `XCloud4-0.7.33-nack-off.pkg` | 8,978,432 | `cff808534bca6c58c1449fd7cf36e1ae5aae443afd9abf951796e874001d9687` |
| `XCloud4-0.7.33-nack-on.pkg` | 8,978,432 | `0c9c5e659e537f3e2c4d6cf8a3b78ece7e7ee20a000a41f13db2861412ae1dc2` |
| `XCloud4-0.7.33-dependency-sources.tar.gz` | 83,613,499 | `e217cf21b2f577c497e4c22c3c1899659a45a4ce1ab82bb6b7e9f9281ba2ca94` |

The dependency archive verifies **9,381 payloads** against its manifest and checksum list. It includes the new vendor generator as well as its matching canonical application copy, port recipes, overlays, pins and licenses. The separate project-source ZIP is taken from the reviewed application commit. SDK binaries, private traces, host logs and credentials are excluded from public assets.

Both native applications compile and pass selected linked inspection. Retrieved package, ELF, OELF and eboot bytes match their respective build receipts; all 74 frozen inputs match the current source. The final rebuild changes only source-export inclusion and its identity relative to the preceding compiled candidate, reusing unchanged verified dependency objects. Inspection is static and does not execute the application or prove runtime scheduling, audio/input behavior, closure, retransmission or diagnostic overhead.

## Verification

- Receiver state: GCC/Clang plain **24 cases / 164,357 checks** each; GCC ASan+UBSan pass.
- Production wire: GCC/Clang plain **12 cases / 54,666 checks** each; GCC ASan+UBSan pass.
- Production handler: GCC/Clang plain **17 cases / 508 checks** each; GCC ASan+UBSan pass.
- Final Python reader: **105 cases pass**.
- Portable monitor, MPSC ingress and receive bridge: corrected GCC/Clang plain and GCC ASan+UBSan/TSan campaigns pass in their covered scopes. Monitor TSan's atomic-fence modeling warning remains a limitation.
- Clang sanitizer campaigns are **unavailable** because runtime libraries are missing; no sanitizer pass is claimed for them.
- Actual Claude Opus 5.5 reviewed the initial integration and its corrections. The final diagnostic rejection flags and exporter inclusion were independently reviewed and rebuilt; they were not part of a third Claude review.

Earlier test-fixture/compile failures are preserved privately and superseded by corrected passing runs. These tests do not execute a PS4 network session.

## Trial and reversal

Use one fresh application process per game session. Test OFF first, then ON with the same game, scene and network, **10–15 manually stable minutes each**. Mark stable play after loading with **L1 + R1 + TRIANGLE**; toggle the mark for transitions and after they finish. Return with **L1 + R1 + CIRCLE**, then close from the catalog with **OPTIONS**. Preserve both paired exports and build identities.

Compare stable pauses/minute, NEW-image cadence distribution, recovery and AU discard reasons, requests, authenticated first returns, confirmed late rejection, usable AU contribution, return timing and actual record coverage. Check audio, controls, return and closure in both sessions. A local successful send is not a delivery acknowledgment; one pair is not a reproducible improvement claim.

To reverse requests, close, reinstall OFF and launch freshly. To restore the prior console checkpoint, reinstall the preserved [0.7.32 release](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.32). The Makefile defaults to NACK OFF; clean application objects when rebuilding another mode. No reorder margin is increased in this release. A separate bounded margin trial would require measured late-return evidence first.
