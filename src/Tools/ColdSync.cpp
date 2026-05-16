// conceal-wallet-init – Cold storage wallet initializer using BoltCore
// Creates an encrypted, pre-synced wallet container ready for conceal-rpc

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "BoltCore/BoltCore.h"
#include "BoltRPC/StateManager.h"

#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
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
  unsigned int numThreads = 0;
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
// Dummy INode for offline operation (implements required methods)
// ---------------------------------------------------------------------------
class OfflineNode : public cn::INode
{
public:
  virtual ~OfflineNode() {}
  bool addObserver(cn::INodeObserver *o) override { return true; }
  bool removeObserver(cn::INodeObserver *o) override { return true; }
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

  // Required by INode (pure virtual)
  virtual std::vector<crypto::Hash> getPoolTransactions() override { return std::vector<crypto::Hash>(); }
  virtual bool getTransactionSync(const crypto::Hash &txHash, cn::Transaction &tx) override { return false; }

  void isSynchronized(bool &sync, const Callback &cb) override
  {
    sync = true;
    cb(std::error_code());
  }
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
    if (!BoltSync::hexToSecretKey(cfg.viewKeyHex, viewKey))
    {
      std::cerr << "Error: Invalid view key hex" << std::endl;
      return 1;
    }

    bool hasSpendKey = !cfg.spendKeyHex.empty();
    if (hasSpendKey && !BoltSync::hexToSecretKey(cfg.spendKeyHex, spendKey))
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
    logging::LoggerManager logManager;
    logging::ConsoleLogger consoleLogger;
    cn::Currency currency = cn::CurrencyBuilder(consoleLogger).currency();
    cn::AccountPublicAddress address = {spendPub, viewPub};
    std::string addressStr = currency.accountAddressAsString(address);
    std::cout << "Wallet address: " << addressStr << std::endl;

    // Determine container path (bolt-wallet.state format)
    std::string containerPath;
    if (!cfg.containerFile.empty())
      containerPath = cfg.containerFile;
    else
    {
      boost::filesystem::path walletDir = boost::filesystem::current_path();
      std::string shortAddr = addressStr.substr(0, 12);
      std::string walletType = hasSpendKey ? "full" : "view";
      walletDir /= "bolt_" + walletType + "_" + shortAddr + ".state";
      containerPath = walletDir.string();
    }

    // Check for existing container
    bool containerExists = false;
    {
      std::ifstream testFile(containerPath);
      containerExists = testFile.good();
    }

    if (containerExists && !cfg.forceOverwrite)
    {
      std::cerr << "Error: Container already exists: " << containerPath << std::endl;
      std::cerr << "Use --force to overwrite, or --container to specify a different path." << std::endl;
      return 1;
    }

    // Scan for outputs using BoltSync
    std::vector<BoltCore::OutputInfo> outputs;
    uint32_t scannedHeight = 0;

    if (cfg.scanBlocks && !cfg.dataDir.empty())
    {
      std::cout << "Starting blockchain scan..." << std::endl;

      BoltSync::Scanner scanner(viewKey, spendPub, hasSpendKey ? &spendKey : nullptr);
      BoltSync::ScanConfig scanCfg;
      scanCfg.dataDir = cfg.dataDir;
      scanCfg.numThreads = cfg.numThreads;
      scanCfg.startBlock = cfg.startBlock;
      scanCfg.endBlock = cfg.endBlock;
      scanCfg.progressFile = cfg.progressFile;

      BoltSync::ScanState state;
      if (!scanner.scan(scanCfg, state))
      {
        std::cerr << "Error: Blockchain scan failed" << std::endl;
        return 1;
      }

      scannedHeight = state.scannedTopHeight;

      // Convert FoundOutput to OutputInfo
      outputs.reserve(state.results.size());
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
        info.isDeposit = fo.isDeposit;
        info.term = fo.term;
        info.subAddress = addressStr;
        outputs.push_back(info);
      }

      std::cout << "Scan complete. Found " << outputs.size() << " outputs at height " << scannedHeight << std::endl;
    }

    // Create BoltRPC state file (encrypted container)
    BoltRPC::StateManager stateManager(containerPath);

    // Prepare output list for saving
    std::vector<BoltCore::OutputInfo> emptyOutputs;

    if (stateManager.save(outputs, scannedHeight, cfg.viewKeyHex, cfg.spendKeyHex, cfg.password))
    {
      std::cout << std::endl
                << "========================================" << std::endl
                << "Wallet container created successfully!" << std::endl
                << "========================================" << std::endl
                << "Path: " << containerPath << std::endl
                << "Address: " << addressStr << std::endl
                << "Type: " << (hasSpendKey ? "Full wallet" : "View-only wallet") << std::endl
                << "Outputs: " << outputs.size() << std::endl
                << "Synced height: " << scannedHeight << std::endl
                << "========================================" << std::endl;
    }
    else
    {
      std::cerr << "Error: Failed to save wallet container" << std::endl;
      return 1;
    }

    // Write JSON results for debugging
    if (!outputs.empty())
    {
      uint64_t totalReceived = 0, totalSpent = 0;
      for (const auto &out : outputs)
      {
        totalReceived += out.amount;
        if (out.spent)
          totalSpent += out.amount;
      }

      std::string resultsFile = containerPath + ".scan_results";
      std::ofstream results(resultsFile);
      if (results.is_open())
      {
        results << "{" << std::endl
                << "  \"address\": \"" << addressStr << "\"," << std::endl
                << "  \"syncedHeight\": " << scannedHeight << "," << std::endl
                << "  \"totalOutputs\": " << outputs.size() << "," << std::endl
                << "  \"totalReceived\": " << totalReceived << "," << std::endl
                << "  \"totalSpent\": " << totalSpent << "," << std::endl
                << "  \"balance\": " << (totalReceived - totalSpent) << std::endl
                << "}" << std::endl;
        results.close();
        std::cout << "Detailed results: " << resultsFile << std::endl;
      }
    }

    std::cout << std::endl
              << "You can now:" << std::endl
              << "  1. Move this file to your desired location" << std::endl
              << "  2. Use with conceal-rpc: conceal-rpc --state-file " << containerPath << " --password <pwd>" << std::endl
              << std::endl;

    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 2;
  }
}