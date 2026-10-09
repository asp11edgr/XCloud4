# Xbox credentials and catalog — confirmed in 0.4.0

## Confirmed starting point

Version 0.3.1 completed native HTTPS and device-code OAuth with XCloud4's own registration on PS4 12.00. The owner's photo shows authorization and Klog confirms HTTP 200, AUTHORIZED and error zero. The Microsoft token remains in memory for that execution.

## Catalog behavior

From `CUENTA`, R1 opens `CATALOGO DE XBOX` after Microsoft authorization. The application queries Xbox using the current account. The D-pad moves through results, L1/R1 move eight entries, Square refreshes and Circle returns to the account. `OPTIONS` retains the confirmed native exit flow, waiting for cancellation of a pending request.

The list has an explicit local limit of **128 entries**. Names come from the public Microsoft Store catalog when available; otherwise the actual Xbox identifier is shown. Catalog success is not evidence of a game starting. Service-reported access and actual streaming availability are separate results.

## Credential exchanges

1. Microsoft token → Xbox user token through RPS.
2. Xbox user token → XSTS for `http://gssv.xboxlive.com/`.
3. XSTS → `xgpuweb` cloud credentials, with `xgpuwebf2p` as an alternative after an eligible refusal of the primary offering.
4. Service-returned default region → `/v2/titles`, using its cloud credential.
5. Returned Store identifiers → public names localized to MX/es-MX.

These calls were confirmed from PS4 by Klog and the owner's photo. Version 0.4.0 did not create a game session or add WebRTC. Credentials stay private on the network worker and are not written to files or logs.

## Protocol references

GreenVita files pinned at `ae2625d295b4fba005a769b1309fd70dcd6cb63f`: `src/api_xbox/auth.rs`, `api.rs`, `game_catalog.rs` and `catalog.rs`. XCloud4 implements the protocol in original C; it does not copy Rust source, keys or user data. The catalog milestone used XCloud4's own OAuth identifier. The later authorized temporary identifier in 0.6.2 is documented in [registration](REGISTRO_MICROSOFT.md). See [references and licenses](REFERENCIAS.md).

## Implementation and package

Claude Opus 5.5 implemented the exchanges, JSON requests and large-response reader. Codex integrated the UI and reviewed the code, removed XErr interpretations that lacked evidence, adjusted the total to valid received entries (the displayed list is deduplicated), and added cancellation during catalog traversal. The local-limit indicator activates only when entries are discarded for capacity. The implementation built and packaged in Lubuntu without errors or warnings.

`XCloud4-0.4.0.pkg`: 6619136 bytes, SHA-256 `bd7ae0d4ee3a213295695d6f7372f99622a34a5644e70a01f256b45b30ec57b1`. The file downloaded back from `/data/pkg/XCloud4-0.4.0.pkg` matched the original hash.

Names are requested for the first 32 entries in batches of eight. A failed name batch does not erase the catalog. Xbox credentials are wiped when the action ends; catalog error or cancellation preserves the Microsoft token while it is valid. No automated tests were added or run.

## Actual console result

Klog confirms HTTP 200 for RPS, XSTS, `xgpuweb` cloud login and the regional list. The response contains **1247270 bytes**, **2733 valid received entries**, **128 retained entries**, and **21 of those marked with access by the service**. Four Store batches returned HTTP 200 and eight names each, for 32 names. The action ended READY, HTTP 200, error zero and XErr zero. The owner's 0.4.0 photo shows the catalog, 2733 total, the 128 local limit, names and access labels.

The own XCloud4/PS4 device description was accepted. The `xgpuwebf2p` alternative was not needed. This establishes catalog and credential exchanges, not game streaming. Navigation, refresh and cancellation were not checked separately. Later session milestones are documented in [SESION_XBOX.md](SESION_XBOX.md) and [AUTORIZACION_CONEXION.md](AUTORIZACION_CONEXION.md).
