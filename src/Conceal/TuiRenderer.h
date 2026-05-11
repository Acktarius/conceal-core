// TuiRenderer.h — FTXUI dashboard renderer
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "StatusPanel.h"
#include <ftxui/dom/elements.hpp>
#include <atomic>

namespace Conceal
{

  ftxui::Element renderDashboard(
      const MainchainStatus &mainchain,
      const SidechainStatus &sidechain,
      const WalletStatus &wallet,
      std::atomic<bool> &quit);

} // namespace Conceal