# AI provider discovery

Status: First implementation, disabled by default. Proposed as [DEC-0005](DECISIONS.md).
Last updated: 2026-11-17, because protocol v1 was redefined in place before any release tag contained it: CCX address contact with an offline spend-key binding, per-second billing, a model digest, sync cursor continuation, reserved local registry slots, and a second capability probe after timeout.
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

`/getinfo` may include `ai_discovery_enabled`, `ai_provider_count`, and `ai_discovery_peers`. The count is the number of non-offline, unexpired records, computed in one pass over the registry. `ai_discovery_peers` is the number of connections in the supported state. The directory itself is not embedded in `/getinfo`. The daemon logs at INFO when that peer count changes between zero and non-zero.

Only directly connected discovery-enabled nodes exchange records. A legacy node answers the capability probe with an error and is not asked again, and it does not forward advertisements it never accepted. Operators who want a directory to spread should link enabled nodes with `add-priority-node` or `add-exclusive-node`. Announcing discovery support in the handshake or the peer list is a future, unscheduled item. It is not implemented.

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
| `contactType`, `contactValue` | `0` none, `1` opaque endpoint of at most 128 bytes, or `2` a standard Conceal address string. |
| `addressBindingSignature` | 64-byte CryptoNote signature. Required for contact type 2. All zero otherwise. It is part of the advertisement canonical bytes. |
| `signature` | CryptoNote signature over the advertisement canonical hash. Not part of the signed bytes. |

Each model offer is `modelId` (1–64 bytes), `maxContextTokens`, `priceAtomicUnits` (integer CCX atomic units), `billingUnit` (`1` per request, `2` per thousand tokens, `3` per second of reserved service), `modelDigestType`, and `modelDigest` (32 bytes). Model ids in one advertisement must be strictly increasing and unique. Two offers with the same `modelId` and different digests are rejected. The daemon never fetches a contact string, never opens a connection because an advertisement named one, and never hashes a model file.

`modelDigestType` `0` means no digest and requires 32 zero bytes. Type `1` means SHA-256 of the model file (for example a GGUF file) and requires a non-zero digest. Clients and miners compare that digest. The daemon does not.

Contact type `2` uses `parseAccountAddressString` and `parameters::CRYPTONOTE_PUBLIC_ADDRESS_BASE58_PREFIX` (`0x7ad4`, the `ccx7` prefix). This tree has one address prefix for mainnet and testnet. A wrong prefix or a malformed string is rejected. The address is how concealed-ai sends a SmartMessage and pays. Contact type `1` stays an opaque hint.

An offline record is a signed withdrawal. It is hidden from the default RPC listing and kept until `expiresAt` so an older available record at a lower sequence cannot replace it. After `expiresAt`, or after the daemon restarts, that protection is gone. An older advertisement that is still inside its own lifetime can be admitted again. Miners must not reuse a sequence.

A provider signature authenticates the advertisement bytes and the service public key. It does not by itself prove ownership of a wallet. Contact type `2` adds a separate binding. The signed message is `cn_fast_hash` of:

1. `u8` length, then `conceal-ai-provider-binding-v1` (30 bytes, no trailing NUL).
2. 16 raw network-id bytes.
3. 32 raw provider service-key bytes.
4. `u32` address length, then the address bytes.

The miner signs that hash with the address spend secret key. The daemon verifies it with `crypto::check_signature` against the spend public key inside the address. The binding does not cover price, expiry, or the other advertisement fields, so the miner can create it once, offline, for each service key, and does not need the spend key while it is publishing. The daemon never holds a spend key. It only verifies. The binding signature is then included in the advertisement canonical bytes, so the provider signature covers it too. Verification of the binding runs only after the cheap checks and, on a peer, spends the same per-peer signature budget. A type-2 record costs two checks.

A same-sequence conflict (different canonical bytes or a different provider signature) is rejected before either check. The stored record was already verified, and a second body at that sequence cannot replace it, so a signature check would not change the result. A lower sequence, and an exact duplicate of the stored canonical bytes and provider signature, also skip both checks. Those records can still consume the per-peer update budget.

Local observation (`localFirstSeen`, `localLastSeen`, `localSource`) is daemon metadata. `localSource` is `1` for RPC registration and `2` for a peer. It is not signed and it is not sent on the P2P wire.

