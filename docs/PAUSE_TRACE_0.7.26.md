# XCloud4 0.7.26 — bounded pause trace

**Native package built and retrieved artifacts/source verified; console export and owner capture are pending.** This version instruments the existing **two-reader** 0.7.25 pipeline. It does not implement the four-reader proposal, change recovery, increase queues or change the worker budget. Four-reader work is deferred to 0.7.27.

The purpose is to distinguish an observed pause in new displayed pictures from local packet admission, AU recovery, native Decode, copy, conversion and presentation activity. No cause or performance improvement is established by adding the hooks.

## What counts as a pause

A pause is a **strictly greater than 100,000 microsecond interval between observed successful live NEW-generation flip completions**. Completion is the existing `VideoOut` condition `status.flipArg == submitted_flip_arg`. The local observation time uses `sceKernelGetProcessTime`; it is not the physical scanout instant or a provider timestamp.

The drawn generation belongs to the RGB slot actually leased by the main thread. A newer worker publication cannot replace that tag during the display call. Freshness compares this drawn generation with the last successful presentation; a failed flip ends the existing main loop without counting a NEW completion. Repeat-generation/UI refreshes do not reset the NEW-image cadence. The monitor runs before the main idle `continue`, during display waiting, on the video worker and on video ingress; a pause can be detected while no new picture arrives.

Intentional closing/inactive states disarm cadence. Activation alone does not create a pause before the first actual NEW completion. Startup, first-copy byte verification and decoder restarts have explicit records. An interval still open at stop is incomplete evidence, not a fabricated completed pause.

## Capture limits and coverage

The optional trace is allocated once per media context; a compile-time bound keeps the whole allocation below **8 MiB**. Allocation failure disables tracing while retaining playback. The source reserves:

| Storage / policy | Bound |
| --- | ---: |
| Rolling numeric history | 8,192 records |
| Saved windows | 12 |
| Records per window | 16,384 |
| Requested prehistory | 500 ms before the preceding NEW completion |
| Requested posthistory | 500 ms after a gap closes |
| Window time cap | 2 seconds after window opening |
| Record size | 32 bytes |

A trace producer attempts a **trace-only gate once**, then records or counts an admission drop. It does not spin, sleep, allocate or write a file to obtain admission. Opening a window performs a bounded scan/copy of up to 8,192 history records while holding that diagnostic gate; this can add overhead and make other producers miss records. Existing queue/mailbox gates are released before event recording.

The capture marks short prehistory, record-admission drops, event/time/storage caps, unstable cadence snapshots, missing reconstructed gap details, open-at-stop intervals, identity errors and ordinal wrap. A cap never becomes an apparently complete interval. Sustained pauses can outlast their saved window. After twelve windows, aggregate counts continue while further detailed windows are unavailable.

The fixed counters survive history overwrites and window caps:

- Eight stage rows: event attempts, admissions and trace-gate drops.
- 512 event rows: the same counts by numeric event. Index is `stage * 64 + item` for namespaces 0..7 and items 0..63; invalid events use the reserved fallback row.
- 64 primary-reset attempt counts, derived **only** from `AU_RESET.b >> 48` (reasons 0..62; other values become unknown 63).
- Authoritative media reset/discard/damage totals printed in fixed numeric rows after successful closure. These differ from event counts: several events can describe the same AU, and trace attempts are observations rather than unique frames.

Admitted records still enter rolling history even when a saved window cannot retain them. A trace-gate drop, missing event or sequence skip is not itself missing RTP or lost video. Absence-based classification must be UNKNOWN when relevant coverage is incomplete.

## Identity and valid associations

| ID | Meaning / limit |
| --- | --- |
| AU ID | A local ordinal when an AU opens, including rejected AUs |
| Decode-call ID | A local ordinal at the actual native call; feed can fail before that call |
| Native-output ID | An independent ordinal for each validated native picture; preserved across decoder restart |
| Decoder epoch | A local start ordinal |
| Copy ID | One selected-copy invocation |
| RGB generation | Existing monotonic completed-publication identity |

The native input ABI has input PTS/DTS, but the output ABI contains validity, geometry and buffer fields **without returned PTS**. Therefore native output is not attributed to the most recently submitted AU. Decode begin/end may identify the submitted input; output IDs remain independent. `OUTPUT_VALID` contains decoder epoch, deliberately no input/call-ID linkage.

The same explicitly leased pending native buffer can connect **validated output → copy → conversion → RGB generation → actual drawn generation → matched flip completion**. NoFrame calls retain the pending output ID. The native reservation-reuse guard still converts a pending output before overwriting it and still returns before Decode on preservation failure. A newer pending valid output can supersede an older pending output under the existing batching policy.

