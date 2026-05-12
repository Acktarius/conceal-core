// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <iostream>
#include <string>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <thread>

#include <boost/program_options.hpp>

#include "CryptoNoteConfig.h"
#include "CryptoNoteCore/Blockchain.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "Common/CommandLine.h"
#include "Common/FileMappedVector.h"
#include "Common/StringTools.h"
#include "Common/Util.h"
#include "Logging/ConsoleLogger.h"
#include "Storage/MDBXBlockchainStorage.h"

namespace po = boost::program_options;
using namespace cn;
using namespace common;

namespace
{
  // Path to the old SwappedVector blockchain data or existing MDBX database (with --rescue)
  const command_line::arg_descriptor<std::string> arg_old_data_dir = {"old-dir", "Path to the old blockchain data (SwappedVector format) or existing MDBX database (with --rescue)", ""};
  // Path where the new MDBX database will be created
  const command_line::arg_descriptor<std::string> arg_new_data_dir = {"new-dir", "Path where the new MDBX database will be created", ""};
  // Use testnet parameters
  const command_line::arg_descriptor<bool> arg_testnet = {"testnet", "Use testnet parameters", false};
  // Maximum database size in GB (0 = no limit)
  const command_line::arg_descriptor<uint64_t> arg_size_limit = {"size-limit", "Maximum database size in GB (0 = no limit, default: 128)", 128};
  // Remove upper size limit entirely
  const command_line::arg_descriptor<bool> arg_no_limit = {"no-limit", "Remove upper size limit entirely (cannot be used with --size-limit)", false};
  // Enable bulk/NOSYNC mode for maximum speed (riskier, database may bloat; safe-mode is the default)
  const command_line::arg_descriptor<bool> arg_bulk = {"bulk", "Enable bulk/NOSYNC mode for maximum migration speed (database may grow larger; not recommended for rescue)", false};
  // Rescue mode: read from an existing MDBX database, verify, and rewrite with atomic writes
  const command_line::arg_descriptor<bool> arg_rescue = {"rescue", "Rescue an existing MDBX database. Reads blocks, verifies consistency, and rewrites with atomic writes", false};
}

// Joins a directory path with a filename, adding a separator if needed
std::string appendPath(const std::string &path, const std::string &fileName)
{
  std::string result = path;
  if (!result.empty() && result.back() != '/')
    result += '/';
  result += fileName;
  return result;
}

// Represents a source of blocks to migrate
struct BlockSource
{
  virtual ~BlockSource() = default;
  virtual uint32_t size() const = 0;
  virtual bool getBlock(uint32_t height, Blockchain::BlockEntry &entry) const = 0;
  virtual crypto::Hash getHash(uint32_t height) const = 0;
  virtual void close() = 0;
};

// Reads blocks from the old SwappedVector format (blocks.dat + blockindexes.dat)
struct SwappedVectorSource : BlockSource
{
  SwappedVector<Blockchain::BlockEntry> blocks;

  bool open(const std::string &oldDir)
  {
    std::string blocksPath = appendPath(oldDir, parameters::CRYPTONOTE_BLOCKS_FILENAME);
    std::string indexesPath = appendPath(oldDir, parameters::CRYPTONOTE_BLOCKINDEXES_FILENAME);

    if (!blocks.open(blocksPath, indexesPath, 1024))
    {
      std::cerr << "Error: failed to open old blockchain files" << std::endl;
      std::cerr << "  blocks file: " << blocksPath << std::endl;
      std::cerr << "  indexes file: " << indexesPath << std::endl;
      return false;
    }
    if (blocks.empty())
    {
      std::cerr << "Error: old blockchain is empty" << std::endl;
      return false;
    }
    return true;
  }

  uint32_t size() const override { return static_cast<uint32_t>(blocks.size()); }

  bool getBlock(uint32_t height, Blockchain::BlockEntry &entry) const override
  {
    if (height >= blocks.size())
      return false;
    entry = const_cast<SwappedVector<Blockchain::BlockEntry> &>(blocks)[height];
    return true;
  }

