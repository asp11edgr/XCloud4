# XCloud4 0.6.2 — confirmed connection authorization

This release preserves the connection-authorization milestone confirmed by the owner on PS4 firmware 12.00 with GoldHEN v2.4b18.7.

## Changes

- Select one authentication profile for device code, token exchange, Microsoft renewal and Passport.
- Use the authorized temporary public client identifier also referenced by GreenVita.
- Retain the original XCloud4 registration and announce the temporary profile in the Spanish PS4 interface.
- Preserve bounded HTTPS, verified TLS, private in-memory credentials, safe OAuth diagnostics and remote-session cleanup.

The own registration completes account/catalog requests but Passport returns `invalid_scope`. The temporary reference client accepts the same flow. This establishes a difference associated with the selected client; Microsoft's exact registration policy is not established.

## Confirmed console result

- Microsoft renewal: HTTP 200.
- Passport: HTTP 200, 1090-byte response.
- Xbox `/connect`: HTTP 202, empty response.
- Automatic session deletion: HTTP 200.
- Final state: CLOSED, `connection_authorized=1`, `cleanup_failed=0`, error zero.

The owner reported completion without an error and Klog supports this result. Earlier confirmed local H.264/PCM playback, clean exit, account authorization and catalog functionality remain in the project.

## Limits

This release does **not** receive actual game video/audio or send game input. SDP, ICE and native WebRTC media are the next stage. Accepted `/connect` is not a streaming connection. Manual cancellation and `OPTIONS` with an active remote session were not checked separately. No automated tests were added or run.

The PS4 UI remains Spanish. Repository documentation and GitHub content are English. The requested repository is private.

## Package

- Filename: `XCloud4-0.6.2.pkg`.
- Application identifier: `XCLD00001`.
- Size: 6619136 bytes.
- SHA-256: `25c724aca93728d53c9d4c6b7e52f0acff3dff45c263779bdbcc87e25699b601`.

The transferred package was downloaded back from the PS4 over FTP and matched the original hash. Install through GoldHEN Package Installer and accept replacing XCloud4. See [installation instructions](INSTALACION_PS4.md), [connection authorization](AUTORIZACION_CONEXION.md), [the investigation](INVESTIGACION_PASSPORT.md), and [third-party notices](../THIRD_PARTY_NOTICES.md).
