# QUIC transport evaluation

Status: Evaluation only. No transport migration, no production listener change, and no discovery dependency on QUIC.
Last updated: 2026-11-16, because AI discovery must stay on the existing TCP/Levin path until a maintainer explicitly approves a separate transport experiment.
Decision: [DEC-0004](DECISIONS.md).
Audience: Conceal Core. conceal-desktop clones this tree and uses `NodeServer` unchanged. This note does not change that contract.

This document records what the current P2P stack couples to TCP, whether a small transport boundary could leave that path alone while allowing an optional QUIC transport later, and how a future proof of concept would have to be measured. It does not claim that QUIC is faster, slower, or safer in deployment. No benchmark was run.

## Decision

Keep blockchain sync and AI provider discovery on `NodeServer` (`src/P2p/NetNode.cpp`): TCP listener, Levin framing, existing command dispatch. Discovery is specified in [`docs/ai-provider-discovery.md`](ai-provider-discovery.md).

Do not add a transport interface to the daemon, do not replace `TcpListener`, and do not route discovery through QUIC.

A separate opt-in proof of concept is specified below. It is not approved and it is not started.

## What was inspected

Production path, which the daemon and conceal-desktop actually run:

- Application messages and KV serialization: `src/CryptoNoteProtocol/CryptoNoteProtocolDefinitions.h`, `src/P2p/P2pProtocolDefinitions.h`, `LevinProtocol::encode` / `decode`.
- Levin framing and command dispatch: `src/P2p/LevinProtocol.h`, `src/P2p/LevinProtocol.cpp`, `NodeServer::handleCommand`, `CryptoNoteProtocolHandler::handleCommand`.
- TCP connection handling: `src/Platform/*/System/TcpListener.*`, `TcpConnection.*`, `TcpConnector.*`.
- Connection lifecycle, queues, and the dispatcher: `P2pConnectionContext`, `NodeServer::acceptLoop`, `connectionHandler`, `writeHandler`, `timeoutLoop`, `timedSyncLoop`, `externalRelayNotifyToAll`.

Unused second stack, compiled into the `P2P` static library but not constructed by the daemon (`P2pNode::P2pNode` has no call site outside its own file):

- `src/P2p/P2pNode.cpp`, `P2pContext.cpp`, `P2pConnectionProxy.cpp`, `P2pInterfaces.h`.

Build and license constraints:

- C++11 (`CMAKE_CXX_STANDARD 11` in `CMakeLists.txt`). `cmake_minimum_required` is 3.5.
- Platforms in tree: Linux, macOS, Windows (MSVC and MinGW), with an Android branch in the top-level CMake flags.
- Project license is MIT (`LICENSE`). The P2P code does not link OpenSSL, BoringSSL, or any other TLS stack.
- Listener port is TCP `P2P_DEFAULT_PORT` (15000). UPnP mapping in `addPortMapping` requests protocol `"TCP"` only. Seed strings are `host:15000`.

## Current layering

The production split is real, and it is not a transport interface.

```text
CryptoNoteProtocolHandler
  command id + KV binary payload (ISerializer)
        |
NodeServer::handleCommand
  P2P commands 1001-1003 (handshake, timed sync, ping)
  otherwise payload handleCommand (blockchain notifies, ids 2001+)
        |
LevinProtocol
  33-byte bucket header + body, signature 0x0101010101012101
        |
platform_system::TcpConnection
  blocking read/write on one TCP socket, yielding inside Dispatcher
        |
TcpListener / TcpConnector
  one accepted or dialed TCP connection per peer session
```

Application messages are already codec-only. `NOTIFY_*` and `COMMAND_*` structs serialize through `ISerializer`. They do not mention sockets. `CryptoNoteProtocolHandler::handleCommand` switches on command id and decodes a `BinaryArray`. Blockchain commands in that switch are notifies. Request/response pairing for sync is the connection state machine (`stateSyncRequired`, `stateSynchronizing`, `statePoolSyncRequired`, `stateNormal` on `CryptoNoteConnectionContext`), not a transport stream id.