  crypto::Hash getHash(uint32_t height) const override
  {
    return get_block_hash(
        const_cast<SwappedVector<Blockchain::BlockEntry> &>(blocks)[height].bl);
  }

  void close() override { blocks.close(); }
};

// Reads blocks from an existing MDBX database (used in --rescue mode)
struct MdbxSource : BlockSource
{
  std::unique_ptr<CryptoNote::MDBXBlockchainStorage> storage;
  uint32_t blockCount = 0;

  bool open(const std::string &path)
  {
    // Open in safe mode, just reading
    storage.reset(new CryptoNote::MDBXBlockchainStorage(path, false, 0));

    uint32_t topHeight = storage->topBlockHeight();
    if (topHeight == 0)
    {
      std::cerr << "Error: MDBX database is empty or has no blocks" << std::endl;
      return false;
    }

    // Check if keys are zero‑padded by trying to read genesis
    cn::BinaryArray ba;
    bool hasPaddedKeys = storage->getBlockEntry(0, ba);
    if (!hasPaddedKeys)
    {
      std::cout << "Legacy key format detected (unpadded keys). Running key migration..." << std::endl;
      std::cout << "This may take several minutes for large databases." << std::endl;
      try
      {
        auto migStart = std::chrono::steady_clock::now();
        storage->migrateToPaddedKeys();
        auto migEnd = std::chrono::steady_clock::now();
        auto migSeconds = std::chrono::duration_cast<std::chrono::seconds>(migEnd - migStart).count();
        std::cout << "Key migration complete. (" << migSeconds << "s)" << std::endl;

        if (!storage->getBlockEntry(0, ba))
        {
          std::cerr << "Error: genesis still unreadable after key migration" << std::endl;
          return false;
        }
      }
      catch (const std::exception &e)
      {
        std::cerr << "Error: key migration failed: " << e.what() << std::endl;
        return false;
      }
    }

    blockCount = topHeight + 1;
    return true;
  }

  uint32_t size() const override { return blockCount; }

  bool getBlock(uint32_t height, Blockchain::BlockEntry &entry) const override
  {
    cn::BinaryArray ba;
    if (!storage->getBlockEntry(height, ba))
      return false;
    if (!cn::fromBinaryArray(entry, ba))
      return false;
    return true;
  }

  crypto::Hash getHash(uint32_t height) const override
  {
    return storage->getBlockHash(height);
  }

  void close() override
  {
    if (storage)
      storage->close();
  }
};

// Local copy of Blockchain::MultisignatureOutputUsage (which is private in Blockchain.h)
struct MultisigOutputUsage
{
  Blockchain::TransactionIndex transactionIndex;
  uint16_t outputIndex;
  bool isUsed;
};

// Container for all the in‑memory indexes built during migration.
// These are serialised to the MDBX meta database at the end so the
// daemon can fast‑load them on restart without a full rebuild.
struct MigrationIndexes
{
  std::vector<crypto::Hash> blockHashes;
  std::unordered_map<crypto::Hash, uint32_t> hashToHeight;
  std::unordered_map<crypto::Hash, Blockchain::TransactionIndex> transactionMap;
  std::unordered_map<crypto::KeyImage, uint32_t> spentKeys;
  std::unordered_map<uint64_t, std::vector<std::pair<Blockchain::TransactionIndex, uint16_t>>> outputs;
  std::unordered_map<uint64_t, std::vector<MultisigOutputUsage>> multisigOutputs;
  std::vector<uint64_t> depositIndex;

  void reserve(uint32_t totalBlocks)
  {
    blockHashes.reserve(totalBlocks);
    hashToHeight.reserve(totalBlocks);
    transactionMap.reserve(totalBlocks * 8);
    spentKeys.reserve(totalBlocks * 8);
  }
};

