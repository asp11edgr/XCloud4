# XCloud4 0.7.29 — catalog search and access groups

This checkpoint adds local navigation over the retained 4,096-entry bounded catalog. It does not change provider entitlement, authentication, network endpoints, native playback, audio, controller reports or recovery policy. Build and hardware status are recorded separately in the [release notes](RELEASE_NOTES_0.7.29.md).

## Search and access meaning

Search matches a **case-insensitive contiguous substring** of either the available display name **or** the title identifier. Queries such as `GEAR` and `FORZA` return entries whose available name or identifier contains that text, subject to the selected access group. Complete results by commercial name remain unconfirmed while most Store names are unavailable. Clearing the query restores all entries in that group. No matches is an explicit empty-result state, not a session launch.

The groups are **all**, **confirmed access**, and **unconfirmed access**. Group totals describe the full retained list, independently of the query; the match count describes the current query and group. `entitled=0` is unconfirmed access, not an explicit purchase requirement. Confirmed access reflects the provider response and cannot guarantee a later session will succeed.

Names are still enriched only for the first 32 retained titles using the existing Microsoft Store request policy. Remaining titles use their actual identifiers. A query can match an identifier, but a missing display name cannot be searched by words that appear only in that unavailable name. This checkpoint does not claim complete Store-name coverage or a purchase-only list.

## PS4 controls

| Mode | Button | Action |
| --- | --- | --- |
| Results | TRIANGLE | Open the search keyboard |
| Results | L2 / R2 | Previous / next access group |
| Results | D-pad up / down | Previous / next result, wrapping |
| Results | L1 / R1 | Move eight results, clamped to bounds |
| Results | X | Start the selected original catalog title |
| Results | SQUARE | Refresh catalog; retain query/group and title anchor where available |
| Results | CIRCLE | Return to account |
| Keyboard | D-pad | Select a letter or digit in the 6×6 keyboard |
| Keyboard | X | Append the selected character |
| Keyboard | SQUARE | Delete the final character |
| Keyboard | TRIANGLE | Clear the query |
| Keyboard | R1 | Insert a space |
| Keyboard | R2 / CIRCLE | Return to filtered results, retaining the query |
| Both | OPTIONS | Existing application cleanup and exit |

The query has a bounded **48-character** ASCII letter/digit/space capacity. It does not provide punctuation, accents, a USB keyboard or the system IME. The search field and match count update as the user edits; applying closes the keyboard. Returning from a game preserves the query, group and selected title where still present.

## Ownership, mapping and lifetime

`X4CatalogBrowser` belongs to main and uses static storage, about **8.2 KiB**, principally a `uint16_t[4096]` index map. It is not a second catalog snapshot and creates no network request. Rebuilding scans the retained list only after a copied catalog revision or a changed query/group. Unchanged playback iterations do not rescan 4,096 titles.

A visible ordinal maps to the **original source index**. Launch requires READY, bounded counts, a valid map entry, a matching saved title ID and the current query/group. Main calls `x4_auth_start_session` only with the validated original index. Empty, stale or invalid selection cannot launch. A refresh can preserve the selected ID across LOADING and a new READY response; changed criteria reset selection to the first match. Invalid/expired catalog state clears the result map. Successful auth-context destruction resets both the catalog cursor and browser.

CIRCLE is consumed locally while editing, preventing unintended account navigation or cancellation. X cannot launch from the keyboard. R2 closes editing there and cycles groups only in result mode. A copied catalog snapshot or any selection/filter navigation press suppresses X for that iteration, requiring a later X edge before a newly selected row can launch. OPTIONS retains the global cleanup path. Existing game-button ownership and L1+R1 playback shortcuts remain unchanged.

The renderer shows eight bounded rows, safely clipped names/IDs, full-list group totals, query match count and an empty-result message. It preserves the hidden gameplay overlay and the loading help above controller status. Cover images and active network-quality probes are deferred.
