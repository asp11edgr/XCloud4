# XCloud4 0.7.32 reception diagnostic test

This test follows the RTP sequence discontinuities observed before the application's video ingress in 0.7.31. It adds observations inside the existing transport libraries and improves diagnostic retention. The playback configuration remains the 0.7.30 queue with four persistent copy readers, the current resolution request, the same reorder limits, and the same recovery decisions.

The [verified 0.7.31 evidence](RECEPTION_EVIDENCE_0.7.31.md) establishes 927,075 valid video callback observations and equal application queue and reorder totals, with 759 forward skipped sequence positions. Those positions were absent when the callback observer advanced. The original capture cannot establish whether they reached the socket, were rejected locally before the callback, or were absent upstream.

## Receive stages

The pinned libdatachannel 0.24.6 build uses libjuice 1.7.4, libsrtp 2.8.0 and Mbed TLS 3.6.7. In this configuration, encrypted media enters the Mbed TLS transport queue before SRTP authentication and decryption. The queue already had a branch that rejects input when full or stopping. This test records that branch without changing its capacity or scheduling; the rejection itself does not distinguish those two conditions.

The diagnostic patch is reproduced by `scripts/webrtc/prepare_receive_diagnostics.py`, called by `prepare_port.py`. It changes the pinned sources before rebuilding their static libraries. Public libdatachannel APIs do not expose all these observations. The patch adds two numeric fields to the existing internal Message object and a read-only snapshot of the receiving handler's RTX configuration. The OpenOrbis SDK remains separate from these project patches.

| Observation | What it establishes | Important boundary |
| --- | --- | --- |
| DATAGRAM | Positive UDP socket receive result, byte count, and candidate fixed RTP header | Kernel NIC and socket receive queues before this return remain unobserved. Outer TURN traffic has a different population. |
| TCP_FRAMED | A complete framed TCP input passed to libjuice | Does not count every TCP read or establish retransmission timing within TCP. |
| TURN_DECAP | Valid TURN data or ChannelData after wrapper processing | Candidate media identity becomes visible here for relayed traffic. |
| ICE_DELIVER | libjuice media delivered to the ICE transport callback | Rejection counters cover the patched inactive, unexpected, unknown-source and parse branches. |
| DTLS_QUEUE_ACCEPT | Successful admission to the existing Mbed TLS queue | The hook after admission can run after a concurrent consumer. Observation times are not a total ordering across threads. |
| DTLS_QUEUE_POP | Input extracted by the Mbed TLS read callback | The preceding timestamp measures residence since the producer's pre-admission timestamp, including scheduling. |
| SRTP_INPUT | Media passed to SRTP processing, before authentication | Fixed header metadata is untrusted. |
| SRTP_VALID | RTP accepted by `srtp_unprotect` | SRTP failures retain the exact numeric libsrtp status, including authentication and replay outcomes. |
| RTCP_VALID | Control accepted by `srtp_unprotect_rtcp` | RTCP is excluded from RTP sequence observation. |
| PEER_DISPATCH | Media received by PeerConnection dispatch | No-track and handler exception branches have separate reasons. |
| TRACK_INCOMING | Input delivered to a Track | Direction and handler rejection remain distinct. |
| TRACK_QUEUE_ATTEMPT | Input about to be pushed into the Track receive queue | This is an attempt, not an acceptance acknowledgment. Queue-full rejection is recorded separately. |
| TRACK_DELIVERED | Input popped from Track before variant conversion | The application callback may subsequently reject or classify it. |
| PEER_HANDLER_OUT | RTP boundary observations inside the receiving handler | Wire RTX and normal RTP retain their different identities. These are not unique-packet counts: unavailable RTX unwrapping can observe the same wire packet twice as the existing branch continues. |
| RTX_NORMALIZED | An observed RTX packet successfully unwrapped to original RTP | A numeric correspondence records original and RTX SSRC, PT and sequence without recording payload bytes. |
| APP_CALLBACK | Video or audio arriving at XCloud4's RTC message callback | The existing application ingress observer remains a separate, later observation. |

The socket hook preserves `errno`. Expected would-block outcomes increment their counter without filling the sparse critical ledger. The fairness-limit continuation branches have no diagnostic hook in this patch. All producers use fixed header inspection, bounded source/range tables, atomic counters and try-only gates. They do not allocate memory, sleep, print, write files, or add a packet to a new queue. Existing library allocations and waits remain existing behavior. The bridge drains outstanding diagnostic readers only during session shutdown.

