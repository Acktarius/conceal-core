// SyncOrchestrator implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SyncOrchestrator.h"
#include "BoltCore.h"

#include <thread>

namespace BoltCore
{
  namespace
  {
    // Convert BoltSync::FoundOutput → BoltCore::OutputInfo.
    // The two structs are almost identical; subAddress is left empty (main address).
    OutputInfo foundOutputToOutputInfo(const BoltSync::FoundOutput &fo)
    {
      OutputInfo out = {};
      out.blockHeight           = fo.blockHeight;
      out.txHash                = fo.txHash;
      out.outputIndex           = fo.outputIndex;
      out.globalOutputIndex     = fo.globalOutputIndex;
      out.hasGlobalOutputIndex  = fo.hasGlobalOutputIndex;
      out.keyDerivationIndex    = fo.keyDerivationIndex;
      out.hasKeyDerivationIndex = fo.hasKeyDerivationIndex;
      out.amount                = fo.amount;
      out.outputKey             = fo.outputKey;
      out.txPublicKey           = fo.txPublicKey;
      out.keyImage              = fo.keyImage;
      out.spent                 = fo.spent;
      out.isDeposit             = fo.isDeposit;
      out.term                  = fo.term;
      // subAddress left as "" — resolved by the wallet from sub-address manager
      return out;
    }

    bool isDbAvailable(const std::string &dataDir)
    {
      return !dataDir.empty();
    }
  }

  // ── Constructor / Destructor ───────────────────────────────────────────────

  SyncOrchestrator::SyncOrchestrator(cn::INode               &node,
                                     const crypto::SecretKey &viewKey,
                                     const crypto::PublicKey &spendPub,
                                     const crypto::SecretKey *spendKey)
      : m_node(node), m_viewKey(viewKey), m_spendPub(spendPub), m_spendKey(spendKey)
  {
  }

  SyncOrchestrator::~SyncOrchestrator()
  {
    stopTail();
  }

  // ── Public sync entry point ────────────────────────────────────────────────

  SyncReport SyncOrchestrator::sync(const OrchestratorConfig &config, Wallet &wallet)
  {
    m_activeConfig = config;  // persisted for the tail thread's reorg recovery

    SyncReport report;

    const bool hasDb  = isDbAvailable(config.dataDir);
    const bool useDb  = hasDb && (config.mode != SyncMode::RpcOnly);
    const bool useRpc = (config.mode != SyncMode::DbOnly);

    // ── Phase A: DB scan ──────────────────────────────────────────────────
    if (useDb)
    {
      SyncReport dbReport = runDbPhase(config, wallet);
      if (!dbReport.ok)
      {
        report.error = dbReport.error;
        if (config.onError)
          config.onError(dbReport.error);
        // Non-fatal for Hybrid/Auto — fall through to RPC from height 0
      }
      else
      {
        report.dbTip         = dbReport.dbTip;
        report.outputsFromDb = dbReport.outputsFromDb;
      }
    }

    if (!useRpc)
    {
      report.ok = true;
      return report;
    }

    // ── Phase B: RPC gap-fill (from dbTip+1 to chain tip) ─────────────────
    // RpcOnly: scan from genesis (0); Hybrid/Auto: pick up where the DB left off.
    const uint32_t rpcStart = (config.mode == SyncMode::RpcOnly) ? 0
                            : (report.dbTip > 0 ? report.dbTip + 1 : 0);
    const uint32_t rpcEnd   = 0;  // 0 = ask node

    SyncReport rpcReport = runRpcPhase(config, wallet, rpcStart, rpcEnd);
    if (!rpcReport.ok)
    {
      report.error = rpcReport.error;
      if (config.onError)
        config.onError(rpcReport.error);
      return report;
    }

    report.rpcTip         = rpcReport.rpcTip;
    report.outputsFromRpc = rpcReport.outputsFromRpc;
    report.ok             = true;
    return report;
  }

  // ── Phase A: MDBX scan ────────────────────────────────────────────────────

  SyncReport SyncOrchestrator::runDbPhase(const OrchestratorConfig &config, Wallet &wallet)
  {
    SyncReport report;

    BoltSync::ScanConfig scanCfg;
    scanCfg.dataDir    = config.dataDir;
    scanCfg.numThreads = config.dbThreads;
    scanCfg.onProgress = config.onDbProgress;

    BoltSync::ScanState state;
    BoltSync::Scanner   scanner(m_viewKey, m_spendPub, m_spendKey);

    if (!scanner.scan(scanCfg, state))
    {
      report.error = "DB scan failed — MDBX database may be missing or corrupt in: "
                     + config.dataDir;
      return report;
    }

    // Convert and load results.
    std::vector<OutputInfo> outputs;
    outputs.reserve(state.results.size());
    for (const auto &fo : state.results)
      outputs.push_back(foundOutputToOutputInfo(fo));

    wallet.loadOutputs(outputs, state.scannedTopHeight);

    // Store the DB checkpoint for reorg recovery in the live tail.
    m_dbOutputs = outputs;
    m_dbTip     = state.scannedTopHeight;

    report.ok           = true;
    report.dbTip        = state.scannedTopHeight;
    report.outputsFromDb = outputs.size();
    return report;
  }

