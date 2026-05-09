// BridgeWatcher.h — watches main chain for CCX deposits and handles unlocks
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
#include <queue>

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
                  const crypto::PublicKey &bridgeSpendPub,
                  const crypto::SecretKey &bridgeSpendKey);

    ~BridgeWatcher();

    // Start watching for deposits
    void start(const DepositCallback &onDeposit);

    // Stop watching
    void stop();

    // Get total locked CCX for a specific token
    uint64_t getLockedAmount(uint64_t tokenId) const;

    // Get the bridge public key (for validator authorization)
    crypto::PublicKey getBridgePublicKey() const { return m_bridgeSpendPub; }

    // Handle a burn event from the sidechain — queue a CCX unlock
    void handleBurn(const Transaction &burnTx);

    // Process pending unlocks (called periodically)
    void processUnlocks();

    // Get pending unlock count
    size_t getPendingUnlockCount() const;

  private:
    void watchLoop(const DepositCallback &onDeposit);
    void unlockLoop();
    bool submitUnlockTransaction(const crypto::PublicKey &toAddress,
                                 uint64_t amount,
                                 const crypto::Hash &burnTxHash);
    std::string extractSidechainDestination(const std::vector<uint8_t> &extra) const;

    SidechainStorage &m_storage;
    cn::INode &m_node;
    crypto::PublicKey m_bridgeViewPub;
    crypto::SecretKey m_bridgeViewKey;
    crypto::PublicKey m_bridgeSpendPub;
    crypto::SecretKey m_bridgeSpendKey;
    bool m_hasSpendKey = false;

    std::thread m_watchThread;
    std::thread m_unlockThread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_lockedAmountsMutex;
    std::unordered_map<uint64_t, uint64_t> m_lockedAmounts; // tokenId -> amount

    // Pending unlocks queue
    struct PendingUnlock
    {
      crypto::PublicKey userAddress;
      uint64_t amount;
      crypto::Hash burnTxHash;
      uint64_t tokenId;
    };
    std::queue<PendingUnlock> m_pendingUnlocks;
    mutable std::mutex m_unlockMutex;
  };
}