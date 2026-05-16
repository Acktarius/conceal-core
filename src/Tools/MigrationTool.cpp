// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>

#include <boost/program_options.hpp>

#include "Blockchain/Blockchain.h"
#include "Common/CommandLine.h"
#include "Common/FileMappedVector.h"
#include "Common/PathHelpers.h"
#include "Common/StringTools.h"
#include "Common/Util.h"
#include "CryptoNoteConfig.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Currency.h"
#include "Logging/ConsoleLogger.h"
#include "Storage/MDBXBlockchainStorage.h"

namespace po = boost::program_options;
using namespace cn;
using namespace common;

namespace
{
  const command_line::arg_descriptor<std::string> arg_old_data_dir = {
      "old-dir", "Path to the old blockchain data (SwappedVector format) or existing MDBX database (with --rescue)", ""};
  const command_line::arg_descriptor<std::string> arg_new_data_dir = {
      "new-dir", "Path where the new MDBX database will be created", ""};
  const command_line::arg_descriptor<bool> arg_testnet = {
      "testnet", "Use testnet parameters", false};
  const command_line::arg_descriptor<uint64_t> arg_size_limit = {
      "size-limit", "Maximum database size in GB (0 = no limit, default: 128)", 128};
  const command_line::arg_descriptor<bool> arg_no_limit = {
      "no-limit", "Remove upper size limit entirely (cannot be used with --size-limit)", false};
  const command_line::arg_descriptor<bool> arg_bulk = {
      "bulk", "Enable bulk/NOSYNC mode for maximum migration speed", false};
  const command_line::arg_descriptor<bool> arg_rescue = {
      "rescue", "Rescue an existing MDBX database. Reads blocks, verifies consistency, and rewrites with atomic writes", false};
}

//  Block source abstraction
struct BlockSource
{
  virtual ~BlockSource() = default;
  virtual uint32_t size() const = 0;
  virtual bool getBlock(uint32_t height, Blockchain::BlockEntry &entry) const = 0;
  virtual crypto::Hash getHash(uint32_t height) const = 0;
  virtual void close() = 0;
};

struct SwappedVectorSource : BlockSource
{
  SwappedVector<Blockchain::BlockEntry> blocks;

