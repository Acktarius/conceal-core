// SidechainMenus.h — sidechain menu functions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Config.h"
#include <string>

namespace cn
{
  class Currency;
}

void sidechainTokensMenu(const Config &cfg);
void sidechainCreateTokenMenu(const Config &cfg, const std::string &spendPubHex);
void sidechainTransferMenu(const Config &cfg, const std::string &spendPubHex, cn::Currency &currency);
void sidechainBalanceMenu(const Config &cfg, const std::string &spendPubHex);
void sidechainStatusMenu(const Config &cfg);
void sidechainQuickCreateMenu(const Config &cfg, const std::string &spendPubHex);
void sidechainTxHistoryMenu();