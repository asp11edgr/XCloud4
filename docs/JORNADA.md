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

## Resultado confirmado de la 0.1.2

El registro de la consola muestra `EXEC /app0/eboot.bin` y `XCloud4 0.1.2: inicio de interfaz y control` para el proceso 75, sin el error de módulos ausentes de los procesos anteriores. El propietario confirmó: "aparecen 2 opciones la de control y da de proyecto, las dos funcionan bien". Quedan confirmados el inicio y las dos vistas en PS4 12.00 con GoldHEN v2.4b18.7. No se han comprobado individualmente todos los botones, ejes y gatillos; tampoco Xbox, decodificación ni audio. Se conserva el PKG verificado y se registra esta versión como base funcional.

## Revisión con Claude Code

Claude Code Pro se utilizó para dos revisiones estáticas con Sonnet 5. Se contrastaron sus propuestas con los encabezados y ejemplos del SDK y con los archivos generados. Las dudas sobre el tipo de memoria, el pitch de vídeo y la lista de archivos del paquete no demostraron defectos. Una propuesta posterior sobre GOT/RELRO requiere confirmar el comportamiento del cargador y no se ha aplicado.

El usuario pidió utilizar Opus 5.5. La versión 2.1.220 rechazó ese modelo porque requiere 2.1.280 o posterior; Claude Code se actualizó mediante WinGet a 2.1.292. Dos revisiones posteriores registran efectivamente `claude-opus-5-5`. La primera rechazó el cambio propuesto sobre GOT/RELRO por no resolver su propia hipótesis y eliminar una sección que requiere el conversor. La segunda, con el registro real, confirmó que la dependencia ausente es la explicación respaldada por la evidencia y señaló que la aceptación del módulo agregado sigue pendiente. Ninguna revisión sustituye la ejecución real en PS4.

## Preparación de la muestra 0.2.0

Se agregó la tercera opción IMAGEN Y SONIDO con muestra H.264 sintética, Videodec2 cargado al entrar, buffers de memoria directa, conversión NV12 a RGB y tonos PCM de 48 kHz mediante AudioOut en un hilo nativo. X repite, cuadrado conserva el silencio y círculo vuelve. Son una imagen y tonos independientes, sin conexión con Xbox ni demostración de sincronización WebRTC. Las estructuras ABI se adaptaron de la propuesta GPL OpenOrbis #213 con fuente fijada y atribución; no se copió la implementación de Moonlight.

Claude Code Pro realizó una revisión estática con `claude-opus-5-5`, confirmada en `modelUsage` del resultado. Se contrastaron sus propuestas con tipos y formatos reales y se aplicaron correcciones sobre capacidad de enumeración de módulos, nombres con extensión, aceptación de buffers, fallos de mapeo y estado del audio. Los límites de esta revisión están en `MULTIMEDIA.md`. No se ejecutaron pruebas automatizadas.

El paquete final se compiló y generó en Lubuntu sin errores de compilación. El GP4 incluye la muestra, el ejecutable y ambos auxiliares OpenOrbis. Tamaño: 6619136 bytes. SHA-256: `a67ddc21d3d2e5fc2e700ec24407862f83238c6c2b5f3e633171c531671f0a5d`. Se copió a `/data/pkg/XCloud4-0.2.0.pkg` y la copia recuperada por FTP tiene la misma huella. Se avisó al propietario para instalar, abrir IMAGEN Y SONIDO e informar el resultado mientras se captura Klog. Inicio y reproducción de la 0.2.0 pendientes; la base 0.1.2 sigue conservada.

## Resultado de video 0.2.0 y corrección de audio 0.2.1

El propietario informó que el video parece ir bien. Su foto de IMAGEN Y SONIDO muestra la imagen sintética y 217 / 240 imágenes; el audio indica `0x809B0001`. Klog confirma `primera imagen H264 640x368 pitch=640`, `MUESTRA TERMINADA` y `[AudioOut] Error:sceMbusAddHandleByUserId 0x20000007` al iniciar audio. Queda confirmada la reproducción de esta muestra H.264 en PS4 12.00. No demuestra rendimiento a 720p ni reproducción desde WebRTC.

