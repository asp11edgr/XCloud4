# Development log — October 8, 2026

## Current confirmed state

The owner and console logs confirm **0.6.2** through Microsoft authorization, Xbox catalog, remote preparation, Passport HTTP 200, `/connect` HTTP 202 and automatic DELETE HTTP 200, with no cleanup error. Actual WebRTC game video/audio and input are the next milestone. The owner authorized continued work toward real video and audio.

Repository documentation and GitHub content are English; the PS4 interface remains Spanish. The GitHub repository must be private. Historical entries below distinguish preparation, static review, compilation and subsequent console confirmation. No automated tests were added or run during these milestones.

## Initial environment and application

The existing Lubuntu VM was prepared with local Windows access. XCloud4 added an original home/controller/project UI, double-buffered VideoOut with errors, DualShock 4 reading/pressed transitions/reconnection, ELF/eboot/PKG generation, an original icon, GPL-3.0-only licensing and dependency notices. GreenVita, Better xCloud and Moonlight PS4 sources/licenses were researched; source adaptation was still pending.

### 0.1.0: missing Fios2

The owner installed 0.1.0 on PS4 12.00 with GoldHEN v2.4b18.7. Startup showed the damaged-application-data screen. The photo established a startup failure but not its cause.

Klog access was obtained. The second launch recorded `EXEC /app0/eboot.bin`, `Loading /app0/sce_module/libSceFios2.prx fails (0x80020002)` and `PRX_SCE_MODULE_LOAD_ERROR` (`0xa0020102`). This identified a missing dependency before `main`. Complete logs stay outside the repository.

### 0.1.1: Fios2 added, libc missing

Version 0.1.1 added Fios2 from an external module directory, using the SELF from the verified OpenOrbis v0.5.4 hello_world example without modifying it or committing the binary. GP4 includes `sce_module/libSceFios2.prx`. Its SHA-256 is `3f8236c5996cf8e9917b8d27706d9743f6603d89de3b9859eacbb9f466b19c01`.

The PKG was built in Lubuntu, sent to `/data/pkg/XCloud4-0.1.1.pkg` and downloaded back over FTP with a matching hash. The owner's launch then confirmed Fios2 loaded, followed by missing `/app0/sce_module/libc.prx` with the same loading errors.

A native Fios2 copy was also obtained from the console by FTP. It began with ELF, as the server returned a decrypted module rather than a package-ready SELF. It stayed outside Git and was not used. The packager rejects an unconverted ELF.

### 0.1.2: startup confirmed

Version 0.1.2 added libc from the same SDK example and made packaging require both modules. The original characterization was corrected: their corresponding source is in OpenOrbis v0.5.4 `src/modules`, with `build-and-copy.sh` generating the auxiliaries. They are open OpenOrbis modules, not proprietary Sony modules. The console-derived module was not used. Auxiliary libc SHA-256: `39ad53672bb0b14895f8465eb1619478ab935f5afe5dbf69c15f02cfc0a75ce6`.

The package built in Lubuntu and its FTP round-trip hash matched. Klog shows `EXEC /app0/eboot.bin` and the 0.1.2 startup header for process 75 without previous missing-module errors. The owner confirmed that `CONTROL` and `PROYECTO` appear and both work. Startup and those views are confirmed on PS4 12.00; every button/axis/trigger was not checked individually. Xbox, decoding and sound were still future work at this stage. This milestone is preserved as `v0.1.2`.

## Claude Code setup and initial reviews

Claude Code Pro performed two static reviews with Sonnet 5. Proposals were compared with SDK headers/examples and generated files. Concerns about memory type, video pitch and package file lists did not establish defects. A later GOT/RELRO proposal depended on unverified loader behavior and was not applied.

The owner requested Opus 5.5. Claude Code 2.1.220 rejected that model because it required 2.1.280 or newer; WinGet updated it to 2.1.292. Two later reviews actually recorded `claude-opus-5-5`. One rejected the proposed GOT/RELRO change because it did not resolve its hypothesis and removed a section needed by the converter. The review with console evidence identified the missing dependency as the supported explanation, while noting that the added module still needed console confirmation. Static review never substitutes for PS4 execution.

## 0.2.0: local media sample

The third home option, `IMAGEN Y SONIDO`, adds an eight-second synthetic H.264 pattern and alternating soft stereo PCM tones. It uses native Videodec2 and AudioOut; it is not Xbox footage or an audiovisual synchronization result.

Claude Opus 5.5 reviewed module-list capacity, names with extensions, decoder buffer ownership, mapping failures and audio state. The review limits and ABI attribution are in [MULTIMEDIA.md](MULTIMEDIA.md). The package built in Lubuntu without compile errors and GP4 includes the sample, executable and both OpenOrbis auxiliaries. The transferred and downloaded package hashes matched.

The owner reported apparently working video. The photo shows 217 / 240 frames and audio error `0x809B0001`. Klog confirms first H.264 frame `640x368 pitch=640`, sample completion and `[AudioOut] Error:sceMbusAddHandleByUserId 0x20000007`. This confirms the local H.264 sample on 12.00, not 720p performance or WebRTC reception.

## 0.2.1: local sound confirmed, exit failure located

Version 0.2.1 opens MAIN with SYSTEM (`0xFF`) following OpenOrbis v0.5.4's public audio-wav example, waits for consumption before reusing PCM and logs init/open/thread/completion. Claude Opus 5.5 found no concrete ABI, thread or buffer-lifetime defect in that change. The ALREADY_INIT constant was compared with the SDK header. Video remained unchanged. The package built and its FTP round-trip hash matched.

The owner confirmed sound works. Klog records successful SYSTEM AudioOutOpen and two plays of 384000 samples with error zero.

The owner also reported `CE-34878-0` on `OPTIONS`. Process 84 ended with SIGSYS in libkernel after a branch from `0x409330`; the exact 0.2.1 ELF contains `_exit@plt` at `0x9330`, reached by returning from `main`. This locates the failure at final process exit.

## 0.2.2: clean exit confirmed

