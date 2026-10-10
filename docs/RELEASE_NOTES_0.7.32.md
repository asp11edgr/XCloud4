# XCloud4 0.7.32 reception observations

Product 0.7.32, PS4 APP_VER 01.02, adds diagnostics inside the pinned receive path to investigate the 759 forward skipped RTP positions verified in the 0.7.31 callback observer. It preserves the MPSC video queue, four copy readers, resolution request, reorder rules and recovery decisions. It does not implement a playback correction.

## Changes

- Numeric receive observations from UDP or framed TCP through ICE, the existing Mbed TLS queue, SRTP processing, PeerConnection, the receiving handler, Track and the XCloud4 callback. Authentication and replay rejection categories retain separate counters and exact local result codes in the bounded critical ledger.
- Stage/source sequence identities with extended wrap handling, missing ranges, later observations and explicit ambiguity or retention omissions. Original RTP and RTX remain separate unless an observed normalization relates their header identities.
- One detailed context per pause ID, bounded prehistory and reserved end context. A separate critical ring retains gaps, rejects, recovery, local PLI steps and numeric session settings with cumulative omission counts.
- Separate counters for structurally complete AU, recovery-filter rejection and accepted Decode feed. Seeing an IDR, producing native output, publishing RGB and completing presentation remain distinct observations.
- Read-only offered/accepted feedback and RTX summaries. NACK/RTX negotiation and feedback generation remain unchanged.
- Versioned X4PROG2 export and reader compatibility with original X4PROG1 files. The original 0.7.31 binaries, analyses and reader snapshots are preserved.

## Test scope

The portable monitor, ingress, receive bridge and Python reader checks exercise bounded storage, concurrency, wrap, later arrivals, parser invariants and shutdown lifetime using synthetic data. Native compilation and selected linked inspection verify the prepared artifact separately. Their receipts and matching source identities accompany the development release; neither establishes the native scheduling cost of added observations.

The next owner console pass starts from a freshly launched application and contains one game session for 10–15 stable minutes. The process-level receive sink cannot establish attribution of late asynchronous transport work across successive sessions in one process. A subsequent owner/epoch link would be needed for that broader scope.

See the [reception test plan and stage limits](RECEPTION_DIAGNOSTICS_0.7.32.md) and the [verified original evidence](RECEPTION_EVIDENCE_0.7.31.md). The console chronology and any functional correction remain pending this new capture. Raw packet metadata and console logs remain private. Exact application and modified dependency sources accompany the package; the OpenOrbis SDK is external.
