# XCloud4 0.7.18 — native SCTP address ABI and initialization diagnostics

Product **0.7.18**, PS4 **APP_VER 00.88**, application identifier **XCLD00001**. Retains the receiving SSRC guard, audio-first offer, validated Teredo handling, native condition-variable replacement and monotonic ICE clock. **Live game media remains unconfirmed.** The latest [0.7.18 console attempt](ERROR_REPORT_0.7.18.md) reaches native SCTP start, then crashes in the receive worker. The previous [0.7.17 result](ERROR_REPORT_0.7.17.md) reached ICE completion, DTLS connection and SRTP key derivation, followed by managed peer failure without RTP.

## Concrete SCTP address-layout mismatch

The installed SDK's libc++ `__config` undefines `__FreeBSD__`. Consequently, the C++ translation unit reads the public usrsctp `sockaddr_conn` declaration using a **16-bit family at offset 0**, without the BSD length byte. The actual usrsctp C library is built with **HAVE_SCONN_LEN/HAVE_SA_LEN**, selecting its internal BSD representation: **8-bit length at offset 0 and 8-bit family at offset 1**. Its C flags deliberately undefine `__FreeBSD__`; the detected length-layout settings are what establish its internal representation.

The distinction is between the public caller declaration and the **internal C implementation**, rather than every C header having one layout. In the immutable 0.7.17 source snapshot, `user_socket.c` includes `sctp_os.h`, which selects `sctp_os_userspace.h` under `__Userspace__`; its internal `sockaddr_conn` is controlled by `HAVE_SCONN_LEN`. The native generic `sockaddr` also has length/family at offsets 0/1. The public `usrsctp.h` separately uses platform macros. These include paths and the actual old build flags establish the internal representation without guessing which macros a configure probe received.

The preceding actual `getSockAddrConn` object emits the low word **0x007b**, encoding family 123 in the first byte and zero in the second. The matching BSD representation is **0x7b10**, encoding length 16 then family 123. Native usrsctp's address handling expects its family at offset 1. This is a verified caller/library ABI mismatch, distinct from a proven cause of the 0.7.17 peer failure.

The target-only checked patch selects the BSD public declaration for `X4_OPENORBIS`, writes `sconn_len`, and requires C++ static assertions for size **16** and offsets **0/1/2/8** for length/family/port/pointer. Other targets retain the original declaration and length rule. It does not globally redefine `__FreeBSD__` or alter socket-option constants.

Inspection also confirms that SO_LINGER and SOL_SOCKET already match the SCTP library (**0x80 / 0xffff**), including the original object instructions. No speculative option remapping is included.

## Bounded initialization diagnostics

Events **91/92/93** record fixed operation step, return value and immediately saved errno, then restore errno before returning the original result. Successful operations log errno zero for clarity without clearing the real saved errno. Connect retains its original EINPROGRESS handling. Steps are:

| Step | Operation or stage |
|---|---|
| 1 / 2 | Socket creation / nonblocking mode |
| 3 / 4 / 5 | Linger / stream reset / receive information |
| 6 / 7 / 8 | Association-change / sender-dry / stream-reset events |
| 9 / 10 / 11 | No-delay / peer-address parameters / initialization parameters |
| 12 / 13 | Fragment interleave / zero checksum |
| 14 / 15 | Read receive / send buffer size |
| 16 / 17 | Set receive / send buffer size |
| 18 / 19 | Constructor complete / start begin |
| 20 / 21 | Bind / connect |
| 22 | Start complete |

A nonblocking connect at step **21** may normally return **-1** with **EINPROGRESS**. That pair indicates connection in progress and must not be classified as a failed setup by itself. Diagnostic lines from different threads may interleave; step/result/errno evidence must be interpreted with the surrounding phase events.

Event **94** records SCTP construction: 0 begin, 1 constructed, 2 inserted/started, 3 no returned transport. Events **95/96** classify initialization exceptions and system error codes; **97** records catch-time errno, which may be residual and is not equivalent to the immediate operation errno. Existing state callbacks, exception propagation and cleanup remain. No raw exception message, address, port, MID, SDP, account data or token is added to diagnostics.

## Review and build evidence

