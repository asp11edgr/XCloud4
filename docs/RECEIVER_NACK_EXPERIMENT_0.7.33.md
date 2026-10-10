# XCloud4 0.7.33 — bounded receiver NACK experiment

**Subsequent console evidence (2026-10-10):** both fresh OFF/ON passes are complete. The [console report](CONSOLE_REPORT_0.7.33.md) records authenticated returns after local hole abandonment and no useful AU repair under this policy. The preparation-time status and contract below are preserved; they do not claim the later test had already been measured.

## Status and preserved evidence

This experiment adds one bounded original-video receiver NACK generator to test whether later authenticated packets can contribute to usable access units before the existing reorder/recovery decision. **No 0.7.33 console result or fluency correction is established yet.** Portable state, production-handler/wire and reader checks pass in the scopes below. Both native OFF/ON applications compile and pass selected linked inspection. Console behavior, diagnostic overhead and useful repair remain unmeasured.

Preparation started from reviewed application HEAD `d550518988abbb85eb9634be406dfd0ddcd36d04`, the 0.7.32 console-report commit. The initial live fetch succeeded: HEAD and `origin/main` matched, with no post-0.7.32 local/remote commit or uncommitted application change. Seventy runtime source files in the build mirror matched the preserved 0.7.32 source before staging the new work. The new experiment changes are separate from the preserved source, package, release assets and original private exports.

The [0.7.32 console report](CONSOLE_REPORT_0.7.32.md) remains unchanged: **630.924914 seconds / 10.515415 manually stable minutes, 10 NEW-image intervals >100 ms, 0.950985/min**, with stable p50/p95/max **33.044 / 49.952 / 500.984 ms**. Its complete critical ledger retains **643/643 events**, **14 video gaps / 202 absent sequence positions**, and **two audio gaps / three positions** already visible at the positive UDP-read boundary and corroborated downstream. Eight stable pauses overlap recovery; the other two remain incompletely explained. Those observations cannot separate sender, network, NIC/kernel or socket behavior before the read.

The accepted answer announces video NACK, but the pinned 0.7.32 receive chain has no receiver request generator. This source mismatch motivates the experiment. It does not establish that the missing generator caused all pauses or that the server will retransmit requested positions.

## Configuration held constant

| Setting | Preserved value |
|---|---|
| Video ingress | NEW/MPSC, 256 entries |
| Copying | Four persistent readers |
| Requested stream | 960 × 540, maxFPS 30, 5,000 kbps |
| Previously delivered geometry | 1280 × 720; verify actual geometry again in both new passes |
| Worker | 16 ms budget, four Decode feeds per batch |
| Reorder | 128 entries, depth 32, 25,000 µs wait |
| Media/repair policy | Existing H.264/Opus, PLI, recovery and decoder-surface ownership |
| Negotiation | Existing SDP and codec/PT mapping; no RTX enablement |

The 25 ms setting is **not a minimum wait**. The existing reorder can skip a hole when depth reaches 32 before time expiry. In the 0.7.32 follow-up, eight of nine playback recovery clusters begin **2.426–3.472 ms** after the first socket gap observation; the remaining cluster begins about 27.2 ms later. Nine of twelve retained decisions are depth-only. These spans do not establish an available round-trip repair budget. A late authenticated return can be rejected by reorder or the recovery filter; that outcome alone does not prove lack of server NACK support. Presentation may already have stopped before the hole is recognized.

Dependency revisions stay pinned:

| Component | Revision |
|---|---|
| libdatachannel 0.24.6 | `bdc5ff28e9d3b863144c94a677ecf5bf043aaf15` |
| libjuice 1.7.4 | `b89c792e3612faf2f12cf35bcc56857313a06be3` |
| libsrtp 2.8.0 | `d33b8ffb1491a0b4b58a206889f09800cf7310ab` |
| Mbed TLS 3.6.7 | `068ff080b369adfac81509f9b57b2afabaf82dc5` |

The remaining pins and Opus archive hash remain in `scripts/webrtc/export_source.sh`. SDK and dependency binaries remain external to Git.

