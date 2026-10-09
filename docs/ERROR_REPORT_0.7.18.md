# XCloud4 0.7.18 — first console attempt and native receive crash

Observed on **2026-10-09** on the owner's PS4 Fat, firmware **12.00**, GoldHEN **v2.4b18.7**. The launch banner confirms **0.7.18** and PS4 APP_VER **00.88**. The owner reports the displayed code as **CE-34878-8**; that text is preserved as reported, without silently replacing it with a different code. The kernel provides the more specific evidence: **signal 11 (SIGSEGV)** during a user write to an unmapped page.

## Package and preserved evidence

- Console package SHA-256: `8944ac0549ed45c238c66746b124d22a7b2400d7974c56c6c7bdb07d16486779`, previously matched across VM, PC and retrieved PS4 FTP copy.
- Source checkpoint: `9c573a74346b97a143f4e575301b3bdcf54c8cb5`; corresponding dependency archive SHA-256: `8caec278e2f0bd8e337a347c016ad3bc5ad1a352749b8a05e2697baeb8fb6acc`.
- The private attempt extract preserves **387 selected application/crash lines**, SHA-256 `4d6cf54bcc4bbd7c0d0767e52e73dd007f4a3a4b1dc3ad1aef80a8c6fc8e3cd2`. Raw console logs remain outside Git. This report contains bounded numeric and structural evidence only.

## Observed sequence

1. Microsoft authorization, Xbox user token, XSTS and cloud credentials complete with HTTP 200. Repeated HTTP 400 responses during device authorization were followed by successful authorization; they are not this terminal crash.
2. Catalog loading completes. Native live H.264 decoder and Opus/audio output initialize; initialization does not establish receipt or playback of game media.
3. Xbox session creation returns HTTP 202 and provisioning reaches ready. Connection authorization succeeds.
4. The local offer is submitted, and the HTTP 200 response contains a valid **1,504-byte** remote SDP applied successfully. ICE POST returns 202 and remote ICE GET returns 200 with **452 bytes**.
5. Remote ICE summary: **3 entries received**, **1 wrapper submission**, **1 new cached candidate**, no duplicates. A non-Teredo IPv6 entry is skipped with reason **12**. The third entry is omitted during normalization; this evidence does not distinguish an empty/end marker from another unsupported format.
6. C API **ICE state 3 (completed)** and native **DTLS state 2 (connected)** appear.
7. SCTP construction begins. Operation steps **1–17** return zero with logged operation errno zero; constructor step **18** completes. Start step **19** is followed by bind step **20**, which also returns zero.
8. Connect step **21** returns **-1 / errno 36**, the native **EINPROGRESS** result. Start step **22** and construction stage **2** then complete. This is an accepted nonblocking start, not a failed bind or a proven completed SCTP association.
9. SRTP key-derivation stages **0 then 1** complete.
10. A worker thread immediately receives SIGSEGV. No subsequent SCTP-connected/channel-open evidence, media callback evidence or normal final session-counter/cleanup summary is captured before the fatal signal.

## Native crash localization

The console identifies the executable text base as **0x00400000**. The faulting RIP is **0x004e27e2**, therefore executable offset **0x000e27e2**. Mapping that offset with the same 0.7.18 ELF resolves to **`rtc::impl::SctpTransport::doRecv()`**. Dependency line information is unavailable at that address; the function symbol is resolved.

The linked function begins at **0x000e27c0**, subtracts **0x10218 (66,072 bytes)** from its stack pointer, then reaches the faulting `call std::__1::mutex::lock()` instruction. Recorded values:

| Register/evidence | Value |
|---|---|
| RBP | `0x00000007eec37cb0` |
| RSP | `0x00000007eec27a70` |
| Faulting write address | `0x00000007eec27a68` = **RSP − 8** |
| RIP | `0x004e27e2` |

The return-address push at that call targets the unmapped page. The corresponding actual dependency source declares **`byte buffer[65536]`** inside `SctpTransport::doRecv()`. The symbolic backtrace also resolves through a scheduled processor task, `packaged_task`, `ThreadPool::run()` and the C++ thread proxy.

These observations strongly support **worker-stack exhaustion caused by the large local receive buffer** as a current hypothesis. They do not yet establish the configured or mapped worker stack size, exclude prior stack corruption, or prove that changing allocation/thread sizing repairs console behavior. The fault occurs at the call instruction; the evidence does not establish an invalid mutex object as the cause.

## Comparison and investigation scope

The previous 0.7.17 result was a managed peer failure after DTLS connection. In 0.7.18, construction and bind now succeed and nonblocking SCTP start completes before a distinct fatal memory fault. A completed SCTP association, received game media and working input remain unconfirmed.

The owner requested an extensive Antigravity comparison against discoverable Xbox clients and relevant native PS4/PS5/Vita media implementations, using this new attempt. That research must prioritize the worker stack/receive-buffer boundary, while also comparing signaling, queue handling, DTLS/SRTP/SCTP, RTP, decoding, audio, input and cleanup. Proposed solutions must identify exact source evidence, platform/protocol differences, licensing, expected effects, risks and the next observation needed. No new code fix, build or console deployment is part of this report.