Version 0.2.2 prepares/resolves SystemService before releasing resources, requests LoadExec with `"exit"` and avoids returning from `main`. The UI allows another attempt if preparation or the request fails, or ten seconds pass without process removal. Claude Opus 5.5 reviewed the flow; bounded waiting and a pause between attempts were applied. Confirmed audio/video implementations were preserved. The package built and its FTP round-trip hash matched.

After installing, the owner confirmed return to the PS4 home screen without `CE-34878-0`. Klog for process 90 records resources released, exit request, `Kill for LoadExec(0x5a)` and `Kill for LoadExec(0x5a) => 0`. Local video, PCM tones and clean exit are confirmed and preserved as `v0.2.2`. Microsoft, catalog, Opus and WebRTC were still pending at that point.

## Microsoft registration and 0.3.0

The owner requested continued Claude use and a project-owned registration, authorizing a temporary reference client if the own registration could not be obtained. Microsoft Entra was switched to the personal account directory. XCloud4 was configured for personal accounts and the owner authorized pressing Register, accepting Microsoft's policies. The portal issued public client ID `f9ac8684-1032-4131-bb47-d2f58da9bb93`; public client flows were enabled and saved. No secret, redirect URI or administrator consent was created. The own client was used for this milestone.

Claude Opus 5.5 wrote the native HTTPS layer and device-code worker in two scoped assignments. Codex integrated `CUENTA`, bounded JSON, own registration, module loading and cancellation before exit. Retained HTTP inputs and the worker completion/cancellation race were reviewed. Confirmed 0.2.2 audio/video sources stayed intact.

A third Claude integration review reported no serious defect and noted the `sceHttpReadData` size argument. Its 64-bit declaration was confirmed and adjusted to `size_t`. The final 0.3.0 package built without errors/warnings and its FTP round-trip hash matched. Actual HTTPS and device authorization were still pending before installation.

### 0.3.0 SSL lookup failure

The owner's photo shows `INICIO HTTPS (RED/TLS)`, detail `0x80020002`, HTTP 0. Klog shows `Sysmodule libSceSsl -> 0x00000000`, followed by `modulo libSceSsl -> 0x80020002`: name lookup failed and the fallback returned ENOENT. No Microsoft request had been sent, so this was not evidence of account, registration or TLS-negotiation failure. FTP listings placed Ssl/Http under `/system/priv/lib`, but that view did not establish access from the application sandbox.

## 0.3.1: HTTPS and Microsoft authorization confirmed

Claude Opus 5.5 analyzed the log and reviewed the concrete fix. Name lookup was retained and matching Init/Term exports were added for SSL/HTTP. Equal export addresses identify one provider; differing addresses stop lookup as ambiguous. Probes work even without a module name. The native sandbox common path uses the declared OpenOrbis function without logging its identifier. Existing fallbacks and private native paths are later attempts, with result logging. Media loading order stayed intact; no invented aliases or disabled TLS validation were introduced.

The first build found OpenOrbis lacks `strnlen`; a bounded scan replaced it. The final package built without errors/warnings and its FTP round-trip hash matched.

The owner's photo shows `CUENTA MICROSOFT AUTORIZADA`. Klog identifies 0.3.1, resolves `sceSslInit` from `libSceSsl2.sprx` (handle `0x3a`), initializes Net with zero and marks Net/Ssl/Http HTTPS ready. The connection check ends state 3, HTTP 200, error zero. Device-code and token requests return HTTP 200; authorization ends state 4 (`X4_AUTH_AUTHORIZED`), error zero.

Native HTTPS and Microsoft authorization with XCloud4's own registration are confirmed on PS4 12.00, preserved as `v0.3.1`. Logs contain states/sizes, not codes or tokens. The account exists only in memory and is lost on exit. That result alone did not establish Xbox credentials, catalog, streaming eligibility or game media.

## 0.4.0: Xbox catalog

The owner asked to proceed with Xbox and asked about progress. At that time, stage 3 of 5 had Microsoft confirmed, with Xbox credentials/catalog under construction; WebRTC was the next stage.

Claude Opus 5.5 implemented and statically reviewed Xbox requests, JSON and the span reader. Its final run completed **56 turns** without tool denials and recorded the selected model. Codex integrated account → R1 → catalog, Square refresh, D-pad selection and L1/R1 movement by eight. One worker reuses private Microsoft access for RPS, XSTS, cloud credentials and default-region `/v2/titles`. Valid Microsoft access survives catalog error/cancellation. The list retains 128 entries; the first 32 request Store names in batches of eight. Public snapshots/logs receive no tokens or bodies. TLS validation, time limits and disabled redirects remain enabled.

Codex removed XErr meanings that Claude acknowledged were unverified, adjusted the received counter/local-limit indicator and added cancellation during catalog traversal. The package built without errors/warnings and the FTP round-trip hash matched.

### Confirmed catalog result

The owner sent a photo showing 0.4.0, 2733 titles received and 128 locally retained. Klog confirms Microsoft AUTHORIZED, then RPS, XSTS, `xgpuweb` login and `/v2/titles`, all HTTP 200. The regional response is 1247270 bytes with 2733 valid entries, 128 retained and 21 of those marked with access by Xbox. Four Store batches obtain eight names each, 32 total. Final state is READY, HTTP 200, error zero, XErr zero.

The own XCloud4/PS4 description was accepted. The free-to-play fallback was unnecessary. This milestone is preserved as `v0.4.0`. Catalog navigation, refresh and cancellation were not checked separately, and no game was started or streamed. Stage 3 reached its first real account/catalog milestone.

## 0.5.0: game preparation and closure

The owner requested title search as a future update, not implementation now. It is recorded in [MEJORAS_FUTURAS.md](MEJORAS_FUTURAS.md). Existing applications remain research bases: GreenVita for Xbox exchanges, Better xCloud for settings and Moonlight PS4 for native APIs. WebRTC candidates and SDK evidence were recorded in [WEBRTC_PS4.md](WEBRTC_PS4.md); none was integrated at this point.

Claude Opus 5.5 implemented session transport, private Xbox credential reuse and account-worker integration, completing **52 turns** successfully. Codex integrated X from the catalog, the session view, waiting for closure on return/exit, fixed local error messages and packaging. Static reading added a deadline check after the remote state response, preventing late READY, and validated the title identifier before public display.