  // ── Phase B: RPC scan ─────────────────────────────────────────────────────

  SyncReport SyncOrchestrator::runRpcPhase(const OrchestratorConfig &config,
                                           Wallet  &wallet,
                                           uint32_t fromHeight,
                                           uint32_t toHeight)
  {
    SyncReport report;

    RpcScanConfig rpcCfg;
    rpcCfg.startHeight  = fromHeight;
    rpcCfg.endHeight    = toHeight;   // 0 = ask node
    rpcCfg.batchSize    = config.rpcBatchSize;
    rpcCfg.rpcTimeout   = config.rpcTimeout;
    rpcCfg.onProgress   = config.onRpcProgress;

    RpcBlockScanner scanner(m_node, m_viewKey, m_spendPub, m_spendKey);
    RpcScanResult   result = scanner.scan(rpcCfg, wallet);

    if (!result.ok)
    {
      report.error = result.error;
      return report;
    }

    report.ok             = true;
    report.rpcTip         = result.scannedTop;
    report.outputsFromRpc = result.outputsFound;
    return report;
  }

  // ── Reorg recovery ─────────────────────────────────────────────────────────

  void SyncOrchestrator::handleReorg(const OrchestratorConfig &config,
                                     Wallet  &wallet,
                                     uint32_t reorgHeight)
  {
    // Determine the safe rollback point: the lower of the reorg height and our DB tip.
    const uint32_t rollbackTo = std::min(reorgHeight, m_dbTip);

    if (config.onError)
      config.onError("Reorg detected (height " + std::to_string(reorgHeight)
                     + "). Rolling back to height " + std::to_string(rollbackTo));

    if (m_dbTip > 0 && !m_dbOutputs.empty())
    {
      // Reset to DB checkpoint — safe, consistent, no RPC calls needed.
      wallet.loadOutputs(m_dbOutputs, m_dbTip);
    }

    // Re-fill the RPC gap from rollbackTo+1 to current chain tip.
    const uint32_t rpcStart = rollbackTo + 1;
    runRpcPhase(config, wallet, rpcStart, 0);
  }

  // ── Live tail ──────────────────────────────────────────────────────────────

  void SyncOrchestrator::startTail(Wallet &wallet, uint32_t fromHeight, uint32_t pollMs)
  {
    stopTail();  // idempotent

    m_tailStop = false;
    m_tailThread = std::thread([this, &wallet, fromHeight, pollMs]()
    {
      // m_activeConfig carries the user's callbacks and settings from sync().
      uint32_t lastProcessed = (fromHeight > 0) ? fromHeight - 1 : 0;

      while (!m_tailStop.load(std::memory_order_relaxed))
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        if (m_tailStop.load(std::memory_order_relaxed))
          break;

        const uint32_t nodeTip = m_node.getLastKnownBlockHeight();
        if (nodeTip == 0)
          continue;

        // ── Reorg guard ─────────────────────────────────────────────────
        // If the node tip dropped below lastProcessed a reorg occurred.
        // Roll back to the DB checkpoint and re-scan the gap.
        if (nodeTip < lastProcessed)
        {
          handleReorg(m_activeConfig, wallet, nodeTip);
          lastProcessed = nodeTip;
          continue;
        }

        if (nodeTip <= lastProcessed)
          continue;  // nothing new yet

        // ── New blocks ───────────────────────────────────────────────────
        RpcScanConfig rpcCfg;
        rpcCfg.startHeight = lastProcessed + 1;
        rpcCfg.endHeight   = nodeTip;
        rpcCfg.batchSize   = m_activeConfig.rpcBatchSize;
        rpcCfg.rpcTimeout  = m_activeConfig.rpcTimeout;
        rpcCfg.onProgress  = m_activeConfig.onRpcProgress;

        RpcBlockScanner scanner(m_node, m_viewKey, m_spendPub, m_spendKey);
        RpcScanResult   result = scanner.scan(rpcCfg, wallet);

        if (result.ok)
          lastProcessed = result.scannedTop;
      }
    });
  }

  void SyncOrchestrator::stopTail()
  {
    m_tailStop = true;
    if (m_tailThread.joinable())
      m_tailThread.join();
  }

  bool SyncOrchestrator::isTailRunning() const
  {
    std::lock_guard<std::mutex> lock(m_tailMutex);
    return m_tailThread.joinable() && !m_tailStop.load(std::memory_order_relaxed);
  }

} // namespace BoltCore
