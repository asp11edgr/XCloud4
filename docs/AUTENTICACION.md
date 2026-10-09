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
- El propietario registró XCloud4 y se incorporó su identificador público de aplicación OAuth. El portal confirma que permite cuentas personales y flujos de clientes públicos. Consulta `REGISTRO_MICROSOFT.md`. El identificador de GreenVita no se utiliza.
- Tokens solo en memoria durante el primer hito; evitar imprimirlos o incluirlos en Git, paquetes o registros. Persistencia y renovación se diseñarán después.

## Fuentes revisadas

- Microsoft, flujo OAuth de dispositivo: https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code
- GreenVita, MPL-2.0, `src/api_xbox/auth.rs`, versión fijada en `REFERENCIAS.md`. Es una referencia de los intercambios Xbox; todavía no se ha adaptado su implementación.

Este documento describe la siguiente implementación. No demuestra autenticación ni disponibilidad de juegos en la PS4.

## Entrega 0.3.0

Se añade CUENTA al menú. X solicita acceso por código, cuadrado comprueba HTTPS con los metadatos públicos de Microsoft y triángulo borra la sesión local. Entrar en la pantalla no solicita un código automáticamente. Círculo cancela una solicitud pendiente y vuelve al menú; OPTIONS espera su cancelación manteniendo la interfaz antes de solicitar la salida nativa confirmada en 0.2.2.

El intercambio de credenciales Xbox y el catálogo aún no forman parte de esta entrega. Una cuenta autorizada solo significa que se obtuvo un token Microsoft para los permisos solicitados. No demuestra inicio de un juego ni acceso cloud. No se ejecutaron pruebas automatizadas; los resultados en consola se documentarán por separado.

## Corrección 0.3.1

La 0.3.0 falló al localizar el módulo SSL, antes de enviar una solicitud HTTPS. La 0.3.1 amplía la localización por exportaciones y rutas nativas del sandbox. El propietario confirmó con una fotografía la cuenta Microsoft autorizada. Klog confirma comprobación de conexión con HTTP 200 y finalización del acceso en `X4_AUTH_AUTHORIZED`, HTTP 200, error cero. El token permanece en memoria durante esta ejecución. Sesión Xbox, catálogo y juegos siguen pendientes.
