# XCloud4 0.7.12 — final SDP diagnostic

This checkpoint adds one bounded, diagnostic-only SDP request after a specific inactive-session keepalive failure. Source scope, focused read-only review, full build, matching source and transfer integrity are confirmed. **The console identifies a logical SDP refusal as `ConnectionExchangeFailed` after successful keepalive. The new terminal GET is not entered. No corrected negotiation, precise underlying cause, server lifetime or game video/audio is established.**

## Previous confirmed evidence

The 0.7.11 console attempt submits a current **1286-byte offer with two candidates**, followed by **38** pending HTTP 204 answer polls. Keepalive returns **HTTP 410 / 118 bytes**, root-code class **2 (`SessionNotActive`)**, without errorDetails. The restricted service-name helper is not entered. Cleanup returns HTTP 200 and no RTP/game media is received. The underlying earlier HTTP 200 / 243-byte logical SDP refusal remains uncaptured. See [0.7.11 exact evidence](RELEASE_NOTES_0.7.11.md).

## Frozen diagnostic scope

The original change is limited to `src/auth/xbox_live.c`. The final diagnostic is entered only when the normal SDP exchange remains pending and keepalive fails with all of these conditions:

- The session outcome is `X4_SESSION_ERROR`, error `X4_AUTH_E_STATUS`, status **410**.
- The actual keepalive HTTP status and its public snapshot status are both **410**.
- The real response length permits a strict JSON parse of a root **object**, with exact allowlisted code **2 (`SessionNotActive`)**.

The private flag is reset at the beginning of **every** keepalive call, including a call that is not due. The diagnostic helper consumes it before any request. It makes at most one direct `GET /sdp` if cancellation is absent and at least **30 seconds of negotiation budget** remain within the existing **90-second deadline**, reserving the native HTTP request budget without extending the deadline. Cancellation and deadline are checked again after the request.

The direct session HTTP call and const original outcome preserve the session failure, public snapshot and poll counters. The response is classified for local diagnostics before clearing it, then discarded: a returned answer is not applied, and no SDP POST retry or worker is introduced. Response, exchange, SDP and request buffers are fully cleared on every eligible path, including a skipped diagnostic. Logs contain HTTP/return/result/byte/cancellation/deadline summaries and existing restricted error diagnostics; no raw provider message/body/SDP/ICE credentials are printed.

The 0.7.11 **30-second heartbeat interval and dispatch-based scheduling** remain. Codec/media settings and queue limits remain; this diagnostic does not continue a closed session or establish any particular server timeout. The existing strict direct error-code identifier filter remains, with no raw message/body/SDP/ICE credentials printed.

## Metadata and evidence status

Product version is **0.7.12**, with PS4 package **APP_VER 00.82**. Claude Opus 5.5 completed the focused read-only review successfully in **3 turns / 2 reads**, without a concrete defect in that visible scope. Independent source inspection confirmed response retention for diagnosis, private exchange parsing, the URL error definition and cleanup URL reconstruction. Existing native HTTP timeout checkpoints remain a limitation; before/after guards do not establish precise interruption of every synchronous operation.

## Package and matching source

The full application/package built without warnings or errors:

- Package: `XCloud4-0.7.12.pkg`, identifier `XCLD00001`.
- Size: **8912896 bytes**.
- SHA-256: `eafd597883970adb622bac06efe46b372516323c9e70de59a502ad24a31f040f`.
- VM, PC and PS4 FTP retrieval hashes match.

Application checkpoint `3710c623b10a4fb30fe96ba5d91f2a3ef7bb144d` supplies the authentication change separately from the corresponding dependency/native-adapter archive, closed from the frozen mirror:

- Filename: `XCloud4-0.7.12-dependency-sources.tar.gz`.
- Size: **83559046 bytes**.
- SHA-256: `a8536d57a7ea626dadcc15a11a1d22ff3f2d931a48ade115c2d7ee3db5f54fa2`.
- All **9367** manifest source hashes verified locally, covering **150841509 source bytes**; the PC archive hash matches the guest archive.

Pinned vendors, RTC adapters and configuration are unchanged, with matching notices. Generated artifacts, Git metadata, local credentials and logs are excluded; original public upstream fixtures remain. Both application source and the dependency/native-adapter archive, with retained licenses, accompany the package checkpoint. Every earlier snapshot stays immutable.

## Console result

Readiness/provisioning, Microsoft/Passport authorization (**HTTP 200**) and accepted `/connect` (**HTTP 202**) succeed. Native DNS lookup returns **0** in **14251 microseconds**, with the nonzero-address boolean **1**. The current offer is **1286 bytes / two candidates**, with three media sections and three BUNDLE entries. SDP submission returns **HTTP 202**, followed by **37** pending answer polls returning **HTTP 204**.

Keepalive is attempted at **29723 ms** and succeeds with **HTTP 200 / 37 bytes**. The next SDP poll returns **HTTP 200 / 243 bytes** at **30840 ms**, with the poll count now **38**. Safe diagnostics identify an errorDetails **object**, code **string / class 1** (outside the fixed allowlist), and message **string**. The existing restricted-name helper introduced in 0.7.10 captures the exact validated public identifier **`ConnectionExchangeFailed`**. The message text is not logged; no precise underlying SDP/codec cause is established.

The new 0.7.12 terminal GET is **not entered**, because the keepalive succeeds. The normal SDP response supplies the diagnostic, which is a logical refusal despite HTTP 200. Remote deletion returns **HTTP 200**, with no RTP, video or audio received. The owner reports the session error. This capture identifies the previously unknown public refusal category but does not demonstrate a corrected negotiation or media connection.

No automated tests were added or run. The PS4 UI stays Spanish, repository documentation stays English and GitHub stays private. See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
