# XCloud4 0.7.36 graphical catalog

**Publication scope:** this beta publishes reports, six labeled host captures
and inspection receipts only. The 0.7.36 implementation, font/atlas assets,
new dependencies, test sources and installer remain local. Paths to those
resources below describe the prepared workspace, not files available in this
documentation tag. The historical build receipt predates the console pass;
use [the console report](CONSOLE_UI_REPORT_0.7.36.md) for its later results.

## Scope and baseline

This is a UI/data change against local HEAD
`46be5fec49122e2ad51486c3468a4b113846acf0`. Existing work is preserved.
Streaming experiments are paused: no cloud games or G0/G45 comparisons were
performed for this change. The build/host phase did not install or publish it.
The owner's subsequent manual console installation and first UI validation are
recorded in [the console UI report](CONSOLE_UI_REPORT_0.7.36.md). This beta
publishes that documentation, without the runtime implementation or installer.

Runtime source identity: `5755881046bc173055a9fa2b5f82c1271f49a373`.
Build mode: MPSC256, four readers, receiver NACK ON, grace 45 ms, rumble OFF.
Resolution, reorder 128/depth 32/25 ms, 16 ms/four-Decode work budget,
recovery, SDP and RTX retain their existing configuration. All 51 tracked
files under `src/streaming`, `src/media`, `src/video`, `src/audio` and
`src/input` were compared with HEAD and are unchanged. The cloud-session
implementation below its existing boundary in `xbox_live.c` is also unchanged.
Only the catalog coordinator suspends cover work before entering that flow.

## Delivered UI

- CPU renderer at 1920×1080; safe margins, dark background, restrained green
  focus, six cards in three columns/two rows, selected-title details on the right.
- Actual product names, category, publisher and description when returned by
  Microsoft. No fixed game list or inferred genre/access data is introduced.
- Progressive public product metadata and artwork. Missing/corrupt images
  produce a placeholder; navigation does not wait for a cover.
- Noto Sans Regular/Semibold from a precomputed atlas, with a smooth embedded
  fallback. Case, Latin accents, ñ, ü, ¿ and ¡ are retained. Measured UTF-8
  truncation never cuts a code point. Complex-script shaping and pair kerning
  are not provided by this small CPU text renderer.
- Search spans every validated catalog ID and every name currently available,
  including progressively enriched names retained after image-cache eviction.
  Unknown names outside the requested pages remain searchable by ID until their
  metadata has been visited; this release does not download all names at entry.
- Todos, Con acceso and Por confirmar remain. Por confirmar means unknown
  permission, not a paid or blocked game. Artwork never establishes permission.
- Refresh and asynchronous enrichment preserve selection by validated title ID.
  Movement, page/filter/query changes and new snapshots suppress same-frame X.
  A queued X waits for cover cancellation while retaining its ID; a metadata
  completion defers dispatch without consuming that intent.

### Control guide

| Control | Action |
| --- | --- |
| D-pad left/right | Adjacent card in the focused section |
| D-pad up/down | Grid row; first grid row up enters recents, recents down enters grid |
| L1/R1 | Previous/next six-item page in the focused section |
| Triangle | Existing on-screen search keyboard |
| L2/R2 | Catalog access group |
| Square | Refresh catalog; retry MRU when the failed recent section is focused |
| X | Launch the validated selected title through the existing access/session flow |
| Circle | Account/back; first exits search editing |
| OPTIONS | Existing cleanup and exit |

Incomplete last pages clamp to actual entries. Recent six-card horizontal
positions map to the corresponding three-column grid positions. Search keeps
the existing A–Z/digit keyboard; matching accepts UTF-8/accented names, but the
keyboard has no new accented-character page.

## Server recent titles and credential boundary

The local-history proposal was removed. Jugados recientemente uses only
`GET /v2/titles/mru?mr=25` on the existing authorized xCloud regional origin.
The auth worker owns the GS token, constructs the private bearer, wipes the GS
token immediately, performs the exact allowlisted request, then wipes the
bearer before public metadata work. UI snapshots contain only state/status,
up to 25 validated title IDs and opaque lifecycle epochs.