### Canonical signing bytes

Domain string `conceal-ai-provider-ad-v1` (25 bytes, no trailing NUL). All integers are little-endian. The signature is excluded.

1. `u8` domain length, then the domain bytes.
2. `u8` protocolVersion.
3. 16 raw network-id bytes.
4. 32 raw public-key bytes.
5. `u64` sequence, `u64` issuedAt, `u64` expiresAt.
6. `u8` status, `u32` availableSlots, `u32` model count.
7. For each model, in order: `u32` id length, id bytes, `u32` maxContextTokens, `u64` priceAtomicUnits, `u8` billingUnit, `u8` modelDigestType, 32 digest bytes.
8. `u8` contactType, `u32` contact length, contact bytes.
9. 64 raw `addressBindingSignature` bytes. All zero when the contact type is not 2.

The hash is `crypto::cn_fast_hash` of those bytes (CryptoNote Keccak-256, not SHA3-256). The signature is `crypto::generate_signature` / `crypto::check_signature` (CryptoNote ed25519, not RFC 8032). `generate_signature` draws a random nonce, so two valid signatures of the same advertisement differ. A Rust implementation must reproduce the canonical bytes, the hash, and `check_signature`. It must not expect the signature bytes below from a fresh `generate_signature` call.

Locked vector in `tests/UnitTests/TestAiProviderDiscovery.cpp`. No release tag contains commit `68b39f8`, so this redefines protocol v1 in place and keeps the domain string. Provider seed is 32 bytes `0x01` through `0x20`. Spend seed is `0x21` through `0x40`. View seed is `0x41` through `0x60`. All three go through `generate_keys_from_seed`. Advertisement: protocol 1, mainnet id, sequence 1, issuedAt `1700000000`, expiresAt `1700001800`, status available, 2 slots, model `demo` / 4096 / 1000 / per second, SHA-256 digest `80` through `9f`, contact type 2.

- Address: `ccx7FaPyWkgHPaFMtGrZi14kuNBCfWmjoTrxyyTjfom1QLZY3QNnjPP67gpGMufB5E6kffR9rt3mfAqp4Nh4AbeJAP125xKvEP`
- Provider public key: `ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99`
- Binding canonical: `1e636f6e6365616c2d61692d70726f76696465722d62696e64696e672d7631ff00a1b2c75f8277f512a336e7e3dd12ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99620000006363783746615079576b67485061464d7447725a6931346b754e424366576d6a6f5472787979546a666f6d31514c5a5933514e6e6a505036376770474d7566423545366b666652397274336d66417170344e68344162654a4150313235784b764550`
- Binding hash: `87e42684f8be5560b1364d26343a6a5bfad8c5b96b12f348d8f8d1593ae9c5e0`
- One verifying binding signature: `1742858484895cddce7be599be37573394532d5e24c34102a263d3e354927e03190391660fdb750785986399b3f1d8c1c95afaee107066c7244509b56edfeb07`
- Advertisement canonical: `19636f6e6365616c2d61692d70726f76696465722d61642d763101ff00a1b2c75f8277f512a336e7e3dd12ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99010000000000000000f153650000000008f85365000000000102000000010000000400000064656d6f00100000e8030000000000000301808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f02620000006363783746615079576b67485061464d7447725a6931346b754e424366576d6a6f5472787979546a666f6d31514c5a5933514e6e6a505036376770474d7566423545366b666652397274336d66417170344e68344162654a4150313235784b7645501742858484895cddce7be599be37573394532d5e24c34102a263d3e354927e03190391660fdb750785986399b3f1d8c1c95afaee107066c7244509b56edfeb07`
- Advertisement hash: `25719326248403979c0f8f473717c87f082cfffd6d18259f58c7e21aed6f2541`
- One verifying provider signature: `7c7934a88a9b07ed00fbb11cb8428d7e6c05af9eee12337a29b988fbddc10902d599aaf6d396c0dc86dbbdebbbee2a6104ecc38561560dc801638bdbb0a8d602`

## Registry

`AiProviderRegistry` is an in-memory map keyed by provider public key. It is not written to `.p2pstate`. Keys `m_net_config` and `m_peer_id` are unchanged.

Acceptance results: `accepted`, `duplicate`, `stale`, `invalid`, `capacityRejected`, `rateLimited`. Only `accepted` is relayed.

