// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "BoltCore/BoltCore.h"
#include "BoltSync/BoltSync.h"
#include "INode.h"

namespace BoltRPC
{

  // Runs in a background thread, polls the node for new blocks,
  // and calls onNewOutputs when new outputs are found.
  class SyncMonitor
  {
  public:
    using OutputCallback = std::function<void(
        const std::vector<BoltCore::OutputInfo> &, uint32_t newHeight)>;

    SyncMonitor(cn::INode &node,
                const crypto::SecretKey &viewKey,
                const crypto::PublicKey &viewPub,
                const crypto::SecretKey *spendKey,
                const std::string &dataDir,
                uint32_t lastScannedHeight,
                OutputCallback onNewOutputs);

    ~SyncMonitor();

    void start();
    void stop();

    uint32_t lastScannedHeight() const
    {
      return m_lastScannedHeight.load(std::memory_order_relaxed);
    }

  private:
    void runLoop();

    cn::INode &m_node;
    crypto::SecretKey m_viewKey;
    crypto::PublicKey m_viewPub;
    const crypto::SecretKey *m_spendKey;
    std::string m_dataDir;
    std::atomic<uint32_t> m_lastScannedHeight;
    OutputCallback m_onNewOutputs;

    std::atomic<bool> m_stop{false};
    std::thread m_thread;

    static constexpr uint32_t POLL_INTERVAL_MS = 5000;
    static constexpr uint32_t MIN_BLOCKS_TO_SCAN = 1;
  };

} // namespace BoltRPC