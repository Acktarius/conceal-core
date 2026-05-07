// BridgeWatcher.h — watches main chain for CCX deposits
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "SidechainTypes.h"
#include "SidechainStorage.h"
#include "BoltSync/BoltSync.h"
#include "INode.h"
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>

namespace Sidechain
{
  class BridgeWatcher
  {
  public:
    using DepositCallback = std::function<void(const Transaction &depositTx)>;

    BridgeWatcher(SidechainStorage &storage,
                  cn::INode &node,
                  const crypto::PublicKey &bridgeViewPub,
                  const crypto::SecretKey &bridgeViewKey,
                  const crypto::PublicKey &bridgeSpendPub);

    ~BridgeWatcher();

    // Start watching for deposits
    void start(const DepositCallback &onDeposit);

    // Stop watching
    void stop();

    // Get total locked CCX for a specific token
    uint64_t getLockedAmount(uint64_t tokenId) const;

  private:
    void watchLoop(const DepositCallback &onDeposit);

    SidechainStorage &m_storage;
    cn::INode &m_node;
    crypto::PublicKey m_bridgeViewPub;
    crypto::SecretKey m_bridgeViewKey;
    crypto::PublicKey m_bridgeSpendPub;

    std::thread m_watchThread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_lockedAmountsMutex;
    std::unordered_map<uint64_t, uint64_t> m_lockedAmounts; // tokenId -> amount
  };
}