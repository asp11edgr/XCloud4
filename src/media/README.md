# Live media reception

`live_media.c` receives borrowed, decrypted RTP through bounded copy queues. Video RTP is reordered, depacketized into complete H.264 access units and passed to the native video decoder on the main thread. Audio RTP is consumed by the native audio worker and decoded with Opus.

Loss or malformed video access units request an IDR, and foreign/late packets do not themselves discard a valid in-progress frame. Each track locks payload type and SSRC. Parameter sets commit only from complete valid access units. The transport and authentication worker must stop and join before freeing this context.

This is development code, not a confirmed live game stream. See [design and limits](../../docs/MULTIMEDIA_EN_VIVO.md).
