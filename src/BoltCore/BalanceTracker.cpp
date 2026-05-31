// BalanceTracker implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BalanceTracker.h"
#include "OutputUtils.h"

#include <ctime>
#include <cstring>

namespace BoltCore
{
  bool BalanceTracker::outputExists(const crypto::Hash &txHash, uint32_t outputIndex) const
  {
    for (const auto &existing : m_outputs)
    {
      if (existing.txHash == txHash && existing.outputIndex == outputIndex)
        return true;
    }
    return false;
  }

  void BalanceTracker::refreshDepositBuckets()
  {
    m_total.lockedDeposit = 0;
    m_total.unlockedDeposit = 0;
    for (auto &pair : m_byAddress)
    {
      pair.second.lockedDeposit = 0;
      pair.second.unlockedDeposit = 0;
    }

    for (const auto &out : m_outputs)
    {
      if (out.spent || !out.isDeposit)
        continue;

      AddressBalance &addr = m_byAddress[out.subAddress];
      if (isDepositUnlocked(out, m_currentHeight))
      {
        addr.unlockedDeposit += out.amount;
        m_total.unlockedDeposit += out.amount;
      }
      else
      {
        addr.lockedDeposit += out.amount;
        m_total.lockedDeposit += out.amount;
      }
    }
  }

  void BalanceTracker::subtractFromBalance(const OutputInfo &out)
  {
    Balance &total = m_total;
    AddressBalance &addr = m_byAddress[out.subAddress];

    if (out.isDeposit)
    {
      if (isDepositUnlocked(out, m_currentHeight))
      {
        addr.unlockedDeposit -= out.amount;
        total.unlockedDeposit -= out.amount;
      }
      else
      {
        addr.lockedDeposit -= out.amount;
        total.lockedDeposit -= out.amount;
      }
    }
    else
    {
      addr.actual -= out.amount;
      total.actual -= out.amount;
    }
  }

  void BalanceTracker::loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    m_currentHeight = currentHeight;
    m_outputs.clear();
    m_byAddress.clear();
    m_transactions.clear();
    m_total = {0, 0, 0, 0, 0, 0, currentHeight};

    for (const auto &out : outputs)
    {
      addOutput(out);
    }
  }

  void BalanceTracker::addOutput(const OutputInfo &out)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    if (outputExists(out.txHash, out.outputIndex))
      return;

    m_outputs.push_back(out);

    if (out.spent)
      return;

    Balance &total = m_total;
    AddressBalance &addr = m_byAddress[out.subAddress];

    if (out.isDeposit)
    {
      if (isDepositUnlocked(out, m_currentHeight))
      {
        addr.unlockedDeposit += out.amount;
        total.unlockedDeposit += out.amount;
      }
      else
      {
        addr.lockedDeposit += out.amount;
        total.lockedDeposit += out.amount;
      }
    }
    else
    {
      addr.actual += out.amount;
      total.actual += out.amount;
    }
  }

  bool BalanceTracker::markSpent(const crypto::KeyImage &keyImage)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    static const crypto::KeyImage NULL_KI = {};
    if (std::memcmp(&keyImage, &NULL_KI, sizeof(crypto::KeyImage)) == 0)
      return false;

    for (auto &out : m_outputs)
    {
      if (out.keyImage == keyImage && !out.spent)
      {
        out.spent = true;
        subtractFromBalance(out);
        return true;
      }
    }
    return false;
  }

  bool BalanceTracker::markDepositSpent(const crypto::Hash &txHash, uint32_t outputIndex)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (auto &out : m_outputs)
    {
      if (out.txHash == txHash && out.outputIndex == outputIndex && out.isDeposit && !out.spent)
      {
        out.spent = true;
        subtractFromBalance(out);
        return true;
      }
    }
    return false;
  }

  void BalanceTracker::setCurrentHeight(uint32_t height)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    if (height == m_currentHeight)
      return;
    m_currentHeight = height;
    refreshDepositBuckets();
  }

  uint32_t BalanceTracker::getCurrentHeight() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return m_currentHeight;
  }

  Balance BalanceTracker::getTotalBalance() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    Balance balance = m_total;
    balance.currentHeight = m_currentHeight;
    return balance;
  }

  Balance BalanceTracker::getBalance(const std::string &address) const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    auto it = m_byAddress.find(address);
    if (it != m_byAddress.end())
    {
      return {it->second.actual, it->second.pending,
              it->second.lockedDeposit, it->second.unlockedDeposit, 0, 0, m_currentHeight};
    }
    return {0, 0, 0, 0, 0, 0, m_currentHeight};
  }

  uint64_t BalanceTracker::totalActual() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return m_total.actual;
  }

  uint64_t BalanceTracker::totalPending() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return m_total.pending;
  }

  std::vector<OutputInfo> BalanceTracker::getOutputs() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return m_outputs;
  }

  void BalanceTracker::addPendingOutgoing(const crypto::Hash &txHash, uint64_t amount, uint64_t fee)
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    PendingTx pending;
    pending.txHash = txHash;
    pending.amount = amount;
    pending.fee = fee;
    pending.timestamp = std::time(nullptr);

    m_pendingOutgoing[txHash] = pending;
    m_pendingOutgoingAmount += amount + fee;
  }

  void BalanceTracker::confirmTransaction(const crypto::Hash &txHash, uint32_t blockHeight)
  {
    (void)blockHeight;
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    auto it = m_pendingOutgoing.find(txHash);
    if (it != m_pendingOutgoing.end())
    {
      m_pendingOutgoingAmount -= it->second.amount + it->second.fee;
      m_pendingOutgoing.erase(it);
    }
  }

  uint64_t BalanceTracker::getPendingOutgoingAmount() const
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return m_pendingOutgoingAmount;
  }

  std::vector<BalanceTracker::PendingTx> BalanceTracker::getPendingTransactions() const
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    std::vector<PendingTx> result;
    result.reserve(m_pendingOutgoing.size());
    for (const auto &pair : m_pendingOutgoing)
    {
      result.push_back(pair.second);
    }
    return result;
  }

  void BalanceTracker::addTransaction(const TransactionRecord &tx)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (const auto &existing : m_transactions)
    {
      if (existing.txHash == tx.txHash)
        return;
    }
    m_transactions.push_back(tx);
  }

  std::vector<TransactionRecord> BalanceTracker::getTransactions(uint32_t offset, uint32_t limit) const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    std::vector<TransactionRecord> result;
    if (offset >= m_transactions.size())
      return result;

    uint32_t end = std::min(offset + limit, static_cast<uint32_t>(m_transactions.size()));
    result.assign(m_transactions.begin() + offset, m_transactions.begin() + end);
    return result;
  }

  uint32_t BalanceTracker::getTransactionCount() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return static_cast<uint32_t>(m_transactions.size());
  }
}