## Event payload schema

Each record is `{t_us:uint64, a:uint64, b:uint64, ordinal:uint32, event:uint16, flags:uint16}`. Native/error `rc32` values are unsigned 32-bit patterns interpreted as signed 32-bit values where appropriate. Numeric times, sizes, local IDs, seq16, geometry, flags and enum reasons are permitted. There are **no payload bytes, NAL bytes, credentials, SDP, IP addresses, pointers, SSRC or raw RTP/PTS/DTS timestamps**.

### RTP, queue, reorder and AU

| Event hex | Payload and flags |
| --- | --- |
| `0400 RX_VALID` | `a=seq16,b=packet_bytes`; marker 1, successfully queued 64 |
| `0401 RX_REJECT` | `a=packet_bytes,b=framing_subreason`; RTP_INVALID reason in flags high byte |
| `0109 QUEUE_DROP` | `a=seq16,b=sampled_depth`; exact SIZE/FULL/PUSH_CONTENDED reason in high byte |
| `0107 QUEUE_POP` | `a=(seq16<<32)|packet_bytes,b=arrival_dwell_us`; marker 1 |
| `0128 POP_CONTENDED` | `a=sampled_depth,b=0`; no packet was popped or dropped by this observation |
| `010d REORDER_DROP` | `a=seq16,b=0`; FOREIGN reason |
| `0111 SOURCE_EPOCH` | `a=local_track_epoch,b=first_seq16`; initial 0 / prior track locked 1, no source identifier |
| `010a REORDER_INSERT` | `a=(expected_before<<32)|seq16,b=buffered_before`; result STORED 0 / LATE 1 / JUMP 2 |
| `010c REORDER_HOLE` | `a=(expected_before<<32)|skipped_count,b=buffered_before`; eligible DEPTH 1 / TIME 2 / both 3, allow-expire 4 |
| `010b REORDER_EMIT` | `a=seq16,b=packet_bytes`; marker 1 |
| `0100 AU_BEGIN` | `a=AU_ID,b=first_seq16`; waiting 1 |
| `0120 DAMAGE_FIRST` | `a=AU_ID,b=first_damage_bits`; once per AU |
| `0121 IDR_SEEN` | `a=AU_ID,b=0`; complete type 5 NAL assembled |
| `0101 AU_COMPLETE` | `a=AU_ID,b=AU_bytes_before_prefix`; FU-active 1 / damaged 2 / empty 4 / IDR 8 |
| `0122 SUBMIT_BEGIN` | `a=AU_ID,b=feed_bytes_including_existing_prefix`; waiting 1 |
| `0123 SUBMIT_END` | `a=AU_ID,b=feed_rc32`; success does not prove a native output |
| `0102 AU_RESET` | `a=AU_ID,b=(reason<<48)|(detail<<32)|damage_bits`; state flags below |
| `0126 WAIT_BEGIN` | `a=AU_ID,b=0`; caller reason, initial startup specifically flag `0x8000` |
| `0127 WAIT_END` | `a=successful_recovery_AU_ID,b=0`; ends at existing successful-feed branch |
| `010e WORKER_YIELD` | `a=native_Decode_calls_this_batch,b=batch_elapsed_us`; exact NONE/STOP/DECODE-budget/TIME-budget reason observed at a budget check |
| `010f / 0110 MEDIA_START / STOP` | start `a=2,b=16000`; stop `b=worker_close_rc32` |
| `0105 PLI_REQUEST` | `a=consumed_local_request_count,b=0`; not proof of RTCP transmission |
| `0124 RESET_TOTAL` | `a=reason,b=count`; all reset calls flag 0, dropped-open-AU increments flag 1 |
| `0125 DAMAGE_TOTAL` | `a=damage_bit_index,b=discarded_open_AUs_containing_bit` |

Framing subreasons preserve the old parser branches: 1 initial argument/size/version/RTCP-header rejection; 2 CSRC overrun; 3 short extension header; 4 extension-length overrun; 5 invalid padding; 6 empty payload. Exact queue reasons come from the actual branch, not differences between concurrently changing counters. Seq16 forward/equal/backward observations can be classified only over continuous unambiguous observations; they are not proof of wire-level loss. Known foreign/source changes or missing capture invalidate that attribution.

### Reset, damage and waiting for IDR

`AU_RESET` flags: gap 1, AU-open 2, FU-open 4, IDR 8, damaged 16, empty 32, waiting-before 64 and waiting-after 128. The primary reasons are:

