# Bases del proyecto

Revisión de fuentes oficiales realizada el 8 de octubre de 2026. Los identificadores siguientes fijan lo que se revisó, aunque los proyectos sigan cambiando.

| Proyecto | Versión revisada | Uso previsto | Licencia observada |
|---|---|---|---|
| [GreenVita](https://github.com/Day-OS/green-vita) | `ae2625d295b4fba005a769b1309fd70dcd6cb63f` | Autenticación, sesión Xbox y transporte WebRTC | MPL-2.0 |
| [Better xCloud](https://github.com/redphx/better-xcloud) | `f8397043f6d2148d2345d508902a38c69cf1ee20` | Preferencias de sesión y selección de región | MIT |
| [Moonlight PS4](https://github.com/JaimeJimenezG/Moonlight-ps4) | `61427a214d4e632ee246816a98ee4f2374844a73` | Investigación de H.264, VideoOut, audio y control en PS4 | No se encontró LICENSE/COPYING en la raíz del árbol revisado; pendiente aclarar el permiso para adaptar su código propio |

## GreenVita

Su cliente está escrito en Rust para PS Vita. Se revisaron `src/api_xbox/auth.rs` y `src/api/streaming/rtc/session.rs`: contienen flujo de autenticación, tokens y abstracción de una sesión WebRTC con recepción de audio/video y envío del control. Las rutas, tipos de memoria y decodificador de Vita necesitan implementación específica de PS4.

Siguiente trabajo: documentar los intercambios del protocolo y escoger bibliotecas de HTTP/TLS/JSON y WebRTC que puedan compilarse con OpenOrbis. La implementación deberá conservar los avisos aplicables a cualquier archivo adaptado. No se copiaron credenciales ni identificadores de cliente a XCloud4.

## Better xCloud

Es un proyecto TypeScript para el cliente web. `src/utils/region.ts` consulta la región preferida y, cuando corresponde, la región por defecto devuelta por el servicio. `src/modules/stream/stream-settings-utils.ts` es una referencia para los ajustes de sesión. XCloud4 necesitará sus propios modelos e interfaz; no se incorpora un navegador a partir de ese script.

## Moonlight PS4

Se revisaron `src/video/decoder_orbis.c`, `src/audio/audio_orbis.c`, README, PLAN y documentación de consola. Su README declara validación en firmware 9.00. Eso no confirma compatibilidad con nuestro firmware 12.00.

El proyecto expone decodificación H.264 con `libSceVideodec2`, presentación y audio con `sceAudioOut`. Su transporte es Moonlight/Sunshine; Xbox Cloud Gaming requiere la sesión y transporte WebRTC correspondientes. Los parches de kernel publicados para 9.00 no se incorporan al cliente de 12.00.

## Código utilizado hasta ahora

Los tres proyectos anteriores se usan como referencias de investigación; aún no se ha incorporado su código. La versión 0.1.0 adapta las secuencias de inicialización pública de VideoOut y Pad de OpenOrbis y añade pantallas, renderizado e icono propios bajo GPL-3.0-only.
