// DexMenus.cpp — DEX menu functions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "DexMenus.h"
#include "JsonHelpers.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include <iostream>
#include <sstream>

void dexOrderBookMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== DEX Order Book ===" << std::endl
            << std::endl;

  uint64_t baseTokenId, quoteTokenId;
  std::cout << "Base token ID: ";
  std::cin >> baseTokenId;
  std::cout << "Quote token ID (0 = SCCX): ";
  std::cin >> quoteTokenId;
  std::cin.ignore();

  std::ostringstream params;
  params << R"({"baseTokenId":)" << baseTokenId
         << R"(,"quoteTokenId":)" << quoteTokenId << "}";

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "getOrders", params.str());

  std::cout << std::endl;

  // Parse sells (ascending price)
  std::cout << "Sells:" << std::endl;
  size_t pos = 0;
  bool foundSells = false;
  while ((pos = result.find("\"type\":\"sell\"", pos)) != std::string::npos)
  {
    // Back up to find the start of this object
    size_t objStart = result.rfind("{", pos);
    if (objStart == std::string::npos)
    {
      pos++;
      continue;
    }
    size_t objEnd = result.find("}", pos);
    if (objEnd == std::string::npos)
    {
      pos++;
      continue;
    }
    std::string obj = result.substr(objStart, objEnd - objStart + 1);

    uint64_t id = extractJsonNumber(obj, "id");
    uint64_t amount = extractJsonNumber(obj, "amount");
    uint64_t price = extractJsonNumber(obj, "price");
    std::string owner = extractJsonString(obj, "owner");

    std::cout << "  #" << id << " | Price: " << formatAmount(price)
              << " | Amount: " << formatAmount(amount)
              << " | Owner: " << formatHash(owner) << std::endl;
    foundSells = true;
    pos = objEnd + 1;
  }
  if (!foundSells)
    std::cout << "  No sell orders." << std::endl;

  // Parse buys (descending price)
  std::cout << std::endl
            << "Buys:" << std::endl;
  pos = 0;
  bool foundBuys = false;
  while ((pos = result.find("\"type\":\"buy\"", pos)) != std::string::npos)
  {
    size_t objStart = result.rfind("{", pos);
    if (objStart == std::string::npos)
    {
      pos++;
      continue;
    }
    size_t objEnd = result.find("}", pos);
    if (objEnd == std::string::npos)
    {
      pos++;
      continue;
    }
    std::string obj = result.substr(objStart, objEnd - objStart + 1);

    uint64_t id = extractJsonNumber(obj, "id");
    uint64_t amount = extractJsonNumber(obj, "amount");
    uint64_t price = extractJsonNumber(obj, "price");
    std::string owner = extractJsonString(obj, "owner");

    std::cout << "  #" << id << " | Price: " << formatAmount(price)
              << " | Amount: " << formatAmount(amount)
              << " | Owner: " << formatHash(owner) << std::endl;
    foundBuys = true;
    pos = objEnd + 1;
  }
  if (!foundBuys)
    std::cout << "  No buy orders." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void dexSubmitOrderMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Place DEX Order ===" << std::endl
            << std::endl;

  std::string type;
  uint64_t baseTokenId, quoteTokenId, amount, price;

  std::cout << "Order type (buy/sell): ";
  std::getline(std::cin, type);
  if (type != "buy" && type != "sell")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Base token ID (token to buy/sell): ";
  std::cin >> baseTokenId;
  std::cout << "Quote token ID (0 = SCCX): ";
  std::cin >> quoteTokenId;
  std::cout << "Amount: ";
  std::cin >> amount;
  std::cout << "Price (in quote token): ";
  std::cin >> price;
  std::cin.ignore();

  std::ostringstream params;
  params << "{"
         << R"("type":")" << type << R"(")"
         << R"(,"owner":")" << spendPubHex << R"(")"
         << R"(,"baseTokenId":)" << baseTokenId
         << R"(,"quoteTokenId":)" << quoteTokenId
         << R"(,"amount":)" << amount
         << R"(,"price":)" << price
         << "}";

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "submitOrder", params.str());
  std::cout << std::endl
            << "Result: " << result << std::endl;

  if (result.find("true") != std::string::npos)
    std::cout << "Order placed successfully!" << std::endl;
  else
    std::cout << "Order failed. Check your escrow balance." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void dexTradeHistoryMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== DEX Trade History ===" << std::endl
            << std::endl;

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "getAllTrades", R"({"limit":20})");

  size_t pos = 0;
  int count = 0;
  while ((pos = result.find("\"id\":", pos)) != std::string::npos)
  {
    size_t objStart = result.rfind("{", pos);
    if (objStart == std::string::npos)
    {
      pos++;
      continue;
    }
    size_t objEnd = result.find("}", pos);
    if (objEnd == std::string::npos)
    {
      pos++;
      continue;
    }
    std::string obj = result.substr(objStart, objEnd - objStart + 1);

    uint64_t id = extractJsonNumber(obj, "id");
    uint64_t amount = extractJsonNumber(obj, "amount");
    uint64_t price = extractJsonNumber(obj, "price");
    uint64_t baseTokenId = extractJsonNumber(obj, "baseTokenId");
    uint64_t quoteTokenId = extractJsonNumber(obj, "quoteTokenId");
    std::string buyer = extractJsonString(obj, "buyer");
    std::string seller = extractJsonString(obj, "seller");
    std::string settled = extractJsonString(obj, "settled");
    uint64_t timestamp = extractJsonNumber(obj, "timestamp");

    std::cout << "  Trade #" << id;
    std::cout << " | Amount: " << formatAmount(amount);
    std::cout << " | Price: " << formatAmount(price);
    std::cout << " | Token: " << baseTokenId << "/" << quoteTokenId;
    std::cout << " | " << (settled == "true" ? "Settled" : "Pending");
    if (timestamp > 0)
    {
      time_t t = static_cast<time_t>(timestamp);
      std::string ts = std::ctime(&t);
      if (!ts.empty() && ts.back() == '\n')
        ts.pop_back();
      std::cout << " | " << ts;
    }
    std::cout << std::endl;
    count++;
    pos = objEnd + 1;
  }

  if (count == 0)
    std::cout << "  No trades yet." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void dexCancelOrderMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Cancel DEX Order ===" << std::endl
            << std::endl;

  uint64_t orderId;
  std::cout << "Order ID to cancel: ";
  std::cin >> orderId;
  std::cin.ignore();

  std::ostringstream params;
  params << R"({"orderId":)" << orderId
         << R"(,"owner":")" << spendPubHex << R"("})";

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "cancelOrder", params.str());
  std::cout << std::endl
            << "Result: " << result << std::endl;

  if (result.find("true") != std::string::npos)
    std::cout << "Order cancelled. Funds returned to escrow." << std::endl;
  else
    std::cout << "Cancellation failed. Check the order ID and owner." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void dexDepositAddressMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== DEX Deposit Address ===" << std::endl
            << std::endl;

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "deposit", "{}");
  std::string address = extractJsonString(result, "dexAddress");

  if (address.empty())
  {
    std::cout << "Could not retrieve DEX address. Is the DEX running?" << std::endl;
  }
  else
  {
    std::cout << "Send tokens to this address to fund your DEX account:" << std::endl;
    std::cout << "  " << address << std::endl;
    std::cout << std::endl;
    std::cout << "Use S4 (Send Token) to transfer tokens to this address." << std::endl;
    std::cout << "Then check your escrow balance with D5." << std::endl;
  }

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void dexEscrowBalanceMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== DEX Escrow Balance ===" << std::endl
            << std::endl;

  // Check SCCX escrow
  std::ostringstream sccxParams;
  sccxParams << R"({"owner":")" << spendPubHex << R"(","tokenId":0})";
  std::string sccxResult = sidechainCall(cfg.dexHost, cfg.dexPort, "getEscrowBalance", sccxParams.str());
  uint64_t sccxEscrow = 0;
  try
  {
    sccxEscrow = std::stoull(sccxResult);
  }
  catch (...)
  {
  }

  std::cout << "SCCX Escrow: " << formatAmount(sccxEscrow) << std::endl;

  // Check token escrow balances - ask for token ID
  std::cout << std::endl;
  std::cout << "Check escrow for a specific token ID?" << std::endl;
  std::cout << "Token ID (0 to skip): ";
  uint64_t tokenId;
  std::cin >> tokenId;
  std::cin.ignore();

  if (tokenId > 0)
  {
    std::ostringstream tokParams;
    tokParams << R"({"owner":")" << spendPubHex << R"(","tokenId":)" << tokenId << "}";
    std::string tokResult = sidechainCall(cfg.dexHost, cfg.dexPort, "getEscrowBalance", tokParams.str());
    uint64_t tokEscrow = 0;
    try
    {
      tokEscrow = std::stoull(tokResult);
    }
    catch (...)
    {
    }
    std::cout << "Token #" << tokenId << " Escrow: " << formatAmount(tokEscrow) << std::endl;
  }

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}