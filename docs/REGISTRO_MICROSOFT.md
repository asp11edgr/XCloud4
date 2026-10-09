# Registro propio de XCloud4

El propietario autorizó crear XCloud4 en su directorio personal de Microsoft Entra. El portal confirmó el registro y la configuración guardada de cliente público.

- Nombre: **XCloud4**.
- Id. público de aplicación (cliente): `f9ac8684-1032-4131-bb47-d2f58da9bb93`.
- Cuentas admitidas: solo cuentas personales de Microsoft.
- Permitir flujos de clientes públicos: habilitado y guardado.
- Compatibilidad con SDK de Live: habilitada, valor inicial del portal.
- Sin URI de redirección ni secreto de cliente para el flujo de dispositivo.

Este identificador es público y puede incluirse en el código. Las contraseñas, códigos de dispositivo y tokens nunca se incluyen en este documento ni en Git. El registro propio se utilizó hasta 0.6.1 y queda conservado.

## Perfil temporal 0.6.2

El registro propio completa acceso y catálogo, pero Passport rechaza el permiso de conexión con `invalid_scope`. Por autorización previa del propietario, 0.6.2 usa temporalmente el identificador público `1f907974-e22b-4810-a9de-d9647380c97e` que utiliza GreenVita. No es una aplicación registrada por este proyecto ni se presume que pertenezca a GreenVita. La interfaz informa del cliente temporal; Microsoft mostrará el nombre asociado al identificador en su autorización.

Cada ejecución usa un único cliente para código, intercambio, renovación y Passport. Se requiere acceso nuevo completo; los tokens privados del registro propio no se reutilizan con otro cliente. La selección se conserva en `src/auth/auth_profile.h`. No se afirma resuelto el permiso hasta comprobar el cliente de referencia en consola. Consulta [INVESTIGACION_PASSPORT.md](INVESTIGACION_PASSPORT.md).

El registro no demuestra que Microsoft haya emitido credenciales de Xbox ni que la consola pueda iniciar un juego. Esos resultados se comprobarán en sus respectivas etapas. El propietario deberá autorizar el acceso en la página de Microsoft cuando solicite un código desde la PS4.

Guía del registro: https://learn.microsoft.com/en-us/entra/identity-platform/quickstart-register-app

Flujo por código: https://learn.microsoft.com/en-us/entra/identity-platform/v2-oauth2-device-code
