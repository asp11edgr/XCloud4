# XCloud4 0.7.28 — catalog and playback interface candidate

Product **0.7.28**, PS4 **APP_VER 00.98**, title identifier **XCLD00001**. Final selected-source reviews, native build, linked inspection, matching VM/PC artifacts and console installer transfer/readback are verified. Owner installation and visual/catalog results remain pending. Repository documentation is English; the PS4 interface stays Spanish.

## Changes

- Raise the bounded retained catalog from 128 to **4,096 distinct validated titles**, with the existing truncation notice beyond that bound. The current console response contains 2,734 validated entries, including possible repetitions.
- Cache the enlarged catalog snapshot by protected revision and cancellation visibility, avoiding a repeated approximately 1 MiB copy every main loop. Preserve account expiry, cancellation, selection and private session arguments.
- Remove the permanent 80-pixel playback strip. The complete game image remains visible during normal playback; hold **L1 + R1** for a compact controls hint. Keep small mute/error warnings and the existing fatal-error page.
- Explain the local shortcuts on the loading/status page before the first video picture. Correct its obsolete account/catalog preparation text. The new help at y=940 avoids the controller status at y=963.
- Preserve the 0.7.27 four-reader video processing and the existing audio, controls, protocol and profile. Change only product metadata, the numeric export filename and a stale ownership comment in those paths.

Title search/access groups and a real-destination network check are now authorized requests for subsequent work, replacing the search deferral. This checkpoint implements the capacity/overlay change only. Store naming remains the first 32 entries in batches of 8; unnamed entries display their title identifiers. See the [capacity, cache and overlay contract](CATALOG_AND_OVERLAY_0.7.28.md) and [updated requests](MEJORAS_FUTURAS.md).

## Evidence status

| Evidence | Status |
| --- | --- |
| Owner report for 0.7.27 | Substantially improved playback; Ethernet; owner confirms closing without an error |
| Catalog cause | Confirmed source bound and actual 2734/128 console counters |
| Candidate source / independent review | Final 10 frozen runtime inputs inspected; no material defect in scope |
| Actual Claude Opus 5.5 review | Original catalog/cache/UI scope: 28 turns / 27 successful Reads; final UI followup: 20 turns / 19 successful Reads; no tool errors or out-of-scope reads |
| Native build / compiled inspection | Verified native package/import/lifecycle inspection; selected six catalog/UI/main functions inspected, including cache hit bypass, 1,065,172-byte static catalog and final instruction coordinates |
| VM/PC package and exact dependency-source correspondence | Matching package/ELF/OELF/eboot, 70 frozen host/VM inputs before/after, exact compressed VM archive and all 9,369 manifest payloads / 9,781 archive entries verified |
| Public release | Prepared for publication; owner console results remain pending |
| PS4 transfer and readback | Matching 8,912,896-byte package and SHA-256 verified; only the exact previous 0.7.27 installer removed, PC rollback preserved |
| Console catalog, full image, hint clearing, playback and closure | Pending |

The initial UI review is preserved with its superseded-source limitation. A proposed loading line overlapped the controller-status line; it was corrected to y=940 before the fresh actual final-UI review and final build. Current runtime correspondence combines the unchanged nine initial inputs with that final UI review. Source/compiled inspection does not establish physical console behavior. Main's compiled local stack reservation is 2,408 bytes; this is not a measurement of all native thread stacks or total memory use. The approximately 2.95 MiB catalog growth is a layout calculation, not a measured performance gain.

## Verified artifacts

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| XCloud4-0.7.28.pkg | 8,912,896 | `48c40c9dc62ed88c74ed1874bf447255e00b063e5e113e7a2494c6580b1a4d89` |
| XCloud4-0.7.28-dependency-sources.tar.gz | 83,585,539 | `4ed982d54e83ba9dddffe4b9f57c7737f73e3e8f89cd4ff2ef164b2e2d8a9030` |

Checksums and source correspond to the final retrieved native artifacts. No test, controlled benchmark, internet/server root cause, stable FPS or end-to-end latency result is claimed. Credentials, SDK material, raw logs and private traces stay outside Git. Earlier releases and the two-reader/four-reader reference artifacts remain preserved. The new [0.7.26/0.7.27 observational comparison](VIDEO_COMPARISON_0.7.26_0.7.27.md) documents the completed reference captures separately.
