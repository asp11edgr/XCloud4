# Xbox session preparation — 0.5.0

## Historical scope

From the catalog, X requests preparation of the selected title. The view shows the service response and closure. Version 0.5.0 did not receive game media, send controller messages or negotiate SDP/ICE. Preparation and automatic closure are confirmed on PS4.

Connection authorization was subsequently confirmed in 0.6.2; see [AUTORIZACION_CONEXION.md](AUTORIZACION_CONEXION.md). Actual streaming remains under development.

## Confirmed result on October 8, 2026

The owner's photo shows AMONGUS, the Spanish messages that Xbox prepared the session and was ready to negotiate, and 34 seconds remaining before automatic closure.

Klog identifies XCloud4 0.5.0 and confirms creation **HTTP 202**, resource wait **HTTP 200**, and transition to READY. It then records DELETE **HTTP 200** with an empty response. Final state is CLOSED, error zero, `ready_seen=1`, `cleanup_failed=0`, cleanup HTTP 200. The own XCloud4/PS4/Orbis request was accepted for preparation.

This milestone is preserved as `v0.5.0`. It did not confirm Passport, WebRTC, game media or game input. Manual cancellation during creation, closure through Circle and `OPTIONS` with an active session were not checked separately.

The protocol is researched from [GreenVita](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/stream.rs) and implemented in C for the native environment. The 0.5.0 credentials used XCloud4's own registration. The request describes Sony PS4 / Orbis.

## User flow

1. Authorize Microsoft in `CUENTA` and open the catalog with R1.
2. Select a title, preferably marked `CON ACCESO`, and press X.
3. Wait for Xbox. The service may queue or refuse the request.
4. If Xbox reports ReadyToConnect or Provisioned, the UI shows that the session is prepared. This does not confirm a WebRTC connection or visible game.
5. Circle requests closure and returns after the worker finishes. `OPTIONS` waits for closure before requesting the PS4 menu.

Preparation is limited to three minutes. In the confirmed preparation/authorization milestones, a ready session is held for at most 45 seconds, then closed automatically. Unconfirmed closure remains an error, rather than being presented as successful cancellation.

## Ownership and limits

- One worker performs account, catalog or session actions; the UI thread does not make network requests.
- The Microsoft account and catalog are retained during preparation while the token is valid.
- Xbox credentials, regional origin and session path stay private and are wiped when the action ends.
- Public snapshots contain states, title name, timing and numeric errors, not tokens, paths or session identifiers.
- Creation is not retried automatically after transport failure; it may already have reached the server.
- Closure uses DELETE on a validated server path with a bounded timeout, even after cancellation.
- Only expected methods/routes under the regional Xbox domain are allowed. TLS validation, disabled redirects and transport limits are retained.

Media reception and controller channels are documented in [WEBRTC_PS4.md](WEBRTC_PS4.md).
