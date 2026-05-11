// CLI.cpp — command-line argument handling implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "CLI.h"
#include <boost/program_options.hpp>
#include <iostream>

namespace po = boost::program_options;

namespace Conceal
{

  bool parseArgs(int argc, char *argv[], Config &cfg)
  {
    // Build the full options description with all flags grouped by subsystem
    po::options_description desc("Conceal — Unified Node, Sidechain, and Wallet");
    desc.add_options()
        // Help and setup
        ("help,h", "Show help")
        ("data-dir", po::value<std::string>()->default_value("conceal-data"), "Data directory for all components")
        ("testnet", po::bool_switch(), "Run in testnet mode")

        // Mainchain
        ("no-mainchain", po::bool_switch(), "Disable mainchain daemon")
        ("p2p-bind-ip", po::value<std::string>()->default_value("0.0.0.0"), "P2P bind IP")
        ("p2p-bind-port", po::value<uint16_t>()->default_value(15000), "P2P bind port")
        ("rpc-bind-ip", po::value<std::string>()->default_value("127.0.0.1"), "RPC bind IP")
        ("rpc-bind-port", po::value<uint16_t>()->default_value(16000), "RPC bind port")

        // Sidechain
        ("validator", po::bool_switch(), "Start sidechain validator")
        ("watch-bridge", po::bool_switch(), "Watch main chain for CCX deposits")
        ("bridge-view-key", po::value<std::string>()->default_value(""), "Bridge view key (64-char hex)")
        ("bridge-spend-key", po::value<std::string>()->default_value(""), "Bridge spend key (64-char hex)")
        ("seed-host", po::value<std::string>()->default_value(""), "Seed validator host")
        ("seed-port", po::value<uint16_t>()->default_value(0), "Seed validator RPC port")
        ("sidechain-port", po::value<uint16_t>()->default_value(8080), "Sidechain RPC bind port")
        ("reward-address", po::value<std::string>()->default_value(""), "Block reward address")
        ("dex-fee", po::value<double>()->default_value(0.0), "DEX trading fee percentage")

        // Wallet
        ("no-wallet", po::bool_switch(), "Disable wallet RPC server")
        ("use-bridge-wallet", po::bool_switch(), "Use bridge keys for wallet instead of separate wallet keys")
        ("wallet-view-key", po::value<std::string>()->default_value(""), "Wallet view key (64-char hex)")
        ("wallet-spend-key", po::value<std::string>()->default_value(""), "Wallet spend key (64-char hex)")
        ("wallet-port", po::value<uint16_t>()->default_value(8070), "Wallet RPC bind port")
        ("scan-threads", po::value<size_t>()->default_value(0), "Wallet scan threads (0 = auto)")

        // Headless / GUI integration
        ("no-tui", po::bool_switch(), "Disable terminal UI, run as headless server for GUI/automation")
        ("status-file", po::value<std::string>()->default_value(""), "Write JSON status snapshot to this file every 2 seconds")
        ("ready-signal", po::value<std::string>()->default_value(""), "Create this sentinel file when all RPC services are accepting connections")
        ("log-file", po::value<std::string>()->default_value(""), "Redirect all log output to this file instead of stdout")
        ("wallet-auto-save", po::value<uint32_t>()->default_value(0), "Auto-save wallet state every N seconds (0 = disabled)")

        // Misc
        ("rpc-threads", po::value<size_t>()->default_value(1), "RPC server thread count");

    // Parse the command line, show help if requested
    po::variables_map vm;
    try
    {
      po::store(po::parse_command_line(argc, argv, desc), vm);
      if (vm.count("help"))
      {
        std::cout << desc << std::endl;
        return false; // Return false to signal caller to exit cleanly
      }
      po::notify(vm);
    }
    catch (const std::exception &e)
    {
      std::cerr << "Error: " << e.what() << std::endl;
      return false;
    }

    // Transfer parsed values into the Config struct
    // Setup
    cfg.dataDir = vm["data-dir"].as<std::string>();
    cfg.testnet = vm["testnet"].as<bool>();

    // Mainchain
    cfg.runMainchain = !vm["no-mainchain"].as<bool>();
    cfg.p2pBindIp = vm["p2p-bind-ip"].as<std::string>();
    cfg.p2pBindPort = vm["p2p-bind-port"].as<uint16_t>();
    cfg.rpcBindIp = vm["rpc-bind-ip"].as<std::string>();
    cfg.rpcBindPort = vm["rpc-bind-port"].as<uint16_t>();

    // Sidechain
    cfg.runSidechain = vm["validator"].as<bool>();
    cfg.watchBridge = vm["watch-bridge"].as<bool>();
    cfg.bridgeViewKey = vm["bridge-view-key"].as<std::string>();
    cfg.bridgeSpendKey = vm["bridge-spend-key"].as<std::string>();
    cfg.seedHost = vm["seed-host"].as<std::string>();
    cfg.seedPort = vm["seed-port"].as<uint16_t>();
    cfg.sidechainBindPort = vm["sidechain-port"].as<uint16_t>();
    cfg.rewardAddress = vm["reward-address"].as<std::string>();
    cfg.dexFee = vm["dex-fee"].as<double>();

    // Wallet
    cfg.runWallet = !vm["no-wallet"].as<bool>();
    cfg.useBridgeWallet = vm["use-bridge-wallet"].as<bool>();
    cfg.walletViewKey = vm["wallet-view-key"].as<std::string>();
    cfg.walletSpendKey = vm["wallet-spend-key"].as<std::string>();
    cfg.walletBindPort = vm["wallet-port"].as<uint16_t>();
    cfg.scanThreads = vm["scan-threads"].as<size_t>();

    // Headless / GUI integration
    cfg.noTui = vm["no-tui"].as<bool>();
    cfg.statusFile = vm["status-file"].as<std::string>();
    cfg.readySignal = vm["ready-signal"].as<std::string>();
    cfg.logFile = vm["log-file"].as<std::string>();
    cfg.walletAutoSaveInterval = vm["wallet-auto-save"].as<uint32_t>();

    // Misc
    cfg.rpcThreads = vm["rpc-threads"].as<size_t>();

    return true;
  }

} // namespace Conceal