Levin is the framing and the only request/response bit on the wire. `bucket_head2` carries command, payload length, a return-data flag, flags (`REQUEST` / `RESPONSE`), and a return code. `LevinProtocol::invoke` writes a request and then reads the next bucket, and it accepts that bucket only when `isResponse` is set. That call is used for the outgoing handshake and for the short-lived TCP back-ping. The long-lived session does not use `invoke`. Its reader loop pulls one command, dispatches it, and optionally queues a reply.

TCP ownership sits in the connection object. `P2pConnectionContext` holds a `platform_system::TcpConnection` by value. `NodeServer` holds a `platform_system::TcpListener`. Outbound dials use `TcpConnector` with `connection_timeout` (5 seconds). Handshake uses `connection_timeout * 3`. Back-ping opens a second TCP connection to the advertised port and checks that `COMMAND_PING` returns the same `peer_id`.

Lifecycle is fiber-scoped. `acceptLoop` accepts, inserts into `m_connections`, and spawns `connectionHandler`. That handler spawns `writeHandler`, then reads forever. `pushMessage` appends to a per-connection `writeQueue` and wakes `queueEvent`. `externalRelayNotifyToAll` hops onto the dispatcher with `remoteSpawn` before it touches the queue, so other threads do not write the queue directly. `sendStopSignal` sets `m_stop` and, on the dispatcher, signals `m_stopEvent` and stops the payload handler. Per-connection shutdown sets `stopped`, signals the queue event, and interrupts the fiber.

`P2pNode` is not an available seam. `IP2pConnection::read` / `write` look message-oriented, but `P2pContext` still takes a `TcpConnection` and calls `LevinProtocol` on it. `P2pConnectionProxy` still special-cases handshake, ping, and timed sync. `cn::P2pMessage` in `P2pInterfaces.h` (command id + buffer) and `cn::P2pMessage` in `NetNode.h` (COMMAND / REPLY / NOTIFY plus return code) are different types with the same name. The two translation units do not include both headers. Treating `IP2pConnection` as the transport boundary would fork the live listener onto an unused stack.

## What is transport-coupled

Coupled to TCP and to the single ordered byte stream:

| Piece | Coupling |
| --- | --- |
| `TcpListener`, `TcpConnector`, `TcpConnection` | The socket API. `read` / `write` are sized byte copies. There is no datagram, peer-address-per-packet, or timer wheel. |
| `LevinProtocol::m_conn` | Hard-typed as `platform_system::TcpConnection&`. `readStrict` / `writeStrict` assume one ordered stream and treat a zero read as EOF. |
| `LevinProtocol::invoke` | The next bucket on that same stream must be the response. Any interleaved message fails the call. |
| `P2pConnectionContext::connection` | The session object is the TCP connection. |
| `connectionHandler` + `writeHandler` | One reader fiber and one writer fiber per TCP connection. The reader does not pull the next command until `handleCommand` returns. |
| Handshake and back-ping | Separate TCP connection attempts, TCP timeouts, and a TCP UPnP mapping. Peerlist admission for an incoming peer depends on a successful TCP ping to `my_port`. |
| Peer identity | `peer_id` is a stored 64-bit value. Reachability is "this TCP port answered ping with this id". `network_id` is a uuid compared in the handshake. There is no certificate, channel binding, or long-term key. |
| Write queue | Byte cap `P2P_CONNECTION_MAX_WRITE_BUFFER_SIZE` (64 MiB) on the TCP write buffer, then `interrupt()`. |
| Timed sync | Every `P2P_DEFAULT_HANDSHAKE_INTERVAL` (60 s), a `COMMAND_TIMED_SYNC` request is pushed onto each live connection's write queue. |
| Dispatcher | Cooperative contexts over epoll / IOCP / kqueue. A handler that verifies a block runs before that connection reads again, and it shares the thread with every other connection. |

Not coupled to TCP:

- KV layout of handshake, timed sync, ping, and `NOTIFY_*` payloads.
- Command numbers (`P2P_COMMANDS_POOL_BASE` 1000, `BC_COMMANDS_POOL_BASE` 2000).
- `CryptoNoteProtocolHandler` once it has a command id and a buffer.
- Peerlist, `.p2pstate`, and `IP2pEndpoint` as used by the protocol handler (`relayNotifyToAll`, `invokeNotifyToPeer`, `dropConnection`). Those APIs identify a connection by id. They do not name TCP. Their only implementation still does.

