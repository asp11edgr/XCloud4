# Live native media — implementation in progress

The owner authorized continuing beyond the confirmed 0.6.2 session-authorization milestone toward actual game video and audio. This document describes current source and static review, **not confirmed PS4 playback**. The local synthetic media sample remains separate.

## Reception and H.264 reconstruction

`src/media/live_media.c` validates RTP v2 headers, CSRC/extension/padding bounds and the 2048-byte packet limit, then copies packets into bounded queues. Transport callbacks never retain a borrowed packet or decode/render it. A track locks payload type and SSRC; foreign, late and duplicate packets are dropped without switching decoder state.

Video uses bounded sequence reordering, a short hole timeout and a 2 MiB access-unit limit. It implements single NAL units, STAP-A and FU-A for non-interleaved packetization mode 1, following [RFC 6184](https://www.rfc-editor.org/rfc/rfc6184). A timestamp change before completion, missing fragments, malformed aggregation or overflow discards the damaged unit and requests a keyframe. Damaged timestamps cannot resume as apparently complete IDRs. SPS/PPS changes commit only from a complete valid unit. Recovery waits for IDR plus cached parameter sets.

The main thread feeds `src/video/live_h264.c`. Its configuration currently limits output to 1280 × 720, profile 66, max level 31, decode pipeline depth one, and four native output buffers. This matches the current offer's H.264 `42e01f` level limit. Two persistent ONION input buffers keep the immediately previous compressed submission untouched. Output dimensions, pitch, size and buffer ownership are validated before NV12→RGB conversion. Inputs/outputs are retained if decoder, queue, unmap or direct-memory release fails.

These parameters and input-consumption assumptions require actual console evidence. They must not be reported as established by static review or a successful build.

## Opus and AudioOut

`src/audio/live_audio.c` reorders bounded RTP and decodes payloads using the external Opus **1.5.2** library. The decoder produces 48 kHz stereo S16 PCM. [RFC 7587](https://www.rfc-editor.org/rfc/rfc7587) supplies the Opus RTP protocol reference; the library's decoder validates compressed payloads.

The PCM queue is bounded, trims excess latency and outputs silence on underflow after playback begins. Native MAIN AudioOut uses SYSTEM (`0xFF`), preserving the confirmed local audio association. The worker waits before overwriting its output buffer; that submitted buffer belongs to the context, so a failed final wait does not leave native code pointing at a retired thread stack. Join/close failure retains allocations. Negotiated payload types can be fixed before workers start.

License, official source release and verified archive hash are in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Lifecycle and observable state

- Create/start/tick/draw/snapshot run on the main thread.
- RTP callbacks only copy data and update atomics; the audio worker owns its decoder/PCM state.
- Keyframe requests are exchanged atomically and counted; the transport owner sends the corresponding request.
- Close is permitted only after RTC callbacks stop and the authentication owner worker is joined.
- Counters distinguish real decoded pictures/Opus packets from dropped data and underflow silence. No picture or sound is claimed until native decode produces it.

## Current limitations

- No timestamp-based audiovisual synchronization, Opus packet-loss concealment or long-session clock handling yet.
- The parameter-set cache retains one SPS and one PPS, not a bank of parameter IDs.
- SSRC migration requires an explicit new track/session; packets cannot implicitly switch sources.
- A fatal native decoder error stops further video feed and is exposed in the snapshot.
- Native decoder/output ABI, live AudioOut behavior and 720p performance are not confirmed.

No automated tests were added or run for this implementation. Actual console logs and the owner's visible/audible result are required to confirm the next milestone.
