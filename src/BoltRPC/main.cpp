// BoltRPC — drop-in walletd replacement using BoltSync + BoltCore
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <boost/program_options.hpp>

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "BoltCore/BoltCore.h"

#include "BoltRpcServer.h"
#include "StateManager.h"
#include "SyncMonitor.h"

#include "Common/SignalHandler.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include <System/Dispatcher.h>

#include "version.h"

namespace po = boost::program_options;
using namespace cn;

namespace
{
  // Portable secure memory zeroing without external dependencies
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
} // namespace

struct Config
{
  bool testnet = false;
  std::string viewKeyHex;
  std::string spendKeyHex;
  std::string dataDir;
  bool skipScan = false;
  unsigned int scanThreads = 0;
  std::string daemonHost = "127.0.0.1";
  uint16_t daemonPort = 16000;
  std::string bindIp = "127.0.0.1";
  uint16_t bindPort = 8070;
  std::string stateFile = "bolt-wallet.state";
  std::string sidechainHost;
  uint16_t sidechainPort = 8080;
  size_t rpcThreads = 1;
  std::string password;
};

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("BoltRPC — Conceal RPC Wallet (walletd replacement)");
  desc.add_options()("help,h", "Show help")("view-key", po::value<std::string>()->default_value(""), "64-char hex private view key (optional at startup)")("spend-key", po::value<std::string>()->default_value(""), "64-char hex private spend key (optional, for full wallet)")("password", po::value<std::string>()->default_value(""), "Passphrase to encrypt wallet state file (optional at startup)")("data-dir", po::value<std::string>()->default_value(""), "Path to blockchain MDBX data directory (required for initial scan)")("skip-scan", po::bool_switch(), "Skip initial chain scan, load from state file instead")("threads", po::value<unsigned int>()->default_value(0), "Number of scan threads (0 = auto)")("daemon-host", po::value<std::string>()->default_value("127.0.0.1"), "Daemon RPC host")("daemon-port", po::value<uint16_t>()->default_value(16000), "Daemon RPC port")("bind-ip", po::value<std::string>()->default_value("127.0.0.1"), "RPC server bind IP")("bind-port", po::value<uint16_t>()->default_value(8070), "RPC server bind port")("state-file", po::value<std::string>()->default_value("bolt-wallet.state"), "File to persist wallet state")("sidechain-host", po::value<std::string>()->default_value(""), "Sidechain validator RPC host")("sidechain-port", po::value<uint16_t>()->default_value(8080), "Sidechain validator RPC port")("testnet", po::bool_switch(), "Run in testnet mode")("rpc-threads", po::value<size_t>()->default_value(1), "RPC server thread count");

  po::variables_map vm;
  try
  {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help"))
    {
      std::cout << desc << std::endl;
      return false;
    }
    po::notify(vm);
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl
              << desc << std::endl;
    return false;
  }

  cfg.viewKeyHex = vm["view-key"].as<std::string>();
  cfg.spendKeyHex = vm["spend-key"].as<std::string>();
  cfg.password = vm["password"].as<std::string>();

  if (!cfg.viewKeyHex.empty() && cfg.viewKeyHex.size() != 64)
  {
    std::cerr << "Error: view-key must be 64 hex characters" << std::endl;
    return false;
  }

  if (!cfg.spendKeyHex.empty() && cfg.spendKeyHex.size() != 64)
  {
    std::cerr << "Error: spend-key must be 64 hex characters" << std::endl;
    return false;
  }

  cfg.dataDir = vm["data-dir"].as<std::string>();
  cfg.skipScan = vm["skip-scan"].as<bool>();
  cfg.scanThreads = vm["threads"].as<unsigned int>();
  cfg.daemonHost = vm["daemon-host"].as<std::string>();
  cfg.daemonPort = vm["daemon-port"].as<uint16_t>();
  cfg.bindIp = vm["bind-ip"].as<std::string>();
  cfg.bindPort = vm["bind-port"].as<uint16_t>();
  cfg.stateFile = vm["state-file"].as<std::string>();
  cfg.sidechainHost = vm["sidechain-host"].as<std::string>();
  cfg.sidechainPort = vm["sidechain-port"].as<uint16_t>();
  cfg.testnet = vm["testnet"].as<bool>();
  cfg.rpcThreads = vm["rpc-threads"].as<size_t>();

  return true;
}

