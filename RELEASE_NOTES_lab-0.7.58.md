# XCloud4 Lab 0.7.58-lab — Experimental Tester Release Notes

> **EXPERIMENTAL BUILD WARNING:** `XCloud4 Lab` is an experimental laboratory edition intended for advanced testers and protocol/decoder diagnostics. Some combinations of settings **may be unstable, freeze, or abort the stream**. For everyday play, use the **Public Beta (`XCloud4-1.2.1-beta-publico.pkg`)**.

---

## Package Information

- **File:** `XCloud4-0.7.58-lab.pkg`
- **PS4 Menu Title:** `XCloud4 Lab`
- **Title ID / Content ID:** `XCLD00001` (`IV0000-XCLD00001_00-XCLOUD4APP000000`)
- **Product Version / PS4 `APP_VER`:** `0.7.58-lab` / `01.26`
- **Size:** `17,563,648` bytes
- **SHA-256:** `1f6341d92701652a29854c4bcaeaa86f03771facbc62fdf48e64083a5ea18de9`

### Side-by-Side Installation with the Public Beta

`XCloud4 Lab` uses its own Title ID (`XCLD00001`), menu title (`XCloud4 Lab`), and `LAB` badge icon (`assets/icon0-lab.png`). It can be installed on the PS4 **at the same time** as the Public Beta (`XCloud4`, `XCLD00003`) without overwriting it.

---

## What's New in 0.7.58-lab

- **País de acceso (opcional)** en la tarjeta de posible restricción regional y en **OPTIONS > Acceso**, igual que en la [pública 1.2.1-beta](https://github.com/asp11edgr/XCloud4/releases/tag/v1.2.1-beta). Por defecto está en Automático (sin cambios en el inicio de sesión); con otro país, solo el inicio de sesión en la nube lleva la declaración de país. La elección se guarda en `/data/xcloud4/access-v1.bin`, sin datos de la cuenta.
- **Aviso de actualización al abrir la app**, con QR a la release de GitHub; el Lab compara con las releases `lab-*`.
- **Panel técnico `R3`: "Probar aviso de actualización".** Muestra la ventana con datos de ejemplo (versión `9.9.9` y la página general de releases) para probarla en la consola sin consultar nada nuevo. Solo existe en el Lab.
- **klog:** `XCloud4: pais de acceso=<codigo|auto> origen=<tarjeta|ajustes|guardado>` y `XCloud4: login gssv oferta=<oferta> pais_acceso=<codigo|auto> http=<n>`.
- Sin cambios en vídeo, audio, mando, vibración ni conexión con Xbox respecto a la 0.7.57.

**Aviso del País de acceso:** Usar un país de acceso distinto puede ir contra los términos de Xbox y Microsoft podría restringir tu cuenta. Úsalo bajo tu propia responsabilidad.
**Access country notice:** Using a different access country may go against Xbox's terms, and Microsoft could restrict your account. Use it at your own risk.

---

## Requirements

- **Console:** PlayStation 4 running **GoldHEN**. Probado en: PS4 Fat, firmware 12.00, GoldHEN v2.4b18.7. Si la usas en otro modelo o firmware, cuéntanos en un issue si funciona.
- **Subscription / Account:** An Xbox Game Pass plan that includes cloud gaming, or a free Microsoft account with an Xbox profile for Fortnite.
- **Network:** A stable internet connection of at least **15 Mbps** (wired Ethernet recommended).

---

## Aviso / Disclaimer

Aviso: XCloud4 es un proyecto independiente, sin afiliación con Microsoft, Xbox ni Sony. Se ofrece tal cual, sin garantías. Lo usas bajo tu propia responsabilidad: el proyecto no se hace responsable de problemas con tu consola, tu cuenta o tus suscripciones. Usa XCloud4 respetando los términos de servicio de Microsoft y Xbox.

Disclaimer: XCloud4 is an independent project, not affiliated with Microsoft, Xbox or Sony. It is provided as is, without warranty. You use it at your own risk: the project is not responsible for problems with your console, account or subscriptions. Use XCloud4 in accordance with Microsoft and Xbox terms of service.
