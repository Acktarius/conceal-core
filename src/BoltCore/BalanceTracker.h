// BalanceTracker - wallet balance calculation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace BoltCore
{
  class BalanceTracker
  {
  public:
    struct PendingTx
    {
      crypto::Hash txHash;
      uint64_t amount;
      uint64_t fee;
      uint64_t timestamp;
    };

    void loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight = 0);

    Balance getTotalBalance() const;
    Balance getBalance(const std::string &address) const;

    void addOutput(const OutputInfo &output);
    bool markSpent(const crypto::KeyImage &keyImage);
    bool markDepositSpent(const crypto::Hash &txHash, uint32_t outputIndex);

    void setCurrentHeight(uint32_t height);
    uint32_t getCurrentHeight() const;

    uint64_t totalActual() const;
    uint64_t totalPending() const;

    std::vector<OutputInfo> getOutputs() const;

    void addPendingOutgoing(const crypto::Hash &txHash, uint64_t amount, uint64_t fee);
    void confirmTransaction(const crypto::Hash &txHash, uint32_t blockHeight);
    uint64_t getPendingOutgoingAmount() const;
    std::vector<PendingTx> getPendingTransactions() const;

    void addTransaction(const TransactionRecord &tx);
    std::vector<TransactionRecord> getTransactions(uint32_t offset = 0, uint32_t limit = 50) const;
    uint32_t getTransactionCount() const;

  private:
    struct AddressBalance
    {
      uint64_t actual = 0;
      uint64_t pending = 0;
      uint64_t lockedDeposit = 0;
      uint64_t unlockedDeposit = 0;
    };

    bool outputExists(const crypto::Hash &txHash, uint32_t outputIndex) const;
    void subtractFromBalance(const OutputInfo &out);
    void refreshDepositBuckets();

    mutable std::recursive_mutex m_dataMutex;
    mutable std::mutex m_pendingMutex;
    std::unordered_map<crypto::Hash, PendingTx> m_pendingOutgoing;
    uint64_t m_pendingOutgoingAmount = 0;
    uint32_t m_currentHeight = 0;
    std::vector<OutputInfo> m_outputs;
    std::unordered_map<std::string, AddressBalance> m_byAddress;
    Balance m_total;
    std::vector<TransactionRecord> m_transactions;
  };
}
