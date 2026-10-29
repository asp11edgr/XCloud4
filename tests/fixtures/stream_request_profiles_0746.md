# Runtime request profiles and resolution aliases

These are requests, not proof of the resolution or cadence delivered by Xbox.
The single streaming package uses `X4_STREAM_PROFILE=1`. Its process-local
startup default is **720p-HQ / automatic color**, also under compatibility compile
flag 2. Compile flag 0 retains its legacy/probe path. The common `/play` timezone
is deliberately corrected to `timezoneOffsetMinutes:-360` in every profile,
including legacy. This global correction is distinct from the resolution trial.

## Public request contracts

Only exact authorized regional `POST /v5/sessions/cloud/play` can select an
experimental device header. G, HQ and the appended 720p-HQ use the same web Windows/4096x2160 object
as profile D in 0.7.42. H uses the native device object, changing only
`dev.os.name` from Android to Windows. Its header has no `displayInfo`.
All four alias-profile play bodies use `settings.osName:"windows"`.

Titles, MRU, provisioning, connect, SDP, ICE, keepalive and DELETE retain the
fixed native 720p/Android header byte-for-byte. The native protocol
`clientAppVersion:"0.7.33"` is independent from app/package version. The rest of
the play body is unchanged except the explicit timezone correction above.
`TEST` is a validated synthetic title ID; no credentials/account data appear here.