The actual cost in PS4 scheduling requires the comparable console pass below. Portable tests establish structure, bounds and lifetime behavior; they do not establish native performance. Window compaction uses a fixed 32 KiB temporary array when freezing a window; native sampler stack headroom has not been measured by these portable tests.

The bridge observes the process's currently bound trace. Library peer deletion schedules asynchronous teardown; draining entered diagnostic readers protects storage lifetime but does not prove that old transport work cannot arrive after a new session binds. This pass therefore starts from a fresh application launch and uses one game session. Across multiple sessions in one process, a late old hook cannot be attributed reliably without a further owner/epoch link at ICE and Message creation. That link is a separate diagnostic extension. A binding-count summary records this boundary explicitly.

UDP receive failures and the existing socket-error readiness branch are observed. Fatal poll returns, POLLNVAL, TCP reads/errors before complete framing, and weak Track expiration are not covered by universal counters. The enum reserves reason 24 (`ICE_CALLBACK_EXCEPTION`) and reason 26 (`SOCKET_FAIRNESS`), but the pinned patch has no emitting hook for either. Their allocated raw counter cells are preserved, including zero; the reader marks their measured outcome as unavailable rather than treating zero as a measured absence.

Minimum additional observations would be numeric hooks in the existing `IceTransport::RecvCallback` exception handler and in the UDP/TCP fairness-limit continuation branches. Hooks at fatal poll, POLLNVAL, pre-framing TCP errors and weak-Track expiration would fill the other listed blind spots. Distinguishing reason 8's two conditions would require an observation inside `Queue::tryPush` where the limit and stopping conditions are available. These are separate native diagnostic extensions, not changes included in this package. The public C API does not expose them. Kernel or NIC receive drops require supported native socket/network statistics, whose availability is not established by this interface.

`REJECT` records and the reason counters identify an observed source branch; the record name alone does not prove that a packet was discarded. Interpret these cases with the actual branch behavior:

| Reason | Interpretation in this pinned patch |
| --- | --- |
| 8, `DTLS_QUEUE_FULL_OR_STOPPING` | `tryPush` returned false because the queue was full or stopping; the hook does not distinguish those conditions. |
| 12, `DEMUX_UNKNOWN` | The unknown-type branch was reached in demultiplexing. Its `SRTP_INPUT` stage label is the branch location, not proof that the packet first reached the separately counted SRTP input observation. |
| 21, `RTX_UNWRAP_UNAVAILABLE` | Unwrapping returned no replacement. The existing upstream branch continues and may still push the original wire RTX packet; the added pre-unwrapping and output observations can count that same tuple twice at `PEER_HANDLER_OUT`. This is not proof of two arrivals or a packet discard. |
| 24, `ICE_CALLBACK_EXCEPTION`; 26, `SOCKET_FAIRNESS` | Reserved counter slots without hooks. Their outcomes remain unknown. Fairness continuation itself gives another socket a turn and does not establish a drop. |

Authentication failure, replay failure and old replay window failure use separate reasons 28, 29 and 30. Their numeric libsrtp statuses are 7, 9 and 10 respectively; other SRTP failures keep their exact local result, including cipher failure 8, with the SRTP error reason. SRTCP uses the control stage and the same specific categories, with its own generic error branch. This is a count of the library branch outcome, not a diagnosis of physical loss or a universal count of packets missing from the callback.

## Gap identity and coverage

Each observed discontinuity contains the common monotonic time, stage, SSRC, PT, extended expected and received sequence, and the skipped count. Wire sequence is the low 16 bits. Wrap advances the stage's local extended high-water sequence. Exact half-range ambiguity or a missed sequence observation invalidates that stage's continuity and clears its watched ranges with counted evictions.

Each stage/source watches 16 recent missing ranges. Later observations identify the range's diagnostic ID, its creation time and the extended position. The first 64 positions have a uniqueness bitmap; farther positions carry an explicit uniqueness-unknown flag. Range eviction, source-table exhaustion, sequence-gate omissions and critical-ring overwrites are reported separately. An absent later record does not establish permanent loss.

Extensions initialize and resynchronize independently at each stage. Equal numeric ranges are correlation candidates until their epochs and source mapping can be aligned. For TURN, compare the decapsulated population with ICE, rather than treating the outer socket count as a count of media packets. Original RTP and wire RTX remain separate by SSRC and PT. A candidate clear header before SRTP authentication is never labeled trusted media.

