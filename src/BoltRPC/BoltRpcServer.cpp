// BoltRpcServer.cpp — BoltRPC implementation with encrypted wallet security
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
#include "crypto/crypto.h"
#include "version.h"

#include "Storage/MDBXBlockchainStorage.h"
#include "Common/PathHelpers.h"

#include <sstream>
#include <future>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <boost/filesystem.hpp>

namespace BoltRPC
{
  namespace
  {
    // Portable secure memory zeroing — no external dependencies
    void secureZero(void *ptr, size_t len)
    {
      volatile unsigned char *p = static_cast<volatile unsigned char *>(ptr);
      while (len--)
      {
        *p++ = 0;
      }
    }

    void secureZero(std::string &str)
    {
      if (!str.empty())
      {
        secureZero(&str[0], str.size());
        str.clear();
      }
    }

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
      : m_logger(logger, "BoltRPC"), m_wallet(wallet), m_node(node),
        m_currency(currency), m_address(address), m_dispatcher(dispatcher), m_spvWallet(NULL)
  {
  }

  BoltRpcServer::~BoltRpcServer()
  {
    // Secure cleanup of sensitive material
    secureZero(m_derivedKey);
    secureZero(m_password);

    crypto::SecretKey emptyKey{};
    m_savedViewKey = emptyKey;
    m_savedSpendKey = emptyKey;
    m_syncViewKey = emptyKey;
    m_syncSpendKey = emptyKey;

    if (m_spvWallet)
    {
      delete m_spvWallet;
      m_spvWallet = NULL;
    }
  }

  void BoltRpcServer::start(const std::string &bindIp, uint16_t bindPort, size_t threadCount)
  {
    m_server.reset(new BoltHttp::Server(&m_dispatcher, threadCount));
    m_server->onRequest([this](const BoltHttp::Request &req, BoltHttp::Response &resp)
                        { handleRequest(req, resp); });
    m_server->start(bindIp, bindPort);
    m_logger(logging::INFO) << "BoltRPC listening on " << bindIp << ":" << bindPort;

    // Start auto-save timer if we have state persistence configured
    if (m_stateManager && m_outputs && !m_derivedKey.empty())
    {
      startAutoSave();
    }
  }

  void BoltRpcServer::stop()
  {
    // Stop SyncMonitor first — it holds a reference to m_node which will be
    // deleted by the caller immediately after stop() returns.
    if (m_syncMonitor)
    {
      m_syncMonitor->stop();
      m_syncMonitor.reset();
    }

    // Stop auto-save timer
    m_autoSaveRunning.store(false, std::memory_order_relaxed);
    if (m_autoSaveThread.joinable())
    {
      m_autoSaveThread.join();
    }

    // Final save if we have keys and password
    if (m_stateManager && !m_locked && !m_derivedKey.empty())
    {
      saveWalletState();
    }

    if (m_server)
      m_server->stop();

    // Secure cleanup
    secureZero(m_derivedKey);
    secureZero(m_password);
  }

  void BoltRpcServer::setSidechainConnection(const std::string &host, uint16_t port)
  {
    m_sidechainHost = host;
    m_sidechainPort = port;
    m_sidechainConnected = true;
    m_logger(logging::INFO) << "Sidechain connection set to " << host << ":" << port;
  }

