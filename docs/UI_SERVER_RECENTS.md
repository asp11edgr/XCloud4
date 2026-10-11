# Server xCloud recently played titles

This is a documentation-only beta report. The 0.7.36 implementation and
test/notice files referenced below remain local and are not provided by the
beta tag. Its actual-account console evidence is linked separately; historical
host fixtures and receipts are not represented as authenticated responses.

The recent section uses the cloud service's MRU, not a new local history.
Its validated title IDs stay only in temporary memory. No history files,
account hash, tokens, credentials, deletion requests or resume feature are
added. A recent entry must resolve against the current validated catalog
before launch; previous play does not prove current access.

## Primary implementation evidence

Greenlight was inspected at upstream commit
`07a3f01cba527c9c8287f90ef326c5e89c64687b`, retrieved 2026-10-11 UTC:

- [getRecentTitles route](https://github.com/unknownskl/greenlight/blob/07a3f01cba527c9c8287f90ef326c5e89c64687b/packages/desktop/main/helpers/xcloudapi.ts#L158):
  GET `/v2/titles/mru?mr=25`.
- [MRU result consumption](https://github.com/unknownskl/greenlight/blob/07a3f01cba527c9c8287f90ef326c5e89c64687b/packages/desktop/main/ipc/xcloud.ts#L73):
  `results` contains objects with `titleId`, consumed in returned order.
- [Recent titles presentation](https://github.com/unknownskl/greenlight/blob/07a3f01cba527c9c8287f90ef326c5e89c64687b/packages/desktop/renderer/pages/xcloud/home.tsx#L17).

The reference above is evidence from the primary client implementation. The
parser's deterministic inputs are explicitly synthetic fixtures derived from
that shape. The subsequent first live PS4 UI pass accepted the owner's actual
account MRU: HTTP 200, valid `results[].titleId` schema and 25 entries, with
populated recents in the owner's photo. This validates the live parser/route
path; full server-order and account-switch checks remain pending. No game was
started for this UI validation. See [the console report](CONSOLE_UI_REPORT_0.7.36.md).

The inspected Greenlight revision uses MIT (copyright 2019 UnknownSKL/Jim
Kroon). Its TypeScript implementation was not copied; only the public
protocol path and consumed JSON fields informed the original C integration.
See `THIRD_PARTY_NOTICES.md` for the pinned license/reference links.

## Credential and worker ownership

The only new authenticated allowlist entry is exact GET
`https://<existing validated region>.gssv-play-prod.xboxlive.com/v2/titles/mru?mr=25`.
The existing verified HTTPS transport retains its certificate verification,
redirect/cookie restrictions, native timeouts, header allowlist and region
validation. No URL is read from MRU metadata and no arbitrary query is accepted.

The existing auth worker owns the GS token, bearer header and raw response.
During initial catalog retrieval the same regional credential obtains titles
then MRU. The titles bearer is wiped after that request; the GS token is wiped
immediately after building the MRU bearer, and that bearer is wiped after its
request. All failure and final cleanup paths also wipe these private buffers.
MRU failure is separate from catalog failure; a catalog can remain usable if
MRU is unavailable.

For a later menu entry or return from a session, `X4_AUTH_XBOX_RECENTS` uses the
same auth bootstrap to acquire fresh regional credentials, then requests only
MRU. It does not request all titles, replace the catalog or prepare a cloud
session. The GUI coordinator suspends public cover work and waits for its idle
gate before requesting this foreground job. The existing auth worker already
serializes jobs and refuses another start while busy.

## Bounded parsing and identity

The MRU response is limited to 256 KiB and at most 25 source entries. The
parser validates JSON, requires `results` to be an array and every element to
have one unambiguous printable ASCII `titleId` of at most 95 bytes from the
existing title-ID character set. Missing/malformed/overlong/oversized inputs
fail closed with zero entries. Duplicates retain their first position. The
parser does not derive permission, purchase status or chronology from optional
`details` or `titleHistory` fields.

The public view contains only state/error/status, up to 25 title IDs, and
opaque account/request epochs. Account epochs are local lifecycle counters,
not identifiers derived from a credential or user hash. Sign-in, forget,
expiry and invalid-grant cleanup clear MRU. Publication validates both epochs;
old jobs cannot overwrite a new account/request. Revision/cancellation-aware
snapshot copying avoids repeated whole-view copies when unchanged.

Only schema validity, count and HTTP status are logged for an MRU response.
Bodies, IDs, timestamps, headers and credentials are not exported by this new
diagnostic. No SDP or media content is touched.

## Verification

`scripts/test_xbox_recent.sh` exercises the production parser with a portable
host harness: ordered titles, deduplication, empty MRU, missing/wrong schema,
invalid Unicode/controls/path characters in IDs, duplicate keys, trailing data,
25-entry acceptance, 26-entry rejection, overlong IDs and oversized response.
Other UI tests resolve provider IDs to the current catalog and reject stale
account/request results. A synthetic fixture or successful local parser test
does not prove the live account endpoint currently returns this schema.

Host results from the established Lubuntu toolchain: parser 41 checks and
auth lifecycle 58 checks passed with GCC, GCC ASan/UBSan and Clang. The exact
HTTP route policy passed 14 allowed/rejected URL cases with GCC and Clang.
The lifecycle harness controls Xbox/auth responses and verifies temporary
MRU identity, cancellation, account changes and preservation of the current
catalog. These are local controlled tests, not authenticated server results.

The production `xbox_live.c` catalog/MRU exchange harness also passed **199
checks** with GCC, Clang and GCC ASan/UBSan. It verifies the exact regional
route, private token/header lifetime before public Store calls, full private
buffer cleanup, isolated MRU errors, cancellation and the MRU-only job.
Controlled fake responses are used throughout. The host-only Clang harness
suppresses its C23-extension diagnostic for an existing label/declaration in
the included streaming translation unit; that unrelated code is discarded
by the test link and was not modified. These checks do not execute streaming
or validate an authenticated service response.
