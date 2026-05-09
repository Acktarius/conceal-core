// BoltRpcServer.cpp — BoltRPC implementation with sidechain integration
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltRpcServer.h"
#include "Rpc/JsonRpc.h"
#include "Common/JsonValue.h"
#include "Common/StringTools.h"
#include "HTTP/HttpRequest.h"
#include "HTTP/HttpResponse.h"
#include "BoltHttp/BoltHttpClient.h"

#include <sstream>
#include <future>
#include <mutex>

namespace BoltRPC
{

  // ---------------------------------------------------------------------------
  // Helpers
  // ---------------------------------------------------------------------------
  namespace
  {

    std::string makeResult(const std::string &resultJson, const common::JsonValue &id)
    {
      std::ostringstream ss;
      ss << R"({"jsonrpc":"2.0","result":)" << resultJson
         << R"(,"id":)" << id << "}";
      return ss.str();
    }

    std::string makeError(int code, const std::string &message,
                          const common::JsonValue &id)
    {
      std::ostringstream ss;
      ss << R"({"jsonrpc":"2.0","error":{"code":)" << code
         << R"(,"message":")" << message << R"("},"id":)" << id << "}";
      return ss.str();
    }

  } // namespace

  // ---------------------------------------------------------------------------
  BoltRpcServer::BoltRpcServer(platform_system::Dispatcher &dispatcher,
                               logging::ILogger &logger,
                               BoltCore::Wallet &wallet,
                               cn::INode &node,
                               const cn::Currency &currency,
                               const std::string &address)
      : m_logger(logger, "BoltRPC"), m_wallet(wallet), m_node(node), m_currency(currency), m_address(address), m_dispatcher(dispatcher)
  {
  }

  void BoltRpcServer::start(const std::string &bindIp, uint16_t bindPort, size_t threadCount)
  {
    m_server.reset(new BoltHttp::Server(&m_dispatcher, threadCount));
    m_server->onRequest([this](const BoltHttp::Request &req, BoltHttp::Response &resp)
                        { handleRequest(req, resp); });
    m_server->start(bindIp, bindPort);
    m_logger(logging::INFO) << "BoltRPC listening on " << bindIp << ":" << bindPort;
  }

  void BoltRpcServer::stop()
  {
    if (m_server)
      m_server->stop();
  }

  void BoltRpcServer::setSidechainConnection(const std::string &host, uint16_t port)
  {
    m_sidechainHost = host;
    m_sidechainPort = port;
    m_sidechainConnected = true;
    m_logger(logging::INFO) << "Sidechain connection set to " << host << ":" << port;
  }

