# Dependencias y referencias

## OpenOrbis PS4 Toolchain

Versión fijada para la preparación: v0.5.4.

- Fuente: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
- Archivo: `toolchain-llvm-18.tar.gz`
- SHA-256 publicado por GitHub: `3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526`
- Licencia del repositorio: GPL-3.0. Los componentes distribuidos dentro del SDK conservan sus propias licencias.
- El Makefile y el empaquetado adaptan parámetros de los ejemplos hello_world/input. La secuencia de VideoOut de `src/video/display.c` y la inicialización de `src/input/controller.c` se adaptaron de los ejemplos públicos del SDK. La copia de la licencia original se conserva en `docs/licenses/OpenOrbis-GPL-3.0.txt`.

El SDK se instala fuera del repositorio de XCloud4, en `~/.local/share/xcloud4/OpenOrbis/PS4Toolchain`.

### Declaraciones de Videodec2

`src/video/videodec2_abi.h` adapta las estructuras públicas de la propuesta #213 de OpenOrbis, de Backporter, bajo la licencia GPL-3.0 del repositorio. Fuente fijada: `9b9e82a2ec4e8cd3c34a086ca82339032cf69da0`, archivo `include/orbis/_types/Videodec2.h`.

- Propuesta: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/pull/213
- Fuente correspondiente: https://github.com/Backporter/OpenOrbis-PS4-Toolchain/blob/9b9e82a2ec4e8cd3c34a086ca82339032cf69da0/include/orbis/_types/Videodec2.h
- Licencia conservada: `docs/licenses/OpenOrbis-GPL-3.0.txt`.

El SDK v0.5.4 tiene declaraciones incompletas para esta API. Se conserva la estructura de salida de 48 bytes de la propuesta y se resuelven las funciones al abrir la muestra. Las implementaciones de carga, manejo de memoria, lectura de la muestra, conversión NV12 y reproducción PCM son propias de XCloud4. No se ha copiado la implementación de Moonlight PS4.

### Muestra local

`assets/sample.h264` es un patrón sintético generado con FFmpeg 8.0.1 y libx264 en Lubuntu, mediante `scripts/generar-muestra.sh`. No contiene material de un juego, música ni imágenes de terceros. FFmpeg y libx264 solo se utilizan en la PC; sus ejecutables y bibliotecas no se incluyen en el PKG. Los tonos PCM se generan en XCloud4.

## Referencias futuras

GreenVita (MPL-2.0), Better xCloud (MIT) y Moonlight PS4 (licencia del código propio pendiente de aclarar) se consideran referencias de investigación. No se ha incorporado su código. Las versiones revisadas y los archivos de interés están en `docs/REFERENCIAS.md`.

## Bibliotecas del empaquetador en la PC

El empaquetador LibOrbisPkg incluido en OpenOrbis usa .NET Core 3.0. Para ejecutarlo en Ubuntu 26.04 se extrae el paquete oficial de Ubuntu `libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb` en una carpeta privada, sin instalarlo en el sistema. SHA-256: `7cf39d70a639017d1dd7c8d36daa2258063608688e449fddf40ffdd46f992a78`. Fuente: https://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb

Estas bibliotecas se usan únicamente durante el empaquetado local. No se incluyen en el PKG ni en el repositorio de XCloud4.

El código fuente de XCloud4 se distribuye bajo GPL-3.0-only; consulta `LICENSE`. El alfabeto de píxeles, el icono y las pantallas se crearon para este proyecto.

## Módulos auxiliares de OpenOrbis

El registro real de la consola confirmó que el cargador exige `sce_module/libSceFios2.prx` y `sce_module/libc.prx` antes de entrar en `main`: la 0.1.0 falló por Fios2 ausente y la 0.1.1 por libc ausente después de agregar Fios2. El PKG 0.1.2 preparado para el propietario incluye ambos módulos del ejemplo de OpenOrbis v0.5.4, que se conservan en una carpeta externa al repositorio. No se incorpora su código fuente ni se modifica su contenido.

Los dos módulos son auxiliares abiertos del repositorio de OpenOrbis, no bibliotecas propietarias del SDK de Sony. Su fuente en la etiqueta v0.5.4 está en `src/modules/libSceFios2/libSceFios2/lib.c` y `src/modules/libc/libc/lib.c`. El script `src/modules/build-and-copy.sh` los compila y copia a los ejemplos; conservan la licencia GPL-3.0 del proyecto OpenOrbis, cuya copia se incluye en `docs/licenses/OpenOrbis-GPL-3.0.txt`.

Fuente correspondiente: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/tree/v0.5.4/src/modules

Los binarios generados se mantienen fuera del repositorio de XCloud4. El módulo de sistema obtenido por FTP de la consola no se utiliza ni se distribuye. El funcionamiento de la app requiere confirmarse en la consola.
