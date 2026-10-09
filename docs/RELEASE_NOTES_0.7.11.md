# XCloud4 0.7.11 — pending SDP before heartbeat

This checkpoint processes a pending SDP response before heartbeat servicing and schedules subsequent heartbeats from dispatch time. Build, focused read-only review, matching source and transfer integrity are confirmed. **The console still reaches keepalive HTTP 410 / `SessionNotActive`, without capturing the underlying logical SDP refusal or receiving RTP/game media. No server lifetime or corrected negotiation result is established.**

## Previous confirmed evidence

Two 0.7.10 attempts produce a two-candidate current offer, accepted SDP submission and pending HTTP204 answer polls. Keepalive then returns HTTP410 / `SessionNotActive`, with no errorDetails; cleanup succeeds and no RTP/media is received. The restricted error-name helper is not entered, so the earlier HTTP200/243-byte logical SDP refusal remains uncaptured. Both attempts pass ready/accepted-connect/provisioned stages before SDP. See [0.7.10 exact evidence](RELEASE_NOTES_0.7.10.md).

## Scheduling and order change

The original `src/auth/xbox_live.c` implementation now:

- Performs pending `GET /sdp` and processes its answer/refusal before the heartbeat.
- If still pending, clears private response/exchange data, services a due heartbeat, then retains the existing **500-ms** poll wait.
- If a valid answer is applied, services a due heartbeat before returning toward ICE negotiation.
- Keeps the heartbeat interval at **30 seconds**, calculating the next deadline from the timestamp captured before dispatch rather than after HTTP completion. Heartbeat response duration is therefore not added to that scheduled deadline.

The initial heartbeat during gathering/pre-submission remains. GET is still synchronous and may delay a due heartbeat by one request; this patch does not create an independent heartbeat worker. Existing cancellation and 90-second negotiation deadline checks around native HTTP remain. SDP parsing, configuration, media behavior, initial queue limit and cleanup are unchanged.

The change is intended to expose an available logical SDP refusal before a heartbeat HTTP 410 abort. The console result below does not capture that refusal; no particular server timeout or refusal cause is inferred from timing.

## Primary reference and metadata

The pinned [GreenVita backend](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/streaming/backend.rs#L193), revision `ae2625d295b4fba005a769b1309fd70dcd6cb63f`, declares its 30-second interval and sets the next deadline before launching an asynchronous keepalive job. XCloud4 uses that scheduling as protocol research; no Rust implementation is copied, and its native HTTP operations remain synchronous.

Product version is **0.7.11**, with PS4 package **APP_VER 00.81**. Claude Opus 5.5 completed the focused read-only review successfully in **3 turns / 2 reads**, without a concrete defect in the reviewed scheduling/source scope. The synchronous-GET delay remains a known limitation.

## Package and matching source

The full application/package built without warnings or errors:

- Package: `XCloud4-0.7.11.pkg`, identifier `XCLD00001`.
- Size: **8912896 bytes**.
- SHA-256: `13337dc56d3b1a84b8f2e083721c1348860e257d79e494eac20e45c54e558c9b`.
- VM, PC and PS4 FTP retrieval hashes match.

Application checkpoint `27dc26112a4df0176c5c7b782d3a051bb4124b4a` supplies the authentication change separately from the corresponding dependency/native-adapter archive, closed from the frozen mirror:

- Filename: `XCloud4-0.7.11-dependency-sources.tar.gz`.
- Size: **83560456 bytes**.
- SHA-256: `fbd5e7dd0213173bf52aab4a2a21489fd026bda66392e2a24004b831daf57778`.
- All **9367** manifest source hashes verified, covering **150840264 source bytes**; the PC archive hash matches the guest archive.

Pinned vendors, RTC adapters and configuration are unchanged, with matching notices. Generated artifacts, Git metadata, local credentials and logs are excluded; original public upstream fixtures remain. Both application source and the dependency/native-adapter archive, with retained licenses, accompany the package checkpoint. Every earlier snapshot stays immutable.

## Console result

The console identifies the installed application as **0.7.11**. Readiness, Microsoft/Passport authorization (**HTTP 200**) and accepted `/connect` (**HTTP 202**) succeed. Native DNS lookup returns **0**, taking **15248 microseconds**, with the nonzero-address boolean **1**. The current offer is **1286 bytes / two candidates**, with **three media sections** and **three unique, matched BUNDLE entries**. Xbox accepts `POST /sdp` with **HTTP 202**, followed by **38** pending answer polls returning **HTTP 204**.

Keepalive is attempted at **29903 ms**, then returns **HTTP 410 / 118 bytes** at **30201 ms**, with root-code class **2 (`SessionNotActive`)**. There is no errorDetails object, so the restricted service-name helper is **not entered**. Remote deletion returns **HTTP 200**, with no RTP, video or audio received. The scheduling change does not reveal the earlier HTTP 200 / 243-byte logical SDP refusal or establish a correction.

This failure follows successful readiness/authorization/connect, rather than an observed provisioning-queue timeout. The existing 180-second initial queue limit and extended-queue backlog remain unchanged.

No automated tests were added or run. The PS4 UI stays Spanish, repository documentation stays English and GitHub stays private. See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
