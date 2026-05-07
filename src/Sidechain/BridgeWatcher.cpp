// BridgeWatcher implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BridgeWatcher.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include <chrono>

namespace Sidechain
{
  BridgeWatcher::BridgeWatcher(SidechainStorage &storage,
                               cn::INode &node,
                               const crypto::PublicKey &bridgeViewPub,
                               const crypto::SecretKey &bridgeViewKey,
                               const crypto::PublicKey &bridgeSpendPub)
      : m_storage(storage),
        m_node(node),
        m_bridgeViewPub(bridgeViewPub),
        m_bridgeViewKey(bridgeViewKey),
        m_bridgeSpendPub(bridgeSpendPub)
  {
  }

  BridgeWatcher::~BridgeWatcher()
  {
    stop();
  }

  void BridgeWatcher::start(const DepositCallback &onDeposit)
  {
    if (m_running)
      return;
    m_running = true;
    m_watchThread = std::thread(&BridgeWatcher::watchLoop, this, onDeposit);
  }

  void BridgeWatcher::stop()
  {
    m_running = false;
    if (m_watchThread.joinable())
      m_watchThread.join();
  }

  uint64_t BridgeWatcher::getLockedAmount(uint64_t tokenId) const
  {
    std::lock_guard<std::mutex> lock(m_lockedAmountsMutex);
    auto it = m_lockedAmounts.find(tokenId);
    return it != m_lockedAmounts.end() ? it->second : 0;
  }

  void BridgeWatcher::watchLoop(const DepositCallback &onDeposit)
  {
    uint32_t lastScannedHeight = 0;

    while (m_running)
    {
      try
      {
        // Get current chain height from daemon
        uint32_t currentHeight = m_node.getLastLocalBlockHeight();

        if (currentHeight > lastScannedHeight)
        {
          // Scan new blocks for deposits to the bridge address
          BoltSync::Scanner scanner(m_bridgeViewKey, m_bridgeViewPub, nullptr);
          BoltSync::ScanConfig scanCfg;
          scanCfg.startBlock = lastScannedHeight + 1;
          scanCfg.endBlock = currentHeight;

          BoltSync::ScanState state;
          if (scanner.scan(scanCfg, state))
          {
            for (const auto &output : state.results)
            {
              // This output was sent to the bridge address
              Transaction depositTx;
              depositTx.type = TransactionType::Mint;
              depositTx.from = m_bridgeSpendPub; // From bridge
              depositTx.amount = output.amount;
              depositTx.txHash = output.txHash;
              depositTx.extra.insert(
                  depositTx.extra.end(),
                  output.txPublicKey.data,
                  output.txPublicKey.data + sizeof(crypto::PublicKey));

              if (onDeposit)
                onDeposit(depositTx);
            }
          }

          lastScannedHeight = currentHeight;
        }
      }
      catch (...)
      {
        // Ignore errors, retry on next iteration
      }

      // Poll every 5 seconds
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }
}