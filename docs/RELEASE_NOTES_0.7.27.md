# XCloud4 0.7.27 — prepared four-reader experiment

Product **0.7.27**, PS4 **APP_VER 00.97**, application identifier **XCLD00001**. This authorized candidate has completed selected-source and selected compiled inspection, native build and matching VM/PC artifact retrieval. Console delivery/results are **pending**. The PS4 interface stays Spanish; repository documentation stays English.

## Purpose

The completed [0.7.26 two-reader diagnostic capture](VIDEO_REPORT_0.7.26.md) records **16.016 ms mean copy** and **26.095 new draws/s** over **82 complete intervals / 410.310 seconds**. Its exact trace counter reports **96 NEW-picture matching intervals over 100 ms**, with retained detail for **13**; all retained detailed intervals contain recovery activity. Incomplete coverage does not establish every pause's cause.

The next isolated experiment asks whether **the video owner plus three persistent helpers** can reduce the native-to-cached copy wall time while preserving ownership and recovery. It does not claim that four readers fix the observed pauses.

## Candidate scope

- Four contiguous, bounded NV12 spans, with 64-byte split boundaries and the existing tail routine.
- Stable per-helper job/result/submission/completion ownership; no native decoder calls from helpers.
- Require all helpers idle before dispatch and drain all published completions before conversion, fallback, reuse or error cleanup.
- Four readers or serial fallback after successful partial-creation cleanup; no silent two-/three-reader mode. Stop all created helpers before joining all; latch failed joins and retain storage before native teardown.
- Preserve the first-valid-picture serial byte comparison, separate check cost, safe serial fallback and pending-reservation guard before Decode.
- Distinguish the configured reader count from actual per-picture mode: trace-header **4** describes the pool; transition event `COPY_CONFIG` (`0x22f`) records its state, and `COPY_END` identifies the actual selected path. Flag **0** initially reports the available full pool with its first-picture check pending; it can also report restored eligibility after a successful check. It alone does not prove a new check or four-reader copying of every picture. Each helper's immutable index occupies the event flags' high byte. Existing timing fields retain their names; helper elapsed time becomes a **sum of overlapping spans**, not copy wall time.

The **16 ms / four Decode** checks, native type 3/four output reservations, conversion, RGB mailbox, display, ring/reorder/recovery policies, audio, controls, protocol and requested profile remain the experiment's baseline. Diagnostics retain the 0.7.26 definitions and coverage limits, with helper identity added. See the [full experiment contract](FOUR_READERS_EXPERIMENT_0.7.27.md).

## Evidence status

| Evidence | Current state |
| --- | --- |
| Owner authorization | Confirmed for the four-reader trial |
| Two-reader reference capture / numeric export | Confirmed in 0.7.26; independently retrieved and inspected |
| Four-reader candidate | Built from the frozen, reviewed source |
| Actual Claude Opus 5.5 final selected-source review | No material defect found; 21 turns, 20 attempted Read calls, 19 successful files; failed-path caveat below |
| Independent selected-source inspection | PASS; no material defect found in the selected scope |
| Independent selected compiled inspection | PASS: 47 disassembly entries / 43,737 matching bytes; no blocking discrepancy in the inspected scope |
| Native build / linked inspection | PASS |
| Frozen source / dependency inventory correspondence | PASS: 70 build inputs match host/VM before and after build; 9,369 archive payloads verified |
| Package/archive size and SHA-256 | Verified; exact VM/PC matches listed below |
| Console transfer / readback / installation | Pending |
| Actual four-reader mode / first-picture byte equality | Pending |
| Sustained copy/gap/recovery result and clean closure/export | Pending |

The fresh review used the requested and observed model **`claude-opus-5-5`**, completed with exit status **0** and empty stderr, and used only Read tools with an empty MCP configuration. Required selected-source reads and frozen-source hash correspondence were checked independently. One attempted Read used a mistyped, nonexistent evidence path and returned no contents; the reviewer then read the correct scoped copy. The technical review is usable with this explicit failed-path caveat, rather than a claim of perfect allowlist compliance. It executed no application, test, build or hardware check.

The existing media-header comment mentioning one optional helper is stale: this candidate implements three helpers. Correcting that comment is deferred to a later source snapshot; the 70 frozen build inputs remain intact.

The independent compiled inspection checked selected linked bytes against disassembly, decoded native imports, and inspected helper stop/join, partial-creation retention, span bounds, completion draining, pending-picture guards, trace bounds/exclusive export and native-stop ordering. Its **47 selected entries / 43,737 matching bytes** are a bounded audit, not execution coverage or proof of hardware behavior.

No earlier review is substituted for this candidate's fresh review. Antigravity's previously documented final-review quota limitation does not supply new approval. No benchmark, console improvement, stable FPS or end-to-end latency result is asserted.

## Verified built artifacts

| Artifact | Bytes | SHA-256 |
| --- | --- | --- |
| `XCloud4-0.7.27.pkg` | 8,912,896 | `4c241acc49171168bfd9facf27dcaf5f7c17f6b76f30380be3b75f46f4fbfc14` |
| `XCloud4-0.7.27-dependency-sources.tar.gz` | 83,583,637 | `dcc64fbe5a4bead09d42a5ea7902e57219d9efe248a463e71cc6f3232291622f` |

The retrieved package, ELF/OELF/eboot and compressed dependency archive match the build VM's hashes. The archive contains **9,369 manifest payloads / 9,781 entries / 150,890,220 source bytes**, with all payload hashes verified and no forbidden entries. The compressed archive itself matches the VM export. These checks establish retrieved build/source correspondence; they do not establish PS4 transfer, installation or playback. Credentials, raw logs, private trace files, SDK material and auxiliary runtime binaries remain outside Git.
