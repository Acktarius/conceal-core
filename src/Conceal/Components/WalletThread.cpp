// WalletThread.cpp — wallet RPC wrapper for unified Conceal client
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "WalletThread.h"

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "BoltCore/BoltCore.h"
#include "BoltRPC/BoltRpcServer.h"
#include "BoltRPC/StateManager.h"
#include "BoltRPC/SyncMonitor.h"

#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "Logging/LoggerManager.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include <System/Dispatcher.h>

using namespace cn;

namespace Conceal
{

  namespace
  {
    // Lightweight promise-based observer for async daemon connection
    class NodeInitObserver
    {
    public:
      void initCompleted(std::error_code ec)
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_fulfilled)
          return;
        m_ec = ec;
        m_fulfilled = true;
        m_promise.set_value();
      }
      void waitForInitEnd()
      {
        m_promise.get_future().get();
        if (m_ec)
          throw std::system_error(m_ec);
      }

    private:
      std::error_code m_ec;
      std::promise<void> m_promise;
      std::mutex m_mutex;
      bool m_fulfilled = false;
    };
  }

  void runWallet(const Config &cfg, WalletStatus &status, std::atomic<bool> &stopRequested)
  {
    logging::LoggerManager logManager;
    logging::LoggerRef logger(logManager, "wallet");

    logger(logging::INFO) << "Wallet starting...";

    // Determine which keys to use (wallet keys or bridge keys via --use-bridge-wallet)
    std::string viewKeyHex = cfg.walletViewKey;
    std::string spendKeyHex = cfg.walletSpendKey;

    if (cfg.useBridgeWallet)
    {
      if (cfg.bridgeViewKey.empty())
      {
        logger(logging::ERROR) << "--use-bridge-wallet set but no --bridge-view-key provided";
        return;
      }
      viewKeyHex = cfg.bridgeViewKey;
      spendKeyHex = cfg.bridgeSpendKey;
      logger(logging::INFO) << "Wallet using bridge keys";
    }

    bool hasViewKey = !viewKeyHex.empty();
    bool hasSpendKey = !spendKeyHex.empty();

    // Parse keys into crypto structures
    crypto::SecretKey viewKey{};
    crypto::SecretKey spendKey{};
    crypto::PublicKey viewPub{};
    crypto::PublicKey spendPub{};
    std::string address = "No wallet loaded";

    if (hasViewKey)
    {
      BoltSync::hexToSecretKey(viewKeyHex, viewKey);
      crypto::secret_key_to_public_key(viewKey, viewPub);

      if (hasSpendKey)
      {
        BoltSync::hexToSecretKey(spendKeyHex, spendKey);
        crypto::secret_key_to_public_key(spendKey, spendPub);
      }

      cn::Currency currency = cn::CurrencyBuilder(logManager).currency();
      address = currency.accountAddressAsString({spendPub, viewPub});
      status.setAddress(address);
      logger(logging::INFO) << "Wallet: " << address;
    }
    else
    {
      logger(logging::INFO) << "No view key — wallet in setup mode";
    }

    // Connect to local daemon (same process, shared MDBX)
    platform_system::Dispatcher dispatcher;
    cn::Currency currency = cn::CurrencyBuilder(logManager).currency();
    NodeRpcProxy node(dispatcher, "127.0.0.1", cfg.rpcBindPort, logManager);
    NodeInitObserver initObs;
    node.init([&initObs](std::error_code ec)
              { initObs.initCompleted(ec); });
    try
    {
      initObs.waitForInitEnd();
      logger(logging::INFO) << "Connected to daemon";
    }
    catch (...)
    {
      logger(logging::WARNING) << "Daemon not reachable — offline mode";
    }

    // Wallet state file path inside the shared data directory
    std::string stateFile = cfg.dataDir;
    if (!stateFile.empty() && stateFile.back() != '/')
      stateFile += '/';
    stateFile += "bolt-wallet.state";

    // Mainchain MDBX path for BoltSync to scan
    std::string mainDataDir = cfg.dataDir;
    if (!mainDataDir.empty() && mainDataDir.back() != '/')
      mainDataDir += '/';
    mainDataDir += "main";

    BoltRPC::StateManager stateManager(stateFile);
    std::vector<BoltCore::OutputInfo> outputInfos;
    std::atomic<uint32_t> lastScannedHeight{0};

    // Initial scan or resume from saved state
    if (hasViewKey)
    {
      uint32_t initialHeight = 0;
      if (stateManager.exists())
      {
        logger(logging::INFO) << "Loading wallet state...";
        if (stateManager.load(outputInfos, initialHeight))
        {
          lastScannedHeight.store(initialHeight, std::memory_order_relaxed);
          logger(logging::INFO) << "Loaded " << outputInfos.size() << " outputs";
        }
      }
      else
      {
        logger(logging::INFO) << "Starting BoltSync scan on " << mainDataDir;

        BoltSync::Scanner scanner(viewKey, viewPub, hasSpendKey ? &spendKey : nullptr);
        BoltSync::ScanConfig scanCfg;
        scanCfg.dataDir = mainDataDir;
        scanCfg.numThreads = cfg.scanThreads;

        BoltSync::ScanState state;
        if (scanner.scan(scanCfg, state))
        {
          // Convert BoltSync results to BoltCore OutputInfo format
          for (const auto &fo : state.results)
          {
            BoltCore::OutputInfo info;
            info.blockHeight = fo.blockHeight;
            info.txHash = fo.txHash;
            info.outputIndex = fo.outputIndex;
            info.globalOutputIndex = fo.outputIndex;
            info.amount = fo.amount;
            info.outputKey = fo.outputKey;
            info.txPublicKey = fo.txPublicKey;
            info.keyImage = fo.keyImage;
            info.spent = fo.spent;
            info.isDeposit = false;
            info.term = 0;
            info.subAddress = address;
            outputInfos.push_back(info);
          }
          uint32_t nodeHeight = node.getLastLocalBlockHeight();
          lastScannedHeight.store(nodeHeight, std::memory_order_relaxed);
          stateManager.save(outputInfos, nodeHeight);
          logger(logging::INFO) << "Scan complete: " << outputInfos.size() << " outputs";
        }
      }
    }

    // Initialize wallet engine with parsed keys
    BoltCore::Wallet wallet(viewKey, spendKey, viewPub, spendPub, node, currency);
    if (hasViewKey)
    {
      wallet.loadOutputs(outputInfos);
      auto bal = wallet.getBalance();
      status.availableBalance = bal.actual;
      status.lockedBalance = bal.pending;
    }

    // Start the JSON-RPC server on the configured port
    BoltRPC::BoltRpcServer rpcServer(dispatcher, logManager, wallet, node, currency, address);
    rpcServer.setStateManager(&stateManager, &outputInfos, &lastScannedHeight);

    // Auto-connect to sidechain if the validator is running in this process
    if (cfg.runSidechain)
    {
      rpcServer.setSidechainConnection("127.0.0.1", cfg.sidechainBindPort);
    }

    // Incremental sync monitor: scans new blocks as they arrive
    std::unique_ptr<BoltRPC::SyncMonitor> syncMonitor;
    if (hasViewKey && !mainDataDir.empty())
    {
      uint32_t startHeight = lastScannedHeight.load(std::memory_order_relaxed);
      syncMonitor.reset(new BoltRPC::SyncMonitor(
          node, viewKey, viewPub, hasSpendKey ? &spendKey : nullptr,
          mainDataDir, startHeight,
          [&](const std::vector<BoltCore::OutputInfo> &newOuts, uint32_t newHeight)
          {
            rpcServer.onNewOutputs(newOuts, newHeight);
            for (const auto &o : newOuts)
              outputInfos.push_back(o);
            lastScannedHeight.store(newHeight, std::memory_order_relaxed);
            stateManager.save(outputInfos, newHeight);
          }));
      syncMonitor->start();
    }

    rpcServer.start(cfg.rpcBindIp, cfg.walletBindPort, cfg.rpcThreads);
    logger(logging::INFO) << "Wallet RPC on " << cfg.rpcBindIp << ":" << cfg.walletBindPort;

    // Wire SSE broadcaster for real-time push to GUI
    rpcServer.setSseBroadcaster(&rpcServer.server()->sseBroadcaster());

    // SSE keep-alive ping every 30 seconds to prevent connection drops
    BoltHttp::SseBroadcaster *broadcaster = &rpcServer.server()->sseBroadcaster();
    std::thread sseKeepAlive([broadcaster, &stopRequested]()
                             {
      while (!stopRequested)
      {
          for (int i = 0; i < 30 && !stopRequested; ++i)
              std::this_thread::sleep_for(std::chrono::seconds(1));
          if (!stopRequested)
              broadcaster->pingAll();
      } });

    // Periodic status broadcast via SSE every 2 seconds
    std::thread statusBroadcast([broadcaster, &status, &stopRequested]()
                                {
      while (!stopRequested)
      {
          std::this_thread::sleep_for(std::chrono::seconds(2));
          if (!stopRequested)
          {
              std::ostringstream data;
              data << "{"
                   << R"("mainchainHeight":)" << status.walletHeight << ","
                   << R"("availableBalance":)" << status.availableBalance << ","
                   << R"("lockedBalance":)" << status.lockedBalance << ","
                   << R"("address":")" << status.getAddress() << R"(")"
                   << "}";
              broadcaster->broadcast("status", data.str());
          }
      } });

    // Auto-save timer: periodically persist wallet state without RPC calls
    std::thread autoSaveThread;
    if (cfg.walletAutoSaveInterval > 0)
    {
      autoSaveThread = std::thread([&]()
                                   {
        while (!stopRequested)
        {
          for (uint32_t i = 0; i < cfg.walletAutoSaveInterval && !stopRequested; ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
          if (!stopRequested)
          {
            rpcServer.saveWalletState();
            logger(logging::TRACE) << "Wallet auto-saved";
          }
        } });
    }

    // Status update loop for the TUI dashboard
    while (!stopRequested)
    {
      if (hasViewKey)
      {
        auto bal = wallet.getBalance();
        status.availableBalance = bal.actual;
        status.lockedBalance = bal.pending;
      }
      status.walletHeight = lastScannedHeight.load(std::memory_order_relaxed);
      status.synced = (status.walletHeight >= status.walletHeight);

      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Graceful shutdown
    if (sseKeepAlive.joinable())
      sseKeepAlive.join();
    if (statusBroadcast.joinable())
      statusBroadcast.join();
    if (autoSaveThread.joinable())
      autoSaveThread.join();
    if (syncMonitor)
    {
      syncMonitor->stop();
      stateManager.save(outputInfos, syncMonitor->lastScannedHeight());
    }
    // Final save on shutdown
    rpcServer.saveWalletState();
    rpcServer.stop();
    node.shutdown();
    logger(logging::INFO) << "Wallet stopped.";
  }

} // namespace Conceal