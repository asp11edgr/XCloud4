# Investigación de `invalid_scope`

## Evidencia de la PS4

Con el registro propio de XCloud4, 0.6.1 completa autorización Microsoft, catálogo, preparación de juego y renovación Microsoft. Passport devuelve HTTP 400 con el código exacto `invalid_scope`; `/connect` no se envía. DELETE devuelve HTTP 200 y confirma el cierre. La fotografía del propietario coincide con Klog. No se obtuvo un subcódigo numérico.

Esto sitúa el bloqueo en la solicitud del permiso de conexión. No establece por sí solo una restricción específica de Microsoft ni que la cuenta, red o PS4 estén averiadas.

## Comparación de fuentes primarias

| Fuente | Resultado observado en su código/documentación |
|---|---|
| GreenVita, `ae2625d295b4fba005a769b1309fd70dcd6cb63f` | Usa `1f907974-e22b-4810-a9de-d9647380c97e`, renovación Microsoft previa y el mismo scope Passport en `login.live.com/oauth20_token.srf`; lee `access_token` y envía `userToken` a `/connect` |
| Stratix, `59d804185192f0c0f618836aace9229b77ce48e4` | Usa el mismo identificador, endpoint y scope; codifica el formulario y lee `access_token` |
| Caso del repositorio Microsoft MSAL #3491 (2021) | Muestra el mismo identificador junto al scope Passport; el autor marca su fuente como Internal (Microsoft). Es evidencia histórica del uso, no confirmación actual del permiso de nuestro registro |
| Microsoft Identity y OAuth RFC 6749 | `invalid_scope` identifica un scope inválido; OAuth contempla también un scope desconocido, mal formado o superior a lo concedido |

El formulario de XCloud4 codifica sus valores correctamente y el scope coincide con esas dos implementaciones. La diferencia identificada es el cliente OAuth: nuestro registro usa `f9ac8684-1032-4131-bb47-d2f58da9bb93`. Que el servicio no admita ese permiso para el registro propio es una hipótesis razonable, aún sin confirmar. No se encontró en las guías consultadas un procedimiento público específico que habilite este scope Passport para un registro nuevo; no se afirma que no exista.

## Siguiente diagnóstico preparado

La 0.6.2 usa temporalmente el identificador de referencia que utiliza GreenVita, con autorización previa del propietario. Ese identificador no es el registro propio de XCloud4 ni debe presentarse como una aplicación creada por GreenVita. La pantalla informa que es un cliente temporal de referencia. La autorización en Microsoft mostrará el nombre asociado a ese identificador; no se presume cuál será.

Todos los pasos (código, intercambio, renovación y Passport) usan el mismo cliente en toda la ejecución. Es necesario iniciar sesión nuevamente; los tokens del registro propio no se reutilizan con otro identificador. No hay cambio automático de cliente después de un rechazo. El identificador propio se conserva en `auth_profile.h`, con selección de perfil al compilar.

Si el cliente de referencia acepta Passport y el propio lo rechaza, tendremos evidencia concreta de una diferencia vinculada al cliente. Si también rechaza el mismo scope, habrá que investigar el endpoint o concesión inicial antes de atribuirlo al registro. El resultado todavía está pendiente: código fuente de otros clientes no demuestra funcionamiento actual en nuestra consola.

## Fuentes

- [GreenVita, autorización](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/auth.rs).
- [Stratix, MicrosoftAuthService](https://github.com/nafields/stratix/blob/59d804185192f0c0f618836aace9229b77ce48e4/Packages/XCloudAPI/Sources/XCloudAPI/Auth/MicrosoftAuthService.swift).
- [Microsoft MSAL, caso #3491](https://github.com/AzureAD/microsoft-authentication-library-for-js/issues/3491).
- [Microsoft, errores de identidad](https://learn.microsoft.com/en-us/entra/identity-platform/reference-error-codes).
- [OAuth 2.0, RFC 6749, sección 5.2](https://www.rfc-editor.org/rfc/rfc6749#section-5.2).
