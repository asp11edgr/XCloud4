# XCloud4 1.3.0-beta — Public Beta Release Notes

**XCloud4** es un cliente nativo e independiente de Xbox Cloud Gaming para PS4 con GoldHEN.
**XCloud4** is an independent, native Xbox Cloud Gaming client for PS4 consoles running GoldHEN.

> **Misión / Mission:** Que más personas puedan jugar títulos de nueva generación sin comprar una consola nueva. / Helping more people play next-gen games without buying a new console.

---

## Novedades / What's New

### Español

**Interfaz nueva**
- Rediseño completo: barra lateral con Inicio, Descubrir, Catálogo, Mis juegos, Buscar y Ajustes, y nuevo icono y fondo en el menú de la PS4.
- **Inicio:** tu última partida, «Continuar jugando» y «Populares en la nube».
- **Descubrir:** carrusel de destacados y colecciones por género.
- **Catálogo:** buscador, filtros de género, acceso, favoritos y multijugador, y orden.
- **Ficha de cada juego:** capturas, detalles, juegos similares, favoritos y QR para obtenerlo o verlo en la tienda.
- **Mis juegos:** recientes y favoritos.
- **Historial en tu PS4:** última sesión y tiempo jugado por juego, también en «Tu cuenta».

**Idiomas**
- Español, English, Português (Brasil), Français, Italiano y Deutsch.
- Se elige en Ajustes → Interfaz. Automático sigue el idioma de la consola.

**Jugar**
- **Varios mandos:** hasta 4, cada uno con su usuario de PS4. Actívalo en Ajustes → Mandos.
- **Micrófono (opcional):** chat de voz en juegos compatibles con auriculares que reconozca la PS4. Está apagado por defecto; se activa en Ajustes → Micrófono y se silencia desde el menú de la partida.
- **Reconexión automática** si se corta la red, y **«Recargar transmisión»** en el menú de la partida (dos dedos en el touchpad 1 s).
- **Estadísticas de red** opcionales durante la partida.
- **Mejor imagen y estabilidad:**
  - recuperación de vídeo ante pérdidas;
  - sincronía vertical activada por defecto, sin la franja horizontal;
  - XCloud4 avisa a Xbox de tu ancho de banda;
  - corrección de errores en el audio;
  - conversión de color por GPU.
- **Calidad «1080p Máxima»** para cuentas Ultimate.

**Cuenta y conexión**
- **Recordar sesión (opcional):** no tendrás que iniciar sesión cada vez. Está apagado por defecto, con aviso, y se borra al cerrar sesión.
- **Conexión segura propia (TLS):** debería arreglar los errores de conexión en firmwares antiguos como 5.05 (sin confirmar en esa versión).
- **Mensajes claros** cuando Xbox cierra la sesión, se agota tu tiempo en la nube o un juego gratis necesita «Obtener» en xbox.com.

**Actualizaciones desde la app**
- Al salir una versión nueva, «Actualizar ahora» la descarga desde GitHub y comprueba su firma digital.
- La deja en `/data/pkg` y te indica cómo instalarla: **botón PS → Ajustes → GoldHEN → Package Installer**.
- También en Acerca de → «Buscar actualizaciones».
- *Esta función sirve a partir de la próxima versión: la 1.3.0 hay que instalarla a mano como siempre.*

### English

**New interface**
- Complete redesign: sidebar with Home, Discover, Catalog, My games, Search and Settings, plus a new icon and background in the PS4 menu.
- **Home:** your last game, "Continue playing" and "Popular in the cloud".
- **Discover:** featured carousel and genre collections.
- **Catalog:** search, genre/access/favorites/multiplayer filters and sorting.
- **Game page:** screenshots, details, similar games, favorites and a QR to get the game or open it in the Store.
- **My games:** recent games and favorites.
- **Play history on your PS4:** last session and time played per game, also shown in "Your account".

**Languages**
- Español, English, Português (Brasil), Français, Italiano and Deutsch.
- Choose one in Settings → Interface. Automatic follows the console language.

**Playing**
- **Multiple controllers:** up to 4, one PS4 user per controller. Enable it in Settings → Controllers.
- **Microphone (optional):** voice chat in supported games with a headset the PS4 recognizes. It's off by default; turn it on in Settings → Microphone and mute it from the in-game menu.
- **Automatic reconnection** when the network drops, and **"Reload stream"** in the in-game menu (two fingers on the touchpad for 1 s).
- Optional **network statistics** while playing.
- **Better picture and stability:**
  - video loss recovery;
  - vertical sync on by default, with no horizontal tearing band;
  - XCloud4 tells Xbox your available bandwidth;
  - audio error correction;
  - GPU color conversion.
- **"1080p Max" quality** for Ultimate accounts.

**Account and connection**
- **Remember sign-in (optional):** no need to sign in every time. It's off by default, shows a warning first, and is erased when you sign out.
- **Built-in secure connection (TLS):** should fix connection errors on older firmwares such as 5.05 (not yet confirmed on that version).
- **Clear messages** when Xbox ends the session, your cloud time runs out, or a free game needs "Get" on xbox.com.

**In-app updates**
- When a new version is released, "Update now" downloads it from GitHub and checks its digital signature.
- It saves the package to `/data/pkg` and tells you how to install it: **PS button → Settings → GoldHEN → Package Installer**.
- Also available in About → "Check for updates".
- *This works starting with the next release: install 1.3.0 manually as usual.*

---

## Instalación / Installation

1. Descarga `XCloud4-1.3.0-beta-publico.pkg` (abajo, en *Assets*). / Download `XCloud4-1.3.0-beta-publico.pkg` (below, under *Assets*).
2. Cópialo a la PS4 (USB o FTP en `/data/pkg`) e instálalo con **GoldHEN → Package Installer**. / Copy it to the PS4 (USB, or FTP to `/data/pkg`) and install it with **GoldHEN → Package Installer**.
3. Comprueba el SHA-256 con `SHA256SUMS.txt` si quieres. / Optionally verify the SHA-256 against `SHA256SUMS.txt`.

## Requisitos / Requirements

- PS4 con **GoldHEN**. Probado en PS4 Fat, firmware 12.00, GoldHEN v2.4b18.7. / PS4 with **GoldHEN**. Tested on a PS4 Fat, firmware 12.00, GoldHEN v2.4b18.7.
- Game Pass con juego en la nube, o una cuenta gratuita de Microsoft con perfil de Xbox para juegos gratis como Fortnite. / Game Pass with cloud gaming, or a free Microsoft account with an Xbox profile for free games such as Fortnite.
- Conexión estable de 15 Mbps o más, mejor por cable. / A stable 15 Mbps or faster connection, preferably wired.

## Código fuente / Source code

XCloud4 es software libre (GPL-3.0). Este release incluye el código fuente completo de esta versión y el de sus dependencias.
XCloud4 is free software (GPL-3.0). This release includes the complete source code of this version and of its dependencies.

---

*XCloud4 es un proyecto independiente, sin afiliación con Microsoft, Xbox ni Sony. / XCloud4 is an independent project, not affiliated with Microsoft, Xbox or Sony.*
