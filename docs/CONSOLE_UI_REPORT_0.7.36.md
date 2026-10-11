# XCloud4 0.7.36: first graphical catalog console validation

## Scope and identity

Validated on 2026-10-11 UTC using the already compiled UI package. The owner
installed the package manually and reported that it works well. This pass
checks the menu; it is not a streaming, recovery or G0/G45 comparison. No
runtime code, networking, NACK/grace, media queue or rumble setting was changed
to obtain this evidence. No publication was performed during the validation
pass; this later beta checkpoint publishes its report only. The 0.7.36 runtime
implementation and installer remain local.

- Package: `XCloud4-0.7.36-ui.pkg`, 13,107,200 bytes.
- SHA-256: `e309ab56fdcde99b3127b4acd3c89f7df27eb8901d5b72cb4b63896a9d30ec06`.
- Observed runtime source identity: `5755881046bc173055a9fa2b5f82c1271f49a373`.
- Frozen build: `5755881046bc173055a9fa2b5f82c1271f49a373-n1-g45-r0`.
- Transfer to the console passed two exact readbacks. The previous 0.7.35
  installers were preserved. Transfer does not itself prove installation;
  the owner's photo and the observed runtime identity establish execution.

## Observed console evidence

| Check | Evidence and interpretation |
| --- | --- |
| Version and startup | Owner's photo shows 0.7.36; the private kernel capture contains the frozen source identity. |
| Catalog retrieval | 2,734 valid titles, all 2,734 retained. The photo shows 2,734 results and page 1/456. |
| Access classification | The catalog reports 578 titles with confirmed access. This does not validate the entitlement of every displayed selection or successful game launch. |
| Actual account MRU | Two recorded responses report HTTP 200, valid `results[].titleId` schema and 25 parsed entries. This is live console/account evidence, distinct from the synthetic host fixtures. |
| Graphical presentation | Photo shows a three-column/two-row catalog, six visible recent cards, loaded artwork, sans serif labels, ellipsis and the selected-title panel. |
| Metadata | Selected recent has a cover, title, category, publisher and description. |
| General owner feedback | The owner reports that it works well. Individual controls and edge cases were not separately reported. |

The retained safe protocol observations are:

```text
XCloud4: catalogo Xbox validos=2734 guardados=2734 con acceso=578
XCloud4: MRU esquema results[] titleId valido=1 count=25 HTTP=200
XCloud4: catalogo Xbox fin estado=2 http=200 error=0x00000000 xerr=0 titulos=2734/2734
```

Each of these three lines occurs twice in the retained snapshot. This does
not independently identify which user action triggered each request. The
MRU body, individual history IDs, credentials and request headers were not
exported. Schema acceptance and visible results do not independently verify
the entire server order or account-switch isolation.

## Evidence retention and coverage

The private kernel snapshot was frozen at **2026-10-11T20:50:52.389532+00:00**.
It contains 72,493 bytes and has SHA-256
`1b288686b6186a735423768c0dabb87ef8b67351fdd9f14177b779b0e4e58432`.
The original owner photo has SHA-256
`d0cf813662132a4494a4f856a505b4e11fd310693baaf19bfd8d93ad1938c670`.
Both were preserved outside Git. The bounded capture was still connected at
the snapshot; this is not a closed-session trace or an exit result.

The photograph crops the right edge. It establishes the visible layout,
but not the complete right TV margin, title wrapping or full description.
No per-frame performance or memory measurement is inferred from a still photo.

## Remaining console checks

Six-card paging and the incomplete final page, group changes, search,
selection retention during refresh, missing-image/error retry, account changes,
cover cancellation and clean menu exit remain unverified individually on this
UI build. Host results for those cases remain recorded separately in
[the build/UI report](UI_REPORT_0.7.36.md).

Game launch and return from a streaming session remain excluded from this pass.
The previous package remains available for reversal; no server history was
created, deleted or replaced by a local-history fallback.
