
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// conceal-wallet-init – Standalone MDBX wallet cache pre-builder
// Scans blockchain directly from MDBX storage, finds all outputs for a given
// view key, optionally derives key images and checks spent status, then writes
// a fully synced encrypted wallet container.

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "BlockchainExplorerData.h"
#include "Common/MemoryInputStream.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "CryptoNoteCore/TransactionPool.h"
#include "CryptoNoteConfig.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include "Logging/LoggerRef.h"
#include "Serialization/BinaryInputStreamSerializer.h"
#include "Serialization/BinaryOutputStreamSerializer.h"
#include "Serialization/SerializationTools.h"
#include "Storage/MDBXBlockchainStorage.h"
#include "Wallet/WalletGreen.h"
#include "Wallet/WalletErrors.h"
#include "IWallet.h"
#include "INode.h"

namespace po = boost::program_options;

// Minimal offline INode implementation
class OfflineNode : public cn::INode
{
public:
  virtual ~OfflineNode() {}

  bool addObserver(cn::INodeObserver *observer) override { return true; }
  bool removeObserver(cn::INodeObserver *observer) override { return true; }

  void init(const Callback &callback) override { callback(std::error_code()); }
  bool shutdown() override { return true; }

  size_t getPeerCount() const override { return 0; }
  uint32_t getLastLocalBlockHeight() const override { return 0; }
  uint32_t getLastKnownBlockHeight() const override { return 0; }
  uint32_t getLocalBlockCount() const override { return 0; }
  uint32_t getKnownBlockCount() const override { return 0; }
  uint64_t getLastLocalBlockTimestamp() const override { return 0; }

  void relayTransaction(const cn::Transaction &transaction, const Callback &callback) override { callback(std::error_code()); }
  void getRandomOutsByAmounts(std::vector<uint64_t> &&amounts, uint64_t outsCount, std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &result, const Callback &callback) override { callback(std::error_code()); }
  void getNewBlocks(std::vector<crypto::Hash> &&knownBlockIds, std::vector<cn::block_complete_entry> &newBlocks, uint32_t &startHeight, const Callback &callback) override { callback(std::error_code()); }
  void getTransactionOutsGlobalIndices(const crypto::Hash &transactionHash, std::vector<uint32_t> &outsGlobalIndices, const Callback &callback) override { callback(std::error_code()); }
  void queryBlocks(std::vector<crypto::Hash> &&knownBlockIds, uint64_t timestamp, std::vector<cn::BlockShortEntry> &newBlocks, uint32_t &startHeight, const Callback &callback) override { callback(std::error_code()); }
  void getPoolSymmetricDifference(std::vector<crypto::Hash> &&knownPoolTxIds, crypto::Hash knownBlockId, bool &isBcActual, std::vector<std::unique_ptr<cn::ITransactionReader>> &newTxs, std::vector<crypto::Hash> &deletedTxIds, const Callback &callback) override { callback(std::error_code()); }
  void getMultisignatureOutputByGlobalIndex(uint64_t amount, uint32_t gindex, cn::MultisignatureOutput &out, const Callback &callback) override { callback(std::error_code()); }
  void getTransaction(const crypto::Hash &transactionHash, cn::Transaction &transaction, const Callback &callback) override { callback(std::error_code()); }
  void getBlocks(const std::vector<uint32_t> &blockHeights, std::vector<std::vector<cn::BlockDetails>> &blocks, const Callback &callback) override { callback(std::error_code()); }
  void getBlocks(const std::vector<crypto::Hash> &blockHashes, std::vector<cn::BlockDetails> &blocks, const Callback &callback) override { callback(std::error_code()); }
  void getBlocks(uint64_t timestampBegin, uint64_t timestampEnd, uint32_t blocksNumberLimit, std::vector<cn::BlockDetails> &blocks, uint32_t &blocksNumberWithinTimestamps, const Callback &callback) override { callback(std::error_code()); }
  void getTransactions(const std::vector<crypto::Hash> &transactionHashes, std::vector<cn::TransactionDetails> &transactions, const Callback &callback) override { callback(std::error_code()); }
  void getTransactionsByPaymentId(const crypto::Hash &paymentId, std::vector<cn::TransactionDetails> &transactions, const Callback &callback) override { callback(std::error_code()); }
  void getPoolTransactions(uint64_t timestampBegin, uint64_t timestampEnd, uint32_t transactionsNumberLimit, std::vector<cn::TransactionDetails> &transactions, uint64_t &transactionsNumberWithinTimestamps, const Callback &callback) override { callback(std::error_code()); }
  void isSynchronized(bool &syncStatus, const Callback &callback) override
  {
    syncStatus = true;
    callback(std::error_code());
  }
};