Server `results[].titleId` order is retained and joined to the catalog's title
IDs. `product_id` is obtained from that catalog mapping, never assumed equal to
`titleId`. Initial catalog retrieval obtains MRU with the same temporary
regional credential. Subsequent catalog entries and returns from a session
schedule one independent recent refresh through the existing auth bootstrap.
Old account/request results are discarded. No repeated polling, history file,
account hash, local success recorder, delete-history request or resume feature
is added. Historical presence does not replace current launch validation.

Failure displays **No se pudieron cargar tus recientes**, with a retry. Empty
results display **Tus juegos recientes aparecerán aquí**. No local list is
substituted. This represents the MRU exposed by xCloud, not necessarily the
account's entire Xbox history.

The route and consumed JSON fields were verified against pinned primary
Greenlight source. The subsequent console pass accepted actual account MRU
responses: HTTP 200, valid `results[].titleId`, 25 entries; the owner's photo
shows populated recents. Full order/account-switch checks remain pending.
Deterministic host MRU fixtures remain synthetic and identified as such;
malformed or unexpected live schema fails closed. See
[server MRU details](UI_SERVER_RECENTS.md) and
[the console evidence](CONSOLE_UI_REPORT_0.7.36.md).

## Assets, memory and licenses

One persistent public-assets worker downloads at most one image at a time.
Demand is limited to 24 deduplicated products, prioritizing the visible page and
an adjacent page. Only inspected Microsoft Store metadata and the exact public
Microsoft artwork host/path are accepted. Verified HTTPS, no redirects, no
credentials and no arbitrary-URL downloader are retained. Results are tagged
with product and refresh generation; the UI consumes only completed metadata.

| Additional allocation | Bound |
| --- | ---: |
| Cover raster design budget, including an in-flight set | 24 MiB |
| 24 completed grid/detail/recent sets | 21,921,792 bytes |
| Compressed image | 2 MiB |
| Image decode allocations | 16 MiB; dimensions ≤2048×2048 |
| Public metadata response | 256 KiB |
| Font atlas plus embedded smooth fallback | <4 MiB |
| Main-thread retained-name overlay | 704,560 bytes |
| Cache slots | Fixed 24, approximately 40 KiB |

Grid/detail/recent rasters are 320×224, 512×288 and 96×96 RGBA respectively.
Images are aspect-fitted once off the drawing thread. Their packed channels are
`0xAABBGGRR`, verified independently with red/blue/alpha fixtures. Existing
framebuffers and streaming allocations are not included in this added-assets
table. No streaming queue or decoder memory was increased.

New redistributed resources:

- Noto Sans, pinned upstream `ffebf8c1ee449e544955a7e813c54f9b73848eac`,
  SIL Open Font License 1.1. Atlas source manifest and notices are packaged.
- `stb_image.h`, pinned `5736b15f7ea0ffb08dd38af21067c314d6a3aae9`,
  MIT alternative, JPEG/PNG only; notices and source hashes retained.
- Greenlight is a protocol/reference source, not a copied runtime dependency.

See [public assets](UI_PUBLIC_ASSETS.md), `assets/fonts/README.md`,
`THIRD_PARTY_NOTICES.md` and `docs/licenses/` for exact provenance.

## Verification and visual provenance

Host tests compile production components on the established Lubuntu toolchain,
using bounded adapters for native synchronization/transport. They cover grid
navigation, partial pages, search/groups, ID preservation, asynchronous and old
account/generation replies, invalid IDs, UTF-8/long names, empty catalogs,
missing/corrupt/oversized images, memory bounds, pinning, cancellation, exact
HTTP policy and private-buffer clearing. Tests do not launch a cloud game.

Final test/build results and binary hashes are recorded separately in
[BUILD_VERIFICATION_0.7.36.json](BUILD_VERIFICATION_0.7.36.json). A fixture run
that aborted while the VM disk was full is retained as a failed attempt, not
counted as a passed memory test. After restoring space, the final renderer
passed all six captures with GCC, Clang and GCC ASan/UBSan. Pixel outputs were
identical between those three runs. The failed run retained no sanitizer
diagnostic, so it is not presented as evidence of a memory bug or its repair.