Two limits are easy to misuse in a new transport:

- `network_config::packet_max_size` is set to `P2P_DEFAULT_PACKET_MAX_SIZE` (50,000,000) and the comment next to the assignment says "20 MB". Nothing in the read path consults that field.
- `LevinProtocol::readCommand` rejects a body larger than `LEVIN_DEFAULT_MAX_PACKET_SIZE` (100,000,000) and then allocates the body. The configured 50 MB value is not the cap that runs.

`NodeServer::acceptLoop` has no application-level incoming-connection cap. Outgoing fan-out targets `P2P_DEFAULT_CONNECTIONS_COUNT` (8), plus anchor peers. A write that stays in `write()` longer than `P2P_DEFAULT_INVOKE_TIMEOUT` (120 s) is interrupted by `timeoutLoop`.

## Can a small boundary serve both transports?

A byte-pipe under Levin is a small seam. An independent-session transport is not.

The smallest seam that leaves framing alone is to change `LevinProtocol` so `readStrict` / `writeStrict` call something with `read(uint8_t*, size_t)` and `write(const uint8_t*, size_t)`, and to keep a `TcpConnection` adapter as the only production implementation. Handshake bytes, command ids, and `CryptoNoteProtocolHandler` stay as they are. One QUIC bidirectional stream can present that same pipe, so a future experiment can carry today's buckets without a new codec.

That seam does not provide independent sessions. One stream is still one ordered pipe. `invoke` still requires that the next bucket is the response. The reader still handles one command to completion. Bulk `NOTIFY_RESPONSE_GET_OBJECTS` traffic and a small control or discovery message still share that order. QUIC would not change those facts until the read loop, the write queue, and reply matching become per session.

Independent sessions already have a precedent that is not QUIC: back-ping dials a second TCP connection, runs one Levin invoke, and closes it. A discovery session can be another TCP connection, or another command range on the existing connection, using the same codec. Either choice stays inside the current transport. A second TCP connection isolates discovery from a bulk sync write. A new command id on the existing connection does not.

What a QUIC-shaped boundary would have to add, beyond the byte pipe:

- One UDP socket demultiplexing many connections, each with connection ids.
- A timer source for loss detection, probe timeout, idle timeout, and ACK.
- Per-stream read and write fibers, or an explicit rule that blockchain traffic stays on stream 0 and nothing else shares it.
- Reply matching that does not use "the next bucket on the only stream", if more than one request can be outstanding.
- Peerlist and back-ping policy that still means "TCP port accepts a ping". QUIC path validation does not prove the TCP listener.
- A way to wake Dispatcher fibers from a sans-I/O QUIC engine, or a dedicated thread whose only job is the UDP socket. The engine's callbacks must hop onto the dispatcher before they touch `m_connections` or the core.

Doing that inside `NodeServer` is a networking rewrite. Doing it only in a separate binary is the experiment in this note.

Recommendation: do not introduce the byte-pipe in production code now. The production type is used in a handful of call sites, but the justification for touching them is a measured concurrency problem, and that measurement does not exist. Discovery does not need the seam.

## Control messages and sessions on streams

If a later experiment is approved, the mapping that preserves today's wire format is:

| Session | Today | QUIC experiment mapping |
| --- | --- | --- |
| Handshake, timed sync, ping-on-session, future discovery commands | Buckets on the long-lived TCP connection | One long-lived bidirectional stream. Same buckets. |
| Bulk sync (`NOTIFY_REQUEST_GET_OBJECTS` / `NOTIFY_RESPONSE_GET_OBJECTS`, chain entry) | Same TCP connection, same write queue | A separate bidirectional stream, still Levin buckets, opened for that transfer and closed after it. Not stream 0. |
| Back-ping admission | New TCP connection to the advertised port | Leave this on TCP. Do not treat a QUIC path validation as a substitute. |
| AI discovery | Not implemented | Same TCP/Levin session as the rest of P2P, new command ids above the blockchain pool. Not a QUIC stream. |
| Short `invoke` (handshake, ping) | Exclusive use of a connection until the next bucket arrives | Only on a stream where nothing else is read or written until the response arrives. |

Unidirectional streams are a poor fit for handshake and timed sync, because those commands have replies. Datagrams are a poor fit for Levin, because a bucket can be tens of megabytes and datagrams are unreliable and size-limited.

