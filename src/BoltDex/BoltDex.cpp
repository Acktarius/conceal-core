// BoltDex.cpp — order matching engine with settlement
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltDex.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include <algorithm>
#include <iostream>
#include <chrono>
#include <sstream>

std::string extractJsonValue(const std::string &json, size_t start, const std::string &key)
{
  std::string search = "\"" + key + "\":\"";
  size_t pos = json.find(search, start);
  if (pos == std::string::npos)
  {
    search = "\"" + key + "\":";
    pos = json.find(search, start);
    if (pos == std::string::npos)
      return "";
    pos += search.length();
    size_t end = json.find_first_of(",}", pos);
    if (end == std::string::npos)
      return "";
    return json.substr(pos, end - pos);
  }
  pos += search.length();
  size_t end = json.find("\"", pos);
  if (end == std::string::npos)
    return "";
  return json.substr(pos, end - pos);
}

namespace BoltDex
{
  Engine::Engine()
  {
  }

  void Engine::setRpcCaller(RpcCaller caller)
  {
    m_rpcCaller = caller;
  }

  std::string Engine::escrowKey(const crypto::PublicKey &owner, uint64_t tokenId) const
  {
    return common::podToHex(owner) + "_" + std::to_string(tokenId);
  }

  uint64_t Engine::getEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId) const
  {
    auto it = m_escrow.find(escrowKey(owner, tokenId));
    return it != m_escrow.end() ? it->second : 0;
  }

  void Engine::addEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount)
  {
    m_escrow[escrowKey(owner, tokenId)] += amount;
  }

  void Engine::subEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount)
  {
    auto it = m_escrow.find(escrowKey(owner, tokenId));
    if (it != m_escrow.end() && it->second >= amount)
      it->second -= amount;
  }

  bool Engine::transferTokens(const crypto::PublicKey &from, const crypto::PublicKey &to,
                              uint64_t tokenId, uint64_t amount)
  {
    if (!m_rpcCaller)
    {
      std::cerr << "DEX: no RPC caller set" << std::endl;
      return false;
    }

    std::ostringstream params;
    params << "{"
           << R"("from":")" << common::podToHex(from) << R"(")"
           << R"(,"to":")" << common::podToHex(to) << R"(")"
           << R"(,"amount":)" << amount
           << R"(,"tokenId":)" << tokenId
           << "}";

    auto result = m_rpcCaller("transfer", params.str());

    if (result.success && result.response.find("false") == std::string::npos)
    {
      std::cout << "DEX: transferred " << amount
                << " of token " << tokenId
                << " from " << common::podToHex(from).substr(0, 16)
                << " to " << common::podToHex(to).substr(0, 16) << std::endl;
      return true;
    }

    std::cerr << "DEX: transfer failed: " << result.response << std::endl;
    return false;
  }

  bool Engine::submitOrder(const Order &order)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    // For sell orders, require the seller to deposit tokens into escrow
    if (order.type == OrderType::Sell)
    {
      uint64_t escrowed = getEscrowBalance(order.owner, order.baseTokenId);
      if (escrowed < order.amount)
      {
        std::cerr << "DEX: insufficient escrow balance for sell order. "
                  << "Have " << escrowed << ", need " << order.amount << std::endl;
        return false;
      }
      subEscrowBalance(order.owner, order.baseTokenId, order.amount);
    }
    // For buy orders, require the buyer to deposit quote tokens into escrow
    else
    {
      uint64_t totalCost = order.amount * order.price;
      uint64_t escrowed = getEscrowBalance(order.owner, order.quoteTokenId);
      if (escrowed < totalCost)
      {
        std::cerr << "DEX: insufficient escrow balance for buy order. "
                  << "Have " << escrowed << ", need " << totalCost << std::endl;
        return false;
      }
      subEscrowBalance(order.owner, order.quoteTokenId, totalCost);
    }

    Order newOrder = order;
    newOrder.id = m_nextOrderId++;
    newOrder.status = OrderStatus::Open;
    newOrder.filled = 0;
    newOrder.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    m_orders.push_back(newOrder);

    std::cout << "DEX: new " << (order.type == OrderType::Buy ? "buy" : "sell")
              << " order #" << newOrder.id
              << " amount=" << order.amount
              << " price=" << order.price << std::endl;

    matchOrders(order.baseTokenId, order.quoteTokenId);

    return true;
  }

  bool Engine::cancelOrder(uint64_t orderId, const crypto::PublicKey &owner)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto &order : m_orders)
    {
      if (order.id == orderId && order.owner == owner && order.status == OrderStatus::Open)
      {
        order.status = OrderStatus::Cancelled;

        // Return escrowed funds
        if (order.type == OrderType::Sell)
        {
          uint64_t remaining = order.amount - order.filled;
          addEscrowBalance(order.owner, order.baseTokenId, remaining);
        }
        else
        {
          uint64_t remaining = (order.amount - order.filled) * order.price;
          addEscrowBalance(order.owner, order.quoteTokenId, remaining);
        }

        std::cout << "DEX: order #" << orderId << " cancelled, funds returned to escrow" << std::endl;
        return true;
      }
    }
    return false;
  }

  std::vector<Order> Engine::getOrders(uint64_t baseTokenId, uint64_t quoteTokenId) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Order> result;
    for (const auto &order : m_orders)
    {
      if (order.baseTokenId == baseTokenId &&
          order.quoteTokenId == quoteTokenId &&
          order.status == OrderStatus::Open)
      {
        result.push_back(order);
      }
    }
    return result;
  }

  bool Engine::getOrder(uint64_t orderId, Order &order) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &o : m_orders)
    {
      if (o.id == orderId)
      {
        order = o;
        return true;
      }
    }
    return false;
  }

  std::vector<Trade> Engine::getTrades(uint64_t baseTokenId, uint64_t quoteTokenId, size_t limit) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Trade> result;
    for (const auto &trade : m_trades)
    {
      if (trade.baseTokenId == baseTokenId && trade.quoteTokenId == quoteTokenId)
      {
        result.push_back(trade);
      }
    }
    std::reverse(result.begin(), result.end());
    if (result.size() > limit)
      result.resize(limit);
    return result;
  }

  std::vector<Trade> Engine::getAllTrades(size_t limit) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Trade> result = m_trades;
    std::reverse(result.begin(), result.end());
    if (result.size() > limit)
      result.resize(limit);
    return result;
  }

  std::vector<Settlement> Engine::getPendingSettlements() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pendingSettlements;
  }

  void Engine::onTrade(TradeCallback callback)
  {
    m_tradeCallback = callback;
  }

  void Engine::processSettlements()
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto &settlement : m_pendingSettlements)
    {
      if (transferTokens(settlement.from, settlement.to, settlement.tokenId, settlement.amount))
      {
        std::cout << "DEX: settlement #" << settlement.tradeId
                  << " completed: " << settlement.description << std::endl;
      }
    }
    m_pendingSettlements.clear();
  }

  void Engine::matchOrders(uint64_t baseTokenId, uint64_t quoteTokenId)
  {
    std::vector<Order *> buys;
    std::vector<Order *> sells;

    for (auto &order : m_orders)
    {
      if (order.status != OrderStatus::Open)
        continue;
      if (order.baseTokenId != baseTokenId || order.quoteTokenId != quoteTokenId)
        continue;

      if (order.type == OrderType::Buy)
        buys.push_back(&order);
      else
        sells.push_back(&order);
    }

    std::sort(buys.begin(), buys.end(), [](Order *a, Order *b)
              { return a->price > b->price; });
    std::sort(sells.begin(), sells.end(), [](Order *a, Order *b)
              { return a->price < b->price; });

    for (auto *buy : buys)
    {
      for (auto *sell : sells)
      {
        if (buy->price >= sell->price &&
            buy->status == OrderStatus::Open &&
            sell->status == OrderStatus::Open)
        {
          uint64_t buyRemaining = buy->amount - buy->filled;
          uint64_t sellRemaining = sell->amount - sell->filled;
          uint64_t matchAmount = std::min(buyRemaining, sellRemaining);

          if (matchAmount > 0)
          {
            executeTrade(*buy, *sell, matchAmount);
          }
        }
      }
    }
  }

  bool Engine::executeTrade(const Order &buy, const Order &sell, uint64_t amount)
  {
    Trade trade;
    trade.id = m_nextTradeId++;
    trade.buyOrderId = buy.id;
    trade.sellOrderId = sell.id;
    trade.buyer = buy.owner;
    trade.seller = sell.owner;
    trade.baseTokenId = buy.baseTokenId;
    trade.quoteTokenId = buy.quoteTokenId;
    trade.amount = amount;
    trade.price = sell.price;
    trade.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    trade.settled = false;

    m_trades.push_back(trade);

    uint64_t totalQuoteAmount = amount * sell.price;

    // Calculate DEX trading fee (in quote token)
    uint64_t dexFee = 0;
    if (m_tradingFee > 0.0)
    {
      dexFee = static_cast<uint64_t>(totalQuoteAmount * (m_tradingFee / 100.0));
      if (dexFee > 0 && dexFee < totalQuoteAmount)
      {
        // Fee goes to DEX operator
        Settlement feeSettlement;
        feeSettlement.tradeId = trade.id;
        feeSettlement.from = buy.owner;
        feeSettlement.to = m_dexPubKey;
        feeSettlement.tokenId = buy.quoteTokenId;
        feeSettlement.amount = dexFee;
        feeSettlement.description = "Trade #" + std::to_string(trade.id) + " trading fee";
        m_pendingSettlements.push_back(feeSettlement);
      }
    }

    uint64_t sellerAmount = totalQuoteAmount - dexFee;

    // Seller gets quote tokens from buyer (minus fee)
    Settlement sellerSettlement;
    sellerSettlement.tradeId = trade.id;
    sellerSettlement.from = buy.owner;
    sellerSettlement.to = sell.owner;
    sellerSettlement.tokenId = buy.quoteTokenId;
    sellerSettlement.amount = sellerAmount;
    sellerSettlement.description = "Trade #" + std::to_string(trade.id) + " seller payment";
    m_pendingSettlements.push_back(sellerSettlement);

    // Buyer gets base tokens from seller
    Settlement buyerSettlement;
    buyerSettlement.tradeId = trade.id;
    buyerSettlement.from = sell.owner;
    buyerSettlement.to = buy.owner;
    buyerSettlement.tokenId = buy.baseTokenId;
    buyerSettlement.amount = amount;
    buyerSettlement.description = "Trade #" + std::to_string(trade.id) + " buyer delivery";
    m_pendingSettlements.push_back(buyerSettlement);

    // Update filled amounts
    for (auto &order : m_orders)
    {
      if (order.id == buy.id)
      {
        order.filled += amount;
        if (order.filled >= order.amount)
          order.status = OrderStatus::Filled;
      }
      if (order.id == sell.id)
      {
        order.filled += amount;
        if (order.filled >= order.amount)
          order.status = OrderStatus::Filled;
      }
    }

    std::cout << "DEX: trade executed #" << trade.id
              << " amount=" << amount
              << " price=" << trade.price
              << " fee=" << dexFee << std::endl;

    if (m_tradeCallback)
      m_tradeCallback(trade);

    return true;
  }

  void Engine::setDexKeys(const crypto::PublicKey &pubKey, const crypto::SecretKey &secKey)
  {
    m_dexPubKey = pubKey;
    m_dexSecKey = secKey;
    std::cout << "DEX public key: " << common::podToHex(pubKey) << std::endl;
  }

  void Engine::watchDeposits()
  {
    if (!m_rpcCaller)
      return;

    // Get current height
    std::string statusJson = m_rpcCaller("getStatus", "{}").response;
    size_t heightPos = statusJson.find("\"height\":");
    if (heightPos == std::string::npos)
      return;
    heightPos += 8;
    std::string heightStr;
    while (heightPos < statusJson.size() && statusJson[heightPos] >= '0' && statusJson[heightPos] <= '9')
      heightStr += statusJson[heightPos++];
    uint64_t currentHeight = heightStr.empty() ? 0 : std::stoull(heightStr);

    // Only scan if there are new blocks
    if (currentHeight <= m_lastScannedHeight)
    {
      // Initialize on first run
      if (m_lastScannedHeight == 0)
        m_lastScannedHeight = currentHeight;
      return;
    }

    std::cout << "DEX: scanning blocks " << (m_lastScannedHeight + 1) << " to " << currentHeight << std::endl;

    // Query transactions for the DEX address
    std::ostringstream params;
    params << R"({"address":")" << common::podToHex(m_dexPubKey) << R"("})";
    std::string txJson = m_rpcCaller("getTransactions", params.str()).response;

    // Extract the result array from the JSON-RPC response
    size_t resultStart = txJson.find("\"result\":[");
    if (resultStart == std::string::npos)
    {
      m_lastScannedHeight = currentHeight;
      return;
    }

    // Find incoming transfers to the DEX address
    std::string dexAddrHex = common::podToHex(m_dexPubKey);
    size_t pos = resultStart;
    while ((pos = txJson.find("\"to\":\"", pos)) != std::string::npos)
    {
      pos += 6;
      size_t toEnd = txJson.find("\"", pos);
      if (toEnd == std::string::npos)
      {
        pos++;
        continue;
      }
      std::string toHex = txJson.substr(pos, toEnd - pos);

      if (toHex != dexAddrHex)
      {
        pos++;
        continue;
      }

      // Find the block height for this transaction
      size_t blockPos = txJson.rfind("\"blockHeight\":", pos);
      if (blockPos == std::string::npos || blockPos < resultStart)
      {
        pos++;
        continue;
      }
      blockPos += 14;
      std::string blockStr;
      while (blockPos < txJson.size() && txJson[blockPos] >= '0' && txJson[blockPos] <= '9')
        blockStr += txJson[blockPos++];
      uint64_t blockHeight = blockStr.empty() ? 0 : std::stoull(blockStr);

      // Only process new blocks
      if (blockHeight <= m_lastScannedHeight)
      {
        pos++;
        continue;
      }

      // Find the type
      size_t typePos = txJson.rfind("\"type\":\"", pos);
      if (typePos == std::string::npos || typePos < resultStart)
      {
        pos++;
        continue;
      }
      typePos += 8;
      size_t typeEnd = txJson.find("\"", typePos);
      std::string type = txJson.substr(typePos, typeEnd - typePos);
      if (type != "Transfer")
      {
        pos++;
        continue;
      }

      // Find the sender
      size_t fromPos = txJson.rfind("\"from\":\"", pos);
      if (fromPos == std::string::npos || fromPos < resultStart)
      {
        pos++;
        continue;
      }
      fromPos += 8;
      size_t fromEnd = txJson.find("\"", fromPos);
      std::string fromHex = txJson.substr(fromPos, fromEnd - fromPos);

      // Find the amount
      size_t amtPos = txJson.rfind("\"amount\":", pos);
      if (amtPos == std::string::npos || amtPos < resultStart)
      {
        pos++;
        continue;
      }
      amtPos += 9;
      std::string amtStr;
      while (amtPos < txJson.size() && txJson[amtPos] >= '0' && txJson[amtPos] <= '9')
        amtStr += txJson[amtPos++];
      uint64_t amount = amtStr.empty() ? 0 : std::stoull(amtStr);

      // Find the token ID
      size_t tokPos = txJson.rfind("\"tokenId\":", pos);
      uint64_t tokenId = 0;
      if (tokPos != std::string::npos && tokPos > resultStart)
      {
        tokPos += 10;
        std::string tokStr;
        while (tokPos < txJson.size() && txJson[tokPos] >= '0' && txJson[tokPos] <= '9')
          tokStr += txJson[tokPos++];
        tokenId = tokStr.empty() ? 0 : std::stoull(tokStr);
      }

      crypto::PublicKey senderPub;
      common::podFromHex(fromHex, senderPub);

      {
        std::lock_guard<std::mutex> lock(m_mutex);
        addEscrowBalance(senderPub, tokenId, amount);
      }

      std::cout << "DEX: deposit credited: " << amount
                << " of token " << tokenId
                << " from " << fromHex.substr(0, 16) << std::endl;

      pos++;
    }

    m_lastScannedHeight = currentHeight;
  }

  bool Engine::withdraw(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    uint64_t balance = getEscrowBalance(owner, tokenId);
    if (balance < amount)
    {
      std::cerr << "DEX: insufficient escrow balance for withdrawal" << std::endl;
      return false;
    }

    subEscrowBalance(owner, tokenId, amount);

    // Send tokens from DEX to user via sidechain
    if (transferTokens(m_dexPubKey, owner, tokenId, amount))
    {
      std::cout << "DEX: withdrawal of " << amount
                << " of token " << tokenId
                << " to " << common::podToHex(owner).substr(0, 16) << std::endl;
      return true;
    }

    // Refund on failure
    addEscrowBalance(owner, tokenId, amount);
    return false;
  }
}
