# Pantalla y video

`display.c` reserva dos buffers de 1920 × 1080 para VideoOut y espera la presentación antes de reutilizarlos. Esa salida de interfaz funcionó en PS4 con la 0.1.2.

`h264_demo.c` agrega en la 0.2.0 una muestra H.264 local de 640 × 368. Resuelve Videodec2 al abrir la muestra, consulta su memoria, conserva el clip y convierte las imágenes NV12 a RGB para la interfaz. `videodec2_abi.h` adapta tipos públicos de OpenOrbis con atribución en `THIRD_PARTY_NOTICES.md`. Reproducción real en PS4 pendiente.

No decodifica una sesión de Xbox. Recepción desde WebRTC, manejo de pérdida de cuadros, sincronización y rendimiento a 720p siguen pendientes. Consulta `docs/MULTIMEDIA.md`.