The critical ring retains the latest 4,096 sparse events. Per-type attempts and omissions survive admission contention and overwriting. Detailed pause windows retain at most 512 prehistory records, use a body limit of 3,072 including prehistory, and reserve 1,024 records for the end boundary and rolling post-context. The same pause ID cannot open another window. Context selection and overwrites have their own omission counts. Retained details cannot supply negative evidence for an interval whose coverage is incomplete.

The binary files contain numeric observations only. No media payload, password, key, token, or complete SDP is added to these diagnostic exports. Raw console logs remain private because unrelated existing logging may contain session information.

The reader accepts the preserved v1 files and the new v2 appendix. Its helper-span matching now reads actor identity from the upper flag byte. Previous original files, reader snapshots and saved analyses remain immutable; a fresh reanalysis has a distinct reader hash and output name.

## NACK and RTX evidence

The existing local offer announces video NACK feedback, but `RtcpReceivingSession` in this pinned version does not generate receiver NACK requests. `RtcpNackResponder` handles requests on the sender side; XCloud4 does not chain it. Announced feedback therefore does not establish an implemented retransmission request path.

The prior local offer did not offer a video RTX payload. Its accepted remote feedback and RTX mapping were not retained in 0.7.31. Version 0.7.32 records numeric offer and accepted-answer codec/PT, `apt`, FID and feedback summaries, plus the handler's current RTX configuration and any observed normalization. It does not change SDP, enable RTX, add NACK requests or install a parallel feedback mechanism. Missing or overwritten setting records remain unknown.

## AU and presentation evidence

Three cumulative counters distinguish structurally complete access units before the recovery filter, rejection by that filter for missing IDR/SPS/PPS, and acceptance for the existing Decode feed. The filter also has an independent eight-entry reason histogram. Recovery rejection is not an assembly failure.

Local keyframe requests, actual API dispatch attempts and local API results are separate events. API success does not establish delivery or an acknowledgment from the server. Seeing an IDR NAL does not establish a complete accepted or presented image. Native output, RGB publication, RGB consumption, flip submission and flip completion remain separate facts. The native decoder does not return output PTS; output is not attributed to the most recent Decode input.

## Comparable console pass

1. Close any existing XCloud4 process, install `XCloud4-0.7.32.pkg` and launch it freshly. Confirm version 0.7.32. Record the build identity from its exported trace and the release artifact hash.
2. Authorize the account and open the same game and a comparable scene used for 0.7.31. Keep startup and loading separate from gameplay.
3. After loading finishes, hold L1 + R1 and press TRIANGLE once to mark stable gameplay. Play for 10–15 minutes after that marker.
4. Mark a transition within the same game at its beginning and mark stable gameplay again when it ends using the same combination. Describe the scene and any unrelated loading. Do not open a second game in this application process for this diagnostic pass.
5. Observe audio, controls and visible pauses. Return with L1 + R1 + CIRCLE; close from the catalogue with OPTIONS. Retrieve the paired trace files after this shutdown, preserving originals and recording hashes.
6. Compare stable-only gaps per minute, complete cadence distribution, recovery counts and the observed copy/Decode distributions with 0.7.31. Report the exact marked stable duration used as denominator. Check that instrumentation did not worsen audio, control response, return or close.

Resolution request remains 960 × 540, maximum 30 FPS and 5,000 kbps; 0.7.31 actually delivered 1280 × 720. The new trace must verify actual output geometry again. Four readers, MPSC capacity 256, reorder capacity 128, depth 32, wait 25 ms, worker budget 16 ms and four Decode feeds per batch remain unchanged.

The native export retains the historical filename prefix `/data/xcloud4-trace-0731-…bin`, with its paired `.progress.bin`, even for this release. Identify the 0.7.32 progress trace by its `X4PROG2` header, product version and build ID `152d863f3e21a2111101fcccd6c9094c52eb57a4`, not by the filename prefix.

## Report after the pass

For each retained sequence hole, give stage time, original or RTX namespace, expected/received/skipped range, explicit rejection and late reappearance evidence, and the first boundary that the evidence can identify. When observations only bound the problem between stages, state that interval. Do not turn missing sparse detail into a packet loss assertion.

Include detail coverage, critical omissions by type, sequence omissions, range evictions, source capacity omissions, actual sampler execution times, phase coverage and retained time bounds. Relate holes to presentation pauses only through retained chronology and stated limits. A receive gap cannot by itself distinguish server behavior, physical network loss or discards before the socket hook.

Select one subsequent functional correction only when the new evidence identifies a relevant branch or bounds a useful test. Receiver NACK implementation is a separate possible experiment because its announced support exceeds its implemented request path; it is not an established cause or a change included here.
