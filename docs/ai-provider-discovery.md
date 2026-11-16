# AI provider discovery

Status: First implementation, disabled by default. Proposed as [DEC-0005](DECISIONS.md).
Last updated: 2026-11-16, because discovery can be enabled from the command line or from `enable-ai-discovery=TRUE` in the daemon config file.
Audience: Conceal Core, conceal-ai-miner, and concealed-ai.

This is discovery only. It does not negotiate contracts, move prompts, stream inference, schedule GPUs, or talk to llama.cpp. It does not open a second listener and it does not use QUIC. [DEC-0004](DECISIONS.md) still holds: discovery stays on production TCP/Levin, QUIC performance is unmeasured, and the transport proof of concept is proposed, not approved, and not started.

## Component boundaries

| Component | Role |
| --- | --- |
| conceal-ai-miner | Holds a dedicated provider service key, signs advertisements, and registers them with a local daemon. Mining pause does not remove a still-valid advertisement. |
| conceal-daemon | Validates, caches, and relays advertisements. Answers bounded directory queries. Does not run models and does not store wallet spend keys. |
| concealed-ai | Asks a daemon for providers. Later contract work is out of scope. |

The daemon cache is not a complete global directory. `directoryComplete` is always false. Discovery does not prove that a provider is reachable, honest, or able to serve the advertised capacity, and it does not reserve a slot.

## Enablement

Discovery stays off unless one of these is set:

- Command line: `--enable-ai-discovery`
- Daemon config file (`--config-file`, default `conceal.conf`): `enable-ai-discovery=TRUE`

`TRUE`, `true`, and `1` enable it. `false` and `0` leave it off. The command line wins when both are present. The file is `name=value` lines. A JSON object is not read, including a file that happens to be named `config.json`. Point `--config-file` at that file only when it contains the line above.

With discovery off:

- `/registeraiprovider` and `/getaiproviders` return HTTP 404.
- The daemon does not probe peers, does not sync a directory, and does not admit advertisements.
- An incoming capability request is still answered, with `discoverySupported` false, so a probing peer can stop.
- An incoming directory request is answered with result `unsupported`.

`/getinfo` may include `ai_discovery_enabled` and `ai_provider_count`. The count is the number of non-offline, unexpired local records. The directory itself is not embedded in `/getinfo`.

## Advertisement

Signed fields:

| Field | Meaning |
| --- | --- |
| `protocolVersion` | `1` |
| `networkId` | 16-byte network id. Mainnet is `CRYPTONOTE_NETWORK`. |
| `providerPublicKey` | 32-byte provider service key. Not a wallet spend key. |
| `sequence` | Monotonic revision. The miner must persist this counter across its own restarts. |
| `issuedAt`, `expiresAt` | Unix seconds. Lifetime at most 1800 seconds. Issued time may be at most 120 seconds ahead of the daemon clock. |
| `status` | `1` available, `2` busy, `3` offline. |
| `availableSlots` | Integer, at most 1024. |
| `models` | At most 8 offers. Empty only when status is offline. |
| `contactType`, `contactValue` | `0` none, or `1` endpoint plus an opaque string of at most 128 bytes. |
| `signature` | CryptoNote signature over the canonical hash. Not part of the signed bytes. |

Each model offer is `modelId` (1–64 bytes), `maxContextTokens`, `priceAtomicUnits` (integer CCX atomic units), and `billingUnit` (`1` per request, `2` per thousand tokens). Model ids in one advertisement must be strictly increasing. The daemon never fetches a contact string and never opens a connection because an advertisement named one.

An offline record is a signed withdrawal. It is hidden from the default RPC listing and kept until `expiresAt` so an older available record at a lower sequence cannot replace it. After `expiresAt`, or after the daemon restarts, that protection is gone. An older advertisement that is still inside its own lifetime can be admitted again. Miners must not reuse a sequence.

A provider signature authenticates the advertisement bytes and the service public key. It does not prove ownership of a wallet address or of any identity that appears only inside `contactValue`.

Local observation (`localFirstSeen`, `localLastSeen`, `localSource`) is daemon metadata. `localSource` is `1` for RPC registration and `2` for a peer. It is not signed and it is not sent on the P2P wire.

### Canonical signing bytes

Domain string `conceal-ai-provider-ad-v1` (25 bytes, no trailing NUL). All integers are little-endian. The signature is excluded.

1. `u8` domain length, then the domain bytes.
2. `u8` protocolVersion.
3. 16 raw network-id bytes.
4. 32 raw public-key bytes.
5. `u64` sequence, `u64` issuedAt, `u64` expiresAt.
6. `u8` status, `u32` availableSlots, `u32` model count.
7. For each model, in order: `u32` id length, id bytes, `u32` maxContextTokens, `u64` priceAtomicUnits, `u8` billingUnit.
8. `u8` contactType, `u32` contact length, contact bytes.

