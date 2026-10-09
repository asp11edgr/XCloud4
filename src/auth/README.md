# Authentication and Xbox sessions

`http_client` resolves native Net/Ssl/Http on a worker thread, with TLS certificate validation, bounded requests and restricted destinations. `json` validates bounded responses and extracts root fields; its span reader supports complete validation up to 2 MiB. `device_auth` handles Microsoft device-code authorization, polling, cancellation, expiry and token renewal without blocking the UI.

Tokens live only in private memory. UI snapshots and Klog do not receive tokens or device codes. The owner's password is entered on Microsoft's website from a phone or PC. Native HTTPS and Microsoft authorization were confirmed in PS4 version 0.3.1.

`xbox_live` runs on the same worker. The catalog flow is Microsoft → Xbox user token (RPS) → XSTS for `http://gssv.xboxlive.com/` → `xgpuweb` credentials, or `xgpuwebf2p` after an eligible 4xx refusal → default-region `/v2/titles` → Microsoft Store names (MX/es-MX) for the first 32 titles in batches of eight. Regional Bearer headers and other destinations are restricted. Xbox credentials are wiped when the action ends; public snapshots contain no tokens, user hashes or response bodies. The catalog was confirmed in 0.4.0.

Session creation/readiness/deletion were confirmed in 0.5.0. Version 0.6.2 also confirms Passport HTTP 200 and `/connect` HTTP 202, using the authorized temporary public client referenced by GreenVita. The own registration remains available in `auth_profile.h`; one selected client is used throughout each authorization flow. The own client's Passport `invalid_scope` refusal is documented separately. Connection acceptance is not evidence of game media reception.

See [registration](../../docs/REGISTRO_MICROSOFT.md), [authentication](../../docs/AUTENTICACION.md), [catalog](../../docs/CATALOGO_XBOX.md), [connection authorization](../../docs/AUTORIZACION_CONEXION.md), and [pinned references](../../docs/REFERENCIAS.md).
