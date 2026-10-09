# Muestra local 0.2.0

## Alcance

La pantalla IMAGEN Y SONIDO permite ejecutar el primer camino de reproducción nativo. No tiene conexión con Xbox. El resultado real de Videodec2 y AudioOut en PS4 12.00 queda pendiente hasta abrir el paquete en la consola.

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
