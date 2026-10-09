# XCloud4 0.7.9 — logical signaling failure diagnostics

This diagnostic checkpoint extends bounded classification to logical signaling failures carried by successful HTTP responses. The full application/package built without warnings/errors, and VM/PC/PS4 retrieval hashes match. **The console confirms an errorDetails object with an unrecognized string code; the exact refusal category remains unknown. Negotiation behavior is unchanged, and no RTP or game video/audio is received.**

## Previous confirmed evidence

In 0.7.8, native DNS succeeds and the current local offer contains **two candidates / 1286 bytes**. Xbox accepts submission with HTTP 202; after 37 pending answer polls and a successful keepalive, `GET /sdp` returns **HTTP 200 / 243 bytes** with non-null `errorDetails`, causing exchange result **-2**. Remote cleanup succeeds with HTTP 200; no RTP, video or audio is received. The exact server refusal category remains unknown. See [0.7.8 runtime evidence](RELEASE_NOTES_0.7.8.md).

## Diagnostic scope

Only the original `src/auth/xbox_live.c` diagnostic implementation changes. It logs before clearing private response data at four existing failure points: negative GET SDP exchange, negative GET ICE exchange, failed SDP/ICE POST acknowledgement, and failed/ended keepalive. Existing non-2xx logging and error-code allowlist classes **0–14** remain.

The additional node summary is bounded:

- Nodes **0/1/2**: root, `errorDetails`, and `error`.
- Nodes **3/4**: one nested `details` object level beneath the error objects.
- JSON node type and fixed error-code class.
- Code JSON type, validated numeric flag, normalized uint32/signed32 code bits and negative flag.
- Status JSON type, validated uint32 flag and numeric status.
- `details` and `message` JSON types only; message contents are neither printed nor classified by words.

The diagnostic uses no recursion or allocation. No arbitrary provider text, raw SDP, addresses, ICE values or credentials are logged.

The existing negotiation decisions, media configuration and playback behavior are unchanged. The purpose is to identify the logical refusal category observed in 0.7.8; diagnostics alone do not correct that refusal or demonstrate a working stream.

## Review and package evidence

Claude Opus 5.5 completed the focused read-only review successfully in **8 turns**, without a material finding in the reviewed diagnostic/JSON scope. It inspected `xbox_live.c` lines1293–1700 and `json.h`/`json.c`, including pre-wipe calls, bounds, privacy and unchanged control flow. Claude made no source edits and executed no tests.

- Package: `XCloud4-0.7.9.pkg`, identifier `XCLD00001`.
- Size: **8912896 bytes**.
- SHA-256: `9a1fe23e6aa598ceb95ddfb8104510cc87d03aa56c8086c00e39e3327d80674c`.
- VM, PC and PS4 FTP retrieval hashes match.

## Actual console result

Native DNS succeeds (lookup **0**, elapsed **11086 microseconds**). The current offer is **1286 bytes / two candidates**, and Xbox accepts submission with **HTTP 202**. After **37** answer polls return **HTTP 204**, keepalive succeeds with **HTTP 200 / 37 bytes**. The next SDP request returns **HTTP 200 / 243 bytes** at **30251 ms** after submission, bringing the diagnostic poll count to **38**.

The safe classification records route **1 (SDP)**, `errorDetails` JSON type **1 (object)**, code type **3 (string)** and code class **1 (unrecognized)**; message type is **3 (string)**. No root-code, status or nested-details value is reported. The string contents are not logged, so this evidence does not identify a specific server refusal or corrective setting. Remote deletion returns **HTTP 200**, with no RTP, video or audio received. The diagnostic identifies response structure while the exact refusal category and successful negotiation remain unresolved.

## Matching source

The application runtime checkpoint is `0e328bf`; its authentication source supplies these new diagnostics. The separate exact dependency/native-adapter source archive closed from the frozen mirror:

- Filename: `XCloud4-0.7.9-dependency-sources.tar.gz`.
- Size: **83545064 bytes**.
- SHA-256: `b2ab42ea193254aa4aa71c891a3e53396850685418cc6c366783e3abfdd94235`.
- All **9367** manifest hashes verified; PC and guest archive hashes match.

Both the exact application revision and dependency source/licenses must accompany any distributed package. Pinned vendors, RTC adapters and configuration are unchanged; generated artifacts/Git metadata/local credentials/logs are excluded, and original public upstream fixtures remain. Every earlier snapshot is immutable. Review/build/integrity evidence does not establish a complete stream.

No automated tests were added or run. The PS4 UI stays Spanish, repository documentation stays English and GitHub stays private. See [WebRTC status](WEBRTC_PS4.md) and [third-party notices](../THIRD_PARTY_NOTICES.md).
