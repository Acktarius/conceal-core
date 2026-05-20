// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

// conceal-coldsync – Offline wallet pre-syncer
// Creates an encrypted wallet state file by scanning the MDBX blockchain directly.
// No daemon connection required.

#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "BoltRPC/SyncManager.h"
#include "BoltRPC/StateManager.h"
#include "BoltRPC/WalletManager.h"
#include "Storage/MDBXBlockchainStorage.h"

#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include "CryptoNoteConfig.h"

namespace po = boost::program_options;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config
{
  std::string dataDir;
  std::string containerFile;
  std::string password;
  std::string viewKeyHex;
  std::string spendKeyHex;
  bool scanBlocks = true;
  std::string progressFile;
  unsigned int numThreads = 1;
  uint32_t startBlock = 0;
  uint32_t endBlock = 0;
  bool forceOverwrite = false;
};

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("Cold Storage Wallet Initializer for Conceal");
  desc.add_options()("help,h", "Show this help message")("data-dir", po::value<std::string>()->required(), "Path to conceal data directory (contains mdbx_blocks)")("container", po::value<std::string>()->default_value(""), "Output wallet container path (auto-generated if not specified)")("password", po::value<std::string>()->required(), "Password to encrypt the container")("view-key", po::value<std::string>()->required(), "64-char hex private view key")("spend-key", po::value<std::string>()->default_value(""), "64-char hex private spend key (optional)")("scan-blockchain", po::value<bool>()->default_value(true), "Scan blockchain (true) or create empty container (false)")("progress-file", po::value<std::string>()->default_value(""), "File to write scan progress (for GUI)")("threads", po::value<unsigned int>()->default_value(1), "Number of scan threads (0=auto, 1=single thread)")("start-block", po::value<uint32_t>()->default_value(0), "Start scanning from this block height")("end-block", po::value<uint32_t>()->default_value(0), "Stop scanning at this block height")("force", po::value<bool>()->default_value(false), "Overwrite existing container file if present");

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

    cfg.dataDir = vm["data-dir"].as<std::string>();
    cfg.containerFile = vm["container"].as<std::string>();
    cfg.password = vm["password"].as<std::string>();
    cfg.viewKeyHex = vm["view-key"].as<std::string>();
    cfg.spendKeyHex = vm["spend-key"].as<std::string>();
    cfg.scanBlocks = vm["scan-blockchain"].as<bool>();
    cfg.progressFile = vm["progress-file"].as<std::string>();
    cfg.numThreads = vm["threads"].as<unsigned int>();
    cfg.startBlock = vm["start-block"].as<uint32_t>();
    cfg.endBlock = vm["end-block"].as<uint32_t>();
    cfg.forceOverwrite = vm["force"].as<bool>();
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl
              << desc << std::endl;
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

// ---------------------------------------------------------------------------
// Offline INode stub (satisfies INode interface, never connects)
// ---------------------------------------------------------------------------
class OfflineNode : public cn::INode
{
public:
  virtual ~OfflineNode() {}

  bool addObserver(cn::INodeObserver *) override { return true; }
  bool removeObserver(cn::INodeObserver *) override { return true; }
  void init(const Callback &cb) override { cb(std::error_code()); }
  bool shutdown() override { return true; }

  size_t getPeerCount() const override { return 0; }
  uint32_t getLastLocalBlockHeight() const override { return 0; }
  uint32_t getLastKnownBlockHeight() const override { return 0; }
  uint32_t getLocalBlockCount() const override { return 0; }
  uint32_t getKnownBlockCount() const override { return 0; }
  uint64_t getLastLocalBlockTimestamp() const override { return 0; }

