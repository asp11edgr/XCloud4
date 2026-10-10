# 0.7.24 console playback and remaining pauses

Product **0.7.24**, PS4 **APP_VER 00.94**, application identifier **XCLD00001**. The owner reports more fluid video and responsive controls, interrupted by recurring brief pauses before normal playback resumes. The tested title was not identified. This is a subjective report, not a complete controller or audio-quality validation.

The current source baseline is [`6e8d89dee089bc3fbfc0c9748d96d68bedb31f79`](https://github.com/asp11edgr/XCloud4/tree/6e8d89dee089bc3fbfc0c9748d96d68bedb31f79). The first valid H.264 picture is **1280×720, pitch 1280**, despite the **960×540 / maximum 30 FPS** request. Requested capabilities do not establish the dimensions or cadence actually delivered by Xbox.

## Bounded local measurements

The completed frozen selection contains **134 complete report windows totaling 670.472978 seconds** after playback begins. Startup and the first busy partial interval are excluded; the selection does not represent the entire session. Rates divide selected counter deltas by selected elapsed time. Stage means divide accumulated stage time by its operation count.

| Measurement | Completed selection |
|---|---:|
| Selected duration | 670.472978 s |
| New image draws / second | **17.886** |
| Local Decode calls / second | 40.771 |
| Mean Decode duration | 8.377 ms |
| Mean WC→cached copy duration | **29.382 ms** |
| Mean NV12→RGB conversion duration | 3.003 ms |
| Mean accepted-packet residence in ingress ring | **198.154 ms** |
| Maximum sampled ingress residence | **703.080 ms** |
| Maximum local converted-picture completion gap | 726.949 ms |

The complete selection records **11992 new draws**, **16 repeated draws**, **1944 full-queue events**, **2193 dropped-packet increments**, **2200 lost-packet increments**, **12953 `damaged` increments**, and **3490 `pli` increments**. Repeated draws are uncommon in these windows, while copy cost and recovery pressure remain.

The separate [0.7.23 selection](ERROR_REPORT_0.7.23.md) reported **13.775 new image draws/s**, **29.349 ms copy** and **3.003 ms conversion**. The 0.7.24 selection has a higher local new-draw rate, but these are different uncontrolled live samples, **not an A/B benchmark**, a guaranteed improvement or server/panel FPS. Copy duration remains approximately unchanged.

## What the counters mean

- **`picture_age_us` starts after local conversion.** It excludes earlier ingress residence, assembly and decoding; it is not capture-to-display latency or Xbox RTT.
- **Queue residence begins at accepted local RTP ingress.** Its oldest-packet snapshot is sampled independently. It measures a local packet queue, not network jitter, AU age or input latency.
- **`damaged` counts parser AU resets/discards**, rather than visibly damaged decoded pictures. Missing packets, rejected reconstruction and recovery gating can increment it.
- **`pli` counts pending local keyframe requests consumed by the main thread.** Requests are coalesced again by authentication/session state. Explicit application calls to the transport are gated at **at least 500 ms**; the count is not the number of RTCP PLIs sent, received or acknowledged. The transport also attempts an Xbox keyframe channel message. Actual dispatch counts/results and recovery time until a valid IDR are not measured. See the [local request counter](https://github.com/asp11edgr/XCloud4/blob/6e8d89dee089bc3fbfc0c9748d96d68bedb31f79/src/media/live_media.c#L664-L668), [session gate](https://github.com/asp11edgr/XCloud4/blob/6e8d89dee089bc3fbfc0c9748d96d68bedb31f79/src/auth/xbox_live.c#L2218-L2223) and [transport call](https://github.com/asp11edgr/XCloud4/blob/6e8d89dee089bc3fbfc0c9748d96d68bedb31f79/src/streaming/rtc_transport.c#L691-L695).

Aggregated sums/maxima cannot recover **p50/p95** distributions. AU/output timestamp association and actual source FPS remain unmeasured. Local Decode can drain queued work faster than new pictures arrive. H.264 RTP uses a **90 kHz** timestamp clock, but its cadence must be observed per complete AU with duplicate/gap/discontinuity handling before drawing a source-rate conclusion. [RFC 6184 §5.1](https://www.rfc-editor.org/rfc/rfc6184.html#section-5.1)

## Applicability of the six external review findings

The supplied review used an older 0.7.23 runtime. These statuses refer to the packaged 0.7.24 baseline.

| Finding | Status in 0.7.24 |
|---|---|
| WC copy is the dominant measured local stage | **Still applicable.** Moving work off main does not remove its approximately 29 ms cost. |
| Timing needs ingress/AU/output/presentation association | **Partly addressed.** Accepted ingress timestamps and queue residence exist; picture age starts after conversion. Reorder arrival still starts at dequeue. AU/output association and percentile histograms remain absent; the adapted native output ABI exposes no PTS. |
| Global ingress-gap reset can discard an intact older queued AU | **Candidate remains.** The batch resets before draining old entries. Conservative recovery may discard earlier intact work; its contribution to pauses is unproven. |
| Persistent parallel readback with byte equivalence | **Pending.** Two persistent compressed inputs and four native output reservations already exist; the WC copy remains single-reader. |
| Native 720p or 1:1 display | **Separate pending experiment.** Current 720p pictures are scaled to 1080p output. This changes drawing, not the WC copy itself. |
| Recovery requests versus actual PLI dispatch/time-to-IDR | **Interpretation corrected; diagnostics pending.** The existing `pli` label counts local consumed requests, with a separate 500 ms transport gate. |

The copy bottleneck, local backlog and recovery are supported observations. They do not prove a single network, server, decoder or parser cause for every pause. Removing the global gap reset requires separate FU-A/marker/SPS/PPS, sequence-wrap and IDR-recovery evidence. Enlarging queues or arbitrarily dropping predictive compressed AUs is not a demonstrated fix.

## Closure observed

The owner reported closing the application. The later capture records session cancellation, session DELETE **HTTP 200**, zero reported cleanup error, and closure of authentication, audio, decoder, controller and display resources. This supports completed application cleanup. The reviewer did not independently observe the PS4 home screen or obtain an explicit confirmation that no CE notice appeared.

## Proposed next isolated checkpoint

The next experiment is **two persistent CPU copy readers over disjoint spans**, preserving **16 ms / four Decode calls**, existing queues, native output type/bounds, prediction references, resolution, scaling and audio/input paths. Keep native APIs on the video owner; finish both readers before conversion, publication, another Decode/output reuse or teardown. Serial fallback, first-valid-picture byte equivalence and copy wall-time evidence must precede a performance claim. The proposal is **not part of packaged 0.7.24**, and a 0.7.25 package/result is not established by this report.

Pinned [Moonlight PS4](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c#L99-L224) provides a persistent-copy-worker concept; its cache/alias and output-lifetime contracts are not assumed compatible. [PSBox](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/app/stream_player.cpp#L227-L238) preserves Decode while omitting intermediate presentation. [AJ GeForce NOW PS4](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/webrtc/media.cpp#L167-L189) couples stale compressed-queue removal to IDR recovery. These inform separate concepts, not copied code, confirmed native compatibility or promised speedup. See the [full pinned comparison](CLIENT_VIDEO_COMPARISON_0.7.24.md).

The original [0.7.24 release notes](RELEASE_NOTES_0.7.24.md), tag and assets preserve their preparation-time status. This report adds later console evidence. Raw logs, account details and private review snapshots remain outside Git; no automated tests or application execution were performed by this report's reviewer.
