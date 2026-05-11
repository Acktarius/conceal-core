// StatusPanel.h — shared status structs for the unified Conceal client
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <mutex>

namespace Conceal
{

  struct MainchainStatus
  {
    std::atomic<uint32_t> localHeight{0};
    std::atomic<uint32_t> networkHeight{0};
    std::atomic<size_t> peerCount{0};
    std::atomic<bool> synced{false};
    std::string topBlockHash;
    mutable std::mutex hashMutex;

    void setHash(const std::string &h)
    {
      std::lock_guard<std::mutex> lock(hashMutex);
      topBlockHash = h;
    }
    std::string getHash() const
    {
      std::lock_guard<std::mutex> lock(hashMutex);
      return topBlockHash;
    }
  };

  struct SidechainStatus
  {
    std::atomic<uint64_t> height{0};
    std::atomic<size_t> validatorCount{0};
    std::atomic<size_t> pendingTxCount{0};
    std::atomic<size_t> tokenCount{0};
    std::atomic<double> dexFee{0.0};
    std::atomic<bool> bridgeWatching{false};
    std::atomic<size_t> pendingUnlocks{0};
  };

  struct WalletStatus
  {
    std::atomic<uint64_t> availableBalance{0};
    std::atomic<uint64_t> lockedBalance{0};
    std::atomic<uint32_t> walletHeight{0};
    std::atomic<bool> synced{false};
    std::string address;
    mutable std::mutex addrMutex;

    void setAddress(const std::string &a)
    {
      std::lock_guard<std::mutex> lock(addrMutex);
      address = a;
    }
    std::string getAddress() const
    {
      std::lock_guard<std::mutex> lock(addrMutex);
      return address;
    }
  };

} // namespace Conceal