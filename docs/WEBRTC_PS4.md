# Native WebRTC transport on PS4

## Existing application references

GreenVita is the main Xbox protocol reference: session creation, readiness, connection authorization, SDP, ICE, keepalive and closure. Better xCloud supplies settings/region research. Moonlight PS4 supplies native API research; its Sunshine transport does not itself connect to Xbox.

XCloud4's original C implementation of Microsoft, RPS, XSTS, cloud credentials and catalog was confirmed on PS4 in 0.4.0. Session preparation/deletion followed in 0.5.0 and Passport plus `/connect` in 0.6.2. Reusing protocol research retains that work while adapting console operations to OpenOrbis. GreenVita's Rust and Moonlight PS4's implementations have not been copied.

**The confirmed 0.6.2 milestone does not receive game video/audio.** Native WebRTC and actual media reception are the current development stage.

## Libraries reviewed on October 8, 2026

| Library | Pinned revision | Observed components | Status at the confirmed baseline |
|---|---|---|---|
| libdatachannel | `bdc5ff28e9d3b863144c94a677ecf5bf043aaf15` | C++17, C API, libjuice ICE, DTLS, SRTP, SCTP; optional Mbed TLS | Configured for OpenOrbis; first build stopped on incompatible headers; not included in 0.6.2 |
| libpeer | `5b849de378545c31d34759a145413846953e1366` | C, BSD sockets, Mbed TLS, libsrtp, usrsctp, cJSON | Alternative requiring comparison against Xbox media and channel requirements |

Primary sources: [libdatachannel](https://github.com/paullouisageneau/libdatachannel/tree/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15), [libpeer](https://github.com/sepfy/libpeer/tree/5b849de378545c31d34759a145413846953e1366). Observed licenses are MPL-2.0 and MIT, respectively. Dependencies retain separate licenses and must be pinned/attributed before distribution.

## Local toolchain evidence

The Lubuntu OpenOrbis v0.5.4 installation contains `include/c++/v1`, `libc++.a`, `libc++abi.a`, `pthread.h`, `poll.h`, `sys/socket.h` and import library `libScePosix.so`. Symbol inspection finds `pthread_create`, `pthread_join`, `socket`, `sendto`, `recvfrom`, `select` and `clock_gettime` in Posix; `poll` also appears in libkernel.

Available declarations/symbols support porting research. They do not establish that calls, ABI structures or WebRTC libraries work on firmware 12.00. No transport runtime result is claimed from this inspection.

## Initial external build work

Mbed TLS **3.6.7**, revision `068ff080b369adfac81509f9b57b2afabaf82dc5`, built static `libmbedcrypto.a`, `libmbedx509.a` and `libmbedtls.a` in Lubuntu for OpenOrbis. External source and archives stay outside Git/PKG at the confirmed 0.6.2 baseline. `scripts/webrtc/openorbis.cmake` and `mbedtls-user-config.h` record configuration. No Mbed TLS programs or tests were run.

The profile enables DTLS-SRTP, disables Mbed TLS's own network/file layers and requires `mbedtls_hardware_poll` for native cryptographic entropy. Initial configuration did not yet include that adapter. A fixed seed or non-cryptographic substitute is not acceptable.

libdatachannel configuration with Mbed TLS succeeded. Its first `datachannel-static` build stopped on missing BSD `u_int`/`u_long` types in usrsctp and missing `pthread_np.h` in libjuice. Before console use, the port must also review SDK `sockaddr_storage`, DNS/interface discovery, thread ABI and libjuice entropy. The current stage addresses those items; successful archive compilation must be distinguished from a working native session.

Pinned submodules from that review:

- libjuice: `b89c792e3612faf2f12cf35bcc56857313a06be3`.
- libsrtp: `d33b8ffb1491a0b4b58a206889f09800cf7310ab`.
- usrsctp: `fec583d54493f879d2ae44a743423bf8a04371ab`.

Each retains its own license; distribution requires applicable notices and modification/source references. Primary sources: [Mbed TLS 3.6.7](https://github.com/Mbed-TLS/mbedtls/tree/068ff080b369adfac81509f9b57b2afabaf82dc5) and [libdatachannel build documentation](https://github.com/paullouisageneau/libdatachannel/blob/bdc5ff28e9d3b863144c94a677ecf5bf043aaf15/BUILDING.md).

## Implementation requirements

1. Pin dependencies and configure static OpenOrbis builds with examples/tests disabled.
2. Review sockets, threads, timing, DNS and cryptographic entropy ABI. Resolve missing declarations/exports while retaining bounds and cancellation.
3. Generate a real SDP offer and DTLS fingerprint on PS4 and exchange them with Xbox using the researched session protocol.
4. Receive SRTP, reconstruct H.264 frames for native decoding, decode Opus and feed AudioOut.
5. Implement Xbox control/input channels and connect DualShock 4 state.

Session preparation or accepted `/connect` is not evidence of functioning WebRTC or a complete game.

## Protocol sources

- [Session creation/settings](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/api.rs).
- [State, connection, SDP, ICE and closure](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api_xbox/stream.rs).
- [GreenVita RTC session](https://github.com/Day-OS/green-vita/blob/ae2625d295b4fba005a769b1309fd70dcd6cb63f/src/api/streaming/rtc/session.rs).
