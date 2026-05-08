// BoltDexRpcServer.h — JSON-RPC server for DEX
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once
#include <memory>
#include "BoltDex.h"
#include "BoltHttp/BoltHttpServer.h"
#include "Common/JsonValue.h"
#include "Logging/LoggerRef.h"

namespace BoltDex
{
  class RpcServer
  {
  public:
    RpcServer(logging::ILogger &logger, Engine &engine);

    void start(const std::string &bindIp, uint16_t bindPort, size_t threadCount = 1);
    void stop();

  private:
    std::string handleJsonRpc(const std::string &body);

    std::string methodGetOrders(const common::JsonValue &params);
    std::string methodGetTrades(const common::JsonValue &params);
    std::string methodGetAllTrades(const common::JsonValue &params);
    std::string methodSubmitOrder(const common::JsonValue &params);
    std::string methodCancelOrder(const common::JsonValue &params);
    std::string methodDeposit(const common::JsonValue &params);
    std::string methodWithdraw(const common::JsonValue &params);
    std::string methodGetEscrowBalance(const common::JsonValue &params);
    std::string methodGetStatus(const common::JsonValue &params);

    logging::LoggerRef m_logger;
    Engine &m_engine;
    std::unique_ptr<BoltHttp::Server> m_httpServer;
  };
}