- A higher sequence replaces the record and keeps `firstSeen`.
- The same canonical bytes and the same signature at the same sequence are a duplicate and are not relayed.
- Any other record at the same sequence is a conflict and is rejected.
- A lower sequence is stale and is not relayed.
- Wrong network, bad structure, expiry, and a bad signature are `invalid`.
- A new public key from a peer is `capacityRejected` when peer-sourced records already fill `AI_MAX_REGISTRY_ENTRIES - AI_LOCAL_RESERVED_ENTRIES` (1024 − 64). Those 64 slots stay available for local registration. An update of an existing key does not need a free slot, including an update that changes the source. A registry constructed below 64 entries (tests) still keeps one slot away from peers when it holds more than one record. A one-slot registry cannot reserve a separate local slot.
- When the map is full, a new local registration may evict the peer-sourced record closest to expiry. Local records are never evicted. An offline withdrawal is never evicted before `expiresAt`. If no peer record can be evicted, the local registration is `capacityRejected`. Peer inserts do not evict anyone.

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

Unknown commands already return `LevinError::ERROR_CONNECTION_HANDLER_NOT_DEFINED` (-6) with an empty body and do not shut the connection down. Unknown notifies are ignored. A legacy peer answers quickly with that error. An empty body, an undecodable body, or return code -6 marks the peer unsupported immediately, with no retry. A probe that times out is not that case: the daemon allows one more probe after `AI_PROBE_RETRY_BACKOFF_SECONDS` (60 seconds, the same value as the sync backoff), and at most `AI_MAX_CAPABILITY_PROBES` (2) probes per connection. The handshake `version` field is not reused.

Relay goes only to connections whose capability state is supported, and it skips the connection that delivered the record. A full registry does not broadcast to legacy peers. If the per-connection discovery queue or the shared write queue is already too full, the notify is dropped. It is not retried in a loop. A later directory sync is the catch-up path.

Directory sync keeps at most one outstanding request per connection and at most 4 connections syncing at once. `requestId` is a random `uint64_t` (1 if the draw is 0). A response with a different id is ignored and the outstanding request stays in place. An undecodable response, a result other than ok, a structural or signature failure, or a cursor that does not advance backs off for 60 seconds and does not move the cursor. A record that fails only because it is expired, or issued too far in the future or the past, is skipped on its own and does not back off. A finished round, when the peer sets `done`, waits 300 seconds and resets the cursor to 0. A round that stops because it hit 8 pages or 64 KiB keeps the cursor and waits the same 300 seconds (`AI_SYNC_CONTINUATION_INTERVAL_SECONDS`) before the next pages. If that cursor is past the end because the peer's registry shrank, the peer returns an empty page with `done` true, and the round ends without backoff. Request timeout is 20 seconds. Disconnect clears that connection's outstanding sync. Shutdown sets a stopped flag; maintenance and new page requests stop. Discovery failures do not stop blockchain synchronization.

Separate command ids do not remove head-of-line blocking. Discovery and blockchain traffic share the connection read loop, the dispatcher, and the write queue. A slow blockchain handler still stalls the connection.

## RPC

| Path | Behavior |
| --- | --- |
| `POST /registeraiprovider` | Body `{ "advertisement": { ... } }`. Same validation as a peer advertisement. Response `status` is the acceptance name and `reason` is a short string. Relays only when the result is `accepted`. |
| `POST /getaiproviders` | Filters `modelId`, `status`, `minContextTokens`, `billingUnit`, plus `cursor` and `limit`. Returns signed advertisements and the separate local observation. `directoryComplete` is false. |

Registration is not public just because RPC is public. The daemon does not implement the declared `restrictRPC` flag, and it does not trust `X-Forwarded-For`. `HttpServer::authenticate` accepts HTTP Basic credentials when `start` is given a user and password, but `conceal-daemon` calls `rpcServer.start` with only the bind address and port. `RpcServerConfig` has no `rpc-user` or `rpc-password`. Those options exist on the wallet RPC and the payment gate, not on this daemon. With an empty credential store, `authenticate` returns success for every request. There is therefore no working daemon RPC authentication to hang a remote registration flag on, and none was added. `/registeraiprovider` stays allowed only when the accepted socket's IPv4 address is in `127.0.0.0/8`. Any other peer, including a failed address lookup, gets HTTP 403.

