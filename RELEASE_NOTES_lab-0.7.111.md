# XCloud4 Lab 0.7.111 — Experimental Tester Release Notes

> **EXPERIMENTAL BUILD WARNING:** `XCloud4 Lab` is an experimental laboratory edition intended for advanced testers and protocol/decoder diagnostics. Some combinations of settings **may be unstable, freeze, or abort the stream**. For everyday play, use the **Public Beta (`XCloud4-1.3.0-beta-publico.pkg`)**.

---

## Package Information

- **File:** `XCloud4-0.7.111-lab.pkg`
- **PS4 Menu Title:** `XCloud4 Lab`
- **Title ID / Content ID:** `XCLD00001` (`IV0000-XCLD00001_00-XCLOUD4APP000000`)
- **Product Version / PS4 `APP_VER`:** `0.7.111-lab` / `01.79`
- **Size:** `19,857,408` bytes
- **SHA-256:** `a39d1c15a3844860f071fa069859759e422750579eb8715fa9dc56cc48d893d1`

### Side-by-Side Installation with the Public Beta

`XCloud4 Lab` uses its own Title ID (`XCLD00001`), menu title (`XCloud4 Lab`), and `LAB` badge icon (`assets/icon0-lab.png`). It can be installed on the PS4 **at the same time** as the Public Beta (`XCloud4`, `XCLD00003`) without overwriting it.

---

## Novedades / What's New

### Español

**Todo lo nuevo de la versión pública 1.3.0-beta:**
- Nueva interfaz completa con barra lateral (Inicio, Descubrir, Catálogo, Mis juegos, Buscar y Ajustes).
- Soporte para 6 idiomas (Español, English, Português do Brasil, Français, Italiano, Deutsch).
- Hasta 4 mandos multijugador simultáneos.
- Chat de voz con micrófono opcional en juegos compatibles.
- Reconexión automática y opción «Recargar transmisión» en el menú de partida.
- Conversión de color por GPU, sincronía vertical por defecto y calidad «1080p Máxima» para Ultimate.
- Recordar sesión de Microsoft opcional y pila TLS propia integrada.
- Actualizador integrado: comprueba y descarga paquetes firmados a `/data/pkg`.

**Funciones exclusivas de XCloud4 Lab 0.7.111:**
- **Motor de scripts del mando (Lua 5.4.9):**
  - Permite cargar scripts en Lua para reasignar botones, configurar zonas muertas y curvas de sticks (lineal, precisión mejorada, cuadrática), apuntar mediante el giroscopio del DualShock 4 y funciones de turbo para accesibilidad.
  - Scripts de serie de comodidad y accesibilidad en `assets/scripts/` (`reasignar.lua`, `sticks.lua`, `giroscopio.lua`, `turbo.lua`) y scripts personalizados del usuario en `/data/xcloud4/scripts/`.
  - El motor está abierto y cada usuario carga lo que desee bajo su responsabilidad. Sin macros competitivas ni antirretroceso de serie.
- **Panel de opciones del Lab (R3):**
  - Se abre con R3 desde los menús y reúne ajustes experimentales de imagen, presentación, recuperación de vídeo y diagnóstico.
  - Incluye «Actualizar desde /data/pkg» para probar el actualizador sin publicar nada, y una prueba de diagnóstico del instalador de la PS4.
  - Las estadísticas de red durante la partida están en el menú de la partida (dos dedos en el touchpad), igual que en la versión pública.
- **«Intentar instalar (experimental, puede cerrar la app)»:**
  - En la ventana de actualización tras descargar y verificar un paquete firmado, se ofrece una opción experimental de instalación directa a través del servicio BGFT de PS4.
  - **Aviso:** hoy, en la PS4, la inicialización del instalador del sistema (BGFT o AppInstUtil) cierra la aplicación. Se está investigando.
  - **Recomendación:** Se recomienda utilizar siempre la **instalación manual** recomendada (botón PS → Ajustes → GoldHEN → Package Installer).

### English

**All features from Public 1.3.0-beta:**
- Complete new interface with sidebar (Home, Discover, Catalog, My games, Search, Settings).
- Support for 6 languages (Español, English, Português do Brasil, Français, Italiano, Deutsch).
- Up to 4 simultaneous local controllers.
- Optional voice chat microphone in supported games.
- Automatic stream reconnection and "Reload stream" action in the in-game menu.
- GPU color conversion, VSync enabled by default, and "1080p Max" quality for Ultimate accounts.
- Optional Microsoft session persistence and built-in TLS stack.
- In-app updater: checks and downloads signed releases to `/data/pkg`.

**Features exclusive to XCloud4 Lab 0.7.111:**
- **Controller Scripting Engine (Lua 5.4.9):**
  - Execute Lua scripts to remap buttons, customize stick deadzones and response curves (linear, enhanced precision, quadratic), gyro-assisted aiming with the DualShock 4, and accessibility turbo.
  - Bundled accessibility and convenience scripts in `assets/scripts/` (`reasignar.lua`, `sticks.lua`, `giroscopio.lua`, `turbo.lua`) and user-provided custom scripts in `/data/xcloud4/scripts/`.
  - Open engine running under user responsibility. No competitive macros or anti-recoil scripts bundled.
- **Lab options panel (R3):**
  - Opens with R3 from the menus and groups experimental picture, presentation, video recovery and diagnostic settings.
  - Includes "Update from /data/pkg" to test the updater without publishing anything, and a diagnostic test of the PS4 installer.
  - In-game network statistics are in the in-game menu (two fingers on the touchpad), as in the public version.
- **"Attempt install (experimental, may close the app)":**
  - In the update dialog after downloading and verifying a signed package, an experimental direct installation option via PS4 BGFT is available.
  - **Notice:** currently, on the PS4, initializing the system installer (BGFT or AppInstUtil) closes the app. This is under investigation.
  - **Recommendation:** Always use the recommended **manual installation** method (PS button → Settings → GoldHEN → Package Installer).

---

## Requisitos / Requirements

- **PS4 con GoldHEN / PS4 running GoldHEN:** Probado en PS4 Fat, firmware 12.00, GoldHEN v2.4b18.7. / Tested on PS4 Fat, firmware 12.00, GoldHEN v2.4b18.7.
- **Game Pass / Xbox Account:** Game Pass con juego en la nube o cuenta de Microsoft con perfil Xbox para Fortnite. / Game Pass Ultimate or free Microsoft account with Xbox profile for Fortnite.
- **Red / Network:** Conexión estable de 15 Mbps o más (cable recomendado). / Stable 15 Mbps or faster connection (wired recommended).

---

## Instalación / Installation

1. Descarga `XCloud4-0.7.111-lab.pkg` (abajo, en *Assets*). / Download `XCloud4-0.7.111-lab.pkg` (below, under *Assets*).
2. Cópialo a la PS4 (USB o FTP en `/data/pkg`) e instálalo con **GoldHEN → Package Installer**. / Copy it to the PS4 (USB, or FTP to `/data/pkg`) and install it with **GoldHEN → Package Installer**.
3. Comprueba el SHA-256 con `SHA256SUMS.txt` si quieres. / Optionally verify the SHA-256 against `SHA256SUMS.txt`.

---

## Código fuente / Source code

XCloud4 es software libre (GPL-3.0). Este release incluye el código fuente completo de esta versión y el de sus dependencias.
XCloud4 is free software (GPL-3.0). This release includes the complete source code of this version and of its dependencies.

---

*XCloud4 es un proyecto independiente, sin afiliación con Microsoft, Xbox ni Sony. / XCloud4 is an independent project, not affiliated with Microsoft, Xbox or Sony.*
