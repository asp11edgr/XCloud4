# Siguiente etapa: cuenta Microsoft y catálogo

La base 0.2.2 ya reproduce la muestra y cierra correctamente. La siguiente entrega preparará conexión HTTPS y acceso por código desde PS4; después se integrará el catálogo de Xbox. Todavía no hay un código real, sesión ni token emitidos para XCloud4.

## Flujo previsto

1. Al elegir iniciar sesión, pedir a Microsoft un código de dispositivo. Mostrar su dirección de verificación, código del usuario y tiempo restante.
2. El propietario autoriza desde el navegador de su teléfono o PC. Su contraseña se introduce en Microsoft.
3. Consultar el estado respetando el intervalo y la caducidad devueltos. Permitir cancelar, manejar acceso pendiente, rechazo y expiración.
4. Con la autorización, obtener las credenciales de Xbox y del servicio cloud mediante los intercambios revisados en GreenVita.
5. Solicitar el catálogo, mostrar los títulos y su disponibilidad real. La creación de la sesión de juego y WebRTC será otra etapa.

## Implementación en PS4

- HTTP y TLS con las bibliotecas nativas del SDK (`Http.h`, `Ssl.h`, `Net.h`); funciones resueltas al entrar, igual que en la muestra multimedia. Validación de certificados habilitada.
- Solicitudes fuera del hilo de dibujo, con límites de tiempo y respuesta. El menú debe seguir aceptando el mando durante el acceso.
- JSON con límites y comprobación de tipos; distinguir fallos de conexión, del protocolo y del servicio.
- El identificador público de aplicación OAuth debe configurarse y comprobarse antes de pedir un código. No se ha elegido ni copiado el identificador de GreenVita.
- Tokens solo en memoria durante el primer hito; evitar imprimirlos o incluirlos en Git, paquetes o registros. Persistencia y renovación se diseñarán después.

## Fuentes revisadas

- Microsoft, flujo OAuth de dispositivo: https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code
- GreenVita, MPL-2.0, `src/api_xbox/auth.rs`, versión fijada en `REFERENCIAS.md`. Es una referencia de los intercambios Xbox; todavía no se ha adaptado su implementación.

Este documento describe la siguiente implementación. No demuestra autenticación ni disponibilidad de juegos en la PS4.
