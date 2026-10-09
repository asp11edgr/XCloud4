# Sesión Xbox y catálogo — confirmado en 0.4.0

## Punto de partida confirmado

La 0.3.1 completó HTTPS y OAuth por dispositivo con el registro propio de XCloud4 en PS4 12.00. La foto del propietario muestra la autorización y Klog confirma HTTP 200, estado AUTHORIZED y error cero. El token Microsoft permanece en memoria durante esa ejecución.

## Siguiente entrega

Desde CUENTA, después de autorizar Microsoft, R1 abre CATALOGO DE XBOX. La aplicación consulta los servicios de Xbox con la sesión actual. La cruceta recorre los resultados, L1/R1 cambian ocho posiciones, cuadrado actualiza y círculo vuelve a CUENTA. OPTIONS conserva el cierre confirmado, esperando la cancelación de una consulta pendiente.

La lista tendrá un límite local explícito de 128 entradas. Los nombres procederán del catálogo público de Microsoft Store cuando se obtengan; si no hay nombre, se mostrará el identificador real devuelto por Xbox. Un resultado de catálogo no demuestra inicio de un juego. El permiso indicado por el servicio y la disponibilidad real de streaming deben mantenerse separados.

## Intercambios previstos

1. Token Microsoft → token de usuario Xbox, con RPS.
2. Token de usuario Xbox → XSTS para `http://gssv.xboxlive.com/`.
3. XSTS → credenciales cloud de `xgpuweb`, con alternativa `xgpuwebf2p` cuando el servicio rechaza el offering principal.
4. Región predeterminada recibida del servicio → `/v2/titles`, con la credencial cloud correspondiente.
5. Identificadores Store recibidos → nombres públicos localizados a MX/es-MX.

Estas llamadas se confirmaron desde la PS4 mediante Klog y la fotografía del propietario. No se inicia una sesión de juego ni se añade WebRTC en esta entrega. Las credenciales permanecen privadas en el trabajador de red y no se escriben en registros ni archivos.

## Referencias de protocolo

Se consultan archivos de GreenVita fijados en `ae2625d295b4fba005a769b1309fd70dcd6cb63f`: `src/api_xbox/auth.rs`, `api.rs`, `game_catalog.rs` y `catalog.rs`. La implementación de XCloud4 es propia; no se copia código Rust, claves, identificadores de cliente ni datos del usuario. Las referencias y licencias están en REFERENCIAS.md.

## Estado

Claude Opus 5.5 implementó los intercambios, las solicitudes JSON y el lector de respuestas grandes. Codex integró la interfaz y revisó el código, retiró mensajes de XErr cuyo significado no estaba verificado, ajustó el total a entradas válidas recibidas (la lista mostrada se deduplica) y añadió cancelación durante el recorrido. El indicador de límite local se activa solo al descartar entradas por capacidad. La implementación completa compiló y se empaquetó en Lubuntu sin errores ni avisos. El paquete está copiado y verificado en la PS4; catálogo confirmado por fotografía y registro.

`XCloud4-0.4.0.pkg`: 6619136 bytes, SHA-256 `bd7ae0d4ee3a213295695d6f7372f99622a34a5644e70a01f256b45b30ec57b1`. La huella del archivo recuperado desde `/data/pkg/XCloud4-0.4.0.pkg` coincide con el original.

Los nombres se consultan para las primeras 32 entradas en lotes de ocho. El rechazo de un lote de nombres no borra el catálogo. Las credenciales Xbox se eliminan al finalizar la consulta; un error o cancelación del catálogo conserva el token Microsoft mientras sea vigente. No se añadieron ni ejecutaron pruebas automatizadas.

## Resultado real en la consola

Klog confirma HTTP 200 en RPS, XSTS, login cloud `xgpuweb` y lista regional. La lista regional contiene 1247270 bytes: 2733 entradas válidas recibidas, 128 guardadas y 21 de esas 128 con permiso indicado por el servicio. Los cuatro lotes Store recibieron HTTP 200 y aportaron ocho nombres cada uno (32 en total). La consulta finalizó en READY, HTTP 200, error cero y XErr cero. La fotografía de la 0.4.0 muestra el catálogo, el total de 2733 y el límite local de 128, con nombres y estados de acceso.

La descripción propia XCloud4/PS4 fue aceptada en esta consulta. No se necesitó la alternativa `xgpuwebf2p`. Este resultado confirma el catálogo y los intercambios de credenciales; no demuestra transmisión de un juego ni navegación o actualización comprobadas por separado. La siguiente etapa es crear una sesión de juego y establecer el transporte WebRTC.
