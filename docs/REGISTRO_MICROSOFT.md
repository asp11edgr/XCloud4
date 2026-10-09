# XCloud4's own Microsoft registration

The owner authorized creating XCloud4 in the personal Microsoft Entra directory. The portal confirmed the registration and saved public-client configuration.

- Name: **XCloud4**.
- Public application/client ID: `f9ac8684-1032-4131-bb47-d2f58da9bb93`.
- Supported accounts: personal Microsoft accounts only.
- Allow public client flows: enabled and saved.
- Live SDK compatibility: enabled, the portal's initial value.
- No redirect URI or client secret for the device-code flow.

This identifier is public and may appear in source. Passwords, device codes and tokens are never included in this document or Git. The own registration was used through 0.6.1 and is preserved.

## Temporary profile in 0.6.2

The own registration completes account access and catalog requests, but Passport refuses the connection permission with `invalid_scope`. With the owner's prior authorization, 0.6.2 temporarily uses public identifier `1f907974-e22b-4810-a9de-d9647380c97e`, also used by GreenVita. It was not registered by XCloud4 and ownership by GreenVita is not assumed. The interface announces the temporary client; Microsoft shows its associated application name during authorization.

One selected client is used for device code, exchange, renewal and Passport throughout the execution. A complete new authorization is required; private tokens from the own registration are never reused with another client. Selection lives in `src/auth/auth_profile.h`.

The owner and Klog confirmed Passport HTTP 200, `/connect` HTTP 202 and automatic DELETE HTTP 200 in 0.6.2, without a cleanup error. This establishes a client-dependent difference from the own registration's refusal, but not Microsoft's exact registration policy. See [the investigation](INVESTIGACION_PASSPORT.md).

Registration itself is not proof that Microsoft has issued Xbox credentials or that a game can stream. Those outcomes require console evidence at their respective stages. The owner authorizes each device-code request on Microsoft's website.

## Microsoft documentation

- [Register an application](https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app).
- [Device-code flow](https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code).