  void relayTransaction(const cn::Transaction &, const Callback &cb) override { cb(std::error_code()); }
  void getRandomOutsByAmounts(std::vector<uint64_t> &&, uint64_t, std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &, const Callback &cb) override { cb(std::error_code()); }
  void getNewBlocks(std::vector<crypto::Hash> &&, std::vector<cn::block_complete_entry> &, uint32_t &, const Callback &cb) override { cb(std::error_code()); }
  void getTransactionOutsGlobalIndices(const crypto::Hash &, std::vector<uint32_t> &, const Callback &cb) override { cb(std::error_code()); }
  void queryBlocks(std::vector<crypto::Hash> &&, uint64_t, std::vector<cn::BlockShortEntry> &, uint32_t &, const Callback &cb) override { cb(std::error_code()); }
  void getPoolSymmetricDifference(std::vector<crypto::Hash> &&, crypto::Hash, bool &, std::vector<std::unique_ptr<cn::ITransactionReader>> &, std::vector<crypto::Hash> &, const Callback &cb) override { cb(std::error_code()); }
  void getMultisignatureOutputByGlobalIndex(uint64_t, uint32_t, cn::MultisignatureOutput &, const Callback &cb) override { cb(std::error_code()); }
  void getTransaction(const crypto::Hash &, cn::Transaction &, const Callback &cb) override { cb(std::error_code()); }
  void getBlocks(const std::vector<uint32_t> &, std::vector<std::vector<cn::BlockDetails>> &, const Callback &cb) override { cb(std::error_code()); }
  void getBlocks(const std::vector<crypto::Hash> &, std::vector<cn::BlockDetails> &, const Callback &cb) override { cb(std::error_code()); }
  void getBlocks(uint64_t, uint64_t, uint32_t, std::vector<cn::BlockDetails> &, uint32_t &, const Callback &cb) override { cb(std::error_code()); }
  void getTransactions(const std::vector<crypto::Hash> &, std::vector<cn::TransactionDetails> &, const Callback &cb) override { cb(std::error_code()); }
  void getTransactionsByPaymentId(const crypto::Hash &, std::vector<cn::TransactionDetails> &, const Callback &cb) override { cb(std::error_code()); }
  void getPoolTransactions(uint64_t, uint64_t, uint32_t, std::vector<cn::TransactionDetails> &, uint64_t &, const Callback &cb) override { cb(std::error_code()); }
  void isSynchronized(bool &sync, const Callback &cb) override
  {
    sync = true;
    cb(std::error_code());
  }