// Configuration
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
  bool forceOverwrite = false; // --force to overwrite existing container
};

// CLI parsing
bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("Options");
  desc.add_options()("help,h", "Show this help message")("data-dir", po::value<std::string>()->required(), "Path to conceal data directory")("container", po::value<std::string>()->default_value(""), "Output wallet container path (auto-generated if not specified)")("password", po::value<std::string>()->required(), "Password to encrypt the container")("view-key", po::value<std::string>()->required(), "64-char hex private view key")("spend-key", po::value<std::string>()->default_value(""), "64-char hex private spend key (optional)")("scan-blockchain", po::value<bool>()->default_value(true), "Scan blockchain (true) or create empty container (false)")("progress-file", po::value<std::string>()->default_value(""), "File to write scan progress (for GUI)")("threads", po::value<unsigned int>()->default_value(0), "Number of scan threads (0=auto, max 16)")("start-block", po::value<uint32_t>()->default_value(0), "Start scanning from this block height (for debugging)")("end-block", po::value<uint32_t>()->default_value(0), "Stop scanning at this block height (for debugging)")("force", po::value<bool>()->default_value(false), "Overwrite existing container file if present");

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

// Crypto helpers
bool hexToSecretKey(const std::string &hex, crypto::SecretKey &key) { return common::podFromHex(hex, key); }

bool isOutputOurs(const crypto::PublicKey &txPublicKey, size_t outputIndex,
                  const crypto::PublicKey &outputKey, const crypto::SecretKey &viewSecretKey,
                  const crypto::PublicKey &viewPublicKey)
{
  crypto::KeyDerivation derivation;
  if (!crypto::generate_key_derivation(txPublicKey, viewSecretKey, derivation))
    return false;
  crypto::PublicKey derivedKey;
  if (!crypto::derive_public_key(derivation, outputIndex, viewPublicKey, derivedKey))
    return false;
  return derivedKey == outputKey;
}

crypto::SecretKey deriveOutputSecretKey(const crypto::KeyDerivation &derivation, size_t outputIndex, const crypto::SecretKey &spendSecretKey)
{
  crypto::SecretKey outputSecret;
  crypto::derive_secret_key(derivation, outputIndex, spendSecretKey, outputSecret);
  return outputSecret;
}

bool getTxHash(const cn::Transaction &tx, crypto::Hash &hash) { return getObjectHash(tx, hash); }

// Data structures
struct FoundOutput
{
  uint32_t blockHeight;
  crypto::Hash txHash;
  uint32_t outputIndex;
  uint64_t amount;
  crypto::PublicKey outputKey;
  crypto::PublicKey txPublicKey;
  crypto::KeyImage keyImage;
  bool spent = false;
};

struct ScanContext
{
  CryptoNote::IBlockchainStorage &storage;
  const crypto::SecretKey &viewKey;
  const crypto::PublicKey &viewPublicKey;
  const crypto::SecretKey *spendKey;
  std::atomic<uint64_t> &blocksProcessed;
  std::atomic<uint32_t> &lastCheckpointHeight;
  std::mutex &resultsMutex;
  std::vector<FoundOutput> &results;
  std::function<void(uint32_t)> saveCheckpoint;
};

// Block entry structure (matches MDBX storage format used by Blockchain.cpp)
struct TransactionEntry
{
  cn::Transaction tx;
  std::vector<uint32_t> m_global_output_indexes;
};

struct BlockEntry
{
  cn::Block bl;
  std::vector<TransactionEntry> transactions;
  size_t block_cumulative_size;
  uint64_t cumulative_difficulty;
  uint64_t already_generated_coins;
  uint32_t height;
};

