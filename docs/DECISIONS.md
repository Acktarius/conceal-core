# Conceal PQ Decisions Log

Status: Active
Maintainers: Conceal contributors working on post-quantum planning, architecture, benchmarking, and implementation
Scope: Decision tracking for Conceal post-quantum research, migration strategy, and implementation work

## How to use this file

Use this file to record major technical and strategic decisions related to Conceal post-quantum work.

Create a new entry whenever the team changes or confirms:
- PQ scheme direction
- Hybrid versus full migration approach
- Signature, KEM, or key-management strategy
- Benchmark-driven selection criteria
- Wallet, daemon, RPC, or storage implications
- Compatibility, rollout, fallback, or deprecation policy
- Architecture decisions with multi-file or multi-component impact

Guidelines:
- Prefer appending new entries over overwriting history
- If a decision changes, mark the old one as superseded
- Keep entries concise but explicit about rationale and consequences
- Link related docs, issues, PRs, benchmarks, or design notes whenever available

## Entry template

---

### DEC-XXXX: Short decision title

- Date: YYYY-MM-DD
- Status: Proposed | Accepted | Deferred | Superseded | Rejected
- Owners: @handle, @handle
- Related work: issue/PR/doc links
- Supersedes: DEC-XXXX or none
- Superseded by: DEC-XXXX or none

#### Context

Describe the technical, strategic, or operational situation that required a decision.

#### Decision

State the decision clearly and directly.

#### Rationale

Explain why this option was chosen over alternatives.

#### Consequences

List the expected effects on implementation, planning, benchmarks, operations, or contributor workflow.

#### Compatibility impact

State the expected effect on backward compatibility, mixed deployments, migration, or external interfaces.

#### Evidence

Reference benchmarks, research notes, prototype results, or linked design material.

#### Open questions

List unresolved items that could change follow-on work.

#### Documentation impact

List which files in `docs/` should be updated because of this decision.

---

## Suggested tags

Use these tags inside entries when helpful:
- `PQ-schemes`
- `hybrid-migration`
- `benchmarks`
- `wallet`
- `daemon`
- `rpc`
- `storage`
- `consensus-assumptions`
- `compatibility`
- `rollout`
- `security`

## Example entry

---

### DEC-0001: Record benchmark gate before narrowing PQ candidates

- Date: 2026-07-15
- Status: Proposed
- Owners: @team
- Related work: pq-conceal benchmark notes, scheme evaluation docs
- Supersedes: none
- Superseded by: none

#### Context

Multiple post-quantum candidate paths may remain viable until benchmark data is collected across the daemon, wallet, and transaction-critical flows.

#### Decision

Do not finalize a preferred PQ candidate set until benchmark criteria and test conditions are documented and repeatable.

#### Rationale

A scheme choice made before consistent measurements are available could optimize for incomplete assumptions and force rework later.

#### Consequences

Benchmark definitions become a gating deliverable for strategy updates, implementation sequencing, and documentation claims.

#### Compatibility impact

No immediate compatibility change, but future rollout planning should treat benchmark outcomes as a decision dependency.

#### Evidence

Link benchmark plans, prototype measurements, and evaluation dashboards.

#### Open questions

Which transaction paths, key sizes, verification costs, and storage impacts must be measured first?

#### Documentation impact

Update benchmark docs, migration planning docs, and any decision dashboard that references candidate narrowing.

---

### DEC-0002: Normalize Conceal Core CLI binary names under a `conceal-` prefix

- Date: 2026-11-04
- Status: Accepted — implemented for 6.7.6 as a hard rename without legacy
  aliases (not yet released; see "Implementation update" below)
- Owners: @Acktarius / Conceal Core maintainers
- Related work: Arch/Debian packaging (`docs/packaging.md`),
  [`docs/binary-naming.md`](binary-naming.md), conceal-guardian daemon spawn /
  auto-update, upcoming operator release notes
- Supersedes: none
- Superseded by: none
- Tags: `compatibility`, `rollout`, `daemon`, `wallet`

#### Context