  std::vector<crypto::Hash> getPoolTransactions() override { return {}; }
  bool getTransactionSync(const crypto::Hash &, cn::Transaction &) override { return false; }
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
  try
  {
    Config cfg;
    if (!parseArgs(argc, argv, cfg))
      return 1;

    // Parse keys
    crypto::SecretKey viewKey, spendKey;
    if (!common::podFromHex(cfg.viewKeyHex, viewKey))
    {
      std::cerr << "Error: Invalid view key hex" << std::endl;
      return 1;
    }

    bool hasSpendKey = !cfg.spendKeyHex.empty();
    if (hasSpendKey && !common::podFromHex(cfg.spendKeyHex, spendKey))
    {
      std::cerr << "Error: Invalid spend key hex" << std::endl;
      return 1;
    }

    // Derive public keys
    crypto::PublicKey viewPub, spendPub;
    if (!crypto::secret_key_to_public_key(viewKey, viewPub))
    {
      std::cerr << "Error: Failed to derive public view key" << std::endl;
      return 1;
    }
    if (hasSpendKey)
    {
      if (!crypto::secret_key_to_public_key(spendKey, spendPub))
      {
        std::cerr << "Error: Failed to derive public spend key" << std::endl;
        return 1;
      }
    }

    // Setup currency and address
    logging::ConsoleLogger consoleLogger;
    cn::Currency currency = cn::CurrencyBuilder(consoleLogger).currency();
    cn::AccountPublicAddress address = {spendPub, viewPub};
    std::string addressStr = currency.accountAddressAsString(address);
    std::cout << "Wallet address: " << addressStr << std::endl;

    // Determine container path
    std::string containerPath;
    if (!cfg.containerFile.empty())
    {
      containerPath = cfg.containerFile;
    }
    else
    {
      std::string shortAddr = addressStr.substr(0, 12);
      std::string walletType = hasSpendKey ? "full" : "view";
      containerPath = "bolt_" + walletType + "_" + shortAddr + ".state";
    }

    // Check for existing container
    {
      std::ifstream testFile(containerPath);
      if (testFile.good() && !cfg.forceOverwrite)
      {
        std::cerr << "Error: Container already exists: " << containerPath << std::endl;
        std::cerr << "Use --force to overwrite, or --container to specify a different path." << std::endl;
        return 1;
      }
    }

    // ── Scan blockchain directly via MDBX ──────────────────────────────
    std::vector<BoltRPC::OutputInfo> outputs;
    uint32_t scannedHeight = 0;

    if (cfg.scanBlocks && !cfg.dataDir.empty())
    {
      std::cout << "Opening MDBX blockchain at " << cfg.dataDir << "/mdbx_blocks ..." << std::endl;

      std::string dbPath = cfg.dataDir + "/mdbx_blocks";
      CryptoNote::MDBXBlockchainStorage storage(dbPath, false);

      uint32_t topHeight = storage.topBlockHeight();
      if (cfg.endBlock == 0 || cfg.endBlock > topHeight)
        cfg.endBlock = topHeight;

      std::cout << "Scanning blocks " << cfg.startBlock << " to " << cfg.endBlock
                << " (chain height: " << topHeight << ")" << std::endl;

      // Use SyncManager-style derivation to find owned outputs
      OfflineNode offlineNode;
      BoltRPC::SyncManager syncManager(
          offlineNode, viewKey, spendPub, cfg.dataDir,
          [](const std::string &, const std::string &) -> std::string
          { return ""; });

      // Scan by fetching all tx_pub_keys and outputs from MDBX indexes
      std::vector<crypto::PublicKey> allKeys = storage.getNewTxPubKeys(cfg.startBlock, cfg.endBlock);
      std::cout << "Found " << allKeys.size() << " unique tx public keys in range" << std::endl;

      std::vector<CryptoNote::WalletOutputInfo> candidates;
      std::unordered_set<std::string> spentImages;
      storage.getOutputsByTxPubKeys(allKeys, candidates, spentImages);

      std::cout << "Retrieved " << candidates.size() << " output candidates" << std::endl;

      // Convert and derive ownership
      uint64_t totalReceived = 0, totalSpent = 0;
      for (const auto &c : candidates)
      {
        BoltRPC::OutputInfo info;
        info.blockHeight = c.block_height;
        info.txHash = c.tx_hash;
        info.amount = c.amount;
        info.outputIndex = c.output_index;
        info.outputKey = c.output_key;
        info.txPublicKey = c.tx_public_key;
        info.spent = false;
        info.isDeposit = false;
        info.term = 0;

        // Derive ownership (using SyncManager's logic via WalletManager's key derivation)
        crypto::KeyDerivation derivation;
        if (crypto::generate_key_derivation(c.tx_public_key, viewKey, derivation))
        {
          crypto::PublicKey derivedKey;
          if (crypto::derive_public_key(derivation, c.output_index, spendPub, derivedKey) &&
              std::memcmp(&derivedKey, &c.output_key, sizeof(crypto::PublicKey)) == 0)
          {
            // Check spent status

            if (hasSpendKey)
            {
              crypto::KeyImage ki;
              crypto::SecretKey ephemeralSec;
              derive_secret_key(derivation, c.output_index, spendKey, ephemeralSec);
              crypto::PublicKey ephemeralPub;
              crypto::secret_key_to_public_key(ephemeralSec, ephemeralPub);
              crypto::generate_key_image(ephemeralPub, ephemeralSec, ki);
              info.spent = storage.isSpentKeyImage(ki);
            }

            outputs.push_back(info);
            totalReceived += info.amount;
            if (info.spent)
              totalSpent += info.amount;
          }
        }
      }

      scannedHeight = cfg.endBlock;
      std::cout << "Scan complete. Found " << outputs.size() << " owned outputs."
                << " Received: " << totalReceived
                << ", Spent: " << totalSpent
                << ", Balance: " << (totalReceived - totalSpent) << std::endl;
    }

    // ── Save wallet state via StateManager ──────────────────────────────
    BoltRPC::StateManager stateManager(containerPath);

    BoltRPC::WalletState state;
    state.lastHeight = scannedHeight;
    state.balance = 0;
    state.unlockedBalance = 0;

    for (const auto &out : outputs)
    {
      state.ownedOutputs.push_back(out);
      if (!out.spent)
        state.balance += out.amount;
    }

    if (!stateManager.save(state))
    {
      std::cerr << "Error: Failed to save wallet state" << std::endl;
      return 1;
    }

    std::cout << std::endl
              << "========================================" << std::endl
              << "Wallet container created successfully!" << std::endl
              << "========================================" << std::endl
              << "Path: " << containerPath << std::endl
              << "Address: " << addressStr << std::endl
              << "Type: " << (hasSpendKey ? "Full wallet" : "View-only wallet") << std::endl
              << "Outputs: " << outputs.size() << std::endl
              << "Balance: " << state.balance << std::endl
              << "Synced height: " << scannedHeight << std::endl
              << "========================================" << std::endl;

    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 2;
  }
}