The public web object is pinned to Better xCloud
[`generateMsDeviceInfo`, network.ts:316-337](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/network.ts#L316-L337).
Its `window.location.host` becomes the public literal `www.xbox.com`.
Display dimensions are a client advertisement, not decoder geometry or proof of
what the server delivers. The pinned
[`handlePlay`, xcloud-interceptor.ts:156-161](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/xcloud-interceptor.ts#L156-L161)
and [dispatcher, lines 268-269](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/xcloud-interceptor.ts#L268-L269)
limit the header/body change to `/sessions/cloud/play`. XCloud4 implements its
own bounded constructors; no TypeScript interception implementation is copied.
The pinned upstream [MIT license](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/LICENSE)
is preserved in `docs/licenses/Better-xCloud-MIT-LICENSE.txt`.
The commit's author/committer date is July 14, 2026; the separately recorded
repository `pushed_at` on October 4, 2026 is not that commit's date.

### 720p60

`X-MS-Device-Info`:

```json
{"appInfo":{"env":{"clientAppId":"XCloud4","clientAppType":"native","clientAppVersion":"0.7.33","httpEnvironment":"prod"}},"dev":{"hw":{"make":"Sony","model":"PS4"},"os":{"name":"android","platform":"console"}}}
```

`/play`:

```json
{"clientSessionId":"","titleId":"TEST","systemUpdateGroup":"","settings":{"nanoVersion":"V3;WebrtcTransport.dll","enableOptionalDataCollection":false,"enableTextToSpeech":false,"highContrast":0,"locale":"es-MX","useIceConnection":false,"timezoneOffsetMinutes":-360,"sdkType":"web","osName":"android"},"serverId":"","fallbackRegionNames":[]}
```

### 1080p-G

`X-MS-Device-Info`:

```json
{"appInfo":{"env":{"clientAppId":"www.xbox.com","clientAppType":"browser","clientAppVersion":"26.1.97","clientSdkVersion":"10.3.7","httpEnvironment":"prod","sdkInstallId":""}},"dev":{"os":{"name":"windows","ver":"22631.2715","platform":"desktop"},"hw":{"make":"Microsoft","model":"unknown","sdktype":"web"},"browser":{"browserName":"chrome","browserVersion":"140.0.3485.54"},"displayInfo":{"dimensions":{"widthInPixels":4096,"heightInPixels":2160},"pixelDensity":{"dpiX":1,"dpiY":1}}}}
```

`/play`:

```json
{"clientSessionId":"","titleId":"TEST","systemUpdateGroup":"","settings":{"nanoVersion":"V3;WebrtcTransport.dll","enableOptionalDataCollection":false,"enableTextToSpeech":false,"highContrast":0,"locale":"es-MX","useIceConnection":false,"timezoneOffsetMinutes":-360,"sdkType":"web","osName":"windows"},"serverId":"","fallbackRegionNames":[]}
```

### 1080p-H

`X-MS-Device-Info`:

```json
{"appInfo":{"env":{"clientAppId":"XCloud4","clientAppType":"native","clientAppVersion":"0.7.33","httpEnvironment":"prod"}},"dev":{"hw":{"make":"Sony","model":"PS4"},"os":{"name":"windows","platform":"console"}}}
```

`/play`:

```json
{"clientSessionId":"","titleId":"TEST","systemUpdateGroup":"","settings":{"nanoVersion":"V3;WebrtcTransport.dll","enableOptionalDataCollection":false,"enableTextToSpeech":false,"highContrast":0,"locale":"es-MX","useIceConnection":false,"timezoneOffsetMinutes":-360,"sdkType":"web","osName":"windows"},"serverId":"","fallbackRegionNames":[]}
```

### 1080p-HQ

`X-MS-Device-Info`:

```json
{"appInfo":{"env":{"clientAppId":"www.xbox.com","clientAppType":"browser","clientAppVersion":"26.1.97","clientSdkVersion":"10.3.7","httpEnvironment":"prod","sdkInstallId":""}},"dev":{"os":{"name":"windows","ver":"22631.2715","platform":"desktop"},"hw":{"make":"Microsoft","model":"unknown","sdktype":"web"},"browser":{"browserName":"chrome","browserVersion":"140.0.3485.54"},"displayInfo":{"dimensions":{"widthInPixels":4096,"heightInPixels":2160},"pixelDensity":{"dpiX":1,"dpiY":1}}}}
```

`/play`:

```json
{"clientSessionId":"","titleId":"TEST","systemUpdateGroup":"","settings":{"nanoVersion":"V3;WebrtcTransport.dll","enableOptionalDataCollection":false,"enableTextToSpeech":false,"highContrast":0,"locale":"es-MX","useIceConnection":false,"timezoneOffsetMinutes":-360,"sdkType":"web","osName":"windows"},"serverId":"","fallbackRegionNames":[]}
```

### 720p-HQ

`X-MS-Device-Info`:

```json
{"appInfo":{"env":{"clientAppId":"www.xbox.com","clientAppType":"browser","clientAppVersion":"26.1.97","clientSdkVersion":"10.3.7","httpEnvironment":"prod","sdkInstallId":""}},"dev":{"os":{"name":"windows","ver":"22631.2715","platform":"desktop"},"hw":{"make":"Microsoft","model":"unknown","sdktype":"web"},"browser":{"browserName":"chrome","browserVersion":"140.0.3485.54"},"displayInfo":{"dimensions":{"widthInPixels":4096,"heightInPixels":2160},"pixelDensity":{"dpiX":1,"dpiY":1}}}}
```

`/play`:

```json
{"clientSessionId":"","titleId":"TEST","systemUpdateGroup":"","settings":{"nanoVersion":"V3;WebrtcTransport.dll","enableOptionalDataCollection":false,"enableTextToSpeech":false,"highContrast":0,"locale":"es-MX","useIceConnection":false,"timezoneOffsetMinutes":-360,"sdkType":"web","osName":"windows"},"serverId":"","fallbackRegionNames":[]}
```

## Runtime signaling contract

| Profile | Requested video | Nominal diagnostic bitrate | Capabilities content | Resolution alias | Header displayInfo |
|---|---|---|---|---|---|
| 720p60 | 1280x720, max 60 FPS | 10000 kbps | Unchanged native 720p JSON | No alias message | Absent |
| 1080p-G | 1920x1080, max 60 FPS | 12000 kbps | Exactly `{}` | `1080` | 4096x2160 |
| 1080p-H | 1920x1080, max 60 FPS | 12000 kbps | Exactly `{}` | `1080` | Absent |
| 1080p-HQ | 1920x1080, max 60 FPS | 12000 kbps | Exactly `{}` | `1080HQ` | 4096x2160 |
| 720p-HQ | 1280x720, max 60 FPS | 10000 kbps | Exactly `{}` | `720HQ` | 4096x2160 |

The four alias profiles' `clientdevicecapabilities` content is an empty JSON object. Dimensions
use 1280x720 for 720p-HQ and the existing 1920x1080 JSON for G/H/HQ and safe-area fields. The 720p
capabilities and dimensions are byte-identical to 0.7.42. The retained 720p60
capabilities still transmit their existing 10000 kbps value. The alias profiles'
empty capabilities do not transmit a bitrate field or limit: their
`requested.bitrate_kbps` value is nominal diagnostic metadata, not a wire cap
or a guarantee of received bitrate, including the HQ variants. SDP/FPS and the
nominal diagnostic values are unchanged: 720p uses `42e020`, max-fs
5120/max-mbps 216000; HD uses `42e02a`,
max-fs 8704/max-mbps 522240. No new decoder/streaming capacity is advertised.
The RTC owner applies the enum-derived alias through the existing control path;
720p does not send a resolution alias.

### Bounded startup after an alias

The startup integration contract measures a ten-second transition window from
the successful send of the selected alias. During that window it discards
access units whose SPS exceeds the existing 1920x1088 coded-size limit, asks for
a keyframe intention approximately every 300 ms, and can start on a later supported SPS.
It does not enlarge decoder capacity. If the unsupported size persists to the
deadline, the existing size-error path reports the actual rejected dimensions
and allows OPTIONS to exit. The temporary wait is shown separately from that
error. These startup rules do not change recovery after decoding has begun. In 0.7.44
all intentions share the common keyframe limiter; an intention is not a send.
The 500 ms interval and moving five-second cap can defer startup retries.
The integration owner verifies the bounded transition and timeout; this
document alone does not establish successful execution.

## Snapshot ownership and UI

`x4_stream_settings_get()` atomically copies profile and color together. Main
captures one public config for media and authentication before starting a
session. The HTTP worker and RTC retain that confirmed value. The new
`x4_stream_request_resolution_alias()` derives NULL/`1080`/`1080HQ`/`720HQ` from its enum;
it adds no field to `X4StreamRequestConfig`. The x86_64 size/offsets and internal
A=G and B=H aliases remain 1/2. C is an explicit admission ceiling (MAX=4),
not an alias for HQ; HQ remains ID3 and the appended 720p-HQ is ID4. Later menu edits cannot change an
existing session's header, dimensions, SDP or alias. No setting is stored on disk.

The enum IDs remain 0/1/2/3/4; UI order is 720p60, 720p-HQ, G, H, HQ.
Atomic publication uses three profile bits and a separate color bit, so profile4
never overlaps color. No config or UI-state field is added.

The Spanish catalog shows `Calidad: <profile>` and uses R3 to open settings.
One X applies and closes, showing `Aplicado: <profile>`; CIRCLE cancels a draft.
Navigation plus X cannot commit, and the commit frame cannot also launch a game.
The existing automatic/BT.601 choice and received-signal banner are unchanged.

## Offline checks and evidence limits

- `scripts/test_stream_request_profile.sh`: exact native/web/H headers and play
  bodies, ten JSON strings against this document, aliases, unchanged 720p
  capabilities/dimensions, exact empty HD capabilities, bounds, enum snapshots,
  config ABI and atomic settings; plain and ASan/UBSan variants.
- `scripts/test_stream_os_profile.sh`: real Xbox/HTTP constructors with native
  ABI mocked, immutable captured identity, strict play-only header selection,
  and unchanged native headers on all other allowed endpoints.
- `scripts/test_stream_settings.sh`: navigation, one-X application, cancellation,
  launch-frame guards and real-renderer captures for all runtime labels.
- `tests/stream_report_reader_test.py`: new G/H/HQ/720p-HQ identity mapping while retaining
  old A/B/C and D/E/F reports. Metric and diagnostic calculations do not change.

Execution/build/console results belong in the round's external verification
report. This design document declares no executed PASS or delivered resolution.

## Captured alias vocabulary and deferred live updates

The supplied control observations contain `720`, `720HQ`, `1080`, `1080HQ`,
`1440` and `Auto`. This round implements only the selected startup aliases
`720HQ`, `1080` and `1080HQ`; native 720p60 sends no alias. `1440` and `Auto`
are not offered. Observed live-update behavior is documented for later research;
no runtime live update or new setting is implemented here.


## 0.7.45: CPU y 1080p-G8

Se conservan los cinco perfiles y todos sus alias. Se añade ID estable 5,
`1080p-G8`: la cabecera `/play`, el body `/play`, `clientdevicecapabilities`,
`dimensionschanged`, alias `1080`, FMTP y color son idénticos a 1080p-G.
Únicamente nuestra oferta SDP entregada al servidor incluye `b=AS:8000`
en cada sección `m=video`; una línea anterior se sustituye y los duplicados
de esa sección se normalizan. Audio, application y nivel de sesión no cambian.
La respuesta solo se inspecciona: presencia, cantidad y primer `b=AS` del vídeo
son datos numéricos, no una garantía ni una medición del bitrate recibido.
Se conservan también si libdatachannel rechaza la respuesta; se registra
por separado el retorno de aceptación, sin claves ni texto SDP.

El selector R3 añade CPU-0/1/2 con confirmación X y cancelación CÍRCULO.
CPU-0 es el valor inicial. La instantánea de cada sesión retiene las tres
selecciones (perfil, color, CPU) en una sola palabra atómica: cambiar el
menú no altera una sesión ya capturada y cancelar no publica el borrador.

- CPU-0: decoder nativo prioridad 700, máscara 0x3f; hilos propios sin cambios.
- CPU-1: decoder nativo prioridad 256, máscara 0x3f; hilos propios sin cambios.
- CPU-2: decoder nativo prioridad 256, máscara 0x0f; conversor 0 prioridad 700
  máscara 0x10, conversor 1 prioridad 700 máscara 0x20; ensamblado x4-video
  prioridad 500 máscara 0x20. El hilo que llama Decode conserva su prioridad heredada y en CPU-2 también se fija a la máscara 0x0f.

Las prioridades y afinidades se configuran antes de comenzar el trabajo del
hilo, con retorno por llamada. Un error se propaga; no hay alternativa silenciosa.
Los ajustes internos del decoder se pasan idénticos a Query y Create, y sus
retornos se conservan en los intentos del informe. No se alteran los hilos
de libdatachannel ni audio.

Referencia de protocolo: Better xCloud `typescript` fijado en
[f8397043f6d2148d2345d508902a38c69cf1ee20, src/utils/sdp.ts:55-114](https://github.com/redphx/better-xcloud/blob/f8397043f6d2148d2345d508902a38c69cf1ee20/src/utils/sdp.ts#L55).
Su `patchSdpBitrate` usa `b=AS` con valores en kbps. Esta implementación
es independiente y acotada, sin código copiado del proyecto.

## 0.7.46: CPU con ajustes opcionales y estado observado

La tabla de 0.7.45 anterior se conserva como antecedente. En 0.7.46 el selector
R3 ofrece cuatro variantes, con CPU-0 como valor inicial. Se mantiene la misma
captura atómica por sesión y cancelar el menú no publica ningún cambio.

| Variante | Prioridad nativa Videodec2 | Máscara nativa | Hilos propios |
| --- | ---: | --- | --- |
| CPU-0 | 700 | 0x3f | Sin ajustes |
| CPU-1 | 480 | 0x3f | Sin ajustes |
| CPU-2 | 480 | 0x0f | Caller Decode 0x0f sin Setprio; conversores 700/0x10 y 700/0x20; x4-video 500/0x20 |
| CPU-3 | 256 | 0x0f | Los mismos ajustes propios de CPU-2 |

Query y Create reciben el mismo objeto de configuración, incluida su prioridad
y afinidad. Estas cifras son solicitudes; esta documentación no afirma que
mejoren Decode ni que el firmware vaya a admitir cada ajuste.

Los setters de los hilos propios son opcionales. Antes de usarlos se consultan
la prioridad y afinidad reales mediante `scePthreadGetprio`, `scePthreadAttrInit`,
`scePthreadAttrGet`, `scePthreadAttrGetaffinity` y `scePthreadAttrDestroy`.
No se inventan valores originales de 700/0x3f para restaurarlos después.
Si falla cualquier paso de captura, no se llama a ningún setter de ese hilo.
La ejecución de vídeo continúa y el informe marca la selección CPU parcial.

Si un setter falla después de otro ajuste, se intenta restaurar cada campo
que se intentó cambiar, incluido el setter que devolvió error: un error no
prueba que su estado anterior permanezca intacto. Se conservan todos los
retornos de ajuste y restauración y se vuelve a consultar el estado real.
Solo se informa `restored` cuando esa observación coincide con los valores
capturados; si no coincide, o la consulta final falla, se informa `partial`.
No se declara una restauración por el mero hecho de haberla intentado.

Cada registro CPU conserva los campos previos de solicitudes y retornos,
añade `nonfatal`, `partial_applied`, `outcome`, snapshots `before`/`after` y
los flags/retornos de restauración. Los outcomes son `unchanged`, `applied`,
`skipped_capture`, `restored` y `partial`. `partial_applied` señala que la
selección solicitada de ese hilo no quedó completamente aplicada y observada;
incluye la omisión por captura y la vuelta comprobada al estado original.
El retorno del helper permite registrar el primer error, pero no debe entrar
en la ruta de fallo del vídeo. No se alteran audio ni libdatachannel.
