# XCloud4 0.7.31 — reception boundary evidence

## Scope and verified inputs

This is a read-only recheck of the original 2026-10-10 console capture and the corresponding 0.7.31 source checkpoint. It does not establish the cause of upstream discontinuities or report a new console trial.

| Preserved input | Bytes | SHA-256 |
|---|---:|---|
| Legacy trace | 2,570,400 | `a07b07c77fa4c19eba7a016a8ca53c259a54e8922ab08c11f2fd5d6e8b9d63c0` |
| Independent progress trace | 10,719,616 | `7a8a381f83326523c0cbd859db5fb9e565b6a55caf6bda77411f44b6dd95281f` |
| Exact dependency-source archive | 83,585,834 | `b306e040c696e4334cfee2cfd48f8569bd9267a77f5fe1aebc52698c71cdacda` |

The original bytes still match their retrieval receipts. A fresh read with the frozen 0.7.31 progress reader reproduces the complete saved analysis exactly. Its SHA-256 is `f8c3f85bfb4bfb6b40a447b328c253045c9c3a72797f457b66c85160b0994d3b`. Runtime input identity is `0caa54f34a994de1ffed125d27e47767062384b2`; the source checkpoint is `c1bacf372a139cdc8d4f136d7c609761be532856`. Raw traces and numeric source identifiers remain private.

## What the reception counters establish

| Observation | Total | Exact boundary or limitation |
|---|---:|---|
| Video sources in the callback observer | 1 | Local session + video kind + SSRC; not a count of raw socket sources |
| Valid video callbacks | 927,075 | Valid RTP seen inside `x4_live_media_receive` |
| Ingress admissions | 927,075 | Successful application queue admission |
| Queue pops | 927,075 | Valid RTP parsed after application dequeue |
| Reorder insertion attempts | 927,075 | Includes all insertion return codes in this counter |
| Reorder emissions | 927,075 | RTP emitted to the application depacketizer |
| Callback forward-skipped sequence positions | 759 | Sum of positive modular discontinuities, not 759 distinct episodes |
| Callback backward-or-duplicate observations | 0 | Combined observer category; original/RTX identities are not retained separately here |
| Sequence observation omissions / unknown observations / source-cap omissions | 0 / 0 / 0 | These cumulative observer fields are complete despite detail-ring omissions |
| Reorder window-jump attempts | 0 | Independent cumulative event counter; not absence inferred from an empty detail list |

The final retained independent sample has a coherent empty application ingress queue (depth 0) and the worker's last published reorder state has buffered 0. It also repeats all final 927,075 receive/admission/pop/emission totals. Final header counters agree. The last observed callback sequence and published reorder expectation differ by exactly one. These are explicit local witnesses; they do not inspect the upstream DTLS/Track queues.

The callback observer performs sequence comparison using `uint16_t(received - previous)`: 1 is contiguous; 2 through 32767 add `difference - 1` forward positions; 0 or 32768 through 65535 enter the combined backward-or-duplicate category. Wrap from 65535 to 0 is contiguous. This uses the RTP half-range convention; it cannot unambiguously interpret an unobserved jump of 32768 or more positions, or a sender restart reusing the same SSRC. A future per-gap ledger must preserve the local session/source epoch and classify that ambiguity rather than inventing an enormous missing range.

**The measured discontinuities already exist before or at this application reception boundary.** The 0.7.31 observer runs after libdatachannel's Track delivery, the C callback wrapper, XCloud4's callback entry/data gates and local RTP parsing. Therefore its 927,075 valid calls are not necessarily every Track event, every transport message or every socket datagram. They do not establish physical Internet loss. Server omission, local socket/transport scheduling, transport filtering, SRTP rejection and library delivery drops remain unseparated upstream possibilities.

No complete per-discontinuity identity ledger exists in 0.7.31. The 759-position cumulative count survives, but the retained RX detail records are only a subset. Subtracting sequences of two sparsely retained records would conflate recording omissions with reception holes. Individual ranges and their first upstream disappearance must be measured in the next run.

### `REORDER_DROP` is not a universal late-packet count

In this application, `MT_FOREIGN` aliases `X4_TRACE_REORDER_DROP` (event `0x10d`). Its call site records a packet rejected by `x4_live_track_accept` because its payload type or SSRC is foreign. A late, duplicate or occupied-slot rejection returned by `x4_live_reorder_insert` is encoded in the **return flags of `REORDER_INSERT`** and increments the broader `m->dropped`; it does not emit `REORDER_DROP`.

