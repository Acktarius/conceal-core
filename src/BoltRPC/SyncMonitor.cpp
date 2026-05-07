#include "SyncMonitor.h"
#include <chrono>
#include <iostream>

namespace BoltRPC
{

  SyncMonitor::SyncMonitor(cn::INode &node,
                           const crypto::SecretKey &viewKey,
                           const crypto::PublicKey &viewPub,
                           const crypto::SecretKey *spendKey,
                           const std::string &dataDir,
                           uint32_t lastScannedHeight,
                           OutputCallback onNewOutputs)
      : m_node(node), m_viewKey(viewKey), m_viewPub(viewPub), m_spendKey(spendKey), m_dataDir(dataDir), m_lastScannedHeight(lastScannedHeight), m_onNewOutputs(std::move(onNewOutputs))
  {
  }

  SyncMonitor::~SyncMonitor() { stop(); }

  void SyncMonitor::start()
  {
    m_stop = false;
    m_thread = std::thread(&SyncMonitor::runLoop, this);
  }

  void SyncMonitor::stop()
  {
    m_stop = true;
    if (m_thread.joinable())
      m_thread.join();
  }

  void SyncMonitor::runLoop()
  {
    while (!m_stop)
    {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(POLL_INTERVAL_MS));

      if (m_stop)
        break;
      if (m_dataDir.empty())
        continue;

      uint32_t nodeHeight = m_node.getLastLocalBlockHeight();
      uint32_t lastScanned = m_lastScannedHeight.load(std::memory_order_relaxed);

      if (nodeHeight <= lastScanned + MIN_BLOCKS_TO_SCAN)
        continue;

      // New blocks available — run BoltSync on the new range only
      BoltSync::Scanner scanner(m_viewKey, m_viewPub, m_spendKey);

      BoltSync::ScanConfig cfg;
      cfg.dataDir = m_dataDir;
      cfg.numThreads = 1; // incremental scans are small, 1 thread is fine
      cfg.startBlock = lastScanned + 1;
      cfg.endBlock = nodeHeight;

      BoltSync::ScanState state;
      if (!scanner.scan(cfg, state))
      {
        std::cerr << "SyncMonitor: incremental scan failed "
                  << "(range " << cfg.startBlock << "-" << cfg.endBlock << ")"
                  << std::endl;
        continue;
      }

      if (!state.results.empty())
      {
        // Convert BoltSync results to BoltCore OutputInfo
        std::vector<BoltCore::OutputInfo> newOutputs;
        newOutputs.reserve(state.results.size());
        for (const auto &fo : state.results)
        {
          BoltCore::OutputInfo info;
          info.blockHeight = fo.blockHeight;
          info.txHash = fo.txHash;
          info.outputIndex = fo.outputIndex;
          info.globalOutputIndex = fo.outputIndex; // TODO: resolve via node
          info.amount = fo.amount;
          info.outputKey = fo.outputKey;
          info.txPublicKey = fo.txPublicKey;
          info.keyImage = fo.keyImage;
          info.spent = fo.spent;
          info.isDeposit = false;
          info.term = 0;
          newOutputs.push_back(std::move(info));
        }

        m_onNewOutputs(newOutputs, nodeHeight);
      }

      m_lastScannedHeight.store(nodeHeight, std::memory_order_relaxed);
    }
  }

} // namespace BoltRPC