Head-of-line blocking in this codebase is two different mechanisms. The byte-stream mechanism is the shared write queue and the single TCP send. The application mechanism is `connectionHandler` running `handleCommand` to completion on the dispatcher before the next read. Multi-stream QUIC can separate the first. It does not separate the second while block verification stays inline on that thread. A benchmark that only adds QUIC under the same handler will mix the two and cannot support a "QUIC is faster" claim.

0-RTT and session resumption are out of scope for the experiment until each discovery and sync command is classified for replay. Default the experiment to 1-RTT, no early data.

## Libraries

The daemon standard is C++11. Libraries that require C++17 or C++20, or that pull Folly or the Chromium tree, are not candidates for this repository. HTTP/3 is the wrong layer: Conceal messages are Levin buckets, not HTTP.

Compatible with linking from an MIT C++11 program means the library is callable without raising `CMAKE_CXX_STANDARD`, and its license can be shipped with this MIT tree (MIT, BSD, Apache-2.0 with notices preserved). GPL-only TLS stacks are not candidates.

| Library | Fits a later PoC? | Why |
| --- | --- | --- |
| ngtcp2 | Preferred engine for a sans-I/O PoC | C, MIT, RFC 9000. The core library does not link TLS. The application owns the UDP socket, which is what a Dispatcher adapter would need. Crypto helpers exist for several TLS stacks. Upstream documents the OpenSSL helper as experimental, and the OpenSSL QUIC TLS API has lifetime rules (`ngtcp2_conn` must outlive `SSL`, or `SSL_set_app_data(ssl, NULL)` before `SSL_free`). Example clients in the ngtcp2 tree are C++23; the PoC must not build those. It would use the C API from a C++11 driver. |
| MsQuic 2.6.x | Strong alternative if the experiment wants a maintained engine that owns I/O | C API, MIT, RFC 9000, Windows / Linux / macOS packages, Rust and C# interop layers. Current NuGet line at the time of writing is 2.6.4 (11 Nov 2026). It wants the UDP socket and its own execution model, which fights the single Dispatcher. MinGW and the in-tree Android flag were not built here. Use the C API, not a C++ wrapper, so the standard stays at C++11. |
| Cloudflare quiche | Usable from C, poor fit for the daemon build | BSD-2-Clause, sans-I/O, C API over Rust. Upstream README at the time of writing requires Rust 1.88 or newer and builds BoringSSL with CMake (NASM on Windows). That is acceptable in a standalone PoC and a bad dependency for `conceal-daemon`, whose CI is CMake/C++. |
| quinn | Rust client side of an interop test | The natural crate for a future concealed-ai client. MIT or Apache-2.0. Interop with ngtcp2 or MsQuic is a QUIC v1 question, not a reason to put quinn in the daemon. |
| LSQUIC | Possible, not first | C, MIT, CMake, BoringSSL, zlib. Maintained (4.10.0 docs dated Sep 2026). Defaults include QUIC v1 and v2. Older Google QUIC versions still exist and must stay disabled. Extra surface this experiment does not need. |
| picoquic | Research-quality C alternative | C and BSD-style, sans-I/O, picotls. Smaller maintainer base than ngtcp2 or MsQuic. Fine as a cross-check, not the library to center the PoC on. |
| OpenSSL 3.5+ QUIC API | TLS backend, not the application transport | Server-side QUIC arrived in OpenSSL 3.5 (8 Apr 2025, LTS). The socket-owning server API is a different shape from `TcpConnection` and from a multi-stream Levin mapper. Apache-2.0 can be linked from this MIT tree if notices are kept. Prefer OpenSSL as the TLS backend behind ngtcp2 only if the experimental helper's lifetime rules are wrapped and tested. |
| wolfSSL QUIC | Do not use | Typical license is GPL or commercial. GPL is not acceptable for this MIT tree. |
| mvfst, Chromium QUICHE, ngtcp2's C++23 examples | Do not use | C++ version and dependency weight. |

No library here removes the need for a new TLS dependency. The P2P library has none today. BoringSSL is unversioned and awkward to package. aws-lc is a maintained BoringSSL-derived alternative and is one of ngtcp2's tested backends. Either choice is a supply-chain and packaging change (Linux packages, macOS, MSVC static runtime, MinGW). That change belongs to the PoC's own CMake, not to the top-level `CMakeLists.txt`, until a maintainer accepts it.

