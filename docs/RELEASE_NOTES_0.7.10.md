# XCloud4 0.7.10 — restricted service error name

This diagnostic checkpoint exposes only a validated service-code identifier from the direct `errorDetails.code` field. The focused read-only Claude review and full application/package build completed, with no build warnings/errors and matching VM/PC/PS4 retrieval hashes. **Two console attempts end with keepalive HTTP 410 / SessionNotActive and no errorDetails, so the new helper is not entered. The underlying logical SDP refusal remains uncaptured; no game video/audio is received.**

## Previous confirmed evidence

Version 0.7.9 receives **HTTP 200 / 243 bytes** containing an `errorDetails` object with a string code classified as unrecognized and a string message. The actual code/message text is not logged, so its precise refusal category remains unknown. Native DNS and the two-candidate current offer succeed; remote deletion returns HTTP 200 with no RTP/game media. See [0.7.9 evidence](RELEASE_NOTES_0.7.9.md).

## Diagnostic change

The original helper in `src/auth/xbox_live.c` reads only the direct `errorDetails` object and its string `code`:

- Length **1–64 ASCII characters**.
- First character must be a letter.
- Remaining characters must be letters, digits or underscore.
- Invalid, absent, duplicated or unsupported fields produce no name output.
- Only the validated identifier is printed locally; the temporary **65-byte** buffer is cleared on every path.

Message text and top-level/nested/other strings remain hidden. All earlier numeric diagnostics and classes0–14 remain. No negotiation, parsing decisions, SDP/ICE configuration, keepalive, retries, cleanup or media behavior changes.

## Primary references and source

[CloudNow's diagnostic filter](https://github.com/owenselles/CloudNow/blob/6627c0423474ebe6647e86a2db953ce55dabaae1/CloudNow/Xbox/XboxCloudSignalingAPI.swift#L820), pin `6627c0423474ebe6647e86a2db953ce55dabaae1`, filters service-error identifiers separately from message content. [OpenXbox's response model](https://github.com/OpenXbox/xcloud-rs/blob/cd94b22f611bece91c5963c5647588b06adc705f/gamestreaming_webrtc/src/api.rs#L573), pin `cd94b22f611bece91c5963c5647588b06adc705f`, separates optional error code and message fields. XCloud4 does not copy those implementations: its original helper has narrower direct-field, ASCII and length bounds.

The inspected references did not establish an exhaustive server-code enumeration. No refusal category or timeout cause is inferred from response timing. A capture containing a direct errorDetails string code is still needed to exercise the new diagnostic.

## Version metadata and review

The product version is **0.7.10**. PS4 package `APP_VER` is **00.80**, following **00.79** while retaining that field's two-digit format. The displayed product version and package version field are recorded separately.

Claude Opus 5.5 completed the focused read-only review successfully in **3 turns / 2 reads**, without a material finding in the helper/JSON scope. It inspected bounds, control-character rejection, duplicate fields, constant output format and buffer clearing. An earlier broader attempt reached its turn limit without a result; the successful focused review is recorded separately. No source edits or tests were performed by Claude.

## Package and matching source

- Package: `XCloud4-0.7.10.pkg`, identifier `XCLD00001`.
- Size: **8912896 bytes**.
- SHA-256: `c3a8e1cd1bf8d78c15f8c7bec9e73470afc0e1fe14053692357bd15fc5d82ffe`.
- VM, PC and PS4 FTP retrieval hashes match.

The exact application checkpoint `0fbe73a` supplies the authentication helper separately from the corresponding dependency/native-adapter archive, closed from the frozen mirror:

- Filename: `XCloud4-0.7.10-dependency-sources.tar.gz`.
- Size: **83558285 bytes**.
- SHA-256: `4dc79943434d23a05840665254769984d905e0e0e1bb9e4a186fa4ec03b39a77`.
- All **9367** manifest source hashes verified; the PC copy matches the guest archive.

Pinned vendors, RTC adapters and configuration are unchanged, with updated corresponding notices. Generated artifacts/Git metadata/local credentials/logs are excluded; original public upstream fixtures remain. Both source sets and retained licenses must accompany any distributed package. Earlier snapshots remain immutable. Build/review/integrity evidence does not establish a returned Xbox SDP answer or game media.

## First console attempt

Native DNS succeeds (lookup **0**, elapsed **12356 microseconds**, nonzero-address boolean **1**). The current offer is **1285 bytes / two candidates**, with three media sections and three BUNDLE entries. Xbox accepts submission with **HTTP 202**, but **38** pending answer polls return **HTTP 204**.

Keepalive is attempted at **30379 ms** and returns **HTTP 410 / 118 bytes** at **30670 ms**, with root-code class **2 (`SessionNotActive`)**. There is no errorDetails object, so the restricted-name helper is **not entered**. Remote deletion returns **HTTP 200**, with no RTP, video or audio received. This attempt does not reveal the earlier HTTP200/243-byte logical SDP refusal or demonstrate a correction.

## Second console attempt

The owner repeats the same installed 0.7.10 version. The offer is again **1285 bytes / two candidates**, with **37** pending HTTP204 polls. Keepalive is attempted at **30187 ms**, then returns **HTTP410 / 118 bytes** at **30480 ms**, root-code class **2 (`SessionNotActive`)**, with no errorDetails. The restricted-name helper is again not entered. Remote deletion returns **HTTP200**, with no RTP/video/audio. Both attempts identify a session that is no longer active, while the underlying earlier logical SDP refusal remains uncaptured; neither establishes a correction.

## Queue behavior and current limit

Both attempts pass `ReadyToConnect`, accepted `/connect` (**HTTP202**) and `Provisioned` before SDP submission. Their observed failure occurs during subsequent negotiation/polling.

The application recognizes `WaitingForResources` as a waiting state, with an initial provisioning limit of **180 seconds (three minutes)**. Extended queue support is incomplete: waiting duration and queue UI need improvement. No server wait-time estimate is established by these captures. The bounded request is recorded in [future improvements](MEJORAS_FUTURAS.md).

Package/source snapshots remain unchanged. No automated tests were added or run. The PS4 UI stays Spanish, GitHub content stays English and the repository stays private.

See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