Manual Arch `.pkg.tar.zst` and Debian `.deb` workflows now install Core CLI
binaries into `/usr/bin`. That makes on-disk names part of an operational
contract for packages, systemd `ExecStart`, scripts, and Conceal Guardian
(`execa` + `getNodeExecutableName()` + release extract/copy). Current names are
inconsistent: `conceald`, `concealwallet`, `walletd`, `optimizer`.

Docs-only planning for this decision was opened against GitHub base `master`
while maintainers intend review on `development`; retargeting is a process
step, not part of the rename itself.

#### Decision

Normalize shipped executable names to a branded kebab-case set (pending
acceptance; CLI wallet spelling locked to `conceal-wallet`, not
`conceal-walletCli`):

| Current | Proposed |
| --- | --- |
| `conceald` | `conceal-daemon` |
| `concealwallet` | `conceal-wallet` |
| `walletd` | `conceal-walletd` |
| `optimizer` | `conceal-optimizer` |

Align internal CMake **executable target** names with the branded set for
maintainability (`Daemon` → `ConcealDaemon`, `PaymentGateService` →
`ConcealWalletd`, `Optimizer` → `ConcealOptimizer`; keep `ConcealWallet`).
Target alignment may land before shipped `OUTPUT_NAME` changes and does not by
itself affect packages or downstream consumers.

**Work priority:** (1) conceal-guardian dual-name support for **both** legacy
and new daemon filenames; (2) conceal-core rename/aliases plus GitHub wiki
updates; (3) verify low impact on conceal-desktop; (4) conceal-faucet-api
operator docs/templates. Prefer completing (1) before shipping a Core
rename-only release. Permit Core-first / same-day rollout only with recorded
evidence that old Guardian can extract, install or copy, restart, and later
update a transition Core build.
Package aliases, extracted release aliases, and updater behavior are different
distribution paths; archive aliases alone do not prove updater compatibility.
Windows/zip compatibility requires an explicit tested mechanism (copy, hard
link, or wrapper)—not interchangeable with Unix symlinks—especially because
Guardian’s zip extractor rejects symlink entries. Preserve explicit `node.path`
(do not silently replace an invalid configured path). Gate alias removal on
demonstrated readiness and operator notice; Guardian fallback removal is a
separate decision. Transition start, earliest removal, minimum Guardian
version, and notice interval remain **TBD** until maintainers decide.

This decision entry and [`docs/binary-naming.md`](binary-naming.md) are
documentation only until implementation is accepted.

#### Implementation update (2026-11-04): hard rename in 6.7.6, no legacy aliases

Last updated because: the maintainer decided that 6.7.6 ships only the new
names; this supersedes the transitional-alias plan above and below.

- 6.7.6 is the transition start. Its packages, release tarballs, and release
  zips contain only `conceal-daemon`, `conceal-wallet`, `conceal-walletd`, and
  `conceal-optimizer`. No symlinks, copies, hard links, or wrappers with the
  old names are shipped. `package/test-install.sh` fails if a package installs
  an old name.
- CMake targets are `ConcealDaemon`, `ConcealWallet`, `ConcealWalletd`, and
  `ConcealOptimizer` (`src/CMakeLists.txt`).
- Default CLI logs move out of the executable's directory, which is
  `/usr/bin` for package installs, into the data directory with fixed
  basenames: `conceal-daemon.log`, `conceal-wallet.log`, `conceal-walletd.log`
  (`tools::getLogFilePath` in `src/Common/Util.cpp`). An explicit absolute or
  relative `--log-file` path is used as given; a bare file name goes in the
  data directory. `conceal-optimizer` logs to the console only.