CMake 3.5 is the project's declared minimum. ngtcp2 and BoringSSL builds in the wild often want a newer CMake. The PoC may raise its own minimum. The daemon must not.

## Authentication and service identity

Today's authentication is a network uuid plus a self-assigned `peer_id`, checked in `NodeServer::handshake` and `handleHandshake`. Mismatched `network_id` or a version below `P2P_MINIMUM_VERSION` closes the connection. A `peer_id` equal to the local id is treated as a self-connection. The optional `proof_of_trust` path is behind `ALLOW_DEBUG_COMMANDS` and is not the production handshake.

QUIC always authenticates a TLS 1.3 handshake. That identity is not `peer_id` and it is not `network_id`. A self-signed certificate with an unchecked subject proves only that the peer who finished the handshake holds that key. It does not prove the peerlist entry.

For any future experiment, binding has to be explicit:

- Pin the certificate or the SubjectPublicKeyInfo hash next to the legacy `peer_id` inside the Levin handshake, and check it against the peer certificate after the TLS handshake.
- Keep `network_id` as the network discriminator. Do not overload the certificate common name for it.
- Use ALPN to separate services. A blockchain experiment token and an AI token must differ. Completing QUIC with the blockchain token must not authorize AI operations.
- Discovery implemented on TCP must not read ALPN, certificates, or a QUIC transport flag. Its service identity is the existing `network_id` plus a new command id range.

Comparable security for a benchmark is not "plaintext TCP versus QUIC". Production TCP is unencrypted and does not authenticate a key. QUIC pays for a TLS 1.3 handshake, encryption, and ACK machinery. The report has to include two baselines: production TCP as it is, and TCP plus TLS 1.3 with the same certificate style and no 0-RTT, against QUIC with the same certificate style and no 0-RTT. A latency or CPU difference that appears only against plaintext TCP is not evidence that QUIC is the better transport.

## UDP reachability, fallback, and downgrade

The live network is TCP 15000. UPnP maps TCP. Seeds publish TCP ports. Many NATs and firewalls that pass that TCP port drop UDP, or drop it after a short idle time. QUIC on UDP 15000 is a different reachability domain from the listener operators already open.

Fallback, if an experiment ever dials QUIC first:

- Budget a short connect timeout, then dial the existing TCP path. Failure to receive a QUIC response is not proof of a dead peer.
- Use the TCP path for handshake, peerlist, and sync with today's rules. Do not mark the peer QUIC-capable in a way that later skips TCP.
- Do not let a peer's advertised preference disable TCP. A network that required QUIC would partition nodes behind UDP filters and would change seed and desktop assumptions.
- Keep back-ping on TCP. A node that answers QUIC and does not answer TCP ping stays out of the white list under current admission rules.

Downgrade protection:

- UDP blocking, packet loss, and a hostile injector can all force the TCP fallback. The fallback has the security of today's TCP, which is the production baseline, not a broken QUIC.
- Operations that required the TLS binding (a future AI session that pinned a key) must not run on the fallen-back TCP connection.
- Discovery must not gain or lose privilege based on which transport answered. It stays on TCP, so there is no downgrade edge for it.
- Version and ALPN negotiation must ignore unknown tokens. A QUIC-only flag inside a TCP handshake must not cause the node to drop the TCP peer.

NAT and reconnect, to be measured rather than assumed:

- Idle timeouts on UDP NATs are often shorter than the 60 s timed-sync interval. The experiment needs its own keepalive and an idle timeout, and it needs to record connection loss when keepalive is at 60 s versus a shorter interval.
- QUIC connection migration is not useful while identity is `peer_id` plus IP and port. Do not design peerlist entries around connection ids.
- Reconnect is a new handshake. Reusing 0-RTT across reconnect is disabled until replay is reviewed.
- TCP reconnect behavior stays the current one: dial, handshake, process payload sync, replace the session state.

## Queues, stream limits, exhaustion, and shutdown

Limits that exist and must remain on the TCP path:

