La entrega 0.3.0 prepara HTTPS nativo y acceso Microsoft por código con el registro propio de XCloud4. `http_client` carga Net/Ssl/Http en un hilo de trabajo y conserva la validación TLS; `json` valida respuestas con límites y extracción de campos del objeto raíz; `device_auth` administra la solicitud, espera, cancelación y caducidad sin bloquear el menú.

Los tokens quedan solo en memoria privada. La pantalla y Klog no reciben tokens ni el código de dispositivo. La contraseña se introduce en Microsoft desde el teléfono o PC. Todavía deben confirmarse conexión y autorización reales desde la PS4. Credenciales Xbox, catálogo y juegos siguen pendientes.

Consulta `docs/REGISTRO_MICROSOFT.md`, `docs/AUTENTICACION.md` y las referencias fijadas en `docs/REFERENCIAS.md`.