## Receive and protected feedback path

`prepare_receiver_nack.py` overlays the pinned vendor source. Its explicitly added owner APIs are XCloud4 extensions, not claimed upstream libdatachannel APIs:

- `x4_rtc_configure_receiver_nack(track, enabled, epoch)` installs exactly one video observer/generator and rejects an unexpected or incompatible handler chain.
- `x4_rtc_receiver_nack_accept(track, peer_connection)` checks the actual PeerConnection **remote Answer**, matching Track MID and original video PT102/H.264 with accepted `nack` feedback.
- `x4_rtc_poll_receiver_nack(track)` performs a bounded dispatch from the owner sender context.
- `x4_rtc_close_receiver_nack(track)` retires requests and exports final counters during shutdown.

The local Track description advertises the offer; it is **not evidence that the answer accepted feedback**. Requests are gated by the actual Answer check. A local check error leaves the fresh generator's acceptance gate false and records RTC_SETTING24; media startup continues normally in either experiment mode. The error does not establish that the server lacks NACK support. The receiving handler's `incomingChain` executes its next link first. The generator is therefore **prepended** as root with `next = existing RtcpReceivingSession`, so ReceivingSession runs before generator observation. Appending it would reverse the intended execution order.

The generator accepts only authenticated original video RTP, excluding RTX provenance and other payload types. It validates version, fixed header, CSRC, extension, padding and nonempty payload boundaries before updating sequence state. The existing SRTP replay policy stays unchanged; replay/authentication failures remain separate observed branches.

Reception performs bounded state observation and request reservation. It does not send, allocate a feedback packet, sleep or print. A separate dispatcher releases the receive-state gate before packet construction and transport calls. The packet is a minimal compound **RR + SDES(CNAME) + Generic NACK**, using modular PID/BLP packing, a session feedback sender SSRC and the original video media SSRC. It follows the existing Track/DTLS-SRTP/ICE path and `srtp_protect_rtcp`; it does not bypass encryption or send raw plaintext feedback directly to a socket.

