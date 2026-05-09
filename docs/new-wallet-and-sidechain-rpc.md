# Conceal Wallet & Sidechain RPC API Reference

## Overview

The Conceal ecosystem provides two JSON-RPC 2.0 endpoints for wallet and sidechain operations:

| Service | Default Port | Purpose |
|---|---|---|
| **BoltRPC** | 8070 | Unified wallet backend (mainchain CCX + proxy to sidechain/DEX/bridge) |
| **Sidechain RPC** | 8080 | Sidechain validator node (tokens, transfers, DEX, bridge) |

All requests are `POST` to `/json_rpc` with `Content-Type: application/json`.  

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

---

## 1. BoltRPC Wallet API (Port 8070)

BoltRPC is the unified wallet backend. It handles mainchain CCX operations directly and proxies sidechain, DEX, and bridge calls when connected to a sidechain validator via `--sidechain-host` and `--sidechain-port`.

### 1.1 Mainchain Wallet Methods

These methods interact with the Conceal mainchain through the connected daemon and BoltCore wallet engine.

#### getBalance

Get the wallet's mainchain CCX balance.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `availableBalance` | uint64 | Spendable CCX (atomic units) |
| `lockedAmount` | uint64 | Pending/spend‑locked CCX |
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

---

#### getAddress

Returns the wallet's mainchain address.

**Parameters:** none

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

General daemon and wallet status. If sidechain is configured, the connection details are included.

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

---

#### getSyncStatus

Detailed wallet sync progress. Shows how far the wallet scan has reached relative to the daemon.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `walletHeight` | uint32 | Last block scanned by wallet |
| `nodeHeight` | uint32 | Current daemon height |
| `synced` | bool | True when wallet is fully caught up |

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

#### transfer

Send CCX to one or more addresses.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `destinations` | array | Yes | Array of `{address, amount}` |
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

**Errors:** View‑only wallets cannot send; transaction build/relay errors are returned.

---

#### getTransactions

List wallet transaction history derived from tracked outputs.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `items` | array | List of output‑based transaction entries |
| `items[].hash` | string | Transaction hash (hex) |
| `items[].amount` | uint64 | Amount in atomic units |
| `items[].blockHeight` | uint32 | Block containing the output |
| `items[].spent` | bool | Whether this output has been spent |
| `items[].isDeposit` | bool | Whether this output is a deposit |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTransactions","params":{},"id":1}'
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
    ]
  },
  "id": 1
}
```

---

#### createDeposit

Lock CCX in a time‑locked deposit that earns interest.

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

List all deposits.

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
        "unlockHeight": 2069294,
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

Mark wallet for a full rescan on next restart.

**Parameters:** none

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"reset","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "will rescan on next restart"
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

---

### 1.2 Sidechain Proxy Methods

These methods forward requests to the configured sidechain validator. They require `--sidechain-host` and `--sidechain-port` set on BoltRPC.

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

List all tokens registered on the sidechain.

**Parameters:** none

Proxies to sidechain's `getTokens`. Example response:
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

Get the balance of a specific sidechain token for an address.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Hex‑encoded public key (64 hex chars) |
| `tokenId` | uint64 | Yes | Token ID |

Proxies to sidechain's `getTokenBalance`. Returns a plain string with the balance in atomic units.

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

Transfer sidechain tokens between addresses.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Sender public key (hex) |
| `to` | string | Yes | Recipient public key (hex) |
| `amount` | uint64 | Yes | Amount to send |
| `tokenId` | uint64 | Yes | Token ID |

Proxies to sidechain's `transfer`. Returns transaction hash on success.

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

Create a new token on the sidechain.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Creator public key (hex) |
| `name` | string | Yes | Token name (up to 16 chars) |
| `symbol` | string | Yes | Token symbol (up to 8 chars) |
| `initialSupply` | uint64 | Yes | Initial minted supply |
| `backingModel` | uint64 | Yes | 0=Unbacked, 1=Backed, 2=Hybrid |

Proxies to sidechain's `createToken`.

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

### 1.3 DEX Proxy Methods

All DEX methods require a connected sidechain with `--enable-dex`.

#### dexGetOrderBook

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

Proxies to `dex_getOrders`. Returns an array of open orders (see sidechain DEX methods).

---

#### dexPlaceOrder

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

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `orderId` | uint64 | Yes | Order ID to cancel |
| `owner` | string | Yes | Owner public key (hex) |

---

#### dexGetMyOrders

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

Proxies to `dex_getOrders` and filters by owner on the sidechain.

---

#### dexGetTradeHistory

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

Proxies to `dex_getTrades`. Returns recent trades for the pair.

---

#### dexGetEscrowBalance

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `tokenId` | uint64 | Yes | Token ID |

Proxies to `dex_getEscrowBalance`. Returns balance locked in DEX escrow.

---

### 1.4 Bridge Proxy Methods

#### bridgeGetStatus

Proxies to sidechain's `getBridgeStatus`. Returns bridge asset registry and pending unlocks.

**Parameters:** none

---

#### bridgeLock

Lock CCX on mainchain to be bridged to the sidechain. This sends a CCX transfer to the bridge address.

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

---

#### bridgeUnlock

Request unlocking of CCX from the bridge (burns wrapped tokens on sidechain).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `lockId` | uint64 | Yes | ID of the bridge lock to release |
| `userAddress` | string | Yes | User's sidechain public key (hex) |

Proxies to sidechain's `bridgeUnlock`.

---

## 2. Sidechain RPC API (Port 8080)

The sidechain validator exposes its own JSON-RPC endpoint. All methods are directly accessible; they are also proxied by BoltRPC when it is connected.

### 2.1 Account Methods

#### getBalance

Get native SCCX balance of an address.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |

Returns a plain string with the balance.

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

#### getTokenBalance

Get token balance of an address.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |
| `tokenId` | uint64 | Yes | Token ID |

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

List all registered tokens.

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
| `sourceChain` | string | Source chain (if bridged) |
| `sourceAsset` | string | Source asset (if bridged) |
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

---

#### createToken

Create a new token. Rate‑limited per address (cooldown of 10 blocks).

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

Returns the transaction hash on success.

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

#### transfer

Transfer tokens between sidechain addresses.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Sender public key (hex) |
| `to` | string | Yes | Recipient public key (hex) |
| `amount` | uint64 | Yes | Amount to send |
| `tokenId` | uint64 | Yes | Token ID |
| `feeTokenId` | uint64 | No | Token used for fee (default 0 = SCCX) |

A fee is automatically added (`DEFAULT_FEE` or `TESTNET_FEE`). Returns the transaction hash.

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

Mint new tokens (bridge only). Requires authorization from the bridge key.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Bridge public key (must match bridge operator) |
| `to` | string | Yes | Recipient public key |
| `amount` | uint64 | Yes | Amount to mint |
| `tokenId` | uint64 | Yes | Token ID |
| `mainChainTxHash` | string | No | Associated mainchain tx hash |

---

#### burnToken

Burn tokens (for bridge unlock).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Owner public key |
| `amount` | uint64 | Yes | Amount to burn |
| `tokenId` | uint64 | Yes | Token ID |

Returns transaction hash.

---

#### getTransactions

Get transaction history for an address.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |

Returns an array of transactions involving the address. Each entry:
| Field | Type | Description |
|-------|------|-------------|
| `txHash` | string | Transaction hash (hex) |
| `type` | string | "Transfer", "CreateToken", "Mint", "Burn" |
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

---

### 2.4 Status & Validators

#### getStatus

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `height` | uint64 | Current sidechain block height |
| `tokenCount` | uint64 | Number of registered tokens |
| `pendingTransactions` | uint64 | Mempool count |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getStatus","params":{},"id":1}'
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

---

### 2.5 Asset Registry & Bridge

#### getAssetRegistry

List all bridged assets with full provenance.

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

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getEquivalenceGroup","params":{"equivalenceClass":"conceal:native"},"id":1}'
```