int main(int argc, char *argv[])
{
  Config cfg;
  if (!parseArgs(argc, argv, cfg))
    return 1;

  logging::LoggerManager logManager;
  logging::ConsoleLogger consoleLogger;
  logging::LoggerRef logger(logManager, "BoltRPC");

  logger(logging::INFO) << "BoltRPC starting (" << CCX_RELEASE_VERSION << ")";

  // Keys (optional at startup)
  crypto::SecretKey viewKey{};
  crypto::SecretKey spendKey{};
  crypto::PublicKey viewPub{};
  crypto::PublicKey spendPub{};
  bool hasViewKey = !cfg.viewKeyHex.empty();
  bool hasSpendKey = !cfg.spendKeyHex.empty();
  bool hasPassword = !cfg.password.empty();

  std::string address = "No wallet loaded";

  if (hasViewKey)
  {
    if (!BoltSync::hexToSecretKey(cfg.viewKeyHex, viewKey))
    {
      logger(logging::ERROR) << "Invalid view key hex";
      return 1;
    }

    if (hasSpendKey && !BoltSync::hexToSecretKey(cfg.spendKeyHex, spendKey))
    {
      logger(logging::ERROR) << "Invalid spend key hex";
      return 1;
    }

    if (!crypto::secret_key_to_public_key(viewKey, viewPub))
    {
      logger(logging::ERROR) << "Failed to derive public view key";
      return 1;
    }

    if (hasSpendKey && !crypto::secret_key_to_public_key(spendKey, spendPub))
    {
      logger(logging::ERROR) << "Failed to derive public spend key";
      return 1;
    }

    cn::Currency currency = cn::CurrencyBuilder(logManager).currency();
    address = currency.accountAddressAsString({spendPub, viewPub});
    logger(logging::INFO) << "Wallet address: " << address;
  }
  else
  {
    logger(logging::INFO) << "No view key provided — starting in setup mode";
    logger(logging::INFO) << "Use the GUI or importWallet RPC to set keys later";
  }

  // Connect to daemon
  platform_system::Dispatcher dispatcher;
  cn::Currency currency = cn::CurrencyBuilder(logManager).currency();
  NodeRpcProxy node(dispatcher, cfg.daemonHost, cfg.daemonPort, consoleLogger);
  NodeInitObserver initObs;
  node.init([&initObs](std::error_code ec)
            { initObs.initCompleted(ec); });
  try
  {
    initObs.waitForInitEnd();
    logger(logging::INFO) << "Connected to daemon " << cfg.daemonHost << ":" << cfg.daemonPort;
  }
  catch (...)
  {
    logger(logging::WARNING) << "Daemon not reachable — running in offline mode (no transactions can be sent)";
  }

  // Load or scan wallet state (outputs only — keys are loaded separately via RPC unlock)
  BoltRPC::StateManager stateManager(cfg.stateFile);
  std::vector<BoltCore::OutputInfo> outputInfos;
  std::atomic<uint32_t> lastScannedHeight{0};

  uint32_t initialHeight = 0;
  bool stateLoaded = false;

  // Determine if an encrypted state file exists and whether we have keys
  bool encryptedState = stateManager.exists() && stateManager.isEncrypted();

  if (encryptedState)
  {
    // Encrypted state file exists — load outputs only (no keys without password)
    logger(logging::INFO) << "Found encrypted wallet state file";

    if (!stateManager.load(outputInfos, initialHeight))
    {
      logger(logging::ERROR) << "Failed to load encrypted state file";
      return 1;
    }

    lastScannedHeight.store(initialHeight, std::memory_order_relaxed);
    stateLoaded = true;
    logger(logging::INFO) << "Loaded " << outputInfos.size()
                          << " outputs (last height " << initialHeight
                          << ") — wallet is locked";

    // If password was provided on CLI, attempt to unlock immediately
    if (hasPassword && hasViewKey)
    {
      // Keys were provided on CLI — they override the stored keys
      logger(logging::INFO) << "CLI keys provided with password — encrypting state with new keys";
    }
    else if (hasPassword)
    {
      // Password provided but no keys — try to unlock the existing encrypted state
      std::string storedViewHex, storedSpendHex;
      if (stateManager.loadKeys(storedViewHex, storedSpendHex, cfg.password))
      {
        logger(logging::INFO) << "State file unlocked with CLI password";

        if (!storedViewHex.empty())
        {
          if (!BoltSync::hexToSecretKey(storedViewHex, viewKey))
          {
            logger(logging::ERROR) << "Invalid view key in state file";
            return 1;
          }
          hasViewKey = true;

          if (!crypto::secret_key_to_public_key(viewKey, viewPub))
          {
            logger(logging::ERROR) << "Failed to derive public view key from stored key";
            return 1;
          }
        }

        if (!storedSpendHex.empty())
        {
          if (!BoltSync::hexToSecretKey(storedSpendHex, spendKey))
          {
            logger(logging::ERROR) << "Invalid spend key in state file";
            return 1;
          }
          hasSpendKey = true;

          if (!crypto::secret_key_to_public_key(spendKey, spendPub))
          {
            logger(logging::ERROR) << "Failed to derive public spend key from stored key";
            return 1;
          }
        }

        if (hasViewKey)
        {
          address = currency.accountAddressAsString({spendPub, viewPub});
          logger(logging::INFO) << "Wallet unlocked: " << address;
        }
      }
      else
      {
        logger(logging::WARNING) << "Failed to unlock state file with provided password — starting locked";
      }
    }
    else
    {
      logger(logging::INFO) << "Wallet is encrypted — use 'unlock' RPC with password to access keys";
    }
  }
  else if (stateManager.exists())
  {
    // Legacy unencrypted state file
    logger(logging::INFO) << "Found unencrypted wallet state file (legacy format)";
    logger(logging::WARNING) << "Consider using 'importWallet' or 'save' with a password to encrypt it";

    if (!stateManager.load(outputInfos, initialHeight))
    {
      logger(logging::ERROR) << "Failed to load state file";
      return 1;
    }

    lastScannedHeight.store(initialHeight, std::memory_order_relaxed);
    stateLoaded = true;
    logger(logging::INFO) << "Loaded " << outputInfos.size()
                          << " outputs (last height " << initialHeight << ")";

    // Try to load keys from unencrypted blob
    std::string storedViewHex, storedSpendHex;
    if (stateManager.loadKeys(storedViewHex, storedSpendHex, ""))
    {
      if (!hasViewKey && !storedViewHex.empty())
      {
        if (BoltSync::hexToSecretKey(storedViewHex, viewKey))
        {
          hasViewKey = true;
          crypto::secret_key_to_public_key(viewKey, viewPub);
          logger(logging::INFO) << "Loaded view key from unencrypted state (no password set)";
        }
      }

      if (!hasSpendKey && !storedSpendHex.empty())
      {
        if (BoltSync::hexToSecretKey(storedSpendHex, spendKey))
        {
          hasSpendKey = true;
          crypto::secret_key_to_public_key(spendKey, spendPub);
          logger(logging::INFO) << "Loaded spend key from unencrypted state (no password set)";
        }
      }

      if (hasViewKey)
      {
        address = currency.accountAddressAsString({spendPub, viewPub});
        logger(logging::WARNING) << "Keys loaded from unencrypted state — wallet is vulnerable";
        logger(logging::WARNING) << "Use 'save' RPC with a password to encrypt your wallet";
      }
    }
  }
  else if (hasViewKey)
  {
    // No state file exists, but keys were provided on CLI
    logger(logging::INFO) << "No existing state file — keys will be used for initial scan";

    if (!cfg.dataDir.empty())
    {
      logger(logging::INFO) << "Starting BoltSync scan on " << cfg.dataDir;

      BoltSync::Scanner scanner(viewKey, viewPub, hasSpendKey ? &spendKey : nullptr);
      BoltSync::ScanConfig scanCfg;
      scanCfg.dataDir = cfg.dataDir;
      scanCfg.numThreads = cfg.scanThreads;

      BoltSync::ScanState state;
      if (!scanner.scan(scanCfg, state))
      {
        logger(logging::ERROR) << "BoltSync scan failed. Is the data directory correct?";
        return 1;
      }

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
      logger(logging::INFO) << "Scan complete — " << outputInfos.size() << " outputs found";

      // Save state — encrypted if password provided, otherwise save without key blob
      if (hasPassword)
      {
        stateManager.save(outputInfos, nodeHeight, cfg.viewKeyHex, cfg.spendKeyHex, cfg.password);
        logger(logging::INFO) << "Encrypted state saved to " << cfg.stateFile;
      }
      else
      {
        // Save outputs only — no key blob (keys must be provided again on next start or via RPC)
        logger(logging::WARNING) << "No password provided — state saved without keys";
        logger(logging::WARNING) << "Keys will NOT be persisted. Use 'importWallet' or 'save' RPC to persist your wallet";
        stateManager.save(outputInfos, nodeHeight, "", "");
        logger(logging::INFO) << "State (outputs only) saved to " << cfg.stateFile;
      }
    }
    else
    {
      logger(logging::INFO) << "No --data-dir — keys loaded in memory only (no scan performed)";
    }
  }
  else
  {
    // No state file and no keys on CLI — completely fresh start
    logger(logging::INFO) << "No state file and no keys — fresh wallet instance";
    logger(logging::INFO) << "Use 'importWallet' or 'generateWallet' RPC to set up a wallet";
  }

  // Initialize wallet
  BoltCore::Wallet wallet(viewKey, spendKey, viewPub, spendPub, node, currency);
  if (hasViewKey)
  {
    wallet.loadOutputs(outputInfos);

    auto balance = wallet.getBalance();
    logger(logging::INFO) << "Balance: " << balance.actual
                          << " pending: " << balance.pending;
  }
  else
  {
    logger(logging::INFO) << "Wallet not loaded — waiting for keys via RPC";
  }

  // RPC server
  BoltRPC::BoltRpcServer rpcServer(dispatcher, consoleLogger,
                                   wallet, node, currency, address);

  // Wire up StateManager so save() and auto-save can persist wallet state
  rpcServer.setStateManager(&stateManager, &outputInfos, &lastScannedHeight);

  // Pass the data directory to the RPC server so importWallet can start sync
  if (!cfg.dataDir.empty())
    rpcServer.setDataDir(cfg.dataDir);

  // If we have a password from CLI and keys are loaded, set it in the RPC server
  // This enables auto-save and lock/unlock without re-prompting
  if (hasPassword && hasViewKey)
  {
    rpcServer.setPassword(cfg.password);
    logger(logging::INFO) << "Wallet initialized — password configured for session";
  }
  else if (encryptedState && !hasPassword)
  {
    logger(logging::INFO) << "Encrypted wallet loaded — waiting for 'unlock' RPC with password";
  }
  else if (!hasPassword && hasViewKey)
  {
    logger(logging::WARNING) << "Wallet loaded without password — keys held in memory only";
    logger(logging::WARNING) << "Use 'save' RPC with a password to persist your wallet securely";
  }

  // Connect to sidechain if configured
  if (!cfg.sidechainHost.empty())
  {
    rpcServer.setSidechainConnection(cfg.sidechainHost, cfg.sidechainPort);
    logger(logging::INFO) << "Sidechain connection: " << cfg.sidechainHost << ":" << cfg.sidechainPort;
  }
  else
  {
    logger(logging::INFO) << "No sidechain configured. Use --sidechain-host to enable sidechain features.";
  }

  // If keys were provided at startup and data dir is available, begin incremental sync now
  if (hasViewKey && !cfg.dataDir.empty())
  {
    rpcServer.startSync(cfg.dataDir, viewKey, viewPub,
                        hasSpendKey ? &spendKey : nullptr);
  }

  rpcServer.start(cfg.bindIp, cfg.bindPort, cfg.rpcThreads);
  rpcServer.setSyncedHeight(lastScannedHeight.load());

  // Graceful shutdown
  std::atomic<bool> stopRequested{false};
  tools::SignalHandler::install([&stopRequested]
                                { stopRequested = true; });

  logger(logging::INFO) << "BoltRPC ready on " << cfg.bindIp << ":" << cfg.bindPort;

  if (hasViewKey)
  {
    logger(logging::INFO) << "Wallet loaded: " << address;
    if (!hasPassword && encryptedState)
    {
      logger(logging::INFO) << "Wallet is LOCKED — use 'unlock' RPC to access spending functions";
    }
  }
  else
  {
    logger(logging::INFO) << "Setup mode — waiting for 'importWallet' or 'generateWallet' RPC call";
  }

  if (!cfg.sidechainHost.empty())
  {
    logger(logging::INFO) << "Sidechain features enabled via " << cfg.sidechainHost << ":" << cfg.sidechainPort;
    logger(logging::INFO) << "  Mainchain: getBalance, transfer, createDeposit, withdrawDeposit, getDeposits";
    logger(logging::INFO) << "  Sidechain: getSidechainTokens, sidechainTransfer, sidechainCreateToken, getTokenBalance";
    logger(logging::INFO) << "  DEX:      dexGetOrderBook, dexPlaceOrder, dexCancelOrder, dexGetMyOrders, dexGetTradeHistory";
    logger(logging::INFO) << "  Bridge:   bridgeGetStatus, bridgeLock, bridgeUnlock";
  }

  logger(logging::INFO) << "Press Ctrl+C to stop";

  while (!stopRequested)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

  logger(logging::INFO) << "Shutting down...";

  // Attempt to save state on shutdown if we have keys and password
  // The RPC server will handle secure cleanup in its destructor
  rpcServer.stop();

  node.shutdown();

  // Secure cleanup of any sensitive data remaining in config
  secureZero(cfg.password);
  secureZero(cfg.viewKeyHex);
  secureZero(cfg.spendKeyHex);

  logger(logging::INFO) << "BoltRPC stopped";
  return 0;
}