- Write queue: 64 MiB per connection, then interrupt.
- Levin body: allocate up to 100 MB per command. The 50 MB config field is not enforced.
- Invoke / stuck write: 120 s, then interrupt.
- Outgoing target: 8 connections. Incoming: no application cap.
- One command is parsed and handled at a time per connection. Memory still spikes by the size of that body.

Limits a QUIC experiment has to set explicitly, because the UDP peer can otherwise pin server memory before a Levin header is validated:

- Handshake timeout and, unless the PoC is lab-only on a trusted LAN, a retry or equivalent so a spoofed source cannot amplify.
- Caps on concurrent connections, bidirectional streams, and unidirectional streams. Unidirectional streams can be zero.
- Connection and stream flow-control windows no larger than the Levin body cap the experiment actually accepts. Prefer the 50 MB config value as the experiment's cap and log that production TCP still uses the 100 MB constant.
- Per-stream write queues with the same 64 MiB failure behavior, plus a connection-wide budget so many streams cannot multiply it.
- A cap on buffered UDP datagrams before the engine runs.
- Idle timeout and a draining period after close.

Shutdown today interrupts the fiber and drops the TCP connection. The write loop exits on an empty queue once `stopped` is set, or earlier if the interrupt aborts the blocked `write`. QUIC needs a `CONNECTION_CLOSE` and a drain, or the peer retransmits until its idle timer. The experiment should record time-to-quiet and resident memory after close, including close during a large stream.

The shared dispatcher is part of exhaustion behavior. A handler that takes a long time inside `handle_notify_new_block` stalls every connection's reads and the accept loop. QUIC windows do not fix that. The benchmark should record dispatcher delay, or an equivalent "handler occupied" time, next to transport latency.

## Interoperability with a future Rust client

concealed-ai is not in this repository. Nothing here was checked against its source. Interoperability, when a maintainer wants it, is specified as:

- QUIC v1 (RFC 9000) and TLS 1.3. No Google QUIC, no draft versions, no QUIC v2 unless both sides record it.
- A named ALPN for the experiment, different from any later AI ALPN.
- Levin buckets as the stream payload, so the Rust client can implement the header without linking this C++ object.
- No HTTP/3 and no QPACK.
- Client side: quinn, or quiche's Rust API. Server side of the PoC: ngtcp2 or MsQuic. The test passes when a quinn client completes the handshake, opens the control stream, exchanges a handshake bucket and a small notify, and transfers a large body on a second stream while the control stream still returns a timed-sync response.
- Certificate check as in the binding section. `peer_id` alone is not enough once TLS is on.
- 0-RTT off on both sides for the first interop run.

Discovery does not use this client path. A Rust client that only speaks QUIC cannot perform discovery until a maintainer approves that transport. Until then, a Rust client that needs discovery uses the TCP Levin port.

## Is a transport abstraction justified now?

No.

The live code already separates payload serialization from Levin, and Levin from the protocol handler. The remaining coupling is the socket type inside Levin and inside `P2pConnectionContext`, plus the assumption that one connection is one ordered session. That is a small edit, and it is also an edit with nothing in production to sit on the other side of the interface. An interface with one implementation is a rewrite risk without a measured benefit.

`P2pNode` shows the cost of a second stack that never became the listener. Extending it into a transport abstraction would duplicate handshake, peerlist, ping, and sync behavior that `NodeServer` already owns.

Discovery can use a new command range on the current session, or a dedicated TCP connection modeled on back-ping. Neither choice depends on QUIC or on a new interface.

Revisit the byte-pipe only after the proof of concept below has numbers, and only if those numbers show a control-latency or loss problem that a second TCP connection does not remove. Revisit a multi-stream session API only after that, and keep it out of the blockchain read loop until sync on TCP is unchanged in a side-by-side run.

## Proof of concept and benchmark

Status: proposed, not approved, not started. It must be a separate opt-in target. It must not be linked into `conceal-daemon`, must not replace `TcpListener`, and must not change default peer dialing.

Purpose: compare the current transport with QUIC under comparable security conditions, and observe fallback. The purpose is not to ship a second listener.

Layout, when approved:

