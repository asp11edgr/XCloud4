# Scripts del mando en XCloud4 / Controller Scripts in XCloud4

[Español](#español) | [English](#english)

---

<a name="español"></a>
## Español

### 1. Introducción y Filosofía
XCloud4 incluye un motor de scripts en Lua para transformar la entrada del mando DualShock 4 antes de transmitirla a Xbox Cloud Gaming.
- **Propósito:** Comodidad, personalización y accesibilidad (reasignar botones, ajuste de curvas de stick, zonas muertas, apuntado por giroscopio y repetición turbo para accesibilidad).
- **Responsabilidad:** El motor de scripts está abierto y cada usuario carga lo que desee bajo su propia responsabilidad.
- **Políticas de serie:** Los scripts incluidos de fábrica son exclusivamente de comodidad y accesibilidad. No contienen ni anuncian funciones de ventaja competitiva (sin antirretroceso, sin macros de disparo rápido en línea).

> [!WARNING]
> Los scripts modifican lo que el mando envía a Xbox. Usar macros, disparo rápido o antirretroceso en juegos online puede causar la suspensión de tu cuenta. XCloud4 no se hace responsable del uso de los scripts: cada usuario es responsable de los que activa.

---

### 2. Dónde colocar los scripts
El sistema busca automáticamente archivos `.lua` (hasta 64 KB cada uno, máximo 32 scripts en total) en:
1. **De serie:** `/app0/assets/scripts/*.lua` (de solo lectura, incluidos en la app).
2. **Del usuario en consola:** `/data/xcloud4/scripts/*.lua` (puedes copiarlos por FTP o explorador de archivos).
3. **Memoria USB:** `/mnt/usb0/xcloud4/scripts/*.lua` (carpeta `xcloud4/scripts/` en tu pendrive USB).

La selección del script activo se realiza en el panel de ajustes **OPTIONS / R3** dentro de la partida («Script del mando») y se guarda de forma persistente en `/data/xcloud4/lab-scripts-v2.bin`.

---

### 3. API v1 de Scripts

Cada script define la función principal:
```lua
function on_input(pad, out, dt)
    -- Lógica del script
end
```

#### Parámetros:
- **`dt`** *(número)*: Segundos reales desde la llamada anterior del script, limitados a [0, 0,1]; primera llamada 0.
- **`pad`** *(tabla de entrada; no se deben borrar sus subtablas ni instalar metatablas)*:
  - **Botones** *(booleanos)*: `cross`, `circle`, `square`, `triangle`, `l1`, `r1`, `l3`, `r3`, `options`, `touchpad`, `up`, `down`, `left`, `right`.
  - **Sticks** *(números en `[-1.0, 1.0]`, Y positiva hacia arriba)*: `lx`, `ly`, `rx`, `ry`.
  - **Gatillos** *(números en `[0.0, 1.0]`)*: `l2`, `r2`.
  - **Giroscopio** *(tabla con velocidad angular en rad/s)*: `gyro.x`, `gyro.y`, `gyro.z`.
    - **Eje X (Pitch):** Inclinación arriba / abajo.
    - **Eje Y (Yaw):** Giro a la izquierda / derecha.
    - **Eje Z (Roll):** Alabeo / ladeo lateral.
  - **Acelerómetro** *(tabla con aceleración en g, 1 g ≈ 9.8 m/s²)*: `accel.x`, `accel.y`, `accel.z`.
  - **Panel táctil** *(tabla con hasta 2 dedos)*: `touch[1]` y `touch[2]`, cada uno con `{ x = [0.0, 1.0], y = [0.0, 1.0], active = bool }`.
  - **Tiempo** *(número)*: `time`, segundos de sesión transcurridos.
- **`out`** *(tabla modificable pre-rellenada con la conversión actual a Xbox)*:
  - **Botones** *(booleanos)*: `a`, `b`, `x`, `y`, `lb`, `rb`, `ls`, `rs`, `view`, `menu`, `up`, `down`, `left`, `right`.
  - **Sticks** *(números en `[-1.0, 1.0]`, Y positiva hacia arriba)*: `lx`, `ly`, `rx`, `ry`.
  - **Gatillos** *(números en `[0.0, 1.0]`)*: `lt`, `rt`.

El host satura automáticamente cualquier valor fuera de rango y convierte los datos al formato `X4GamepadFrame`.

#### Funciones de ayuda:
- **`xcloud.log(texto)`**: Imprime un mensaje en el registro del sistema (con limitación de frecuencia para evitar sobrecarga).
- **`xcloud.notify(texto)`**: Muestra un aviso flotante de ~3 segundos en la esquina superior de la pantalla.

---

### 4. Seguridad, Aislamiento y Límites
Para garantizar la estabilidad del juego y la seguridad de la consola:
1. **Entorno aislado (Sandbox):** No existen las bibliotecas `io`, `os`, `package`, `debug` ni `coroutine`. Tampoco las funciones `dofile` ni `loadfile`.
2. **Solo código fuente en texto:** No se admite bytecode precompilado; cualquier intento de carga binaria es rechazado.
3. **Memoria máxima:** 2 MiB físicos en las asignaciones de Lua, incluidas cabeceras, capacidad retenida y los dos bloques durante una copia de crecimiento. La reducción reutiliza el bloque y nunca falla; su capacidad completa sigue contabilizada hasta liberarlo. Un crecimiento sin margen conserva el bloque anterior y devuelve error; el script se desactiva de forma protegida. No se usa realloc del SDK.
4. **Límites por llamada:** hook cada 1.000 instrucciones; máximo 50.000 instrucciones y 2 ms de tiempo real. El reloj también se comprueba al retornar y después del paso de GC protegido. Una función C termina antes de la siguiente comprobación: no es una garantía de tiempo real duro. `string.match`, `string.gmatch` y `string.gsub` no existen; `string.find` siempre busca texto literal, aunque se pida modo patrón. `print` y `xcloud.log` comparten un máximo de 10 mensajes por segundo (una línea de hasta 255 bytes).
5. **Comportamiento ante fallos:** Si un script lanza un error o supera los límites, se desactiva automáticamente para el resto de la sesión. El mando regresa instantáneamente al modo directo (passthrough) sin interrumpir la partida, se muestra un aviso en pantalla y el registro detalla la línea del error.
6. **Inmunidad de controles locales:** El menú del touchpad, el botón NEXUS y la salida del juego nunca dependen del script. Mientras el menú del touchpad esté abierto, el script no se ejecuta.

---

<a name="english"></a>
## English

### 1. Overview & Philosophy
XCloud4 includes a Lua scripting engine to transform DualShock 4 controller input before sending it to Xbox Cloud Gaming.
- **Purpose:** Comfort, customization, and accessibility (button remapping, stick response curves, deadzones, gyro aiming, and turbo repeat for accessibility).
- **User Responsibility:** The scripting engine is open and users may load custom scripts under their own responsibility.
- **Stock Scripts Policy:** Stock scripts bundled with the application are strictly for comfort and accessibility. They do not include or advertise competitive advantages (no anti-recoil, no online rapid-fire macros).

> [!WARNING]
> Scripts modify what the controller sends to Xbox. Using macros, rapid fire, or anti-recoil in online games may lead to account suspension. XCloud4 is not responsible for script usage: each user is responsible for the scripts they enable.

---

### 2. Script Locations
The application automatically scans for `.lua` files (up to 64 KB each, maximum 32 scripts total) in:
1. **Stock:** `/app0/assets/scripts/*.lua` (read-only, bundled with the app).
2. **User data on console:** `/data/xcloud4/scripts/*.lua` (accessible via FTP or file manager).
3. **USB drive:** `/mnt/usb0/xcloud4/scripts/*.lua` (`xcloud4/scripts/` folder on a USB stick).

Active script selection is available in the **OPTIONS / R3** in-game settings panel ("Controller script") and is persistently saved to `/data/xcloud4/lab-scripts-v2.bin`.

---

### 3. Script API v1

Each script defines an entry-point function:
```lua
function on_input(pad, out, dt)
    -- Script logic
end
```

#### Parameters:
- **`dt`** *(number)*: Actual seconds since the previous script call, clamped to [0, 0.1]; first call 0.
- **`pad`** *(input table; deleting its nested tables or attaching metatables disables the script)*:
  - **Buttons** *(boolean)*: `cross`, `circle`, `square`, `triangle`, `l1`, `r1`, `l3`, `r3`, `options`, `touchpad`, `up`, `down`, `left`, `right`.
  - **Sticks** *(numbers in `[-1.0, 1.0]`, positive Y is UP)*: `lx`, `ly`, `rx`, `ry`.
  - **Triggers** *(numbers in `[0.0, 1.0]`)*: `l2`, `r2`.
  - **Gyroscope** *(table with angular velocity in rad/s)*: `gyro.x`, `gyro.y`, `gyro.z`.
    - **X axis (Pitch):** Tilt up / down.
    - **Y axis (Yaw):** Turn left / right.
    - **Z axis (Roll):** Bank / tilt sideways.
  - **Accelerometer** *(table with acceleration in g, 1 g ≈ 9.8 m/s²)*: `accel.x`, `accel.y`, `accel.z`.
  - **Touchpad** *(table with up to 2 fingers)*: `touch[1]` and `touch[2]`, each containing `{ x = [0.0, 1.0], y = [0.0, 1.0], active = bool }`.
  - **Time** *(number)*: `time`, session elapsed time in seconds.
- **`out`** *(mutable table pre-filled with current Xbox mapping)*:
  - **Buttons** *(boolean)*: `a`, `b`, `x`, `y`, `lb`, `rb`, `ls`, `rs`, `view`, `menu`, `up`, `down`, `left`, `right`.
  - **Sticks** *(numbers in `[-1.0, 1.0]`, positive Y is UP)*: `lx`, `ly`, `rx`, `ry`.
  - **Triggers** *(numbers in `[0.0, 1.0]`)*: `lt`, `rt`.

The host automatically clamps any out-of-range values and packs them into the `X4GamepadFrame`.

#### Helper Functions:
- **`xcloud.log(text)`**: Outputs a line to system log (rate-limited).
- **`xcloud.notify(text)`**: Displays a brief ~3-second on-screen toast notification.

---

### 4. Sandbox, Limits & Safety
1. **Isolated Sandbox:** Libraries `io`, `os`, `package`, `debug`, and `coroutine` are disabled. Functions `dofile` and `loadfile` are removed.
2. **Text-only:** Precompiled bytecode is rejected.
3. **Memory Quota:** 2 MiB physical limit for Lua allocations, including headers, retained capacity and both blocks during a growth copy. Shrinking reuses the block and never fails; its full capacity remains charged until freed. Growth without headroom preserves the old block and returns an error; the script is disabled safely. SDK realloc is not used.
4. **Per-call budgets:** a hook checks every 1,000 instructions; limits are 50,000 instructions and 2 ms wall time. Time is checked again on return and after the protected GC step. A C function finishes before the next check: this is not a hard real-time guarantee. `string.match`, `string.gmatch` and `string.gsub` are unavailable; `string.find` always performs a literal search even if pattern mode is requested. `print` and `xcloud.log` share a limit of 10 messages/second (one line, up to 255 bytes).
5. **Fault Recovery:** Errors or budget violations cleanly disable the script for the rest of the session. Controller switches to direct passthrough immediately without interrupting the stream, an on-screen toast is shown, and the error line is recorded in klog.
6. **Local Controls Immunity:** Touchpad menu, NEXUS, and exit shortcuts never depend on script logic. When touchpad menu is open, scripts are not evaluated.

### Persistencia y métricas / Persistence and metrics

ES: v2 guarda el origen (de serie, datos o USB) y el nombre de archivo, y reconstruye la ruta sin truncarla. El panel vuelve a escanear al abrirse. El diagnóstico se exporta antes de cerrar Lua a un único archivo que se sobrescribe; solo contiene nombre, estado, motivo escapado y métricas, nunca el contenido del script. Los percentiles son histogramas fijos de 1 µs por ventana de aproximadamente 5 s, sin ordenar muestras en el mando; el máximo es exacto. Si un percentil cae en el cubo de desbordamiento (≥2,048 ms), se informa el máximo exacto de la ventana como estimación conservadora, sin infravalorar pausas de planificación. Si la ventana actual está vacía, se conserva la última ventana completada.

EN: v2 stores the origin (stock, data or USB) and filename, then reconstructs the full path without truncation. The panel rescans when opened. Diagnostics are exported before closing Lua into one overwritten file; they contain only name, state, escaped error and metrics, never source code. Percentiles use fixed 1 µs histograms over approximately 5 s windows, without sorting samples in the controller path; maximum is exact. If a percentile falls in the overflow bucket (≥2.048 ms), it reports the exact window maximum as a conservative estimate, avoiding understated scheduling stalls. An empty current window retains the last completed window.
