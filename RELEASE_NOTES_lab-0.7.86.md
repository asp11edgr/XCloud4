# XCloud4 Lab 0.7.86 · APP_VER 01.54

- Reorden de vídeo de 512 plazas y gracia según el RTT. NACK reutiliza solicitudes, pide rangos largos y admite un reintento dentro del plazo.
- Ajustes del Lab para comparar el reorden T43 con T43b y el informe de pérdidas Cero con Real. T43b y Real son los predeterminados.
- El informe Real usa los paquetes originales y reparados, excluye duplicados y mide jitter. La prueba en consola determina si Xbox envía FlexFEC.