Protection result and the propagated transport boolean are separate **local API outcomes**. Neither is a network acknowledgment or proof that the server received the NACK. SRTCP_RESULT counts bytes in the protected copy; LOCAL_SEND_RESULT counts the original plaintext message, so these byte counts can differ for one attempt. The wire construction follows [RFC 4585 Generic NACK](https://www.rfc-editor.org/rfc/rfc4585#section-6.2.1); compatibility with this session still requires the actual trial.

## Bounds, cancellation and lifecycle

- At most **64 request attempts per fresh transport session**, reserved before dispatch. One attempt per newly observed eligible missing range, **no retries** and no refunded budget after failure or cancellation.
- Each range contains at most **64 positions**; at most **16 ranges** remain watched. Oversized gaps, exhausted budget, evictions and retired positions have explicit cumulative counters.
- Source/sequence continuity handles ordinary wrap, duplicates, SSRC changes and half-span ambiguity. A missed receive-state observation invalidates continuity and retires watches, so a local observer omission cannot manufacture an upstream hole. SSRC changes and resynchronization do not reset the session attempt budget.
- Serialized dispatch selection removes positions already observed. The selected bitmap is recorded independently of the original missing range. Later arrivals can race packet construction and protection after selection; selection is not atomic send.
- ON and OFF run the same observer and dispatcher. OFF observes gaps and suppressions without reserving requests. The dispatcher requests a **500 µs sleep**, which is a scheduling request, not a promised deadline.
- Receive/dispatch gate failures, pending/watch evictions, close cancellation, unavailable thread ticket slots, reorder-ticket mismatch and AU-ticket capacity have explicit counters. Missing or overwritten detail prevents absence claims.
- Shutdown joins the dispatcher before retiring the generator. Incoming observation never waits for shutdown. Numeric packet tickets follow the existing ingress publication and reorder ownership. An unfinished AU discarded on close has its own shutdown flag, rather than being called recovery completion.

Fresh launches with one game session per application process retain the existing process-global trace-binding limitation. Cross-session asynchronous transport attribution is not repaired by a request epoch alone.

## Numeric evidence contract

X4PROG2 retains its existing **32 event slots / 4,096 critical records**. Added slots 18–30 carry numeric observations only. Request association uses original source/PT plus full session-epoch/request identity; raw source identifiers stay private. Extended sequence is source-local and is not a universal packet ID.

| Event | Observation |
|---|---|
| NACK_DETECT | Inclusive missing range and forward received position |
| NACK_REQUEST | Reserved request identity/range and detection clock |
| NACK_DISPATCH_MASK | Exact pending bitmap selected under the state gate and selection clock |
| NACK_DISPATCH | Dispatch attempt for that identity, preserving the original range and request clock |
| NACK_SRTCP_BEGIN / RESULT | Protection entry/result, byte count and transport dispatch clock |
| NACK_LOCAL_SEND_RESULT | Boolean local transport result or explicit exception/unavailability |
| NACK_AUTH_RETURN | Later authenticated original position, first/duplicate/completed-range flags and request reference |
| NACK_REORDER_RESULT | Existing stored/rejected/window-jump result, with explicit behind-expected/occupied-slot/other rejection flags |
| NACK_AU_CONTRIBUTION | In-progress contribution, structural completion, acceptance by recovery before Decode feed, recovery filtering or discard |
| NACK_STATE / SUPPRESS / COUNTER | Configuration/lifecycle, exact suppression and cumulative snapshots |

The reader reports selection and actual transport dispatch clocks separately. Actual dispatch references come from consistent retained SRTCP/local-result records; otherwise that reference is unavailable. Later record timestamps are not assumed identical to the earlier embedded operation clocks. Returns before dispatch remain observations and are not automatically classified as cancellation or retransmission response.

An authenticated later position might be delayed/reordered original traffic. Its appearance does not prove NACK delivery or retransmission causality. AU stage2, `accepted_by_recovery_for_Decode`, is emitted **before** the feed call: the recovery filter accepted the AU. It proves neither a successful feed nor native Decode output, RGB publication or NEW presentation. A subsequent feed failure can legitimately leave stage2 and stage4 discard observations for the same AU; both facts remain in the chronology. The decoder has **no returned output PTS**; compressed input is never assigned automatically to the latest native image. PLI local requests, actual dispatch attempts and local results remain separate.

Only a non-closing NACK_STATE bearing **flag16** observes a completed actual remote Answer check. Bit2 then supplies its accepted/rejected result. Attach/media STATE records without flag16 leave acceptance **unknown/null**, even if bit2 is present. The summary's `answer_checked`, `remote_feedback_accepted` and `effective_requests_enabled` use a retained checked result in the current configuration epoch; no checked result means unknown, not rejection or effective activation. Missing records prevent a final-state claim. A closing STATE cannot replace this result or change the configured mode.

Counter102 combines non-authenticated messages and messages flagged as retransmission/RTX-normalized provenance; it is not a count of authentication failures alone. Counter109 counts omitted ticket setter/getter **operations**, potentially twice for one packet, and is not a lost-packet count. SUPPRESS can encode either a request or gap identity; an ordinal collision alone does not associate a suppression with a request. SUPPRESS reason6, `feedback_not_confirmed`, covers an unknown/unavailable check as well as a completed negative result; the suppression alone does not establish rejection.

Reorder result code1 (`late_or_occupied_slot`) is not enough to prove arrival after the worker abandoned a position: the existing function also returns it for an occupied slot or invalid input. Only flag1 records that the sequence was behind the worker's expected position before insertion. Flag2 records an occupied slot at a nonnegative difference; flag4 records another/unclassified rejection. Flag0 or conflicting flags leave the reason unknown. Result0 and result2 are admitted results, with result2 retaining its window-jump meaning. The reader counts confirmed behind-expected observations separately from other/unknown rejections; none of these outcomes proves that NACK is unsupported.

Post-dispatch observations include duplicates and remain in the raw chronology. First-arrival repair candidates and post-dispatch completion require an explicit first-arrival flag without a duplicate flag, after the stated dispatch boundary. A duplicate appearing after dispatch cannot turn an earlier authenticated arrival into a newly repaired position. If the first record was omitted, the later duplicate supplies no missing first-arrival timing. Unknown/conflicting flags stay observations. These candidate counts still do not prove retransmission or a presented image; raw AU/reorder observation counts also must not be read as counts of unique repaired images.

`all_selected_positions_have_first_authenticated_arrival_after_dispatch` concerns the validated **selected bitmap**, after already observed positions were removed, and requires first-arrival flags for each selected position. It does not cover every position in the original reserved range. `all_original_range_positions_observed_in_retained_authenticated_returns` separately describes raw retained observations across that range, including observations before dispatch; it is not post-dispatch repair completion. The unpublished misleading `all_requested_positions_observed_in_retained_authenticated_returns` alias has been removed before release.

`upstream_diagnostics.receiver_NACK` reports stage records, range/mask validation, request/return spans, exact coverage and counter snapshots without summing repeated cumulative values. Older v2 captures lacking NACK_STATE report NACK instrumentation unavailable, rather than an experiment proven disabled. A closing STATE does not change the recorded mode. No payload, token, key, complete SDP or private address is added to public documentation or diagnostic records.

## Comparable console procedure

1. Begin with **OFF**: close the existing application, install `XCloud4-0.7.33-nack-off.pkg`, launch freshly and confirm the displayed version/mode. Preserve package hash, exported Build ID and actual configuration. Authorize and open one game.
2. After initial loading finishes, hold **L1 + R1** and press **TRIANGLE** once to mark stable gameplay. Play **10–15 minutes after that mark**, using the same scene/action plan. Observe visible pauses, audio and controls.
3. At any new loading/cinematic transition, use the same combination to mark transition; repeat when stable play resumes. Unmarked transitions remain in the manually stable denominator. Describe the scene and marks rather than inferring them afterward.
4. Return with **L1 + R1 + CIRCLE**, then close from the catalog with **OPTIONS**. Preserve paired exports, exact hashes, export return codes and coverage counters. Do not open another game in this application process.
5. Repeat from a fresh launch using **ON**, `XCloud4-0.7.33-nack-on.pkg`, the same game/scene, duration, marking and exit procedure. Do not change readers, reorder, recovery, network settings or resolution request during this pair.

Compare the two new passes with each other and the preserved 0.7.32 stable population. Report the exact stable seconds/minutes used as denominator, closed NEW intervals strictly **>100 ms per minute**, cadence count/p50/p95/max, pause chronology, recovery count/duration/reasons and AU discards/filtering by reason. Separate startup, transitions and stable play; report any unfinished pause and exclusions.

For every retained request, distinguish detection, reservation, selected positions, dispatch, protection, local send, authenticated return, reorder admission and AU utility. Report how many selected positions reappear before/after the actual dispatch boundary, how many are explicitly behind the worker's expected position, how many have occupied-slot/other/unknown rejection reasons, and how many contribute to structurally complete/recovery-accepted AUs. Relate those facts to native output, RGB and presentation only through observed chronology with the PTS limitation. Include all watch/gate/ticket/critical omissions, cap exhaustion, retained bounds and missing association evidence.

Stop and retain rollback if ON causes audio/input/return/closure regression or materially worse stable cadence. A useful repair candidate requires observed usable AU progress before the existing recovery decision and fewer recovery-associated pauses, with comparable exposure and no guardrail regression. One OFF→ON pair cannot establish causality under changing server/network conditions. Counterbalanced or A/B/A repetition is a later step if needed; no improvement is declared resolved until replicated evidence supports it.

## Preparation verification and pending work

| Check | Current preparation evidence |
|---|---|
| Receiver state | GCC and Clang plain: 24 cases / 164,357 checks each; GCC ASan+UBSan pass |
| Python reader | 105 cases pass, including distinct clocks, selected masks, omissions, source/epoch separation, first/duplicate returns, reorder rejection reasons and shutdown |
| Monitor, ingress and receive bridge | Corrected GCC/Clang plain campaigns pass; GCC ASan+UBSan and TSan campaigns complete without reported sanitizer failures |
| Clang sanitizers | Unavailable because required sanitizer libraries are absent; no pass claimed |
| GCC monitor TSan limit | Standalone atomic-fence modeling warning retained; this does not establish PS4 fence behavior |
| Production Generic NACK wire | GCC/Clang plain: 12 cases / 54,666 checks each; GCC ASan+UBSan pass |
| Production generator/handler | GCC/Clang plain: 17 cases / 508 checks each; GCC ASan+UBSan pass, including reverse chain execution, no allocation/send in incoming observation, acceptance gate, cancellation, caps and fresh-session close |
| Vendor overlay | Combined preparation is idempotent across 206 selected files; current/legacy and missing/duplicate-anchor fixtures pass within their stated scope |
| Native dependencies | OpenOrbis v0.5.4 / Clang 21 build passes; final pair reuses these unchanged dependency objects |
| Native OFF/ON application and linked inspection | Both compile, package and pass selected artifact/import/ownership inspection; no application execution is implied |
| OFF/ON console behavior and NACK utility | **PENDING** — no 0.7.33 console result yet |

The corrected host logs supersede the earlier GCC enum-comparison, test-link and handler-fixture compile failures. Checks include the unchanged v2 header/capacities, current cumulative-counter mapping, watch retirement after omitted observations, selected bitmap cancellation, and shutdown/ticket lifetimes. Host checks establish only their covered portable contracts, not native scheduling, receive/dispatch gate contention on PS4, diagnostic overhead, server retransmission behavior or pause reduction. Request reservations, protection and native-output interpretation remain separate even when their aggregate counts agree.

Actual Claude Opus 5.5 completed an initial selected-source review and a second review of its corrections. The final added reorder-rejection labels and source-export inclusion received independent source review and native rebuilding, rather than another Claude run. Review is bounded by the files inspected; it is not console validation.

Reproduce portable checks from the project root with GCC/Clang and the pinned, prepared dependency tree:

```sh
CC=gcc bash scripts/test_receiver_nack_state.sh
CC=gcc bash scripts/test_progress_monitor.sh
CC=gcc bash scripts/test_video_ingress.sh
CC=gcc bash scripts/test_rtc_receive_diag.sh
X4_LIBDATACHANNEL_SOURCE="$X4_WEBRTC_ROOT/libdatachannel" CXX=g++ bash scripts/test_receiver_nack_wire.sh
X4_LIBDATACHANNEL_SOURCE="$X4_WEBRTC_ROOT/libdatachannel" CC=gcc CXX=g++ bash scripts/test_receiver_nack_handler.sh
python3 -m unittest discover -s tests -p progress_trace_test.py
```

See [release evidence](RELEASE_NOTES_0.7.33.md) for the final native identity, exact artifact hashes, corresponding sources and remaining trial status.

## Rollback

The Makefile defaults to `X4_RECEIVER_NACK=0`. A fresh application rebuild with that value disables requests while preserving the observer/dispatcher. Clean application objects before switching compile-time modes, and keep the mode, artifact hash and displayed label consistent. The already verified **0.7.32 installer and release assets remain preserved** as the prior console checkpoint. Keep both experiment installers and private exports distinguishable; do not replace original baseline evidence.

For a direct console reversal, close the application, reinstall the OFF package and launch freshly. Reinstall the preserved 0.7.32 package to return to the prior console checkpoint. Do not switch modes within an active game session.

## Decision after the pair

If authenticated requested positions return mainly after confirmed behind-expected rejection, the next single proposed experiment is a separately bounded reorder margin based on measured dispatch-to-first-return times. No value is chosen or enabled here. An observed local send without authenticated returns is insufficient to locate failure at the server, and an authenticated return alone is insufficient to establish a useful repaired image. Keep the two unexplained roughly 100 ms baseline pauses separate from this repair experiment.
