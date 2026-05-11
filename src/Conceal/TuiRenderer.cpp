// TuiRenderer.cpp — FTXUI dashboard implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "TuiRenderer.h"
#include <sstream>
#include <iomanip>

using namespace ftxui;

namespace Conceal
{

  namespace
  {
    std::string formatAmount(uint64_t amount)
    {
      double ccx = static_cast<double>(amount) / 1000000.0;
      std::ostringstream ss;
      ss << std::fixed << std::setprecision(2) << ccx;
      return ss.str();
    }

    std::string formatHeight(uint32_t height)
    {
      std::ostringstream ss;
      ss << std::fixed << std::setprecision(0) << height;
      return ss.str();
    }
  }

  Element renderDashboard(
      const MainchainStatus &mainchain,
      const SidechainStatus &sidechain,
      const WalletStatus &wallet,
      std::atomic<bool> &quit)
  {
    // Title
    auto title = text(" CONCEAL NETWORK ") | bold | center | border;

    // Mainchain panel
    float mainProgress = mainchain.networkHeight > 0
                             ? static_cast<float>(mainchain.localHeight) / mainchain.networkHeight
                             : 0.0f;

    auto mainPanel = vbox({text(" Mainchain ") | bold,
                           separator(),
                           text("Height: " + formatHeight(mainchain.localHeight) +
                                " / " + formatHeight(mainchain.networkHeight)),
                           gauge(mainProgress) | color(mainchain.synced ? Color::Green : Color::Yellow),
                           text("Peers: " + std::to_string(mainchain.peerCount) +
                                "  Backend: " + (mainchain.localHeight > 0 ? "MDBX" : "SwappedVector")),
                           text(mainchain.synced ? " Status: Synced" : " Status: Syncing...")}) |
                     border;

    // Sidechain panel
    auto scPanel = vbox({text(" Sidechain ") | bold,
                         separator(),
                         text("Height: " + std::to_string(sidechain.height.load())),
                         text("Validators: " + std::to_string(sidechain.validatorCount.load()) +
                              "  Tokens: " + std::to_string(sidechain.tokenCount.load())),
                         text("DEX Fee: " + std::to_string(sidechain.dexFee.load()) + "%"),
                         text(sidechain.bridgeWatching ? " Bridge: watching" : " Bridge: disabled")}) |
                   border;

    // Wallet panel
    std::string walletHeightText;
    if (wallet.walletHeight == 0 && !wallet.synced)
      walletHeightText = "No transactions yet";
    else
      walletHeightText = "Height: " + formatHeight(wallet.walletHeight) +
                         (wallet.synced ? " (synced)" : " (syncing...)");

    auto walletPanel = vbox({text(" Wallet ") | bold,
                             separator(),
                             text("Balance: " + formatAmount(wallet.availableBalance) + " CCX"),
                             text("Locked:  " + formatAmount(wallet.lockedBalance) + " CCX"),
                             text(walletHeightText),
                             text("Address: " + wallet.getAddress().substr(0, 16) + "...")}) |
                       border;

    // Bottom bar
    auto bottom = hbox({text(" q: Quit ") | dim,
                        text(" h: Help ") | dim,
                        text(" s: Save ") | dim}) |
                  center;

    return vbox({title,
                 hbox({mainPanel, scPanel, walletPanel}),
                 bottom}) |
           border;
  }

} // namespace Conceal