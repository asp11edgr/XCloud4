# XCloud4 0.7.20 — measured video-conversion bottleneck

Product **0.7.20**, PS4 **APP_VER 00.90**, application identifier **XCLD00001**. The owner still sees slow game video. Controller response is tentative; this attempt is not a complete validation of buttons, axes or local chords.

## Evidence boundary

Only observations after the current **0.7.20 application banner** are used here. Earlier crash and suspension records in the same private capture belong to preceding launches. The immutable [0.7.20 release notes](RELEASE_NOTES_0.7.20.md) preserve package/source hashes, the actual review findings and their corrections. Raw account/system/console logs remain private.

The current launch records **`Gnm submitDone result=0x00000000`**, starts the Xbox input sender, decodes a first Opus packet with **960 samples**, and produces an H.264 image of **1280 × 720**, pitch **1280**. A successful call does not by itself prove repaired external suspension or closure.

## Measured processing costs

One paired media/render interval records:

| Observation | Recorded value | Derived mean |
|---|---:|---:|
| Actual interval | 5,257,464 µs | 5.257464 s |
| Native Decode calls | 10; 95,930 µs total | 9.593 ms per call |
| NV12-to-RGB conversions | 10; 4,995,965 µs total | 499.5965 ms per converted image |
| Image draws | 10; 57,716 µs total | 5.7716 ms per draw |
| New image draws | 10 | About 1.90 per second |
| Media ticks | 10; 5,093,967 µs total | Conversion accounts for about 98% of tick time |

The new-draw rate is calculated from the recorded count and elapsed interval; it is not an independent measurement of physical scanout FPS. Later intervals repeatedly report approximately **500 ms per conversion**, with conversion maxima around **500–502 ms**. Native decode and draw costs vary; their first interval means above are not a fixed device-wide guarantee.

The paired queue summary reports **249 queued packets**, a **256-packet high-water mark**, **180 full-queue events** and **181 dropped packets**. Following intervals reach depth 256 and record further overflow, lost/damaged reconstruction and keyframe requests. These counters show queue pressure during slow processing; they do not identify every packet-loss cause.

The source checks its 6 ms/two-Decode budget between operations. A single conversion takes roughly half a second, so that budget cannot interrupt it or guarantee a 6 ms tick. The evidence identifies the conversion path as the dominant measured local cost in this sample.

## Memory-access hypothesis and next change

Native output pictures are direct-memory **type 3 (`WC_GARLIC`)**, mapped with CPU/GPU read/write protection. The current conversion reads Y and interleaved UV bytes directly from that picture inside its per-pixel loop. The protection bits grant access; they do not make the mapping CPU-cacheable.

The public [Moonlight decoder reference](https://github.com/JaimeJimenezG/Moonlight-ps4/blob/61427a214d4e632ee246816a98ee4f2374844a73/src/video/decoder_orbis.c#L111) uses streaming loads to copy WC pictures into ordinary CPU storage before conversion. [Intel's SSE4.1 documentation](https://www.intel.com/content/dam/develop/external/us/en/documents/d9156103-705230.pdf), section 2.2.3, explains the aligned streaming-load mechanism for WC memory. This supports an access-path optimization; it does not prove that the exact measured cost comes solely from caching or establish the next version's speed.

The [0.7.21 implementation](RELEASE_NOTES_0.7.21.md) retains the working native output memory and adds one bounded copy into persistent CPU storage before the existing RGB conversion. It selects SSE4.1 only when runtime CPUID leaf 1, ECX bit 19 reports support, otherwise using baseline SSE2. CPU ordering fences do not establish decoder/GPU completion. The existing even-width/even-height and pitch/owned-buffer bounds remain; no new even-pitch restriction is introduced. Copy and conversion timings are recorded separately.

The native package build, matching-source inventory and emitted instruction dispatch are verified, and a fresh actual Claude Opus 5.5 source review returned PASS. VM, PC and the retrieved PS4 package hashes match. The 0.7.21 console result remains pending; no speed improvement is established. A pre-existing counter-reset issue can produce one wrapped timing delta across a decoder restart, so that interval must be excluded from comparisons. Actual improvement, queue behavior and game controls still require the next console attempt. The previous asynchronous graphics exception remains separate from this performance report.
