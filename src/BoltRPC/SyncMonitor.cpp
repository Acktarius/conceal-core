// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SyncMonitor.h"
#include <chrono>
#include <iostream>

namespace BoltRPC
{

// Out-of-class definitions required by C++11 for static constexpr members that are ODR-used
constexpr uint32_t SyncMonitor::POLL_INTERVAL_MS;
constexpr uint32_t SyncMonitor::MIN_BLOCKS_TO_SCAN;

  SyncMonitor::SyncMonitor(cn::INode &node,
                           const crypto::SecretKey &viewKey,
                           const crypto::PublicKey &spendPub,
                           const crypto::SecretKey *spendKey,
                           const std::string &dataDir,
                           uint32_t lastScannedHeight,
                           OutputCallback onNewOutputs)
      : m_node(node), m_viewKey(viewKey), m_spendPub(spendPub), m_spendKey(spendKey), m_dataDir(dataDir), m_lastScannedHeight(lastScannedHeight), m_onNewOutputs(std::move(onNewOutputs))
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
    // ── Initial full scan if starting from 0 ──
    if (m_lastScannedHeight.load(std::memory_order_relaxed) == 0 && !m_dataDir.empty())
    {
      uint32_t nodeHeight = m_node.getLastKnownBlockHeight();
      if (nodeHeight == 0)
        nodeHeight = m_node.getLastLocalBlockHeight();

      if (nodeHeight > 0)
      {
        std::cerr << "SyncMonitor: starting initial scan 0-" << nodeHeight << std::endl;

        BoltSync::Scanner scanner(m_viewKey, m_spendPub, m_spendKey);
        BoltSync::ScanConfig cfg;
        cfg.dataDir = m_dataDir;
        cfg.numThreads = 1;
        cfg.startBlock = 0;
        cfg.endBlock = nodeHeight;
        cfg.onProgress = [this](uint32_t h)
        {
          m_lastScannedHeight.store(h, std::memory_order_relaxed);
        };

        BoltSync::ScanState state;
        if (scanner.scan(cfg, state))
        {
          if (!state.results.empty())
          {
            std::vector<BoltCore::OutputInfo> newOutputs;
            newOutputs.reserve(state.results.size());
            for (const auto &fo : state.results)
            {
              BoltCore::OutputInfo info;
              info.blockHeight = fo.blockHeight;
              info.txHash = fo.txHash;
              info.outputIndex = fo.outputIndex;
              info.globalOutputIndex = fo.outputIndex;
              info.amount = fo.amount;
              info.outputKey = fo.outputKey;
              info.txPublicKey = fo.txPublicKey;
              info.keyImage = fo.keyImage;
              info.spent = fo.spent;
              info.isDeposit = fo.isDeposit;
              info.term = fo.term;
              newOutputs.push_back(std::move(info));
            }
            m_onNewOutputs(newOutputs, nodeHeight);
          }
          m_lastScannedHeight.store(nodeHeight, std::memory_order_relaxed);
        }
      }
    }

    // ── Incremental polling loop with block detection ──
    while (!m_stop)
    {
      uint32_t lastScanned = m_lastScannedHeight.load(std::memory_order_relaxed);
      uint32_t nodeHeight = m_node.getLastKnownBlockHeight();
      if (nodeHeight == 0)
        nodeHeight = m_node.getLastLocalBlockHeight();

      // If we're at the tip, wait for new blocks
      if (lastScanned >= nodeHeight && nodeHeight > 0)
      {
        uint32_t currentHeight = nodeHeight;

        // Wait for node height to increase (no fixed sleep)
        while (!m_stop)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(500));

          uint32_t newHeight = m_node.getLastKnownBlockHeight();
          if (newHeight == 0)
            newHeight = m_node.getLastLocalBlockHeight();

          if (newHeight > currentHeight)
          {
            nodeHeight = newHeight;
            break;
          }
        }

        if (m_stop)
          break;
      }

      // New blocks available - scan them
      if (m_dataDir.empty())
        continue;

      uint32_t lastScannedNow = m_lastScannedHeight.load(std::memory_order_relaxed);

      if (nodeHeight <= lastScannedNow + MIN_BLOCKS_TO_SCAN)
        continue;

      BoltSync::Scanner scanner(m_viewKey, m_spendPub, m_spendKey);
      BoltSync::ScanConfig cfg;
      cfg.dataDir = m_dataDir;
      cfg.numThreads = 1;
      cfg.startBlock = lastScanned + 1;
      cfg.endBlock = nodeHeight;
      cfg.onProgress = [this](uint32_t h)
      {
        m_lastScannedHeight.store(h, std::memory_order_relaxed);
      };

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
        std::vector<BoltCore::OutputInfo> newOutputs;
        newOutputs.reserve(state.results.size());
        for (const auto &fo : state.results)
        {
          BoltCore::OutputInfo info;
          info.blockHeight = fo.blockHeight;
          info.txHash = fo.txHash;
          info.outputIndex = fo.outputIndex;
          info.globalOutputIndex = fo.outputIndex;
          info.amount = fo.amount;
          info.outputKey = fo.outputKey;
          info.txPublicKey = fo.txPublicKey;
          info.keyImage = fo.keyImage;
          info.spent = fo.spent;
          info.isDeposit = fo.isDeposit;
          info.term = fo.term;
          newOutputs.push_back(std::move(info));
        }
        m_onNewOutputs(newOutputs, nodeHeight);
      }

      m_lastScannedHeight.store(nodeHeight, std::memory_order_relaxed);
    }
  }
} // namespace BoltRPC