La 0.2.1 abre MAIN con el usuario SYSTEM (0xFF), siguiendo el ejemplo oficial de audio-wav de OpenOrbis v0.5.4, y espera el consumo antes de reutilizar el bloque PCM. Agrega registros de init, open, hilo y finalización. Claude Code Pro revisó el ajuste con `claude-opus-5-5`; no detectó defectos concretos de ABI, hilo o vida útil del buffer. Se contrastó el código de ALREADY_INIT con el encabezado del SDK. No se ejecutaron pruebas automatizadas.

La corrección se compiló y empaquetó en Lubuntu sin errores. Paquete `XCloud4-0.2.1.pkg`, 6619136 bytes, SHA-256 `6023aeacd4521e3e486ad3fb335bafab77631bd92613c56a5383037f341ab2e9`. Se copió a `/data/pkg` y la copia recuperada por FTP tiene la misma huella. Se avisó al propietario para instalarla mientras se captura Klog; sonido 0.2.1 pendiente. La implementación de video no cambió.

## Confirmación de sonido y corrección del cierre 0.2.2

El propietario confirmó que el sonido de la 0.2.1 funciona. Klog registra AudioOutOpen SYSTEM correcto y dos reproducciones de 384000 muestras sin error. Informó también CE-34878-0 al pulsar OPTIONS. El proceso 84 termina con SIGSYS en libkernel después de un salto desde `0x409330`; el ELF exacto de la 0.2.1 contiene `_exit@plt` en `0x9330`, y el retorno de main termina en esa función. El fallo está ubicado en la salida final.

La 0.2.2 prepara y resuelve SystemService antes del cierre de recursos, solicita LoadExec con `"exit"` y evita retornar de main. Permite reintentar desde la interfaz si falla la preparación, se rechaza la solicitud o pasan diez segundos sin que la consola retire el proceso. Claude Opus 5.5 revisó el flujo; se aplicó su observación sobre la espera limitada y la pausa entre reintentos. Se conservaron las implementaciones confirmadas de audio y video. No se ejecutaron pruebas automatizadas.

Se compiló y empaquetó en Lubuntu sin errores. `XCloud4-0.2.2.pkg`: 6619136 bytes, SHA-256 `be3c6ceceaac2fd5ccc20a169bc2f2127ef83dc616eb1f4e4001803adc2f497e`. Se copió a `/data/pkg` de PS4 y se comparó la huella del archivo recuperado por FTP: coincide. Se avisó al propietario para instalar y comprobar OPTIONS con captura Klog. Cierre real de esta versión pendiente.

## Resultado confirmado de la 0.2.2

El propietario instaló la 0.2.2 y confirmó: "si ya funciono correctamente" al preguntar si OPTIONS regresa al inicio sin CE-34878-0. Quedan confirmados video de la muestra, tonos PCM y cierre al menú de PS4. Se conserva la 0.2.2 como nueva base funcional. Cuenta Microsoft, catálogo, Opus y WebRTC siguen pendientes.

Klog registra `recursos cerrados`, `solicitar salida al menu PS4`, `Kill for LoadExec(0x5a)` y `Kill for LoadExec(0x5a) => 0` para el proceso 90. Esto confirma la solicitud de cierre de la nueva versión y coincide con el resultado visual informado por el propietario.

## Registro Microsoft y preparación 0.3.0

El propietario pidió continuar usando Claude y preparar el registro propio, con GreenVita como alternativa temporal si no se obtenía. En Microsoft Entra se cambió al directorio de su cuenta personal, se preparó XCloud4 para cuentas personales y el propietario autorizó pulsar Registrar, que acepta las directivas de Microsoft. El portal entregó el identificador público `f9ac8684-1032-4131-bb47-d2f58da9bb93`. Se guardó Permitir flujos de clientes públicos habilitado. No se creó un secreto, no se concedió consentimiento administrativo y no se usa el identificador de GreenVita.

