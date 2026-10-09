# Muestra local 0.2.0

## Alcance

La pantalla IMAGEN Y SONIDO permite ejecutar el primer camino de reproducción nativo. No tiene conexión con Xbox. En la 0.2.0, el propietario informó que el video va bien y la foto muestra 217 / 240 imágenes. Klog registra la primera imagen H.264 de 640 × 368 con pitch 640 y `MUESTRA TERMINADA`. El audio de esa versión falló con `0x809B0001`.

- Clip sintético H.264 Annex B, baseline 3.0, 640 × 368, 30 cuadros por segundo, ocho segundos, sin B-frames y con un delimitador AUD por cuadro. Lo genera `scripts/generar-muestra.sh` con FFmpeg y libx264 en la PC.
- SHA-256 del clip incluido: `fc920497b038586e8611b65accc0cd9bb81eb0f768332e37e95462119c473a31`. Se incluye el archivo para compilar sin requerir FFmpeg. Para regenerarlo, usar `bash scripts/generar-muestra.sh`; `X4_FFMPEG` permite indicar la ruta del ejecutable local.
- El archivo completo queda en memoria directa ONION mientras existe el decoder; no se sobrescriben unidades comprimidas en vuelo. Las imágenes utilizan cuatro buffers de memoria directa GARLIC y una copia RGB para la interfaz. Profundidad de decodificación: uno.
- Se resuelven las funciones de Videodec2 y AudioOut al entrar. Sysmodule solicita sus dependencias; el cargador consulta módulos ya presentes y admite nombre con o sin extensión. No se agregan estas bibliotecas a las dependencias iniciales del ejecutable.
- La salida esperada es NV12 lineal de 640 × 368. Se comprueban dimensiones, pitch, tamaño y pertenencia del buffer antes de convertirlo. Una salida diferente se muestra como error, sin interpretar memoria ajena.
- Tonos triangulares suaves de 440 Hz, PCM S16 estéreo, 48 kHz, en bloques de 1024 muestras. Se alternan izquierda y derecha con pausas; un hilo nativo alimenta AudioOut. No es una banda sonora del video y no demuestra sincronización audiovisual de xCloud.
- X reinicia la muestra; cuadrado conserva el estado de silencio incluso al repetir; círculo vuelve. OPTIONS sale. El cierre espera al hilo de audio y elimina el decoder antes de liberar su memoria. Si falla eliminar el decoder o su cola, se conservan sus buffers hasta que termine el proceso.

## Fuentes y límites

Las estructuras de Videodec2 se adaptan de OpenOrbis PR213, fijadas en `THIRD_PARTY_NOTICES.md`. El SDK v0.5.4 aporta AudioOut, kernel, Pad y VideoOut. El código de reproducción y conversión es propio. No se copió código de Moonlight PS4 ni se aplicaron cambios al kernel.

El formato de pantalla es A8B8G8R8_SRGB, igual que en la base confirmada. Los tamaños ABI se comprueban al compilar. Una compilación correcta no demuestra que el decodificador, sus parámetros o el sonido funcionen en el firmware de la consola.

## Revisión

Claude Code Pro 2.1.292 ejecutó una revisión estática con `claude-opus-5-5` (proveedor firstParty). Se aplicaron ajustes sobre capacidad de la lista de módulos, nombres con extensión, buffers aceptados por el decoder, direcciones tras fallos de mapeo y estado del audio. La capacidad expresada en entradas se contrastó con la implementación primaria de flatz: https://github.com/flatz/ps4_remote_pkg_installer/blob/master/module.c

Se descartaron propuestas sin defecto demostrado: intercambiar rojo/azul (la pantalla usa ABGR), borrar archivos de empaquetado (el GP4 enumera explícitamente sus archivos) o quitar la primera presentación (cada flip espera su finalización). No se cambiaron profile/level a cero: esa sugerencia era una hipótesis de hardware. Los registros de la consola decidirán cualquier cambio posterior de parámetros.

No se añadieron ni ejecutaron pruebas automatizadas. La 0.1.2 y su etiqueta se conservan como base confirmada.

## Audio 0.2.1

El registro real contiene `[AudioOut] Error:sceMbusAddHandleByUserId 0x20000007` en cada inicio de la muestra 0.2.0. La pantalla y la foto del propietario muestran `0x809B0001`; la implementación pasaba a MAIN el usuario obtenido de UserService. El ejemplo público OpenOrbis v0.5.4 abre MAIN con `ORBIS_USER_SERVICE_USER_ID_SYSTEM` (0xFF). La 0.2.1 adopta esa asociación y elimina la consulta del usuario para el audio.

También sigue la espera explícita con `sceAudioOutOutput(handle, NULL)` antes de reutilizar el bloque PCM y al finalizar, y registra los resultados de init, open y creación del hilo. Fuente primaria: https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/v0.5.4/samples/audio-wav/audio-wav/main.cpp. Las credenciales de Microsoft no intervienen. Se conserva el video de la 0.2.0. La corrección de sonido queda pendiente de confirmación real.

El propietario confirmó después: "La version 2.1 ya corrigio el sonido funciona bien". El registro muestra `AudioOutOpen MAIN SYSTEM(0xff) -> 0x20000007`, creación del hilo y `PCM terminado, muestras=384000 error=0x00000000` en dos reproducciones. Esta confirmación cubre la muestra local; Opus, WebRTC y sincronización siguen pendientes.

## Cierre 0.2.2

OPTIONS causó CE-34878-0 en la 0.2.0 y la 0.2.1. Para el proceso 84 de la 0.2.1, Klog registra SIGSYS en el hilo principal: RIP `libkernel + 0x28bc`, último salto desde `0x409330` hacia `libkernel + 0x28b0`. El ELF de esa versión contiene `_exit@plt` en el desplazamiento `0x9330`, y su inicio C retorna de main hacia `exit` → `_Exit` → `_exit`. Esta evidencia ubica el fallo en la salida final del programa.

La 0.2.2 resuelve `sceSystemServiceLoadExec` al pedir salir y solicita `"exit"` después de cerrar los recursos. Espera a que el sistema retire la aplicación en vez de retornar de main. Si preparar la solicitud falla, conserva la interfaz; si la solicitud es rechazada después del cierre, la vuelve a abrir para permitir otro intento. Se agregan registros por etapa de cierre. Audio y video permanecen iguales a la 0.2.1.

La espera de retirada está limitada a diez segundos; si se agota, vuelve a abrir la interfaz con un error. Se añade una pausa antes de reabrir para evitar un ciclo rápido de fallo de presentación y rechazo de salida. Claude Opus 5.5 revisó el cambio; esas medidas responden a sus observaciones. La comparación del último salto con `_exit@plt` se hizo contra el ejecutable exacto de la 0.2.1. La revisión estática no confirma el resultado de LoadExec en la consola.

Contrato de la función: `include/orbis/SystemService.h` de OpenOrbis v0.5.4. Ejemplo primario de uso investigado, sin copiar su implementación: https://github.com/bucanero/PS4CheatsManager/blob/main/source/main.c (`terminate`, solicitud LoadExec con `"exit"`). La salida real sin CE-34878-0 queda pendiente hasta abrir la 0.2.2 en la consola. No se ejecutaron pruebas automatizadas.

Resultado posterior confirmado: el propietario informó que la 0.2.2 cierra correctamente. Klog muestra recursos cerrados, solicitud de salida y `Kill for LoadExec(0x5a) => 0`, sin un nuevo SIGSYS en ese cierre. Se conserva `v0.2.2` como base de reproducción local y salida correcta.
