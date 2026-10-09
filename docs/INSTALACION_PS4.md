# Installing XCloud4 on PS4

## Confirmed package: 0.6.2

`XCloud4-0.6.2.pkg` is installed and confirmed on the owner's PS4. Passport authorization, `/connect` acceptance and automatic session deletion work without an error. It is the latest confirmed milestone; it does not yet stream the game.

- Application identifier: `XCLD00001`.
- Package size: 6619136 bytes.
- SHA-256: `25c724aca93728d53c9d4c6b7e52f0acff3dff45c263779bdbcc87e25699b601`.
- Transferred location: `/data/pkg/XCloud4-0.6.2.pkg`; the FTP round-trip hash matched.
- Target: PS4 firmware 12.00 with GoldHEN v2.4b18.7.

The Spanish account view announces the temporary reference client also used by GreenVita. A complete new Microsoft authorization is required when changing client profiles. Microsoft's consent page may show a different application name.

## Installation over the network

1. Close XCloud4 and enable the GoldHEN FTP server.
2. Transfer the desired PKG to `/data/pkg`.
3. In GoldHEN → Debug Settings, choose **Package Source: HDD** or **ALL**.
4. Open **Package Installer**, choose the exact package filename and accept replacing XCloud4 when prompted.
5. Open XCloud4 from the PS4 menu. Retain the actual result or a photo of any error.

GoldHEN supports packages in `/data/pkg`; [upstream reference](https://github.com/GoldHEN/GoldHEN). Menu names and placement may vary by GoldHEN version.

## Installation by USB

1. Turn on PS4 and enable GoldHEN using the owner's usual procedure.
2. Connect a FAT32/exFAT USB drive already recognized by the console to the PC. Formatting is unnecessary if it already works.
3. Copy the desired XCloud4 PKG to the USB root and safely eject it from Windows.
4. Connect it to PS4, open Package Installer and select the exact XCloud4 filename.
5. Confirm the application name before accepting replacement, then open it from the console menu.

All versions share `XCLD00001`; a replacement installs the new version over that application.

## Using the confirmed 0.6.2 milestone

1. Open `CUENTA`. Press X to request access, then authorize on Microsoft's website from a phone or PC.
2. Open the catalog with R1 after authorization.
3. Select a title marked `CON ACCESO` and press X.
4. Wait for preparation and connection authorization. The Spanish UI reports Xbox acceptance.
5. The confirmed version closes the prepared/authorized session automatically after its 45-second hold. No game video or audio is expected from this milestone.

Circle requests session closure before returning; `OPTIONS` waits for closure before exiting. The automatic close path is confirmed, while those manual active-session paths were not checked separately. See [connection authorization](AUTORIZACION_CONEXION.md).

## Historical package checks

These describe earlier stage-specific checks; later confirmed results supersede their original pending status.

| Version | Purpose and result |
|---|---|
| 0.1.2 | Included missing Fios2/libc auxiliary modules; owner confirmed startup, `CONTROL` and `PROYECTO`. |
| 0.2.0 | Added `IMAGEN Y SONIDO`: synthetic eight-second H.264 clip and alternating PCM tones. Video worked; audio returned `0x809B0001`. |
| 0.2.1 | SYSTEM-user MAIN AudioOut fix; owner confirmed sound. `OPTIONS` still caused `CE-34878-0`. |
| 0.2.2 | Native LoadExec exit; owner confirmed return to PS4 without `CE-34878-0`. |
| 0.5.0 | Owner and Klog confirmed AMONGUS preparation and automatic deletion without error. |
| 0.6.0 | Own-client Passport refused authorization with HTTP 400 before `/connect`; deletion succeeded. |
| 0.6.1 | Identified the exact Passport refusal as `invalid_scope`; deletion succeeded. |
| 0.6.2 | Temporary reference client: Passport 200, `/connect` 202, DELETE 200 confirmed. |

## Controls

- Home: D-pad chooses a view; X opens it.
- `CONTROL`: button colors, stick positions and trigger values are shown. Hold L1 and press Circle to return.
- `PROYECTO`: Circle returns.
- `IMAGEN Y SONIDO`: X repeats the local sample; Square mutes/unmutes; Circle returns.
- `CUENTA`: X requests authorization; Square checks connection; Triangle clears the local account; R1 opens the catalog after authorization.
- Catalog: D-pad selects; L1/R1 move eight entries; Square refreshes; X requests the selected title's session; Circle returns to the account.
- Session: Circle requests closure and returns after completion.
- `OPTIONS`: requests application exit, waiting for active network cleanup.

The local sample contains a synthetic 640 × 368 H.264 image and soft alternating stereo tones. They are independent demonstrations, not an Xbox stream or evidence of WebRTC audiovisual synchronization.

If installation fails, the screen stays black or the app closes, retain the exact message or a photo. Console logs distinguish dependency-loading, network and media failures. Individual controller values and every manual cancellation path have not been verified separately.
