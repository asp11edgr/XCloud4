# XCloud4

Cliente nativo experimental de Xbox Cloud Gaming para PS4 Fat con firmware 12.00 y GoldHEN.

## Base confirmada — 0.1.2

Compilada y empaquetada en Lubuntu con OpenOrbis v0.5.4 y Clang/LLD 21.1.8. Incluye pantalla de inicio, vista del proyecto y lectura del DualShock 4 con botones, palancas y gatillos. La 0.1.0 se instaló en PS4 12.00 pero falló por Fios2 ausente; la 0.1.1 pasó ese bloqueo y falló por libc ausente. Ambos resultados se confirmaron en el registro de la consola.

La 0.1.2 incluye las dos dependencias. El registro confirma el inicio del programa y el propietario confirmó que aparecen las opciones CONTROL y PROYECTO y que ambas funcionan. Esta confirmación cubre el inicio y las dos vistas; no se han comprobado por separado todos los valores de botones, palancas y gatillos. Conexión con Xbox, reproducción de video y audio siguen pendientes.

El entorno de trabajo elegido es la máquina virtual de Lubuntu existente en VirtualBox. Se mantiene su red NAT para descargar herramientas y preparar compilaciones.

## Nueva etapa — 0.2.0

Agrega IMAGEN Y SONIDO al menú: una muestra local H.264 de ocho segundos, 640 × 368, con decodificación mediante Videodec2 y presentación NV12 convertida a RGB. Reproduce tonos suaves alternados entre izquierda y derecha, mediante AudioOut a 48 kHz. X repite la muestra, cuadrado silencia y círculo vuelve al inicio. La carga de estas bibliotecas se realiza al entrar en la muestra, para conservar el inicio de la interfaz sin depender del reproductor.

La 0.2.0 está compilada y empaquetada en Lubuntu. El propietario informó que el video va bien y su foto muestra 217 / 240 imágenes; Klog registra la primera imagen 640 × 368, pitch 640, y el final de la muestra. El audio falló con `0x809B0001` y Klog informa `sceMbusAddHandleByUserId`. No incluye cuenta Microsoft, catálogo, WebRTC ni juegos de Xbox. La 0.1.2 se conserva con la etiqueta `v0.1.2` y su paquete separado.

### Corrección 0.2.1

La salida de audio MAIN usa el usuario SYSTEM (0xFF), siguiendo el ejemplo público de OpenOrbis, y espera el consumo del bloque PCM antes de sobrescribirlo. Agrega registros de inicialización, apertura, hilo y finalización. El propietario confirmó que el sonido funciona en la 0.2.1; Klog muestra 384000 muestras enviadas sin error. El video conserva la implementación de la 0.2.0. El cierre con OPTIONS mostró CE-34878-0 y SIGSYS al entrar en `_exit`.

### Corrección de cierre 0.2.2

OPTIONS prepara la solicitud de salida mediante SystemService antes de cerrar recursos. Después solicita `sceSystemServiceLoadExec("exit", NULL)` y espera que la consola retire el proceso, evitando regresar a la ruta `_exit` que falló. Una solicitud rechazada permite reintentar desde la interfaz. Audio y video conservan sus implementaciones confirmadas. El propietario confirmó que la 0.2.2 regresa al inicio sin el error: esta es la nueva base funcional.

## Acceso Microsoft — 0.3.0

Se prepara la pantalla CUENTA con conexión HTTPS y acceso por código. Usa el registro **XCloud4** creado por el propietario, para cuentas personales y con flujos de cliente público habilitados. X solicita un código, cuadrado comprueba la conexión, triángulo elimina la sesión local y círculo cancela/vuelve. Los tokens quedan en memoria durante esta ejecución y se borran al cerrar. La muestra y salida nativa conservan la base confirmada 0.2.2.

