// CLI.h — command-line argument handling for the unified Conceal client
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <cstdint>

namespace Conceal
{

  struct Config
  {
    // Data
    std::string dataDir = "conceal-data";  // Root data directory for all components

    // Mainchain
    bool runMainchain = true;              // Start the mainchain daemon
    bool useMdbx = true;                   // Use MDBX storage backend (default: yes)

    // Sidechain
    bool runSidechain = false;             // Start the sidechain validator
    bool watchBridge = false;              // Enable bridge watcher (requires validator)
    std::string bridgeViewKey;             // Bridge view key hex (64 chars)
    std::string bridgeSpendKey;            // Bridge spend key hex (64 chars)
    std::string seedHost;                  // Seed validator hostname/IP
    uint16_t seedPort = 0;                 // Seed validator RPC port
    uint16_t sidechainBindPort = 8080;     // Sidechain RPC bind port
    std::string rewardAddress;             // Public key for block reward collection
    double dexFee = 0.0;                   // DEX trading fee percentage (0 = no fee)

    // Wallet
    bool runWallet = true;                 // Start the wallet RPC server
    bool useBridgeWallet = false;          // Use bridge keys for wallet instead of separate keys
    std::string walletViewKey;             // Wallet view key hex (64 chars)
    std::string walletSpendKey;            // Wallet spend key hex (64 chars)
    uint16_t walletBindPort = 8070;        // Wallet RPC bind port

    // Networking
    std::string p2pBindIp = "0.0.0.0";     // P2P bind IP
    uint16_t p2pBindPort = 15000;          // P2P bind port
    std::string rpcBindIp = "127.0.0.1";   // Mainchain RPC bind IP
    uint16_t rpcBindPort = 16000;          // Mainchain RPC bind port

    // Headless / GUI integration
    bool noTui = false;                    // Disable FTXUI, run as headless server for GUI/automation
    std::string statusFile;                // Write JSON status snapshot to this file every 2 seconds
    std::string readySignal;               // Create this sentinel file when all RPC services are accepting connections
    std::string logFile;                   // Redirect all log output to this file instead of stdout
    uint32_t walletAutoSaveInterval = 0;   // Auto-save wallet state every N seconds (0 = disabled, manual only)

    // Misc
    bool testnet = false;                  // Run in testnet mode
    size_t rpcThreads = 1;                 // Number of RPC server threads
    size_t scanThreads = 0;                // Wallet scan threads (0 = auto-detect)
  };

  // Parse command-line arguments into a Config struct.
  // Returns false if help was shown or an error occurred.
  bool parseArgs(int argc, char *argv[], Config &cfg);

} // namespace Conceal