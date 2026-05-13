// BalanceTracker - wallet balance calculation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace BoltCore
{
  class BalanceTracker
  {
  public:
    void loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight = 0);

    Balance getTotalBalance() const;
    Balance getBalance(const std::string &address) const;

    void addOutput(const OutputInfo &output);
    void markSpent(const crypto::KeyImage &keyImage);
    void setCurrentHeight(uint32_t height) { m_currentHeight = height; }

    uint64_t totalActual() const;
    uint64_t totalPending() const;

    const std::vector<OutputInfo> &getOutputs() const { return m_outputs; }

  private:
    struct AddressBalance
    {
      uint64_t actual = 0;
      uint64_t pending = 0;
      uint64_t lockedDeposit = 0;
      uint64_t unlockedDeposit = 0;
    };

    uint32_t m_currentHeight = 0;
    std::vector<OutputInfo> m_outputs;
    std::unordered_map<std::string, AddressBalance> m_byAddress;
    Balance m_total;
  };
}