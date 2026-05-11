// BoltRpcServer.h — BoltRPC wallet JSON-RPC server (walletd replacement)
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "BoltHttp/BoltHttpServer.h"
#include "BoltCore/BoltCore.h"
#include "Common/JsonValue.h"
#include "CryptoNoteCore/Currency.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include "Logging/LoggerRef.h"

namespace BoltRPC
{

  class StateManager;

  class BoltRpcServer
  {
  public:
    BoltRpcServer(platform_system::Dispatcher &dispatcher,
                  logging::ILogger &logger,
                  BoltCore::Wallet &wallet,
                  cn::INode &node,
                  const cn::Currency &currency,
                  const std::string &address);

    void start(const std::string &bindIp, uint16_t bindPort, size_t threadCount = 1);
    void stop();

    // Connect to a sidechain validator for proxied RPC calls
    void setSidechainConnection(const std::string &host, uint16_t port);

    // Called by SyncMonitor when new outputs are discovered
    void onNewOutputs(const std::vector<BoltCore::OutputInfo> &outputs, uint32_t newHeight);

    uint32_t getNodeHeight() const;

    // Wire up state persistence so save() and auto-save can write wallet state
    void setStateManager(StateManager *stateManager,
                         std::vector<BoltCore::OutputInfo> *outputs,
                         std::atomic<uint32_t> *syncedHeight);

    // Public save method for auto-save timer — persists outputs AND keys
    bool saveWalletState();

  private:
    void handleRequest(const BoltHttp::Request &request, BoltHttp::Response &response);
    std::string handleJsonRpc(const std::string &body);
    std::string sidechainRpcCall(const std::string &method, const std::string &params);
    void submitTransaction(const BoltCore::TransferResult &result);

    // System
    std::string methodGetVersion(const common::JsonValue &params);

    // Wallet lifecycle
    std::string methodImportWallet(const common::JsonValue &params);
    std::string methodGenerateWallet(const common::JsonValue &params);
    std::string methodUnlock(const common::JsonValue &params);
    std::string methodLock(const common::JsonValue &params);
    std::string methodGetViewKey(const common::JsonValue &params);
    std::string methodGetSpendKey(const common::JsonValue &params);

    // Mainchain
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

    // Sidechain
    std::string methodGetSidechainStatus(const common::JsonValue &params);
    std::string methodGetSidechainTokens(const common::JsonValue &params);
    std::string methodSidechainTransfer(const common::JsonValue &params);
    std::string methodSidechainCreateToken(const common::JsonValue &params);
    std::string methodGetTokenBalance(const common::JsonValue &params);

    // DEX
    std::string methodDexGetOrderBook(const common::JsonValue &params);
    std::string methodDexPlaceOrder(const common::JsonValue &params);
    std::string methodDexCancelOrder(const common::JsonValue &params);
    std::string methodDexGetMyOrders(const common::JsonValue &params);
    std::string methodDexGetTradeHistory(const common::JsonValue &params);
    std::string methodDexGetEscrowBalance(const common::JsonValue &params);

    // Bridge
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

    // State persistence
    StateManager *m_stateManager = nullptr;
    std::vector<BoltCore::OutputInfo> *m_outputs = nullptr;
    std::atomic<uint32_t> *m_externalSyncedHeight = nullptr;

    // Locked state (keys zeroed out but RPC stays alive)
    bool m_locked = false;
    crypto::SecretKey m_savedViewKey;
    crypto::SecretKey m_savedSpendKey;
  };

} // namespace BoltRPC