The hash is `crypto::cn_fast_hash` of those bytes (CryptoNote Keccak-256, not SHA3-256). The signature is `crypto::generate_signature` / `crypto::check_signature` (CryptoNote ed25519, not RFC 8032). `generate_signature` draws a random nonce, so two valid signatures of the same advertisement differ. A Rust implementation must reproduce the canonical bytes, the hash, and `check_signature`. It must not expect the signature bytes below from a fresh `generate_signature` call.

Locked vector in `tests/UnitTests/TestAiProviderDiscovery.cpp`. Seed is 32 bytes `0x01` through `0x20`, passed through `generate_keys_from_seed`. Advertisement: protocol 1, mainnet id, sequence 1, issuedAt `1700000000`, expiresAt `1700001800`, status available, 2 slots, model `demo` / 4096 / 1000 / per request, contact endpoint `127.0.0.1:18080`.

- Public key: `ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99`
- Canonical: `19636f6e6365616c2d61692d70726f76696465722d61642d763101ff00a1b2c75f8277f512a336e7e3dd12ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99010000000000000000f153650000000008f85365000000000102000000010000000400000064656d6f00100000e80300000000000001010f0000003132372e302e302e313a3138303830`
- Hash: `6d40f89ddfe5b5b5236d2e5f707f02e79e5680057b0d44c26b29453c6ae7dc16`
- One verifying signature: `32f3d4b8165aed4219d58f8aac3cea19a4ede108e2d198b756f25b2e19253009b6b9906314df6b40dda0c253ac61a8c83415feee27eb0bdf567121f43c12570b`

## Registry

`AiProviderRegistry` is an in-memory map keyed by provider public key. It is not written to `.p2pstate`. Keys `m_net_config` and `m_peer_id` are unchanged.

Acceptance results: `accepted`, `duplicate`, `stale`, `invalid`, `capacityRejected`, `rateLimited`. Only `accepted` is relayed.

- A higher sequence replaces the record and keeps `firstSeen`.
- The same canonical bytes and the same signature at the same sequence are a duplicate and are not relayed.
- Any other record at the same sequence is a conflict and is rejected.
- A lower sequence is stale and is not relayed.
- Wrong network, bad structure, expiry, and a bad signature are `invalid`.
- A new public key is `capacityRejected` when the map already holds 1024 records. An update of an existing key does not need a free slot. Valid unexpired records are not evicted to make room.

`expire` deletes a record when `expiresAt <= now`, including the sequence memory for that key.

RPC `query` walks visible records in public-key order. Offline records are omitted unless the request sets `status` to offline. The cursor is an index into that filtered list. If the map changes between pages, the cursor can skip or repeat. That is best-effort pagination.

P2P `directoryPage` walks every unexpired record, including offline withdrawals, so a withdrawal can propagate. The same cursor limitation applies.

## Wire commands

Ids sit above the P2P pool (1001–1006) and the blockchain pool (2001–2010).

| Id | Name | Kind |
| --- | --- | --- |
| 3001 | `COMMAND_AI_DISCOVERY_CAPABILITIES` | Request. One probe per connection. |
| 3002 | `NOTIFY_AI_PROVIDER_ADVERTISEMENTS` | Notify. At most 8 advertisements. |
| 3003 | `COMMAND_REQUEST_AI_PROVIDERS` | Request and response, matched by `requestId`. |

Dispatch is `NodeServer::handleCommand` on the existing read loop. Discovery responses are taken before the command switch, the same way timed-sync responses are, so `LevinProtocol::invoke` is not used on the long-lived session. There is no second reader on the socket.

Unknown commands already return `LevinError::ERROR_CONNECTION_HANDLER_NOT_DEFINED` (-6) with an empty body and do not shut the connection down. Unknown notifies are ignored. A single capability request is therefore safe for a legacy peer: the error reply is not a supported-capability payload, the peer is marked unsupported, and it is not probed again. The handshake `version` field is not reused.

Relay goes only to connections whose capability state is supported, and it skips the connection that delivered the record. A full registry does not broadcast to legacy peers. If the per-connection discovery queue or the shared write queue is already too full, the notify is dropped. It is not retried in a loop. A later directory sync is the catch-up path.

Directory sync keeps at most one outstanding request per connection and at most 4 connections syncing at once. `requestId` is a random `uint64_t` (1 if the draw is 0). A response with a different id is ignored and the outstanding request stays in place. An undecodable response, a result other than ok, or a cursor that does not advance backs off for 60 seconds. A finished round, or a round that hits 8 pages or 64 KiB, waits 300 seconds and resets the cursor to 0. Request timeout is 20 seconds. Disconnect clears that connection's outstanding sync. Shutdown sets a stopped flag; maintenance and new page requests stop. Discovery failures do not stop blockchain synchronization.