A reverse proxy on the same host makes that path reachable from outside, because the daemon sees the proxy's loopback connection. Do not put one in front of the RPC port unless the proxy blocks `/registeraiprovider` or authenticates it itself. Example nginx snippet:

```nginx
location /registeraiprovider {
    deny all;
    return 403;
}
```

### JSON examples

These bodies are `storeToJson` output from `tests/UnitTests/TestAiProviderDiscovery.cpp`, not hand-written. Binary fields (`networkId`, `providerPublicKey`, `signature`, `addressBindingSignature`, `modelDigest`) are lowercase hex. Object keys are sorted. The address below is a test vector, not a funded wallet. CryptoNote signatures use a random nonce, so a new `generate_signature` call will not reproduce the signature hex. Rust must reproduce the canonical bytes and the hash and verify the signature.

`POST /registeraiprovider` request:

```json
{"advertisement":{"addressBindingSignature":"1742858484895cddce7be599be37573394532d5e24c34102a263d3e354927e03190391660fdb750785986399b3f1d8c1c95afaee107066c7244509b56edfeb07","availableSlots":2,"contactType":2,"contactValue":"ccx7FaPyWkgHPaFMtGrZi14kuNBCfWmjoTrxyyTjfom1QLZY3QNnjPP67gpGMufB5E6kffR9rt3mfAqp4Nh4AbeJAP125xKvEP","expiresAt":1700001800,"issuedAt":1700000000,"models":[{"billingUnit":3,"maxContextTokens":4096,"modelDigest":"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f","modelDigestType":1,"modelId":"demo","priceAtomicUnits":1000}],"networkId":"ff00a1b2c75f8277f512a336e7e3dd12","protocolVersion":1,"providerPublicKey":"ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99","sequence":1,"signature":"7c7934a88a9b07ed00fbb11cb8428d7e6c05af9eee12337a29b988fbddc10902d599aaf6d396c0dc86dbbdebbbee2a6104ecc38561560dc801638bdbb0a8d602","status":1}}
```

Response:

```json
{"reason":"accepted","status":"accepted"}
```

`POST /getaiproviders` request:

```json
{"billingUnit":0,"cursor":0,"limit":8,"minContextTokens":0,"modelId":"","status":0}
```

Response:

```json
{"directoryComplete":false,"nextCursor":1,"pageDone":true,"providers":[{"advertisement":{"addressBindingSignature":"1742858484895cddce7be599be37573394532d5e24c34102a263d3e354927e03190391660fdb750785986399b3f1d8c1c95afaee107066c7244509b56edfeb07","availableSlots":2,"contactType":2,"contactValue":"ccx7FaPyWkgHPaFMtGrZi14kuNBCfWmjoTrxyyTjfom1QLZY3QNnjPP67gpGMufB5E6kffR9rt3mfAqp4Nh4AbeJAP125xKvEP","expiresAt":1700001800,"issuedAt":1700000000,"models":[{"billingUnit":3,"maxContextTokens":4096,"modelDigest":"808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f","modelDigestType":1,"modelId":"demo","priceAtomicUnits":1000}],"networkId":"ff00a1b2c75f8277f512a336e7e3dd12","protocolVersion":1,"providerPublicKey":"ce92350b547b6cf028df0618bf9aba55f949930059308d83ebd727e13472ed99","sequence":1,"signature":"7c7934a88a9b07ed00fbb11cb8428d7e6c05af9eee12337a29b988fbddc10902d599aaf6d396c0dc86dbbdebbbee2a6104ecc38561560dc801638bdbb0a8d602","status":1},"observation":{"localFirstSeen":1700000100,"localLastSeen":1700000100,"localSource":1}}],"status":"OK"}
```

`/getaiproviders` is readable by any RPC client while the feature is enabled. It returns only data the provider chose to publish.

Both paths are capped at 16 KiB before the HTTP body is read, using `Content-Length`. A request without that header is not given this early cap.

## Limits

