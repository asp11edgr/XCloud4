# XCloud4 0.7.13 — fixed message categories

This checkpoint adds a local, bounded keyword summary of a direct service error message. Helper scope, focused read-only reviews, full build, matching source and transfer integrity are confirmed. **Two console attempts have distinct outcomes: a 2458-byte HTTP 200 response fails validation without keyword output; a later logical refusal reports `ConnectionExchangeFailed` and message mask `0x00010400`, referencing the SDP exchange command without identifying its underlying cause. No valid remote answer, corrected negotiation or game video/audio is established.**

## Previous confirmed evidence

The 0.7.12 console capture identifies the public service code **`ConnectionExchangeFailed`**, inside an errorDetails object carried by an **HTTP 200 / 243-byte** SDP response after successful keepalive. The message is a string, with its text hidden. Remote cleanup succeeds and no RTP/game media is received. The terminal GET introduced in 0.7.12 is not entered because keepalive succeeds. See [0.7.12 exact evidence](RELEASE_NOTES_0.7.12.md).

Claude Opus 5.5's read-only analysis of that evidence completed in **7 turns / 5 reads**. It did not establish a precise failure cause and suggested restricted message categories as a further diagnostic. That analysis is separate from the new-helper review recorded below.

## Frozen diagnostic scope

The original change is limited to `src/auth/xbox_live.c`. The helper examines only a direct `errorDetails.message` string: errorDetails must be an object, its message member must occur exactly once and have string type. Existing strict JSON parsing/member checks reject duplicate or inappropriate fields.

The **1025-byte temporary buffer** accepts **1–1024 decoded ASCII bytes**. NUL, other controls and non-ASCII are rejected, with **CR/LF/TAB** allowed. The raw JSON string span includes its quotes; spans above **6146 bytes** (`6 * 1024 + 2`) skip decoding. That raw length can still be recorded numerically. The helper reports only validity, raw span length, decoded length and an **18-bit fixed keyword mask**. Missing, duplicate, wrongly typed or invalid messages produce validity zero and an empty mask. The whole buffer is securely cleared on every path; message text, arbitrary provider response data, SDP and credentials are never printed.

Case folding is explicitly ASCII and independent of locale. ASCII letters/digits/underscore define word boundaries, preventing a term such as `ice` from matching inside `service` or `device`. Phrase spaces accept one or more ordinary spaces, CR, LF or TAB. The fixed categories are:

| Bit | Keyword category |
|---|---|
| 0 | timeout / timedout / timed out |
| 1 | SDP |
| 2 | offer |
| 3 | answer |
| 4 | codec |
| 5 | h264 / h.264 / profile |
| 6 | ICE / candidate |
| 7 | DTLS |
| 8 | parse / parsing / invalid |
| 9 | unsupported / not supported |
| 10 | command |
| 11 | target |
| 12 | null / reference |
| 13 | exception |
| 14 | expire / expired |
| 15 | authorization / unauthorized / forbidden |
| 16 | PerformSdpExchangeV1Command |
| 17 | PerformIceExchangeV1Command |

A set bit records a recognized term's presence. It does not identify the operation that failed, distinguish a cause from surrounding context or demonstrate a codec/ICE/timeout defect. An empty mask would likewise not establish success or an absent cause.

Existing restricted public-code and numeric diagnostics remain. Network flow, business parsing decisions, SDP/media settings, retry/cancellation/cleanup behavior, queue limits and the 30-second dispatch-based heartbeat cadence remain unchanged.

## Metadata and evidence status

Product version is **0.7.13**, with PS4 package **APP_VER 00.83**. RTC peer inspection and Claude Opus 5.5's focused read-only review completed without a material finding in the reviewed helper/decoder scope. Claude's new-code review completed in **3 turns / 2 reads**; it is distinct from the preceding protocol investigation. No runtime result is inferred from either review.

## Package and matching source

The full application/package built without warnings or errors:

- Package: `XCloud4-0.7.13.pkg`, identifier `XCLD00001`.
- Size: **8912896 bytes**.
- SHA-256: `4f2e4568435ae1e2e354d4655ad51db9b28eabe827c581a64d8c27afb9edd0b4`.
- VM, PC and PS4 FTP retrieval hashes match.

Application checkpoint `1d3a3d13cc73aa1cfdd3b3e313a778cc4694fa49` supplies the authentication diagnostic separately from the corresponding dependency/native-adapter archive, closed from the frozen mirror:

- Filename: `XCloud4-0.7.13-dependency-sources.tar.gz`.
- Size: **83562288 bytes**.
- SHA-256: `9d54b7c68564b06ea5d2da8cd4654a5045e1669d2d5386cfe06140790c574037`.
- All **9367** manifest source hashes verified locally, covering **150842717 source bytes**; the PC archive hash matches the guest archive.

Pinned vendors, RTC adapters and configuration are unchanged, with matching notices. Generated artifacts, Git metadata, local credentials and logs are excluded; original public upstream fixtures remain. Both application source and the dependency/native-adapter archive, with retained licenses, accompany the package checkpoint. Every earlier snapshot stays immutable.

## Console result

### First attempt: invalid response before SDP application

The console identifies the installed application as **0.7.13**. Native DNS lookup returns **0** in **15948 microseconds**, with the nonzero-address boolean **1**. The current offer is **1286 bytes / two candidates**, with three media sections and three BUNDLE entries. After accepted SDP submission, **33** pending answer polls return **HTTP 204**.

Keepalive at **29459 ms** succeeds with **HTTP 200 / 37 bytes**. The next SDP request returns **HTTP 200 / 2458 bytes**. No public-code, errorDetails or message-keyword diagnostic is emitted. The final application stage reports an invalid SDP response, error **`X4_AUTH_E_RESPONSE` / `0xFFFFF82A`**, before remote SDP is applied. The response length and HTTP status do not establish a valid answer; the schema/decoder validation failure needs investigation.

The new helper therefore supplies no observed message mask in this attempt, and `ConnectionExchangeFailed` is not identified here. Remote deletion returns **HTTP 200**, with no RTP, video or audio received. The owner reports the session error. This is distinct from the second attempt below.

### Second attempt: SDP command named in a logical refusal

The owner repeats the same installed **0.7.13** version. Native DNS lookup returns **0** in **10484 microseconds**; the current offer is **1286 bytes / two candidates**. `POST /sdp` returns **HTTP 202**, followed by **38** pending answer polls returning **HTTP 204**. Keepalive dispatched at **29758 ms** succeeds with **HTTP 200 / 37 bytes**.

The normal SDP GET then returns **HTTP 200 / 243 bytes**, poll **39**, at **30872 ms**. The direct public code is **`ConnectionExchangeFailed`**. The new keyword diagnostic records **valid 1**, raw JSON span **156 bytes**, decoded message **154 bytes**, mask **`0x00010400`**. Only **bit 10 (`command`)** and **bit 16 (`PerformSdpExchangeV1Command`)** are set. No message text is logged.

The mask identifies the operation referenced by the message, not the underlying cause. It does not establish an ICE, codec, timeout or parsing defect; unset keyword bits do not rule those out. This remains a logical refusal, not a successful remote SDP answer. ICE/RTC/video/audio counters are **zero**, remote deletion returns **HTTP 200** and no game media is received. Package/source/NOTICE snapshots are unchanged across both attempts; no corrected negotiation is established.

No automated tests were added or run. The PS4 UI stays Spanish, repository documentation stays English and GitHub stays private. See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
