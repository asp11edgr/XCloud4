# XCloud4

Cliente nativo experimental de Xbox Cloud Gaming para PS4 Fat con firmware 12.00 y GoldHEN.

## Versión inicial — 0.1.0

Compilada y empaquetada en Lubuntu con OpenOrbis v0.5.4 y Clang/LLD 21.1.8. Incluye pantalla de inicio, vista del proyecto y lectura del DualShock 4 con botones, palancas y gatillos. El paquete todavía no se ha ejecutado en la PS4; conexión con Xbox, reproducción de video y audio siguen pendientes.

El entorno de trabajo elegido es la máquina virtual de Lubuntu existente en VirtualBox. Se mantiene su red NAT para descargar herramientas y preparar compilaciones.

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
make package
```

Se generan `build/xcloud4.elf`, `build/eboot.bin` y `dist/XCloud4-0.1.0.pkg`. El empaquetador antiguo de OpenOrbis necesita bibliotecas de OpenSSL 1.1 aisladas; prepáralas una vez con `bash scripts/preparar-empaquetador.sh`. Ese paso no las instala en el sistema. Consulta [instalación en PS4](docs/INSTALACION_PS4.md).

## Organización

- `src/core`: entrada y ciclo de vida.
- `src/auth`: autenticación pendiente.
- `src/streaming`: sesión y transporte pendientes.
- `src/video`: salida de pantalla con VideoOut; decodificación pendiente.
- `src/input`: lectura y reconexión del DualShock 4.
- `src/audio`: integración pendiente.
- `src/ui`: pantalla de inicio, control y estado del proyecto.
- `docs`: decisiones, arquitectura, preparación y referencias.
- `scripts`: herramientas de preparación y trabajo local.

No guardes contraseñas, tokens ni credenciales de Microsoft en este repositorio.

## Referencias

- https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain
- https://github.com/Day-OS/green-vita
- https://github.com/redphx/better-xcloud
- https://github.com/JaimeJimenezG/Moonlight-ps4

El mapa de adaptación y las versiones revisadas están en [REFERENCIAS.md](docs/REFERENCIAS.md). XCloud4 usa GPL-3.0-only; consulta `LICENSE` y `THIRD_PARTY_NOTICES.md`.
