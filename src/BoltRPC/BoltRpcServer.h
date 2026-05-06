// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once
#include <memory>
#include <mutex>
#include <string>

#include "Common/JsonValue.h"
#include "BoltCore/BoltCore.h"
#include "Rpc/HttpServer.h"
#include "Logging/LoggerRef.h"
#include <System/Dispatcher.h>
#include "CryptoNoteConfig.h"

namespace BoltRPC
{

  class BoltRpcServer : public cn::HttpServer
  {
  public:
    BoltRpcServer(platform_system::Dispatcher &dispatcher,
                  logging::ILogger &logger,
                  BoltCore::Wallet &wallet,
                  cn::INode &node,
                  const cn::Currency &currency,
                  const std::string &address);

    void start(const std::string &bindIp, uint16_t bindPort);
    void stop();

    // Called by SyncMonitor when new outputs arrive
    void onNewOutputs(const std::vector<BoltCore::OutputInfo> &outputs,
                      uint32_t newHeight);

    uint32_t getNodeHeight() const;

  protected:
    // HttpServer override — all requests come through here
    void processRequest(const cn::HttpRequest &request,
                        cn::HttpResponse &response) override;

  private:
    std::string handleJsonRpc(const std::string &body);

    // --- Method handlers ---
    // Each returns a JSON result string or throws JsonRpcError
    std::string methodGetBalance(const common::JsonValue &params);
    std::string methodGetAddress(const common::JsonValue &params);
    std::string methodGetStatus(const common::JsonValue &params);
    std::string methodTransfer(const common::JsonValue &params);
    std::string methodGetTransactions(const common::JsonValue &params);
    std::string methodCreateDeposit(const common::JsonValue &params);
    std::string methodWithdrawDeposit(const common::JsonValue &params);
    std::string methodGetDeposits(const common::JsonValue &params);
    std::string methodSendFusionTransaction(const common::JsonValue &params);
    std::string methodEstimateFusion(const common::JsonValue &params);
    std::string methodReset(const common::JsonValue &params);
    std::string methodSave(const common::JsonValue &params);

    logging::LoggerRef m_logger;
    BoltCore::Wallet &m_wallet;
    cn::INode &m_node;
    const cn::Currency &m_currency;
    std::string m_address;
    std::atomic<uint32_t> m_syncedHeight{0};
    mutable std::mutex m_walletMutex; // guards wallet state for concurrent RPC calls
  };

} // namespace BoltRPC