Separate command ids do not remove head-of-line blocking. Discovery and blockchain traffic share the connection read loop, the dispatcher, and the write queue. A slow blockchain handler still stalls the connection.

## RPC

| Path | Behavior |
| --- | --- |
| `POST /registeraiprovider` | Body `{ "advertisement": { ... } }`. Same validation as a peer advertisement. Response `status` is the acceptance name and `reason` is a short string. Relays only when the result is `accepted`. |
| `POST /getaiproviders` | Filters `modelId`, `status`, `minContextTokens`, `billingUnit`, plus `cursor` and `limit`. Returns signed advertisements and the separate local observation. `directoryComplete` is false. |

Registration is not public just because RPC is public. The daemon does not implement the declared `restrictRPC` flag, and it does not trust `X-Forwarded-For`. `/registeraiprovider` is allowed only when the accepted socket's IPv4 address is in `127.0.0.0/8`. Any other peer, including a failed address lookup, gets HTTP 403. A local reverse proxy that connects from 127.0.0.1 looks like loopback; do not put one in front of registration unless that proxy enforces its own authentication.

`/getaiproviders` is readable by any RPC client while the feature is enabled. It returns only data the provider chose to publish.

Both paths are capped at 16 KiB before the HTTP body is read, using `Content-Length`. A request without that header is not given this early cap.

## Limits

| Limit | Value |
| --- | --- |
| Discovery Levin body, before allocation | 16 KiB. Other commands keep the existing 100 MB Levin cap. `packet_max_size` is not an incoming-body limit. |
| Canonical advertisement | 8 KiB |
| Models, model id, contact | 8, 64 bytes, 128 bytes |
| Registry entries | 1024. No eviction of a live record. |
| Lifetime, clock skew | 1800 s, 120 s |
| Ads per notify, page records, page bytes | 8, 8, 12 KiB |
| Sync pages, sync bytes, concurrent syncs | 8, 64 KiB, 4 |
| Request timeout, backoff, round interval | 20 s, 60 s, 300 s |
| Local registrations | 30 per minute |
| Peer updates, queries, signature checks | 32, 4, and 64 per minute |
| Discovery bytes queued on one connection | 32 KiB |
| Yield when the shared write queue is already | 1 MiB. Discovery is dropped, and the connection is not interrupted. |

Cheap structure, network, and time checks run before signature verification. A malformed record does not consume the signature budget. A bad signature does.

`LevinProtocol::readCommand` rejects an oversized discovery body before `buf.resize`. That is the receive-side allocation check for this feature. After that, the KV decoder still builds a `JsonValue` for the capped body before the command handler runs. Checking vector sizes after that parse is not allocation protection for the JSON tree. `serializeBoundedVector` does refuse to resize a destination vector past the collection cap, and it refuses to encode an oversized collection.

## Privacy and trust

The production P2P port is unencrypted. A signature gives integrity and publisher authentication, not confidentiality. Advertise only data that is meant to be public. Do not put prompts, private contract text, wallet secrets, or credentials in an advertisement or in a discovery RPC body.

Signatures do not provide Sybil resistance. Many service keys are cheap. Operators should treat the directory as untrusted hints.

## Tests

`tests/UnitTests/TestAiProviderDiscovery.cpp` covers canonical vectors, signature rejection, sequence rules, offline replay protection, capacity, pagination, rate limits, the loopback registration predicate, one capability probe against an empty legacy-style response, in-process relay of a busy update, and shutdown with outstanding sync state.

That file does not start two daemons and it does not run a legacy binary. A live TCP mixed-version run and a live blockchain sync beside discovery traffic were not exercised. The legacy behavior that was checked is the capability state machine: an empty or undecodable capability response marks the peer unsupported and does not probe again. Command ids outside 3001–3003 still use the existing handler path.

Build and run from an out-of-source build directory configured with `-DBUILD_TESTS=ON`:

```bash
cmake --build . --target UnitTests ConcealDaemon -j$(nproc)
./tests/unit_tests --gtest_filter=AiProviderDiscovery.*
```

On 2026-11-16 that filter passed 6 tests, and `conceal-daemon` linked. Do not treat that as a mixed-version or two-daemon pass.

## Files

- `src/P2p/AiDiscoveryLimits.h` — ids and limits
- `src/P2p/AiProviderTypes.h` / `.cpp` — records, canonical bytes, sign and verify
- `src/P2p/AiProviderValidation.h` / `.cpp` — structural checks and rate windows
- `src/P2p/AiProviderRegistry.h` / `.cpp` — in-memory registry
- `src/P2p/AiDiscoveryProtocolDefinitions.h` — wire commands
- `src/P2p/AiDiscoveryService.h` / `.cpp` — dispatcher-thread session logic
- `src/Rpc/AiProviderRpcCommands.h` — RPC bodies
