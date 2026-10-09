# Microsoft authentication and Xbox credentials

## Current confirmed state

Native HTTPS and Microsoft device-code authorization were confirmed in **0.3.1** with XCloud4's own registration. The Xbox catalog and credential exchanges were confirmed in **0.4.0**. Session creation/deletion followed in **0.5.0**; **0.6.2** confirms Passport and `/connect` using an authorized temporary reference client. Actual WebRTC game media remains pending.

The own registration is retained, but its Passport request returned `invalid_scope` in 0.6.1. Version 0.6.2 uses the public client identifier also used by GreenVita for the entire authentication flow. See [registration](REGISTRO_MICROSOFT.md), [connection authorization](AUTORIZACION_CONEXION.md), and [the investigation](INVESTIGACION_PASSPORT.md).

## Device-code flow

1. On the owner's request, ask Microsoft for a device code. Display the verification address, user code and remaining time.
2. The owner authorizes access from a phone or PC browser. The password is entered on Microsoft's website.
3. Poll at the returned interval and respect expiry. Handle pending authorization, refusal, expiration and cancellation.
4. After authorization, exchange Microsoft credentials for Xbox and cloud credentials using the protocol researched in GreenVita.
5. Request the catalog and display actual title identifiers and service-reported access. Account authorization alone does not demonstrate streaming eligibility or a playable game.

## Native implementation

- Native SDK HTTP/TLS libraries (`Http.h`, `Ssl.h`, `Net.h`) are resolved when needed, as with the local media sample. Certificate validation remains enabled.
- Requests run outside the drawing thread, with request and response limits. The UI continues reading the controller while authorization is pending.
- Bounded JSON parsing checks types and distinguishes transport, protocol and service failures.
- Tokens remain in private memory for the current execution. They are not printed, persisted, packaged or committed.
- Microsoft renewal is implemented for connection authorization in 0.6.x; persistent account storage is future work. Valid rotated refresh tokens are retained before a cancellation is honored.
- One client profile is selected for device code, token exchange, renewal and Passport. Tokens are never reused across client identifiers.

## Version history

### 0.3.0: first account view

The confirmed 0.2.2 local media/exit baseline was extended with `CUENTA`. X requests device-code access, Square checks HTTPS using public Microsoft metadata, and Triangle clears the local account. Entering the view does not request a code automatically. Circle cancels a pending action and returns; `OPTIONS` keeps the interface responsive while cancellation finishes, then uses the confirmed native exit path.

Xbox credential exchanges and catalog access were not part of that version. The initial plan had no emitted Microsoft token until execution on the console; later results below supersede that historical pending status.

### 0.3.1: HTTPS and account confirmed

Version 0.3.0 failed to locate SSL before sending an HTTPS request. Version 0.3.1 expands module lookup through exports and native sandbox paths. The owner's photo shows an authorized Microsoft account. Klog confirms the connection check with HTTP 200 and authorization ending in `X4_AUTH_AUTHORIZED`, HTTP 200, error zero. The token stays in memory until the application closes.

### 0.4.0: Xbox credentials and catalog confirmed

Microsoft access, RPS, XSTS, `xgpuweb` cloud credentials and the regional catalog all returned HTTP 200 on PS4. The photo and Klog agree on 2733 titles received and 128 retained locally. Klog also confirms 32 Store names and completion without errors. See [catalog evidence](CATALOGO_XBOX.md).

### 0.6.2: connection authorization confirmed

After a new device-code authorization with the temporary reference client, Microsoft renewal returned HTTP 200, Passport HTTP 200 and `/connect` HTTP 202. Automatic session deletion returned HTTP 200. This establishes authorization acceptance; it does not establish WebRTC, game video/audio or game input.

## Sources

- [Microsoft OAuth device-code flow](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code).
- GreenVita, MPL-2.0, `src/api_xbox/auth.rs`, pinned in [REFERENCIAS.md](REFERENCIAS.md). Its protocol is researched; the Rust implementation has not been copied.

No automated tests were added or run. Console outcomes are documented separately from static review and compilation.
