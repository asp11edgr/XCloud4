# XCloud4 1.2.1-beta — Public Beta Release Notes

**XCloud4** is an independent, native Xbox Cloud Gaming client for PlayStation 4 consoles running GoldHEN, built with the open-source OpenOrbis PS4 Toolchain.

> **Mission:** Helping more people play next-gen games without buying a new console.

---

## What's New in 1.2.1-beta

- **País de acceso (opcional):**
  - Si Xbox rechaza el acceso a la nube por una posible restricción regional, la tarjeta de error permite elegir un **país de acceso** (Estados Unidos, Brasil, Japón, Corea o Polonia) y pulsar **X Probar acceso** con el mando, sin volver a iniciar sesión.
  - También está en **OPTIONS > Acceso** (`L1`/`R1` recorre Imagen, Servidor y Acceso), donde puedes volver a **Automático** cuando quieras.
  - Declara otro país solo al iniciar la sesión en la nube de Xbox, con la misma técnica que [Better xCloud](https://github.com/redphx/better-xcloud). No es una VPN, no da una suscripción, no cambia el país de tu cuenta ni el servidor donde juegas y no está garantizado.
  - No sirve para errores de la cuenta (sin perfil de Xbox, país de la cuenta, edad o control parental): esos conservan su mensaje propio.
  - Por defecto está en **Automático**, que funciona exactamente igual que la 1.2.0. La primera vez que eliges otro país, la app muestra el aviso de riesgo y pide confirmarlo.
  - La guía con QR sigue disponible en **Más ayuda** (Triángulo): [docs/region-es.md](https://github.com/asp11edgr/XCloud4/blob/main/docs/region-es.md) / [docs/region-en.md](https://github.com/asp11edgr/XCloud4/blob/main/docs/region-en.md).

- **Aviso de actualización al abrir la app:**
  - Cuando haya una versión nueva en GitHub, aparece una ventana con un QR a su página de descarga. Se muestra una vez por arranque, solo en el menú principal, y se cierra con `CÍRCULO`. El aviso verde y el estado en Acerca de se mantienen.
  - Lo verás a partir de la próxima versión publicada después de esta.

- **Sin cambios** en vídeo, audio, mando, vibración ni conexión con Xbox respecto a la 1.2.0.

**Aviso del País de acceso:** Usar un país de acceso distinto puede ir contra los términos de Xbox y Microsoft podría restringir tu cuenta. Úsalo bajo tu propia responsabilidad.
**Access country notice:** Using a different access country may go against Xbox's terms, and Microsoft could restrict your account. Use it at your own risk.

Todo lo de la [1.2.0-beta](https://github.com/asp11edgr/XCloud4/releases/tag/v1.2.0-beta) se mantiene: nueva interfaz, fila de Xbox, selector de servidor con ms reales, calidad de imagen, botón Fortnite, certificados propios y mensajes claros.

---

## Package Information

- **File:** `XCloud4-1.2.1-beta-publico.pkg`
- **PS4 Menu Title:** `XCloud4`
- **Title ID / Content ID:** `XCLD00003` (`IV0000-XCLD00003_00-XCLOUD4PUBLIC000`)
- **Product Version / PS4 `APP_VER`:** `1.2.1-beta` / `01.07`
- **Size:** `16,646,144` bytes
- **SHA-256:** `69456fe3e5349a5f9eca12b16e1d94a02b5f9e481ff65eff84329981295c0e33`

---

## Requirements

- **Console:** PlayStation 4 running **GoldHEN**. Probado en: PS4 Fat, firmware 12.00, GoldHEN v2.4b18.7. Si la usas en otro modelo o firmware, cuéntanos en un issue si funciona.
- **Subscription / Account:** An Xbox Game Pass plan that includes cloud gaming (PC Game Pass does not), or a free Microsoft account with an Xbox profile for Fortnite.
- **Network:** A stable internet connection of at least **15 Mbps** (wired Ethernet recommended).

---

## Installation

1. Copy `XCloud4-1.2.1-beta-publico.pkg` to your PS4 using a USB drive or via the GoldHEN FTP server to `/data/pkg/`.
2. On the PS4, open **GoldHEN > Debug Settings > Package Installer** and install the package. It updates an existing 1.2.0 install and keeps your settings.
3. Launch **XCloud4** from the PS4 home screen and sign in with your Microsoft account using the on-screen QR code or code at `microsoft.com/link`.

---

## Controls

- **OPTIONS (fuera del juego / outside of game):** Abre Ajustes (**Imagen**, **Servidor** y **Acceso**, alternando con `L1`/`R1`).
- **Dos dedos 1 s en el touchpad (en el juego / in game):** Abre el menú del juego (`Volver al catálogo`, `Silenciar/Activar sonido`, `Botón Xbox`, `Cancelar`).
- **Salir / Exit:** **Botón PS > Cerrar aplicación** (`PS button > Close Application`).

---

## Aviso / Disclaimer

Aviso: XCloud4 es un proyecto independiente, sin afiliación con Microsoft, Xbox ni Sony. Se ofrece tal cual, sin garantías. Lo usas bajo tu propia responsabilidad: el proyecto no se hace responsable de problemas con tu consola, tu cuenta o tus suscripciones. Usa XCloud4 respetando los términos de servicio de Microsoft y Xbox.

Disclaimer: XCloud4 is an independent project, not affiliated with Microsoft, Xbox or Sony. It is provided as is, without warranty. You use it at your own risk: the project is not responsible for problems with your console, account or subscriptions. Use XCloud4 in accordance with Microsoft and Xbox terms of service.
