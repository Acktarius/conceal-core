// SyncOrchestrator - coordinates DB (BoltSync) and RPC (RpcBlockScanner) sync
// phases to bring a BoltCore::Wallet from zero to chain tip.
//
// Three modes:
//   DbOnly  — offline scan from local MDBX database only
//   RpcOnly — scan via remote daemon RPC with no local DB
//   Hybrid  — DB scan up to db tip, then RPC fills the gap to chain tip;
//             a user who ran a local daemon can thus hand off to a remote node
//             (e.g. 66.203.178.176:16000) for the final blocks and live tail
//
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "RpcBlockScanner.h"
#include "BoltSync/BoltSync.h"
#include "INode.h"
#include "crypto/crypto.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace BoltCore
{
  class Wallet;

  // ── Config ─────────────────────────────────────────────────────────────────

  enum class SyncMode
  {
    Auto,    // Use DB if dataDir is non-empty, then RPC for the gap (= Hybrid)
    DbOnly,  // Offline; no INode calls after initial check
    RpcOnly, // No local DB; full scan via INode
    Hybrid,  // Explicit: DB first, then RPC gap-fill (same as Auto when DB present)
  };

  struct OrchestratorConfig
  {
    SyncMode    mode         = SyncMode::Auto;
    std::string dataDir;      // path to conceald data dir; empty → skip DB phase
    uint32_t    dbThreads    = 0;    // 0 → hardware_concurrency
    uint32_t    rpcBatchSize = 100;  // heights per getBlocks batch [1, 300]
    std::chrono::seconds rpcTimeout{30};

    // Progress callbacks (both optional)
    std::function<void(uint32_t /*height*/)>            onDbProgress;
    std::function<void(uint32_t /*height*/, size_t /*found*/)> onRpcProgress;
    std::function<void(const std::string & /*msg*/)>    onError;
  };

  struct SyncReport
  {
    bool     ok              = false;
    uint32_t dbTip           = 0;   // highest height scanned from DB (0 if skipped)
    uint32_t rpcTip          = 0;   // highest height scanned via RPC (0 if skipped)
    size_t   outputsFromDb   = 0;
    size_t   outputsFromRpc  = 0;
    std::string error;
  };

  // ── Orchestrator ───────────────────────────────────────────────────────────

  class SyncOrchestrator
  {
  public:
    // node    — INode pointed at either a local or remote daemon
    // spendKey — nullptr for view-only wallets
    SyncOrchestrator(cn::INode               &node,
                     const crypto::SecretKey &viewKey,
                     const crypto::PublicKey &spendPub,
                     const crypto::SecretKey *spendKey);

    ~SyncOrchestrator();

    // One-shot sync: brings wallet to chain tip, then returns.
    // Call startTail() afterwards to stay in sync with new blocks.
    SyncReport sync(const OrchestratorConfig &config, Wallet &wallet);

    // ── Live tail ─────────────────────────────────────────────────────────

    // Starts a background poll thread that detects new blocks and scans them.
    // fromHeight  — first height to monitor (usually SyncReport::rpcTip + 1)
    // pollMs      — milliseconds between node height checks (default 5 s)
    //
    // On reorg (detected when node tip drops below lastProcessed):
    //   - Wallet is reset to the DB checkpoint (loadOutputs from stored DB results)
    //   - RPC gap-fill re-runs from dbTip + 1
    //   This keeps the guard minimal with no extra data structures.
    void startTail(Wallet  &wallet,
                   uint32_t fromHeight,
                   uint32_t pollMs = 5000);

    void stopTail();
    bool isTailRunning() const;

  private:
    // ── Internal phase runners ─────────────────────────────────────────────

    // Phase A: MDBX scan via BoltSync::Scanner.
    // Populates m_dbOutputs / m_dbTip for the reorg guard.
    SyncReport runDbPhase(const OrchestratorConfig &config, Wallet &wallet);

    // Phase B: RPC scan from [fromHeight, toHeight].
    SyncReport runRpcPhase(const OrchestratorConfig &config,
                           Wallet &wallet,
                           uint32_t fromHeight,
                           uint32_t toHeight);

    // Resets wallet to the DB checkpoint, then re-scans the RPC gap.
    // Called by the tail thread when a reorg is detected.
    void handleReorg(const OrchestratorConfig &config, Wallet &wallet, uint32_t reorgHeight);

    // ── DB checkpoint storage (for reorg recovery) ────────────────────────

    std::vector<OutputInfo> m_dbOutputs;  // result of DB scan (kept for reorg reset)
    uint32_t                m_dbTip = 0;

    // ── Members ───────────────────────────────────────────────────────────

    cn::INode               &m_node;
    const crypto::SecretKey &m_viewKey;
    const crypto::PublicKey &m_spendPub;
    const crypto::SecretKey *m_spendKey;

    // Stored when sync() is called so the tail thread can reuse callbacks/config.
    OrchestratorConfig m_activeConfig;

    std::atomic<bool>  m_tailStop{false};
    std::thread        m_tailThread;
    mutable std::mutex m_tailMutex;
  };

} // namespace BoltCore
