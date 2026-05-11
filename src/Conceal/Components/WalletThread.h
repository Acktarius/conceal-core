// WalletThread.h — wallet RPC wrapper
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "../CLI.h"
#include "../StatusPanel.h"

namespace Conceal
{
  void runWallet(const Config &cfg, WalletStatus &status, std::atomic<bool> &stopRequested);
} // namespace Conceal