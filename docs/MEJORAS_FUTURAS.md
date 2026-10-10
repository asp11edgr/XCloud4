# Future improvements

## Search titles in the catalog

Owner request, October 8, 2026: add an option to search titles from the Xbox catalog.

**Status: now authorized for the next catalog update.** On October 9, 2026, the owner replaced the earlier deferral: first show the complete available catalog, group confirmed access and titles needing purchase when explicitly established, and add working case-insensitive substring search so gear/forza return every match. Unconfirmed entitlement alone does not establish a purchase requirement. Cover images are a subsequent improvement.

Version 0.7.28 prepares a bounded capacity of 4,096 distinct entries instead of 128, with cached snapshots and explicit truncation. The owner confirms complete retention and the menu behavior; the console retains 2,734/2,734 titles. The 0.7.29 local search/filter candidate is prepared, with console behavior pending. Search must distinguish the available catalog from retained or named entries, support DualShock 4 text entry and return to selection without losing the account session. Store name enrichment currently covers only the first 32 entries; identifiers remain available for others.

Live audio/video and controller input now work in the owner's trials. The current priorities are remaining pauses, complete catalog/search/access groups, and unobstructed playback.

## Connection quality check

Owner request, October 9, 2026: check whether the current network is suitable for cloud play, preferably against the real selected xCloud destination. Investigate the service-provided region/base URI and the actual session/ICE media destination; do not substitute a generic speed test or HTTPS duration for multimedia RTT, jitter or wire loss. State which target/protocol/stage each result measures and which quality dimensions remain unknown. No viability test or fixed quality threshold is implemented at this checkpoint.

## Extended cloud queues and waiting UI

The owner's queue question exposed a current limit: `WaitingForResources` is recognized as waiting, but initial provisioning is bounded to **180 seconds (three minutes)**. Extended queue support is incomplete.

**Status: recorded for a later update; no runtime change implemented.**

Review an appropriate longer waiting duration and clear queue/cancellation UI using actual server state. Present a server estimate only when one is supplied and validated; do not invent waiting times. Preserve cleanup and cancellation while researching the required session-maintenance behavior.

Both 0.7.10 attempts reached `ReadyToConnect`, accepted `/connect` and `Provisioned` before their SDP-stage failures. Those captures do not establish a long-queue failure or a server queue-time estimate.