La 0.3.0 falló al localizar SSL. La corrección **0.3.1** ya está confirmada por fotografía del propietario y Klog: conexión HTTPS con HTTP 200 y cuenta Microsoft autorizada mediante el registro propio de XCloud4, sin error. La sesión permanece en memoria hasta cerrar la aplicación. Credenciales de Xbox, catálogo y sesión de juego aún no están implementados. Consulta `docs/REGISTRO_MICROSOFT.md` y `docs/AUTENTICACION.md`.

## Catálogo Xbox — 0.4.0

Desde CUENTA, después de autorizar Microsoft, R1 abre el catálogo. Se añade el intercambio de credenciales Xbox y la consulta de títulos en la región predeterminada que devuelva el servicio, con una lista local de hasta 128 entradas y nombres de Microsoft Store para las primeras 32 cuando se obtengan. La cruceta recorre los títulos; L1/R1 cambian ocho posiciones; cuadrado actualiza y círculo vuelve a la cuenta. Los permisos se muestran solo cuando los indica Xbox. La 0.4.0 está confirmada por fotografía y Klog: recibió 2733 títulos, guardó 128 por el límite local y obtuvo 32 nombres Store, con HTTP 200 y sin errores. Se conserva con la etiqueta v0.4.0. No inicia juegos ni incorpora WebRTC. Consulta `docs/CATALOGO_XBOX.md`.

## Etapas

1. Preparar OpenOrbis y las herramientas de compilación; generar una aplicación mínima propia.
2. Comprobar en la PS4 la salida de video, el audio y DualShock 4.
3. Implementar autenticación de Microsoft y catálogo.
4. Implementar sesión de xCloud, transporte WebRTC, video/audio e instrucciones del control.
5. Trabajar en reconexión, errores y rendimiento.

Objetivo propuesto para la primera beta: 720p a 30 FPS. Su viabilidad depende de las pruebas reales de decodificación y transporte en la consola.

## Compilación

En Lubuntu, después de preparar el entorno:

```bash
cd "$HOME/Projects/XCloud4"
source "$HOME/.config/xcloud4/env.sh"
make -j2
export X4_RUNTIME_MODULES="$HOME/.local/share/xcloud4/runtime/sdk-v0.5.4"
make package
```

Se generan `build/xcloud4.elf`, `build/eboot.bin` y `dist/XCloud4-0.3.1.pkg`. `X4_RUNTIME_MODULES` debe apuntar a una carpeta externa con `libSceFios2.prx` y `libc.prx` en formato SELF. Son módulos auxiliares abiertos de OpenOrbis, disponibles en su distribución y con fuente en `src/modules`. Los binarios se conservan fuera de Git; el empaquetador se detiene si falta alguno o si recibe un ELF sin convertir. Consulta `THIRD_PARTY_NOTICES.md`.

El empaquetador antiguo de OpenOrbis necesita bibliotecas de OpenSSL 1.1 aisladas; prepáralas una vez con `bash scripts/preparar-empaquetador.sh`. Ese paso no las instala en el sistema. Consulta [instalación en PS4](docs/INSTALACION_PS4.md).

## Organización

- `src/core`: entrada y ciclo de vida.
- `src/auth`: HTTPS y acceso Microsoft por código, confirmados en consola con 0.3.1.
- `src/streaming`: sesión y transporte pendientes.
- `src/video`: VideoOut y muestra H.264 con Videodec2.
- `src/input`: lectura y reconexión del DualShock 4.
- `src/audio`: muestra PCM con AudioOut; Opus pendiente.
- `src/ui`: inicio, control, proyecto, imagen y sonido, y cuenta.
- `docs`: decisiones, arquitectura, preparación y referencias.
- `scripts`: herramientas de preparación y trabajo local.

No guardes contraseñas, tokens ni credenciales de Microsoft en este repositorio.

## Referencias

- https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
- https://github.com/Day-OS/green-vita
- https://github.com/redphx/better-xcloud
- https://github.com/JaimeJimenezG/Moonlight-ps4

El mapa de adaptación y las versiones revisadas están en [REFERENCIAS.md](docs/REFERENCIAS.md). XCloud4 usa GPL-3.0-only; consulta `LICENSE` y `THIRD_PARTY_NOTICES.md`.
