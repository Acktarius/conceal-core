// SidechainMenus.cpp — sidechain menu functions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SidechainMenus.h"
#include "TxHistory.h"
#include "JsonHelpers.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include <iostream>
#include <sstream>

void sidechainTokensMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== Sidechain Tokens ===" << std::endl
            << std::endl;

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getStatus", "{}");
  uint64_t height = extractJsonNumber(result, "height");
  uint64_t tokenCount = extractJsonNumber(result, "tokenCount");
  uint64_t pendingTx = extractJsonNumber(result, "pendingTransactions");

  std::cout << "Network Summary:" << std::endl;
  std::cout << "  Height: " << height << std::endl;
  std::cout << "  Tokens: " << tokenCount << std::endl;
  std::cout << "  Pending: " << pendingTx << std::endl;
  std::cout << std::endl;

  result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokens", "{}");

  std::cout << "Tokens:" << std::endl;
  size_t arrayStart = result.find("\"result\":[");
  if (arrayStart == std::string::npos)
  {
    std::cout << "  None found." << std::endl;
  }
  else
  {
    arrayStart += 10;
    std::string arrayContent = result.substr(arrayStart);

    size_t pos = 0;
    int count = 0;
    while ((pos = arrayContent.find("\"id\":", pos)) != std::string::npos)
    {
      uint64_t id = extractJsonNumber(arrayContent.substr(pos), "id");
      std::string name = extractJsonString(arrayContent.substr(pos), "name");
      std::string symbol = extractJsonString(arrayContent.substr(pos), "symbol");
      uint64_t supply = extractJsonNumber(arrayContent.substr(pos), "totalSupply");
      uint64_t maxSupply = extractJsonNumber(arrayContent.substr(pos), "maxSupply");
      uint64_t decimals = extractJsonNumber(arrayContent.substr(pos), "decimals");
      uint64_t model = extractJsonNumber(arrayContent.substr(pos), "backingModel");
      uint64_t backingRatio = extractJsonNumber(arrayContent.substr(pos), "backingRatio");
      uint64_t lockedCCX = extractJsonNumber(arrayContent.substr(pos), "lockedCCXAmount");
      std::string sourceChain = extractJsonString(arrayContent.substr(pos), "sourceChain");
      bool verified = extractJsonString(arrayContent.substr(pos), "verified") == "true";

      if (!name.empty() && !symbol.empty())
      {
        std::string modelName = getBackingModelName(static_cast<uint8_t>(model));
        std::cout << "  " << symbol << " (" << name << ")" << std::endl;
        std::cout << "    ID: " << id << " | Supply: " << formatAmount(supply, decimals)
                  << " | Max: " << formatAmount(maxSupply, decimals)
                  << " | Decimals: " << decimals << " | " << modelName << std::endl;

        if (model > 0)
        {
          std::cout << "    Backing Ratio: " << backingRatio << "%";
          if (lockedCCX > 0)
            std::cout << " | Locked CCX: " << formatAmount(lockedCCX, 6);
          std::cout << std::endl;
        }

        if (!sourceChain.empty())
        {
          std::cout << "    Source: " << sourceChain;
          if (verified)
            std::cout << " [verified]";
          std::cout << std::endl;
        }
      }
      pos++;
      count++;
    }
    if (count == 0)
      std::cout << "  None found." << std::endl;
  }

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainCreateTokenMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Create Token ===" << std::endl
            << std::endl;
  std::cout << "Backing models:" << std::endl;
  std::cout << "  0 = Unbacked — free to create, no CCX needed" << std::endl;
  std::cout << "  1 = Fully Backed — CCX locked 1:1 as collateral" << std::endl;
  std::cout << "  2 = Hybrid — CCX-backed with configurable ratio" << std::endl;
  std::cout << std::endl;

  std::string name, symbol;
  uint64_t initialSupply;
  int backingModel;
  int decimals = 6;
  uint64_t backingRatio = 100;
  uint64_t lockedCCXAmount = 0;

  std::cout << "Token name: ";
  std::getline(std::cin, name);
  if (name.empty())
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Token symbol: ";
  std::getline(std::cin, symbol);
  if (symbol.empty())
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Initial supply (atomic units): ";
  std::cin >> initialSupply;
  if (initialSupply == 0)
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.ignore();
    std::cin.get();
    return;
  }

  std::cout << "Decimals (default 6): ";
  std::cin >> decimals;
  if (decimals > 18)
    decimals = 6;

  std::cout << "Backing model (0=Unbacked, 1=Fully Backed, 2=Hybrid): ";
  std::cin >> backingModel;
  std::cin.ignore();

  if (backingModel < 0 || backingModel > 2)
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  if (backingModel == 1 || backingModel == 2)
  {
    std::cout << std::endl
              << "Backing configuration:" << std::endl;

    if (backingModel == 2)
    {
      std::cout << "Backing ratio (% — default 100): ";
      std::string ratioStr;
      std::getline(std::cin, ratioStr);
      if (!ratioStr.empty())
        backingRatio = std::stoull(ratioStr);
    }

    std::cout << "Locked CCX amount (atomic units, 0 to skip): ";
    std::cin >> lockedCCXAmount;
    std::cin.ignore();

    if (lockedCCXAmount == 0)
    {
      std::cout << "Warning: Creating a backed token without locked CCX. It can be funded later via bridge Mint." << std::endl;
      std::cout << "Continue? (y/n): ";
      std::string cont;
      std::getline(std::cin, cont);
      if (cont != "y" && cont != "Y")
      {
        std::cout << "Cancelled." << std::endl;
        std::cin.get();
        return;
      }
    }
  }

  std::cout << std::endl
            << "Confirm:" << std::endl;
  std::cout << "  Name: " << name << std::endl;
  std::cout << "  Symbol: " << symbol << std::endl;
  std::cout << "  Supply: " << initialSupply << std::endl;
  std::cout << "  Decimals: " << decimals << std::endl;
  std::cout << "  Model: " << getBackingModelName(static_cast<uint8_t>(backingModel)) << std::endl;

  if (backingModel > 0)
  {
    std::cout << "  Backing Ratio: " << backingRatio << "%" << std::endl;
    std::cout << "  Locked CCX: " << formatAmount(lockedCCXAmount, 6) << std::endl;
  }

  std::cout << "Create? (y/n): ";

  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::string nameHex = common::toHex(common::asBinaryArray(name));
  std::string symbolHex = common::toHex(common::asBinaryArray(symbol));

  std::ostringstream params;
  params << "{"
         << R"("from":")" << spendPubHex << R"(")"
         << R"(,"nameHex":")" << nameHex << R"(")"
         << R"(,"symbolHex":")" << symbolHex << R"(")"
         << R"(,"initialSupply":)" << initialSupply
         << R"(,"backingModel":)" << backingModel
         << R"(,"decimals":)" << decimals;
  if (backingModel > 0)
  {
    params << R"(,"backingRatio":)" << backingRatio
           << R"(,"lockedCCXAmount":)" << lockedCCXAmount;
  }
  params << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "createToken", params.str());
  std::cout << std::endl;

  if (result.find("true") != std::string::npos)
  {
    std::cout << "Token " << symbol << " created!" << std::endl;
    loadTokenCache(cfg.sidechainHost, cfg.sidechainPort);
  }
  else
    std::cout << "Failed to create token." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void sidechainTransferMenu(const Config &cfg, const std::string &spendPubHex, cn::Currency &currency)
{
  clearScreen();
  std::cout << "=== Send Tokens ===" << std::endl
            << std::endl;

  std::string to;
  uint64_t amount, tokenId;

  std::cout << "Send to (address or public key): ";
  std::getline(std::cin, to);
  if (to.empty())
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::string toPubHex = addressToHexPubKey(to, currency);

  std::cout << "Amount (atomic units): ";
  std::cin >> amount;
  if (amount == 0)
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.ignore();
    std::cin.get();
    return;
  }

  std::cout << std::endl
            << "Available tokens:" << std::endl;

  const auto &cache = getTokenCache();
  std::cout << "  0 = SCCX (native gas token)" << std::endl;

  for (const auto &entry : cache)
  {
    uint64_t id = entry.first;
    const TokenInfoCache &info = entry.second;
    if (id == 0)
      continue;
    std::cout << "  " << id << " = " << info.symbol << " (" << info.name << ")";
    if (!info.sourceChain.empty())
    {
      std::cout << " [" << info.sourceChain;
      if (info.verified)
        std::cout << " · verified";
      std::cout << "]";
    }
    if (info.backingModel > 0)
      std::cout << " · " << getBackingModelName(info.backingModel);
    std::cout << std::endl;
  }

  std::cout << std::endl
            << "Token to send: ";
  std::cin >> tokenId;
  std::cin.ignore();

  std::string tokenSymbol = getTokenSymbol(tokenId);
  uint8_t decimals = getTokenDecimals(tokenId);
  std::cout << "Sending " << formatAmount(amount, decimals) << " " << tokenSymbol << "..." << std::endl;

  std::ostringstream params;
  params << R"({"from":")" << spendPubHex << R"(")"
         << R"(,"to":")" << toPubHex << R"(")"
         << R"(,"amount":)" << amount
         << R"(,"tokenId":)" << tokenId << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "transfer", params.str());

  std::string txHash;
  size_t hashStart = result.find("\"result\":\"");
  if (hashStart != std::string::npos)
  {
    hashStart += 10;
    size_t hashEnd = result.find("\"", hashStart);
    if (hashEnd != std::string::npos)
      txHash = result.substr(hashStart, hashEnd - hashStart);
  }

  if (!txHash.empty() && txHash != "false")
  {
    std::cout << "Sent " << formatAmount(amount, decimals) << " " << tokenSymbol << "!" << std::endl;
    std::cout << "Transaction: " << formatHash(txHash) << std::endl;

    TxEntry entry;
    entry.type = "Transfer";
    entry.tokenSymbol = tokenSymbol;
    entry.tokenId = tokenId;
    entry.amount = amount;
    entry.to = toPubHex;
    entry.timestamp = getTimestamp();
    entry.txHash = txHash;
    addTxToHistory(entry);
  }
  else
  {
    std::cout << "Transfer failed. Check your balance and fee amount." << std::endl;
  }

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void sidechainBalanceMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Balances ===" << std::endl
            << std::endl;

  std::ostringstream nativeParams;
  nativeParams << R"({"address":")" << spendPubHex << R"(","tokenId":0})";
  std::string nativeBalJson = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokenBalance", nativeParams.str());
  uint64_t sccxBalance = extractJsonNumber(nativeBalJson, "result");
  std::cout << "SCCX: " << formatAmount(sccxBalance, 6) << std::endl
            << std::endl;

  const auto &cache = getTokenCache();
  bool shownAny = false;

  for (const auto &entry : cache)
  {
    uint64_t id = entry.first;
    const TokenInfoCache &info = entry.second;
    if (id == 0)
      continue;

    std::ostringstream tokParams;
    tokParams << R"({"address":")" << spendPubHex << R"(","tokenId":)" << id << "}";
    std::string tokBalJson = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokenBalance", tokParams.str());
    uint64_t tokBalance = extractJsonNumber(tokBalJson, "result");

    std::cout << info.symbol << " (" << info.name << ")" << std::endl;
    std::cout << "  Balance: " << formatAmount(tokBalance, info.decimals)
              << " / Supply: " << formatAmount(info.lockedCCXAmount > 0 ? info.lockedCCXAmount : 0, info.decimals)
              << " | " << getBackingModelName(info.backingModel) << std::endl;

    if (!info.sourceChain.empty())
    {
      std::cout << "  Source: " << info.sourceChain;
      if (!info.sourceAsset.empty() && info.sourceAsset != "native")
        std::cout << " (" << info.sourceAsset << ")";
      if (info.verified)
        std::cout << " [verified]";
      std::cout << std::endl;
    }

    if (info.backingModel > 0)
    {
      std::cout << "  Backing: " << info.backingRatio << "%";
      if (info.lockedCCXAmount > 0)
        std::cout << " | Locked: " << formatAmount(info.lockedCCXAmount, 6) << " CCX";
      std::cout << std::endl;
    }

    std::cout << std::endl;
    shownAny = true;
  }

  if (!shownAny)
    std::cout << "No tokens found." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void sidechainStatusMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== Network Status ===" << std::endl
            << std::endl;

  std::string status = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getStatus", "{}");
  if (status.empty() || status == "{}")
  {
    std::cout << "Not connected." << std::endl;
  }
  else
  {
    uint64_t height = extractJsonNumber(status, "height");
    uint64_t tokenCount = extractJsonNumber(status, "tokenCount");
    uint64_t pendingTx = extractJsonNumber(status, "pendingTransactions");

    std::cout << "Height: " << height << std::endl;
    std::cout << "Tokens: " << tokenCount << std::endl;
    std::cout << "Pending transactions: " << pendingTx << std::endl;
    std::cout << std::endl;
    std::cout << "Connected to: " << cfg.sidechainHost << ":" << cfg.sidechainPort << std::endl;
    std::cout << "Mode: " << (cfg.sidechainTestnet ? "Testnet" : "Mainnet") << std::endl;
  }
  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainQuickCreateMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Quick Create Token ===" << std::endl
            << std::endl;

  std::string name = "test" + std::to_string(time(nullptr) % 10000);
  std::string symbol = "T" + std::to_string(time(nullptr) % 1000);
  uint64_t initialSupply = 1000;
  int backingModel = 0;
  uint8_t decimals = 6;

  std::cout << "Creating token:" << std::endl;
  std::cout << "  Name: " << name << std::endl;
  std::cout << "  Symbol: " << symbol << std::endl;
  std::cout << "  Supply: " << initialSupply << std::endl;
  std::cout << "  Decimals: " << static_cast<int>(decimals) << std::endl;
  std::cout << "  Model: " << getBackingModelName(static_cast<uint8_t>(backingModel)) << std::endl
            << std::endl;

  std::string nameHex = common::toHex(common::asBinaryArray(name));
  std::string symbolHex = common::toHex(common::asBinaryArray(symbol));

  std::ostringstream params;
  params << "{"
         << R"("from":")" << spendPubHex << R"(")"
         << R"(,"nameHex":")" << nameHex << R"(")"
         << R"(,"symbolHex":")" << symbolHex << R"(")"
         << R"(,"initialSupply":)" << initialSupply
         << R"(,"backingModel":)" << backingModel
         << R"(,"decimals":)" << static_cast<int>(decimals)
         << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "createToken", params.str());

  std::string txHash;
  size_t hashStart = result.find("\"result\":\"");
  if (hashStart != std::string::npos)
  {
    hashStart += 10;
    size_t hashEnd = result.find("\"", hashStart);
    if (hashEnd != std::string::npos)
      txHash = result.substr(hashStart, hashEnd - hashStart);
  }

  if (!txHash.empty() && txHash != "false")
  {
    std::cout << "Token " << symbol << " created!" << std::endl;

    TxEntry entry;
    entry.type = "CreateToken";
    entry.tokenSymbol = symbol;
    entry.tokenId = 0;
    entry.amount = initialSupply;
    entry.to = spendPubHex;
    entry.timestamp = getTimestamp();
    entry.txHash = txHash;
    addTxToHistory(entry);

    loadTokenCache(cfg.sidechainHost, cfg.sidechainPort);
  }
  else
  {
    std::cout << "Failed to create token." << std::endl;
  }

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainTxHistoryMenu()
{
  clearScreen();
  std::cout << "=== Transaction History ===" << std::endl
            << std::endl;

  if (txHistory.empty())
  {
    std::cout << "No transactions yet." << std::endl;
  }
  else
  {
    for (size_t i = 0; i < txHistory.size(); ++i)
    {
      const auto &tx = txHistory[i];
      std::cout << (i + 1) << ". " << tx.type << " — " << tx.tokenSymbol;
      std::cout << " | Amount: " << formatAmount(tx.amount);
      std::cout << " | Hash: " << formatHash(tx.txHash);
      std::cout << " | " << tx.timestamp;
      std::cout << std::endl;
    }
  }

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}