# XCloud4 0.7.8 — native DNS timeout units

This checkpoint corrects the native resolver's timeout units and adds numeric diagnostics. The full application/package compiled without warnings/errors, and VM/PC/PS4 retrieval hashes match. **The console confirms successful DNS and an offer with two candidates, but Xbox then returns a logical SDP refusal inside HTTP 200. No game video/audio was received.**

## Last confirmed console result

Version 0.7.7 obtains the current local description after gathering: **1201 bytes**, **one real candidate**, and three unique/matched BUNDLE entries. Xbox accepts SDP submission with HTTP 202, but 33 answer polls return HTTP 204 before keepalive reports HTTP 410 / `SessionNotActive`. Remote deletion returns HTTP 200, no RTP or game media is received, and OPTIONS exits correctly. The stale initial offer was repaired, while remote negotiation remains unresolved. See [0.7.7 evidence](RELEASE_NOTES_0.7.7.md).

## Resolver change

The original hostname-resolution adapter passed `2` as the native timeout. The corrected value is **`2000000` microseconds**, providing the intended **two-second** bound. Numeric diagnostics describe:

- Event 69: resolver creation result (zero or native error).
- Event 70: configured timeout in microseconds.
- Event 71: native lookup result.
- Event 72: elapsed microseconds, bounded to `INT_MAX`.
- Event 73: whether the resulting address is nonzero, as a boolean only.

No hostname, address, credential or ICE value is printed. The source changes are confined to the original `rtc_net.c` adapter and fixed diagnostic labels in `rtc_native.c`; pinned vendor trees are unchanged.

The pinned [ioQuake3-PS4 resolver call](https://github.com/Mayo1970/ioQuake3-PS4/blob/d4c7912af73c3b0eaa57195391ad76f21e79a910/code/qcommon/net_ip.c#L447), revision `d4c7912af73c3b0eaa57195391ad76f21e79a910`, passes `3 * 1000 * 1000`. Its [technical DNS note](https://github.com/Mayo1970/ioQuake3-PS4/blob/d4c7912af73c3b0eaa57195391ad76f21e79a910/AGENTS.md#L193), read as external reference data, explicitly identifies microsecond units. XCloud4's implementation is original and uses its own two-second limit; no external implementation is copied.

This corrects a concrete timeout argument in source. The previous console evidence does not establish DNS as the sole cause of the earlier missing answer. The media profile and current-offer behavior are not changed by this resolver patch.

## Review and actual console result

Claude Opus 5.5 completed a focused read-only review successfully in **3 turns / 2 reads**, without finding a concrete defect within the reviewed resolver/diagnostic scope.

The SDK's `sceKernelGetProcessTime` return type was confirmed as `uint64_t` for the elapsed-time diagnostic.

The console records resolver creation **0**, timeout **2000000 microseconds**, lookup result **0**, elapsed time **11519 microseconds**, and a nonzero-address boolean **1**. The current offer is **1286 bytes**, with **two candidates**, three media sections and three BUNDLE entries. DNS succeeds in this capture; that does not prove it was the sole cause of the earlier negotiation failure.

Xbox accepts SDP submission with **HTTP 202**. After **37** answer polls return **HTTP 204**, the next keepalive succeeds with **HTTP 200 / 37 bytes**. An SDP answer request then returns **HTTP 200 / 243 bytes**, but the exchange parser returns **-2**, identifying a non-null `errorDetails` value. This is a logical SDP refusal carried by a successful HTTP response; the exact server error category is not yet known. The owner reports a new error view. Remote deletion returns **HTTP 200**, and no RTP, game video or audio is received. Further diagnostics must inspect that bounded logical-error category without exposing arbitrary response data.

## Package and matching source

- Package: `XCloud4-0.7.8.pkg`, identifier `XCLD00001`.
- Size: **8912896 bytes**.
- SHA-256: `b562ab50eb0336b7d518bba8e8db74eb9a8af87ed6bd6c0ed3f0eaef14812664`.
- VM, PC and PS4 FTP retrieval hashes match.

The exact corresponding sources closed from the frozen build mirror:

- Filename: `XCloud4-0.7.8-dependency-sources.tar.gz`.
- Size: **83545142 bytes**.
- SHA-256: `9aac8351b1b93b0f4e619e81a3291f74567ccaf0dd4da516f0e61fecf654a0e0`.
- All **9367** manifest source hashes verified; the PC copy matches the guest archive.

The archive retains the actual two changed adapters, unchanged pinned vendor trees, configuration, overlays and original licenses. Generated artifacts/Git metadata/local credentials/logs are excluded; original public upstream fixtures remain. Every prior snapshot is immutable. Any distributed package must be accompanied by this matching source and retained licenses. The successful resolver/candidate result does not establish a complete remote negotiation or native media reception.

No automated tests were added or run. The PS4 UI stays Spanish, repository documentation stays English and GitHub stays private. See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
