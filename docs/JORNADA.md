# Jornada del 8 de octubre de 2026

## Avance

- Entorno de Lubuntu preparado y acceso local desde Windows.
- Interfaz propia con inicio, control y proyecto.
- VideoOut con dos buffers y manejo de errores.
- Lectura del DualShock 4, detección de pulsaciones y reconexión.
- Compilación a ELF/eboot y generación del primer PKG.
- Icono propio, licencia GPL-3.0-only y avisos de dependencias.
- Fuentes y licencias de GreenVita, Better xCloud y Moonlight PS4 revisadas; adaptación pendiente.

## Pendiente inmediato

El usuario instaló el paquete 0.1.0 en su PS4 12.00 con GoldHEN v2.4b18.7. Al abrirlo, la consola mostró que los datos de la aplicación están dañados. La foto confirma el fallo de inicio; todavía no demuestra cuál es su causa. Pantalla y control siguen sin confirmarse en la consola. Xbox, audio y decodificación siguen pendientes. No se han ejecutado pruebas automatizadas.

Se obtuvo acceso al registro de la consola por el servidor Klog de GoldHEN. En el segundo intento se registró `EXEC /app0/eboot.bin`, seguido de `Loading /app0/sce_module/libSceFios2.prx fails (0x80020002)` y `PRX_SCE_MODULE_LOAD_ERROR` (`0xa0020102`). Esto identifica una dependencia ausente antes de `main`. Los registros completos se guardan fuera del repositorio.

La corrección 0.1.1 agregó Fios2 al paquete desde una carpeta externa de módulos locales. Se utiliza la variante SELF del ejemplo `hello_world` incluido en el archivo verificado de OpenOrbis v0.5.4. No se cambia su contenido ni se incorpora el módulo a Git.

El paquete 0.1.1 se compiló y generó en Lubuntu. El GP4 incluye `sce_module/libSceFios2.prx`; el módulo tiene SHA-256 `3f8236c5996cf8e9917b8d27706d9743f6603d89de3b9859eacbb9f466b19c01`. El PKG mide 6619136 bytes y tiene SHA-256 `f38e3321f1327d4c2bfeffce42b732840e3d705e1c5f5bf9f662fd867a544497`. Se transfirió a `/data/pkg/XCloud4-0.1.1.pkg` mediante FTP y se descargó esa copia para comparar su huella: coincide. Se solicita al propietario instalar y abrir la versión corregida.

También se obtuvo una copia local de Fios2 de la consola por FTP. Empieza con la cabecera ELF: el servidor entrega el módulo descifrado, no un SELF listo para empaquetar. Se conserva fuera de Git como evidencia; no se utilizó en el paquete. El empaquetador rechaza esta entrada sin convertir.

El usuario instaló y abrió la 0.1.1. El registro confirmó que Fios2 se cargó y que el siguiente bloqueo es `/app0/sce_module/libc.prx` ausente, con los mismos códigos de error de carga. La 0.1.2 agrega libc desde el mismo ejemplo del SDK y exige ambos módulos durante el empaquetado.

Se corrigió la identificación inicial de estos módulos: las fuentes de ambos están en `src/modules` de OpenOrbis v0.5.4, y su script `build-and-copy.sh` genera los módulos que acompañan a los ejemplos. Son auxiliares abiertos de OpenOrbis, no módulos propietarios de Sony. El módulo real de sistema obtenido por FTP se conserva aparte y no se utiliza. SHA-256 del auxiliar `libc.prx`: `39ad53672bb0b14895f8465eb1619478ab935f5afe5dbf69c15f02cfc0a75ce6`.

La 0.1.2 se compiló y empaquetó en Lubuntu con ambos auxiliares. Mide 6619136 bytes; SHA-256 `4c42328bed5150f2f7654a4b9b37c31c8abdcfa88447899c09966e3724141bee`. Se copió por FTP a `/data/pkg/XCloud4-0.1.2.pkg` y la copia descargada de vuelta tiene la misma huella. Se avisó al usuario para instalar y abrir esta versión mientras se captura el registro. Inicio, pantalla y control de esta versión siguen pendientes de confirmación.

## Revisión con Claude Code

Claude Code Pro se utilizó para dos revisiones estáticas con Sonnet 5. Se contrastaron sus propuestas con los encabezados y ejemplos del SDK y con los archivos generados. Las dudas sobre el tipo de memoria, el pitch de vídeo y la lista de archivos del paquete no demostraron defectos. Una propuesta posterior sobre GOT/RELRO requiere confirmar el comportamiento del cargador y no se ha aplicado.

El usuario pidió utilizar Opus 5.5. La versión 2.1.220 rechazó ese modelo porque requiere 2.1.280 o posterior; Claude Code se actualizó mediante WinGet a 2.1.292. Dos revisiones posteriores registran efectivamente `claude-opus-5-5`. La primera rechazó el cambio propuesto sobre GOT/RELRO por no resolver su propia hipótesis y eliminar una sección que requiere el conversor. La segunda, con el registro real, confirmó que la dependencia ausente es la explicación respaldada por la evidencia y señaló que la aceptación del módulo agregado sigue pendiente. Ninguna revisión sustituye la ejecución real en PS4.

## GitHub

El usuario pidió subir el avance al terminar la jornada y eligió un repositorio privado. La cuenta conectada consultada es `asapedgr`. Mantener código y documentación en Git local hasta ese momento; el SDK y las credenciales quedan fuera del repositorio. Los paquetes se guardan aparte de las fuentes.
