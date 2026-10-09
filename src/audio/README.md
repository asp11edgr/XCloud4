# Audio

`demo_audio.c` genera tonos PCM S16 suaves a 48 kHz, alternando izquierda y derecha durante ocho segundos. Un hilo nativo alimenta AudioOut en bloques de 1024 muestras; el silencio persiste al repetir la muestra. El audio 0.2.0 falló con 0x809B0001 al asociar el usuario a la salida. La 0.2.1 abre MAIN con SYSTEM (0xFF), siguiendo el ejemplo público OpenOrbis, y espera antes de reutilizar los datos. El propietario confirmó el sonido; Klog registra 384000 muestras sin error. La 0.2.2 conserva esta implementación.

Decodificar Opus, alimentar audio desde WebRTC y sincronizarlo con video son etapas posteriores. Consulta `docs/MULTIMEDIA.md`.
