# Instalar la versión inicial

Paquete: `XCloud4-0.1.2.pkg`. Identificador: `XCLD00001`. Esta es una aplicación inicial con pantalla y control; aún no inicia sesiones ni juegos de Xbox. Incluye Fios2 y libc, módulos auxiliares de OpenOrbis ausentes en los paquetes anteriores. Su inicio requiere confirmación en la consola.

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

Si el paquete no instala, aparece pantalla negra o la aplicación se cierra, conserva el texto o una foto del error para continuar desde el resultado real. La compatibilidad en PS4 12.00 queda pendiente hasta ejecutar esta versión en la consola.
