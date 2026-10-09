# Native video pipeline comparison for 0.7.24

## Measured starting point

The [bounded 0.7.23 attempt](ERROR_REPORT_0.7.23.md) delivered a first **1280 × 720** picture despite requesting 540p. Copy averaged **29.349 ms**, conversion **3.003 ms**, and new image draws **13.775/second**. The selection also counted **1765 repeated draws**. These are local application measurements from one live attempt, not controlled comparisons, server FPS or input latency.

This review inspected selected public native implementations at immutable revisions. Source behavior can explain a design choice; it does not prove that another client's reported performance will transfer to this PS4. No upstream code, tests, binary or hardware probe was executed or incorporated by this research task.

## What other clients actually do

| Client and pin | Verified source behavior | Portable concept and platform limit |
| --- | --- | --- |
| **GreenVita** `ae2625d295b4fba005a769b1309fd70dcd6cb63f` | A [dedicated decode worker](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/streaming/video/worker.rs#L38-L108) has a bounded 2–6 AU backlog. Its [result slot replaces a pending result](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/streaming/video/worker.rs#L263-L274). | Separate expensive video work from UI/input. [Two GXM textures](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/streaming/video/mod.rs#L95-L115) have explicit ownership and a bounded wait. Vita's [RGB565 decoder output](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/streaming/video/decoder.rs#L157-L198) cannot establish a PS4 decoder/display format contract. |
| **PSBox / Xbox Cloud Gaming PS5** `ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e` | [Video and audio workers](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/app/stream_player.cpp#L400-L403) run separately. When behind, it [decodes before skipping presentation](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/app/stream_player.cpp#L227-L238), with at most two consecutive omissions. | Preserve H.264 prediction while favoring a newer decoded image. This pin's [primary decoder is FFmpeg; hardware is an optional probe](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/app/stream_player.cpp#L135-L201). Its [Vulkan YUV upload/compute path](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/src/display/gpu.cpp#L831-L865) requires a separate PS4 graphics implementation. |
| **ProsperoLight / Moonlight PS5** `a6e56cf4e8e9e469d5bc5cc06ff71ad4ca16a6a9` | [Decode and presentation ownership](https://github.com/blackbearreloaded/ProsperoLight/blob/a6e56cf4e8e9e469d5bc5cc06ff71ad4ca16a6a9/src/moonlight_stream.cpp#L592-L595) is separated. A [ready-image mailbox](https://github.com/blackbearreloaded/ProsperoLight/blob/a6e56cf4e8e9e469d5bc5cc06ff71ad4ca16a6a9/src/moonlight_stream.cpp#L1648-L1668) replaces the oldest ready surface. Presentation [retires the previous flip before choosing the next image](https://github.com/blackbearreloaded/ProsperoLight/blob/a6e56cf4e8e9e469d5bc5cc06ff71ad4ca16a6a9/src/moonlight_stream.cpp#L2390-L2428). | Use explicit leases and choose the newest completed image late. [Workers join before teardown](https://github.com/blackbearreloaded/ProsperoLight/blob/a6e56cf4e8e9e469d5bc5cc06ff71ad4ca16a6a9/src/moonlight_stream.cpp#L2461-L2476). PS5 AGC surfaces, fences and CPU placement are not PS4 API guarantees. |
| **Moonlight PS4** `61427a214d4e632ee246816a98ee4f2374844a73` | It [preserves Decode and may omit bounce/presentation](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c#L599-L608). Its [conversion pipeline](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c#L777-L792) overlaps conversion with the following Decode. | Research ownership and overlap. Its cache aliases, parallel bounce timings and firmware context are not verified for this console. No root LICENSE/COPYING was found in the pinned tree: its application code remains research-only. |
| **AJ GeForce NOW PS4** `87568ca50a89e6f8dd65fe005d21521c27e65c26` | A [decoder worker](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/webrtc/media.cpp#L18-L60) measures queue residence. Its [AU policy](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/stream/DecodeQueuePolicy.hpp#L14-L22) permits 2–8 units and 67 ms; [overflow explicitly enters resynchronization](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/src/opennow/webrtc/media.cpp#L167-L189). | Measure residence and isolate decoder work. Do not import its queue limits or discard compressed units without its recovery contract. Its provider protocol and software-decoder routes differ. |

Browser techniques were separately reviewed in the [Better xCloud comparison](BETTER_XCLOUD_COMPARISON_0.7.23.md). Browser canvas/WebGL/WebGPU and WebAudio controls are not native PS4 switches. Lower compressed bitrate does not reduce the byte count of an unchanged 720p NV12 picture.

## Proposed 0.7.24 scope

1. Give one video worker exclusive ownership of decoder start/stop, RTP reorder/assembly, Decode, staging copy and conversion. Keep UI/input polling and presentation independent.
2. Exchange completed RGB images through a bounded triple-buffer mailbox. A consumer-held image stays immutable; only an unleased pending image can be replaced. Continue decoding admitted predictive AUs in order.
3. Present a newly completed image once. Keep the previous display image when no new image exists; UI changes still require an update. Preserve flip retirement, `sceGnmSubmitDone` and safe framebuffer reuse.
4. Preserve audio, native output memory type 3, four native output slots, bounds, IDR recovery and the existing exit path. Join the worker before freeing its decoder or buffers.
5. Log actual output dimensions, unique updates, replacements, local queue residence, copy/convert timings and shutdown outcome. Distinguish ingress packet residence and local mailbox age from end-to-end video or input latency.

These changes target serialized work, repeat rendering and queue growth. The approximately 29 ms copy still limits worker throughput; the design cannot promise 60 FPS or remove this transfer. GPU conversion, direct decoder-to-display surfaces, cache aliases, CPU-affinity changes and compressed-video catch-up policies require separate platform evidence and are outside this proposal. Console playback and clean closure must be observed after any implementation.

## Source and license receipts

Thirty-one downloaded files were byte-verified against their pinned Git-tree blob identifiers; full SHA-256 fingerprints remain in the private research manifest. License-file SHA-256 values are:

| Project | Inspected license evidence | SHA-256 of root license |
| --- | --- | --- |
| GreenVita | [MPL-2.0](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/LICENSE#L1-L2) | `542dfacb551bb6d091d7de38c696b5a6c3afe2e9f86007876602fd4cb0f98089` |
| PSBox | [GPL-3.0 text](https://github.com/RafaelNGP/xbox-cloud-gaming-ps5/blob/ef22c57a9fdf542e0d4c7a0ecd81ea9c688c1b1e/LICENSE#L1-L3); inspected source has GPL-3.0-or-later SPDX | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |
| ProsperoLight | [GPL-3.0 text](https://github.com/blackbearreloaded/ProsperoLight/blob/a6e56cf4e8e9e469d5bc5cc06ff71ad4ca16a6a9/LICENSE#L1-L3); inspected source has GPL-3.0-or-later SPDX | `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986` |
| AJ GeForce NOW PS4 | [MIT](https://github.com/AJfiles/AJ-GeforceNow-PS4/blob/87568ca50a89e6f8dd65fe005d21521c27e65c26/LICENSE#L1-L21) | `fc277a8a15148216929685a49104bb4723106540c681d2afdff537709f784ca8` |
| Moonlight PS4 | No root license or copying file in inspected tree | Not available |

This research documents original implementation concepts. It neither copies upstream application source nor establishes that any upstream runtime behavior has been reproduced on the owner's PS4.