// Validates a single transaction for structural integrity.
// Returns true if the transaction appears valid, false if corrupted.
bool validateTransaction(const Transaction &tx, uint32_t height)
{
  // Check that inputs are parseable
  for (const auto &input : tx.inputs)
  {
    if (input.type() == typeid(KeyInput))
    {
      const auto &ki = boost::get<KeyInput>(input);
      // keyImage should not be all zeros
      static const crypto::KeyImage ZERO_KI = {};
      if (memcmp(&ki.keyImage, &ZERO_KI, sizeof(crypto::KeyImage)) == 0)
        return false;
    }
    else if (input.type() == typeid(MultisignatureInput))
    {
      const auto &ms = boost::get<MultisignatureInput>(input);
      if (ms.amount == 0)
        return false;
    }
    else if (input.type() == typeid(BaseInput))
    {
      const auto &base = boost::get<BaseInput>(input);
      if (base.blockIndex != height)
        return false;
    }
    else
    {
      // Unknown input type
      return false;
    }
  }

  // Check that outputs are parseable and have valid types
  for (const auto &output : tx.outputs)
  {
    if (output.amount == 0)
      return false;

    if (output.target.type() == typeid(KeyOutput))
    {
      const auto &ko = boost::get<KeyOutput>(output.target);
      static const crypto::PublicKey ZERO_PK = {};
      if (memcmp(&ko.key, &ZERO_PK, sizeof(crypto::PublicKey)) == 0)
        return false;
    }
    else if (output.target.type() == typeid(MultisignatureOutput))
    {
      const auto &mo = boost::get<MultisignatureOutput>(output.target);
      if (mo.keys.empty() || mo.requiredSignatureCount == 0)
        return false;
    }
    else
    {
      // Unknown output type
      return false;
    }
  }

  // Check that unlock time is reasonable (not a garbage value)
  // Max unlock time is current timestamp + some reasonable delta
  // A garbage value like 7948606249959508055 is clearly invalid
  if (tx.unlockTime > static_cast<uint64_t>(time(nullptr)) + (100 * 365 * 24 * 60 * 60))
  {
    // Unlock time is more than 100 years in the future, likely garbage
    return false;
  }

  // Check that version is valid
  if (tx.version < 1 || tx.version > 2)
    return false;

  // Check that signature count matches input count
  if (tx.signatures.size() != tx.inputs.size())
    return false;

  return true;
}

