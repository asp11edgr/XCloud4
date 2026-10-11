# Public catalog metadata and cover cache

This report is published in a documentation-only beta. Implementation,
new dependency/notice files, font assets and tests referenced below describe
the prepared local workspace and are not included in this beta tag. No
installer is distributed by this checkpoint.

This component enriches the presentation catalog. It never changes a title's
access status and never receives Xbox credentials. Public product metadata is
not a substitute for the current authenticated service's access check.

## Verified public schema

Before implementing the parser, an HTTPS GET was made to the Microsoft Store
Display Catalog endpoint on 2026-10-11 UTC:

`https://displaycatalog.mp.microsoft.com/v7.0/products?bigIds=9NKX70BBCDRN&market=MX&languages=es-MX&fieldsTemplate=Details`

The response was HTTP 200, 54,428 bytes. Its retained public fixture and source
receipt are in `tests/fixtures/public_product_displaycatalog.*`. SHA-256:
`0567e32106b6691641cf780d9bb4ea5f4af5d11ff40e23b9e7db339495f3c6ac`.

Observed fields used by the parser:

| Field | Presentation use |
| --- | --- |
| `Products[].ProductId` | Match a validated requested public product ID |
| `LocalizedProperties[].ProductTitle` | UTF-8 title, preserving case and accents |
| `ShortDescription`, with `ProductDescription` fallback | Bounded description |
| `PublisherName`, `DeveloperName` | Optional real provider metadata |
| `Properties.Category` | Optional real category |
| `Images[].ImagePurpose`, `Uri`, `Width`, `Height` | Select inspected public artwork |

The requested response language is `es-MX`; the parser uses the first returned
localized entry, matching the existing Store request. Optional absent values
remain empty. Duplicate matching products are rejected as ambiguous. Missing
names remain an ID in the coordinator. No access, purchase or Game Pass status
is derived from this metadata or an image.

## Download policy

Only `https://store-images.s-microsoft.com/image/apps.<safe asset>` is accepted.
Asset text contains only ASCII letters, numbers, dots, hyphens and underscores.
The exact added query is `?w=512&q=80&format=jpg`. Protocol-relative Microsoft
URIs are converted to HTTPS. Ports, userinfo, escaped paths, extra queries,
redirects, alternate hosts and arbitrary URLs are rejected.

A live request using that exact query was also HTTP 200, `image/jpeg`, 72,921
bytes. That downloaded artwork is kept only as a local visual inspection input;
the offline image fixtures are original synthetic colored JPEG/PNG images.
The cover request accepts no caller-supplied headers, tokens or body. It uses
the existing full TLS/certificate verification, disabled redirects/cookies,
identity encoding, native timeouts and 30-second overall deadline unchanged.

Artwork priority is BrandedKeyArt, Poster, BoxArt, Tile, then TitledHeroArt.
Source metadata dimensions are only advisory; decoded dimensions and actual
download size are independently bounded.

## Memory and scheduling

One persistent worker handles metadata, a single cover download, image decode
and once-only resizing. Demand is at most 24 validated, deduplicated product
IDs in priority order: six visible catalog entries, six adjacent entries, and
up to twelve visible/adjacent recent entries. A 25-title server MRU does not
cause 25 simultaneous cover downloads.

| Allocation | Limit |
| --- | ---: |
| Cached rasters, plus a new in-flight raster set | 24 MiB hard allocation design budget |
| 24 complete cached raster sets | 21,921,792 bytes (20.91 MiB) |
| Grid raster per slot | 320 × 224 × 4 bytes |
| Detail raster per slot | 512 × 288 × 4 bytes |
| Recent raster per slot | 96 × 96 × 4 bytes |
| Compressed cover buffer | 2 MiB + terminator |
| All stb decode allocations including headers/temporaries | 16 MiB |
| Decoded source dimensions | At most 2048 × 2048 |
| Public metadata response buffer | 256 KiB + terminator |
| Cache metadata/slots | Fixed 24 slots, approximately 40 KiB |

Rasters fit the source aspect ratio against a dark letterbox background.
Their native packed pixels match `x4_rgb`/VideoOut: `0xAABBGGRR`, with red
in the low byte. A synthetic red/blue/alpha fixture verifies exact channel
order independently of an artwork's appearance.
Resampling happens once in the worker. Drawing borrows pinned rasters; it does
not decode, download or scale per frame. Pinned or active slots cannot be
evicted. Navigation requests and drawing acquisition use a try-lock and may
retry on a later frame. Completed names may be retained separately by product
in the presentation overlay; this does not mutate the authentication snapshot.

Generation tagging rejects old refresh results. Suspend cancels the current
native request cooperatively, prevents any new request using an atomic start
gate, and returns immediately. The existing native operation can still run
until its existing timeout. The coordinator waits for `idle` before starting
foreground auth/catalog/session work, and resumes only when that worker is
idle. This preserves the existing global module/Net initialization ownership;
no network initialization settings were changed. Cache destruction joins the
worker only at application shutdown, after suspension.

## Dependency and tests

`third_party/stb/stb_image.h` is pinned to upstream commit
`5736b15f7ea0ffb08dd38af21067c314d6a3aae9`. Only JPEG and PNG are enabled;
stdio, HDR and floating-point linear image conversion are disabled. The MIT
alternative license, complete upstream notices, source commit and file hashes
are retained beside the header. No new runtime image library is required.

Run `bash scripts/test_catalog_assets.sh` with GCC or Clang on the established
host. The test compiles the production cache, metadata parser, UTF-8 decoder,
image decoder and URL policy, with a pthread adapter and controlled fake public
HTTP transport. It covers URL/ID rejection, inspected schema, Unicode and safe
truncation, optional metadata, ambiguity, JPEG/PNG, corrupt/oversized images,
bounded 24-slot demand/memory, pinning, stale generations, cancellation,
suspend/start ownership and missing/corrupt image placeholders. The selected
MIT notice is also copied to `docs/licenses/stb_image-MIT.txt` for packaging.

These controlled host transport responses verify local UI/data behavior. They
do not constitute a PS4 networking, visual or streaming measurement.

The final targeted asset suite passed **288 checks** with GCC, Clang and GCC
ASan/UBSan on the established Lubuntu host, including exact red/blue/alpha
packing. No memory/leak or undefined-behavior error was reported by that GCC
sanitizer run. Clang compiler-rt is unavailable on that host, so its plain
pass is not a Clang sanitizer pass. The harness respects the production
nonblocking API: an unavailable try-lock snapshot is not treated as completed
work, and drawing retries acquisition instead of assuming every attempt
must succeed.
