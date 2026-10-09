# Instalar XCloud4

## Cliente temporal — 0.6.2

Cerrar XCloud4, instalar `XCloud4-0.6.2.pkg` desde Package Installer con HDD o ALL y aceptar reemplazarla. Está copiada y verificada en `/data/pkg`. CUENTA informa que usa temporalmente el identificador de referencia que utiliza GreenVita; autorizar en Microsoft desde cero. El consentimiento puede mostrar otro nombre de aplicación. Abrir catálogo con R1 y elegir un título CON ACCESO con X. Conservar el resultado de Passport y `/connect` para comparar con el rechazo `invalid_scope` del registro propio; aceptación pendiente. Todavía no se transmite el juego.

## Diagnóstico Passport — 0.6.1

Cerrar XCloud4 e instalar `XCloud4-0.6.1.pkg` desde Package Installer con HDD o ALL; aceptar reemplazarla. Autorizar Microsoft nuevamente, abrir catálogo con R1 y elegir un título CON ACCESO con X. Conservar una foto del mensaje `Passport:` y su código si el rechazo se repite. Esta versión identifica el motivo del HTTP 400 observado en 0.6.0; todavía no se afirma corregido. El paquete está copiado y verificado en `/data/pkg`.

## Autorización de conexión — 0.6.0

Cerrar XCloud4, instalar `XCloud4-0.6.0.pkg` y aceptar reemplazar la aplicación. Está copiada y verificada en `/data/pkg`; elegir HDD o ALL en Package Source. Autorizar Microsoft nuevamente en CUENTA, abrir el catálogo con R1, elegir un título CON ACCESO y pulsar X. La autorización Passport y `/connect` se intentan automáticamente después de la preparación. Si Xbox acepta, aparece `XBOX ACEPTO LA CONEXION`; todavía no se transmite el juego. Esperar el cierre automático de 45 segundos y conservar una fotografía del mensaje o detalle HTTP si falla. Confirmación real pendiente. Consulta [AUTORIZACION_CONEXION.md](AUTORIZACION_CONEXION.md).

## Preparación de sesión — 0.5.0

Cerrar XCloud4 antes de instalar `XCloud4-0.5.0.pkg`, aceptar reemplazar la aplicación y autorizar Microsoft nuevamente en CUENTA. Abrir el catálogo con R1, seleccionar un título marcado CON ACCESO y pulsar X. Esta versión prepara una sesión remota; todavía no transmite el juego.

Si Xbox prepara la sesión, anotar el mensaje mostrado. Círculo debe esperar el cierre remoto y volver al catálogo. Si se deja abierta, una sesión lista se cierra después de 45 segundos. Si aparece un error, conservar su detalle numérico y estado HTTP para el diagnóstico. OPTIONS solicita cierre de la sesión antes de salir de la aplicación. Consulta [SESION_XBOX.md](SESION_XBOX.md).

El propietario y Klog confirmaron preparación de AMONGUS y cierre automático de la sesión sin error. La cancelación con Círculo y el cierre de una sesión activa mediante OPTIONS no se comprobaron por separado. Todavía no se transmite un juego.

## Corrección de cierre 0.2.2

Instalar `XCloud4-0.2.2.pkg` y aceptar reemplazar XCloud4. Abrir IMAGEN Y SONIDO y pulsar OPTIONS para salir. El propietario confirmó video, sonido y retorno correcto al inicio sin CE-34878-0; Klog confirma el cierre mediante LoadExec. Esta versión queda como base funcional.

## Corrección de sonido 0.2.1

Seleccionar `XCloud4-0.2.1.pkg` en Package Installer (HDD o ALL si se copió por red) y aceptar reemplazar XCloud4. Abrir IMAGEN Y SONIDO; debe conservar el video y ahora intentar abrir el audio MAIN con el usuario SYSTEM. X repite, cuadrado silencia y círculo vuelve. Sonido pendiente de confirmación. La 0.2.0 ya reprodujo video en la consola, pero su audio mostró `0x809B0001`.

