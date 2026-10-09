# Investigating Passport `invalid_scope`

## Console evidence

With XCloud4's own registration, 0.6.1 completed Microsoft authorization, catalog access, game preparation and Microsoft renewal. Passport returned **HTTP 400** with the exact OAuth code **`invalid_scope`**; `/connect` was not sent. DELETE returned HTTP 200 and confirmed closure. The owner's photo matches Klog. No numeric subcode was obtained.

This places the refusal at the connection-permission request. It does not by itself establish a particular Microsoft policy or a fault in the account, network or PS4.

## Comparison of primary sources

| Source | Observation in its pinned code/documentation |
|---|---|
| GreenVita, `ae2625d295b4fba005a769b1309fd70dcd6cb63f` | Uses `1f907974-e22b-4810-a9de-d9647380c97e`, prior Microsoft renewal and the same Passport scope at `login.live.com/oauth20_token.srf`; reads `access_token` and sends `userToken` to `/connect`. |
| Stratix, `59d804185192f0c0f618836aace9229b77ce48e4` | Uses the same identifier, endpoint and scope; encodes the form and reads `access_token`. |
| Microsoft MSAL repository issue #3491 (2021) | Shows the same identifier with the Passport scope; the author labels the source Internal (Microsoft). This is historical usage evidence, not confirmation of the own registration's current permission. |
| Microsoft Identity documentation and OAuth RFC 6749 | `invalid_scope` indicates an invalid scope; OAuth also covers unknown, malformed or excessive requested scopes. |

XCloud4 form-encodes values and requests the scope used by those implementations:

```text
service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN
```

The identified difference was the OAuth client: the own registration uses `f9ac8684-1032-4131-bb47-d2f58da9bb93`. At that stage, client-specific permission refusal was a hypothesis. No public procedure specifically enabling this Passport scope for a new registration was found in the consulted guides; this does not establish that no such procedure exists.

## Confirmed comparison in 0.6.2

With the owner's authorization, 0.6.2 uses the public reference identifier also used by GreenVita. It is not this project's own registration, and ownership by GreenVita is not assumed. The interface announces a temporary reference client. Microsoft displays the name associated with that identifier; no particular name is presumed.

All steps — device code, token exchange, renewal and Passport — use the same client during the execution. A new sign-in is required. Tokens from the own registration are not reused with another identifier, and no automatic client switch occurs after refusal. The own identifier remains in `auth_profile.h`, selected at compilation.

The owner confirmed no error. Klog identifies 0.6.2 and records Microsoft renewal **HTTP 200**, Passport **HTTP 200** (1090 bytes), `/connect` **HTTP 202** with an empty response, and automatic DELETE **HTTP 200**. The final state is CLOSED, `connection_authorized=1`, `cleanup_failed=0`, error zero.

**Conclusion supported by this comparison:** the reference client accepts the same Passport flow that the own client refused. The outcome establishes a difference associated with the selected client. It does **not** establish Microsoft's exact registration policy or a verified portal setting that would enable the own client. The original registration remains available while work proceeds with the temporary profile.

This confirms connection authorization only. No SDP/ICE, game video, game audio or game input was received in 0.6.2.

## Sources

- [GreenVita authentication](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/auth.rs).
- [Stratix MicrosoftAuthService](https://github.com/nafields/stratix/blob/59d804185192f0c0f618836aace9229b77ce48e4/Packages/XCloudAPI/Sources/XCloudAPI/Auth/MicrosoftAuthService.swift).
- [Microsoft MSAL issue #3491](https://github.com/AzureAD/microsoft-authentication-library-for-js/issues/3491).
- [Microsoft identity error codes](https://learn.microsoft.com/en-us/entra/identity-platform/reference-error-codes).
- [OAuth 2.0 RFC 6749, section 5.2](https://www.rfc-editor.org/rfc/rfc6749#section-5.2).