The worker POSTs `/v5/sessions/cloud/play` once for the selected title, then polls readiness. It uses the own XCloud4/PS4/Orbis registration/profile. Creation is not repeated after transport failure. ReadyToConnect or Provisioned mean prepared for negotiation, not a connected game. A ready session is held at most 45 seconds before automatic deletion. Circle waits for closure before returning; `OPTIONS` waits before native exit. DELETE remains bounded after cancellation; unconfirmed closure remains an error. No Passport, SDP, ICE or game media was present in 0.5.0.

The package built without errors/warnings and its FTP round-trip hash matched.

### Confirmed preparation result

The owner's AMONGUS photo shows Xbox preparation, readiness to negotiate and 34 seconds remaining. Klog identifies 0.5.0: creation HTTP 202 (153 bytes), resource wait HTTP 200 (75 bytes), readiness HTTP 200 (70 bytes), READY. At timeout: STOPPING, DELETE HTTP 200 with empty response, error zero, CLOSED. Result: `ready_seen=1`, `cleanup_failed=0`, cleanup HTTP 200.

Creation, preparation and automatic deletion with the own device description are confirmed, preserved as `v0.5.0`. Passport, WebRTC, game media and input were still pending. Circle, cancellation during creation and `OPTIONS` with an active session were not checked separately.

## 0.6.0: authorization implementation and external WebRTC builds

Claude Opus 5.5 implemented Microsoft renewal, Passport and regional `/connect` in **53 turns**, successfully. It retained own registration, in-memory secrets, verified TLS, restricted routes, cancellation and final DELETE. Codex integrated states/UI and tightened `/connect` response validation: reject malformed JSON, a non-object root or non-null `errorDetails`. Accepted authorization does not establish WebRTC or visible media.

Mbed TLS 3.6.7 built static archives for OpenOrbis without running programs/tests. libdatachannel configuration completed, but the first build stopped on missing BSD types and `pthread_np.h`; ABI, DNS and entropy work was still required. Pins/configuration/limits are in [WEBRTC_PS4.md](WEBRTC_PS4.md). Those dependencies were not in the 0.6.0 package.

Lubuntu stopped responding to SSH and VirtualBox guest execution; the hypervisor log reported an unresponsive guest. Restoring NAT forwarding and requesting a normal shutdown did not recover it. The VM was reset; SSH recovered and saved Windows source/external archives were retained. The cause of the hang was not established.

The 0.6.0 package built without errors/warnings and its FTP round-trip hash matched. Before the owner's console result, `v0.5.0` remained the last confirmed milestone.

### Observed Passport refusal

The owner's 1000XRESIST photo shows remote readiness and Microsoft's authorization refusal, detail `0xfffff828`, HTTP 400. Klog confirms account/catalog success, creation HTTP 202, readiness HTTP 200, Microsoft renewal HTTP 200, Passport HTTP 400 (211 bytes). `/connect` was not sent. DELETE returned HTTP 200 without cleanup error.

The specific OAuth code was not retained in 0.6.0. At that point, no account/network fault or exact cause could be concluded.

## 0.6.1: safe OAuth diagnosis

Claude Opus 5.5 implemented safe error classification in `device_auth.c`, completing **15 turns** successfully. Codex reviewed it, updated version/error-heading color and documented diagnosis separately from a fix. No external identifier, retry or scope change was added to that version. The package built without errors/warnings and its FTP round-trip hash matched.

The owner's next photo and Klog identified **`invalid_scope`**. Remote preparation/renewal succeeded, Passport returned HTTP 400, `/connect` was not sent, and DELETE HTTP 200 confirmed closure. No numeric Microsoft subcode was obtained. The cause comparison is documented in [INVESTIGACION_PASSPORT.md](INVESTIGACION_PASSPORT.md).

## 0.6.2: connection authorization confirmed

With the owner's prior fallback authorization, Claude Opus 5.5 implemented the shared authentication profile in **12 turns**, successfully. All steps use one selected client; no refresh tokens cross clients. The own registration remains available. The interface/log announce the temporary public client also used by GreenVita, without implying this project owns that registration.

The owner confirmed that no error occurred. Klog identifies 0.6.2 and records Microsoft renewal **HTTP 200**, Passport **HTTP 200** (1090 bytes), `/connect` **HTTP 202** (empty response), AUTHORIZED and automatic DELETE **HTTP 200**. Final state: CLOSED, `connection_authorized=1`, `cleanup_failed=0`, error zero.

The reference client accepts the same flow that the own client refused. This supports a client-dependent difference, not a verified explanation of Microsoft's exact registration policy. No SDP, ICE, game video/audio or game input was present in this version. This milestone is preserved as **`v0.6.2`**.

## Package integrity record

Every listed PKG is **6619136 bytes**. Each was transferred to `/data/pkg/XCloud4-VERSION.pkg`, downloaded back over FTP and matched to the development PC hash. These checks establish file integrity, not hardware behavior.

| Version | SHA-256 |
|---|---|
| 0.1.1 | `f38e3321f1327d4c2bfeffce42b732840e3d705e1c5f5bf9f662fd867a544497` |
| 0.1.2 | `4c42328bed5150f2f7654a4b9b37c31c8abdcfa88447899c09966e3724141bee` |
| 0.2.0 | `a67ddc21d3d2e5fc2e700ec24407862f83238c6c2b5f3e633171c531671f0a5d` |
| 0.2.1 | `6023aeacd4521e3e486ad3fb335bafab77631bd92613c56a5383037f341ab2e9` |
| 0.2.2 | `be3c6ceceaac2fd5ccc20a169bc2f2127ef83dc616eb1f4e4001803adc2f497e` |
| 0.3.0 | `13d826e4b2e682f4ec40aeeaec0ecf4fc9b39b4d577ff08aff86416cd7485859` |
| 0.3.1 | `abb2ca847e67998e8534d51bc59441d9979d28c9424d4e8821486dd1be7c3022` |
| 0.4.0 | `bd7ae0d4ee3a213295695d6f7372f99622a34a5644e70a01f256b45b30ec57b1` |
| 0.5.0 | `bda87ebf4f46faf427c5ee54724789cb41020f890f26417152071dce3a252440` |
| 0.6.0 | `3b83acf2c2e4f878b447ee70542edf247dec0822516c6d95135450c107404474` |
| 0.6.1 | `bcae7dc054a35fe5d1fe92e8c1eb9b4a2c064f6e1a14193a23203833978bee3e` |
| 0.6.2 | `25c724aca93728d53c9d4c6b7e52f0acff3dff45c263779bdbcc87e25699b601` |

