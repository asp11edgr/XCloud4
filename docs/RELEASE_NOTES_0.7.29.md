# XCloud4 0.7.29 — catalog search candidate

**Subsequent owner checkpoint:** the requested search/filter/selected-title/return interaction worked correctly. See the [owner report](CONSOLE_REPORT_0.7.29.md). The preparation-time evidence and limits below remain preserved; their pending owner status has been superseded for that specific interaction, without an exhaustive catalog or video benchmark.

Product **0.7.29**, PS4 **APP_VER 00.99**, title identifier **XCLD00001**. Final selected-source reviews, native compilation, selected linked inspection, artifact correspondence and installer transfer/readback are verified. Owner console search/filter/game-return results remain pending. GitHub documentation is English; the PS4 interface remains Spanish.

## Changes

- Add case-insensitive substring search over available catalog names and identifiers, with a 48-character controller keyboard.
- Add **all**, **confirmed access** and **unconfirmed access** groups, with full-list totals and the current query match count.
- Map filtered selection back to the original catalog index, with READY/bounds/query/group/title-ID validation before launching.
- Preserve query/group when returning from a game or refreshing; consume keyboard CIRCLE/R2 locally while retaining global OPTIONS exit.
- Rebuild the approximately 8.2 KiB static browser only on changed catalog revisions or changed criteria.
- Keep the 0.7.28 catalog capacity/cache and hidden playback menu; keep the 0.7.27 four-reader media baseline. Media/protocol changes are version metadata and numeric trace filename only.

The [search contract](CATALOG_SEARCH_0.7.29.md) describes controls and limitations. Store names remain limited to 32 retained entries. Unconfirmed access does not imply purchase required; name-only words cannot match a missing name. Cover images and a network test are not implemented. The [network-quality plan](NETWORK_QUALITY_PLAN.md) separates HTTPS service timing, SCTP observations and actual RTP media metrics.

## Previous console result

The owner confirms the 0.7.28 complete catalog and hidden/show/clear menu behavior. Its console response retained **2,734 / 2,734** titles, with **578** marked with confirmed access. See the [0.7.28 console report](CONSOLE_REPORT_0.7.28.md). This is not a fresh video benchmark.

## Evidence status

| Evidence | Status |
| --- | --- |
| Integrated search/index model and UI | Final source prepared; copied-snapshot and navigation/filter launch-frame guard included |
| Independent selected-source review | No unresolved material source finding; all 26 selected identities and 72 build inputs match the final freeze |
| Actual Claude Opus 5.5 review | Original scope: 23 turns / 22 Reads / 21 files; fresh launch-guard followup: 17 turns / 16 Reads / 16 files; complete returned-line coverage, zero failed/outside reads |
| Native build and selected linked inspection | Native compilation and import/lifecycle checks verified; 17 selected catalog/model/UI/main functions inspected; static browser 8,372 bytes, catalog 1,065,172 bytes; main fixed stack reservation 2,424 bytes |
| Frozen host/VM inputs and retrieved artifacts | 72 exact inputs before/after build; matching package/ELF/OELF/eboot and original compressed dependency archive; all 9,369 manifest payloads / 9,781 entries verified |
| Release preparation | Source and four release assets prepared for dev-0.7.29; owner installation remains pending |
| PS4 installer transfer/readback | Matching 8,912,896-byte package verified; only previous 0.7.28 installer removed; PC rollback preserved |
| Owner search/filter/selected-game/return/closure behavior | Pending |

No automatic application run, added test, controlled benchmark, guaranteed entitlement, complete naming, network/server cause, stable FPS or end-to-end latency result is claimed. Existing checkpoints and rollback artifacts are preserved. Credentials, SDK files, raw private logs and traces stay outside Git.

## Verified artifacts and review limits

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| XCloud4-0.7.29.pkg | 8,912,896 | `8c4d7bf298f596820d5e0351f61f66d538ecaad60660ab30920ed8dabf859c2a` |
| XCloud4-0.7.29-dependency-sources.tar.gz | 83,582,392 | `a5e23b5d13da469dfc867ddb2ad9dbf2ed59e9e41aa0245cd9e3554fcb321276` |

The original source review and first native candidate are preserved with their superseded-main limitation. The final followup covers the new launch guard and qualified search documentation; unchanged full auth/UI source was covered by the original review and final hash correspondence, not reread in that followup. These are selected-source reviews, not a full repository audit or complete commercial-name search approval. An optional narrow case on returning from failed application exit remains: a valid selected row may not have been displayed during the closing screen. No hardware behavior is inferred from compilation or disassembly.

Native source and machine-code inspections establish the bounded static browser size and selected structural checks; they do not establish total thread-stack usage, memory headroom, actual presentation or gamepad behavior. Public evidence status paragraphs were updated after source reviews to record actual build and transfer receipts; the final runtime source remains unchanged.