| Reason | Actual path / detail |
| ---: | --- |
| 0 | Normal submitted AU; gap=false |
| 1 | Timestamp changed while previous AU remained open |
| 2 | Marker incomplete; detail FU 1 / damaged 2 / empty 4 |
| 3 | Waiting for recovery; detail missing IDR 1 / SPS 2 / PPS 4 |
| 4 | Existing parameter-prefix capacity check |
| 5 | Feed error; rc in SUBMIT_END |
| 6 | Consumed existing coalesced ingress-gap flag |
| 7 | Initial/new-track branch; detail prior-track-locked 1 |
| 8 | Reorder jump |
| 9 | Reorder hole skip; detail depth/time eligibility bits |

The worker's coalesced ingress flag is not falsely associated one-to-one with a callback drop. The callback records its exact parse/queue branch. If both depth and time made a reorder skip eligible, both are recorded without claiming which one uniquely caused the policy decision.

Damage bit indices 0..12: forbidden NAL bit; invalid NAL type; single/STAP while FU active; short STAP length; zero/out-of-bounds STAP length; trailing STAP bytes; bad FU header; nested FU start; orphan FU continuation; FU header mismatch; Annex-B append capacity; continuation of an already discarded timestamp; unsupported/short packetization. Bits can overlap. The first diagnostic event and accumulated reset mask remain distinct; overlapping bit counts must not be summed as exclusive damaged AUs.

The existing printed **`damaged` counter is broader than malformed payload**: `reset_au(gap=true)` increments dropped_frames whenever an AU is open. Timestamp/reorder gaps, incomplete fragments, waiting for IDR or missing SPS/PPS, capacity and feed failures can therefore increase it while queue-full is zero. The trace distinguishes these paths; this static counter definition does not establish the cause of any specific observed pause.

WAIT_BEGIN records false→true recovery transitions and separately marks initial startup. WAIT_END occurs only when the existing successful feed ends waiting. IDR_SEEN means a type 5 NAL was assembled, including FU completion; it is not a decoded/displayed keyframe. Neither local request consumption nor successful feed proves actual PLI dispatch or provider response.

### Native Decode, copy and conversion

| Event hex | Payload and flags |
| --- | --- |
| `022a / 022b VIDEO_START / STOP` | `a=decoder_epoch,b=0`; stop is an attempt, failure appears separately |
| `0229 VIDEO_ERROR` | `a=decoder_epoch,b=rc32`; no stage string |
| `022c FEED_REJECT` | `a=input_AU_ID,b=feed_size` |
| `022d / 022e PRESERVE_BEGIN / END` | `a=pending_output_ID`; begin `b=triggering_AU_ID`, end `b=rc32`; triggering input did not necessarily produce output |
| `0200 DECODE_BEGIN` | `a=call_ID,b=submitted_AU_ID` |
| `0201 DECODE_END` | `a=call_ID,b=native_rc32`; input-accepted 1 / output-valid 2 / output-error-frame 4 |
| `0204 OUTPUT_REJECT` | `a=observed_call_ID,b=epoch`; validation group 1..4 |
| `0203 OUTPUT_NONE` | `a=observed_call_ID,b=epoch` |
| `0202 OUTPUT_VALID` | `a=independent_output_ID,b=epoch`; observed metadata 64 |
| `0228 OUTPUT_GEOMETRY` | `a=output_ID,b=(pitch32<<32)|(height16<<16)|width16` |
| `0205 OUTPUT_SUPERSEDED` | `a=old_pending_output_ID,b=new_output_ID` |
| `0206 COPY_BEGIN` | `a=Copy_ID,b=leased_output_ID`; proposed parallel 1 / serial 0 |
| `0207 COPY_END` | `a=Copy_ID,b=selected_wall_us`; actual parallel 1 / serial 0, job failure 2 |
| `0220 / 0221 OWNER_SPAN_BEGIN / END` | `a=Copy_ID`; begin `b=span_bytes`, end `b=0`; parallel 1 / serial 0 |
| `0222 / 0223 HELPER_SPAN_BEGIN / END` | immutable local `a=Copy_ID`; begin `b=span_bytes`, end `b=rc32`; end invalid-job 1 / success 0 |
| `0224 / 0225 HELPER_WAIT_BEGIN / END` | `a=Copy_ID`; begin `b=output_ID`, end `b=wait_us`; no per-poll event |
| `0226 / 0227 BYTECHECK_BEGIN / END` | `a=Copy_ID`; begin `b=bytes`, end `b=NOT_CHECKED 0 / PASS 1 / MISMATCH 2`; end reason NONE 0 / NO_HELPER 1 / SPANS 2 / MEMORY 3 / JOB 4 |
| `020c / 020d CONVERT_BEGIN / END` | `a=leased_output_ID`; begin `b=0`, end `b=convert_us` |
| `020e RGB_PUBLICATION` | `a=monotonic_generation,b=converted_output_ID` |
| `020f RGB_SUPERSEDED` | `a=old_ready_generation,b=replacement_generation` |
| `0300 / 0301 DRAW_BEGIN / END` | `a=actual_claimed_generation,b=its_output_ID`; NEW 4 / REPEAT 8 relative to successful presentation |

