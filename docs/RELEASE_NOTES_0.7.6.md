# XCloud4 0.7.6 — SDP and HTTP diagnostics

This diagnostic checkpoint compiled, packaged and ran on the owner's PS4. **Xbox still returns no remote SDP answer; the HTTP 410 body is now classified as `SessionNotActive`. No game video/audio was received, and no corrected server result is claimed.**

## Previous confirmed result

Version 0.7.5 accepts the native nonblocking option and generates a local offer after gathering a host candidate. Xbox accepts `POST /sdp` with HTTP 202, but answer requests remain HTTP 204 until keepalive returns HTTP 410. Remote deletion returns HTTP 200; no completed ICE connectivity, RTP or game media is recorded. See [the exact 0.7.5 evidence](RELEASE_NOTES_0.7.5.md).

## Diagnostic scope

- Summarize local SDP structure through fixed enums and numeric counts: media type/order, direction/setup, numeric port/MID/payload type, H.264/Opus types, RTCP feedback flags, bounded profile parsing, BUNDLE matching, trickle, SCTP port and candidate count.
- Categorize HTTP failures before clearing the response buffer, using a fixed error-code allowlist, JSON object/type summaries, route/status/byte count, SDP poll count and elapsed time.
- Omit raw SDP, ICE values, account credentials and arbitrary remote response text.
- Preserve the existing negotiation behavior while collecting evidence.

The diagnostic implementation is original. Pinned dependency and port source, retained licenses and the exact native adapters must accompany any distributed package in its matching source snapshot. Earlier source snapshots remain unchanged.

## Review and unresolved cause

Claude Opus 5.5 completed a read-only SDP review in **7 turns**, with **6 reads**, successfully. It did not establish a concrete cause or demonstrate a correction. Reviewed configuration, ordering and the **30-second keepalive interval** match the researched GreenVita protocol path. The generic offer's video-first ordering does not establish an audio-first requirement.

The structural scanner received a separate successful read-only Claude Opus 5.5 review in **2 turns**. It found a diagnostic limitation when an SDP contains more than eight media sections: setup/trickle on an overflow section can be mislabeled as session-level. The actual offer has three sections, so this does not affect that capture or negotiation. The 0.7.6 snapshot preserves the reviewed code unchanged; the limitation is retained for the next checkpoint.

## Actual console result

Native nonblocking setup and gathering return zero. The local callback's structural summary, reconstructed from interleaved log fragments, records **1093 bytes**, **three media sections**, **zero candidates**, and three unique/matched BUNDLE entries. The header was split by concurrent logging; the reconstructed values are not presented as an intact original log line.

The separate media summaries record video index 0, audio index 1 and application index 2, each with port 9. Video uses H.264 payload type **102**, four RTCP feedback entries and profile **42E01F**; audio uses Opus **111**. SCTP port is **5000**, with actpass/trickle reported. These structural settings alone do not establish a server-compatible negotiation.

Xbox accepts the offer with **HTTP 202**, but **33** answer polls return **HTTP 204**. Keepalive then returns **HTTP 410**, with a **118-byte** JSON object and allowlist code **2 (`SessionNotActive`)**; no nested `errorDetails`/`error` is reported. The failure occurs **30317 ms** after SDP submission, with the keepalive interval unchanged at **30000 ms** and the last attempt logged at **29920 ms**. Remote deletion returns **HTTP 200**. No completed ICE connectivity, RTP, video or audio is recorded; the owner reports the same error.

Source inspection identifies that the submitted offer comes from the initial callback snapshot even after gathering, while that callback summary contains zero candidates. The next checkpoint will obtain the current local description after gathering. That finding does not yet prove the reason for `SessionNotActive` or a successful correction.

## Package and matching source

- Package: `XCloud4-0.7.6.pkg`, identifier `XCLD00001`.
- Actual size: **8912896 bytes**.
- Package SHA-256: `bfc2facb54c9c974851e2e33954deea44a952b7586511d0ac8db9155cde01e6d`.
- VM, PC and PS4 FTP retrieval match; the installer directory contains only 0.7.6 after retiring the previous installer.
- Matching archive: `XCloud4-0.7.6-dependency-sources.tar.gz`, **83544715 bytes**.
- Archive SHA-256: `14f8578e49d8bef7497b68b8803d6ed71c0fc5d720f72687d7ba05b515246f2d`.
- All **9367** manifest source hashes verified; the PC copy matches the guest archive.

The source snapshot includes the exact structural scanner, native adapters, unchanged pinned dependency trees, configuration and retained licenses. Generated artifacts/Git metadata/local credentials/logs are excluded; original public upstream fixtures remain. Every earlier snapshot is unchanged. No automated tests were added or run. The PS4 UI stays Spanish, repository documentation stays English and GitHub stays private.

See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
