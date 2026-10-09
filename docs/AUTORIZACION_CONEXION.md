# Autorización de conexión Xbox — 0.6.0

## Alcance

Después de preparar el título con el flujo confirmado en 0.5.0, el trabajador renueva el acceso Microsoft con el registro propio de XCloud4, obtiene un token Passport de transferencia de consola y envía una sola solicitud POST al recurso regional `/connect` de esa sesión. No negocia SDP/ICE ni recibe medios todavía. La aceptación de `/connect` no confirma WebRTC ni un juego visible.

La implementación se basa en los intercambios de [autorización de GreenVita](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/auth.rs) y [sesión de GreenVita](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/stream.rs), adaptados en C a la capa HTTPS nativa existente. No se usa el identificador de otra aplicación.

## Flujo

1. Autorizar Microsoft, abrir el catálogo y elegir un título CON ACCESO con X.
2. Esperar preparación remota y autorización automática de conexión.
3. Si Xbox acepta `/connect`, aparece `XBOX ACEPTO LA CONEXION`. La sesión permanece como máximo 45 segundos y se elimina automáticamente.
4. Círculo solicita cierre antes de volver al catálogo; OPTIONS solicita cierre antes de salir.
5. Ante un rechazo, conservar el mensaje, detalle numérico y HTTP. La aplicación intenta eliminar la sesión creada aunque falle Passport o `/connect`.

La 0.6.0 compiló y se empaquetó sin errores ni avisos. El PKG tiene 6619136 bytes y SHA-256 `3b83acf2c2e4f878b447ee70542edf247dec0822516c6d95135450c107404474`. Se copió a `/data/pkg/XCloud4-0.6.0.pkg`; la copia recuperada por FTP tiene la misma huella. La confirmación real en consola está pendiente. La base confirmada se conserva como `v0.5.0`.

## Resultado observado en la 0.6.0

La fotografía del propietario muestra 1000XRESIST, `MICROSOFT RECHAZO LA AUTORIZACION DE CONEXION`, HTTP 400 y detalle `0xfffff828` (rechazo OAuth local, no código específico de Microsoft). Klog confirma preparación lista, renovación Microsoft HTTP 200, Passport HTTP 400 con 211 bytes y `/connect` sin enviar. DELETE termina HTTP 200; el cierre quedó confirmado, sin error de limpieza.

La respuesta Passport se borró antes de clasificar su campo OAuth `error`. Por tanto no se estableció si el rechazo corresponde al permiso solicitado, al registro de aplicación o al token. No se confirmó autorización de conexión ni se etiqueta la 0.6.0 como hito funcional. La 0.6.1 prepara una clasificación segura del motivo para continuar el diagnóstico; conserva el mismo registro, endpoint y permiso hasta tener evidencia.

## Diagnóstico 0.6.1

Claude Opus 5.5 añadió clasificación de nueve códigos OAuth conocidos antes de borrar la respuesta. La interfaz muestra una frase fija y el código permitido; cualquier otro valor se convierte en `unknown`. Los registros solo incluyen ese código fijo y, si existe, el primer entero válido de `error_codes`. No se registra `error_description`, el cuerpo, tokens ni datos de la cuenta. Passport `invalid_grant` no borra la cuenta: no equivale por sí solo a que caduque la renovación Microsoft que acaba de funcionar.

Se corrigió también el color del encabezado de error para que no aparezca como éxito por haber preparado antes la sesión. No se añadieron reintentos ni se cambió el permiso solicitado. Esta entrega identifica la causa; no afirma corregir el rechazo.

Compilada y empaquetada sin errores ni avisos. `XCloud4-0.6.1.pkg`: 6619136 bytes, SHA-256 `bcae7dc054a35fe5d1fe92e8c1eb9b4a2c064f6e1a14193a23203833978bee3e`. Copiada a `/data/pkg` y verificada contra el archivo recuperado por FTP. Resultado del diagnóstico en PS4 pendiente.

## Perfil temporal 0.6.2

La 0.6.1 ya confirmó `invalid_scope` en la consola y cierre remoto HTTP 200, sin enviar `/connect`. La comparación de fuentes e hipótesis del rechazo está en [INVESTIGACION_PASSPORT.md](INVESTIGACION_PASSPORT.md).

La 0.6.2 utiliza el identificador público de referencia usado por GreenVita durante todo el acceso, con autorización previa del propietario. La interfaz y el registro anuncian el perfil temporal. El registro propio se conserva; no se mezclan tokens ni se cambia de cliente automáticamente tras un error. Endpoints, scope y diagnóstico permanecen iguales. El cliente de referencia exige iniciar sesión nuevamente y Microsoft puede mostrar un nombre de aplicación diferente del registro XCloud4.

Compilada y empaquetada sin errores ni avisos. `XCloud4-0.6.2.pkg`: 6619136 bytes, SHA-256 `25c724aca93728d53c9d4c6b7e52f0acff3dff45c263779bdbcc87e25699b601`. Copiada a `/data/pkg` y recuperada por FTP con la misma huella. El propietario y Klog confirmaron Passport HTTP 200, `/connect` HTTP 202 y cierre automático DELETE HTTP 200, sin error de limpieza. La autorización funciona con el cliente temporal; WebRTC y la recepción del juego siguen pendientes.

## Credenciales y cancelación

El acceso y la renovación de Microsoft permanecen privados y solo en memoria. La renovación acepta los candidatos únicamente después de validar toda la respuesta; conserva el token de renovación anterior si Microsoft no devuelve otro. Un `invalid_grant` exacto en esa renovación elimina la cuenta y catálogo locales. Un rechazo de Passport se informa como error de autorización.

La solicitud de renovación tiene plazo de 30 segundos y termina antes de atender una cancelación para conservar una posible rotación del token. Passport y `/connect` sí atienden cancelación. Siempre se intenta DELETE con tiempo limitado una vez conocida una ruta de sesión válida. No se repite `/connect` ni se cierra globalmente la cuenta Microsoft.

El token Passport se usa únicamente para construir `userToken`, con caracteres y longitud acotados, y se borra después del envío. Los registros incluyen etapas fijas, estados y códigos HTTP; no incluyen credenciales ni cuerpos de respuesta. `/connect` requiere 2xx y cuerpo vacío o un objeto JSON válido sin `errorDetails` no nulo. Un valor JSON de otro tipo se rechaza.

La pantalla y el registro distinguen preparación, autorización y cierre. `connection_authorized` indica aceptación de `/connect` y se conserva en el resultado final para diagnóstico; no indica recepción de video.