Output validation groups preserve the old accepted format and checks: 1 native error-frame; 2 codec/width/height/pitch bounds; 3 returned size/staging/alignment; 4 non-owned native reservation. Odd pitch remains permitted. Native Decode durations, selected copy wall, conversion and observed display completion are independent local measurements, not input-to-output or end-to-end latency.

Helper END is captured **after publishing completion**, with its immutable local job ID. Scheduling can therefore place a wait-end/conversion record before this helper-end record even though its data copy is complete. The owner still waits for completion before conversion, another Decode/reuse or teardown. The first serial byte comparison remains separately marked and excluded from selected-copy time. COPY_BEGIN's proposed mode can become serial when the first-check allocation fails; COPY_END describes the actual selected mode. Existing SSE2/SSE4.1 bodies, feature guard and CPU fences are retained.

### Display and reconstructed gaps

Display events `0303 FLIP_SUBMIT`, `0304 GNM_DONE`, `0305 FLIP_MATCH`, `0306 PRESENT_NEW`, `0307 PRESENT_REPEAT`, `0308 FLIP_FAIL` surround the existing calls. FLIP_MATCH contains submitted flip argument and observed `status.num`; the latter is **not asserted to be FPS/VSYNC/source-frame count**. PRESENT_NEW contains actual generation and NEW serial. Repeat matches retain current serial without cadence reset. Submit/Gnm/status/timeout failures have distinct numeric reasons.

GAP_BEGIN `0502` has `a=pause_ID,b=preceding_NEW_completion_us`; its record time is detection. GAP_CLOSED `0503` has the same ID and exact MAIN-observed interval duration; its record time is the following NEW completion. Reconstructed/late flags describe a gap recovered after the next completion rather than observed while open. Multiple unseen closures are counted as missing detail. The offline parser must preserve these limits.

## Export and lifecycle

After transport callbacks and auth use have stopped, close disarms cadence, stops/joins the video owner and copy helper, joins audio and checks native teardown. Only then does it end the trace and attempt exclusive creation of **`/data/xcloud4-trace-0726-<local_numeric_session>.bin`**. The name uses local numeric process time, not an account/title/provider-session identifier. `fopen("wbx")` has no overwrite fallback; a collision or I/O failure is reported separately and does not change playback close rc.

A failed join or native teardown retains media/native/staging and trace memory; no export/free occurs with uncertain producer lifetime. The trace pointer is explicitly preserved across decoder-start structure clearing. RGB/native IDs survive restart without exposing native pointers. No live file/network output is added by trace recording.

The offline tool **[`scripts/analyze_pause_trace.py`](../scripts/analyze_pause_trace.py)** validates schema dimensions, bounds, counter consistency and retained record identity. It reports coverage, observed stage activity, separate before/after-decoder timing distributions, exact reset observations and retained worker-exit reset/discard/damage totals. A missing exit row is unknown, not zero. Each window reports its own gap-ID range; other delayed gap metadata copied into prehistory stays context. The tool does not infer AU→output attribution, decode-stage inactivity from missing events, or service/network causation from local sequence gaps.

## Current verification state

Actual Claude Opus 5.5 completed the integrated review (29 turns / 28 Read calls) and a fresh correction review (22 turns / 21 Read calls), finding no material defect in the reviewed scope. Native compilation completed, the retrieved package/ELF/OELF/eboot match their VM hashes, and all 70 frozen build inputs match before and after compilation on both host and VM. The matching dependency-source archive was fully retrieved and its 9,369 payload files verified. These checks do not establish console behavior.

Compiled inspection confirms the 6,570,008-byte optional allocation, the two-reader design, native imports and the linked libc's exclusive-create mode. Actual exclusive trace export and owner capture remain pending. Diagnostic overhead, file-system success, hardware scheduling and pause classification require that capture. There is no speedup or fixed-stutter claim for this version.
