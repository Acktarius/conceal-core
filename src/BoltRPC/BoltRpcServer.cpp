// BoltRpcServer.cpp — BoltRPC implementation with sidechain integration
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltRpcServer.h"
#include "Rpc/JsonRpc.h"
#include "Common/JsonValue.h"
#include "Common/StringTools.h"
#include "HTTP/HttpRequest.h"
#include "HTTP/HttpResponse.h"

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

    // Simple HTTP client for sidechain RPC calls
    std::string sidechainRpcCall(const std::string &host, uint16_t port,
                                 const std::string &method, const std::string &params)
    {
      // For now, return empty - will be wired up when HTTP client is available
      // TODO: use HttpClient to call sidechain RPC
      return "{}";
    }

  } // namespace

  // ---------------------------------------------------------------------------
  BoltRpcServer::BoltRpcServer(platform_system::Dispatcher &dispatcher,
                               logging::ILogger &logger,
                               BoltCore::Wallet &wallet,
                               cn::INode &node,
                               const cn::Currency &currency,
                               const std::string &address)
      : cn::HttpServer(dispatcher, logger), m_logger(logger, "BoltRPC"), m_wallet(wallet), m_node(node), m_currency(currency), m_address(address)
  {
  }

  void BoltRpcServer::start(const std::string &bindIp, uint16_t bindPort)
  {
    cn::HttpServer::start(bindIp, bindPort);
    m_logger(logging::INFO) << "BoltRPC listening on "
                            << bindIp << ":" << bindPort;
  }

  void BoltRpcServer::stop()
  {
    cn::HttpServer::stop();
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
  void BoltRpcServer::processRequest(const cn::HttpRequest &request,
                                     cn::HttpResponse &response)
  {
    if (request.getMethod() != "POST" || request.getUrl() != "/json_rpc")
    {
      response.setStatus(cn::HttpResponse::STATUS_404);
      response.setBody("Not found");
      return;
    }

    std::string responseBody = handleJsonRpc(request.getBody());
    response.setStatus(cn::HttpResponse::STATUS_200);
    response.addHeader("Content-Type", "application/json");
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
      else
        return makeError(-32601, "Method not found", id);

      return makeResult(result, id);
    }
    catch (const std::exception &e)
    {
      return makeError(-32603, e.what(), id);
    }
  }

  // Sidechain method implementations

  std::string BoltRpcServer::methodGetSidechainStatus(const common::JsonValue &)
  {
    std::ostringstream ss;
    ss << R"({"sidechainEnabled":true)"
       << R"(,"sidechainHost":")" << m_sidechainHost << R"(")"
       << R"(,"sidechainPort":)" << m_sidechainPort
       << "}";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetSidechainTokens(const common::JsonValue &)
  {
    // Proxy to sidechain RPC
    std::string result = sidechainRpcCall(m_sidechainHost, m_sidechainPort,
                                          "getTokens", "{}");
    return result;
  }

  std::string BoltRpcServer::methodSidechainTransfer(const common::JsonValue &params)
  {
    // Build params for sidechain RPC
    std::ostringstream sideParams;
    sideParams << R"({"from":")" << params("from").getString() << R"(")"
               << R"(,"to":")" << params("to").getString() << R"(")"
               << R"(,"amount":)" << params("amount").getInteger()
               << R"(,"tokenId":)" << params("tokenId").getInteger()
               << "}";

    return sidechainRpcCall(m_sidechainHost, m_sidechainPort,
                            "transfer", sideParams.str());
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

    return sidechainRpcCall(m_sidechainHost, m_sidechainPort,
                            "createToken", sideParams.str());
  }

  std::string BoltRpcServer::methodGetTokenBalance(const common::JsonValue &params)
  {
    std::ostringstream sideParams;
    sideParams << R"({"address":")" << params("address").getString() << R"(")"
               << R"(,"tokenId":)" << params("tokenId").getInteger()
               << "}";

    return sidechainRpcCall(m_sidechainHost, m_sidechainPort,
                            "getTokenBalance", sideParams.str());
  }

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
       << R"(,"lastBlockHash":")" << "" << R"(")"
       << R"(,"peerCount":)" << peerCount
       << R"(,"walletHeight":)" << syncedHeight
       << "}";
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

    std::ostringstream ss;
    ss << R"({"transactionHash":")" << result.txHash << R"("})";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetTransactions(const common::JsonValue &params)
  {
    return R"({"items":[]})";
  }

  std::string BoltRpcServer::methodCreateDeposit(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);

    uint64_t amount = static_cast<uint64_t>(params("amount").getInteger());
    uint32_t term = static_cast<uint32_t>(params("term").getInteger());

    auto result = m_wallet.createDeposit(amount, term, m_address);
    if (!result.success)
      throw std::runtime_error(result.error);

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

} // namespace BoltRPC