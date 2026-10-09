# Transporte WebRTC en PS4

## Base de aplicaciones existentes

GreenVita es la referencia principal del protocolo de Xbox: creación de la sesión, consulta de estado, autorización de conexión, SDP, ICE, mantenimiento y cierre. Better xCloud aporta ajustes y regiones. Moonlight PS4 aporta investigación de las APIs de consola; su transporte Sunshine no conecta por sí mismo con Xbox.

XCloud4 ya implementa en C los intercambios descritos por GreenVita para Microsoft, RPS, XSTS, credenciales cloud y catálogo, confirmados en la PS4 con 0.4.0. Reutilizar el protocolo permite conservar ese trabajo y adaptar la parte de consola a OpenOrbis. No se ha incorporado código Rust de GreenVita ni la implementación de Moonlight PS4.

## Bibliotecas revisadas el 8 de octubre de 2026

| Biblioteca | Fuente fijada | Componentes observados | Decisión actual |
|---|---|---|---|
| libdatachannel | `bdc5ff28e9d3b863144c94a677ecf5bf043aaf15` | C++17, API C, ICE con libjuice, DTLS, SRTP, SCTP; opción de Mbed TLS | Primera candidata para adaptar; no está compilada ni integrada en PS4 |
| libpeer | `5b849de378545c31d34759a145413846953e1366` | C, sockets BSD, Mbed TLS, libsrtp, usrsctp y cJSON | Alternativa pendiente de comparar recepción de medios y canales con los requisitos de Xbox |

Fuentes primarias: [libdatachannel](https://github.com/paullouisageneau/libdatachannel/tree/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15), [libpeer](https://github.com/sepfy/libpeer/tree/5b849de378545c31d34759a145413846953e1366). Licencias observadas en esas versiones: MPL-2.0 y MIT, respectivamente. Las dependencias conservan licencias propias y deberán quedar fijadas antes de incorporarlas.

## Evidencia del entorno local

En el SDK OpenOrbis v0.5.4 instalado en Lubuntu existen `include/c++/v1`, `libc++.a`, `libc++abi.a`, los encabezados `pthread.h`, `poll.h` y `sys/socket.h`, y la biblioteca de importaciones `libScePosix.so`. La inspección de símbolos muestra `pthread_create`, `pthread_join`, `socket`, `sendto`, `recvfrom`, `select` y `clock_gettime` en Posix; `poll` también aparece en libkernel.

La presencia de encabezados y símbolos permite estudiar una adaptación. No confirma el funcionamiento de esas llamadas, estructuras o bibliotecas WebRTC en firmware 12.00. No se compiló ni ejecutó una prueba de transporte.

## Trabajo necesario

1. Fijar dependencias y configurar compilación estática para OpenOrbis, con ejemplos y pruebas desactivados.
2. Revisar ABI de sockets, hilos, tiempo, resolución DNS y entropía criptográfica. Resolver funciones que el SDK no declare o exporte, conservando límites y cancelación.
3. Generar una oferta SDP y huella DTLS reales en la PS4; intercambiarlas con Xbox siguiendo el contrato de GreenVita. No fabricar una oferta para aparentar conexión.
4. Recibir SRTP, reconstruir imágenes H.264 y adaptarlas al decodificador nativo; decodificar Opus y alimentar AudioOut.
5. Implementar los canales de control y entrada de Xbox y conectar los datos del DualShock 4.

La primera entrega de sesión puede confirmar preparación y cierre remotos sin recibir medios. Ese resultado no significa que WebRTC o el juego completo funcionen.

## Referencias del protocolo

- [Creación de sesión y ajustes](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/api.rs).
- [Estado, conexión, SDP, ICE y cierre](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/stream.rs).
- [Sesión RTC de GreenVita](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api/streaming/rtc/session.rs).
