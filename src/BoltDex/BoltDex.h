// BoltDex.h — order matching engine with settlement, deposits, and withdrawals
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltDexTypes.h"
#include <vector>
#include <mutex>
#include <string>
#include <functional>
#include <unordered_map>

namespace BoltDex
{
  class Engine
  {
  public:
    using TradeCallback = std::function<void(const Trade &)>;

    Engine();

    // Set the RPC caller for sidechain communication
    void setRpcCaller(RpcCaller caller);

    // Set the DEX keypair
    void setDexKeys(const crypto::PublicKey &pubKey, const crypto::SecretKey &secKey);

    // Get the DEX public key (users send deposits here)
    crypto::PublicKey getDexPublicKey() const { return m_dexPubKey; }

    // Watch for incoming deposits to the DEX address
    void watchDeposits();

    // Submit a new order to the book
    bool submitOrder(const Order &order);

    // Cancel an existing order
    bool cancelOrder(uint64_t orderId, const crypto::PublicKey &owner);

    // Withdraw funds from escrow
    bool withdraw(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount);

    // Get all open orders for a token pair
    std::vector<Order> getOrders(uint64_t baseTokenId, uint64_t quoteTokenId) const;

    // Get order by ID
    bool getOrder(uint64_t orderId, Order &order) const;

    // Get trade history for a token pair
    std::vector<Trade> getTrades(uint64_t baseTokenId, uint64_t quoteTokenId, size_t limit = 50) const;

    // Get all trades
    std::vector<Trade> getAllTrades(size_t limit = 100) const;

    // Get pending settlements
    std::vector<Settlement> getPendingSettlements() const;

    // Get escrow balance for an address
    uint64_t getEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId) const;

    // Set callback for when a trade executes
    void onTrade(TradeCallback callback);

    // Process all pending settlements
    void processSettlements();

    // Set the trading fee percentage (e.g. 0.5 = 0.5%)
    void setTradingFee(double feePercent) { m_tradingFee = feePercent; }

  private:
    void matchOrders(uint64_t baseTokenId, uint64_t quoteTokenId);
    bool executeTrade(const Order &buy, const Order &sell, uint64_t amount);
    bool transferTokens(const crypto::PublicKey &from, const crypto::PublicKey &to,
                        uint64_t tokenId, uint64_t amount);

    RpcCaller m_rpcCaller;
    std::vector<Order> m_orders;
    std::vector<Trade> m_trades;
    std::vector<Settlement> m_pendingSettlements;
    uint64_t m_nextOrderId = 1;
    uint64_t m_nextTradeId = 1;
    mutable std::mutex m_mutex;
    TradeCallback m_tradeCallback;

    double m_tradingFee = 0.0; // trading fee percentage

    // DEX identity
    crypto::PublicKey m_dexPubKey;
    crypto::SecretKey m_dexSecKey;
    uint64_t m_lastScannedHeight = 0;

    // Escrow balances: key = "owner_tokenId"
    std::unordered_map<std::string, uint64_t> m_escrow;
    std::string escrowKey(const crypto::PublicKey &owner, uint64_t tokenId) const;
    void addEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount);
    void subEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount);
  };
}