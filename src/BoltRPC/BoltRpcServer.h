// BoltRpcServer.h — BoltRPC wallet JSON-RPC server (walletd replacement)
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "BoltHttp/BoltHttpServer.h"
#include "BoltCore/BoltCore.h"
#include "Common/JsonValue.h"
#include "CryptoNoteCore/Currency.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include "Logging/LoggerRef.h"

namespace BoltRPC
{

  class BoltRpcServer
  {
  public:
    BoltRpcServer(platform_system::Dispatcher &dispatcher,
                  logging::ILogger &logger,
                  BoltCore::Wallet &wallet,
                  cn::INode &node,
                  const cn::Currency &currency,
                  const std::string &address);

    void start(const std::string &bindIp, uint16_t bindPort, size_t threadCount = 4);
    void stop();

    void setSidechainConnection(const std::string &host, uint16_t port);
    void onNewOutputs(const std::vector<BoltCore::OutputInfo> &outputs, uint32_t newHeight);
    uint32_t getNodeHeight() const;

  private:
    void handleRequest(const BoltHttp::Request &request, BoltHttp::Response &response);
    std::string handleJsonRpc(const std::string &body);
    std::string sidechainRpcCall(const std::string &method, const std::string &params);
    void submitTransaction(const BoltCore::TransferResult &result);

    // Method handlers
    std::string methodGetBalance(const common::JsonValue &params);
    std::string methodGetAddress(const common::JsonValue &params);
    std::string methodGetStatus(const common::JsonValue &params);
    std::string methodGetSyncStatus(const common::JsonValue &params);
    std::string methodTransfer(const common::JsonValue &params);
    std::string methodGetTransactions(const common::JsonValue &params);
    std::string methodCreateDeposit(const common::JsonValue &params);
    std::string methodWithdrawDeposit(const common::JsonValue &params);
    std::string methodGetDeposits(const common::JsonValue &params);
    std::string methodEstimateFusion(const common::JsonValue &params);
    std::string methodSendFusionTransaction(const common::JsonValue &params);
    std::string methodReset(const common::JsonValue &params);
    std::string methodSave(const common::JsonValue &params);
    std::string methodGetSidechainStatus(const common::JsonValue &params);
    std::string methodGetSidechainTokens(const common::JsonValue &params);
    std::string methodSidechainTransfer(const common::JsonValue &params);
    std::string methodSidechainCreateToken(const common::JsonValue &params);
    std::string methodGetTokenBalance(const common::JsonValue &params);
    std::string methodDexGetOrderBook(const common::JsonValue &params);
    std::string methodDexPlaceOrder(const common::JsonValue &params);
    std::string methodDexCancelOrder(const common::JsonValue &params);
    std::string methodDexGetMyOrders(const common::JsonValue &params);
    std::string methodDexGetTradeHistory(const common::JsonValue &params);
    std::string methodDexGetEscrowBalance(const common::JsonValue &params);
    std::string methodBridgeGetStatus(const common::JsonValue &params);
    std::string methodBridgeLock(const common::JsonValue &params);
    std::string methodBridgeUnlock(const common::JsonValue &params);

    logging::LoggerRef m_logger;
    BoltCore::Wallet &m_wallet;
    cn::INode &m_node;
    const cn::Currency &m_currency;
    std::string m_address;
    std::atomic<uint32_t> m_syncedHeight{0};
    std::mutex m_walletMutex;
    std::string m_sidechainHost;
    uint16_t m_sidechainPort = 8080;
    bool m_sidechainConnected = false;
    platform_system::Dispatcher &m_dispatcher;
    std::unique_ptr<BoltHttp::Server> m_server;
  };

} // namespace BoltRPC