// Writes all fast‑load indexes to the MDBX meta database.
void writeFastIndexes(CryptoNote::MDBXBlockchainStorage &mdbxStorage,
                      const MigrationIndexes &idx)
{
  std::cout << "Writing in-memory indexes to meta database..." << std::endl;

  // idx_hashes: the full block hash array
  mdbxStorage.putMeta("idx_hashes",
                      std::vector<uint8_t>(
                          reinterpret_cast<const uint8_t *>(idx.blockHashes.data()),
                          reinterpret_cast<const uint8_t *>(idx.blockHashes.data()) +
                              idx.blockHashes.size() * sizeof(crypto::Hash)));

  // idx_hash2height: hash-to-height lookup table
  {
    std::vector<uint8_t> buf;
    buf.reserve(idx.hashToHeight.size() * (sizeof(crypto::Hash) + sizeof(uint32_t)));
    for (const auto &p : idx.hashToHeight)
    {
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(p.first.data),
                 reinterpret_cast<const uint8_t *>(p.first.data) + sizeof(crypto::Hash));
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&p.second),
                 reinterpret_cast<const uint8_t *>(&p.second) + sizeof(uint32_t));
    }
    mdbxStorage.putMeta("idx_hash2height", std::move(buf));
  }

  // idx_txmap: transaction hash to (block, txIndex)
  {
    std::vector<uint8_t> buf;
    buf.reserve(idx.transactionMap.size() * (sizeof(crypto::Hash) + sizeof(uint64_t)));
    for (const auto &kv : idx.transactionMap)
    {
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(kv.first.data),
                 reinterpret_cast<const uint8_t *>(kv.first.data) + sizeof(crypto::Hash));
      uint64_t packed = (static_cast<uint64_t>(kv.second.block) << 16) | kv.second.transaction;
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&packed),
                 reinterpret_cast<const uint8_t *>(&packed) + sizeof(packed));
    }
    mdbxStorage.putMeta("idx_txmap", std::move(buf));
  }

  // idx_spentkeys: key image to block height
  {
    std::vector<uint8_t> buf;
    buf.reserve(idx.spentKeys.size() * (sizeof(crypto::KeyImage) + sizeof(uint32_t)));
    for (const auto &p : idx.spentKeys)
    {
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(p.first.data),
                 reinterpret_cast<const uint8_t *>(p.first.data) + sizeof(crypto::KeyImage));
      uint32_t h = p.second;
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&h),
                 reinterpret_cast<const uint8_t *>(&h) + sizeof(h));
    }
    mdbxStorage.putMeta("idx_spentkeys", std::move(buf));
  }

  // idx_outputs: amount -> list of (block, tx, outputIndex)
  {
    std::vector<uint8_t> buf;
    for (const auto &p : idx.outputs)
    {
      uint64_t amount = p.first;
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&amount),
                 reinterpret_cast<const uint8_t *>(&amount) + sizeof(amount));
      uint32_t count = static_cast<uint32_t>(p.second.size());
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&count),
                 reinterpret_cast<const uint8_t *>(&count) + sizeof(count));
      for (const auto &pair : p.second)
      {
        uint32_t block = pair.first.block;
        uint16_t tx = pair.first.transaction;
        uint16_t outIdx = pair.second;
        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t *>(&block),
                   reinterpret_cast<const uint8_t *>(&block) + sizeof(block));
        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t *>(&tx),
                   reinterpret_cast<const uint8_t *>(&tx) + sizeof(tx));
        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t *>(&outIdx),
                   reinterpret_cast<const uint8_t *>(&outIdx) + sizeof(outIdx));
      }
    }
    mdbxStorage.putMeta("idx_outputs", std::move(buf));
  }

  // idx_msig: amount -> list of MultisignatureOutputUsage
  {
    std::vector<uint8_t> buf;
    for (const auto &p : idx.multisigOutputs)
    {
      uint64_t amount = p.first;
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&amount),
                 reinterpret_cast<const uint8_t *>(&amount) + sizeof(amount));
      uint32_t count = static_cast<uint32_t>(p.second.size());
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&count),
                 reinterpret_cast<const uint8_t *>(&count) + sizeof(count));
      for (const auto &usage : p.second)
      {
        uint32_t block = usage.transactionIndex.block;
        uint16_t tx = usage.transactionIndex.transaction;
        uint16_t outIdx = usage.outputIndex;
        uint8_t used = usage.isUsed ? 1 : 0;
        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t *>(&block),
                   reinterpret_cast<const uint8_t *>(&block) + sizeof(block));
        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t *>(&tx),
                   reinterpret_cast<const uint8_t *>(&tx) + sizeof(tx));
        buf.insert(buf.end(),
                   reinterpret_cast<const uint8_t *>(&outIdx),
                   reinterpret_cast<const uint8_t *>(&outIdx) + sizeof(outIdx));
        buf.push_back(used);
      }
    }
    mdbxStorage.putMeta("idx_msig", std::move(buf));
  }

  // idx_deposits: array of uint64_t deposit amounts per height
  {
    std::vector<uint8_t> buf;
    buf.reserve(sizeof(uint64_t) * idx.depositIndex.size());
    for (size_t i = 0; i < idx.depositIndex.size(); ++i)
    {
      uint64_t val = idx.depositIndex[i];
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&val),
                 reinterpret_cast<const uint8_t *>(&val) + sizeof(val));
    }
    mdbxStorage.putMeta("idx_deposits", std::move(buf));
  }

  // idx_topheight: the current chain height
  {
    uint32_t topHeight = static_cast<uint32_t>(idx.blockHashes.size() - 1);
    mdbxStorage.putMeta("idx_topheight",
                        std::vector<uint8_t>(
                            reinterpret_cast<const uint8_t *>(&topHeight),
                            reinterpret_cast<const uint8_t *>(&topHeight) + sizeof(topHeight)));
  }

  mdbxStorage.flush();
  std::cout << "Fast indexes written successfully." << std::endl;
}

