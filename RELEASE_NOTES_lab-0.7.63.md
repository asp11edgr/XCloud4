# XCloud4 Lab 0.7.63

## Español

Candidata privada de la TAREA 27, sin publicar. APP_VER **01.31**, XCLD00001.

- Sincronía vertical **Automática / Siempre / Nunca**. Automática empieza sin VSync;
  tres segundos completos entre27 y33 AU/s la activan; tres entre55 y65 la desactivan.
  Los segundos vacíos o fuera de esos rangos se ignoran, sin cortar la racha.
- Presentación experimental **Cola / Mailbox**, solo con VSync efectivo. Mailbox conserva
  la imagen convertida más reciente, cuatro scanouts y un único flip pendiente.
  Sus resultados y el margen antes del refresco necesitan medición en consola.
- La verificación de GPU copia pequeñas bandas a memoria propia y las compara en un
  hilo de baja prioridad. Una diferencia confirma cuarentena y vuelta segura a CPU.
- Diagnóstico de esperas del decoder y ocupación del anillo, separado por presentación.

Se conservan los controles, audio, configuración del decoder y las opciones de imagen
Dither, Deband y CAS del Lab. Los shaders anteriores no cambian. El selector es temporal;
solo la preferencia CPU/GPU del Lab conserva su persistencia anterior.

Prueba propuesta: Fortnite, Siempre+Cola y Siempre+Mailbox, cinco minutos cada uno;
Forza en Automática. El banco T27 es un PKG separado. No se afirma ningún resultado
nuevo de PS4 antes de ejecutar esas pruebas. No se publica nada automáticamente.

## English

Private TASK27 candidate, not published. APP_VER **01.31**, XCLD00001.

- Vertical synchronization **Automatic / Always / Never**. Automatic starts disabled,
  enables after three complete27–33 AU/s seconds, and disables after three55–65 AU/s
  seconds. Empty and other seconds are ignored without resetting the streak.
- Experimental **Queue / Mailbox** presentation, effective only with VSync. Mailbox
  retains the newest converted image, uses four scanouts and one pending flip.
- GPU verification copies sparse bands to owned memory for a low-priority worker.
  A mismatch quarantines the GPU and safely returns conversion to CPU.
- Decoder waits and ring ownership diagnostics, separated by presentation mode.

Decoder configuration, audio, input and existing shaders are unchanged. Existing Lab
Dither, Deband and CAS options remain available. New presentation choices last until
app exit; the existing Lab CPU/GPU preference keeps its prior persistence.

Console verification is pending. The T27 probe is a separate package; nothing is
installed, launched or published by preparing this release.