## GitHub and continuing work

The owner requested publication, selecting a **private** repository. The earlier connected account was observed as `asapedgr`; live publication used the verified account **`asp11edgr`**. The private repository is [asp11edgr/XCloud4](https://github.com/asp11edgr/XCloud4). English documentation was published on `main` at `aa6d913`, and the confirmed 0.6.2 release/package was published separately. Keep SDK, credentials, private keys, logs and generated packages outside source history. Packages are stored separately from source.

The owner clarified that English applies to GitHub content, while the PS4 interface stays Spanish. After briefly preserving 0.6.2, the owner authorized the next streaming stage and reiterated use of Claude. Current work targets real WebRTC negotiation, H.264 reception and Opus/AudioOut, with actual console results still required before calling that milestone complete.

Native live media source now includes bounded RTP queues/reordering, H.264 single-NAL/STAP-A/FU-A reconstruction, native decoder output validation and Opus/AudioOut integration. A static review repaired SSRC switching, false loss recovery on late/foreign packets, damaged-unit parameter caching, valid Opus payload rejection, output-buffer lifetime and teardown ownership. These source changes are not evidence of working live playback. See [live media design and limits](MULTIMEDIA_EN_VIVO.md). The external Opus 1.5.2 archive hash was verified, and its exact `COPYING` was retained under `docs/licenses`.

## 0.7.0: native WebRTC/media development package

Claude Opus 5.5 contributed transport/media analysis and implementation before its usage limit paused further calls. Codex completed integration, native media lifetime review, SDK ABI adapters and dependency porting. The pinned libdatachannel static build reached 100%; Mbed TLS and Opus archives were installed outside Git. The final dependency trees are preserved by the source exporter, including modified MPL files, original licenses and reproducible port configuration.

The full application compiled, linked, converted to SELF and packaged successfully in Lubuntu. Static ELF inspection recorded **104 imports**, **34 constructor entries** and **8 bytes of TLS**. This establishes build/structure evidence only; native execution of those imports and live media require the owner's PS4 result.

Package `XCloud4-0.7.0.pkg` is **8847360 bytes**, SHA-256 `80860ced1f18ef794fc628736864d5b4c894c9bea344759d4bbfcfdc84c6cf19`. Its upload and FTP retrieval matched that hash. No completed `v0.7.0` milestone or published 0.7 release is claimed.

The corresponding dependency-source archive is prepared locally as `XCloud4-0.7.0-dependency-sources.tar.gz`, **83532544 bytes**, SHA-256 `0e48073b43a94e68c109cf76ca08fdfb7c3e967f0908f2f72e732fb843624d68`. Its manifest covers **9367 files**, and each recorded source hash matched the archive. It includes all pinned submodules, Opus 1.5.2, the actual overlays/configuration, seven native streaming adapters and retained notices. Generated libraries/build caches, Git metadata and local credentials/logs are excluded. Original upstream public test fixtures are retained as source. See [0.7.0 development notes](RELEASE_NOTES_0.7.0.md).

### First 0.7.0 console result: local RTC creation failure

The owner installed/tried 0.7.0. Xbox preparation and authorization succeeded: `/connect` HTTP 202 and Provisioned state HTTP 200. Native entropy and NetCtl initialization returned zero. Local RTC opening then returned **-2 before SDP, ICE or RTP**. The remote session was deleted with HTTP 200. This is a local RTC creation failure; it does not establish a media connection or explain a server negotiation failure, and it is distinct from the earlier Passport permission refusal. Instrumentation and repair of that creation path are underway. Actual live video/audio remains unconfirmed.

## 0.7.1: local initialization diagnostics

Version 0.7.1 adds bounded initialization-stage labels and numeric native/Mbed TLS errors to narrow the local RTC failure. Diagnostic vendor changes touch `src/capi.cpp`, `src/impl/init.cpp` and `src/impl/tls.cpp`; native adapters report matching stage/error categories. Account credentials, SDP and ICE values are not logged. Claude Opus 5.5 completed its read-only review in 29 turns without an error, recommending an explicit pool bound before initialization.

The diagnostic package compiled and packaged successfully. `XCloud4-0.7.1.pkg` is **8847360 bytes**, SHA-256 `6769f0b3a93f87143a1d4511b7f0a3657e8a60cfe393c4a5704fd1c17fdefe7d`; its upload and FTP retrieval matched. Its actual console log reached thread-pool creation requesting **200112 workers**, then numeric **system error 1** before the pool-ready checkpoint. SDP, ICE and media were not reached. This identified the initialization stage; no live video/audio result or completed 0.7.1 milestone is claimed. The corresponding dependency source is captured separately from the immutable 0.7.0 snapshot. See [0.7.1 diagnostic notes](RELEASE_NOTES_0.7.1.md).

Source archive `XCloud4-0.7.1-dependency-sources.tar.gz` is **83536524 bytes**, SHA-256 `46501c565b2bf1782c8dbf971c13f474b32f4ccf76a86083f343207ecbdf9891`. All **9367** manifest source hashes matched. The captured libdatachannel modification list includes the three diagnostic files as well as earlier port changes. Native adapters and the updated reproducible patch script are included. The exporter selects the package version and refuses existing-output replacement, keeping 0.7.0 intact.

## 0.7.2: bounded RTC worker initialization

ABI inspection traced 0.7.1's count to the prebuilt SDK libc++ CPU-count probe calling `sysconf(84)`. Native FreeBSD selector 84 denotes `_SC_THREAD_CPUTIME`, whose `_POSIX_THREAD_CPUTIME` value is **200112L**; native CPU-count selectors are 57/58. Version 0.7.2 explicitly calls `rtcSetThreadPoolSize(4)` before peer-connection initialization and checks its result. Additional `src/impl/threadpool.cpp` diagnostics record how many workers were actually created, including the count at failure. The existing SDK thread-ID adapter is retained.

`XCloud4-0.7.2.pkg` compiled and packaged successfully: **8847360 bytes**, SHA-256 `f2d8cf6e606435927f98ddcd2ae274fed795787468e2bbe8bc7d2bfb9dfbc66a`. Its upload and FTP retrieval matched. Matching patched sources are exported separately, retaining previous immutable snapshots. See [0.7.2 notes](RELEASE_NOTES_0.7.2.md).

### Actual 0.7.2 console result

The console capture confirms `rtcSetThreadPoolSize(4)` returned zero and workers **1, 2, 3 and 4** were created. Thread-pool, PSA, SCTP, DTLS, SRTP and ICE library initialization completed with zero status. Peer-connection creation returned handle **1**, and local track/data-channel creation succeeded. The previous worker-pool initialization failure is resolved in this capture.

A new failure occurs at `rtcSetLocalDescription(pc, "offer")`: a runtime-error category and return **-2**, with no SDP callback captured at that point. No media was received. Successful library/object initialization does not establish ICE connectivity or a remote stream. Claude Opus 5.5 completed its read-only offer review in nine turns; queued callback delivery means its absence alone does not locate the internal failure stage. Bounded 0.7.3 diagnostics cover offer/ICE/certificate/network stages. See [the diagnostic package](RELEASE_NOTES_0.7.3.md).

The immutable 0.7.2 source snapshot is **83536843 bytes**, SHA-256 `3932e6593bf76eb26851c4db8ee0448d9b76e9f95a68163022e975558251c3c0`; all **9367** source hashes matched. A later patch-script idempotence check changes no compiled provider code and will be captured with the next stable source snapshot.

At the owner's request, twelve older PKG installer files (0.2.0 through 0.7.1) were removed from the console's `/data/pkg` directory. The subsequent listing confirmed only the 0.7.2 installer remains there. Development packages and corresponding sources remain preserved on the PC.

## 0.7.3: offer and native network diagnostics

The stable diagnostic changes add offer stages 20–26, ICE-agent results 30–33/35, certificate stages 40–45/48–49 and numeric UDP/socket/poll/resolver/interface results 50–63. Reproducible modifications cover libdatachannel peer-connection/ICE/certificate/TLS sources plus libjuice `agent.c`, `conn_poll.c` and `udp.c`. Native adapters report fixed labels and numeric values; the logger preserves `errno`. Account data, SDP, ICE values and certificate contents are not logged. The four-worker RTC policy is retained.

Claude Opus 5.5 completed a read-only offer review in **9 turns**, successfully, without changing code or demonstrating a fix. The review identified generalized initialization exception masking and ambiguity from queued certificate/callback/gathering work. The final dependency libraries and full application compiled successfully, then packaged as `XCloud4-0.7.3.pkg`: **8847360 bytes**, SHA-256 `3290f527aa67d94f17eb345cba055b3f96718e67f147dec807b9a8caa4f5172a`. Upload and FTP retrieval matched that hash.

### Actual 0.7.3 console result

The capture records certificate DER **362 bytes** and certificate readiness. ICE-agent creation and local description return zero; the offer receives media count **3** and is committed. Gathering begins, with poll-pipe/poll-thread and bind-address resolution returning zero. Setting UDP flags through `F_SETFL` fails with **errno 13 (`EACCES`)**, then connection -1, gathering -2 and local-description request -2. Remote cleanup returns **HTTP 200**. The owner reports the same error view and the capture contains no received RTP.

This isolates the operation that fails after successful certificate/local-offer preparation. It does not establish the exact cause of native denial or a working fix; ICE connectivity and live game video/audio remain unconfirmed. Matching sources were closed before further repair work as `XCloud4-0.7.3-dependency-sources.tar.gz`: **83543499 bytes**, SHA-256 `5c39a0ed1d50c39cc8bff12550f6e86ff7e6f13a55a4abab029e4ec086f857e9`, with all **9367** source hashes verified. Every earlier snapshot remains unchanged. See [0.7.3 notes](RELEASE_NOTES_0.7.3.md).

## 0.7.4: direct BSD nonblocking socket setup

The port restores libjuice's upstream UDP/TCP ioctl setup and defines the missing BSD `FIONBIO` name through existing SDK encoding `_IOW('f', 126, int)` (**0x8004667e**, four-byte integer). Numeric event 64 records success zero or actual socket errno; setup failure closes the socket and reports failure. Pipe flag handling remains in place. BSD source shows `F_SETFL` also requesting asynchronous mode; identifying that extra operation as the PS4 denial remains a hypothesis, while the observed `F_SETFL`/EACCES and verified constants are evidence.

Claude Opus 5.5 completed a read-only socket review in **5 turns**, successfully, without identifying a defect within that review. Static ELF/converted-OELF inspection confirms the request, pointer to integer one and native `_ioctl` import mapping. Final dependency and application compilation/package creation succeeded: `XCloud4-0.7.4.pkg`, **8847360 bytes**, SHA-256 `c36c1f0ad36827d8f102e893535446e9da0387d95f5cdc80e9ed6cf08f669955`. Its PS4 upload/FTP retrieval matched. The older 0.7.3 installer was removed after verification; the installer directory then contained only 0.7.4.

### Actual 0.7.4 console result

Certificate DER result is **359 bytes**, local-offer media count is **3**, and the offer is committed. Direct `FIONBIO` reports **errno 13 (`EACCES`)**, followed by connection -1, gathering -2 and local-description request -2. Remote cleanup returns **HTTP 200** and no RTP is received. The owner and console capture confirm the failed attempt. The proposed direct ioctl does not resolve the denial, and no precise native cause or successful game media is established. Native network nonblocking APIs and descriptor/API compatibility are the next investigation stage.

The immutable matching source snapshot is **83543344 bytes**, SHA-256 `6bf845b53bdeab0898b33d3d1e07876661c24324f7a4ad6a06c4bfa18d311c31`; all **9367** source hashes matched. Every earlier snapshot remains unchanged. See [0.7.4 notes and primary references](RELEASE_NOTES_0.7.4.md).

## 0.7.5: original native SO_NBIO adapter

The new original adapter uses POSIX `setsockopt` with PlayStation `SO_NBIO=0x1200` on the existing descriptor, then reads the option through `getsockopt` and requires enabled mode/four-byte size. Events 65–68 report numeric set/get errno, mode and size. Invalid results reject setup; UDP/TCP both use the adapter. The four-worker policy and pipe handling remain in place.

API research used the pinned Sony/WebKit PlayStation additions and WoWPS socket-control source listed in [0.7.5 notes](RELEASE_NOTES_0.7.5.md), without copying their implementations. WoWPS's description of an OpenOrbis limitation supports research; actual XCloud4 EACCES is established by the owner's console capture, not a verified SDK implementation or confirmed internal cause. The exact source provenance is retained in third-party notices.

Claude Opus 5.5 completed a read-only review in **5 turns**, successfully, using four grep inspections and identifying no concrete defect within that scope. Dependency and full application/package compilation succeeded in Lubuntu without warnings/errors: `XCloud4-0.7.5.pkg`, **8847360 bytes**, SHA-256 `077a7c2a04e8caad38cc57eb955a200c339c67116eb6bb4ef881cd4ec0eb97fb`. VM/PC/PS4 retrieval hashes match. Static ELF/OELF inspection confirms the option constants, four-byte arguments, same-descriptor query, returned-state checks and native libkernel set/get imports. The earlier installer was retired only after verification; the console installer directory then contained only 0.7.5.

### Actual 0.7.5 console result

Set/query errno values are **0**, returned mode is **256 (`0x100`)**, size is **4**, and socket nonblocking setup succeeds. Connection/interface query succeed with one interface and one host candidate. Gathering, offer readiness and `rtcSetLocalDescription` return zero. Xbox `POST /sdp` returns **HTTP 202**. The previous socket operation failure is resolved in this capture.

Repeated answer requests return **HTTP 204** without remote SDP until keepalive returns **HTTP 410**. The application reports that Xbox did not maintain the session; deletion succeeds with **HTTP 200**. The owner reports a new error view. Final counters show no completed ICE, video or audio, and no RTP is received. Successful local gathering and accepted SDP submission do not establish remote negotiation or media. The precise cause of the missing answer/session expiry remains under investigation.

The exact immutable source archive is **83543722 bytes**, SHA-256 `8efc459212d3bc99304442e3f98f1c970af3907fb225600933493001eb9d841b`. All **9367** recorded source hashes matched, and the PC copy matches the guest archive. It includes the final SO_NBIO adapter and matching libjuice changes; all earlier snapshots remain preserved. See [0.7.5 release evidence](RELEASE_NOTES_0.7.5.md).

## 0.7.6: structural SDP and HTTP diagnostics

This checkpoint adds an original bounded scanner for SDP shape and fixed allowlist HTTP error/timing summaries before clearing response data. Raw SDP, ICE values, credentials and arbitrary remote text are omitted. Negotiation and the 30000-ms keepalive interval remain unchanged. Claude Opus 5.5 completed a read-only protocol/SDP review in **7 turns / 6 reads**, without establishing a cause or correction. Its separate scanner review completed in **2 turns** and found setup/trickle labeling can leak into the session-level diagnostic category above eight media sections. The actual offer has three, so this diagnostic limitation does not affect its negotiation; the immutable checkpoint preserves it for a later repair.

The full application/package build succeeded without warnings/errors: `XCloud4-0.7.6.pkg`, **8912896 bytes**, SHA-256 `bfc2facb54c9c974851e2e33954deea44a952b7586511d0ac8db9155cde01e6d`. VM/PC/PS4 retrieval hashes match. The previous installer was retired after verification, leaving only 0.7.6 in the console installer directory.

### Actual 0.7.6 console result

Native nonblocking setup and gathering succeed. The callback header, reconstructed from fragments interleaved by concurrent logging, records **1093 bytes**, three media sections, zero candidates and three unique/matched BUNDLE entries. Individual media summaries show video index0/audio1/application2, each port9, H.264102 with four feedback entries and profile42E01F, Opus111 and SCTP5000 with actpass/trickle. This is a structural report, not a confirmed compatible offer.

Xbox accepts SDP submission with **HTTP 202**, then **33** pending answer polls return **HTTP 204**. Keepalive returns **HTTP 410**, **118 bytes**, with a valid JSON object and allowlist code **2 (`SessionNotActive`)**, no nested errorDetails/error. It occurs **30317 ms** after submission; the last keepalive attempt is logged at **29920 ms**, with the interval unchanged at **30000 ms**. Remote deletion returns **HTTP 200**. The owner reports the same error, and no remote SDP, completed ICE, RTP, video or audio is received.

Inspection identifies that submission retains the initial callback offer even after gathering; the callback summary contains zero candidates. The next checkpoint will fetch the actual current local description after gathering. This is a source finding, with no demonstrated server correction yet.

The matching source archive closed before that next edit: **83544715 bytes**, SHA-256 `14f8578e49d8bef7497b68b8803d6ed71c0fc5d720f72687d7ba05b515246f2d`. All **9367** source hashes matched, with identical guest/PC archive hashes. Pinned vendor revisions remain unchanged, and the exact scanner/native adapters/configuration/licenses are retained. Every earlier snapshot is preserved. See [0.7.6 release evidence](RELEASE_NOTES_0.7.6.md).

## 0.7.7: obtain the current local offer after gathering

The original transport now reads actual provider SDP through libdatachannel's C API after gathering completes, instead of submitting the initial callback snapshot. It checks offer type, lengths, caller capacity and NUL termination within the 32768-byte limit, preserving pending/error behavior and existing deadlines. No candidates are fabricated or merged, and no additional candidate prerequisite is imposed. The structural summary describes the current offer; the scanner's setup/trickle labeling above eight media sections is repaired. H.264 profile, media order and HTTP signaling remain unchanged.

The full application/package compiled without warnings/errors: `XCloud4-0.7.7.pkg`, **8912896 bytes**, SHA-256 `5a050c7ae06080c0a051e9dad7ca4550e0ed85feec0c3abec4aceb31c50307b3`. VM/PC/PS4 retrieval hashes match; retiring the previous installer after verification leaves only 0.7.7 in the console installer directory. Static peer review of the getter completed successfully. Claude Opus 5.5 completed a focused read-only review in **2 turns / 1 read**, without finding a proven defect within its current-data/bounds/type/NUL/locking/privacy scope.

The corresponding source archive closed from the exact frozen mirror: **83544519 bytes**, SHA-256 `33e30efb0c20cf45e976e950b8d320147f46bad60f4224bf685ce680c14949b8`. All **9367** manifest hashes matched, and the PC copy matches the guest archive. Every prior snapshot is preserved. See [0.7.7 notes](RELEASE_NOTES_0.7.7.md).

### Actual 0.7.7 console evidence

Gathering completes (enum **2**). The actual current SDP is **1201 bytes** with **one candidate** and three unique/matched BUNDLE entries; all media ports are **61345**. The media summaries retain H.264102/profile42E01F/four feedback entries, Opus111 and application SCTP5000. This confirms fresh provider state replaces the initial zero-candidate snapshot.

Xbox accepts SDP submission with **HTTP 202**, but **33** answer polls return **HTTP 204** without a remote answer. Keepalive returns **HTTP 410**, **118 bytes**, code **2 (`SessionNotActive`)**, at **30492 ms** after submission. Remote deletion returns **HTTP 200** and no RTP is received. The owner reports the same session error. OPTIONS closes resources and returns to the PS4 menu without `CE-34878-0`. The refreshed offer repairs the source-state issue, while remote negotiation and game media remain unconfirmed.

The private 0.7.7 draft checkpoint is recorded at target `44c684294d2c113bb67a52a3dfa758d87672d8c7`, with all three asset digests verified. That preserved build/source checkpoint is not a completed streaming milestone.

## 0.7.8: native DNS timeout units

Source review finds the original native resolver passes `2` as its timeout. The intended two-second bound is represented by **2000000 microseconds**; the patch corrects that argument and adds numeric-only return/timing diagnostics, with no hostname/address/ICE/credential output. Interface-unit research uses ioQuake3-PS4 pin `d4c7912af73c3b0eaa57195391ad76f21e79a910`, whose resolver call passes `3 * 1000 * 1000`. The reference implementation is not copied; XCloud4's adapter and chosen two-second limit remain original.

The same pinned project's technical DNS note explicitly labels the timeout in microseconds. Original adapter diagnostics 69–73 record creation result, timeout, lookup result, elapsed microseconds bounded to INT_MAX and a nonzero-address boolean; no actual address is exposed. The two source files are `rtc_net.c` and `rtc_native.c`, with no vendor changes.

Claude Opus 5.5 completed the focused read-only resolver review successfully in **3 turns / 2 reads**, finding no concrete defect within that scope. Actual resolver/negotiation evidence is recorded below separately from that review.

The full application/package compiled without warnings/errors. SDK `sceKernelGetProcessTime` return width is confirmed as `uint64_t`. Package `XCloud4-0.7.8.pkg` is **8912896 bytes**, SHA-256 `b562ab50eb0336b7d518bba8e8db74eb9a8af87ed6bd6c0ed3f0eaef14812664`; VM/PC/PS4 FTP retrieval hashes match.

### Actual 0.7.8 console result

Resolver creation returns **0**, the configured timeout is **2000000 microseconds**, lookup returns **0**, elapsed time is **11519 microseconds**, and the nonzero-address boolean is **1**. The current offer is **1286 bytes**, contains **two candidates**, and has three media sections/BUNDLE entries. Native DNS succeeds in this capture, without proving it was the sole cause of the earlier missing-answer failure.

Xbox accepts submission with **HTTP 202**, and **37** answer polls return **HTTP 204**. The next keepalive succeeds with **HTTP 200 / 37 bytes**. An SDP request then returns **HTTP 200 / 243 bytes**, but `signal_exchange` returns **-2** because `errorDetails` is non-null. This is a logical SDP refusal within a successful HTTP response; its exact server category remains unknown. The owner reports a new error. Remote deletion returns **HTTP 200**, with no RTP, video or audio received. The next investigation must identify the bounded logical-error category; no complete streaming milestone is claimed.

The exact source snapshot closed from the frozen final mirror, and the archive was copied to the PC: **83545142 bytes**, SHA-256 `9aac8351b1b93b0f4e619e81a3291f74567ccaf0dd4da516f0e61fecf654a0e0`. All **9367** manifest source hashes matched; PC/guest archive hashes are identical, and forbidden generated/Git/log/local-credential paths are absent. The two updated adapters are captured with unchanged pinned vendors, configuration, overlays and retained licenses. Every earlier snapshot remains preserved. See [0.7.8 scope, build and primary source](RELEASE_NOTES_0.7.8.md).

## 0.7.9: logical signaling failure diagnostics

The diagnostic checkpoint extends the original authentication's failure classification to successful HTTP responses containing logical SDP/ICE, acknowledgement or keepalive failures. Fixed allowlist/enumeration and numeric JSON/status/byte/timing summaries are captured before clearing private data. No arbitrary provider response text, raw SDP, addresses, ICE values or credentials are logged. Existing negotiation decisions and media behavior are unchanged.

The frozen change is confined to `src/auth/xbox_live.c`: four pre-wipe calls cover negative GET SDP/ICE exchange, failed POST acknowledgement and failed/ended keepalive. Existing non-2xx logging/classes0–14 remain. Bounded nodes0–4 describe root/errorDetails/error plus one nested details-object level, retaining JSON types, validated numeric uint32/signed32 code bits/sign, uint32 status and message/detail types only. The diagnostic adds no recursion/allocation or message-word inspection; business parsing, timing, retries, negotiation and cleanup remain unchanged.

This follows the actual 0.7.8 HTTP200/non-null errorDetails refusal; the exact server category remains unknown. Claude Opus 5.5 completed the focused read-only review successfully in **8 turns**, without a material finding in `xbox_live.c` lines1293–1700 and JSON source/header scope. Pre-wipe calls, bounds, privacy and unchanged flow were reviewed. Claude made no source edits and executed no tests.

The full application/package built without warnings/errors: `XCloud4-0.7.9.pkg`, **8912896 bytes**, SHA-256 `9a1fe23e6aa598ceb95ddfb8104510cc87d03aa56c8086c00e39e3327d80674c`. VM/PC/PS4 retrieval hashes match. The earlier 0.7.8 private draft checkpoint's three asset digests are verified at target `39f81a1`.

### Actual 0.7.9 console result

Native DNS succeeds (lookup **0**, elapsed **11086 microseconds**), with current SDP **1286 bytes / two candidates**. Xbox accepts submission with **HTTP 202**. After **37** pending answer polls return **HTTP 204**, keepalive succeeds with **HTTP 200 / 37 bytes**. The next SDP request returns **HTTP 200 / 243 bytes** at **30251 ms**, with the diagnostic poll count now **38**.

The diagnostic records SDP route **1**, errorDetails object(type **1**), code string(type **3**) classified **1 (unrecognized)**, and message string type **3**. No root-code/status/nested-details value is reported. No raw string is logged, so the exact refusal category is still unknown. Remote deletion returns **HTTP 200**, with no RTP, video or audio received. The diagnostic improves structural evidence without establishing a server correction or complete stream.

The application runtime checkpoint is `0e328bf`. Its authentication change is paired with an immutable dependency/native-adapter source archive: **83545064 bytes**, SHA-256 `b2ab42ea193254aa4aa71c891a3e53396850685418cc6c366783e3abfdd94235`. All **9367** manifest hashes matched; PC/guest hashes are identical, with no forbidden generated/Git/log/local-credential paths. Pinned vendors/RTC/configuration remain unchanged, original public upstream fixtures are retained, and every earlier snapshot is preserved. See [0.7.9 source and console evidence](RELEASE_NOTES_0.7.9.md).

## 0.7.10: restricted service error name

The original 26-line helper reads only the direct errorDetails object's string code and validates 1–64 ASCII characters: first a letter, then letters/digits/underscore. Only a valid identifier is printed locally. Invalid/absent/duplicated fields remain silent; the 65-byte temporary buffer is cleared on every path. Message/body/top-level/nested/other strings stay hidden. Previous numeric diagnostics/classes 0–14 and all parsing/negotiation/keepalive/retry/cleanup/media decisions remain unchanged.

Interface research uses pinned CloudNow and OpenXbox sources, without copying implementations; XCloud4's helper imposes narrower direct-field/ASCII/length bounds. No exhaustive server code list or timing-based refusal cause is established. Product 0.7.10 uses package APP_VER 00.80 after 00.79; the Spanish UI/product version remains separate from that field.

Claude Opus 5.5 completed the focused read-only helper/JSON review successfully in **3 turns / 2 reads**, without a material finding. Bounds, controls, duplicate fields, constant output format and temporary-buffer clearing were reviewed. The earlier broad attempt reached its turn limit without a result, and is not recorded as a successful review.

The full application/package built without warnings/errors: `XCloud4-0.7.10.pkg`, **8912896 bytes**, SHA-256 `c3a8e1cd1bf8d78c15f8c7bec9e73470afc0e1fe14053692357bd15fc5d82ffe`. VM/PC/PS4 retrieval hashes match. The 0.7.9 documentation checkpoint was committed/pushed at `032b715`; its logical refusal is the diagnostic target, with no game media received.

Application checkpoint `0fbe73a` supplies the authentication helper separately from the closed dependency/native-adapter source archive: **83558285 bytes**, SHA-256 `4dc79943434d23a05840665254769984d905e0e0e1bb9e4a186fa4ec03b39a77`. All **9367** manifest hashes matched; PC and guest archive hashes are identical, with no forbidden generated/Git/log/local-credential paths. Vendors, RTC adapters and configuration are unchanged, with corresponding notices updated. Every earlier snapshot stays immutable. See [0.7.10 package/source evidence and fixed references](RELEASE_NOTES_0.7.10.md).

### First 0.7.10 console attempt

Native DNS succeeds (lookup **0**, elapsed **12356 microseconds**, nonzero-address boolean **1**). Current SDP is **1285 bytes / two candidates**, with three media sections and three BUNDLE entries. Xbox accepts submission with **HTTP 202**, followed by **38** pending answer polls returning **HTTP 204**.

Keepalive is attempted at **30379 ms**, then returns **HTTP 410 / 118 bytes** at **30670 ms**, root-code class **2 (`SessionNotActive`)** and no errorDetails. The new restricted-name helper is **not entered**. Remote deletion returns **HTTP 200**, with no RTP, video or audio. This attempt does not capture the earlier HTTP200/243-byte logical SDP refusal or demonstrate a correction.

### Second 0.7.10 console attempt and queue limit

The owner repeats the same installed version. SDP is **1285 bytes / two candidates**, followed by **37** pending HTTP204 polls. Keepalive is attempted at **30187 ms**, then returns **HTTP410 / 118 bytes** at **30480 ms**, root-code class **2 (`SessionNotActive`)**, without errorDetails. The restricted-name helper is again not entered. Remote deletion returns **HTTP200**, with no RTP/video/audio. Both attempts identify an inactive session, while the earlier logical SDP refusal remains uncaptured; no correction is established.

Both attempts pass `ReadyToConnect`, `/connect` HTTP202 and `Provisioned` before SDP. Their failure occurs in the subsequent negotiation/polling stage. Source inspection confirms `WaitingForResources` is handled as waiting, with initial provisioning bounded to **180 seconds (three minutes)**. Extended queue waiting duration and UI need improvement; no server estimate is established by these captures. That bounded backlog item is [recorded separately](MEJORAS_FUTURAS.md).

The package and immutable source snapshots are unchanged.
