# Catalog capacity and unobstructed playback in 0.7.28

## Confirmed reports and scope

The owner reports substantially better playback in 0.7.27, uses wired Ethernet, and is trying multiple games. Live capture continues; a complete comparison is not asserted here. The same capture confirms **2,734 validated catalog entries received / 128 distinct entries retained / 21 retained entries with service-supplied access**. Received entries can include repetitions. The hard retention bound, not the eight-row navigation, prevents the other titles from appearing.

Version 0.7.28 raises the retained-title bound to **4,096**, preserving server order, validation, duplicate merging, service-supplied entitlement and explicit truncation beyond the bound. It does not implement the deferred title search or local sorting. Store naming remains limited to the first **32** retained entries, in batches of **8**; other entries display their actual title identifiers. Increasing the bound does not create hundreds of blocking naming requests. The existing 2 MiB response, token, identifier and URL bounds remain.

## Snapshot cost and cache

On the current ABI, a title is 260 bytes. A catalog view changes from **33,492** to **1,065,172 bytes**. The three embedded Work/Auth/main views add **3,095,040 bytes** in total, approximately **2.95 MiB**, plus small revision/cursor overhead. Main's large view remains static, and Work/Auth remain heap allocations; no large automatic catalog is added.

Main previously copied the entire view under the shared authentication/input gate on every loop. A cursor now tracks source context, protected catalog revision, initialization and cancellation visibility. Reset, publication and actual changed progress metadata advance the revision under the same gate. Unchanged views return without the large copy. Token expiry remains checked, cancellation overlays update immediately, and a saturated revision disables cache hits. The cursor is paired with one output buffer and reset on successful auth closure. The legacy unconditional snapshot API is retained.

A changed publication still incurs a full copy under the gate. The cache removes repeated unchanged copies; it is not a measured scheduling or input-latency guarantee. Session selection still copies the selected validated title and offering privately before starting the worker.

## Playback overlay

The previous live overlay painted an opaque **80-pixel** strip over the bottom of the full 1920×1080 game image. Normal playback now draws no permanent strip and does not crop or change the video viewport. Holding **L1 + R1** shows a compact **36-pixel** controls hint. Press and release are part of the presentation refresh key, so a complete picture redraw erases the hint even when no new picture has arrived. The loading/status page explains that chord and its return/exit/mute/Guide shortcuts before the first video picture. Its instruction at y=940 avoids the controller-status glyphs at y=963; the other line uses the existing y=1004 footer.

Muted audio or audio/input errors retain a small plain-language badge; fatal media/session errors still use the existing status page. The controller bindings and local close/return shortcuts remain. Xbox View is the physical touchpad click, Xbox Menu is OPTIONS, and L1+R1+touchpad is Guide. SHARE remains unassigned.

## Preservation and evidence

The 0.7.27 four-reader copy implementation, decoder ownership, conversion, queues, recovery, audio, controller report serialization, requested profile and display pacing are retained. The media-header ownership comment is corrected to describe three helpers. The numeric export filename becomes `xcloud4-trace-0728-<numeric>.bin`; its schema and coverage limits are retained.

Final selected-source reviews by actual Claude Opus 5.5 and an independent reader found no material defect. The corrected loading layout is covered by a fresh actual UI followup. Native build, selected compiled/import/lifecycle inspection, exact VM/PC package and dependency-source matching, 70 frozen build inputs and installer transfer/readback are verified. Owner installation, retention of all available distinct titles, visual removal of the strip, hint clearing, playback and closure remain unverified. Successful compilation alone will not confirm these console results. Search/access groups and a real-destination network-quality check are newly authorized subsequent work; this checkpoint retains the four-reader processing reference.
