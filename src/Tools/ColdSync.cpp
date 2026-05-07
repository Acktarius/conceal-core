// conceal-wallet-init – CLI wrapper around BoltSync scanner
// Produces a fully synced encrypted wallet container ready for walletd or GUI.

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"

#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteConfig.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include "Wallet/WalletGreen.h"
#include "Wallet/WalletErrors.h"
#include "IWallet.h"
#include "INode.h"

namespace po = boost::program_options;

// ---------------------------------------------------------------------------
// Minimal offline INode (required by WalletGreen constructor)
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
  void isSynchronized(bool &sync, const Callback &cb) override
  {
    sync = true;
    cb(std::error_code());
  }
};

// ---------------------------------------------------------------------------
// CLI Configuration
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
  po::options_description desc("Options");
  desc.add_options()("help,h", "Show this help message")("data-dir", po::value<std::string>()->required(), "Path to conceal data directory")("container", po::value<std::string>()->default_value(""), "Output wallet container path (auto-generated if not specified)")("password", po::value<std::string>()->required(), "Password to encrypt the container")("view-key", po::value<std::string>()->required(), "64-char hex private view key")("spend-key", po::value<std::string>()->default_value(""), "64-char hex private spend key (optional)")("scan-blockchain", po::value<bool>()->default_value(true), "Scan blockchain (true) or create empty container (false)")("progress-file", po::value<std::string>()->default_value(""), "File to write scan progress (for GUI)")("threads", po::value<unsigned int>()->default_value(0), "Number of scan threads (0=auto, max 16)")("start-block", po::value<uint32_t>()->default_value(0), "Start scanning from this block height")("end-block", po::value<uint32_t>()->default_value(0), "Stop scanning at this block height")("force", po::value<bool>()->default_value(false), "Overwrite existing container file if present");

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
    crypto::SecretKey viewKey;
    if (!BoltSync::hexToSecretKey(cfg.viewKeyHex, viewKey))
    {
      std::cerr << "Error: Invalid view key hex" << std::endl;
      return 1;
    }
    crypto::SecretKey spendKey;
    bool hasSpendKey = !cfg.spendKeyHex.empty();
    if (hasSpendKey && !BoltSync::hexToSecretKey(cfg.spendKeyHex, spendKey))
    {
      std::cerr << "Error: Invalid spend key hex" << std::endl;
      return 1;
    }

    // Derive public keys
    crypto::PublicKey viewPub;
    if (!crypto::secret_key_to_public_key(viewKey, viewPub))
    {
      std::cerr << "Error: Failed to derive public view key" << std::endl;
      return 1;
    }
    crypto::PublicKey spendPub;
    if (hasSpendKey)
    {
      if (!crypto::secret_key_to_public_key(spendKey, spendPub))
      {
        std::cerr << "Error: Failed to derive public spend key" << std::endl;
        return 1;
      }
    }
    else
      spendPub = crypto::PublicKey();

    // Setup currency and address
    logging::LoggerManager logManager;
    logging::ConsoleLogger walletLogger;
    cn::Currency currency = cn::CurrencyBuilder(logManager).currency();
    cn::AccountPublicAddress address = {spendPub, viewPub};
    std::string addressStr = currency.accountAddressAsString(address);
    std::cout << "Wallet address: " << addressStr << std::endl;

    // Determine container path
    std::string containerPath;
    if (!cfg.containerFile.empty())
      containerPath = cfg.containerFile;
    else
    {
      boost::filesystem::path walletDir = boost::filesystem::current_path();
      std::string shortAddr = addressStr.substr(0, 12);
      std::string walletType = hasSpendKey ? "full" : "view";
      walletDir /= "conceal_" + walletType + "_" + shortAddr + ".wallet";
      containerPath = walletDir.string();
    }

    // Handle existing container
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

    // Create wallet container
    platform_system::Dispatcher dispatcher;
    OfflineNode offlineNode;
    cn::WalletGreen wallet(dispatcher, currency, offlineNode, walletLogger);

    if (hasSpendKey)
    {
      wallet.initialize(containerPath, cfg.password);
      wallet.createAddress(spendKey);
    }
    else
      wallet.initializeWithViewKey(containerPath, cfg.password, viewKey);
    wallet.save();

    std::cout << std::endl
              << "========================================" << std::endl
              << "Wallet container created successfully!" << std::endl
              << "========================================" << std::endl
              << "Path: " << containerPath << std::endl
              << "Address: " << addressStr << std::endl
              << "Type: " << (hasSpendKey ? "Full wallet" : "View-only wallet") << std::endl
              << "========================================" << std::endl
              << std::endl;

    if (!cfg.scanBlocks)
    {
      wallet.shutdown();
      return 0;
    }

    // Run BoltSync scanner
    BoltSync::ScanConfig scanCfg;
    scanCfg.dataDir = cfg.dataDir;
    scanCfg.numThreads = cfg.numThreads;
    scanCfg.startBlock = cfg.startBlock;
    scanCfg.endBlock = cfg.endBlock;
    scanCfg.progressFile = cfg.progressFile;

    BoltSync::ScanState state;
    {
      BoltSync::Scanner scanner(viewKey, viewPub, hasSpendKey ? &spendKey : nullptr);
      if (!scanner.scan(scanCfg, state))
      {
        std::cerr << "Error: Scan failed" << std::endl;
        wallet.shutdown();
        return 1;
      }
    }

    // state.results holds all found outputs
    auto &allOutputs = state.results;

    std::cout << std::endl
              << "Scan complete. Found " << allOutputs.size() << " matching outputs." << std::endl;

    // Write JSON results
    if (!allOutputs.empty())
    {
      uint64_t totalReceived = 0, totalSpent = 0;
      std::string resultsFile = containerPath + ".scan_results";
      std::ofstream results(resultsFile);
      if (results.is_open())
      {
        results << "{" << std::endl
                << "  \"outputs\": [" << std::endl;
        for (size_t i = 0; i < allOutputs.size(); ++i)
        {
          const auto &fo = allOutputs[i];
          totalReceived += fo.amount;
          if (fo.spent)
            totalSpent += fo.amount;
          results << "    {" << std::endl
                  << "      \"blockHeight\": " << fo.blockHeight << "," << std::endl
                  << "      \"txHash\": \"" << common::podToHex(fo.txHash) << "\"," << std::endl
                  << "      \"outputIndex\": " << fo.outputIndex << "," << std::endl
                  << "      \"amount\": " << fo.amount << "," << std::endl
                  << "      \"outputKey\": \"" << common::podToHex(fo.outputKey) << "\"," << std::endl;
          if (hasSpendKey)
            results << "      \"keyImage\": \"" << common::podToHex(fo.keyImage) << "\"," << std::endl
                    << "      \"spent\": " << (fo.spent ? "true" : "false") << std::endl;
          else
            results << "      \"spent\": false" << std::endl;
          results << "    }";
          if (i < allOutputs.size() - 1)
            results << ",";
          results << std::endl;
        }
        results << "  ]," << std::endl
                << "  \"totalOutputs\": " << allOutputs.size() << "," << std::endl
                << "  \"totalReceived\": " << totalReceived << "," << std::endl
                << "  \"totalSpent\": " << totalSpent << "," << std::endl
                << "  \"balance\": " << (totalReceived - totalSpent) << std::endl
                << "}" << std::endl;
        results.close();
      }
    }

    // Final output
    uint64_t totalReceived = 0, totalSpent = 0;
    for (const auto &fo : allOutputs)
    {
      totalReceived += fo.amount;
      if (fo.spent)
        totalSpent += fo.amount;
    }

    wallet.save(cn::WalletSaveLevel::SAVE_ALL);
    wallet.shutdown();

    std::cout << std::endl
              << "========================================" << std::endl
              << "Wallet pre-synced and saved successfully!" << std::endl
              << "========================================" << std::endl
              << "Path: " << containerPath << std::endl
              << "Address: " << addressStr << std::endl
              << "Outputs found: " << allOutputs.size() << std::endl
              << "Total received: " << totalReceived << std::endl
              << "Total spent: " << totalSpent << std::endl
              << "Balance: " << (totalReceived - totalSpent) << std::endl
              << "Detailed results: " << containerPath << ".scan_results" << std::endl
              << "========================================" << std::endl
              << std::endl
              << "You can now:" << std::endl
              << "  1. Move this file to your desired location" << std::endl
              << "  2. Open it with walletd: walletd --container " << containerPath << " --password <pwd>" << std::endl
              << "  3. Import it into the GUI wallet - it will appear fully synced" << std::endl
              << std::endl;

    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 2;
  }
}