// BalanceTracker implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BalanceTracker.h"

namespace BoltCore
{
  void BalanceTracker::loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight)
  {
    m_currentHeight = currentHeight;
    m_outputs.clear();
    m_byAddress.clear();
    m_total = {0, 0, 0, 0};

    for (const auto &out : outputs)
    {
      addOutput(out);
    }
  }

  void BalanceTracker::addOutput(const OutputInfo &out)
  {
    m_outputs.push_back(out);

    if (out.spent)
      return;

    Balance &total = m_total;
    AddressBalance &addr = m_byAddress[out.subAddress];

    if (out.isDeposit)
    {
      // A deposit is locked until blockHeight + term <= currentHeight (reference: m_currentHeight + 1 >= blockHeight + term)
      bool unlocked = (out.term > 0) && (m_currentHeight + 1 >= out.blockHeight + out.term);
      if (unlocked)
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

  void BalanceTracker::markSpent(const crypto::KeyImage &keyImage)
  {
    for (auto &out : m_outputs)
    {
      if (out.keyImage == keyImage && !out.spent)
      {
        out.spent = true;
        Balance &total = m_total;
        AddressBalance &addr = m_byAddress[out.subAddress];

        if (out.isDeposit)
        {
          addr.lockedDeposit -= out.amount;
          total.lockedDeposit -= out.amount;
        }
        else
        {
          addr.actual -= out.amount;
          total.actual -= out.amount;
        }
        return;
      }
    }
  }

  Balance BalanceTracker::getTotalBalance() const
  {
    return m_total;
  }

  Balance BalanceTracker::getBalance(const std::string &address) const
  {
    auto it = m_byAddress.find(address);
    if (it != m_byAddress.end())
    {
      return {it->second.actual, it->second.pending,
              it->second.lockedDeposit, it->second.unlockedDeposit};
    }
    return {0, 0, 0, 0};
  }

  uint64_t BalanceTracker::totalActual() const
  {
    return m_total.actual;
  }

  uint64_t BalanceTracker::totalPending() const
  {
    return m_total.pending;
  }
}