# Conceal Unified RPC API Reference

## Contents

### Conceal RPC (Port 8070)

#### System

| Method | Description |
|--------|-------------|
| [getVersion](#getversion) | Get Conceal version string |

#### Wallet Lifecycle

| Method | Description |
|--------|-------------|
| [generateWallet](#generatewallet) | Generate a new wallet with random keys |
| [importWallet](#importwallet) | Import a wallet by view key and optional spend key |
| [unlock](#unlock) | Unlock wallet by reloading saved keys |
| [lock](#lock) | Lock wallet, zeroing spend key from memory |
| [getViewKey](#getviewkey) | Get the wallet view key (if unlocked) |
| [getSpendKey](#getspendkey) | Get the wallet spend key (if unlocked) |
| [getWalletHeight](#getwalletheight) | Get current wallet sync height and output count |
| [exportWallet](#exportwallet) | Export wallet keys as base64-encoded JSON blob |

#### Mainchain Wallet

| Method | Description |
|--------|-------------|
| [getBalance](#getbalance) | Get wallet mainchain CCX balance |
| [getAddress](#getaddress) | Get wallet mainchain address |
| [getStatus](#getstatus) | Get daemon and wallet sync status |
| [getSyncStatus](#getsyncstatus) | Get detailed wallet scan progress |
| [getNetworkHeight](#getnetworkheight) | Get network block height from daemon |
| [transfer](#transfer) | Send CCX to one or more addresses |
| [getTransactions](#gettransactions) | List wallet transaction history with pagination |
| [createDeposit](#createdeposit) | Lock CCX in a time-locked deposit |
| [withdrawDeposit](#withdrawdeposit) | Withdraw a matured deposit |
| [getDeposits](#getdeposits) | List all deposits |
| [estimateFusion](#estimatefusion) | Estimate outputs available for fusion |
| [sendFusionTransaction](#sendfusiontransaction) | Consolidate small outputs |
| [reset](#reset) | Mark wallet for full rescan on restart |
| [save](#save) | Persist wallet state to disk |

#### Sidechain Proxy

| Method | Description |
|--------|-------------|
| [getSidechainStatus](#getsidechainstatus) | Get sidechain connection or full status |
| [getSidechainTokens](#getsidechaintokens) | List all sidechain tokens |
| [getTokenBalance](#gettokenbalance) | Get sidechain token balance |
| [sidechainTransfer](#sidechaintransfer) | Transfer sidechain tokens |
| [sidechainCreateToken](#sidechaincreatetoken) | Create a new sidechain token |

#### DEX Proxy

| Method | Description |
|--------|-------------|
| [dexGetOrderBook](#dexgetorderbook) | Get open orders for a trading pair |
| [dexPlaceOrder](#dexplaceorder) | Submit a new DEX order |
| [dexCancelOrder](#dexcancelorder) | Cancel an open DEX order |
| [dexGetMyOrders](#dexgetmyorders) | Get orders owned by an address |
| [dexGetTradeHistory](#dexgettradehistory) | Get recent trades for a pair |
| [dexGetEscrowBalance](#dexgetescrowbalance) | Get DEX escrow balance |

#### Bridge Proxy

| Method | Description |
|--------|-------------|
| [bridgeGetStatus](#bridgegetstatus) | Get bridge assets and pending unlocks |
| [bridgeLock](#bridgelock) | Lock CCX on mainchain for bridging |
| [bridgeUnlock](#bridgeunlock) | Request CCX unlock from bridge |

---

### Sidechain RPC (Port 8080)

#### Account Methods

| Method | Description |
|--------|-------------|
| [getBalance](#sidechain-getbalance) | Get native SCCX balance |
| [getTokenBalance](#sidechain-gettokenbalance) | Get token balance |

#### Token Methods

| Method | Description |
|--------|-------------|
| [getTokens](#gettokens) | List all registered tokens |
| [getTokenByFingerprint](#gettokenbyfingerprint) | Look up token by fingerprint |
| [createToken](#createtoken) | Create a new token |

#### Transaction Methods

| Method | Description |
|--------|-------------|
| [transfer](#sidechain-transfer) | Transfer tokens between addresses |
| [mintToken](#minttoken) | Mint new tokens (bridge only) |
| [burnToken](#burntoken) | Burn tokens (bridge unlock) |
| [getTransactions](#sidechain-gettransactions) | Get address transaction history |
| [getPendingTransactions](#getpendingtransactions) | Get mempool transaction count |

#### Status and Validators

| Method | Description |
|--------|-------------|
| [getStatus](#sidechain-getstatus) | Get sidechain status |
| [getValidators](#getvalidators) | List active validators |

#### Asset Registry and Bridge

| Method | Description |
|--------|-------------|
| [getAssetRegistry](#getassetregistry) | List all bridged assets |
| [getEquivalenceGroup](#getequivalencegroup) | Get tokens in equivalence class |
| [getBridgeStatus](#getbridgestatus) | Get bridge status and pending unlocks |

#### Faucet

| Method | Description |
|--------|-------------|
| [faucet](#faucet) | Claim test SCCX tokens |

#### DEX Methods

| Method | Description |
|--------|-------------|
| [dex_getOrders](#dex_getorders) | Get open orders for a pair |
| [dex_getTrades](#dex_gettrades) | Get recent trades for a pair |
| [dex_getAllTrades](#dex_getalltrades) | Get recent trades across all pairs |
| [dex_submitOrder](#dex_submitorder) | Submit a new order |
| [dex_cancelOrder](#dex_cancelorder) | Cancel an open order |
| [dex_deposit](#dex_deposit) | Get DEX deposit address |
| [dex_withdraw](#dex_withdraw) | Withdraw from DEX escrow |
| [dex_getEscrowBalance](#sidechain-dex_getescrowbalance) | Get DEX escrow balance |

---

### Real-time Streams (Port 8070 and 8080)

#### SSE Events (conceal-rpc)

| Event | Description |
|-------|-------------|
| [status](#sse-status) | Periodic wallet status broadcast (every 2s) |
| [walletSync](#sse-walletsync) | New outputs detected from chain scan |
| [transaction](#sse-transaction) | Transaction submitted by this wallet |

#### SSE Events (sidechain)

| Event | Description |
|-------|-------------|
| [status](#sse-status-sc) | Sidechain height and token count on connect |

#### WebSocket (sidechain)

| Message Type | Description |
|--------------|-------------|
| [subscribe_orders](#ws-subscribe_orders) | Subscribe to real-time order book for a pair |
| [subscribe_trades](#ws-subscribe_trades) | Subscribe to real-time trade history for a pair |
| [orderBookSnapshot](#ws-orderbooksnapshot) | Order book snapshot pushed to client |
| [tradeHistory](#ws-tradehistory) | Trade history snapshot pushed to client |
| [block](#ws-block) | New sidechain block committed |
| [dexTrade](#ws-dextrade) | New DEX trade executed |
| [bridgeDeposit](#ws-bridgedeposit) | Bridge deposit detected |

---

## Overview

The Conceal unified client provides two JSON-RPC 2.0 endpoints plus real-time event streams:

| Service | Default Port | Purpose |
|---|---|---|
| **Conceal RPC** | 8070 | Unified wallet backend (mainchain CCX + proxy to sidechain/DEX/bridge) |
| **Sidechain RPC** | 8080 | Sidechain validator node (tokens, transfers, DEX, bridge) |

| Stream Type | Port | Protocol | Purpose |
|---|---|---|---|
| **Conceal SSE** | 8070 | Server-Sent Events | Real-time wallet sync and transaction events |
| **Sidechain SSE** | 8080 | Server-Sent Events | Sidechain status on connect |
| **Sidechain WebSocket** | 8080 | WebSocket (RFC 6455) | Real-time DEX order book, trades, blocks, bridge deposits |

All JSON-RPC requests are `POST` to `/json_rpc` with `Content-Type: application/json`.

Request format:
```json
{
  "jsonrpc": "2.0",
  "method": "methodName",
  "params": { },
  "id": 1
}
```

Success response:
```json
{
  "jsonrpc": "2.0",
  "result": { },
  "id": 1
}
```

Error response:
```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32603,
    "message": "description"
  },
  "id": 1
}
```

Standard error codes:
| Code | Meaning |
|------|---------|
| -32600 | Invalid request |
| -32601 | Method not found |
| -32603 | Internal error (see message) |

SSE streams are accessed via HTTP GET with the `Accept: text/event-stream` header. WebSocket connections use the standard HTTP upgrade handshake with `Upgrade: websocket`.

---

## 1. Conceal RPC (Port 8070)

The Conceal RPC server is the unified wallet backend. It handles mainchain CCX operations directly and proxies sidechain, DEX, and bridge calls when connected to a sidechain validator.

### 1.1 System

#### getVersion

Get the Conceal binary version string. Useful for GUI compatibility checks.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `version` | string | Conceal release version string |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getVersion","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "version": "8.1.2"
  },
  "id": 1
}
```

---

### 1.2 Wallet Lifecycle

#### generateWallet

Generate a new random wallet with view key, spend key, address, and mnemonic seed phrase (25 words). The wallet is immediately active and persisted to the state file.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" on success |
| `address` | string | Mainchain CCX address |
| `viewKey` | string | 64-char hex private view key |
| `spendKey` | string | 64-char hex private spend key |
| `mnemonic` | string | 25-word mnemonic seed phrase |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"generateWallet","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "address": "ccx7aBcDeFgHiJkLmNoPqRsTuVwXyZ1234567890abcdef...",
    "viewKey": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2",
    "spendKey": "b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3",
    "mnemonic": "witch collapse practice feed shame open despair creek road again ..."
  },
  "id": 1
}
```

**Notes:** The mnemonic is derived from the spend key. Save the mnemonic and keys securely. The wallet is persisted to the state file automatically so keys survive restarts. A rescan begins immediately in the background.

---

#### importWallet

Import an existing wallet by view key and optional spend key. Destroys any existing wallet and replaces it. No restart needed.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `viewKey` | string | Yes | 64-char hex private view key |
| `spendKey` | string | No | 64-char hex private spend key (omit for view-only) |

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" on success |
| `address` | string | Resolved mainchain CCX address |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"importWallet",
    "params":{
      "viewKey":"a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2",
      "spendKey":"b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3"
    },
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "address": "ccx7aBcDeFgHiJkLmNoPqRsTuVwXyZ..."
  },
  "id": 1
}
```

**Errors:** "Invalid view key hex", "Invalid spend key hex" if hex is not valid 64-char.

**Notes:** After import, a full chain rescan begins in the background if the data directory is available. The wallet is persisted to state file immediately.

---

#### unlock

Reload the wallet keys from the state file without requiring the user to re-enter hex keys. Restores the wallet to its pre-lock state.

**Parameters:** none

**Response:** Same as `importWallet` (returns `{"status":"ok","address":"..."}`).

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"unlock","params":{},"id":1}'
```

**Errors:** "No saved keys found. Use importWallet to set keys first." if the wallet was never imported or generated.

**Notes:** Uses `StateManager::loadKeys()` internally. If a spend key was saved, the wallet becomes a full wallet; otherwise view-only.

---

#### lock

Lock the wallet by zeroing out sensitive key material from memory. The RPC server stays alive so you can still call `unlock` later. View-only wallets are locked trivially.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" |
| `message` | string | Status message |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"lock","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "message": "Wallet locked. Use unlock to restore keys."
  },
  "id": 1
}
```

**Notes:** After locking, the wallet is reconstructed as view-only. The spend key is zeroed in memory. The state file is saved with empty keys to respect the locked state across restarts. Calling `lock` when already locked returns `"Already locked"`.

---

#### getViewKey

Return the current wallet view key as a 64-char hex string. Returns empty string if the wallet is locked.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `viewKey` | string | 64-char hex view key or empty string |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getViewKey","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "viewKey": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2"
  },
  "id": 1
}
```

---

#### getSpendKey

Return the current wallet spend key as a 64-char hex string. Returns empty string if the wallet is locked or view-only.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `spendKey` | string | 64-char hex spend key or empty string |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getSpendKey","params":{},"id":1}'
```

---

#### getWalletHeight

Get the current wallet sync height and total output count in the tracked set.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `walletHeight` | uint32 | Last scanned block height |
| `outputCount` | uint32 | Number of tracked outputs |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getWalletHeight","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "walletHeight": 1850000,
    "outputCount": 245
  },
  "id": 1
}
```

---

#### exportWallet

Export the wallet keys and metadata as a base64-encoded JSON blob. Useful for backup or migrating to another instance.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `wallet` | string | Base64-encoded JSON containing version, viewKey, spendKey, walletHeight, address |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"exportWallet","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "wallet": "eyJ2ZXJzaW9uIjoxLCJ2aWV3S2V5IjoiY..."
  },
  "id": 1
}
```

**Errors:** "Wallet is locked. Unlock first." if the wallet is locked.

**Notes:** The decoded JSON contains: `{"version":1,"viewKey":"...","spendKey":"...","walletHeight":...,"address":"..."}`. Spend key is empty string for view-only wallets.

---

### 1.3 Mainchain Wallet Methods

#### getBalance

Get the wallet's mainchain CCX balance.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `availableBalance` | uint64 | Spendable CCX (atomic units) |
| `lockedAmount` | uint64 | Pending/spend-locked CCX |
| `lockedDepositBalance` | uint64 | CCX locked in active deposits |
| `unlockedDepositBalance` | uint64 | CCX from matured deposits (ready to spend) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getBalance","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "availableBalance": 500000000000,
    "lockedAmount": 0,
    "lockedDepositBalance": 100000000000,
    "unlockedDepositBalance": 0
  },
  "id": 1
}
```

**Notes:** If the wallet has no outputs, returns all zeros without blocking.

---

#### getAddress

Returns the wallet's mainchain address.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `address` | string | Mainchain CCX address |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getAddress","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "address": "ccx7aBcDeFgHiJkLmNoPqRsTuVwXyZ..."
  },
  "id": 1
}
```

---

#### getStatus

General daemon and wallet status. If sidechain is configured, connection details are included.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `blockCount` | uint32 | Daemon's local block height |
| `knownBlockCount` | uint32 | Network height reported by peers |
| `peerCount` | size_t | Number of connected peers |
| `walletHeight` | uint32 | Last scanned block by the wallet |
| `sidechainHost` | string | (optional) Sidechain host |
| `sidechainPort` | uint16 | (optional) Sidechain port |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getStatus","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "blockCount": 2069294,
    "knownBlockCount": 2069294,
    "peerCount": 8,
    "walletHeight": 2068500,
    "sidechainHost": "127.0.0.1",
    "sidechainPort": 8080
  },
  "id": 1
}
```

**Notes:** If the daemon is unreachable, blockCount, knownBlockCount, and peerCount return 0.

---

#### getSyncStatus

Detailed wallet sync progress. Shows how far the wallet scan has reached relative to the daemon.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `walletHeight` | uint32 | Last block scanned by wallet |
| `nodeHeight` | uint32 | Current daemon height |
| `synced` | bool | True when walletHeight >= nodeHeight and nodeHeight > 0 |
| `sidechainHost` | string | (optional) Sidechain host |
| `sidechainPort` | uint16 | (optional) Sidechain port |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getSyncStatus","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "walletHeight": 1850000,
    "nodeHeight": 2069294,
    "synced": false
  },
  "id": 1
}
```

---

#### getNetworkHeight

Get the network block height from the connected daemon.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `networkHeight` | uint32 | Network block height |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getNetworkHeight","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "networkHeight": 2069294
  },
  "id": 1
}
```

---

#### transfer

Send CCX to one or more addresses.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `destinations` | array | Yes | Array of `{address, amount}` objects |
| `mixin` | uint64 | No | Ring size (default: network minimum) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"transfer",
    "params":{
      "destinations": [
        {"address":"ccx7aBcDeFg...", "amount":1000000000}
      ],
      "mixin": 5
    },
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6..."
  },
  "id": 1
}
```

**Errors:** "Cannot send from view-only wallet" if the wallet has no spend key. Transaction build/relay errors are returned as internal errors.

---

#### getTransactions

List wallet transaction history derived from tracked outputs, with pagination support. Items are returned in descending block height order (newest first).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `firstBlockIndex` | uint32 | No | Offset into the sorted output list (default: 0) |
| `blockCount` | uint32 | No | Maximum items to return (default: unlimited, 0 = all) |

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `items` | array | List of output-based transaction entries |
| `items[].hash` | string | Transaction hash (hex) |
| `items[].amount` | uint64 | Amount in atomic units |
| `items[].blockHeight` | uint32 | Block containing the output |
| `items[].spent` | bool | Whether this output has been spent |
| `items[].isDeposit` | bool | Whether this output is a deposit |
| `totalItems` | uint32 | Total number of outputs in the wallet |
| `firstBlockIndex` | uint32 | The offset that was requested |
| `blockCount` | uint32 | The limit that was requested (or totalItems if 0) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTransactions","params":{"firstBlockIndex":0,"blockCount":100},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "items": [
      {
        "hash": "a1b2c3d4e5f6...",
        "amount": 1000000000,
        "blockHeight": 2060000,
        "spent": false,
        "isDeposit": false
      }
    ],
    "totalItems": 245,
    "firstBlockIndex": 0,
    "blockCount": 100
  },
  "id": 1
}
```

---

#### createDeposit

Lock CCX in a time-locked deposit that earns interest.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `amount` | uint64 | Yes | Amount to lock |
| `term` | uint32 | Yes | Lock duration in blocks |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"createDeposit",
    "params":{"amount":100000000000,"term":100000},
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "b2c3d4e5f6a7..."
  },
  "id": 1
}
```

---

#### withdrawDeposit

Withdraw a matured deposit.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `depositId` | uint64 | Yes | ID of the deposit |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"withdrawDeposit","params":{"depositId":1},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "c3d4e5f6a7b8..."
  },
  "id": 1
}
```

---

#### getDeposits

List all deposits tracked by the wallet.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `deposits` | array | Array of deposit objects |
| `deposits[].id` | uint64 | Deposit ID |
| `deposits[].amount` | uint64 | Locked amount |
| `deposits[].term` | uint32 | Lock duration (blocks) |
| `deposits[].unlockHeight` | uint32 | Block when deposit matures |
| `deposits[].locked` | bool | Whether still locked |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getDeposits","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "deposits": [
      {
        "id": 1,
        "amount": 100000000000,
        "term": 100000,
        "unlockHeight": 2169294,
        "locked": true
      }
    ]
  },
  "id": 1
}
```

---

#### estimateFusion

Estimate how many outputs are available for fusion (consolidation).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `threshold` | uint64 | No | Minimum amount to consider (default: 1,000,000) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"estimateFusion","params":{"threshold":1000000},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "fusionReadyCount": 12,
    "totalOutputCount": 45
  },
  "id": 1
}
```

---

#### sendFusionTransaction

Create and send a fusion transaction to consolidate small outputs.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `threshold` | uint64 | No | Minimum amount (default: 1,000,000) |
| `mixin` | uint64 | No | Ring size (default: network minimum) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"sendFusionTransaction",
    "params":{"threshold":1000000,"mixin":5},
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "d4e5f6a7b8c9..."
  },
  "id": 1
}
```

---

#### reset

Mark wallet for a full rescan from genesis on the next sync cycle. Clears all tracked outputs and sets scanned height to 0. Keys are preserved so unlock still works after reset.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" |
| `message` | string | Status description |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"reset","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "message": "Wallet reset. Rescan will begin on next sync cycle."
  },
  "id": 1
}
```

---

#### save

Persist current wallet state to disk.

**Parameters:** none

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"save","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok"
  },
  "id": 1
}
```

**Notes:** Also triggered automatically at a configurable interval (walletAutoSaveInterval). Returns `{"status":"error","message":"Failed to write state file"}` on failure.

---

### 1.4 Sidechain Proxy Methods

These methods forward requests to the configured sidechain validator. They require `--sidechain-host` and `--sidechain-port` set on conceal-rpc, or they are auto-configured when the sidechain runs in the same process.

#### getSidechainStatus

Returns sidechain connection status or proxies to the sidechain's `getStatus` if connected.

**Parameters:** none

When connected, the response is the full sidechain status (see sidechain `getStatus`).

When not connected:
```json
{
  "jsonrpc": "2.0",
  "result": { "sidechainEnabled": false },
  "id": 1
}
```

---

#### getSidechainTokens

List all tokens registered on the sidechain. Proxies to sidechain's `getTokens`.

**Parameters:** none

Example response:
```json
{
  "jsonrpc": "2.0",
  "result": [
    {
      "id": 0,
      "fingerprint": "",
      "name": "SCCX",
      "symbol": "SCCX",
      "totalSupply": 1000000,
      "maxSupply": 0,
      "decimals": 6,
      "backingModel": 0,
      "backingRatio": 0,
      "lockedCCXAmount": 0
    }
  ],
  "id": 1
}
```

---

#### getTokenBalance

Get the balance of a specific sidechain token for an address. Proxies to sidechain's `getTokenBalance`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Hex-encoded public key (64 hex chars) |
| `tokenId` | uint64 | Yes | Token ID |

Returns a plain string with the balance in atomic units.

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"getTokenBalance",
    "params":{"address":"a1b2c3d4...","tokenId":0},
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": "500000",
  "id": 1
}
```

---

#### sidechainTransfer

Transfer sidechain tokens between addresses. Proxies to sidechain's `transfer`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Sender public key (hex) |
| `to` | string | Yes | Recipient public key (hex) |
| `amount` | uint64 | Yes | Amount to send |
| `tokenId` | uint64 | Yes | Token ID |

Returns transaction hash on success.

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"sidechainTransfer",
    "params":{
      "from":"a1b2...",
      "to":"c3d4...",
      "amount":1000,
      "tokenId":1
    },
    "id":1
  }'
```

---

#### sidechainCreateToken

Create a new token on the sidechain. Proxies to sidechain's `createToken`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Creator public key (hex) |
| `name` | string | Yes | Token name (up to 16 chars) |
| `symbol` | string | Yes | Token symbol (up to 8 chars) |
| `initialSupply` | uint64 | Yes | Initial minted supply |
| `backingModel` | uint64 | Yes | 0=Unbacked, 1=Backed, 2=Hybrid |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"sidechainCreateToken",
    "params":{
      "from":"a1b2...",
      "name":"MyToken",
      "symbol":"MTK",
      "initialSupply":1000000,
      "backingModel":0
    },
    "id":1
  }'
```

---

### 1.5 DEX Proxy Methods

All DEX methods require a connected sidechain with DEX enabled.

#### dexGetOrderBook

Get open orders for a trading pair. Proxies to `dex_getOrders`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

Returns an array of open orders (see sidechain `dex_getOrders`).

---

#### dexPlaceOrder

Submit a new DEX order. Proxies to `dex_submitOrder`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |
| `amount` | uint64 | Yes | Order amount |
| `price` | uint64 | Yes | Price in quote token units |
| `side` | string | Yes | "buy" or "sell" |

---

#### dexCancelOrder

Cancel an open DEX order. Proxies to `dex_cancelOrder`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `orderId` | uint64 | Yes | Order ID to cancel |
| `owner` | string | Yes | Owner public key (hex) |

---

#### dexGetMyOrders

Get orders owned by an address for a specific pair. Proxies to `dex_getOrders` and filters by owner.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

---

#### dexGetTradeHistory

Get recent trades for a trading pair. Proxies to `dex_getTrades`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

Returns recent trades for the pair.

---

#### dexGetEscrowBalance

Get DEX escrow balance for a user. Proxies to `dex_getEscrowBalance`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `tokenId` | uint64 | Yes | Token ID |

Returns balance locked in DEX escrow as a plain string.

---

### 1.6 Bridge Proxy Methods

#### bridgeGetStatus

Get bridge status including asset registry and pending unlocks. Proxies to sidechain's `getBridgeStatus`.

**Parameters:** none

---

#### bridgeLock

Lock CCX on mainchain to be bridged to the sidechain. This sends a CCX transfer to the bridge address on mainchain.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `amount` | uint64 | Yes | CCX amount to lock |
| `bridgeAddress` | string | Yes | Bridge mainchain address (destination for the CCX transfer) |

Returns the mainchain transaction hash and status `"locked"`.

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"bridgeLock",
    "params":{"amount":100000000,"bridgeAddress":"ccxBridgeAddr..."},
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "e5f6a7b8c9d0...",
    "status": "locked"
  },
  "id": 1
}
```

---

#### bridgeUnlock

Request unlocking of CCX from the bridge (burns wrapped tokens on sidechain). Proxies to sidechain's `bridgeUnlock`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `lockId` | uint64 | Yes | ID of the bridge lock to release |
| `userAddress` | string | Yes | User's sidechain public key (hex) |

---

## 2. Sidechain RPC (Port 8080)

The sidechain validator exposes its own JSON-RPC 2.0 endpoint. All methods are directly accessible; they are also proxied by the Conceal RPC when it is connected.

### 2.1 Account Methods

#### sidechain-getBalance

Get native SCCX balance of an address.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex, 64 chars) |

Returns a plain string with the balance in atomic units.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getBalance","params":{"address":"a1b2c3..."},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": "1500000",
  "id": 1
}
```

---

#### sidechain-getTokenBalance

Get token balance of an address.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |
| `tokenId` | uint64 | Yes | Token ID |

Returns a plain string with the balance in atomic units.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTokenBalance","params":{"address":"a1b2...","tokenId":1},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": "10000",
  "id": 1
}
```

---

### 2.2 Token Methods

#### getTokens

List all registered tokens on the sidechain.

**Parameters:** none

**Response:** Array of token objects:
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint64 | Token ID |
| `fingerprint` | string | Asset fingerprint |
| `name` | string | Token name |
| `symbol` | string | Token symbol |
| `totalSupply` | uint64 | Current total supply |
| `maxSupply` | uint64 | Maximum supply (0 = unlimited) |
| `decimals` | uint8 | Decimal places |
| `backingModel` | uint8 | 0=Unbacked, 1=Backed, 2=Hybrid |
| `backingRatio` | uint64 | Backing ratio (if backed) |
| `lockedCCXAmount` | uint64 | CCX locked for backing |
| `sourceChain` | string | Source chain (if bridged asset) |
| `sourceAsset` | string | Source asset (if bridged asset) |
| `bridgeOperator` | string | Bridge operator public key (hex) |
| `equivalenceClass` | string | DEX equivalence group |
| `verified` | bool | Asset verified status |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTokens","params":{},"id":1}'
```

---

#### getTokenByFingerprint

Look up a token by its asset fingerprint.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `fingerprint` | string | Yes | Asset fingerprint |

Returns a token object or `null`.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTokenByFingerprint","params":{"fingerprint":"abc123..."},"id":1}'
```

---

#### createToken

Create a new token on the sidechain.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Creator public key (hex) |
| `nameHex` | string | Yes | Token name as hex string (max 16 bytes) |
| `symbolHex` | string | Yes | Token symbol as hex string (max 8 bytes) |
| `initialSupply` | uint64 | Yes | Initial supply to mint |
| `backingModel` | uint64 | Yes | 0=Unbacked, 1=Backed, 2=Hybrid |
| `decimals` | uint8 | No | Decimal places (default 6) |
| `backingRatio` | uint64 | No | For backed models |
| `lockedCCXAmount` | uint64 | No | CCX amount locked for backing |

Returns the transaction hash on success, or `false` on failure.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"createToken",
    "params":{
      "from":"a1b2...",
      "nameHex":"4d79546f6b656e",
      "symbolHex":"4d544b",
      "initialSupply":1000000,
      "backingModel":0
    },
    "id":1
  }'
```

---

### 2.3 Transaction Methods

#### sidechain-transfer

Transfer tokens between sidechain addresses.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Sender public key (hex) |
| `to` | string | Yes | Recipient public key (hex) |
| `amount` | uint64 | Yes | Amount to send |
| `tokenId` | uint64 | Yes | Token ID |

A fee is automatically added (DEFAULT_FEE or TESTNET_FEE, in token ID 0 = SCCX). Returns the transaction hash on success, or `false` on failure.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"transfer",
    "params":{
      "from":"a1b2...",
      "to":"c3d4...",
      "amount":500,
      "tokenId":1
    },
    "id":1
  }'
```

---

#### mintToken

Mint new tokens. Requires authorization from the bridge key (the `from` address must match the bridge operator).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Bridge public key (must match bridge operator) |
| `to` | string | Yes | Recipient public key |
| `amount` | uint64 | Yes | Amount to mint |
| `tokenId` | uint64 | Yes | Token ID |
| `mainChainTxHash` | string | No | Associated mainchain tx hash |

Returns transaction hash on success, or `false` on failure.

---

#### burnToken

Burn tokens (for bridge unlock).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Owner public key |
| `amount` | uint64 | Yes | Amount to burn |
| `tokenId` | uint64 | Yes | Token ID |

Returns transaction hash on success, or `false` on failure.

---

#### sidechain-getTransactions

Get transaction history for an address. Scans all sidechain blocks from genesis to current height.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |

**Response:** Array of transactions involving the address:
| Field | Type | Description |
|-------|------|-------------|
| `txHash` | string | Transaction hash (hex) |
| `type` | string | "Transfer", "CreateToken", "Mint", or "Burn" |
| `from` | string | Sender public key |
| `to` | string | Recipient public key |
| `amount` | uint64 | Amount |
| `tokenId` | uint64 | Token ID |
| `fee` | uint64 | Fee paid |
| `blockHeight` | uint64 | Block containing the transaction |
| `timestamp` | uint64 | Unix timestamp |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTransactions","params":{"address":"a1b2..."},"id":1}'
```

---

#### getPendingTransactions

Returns the number of transactions currently in the validator's mempool.

**Parameters:** none

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getPendingTransactions","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": "3",
  "id": 1
}
```

---

### 2.4 Status and Validators

#### sidechain-getStatus

Get current sidechain status.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `height` | uint64 | Current sidechain block height |
| `tokenCount` | uint64 | Number of registered tokens |
| `pendingTransactions` | uint64 | Mempool transaction count |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getStatus","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "height": 50000,
    "tokenCount": 12,
    "pendingTransactions": 3
  },
  "id": 1
}
```

---

#### getValidators

List active validators.

**Parameters:** none

**Response:** Array of validator objects:
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint32 | Validator ID |
| `host` | string | Hostname/IP |
| `port` | uint16 | RPC port |
| `stake` | uint64 | Stake amount |
| `active` | bool | Whether currently active |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getValidators","params":{},"id":1}'
```

---

### 2.5 Asset Registry and Bridge

#### getAssetRegistry

List all bridged assets with full provenance information.

**Parameters:** none

**Response:** Array of asset registry entries, each containing:
| Field | Type | Description |
|-------|------|-------------|
| `tokenId` | uint64 | Sidechain token ID |
| `fingerprint` | string | Asset fingerprint |
| `sourceChain` | string | Origin chain |
| `sourceAsset` | string | Origin asset |
| `bridgeOperator` | string | Bridge operator public key (hex) |
| `equivalenceClass` | string | DEX equivalence group |
| `verified` | bool | Verification status |
| `name` | string | Token name |
| `symbol` | string | Token symbol |
| `totalSupply` | uint64 | Current supply |
| `lockedCCXAmount` | uint64 | CCX locked in backing |
| `backingModel` | uint8 | Backing model |
| `backingRatio` | uint64 | Backing ratio |
| `decimals` | uint8 | Decimal places |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getAssetRegistry","params":{},"id":1}'
```

---

#### getEquivalenceGroup

Get all tokens belonging to a DEX equivalence class.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `equivalenceClass` | string | Yes | Equivalence class string (e.g., "conceal:native") |

**Response:** Array of token summary objects:
| Field | Type | Description |
|-------|------|-------------|
| `tokenId` | uint64 | Token ID |
| `fingerprint` | string | Asset fingerprint |
| `symbol` | string | Token symbol |
| `name` | string | Token name |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getEquivalenceGroup","params":{"equivalenceClass":"conceal:native"},"id":1}'
```

---

#### getBridgeStatus

Returns bridge-related information: a list of bridge assets with locked amounts, total pending unlocks, and details of the first 20 pending unlocks.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `bridgeAssets` | array | Bridged asset details |
| `bridgeAssets[].tokenId` | uint64 | Token ID |
| `bridgeAssets[].sourceChain` | string | Origin chain |
| `bridgeAssets[].sourceAsset` | string | Origin asset |
| `bridgeAssets[].bridgeOperator` | string | Bridge operator public key (hex) |
| `bridgeAssets[].totalLocked` | uint64 | Total CCX locked for this asset |
| `pendingUnlocks` | uint64 | Total number of queued CCX unlocks |
| `pendingUnlockDetails` | array | First 20 unlock entries |
| `pendingUnlockDetails[].lockId` | uint64 | Lock ID |
| `pendingUnlockDetails[].userAddress` | string | User public key (hex) |
| `pendingUnlockDetails[].tokenId` | uint64 | Token ID |
| `pendingUnlockDetails[].amount` | uint64 | Unlock amount |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getBridgeStatus","params":{},"id":1}'
```

---

### 2.6 Faucet (Testnet)

#### faucet

Claim test SCCX tokens on testnet. Requires the address to have prior transaction history on the sidechain.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" or "error" |
| `amount` | uint64 | Amount credited (if ok) |
| `balance` | uint64 | New balance after credit (if ok) |
| `message` | string | Error description (if error) |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"faucet","params":{"address":"a1b2..."},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "amount": 2,
    "balance": 1500002
  },
  "id": 1
}
```

**Errors:** Returns `{"status":"error","message":"already claimed"}` if the address has already used the faucet. Returns `{"status":"error","message":"address has no transaction history"}` if the address has no prior transactions.

---

### 2.7 DEX Methods

All DEX methods are available directly on the sidechain RPC when the DEX engine is enabled.

#### dex_getOrders

Get open orders for a trading pair.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

**Response:** Array of order objects:
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint64 | Order ID |
| `type` | string | "buy" or "sell" |
| `owner` | string | Owner public key (hex) |
| `amount` | uint64 | Total order amount |
| `price` | uint64 | Price in quote token units |
| `filled` | uint64 | Amount already filled |
| `status` | string | "open", "filled", or "cancelled" |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"dex_getOrders",
    "params":{"baseTokenId":0,"quoteTokenId":1},
    "id":1
  }'
```

---

#### dex_getTrades

Get recent trades for a pair.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |
| `limit` | uint64 | No | Max trades to return (default 50) |

**Response:** Array of trade objects:
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint64 | Trade ID |
| `buyer` | string | Buyer public key (hex) |
| `seller` | string | Seller public key (hex) |
| `amount` | uint64 | Trade amount |
| `price` | uint64 | Execution price |
| `settled` | bool | Whether settlement completed |
| `timestamp` | uint64 | Unix timestamp |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"dex_getTrades",
    "params":{"baseTokenId":0,"quoteTokenId":1,"limit":20},
    "id":1
  }'
```

---

#### dex_getAllTrades

Get the most recent trades across all pairs.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `limit` | uint64 | No | Max trades (default 100) |

**Response:** Array of trade objects (includes `baseTokenId` and `quoteTokenId` in addition to `dex_getTrades` fields):
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint64 | Trade ID |
| `buyer` | string | Buyer public key (hex) |
| `seller` | string | Seller public key (hex) |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |
| `amount` | uint64 | Trade amount |
| `price` | uint64 | Execution price |
| `settled` | bool | Whether settlement completed |
| `timestamp` | uint64 | Unix timestamp |

---

#### dex_submitOrder

Submit a new order to the DEX order book. The owner must have sufficient escrow balance.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | "buy" or "sell" |
| `owner` | string | Yes | Owner public key (hex) |
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |
| `amount` | uint64 | Yes | Order amount |
| `price` | uint64 | Yes | Price in quote token units |

Returns `true` on success, `false` on failure (insufficient escrow balance).

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"dex_submitOrder",
    "params":{
      "type":"buy",
      "owner":"a1b2...",
      "baseTokenId":0,
      "quoteTokenId":1,
      "amount":1000,
      "price":500
    },
    "id":1
  }'
```

---

#### dex_cancelOrder

Cancel an open order. Funds are returned to escrow.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `orderId` | uint64 | Yes | Order ID to cancel |
| `owner` | string | Yes | Owner public key (hex) |

Returns `true` on success, `false` on failure.

---

#### dex_deposit

Get the DEX deposit address. Users send tokens to this address to fund their escrow balance.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `dexAddress` | string | DEX public key (hex) |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"dex_deposit","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "dexAddress": "f1e2d3c4b5a6..."
  },
  "id": 1
}
```

**Notes:** Users initiate a standard sidechain `transfer` to this address to deposit tokens into DEX escrow. Deposits are detected automatically when the transfer is included in a block.

---

#### dex_withdraw

Withdraw tokens from DEX escrow back to the user's sidechain balance.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `tokenId` | uint64 | Yes | Token to withdraw |
| `amount` | uint64 | Yes | Amount to withdraw |

Returns `true` on success, `false` on failure (insufficient escrow balance).

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"dex_withdraw",
    "params":{"owner":"a1b2...","tokenId":0,"amount":500},
    "id":1
  }'
```

---

#### sidechain-dex_getEscrowBalance

Get the DEX escrow balance of a user for a specific token.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `tokenId` | uint64 | Yes | Token ID |

Returns a plain string with the balance.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"dex_getEscrowBalance","params":{"owner":"a1b2...","tokenId":0},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": "2500",
  "id": 1
}
```

---

## 3. Real-time Streams

### 3.1 Server-Sent Events (SSE)

SSE streams provide one-way server-to-client events over a persistent HTTP connection. Clients connect via HTTP GET with the `Accept: text/event-stream` header.

#### Connecting to SSE

**Conceal RPC SSE (Port 8070):**
```bash
curl -N -H "Accept: text/event-stream" http://127.0.0.1:8070/
```

**Sidechain SSE (Port 8080):**
```bash
curl -N -H "Accept: text/event-stream" http://127.0.0.1:8080/
```

The server responds with HTTP 200 and the following headers:
```
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
Access-Control-Allow-Origin: *
```

A keep-alive comment (`: keep-alive`) is sent every 30 seconds to prevent the connection from timing out.

#### SSE Events (conceal-rpc Port 8070)

##### status

Broadcast every 2 seconds with the current wallet status.

**Event type:** `status`

**Data format:**
```json
{
  "mainchainHeight": 2069294,
  "availableBalance": 500000000000,
  "lockedBalance": 0,
  "address": "ccx7aBcDeFgHiJkLmNoPqRsTuVwXyZ..."
}
```

| Field | Type | Description |
|-------|------|-------------|
| `mainchainHeight` | uint32 | Current wallet scan height |
| `availableBalance` | uint64 | Spendable CCX |
| `lockedBalance` | uint64 | Pending/locked CCX |
| `address` | string | Mainchain wallet address |

---

##### walletSync

Broadcast when new outputs are detected from an incremental chain scan.

**Event type:** `walletSync`

**Data format:**
```json
{
  "newHeight": 1850050,
  "newOutputs": 3
}
```

| Field | Type | Description |
|-------|------|-------------|
| `newHeight` | uint32 | Block height reached by the scan |
| `newOutputs` | uint32 | Number of new outputs found |

---

##### transaction

Broadcast when the wallet submits a new transaction to the mainchain.

**Event type:** `transaction`

**Data format:**
```json
{
  "txHash": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6..."
}
```

| Field | Type | Description |
|-------|------|-------------|
| `txHash` | string | Transaction hash (hex) |

---

#### SSE Events (sidechain Port 8080)

##### status

Sent immediately when a client connects to provide a status snapshot.

**Event type:** `status`

**Data format:**
```json
{
  "height": 50000,
  "tokens": 12
}
```

| Field | Type | Description |
|-------|------|-------------|
| `height` | uint64 | Current sidechain block height |
| `tokens` | uint64 | Number of registered tokens |

---

### 3.2 WebSocket (Sidechain Port 8080)

The sidechain RPC server supports WebSocket upgrades for bidirectional real-time communication, primarily for DEX order book and trade streaming.

#### Connecting

Connect via standard WebSocket upgrade handshake:
```
GET / HTTP/1.1
Host: 127.0.0.1:8080
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <client-key>
```

The server responds with HTTP 101 Switching Protocols and the connection remains open for bidirectional framed messages.

All messages are JSON text frames (opcode 0x1). The server also handles ping/pong (opcodes 0x9/0xA) for keep-alive.

---

#### Client-to-Server Messages

##### subscribe_orders

Request a snapshot of the current order book for a trading pair. After connecting, clients may also receive real-time updates for this pair via the `block`, `dexTrade`, and `bridgeDeposit` server push events (these are broadcast to all connected WebSocket clients).

**Message format:**
```json
{
  "type": "subscribe_orders",
  "baseTokenId": 0,
  "quoteTokenId": 1
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Must be "subscribe_orders" |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |

---

##### subscribe_trades

Request a snapshot of recent trade history for a trading pair.

**Message format:**
```json
{
  "type": "subscribe_trades",
  "baseTokenId": 0,
  "quoteTokenId": 1
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Must be "subscribe_trades" |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |

---

#### Server-to-Client Push Events

These events are broadcast to all connected WebSocket clients when the corresponding on-chain event occurs.

##### orderBookSnapshot

Sent in response to a `subscribe_orders` message. Contains the full current order book for the requested pair.

**Message format:**
```json
{
  "type": "orderBookSnapshot",
  "baseTokenId": 0,
  "quoteTokenId": 1,
  "orders": [
    {
      "id": 1,
      "type": "buy",
      "amount": 1000,
      "price": 500,
      "filled": 200
    }
  ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "orderBookSnapshot" |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |
| `orders` | array | Array of open orders |
| `orders[].id` | uint64 | Order ID |
| `orders[].type` | string | "buy" or "sell" |
| `orders[].amount` | uint64 | Total order amount |
| `orders[].price` | uint64 | Price in quote token units |
| `orders[].filled` | uint64 | Amount already filled |

---

##### tradeHistory

Sent in response to a `subscribe_trades` message. Contains the 20 most recent trades for the requested pair.

**Message format:**
```json
{
  "type": "tradeHistory",
  "baseTokenId": 0,
  "quoteTokenId": 1,
  "trades": [
    {
      "id": 1,
      "amount": 500,
      "price": 500,
      "timestamp": 1715472000
    }
  ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "tradeHistory" |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |
| `trades` | array | Array of recent trades |
| `trades[].id` | uint64 | Trade ID |
| `trades[].amount` | uint64 | Trade amount |
| `trades[].price` | uint64 | Execution price |
| `trades[].timestamp` | uint64 | Unix timestamp |

---

##### block

Broadcast to all connected WebSocket clients when a new sidechain block is committed.

**Message format:**
```json
{
  "type": "block",
  "data": {
    "height": 50001,
    "txCount": 5,
    "votes": 3
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "block" |
| `data.height` | uint64 | New block height |
| `data.txCount` | uint64 | Number of transactions in the block |
| `data.votes` | uint64 | Number of validator votes for this block |

---

##### dexTrade

Broadcast to all connected WebSocket clients when a new DEX trade is executed.

**Message format:**
```json
{
  "type": "dexTrade",
  "data": {
    "id": 42,
    "buyer": "a1b2c3d4e5f6...",
    "seller": "f6e5d4c3b2a1...",
    "baseTokenId": 0,
    "quoteTokenId": 1,
    "amount": 500,
    "price": 500,
    "timestamp": 1715472000
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "dexTrade" |
| `data.id` | uint64 | Trade ID |
| `data.buyer` | string | Buyer public key (hex) |
| `data.seller` | string | Seller public key (hex) |
| `data.baseTokenId` | uint64 | Base token ID |
| `data.quoteTokenId` | uint64 | Quote token ID |
| `data.amount` | uint64 | Trade amount |
| `data.price` | uint64 | Execution price |
| `data.timestamp` | uint64 | Unix timestamp |

---

##### bridgeDeposit

Broadcast to all connected WebSocket clients when a bridge deposit is detected.

**Message format:**
```json
{
  "type": "bridgeDeposit",
  "data": {
    "amount": 100000000,
    "destination": "a1b2c3d4e5f6...",
    "txHash": "e5f6a7b8c9d0..."
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "bridgeDeposit" |
| `data.amount` | uint64 | CCX amount deposited |
| `data.destination` | string | Destination public key (hex) |
| `data.txHash` | string | Mainchain transaction hash (hex) |

---

## Service Architecture

The Conceal unified client runs three subsystems that may be selectively enabled:

| Subsystem | CLI Flag | Description |
|-----------|----------|-------------|
| Mainchain (daemon) | `--run-mainchain` | Embedded conceald node for blockchain and P2P |
| Sidechain | `--run-sidechain` | Validator node with tokens, DEX, bridge, gossip |
| Wallet (conceal-rpc) | `--run-wallet` | BoltRPC server with wallet, SSE, sidechain proxy |

When the sidechain and wallet run in the same process, the wallet automatically connects to the sidechain at `127.0.0.1:<sidechain-port>`, enabling all proxy methods without manual configuration.

### Default Ports

| Service | Port |
|---------|------|
| Mainchain P2P | 16000 |
| Mainchain RPC (conceald) | 16001 |
| Mainchain SSE | 16101 (RPC port + 100) |
| Sidechain RPC | 8080 |
| Sidechain Gossip | 9080 (RPC port + GOSSIP_PORT_OFFSET) |
| Wallet RPC (conceal-rpc) | 8070 |

### No Authentication

The Conceal RPC, sidechain RPC, SSE, and WebSocket endpoints do not require API keys or authentication. They are designed to run on localhost or a trusted network. For production deployments, use a reverse proxy with authentication or firewall rules to restrict access.