  void BoltRpcServer::onNewOutputs(const std::vector<BoltCore::OutputInfo> &outputs,
                                   uint32_t newHeight)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);
    m_wallet.loadOutputs(outputs);
    m_syncedHeight.store(newHeight, std::memory_order_relaxed);
    m_logger(logging::INFO) << "Synced to height " << newHeight
                            << " (" << outputs.size() << " new outputs)";
  }

  uint32_t BoltRpcServer::getNodeHeight() const
  {
    return m_node.getLastLocalBlockHeight();
  }

  // ---------------------------------------------------------------------------
  // Sidechain RPC proxy
  // ---------------------------------------------------------------------------
  std::string BoltRpcServer::sidechainRpcCall(const std::string &method,
                                              const std::string &params)
  {
    if (!m_sidechainConnected)
      throw std::runtime_error("Sidechain not configured. Use --sidechain-host and --sidechain-port.");

    BoltHttp::HttpClient client(m_sidechainHost, m_sidechainPort);

    std::ostringstream body;
    body << R"({"jsonrpc":"2.0","method":")" << method
         << R"(","params":)" << params
         << R"(,"id":1})";

    auto response = client.post("/json_rpc", body.str());

    if (!response.success)
      throw std::runtime_error("Sidechain RPC failed: " + response.error +
                               " (HTTP " + std::to_string(response.statusCode) + ")");

    return response.body;
  }

  // ---------------------------------------------------------------------------
  void BoltRpcServer::handleRequest(const BoltHttp::Request &request, BoltHttp::Response &response)
  {
    if (request.method != "POST" || request.url != "/json_rpc")
    {
      response.status = 404;
      response.setBody("Not found");
      return;
    }

    std::string body(request.body.begin(), request.body.end());
    std::string responseBody = handleJsonRpc(body);
    response.status = 200;
    response.headers["Content-Type"] = "application/json";
    response.setBody(responseBody);
  }

  std::string BoltRpcServer::handleJsonRpc(const std::string &body)
  {
    common::JsonValue id = common::JsonValue(common::JsonValue::NIL);
    try
    {
      common::JsonValue req = common::JsonValue::fromString(body);

      if (req.contains("id"))
        id = req("id");

      std::string method = req("method").getString();
      common::JsonValue params = req.contains("params")
                                     ? req("params")
                                     : common::JsonValue(common::JsonValue::OBJECT);

      std::string result;
      if (method == "getBalance")
        result = methodGetBalance(params);
      else if (method == "getAddress")
        result = methodGetAddress(params);
      else if (method == "getStatus")
        result = methodGetStatus(params);
      else if (method == "transfer")
        result = methodTransfer(params);
      else if (method == "getTransactions")
        result = methodGetTransactions(params);
      else if (method == "createDeposit")
        result = methodCreateDeposit(params);
      else if (method == "withdrawDeposit")
        result = methodWithdrawDeposit(params);
      else if (method == "getDeposits")
        result = methodGetDeposits(params);
      else if (method == "sendFusionTransaction")
        result = methodSendFusionTransaction(params);
      else if (method == "estimateFusion")
        result = methodEstimateFusion(params);
      else if (method == "reset")
        result = methodReset(params);
      else if (method == "save")
        result = methodSave(params);
      // Sidechain methods
      else if (method == "getSidechainStatus")
        result = methodGetSidechainStatus(params);
      else if (method == "getSidechainTokens")
        result = methodGetSidechainTokens(params);
      else if (method == "sidechainTransfer")
        result = methodSidechainTransfer(params);
      else if (method == "sidechainCreateToken")
        result = methodSidechainCreateToken(params);
      else if (method == "getTokenBalance")
        result = methodGetTokenBalance(params);
      // DEX methods
      else if (method == "dexGetOrderBook")
        result = methodDexGetOrderBook(params);
      else if (method == "dexPlaceOrder")
        result = methodDexPlaceOrder(params);
      else if (method == "dexCancelOrder")
        result = methodDexCancelOrder(params);
      else if (method == "dexGetMyOrders")
        result = methodDexGetMyOrders(params);
      else if (method == "dexGetTradeHistory")
        result = methodDexGetTradeHistory(params);
      else if (method == "dexGetEscrowBalance")
        result = methodDexGetEscrowBalance(params);
      // Bridge methods
      else if (method == "bridgeGetStatus")
        result = methodBridgeGetStatus(params);
      else if (method == "bridgeLock")
        result = methodBridgeLock(params);
      else if (method == "bridgeUnlock")
        result = methodBridgeUnlock(params);
      else
        return makeError(-32601, "Method not found", id);

      return makeResult(result, id);
    }
    catch (const std::exception &e)
    {
      return makeError(-32603, e.what(), id);
    }
  }

  // =========================================================================
  // Mainchain method implementations
  // =========================================================================

  std::string BoltRpcServer::methodGetBalance(const common::JsonValue &)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);
    auto bal = m_wallet.getBalance();
    std::ostringstream ss;
    ss << R"({"availableBalance":)" << bal.actual
       << R"(,"lockedAmount":)" << bal.pending
       << R"(,"lockedDepositBalance":)" << bal.lockedDeposit
       << R"(,"unlockedDepositBalance":)" << bal.unlockedDeposit
       << "}";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetAddress(const common::JsonValue &)
  {
    std::ostringstream ss;
    ss << R"({"address":")" << m_address << R"("})";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetStatus(const common::JsonValue &)
  {
    uint32_t nodeHeight = m_node.getLastLocalBlockHeight();
    uint32_t networkHeight = m_node.getLastKnownBlockHeight();
    size_t peerCount = m_node.getPeerCount();
    uint32_t syncedHeight = m_syncedHeight.load(std::memory_order_relaxed);

    std::ostringstream ss;
    ss << R"({"blockCount":)" << nodeHeight
       << R"(,"knownBlockCount":)" << networkHeight
       << R"(,"peerCount":)" << peerCount
       << R"(,"walletHeight":)" << syncedHeight;

    // Include sidechain status if connected
    if (m_sidechainConnected)
    {
      ss << R"(,"sidechainHost":")" << m_sidechainHost << R"(")"
         << R"(,"sidechainPort":)" << m_sidechainPort;
    }

    ss << "}";
    return ss.str();
  }

  std::string BoltRpcServer::methodTransfer(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);

    if (m_wallet.getType() == BoltCore::WalletType::ViewOnly)
      throw std::runtime_error("Cannot send from view-only wallet");

    const auto &dests = params("destinations");
    std::vector<BoltCore::Transfer> transfers;
    for (size_t i = 0; i < dests.size(); ++i)
    {
      BoltCore::Transfer t;
      t.address = dests[i]("address").getString();
      t.amount = static_cast<uint64_t>(dests[i]("amount").getInteger());
      transfers.push_back(t);
    }

    uint64_t mixin = params.contains("mixin")
                         ? static_cast<uint64_t>(params("mixin").getInteger())
                         : cn::parameters::MINIMUM_MIXIN;

    auto result = m_wallet.transfer(transfers, mixin);
    if (!result.success)
      throw std::runtime_error(result.error);

    // Submit to daemon
    submitTransaction(result);

    std::ostringstream ss;
    ss << R"({"transactionHash":")" << result.txHash << R"("})";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetTransactions(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);
    auto outputs = m_wallet.getOutputs();

    std::ostringstream ss;
    ss << R"({"items":[)";
    bool first = true;
    for (const auto &out : outputs)
    {
      if (!first)
        ss << ",";
      first = false;
      ss << R"({"hash":")" << common::podToHex(out.txHash) << R"(")"
         << R"(,"amount":)" << out.amount
         << R"(,"blockHeight":)" << out.blockHeight
         << R"(,"spent":)" << (out.spent ? "true" : "false")
         << R"(,"isDeposit":)" << (out.isDeposit ? "true" : "false")
         << "}";
    }
    ss << "]}";
    return ss.str();
  }

  std::string BoltRpcServer::methodCreateDeposit(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);

    uint64_t amount = static_cast<uint64_t>(params("amount").getInteger());
    uint32_t term = static_cast<uint32_t>(params("term").getInteger());

    auto result = m_wallet.createDeposit(amount, term, m_address);
    if (!result.success)
      throw std::runtime_error(result.error);

    submitTransaction(result);

    std::ostringstream ss;
    ss << R"({"transactionHash":")" << result.txHash << R"("})";
    return ss.str();
  }

  std::string BoltRpcServer::methodWithdrawDeposit(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);

    uint64_t depositId = static_cast<uint64_t>(params("depositId").getInteger());
    auto result = m_wallet.withdrawDeposit(depositId);
    if (!result.success)
      throw std::runtime_error(result.error);

    submitTransaction(result);

    std::ostringstream ss;
    ss << R"({"transactionHash":")" << result.txHash << R"("})";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetDeposits(const common::JsonValue &)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);
    auto deposits = m_wallet.getDeposits();

    std::ostringstream ss;
    ss << R"({"deposits":[)";
    for (size_t i = 0; i < deposits.size(); ++i)
    {
      const auto &d = deposits[i];
      if (i > 0)
        ss << ",";
      ss << R"({"id":)" << d.id
         << R"(,"amount":)" << d.amount
         << R"(,"term":)" << d.term
         << R"(,"unlockHeight":)" << d.unlockHeight
         << R"(,"locked":)" << (d.locked ? "true" : "false")
         << "}";
    }
    ss << "]}";
    return ss.str();
  }

  std::string BoltRpcServer::methodEstimateFusion(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);

    uint64_t threshold = params.contains("threshold")
                             ? static_cast<uint64_t>(params("threshold").getInteger())
                             : 1000000;

    auto est = m_wallet.estimateFusion(threshold);
    std::ostringstream ss;
    ss << R"({"fusionReadyCount":)" << est.fusionReadyCount
       << R"(,"totalOutputCount":)" << est.totalOutputCount
       << "}";
    return ss.str();
  }

  std::string BoltRpcServer::methodSendFusionTransaction(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);

    uint64_t threshold = params.contains("threshold")
                             ? static_cast<uint64_t>(params("threshold").getInteger())
                             : 1000000;
    uint64_t mixin = params.contains("mixin")
                         ? static_cast<uint64_t>(params("mixin").getInteger())
                         : cn::parameters::MINIMUM_MIXIN;

    auto result = m_wallet.createFusion(threshold, mixin);
    if (!result.success)
      throw std::runtime_error(result.error);

    submitTransaction(result);

    std::ostringstream ss;
    ss << R"({"transactionHash":")" << result.txHash << R"("})";
    return ss.str();
  }

  std::string BoltRpcServer::methodReset(const common::JsonValue &)
  {
    return R"({"status":"will rescan on next restart"})";
  }

  std::string BoltRpcServer::methodSave(const common::JsonValue &)
  {
    return R"({"status":"ok"})";
  }

  // =========================================================================
  // Sidechain method implementations
  // =========================================================================

  std::string BoltRpcServer::methodGetSidechainStatus(const common::JsonValue &)
  {
    if (m_sidechainConnected)
    {
      // Proxy to sidechain for full status
      return sidechainRpcCall("getStatus", "{}");
    }

    std::ostringstream ss;
    ss << R"({"sidechainEnabled":false})";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetSidechainTokens(const common::JsonValue &)
  {
    return sidechainRpcCall("getTokens", "{}");
  }

  std::string BoltRpcServer::methodSidechainTransfer(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"from":")" << params("from").getString() << R"(")"
               << R"(,"to":")" << params("to").getString() << R"(")"
               << R"(,"amount":)" << params("amount").getInteger()
               << R"(,"tokenId":)" << params("tokenId").getInteger()
               << "}";

    return sidechainRpcCall("transfer", sideParams.str());
  }

  std::string BoltRpcServer::methodSidechainCreateToken(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"from":")" << params("from").getString() << R"(")"
               << R"(,"name":")" << params("name").getString() << R"(")"
               << R"(,"symbol":")" << params("symbol").getString() << R"(")"
               << R"(,"initialSupply":)" << params("initialSupply").getInteger()
               << R"(,"backingModel":)" << params("backingModel").getInteger()
               << "}";

    return sidechainRpcCall("createToken", sideParams.str());
  }

  std::string BoltRpcServer::methodGetTokenBalance(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"address":")" << params("address").getString() << R"(")"
               << R"(,"tokenId":)" << params("tokenId").getInteger()
               << "}";

    return sidechainRpcCall("getTokenBalance", sideParams.str());
  }

  // =========================================================================
  // DEX method implementations
  // =========================================================================

  std::string BoltRpcServer::methodDexGetOrderBook(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"baseTokenId":)" << params("baseTokenId").getInteger()
               << R"(,"quoteTokenId":)" << params("quoteTokenId").getInteger()
               << "}";

    return sidechainRpcCall("dex_getOrders", sideParams.str());
  }

  std::string BoltRpcServer::methodDexPlaceOrder(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"owner":")" << params("owner").getString() << R"(")"
               << R"(,"baseTokenId":)" << params("baseTokenId").getInteger()
               << R"(,"quoteTokenId":)" << params("quoteTokenId").getInteger()
               << R"(,"amount":)" << params("amount").getInteger()
               << R"(,"price":)" << params("price").getInteger()
               << R"(,"side":")" << params("side").getString() << R"(")"
               << "}";

    return sidechainRpcCall("dex_submitOrder", sideParams.str());
  }

  std::string BoltRpcServer::methodDexCancelOrder(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"orderId":)" << params("orderId").getInteger()
               << R"(,"owner":")" << params("owner").getString() << R"(")"
               << "}";

    return sidechainRpcCall("dex_cancelOrder", sideParams.str());
  }

  std::string BoltRpcServer::methodDexGetMyOrders(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"owner":")" << params("owner").getString() << R"(")"
               << R"(,"baseTokenId":)" << params("baseTokenId").getInteger()
               << R"(,"quoteTokenId":)" << params("quoteTokenId").getInteger()
               << "}";

    return sidechainRpcCall("dex_getOrders", sideParams.str());
  }

  std::string BoltRpcServer::methodDexGetTradeHistory(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"baseTokenId":)" << params("baseTokenId").getInteger()
               << R"(,"quoteTokenId":)" << params("quoteTokenId").getInteger()
               << "}";

    return sidechainRpcCall("dex_getTrades", sideParams.str());
  }

  std::string BoltRpcServer::methodDexGetEscrowBalance(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"owner":")" << params("owner").getString() << R"(")"
               << R"(,"tokenId":)" << params("tokenId").getInteger()
               << "}";

    return sidechainRpcCall("dex_getEscrowBalance", sideParams.str());
  }

  // =========================================================================
  // Bridge method implementations
  // =========================================================================

  std::string BoltRpcServer::methodBridgeGetStatus(const common::JsonValue &)
  {
    return sidechainRpcCall("getBridgeStatus", "{}");
  }

  std::string BoltRpcServer::methodBridgeLock(const common::JsonValue &params)
  {
    // Lock CCX on mainchain to bridge to sidechain
    // This requires a mainchain deposit transaction
    std::lock_guard<std::mutex> lock(m_walletMutex);

    uint64_t amount = static_cast<uint64_t>(params("amount").getInteger());

    // Build a deposit that the bridge watcher will detect
    BoltCore::Transfer bridgeTransfer;
    bridgeTransfer.address = params("bridgeAddress").getString();
    bridgeTransfer.amount = amount;

    std::vector<BoltCore::Transfer> transfers = {bridgeTransfer};
    auto result = m_wallet.transfer(transfers, cn::parameters::MINIMUM_MIXIN);

    if (!result.success)
      throw std::runtime_error(result.error);

    submitTransaction(result);

    std::ostringstream ss;
    ss << R"({"transactionHash":")" << result.txHash << R"(")"
       << R"(,"status":"locked"})";
    return ss.str();
  }

  std::string BoltRpcServer::methodBridgeUnlock(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"lockId":)" << params("lockId").getInteger()
               << R"(,"userAddress":")" << params("userAddress").getString() << R"(")"
               << "}";

    return sidechainRpcCall("bridgeUnlock", sideParams.str());
  }

  // =========================================================================
  // Transaction submission
  // =========================================================================

  void BoltRpcServer::submitTransaction(const BoltCore::TransferResult &result)
  {
    // Wallet already relays internally via RelayHandler.
    // This method exists for future use if we need to re-relay or verify.
    if (!result.success)
      throw std::runtime_error(result.error);

    m_logger(logging::INFO) << "Transaction completed: " << result.txHash;
  }

} // namespace BoltRPC