int main(int argc, char *argv[])
{
  // Parse command-line arguments
  po::options_description desc("Options");
  command_line::add_arg(desc, arg_old_data_dir);
  command_line::add_arg(desc, arg_new_data_dir);
  command_line::add_arg(desc, arg_testnet);
  command_line::add_arg(desc, arg_size_limit);
  command_line::add_arg(desc, arg_no_limit);
  command_line::add_arg(desc, arg_bulk);
  command_line::add_arg(desc, arg_rescue);

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  std::string oldDir = command_line::get_arg(vm, arg_old_data_dir);
  std::string newDir = command_line::get_arg(vm, arg_new_data_dir);
  bool testnet = command_line::get_arg(vm, arg_testnet);
  uint64_t sizeLimitGB = command_line::get_arg(vm, arg_size_limit);
  bool noLimit = command_line::get_arg(vm, arg_no_limit);
  bool bulkMode = command_line::get_arg(vm, arg_bulk);
  bool rescueMode = command_line::get_arg(vm, arg_rescue);

  // Validate conflicting options
  if (sizeLimitGB != 128 && noLimit)
  {
    std::cerr << "Error: --size-limit and --no-limit cannot be used together" << std::endl;
    return 1;
  }

  if (noLimit)
    sizeLimitGB = 0;

  if (oldDir.empty() || newDir.empty())
  {
    std::cerr << "Error: --old-dir and --new-dir are required" << std::endl;
    return 1;
  }

  if (oldDir == newDir)
  {
    std::cerr << "Error: old-dir and new-dir must be different" << std::endl;
    return 1;
  }

  // Create the output directory if it doesn't exist
  if (!tools::create_directories_if_necessary(newDir))
  {
    std::cerr << "Error: failed to create new directory: " << newDir << std::endl;
    return 1;
  }

  // Print banner
  std::cout << "Conceal Migration Tool v2" << std::endl;
  std::cout << "Source dir: " << oldDir << std::endl;
  std::cout << "Target dir: " << newDir << std::endl;
  std::cout << "Testnet: " << (testnet ? "yes" : "no") << std::endl;
  std::cout << "Mode: " << (rescueMode ? "RESCUE (from MDBX)" : "MIGRATE (from SwappedVector)") << std::endl;
  std::cout << "Write mode: " << (bulkMode ? "BULK (NOSYNC, faster but riskier)" : "SAFE (durable commits)") << std::endl;

  // Initialize currency (needed for genesis block verification)
  logging::ConsoleLogger consoleLogger;
  Currency currency = CurrencyBuilder(consoleLogger).currency();

  // Open the block source
  std::unique_ptr<BlockSource> source;
  if (rescueMode)
  {
    std::unique_ptr<MdbxSource> mdbxSource(new MdbxSource());
    std::string mdbxSourcePath = appendPath(oldDir, "mdbx_blocks");
    if (!mdbxSource->open(mdbxSourcePath))
      return 1;
    source = std::move(mdbxSource);
  }
  else
  {
    std::unique_ptr<SwappedVectorSource> svSource(new SwappedVectorSource());
    if (!svSource->open(oldDir))
      return 1;
    source = std::move(svSource);
  }

  uint32_t totalBlocks = source->size();
  std::cout << "Source blockchain height: " << (totalBlocks - 1) << std::endl;

  // Verify the genesis block matches
  Blockchain::BlockEntry genesisEntry;
  if (!source->getBlock(0, genesisEntry))
  {
    std::cerr << "Error: failed to read genesis block" << std::endl;
    return 1;
  }
  crypto::Hash genesisHash = get_block_hash(genesisEntry.bl);

  if (testnet)
  {
    std::cout << "Testnet genesis hash: " << common::podToHex(genesisHash) << std::endl;
  }
  else
  {
    if (genesisHash != currency.genesisBlockHash())
    {
      std::cerr << "Error: genesis block mismatch" << std::endl;
      std::cerr << "  expected: " << common::podToHex(currency.genesisBlockHash()) << std::endl;
      std::cerr << "  actual: " << common::podToHex(genesisHash) << std::endl;
      return 1;
    }
    std::cout << "Genesis block verified: " << common::podToHex(genesisHash) << std::endl;
  }

  // Initialize the new MDBX database
  if (rescueMode && bulkMode)
  {
    std::cout << "Warning: --bulk with --rescue is not recommended." << std::endl;
    std::cout << "Continuing in 5 seconds... (Ctrl+C to abort)" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(5));
  }

  std::cout << "Initializing MDBX storage backend..." << std::endl;

  uint64_t sizeLimitBytes = sizeLimitGB > 0 ? (sizeLimitGB << 30) : 0;
  std::string mdbxPath = appendPath(newDir, "mdbx_blocks");

  CryptoNote::MDBXBlockchainStorage mdbxStorage(mdbxPath, bulkMode, sizeLimitBytes);

  // In-memory index structures built during migration.
  MigrationIndexes idx;
  idx.reserve(totalBlocks);

  // Track any inconsistencies found
  uint32_t brokenBlocks = 0;
  uint32_t corruptedTxs = 0;
  uint32_t lastGoodHeight = 0;

  // Migrate every block, building all fast‑load indexes as we go
  std::cout << "Starting migration..." << std::endl;
  auto startTime = std::chrono::steady_clock::now();

  for (uint32_t h = 0; h < totalBlocks; ++h)
  {
    Blockchain::BlockEntry entry;
    if (!source->getBlock(h, entry))
    {
      std::cerr << "Error: failed to read block at height " << h << std::endl;
      if (rescueMode)
      {
        brokenBlocks++;
        std::cout << "Rescue: block " << h << " is unreadable. Truncating at height " << lastGoodHeight << std::endl;
        totalBlocks = lastGoodHeight + 1;
        break;
      }
      return 1;
    }

    crypto::Hash blockHash = get_block_hash(entry.bl);

    // In rescue mode, verify the hash matches what the source DB says
    if (rescueMode)
    {
      crypto::Hash sourceHash = source->getHash(h);
      if (sourceHash != crypto::Hash() && blockHash != sourceHash)
      {
        std::cerr << "Warning: block " << h << " hash mismatch" << std::endl;
        std::cerr << "  computed: " << common::podToHex(blockHash) << std::endl;
        std::cerr << "  stored:   " << common::podToHex(sourceHash) << std::endl;
        brokenBlocks++;
      }
    }

    // Validate all transactions in this block
    bool blockValid = true;
    for (uint32_t t = 0; t < entry.transactions.size(); ++t)
    {
      if (!validateTransaction(entry.transactions[t].tx, h))
      {
        std::cerr << "Warning: block " << h << " has corrupted transaction at index " << t << std::endl;
        corruptedTxs++;
        blockValid = false;
        break;
      }
    }

    // Also validate the base transaction
    if (blockValid && !validateTransaction(entry.bl.baseTransaction, h))
    {
      std::cerr << "Warning: block " << h << " has corrupted base transaction" << std::endl;
      corruptedTxs++;
      blockValid = false;
    }

    if (!blockValid)
    {
      brokenBlocks++;
      std::cout << "Block " << h << " contains corrupted data. Truncating chain at height " << lastGoodHeight << std::endl;
      totalBlocks = lastGoodHeight + 1;
      break;
    }

    // Build the lightweight header POD
    BlockHeaderPOD hdr;
    hdr.majorVersion = entry.bl.majorVersion;
    hdr.minorVersion = entry.bl.minorVersion;
    hdr.timestamp = entry.bl.timestamp;
    hdr.previousBlockHash = entry.bl.previousBlockHash;
    hdr.nonce = entry.bl.nonce;
    hdr.blockCumulativeSize = entry.block_cumulative_size;
    hdr.cumulativeDifficulty = entry.cumulative_difficulty;
    hdr.alreadyGeneratedCoins = entry.already_generated_coins;
    hdr.height = entry.height;

    // Build fast‑load indexes
    for (uint32_t t = 0; t < entry.transactions.size(); ++t)
    {
      crypto::Hash txHash = getObjectHash(entry.transactions[t].tx);
      Blockchain::TransactionIndex txIdx = {h, static_cast<uint16_t>(t)};
      idx.transactionMap[txHash] = txIdx;

      // Track spent key images
      for (const auto &input : entry.transactions[t].tx.inputs)
      {
        if (input.type() == typeid(KeyInput))
        {
          idx.spentKeys[boost::get<KeyInput>(input).keyImage] = h;
        }
        else if (input.type() == typeid(MultisignatureInput))
        {
          const auto &msInput = boost::get<MultisignatureInput>(input);
          if (idx.multisigOutputs.count(msInput.amount) > 0 &&
              msInput.outputIndex < idx.multisigOutputs[msInput.amount].size())
          {
            idx.multisigOutputs[msInput.amount][msInput.outputIndex].isUsed = true;
          }
        }
      }

      // Track outputs
      for (uint32_t o = 0; o < entry.transactions[t].tx.outputs.size(); ++o)
      {
        const auto &out = entry.transactions[t].tx.outputs[o];
        if (out.target.type() == typeid(KeyOutput))
        {
          idx.outputs[out.amount].push_back(std::make_pair(txIdx, o));
        }
        else if (out.target.type() == typeid(MultisignatureOutput))
        {
          MultisigOutputUsage usage;
          usage.transactionIndex = txIdx;
          usage.outputIndex = static_cast<uint16_t>(o);
          usage.isUsed = false;
          idx.multisigOutputs[out.amount].push_back(usage);
        }
      }
    }

    // Track deposit amounts
    int64_t deposit = 0;
    uint64_t interest = 0;
    for (const auto &tx : entry.transactions)
    {
      for (const auto &in : tx.tx.inputs)
      {
        if (in.type() == typeid(MultisignatureInput))
        {
          auto &multisign = boost::get<MultisignatureInput>(in);
          if (multisign.term > 0)
            deposit -= multisign.amount;
        }
      }
      for (const auto &out : tx.tx.outputs)
      {
        if (out.target.type() == typeid(MultisignatureOutput))
        {
          auto &multisign = boost::get<MultisignatureOutput>(out.target);
          if (multisign.term > 0)
            deposit += out.amount;
        }
      }
      interest += currency.calculateTotalTransactionInterest(tx.tx, h);
    }

    if (h == 0)
      idx.depositIndex.push_back(static_cast<uint64_t>(deposit));
    else
      idx.depositIndex.push_back(idx.depositIndex.back() + deposit);

    // Serialize the full block entry
    BinaryArray ba = toBinaryArray(entry);

    // Atomic write: entry + header + both height mappings in ONE transaction
    mdbxStorage.pushCompleteBlock(h, blockHash, ba, hdr);

    // Update in-memory block index
    idx.blockHashes.push_back(blockHash);
    idx.hashToHeight[blockHash] = h;
    lastGoodHeight = h;

    // Progress output every 1% or on the last block
    uint32_t percent = (h * 100) / totalBlocks;
    if (h % (totalBlocks / 100 + 1) == 0 || h == totalBlocks - 1)
    {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();
      std::cout << "Progress: " << h << " / " << totalBlocks
                << " (" << percent << "%)"
                << " - " << elapsed << "s elapsed";
      if (brokenBlocks > 0)
        std::cout << " [" << brokenBlocks << " blocks skipped]";
      if (corruptedTxs > 0)
        std::cout << " [" << corruptedTxs << " corrupted txs]";
      std::cout << std::endl;
    }
  }

  // Write all fast‑load indexes to the MDBX meta database
  writeFastIndexes(mdbxStorage, idx);

  // Flush everything to disk
  mdbxStorage.flush();

  // Print summary
  auto endTime = std::chrono::steady_clock::now();
  auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();
  auto totalMinutes = totalSeconds / 60;
  auto remainingSeconds = totalSeconds % 60;

  std::cout << std::endl;
  std::cout << "Migration complete!" << std::endl;
  std::cout << "  Blocks migrated: " << idx.blockHashes.size() << std::endl;
  std::cout << "  Transactions indexed: " << idx.transactionMap.size() << std::endl;
  std::cout << "  Key images tracked: " << idx.spentKeys.size() << std::endl;
  std::cout << "  Output amounts tracked: " << idx.outputs.size() << std::endl;
  std::cout << "  Multisig amounts tracked: " << idx.multisigOutputs.size() << std::endl;
  std::cout << "  Time: " << totalMinutes << "m " << remainingSeconds << "s" << std::endl;
  std::cout << "  Speed: " << (idx.blockHashes.size() / std::max(totalSeconds, 1L)) << " blocks/sec" << std::endl;
  std::cout << "  New database: " << mdbxPath << std::endl;
  if (brokenBlocks > 0)
  {
    std::cout << std::endl;
    std::cout << "  WARNING: " << brokenBlocks << " blocks were skipped due to corruption." << std::endl;
    std::cout << "  The chain was truncated at height " << lastGoodHeight << "." << std::endl;
    std::cout << "  The daemon will sync the remaining blocks from the network." << std::endl;
  }

  // Spot-check verification: verify 20 random blocks are readable and have correct hashes
  std::cout << std::endl
            << "Verifying migrated data..." << std::endl;
  bool verificationOk = true;
  uint32_t blocksToCheck = std::min(static_cast<uint32_t>(20), static_cast<uint32_t>(idx.blockHashes.size()));
  for (uint32_t i = 0; i < blocksToCheck && verificationOk; ++i)
  {
    uint32_t h = (i == 0) ? 0 : (static_cast<uint32_t>(rand()) % (idx.blockHashes.size() > 1 ? idx.blockHashes.size() - 1 : 1) + 1);
    if (h >= idx.blockHashes.size())
      h = idx.blockHashes.size() > 0 ? static_cast<uint32_t>(idx.blockHashes.size() - 1) : 0;

    BinaryArray migratedBa;
    if (!mdbxStorage.getBlockEntry(h, migratedBa))
    {
      std::cerr << "Verification failed: block " << h << " not found in MDBX" << std::endl;
      verificationOk = false;
      break;
    }

    Blockchain::BlockEntry migratedEntry;
    if (!cn::fromBinaryArray(migratedEntry, migratedBa))
    {
      std::cerr << "Verification failed: block " << h << " failed to deserialise" << std::endl;
      verificationOk = false;
      break;
    }

    crypto::Hash expectedHash = idx.blockHashes[h];
    crypto::Hash migratedHash = get_block_hash(migratedEntry.bl);
    if (expectedHash != migratedHash)
    {
      std::cerr << "Verification failed: block " << h << " hash mismatch" << std::endl;
      std::cerr << "  expected: " << common::podToHex(expectedHash) << std::endl;
      std::cerr << "  actual:   " << common::podToHex(migratedHash) << std::endl;
      verificationOk = false;
      break;
    }
  }

  if (verificationOk)
  {
    std::cout << "Verification passed!" << std::endl;
  }
  else
  {
    std::cerr << "Verification failed. Some blocks may not have migrated correctly." << std::endl;
  }

  // Cleanup
  source->close();
  mdbxStorage.close();

  std::cout << std::endl
            << "Done. Start your daemon with:" << std::endl;
  std::cout << "  ./conceald --use-mdbx --data-dir " << newDir << std::endl;

  return verificationOk ? 0 : 1;
}