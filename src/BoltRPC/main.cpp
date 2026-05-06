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

struct Config
{
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
};

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("BoltRPC — Conceal RPC Wallet (walletd replacement)");
  desc.add_options()("help,h", "Show help")("view-key", po::value<std::string>(), "64-char hex private view key (required)")("spend-key", po::value<std::string>()->default_value(""), "64-char hex private spend key (optional, for full wallet)")("data-dir", po::value<std::string>()->default_value(""), "Path to blockchain MDBX data directory (required for initial scan)")("skip-scan", po::bool_switch(), "Skip initial chain scan, load from state file instead")("threads", po::value<unsigned int>()->default_value(0), "Number of scan threads (0 = auto)")("daemon-host", po::value<std::string>()->default_value("127.0.0.1"), "Daemon RPC host")("daemon-port", po::value<uint16_t>()->default_value(16000), "Daemon RPC port")("bind-ip", po::value<std::string>()->default_value("127.0.0.1"), "RPC server bind IP")("bind-port", po::value<uint16_t>()->default_value(8070), "RPC server bind port")("state-file", po::value<std::string>()->default_value("bolt-wallet.state"), "File to persist wallet state");

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

  if (!vm.count("view-key"))
  {
    std::cerr << "Error: --view-key is required" << std::endl;
    std::cout << desc << std::endl;
    return false;
  }

  cfg.viewKeyHex = vm["view-key"].as<std::string>();
  cfg.spendKeyHex = vm["spend-key"].as<std::string>();
  cfg.dataDir = vm["data-dir"].as<std::string>();
  cfg.skipScan = vm["skip-scan"].as<bool>();
  cfg.scanThreads = vm["threads"].as<unsigned int>();
  cfg.daemonHost = vm["daemon-host"].as<std::string>();
  cfg.daemonPort = vm["daemon-port"].as<uint16_t>();
  cfg.bindIp = vm["bind-ip"].as<std::string>();
  cfg.bindPort = vm["bind-port"].as<uint16_t>();
  cfg.stateFile = vm["state-file"].as<std::string>();

  if (cfg.viewKeyHex.size() != 64)
  {
    std::cerr << "Error: view-key must be 64 hex characters" << std::endl;
    return false;
  }
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

  // --- Keys ---
  crypto::SecretKey viewKey;
  if (!BoltSync::hexToSecretKey(cfg.viewKeyHex, viewKey))
  {
    logger(logging::ERROR) << "Invalid view key hex";
    return 1;
  }
  crypto::SecretKey spendKey{};
  bool hasSpendKey = !cfg.spendKeyHex.empty();
  if (hasSpendKey && !BoltSync::hexToSecretKey(cfg.spendKeyHex, spendKey))
  {
    logger(logging::ERROR) << "Invalid spend key hex";
    return 1;
  }

  crypto::PublicKey viewPub, spendPub{};
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
  std::string address = currency.accountAddressAsString({spendPub, viewPub});
  logger(logging::INFO) << "Wallet address: " << address;

  // --- Connect to daemon ---
  platform_system::Dispatcher dispatcher;
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

  // --- Load or scan wallet state ---
  BoltRPC::StateManager stateManager(cfg.stateFile);
  std::vector<BoltCore::OutputInfo> outputInfos;
  uint32_t lastScannedHeight = 0;

  if (cfg.skipScan && stateManager.exists())
  {
    logger(logging::INFO) << "Loading wallet state from " << cfg.stateFile;
    if (!stateManager.load(outputInfos, lastScannedHeight))
    {
      logger(logging::ERROR) << "Failed to load state file";
      return 1;
    }
    logger(logging::INFO) << "Loaded " << outputInfos.size()
                          << " outputs (last height " << lastScannedHeight << ")";
  }
  else if (!cfg.dataDir.empty())
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
      info.globalOutputIndex = fo.outputIndex; // BoltSync doesn't provide global, but it's okay for spending
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

    lastScannedHeight = node.getLastLocalBlockHeight();
    logger(logging::INFO) << "Scan complete — " << outputInfos.size() << " outputs found";

    stateManager.save(outputInfos, lastScannedHeight);
    logger(logging::INFO) << "State saved to " << cfg.stateFile;
  }
  else
  {
    logger(logging::WARNING) << "No --data-dir and no state file — starting with empty wallet.";
    logger(logging::WARNING) << "Perform an initial scan using: ./conceal-rpc --data-dir <path> --view-key ...";
    // Continue with empty wallet; user can later rescan by restarting with --data-dir
  }

  // --- Initialize wallet ---
  BoltCore::Wallet wallet(viewKey, spendKey, viewPub, spendPub, node, currency);
  wallet.loadOutputs(outputInfos);

  auto balance = wallet.getBalance();
  logger(logging::INFO) << "Balance: " << balance.actual
                        << " pending: " << balance.pending;

  // --- RPC server ---
  BoltRPC::BoltRpcServer rpcServer(dispatcher, consoleLogger,
                                   wallet, node, currency, address);

  // --- Incremental sync monitor ---
  BoltRPC::SyncMonitor syncMonitor(
      node, viewKey, viewPub, hasSpendKey ? &spendKey : nullptr,
      cfg.dataDir, lastScannedHeight,
      [&](const std::vector<BoltCore::OutputInfo> &newOuts, uint32_t newHeight)
      {
        rpcServer.onNewOutputs(newOuts, newHeight);
        for (const auto &o : newOuts)
          outputInfos.push_back(o);
        stateManager.save(outputInfos, newHeight);
      });

  syncMonitor.start();
  rpcServer.start(cfg.bindIp, cfg.bindPort);

  // --- Graceful shutdown ---
  std::atomic<bool> stopRequested{false};
  tools::SignalHandler::install([&stopRequested]
                                { stopRequested = true; });

  logger(logging::INFO) << "BoltRPC ready on " << cfg.bindIp << ":" << cfg.bindPort;
  logger(logging::INFO) << "Press Ctrl+C to stop";

  while (!stopRequested)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

  logger(logging::INFO) << "Shutting down...";
  syncMonitor.stop();
  rpcServer.stop();
  stateManager.save(outputInfos, syncMonitor.lastScannedHeight());
  node.shutdown();
  logger(logging::INFO) << "BoltRPC stopped";
  return 0;
}