// BoltRpcServer.cpp — BoltRPC implementation with sidechain integration
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltRpcServer.h"
#include "SyncMonitor.h"
#include "Rpc/JsonRpc.h"
#include "Common/JsonValue.h"
#include "Common/StringTools.h"
#include "HTTP/HttpRequest.h"
#include "HTTP/HttpResponse.h"
#include "BoltHttp/BoltHttpClient.h"
#include "BoltSync/CryptoHelpers.h"
#include "StateManager.h"
#include "Mnemonics/Mnemonics.h"
#include "version.h"

#include <sstream>
#include <future>
#include <mutex>

namespace BoltRPC
{
  // Helpers
  namespace
  {
    std::string base64Encode(const std::string &input)
    {
      static const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string result;
      int val = 0, valb = -6;
      for (unsigned char c : input)
      {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0)
        {
          result.push_back(chars[(val >> valb) & 0x3F]);
          valb -= 6;
        }
      }
      if (valb > -6)
        result.push_back(chars[((val << 8) >> (valb + 8)) & 0x3F]);
      while (result.size() % 4)
        result.push_back('=');
      return result;
    }

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

  // Constructor / start / stop
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

  void BoltRpcServer::setStateManager(StateManager *stateManager,
                                      std::vector<BoltCore::OutputInfo> *outputs,
                                      std::atomic<uint32_t> *syncedHeight)
  {
    m_stateManager = stateManager;
    m_outputs = outputs;
    m_externalSyncedHeight = syncedHeight;
  }

  void BoltRpcServer::setSseBroadcaster(BoltHttp::SseBroadcaster *broadcaster)
  {
    m_sseBroadcaster = broadcaster;
  }