  bool open(const std::string &oldDir)
  {
    std::string blocksPath = PathHelpers::appendPath(oldDir, parameters::CRYPTONOTE_BLOCKS_FILENAME);
    std::string indexesPath = PathHelpers::appendPath(oldDir, parameters::CRYPTONOTE_BLOCKINDEXES_FILENAME);

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

struct MdbxSource : BlockSource
{
  std::unique_ptr<CryptoNote::MDBXBlockchainStorage> storage;
  uint32_t blockCount = 0;

  bool open(const std::string &path)
  {
    storage.reset(new CryptoNote::MDBXBlockchainStorage(path, false, 0));

    uint32_t topHeight = storage->topBlockHeight();
    if (topHeight == 0)
    {
      std::cerr << "Error: MDBX database is empty or has no blocks" << std::endl;
      return false;
    }

    cn::BinaryArray ba;
    bool hasPaddedKeys = storage->getBlockEntry(0, ba);
    if (!hasPaddedKeys)
    {
      std::cout << "Legacy key format detected (unpadded keys). Running key migration..." << std::endl;
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
    return cn::fromBinaryArray(entry, ba);
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

//  In-memory index structures (mirrors Blockchain's private members)
struct MultisigOutputUsage
{
  Blockchain::TransactionIndex transactionIndex;
  uint16_t outputIndex;
  bool isUsed;
};

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

//  Transaction validation
bool validateTransaction(const Transaction &tx, uint32_t height)
{
  for (const auto &input : tx.inputs)
  {
    if (input.type() == typeid(KeyInput))
    {
      static const crypto::KeyImage ZERO_KI = {};
      if (memcmp(&boost::get<KeyInput>(input).keyImage, &ZERO_KI, sizeof(crypto::KeyImage)) == 0)
        return false;
    }
    else if (input.type() == typeid(MultisignatureInput))
    {
      if (boost::get<MultisignatureInput>(input).amount == 0)
        return false;
    }
    else if (input.type() == typeid(BaseInput))
    {
      if (boost::get<BaseInput>(input).blockIndex != height)
        return false;
    }
    else
    {
      return false;
    }
  }

  for (const auto &output : tx.outputs)
  {
    if (output.amount == 0)
      return false;

    if (output.target.type() == typeid(KeyOutput))
    {
      static const crypto::PublicKey ZERO_PK = {};
      if (memcmp(&boost::get<KeyOutput>(output.target).key, &ZERO_PK, sizeof(crypto::PublicKey)) == 0)
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
      return false;
    }
  }

  if (tx.unlockTime > static_cast<uint64_t>(time(nullptr)) + (100ULL * 365 * 24 * 60 * 60))
    return false;

  if (tx.version < 1 || tx.version > 2)
    return false;

  if (tx.signatures.size() != tx.inputs.size())
    return false;

  return true;
}

//  Fast-index writer
void writeFastIndexes(CryptoNote::MDBXBlockchainStorage &storage, const MigrationIndexes &idx)
{
  std::cout << "Writing in-memory indexes to meta database..." << std::endl;
  std::vector<uint8_t> buf;

  // Block hashes
  storage.putMeta("idx_hashes",
                  std::vector<uint8_t>(
                      reinterpret_cast<const uint8_t *>(idx.blockHashes.data()),
                      reinterpret_cast<const uint8_t *>(idx.blockHashes.data()) +
                          idx.blockHashes.size() * sizeof(crypto::Hash)));

  // Hash → height map
  buf.clear();
  for (const auto &p : idx.hashToHeight)
  {
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t *>(p.first.data),
               reinterpret_cast<const uint8_t *>(p.first.data) + sizeof(crypto::Hash));
    uint32_t h = p.second;
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t *>(&h),
               reinterpret_cast<const uint8_t *>(&h) + sizeof(h));
  }
  storage.putMeta("idx_hash2height", buf);

  // Transaction map
  buf.clear();
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
  storage.putMeta("idx_txmap", buf);

  // Spent keys — meta blob + native MDBX DB
  buf.clear();
  std::vector<crypto::KeyImage> keyImages;
  keyImages.reserve(idx.spentKeys.size());
  for (const auto &p : idx.spentKeys)
  {
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t *>(p.first.data),
               reinterpret_cast<const uint8_t *>(p.first.data) + sizeof(crypto::KeyImage));
    uint32_t h = p.second;
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t *>(&h),
               reinterpret_cast<const uint8_t *>(&h) + sizeof(h));
    keyImages.push_back(p.first);
  }
  storage.putMeta("idx_spentkeys", buf);
  storage.markKeyImagesSpent(keyImages);

  // Outputs index
  buf.clear();
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
      buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&block),
                 reinterpret_cast<const uint8_t *>(&block) + sizeof(block));
      buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&tx),
                 reinterpret_cast<const uint8_t *>(&tx) + sizeof(tx));
      buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&outIdx),
                 reinterpret_cast<const uint8_t *>(&outIdx) + sizeof(outIdx));
    }
  }
  storage.putMeta("idx_outputs", buf);

  // Multisig outputs index
  buf.clear();
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
      buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&block),
                 reinterpret_cast<const uint8_t *>(&block) + sizeof(block));
      buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&tx),
                 reinterpret_cast<const uint8_t *>(&tx) + sizeof(tx));
      buf.insert(buf.end(), reinterpret_cast<const uint8_t *>(&outIdx),
                 reinterpret_cast<const uint8_t *>(&outIdx) + sizeof(outIdx));
      buf.push_back(used);
    }
  }
  storage.putMeta("idx_msig", buf);

  // Deposit index
  {
    buf.clear();
    buf.reserve(sizeof(uint64_t) * idx.depositIndex.size());
    for (size_t i = 0; i < idx.depositIndex.size(); ++i)
    {
      uint64_t val = idx.depositIndex[i];
      buf.insert(buf.end(),
                 reinterpret_cast<const uint8_t *>(&val),
                 reinterpret_cast<const uint8_t *>(&val) + sizeof(val));
    }
    storage.putMeta("idx_deposits", buf);
  }

  // Top height
  {
    uint32_t topHeight = static_cast<uint32_t>(idx.blockHashes.size() - 1);
    storage.putMeta("idx_topheight",
                    std::vector<uint8_t>(
                        reinterpret_cast<const uint8_t *>(&topHeight),
                        reinterpret_cast<const uint8_t *>(&topHeight) + sizeof(topHeight)));
  }

  storage.flush();
  std::cout << "Fast indexes written successfully." << std::endl;
}

