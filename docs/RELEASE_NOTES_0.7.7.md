# XCloud4 0.7.7 — refresh the local offer after gathering

This checkpoint obtains the actual current local SDP after gathering instead of using the initial callback snapshot. The full application/package compiled successfully without warnings/errors. **The owner's console result is pending; a successful Xbox negotiation or game video/audio result is not yet established.**

## Evidence motivating the change

In 0.7.6, the callback summary reconstructed from interleaved log fragments records **1093 bytes and zero candidates**, although a host candidate is gathered later. Source inspection shows submission still uses the retained initial callback description. Xbox accepts the offer with HTTP 202 but returns no answer through 33 HTTP 204 polls, then reports `SessionNotActive` with HTTP 410. Cleanup succeeds; no game media is received. See [the exact 0.7.6 evidence](RELEASE_NOTES_0.7.6.md).

The stale local snapshot is a concrete source finding. Its relationship to the missing server answer remains to be checked on the console; it is not presented as a proven cause of the entire failure.

## Transport change

The original transport waits for gathering completion and obtains the current description through libdatachannel's public C API. It reads actual provider state and does not synthesize or merge candidates/addresses. No extra valid-candidate requirement is imposed beyond real provider gathering completion.

The getter verifies offer type, queried/read lengths, caller capacity, the **32768-byte** limit including termination, a final NUL and absence of an early NUL. Library errors remain negative; unavailable provider data stays pending within the existing authentication deadline/keepalive behavior. The one-time structural summary describes this current post-gathering offer instead of the initial callback; the callback reports only gathering state. The scanner's setup/trickle labeling above eight media sections is repaired.

H.264 profile, media ordering and HTTP signaling behavior remain unchanged. Matching source must preserve the updated adapter, unchanged pinned vendor trees, retained notices and configuration separately from every earlier snapshot.

## Package and review evidence

- Package: `XCloud4-0.7.7.pkg`, identifier `XCLD00001`.
- Size: **8912896 bytes**.
- SHA-256: `5a050c7ae06080c0a051e9dad7ca4550e0ed85feec0c3abec4aceb31c50307b3`.

A static peer review of the current-offer getter completed successfully. Claude Opus 5.5 completed its focused read-only review successfully in **2 turns / 1 read**, with no proven defect found within that scope. It inspected current C API data, bounds, type/NUL checks, locking and privacy. VM/PC/PS4 retrieval hashes match; after verification and retiring 0.7.6, the console installer directory contains only 0.7.7. Review/build/integrity evidence does not establish a returned Xbox SDP answer or native game media.

The exact corresponding-source archive closed from the frozen 0.7.7 mirror:

- Filename: `XCloud4-0.7.7-dependency-sources.tar.gz`.
- Size: **83544519 bytes**.
- SHA-256: `33e30efb0c20cf45e976e950b8d320147f46bad60f4224bf685ce680c14949b8`.
- All **9367** manifest hashes verified; the PC copy matches the guest archive.

Previous snapshots remain unchanged. Generated artifacts/Git metadata/local credentials/logs are excluded; original public upstream fixtures remain. No automated tests were added or run. The PS4 UI stays Spanish, GitHub content stays English and the repository stays private.

See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