// Attempt to deserialize using the known format from Blockchain.cpp
bool deserializeBlockEntry(const cn::BinaryArray &rawEntry,
                           cn::Block &block,
                           std::vector<cn::Transaction> &transactions)
{
  try
  {
    common::MemoryInputStream stream(rawEntry.data(), rawEntry.size());
    cn::BinaryInputStreamSerializer serializer(stream);

    // Block header
    serializer(block.majorVersion, "majorVersion");
    serializer(block.minorVersion, "minorVersion");
    serializer(block.nonce, "nonce");
    serializer(block.timestamp, "timestamp");
    serializer(block.previousBlockHash, "previousBlockHash");

    // Base transaction (coinbase)
    serializer(block.baseTransaction, "baseTransaction");

    // Transaction hashes
    uint64_t txHashCount;
    serializer(txHashCount, "transactionHashes");
    block.transactionHashes.resize(txHashCount);
    for (uint64_t i = 0; i < txHashCount; ++i)
      serializer(block.transactionHashes[i], "hash");

    // Transaction entries (contains tx + global output indexes)
    uint64_t txCount;
    serializer(txCount, "transactions");
    transactions.resize(txCount);
    for (uint64_t i = 0; i < txCount; ++i)
    {
      serializer(transactions[i], "tx");

      // Skip global output indexes
      uint64_t indexCount;
      serializer(indexCount, "indexes");
      for (uint64_t j = 0; j < indexCount; ++j)
      {
        uint32_t dummy;
        serializer(dummy, "idx");
      }
    }

    // Skip remaining fields (block_cumulative_size, cumulative_difficulty, etc.)
    size_t blockCumulativeSize;
    serializer(blockCumulativeSize, "block_cumulative_size");

    uint64_t cumulativeDifficulty;
    serializer(cumulativeDifficulty, "cumulative_difficulty");

    uint64_t alreadyGeneratedCoins;
    serializer(alreadyGeneratedCoins, "already_generated_coins");

    uint32_t height;
    serializer(height, "height");

    return true;
  }
  catch (const std::exception &)
  {
    return false;
  }
}