---

#### getBridgeStatus

Returns bridge‑related information: a list of bridge assets with locked amounts, total pending unlocks, and details of the first 20 pending unlocks.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `bridgeAssets` | array | Bridged asset details |
| `pendingUnlocks` | uint64 | Total number of queued CCX unlocks |
| `pendingUnlockDetails` | array | First 20 unlock entries (lockId, userAddress, tokenId, amount) |

---

#### bridgeUnlock

Request an unlock (burn tokens on sidechain). This is the sidechain endpoint; it creates a burn transaction.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `lockId` | uint64 | Yes | Bridge lock ID |
| `userAddress` | string | Yes | User public key (hex) |

---

### 2.6 Faucet (Testnet)

#### faucet

Claim test SCCX tokens on testnet. Requires the address to have prior transaction history.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |

Returns `{"status":"ok","amount":2,"balance":...}` or an error if already claimed or no history.

---

### 2.7 DEX Methods (Sidechain Direct)

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
| `status` | string | "open", "filled", "cancelled" |

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
| `buyer` | string | Buyer public key |
| `seller` | string | Seller public key |
| `amount` | uint64 | Trade amount |
| `price` | uint64 | Execution price |
| `settled` | bool | Whether settlement completed |
| `timestamp` | uint64 | Unix timestamp |

---

#### dex_getAllTrades

Get the most recent trades across all pairs.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `limit` | uint64 | No | Max trades (default 100) |

Response fields include `baseTokenId` and `quoteTokenId` in addition to the fields of `dex_getTrades`.

---

#### dex_submitOrder

Submit a new order.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | "buy" or "sell" |
| `owner` | string | Yes | Owner public key (hex) |
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |
| `amount` | uint64 | Yes | Order amount |
| `price` | uint64 | Yes | Price in quote token units |

Returns `true` on success.

---

#### dex_cancelOrder

Cancel an open order.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `orderId` | uint64 | Yes | Order ID |
| `owner` | string | Yes | Owner public key |

Returns `true` on success.

---

#### dex_deposit

Get the DEX deposit address. Users send tokens to this address to fund their escrow.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `dexAddress` | string | DEX public key (hex) |

---

#### dex_withdraw

Withdraw tokens from DEX escrow back to the user's sidechain balance.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key |
| `tokenId` | uint64 | Yes | Token to withdraw |
| `amount` | uint64 | Yes | Amount to withdraw |

Returns `true` on success.

---

#### dex_getEscrowBalance

Get the DEX escrow balance of a user.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key |
| `tokenId` | uint64 | Yes | Token ID |

Returns a plain string with the balance.
