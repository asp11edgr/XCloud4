# Preparación del entorno — 8 de octubre de 2026

## PC y máquina virtual

- Máquina existente: `Lubuntu 26.04`, en VirtualBox 7.2.18.
- Sistema observado: Ubuntu 26.04.1 LTS, x86_64; usuario `edgarg`.
- Recursos: 4096 MB de RAM, 2 procesadores, disco virtual de 16 GB.
- Espacio después de instalar: 5.7 GB libres en Lubuntu; aproximadamente 6.9 GiB libres en C:.
- Red NAT; acceso SSH desde esta PC mediante `127.0.0.1:2224`.
- Pantalla configurada a 1280 × 720, escala de VirtualBox de 85 %, con autoredimensionado desactivado. Escritorio completo comprobado después del reinicio.

## Contraseña

Se restableció la contraseña de `edgarg` desde el modo de recuperación, por solicitud del propietario. Lubuntu confirmó `passwd: password updated successfully`. La contraseña no se incluye en el proyecto ni en este informe.

## Herramientas instaladas

Se instalaron desde los repositorios configurados de Ubuntu. Versiones observadas:

| Herramienta | Versión |
|---|---|
| Clang | 21.1.8 |
| LLD | 21.1.8 |
| Make | 4.4.1 |
| CMake | 4.2.3 |
| Ninja | 1.13.2 |
| Git | 2.53.0 |
| Python | 3.14.4 |

También se instalaron build-essential, pkg-config, curl y OpenSSH Server. Clang/LLD 18 no estaban disponibles en los repositorios configurados; se usa la versión de la distribución. Su compatibilidad con la aplicación aún debe comprobarse al compilar.

## SDK

OpenOrbis v0.5.4, archivo oficial `toolchain-llvm-18.tar.gz` de 158688666 bytes. Su SHA-256 coincide con el publicado por GitHub y se comprobó de nuevo dentro de Lubuntu:

`3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526`

Se extrajeron cabeceras, bibliotecas, herramientas Linux, archivo del enlazador y licencias. Las dependencias dinámicas de `PkgTool.Core` aparecen presentes; `create-fself` es un ejecutable estático.

## Ubicaciones en Lubuntu

- Proyecto: `/home/edgarg/Projects/XCloud4`.
- SDK: `/home/edgarg/.local/share/xcloud4/OpenOrbis/PS4Toolchain`.
- Variables: `/home/edgarg/.config/xcloud4/env.sh`, cargadas desde `.bashrc`.
- Registro del SDK: `/home/edgarg/.local/share/xcloud4/sdk-*.log`.
- Registro de herramientas: `/tmp/xcloud4-herramientas.log`.

La preparación inicializó Git en `main`. El avance de implementación y compilación posterior se describe en `JORNADA.md`; la publicación en GitHub queda para el cierre de la jornada.

## Acceso desde Windows

OpenSSH está habilitado y se comprobó una conexión como `edgarg` con clave pública. El reenvío de VirtualBox escucha únicamente en `127.0.0.1:2224` de esta PC. La clave privada permanece fuera del proyecto; la clave autorizada restringe terminal interactiva y reenvíos SSH.

La transferencia inicial utilizó un servidor temporal en `127.0.0.1:8765`, accesible desde Lubuntu como `10.0.2.2:8765`. El archivo del proyecto también pasó la comprobación SHA-256. El servidor temporal se detuvo al terminar.

## Siguiente etapa

La preparación inicial no compiló la aplicación. Después se generó la versión 0.1.0 con interfaz y control, incluidos ELF, eboot y PKG. El empaquetado usa las bibliotecas privadas de OpenSSL 1.1 descritas en `THIRD_PARTY_NOTICES.md`. Todavía no existe un cliente funcional de Xbox Cloud Gaming ni un PKG comprobado en la consola.

## Fuentes

- [OpenOrbis v0.5.4](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/tag/v0.5.4).
- [Instrucciones oficiales de OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
- [Red NAT de VirtualBox](https://docs.oracle.com/en/virtualization/virtualbox/7.2/user/networkingdetails.html).
