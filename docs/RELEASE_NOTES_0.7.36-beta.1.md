# XCloud4 0.7.36 Beta 1: graphical catalog reports

## Documentation-only publication

This beta checkpoint publishes reports and their labeled host captures only,
at the owner's explicit request. It includes **no PS4 installer**, no 0.7.36
runtime source update, no font/atlas assets, no new dependency sources and no
test implementation. Existing public runtime sources and historical releases
are preserved. GitHub's automatically generated source archives describe this
documentation tag; they are not a buildable release of the 0.7.36 UI.

No new build, test campaign or streaming session was run for publication.
Streaming experiments remain paused. The local UI binary retains its frozen
configuration; no NACK/grace, decoder, queue, rumble or network changes were
made to prepare these reports.

## Report index

- [UI implementation, prior host/build results and limits](UI_REPORT_0.7.36.md).
- [First PS4 graphical catalog validation](CONSOLE_UI_REPORT_0.7.36.md).
- [Public metadata and bounded cover cache](UI_PUBLIC_ASSETS.md).
- [Server xCloud recently played titles](UI_SERVER_RECENTS.md).
- [Historical build verification receipt](BUILD_VERIFICATION_0.7.36.json).
- [Six labeled production-renderer host captures](ui/0.7.36/).
- [Package inspection receipts, without log transcripts](ui/0.7.36/verification/).

The historical build receipt's `not_done` array describes the pre-console
checkpoint. Later installation and actual-account MRU evidence are recorded
in the console report; that array is not the current validation checklist.
Paths to new code, assets, licenses and tests in the reports describe the
local prepared workspace, not additions available in this documentation tag.

## Actual console checkpoint

The owner manually installed the local 0.7.36 package and reported that it
works well. The private kernel capture and supplied photo establish:

- 2,734 validated catalog titles, all retained; page 1/456 is visible.
- Six catalog cards, recent cards, loaded covers and selected-title details.
- Actual account MRU: HTTP 200, valid `results[].titleId`, 25 entries.
- Runtime source identity `5755881046bc173055a9fa2b5f82c1271f49a373`.

This does not establish every navigation/account-switch/error-retry case or
clean menu exit individually. The photo crops the right edge; a full TV-margin
check remains pending. Game launch and streaming quality are outside this
menu pass. Raw console logs, the owner's private photograph, history IDs,
tokens, credentials, SDK files and generated binaries are not published.

## Reversal

This checkpoint changes documentation only. Existing 0.7.35 source and release
artifacts remain available. No installer action or account-history deletion
is required to stop using the reports. Local 0.7.36 implementation work is
preserved separately and must not be discarded with a repository-wide reset.
