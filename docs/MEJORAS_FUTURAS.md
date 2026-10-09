# Future improvements

## Search titles in the catalog

Owner request, October 8, 2026: add an option to search titles from the Xbox catalog.

**Status: recorded for a later update. Do not implement during the current stage.**

Before implementation, agree on DualShock 4 text entry and how to query or navigate titles beyond the currently retained 128 entries. Search must distinguish the full catalog from the local list and allow returning to selection without losing the account session.

The current priority is game-session connection, WebRTC, actual video/audio and controller input.

## Extended cloud queues and waiting UI

The owner's queue question exposed a current limit: `WaitingForResources` is recognized as waiting, but initial provisioning is bounded to **180 seconds (three minutes)**. Extended queue support is incomplete.

**Status: recorded for a later update; no runtime change implemented.**

Review an appropriate longer waiting duration and clear queue/cancellation UI using actual server state. Present a server estimate only when one is supplied and validated; do not invent waiting times. Preserve cleanup and cancellation while researching the required session-maintenance behavior.

Both 0.7.10 attempts reached `ReadyToConnect`, accepted `/connect` and `Provisioned` before their SDP-stage failures. Those captures do not establish a long-queue failure or a server queue-time estimate.
