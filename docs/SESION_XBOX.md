# Preparación de sesión Xbox — 0.5.0

## Alcance

Desde el catálogo, X solicita a Xbox la preparación del título seleccionado. La pantalla informa la respuesta del servicio y el cierre. Esta entrega no recibe ni presenta medios del juego, no envía controles y no negocia SDP/ICE. La preparación y el cierre automático están confirmados en PS4 con 0.5.0.

## Resultado confirmado el 8 de octubre de 2026

El propietario envió una fotografía de AMONGUS con "XBOX PREPARO LA SESION", "XBOX LISTA PARA NEGOCIAR LA CONEXION" y 34 segundos restantes antes del cierre automático.

Klog identifica XCloud4 0.5.0 y confirma creación con HTTP 202, espera de recursos con HTTP 200 y transición READY con la etapa "Xbox lista para negociar la conexion". Después registra el cierre DELETE con HTTP 200 y cuerpo vacío. El resultado final es CLOSED, error cero, `ready_seen=1`, `cleanup_failed=0` y cierre HTTP 200. La solicitud propia XCloud4/PS4/Orbis fue aceptada para esta preparación.

Se conserva este hito con la etiqueta `v0.5.0`. No se ha confirmado autorización Passport de conexión, negociación WebRTC, imagen, audio ni mando dentro de un juego. Tampoco se comprobaron por separado la cancelación manual durante la creación, el cierre desde Círculo o el cierre de una sesión activa mediante OPTIONS.

Se usa el flujo descrito por [GreenVita](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/stream.rs) y una implementación C para nuestro entorno. Las credenciales se obtienen mediante el registro propio de XCloud4. La solicitud describe el dispositivo como Sony PS4 / Orbis.

## Uso previsto

1. Autorizar Microsoft en CUENTA y abrir el catálogo con R1.
2. Seleccionar un título, preferiblemente marcado CON ACCESO, y pulsar X.
3. Esperar la respuesta de Xbox. El servicio puede poner la solicitud en espera o rechazarla.
4. Si Xbox informa ReadyToConnect o Provisioned, la pantalla indica que preparó la sesión. Eso todavía no confirma una conexión WebRTC ni un juego visible.
5. Círculo pide cerrar la sesión y vuelve al catálogo cuando termina el trabajador. OPTIONS espera el cierre antes de solicitar la salida al menú de PS4.

La espera de preparación tiene un límite de tres minutos. Una sesión lista se conserva como máximo 45 segundos en esta entrega y después se cierra automáticamente. Si no se confirma el cierre, la aplicación lo muestra como error; no se presenta como una cancelación correcta.

## Propiedad y límites

- Un solo trabajador realiza acceso, catálogo o sesión; el hilo de interfaz no hace solicitudes de red.
- La cuenta Microsoft y el catálogo se conservan durante la preparación mientras el token es válido.
- Las credenciales Xbox, origen regional y ruta de sesión permanecen privados y se borran al terminar.
- La vista pública contiene estados, nombre del título, tiempos y errores numéricos. No contiene tokens, rutas ni identificadores de sesión.
- La creación no se repite automáticamente ante un error de transporte; podría haber llegado al servidor.
- El cierre usa DELETE sobre la ruta validada del servidor, con tiempo limitado aunque el usuario ya haya cancelado.
- Solo se permiten los métodos y rutas previstos bajo el dominio regional de Xbox; se conservan TLS, redirecciones desactivadas y límites del transporte existente.

La adaptación de recepción de medios y canales de mando se documenta en [WEBRTC_PS4.md](WEBRTC_PS4.md).
