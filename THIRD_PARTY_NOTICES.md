# Dependencias y referencias

## OpenOrbis PS4 Toolchain

Versión fijada para la preparación: v0.5.4.

- Fuente: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
- Archivo: `toolchain-llvm-18.tar.gz`
- SHA-256 publicado por GitHub: `3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526`
- Licencia del repositorio: GPL-3.0. Los componentes distribuidos dentro del SDK conservan sus propias licencias.
- El Makefile y el empaquetado adaptan parámetros de los ejemplos hello_world/input. La secuencia de VideoOut de `src/video/display.c` y la inicialización de `src/input/controller.c` se adaptaron de los ejemplos públicos del SDK. La copia de la licencia original se conserva en `docs/licenses/OpenOrbis-GPL-3.0.txt`.

El SDK se instala fuera del repositorio de XCloud4, en `~/.local/share/xcloud4/OpenOrbis/PS4Toolchain`.

## Referencias futuras

GreenVita (MPL-2.0), Better xCloud (MIT) y Moonlight PS4 (licencia del código propio pendiente de aclarar) se consideran referencias de investigación. No se ha incorporado su código. Las versiones revisadas y los archivos de interés están en `docs/REFERENCIAS.md`.

## Bibliotecas del empaquetador en la PC

El empaquetador LibOrbisPkg incluido en OpenOrbis usa .NET Core 3.0. Para ejecutarlo en Ubuntu 26.04 se extrae el paquete oficial de Ubuntu `libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb` en una carpeta privada, sin instalarlo en el sistema. SHA-256: `7cf39d70a639017d1dd7c8d36daa2258063608688e449fddf40ffdd46f992a78`. Fuente: https://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_amd64.deb

Estas bibliotecas se usan únicamente durante el empaquetado local. No se incluyen en el PKG ni en el repositorio de XCloud4.

El código fuente de XCloud4 se distribuye bajo GPL-3.0-only; consulta `LICENSE`. El alfabeto de píxeles, el icono y las pantallas se crearon para este proyecto.

## Módulos del paquete local para PS4

El registro real de la consola confirmó que el cargador exige `sce_module/libSceFios2.prx` antes de entrar en `main`. El PKG 0.1.1 preparado para el propietario incluye ese módulo del ejemplo de OpenOrbis v0.5.4, que se conserva en una carpeta externa al repositorio. No se incorpora su código fuente ni se modifica su contenido.

Los módulos de Sony conservan sus derechos originales y no quedan cubiertos por la licencia GPL de XCloud4. No deben publicarse en GitHub ni incorporarse al repositorio. El código y las instrucciones de construcción se mantienen separados de los binarios necesarios para el uso local. La aceptación del módulo y el funcionamiento de la app requieren confirmarse en la consola.
