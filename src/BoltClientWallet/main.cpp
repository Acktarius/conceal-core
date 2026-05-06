// conceal-bolt – Terminal UI wallet powered by BoltSync + BoltCore
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "BoltCore/BoltCore.h"

#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include <System/Dispatcher.h>
#include "NodeRpcProxy/NodeRpcProxy.h"
#include "Rpc/CoreRpcServerCommandsDefinitions.h"

using namespace cn;
namespace po = boost::program_options;

// Configuration
struct Config
{
  std::string dataDir;
  std::string daemonHost = "127.0.0.1";
  uint16_t daemonPort = 16000;
  std::string viewKeyHex;
  std::string spendKeyHex;
  unsigned int scanThreads = 0;
  bool skipScan = false;
};

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("Conceal Bolt - Terminal Wallet");
  desc.add_options()("help,h", "Show help")("data-dir", po::value<std::string>(), "Path to blockchain data directory (for scanning)")("daemon-host", po::value<std::string>()->default_value("127.0.0.1"), "Daemon RPC host")("daemon-port", po::value<uint16_t>()->default_value(16000), "Daemon RPC port")("view-key", po::value<std::string>(), "64-char hex private view key")("spend-key", po::value<std::string>(), "64-char hex private spend key (optional for view-only)")("threads", po::value<unsigned int>()->default_value(0), "Scan threads (0=auto)")("skip-scan", po::bool_switch(), "Skip blockchain scan (use if already scanned)");

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

    if (vm.count("data-dir"))
      cfg.dataDir = vm["data-dir"].as<std::string>();
    cfg.daemonHost = vm["daemon-host"].as<std::string>();
    cfg.daemonPort = vm["daemon-port"].as<uint16_t>();
    if (vm.count("view-key"))
      cfg.viewKeyHex = vm["view-key"].as<std::string>();
    if (vm.count("spend-key"))
      cfg.spendKeyHex = vm["spend-key"].as<std::string>();
    cfg.scanThreads = vm["threads"].as<unsigned int>();
    cfg.skipScan = vm["skip-scan"].as<bool>();
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    return false;
  }

  if (cfg.viewKeyHex.size() != 64)
  {
    std::cerr << "Error: view-key must be 64 hex characters" << std::endl;
    return false;
  }
  if (!cfg.spendKeyHex.empty() && cfg.spendKeyHex.size() != 64)
  {
    std::cerr << "Error: spend-key must be 64 hex characters" << std::endl;
    return false;
  }
  return true;
}

// Simple menu helpers
void clearScreen() { std::cout << "\033[2J\033[1;1H" << std::flush; }

std::string formatAmount(uint64_t amount)
{
  std::string s = std::to_string(amount);
  int pos = s.length() - 3;
  while (pos > 0)
  {
    s.insert(pos, ",");
    pos -= 3;
  }
  return s;
}

