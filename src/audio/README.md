# Audio

`demo_audio.c` genera tonos PCM S16 suaves a 48 kHz, alternando izquierda y derecha durante ocho segundos. Un hilo nativo alimenta AudioOut en bloques de 1024 muestras; el silencio persiste al repetir la muestra. La ejecución real en PS4 queda pendiente en la 0.2.0.

Decodificar Opus, alimentar audio desde WebRTC y sincronizarlo con video son etapas posteriores. Consulta `docs/MULTIMEDIA.md`.
