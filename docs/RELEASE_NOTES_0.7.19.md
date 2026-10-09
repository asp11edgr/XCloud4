# XCloud4 0.7.19 — native SCTP receive buffer on owned heap storage

Product **0.7.19**, PS4 **APP_VER 00.89**, application identifier **XCLD00001**. The target and existing build flags are retained. The PS4 interface remains Spanish; repository and release content remain English. **The owner confirms first live game audio and video. Audio works well; video is very slow, gamepad reports remain unimplemented and external closure fails.** See the [recorded console result](ERROR_REPORT_0.7.19.md).

## Reason and scope

The frozen [0.7.18 console attempt](ERROR_REPORT_0.7.18.md) reaches valid remote SDP application, ICE completion, DTLS connection, SCTP construction/bind and accepted nonblocking start, followed by a fatal receive-worker fault. The owner reports **CE-34878-8**. The matching ELF places the fault in `SctpTransport::doRecv()`, with a **66,072-byte frame** containing a **65,536-byte automatic buffer**; a call pushes its return address at **RSP - 8** into an unmapped page.

That evidence strongly suggests worker-stack exhaustion. The actual native default worker-stack size is still unknown, and successful SCTP construction/start does not establish a connected association. The [public-client comparison](CLIENT_COMPARISON_0.7.18.md) records the proposed buffer change, alternative stack diagnostics and their source/platform limits. The immutable 0.7.18 package and error evidence remain preserved.

## Target-only receive-buffer change

The checked preparation script modifies the pinned libdatachannel `sctptransport.cpp` only under **`X4_OPENORBIS`**:

- Include `<memory>` for this target.
- Move the existing `bufferSize = 65536` into the existing `try`, before the receive loop.
- Allocate `std::unique_ptr<byte[]> receiveStorage(new byte[bufferSize])` once per `doRecv()` invocation and pass its `byte *buffer = receiveStorage.get()` to the existing operations.
- Keep the original lock acquisition and pending-count decrement before `try`.
- Preserve the upstream automatic array and its original scope on other targets.

The allocation preserves the previous uninitialized scratch-storage semantics and exact receive capacity. The receive call, partial-notification/message copies, message-size limit, end-of-record processing and downstream ownership are unchanged. Existing vectors copy the received bytes; no scratch pointer is queued for later use. RAII releases storage as this invocation exits or unwinds while the existing receive lock remains held.

Each invocation now incurs a 65,536-byte heap allocation and release. Allocation failure is handled by the existing `std::exception` catch as a source-level expectation, without changing its warning/return behavior. Allocation-failure behavior remains unverified on the console. No global scratch buffer, allocation per loop iteration, new stack attributes, signaling change, codec change or input change is introduced.

## Review and native artifact evidence

- Actual Claude Code **`claude-opus-5-5`** completed a fresh read-only review successfully in **10 turns**, with **9 Read calls across 7 files**, and reported no material defects. It used no editing, build or test tools.
- Independent source comparison finds exactly two changed regions in the prepared dependency unit: the target-only include and the receive-buffer declaration/allocation block.
- The preparation script ran twice without changing the resulting dependency source hash: **`67cf412e23205d7fe91cd7161fc8c2caa153758f8ead63fbc00693fd9311bba4`**. Its checks verify both expected replacement regions and reject missing or ambiguous anchors.
- The actual SCTP translation unit was rebuilt, and full native application packaging completed successfully with the existing target/flags.
- In the final linked ELF, `SctpTransport::doRecv()` at **0x000e27c0** reserves **0x228 = 552 bytes**, compared with **0x10218 = 66,072 bytes** in 0.7.18. The **65,536-byte `new[]`** allocation follows the existing mutex acquisition and pending decrement; the receive call retains capacity **65,536**.
- The corrected SCTP address-family word **0x7b10** remains present.
- No automated tests were added or run. These checks establish the source change, repeatable preparation and linked artifact; they do not establish live association or media behavior.

The native default worker-stack size and the stack requirements of deeper calls remain unknown. Reducing this local frame addresses the observed large receive-buffer footprint; it does not demonstrate that every remaining native runtime path has adequate stack capacity.

## Package and matching source

| Artifact | Verified size | SHA-256 |
|---|---:|---|
| `XCloud4-0.7.19.pkg` | 8,912,896 bytes | `91761dfb772f763c7924118d5e38778ade56e933061bece57afc2558367faf8a` |
| `XCloud4-0.7.19-dependency-sources.tar.gz` | 83,580,007 bytes | `e1d252ee00a22b95645b5e8b7d4c9f272f3a6f85ae6ec5d7476b94817fe76ac6` |

The package matches byte-for-byte across the VM, PC and a retrieved PS4 FTP copy. The console installer directory now contains only the 0.7.19 XCloud4 installer; the immutable 0.7.18 package and source snapshots remain preserved.

The final matching-source archive contains **9,368 manifested files**, **9,779 archive entries** and **150,879,059 source bytes**. Every manifested file hash was verified; the forbidden-path inspection returned zero findings. The four preparation/build/configuration copies were aligned with the actual project before final export. Those files, the exporter, eight native streaming adapters and the notice match the corresponding PC/VM/archived bytes. Archived `sctptransport.cpp` matches the actual compiled dependency source hash recorded above.

The [public development prerelease](https://github.com/asp11edgr/XCloud4/releases/tag/dev-0.7.19) retains the exact package and matching source used for the first live-media attempt.

## First live-media result and remaining work

The captured attempt reaches SCTP/RTC state 2 and Xbox channels prepared, then decodes a first Opus packet with 960 samples and an H.264 image of 1280×720/pitch 1280. The owner confirms audible game audio and visible game video. The old receive-worker fault is passed in this attempt; native stack defaults and deeper runtime compatibility remain unmeasured.

The owner reports very slow video, including ARK, and no working game controls. The input channel currently sends metadata but no gamepad frames. Closing through the PS4 menu causes CE-34878-0: the kernel records async **0xa0d0c00a / CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC**, after reporting that `submitDone()` was not called. This is a separate graphics-suspension failure with no faulting CPU-thread information. No normal cleanup summary or measured FPS is captured. Input sending, video performance and graphics submission/closure are the next work; see the [version-specific report](ERROR_REPORT_0.7.19.md) for evidence and attribution limits.