  void BoltRpcServer::setDaemonConnection(const std::string &host, uint16_t port)
  {
    m_daemonHost = host;
    m_daemonPort = port;
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

  void BoltRpcServer::setPassword(const std::string &password)
  {
    m_password = password;

    // Derive the encryption key immediately
    crypto::Hash pwd_hash;
    crypto::cn_fast_hash(password.data(), password.size(), pwd_hash);

    m_derivedKey.assign(reinterpret_cast<const char *>(&pwd_hash), sizeof(pwd_hash));

    // Start auto-save if state manager is configured
    if (m_stateManager && m_outputs && m_autoSaveRunning.load(std::memory_order_relaxed) == false)
    {
      startAutoSave();
    }

    m_logger(logging::INFO) << "Password set — wallet encryption enabled";
  }

  void BoltRpcServer::startAutoSave()
  {
    if (m_autoSaveRunning.exchange(true, std::memory_order_relaxed))
      return; // Already running

    m_autoSaveThread = std::thread([this]()
                                   {
      m_logger(logging::INFO) << "Auto-save enabled (every 5 minutes)";
      
      while (m_autoSaveRunning.load(std::memory_order_relaxed))
      {
        // Sleep in 1-second increments so we can shut down quickly
        for (int i = 0; i < 300; ++i)
        {
          if (!m_autoSaveRunning.load(std::memory_order_relaxed))
            break;
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (m_autoSaveRunning.load(std::memory_order_relaxed) && !m_locked)
        {
          try
          {
            if (saveWalletState())
            {
              m_logger(logging::DEBUGGING) << "Auto-save completed";
            }
          }
          catch (const std::exception &e)
          {
            m_logger(logging::ERROR) << "Auto-save failed: " << e.what();
          }
        }
      }
      
      m_logger(logging::INFO) << "Auto-save stopped"; });
  }

  void BoltRpcServer::onNewOutputs(const std::vector<BoltCore::OutputInfo> &outputs,
                                   uint32_t newHeight)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);
    // Add new outputs incrementally — do NOT call loadOutputs() which would clear existing outputs.
    for (const auto &o : outputs)
      m_wallet.addOutput(o);
    m_wallet.setCurrentHeight(newHeight);
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
      else if (method == "changePassword")
        result = methodChangePassword(params);
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
  std::string BoltRpcServer::methodGetVersion(const common::JsonValue &)
  {
    return R"({"version":")" + std::string(CCX_RELEASE_VERSION) + R"("})";
  }

  // Wallet lifecycle
  // Import an existing wallet with view key and optional spend key, with password
  std::string BoltRpcServer::methodImportWallet(const common::JsonValue &params)
  {
    std::string viewKeyHex = params("viewKey").getString();
    std::string spendKeyHex = params.contains("spendKey") ? params("spendKey").getString() : "";
    std::string password = params.contains("password") ? params("password").getString() : "";

    // Validate keys
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

    // Set password if provided — this enables encrypted persistence
    if (!password.empty())
    {
      setPassword(password);
      m_logger(logging::INFO) << "Wallet imported with encryption: " << m_address;
    }
    else
    {
      // Clear any existing password/derived key
      secureZero(m_password);
      secureZero(m_derivedKey);
      m_logger(logging::WARNING) << "Wallet imported WITHOUT password — keys will not be persisted securely";
    }

    // Persist wallet state with encryption if password is set
    if (m_stateManager && m_outputs)
    {
      saveWalletState();
    }

    // Start sync monitor if data directory is configured
    if (!m_dataDir.empty() && !m_locked)
    {
      startSync(m_dataDir, m_savedViewKey,
                m_wallet.getSpendPublicKey(),
                m_wallet.getType() == BoltCore::WalletType::ViewOnly
                    ? nullptr
                    : &m_savedSpendKey);
    }

    // Clear sensitive data from memory after use
    secureZero(viewKeyHex);
    secureZero(spendKeyHex);
    std::string result = R"({"status":"ok","address":")" + m_address + R"("})";
    return result;
  }

  // Generate a new random wallet with keys encrypted by password
  std::string BoltRpcServer::methodGenerateWallet(const common::JsonValue &params)
  {
    std::string password = params.contains("password") ? params("password").getString() : "";

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

    // Set password for encryption
    if (!password.empty())
    {
      setPassword(password);
      m_logger(logging::INFO) << "New wallet generated with encryption: " << m_address;
    }
    else
    {
      // Clear any existing password
      secureZero(m_password);
      secureZero(m_derivedKey);
      m_logger(logging::WARNING) << "New wallet generated WITHOUT password — keys will not be persisted";
    }

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

    // Persist the wallet state
    if (m_stateManager && m_outputs)
    {
      saveWalletState();
    }

    // Start sync monitor if data directory is configured
    if (!m_dataDir.empty() && !m_locked)
    {
      startSync(m_dataDir, m_savedViewKey,
                m_wallet.getSpendPublicKey(),
                m_wallet.getType() == BoltCore::WalletType::ViewOnly
                    ? nullptr
                    : &m_savedSpendKey);
    }

    std::ostringstream ss;
    ss << R"({"status":"ok")"
       << R"(,"address":")" << m_address << R"(")"
       << R"(,"viewKey":")" << viewKeyHex << R"(")"
       << R"(,"spendKey":")" << spendKeyHex << R"(")"
       << R"(,"mnemonic":")" << mnemonic << R"(")"
       << "}";

    // Clear hex strings from memory
    secureZero(viewKeyHex);
    secureZero(spendKeyHex);

    return ss.str();
  }

  // Unlock wallet with password — decrypts and loads keys from state file
  std::string BoltRpcServer::methodUnlock(const common::JsonValue &params)
  {
    if (!m_locked && !m_derivedKey.empty())
    {
      return R"({"status":"ok","message":"Already unlocked"})";
    }

    if (!m_stateManager)
      throw std::runtime_error("State manager not configured. Cannot unlock.");

    if (!m_stateManager->exists())
      throw std::runtime_error("No wallet state file found. Use importWallet or generateWallet first.");

    std::string password = params("password").getString();

    // Check if state file is encrypted
    bool encrypted = m_stateManager->isEncrypted();

    if (encrypted && password.empty())
      throw std::runtime_error("Wallet is encrypted — password required for unlock");

    // Load keys from state file with decryption
    std::string viewKeyHex, spendKeyHex;
    if (!m_stateManager->loadKeys(viewKeyHex, spendKeyHex, password))
    {
      throw std::runtime_error("Failed to unlock wallet. Wrong password or corrupt state file.");
    }

    if (viewKeyHex.empty())
      throw std::runtime_error("No keys found in state file. Use importWallet to set keys first.");

    // Parse the keys
    crypto::SecretKey viewKey, spendKey;
    if (!BoltSync::hexToSecretKey(viewKeyHex, viewKey))
    {
      secureZero(viewKeyHex);
      secureZero(spendKeyHex);
      throw std::runtime_error("Invalid view key in state file");
    }

    if (!spendKeyHex.empty() && !BoltSync::hexToSecretKey(spendKeyHex, spendKey))
    {
      secureZero(viewKeyHex);
      secureZero(spendKeyHex);
      throw std::runtime_error("Invalid spend key in state file");
    }

    crypto::PublicKey viewPub, spendPub;
    crypto::secret_key_to_public_key(viewKey, viewPub);
    if (!spendKeyHex.empty())
      crypto::secret_key_to_public_key(spendKey, spendPub);

    // Reconstruct wallet
    m_wallet.~Wallet();
    new (&m_wallet) BoltCore::Wallet(viewKey, spendKey, viewPub, spendPub, m_node, m_currency);
    m_address = m_currency.accountAddressAsString({spendPub, viewPub});

    // Reload outputs into wallet
    if (m_outputs)
    {
      m_wallet.loadOutputs(*m_outputs, m_node.getLastLocalBlockHeight());
    }

    m_savedViewKey = viewKey;
    m_savedSpendKey = spendKey;
    m_locked = false;

    // Set the password and derived key for future saves
    if (!password.empty())
    {
      setPassword(password);
    }

    // Start sync monitor if applicable
    if (!m_dataDir.empty() && !m_syncMonitor)
    {
      startSync(m_dataDir, m_savedViewKey,
                m_wallet.getSpendPublicKey(),
                m_wallet.getType() == BoltCore::WalletType::ViewOnly
                    ? nullptr
                    : &m_savedSpendKey);
    }

    // Clear sensitive hex strings
    secureZero(viewKeyHex);
    secureZero(spendKeyHex);

    m_logger(logging::INFO) << "Wallet unlocked: " << m_address;

    std::ostringstream ss;
    ss << R"({"status":"ok","address":")" << m_address
       << R"(","encrypted":)" << (encrypted ? "true" : "false") << "}";
    return ss.str();
  }

  // Lock the wallet: encrypt and save state, then zero out keys in memory
  std::string BoltRpcServer::methodLock(const common::JsonValue &)
  {
    if (m_locked)
      return R"({"status":"ok","message":"Already locked"})";

    BoltCore::WalletType type = m_wallet.getType();
    if (type == BoltCore::WalletType::ViewOnly)
    {
      // View-only wallet: just mark as locked
      m_locked = true;

      // Clear any sensitive material
      secureZero(m_derivedKey);
      secureZero(m_password);

      return R"({"status":"ok","message":"View-only wallet locked"})";
    }

    // Full wallet: save state with encryption before locking
    if (!saveWalletState())
    {
      m_logger(logging::WARNING) << "Failed to save wallet state before locking";
    }

    // Zero out keys in memory
    crypto::PublicKey viewPub = m_wallet.getViewPublicKey();

    // Securely clear the saved keys
    crypto::SecretKey emptyKey{};
    m_savedViewKey = emptyKey;
    m_savedSpendKey = emptyKey;

    // Reconstruct wallet as view-only
    m_wallet.~Wallet();
    new (&m_wallet) BoltCore::Wallet(crypto::SecretKey{}, crypto::SecretKey{},
                                     viewPub, crypto::PublicKey{}, m_node, m_currency);
    m_locked = true;

    // Clear derived key and password from memory
    secureZero(m_derivedKey);
    secureZero(m_password);

    m_logger(logging::INFO) << "Wallet locked — keys cleared from memory";

    return R"({"status":"ok","message":"Wallet locked. Use unlock with password to restore keys."})";
  }

  // Change the wallet encryption password
  std::string BoltRpcServer::methodChangePassword(const common::JsonValue &params)
  {
    if (m_locked)
      throw std::runtime_error("Wallet is locked. Unlock first.");

    std::string oldPassword = params.contains("oldPassword") ? params("oldPassword").getString() : "";
    std::string newPassword = params("newPassword").getString();

    if (newPassword.empty())
      throw std::runtime_error("New password cannot be empty");

    // If there's an existing encryption, verify the old password
    if (!m_derivedKey.empty())
    {
      if (oldPassword.empty())
        throw std::runtime_error("Old password required to change encryption");

      // Verify old password by deriving key and comparing
      crypto::Hash oldHash;
      crypto::cn_fast_hash(oldPassword.data(), oldPassword.size(), oldHash);
      std::string oldDerived(reinterpret_cast<const char *>(&oldHash), sizeof(oldHash));

      if (oldDerived != m_derivedKey)
      {
        secureZero(oldDerived);
        throw std::runtime_error("Old password is incorrect");
      }
      secureZero(oldDerived);
    }

    // Set the new password
    setPassword(newPassword);

    // Immediately save with new encryption
    if (!saveWalletState())
    {
      throw std::runtime_error("Failed to save wallet with new password");
    }

    m_logger(logging::INFO) << "Wallet password changed";
    return R"({"status":"ok","message":"Password changed successfully"})";
  }

  // Return the current wallet sync height
  std::string BoltRpcServer::methodGetWalletHeight(const common::JsonValue &)
  {
    uint32_t walletHeight = m_syncedHeight.load(std::memory_order_relaxed);

    // Also check SyncMonitor progress during active scans
    if (m_syncMonitor)
    {
      uint32_t scanHeight = m_syncMonitor->lastScannedHeight();
      if (scanHeight > walletHeight)
        walletHeight = scanHeight;
    }

    uint32_t outputCount = 0;
    {
      std::lock_guard<std::mutex> lock(m_walletMutex);
      outputCount = static_cast<uint32_t>(m_wallet.getOutputs().size());
    }
    std::ostringstream ss;
    ss << R"({"walletHeight":)" << walletHeight
       << R"(,"outputCount":)" << outputCount
       << R"(,"locked":)" << (m_locked ? "true" : "false")
       << R"(,"encrypted":)" << (!m_derivedKey.empty() ? "true" : "false")
       << "}";
    return ss.str();
  }

  // Return the current view key (only when unlocked)
  std::string BoltRpcServer::methodGetViewKey(const common::JsonValue &)
  {
    if (m_locked)
      return R"({"viewKey":"","locked":true})";

    std::string viewKeyHex = common::podToHex(m_savedViewKey);
    std::string result = R"({"viewKey":")" + viewKeyHex + R"("})";
    secureZero(viewKeyHex);
    return result;
  }

  // Return the current spend key (only when unlocked and full wallet)
  std::string BoltRpcServer::methodGetSpendKey(const common::JsonValue &)
  {
    if (m_locked || m_wallet.getType() == BoltCore::WalletType::ViewOnly)
      return R"({"spendKey":"","locked":)" + std::string(m_locked ? "true" : "false") + R"(})";

    std::string spendKeyHex = common::podToHex(m_savedSpendKey);
    std::string result = R"({"spendKey":")" + spendKeyHex + R"("})";
    secureZero(spendKeyHex);
    return result;
  }

  // Export wallet as base64-encoded JSON blob (only when unlocked)
  std::string BoltRpcServer::methodExportWallet(const common::JsonValue &)
  {
    if (m_locked)
      throw std::runtime_error("Wallet is locked. Unlock first.");

    std::string viewKeyHex = common::podToHex(m_savedViewKey);
    std::string spendKeyHex = m_wallet.getType() == BoltCore::WalletType::ViewOnly
                                  ? ""
                                  : common::podToHex(m_savedSpendKey);

    uint32_t walletHeight = m_syncedHeight.load(std::memory_order_relaxed);

    // Build a JSON blob with wallet data
    std::ostringstream blob;
    blob << R"({"version":2)"
         << R"(,"address":")" << m_address << R"(")"
         << R"(,"viewKey":")" << viewKeyHex << R"(")"
         << R"(,"spendKey":")" << spendKeyHex << R"(")"
         << R"(,"walletHeight":)" << walletHeight
         << R"(,"encrypted":)" << (!m_derivedKey.empty() ? "true" : "false")
         << "}";

    std::string jsonBlob = blob.str();
    std::string encoded = base64Encode(jsonBlob);

    // Clear sensitive data
    secureZero(viewKeyHex);
    secureZero(spendKeyHex);

    std::ostringstream ss;
    ss << R"({"wallet":")" << encoded << R"("})";
    return ss.str();
  }

  // Mainchain wallet methods
  std::string BoltRpcServer::methodGetBalance(const common::JsonValue &)
  {
    std::lock_guard<std::mutex> lock(m_walletMutex);
    auto outputs = m_wallet.getOutputs();

    if (outputs.empty())
    {
      return R"({"availableBalance":0,"lockedAmount":0,"lockedDepositBalance":0,"unlockedDepositBalance":0,"accruedInterest":0,"currentHeight":0})";
    }

    auto bal = m_wallet.getBalance();
    uint64_t accruedInterest = 0;
    uint32_t currentHeight = m_wallet.getCurrentHeight();

    // Calculate interest for deposits
    for (const auto &out : outputs)
    {
      if (out.isDeposit && !out.spent && out.term > 0)
      {
        uint32_t maturityHeight = out.blockHeight + out.term;
        if (currentHeight >= maturityHeight)
        {
          // Use the same calculation as DepositManager
          accruedInterest += m_currency.calculateInterest(out.amount, out.term, currentHeight);
        }
      }
    }

    std::ostringstream ss;
    ss << R"({"availableBalance":)" << bal.actual + accruedInterest // Add interest to available
       << R"(,"lockedAmount":)" << bal.pending
       << R"(,"lockedDepositBalance":)" << bal.lockedDeposit
       << R"(,"unlockedDepositBalance":)" << bal.unlockedDeposit
       << R"(,"accruedInterest":)" << accruedInterest
       << R"(,"currentHeight":)" << currentHeight
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
       << R"(,"walletHeight":)" << syncedHeight
       << R"(,"locked":)" << (m_locked ? "true" : "false")
       << R"(,"encrypted":)" << (!m_derivedKey.empty() ? "true" : "false");

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
    if (m_lightMode && m_spvWallet)
    {
      uint32_t localHeight = m_spvWallet->getSyncedHeight();
      uint32_t remoteHeight = m_spvWallet->getNodeHeight();
      bool synced = (localHeight >= remoteHeight);

      std::ostringstream ss;
      ss << R"({"walletHeight":)" << localHeight
         << R"(,"nodeHeight":)" << remoteHeight
         << R"(,"synced":)" << (synced ? "true" : "false")
         << R"(,"lightMode":true)";

      if (m_sidechainConnected)
      {
        ss << R"(,"sidechainHost":")" << m_sidechainHost << R"(")"
           << R"(,"sidechainPort":)" << m_sidechainPort;
      }

      ss << "}";
      return ss.str();
    }

    // Original full node code...
    uint32_t walletHeight = m_syncedHeight.load();

    // Also check SyncMonitor progress during active scans
    if (m_syncMonitor)
    {
      uint32_t scanHeight = m_syncMonitor->lastScannedHeight();
      if (scanHeight > walletHeight)
        walletHeight = scanHeight;
    }

    uint32_t nodeHeight = m_node.getLastKnownBlockHeight();

    if (nodeHeight == 0 && !m_daemonHost.empty())
    {
      try
      {
        BoltHttp::HttpClient client(m_daemonHost, m_daemonPort);
        auto resp = client.get("/getheight");
        if (resp.success && !resp.body.empty())
        {
          auto hpos = resp.body.find("\"height\":");
          if (hpos != std::string::npos)
          {
            hpos += 9;
            while (hpos < resp.body.size() && !std::isdigit(resp.body[hpos]))
              ++hpos;
            uint32_t h = 0;
            while (hpos < resp.body.size() && std::isdigit(resp.body[hpos]))
              h = h * 10 + (resp.body[hpos++] - '0');
            if (h > 0)
              nodeHeight = h;
          }
        }
      }
      catch (...)
      {
      }
    }

    bool synced = (nodeHeight > 0) && (walletHeight + 1 >= nodeHeight);

    std::ostringstream ss;
    ss << R"({"walletHeight":)" << walletHeight
       << R"(,"nodeHeight":)" << nodeHeight
       << R"(,"synced":)" << (synced ? "true" : "false");

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
    if (m_locked)
      throw std::runtime_error("Wallet is locked. Unlock first to send transactions.");

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

    // Light mode: Verify the transaction via SPV after submission
    if (m_lightMode && m_spvWallet && !result.txHash.empty())
    {
      crypto::Hash tx_hash;
      if (common::podFromHex(result.txHash, tx_hash))
      {
        m_logger(logging::INFO) << "Verifying transaction via SPV: " << result.txHash;

        // Wait a few seconds for the transaction to be mined (in light mode, we rely on remote node)
        std::this_thread::sleep_for(std::chrono::seconds(5));

        SPV::TransactionProof proof;
        if (m_spvWallet->getTransactionProof(tx_hash, proof))
        {
          if (m_spvWallet->verifyTransaction(proof))
          {
            m_logger(logging::INFO) << "Transaction verified via SPV in block " << proof.block_height;
          }
          else
          {
            m_logger(logging::WARNING) << "Transaction verification failed: " << result.txHash;
          }
        }
        else
        {
          // Transaction may still be in mempool, not yet mined
          m_logger(logging::INFO) << "Transaction submitted but not yet confirmed (still in mempool)";
        }
      }
    }

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
    if (m_locked)
      throw std::runtime_error("Wallet is locked. Unlock first.");

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
    if (m_locked)
      throw std::runtime_error("Wallet is locked. Unlock first.");

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
    if (m_locked)
      throw std::runtime_error("Wallet is locked. Unlock first.");

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

  std::string BoltRpcServer::methodReset(const common::JsonValue &params)
  {
    bool preserveEncryption = params.contains("preserveEncryption")
                                  ? params("preserveEncryption").getBool()
                                  : true;

    if (m_outputs)
      m_outputs->clear();

    m_syncedHeight.store(0, std::memory_order_relaxed);
    if (m_externalSyncedHeight)
      m_externalSyncedHeight->store(0, std::memory_order_relaxed);

    {
      std::lock_guard<std::mutex> lock(m_walletMutex);
      std::vector<BoltCore::OutputInfo> empty;
      m_wallet.loadOutputs(empty, 0);
    }

    if (m_stateManager)
    {
      // Save empty outputs but preserve keys if requested
      if (preserveEncryption && !m_locked)
      {
        std::string vkHex = common::podToHex(m_savedViewKey);
        std::string skHex = (m_wallet.getType() == BoltCore::WalletType::ViewOnly)
                                ? ""
                                : common::podToHex(m_savedSpendKey);

        std::vector<BoltCore::OutputInfo> empty;
        m_stateManager->save(empty, 0, vkHex, skHex, m_password);

        secureZero(vkHex);
        secureZero(skHex);
      }
      else
      {
        std::vector<BoltCore::OutputInfo> empty;
        m_stateManager->save(empty, 0, "", "", "");
      }
    }

    m_logger(logging::INFO) << "Wallet reset — will rescan from genesis";
    return R"({"status":"ok","message":"Wallet reset. Rescan will begin on next sync cycle."})";
  }

  std::string BoltRpcServer::methodSave(const common::JsonValue &params)
  {
    // Allow optional password parameter for saving without setting it permanently
    std::string password = params.contains("password") ? params("password").getString() : m_password;

    if (m_stateManager && m_outputs)
    {
      uint32_t height = m_externalSyncedHeight
                            ? m_externalSyncedHeight->load(std::memory_order_relaxed)
                            : m_syncedHeight.load(std::memory_order_relaxed);

      std::string vkHex = m_locked ? "" : common::podToHex(m_savedViewKey);
      std::string skHex = (m_locked || m_wallet.getType() == BoltCore::WalletType::ViewOnly)
                              ? ""
                              : common::podToHex(m_savedSpendKey);

      bool success = m_stateManager->save(*m_outputs, height, vkHex, skHex, password);

      secureZero(vkHex);
      secureZero(skHex);

      if (success)
        return R"({"status":"ok","encrypted":)" + std::string(password.empty() ? "false" : "true") + R"(})";
    }

    return R"({"status":"error","message":"Failed to write state file"})";
  }

  bool BoltRpcServer::saveWalletState()
  {
    if (!m_stateManager || !m_outputs)
      return false;

    uint32_t height = m_externalSyncedHeight
                          ? m_externalSyncedHeight->load(std::memory_order_relaxed)
                          : m_syncedHeight.load(std::memory_order_relaxed);

    // Determine keys to save — empty strings if locked
    std::string vkHex = m_locked ? "" : common::podToHex(m_savedViewKey);
    std::string skHex = (m_locked || m_wallet.getType() == BoltCore::WalletType::ViewOnly)
                            ? ""
                            : common::podToHex(m_savedSpendKey);

    bool success = m_stateManager->save(*m_outputs, height, vkHex, skHex, m_password);

    secureZero(vkHex);
    secureZero(skHex);

    return success;
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
    if (m_locked)
      throw std::runtime_error("Wallet is locked. Unlock first.");

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

    // Auto-save after successful transaction
    if (m_stateManager && !m_locked)
    {
      try
      {
        saveWalletState();
      }
      catch (...)
      {
        m_logger(logging::WARNING) << "Failed to auto-save after transaction";
      }
    }
  }

  void BoltRpcServer::startSync(const std::string &dataDir,
                                const crypto::SecretKey &viewKey,
                                const crypto::PublicKey &spendPub,
                                const crypto::SecretKey *spendKey)
  {
    if (m_syncMonitor)
      return;

    m_dataDir = dataDir;
    m_syncViewKey = viewKey;
    m_syncSpendPub = spendPub;
    if (spendKey)
    {
      m_hasSpendKeyForSync = true;
      m_syncSpendKey = *spendKey;
    }

    uint32_t startHeight = m_syncedHeight.load(std::memory_order_relaxed);

    // Clear stale checkpoint for fresh wallets
    if (startHeight == 0)
    {
      std::string dbPath = PathHelpers::appendPath(dataDir, "mdbx_blocks");
      CryptoNote::MDBXBlockchainStorage storage(dbPath, 0);
      storage.putMeta("wallet_init_progress", std::vector<uint8_t>());
      storage.flush();
    }

    m_logger(logging::INFO) << "Starting sync from height " << startHeight;

    m_syncMonitor.reset(new SyncMonitor(
        m_node, viewKey, spendPub, spendKey, dataDir, startHeight,
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
            saveWalletState();
          }
        }));

    m_syncMonitor->start();
  }

  void BoltRpcServer::enableLightMode(const std::string &headersFile,
                                      const std::string &bootstrapUrl,
                                      const std::string &remoteHost,
                                      uint16_t remotePort)
  {
    m_lightMode = true;
    m_headersFile = headersFile;
    m_bootstrapUrl = bootstrapUrl;
    m_remoteHost = remoteHost;
    m_remotePort = remotePort;

    m_logger(logging::INFO) << "Initializing light mode with remote node " << remoteHost << ":" << remotePort;

    // Use raw pointer with new (C++11 compatible)
    m_spvWallet = new SPV::SPVWallet(remoteHost, remotePort);

    // Use the directory of the state file for headers cache
    std::string cacheDir = ".";
    if (m_stateManager)
    {
      boost::filesystem::path statePath(m_stateManager->getFilePath());
      cacheDir = statePath.parent_path().string();
      if (cacheDir.empty())
        cacheDir = ".";
    }

    // Create cache directory if it doesn't exist
    boost::filesystem::path cachePath(cacheDir);
    if (!boost::filesystem::exists(cachePath))
    {
      boost::filesystem::create_directories(cachePath);
    }

    std::string defaultHeadersPath = cacheDir + "/headers.bin";
    m_spvWallet->setDefaultPath(defaultHeadersPath);

    m_logger(logging::INFO) << "Headers cache path: " << defaultHeadersPath;

    bool headersLoaded = false;

    // Try to load from default cache first
    if (m_spvWallet->initFromBootstrap(defaultHeadersPath))
    {
      m_logger(logging::INFO) << "Loaded cached headers from " << defaultHeadersPath;
      headersLoaded = true;
    }
    // Then try user-provided bootstrap file
    else if (!headersFile.empty() && m_spvWallet->initFromBootstrap(headersFile))
    {
      m_logger(logging::INFO) << "Loaded bootstrap headers from " << headersFile;
      // Save to cache for next time
      if (m_spvWallet->saveHeaders())
      {
        m_logger(logging::INFO) << "Cached headers to " << defaultHeadersPath;
      }
      else
      {
        m_logger(logging::WARNING) << "Failed to cache headers to " << defaultHeadersPath;
      }
      headersLoaded = true;
    }
    // Then try download
    else if (!bootstrapUrl.empty())
    {
      m_logger(logging::INFO) << "Downloading bootstrap from " << bootstrapUrl;
      if (m_spvWallet->initFromDownload(bootstrapUrl))
      {
        if (m_spvWallet->saveHeaders())
        {
          m_logger(logging::INFO) << "Cached downloaded headers to " << defaultHeadersPath;
        }
        headersLoaded = true;
      }
    }

    if (!headersLoaded)
    {
      m_logger(logging::ERROR) << "No valid headers found. Please provide --headers-file with a bootstrap file";
      return;
    }

    // Verify checkpoints
    if (!m_spvWallet->verifyCheckpoints())
    {
      m_logger(logging::ERROR) << "Checkpoint verification failed! Headers file may be corrupted or from wrong chain";
      return;
    }

    m_logger(logging::INFO) << "Headers loaded and verified, synced to height " << m_spvWallet->getSyncedHeight();
  }

  uint32_t BoltRpcServer::getNodeHeightLight() const
  {
    if (m_spvWallet)
    {
      return m_spvWallet->getNodeHeight();
    }
    return m_node.getLastLocalBlockHeight();
  }
} // namespace BoltRPC