Thus `REORDER_DROP = 0` alone cannot exclude late insertions. Here, the equal insertion/emission totals, empty final reorder state, one source epoch and zero window-jump/reset-jump counters provide additional accounting evidence that the application reorder did not discard admitted packets in this run. That does not establish completeness of the stream presented to the callback. Source locations: [`live_media.c`](../src/media/live_media.c), aliases and `x4_live_track_accept`, `x4_live_reorder_insert`, `video_batch`, `drain_reordered`; [`live_monitor.c`](../src/media/live_monitor.c), `x4_monitor_rx` and cumulative event handling.

## Exact dependency checkpoint

The archive's `SOURCE_MANIFEST.json`, modified-path inventory and actual source version declarations establish these pins. The build uses libjuice, Mbed TLS and bundled libsrtp, with XCloud4's target patches retained in the corresponding archive. It does not use the OpenSSL or libnice implementation paths for this checkpoint.

| Component | Declared version / role | Pinned revision |
|---|---|---|
| libdatachannel | 0.24.6, Track/DTLS-SRTP | [bdc5ff28e9d3b863144c94a677ecf5bf043aaf15](https://github.com/paullouisageneau/libdatachannel/tree/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15) |
| libjuice | 1.7.4, ICE/STUN/TURN and transport sockets | [b89c792e3612faf2f12cf35bcc56857313a06be3](https://github.com/paullouisageneau/libjuice/tree/b89c792e3612faf2f12cf35bcc56857313a06be3) |
| libsrtp | 2.8.0, SRTP/SRTCP protection | [d33b8ffb1491a0b4b58a206889f09800cf7310ab](https://github.com/cisco/libsrtp/tree/d33b8ffb1491a0b4b58a206889f09800cf7310ab) |
| Mbed TLS | 3.6.7, DTLS handshake/key derivation and TLS | [068ff080b369adfac81509f9b57b2afabaf82dc5](https://github.com/Mbed-TLS/mbedtls/tree/068ff080b369adfac81509f9b57b2afabaf82dc5) |
| usrsctp | Data channels; not the RTP video decoder path | [fec583d54493f879d2ae44a743423bf8a04371ab](https://github.com/paullouisageneau/usrsctp/tree/fec583d54493f879d2ae44a743423bf8a04371ab) |
| nlohmann/json | SDP/signaling support | [55f93686c01528224f448c19128836e7df245f72](https://github.com/nlohmann/json/tree/55f93686c01528224f448c19128836e7df245f72) |
| plog | Library logging | [94899e0b926ac1b0f4750bfbd495167b4a6ae9ef](https://github.com/SergiusTheBest/plog/tree/94899e0b926ac1b0f4750bfbd495167b4a6ae9ef) |
| Mbed TLS framework | Mbed TLS source dependency | [dde0c4a0e448a0552f18817dcea633bb851fd288](https://github.com/Mbed-TLS/mbedtls-framework/tree/dde0c4a0e448a0552f18817dcea633bb851fd288) |
| Opus | 1.5.2, audio decoding | Official archive SHA-256 `65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1` |

The version labels come from the pinned source itself; a revision, rather than a current upstream release number, identifies usrsctp and the support submodules. See [`build_native.sh`](../scripts/webrtc/build_native.sh) and [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) for build configuration, patches and licensing.

## Actual source path before application reception

1. **libjuice socket receive.** UDP transport calls `udp_recvfrom`; the poll loop delivers bytes to `agent_conn_recv`/`agent_input`. ICE TCP support has a separate framed receive path. STUN/TURN processing can consume control traffic or unwrap relay traffic; unknown sources and invalid messages can be rejected before the application-data callback. The selected transport/relay route must be observed, not guessed from this source inventory.
2. **ICE application-data callback.** libdatachannel's `IceTransport::RecvCallback` creates a message and calls the lower transport receive chain. This occurs after libjuice filtering; it is not the raw socket boundary.
3. **Mbed TLS branch's incoming transport queue.** `DtlsTransport::incoming` uses `mIncomingQueue.tryPush`. A full queue can drop a message before SRTP handling. `enqueueRecv` schedules a library worker; `doRecv` takes the receive mutex. In Mbed TLS's `ReadCallback`, the queued message is popped and passed to `demuxMessage` before any DTLS content is supplied to Mbed TLS. Thus this incoming queue and scheduling boundary also precede SRTP media in the actual compiled backend.
4. **DTLS/SRTP/SRTCP classification and protection.** `DtlsSrtpTransport::demuxMessage` distinguishes DTLS from SRTP/SRTCP and unknown types. `recvMedia` rejects truncation and calls bundled `srtp_unprotect` or `srtp_unprotect_rtcp`. Replay, authentication and other failures return before media delivery. The DTLS handshake establishes SRTP keys; it does not decode H.264 video. No keys or decrypted payloads belong in diagnostic records.
5. **PeerConnection routing.** Successful media is forwarded through any global media handler, then dispatched by SSRC to a Track. Truncated control packets, missing Track mappings or exceptions have separate paths. The application does not install a global media interceptor here.
6. **Track/media handler.** Track direction filtering precedes `RtcpReceivingSession`. That handler can normalize configured RTX and consumes RTCP control. Structurally rejected RTP and handler exceptions can prevent forwarding. Remaining media enters Track's receive queue; queue fullness, pending-open state and callback availability can affect delivery.
7. **Track C callback and application entry.** Track queue flushing invokes the registered message callback synchronously. The C API wrapper calls `rtp_callback`; XCloud4 enters its lifetime gate, checks a minimum RTP header/version, obtains the media callback under its data gate, then calls `x4_live_media_receive`. The latter parses RTP, records the 0.7.31 observer and admits the application ingress packet.

Primary implementation references: [libjuice poll receive](https://github.com/paullouisageneau/libjuice/blob/b89c792e3612faf2f12cf35bcc56857313a06be3/src/conn_poll.c), [agent filtering](https://github.com/paullouisageneau/libjuice/blob/b89c792e3612faf2f12cf35bcc56857313a06be3/src/agent.c), [libdatachannel Mbed TLS transport](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/impl/dtlstransport.cpp), [SRTP transport](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/impl/dtlssrtptransport.cpp), [Track](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/impl/track.cpp). XCloud4's exact port modifications are in the preserved dependency archive, not inferred from upstream alone.

The public Track C API does not expose complete per-datagram counters for all these boundaries. Existing periodic library log counters are not complete per-session ledgers with packet identities. The next diagnostic therefore needs small, target-only hooks in the pinned sources for socket/classification, incoming-queue admission/pop, SRTP status, routing, handler outcomes and Track delivery. Those hooks must only observe existing decisions. A successful SRTP unprotect establishes an authenticated media packet at that boundary; a successful dispatch API return does not establish delivery over the network.

## NACK and RTX: announced, implemented and observed

| Mechanism | Verified 0.7.31 fact | What remains unknown |
|---|---|---|
| Video offer feedback | The actual local offer's sanitized log records PT 102 with `nack`, `nack pli`, `ccm fir`, `goog-remb` | Whether the remote answer accepts each feedback item was not retained |
| RTX in the local offer | Exactly one video PT, H.264 102; no offered RTX PT | A remote RTX offer/mapping was not logged; it must not be invented |
| Receiver NACK generation | The chained `RtcpReceivingSession` has no receiver NACK request generator; the application adds none | A new receiver NACK mechanism would require a separate implementation/negotiation trial |
| `RtcpNackResponder` | Implemented in the library but not chained by XCloud4; responds to incoming NACK using cached **outgoing** RTP, optionally RTX | Installing it on a receiving Track would not by itself implement missing-packet requests |
| RTX receive normalization | Implemented conditionally in `RtcpReceivingSession`, based on its description's PT/`apt` mapping | No upstream wire-RTX counter/OSN ledger exists for the run |
| Current Track RTX state | App-created local video description has no RTX. Existing-Track remote-description processing can disable local RTX, but does not add it | Future diagnostics should retain actual handler state and both offer/answer metadata |
| Keyframe requests | 205 local consumed requests; 89 actual local dispatch attempts/results, zero sampled cumulative local errors | No network delivery acknowledgment or server response-time association is measured |

`RtcpReceivingSession::updateSeq` maintains receiver-report statistics. Its return value is not used to discard otherwise valid RTP in `incoming`; probation or a false sequence-statistics result must not be labeled as a delivery rejection. Its keyframe method can emit FIR when the handler's description advertises FIR, otherwise PLI. The application's legacy `PLI` labels cover local keyframe demand/API attempts; they are not proof that every RTCP dispatch was a PLI on the wire.

Primary sources: [receiving session](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/rtcpreceivingsession.cpp), [sender NACK responder](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/rtcpnackresponder.cpp), [default video feedback](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/description.cpp), [existing-Track remote handling](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/impl/peerconnection.cpp). [RFC 4588](https://www.rfc-editor.org/rfc/rfc4588.html) specifies separate original and retransmission sequence spaces and the original-sequence-number association. A later diagnostic must retain wire RTX identity and normalized original identity separately, without payload storage.

No NACK responder, receiver NACK generator, RTX negotiation, reorder timing or recovery behavior is enabled or changed by this evidence review.

## Detailed retention failure verified from original windows

Pause numbers refer to the chronological >100 ms intervals in [the console report](CONSOLE_REPORT_0.7.31.md), including loading. Distinct windows repeat the same internal pause identity in three pairs.

| Window | Pause # | Prehistory records copied at open | Capacity left after prehistory | Total retained | Open-to-freeze |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 3129 | 967 | 4096 | 344.022 ms |
| 2 | 1, duplicate identity | 4096 | 0 | 4096 | 0 ms |
| 3 | 2 | 4096 | 0 | 4096 | 0 ms |
| 4 | 3 | 4096 | 0 | 4096 | 0 ms |
| 5 | 4 | 4096 | 0 | 4096 | 0 ms |
| 6 | 4, duplicate identity | 4096 | 0 | 4096 | 0 ms |
| 7 | 5 | 4096 | 0 | 4096 | 0 ms |
| 8 | 5, duplicate identity | 4096 | 0 | 4096 | 0 ms |

Seven windows froze immediately after copying 4096 prior events. All eight slots had been consumed by pause 5. The reader removed **12,288 overlapping record duplicates**. `window_omitted = 45` counts failed window-allocation/capture attempts, not 45 distinct pauses. The cumulative cadence contains 31 closed long intervals; its complete ledger is independent of these window failures.

The source's `open_locked` only checks whether a window is currently open and whether storage is full. It does not retain a completed identity set. A window that freezes on its event cap can be opened again when the same pause closes. Copying prehistory up to the entire event capacity leaves no guaranteed room for onset, progress, final state or closure.

The next diagnostic must deduplicate by pause identity, cap the prehistory contribution, reserve episode/post capacity, and keep a separate bounded critical record for sequence holes, actual discard reasons and recovery transitions. Each lane needs its own omission/overwrite counters. Increasing the all-packet event ring alone would not correct this observed retention behavior.

## AU assembly and decoder acceptance are different populations

The existing `AU_COMPLETE` event occurs when the RTP marker is seen **before** the checks for an active FU, damage or an empty AU. The existing `AU_VALID` event is emitted **after** structural checks, parameter-cache commit and the waiting-for-keyframe filter.

| Population | 0.7.31 total | Interpretation |
|---|---:|---|
| Marker-reaching AUs (`AU_COMPLETE`) | 57,339 | Includes structural failures and recovery-filter rejections |
| Marker-incomplete discarded AUs | 27 | Structural invalidity at marker |
| Waiting-recovery discarded AUs | 180 | Passed the structural check, then failed IDR/SPS/PPS recovery requirements |
| Accepted AU / Decode entries (`AU_VALID`) | 57,132 | Past those checks; output still has no returned PTS |
| Structurally complete before recovery filter | 57,312, inferred | 57,339 minus 27; no direct distinct counter in this checkpoint |

The inferred 57,312 agrees with accepted 57,132 plus waiting-recovery 180, with zero parameter-prefix-capacity and feed-error reset totals. It is accounting derived from the exact source paths and final counters, not a newly recorded pre-filter event. The 180 recovery-filter rejections are not 180 assembly failures. The next diagnostic must count these stages directly without moving or changing the filter.

Native output, RGB publication and new presentation remain distinct populations. No output PTS is returned by the native decoder, so neither an IDR observation nor the latest decoder input can be automatically associated with a particular native output or displayed image.

## Next evidence required

Keep NEW/MPSC admission, four readers, current requested/delivered video settings, reorder depth/time and recovery decisions fixed. The next comparable 10–15 minute stable run should capture sparse numeric discontinuity/late-arrival records and cumulative stage counters from the earliest accessible transport boundary through application entry, with actual source/session identities and omission coverage. A retained hole should be classified by its **first missing observed boundary**, or by the interval between two boundaries if visibility is insufficient.

Until that run is available, socket arrival, library discard and remote omission are not distinguished for the 759 positions. The evidence supports correcting diagnostic visibility and retention first. It does not yet select a causal playback repair.