- Actual Claude Code **claude-opus-5-5**, read-only focused review, completed successfully in **6 turns** with no material defects reported. An independent source review agreed. The internal-header distinction and original C include chain were then verified against the immutable 0.7.17 source archive, addressing the review's excerpt limitation.
- Actual **Antigravity 2.22.0**, with its available **Claude Opus 4.6 Thinking** selected in the signed-in UI, completed a second read-only review of the port script, native diagnostic adapter, draft notes and notices. It reported no material defects. Its review did not inspect post-patch vendor objects or execute a build/test; those artifact checks were performed separately above. Its reference to configuration not running does not describe this build: native configure did run, while the C length-layout settings and the public C++ platform selection differed.
- The checked preparation script applied successfully and also accepted an immediate repeat without changing the prepared result. Its first invocation lacked the SDK environment and stopped before preparation; sourcing the configured environment resolved that invocation issue.
- Native dependency target and full application packaging completed successfully. The affected C++ units were rebuilt. The existing internal C layout remains selected by its native flags and headers.
- Final linked ELF `getSockAddrConn` at **0x000e0f20** emits **0x7b10**; its static layout assertions compiled. The linked C `sobind` at **0x0010d520** compares the family byte at **offset 1** with **0x7b**; `usrsctp_bind` likewise writes the length byte at offset 0 and checks family at offset 1. This directly verifies both sides of the final native ABI, without relying on a configure probe.
- The monotonic clock selection remains **1**, and the native `build/streaming/rtc_condition_variable.o` remains explicitly selected; the old SDK `condition_variable.cpp.o` member is not selected. Its separate destructor member remains valid.
- No automated tests were added or run. These checks establish source/build/artifact correspondence; they do not establish a successful console SCTP association or live media.

## Package and matching source

| Artifact | Verified size | SHA-256 |
|---|---:|---|
| `XCloud4-0.7.18.pkg` | 8,912,896 bytes | `8944ac0549ed45c238c66746b124d22a7b2400d7974c56c6c7bdb07d16486779` |
| `XCloud4-0.7.18-dependency-sources.tar.gz` | 83,576,591 bytes | `8caec278e2f0bd8e337a347c016ad3bc5ad1a352749b8a05e2697baeb8fb6acc` |

The package matches byte-for-byte across VM, PC and a retrieved PS4 FTP copy. The matching source archive contains **9,368 manifested files**, **9,779 archive entries** and **150,875,412 source bytes**. Every manifest hash was verified; no generated binary, build cache, account file or runtime-log path was found. The exact notice, port script and native streaming adapters match the corresponding working files. The scoped publication-content inspection found no credential-pattern findings in reachable Git text, current project text or released native adapters/scripts; pinned public vendor fixtures are retained separately as upstream source.

The console installer directory contains only this XCloud4 installer after the verified upload; prior immutable packages and source snapshots remain preserved locally and in their releases. The [public development prerelease](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.18) provides the package, matching source and both SHA-256 sidecars.

## First owner console attempt

The owner installed and ran 0.7.18. Auth, provisioning, valid SDP application, ICE completion and DTLS connection succeed. SCTP constructor steps and bind complete successfully; connect returns the accepted native **-1 / EINPROGRESS (36)** result, then start completes. SRTP keys are derived. Before a confirmed SCTP association or game media callback, the worker crashes with **SIGSEGV**; the owner reports **CE-34878-8**.

The matched native ELF resolves the fault to **SctpTransport::doRecv()**, whose source allocates a **65,536-byte automatic receive buffer**. Its frame reserves **66,072 bytes**, and the faulting call writes at **RSP - 8** into an unmapped page. This is strong evidence for a worker-stack-capacity problem, while the actual stack size and a repaired console result remain unconfirmed. No normal terminal media-counter/cleanup summary was produced, so the absence of playback is not presented as a measured final packet count.

See the [version-specific error report](ERROR_REPORT_0.7.18.md) for the frozen attempt hash, symbols, numeric steps and limits. The completed [public-client comparison](CLIENT_COMPARISON_0.7.18.md), including actual Antigravity and fresh Claude Code review scope, proposes an owned heap receive buffer followed by native stack measurements. These proposals are not implemented; the earlier package preparation evidence and release assets remain unchanged. The PS4 interface remains Spanish; repository and release content remain English.
