# XCloud4 Lab 0.7.70 — APP_VER 01.38

- HTTPS propio con Mbed TLS 3.6.7 y TLS 1.2/1.3 para evitar la limitación de suites del TLS nativo en firmware 5.05. Las 11 raíces, las fechas y el nombre se verifican siempre; no existe modo inseguro ni respaldo al TLS nativo.
- Se conservan destinos autorizados, cancelación, límites y plazos, así como las conexiones públicas reutilizables. Los errores de certificado se distinguen de los errores recuperables de red.
- Las nuevas etapas TLS se muestran en español o inglés. El Lab registra host, versión, suite y duración del handshake sin cabeceras, cuerpos ni credenciales.
- Los workers de acceso y portadas solicitan 256 KiB de pila, como el de actualizaciones. Vídeo, audio y controles no cambian.
- La comprobación en consola, incluido firmware 5.05, corresponde al dueño tras revisar esta entrega; no se publica automáticamente.

## English

- Mbed TLS 3.6.7 provides HTTPS with TLS 1.2/1.3, addressing older firmware's native cipher limitations. All 11 roots, certificate dates and hostnames are always verified; there is no insecure mode or native-TLS fallback.
- Existing destination policy, cancellation, bounds, deadlines and public keep-alive connections remain. Certificate failures are distinct from recoverable network errors.
- New TLS stages are localized. Lab diagnostics report host, protocol, suite and handshake time without headers, bodies or credentials.
- Authentication and artwork workers request 256 KiB stacks, matching the update worker. Video, audio and controls are unchanged. Console validation remains pending owner testing.
