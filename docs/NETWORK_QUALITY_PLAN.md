# Network quality measurement plan

**Status: research and proposed design. No network quality check is implemented by this document.** Existing console playback and local pause traces do not establish media RTT, Internet capacity or a network/server cause for pauses.

## Measure the connection actually used

XCloud4 obtains a validated regional HTTPS origin from Xbox's offering response. That origin serves catalog and session preparation requests. The selected ICE peer carries the game's media and is a separate destination. A previous catalog region must not be assumed to identify a later game's media path.

The first useful implementation would observe the owner's normal game start and live traffic:

| Stage / metric | Meaning and limitations |
| --- | --- |
| Preparation request time | Measure already required requests with the same monotonic clock, separating operations and results. This includes HTTP/TLS/service processing and is not media ping. |
| Video/audio received Mbps | Count validated RTP bytes before the local queue, separately by kind. Report packet or payload bytes explicitly. Current stream traffic is not maximum Internet speed or spare bandwidth. |
| Receive sequence quality | Account for validated source, wrap, restart, duplicates and late arrival. Separate packets not observed at the callback from local queue rejection and expired reorder entries. A missing observation does not identify where it was lost. |
| RTP arrival jitter | Use validated RTP timestamps and negotiated clock rates in an independent passive estimator. Callback/OS scheduling contributes to the observation. Existing exported pause traces lack the timestamps required for this calculation. |
| Local processing | Preserve queue residence, recovery, copy and actual new-picture presentation measurements. These are not network RTT. |
| RTT | Unavailable through the current native C wrapper. A future native backend needs a verified metric and lifetime contract. Input send success is not an Xbox acknowledgement or input-to-photon latency. |

Use bounded summaries and infrequent numeric updates, with no per-packet printing, file writes or extra blocking in the media callback. Record measurement gaps and source/path epochs; unknown results must remain unavailable. Keep the playback profile, region, queues and recovery unchanged during measurement. A download saturation test must not compete with the game.

## Lessons from pinned clients

Better xCloud uses browser `getStats()` to read the selected candidate pair's RTT, received bytes and video counters. Its displayed JITTER value is average jitter-buffer residence, which differs from RTP interarrival jitter. Those browser APIs are not present in the PS4 client. GreenVita also uses the service's default region; that discovery does not certify quality.

libdatachannel's public C++ RTT and byte methods delegate to SCTP. They do not provide video/audio RTP byte totals. The current C API exposes addresses/candidate selection but lacks browser-style statistics. A future SCTP metric must be labelled as data-channel RTT; it cannot silently replace selected-ICE RTT or full gameplay delay.

An unmerged [Better xCloud proposal](https://github.com/redphx/better-xcloud/pull/1007) times an opaque HTTP request to a provisioning route. It is outside the pinned release and is not an official Xbox probe contract. No independent harmless regional benchmark endpoint was established in this investigation. Do not automatically change region from HTTP timing.

## Results and presentation

A future passive trial could retain a bounded 30–60 second sample window. Report median, p95 and maximum only from retained individual samples, with sample count, elapsed time, failures, exclusions, resets and coverage limits. Repeatedly reading a stale RTT does not create new probes.

Loading status should distinguish service acceptance from waiting for live quality samples. During play, show optional diagnostics when requested. Describe measured disruption without asserting its cause. Xbox [recommends 20 Mbps for consoles, PCs and tablets](https://www.xbox.com/en-US/cloud-gaming); that recommendation does not certify this homebrew client or supply a guaranteed RTT/jitter threshold.

## Primary source evidence

The comparison used these fixed revisions; no reference implementation was copied:

| Project / license | Revision | Inspected evidence |
| --- | --- | --- |
| Better xCloud / MIT | `f8397043f6d2148d2345d508902a38c69cf1ee20` | [Collector](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/stream-stats-collector.ts), [display](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/modules/stream/stream-stats.ts), [network handling](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/network.ts), [counter types](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/types/stream-stats.d.ts) |
| libdatachannel 0.24.6 / MPL-2.0 | `bdc5ff28e9d3b863144c94a677ecf5bf043aaf15` | [Public methods](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/include/rtc/peerconnection.hpp), [SCTP delegation](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/peerconnection.cpp), [C bindings](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/src/capi.cpp), [C API reference](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/DOC.md) |
| GreenVita / MPL-2.0 | `ae2625d295b4fba005a769b1309fd70dcd6cb63f` | [Default-region discovery](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/auth.rs) |

[W3C WebRTC Stats](https://www.w3.org/TR/webrtc-stats/) defines candidate-pair RTT, per-source jitter and buffer residence as distinct metrics. [RFC 3550](https://datatracker.ietf.org/doc/html/rfc3550) defines RTP sequence accounting and arrival-order jitter, including restart, wrap and duplicate handling. These definitions specify meaning; they do not guarantee a native getter on PS4. A synchronized one-way timestamp or media RTT cannot be inferred from an isolated sender report.