// Main
int main(int argc, char *argv[])
{
  Config cfg;
  if (!parseArgs(argc, argv, cfg))
    return 1;

  // Keys
  crypto::SecretKey viewKey;
  if (!BoltSync::hexToSecretKey(cfg.viewKeyHex, viewKey))
  {
    std::cerr << "Invalid view key" << std::endl;
    return 1;
  }
  crypto::SecretKey spendKey;
  bool hasSpendKey = !cfg.spendKeyHex.empty();
  if (hasSpendKey && !BoltSync::hexToSecretKey(cfg.spendKeyHex, spendKey))
  {
    std::cerr << "Invalid spend key" << std::endl;
    return 1;
  }

  crypto::PublicKey viewPub, spendPub;
  crypto::secret_key_to_public_key(viewKey, viewPub);
  if (hasSpendKey)
    crypto::secret_key_to_public_key(spendKey, spendPub);

  // Currency
  logging::LoggerManager logManager;
  logging::ConsoleLogger logger;
  Currency currency = CurrencyBuilder(logManager).currency();
  std::string addressStr = currency.accountAddressAsString({spendPub, viewPub});

  // Connect to daemon
  platform_system::Dispatcher dispatcher;
  NodeRpcProxy node(dispatcher, cfg.daemonHost, cfg.daemonPort, logger);
  // Init node? NodeRpcProxy doesn't have explicit init; it auto-connects on first call.
  // But we need to ensure it's ready. We'll trust it.

  // Scan blockchain (unless skipped)
  std::vector<BoltSync::FoundOutput> allOutputs;
  if (!cfg.skipScan && !cfg.dataDir.empty())
  {
    std::cout << "\nScanning blockchain... This may take a few minutes.\n"
              << std::endl;
    BoltSync::Scanner scanner(viewKey, viewPub, hasSpendKey ? &spendKey : nullptr);
    BoltSync::ScanConfig scanCfg;
    scanCfg.dataDir = cfg.dataDir;
    scanCfg.numThreads = cfg.scanThreads;
    BoltSync::ScanState state;
    if (!scanner.scan(scanCfg, state))
    {
      std::cerr << "Scan failed. Check data-dir and that the daemon has synced the chain." << std::endl;
      return 1;
    }
    allOutputs = std::move(state.results);
    std::cout << "Scan complete. Found " << allOutputs.size() << " outputs.\n"
              << std::endl;
  }

  // Populate BoltCore wallet
  std::vector<BoltCore::OutputInfo> outputInfos;
  for (const auto &fo : allOutputs)
  {
    BoltCore::OutputInfo info;
    info.blockHeight = fo.blockHeight;
    info.txHash = fo.txHash;
    info.outputIndex = fo.outputIndex;
    info.globalOutputIndex = fo.outputIndex; // TODO: fix global index - not available from BoltSync yet
    info.amount = fo.amount;
    info.outputKey = fo.outputKey;
    info.txPublicKey = fo.txPublicKey;
    info.keyImage = fo.keyImage;
    info.spent = fo.spent;
    info.isDeposit = false; // We'll refine later
    info.term = 0;
    info.subAddress = addressStr;
    outputInfos.push_back(info);
  }

  BoltCore::Wallet wallet(viewKey, spendKey, viewPub, spendPub, node, currency);
  wallet.loadOutputs(outputInfos);

  // Main menu
  std::string input;
  while (true)
  {
    clearScreen();
    auto balance = wallet.getBalance();
    std::cout << "=== Conceal Bolt Wallet ============" << std::endl;
    std::cout << "Address: " << addressStr << std::endl;
    std::cout << "Balance: " << formatAmount(balance.actual) << " (pending " << formatAmount(balance.pending) << ")" << std::endl;
    std::cout << "Deposits locked: " << formatAmount(balance.lockedDeposit) << " unlocked: " << formatAmount(balance.unlockedDeposit) << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "1. Send transfer" << std::endl;
    std::cout << "2. Create deposit" << std::endl;
    std::cout << "3. Withdraw deposit" << std::endl;
    std::cout << "4. Optimize (fusion)" << std::endl;
    std::cout << "5. Generate sub-address" << std::endl;
    std::cout << "6. List sub-addresses" << std::endl;
    std::cout << "7. Show transaction history (soon)" << std::endl;
    std::cout << "8. Exit" << std::endl;
    std::cout << "Choice: ";
    std::getline(std::cin, input);

    if (input == "1")
    {
      std::string destAddr;
      uint64_t amount;
      std::cout << "Destination address: ";
      std::getline(std::cin, destAddr);
      std::cout << "Amount: ";
      std::cin >> amount;
      std::cin.ignore();
      auto res = wallet.transfer(destAddr, amount);
      std::cout << (res.success ? "Sent! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter to continue..." << std::endl;
      std::cin.get();
    }
    else if (input == "2")
    {
      uint64_t amount;
      uint32_t term;
      std::cout << "Amount: ";
      std::cin >> amount;
      std::cout << "Term (blocks): ";
      std::cin >> term;
      std::cin.ignore();
      auto res = wallet.createDeposit(amount, term);
      std::cout << (res.success ? "Deposit created! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (input == "3")
    {
      uint64_t depositId;
      std::cout << "Deposit ID: ";
      std::cin >> depositId;
      std::cin.ignore();
      auto res = wallet.withdrawDeposit(depositId);
      std::cout << (res.success ? "Withdrawn! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (input == "4")
    {
      auto est = wallet.estimateFusion(1000000);
      std::cout << "Fusion ready: " << est.fusionReadyCount << " outputs (total: " << est.totalOutputCount << ")" << std::endl;
      if (est.fusionReadyCount > 0)
      {
        std::cout << "Create fusion? (y/n): ";
        std::string yn;
        std::getline(std::cin, yn);
        if (yn == "y")
        {
          auto res = wallet.createFusion(1000000, cn::parameters::MINIMUM_MIXIN);
          std::cout << (res.success ? "Fusion tx: " + res.txHash : "Error: " + res.error) << std::endl;
        }
      }
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (input == "5")
    {
      auto sub = wallet.generateSubAddress();
      std::cout << "New sub-address: " << sub.address << std::endl;
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (input == "6")
    {
      auto subs = wallet.getSubAddresses();
      std::cout << "Sub-addresses:" << std::endl;
      for (size_t i = 0; i < subs.size(); ++i)
        std::cout << "  " << i << ": " << subs[i].address << std::endl;
      std::cout << "Press enter..." << std::endl;
      std::cin.get();
    }
    else if (input == "8")
    {
      break;
    }
  }

  return 0;
}