| Limit | Value |
| --- | --- |
| Discovery Levin body, before allocation | 16 KiB. Other commands keep the existing 100 MB Levin cap. `packet_max_size` is not an incoming-body limit. |
| Canonical advertisement | 8 KiB |
| Models, model id, contact | 8, 64 bytes, 128 bytes |
| Registry entries | 1024. 64 reserved for local records. A full map may evict the peer record closest to expiry for a new local key. |
| Lifetime, clock skew | 1800 s, 120 s |
| Ads per notify, page records, page bytes | 8, 8, 12 KiB |
| Sync pages, sync bytes, concurrent syncs | 8, 64 KiB, 4 |
| Request timeout, backoff, round interval | 20 s, 60 s, 300 s. The 300 s wait also continues a cursor that stopped on the page or byte budget. A timed-out capability probe retries once after 60 s. |
| Local registrations | 30 per minute |
| Peer updates, queries, signature checks | 32, 4, and 64 per minute |
| Discovery bytes queued on one connection | 32 KiB |
| Yield when the shared write queue is already | 1 MiB. Discovery is dropped, and the connection is not interrupted. |

Cheap structure, network, and time checks run before signature verification. A stale record and an exact duplicate do not consume the signature budget. A same-sequence conflict is rejected before verification. A malformed record does not consume the signature budget. A check that is actually performed does, including both checks on a record that carries an address binding. A notify that fails those checks is dropped. A directory response that fails them backs off. A time-only failure skips that record.

`LevinProtocol::readCommand` rejects an oversized discovery body before `buf.resize`. That is the receive-side allocation check for this feature. After that, the KV decoder still builds a `JsonValue` for the capped body before the command handler runs. Checking vector sizes after that parse is not allocation protection for the JSON tree. `serializeBoundedVector` does refuse to resize a destination vector past the collection cap, and it refuses to encode an oversized collection.

## Privacy and trust

The production P2P port is unencrypted. A signature gives integrity and publisher authentication, not confidentiality. Advertise only data that is meant to be public. Do not put prompts, private contract text, wallet secrets, or credentials in an advertisement or in a discovery RPC body.

Signatures do not provide Sybil resistance. Many service keys are cheap. Operators should treat the directory as untrusted hints.

## Tests

`tests/UnitTests/TestAiProviderDiscovery.cpp` covers the locked vectors and JSON bodies, address binding rejection, digest and per-second billing rules, signature-budget skips for stale and duplicate records, an expired record skipped inside a batch, sync of 150 records across rounds, one probe retry after timeout and no retry after return code -6, local reserved slots and eviction, the single-pass count, the loopback registration predicate, in-process relay of a busy update, and shutdown with outstanding sync state.

That file does not start two daemons and it does not run a legacy binary. `tests/IntegrationTestLib` can start in-process nodes, but those nodes do not enable discovery and do not expose the registration RPC. `tests/ai-provider-discovery-two-daemon.sh` is the reproducible live procedure. It was not executed in this change. A live TCP mixed-version run and a live blockchain sync beside discovery traffic were not exercised. The legacy behavior that was checked is the capability state machine: an empty or undecodable capability response, or return code -6, marks the peer unsupported and does not probe again. A timeout allows one retry. Command ids outside 3001–3003 still use the existing handler path.

Build and run from an out-of-source build directory configured with `-DBUILD_TESTS=ON`:

```bash
cmake --build . --target UnitTests ConcealDaemon -j$(nproc)
./tests/unit_tests --gtest_filter=AiProviderDiscovery.*
```

On 2026-11-17 that filter passed 14 tests, and `conceal-daemon` linked. The whole `unit_tests` binary was also run. Three transaction-pool fusion tests failed; this change does not touch the pool. A sandboxed run left `build/path`, and later `WalletApi` cases then threw `MemoryMappedFile::create: File exists` until that file was removed. `WalletApi.emptyBalance` passed after the leftover file was deleted. Do not treat any of these runs as a mixed-version or two-daemon pass.

## Files

- `src/P2p/AiDiscoveryLimits.h` — ids and limits
- `src/P2p/AiProviderTypes.h` / `.cpp` — records, canonical bytes, sign and verify
- `src/P2p/AiProviderValidation.h` / `.cpp` — structural checks and rate windows
- `src/P2p/AiProviderRegistry.h` / `.cpp` — in-memory registry
- `src/P2p/AiDiscoveryProtocolDefinitions.h` — wire commands
- `src/P2p/AiDiscoveryService.h` / `.cpp` — dispatcher-thread session logic
- `src/Rpc/AiProviderRpcCommands.h` — RPC bodies
