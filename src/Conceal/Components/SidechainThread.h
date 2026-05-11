// SidechainThread.h — sidechain validator wrapper
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "../CLI.h"
#include "../StatusPanel.h"

namespace Conceal
{
  void runSidechain(const Config &cfg, SidechainStatus &status, std::atomic<bool> &stopRequested);
} // namespace Conceal