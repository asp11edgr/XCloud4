# Xbox connection authorization

## Confirmed milestone: 0.6.2

The owner and Klog confirm **Passport HTTP 200**, **`/connect` HTTP 202** and automatic **DELETE HTTP 200**, without a cleanup error, in 0.6.2. This milestone uses the authorized temporary public client also referenced by GreenVita. The own XCloud4 registration is retained. **SDP/ICE negotiation and game media were not implemented in this confirmed version.** Accepted authorization is not evidence of a connected WebRTC stream.

The confirmed package is `XCloud4-0.6.2.pkg`, 6619136 bytes, SHA-256 `25c724aca93728d53c9d4c6b7e52f0acff3dff45c263779bdbcc87e25699b601`. It was copied to `/data/pkg`, downloaded back over FTP and matched to the local hash. The source is preserved as `v0.6.2`.

## Scope and protocol

After preparing a selected title using the session flow confirmed in 0.5.0, the worker renews Microsoft access, obtains a Passport console-transfer token and sends one POST to that session's regional `/connect` resource.

The protocol is researched from [GreenVita authentication](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/auth.rs) and [GreenVita sessions](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/stream.rs), with an original C implementation on the existing native HTTPS layer.

## User flow in the confirmed version

1. Authorize Microsoft in `CUENTA`, open the catalog and select a title marked `CON ACCESO` with X.
2. Wait for remote preparation and automatic connection authorization.
3. After Xbox accepts `/connect`, the Spanish UI shows `XBOX ACEPTO LA CONEXION`. The session is held for at most 45 seconds, then deleted automatically.
4. Circle requests closure before returning to the catalog; `OPTIONS` requests closure before exiting.
5. On refusal, retain the displayed stage, numeric detail and HTTP status. The application attempts to delete any known session even when Passport or `/connect` fails.

The automatic close path is confirmed. Manual cancellation and `OPTIONS` with an active remote session have not been checked separately.

## Earlier versions and observed failures

### 0.6.0

Version 0.6.0 used the own XCloud4 registration. It built and packaged without errors or warnings. Package: 6619136 bytes, SHA-256 `3b83acf2c2e4f878b447ee70542edf247dec0822516c6d95135450c107404474`; the FTP round-trip copy matched.

The owner's photo shows 1000XRESIST, Microsoft's connection-authorization refusal, HTTP 400 and detail `0xfffff828`. This detail is a local OAuth-refusal code, not a specific Microsoft error. Klog confirms remote readiness, Microsoft renewal HTTP 200, Passport HTTP 400 with 211 bytes and no `/connect` request. DELETE returned HTTP 200 with no cleanup error.

That version wiped the response before classifying its OAuth `error`, so the specific cause was not available. It was not tagged as a confirmed authorization milestone; `v0.5.0` remained the confirmed base at that time.

### 0.6.1 diagnostic

Claude Opus 5.5 added safe classification of nine known OAuth codes before wiping the response. The UI shows a fixed phrase and allowlisted code; any other value becomes `unknown`. Logs include only that fixed code and, when present, the first valid integer in `error_codes`. They do not include `error_description`, response bodies, tokens or account data. Passport `invalid_grant` does not clear the account: it does not itself invalidate the Microsoft renewal that just succeeded.

The error heading no longer inherits a success color from prior session readiness. No retries or scope change were added. Package: 6619136 bytes, SHA-256 `bcae7dc054a35fe5d1fe92e8c1eb9b4a2c064f6e1a14193a23203833978bee3e`; its FTP round-trip hash matched.

PS4 execution confirmed **`invalid_scope`**, remote DELETE HTTP 200 and no `/connect` request. See [the Passport investigation](INVESTIGACION_PASSPORT.md).

### 0.6.2 temporary profile

With the owner's prior authorization, 0.6.2 uses the public reference client throughout the complete authentication flow. The interface and log announce that temporary profile. The own registration is preserved; there is no automatic client switch after a failure and no token mixing. Endpoint, scope and diagnostics are unchanged. New device-code authorization is required, and Microsoft's consent page may show a different application name from XCloud4.

The owner confirmed completion without error. Klog records Microsoft renewal HTTP 200, Passport HTTP 200 (1090 bytes), `/connect` HTTP 202 with an empty response, AUTHORIZED, and automatic DELETE HTTP 200. Final state is CLOSED, with `connection_authorized=1`, `cleanup_failed=0` and error zero.

## Credentials, cancellation and response validation

Microsoft access and refresh tokens stay private and in memory. Renewal candidates are accepted only after validating the entire response; the previous refresh token is retained if no replacement is returned. An exact `invalid_grant` from Microsoft renewal clears the local account and catalog. A Passport refusal is reported as an authorization error.

Renewal has a 30-second deadline and completes before honoring cancellation so a rotated refresh token can be retained. Passport and `/connect` honor cancellation. Once a validated session path is known, a bounded DELETE is attempted even after cancellation. `/connect` is not repeated and the Microsoft account is not closed globally.

The Passport token is used only to construct bounded `userToken`, with validated characters, then wiped after sending. Logs contain fixed stages, states and HTTP codes, not credentials or response bodies. `/connect` requires a 2xx status and either an empty body or a valid JSON object without non-null `errorDetails`; other JSON root types are rejected.

The UI and log distinguish preparation, authorization and closure. `connection_authorized` records `/connect` acceptance and remains in the final diagnostic result. It does not indicate video reception.
