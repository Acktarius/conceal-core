// SidechainRpcServer.h — JSON-RPC server for sidechain user interactions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <memory>

#include "SidechainTypes.h"
#include "SidechainStorage.h"
#include "SidechainValidator.h"
#include "Common/JsonValue.h"
#include "Logging/LoggerRef.h"
#include "BoltHttp/BoltHttpServer.h"

namespace Sidechain
{
  class SidechainRpcServer
  {
  public:
    SidechainRpcServer(logging::ILogger &logger,
                       SidechainStorage &storage,
                       SidechainValidator &validator);

    void start(const std::string &bindIp, uint16_t bindPort, size_t threadCount = 1);
    void stop();

    void setSidechainEndpoint(const std::string &host, uint16_t port)
    {
      m_sidechainHost = host;
      m_sidechainPort = port;
    }

    void setTestnet(bool testnet)
    {
      m_testnet = testnet;
    }

  private:
    std::string handleJsonRpc(const std::string &body);

    std::string methodGetBalance(const common::JsonValue &params);
    std::string methodGetTokenBalance(const common::JsonValue &params);
    std::string methodGetTokens(const common::JsonValue &params);
    std::string methodTransfer(const common::JsonValue &params);
    std::string methodCreateToken(const common::JsonValue &params);
    std::string methodMintToken(const common::JsonValue &params);
    std::string methodBurnToken(const common::JsonValue &params);
    std::string methodGetStatus(const common::JsonValue &params);
    std::string methodGetPendingTransactions(const common::JsonValue &params);
    std::string methodGetValidators(const common::JsonValue &params);
    std::string methodGetSidechainStatus(const common::JsonValue &params);
    std::string methodGetSidechainTokens(const common::JsonValue &params);
    std::string methodSidechainTransfer(const common::JsonValue &params);
    std::string methodSidechainCreateToken(const common::JsonValue &params);
    std::string methodGetTransactions(const common::JsonValue &params);
    std::string methodFaucet(const common::JsonValue &params);

    logging::LoggerRef m_logger;
    SidechainStorage &m_storage;
    SidechainValidator &m_validator;
    std::unique_ptr<BoltHttp::Server> m_httpServer;

    std::string m_sidechainHost = "127.0.0.1";
    uint16_t m_sidechainPort = 8080;
    bool m_testnet = false;
  };
}