# P2P naming alignment (core → desktop handoff)

Status: Layer 1 and Layer 2 landed on branch `ack/p2p-naming-align` (not yet merged to `development`).
Last updated: 2026-11-15, because Layer 2 `P2pContext` / `P2pNode` members were given the `m_` prefix.
Audience: Conceal Core, and later **conceal-desktop** when it resyncs vendored `cryptonote/src/P2p`.

This note is the forwardable checklist. Wire protocol, handshake, and timed-sync payloads are unchanged.

## What desktop must update

conceal-desktop embeds core P2P sources for the in-process node (`NodeServer`, `CryptoNoteProtocolHandler`). After this branch is merged, copy the renamed core files and fix desktop call sites that still use the old snake_case names. conceal-guardian does not call these C++ APIs.

### Layer 1 — public API (breaking for embedded C++)

`IP2pEndpoint` in `src/P2p/NetNodeCommon.h`:

| Old | New |
| --- | --- |
| `relay_notify_to_all` | `relayNotifyToAll` |
| `invoke_notify_to_peer` | `invokeNotifyToPeer` |
| `get_connections_count` | `getConnectionsCount` |
| `for_each_connection` | `forEachConnection` |
| `drop_connection` | `dropConnection` |

`externalRelayNotifyToAll` and `externalRelayNotifyToList` were already camelCase.

`NodeServer` methods used outside `src/P2p`:

| Old | New |
| --- | --- |
| `get_payload_object` | `getPayloadObject` |
| `get_connections_count` | `getConnectionsCount` |
| `get_outgoing_connections_count` | `getOutgoingConnectionsCount` |
| `log_peerlist` | `logPeerlist` |
| `log_connections` (on `NodeServer` only) | `logConnections` |

`CryptoNoteProtocolHandler::log_connections()` and `set_p2p_endpoint()` are **unchanged**.

`CryptoNoteConnectionContext` (`src/P2p/ConnectionContext.h`):

| Old | New |
| --- | --- |
| `state_befor_handshake` | `stateBeforeHandshake` |
| `state_synchronizing` | `stateSynchronizing` |
| `state_idle` | `stateIdle` |
| `state_normal` | `stateNormal` |
| `state_sync_required` | `stateSyncRequired` |
| `state_pool_sync_required` | `statePoolSyncRequired` |
| `state_shutdown` | `stateShutdown` |
| `m_connection_id` | `m_connectionId` |
| `m_remote_ip` | `m_remoteIp` |
| `m_remote_port` | `m_remotePort` |
| `m_is_income` | `m_isIncoming` |
| `m_pending_lite_block` | `m_pendingLiteBlock` |
| `m_needed_objects` | `m_neededObjects` |
| `m_requested_objects` | `m_requestedObjects` |
| `m_remote_blockchain_height` | `m_remoteBlockchainHeight` |
| `m_last_response_height` | `m_lastResponseHeight` |

Core call sites already updated: `CryptoNoteProtocolHandler.cpp`, `DaemonCommandsHandler.cpp`, `RpcServer.cpp`.

### Do not rename these lookalikes

- `cn::HttpServer::get_connections_count` (RPC HTTP, not P2P).
- `PeerlistManager` public methods (`get_peerlist_head`, `append_with_peer_white`, …). Still snake_case on core and on `ack/wallet`.
- Wallet / Core `init_options` and `handle_command_line` outside `NodeServer`.

## On-disk peer state

C++ config members are `m_netConfig` and `m_peerId`, but serialization keeps the legacy keys:

```cpp
s(m_netConfig, "m_net_config");
s(m_peerId, "m_peer_id");
```

Existing `.p2pstate` files keep loading. This differs from wallet commit `5acf6f4b`, which let `KV_MEMBER` rename the keys and required a one-time peer-list re-bootstrap. Desktop must keep these string keys if it copies `NodeServer::config::serialize`.

## Layer 2 — private members only

No desktop call-site change unless desktop code reaches into these privates (it should not).

`P2pContext`: `version`, `incoming`, `remoteAddress`, `peerId`, `peerPort`, `dispatcher`, `contextGroup`, `timeStarted`, `stopped`, `lastReadTime`, `timedSyncInterval`, `timedSyncData`, `timedSyncTimer`, `timedSyncFinished`, `connection`, `writeEvent`, `readEvent` → the same names with an `m_` prefix.

`P2pNode`: `logger` → `m_logger`, `workingContextGroup` → `m_workingContextGroup`.

Public methods (`getPeerId`, `isIncoming`, `getRemoteAddress`, …) and constructor parameter names are unchanged.

## Still deferred

- Layer 3: `m_` on `platform_system` types (`Dispatcher`, `Tcp*`, `Event`, `Timer`, `ContextGroup`) across Linux, OSX, and Windows. Wallet only renamed Linux.
- PeerlistManager camelCase.
- P2P auto-scale and GPU connection prefetch from `ack/wallet`.

## Decision

Recorded as DEC-0003 in `docs/DECISIONS.md`.