## Nueva muestra 0.2.0

Paquete: `XCloud4-0.2.0.pkg`, mismo identificador `XCLD00001`. Compilado y empaquetado en Lubuntu; reproducción en PS4 pendiente. Conserva CONTROL y PROYECTO y agrega IMAGEN Y SONIDO. Incluye un clip H.264 sintético de ocho segundos y tonos PCM suaves alternados entre izquierda y derecha. No inicia juegos ni sesión de Xbox.

Por red: cerrar XCloud4, abrir GoldHEN → Debug Settings → Package Source: HDD o ALL → Package Installer → `XCloud4-0.2.0.pkg`. Aceptar reemplazar XCloud4 cuando lo pida. Para USB, copiar este mismo paquete a la raíz de una unidad que la PS4 ya reconozca y elegirlo en Package Installer.

Después de instalar, abrir IMAGEN Y SONIDO con la cruceta y X. Debe aparecer una imagen de colores en movimiento y tonos alternados. X repite; cuadrado silencia o activa el sonido; círculo vuelve; OPTIONS sale. La pantalla informa el estado y códigos de error para diagnosticar el resultado real. La imagen y los tonos son demostraciones independientes; la sincronización de una transmisión WebRTC queda pendiente.

## Base funcional conservada

Paquete: `XCloud4-0.1.2.pkg`. Identificador: `XCLD00001`. Esta es una aplicación inicial con pantalla y control; aún no inicia sesiones ni juegos de Xbox. Incluye Fios2 y libc, módulos auxiliares de OpenOrbis ausentes en los paquetes anteriores. El propietario confirmó el inicio y funcionamiento de las opciones CONTROL y PROYECTO en su PS4 12.00 con GoldHEN v2.4b18.7.

## Por USB

1. Enciende tu PS4 y activa GoldHEN con tu procedimiento habitual.
2. Conecta una USB ya preparada en FAT32 o exFAT a la PC. No es necesario formatearla si la consola ya la reconoce.
3. Copia `XCloud4-0.1.2.pkg` a la raíz de la USB y expúlsala desde Windows.
4. Conéctala a la PS4. En GoldHEN, abre el instalador de paquetes y selecciona XCloud4. Los nombres y ubicación exacta del menú pueden variar según la versión de GoldHEN.
5. Si solicita reemplazar la versión anterior de XCloud4, comprueba el nombre antes de aceptar. Abre XCloud4 desde el menú de la consola.

## Por FTP al disco interno

GoldHEN admite paquetes en `/data/pkg` (fuente: https://github.com/GoldHEN/GoldHEN). La copia local de la 0.1.2 se transfirió allí y su SHA-256 coincide con el archivo de la PC.

1. Activa el servidor FTP de GoldHEN.
2. Copia el PKG a `/data/pkg` mediante FTP.
3. En GoldHEN → Debug Settings, elige Package Source: HDD o ALL.
4. En Package Installer, selecciona `XCloud4-0.1.2.pkg` y reemplaza la versión anterior de XCloud4 si lo solicita.
5. Abre la aplicación y conserva el resultado real. Los registros permiten distinguir dependencias ausentes de errores de pantalla o control.

## Controles

- En inicio: cruceta para elegir CONTROL o PROYECTO; X para abrir.
- En CONTROL: los botones cambian de color, las palancas muestran posición y los gatillos muestran su valor. Mantén L1 y pulsa círculo para volver.
- En PROYECTO: círculo para volver.
- OPTIONS cierra la aplicación.

Si el paquete no instala, aparece pantalla negra o la aplicación se cierra, conserva el texto o una foto del error para continuar desde el resultado real. La confirmación en PS4 12.00 corresponde al inicio y a las dos vistas de esta versión; Xbox y reproducción de audio/video siguen pendientes.