  void BoltRpcServer::onNewOutputs(const std::vector<BoltCore::OutputInfo> &outputs,
                                   uint32_t newHeight)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);
    m_wallet.loadOutputs(outputs);
    m_syncedHeight.store(newHeight, std::memory_order_relaxed);
    m_logger(logging::INFO) << "Synced to height " << newHeight
                            << " (" << outputs.size() << " new outputs)";

    // Push sync event to SSE clients
    if (m_sseBroadcaster)
    {
      std::ostringstream data;
      data << R"({"newHeight":)" << newHeight
           << R"(,"newOutputs":)" << outputs.size() << "}";
      m_sseBroadcaster->broadcast("walletSync", data.str());
    }
  }

  uint32_t BoltRpcServer::getNodeHeight() const
  {
    return m_node.getLastLocalBlockHeight();
  }

  // Sidechain RPC proxy
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

  // Request dispatch
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

      // System
      if (method == "getVersion")
        result = methodGetVersion(params);
      // Wallet lifecycle
      else if (method == "importWallet")
        result = methodImportWallet(params);
      else if (method == "generateWallet")
        result = methodGenerateWallet(params);
      else if (method == "unlock")
        result = methodUnlock(params);
      else if (method == "lock")
        result = methodLock(params);
      else if (method == "getViewKey")
        result = methodGetViewKey(params);
      else if (method == "getSpendKey")
        result = methodGetSpendKey(params);
      else if (method == "getWalletHeight")
        result = methodGetWalletHeight(params);
      else if (method == "exportWallet")
        result = methodExportWallet(params);
      // Mainchain
      else if (method == "getBalance")
        result = methodGetBalance(params);
      else if (method == "getAddress")
        result = methodGetAddress(params);
      else if (method == "getStatus")
        result = methodGetStatus(params);
      else if (method == "getSyncStatus")
        result = methodGetSyncStatus(params);
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
      else if (method == "getNetworkHeight")
        result = methodGetNetworkHeight(params);
      // Sidechain
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
      // DEX
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
      // Bridge
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

  // System
  // Return the binary version string for GUI compatibility checks
  std::string BoltRpcServer::methodGetVersion(const common::JsonValue &)
  {
    return R"({"version":")" + std::string(CCX_RELEASE_VERSION) + R"("})";
  }

  // Wallet lifecycle
  // Import an existing wallet by view key and optional spend key, no restart needed
  std::string BoltRpcServer::methodImportWallet(const common::JsonValue &params)
  {
    std::string viewKeyHex = params("viewKey").getString();
    std::string spendKeyHex = params.contains("spendKey") ? params("spendKey").getString() : "";

    crypto::SecretKey viewKey, spendKey;
    if (!BoltSync::hexToSecretKey(viewKeyHex, viewKey))
      throw std::runtime_error("Invalid view key hex");

    if (!spendKeyHex.empty() && !BoltSync::hexToSecretKey(spendKeyHex, spendKey))
      throw std::runtime_error("Invalid spend key hex");

    crypto::PublicKey viewPub, spendPub;
    crypto::secret_key_to_public_key(viewKey, viewPub);
    if (!spendKeyHex.empty())
      crypto::secret_key_to_public_key(spendKey, spendPub);

    // Destroy old wallet and construct new one in place
    m_wallet.~Wallet();
    new (&m_wallet) BoltCore::Wallet(viewKey, spendKey, viewPub, spendPub, m_node, m_currency);

    m_address = m_currency.accountAddressAsString({spendPub, viewPub});

    // Save keys for lock/unlock cycle
    m_savedViewKey = viewKey;
    m_savedSpendKey = spendKey;
    m_locked = false;

    m_logger(logging::INFO) << "Wallet imported: " << m_address;

    if (m_stateManager && m_outputs)
    {
      uint32_t h = m_externalSyncedHeight
                       ? m_externalSyncedHeight->load(std::memory_order_relaxed)
                       : m_syncedHeight.load(std::memory_order_relaxed);
      // Save keys alongside outputs for unlock support
      m_stateManager->save(*m_outputs, h, viewKeyHex, spendKeyHex);
    }

    // Start sync monitor if data directory is configured
    if (!m_dataDir.empty() && !m_locked)
    {
      startSync(m_dataDir, m_savedViewKey,
                m_wallet.getViewPublicKey(),
                m_wallet.getType() == BoltCore::WalletType::ViewOnly
                    ? nullptr
                    : &m_savedSpendKey);
    }

    return R"({"status":"ok","address":")" + m_address + R"("})";
  }

  // Generate a new random wallet, return keys for backup
  std::string BoltRpcServer::methodGenerateWallet(const common::JsonValue &)
  {
    crypto::SecretKey viewKey, spendKey;
    crypto::PublicKey viewPub, spendPub;

    crypto::generate_keys(viewPub, viewKey);
    crypto::generate_keys(spendPub, spendKey);

    m_wallet.~Wallet();
    new (&m_wallet) BoltCore::Wallet(viewKey, spendKey, viewPub, spendPub, m_node, m_currency);
    m_address = m_currency.accountAddressAsString({spendPub, viewPub});

    m_savedViewKey = viewKey;
    m_savedSpendKey = spendKey;
    m_locked = false;

    std::string viewKeyHex = common::podToHex(viewKey);
    std::string spendKeyHex = common::podToHex(spendKey);

    // Generate mnemonic seed phrase from the spend key (25 words)
    std::string mnemonic;
    try
    {
      mnemonic = mnemonics::privateKeyToMnemonic(spendKey);
    }
    catch (...)
    {
      mnemonic = "";
    }

    std::ostringstream ss;
    ss << R"({"status":"ok")"
       << R"(,"address":")" << m_address << R"(")"
       << R"(,"viewKey":")" << viewKeyHex << R"(")"
       << R"(,"spendKey":")" << spendKeyHex << R"(")"
       << R"(,"mnemonic":")" << mnemonic << R"(")"
       << "}";

    if (m_stateManager && m_outputs)
    {
      uint32_t h = m_externalSyncedHeight
                       ? m_externalSyncedHeight->load(std::memory_order_relaxed)
                       : m_syncedHeight.load(std::memory_order_relaxed);
      m_stateManager->save(*m_outputs, h, viewKeyHex, spendKeyHex);
    }

    return ss.str();
  }

  std::string BoltRpcServer::methodGetWalletHeight(const common::JsonValue &)
  {
    uint32_t walletHeight = m_syncedHeight.load(std::memory_order_relaxed);
    uint32_t outputCount = 0;
    {
      std::lock_guard<std::mutex> lock(m_walletMutex);
      outputCount = static_cast<uint32_t>(m_wallet.getOutputs().size());
    }
    std::ostringstream ss;
    ss << R"({"walletHeight":)" << walletHeight
       << R"(,"outputCount":)" << outputCount << "}";
    return ss.str();
  }

  // Unlock: reload keys from the state file without the user re-entering hex
  std::string BoltRpcServer::methodUnlock(const common::JsonValue &)
  {
    if (!m_stateManager)
      throw std::runtime_error("State manager not configured");

    std::string viewKeyHex, spendKeyHex;
    if (!m_stateManager->loadKeys(viewKeyHex, spendKeyHex))
      throw std::runtime_error("No saved keys found. Use importWallet to set keys first.");

    if (viewKeyHex.empty())
      throw std::runtime_error("No saved keys found. Use importWallet to set keys first.");

    // Reuse importWallet logic by building a params object
    common::JsonValue params(common::JsonValue::OBJECT);
    params.insert("viewKey", viewKeyHex);
    if (!spendKeyHex.empty())
      params.insert("spendKey", spendKeyHex);

    return methodImportWallet(params);
  }

  // Lock the wallet: zero out keys in memory, RPC stays alive for unlock
  std::string BoltRpcServer::methodLock(const common::JsonValue &)
  {
    if (m_locked)
      return R"({"status":"ok","message":"Already locked"})";

    BoltCore::WalletType type = m_wallet.getType();
    if (type == BoltCore::WalletType::ViewOnly)
    {
      m_locked = true;
      return R"({"status":"ok","message":"View-only wallet locked"})";
    }

    // Full wallet: zero out keys and reconstruct as view-only
    crypto::PublicKey viewPub = m_wallet.getViewPublicKey();

    m_wallet.~Wallet();
    new (&m_wallet) BoltCore::Wallet(crypto::SecretKey{}, crypto::SecretKey{},
                                     viewPub, crypto::PublicKey{}, m_node, m_currency);
    m_locked = true;

    // Save state with empty keys to respect locked state
    saveWalletState();

    return R"({"status":"ok","message":"Wallet locked. Use unlock to restore keys."})";
  }

  // Return the current view key for backup purposes
  std::string BoltRpcServer::methodGetViewKey(const common::JsonValue &)
  {
    if (m_locked)
      return R"({"viewKey":""})";

    return R"({"viewKey":")" + common::podToHex(m_savedViewKey) + R"("})";
  }

  // Return the current spend key if available (full wallet only)
  std::string BoltRpcServer::methodGetSpendKey(const common::JsonValue &)
  {
    if (m_locked || m_wallet.getType() == BoltCore::WalletType::ViewOnly)
      return R"({"spendKey":""})";

    return R"({"spendKey":")" + common::podToHex(m_savedSpendKey) + R"("})";
  }

  std::string BoltRpcServer::methodExportWallet(const common::JsonValue &)
  {
    if (m_locked)
      throw std::runtime_error("Wallet is locked. Unlock first.");

    std::string viewKeyHex = common::podToHex(m_savedViewKey);
    std::string spendKeyHex = m_wallet.getType() == BoltCore::WalletType::ViewOnly
                                  ? ""
                                  : common::podToHex(m_savedSpendKey);

    uint32_t walletHeight = m_syncedHeight.load(std::memory_order_relaxed);

    // Build a JSON blob and base64 encode it for easy copy-paste
    std::ostringstream blob;
    blob << R"({"version":1,"viewKey":")" << viewKeyHex << R"(")"
         << R"(,"spendKey":")" << spendKeyHex << R"(")"
         << R"(,"walletHeight":)" << walletHeight
         << R"(,"address":")" << m_address << R"("})";

    std::string jsonBlob = blob.str();
    std::string encoded = base64Encode(jsonBlob);

    std::ostringstream ss;
    ss << R"({"wallet":")" << encoded << R"("})";
    return ss.str();
  }

  // Mainchain wallet methods
  std::string BoltRpcServer::methodGetBalance(const common::JsonValue &)
  {
    // Fast path: if wallet has no outputs, return zeros without blocking
    {
      std::lock_guard<std::mutex> lock(m_walletMutex);
      auto outputs = m_wallet.getOutputs();
      if (outputs.empty())
      {
        return R"({"availableBalance":0,"lockedAmount":0,"lockedDepositBalance":0,"unlockedDepositBalance":0})";
      }
    }

    // Wallet has outputs, take the lock again for the full balance query
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
    uint32_t nodeHeight = 0;
    uint32_t networkHeight = 0;
    size_t peerCount = 0;

    // Try to get daemon status, return zeros if daemon is unreachable
    try
    {
      nodeHeight = m_node.getLastLocalBlockHeight();
      networkHeight = m_node.getLastKnownBlockHeight();
      peerCount = m_node.getPeerCount();
    }
    catch (...)
    {
      // Daemon not reachable, return zeros
    }

    uint32_t syncedHeight = m_syncedHeight.load(std::memory_order_relaxed);

    std::ostringstream ss;
    ss << R"({"blockCount":)" << nodeHeight
       << R"(,"knownBlockCount":)" << networkHeight
       << R"(,"peerCount":)" << peerCount
       << R"(,"walletHeight":)" << syncedHeight;

    if (m_sidechainConnected)
    {
      ss << R"(,"sidechainHost":")" << m_sidechainHost << R"(")"
         << R"(,"sidechainPort":)" << m_sidechainPort;
    }

    ss << "}";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetSyncStatus(const common::JsonValue &params)
  {
    uint32_t walletHeight = m_syncedHeight.load();
    uint32_t nodeHeight = 0;

    try
    {
      nodeHeight = m_node.getLastLocalBlockHeight();
    }
    catch (...)
    {
      // Daemon not reachable
    }

    std::ostringstream ss;
    ss << R"({"walletHeight":)" << walletHeight
       << R"(,"nodeHeight":)" << nodeHeight
       << R"(,"synced":)" << (walletHeight >= nodeHeight && nodeHeight > 0 ? "true" : "false");

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

    submitTransaction(result);

    std::ostringstream ss;
    ss << R"({"transactionHash":")" << result.txHash << R"("})";
    return ss.str();
  }

  std::string BoltRpcServer::methodGetTransactions(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);
    auto outputs = m_wallet.getOutputs();

    // Pagination support
    uint32_t firstBlockIndex = params.contains("firstBlockIndex")
                                   ? static_cast<uint32_t>(params("firstBlockIndex").getInteger())
                                   : 0;
    uint32_t blockCount = params.contains("blockCount")
                              ? static_cast<uint32_t>(params("blockCount").getInteger())
                              : 0;

    uint32_t totalItems = static_cast<uint32_t>(outputs.size());

    // Sort by block height descending (newest first) for predictable pagination
    std::sort(outputs.begin(), outputs.end(),
              [](const BoltCore::OutputInfo &a, const BoltCore::OutputInfo &b)
              { return a.blockHeight > b.blockHeight; });

    std::ostringstream ss;
    ss << R"({"items":[)";
    bool first = true;
    uint32_t index = 0;
    for (const auto &out : outputs)
    {
      // Skip items before the requested start index
      if (index < firstBlockIndex)
      {
        ++index;
        continue;
      }
      // Stop if we've reached the requested page size
      if (blockCount > 0 && (index - firstBlockIndex) >= blockCount)
        break;

      if (!first)
        ss << ",";
      first = false;
      ss << R"({"hash":")" << common::podToHex(out.txHash) << R"(")"
         << R"(,"amount":)" << out.amount
         << R"(,"blockHeight":)" << out.blockHeight
         << R"(,"spent":)" << (out.spent ? "true" : "false")
         << R"(,"isDeposit":)" << (out.isDeposit ? "true" : "false")
         << "}";
      ++index;
    }
    ss << R"(],"totalItems":)" << totalItems
       << R"(,"firstBlockIndex":)" << firstBlockIndex
       << R"(,"blockCount":)" << (blockCount > 0 ? blockCount : totalItems) << "}";
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
    if (m_outputs)
      m_outputs->clear();

    m_syncedHeight.store(0, std::memory_order_relaxed);
    if (m_externalSyncedHeight)
      m_externalSyncedHeight->store(0, std::memory_order_relaxed);

    {
      std::lock_guard<std::mutex> lock(m_walletMutex);
      std::vector<BoltCore::OutputInfo> empty;
      m_wallet.loadOutputs(empty);
    }

    if (m_stateManager)
    {
      std::vector<BoltCore::OutputInfo> empty;
      // Preserve keys across reset so unlock still works
      std::string vkHex = m_locked ? "" : common::podToHex(m_savedViewKey);
      std::string skHex = (m_locked || m_wallet.getType() == BoltCore::WalletType::ViewOnly)
                              ? ""
                              : common::podToHex(m_savedSpendKey);
      m_stateManager->save(empty, 0, vkHex, skHex);
    }

    m_logger(logging::INFO) << "Wallet reset — will rescan from genesis";
    return R"({"status":"ok","message":"Wallet reset. Rescan will begin on next sync cycle."})";
  }

  std::string BoltRpcServer::methodSave(const common::JsonValue &)
  {
    if (saveWalletState())
      return R"({"status":"ok"})";
    return R"({"status":"error","message":"Failed to write state file"})";
  }

  bool BoltRpcServer::saveWalletState()
  {
    if (m_stateManager && m_outputs)
    {
      uint32_t height = m_externalSyncedHeight
                            ? m_externalSyncedHeight->load(std::memory_order_relaxed)
                            : m_syncedHeight.load(std::memory_order_relaxed);

      // Persist keys alongside outputs so unlock works across restarts
      std::string vkHex = m_locked ? "" : common::podToHex(m_savedViewKey);
      std::string skHex = (m_locked || m_wallet.getType() == BoltCore::WalletType::ViewOnly)
                              ? ""
                              : common::podToHex(m_savedSpendKey);

      return m_stateManager->save(*m_outputs, height, vkHex, skHex);
    }
    return false;
  }

  std::string BoltRpcServer::methodGetNetworkHeight(const common::JsonValue &)
  {
    uint32_t height = 0;
    try
    {
      height = m_node.getLastKnownBlockHeight();
    }
    catch (...)
    {
    }
    std::ostringstream ss;
    ss << R"({"networkHeight":)" << height << "}";
    return ss.str();
  }

  // Sidechain methods
  std::string BoltRpcServer::methodGetSidechainStatus(const common::JsonValue &)
  {
    if (m_sidechainConnected)
      return sidechainRpcCall("getStatus", "{}");

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

  // DEX methods
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

  // Bridge methods
  std::string BoltRpcServer::methodBridgeGetStatus(const common::JsonValue &)
  {
    return sidechainRpcCall("getBridgeStatus", "{}");
  }

  std::string BoltRpcServer::methodBridgeLock(const common::JsonValue &params)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);

    uint64_t amount = static_cast<uint64_t>(params("amount").getInteger());

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

  // Transaction submission
  void BoltRpcServer::submitTransaction(const BoltCore::TransferResult &result)
  {
    if (!result.success)
      throw std::runtime_error(result.error);

    m_logger(logging::INFO) << "Transaction completed: " << result.txHash;

    // Push transaction event to SSE clients
    if (m_sseBroadcaster)
    {
      std::ostringstream data;
      data << R"({"txHash":")" << result.txHash << R"("})";
      m_sseBroadcaster->broadcast("transaction", data.str());
    }
  }

  void BoltRpcServer::startSync(const std::string &dataDir,
                                const crypto::SecretKey &viewKey,
                                const crypto::PublicKey &viewPub,
                                const crypto::SecretKey *spendKey)
  {
    if (m_syncMonitor)
      return; // already running

    m_dataDir = dataDir;
    m_syncViewKey = viewKey;
    m_syncViewPub = viewPub;
    if (spendKey)
    {
      m_hasSpendKeyForSync = true;
      m_syncSpendKey = *spendKey;
    }

    uint32_t startHeight = m_syncedHeight.load(std::memory_order_relaxed);

    m_syncMonitor.reset(new SyncMonitor(
        m_node, viewKey, viewPub, spendKey, dataDir, startHeight,
        [this](const std::vector<BoltCore::OutputInfo> &newOuts, uint32_t newHeight)
        {
          onNewOutputs(newOuts, newHeight);
          if (m_outputs)
          {
            for (const auto &o : newOuts)
              m_outputs->push_back(o);
          }
          if (m_stateManager && m_outputs)
          {
            uint32_t height = m_externalSyncedHeight
                                  ? m_externalSyncedHeight->load(std::memory_order_relaxed)
                                  : m_syncedHeight.load(std::memory_order_relaxed);
            m_stateManager->save(*m_outputs, height,
                                 m_locked ? "" : common::podToHex(m_savedViewKey),
                                 (m_locked || m_wallet.getType() == BoltCore::WalletType::ViewOnly)
                                     ? ""
                                     : common::podToHex(m_savedSpendKey));
          }
        }));

    m_syncMonitor->start();
    m_logger(logging::INFO) << "Sync monitor started from height " << startHeight;
  }
} // namespace BoltRPC