int main(int argc, char *argv[])
{
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

  if (sizeLimitGB != 128 && noLimit)
  {
    std::cerr << "Error: --size-limit and --no-limit cannot be used together" << std::endl;
    return 1;
  }

  uint64_t sizeLimitBytes = noLimit ? 0 : (sizeLimitGB << 30);

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

  if (!tools::create_directories_if_necessary(newDir))
  {
    std::cerr << "Error: failed to create new directory: " << newDir << std::endl;
    return 1;
  }

  std::cout << "Conceal Migration Tool v2" << std::endl;
  std::cout << "Source dir: " << oldDir << std::endl;
  std::cout << "Target dir: " << newDir << std::endl;
  std::cout << "Testnet: " << (testnet ? "yes" : "no") << std::endl;
  std::cout << "Mode: " << (rescueMode ? "RESCUE (from MDBX)" : "MIGRATE (from SwappedVector)") << std::endl;
  std::cout << "Write mode: " << (bulkMode ? "BULK" : "SAFE") << std::endl;

  logging::ConsoleLogger consoleLogger;
  Currency currency = CurrencyBuilder(consoleLogger).currency();

  // Open source
  std::unique_ptr<BlockSource> source;
  if (rescueMode)
  {
    auto mdbxSource = std::unique_ptr<MdbxSource>(new MdbxSource());
    std::string mdbxSourcePath = PathHelpers::appendPath(oldDir, "mdbx_blocks");
    if (!mdbxSource->open(mdbxSourcePath))
      return 1;
    source = std::move(mdbxSource);
  }
  else
  {
    auto svSource = std::unique_ptr<SwappedVectorSource>(new SwappedVectorSource());
    if (!svSource->open(oldDir))
      return 1;
    source = std::move(svSource);
  }

  uint32_t totalBlocks = source->size();
  std::cout << "Source blockchain height: " << (totalBlocks - 1) << std::endl;

  // Verify genesis
  Blockchain::BlockEntry genesisEntry;
  if (!source->getBlock(0, genesisEntry))
  {
    std::cerr << "Error: failed to read genesis block" << std::endl;
    return 1;
  }
  crypto::Hash genesisHash = get_block_hash(genesisEntry.bl);

  if (!testnet && genesisHash != currency.genesisBlockHash())
  {
    std::cerr << "Error: genesis block mismatch" << std::endl;
    std::cerr << "  expected: " << common::podToHex(currency.genesisBlockHash()) << std::endl;
    std::cerr << "  actual: " << common::podToHex(genesisHash) << std::endl;
    return 1;
  }
  std::cout << "Genesis block verified: " << common::podToHex(genesisHash) << std::endl;

  // Initialize MDBX
  std::cout << "Initializing MDBX storage backend..." << std::endl;
  std::string mdbxPath = PathHelpers::appendPath(newDir, "mdbx_blocks");
  CryptoNote::MDBXBlockchainStorage mdbxStorage(mdbxPath, bulkMode, sizeLimitBytes);

  // Build indexes during migration
  MigrationIndexes idx;
  idx.reserve(totalBlocks);

  uint32_t brokenBlocks = 0;
  uint32_t corruptedTxs = 0;
  uint32_t lastGoodHeight = 0;

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
        totalBlocks = lastGoodHeight + 1;
        break;
      }
      return 1;
    }

    crypto::Hash blockHash = get_block_hash(entry.bl);

    if (rescueMode)
    {
      crypto::Hash sourceHash = source->getHash(h);
      if (sourceHash != crypto::Hash() && blockHash != sourceHash)
      {
        std::cerr << "Warning: block " << h << " hash mismatch" << std::endl;
        brokenBlocks++;
      }
    }

    // Validate transactions
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

    if (blockValid && !validateTransaction(entry.bl.baseTransaction, h))
    {
      std::cerr << "Warning: block " << h << " has corrupted base transaction" << std::endl;
      corruptedTxs++;
      blockValid = false;
    }

    if (!blockValid)
    {
      brokenBlocks++;
      totalBlocks = lastGoodHeight + 1;
      break;
    }

    // Build header POD
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

    // Build indexes
    for (uint32_t t = 0; t < entry.transactions.size(); ++t)
    {
      crypto::Hash txHash = getObjectHash(entry.transactions[t].tx);
      Blockchain::TransactionIndex txIdx = {h, static_cast<uint16_t>(t)};
      idx.transactionMap[txHash] = txIdx;

      for (const auto &input : entry.transactions[t].tx.inputs)
      {
        if (input.type() == typeid(KeyInput))
          idx.spentKeys[boost::get<KeyInput>(input).keyImage] = h;
        else if (input.type() == typeid(MultisignatureInput))
        {
          const auto &msInput = boost::get<MultisignatureInput>(input);
          if (idx.multisigOutputs.count(msInput.amount) > 0 &&
              msInput.outputIndex < idx.multisigOutputs[msInput.amount].size())
            idx.multisigOutputs[msInput.amount][msInput.outputIndex].isUsed = true;
        }
      }

      for (uint32_t o = 0; o < entry.transactions[t].tx.outputs.size(); ++o)
      {
        const auto &out = entry.transactions[t].tx.outputs[o];
        if (out.target.type() == typeid(KeyOutput))
          idx.outputs[out.amount].push_back(std::make_pair(txIdx, o));
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

    // Track deposits
    int64_t deposit = 0;
    uint64_t interest = 0;
    for (const auto &tx : entry.transactions)
    {
      for (const auto &in : tx.tx.inputs)
      {
        if (in.type() == typeid(MultisignatureInput))
        {
          const auto &multisign = boost::get<MultisignatureInput>(in);
          if (multisign.term > 0)
            deposit -= multisign.amount;
        }
      }
      for (const auto &out : tx.tx.outputs)
      {
        if (out.target.type() == typeid(MultisignatureOutput))
        {
          const auto &multisign = boost::get<MultisignatureOutput>(out.target);
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

    // Atomic write
    BinaryArray ba = toBinaryArray(entry);
    mdbxStorage.pushCompleteBlock(h, blockHash, ba, hdr);

    idx.blockHashes.push_back(blockHash);
    idx.hashToHeight[blockHash] = h;
    lastGoodHeight = h;

    // Progress
    if (h % (totalBlocks / 100 + 1) == 0 || h == totalBlocks - 1)
    {
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - startTime)
                         .count();
      std::cout << "Progress: " << h << " / " << totalBlocks
                << " (" << (h * 100 / totalBlocks) << "%)"
                << " - " << elapsed << "s elapsed";
      if (brokenBlocks > 0)
        std::cout << " [" << brokenBlocks << " blocks skipped]";
      if (corruptedTxs > 0)
        std::cout << " [" << corruptedTxs << " corrupted txs]";
      std::cout << std::endl;
    }
  }

  writeFastIndexes(mdbxStorage, idx);
  mdbxStorage.flush();

  auto endTime = std::chrono::steady_clock::now();
  auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

  std::cout << std::endl;
  std::cout << "Migration complete!" << std::endl;
  std::cout << "  Blocks migrated: " << idx.blockHashes.size() << std::endl;
  std::cout << "  Transactions indexed: " << idx.transactionMap.size() << std::endl;
  std::cout << "  Key images tracked: " << idx.spentKeys.size() << std::endl;
  std::cout << "  Output amounts tracked: " << idx.outputs.size() << std::endl;
  std::cout << "  Multisig amounts tracked: " << idx.multisigOutputs.size() << std::endl;
  std::cout << "  Time: " << (totalSeconds / 60) << "m " << (totalSeconds % 60) << "s" << std::endl;
  std::cout << "  Speed: " << (idx.blockHashes.size() / std::max(totalSeconds, 1L)) << " blocks/sec" << std::endl;
  std::cout << "  New database: " << mdbxPath << std::endl;
  if (brokenBlocks > 0)
  {
    std::cout << std::endl;
    std::cout << "  WARNING: " << brokenBlocks << " blocks were skipped due to corruption." << std::endl;
    std::cout << "  The chain was truncated at height " << lastGoodHeight << "." << std::endl;
  }

  // Spot-check verification
  std::cout << std::endl
            << "Verifying migrated data..." << std::endl;
  bool verificationOk = true;
  uint32_t blocksToCheck = std::min(static_cast<uint32_t>(20), static_cast<uint32_t>(idx.blockHashes.size()));
  for (uint32_t i = 0; i < blocksToCheck && verificationOk; ++i)
  {
    uint32_t h = (i == 0) ? 0 : (static_cast<uint32_t>(rand()) % (idx.blockHashes.size() - 1) + 1);

    BinaryArray migratedBa;
    if (!mdbxStorage.getBlockEntry(h, migratedBa))
    {
      std::cerr << "Verification failed: block " << h << " not found" << std::endl;
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

    crypto::Hash migratedHash = get_block_hash(migratedEntry.bl);
    if (idx.blockHashes[h] != migratedHash)
    {
      std::cerr << "Verification failed: block " << h << " hash mismatch" << std::endl;
      verificationOk = false;
      break;
    }
  }

  if (verificationOk)
    std::cout << "Verification passed!" << std::endl;
  else
    std::cerr << "Verification failed. Some blocks may not have migrated correctly." << std::endl;

  source->close();
  mdbxStorage.close();

  std::cout << std::endl
            << "Done. Start your daemon with:" << std::endl;
  std::cout << "  ./conceald --use-mdbx --data-dir " << newDir << std::endl;

  return verificationOk ? 0 : 1;
}