| Verification | Result |
| --- | --- |
| Font, UTF-8 and clipped blits | PASS, 2,075,185 checks, including 2,073,600 pixel checks |
| Browser/server-MRU navigation | PASS, 12 cases / 501 checks |
| Catalog presentation/demand | PASS, 6 cases / 521 checks |
| Public metadata, covers, bounds and cancellation | PASS, 288 checks |
| Private regional catalog/MRU exchange | PASS, 199 controlled host checks |
| MRU parser | PASS, 41 checks |
| MRU auth lifecycle | PASS, 58 controlled host checks |
| Exact HTTP MRU route policy | PASS, 14 allowed/rejected cases |
| Production GUI captures | PASS, 6 cases; identical GCC/Clang/GCC sanitizer pixels |
| SDK GP4 directory transform | PASS, 9 cases / 39 checks |
| Native OpenOrbis compilation and local package | PASS; final 126 source inputs and 10 dependency archives verified |

GCC normal, GCC ASan/UBSan and Clang normal were used for the C components;
HTTP policy used GCC/Clang normal. Clang's sanitizer runtime is unavailable
and is not counted as a pass. The browser's pending-X case tests helper
composition, not execution of the integrated main loop. The
[package inspection receipts](ui/0.7.36/verification/) are published as JSON;
original test transcripts and all `.log` files remain local.

The local package is `dist/XCloud4-0.7.36-ui.pkg`, 13,107,200 bytes,
SHA-256 `e309ab56fdcde99b3127b4acd3c89f7df27eb8901d5b72cb4b63896a9d30ec06`.
Its native output records XCloud4 / XCLD00001 and SFO APP_VER `01.04`;
the application version displayed by the runtime is `0.7.36`.
The installed package tool passed 28 integrity checks, with no warnings or
errors. Actual extraction verified all five font assets, license notices,
runtime modules and eboot. Thirty of 31 GP4 targets match the staged bytes
exactly. The packager expands SFO from 504 to 1,076 bytes, retaining all eleven
original fields and adding two tool fields; its extracted APP_VER/VERSION are
`01.04`. The extracted eboot contains the final runtime build identity.
These local integrity checks do not establish successful PS4 execution.

The actual SDK project already declared `assets/fonts` as nested directories.
An initial self-closing-string assumption in the new packaging step was
rejected before a package was built. It was replaced with an idempotent XML
directory update. The exact transform passed 9 cases/39 checks, including that
SDK project, preservation of file entries/existing directories and duplicate
rejection. The final source identity includes this packaging correction.

Lubuntu was expanded from 16 to 18 GiB at the owner's request. The verified
post-expansion snapshot had 2,209,857,536 bytes available in ext4. The partition
start, source inputs, SDK/dependency archives and prior native binary were
preserved; the final build subsequently used the source identity above.
Virtual disk capacity is distinct from available Windows disk space.

The six [captures](ui/0.7.36/) use the production font, GUI renderer, browser,
MRU presentation model, public metadata parser and image decoder. Only cache
acquisition/transport is adapted to immutable fixtures. Artwork/names come from
verified public Store responses; access bits, connected-pad state and MRU order
are test inputs. Each capture identifies itself as a **host capture**, not a
PS4 screenshot or an authenticated account result. The fixture artwork is not
bundled as a fixed catalog in the app.

Actual paid Claude Code **Opus 5.5** completed a focused read-only review of a
frozen seven-file UI/API bundle. It found the pending-X/metadata coincidence;
the minimal correction and a helper-composition regression case were added.
The follow-up static review verified that the same pending ID remains until a
later safe frame. This is not an integrated main-loop or HTTP security test,
and no console result is inferred from that review.

### Remaining console checks

The owner has manually installed 0.7.36 and supplied its first console photo;
the capture verifies catalog and actual-account MRU retrieval. Remaining checks
are full TV margins, covers loading while navigating, six-card paging, groups,
search, failure/retry, refresh/selection retention, account changes and menu exit.
Streaming tests remain paused; this checklist does not ask to start a game.
Preserve any existing console packages until this UI is inspected.

## Review and reversal

The runtime change remains local and uncommitted; this beta contains reports
only. The exact prepared changed/new paths are listed in the verification
receipt; no unrelated working files should be reverted.
To reverse, preserve this UI patch and its untracked files first, then restore
only the listed tracked UI/auth/packaging paths from the recorded baseline and
remove only the listed new UI/data files. Do not use a repository-wide reset or
clean. An existing 0.7.35 package remains the previous console baseline; no
server history is changed by this release.