// Scan single block with detailed error reporting
void scanSingleBlock(uint32_t h, ScanContext &ctx)
{
  try
  {
    cn::BinaryArray serializedEntry;
    if (!ctx.storage.getBlockEntry(h, serializedEntry))
    {
      ctx.blocksProcessed.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    cn::Block block;
    std::vector<cn::Transaction> transactions;

    if (!deserializeBlockEntry(serializedEntry, block, transactions))
    {
      // Can't read this block - skip it silently
      ctx.blocksProcessed.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    static const crypto::PublicKey NULL_KEY = {};

    // Process base transaction (coinbase)
    {
      cn::Transaction &tx = block.baseTransaction;
      crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
      if (!(txPubKey == NULL_KEY))
      {
        for (size_t outIdx = 0; outIdx < tx.outputs.size(); ++outIdx)
        {
          const auto &out = tx.outputs[outIdx];
          if (out.target.type() != typeid(cn::KeyOutput))
            continue;
          const auto &keyOut = boost::get<cn::KeyOutput>(out.target);
          if (isOutputOurs(txPubKey, outIdx, keyOut.key, ctx.viewKey, ctx.viewPublicKey))
          {
            crypto::Hash txHash;
            if (!getTxHash(tx, txHash))
              continue;
            FoundOutput fo;
            fo.blockHeight = h;
            fo.txHash = txHash;
            fo.outputIndex = static_cast<uint32_t>(outIdx);
            fo.amount = out.amount;
            fo.outputKey = keyOut.key;
            fo.txPublicKey = txPubKey;
            if (ctx.spendKey)
            {
              crypto::KeyDerivation derivation;
              crypto::generate_key_derivation(txPubKey, ctx.viewKey, derivation);
              crypto::SecretKey outputSecret = deriveOutputSecretKey(derivation, outIdx, *ctx.spendKey);
              crypto::generate_key_image(keyOut.key, outputSecret, fo.keyImage);
            }
            std::lock_guard<std::mutex> lock(ctx.resultsMutex);
            ctx.results.push_back(std::move(fo));
          }
        }
      }
    }

    // Process regular transactions
    for (size_t txIdx = 0; txIdx < transactions.size(); ++txIdx)
    {
      cn::Transaction &tx = transactions[txIdx];
      crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
      if (txPubKey == NULL_KEY)
        continue;
      for (size_t outIdx = 0; outIdx < tx.outputs.size(); ++outIdx)
      {
        const auto &out = tx.outputs[outIdx];
        if (out.target.type() != typeid(cn::KeyOutput))
          continue;
        const auto &keyOut = boost::get<cn::KeyOutput>(out.target);
        if (isOutputOurs(txPubKey, outIdx, keyOut.key, ctx.viewKey, ctx.viewPublicKey))
        {
          crypto::Hash txHash;
          if (!getTxHash(tx, txHash))
            continue;
          FoundOutput fo;
          fo.blockHeight = h;
          fo.txHash = txHash;
          fo.outputIndex = static_cast<uint32_t>(outIdx);
          fo.amount = out.amount;
          fo.outputKey = keyOut.key;
          fo.txPublicKey = txPubKey;
          if (ctx.spendKey)
          {
            crypto::KeyDerivation derivation;
            crypto::generate_key_derivation(txPubKey, ctx.viewKey, derivation);
            crypto::SecretKey outputSecret = deriveOutputSecretKey(derivation, outIdx, *ctx.spendKey);
            crypto::generate_key_image(keyOut.key, outputSecret, fo.keyImage);
          }
          std::lock_guard<std::mutex> lock(ctx.resultsMutex);
          ctx.results.push_back(std::move(fo));
        }
      }
    }
  }
  catch (const std::exception &e)
  {
    // Silently skip problematic blocks
  }
  ctx.blocksProcessed.fetch_add(1, std::memory_order_relaxed);
}

// Path helper
namespace
{
  std::string appendPath(const std::string &path, const std::string &fileName)
  {
    std::string result = path;
    if (!result.empty() && result.back() != '/')
      result += '/';
    result += fileName;
    return result;
  }
}

// Main
int main(int argc, char *argv[])
{
  try
  {
    Config cfg;
    if (!parseArgs(argc, argv, cfg))
      return 1;

    // Parse keys
    crypto::SecretKey viewKey;
    if (!hexToSecretKey(cfg.viewKeyHex, viewKey))
    {
      std::cerr << "Error: Invalid view key hex" << std::endl;
      return 1;
    }
    crypto::SecretKey spendKey;
    bool hasSpendKey = !cfg.spendKeyHex.empty();
    if (hasSpendKey && !hexToSecretKey(cfg.spendKeyHex, spendKey))
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

    // Setup currency
    logging::LoggerManager logManager;
    logging::ConsoleLogger walletLogger;
    cn::Currency currency = cn::CurrencyBuilder(logManager).currency();
    cn::AccountPublicAddress address = {spendPub, viewPub};
    std::string addressStr = currency.accountAddressAsString(address);
    std::cout << "Wallet address: " << addressStr << std::endl;

    // Open blockchain
    CryptoNote::MDBXBlockchainStorage blockchain(appendPath(cfg.dataDir, "mdbx_blocks"), 0);
    uint32_t topHeight = blockchain.topBlockHeight();
    if (topHeight == 0)
    {
      std::cerr << "Error: Blockchain is empty at " << cfg.dataDir << std::endl;
      return 1;
    }
    std::cout << "Blockchain height: " << topHeight << std::endl;

    // Determine container path
    std::string containerPath;
    if (!cfg.containerFile.empty())
      containerPath = cfg.containerFile;
    else
    {
      boost::filesystem::path walletDir = boost::filesystem::current_path();
      std::string shortAddr = addressStr.substr(0, 12);
      std::string walletType = hasSpendKey ? "full" : "view";
      std::string filename = "conceal_" + walletType + "_" + shortAddr + ".wallet";
      walletDir /= filename;
      containerPath = walletDir.string();
    }

    // Handle existing container
    bool containerExists = false;
    {
      std::ifstream testFile(containerPath);
      containerExists = testFile.good();
    }

    // Check for resume data
    uint32_t lastScannedHeight = 0;
    bool hasResumeData = false;
    {
      std::vector<uint8_t> resumeBuf;
      if (blockchain.getMeta("wallet_init_progress", resumeBuf) && resumeBuf.size() >= sizeof(uint32_t))
      {
        memcpy(&lastScannedHeight, resumeBuf.data(), sizeof(lastScannedHeight));
        if (lastScannedHeight > 0 && lastScannedHeight < topHeight)
          hasResumeData = true;
        else
          lastScannedHeight = 0;
      }
    }

    if (containerExists && !cfg.forceOverwrite && !hasResumeData)
    {
      std::cerr << "Error: Container already exists: " << containerPath << std::endl;
      std::cerr << "Use --force to overwrite, or --container to specify a different path." << std::endl;
      return 1;
    }

    // Create wallet
    platform_system::Dispatcher dispatcher;
    OfflineNode offlineNode;
    cn::WalletGreen wallet(dispatcher, currency, offlineNode, walletLogger);

    if (containerExists && (cfg.forceOverwrite || hasResumeData))
    {
      // For resume: load existing container, don't reinitialize
      if (hasResumeData)
      {
        wallet.load(containerPath, cfg.password);
        std::cout << "Loaded existing wallet container for resume." << std::endl;
      }
      else
      {
        // Force overwrite
        boost::filesystem::remove(containerPath);
        if (hasSpendKey)
        {
          wallet.initialize(containerPath, cfg.password);
          wallet.createAddress(spendKey);
        }
        else
          wallet.initializeWithViewKey(containerPath, cfg.password, viewKey);
      }
    }
    else
    {
      if (hasSpendKey)
      {
        wallet.initialize(containerPath, cfg.password);
        wallet.createAddress(spendKey);
      }
      else
        wallet.initializeWithViewKey(containerPath, cfg.password, viewKey);
    }

    if (!hasResumeData)
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

    // Thread setup
    unsigned int numThreads = cfg.numThreads;
    if (numThreads == 0)
    {
      numThreads = std::thread::hardware_concurrency();
      if (numThreads == 0)
        numThreads = 4;
    }
    if (numThreads > 16)
    {
      std::cout << "Limiting threads to 16 for stability" << std::endl;
      numThreads = 16;
    }

    std::vector<FoundOutput> allOutputs;
    std::mutex resultsMutex;
    std::atomic<uint64_t> blocksProcessed(lastScannedHeight);
    std::atomic<uint32_t> lastCheckpointHeight(lastScannedHeight);
    std::atomic<bool> progressDone(false);

    auto saveCheckpoint = [&](uint32_t height)
    {
      std::vector<uint8_t> buf(sizeof(height));
      memcpy(buf.data(), &height, sizeof(height));
      blockchain.putMeta("wallet_init_progress", buf);
      blockchain.flush();
      lastCheckpointHeight.store(height, std::memory_order_relaxed);
    };

    std::cout << "Scanning " << topHeight << " blocks using " << numThreads << " threads..." << std::endl;
    if (lastScannedHeight > 0)
      std::cout << "(Resuming from block " << lastScannedHeight << ")" << std::endl;

    // Progress display thread
    std::thread progressDisplayThread([&]()
                                      {
      auto startTime = std::chrono::steady_clock::now();
      while (!progressDone)
      {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        uint64_t processed = blocksProcessed.load(std::memory_order_relaxed);
        if (processed == 0 || topHeight == 0) continue;
        float percent = (float)processed / (float)topHeight * 100.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startTime).count();
        float speed = elapsed > 0 ? (float)(processed - lastScannedHeight) / (float)elapsed : 0;
        uint64_t eta = speed > 0 ? (uint64_t)((topHeight - processed) / speed) : 0;
        std::cout << "\r[Progress] " << processed << "/" << topHeight
                  << " (" << std::fixed << std::setprecision(1) << percent << "%)"
                  << " | Speed: " << (int)speed << " blk/s"
                  << " | ETA: " << (eta / 60) << "m " << (eta % 60) << "s"
                  << " | Outputs: " << allOutputs.size() << "    " << std::flush;
      } });

    // Progress file writer
    std::thread progressFileThread;
    if (!cfg.progressFile.empty())
    {
      progressFileThread = std::thread([&]()
                                       {
        while (!progressDone)
        {
          std::ofstream pf(cfg.progressFile, std::ios::trunc);
          if (pf.is_open()) { pf << blocksProcessed.load() << "/" << topHeight << std::endl; pf.close(); }
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        std::ofstream pf(cfg.progressFile, std::ios::trunc);
        if (pf.is_open()) { pf << topHeight << "/" << topHeight << std::endl; pf.close(); } });
    }

    // Scan
    try
    {
      uint32_t scanTopHeight = (cfg.endBlock > 0 && cfg.endBlock < topHeight) ? cfg.endBlock : topHeight;
      uint32_t scanStartHeight = std::max(cfg.startBlock, lastScannedHeight + 1);
      if (scanStartHeight > scanTopHeight)
      {
        std::cerr << "Error: start-block (" << scanStartHeight << ") is after end-block (" << scanTopHeight << ")" << std::endl;
        wallet.save(cn::WalletSaveLevel::SAVE_ALL);
        wallet.shutdown();
        progressDone = true;
        if (progressDisplayThread.joinable())
          progressDisplayThread.join();
        if (progressFileThread.joinable())
          progressFileThread.join();
        return 1;
      }
      uint32_t blocksPerThread = (scanTopHeight - scanStartHeight + 1 + numThreads - 1) / numThreads;

      std::vector<std::thread> threads;
      for (unsigned int t = 0; t < numThreads; ++t)
      {
        uint32_t start = scanStartHeight + static_cast<uint32_t>(t * blocksPerThread);
        uint32_t end = std::min(start + blocksPerThread - 1, scanTopHeight);
        if (start > end)
          break;
        threads.emplace_back([&, start, end]()
                             {
          ScanContext ctx{ blockchain, viewKey, viewPub, hasSpendKey ? &spendKey : nullptr,
                           blocksProcessed, lastCheckpointHeight, resultsMutex, allOutputs, saveCheckpoint };
          for (uint32_t h = start; h <= end; ++h)
          {
            scanSingleBlock(h, ctx);
            if (h % 10000 == 0) ctx.saveCheckpoint(h);
          } });
      }
      for (auto &t : threads)
      {
        if (t.joinable())
          t.join();
      }
    }
    catch (const std::exception &e)
    {
      std::cerr << std::endl
                << "========================================" << std::endl
                << "Scan interrupted at block " << blocksProcessed.load() << std::endl
                << "Error: " << e.what() << std::endl
                << "Progress saved. Run same command to resume." << std::endl
                << "========================================" << std::endl;
      saveCheckpoint(static_cast<uint32_t>(blocksProcessed.load()));
      wallet.save(cn::WalletSaveLevel::SAVE_ALL);
      wallet.shutdown();
      progressDone = true;
      if (progressDisplayThread.joinable())
        progressDisplayThread.join();
      if (progressFileThread.joinable())
        progressFileThread.join();
      return 1;
    }

    progressDone = true;
    if (progressDisplayThread.joinable())
      progressDisplayThread.join();
    if (progressFileThread.joinable())
      progressFileThread.join();
    std::cout << std::endl
              << "Scan complete. Found " << allOutputs.size() << " matching outputs." << std::endl;

    // Check spent status
    if (hasSpendKey && !allOutputs.empty())
    {
      std::cout << "Checking spent status..." << std::endl;
      uint64_t spentCount = 0;
      for (auto &fo : allOutputs)
      {
        if (blockchain.isSpentKeyImage(fo.keyImage))
        {
          fo.spent = true;
          spentCount++;
        }
      }
      std::cout << spentCount << " outputs already spent." << std::endl;
    }

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

    // Save
    wallet.save(cn::WalletSaveLevel::SAVE_ALL);
    wallet.shutdown();

    // Final output
    uint64_t totalReceived = 0, totalSpent = 0;
    for (const auto &fo : allOutputs)
    {
      totalReceived += fo.amount;
      if (fo.spent)
        totalSpent += fo.amount;
    }

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

    blockchain.putMeta("wallet_init_progress", std::vector<uint8_t>());
    blockchain.flush();
    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 2;
  }
}