- Rationale: an alias period would only postpone the same break for scripts
  and units while adding per-platform artifact rules (symlinks in packages,
  copies in zips because Guardian's extractor rejects symlinks). Guardian
  0.7.10 already resolves both names, so the updater does not need aliases.
- Superseded: "Core rename/aliases", "every promised alias must be validated",
  the Windows/zip alias mechanism question, and the "earliest alias-removal
  release" TBD. They no longer apply because no aliases ship.
- Consequence: every script, systemd unit, or Guardian `node.path` that names
  `conceald`, `concealwallet`, `walletd`, or `optimizer` breaks on upgrade to
  6.7.6 and must be edited. The operator notice in `docs/binary-naming.md`
  says so.

#### Rationale

Packages made binary names a distribution API. A single `conceal-` + kebab-case
scheme reduces PATH collisions (`walletd`, `optimizer`), matches how Guardian
and operators refer to “the Conceal daemon”, and avoids mixed-case names that
are awkward on Unix. Guardian-first dual-name support reduces the chance that
auto-update users are stuck on a Core release their installed Guardian cannot
extract or copy.

#### Consequences

- Core CMake executable targets should be aligned for maintainability; shipped
  `OUTPUT_NAME`, package install loops, CI staging that hard-codes old names,
  and docs must move together for the binary rename itself.
- Guardian must learn the new daemon filename (with temporary fallback) and
  must be validated on Linux and Windows extract/copy/restart paths.
- Operator units/scripts need a migration notice even if aliases soften the
  first release; every promised alias must be validated (not optional).
  *(Superseded 2026-11-04: no aliases ship; the notice must say old names are
  gone in 6.7.6.)*
- Default log files change name and location (data directory instead of
  beside the binary); data-directory paths themselves do not change.
- Optimizer `--walletd-*` flags can stay for now (RPC endpoint labels, not
  binary paths).
- No consensus, cryptography, RPC, wallet-format, data-directory, package-name,
  or release-asset-prefix change is intended.

#### Compatibility impact

Breaking for anything that hard-codes the old filenames, since 6.7.6 ships no
aliases; dual-name resolvers such as Guardian 0.7.10 keep working. Wallet
files, RPC protocols, and `~/.conceal` data paths stay the same. Default log
files move to the data directory. conceal-desktop: Priority-3 gate passed at
`da23d79` (see Open questions); the earlier "pending full verification" text
is superseded.

#### Evidence

Inventory and checklist in [`docs/binary-naming.md`](binary-naming.md),
including Core paths (`src/CMakeLists.txt`, `package/*`, package workflows,
`.github/workflows/check.yml`, `tools/deploy.sh`), Guardian revision
`63ca611ce24d42e15c6c9618de760ada0f66ff96` (`units/utils.js`, `engine.js`,
`download.js`), Desktop revision
`da23d79768b58ab34406c1987012b8f655bf5047` (partial: `CN_PROJECT_NAME`, no
`conceald` hits; MIME token `concealwallet` in shortcut script), and
faucet-api revision `da1b4fe7b885cb43768c439bcd4072769134554d` (RPC host/port
only in app code; `walletd`/`conceald` hard-coded in setup docs and
`ccx-walletd.service.template`), and Conceal Core GitHub wiki tip
`d0751870c439a7c6eb117f2585de12da8a91fd75` (`Getting-Started` / `Home` /
`Fusion-Transactions` / `Optimize-Wallet-(CLI)` / `Conceal-Wiki` hard-code
`walletd`, `conceald`, `concealwallet`). No rename implementation or Guardian
end-to-end update test is claimed by this decision.

Priority-1 update (2026-11-04): Guardian `b617a4e9c7fd815236b47c740652efd6d28efeab`
(0.7.10, PR #123) prefers `conceal-daemon`, falls back to `conceald` only in
the working directory, keeps explicit `node.path` / `--daemon` unchanged,
copies only real archive binaries (no symlink aliases), and reads the daemon
log from the data directory before the beside-binary file.
`test/daemon-names.test.js` passes 19/19. This is code and unit-test evidence,
not an end-to-end update run against a transition Core build.

#### Open questions

- Fill TBD: ~~transition start release, earliest alias-removal release~~
  (6.7.6, no aliases — 2026-11-04), minimum Guardian version (0.7.10 at
  `b617a4e` is the first with dual-name support; not tagged yet), operator
  notice interval.
- ~~Exact Windows compatibility artifact for release zips and Guardian copy.~~
  Superseded: zips carry no legacy names.
- Guardian follow-ups: try `conceal-daemon.log` when `node.path` names a
  legacy binary, and add Ubuntu 26.04 / `ccx-cli-ubuntu-2604` to its asset
  selection.
- Whether any Core-first exception evidence will be gathered, or priority-1
  Guardian-first remains mandatory.
- Optional later rename of optimizer `--walletd-*` CLI flags.
- Priority-3 Desktop gate passed at `da23d79` (2026-11-04) for Core target /
  `OUTPUT_NAME` renames; re-open only if Desktop changes cryptonote
  integration.
- Priority-4 faucet-api doc owners / timing relative to the Core notice window.

#### Documentation impact

- [`docs/binary-naming.md`](binary-naming.md) — assessment, strategy, checklist
  (primary)
- [`docs/packaging.md`](packaging.md) — package contents and log location
  (updated 2026-11-04 for the 6.7.6 rename)
- Operator release notice draft (in binary-naming.md); wiki / conceal-docs /
  Guardian docs when implementation lands

---

### DEC-0003: Align P2P Layer-1 API to camelCase (manual port from ack/wallet)

- Date: 2026-11-15
- Status: Accepted
- Owners: @Acktarius / Conceal Core maintainers
- Related work: branch `ack/p2p-naming-align`; source checklist commit
  `5acf6f4b` on `origin/ack/wallet`; unit test `tests/UnitTests/TestP2pNaming.cpp`;
  handoff [`docs/p2p-naming-align.md`](p2p-naming-align.md)
- Supersedes: none
- Superseded by: none
- Tags: `p2p`, `naming`, `compatibility`

#### Context

`development` mixed snake_case and camelCase on `IP2pEndpoint` / `NodeServer`
while project rules and most modern modules (`Wallet`, `Transfers`, …) use
camelCase methods. `origin/ack/wallet` already completed a style unify, but a
cherry-pick of `5acf6f4b` onto `development` is unsafe (Daemon/RPC/Protocol
divergence; parent tree includes auto-scale/GPU).

#### Decision

Manually port **Layer-1 (+ connection-context) camelCase** naming onto
`development` using `5acf6f4b` as a rename checklist, not as a git cherry-pick.

- Methods / context fields: snake_case → camelCase (and fix
  `state_befor_handshake` → `stateBeforeHandshake`).
- `.p2pstate` config members may use camelCase C++ names, but **serialize with
  stable legacy keys** (`"m_net_config"`, `"m_peer_id"`) so existing peer-state
  files keep loading.
- Exclude P2P auto-scale, GPU prefetch, MDBX, and PeerlistManager public API
  renames.
- Defer Layer-3 System/Platform private `m_` sweeps (wallet only updated Linux
  Platform).
- Remove unused `RemoteEventLock`.
- conceal-desktop does not vendor P2P sources. Its README clones conceal-core
  into `cryptonote`. Its own code only calls unchanged `NodeServer` methods
  (`init`, `run`, `deinit`, `sendStopSignal`) and `set_p2p_endpoint`. No desktop
  C++ patch is required once it clones a core revision that contains this
  rename. Guardian remains RPC/binary-only. See
  [`docs/p2p-naming-align.md`](p2p-naming-align.md).
- Layer 2 (2026-11-15): private members of `P2pContext` and `P2pNode` use the
  `m_` prefix. Public methods stay the same. Checklist for desktop is
  [`docs/p2p-naming-align.md`](p2p-naming-align.md).

#### Rationale

Matches [`.cursor/rules/main-rule.mdc`](../.cursor/rules/main-rule.mdc) and the
repo’s camelCase-dominant modules without pulling unrelated wallet features or
breaking on-disk peer state the way wallet’s `KV_MEMBER` rename did.

#### Consequences

- Core call sites (`CryptoNoteProtocolHandler`, Daemon commands, RpcServer)
  updated to the new `IP2pEndpoint` / `NodeServer` names.
- Desktop picks up the rename by cloning that core revision into `cryptonote`; its own call sites do not use the renamed names.
- Later wallet P2P features (auto-scale, GPU) remain staged for later tasks.

#### Compatibility impact

- Wire protocol / handshake semantics unchanged.
- `.p2pstate` file format keys unchanged (intentional divergence from wallet).
- No daemon binary rename; no RPC method rename required for this task.

#### Evidence

`rg`-driven rename checklist; `TestP2pNaming` covers stub camelCase API and
stable KV keys; full daemon/unit build on branch `ack/p2p-naming-align`.

---

### DEC-0004: Keep discovery on TCP/Levin; defer QUIC and any transport abstraction

- Date: 2026-11-16
- Status: Accepted (deferral). The proof of concept is proposed and is not approved.
- Owners: @Acktarius / Conceal Core maintainers
- Related work: [`docs/quic-transport-evaluation.md`](quic-transport-evaluation.md)
- Supersedes: none
- Superseded by: none
- Tags: `p2p`, `rollout`, `compatibility`, `security`

#### Context

AI discovery needs a place to run before anyone reworks the peer transport.
The production listener is `NodeServer` on TCP with Levin framing. `P2pNode`
is a second stack that the daemon never constructs, and it is still tied to
`TcpConnection`. QUIC would add UDP reachability, a TLS stack this tree does
not link, and a session model the current read loop does not have.

#### Decision

- Do not replace the TCP listener, do not add a transport interface to the
  daemon, and do not make discovery depend on QUIC.
- Implement discovery on the existing Levin command path (`NodeServer`,
  `LevinProtocol`, `CryptoNoteProtocolHandler` or a sibling command range).
- A separate opt-in QUIC proof of concept may be written only after a
  maintainer approves that experiment. It stays out of `conceal-daemon`.
- Do not describe QUIC as faster or as the discovery transport until the
  measurements in `docs/quic-transport-evaluation.md` exist.

#### Rationale

Payload serialization is already separate from sockets. The live coupling is
small (Levin's `TcpConnection` reference and one ordered session per peer) but
the feature that would justify cutting it — independent sessions under load —
is unmeasured. A second TCP connection, which back-ping already uses, is the
closer isolation tool. Introducing an interface first would edit the production
path for a single implementation.

#### Consequences

- Discovery work continues against TCP port 15000, the current handshake, and
  the current write queue.
- No OpenSSL/BoringSSL/MsQuic/ngtcp2 dependency is added to the daemon.
- Desktop's cloned `NodeServer` API is unchanged.
- The evaluation doc is the source for a later experiment. That experiment is
  not scheduled by this decision.

#### Compatibility impact

None. Wire protocol, peerlist, UPnP (TCP), seeds, and sync are unchanged.

#### Evidence

Code inspection recorded in [`docs/quic-transport-evaluation.md`](quic-transport-evaluation.md).
No benchmark was run. Library notes are from upstream documentation retrieved
2026-11-16 (ngtcp2 MIT C API and experimental OpenSSL helper, MsQuic 2.6.4 MIT,
OpenSSL 3.5 server-side QUIC, quiche BSD-2-Clause requiring a Rust toolchain,
LSQUIC MIT). They are selection constraints, not performance results.

#### Open questions

- Whether a second TCP connection is enough for discovery isolation.
- Which TLS backend a future PoC would pin, if a maintainer approves one.
- UDP/15000 reachability for current operators.
- concealed-ai crate and ALPN choices. That client is not in this repository.

#### Documentation impact

- [`docs/quic-transport-evaluation.md`](quic-transport-evaluation.md) — evaluation,
  coupling map, library constraints, and the unapproved benchmark plan.
- This entry.
- [`docs/ai-provider-discovery.md`](ai-provider-discovery.md) — discovery as implemented on TCP/Levin. It does not change this decision. The proof of concept remains unapproved and unstarted, and QUIC remains unmeasured.

---

### DEC-0005: AI provider discovery on the existing TCP/Levin session

- Date: 2026-11-16
- Status: Proposed
- Owners: @Acktarius / Conceal Core maintainers
- Related work: [`docs/ai-provider-discovery.md`](ai-provider-discovery.md), [DEC-0004](DECISIONS.md)
- Supersedes: none
- Superseded by: none
- Tags: `p2p`, `rpc`, `security`, `rollout`

#### Context

conceal-ai-miner needs to publish an expiring compute advertisement, and concealed-ai needs to read those advertisements from a daemon. The daemon must not run models or hold wallet spend keys. DEC-0004 already keeps this work on `NodeServer` TCP/Levin and leaves the QUIC proof of concept unapproved.

#### Decision

- Add a disabled-by-default discovery command family on the existing session: capability request 3001, advertisement notify 3002, directory request 3003.
- Keep validation, the in-memory registry, and relay in dedicated P2P components. Do not persist the registry in `.p2pstate`.
- Relay only a newly accepted record, and only to peers that answered the capability request. Do not broadcast to legacy peers.
- Expose `POST /registeraiprovider` only to a loopback TCP peer, and `POST /getaiproviders` as a bounded read while the flag is on.
- Treat the directory as an incomplete, untrusted cache. A provider signature does not prove hardware or reachability. Contact type 2 adds an offline spend-key binding from the CCX address to the service key. The daemon only verifies that binding.
- Accept billing unit 3 (CCX atomic units per second) and an optional SHA-256 model digest. The daemon does not hash model files.
- Keep 64 registry slots for local records. A full map may evict the peer record closest to expiry for a new local key, never a local record, and never an offline withdrawal before expiry.
- When a directory round hits its page or byte budget, keep the cursor and wait 300 seconds. Reset the cursor only when the peer reports `done`.
- On a capability timeout, probe once more after 60 seconds, at most twice per connection. Return code -6, or an empty or undecodable reply, still marks the peer unsupported with no retry.
- Do not add authenticated remote registration. The daemon RPC server has no configured credentials. Registration stays loopback-only. Announcing discovery in the handshake or peer list is unscheduled and is not part of this decision.

#### Rationale

Legacy peers answer an unknown request with `ERROR_CONNECTION_HANDLER_NOT_DEFINED` and stay connected, so one capability request is a compatible probe. `LevinProtocol::invoke` cannot share the long-lived read loop. The daemon has no working RPC restriction flag, so registration checks the accepted socket address instead of a header. The feature stays off until an operator passes `--enable-ai-discovery` or sets `enable-ai-discovery=TRUE` in the daemon config file.

#### Consequences

- Operators who leave the option unset see no discovery traffic and both new RPC paths return 404.
- Operators who enable it from the command line or the config file still speak plaintext TCP. Advertisements are public data.
- Miners must store their own sequence counter. A daemon restart forgets the registry and can accept an older advertisement that has not yet expired.
- QUIC is not measured and is not a dependency. DEC-0004 is unchanged.

#### Compatibility impact

Low while the flag is off: handshake, timed sync, wire-field names, and `.p2pstate` keys are unchanged. With the flag on, new command ids are used only toward peers that support them. A legacy peer that receives one capability request gets the existing unknown-command error and is not probed again.

#### Evidence

`AiProviderDiscovery.*` passed in `./tests/unit_tests` (14 tests) after an out-of-source build with `-DBUILD_TESTS=ON` on 2026-11-17. `conceal-daemon` linked. No two-daemon TCP run and no legacy peer binary were executed. The full `unit_tests` binary was run the same day. Three transaction-pool fusion tests failed; the diff does not touch the pool. A sandboxed run also left `build/path`, which made later `WalletApi` cases throw `MemoryMappedFile::create: File exists` until that file was removed. `WalletApi.emptyBalance` passed after the leftover file was deleted. Those failures are outside this decision.

#### Open questions

- How a restarted daemon should remember sequence high-water marks without putting the registry in `.p2pstate`.
- Whether best-effort pagination is enough once more than one daemon is in regular use.
- Authenticated remote registration stays closed: `conceal-daemon` never configures `HttpServer` credentials. Revisit only if daemon RPC authentication is added on purpose.
- Announcing discovery support in the handshake or the peer list. Unscheduled. Not implemented.

#### Update 2026-11-17

Protocol v1 was still unreleased (`git tag --contains 68b39f8` printed nothing), so the signed fields above were redefined in place. DEC-0004 is unchanged.

#### Documentation impact

- [`docs/ai-provider-discovery.md`](ai-provider-discovery.md) — format, ids, RPC, limits, and limitations.
- [`docs/README.md`](README.md) — link from the engineering notes.
- [`docs/quic-transport-evaluation.md`](quic-transport-evaluation.md) — pointer only. The evaluation conclusion is unchanged.

---