Claude Code Pro con `claude-opus-5-5` escribió la capa HTTPS nativa y el trabajador de OAuth por dispositivo en dos encargos separados, con edición limitada a esos archivos. Codex integró CUENTA, el lector JSON acotado, el registro propio, la carga de módulos y la cancelación antes de salir. Se revisaron las entradas retenidas por solicitudes HTTP y la carrera de cancelación al finalizar el trabajador. Audio y decodificación conservan las fuentes confirmadas en 0.2.2. La conexión y autorización desde la PS4 todavía están pendientes.

En una tercera revisión de la integración, Claude no encontró defectos graves y señaló el tamaño del argumento de `sceHttpReadData`. Se confirmó la declaración de 64 bits y se ajustó a `size_t`. Se compiló y empaquetó la versión final en Lubuntu sin errores ni avisos. No se añadieron ni ejecutaron pruebas automatizadas.

`XCloud4-0.3.0.pkg`: 6619136 bytes. SHA-256: `13d826e4b2e682f4ec40aeeaec0ecf4fc9b39b4d577ff08aff86416cd7485859`. Se copió a `/data/pkg/XCloud4-0.3.0.pkg`; la huella del archivo recuperado por FTP coincide. Se inició captura Klog para que el propietario instale, abra CUENTA y compruebe conexión antes del acceso por código. Resultados de esta versión en consola pendientes.

## Fallo HTTPS de 0.3.0 y corrección 0.3.1

La fotografía del propietario muestra `INICIO HTTPS (RED/TLS)`, detalle `0x80020002` y HTTP 0. Klog confirma `Sysmodule libSceSsl -> 0x00000000` seguido de `modulo libSceSsl -> 0x80020002`: la búsqueda por nombre no localiza el módulo y la ruta de respaldo devuelve ENOENT. Todavía no se envió la solicitud a Microsoft; no demuestra un fallo de cuenta, registro ni de negociación TLS. Las listas obtenidas por FTP sitúan Ssl/Http en `/system/priv/lib`, pero esa vista no garantiza acceso desde el sandbox de la aplicación.

Claude Opus 5.5 analizó el registro y revisó la corrección concreta. La 0.3.1 conserva la búsqueda por nombre y añade la resolución de las parejas Init/Term para SSL y HTTP. Coincidencias con las mismas direcciones corresponden al mismo proveedor; direcciones distintas detienen la resolución por ambigüedad. Los sondeos también funcionan si no se obtiene el nombre del módulo. Se añadió la ruta común del sandbox usando la función declarada por OpenOrbis; no se imprime su identificador. Los respaldos existentes y la ruta privada nativa quedan como últimos intentos, con registro de cada resultado. Los medios conservan su orden de carga anterior. No se inventaron alias de bibliotecas ni se desactivó la validación TLS.

La primera compilación detectó que OpenOrbis no declara `strnlen`; se sustituyó por un recorrido acotado. La versión final compiló y se empaquetó sin errores ni avisos. `XCloud4-0.3.1.pkg`: 6619136 bytes, SHA-256 `abb2ca847e67998e8534d51bc59441d9979d28c9424d4e8821486dd1be7c3022`. Se copió a `/data/pkg/XCloud4-0.3.1.pkg` y la huella del archivo recuperado por FTP coincide. No se ejecutaron pruebas automatizadas. Conexión HTTPS y acceso Microsoft reales pendientes de instalar y abrir esta versión en la consola.

## GitHub

El usuario pidió subir el avance al terminar la jornada y eligió un repositorio privado. La cuenta conectada consultada es `asapedgr`. Mantener código y documentación en Git local hasta ese momento; el SDK y las credenciales quedan fuera del repositorio. Los paquetes se guardan aparte de las fuentes.