- A small C++11 driver, its own CMake target, off by default.
- Engine: ngtcp2 C API, with one pinned TLS backend. MsQuic is the alternate build, not a second framing.
- A TCP driver that uses the same Levin header layout and the same payload sizes. One build is plaintext TCP, matching production. One build is TCP plus TLS 1.3, matching the QUIC certificate and cipher policy.
- Optional quinn client binary, built only in the interop job, speaking the same ALPN and buckets.

Workloads, same sizes on every transport:

- Healthy network: one control stream exchanging small buckets (handshake-sized and timed-sync-sized) while N bulk streams or, on TCP, one bulk sender, move large bodies. Use 64 KiB, 1 MiB, and a body near the experiment's cap.
- Impaired network: fixed delay, and packet loss at a few controlled rates, applied to both sides with the same emulator settings. Record the emulator settings in the result file.
- Concurrency: small control requests during the bulk transfer. Report control-message latency separately from bulk throughput.
- UDP blocked: QUIC connect fails, harness falls back to TCP, and the TCP session still completes the small control exchange. Record time-to-fallback and whether any QUIC state remains.
- Reconnect: drop the path, dial again, complete a new handshake. 0-RTT stays disabled.
- Shutdown: close during a bulk transfer and report time until bytes stop and memory settles.

Metrics, per condition, with sample count and machine description:

- Median and a high tail (at least the 99th percentile) for control-message round trip.
- Bulk throughput.
- Process CPU and resident memory, including peak during the large body and after shutdown.
- Handshake time, separately from steady-state latency.
- Fallback count and false fallback under loss (loss should not be reported as "UDP blocked" unless the harness can tell them apart; if it cannot, say so).

Blockchain synchronization:

- Do not point this harness at the live `NodeServer` listener and do not change sync code to feed it.
- Baseline the daemon separately, on TCP only: time to apply a fixed, local block range with no extra load, then the same range while a test peer fills the write queue with large notifies. That shows today's head-of-line behavior. It is not a QUIC result.
- A QUIC result may be compared with that baseline only by payload size and concurrency, and the report must say the sync loop itself stayed on TCP.
- An in-process QUIC listener is a later proposal. It is not part of this PoC.

Pass bar for even discussing a production seam:

- The harness builds as an opt-in target on the platforms the maintainer names (at least Linux; Windows and macOS before any talk of packaging).
- QUIC and TCP+TLS both complete the interop script with quinn, or the report explains the mismatch.
- Control-message tail latency and memory are reported under loss. A throughput win with a worse tail or an unbounded memory curve is not a pass.
- Fallback completes when UDP is dropped, and the TCP result matches the plaintext baseline's functional checks.
- No run is summarized as "QUIC is faster" without the table above. If the table is missing a cell, the cell stays unknown.

## Uncertainties

- No timing, CPU, or memory numbers were collected for this note.
- concealed-ai's repository, crate choices, and ALPN plans were not available here. The interop section is a contract for a future test, not a statement that the client already agrees.
- ngtcp2's OpenSSL crypto helper is experimental upstream. Whether OpenSSL 3.5 LTS or aws-lc is the less costly backend for Conceal packaging was not decided.
- MsQuic on MinGW, and both libraries under the Android CMake flags, were not built.
- Real UDP reachability to port 15000 for current operators is unknown. UPnP as written will not map it.
- How often a 60 s keepalive survives subscriber NAT is unknown.
- The share of control latency that comes from the TCP stream versus from inline handlers on the single dispatcher is unknown. The benchmark has to split them before a transport change can be justified.
- A second TCP connection for discovery may remove the practical problem without QUIC. That alternative was not measured either.
- `packet_max_size` (50 MB), the comment ("20 MB"), and `LEVIN_DEFAULT_MAX_PACKET_SIZE` (100 MB) disagree. Any experiment that copies the constant without noticing will not match production, and production does not match its own config field.

## What proceeds without a transport experiment

Last updated: 2026-11-16, because discovery now has a TCP/Levin implementation. The proof of concept in this document is still not approved and not started. QUIC performance is still unmeasured.

Discovery runs on `NodeServer` and Levin. See [`docs/ai-provider-discovery.md`](ai-provider-discovery.md). Command ids 3001–3003 stay inside the existing encode, dispatch, write queue, and TCP listener. They do not add a listener, a transport interface, or a QUIC dependency. A peer that never speaks QUIC is every current peer, and discovery has to keep working beside that peer when the feature is enabled.
