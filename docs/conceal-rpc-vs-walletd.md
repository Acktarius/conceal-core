## How `conceal-rpc` improves on `walletd`

### Chain scanning speed

`walletd`
* Scanned blocks one at a time by requesting them from the daemon over RPC.
* Single threaded. Each block required a network round trip.
* A full 2 million block scan took hours.

`conceal-rpc`
* Reads the MDBX blockchain database directly from disk with no RPC overhead.
* Multi threaded scanning with a configurable thread pool. Defaults to auto (uses all available cores, capped at 16).
* A full scan completes in minutes instead of hours.

### Restart behaviour

`walletd`
* Held all wallet state in memory. Nothing was persisted to disk.
* Every restart triggered a complete chain rescan from block 0.
* If the daemon was busy or slow, the wallet was unusable until the rescan finished.

`conceal-rpc`
* Persists wallet state to a binary file after the initial scan and after every incremental sync.
* On restart, loading the state file is instant. No rescan needed.
* A background sync monitor keeps the wallet up to date as new blocks arrive, without blocking the RPC server.
* The service/user can also call the `save` RPC method to force a state write at any time.

### Memory usage

`walletd`
* Stored every wallet output in RAM. Memory grew linearly as the chain got longer.
* On machines with limited RAM, this could make running a wallet impractical.

`conceal-rpc`
* Wallet outputs are held in memory only briefly during scanning. The state file on disk is the source of truth.
* Memory usage stays low and flat regardless of how many blocks have been scanned.

### Crash recovery

`walletd`
* A crash during scanning corrupted the in memory cache.
* Recovery meant starting the entire chain scan over from block 0.

`conceal-rpc`
* Wallet state is saved incrementally. A crash only loses the blocks scanned since the last save.
* The state file is written atomically, so it is never left in a corrupted state.
* Resuming is automatic on the next launch.

### Key management

`walletd`
* Required both view key and spend key at startup. The wallet would not start without them.
* No way to change keys without restarting the entire process.

`conceal-rpc`
* Keys are optional at startup. The server starts in setup mode and waits.
* The `generateWallet` RPC creates a brand new wallet with random keys and returns them to the caller.
* The `importWallet` RPC loads an existing wallet at runtime without restarting the process.
* A view only wallet can be created by providing only the view key.

### Transactions and deposits

`conceal-rpc`
* Full deposit support: create, withdraw, and list all time locked deposits with details per deposit.
* Fusion transactions for consolidating small outputs into fewer larger ones, reducing wallet bloat.
* `estimateFusion` lets the GUI show how many outputs are ready to consolidate before committing.
* Transaction history returns every tracked output directly from the wallet engine.

### Sidechain, DEX, and bridge

`conceal-rpc`
* Connects to a sidechain validator and proxies all sidechain RPC methods through the same port.
* Token operations: list all tokens, check balances, transfer tokens, and create new tokens.
* Full DEX access: view order books, place and cancel orders, check trade history, and manage escrow balances.
* Bridge operations: view bridge status, lock CCX for bridging, and request unlocks.
* Every sidechain, DEX, and bridge feature is available through the same single HTTP endpoint on port 8070.

### Architecture and dependencies

`conceal-rpc`
* A standalone process built on three focused libraries: `BoltSync` for scanning, `BoltCore` for transaction logic, and `NodeRpcProxy` for daemon communication.
* No P2P code. No block processing. No consensus logic.
* A smaller binary with a much smaller attack surface.

### External integration

`conceal-rpc`
* Single JSON RPC 2.0 endpoint at `/json_rpc` on port 8070.
* Every feature, mainchain, sidechain, DEX, bridge, is one HTTP POST and one JSON response.
* Any HTTP client in any language can integrate with it. No special libraries needed.
* The server can run in setup mode with no keys, letting the GUI drive the entire wallet lifecycle through RPC calls alone.
